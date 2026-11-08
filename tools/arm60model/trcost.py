#!/usr/bin/env python3
"""Cost of the block translations, from the translation trace of the model.

Usage:
  trcost.py [-all] [-top N] [-templates] [-estimate] DIR|FILE...

Each argument is a trace written by sim with SIMTRLOG (an image built with
mk.sh -labels), or a directory holding one as tr.txt. The translations at
once (timed_translate) are counted, or every translation with -all.

Printed: the cycles per block and per instruction of each part (the
wrapper of a translation at once with its clock look, the block
management, the evictions, the decoder, the analysis and the emitter, each
split into what is charged to the instructions and what is the block's),
the callees timed whole (the flag scans, the link lookups, the output
groups, the busy-wait loops), the stubs by kind, the words emitted, and the
cost of each pass per class of instruction (the [F] classes compute flags
read later).

-templates (traces made with SIMTRWORDS=1): how much of the emitted code is
a function of the instruction's key (prefix, opcode, flags written and read
later, fused test, segment check, interrupt check): the instructions whose
words, branch offsets and segment T-states left out, are the most common
output of their key, then those that differ from it only in the low 12
bits of some words (an immediate or offset field), the size of a table of
those outputs, and the hits of a memo of the last outputs.

-estimate (needs SIMTRWORDS too): the cost of a redesigned translator,
estimated on the traced instructions from the assumptions of the table
PARAMS below (optimistic, central, pessimistic), with the emitter's fast
path given to the template outputs (a table, which does not fit in the
DRAM left) or to the hits of a 256-entry memo, and with the clock look
after each translation kept or replaced by an estimate. See the section
"Step 14 study" of project-context.MD for the design behind the figures.
"""
import collections
import os
import sys

# ----------------------------------------------------------------------------
# Trace reading

STUB_KINDS = ["timeout", "forward", "link", "taken", "write", "leave", "dyn"]
CALLEES = ["flag scans", "link lookups", "link stubs", "output groups", "busy-wait loops",
           "evictions (next zone)"]


def load(path):
    if os.path.isdir(path):
        path = os.path.join(path, "tr.txt")
    blocks = []
    cur = None
    for line in open(path):
        if line[0] == "B":
            p = line.split()
            b = {"pc": int(p[1], 16), "key": int(p[2], 16), "n": int(p[3]), "at_once": p[4] == "1"}
            i = 5
            while p[i] != "sk":
                b[p[i]] = float(p[i + 1])
                i += 2
            b["sk"] = [tuple(map(float, x.split(":"))) for x in p[i + 1:i + 9]]
            j = p.index("callees")
            b["callees"] = [tuple(map(float, x.split(":"))) for x in p[j + 1:j + 7]]
            b["ins"] = []
            blocks.append(b)
            cur = b
        elif line[0] == "I":
            p = line.split()
            cur["ins"].append({"w": [int(x, 16) for x in p[1:11]], "dec": float(p[11]),
                               "ana": float(p[12]), "emit": float(p[13]), "words": int(p[14])})
        elif line[0] == "W":
            cur["ins"][-1]["code"] = [int(x, 16) for x in line.split()[1:]]
    return blocks


def fields(w):
    """prefix, opcode, kind, flags written, flags read later, fused, segment start, irq check"""
    return ((w[4] >> 16) & 0xFF, (w[4] >> 8) & 0xFF, (w[5] >> 24) & 0xFF, w[5] & 0xFF,
            (w[9] >> 24) & 0xFF, (w[8] >> 24) & 0xFF, (w[6] & 0xFF) != 0, ((w[6] >> 8) & 0xFF) != 0)


# ----------------------------------------------------------------------------
# Instruction classes

R8 = ["B", "C", "D", "E", "H", "L", "(HL)", "A"]
ALU = ["ADD", "ADC", "SUB", "SBC", "AND", "XOR", "OR", "CP"]


def cls(pre, op):
    x, y, z = op >> 6, (op >> 3) & 7, op & 7
    if pre in (0, 3, 4):        # unprefixed, DD, FD
        if x == 1:
            if op == 0x76:
                return "HALT"
            return "LD r,r" if 6 not in (y, z) else ("LD r,(HL)" if z == 6 else "LD (HL),r")
        if x == 2:
            return ALU[y] + (" (HL)" if z == 6 else " r")
        if x == 0:
            if z == 0:
                return ["NOP", "EX AF", "DJNZ", "JR", "JR cc", "JR cc", "JR cc", "JR cc"][y]
            if z == 1:
                return "LD rr,nn" if y % 2 == 0 else "ADD HL,rr"
            if z == 2:
                return ["LD (BC),A", "LD A,(BC)", "LD (DE),A", "LD A,(DE)", "LD (nn),HL", "LD HL,(nn)",
                        "LD (nn),A", "LD A,(nn)"][y]
            if z == 3:
                return "INC rr" if y % 2 == 0 else "DEC rr"
            if z in (4, 5):
                return ("INC" if z == 4 else "DEC") + (" (HL)" if y == 6 else " r")
            if z == 6:
                return "LD (HL),n" if y == 6 else "LD r,n"
            return ["RLCA", "RRCA", "RLA", "RRA", "DAA", "CPL", "SCF", "CCF"][y]
        if z == 0:
            return "RET cc"
        if z == 1:
            return ["POP", "RET", "POP", "EXX", "POP", "JP (HL)", "POP", "LD SP,HL"][y]
        if z == 2:
            return "JP cc"
        if z == 3:
            return ["JP", "CB", "OUT (n),A", "IN A,(n)", "EX (SP),HL", "EX DE,HL", "DI", "EI"][y]
        if z == 4:
            return "CALL cc"
        if z == 5:
            return "PUSH" if y % 2 == 0 else "CALL"
        if z == 6:
            return ALU[y] + " n"
        return "RST"
    if pre in (1, 5, 6):        # CB, DD CB, FD CB
        if x == 0:
            return "ROT" + (" (HL)" if z == 6 else " r")
        return ["", "BIT", "RES", "SET"][x] + (" (HL)" if z == 6 else " r")
    if pre == 2:                # ED
        if x == 1:
            return ["IN r,(C)", "OUT (C),r", "SBC/ADC HL", "LD (nn),rr / rr,(nn)", "NEG", "RETN", "IM",
                    "LD I/R"][z]
        if x == 2 and y >= 4 and z < 4:
            return ["LDI", "CPI", "INI", "OUTI"][z] + ["", "D", "R", "DR"][y - 4]
    return "other"


# ----------------------------------------------------------------------------
# Emitted words as templates

def norm(code):
    """The words with the branch offsets and the T-states of a segment check left out."""
    out = []
    for x in code:
        if (x >> 25) & 7 == 5:
            x &= 0xFF000000
        elif (x & 0x0FFFFF00) == 0x02588C00:     # SUBS r8,r8,#t << 8
            x &= 0xFFFFFF00
        out.append(x)
    return tuple(out)


def ikey(w):
    pre, op, _, df, live, fused, seg, irq = fields(w)
    return (pre, op, live & df, fused, seg, irq)


def template_marks(stream):
    """stream: (key, normalised words) in translation order -> per
    instruction: same as the key's most common output, same up to low
    12-bit fields, hit in an LRU memo of 256 outputs."""
    tops = collections.defaultdict(collections.Counter)
    for k, c in stream:
        tops[k][c] += 1
    pure, patch, memo = [], [], []
    lru = collections.OrderedDict()
    for k, c in stream:
        top, cnt = tops[k].most_common(1)[0]
        pure.append(c == top and cnt >= 2)
        patch.append(len(c) == len(top) and all((a ^ b) & 0xFFFFF000 == 0 for a, b in zip(c, top)))
        memo.append(k in lru and lru[k] == c)
        lru[k] = c
        lru.move_to_end(k)
        if len(lru) > 256:
            lru.popitem(last=False)
    return pure, patch, memo, tops


# ----------------------------------------------------------------------------
# The estimate of a redesigned translator (project-context.MD, step 14
# study). Costs in ARM60 cycles, (optimistic, central, pessimistic):
#   decoder: four-word instruction records written with one STM, ED, DD CB,
#     EI and OUT (n),A decoded from opcode records too; RAM blocks keep
#     the full decoder (a factor of their present cost);
#   analysis: the backward walk over one word of each record; the flag scans
#     (jit_flags_needed) and the block's own work (prologue, runs, groups,
#     busy-wait loops) as factors of their present cost;
#   emitter: a loop over the compact records with the dispatch index chosen
#     by the analysis, the segment check, a copy of the handler's output with
#     its fields patched for an instruction with a template (or a memo hit),
#     the present handler's cost times a factor otherwise (the link lookups
#     in assembly), the stubs by kind, the block's own code;
#   the block management and the wrapper (its clock look: 325 cycles, 26 us).
PARAMS = {
    "dec_plain": (45, 54, 62), "dec_xfer": (70, 85, 100), "dec_pre": (52, 60, 70),
    "dec_slow": (65, 80, 100), "dec_ram": (.7, .8, .9), "dec_blk": (180, 210, 250),
    "ana_walk": (22, 28, 34), "scan": (.4, .55, .7), "ana_blk": (.35, .45, .6), "ana_c": (.4, .5, .7),
    "em_loop": (28, 32, 38), "em_seg": (25, 30, 40), "em_tmpl": (32, 40, 55), "em_other": (.5, .65, .8),
    "em_blk": (200, 230, 280), "mgmt": (320, 360, 420), "wrap": (120, 150, 200),
    "stub": ([32, 25, 100, 110, 45, 70, 300], [38, 30, 130, 140, 55, 85, 350],
             [45, 38, 170, 180, 65, 100, 400]),
}
CLOCK_LOOK = 325
EM_LOOP_NOW, EM_SEG_NOW = 61, 56        # the present emitter's loop and segment check


def estimate(blocks, fast, s, clock_kept):
    v = lambda k: PARAMS[k][s]
    now = collections.Counter()
    new = collections.Counter()
    idx = 0
    for b in blocks:
        ins = b["ins"]
        evict = b["callees"][5][1]
        scans, groups, busy = b["callees"][0][1], b["callees"][3][1], b["callees"][4][1]
        now["wrapper (clock look, C)"] += b["tt"] - b["tb"]
        now["block management"] += b["tb"] - b["dec"] - b["ana"] - b["emit"] - evict
        now["evictions"] += evict
        now["decoder"] += b["dec"]
        now["analysis"] += b["ana"]
        now["emitter"] += b["emit"]
        ram = b["key"] == 0x80000
        d = v("dec_blk")
        a = 0.0
        e = v("em_blk")
        cond_scans = 0.0
        for i in ins:
            pre, op, kind, _, _, _, seg, _ = fields(i["w"])
            if ram:
                d += i["dec"] * v("dec_ram")
            elif pre == 0 and op in (0xD3, 0xFB):
                d += v("dec_slow")
            elif pre == 0:
                d += v("dec_xfer") if kind else v("dec_plain")
            elif pre in (1, 3, 4) and i["dec"] < 200:
                d += v("dec_pre")
            else:
                d += v("dec_slow")
            a += v("ana_walk")
            if 1 <= kind <= 6:                   # conditional: its scan
                extra = max(0.0, i["ana"] - 45)
                cond_scans += extra
                a += extra * v("scan")
            e += v("em_loop") + (v("em_seg") if seg else 0)
            if fast[idx]:
                e += v("em_tmpl")
            else:
                e += max(0.0, i["emit"] - EM_LOOP_NOW - (EM_SEG_NOW if seg else 0)) * v("em_other")
            idx += 1
        end_scan = max(0.0, scans - cond_scans)
        rest = max(0.0, b["ana"] - b["anains"] - end_scan - groups - busy)
        a += rest * v("ana_blk") + end_scan * v("scan") + (groups + busy) * v("ana_c")
        for k, (cnt, _) in enumerate(b["sk"][:7]):
            e += cnt * PARAMS["stub"][s][k]
        new["wrapper (clock look, C)"] += v("wrap") + (CLOCK_LOOK if clock_kept else 0)
        new["block management"] += v("mgmt")
        new["evictions"] += evict
        new["decoder"] += d
        new["analysis"] += a
        new["emitter"] += e
    return now, new


# ----------------------------------------------------------------------------

def main():
    args = sys.argv[1:]
    every = "-all" in args
    top = 40
    if "-top" in args:
        top = int(args[args.index("-top") + 1])
    paths = [a for i, a in enumerate(args) if not a.startswith("-") and not (i > 0 and args[i - 1] == "-top")]
    blocks = []
    for p in paths:
        blocks += [b for b in load(p) if every or b["at_once"]]
    if not blocks:
        sys.exit("no translation in the traces")
    B = len(blocks)
    N = sum(b["n"] for b in blocks)
    S = lambda k: sum(b.get(k, 0) for b in blocks)
    print("%s: %d blocks, %d instructions (%.1f per block)" %
          ("translations" if every else "translations at once", B, N, N / B))
    evict = sum(b["callees"][5][1] for b in blocks)
    parts = [("wrapper of a translation at once (clock look included)", S("tt") - S("tb") if not every else 0),
             ("block management", S("tb") - S("dec") - S("ana") - S("emit") - evict),
             ("evictions", evict),
             ("decoder, per instruction", S("decins")), ("decoder, block", S("dec") - S("decins")),
             ("analysis, per instruction (backward walk)", S("anains")),
             ("analysis, block", S("ana") - S("anains")),
             ("emitter, per instruction", S("emitins")), ("emitter, stubs", S("stubs")),
             ("emitter, block", S("emit") - S("emitins") - S("stubs"))]
    total = sum(c for _, c in parts)
    print("cycles per block %.0f, per instruction %.0f" % (total / B, total / N))
    for name, c in parts:
        print("  %-46s %6.0f per block %5.0f per instruction %5.1f %%" % (name, c / B, c / N, 100 * c / total))
    print("callees timed whole:")
    for k, name in enumerate(CALLEES):
        n = sum(b["callees"][k][0] for b in blocks)
        c = sum(b["callees"][k][1] for b in blocks)
        print("  %-22s %5.2f calls per block, %6.0f cycles per call, %5.0f per block" % (name, n / B, c / max(n, 1), c / B))
    sk = [[0, 0.0] for _ in range(8)]
    for b in blocks:
        for k, (n, c) in enumerate(b["sk"]):
            sk[k][0] += n
            sk[k][1] += c
    print("stubs (%.1f per block): " % (S("nstub") / B) +
          ", ".join("%s %.2f x %.0f" % (STUB_KINDS[k], sk[k][0] / B, sk[k][1] / max(sk[k][0], 1))
                    for k in range(7) if sk[k][0]))
    words = sum(i["words"] for b in blocks for i in b["ins"])
    print("words: %.2f per instruction, stubs %.1f per block, tail %.1f per block" %
          (words / N, S("wstub") / B, S("wtail") / B))
    C = collections.defaultdict(lambda: [0, 0.0, 0.0, 0.0, 0])
    for b in blocks:
        for i in b["ins"]:
            pre, op, _, df, live, _, _, _ = fields(i["w"])
            name = cls(pre, op) + (" [F]" if df & live else "")
            c = C[name]
            c[0] += 1
            c[1] += i["dec"]
            c[2] += i["ana"]
            c[3] += i["emit"]
            c[4] += i["words"]
    ctot = sum(v[1] + v[2] + v[3] for v in C.values())
    print("%-22s %6s %5s %5s %5s %5s %6s %6s %5s" % ("class", "count", "%", "dec", "ana", "emit", "sum", "%cyc", "words"))
    for name, v in sorted(C.items(), key=lambda kv: -(kv[1][1] + kv[1][2] + kv[1][3]))[:top]:
        n = v[0]
        print("%-22s %6d %5.1f %5.0f %5.0f %5.0f %6.0f %6.1f %5.1f" %
              (name, n, 100 * n / N, v[1] / n, v[2] / n, v[3] / n, (v[1] + v[2] + v[3]) / n,
               100 * (v[1] + v[2] + v[3]) / ctot, v[4] / n))
    if "-templates" not in args and "-estimate" not in args:
        return
    if not all("code" in i for b in blocks for i in b["ins"]):
        sys.exit("-templates and -estimate need traces made with SIMTRWORDS=1")
    stream = [(ikey(i["w"]), norm(i["code"])) for b in blocks for i in b["ins"]]
    pure, patch, memo, tops = template_marks(stream)
    emits = [i["emit"] for b in blocks for i in b["ins"]]
    etot = sum(emits)
    for name, marks in (("same words as the key's most common output", pure),
                        ("same up to low 12-bit fields", patch), ("hit in a memo of 256 outputs", memo)):
        print("%-44s %5.1f %% of the instructions, %5.1f %% of the emitter's per-instruction cycles" %
              (name, 100 * sum(marks) / N, 100 * sum(e for e, m in zip(emits, marks) if m) / etot))
    # A table of the handler outputs (segment and interrupt checks left out).
    hk = collections.defaultdict(collections.Counter)
    for b in blocks:
        for i in b["ins"]:
            pre, op, _, df, live, fused, seg, irq = fields(i["w"])
            c = norm(i["code"])
            if seg:
                c = c[2:]
            if irq:
                c = c[:-4]
            hk[(pre, op, live & df, fused)][c] += 1
    keys = sum(1 for cnt in hk.values() if cnt.most_common(1)[0][1] >= 2)
    tw = sum(len(cnt.most_common(1)[0][0]) for cnt in hk.values() if cnt.most_common(1)[0][1] >= 2)
    print("table of the handler outputs met twice or more: %d keys, %d words (%.1f KiB)" % (keys, tw, tw / 256))
    if "-estimate" not in args:
        return
    for label, fast in (("emitter fast path on the template outputs (a table: no DRAM for it)", patch),
                        ("emitter fast path on the hits of a 256-entry memo", memo)):
        print(label)
        for sname, s in (("optimistic", 0), ("central", 1), ("pessimistic", 2)):
            for clock in (True, False):
                now, new = estimate(blocks, fast, s, clock)
                tn = sum(now.values())
                tw = sum(new.values())
                inner = lambda p: p["decoder"] + p["analysis"] + p["emitter"] + p["block management"]
                print("  %-11s clock look %-5s per block %5.0f -> %5.0f: %.2fx (jit_translate_block, "
                      "evictions out: %.2fx)" % (sname, "kept," if clock else "gone,", tn / B, tw / B, tn / tw,
                                                 inner(now) / inner(new)))
                if s == 1 and clock:
                    for k in now:
                        print("      %-26s %6.0f -> %6.0f per block" % (k, now[k] / B, new[k] / B))


if __name__ == "__main__":
    main()
