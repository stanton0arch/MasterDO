#!/usr/bin/env python3
"""Bank traffic of a run, as a bank cache would serve it.

    banks.py DIR [-slots 4,6,8,...] [-window a:b[,c:d...]] [-warm N] [-v]
    banks.py -table [-slots ...] [-warm N] DIR...
    banks.py DIR -tiers S1,S2,... -mem BYTES -bankfile FILE [-disc MS] [-warm N] [-window a:b]

DIR holds a run of the model made with SIMBANKLOG=DIR/banks.txt and
-framelog (log.txt): banks.txt has one line per change of the bank a paged
slot shows ("frame slot bank us"), log.txt the Frame: lines (display on or
off for each frame).

A cache of S slots of 16 KiB holds decompressed banks: bank 0 always (the
fixed first 1 KiB of the Sega mapper), the banks the three paged slots
show (they cannot leave while shown), the others while there is room. A
bank mapped while absent is a miss: it is decoded into a slot, the bank
mapped least lately leaving it (LRU, the last time a bank was shown), or,
as the bound no policy beats, the bank mapped again the latest (Belady).
The banks mapped before the first frame (the reset mapping) are loaded
with the image and counted as held.

Printed: the traffic (changes per frame, distinct banks per window, the
banks by number of mappings), then for each slot count the misses with
both policies, split into display-on and display-off frames, the frames
with a miss, the most in one frame, and the same within the windows given
(-window, frames a to b; -warm N leaves out the first N frames of each
window: the misses a fight start may take before the play settles).

-table: one line per run: the banks of the image mapped, the changes per
frame, the most banks held within 100 and 600 frames (bank 0 and the
banks shown included: the slots a cache needs to miss only at the first
mapping of a bank in such a window), and the display-on misses with LRU
for each slot count (-warm N: from frame N on).

-tiers: slots, a compressed store and the disc sharing BYTES (see
tiers()): for each slot count, the banks the store holds, the
display-on misses served by decoding and by the disc, their time in all
and in the worst frame, with the decoding time of each bank measured by
the model's -lztest and a disc read of MS milliseconds (100 by default).
"""
import sys, os, re, bisect
from collections import defaultdict, Counter


def read_frames(path):
    shown = {}
    pat = re.compile(r"^Frame: (\d+) .* shown (\d+) ")
    with open(path) as f:
        for line in f:
            if line.startswith("Frame: "):
                m = pat.match(line)
                if m:
                    shown[int(m.group(1))] = int(m.group(2))
    return shown


def read_banks(path):
    ev = []
    with open(path) as f:
        for line in f:
            p = line.split()
            if len(p) >= 3:
                ev.append((int(p[0]), int(p[1]), int(p[2]), int(p[3]) if len(p) > 3 else 0))
    return ev


def simulate(ev, slots, policy):
    """Misses per frame for a cache of `slots` slots (bank 0 held too)."""
    cur = [None, None, None]
    held = {0}
    last = {0: -1}                  # index of the last event showing the bank
    misses = []                     # (frame, bank)
    # Next use of each event's bank, for Belady.
    nxt = None
    if policy == "opt":
        nxt = [0] * len(ev)
        seen = {}
        for i in range(len(ev) - 1, -1, -1):
            b = ev[i][2]
            nxt[i] = seen.get(b, 1 << 60)
            seen[b] = i
        nextuse = {}                # bank -> index of its next mapping
    for i, (fr, s, b, us) in enumerate(ev):
        old = cur[s]
        cur[s] = b
        if old is not None:
            last[old] = i
        last[b] = i
        if policy == "opt":
            nextuse[b] = nxt[i]
        if b in held:
            continue
        if fr >= 0:
            misses.append((fr, b))
        if len(held) >= slots:
            shown = set(x for x in cur if x is not None)
            cand = [x for x in held if x != 0 and x not in shown]
            if not cand:
                raise SystemExit("banks.py: %d slots cannot hold bank 0 and three shown banks" % slots)
            if policy == "lru":
                victim = min(cand, key=lambda x: last.get(x, -1))
            else:
                victim = max(cand, key=lambda x: nextuse.get(x, 1 << 60))
            held.discard(victim)
        held.add(b)
    return misses


def table(dirs, slots, warm):
    """One line per run: the traffic and the display-on misses (LRU) for
    each slot count, those of the first `warm` frames left out."""
    print("%-24s %5s %5s %6s %5s %5s  %s" % ("run", "banks", "used", "chg/fr", "w100", "w600",
                                          "  ".join("%5d" % s for s in slots)))
    for d in dirs:
        ev = read_banks(os.path.join(d, "banks.txt"))
        shown = read_frames(os.path.join(d, "log.txt"))
        nframes = max(shown) + 1
        on = set(f for f, v in shown.items() if v)
        run = [e for e in ev if e[0] >= 0]
        banks = len(set(e[2] for e in ev) | {0})
        wmax = []
        for span in (100, 600):
            cur = [None, None, None]
            k = 0
            best = 0
            for w in range(nframes // span + 1):
                held = set(x for x in cur if x is not None) | {0}
                while k < len(ev) and ev[k][0] < (w + 1) * span:
                    cur[ev[k][1]] = ev[k][2]
                    held.add(ev[k][2])
                    k += 1
                best = max(best, len(held))
            wmax.append(best)
        row = []
        for s in slots:
            m = simulate(ev, s, "lru")
            row.append(len([x for x in m if x[0] in on and x[0] >= warm]))
        print("%-24s %5s %5d %6.2f %5d %5d  %s" % (os.path.basename(d.rstrip("/")), "", banks,
                                                 len(run) / max(1, nframes), wmax[0], wmax[1],
                                                 "  ".join("%5d" % x for x in row)))


def tiers(d, mem, slot_list, bankfile, disc_ms, warm, windows):
    """Slots of decoded banks, a compressed store, the disc: at load the
    first S banks of the image stay decoded in the S slots, the store
    takes the compressed banks after them, then those before, while they
    fit in what the slots leave of `mem`; a bank in neither is read from
    the disc when mapped. A miss costs the bank's decoding time (from
    `bankfile`: "bank bytes us" lines, the LZ bank: lines of -lztest) or a
    disc read of `disc_ms`."""
    sizes, dec = {}, {}
    for line in open(bankfile):
        p = line.split()
        sizes[int(p[0])] = int(p[1])
        dec[int(p[0])] = int(p[2])
    nb = len(sizes)
    ev = read_banks(os.path.join(d, "banks.txt"))
    shown = read_frames(os.path.join(d, "log.txt"))
    on = set(f for f, v in shown.items() if v)
    print("%s: %d bytes for slots and store, disc read %d ms" % (d, mem, disc_ms))
    hdr = "slots  store  stored  decode_on  disc_on  ms_on  worst_frame_ms"
    for (lo, hi) in windows:
        hdr += "   [%d-%d]%s n/disc/ms" % (lo, hi, " +%d" % warm if warm else "")
    print(hdr)
    for S in slot_list:
        room = mem - S * 0x4000
        if room < 0:
            continue
        store = set()
        used = 0
        for b in list(range(S, nb)) + list(range(0, min(S, nb))):
            if used + sizes[b] <= room:
                store.add(b)
                used += sizes[b]
        held = set(range(min(S, nb)))
        cur = [None, None, None]
        last = {}
        per_frame = defaultdict(float)
        kinds = Counter()
        wmiss = defaultdict(list)
        for i, (fr, sl, b, us) in enumerate(ev):
            old = cur[sl]
            cur[sl] = b
            if old is not None:
                last[old] = i
            last[b] = i
            if b in held:
                continue
            if fr >= 0 and fr >= warm and fr in on:
                cost = dec[b] / 1000.0 if b in store else float(disc_ms)
                per_frame[fr] += cost
                kinds["dec" if b in store else "disc"] += 1
                for (lo, hi) in windows:
                    if lo + warm <= fr <= hi:
                        wmiss[(lo, hi)].append((cost, b not in store))
            if len(held) >= S:
                sh = set(x for x in cur if x is not None)
                cand = [x for x in held if x != 0 and x not in sh]
                held.discard(min(cand, key=lambda x: last.get(x, -1)))
            held.add(b)
        row = "%5d  %6d  %6d  %9d  %7d  %5.0f  %14.1f" % (S, room, len(store), kinds["dec"], kinds["disc"],
                                                         sum(per_frame.values()),
                                                         max(per_frame.values()) if per_frame else 0)
        for w in windows:
            row += "   %10d/%d/%.0f" % (len(wmiss[w]), sum(1 for x in wmiss[w] if x[1]),
                                       sum(x[0] for x in wmiss[w]))
        print(row)


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        return
    if "-tiers" in a:
        d = a[0]
        mem, slot_list, bankfile, disc_ms, warm, windows = 0, [], None, 100, 0, []
        i = 1
        while i < len(a):
            if a[i] == "-tiers":
                slot_list = [int(x) for x in a[i + 1].split(",")]; i += 2
            elif a[i] == "-mem":
                mem = int(a[i + 1]); i += 2
            elif a[i] == "-bankfile":
                bankfile = a[i + 1]; i += 2
            elif a[i] == "-disc":
                disc_ms = int(a[i + 1]); i += 2
            elif a[i] == "-warm":
                warm = int(a[i + 1]); i += 2
            elif a[i] == "-window":
                for w in a[i + 1].split(","):
                    lo, hi = w.split(":")
                    windows.append((int(lo), int(hi)))
                i += 2
            else:
                raise SystemExit("banks.py: unknown option " + a[i])
        tiers(d, mem, slot_list, bankfile, disc_ms, warm, windows)
        return
    if a[0] == "-table":
        slots = [6, 8, 10, 12, 14, 16, 20]
        warm = 0
        dirs = []
        i = 1
        while i < len(a):
            if a[i] == "-slots":
                slots = [int(x) for x in a[i + 1].split(",")]; i += 2
            elif a[i] == "-warm":
                warm = int(a[i + 1]); i += 2
            else:
                dirs.append(a[i]); i += 1
        table(dirs, slots, warm)
        return
    d = a[0]
    slots = [4, 5, 6, 7, 8, 10, 12, 14, 16, 18, 20, 24, 28, 32, 40]
    windows = []
    warm = 0
    verbose = False
    i = 1
    while i < len(a):
        if a[i] == "-slots":
            slots = [int(x) for x in a[i + 1].split(",")]; i += 2
        elif a[i] == "-window":
            for w in a[i + 1].split(","):
                lo, hi = w.split(":")
                windows.append((int(lo), int(hi)))
            i += 2
        elif a[i] == "-warm":
            warm = int(a[i + 1]); i += 2
        elif a[i] == "-v":
            verbose = True; i += 1
        else:
            raise SystemExit("banks.py: unknown option " + a[i])
    ev = read_banks(os.path.join(d, "banks.txt"))
    shown = read_frames(os.path.join(d, "log.txt"))
    nframes = (max(shown) + 1) if shown else (max(e[0] for e in ev) + 1)
    on = set(f for f, v in shown.items() if v)
    run = [e for e in ev if e[0] >= 0]

    # Traffic.
    per_frame = Counter(e[0] for e in run)
    print("%s: %d frames (%d with the display on), %d bank changes (%.2f per frame), "
          "%d banks mapped of the image" % (d, nframes, len(on), len(run), len(run) / max(1, nframes),
                                          len(set(e[2] for e in ev))))
    by_slot = Counter(e[1] for e in run)
    print("  changes per slot: %s" % ", ".join("%d: %d" % (s, by_slot[s]) for s in range(3)))
    if per_frame:
        mx = max(per_frame.values())
        print("  most changes in one frame: %d (frame %d)" % (mx, max(per_frame, key=per_frame.get)))
    # Distinct banks per 100-frame window and per 600 frames.
    for span in (100, 600):
        ws = defaultdict(set)
        cur = [None, None, None]
        for (fr, s, b, us) in ev:
            cur[s] = b
            ws[max(0, fr) // span].add(b)
        # Banks shown across a window without a change count too.
        cur = [None, None, None]
        k = 0
        for w in range(nframes // span + 1):
            while k < len(ev) and ev[k][0] < w * span:
                cur[ev[k][1]] = ev[k][2]; k += 1
            for x in cur:
                if x is not None:
                    ws[w].add(x)
        sizes = [len(ws[w]) + (0 if 0 in ws[w] else 1) for w in range(nframes // span + 1)]
        print("  banks held in %d-frame windows (bank 0 included): median %d, max %d" %
              (span, sorted(sizes)[len(sizes) // 2], max(sizes)))
    cnt = Counter(e[2] for e in run)
    frames_of = defaultdict(set)
    for e in run:
        frames_of[e[2]].add(e[0])
    print("  banks by mappings (bank: mappings / frames with one / first-last frame):")
    line = "   "
    for b, n in cnt.most_common():
        fs = frames_of[b]
        item = " %d: %d/%d/%d-%d" % (b, n, len(fs), min(fs), max(fs))
        if len(line) + len(item) > 100:
            print(line); line = "   "
        line += item
    print(line)
    for (lo, hi) in windows:
        wr = [e for e in run if lo <= e[0] <= hi]
        wpf = len(wr) / max(1, hi - lo + 1)
        print("  window %d-%d: %d changes (%.2f per frame), %d banks mapped" %
              (lo, hi, len(wr), wpf, len(set(e[2] for e in wr))))

    # Misses.
    print()
    hdr = "slots  policy  misses  on  off  frames_on  max/frame"
    for (lo, hi) in windows:
        hdr += "   [%d-%d]%s on/frames" % (lo, hi, " +%d" % warm if warm else "")
    print(hdr)
    for s in slots:
        if s < 4:
            continue
        for pol in ("lru", "opt"):
            m = simulate(ev, s, pol)
            mon = [x for x in m if x[0] in on]
            fon = Counter(x[0] for x in mon)
            pf = Counter(x[0] for x in m)
            row = "%5d  %-6s  %6d  %4d  %4d  %9d  %9d" % (
                s, pol, len(m), len(mon), len(m) - len(mon), len(fon), max(pf.values()) if pf else 0)
            for (lo, hi) in windows:
                wm = [x for x in mon if lo + warm <= x[0] <= hi]
                row += "   %10d/%d" % (len(wm), len(set(x[0] for x in wm)))
            print(row)
            if verbose and pol == "lru":
                wl = ["%d:%d" % x for x in mon[:60]]
                print("        display-on misses: " + " ".join(wl))


if __name__ == "__main__":
    main()
