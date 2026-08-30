#!/usr/bin/env python3
"""Turns the ZEXALL descriptors into the C tables the host bench compiles.

The repository does not track docs/, so a bench that read the assembler at
start-up would not run on a fresh clone -- which is how the first host bench
of this core was lost.  The tables are therefore generated once, committed,
and carry the digest of the file they came from.  run_z80.sh regenerates and
diffs them whenever docs/ is present, so the digest is checked and not merely
displayed.

    python3 tests/z80/gen_tests.py \\
        docs/sms_gg/ZEXALL-SMS-0.21/source/zexall.sms.asm tests/z80

TWO tables come out, because the exerciser publishes TWO CRCs per test:

  documented    FlagMask $D7, the first CRC, the order of the .else branch
  undocumented  FlagMask $FF, the second CRC, the order of the .ifdef branch

The second one is the only thing in this repository that exercises bits 3 and
5 of F.  Emitting only the first left nineteen deliberate sites of the core
unguarded, which a reviewer proved by deleting them all with the bench still
green.
"""

import hashlib
import os
import re
import sys

# The RAM address of MachineStateBeforeTest.  It is not a free choice: several
# descriptors use it as the value of hl/ix/iy, so it is baked into the CRCs.
MSBT = 0xC070
MSBT_FIELDS = {
    "memop": 0, "iy": 2, "ix": 4, "hl": 6,
    "de": 8, "bc": 10, "f": 12, "a": 13, "sp": 14,
}

# zexall.sms.asm:183-187.  The mode decides FlagMask, which appears both as a
# mask byte inside the descriptors and as the .db that follows each CRC.
MODES = {
    "doc":   {"flag_mask": 0xD7, "crc_index": 0, "branch": "else",  "figure": 0},
    "undoc": {"flag_mask": 0xFF, "crc_index": 1, "branch": "ifdef", "figure": 1},
}

TEST_COUNT = 79

# One comment of the assembler contradicts its own masks, and it is a typo in
# the source rather than something to accommodate silently: adc16's shifter is
# 64 bits plus FlagMask, so 70 bits (71 passes) documented and 72 bits (73
# passes) undocumented -- the comment reads "70/72 bits -> 71/74 permutations".
# The bit counts are right, 74 is not.  It is the ONLY comment that states a
# figure for both builds and states one of them wrongly.
COMMENT_ERRATA = {
    ("adc16", "undoc", "shifter"): (74, 73),
}

# Where the assembler states ONE figure for a mask that the build changes.
# That figure is the documented build's; the undocumented one is simply not
# written down, so there is nothing independent to check it against and the
# case total cannot be carried into the table for it either.  The list is
# frozen so that a reformat of the exerciser's comments trips the check
# instead of quietly shrinking it.
UNSTATED_UNDOC = [
    ("alu8r_c", "shifter"), ("alu8r_d", "shifter"), ("alu8r_e", "shifter"),
    ("alu8r_h", "shifter"), ("alu8r_hl", "shifter"), ("alu8r_l", "shifter"),
    ("alu8rx_ixl", "shifter"), ("alu8rx_iyh", "shifter"),
    ("alu8rx_iyl", "shifter"),
    ("cpi1", "shifter"), ("incb", "shifter"), ("incc", "shifter"),
    ("incd", "shifter"), ("incde", "shifter"), ("ince", "shifter"),
    ("inch", "shifter"), ("inchl", "shifter"), ("incix", "shifter"),
    ("inciy", "shifter"), ("incl", "shifter"), ("incm", "shifter"),
    ("incsp", "shifter"), ("incx", "shifter"), ("incxh", "shifter"),
    ("incxl", "shifter"), ("incyh", "shifter"), ("incyl", "shifter"),
    ("ld8bd", "shifter"), ("ldd2", "shifter"), ("ldi1", "shifter"),
    ("ldi2", "shifter"), ("sccf", "counter"),
]

# How many argument slots each macro takes, and how they land in the 20 byte
# record (zexall.sms.asm:534-590).  "op" slots are single opcode bytes, "lo"
# and "hi" the halves of a 16 bit argument, "z" a hardwired zero, and the nine
# trailing slots are always the machine state.
MACROS = {
    "TestData":     (13, ["op", "op", "op", "op"]),
    "TestData1":    (10, ["op", "z", "z", "z"]),
    "TestData2":    (11, ["op", "op", "z", "z"]),
    "TestData3":    (12, ["op", "op", "op", "z"]),
    "TestData3_16": (11, ["op", "lo", "hi", "z"]),
    "TestData4_16": (12, ["op", "op", "lo", "hi"]),
}


def evaluate(expr, flag_mask):
    """Evaluates one WLA DX argument to an integer."""
    text = expr.strip()
    if not text:
        raise ValueError("empty argument")
    text = re.sub(r"\$([0-9a-fA-F]+)", lambda m: str(int(m.group(1), 16)), text)
    text = re.sub(r"%([01]+)", lambda m: str(int(m.group(1), 2)), text)
    text = re.sub(r"\bMachineStateBeforeTest\.([a-z]+)\b",
                  lambda m: str(MSBT + MSBT_FIELDS[m.group(1)]), text)
    text = re.sub(r"\bMachineStateBeforeTest\b", str(MSBT), text)
    text = re.sub(r"\bFlagMask\b", str(flag_mask), text)
    if re.search(r"[A-Za-z_]", text):
        raise ValueError("unresolved symbol in %r" % expr)
    # No "/" in the guard: WLA DX has no division here, and admitting one
    # would let a truncating divide pack a wrong byte without a word.
    if not re.match(r"^[0-9+\-*&|()^ ]+$", text):
        raise ValueError("unexpected characters in %r" % expr)
    return int(eval(text, {"__builtins__": {}}, {}))


def split_args(text):
    """Splits a macro argument list, dropping the trailing comment."""
    body = text.split(";", 1)[0]
    return [a for a in (p.strip() for p in body.split(",")) if a != ""]


def pack(macro, args, flag_mask):
    """Turns one TestData* line into the 20 bytes of a TestCase."""
    count, shape = MACROS[macro]
    if len(args) != count:
        raise ValueError("%s wants %d arguments, got %d" %
                         (macro, count, len(args)))
    values = [evaluate(a, flag_mask) for a in args]
    taken = 0
    opcode = []
    for slot in shape:
        if slot == "z":
            opcode.append(0)
        elif slot == "op":
            opcode.append(values[taken] & 0xFF)
            taken += 1
        elif slot == "lo":
            opcode.append(values[taken] & 0xFF)
        else:  # "hi": same argument as the "lo" that precedes it
            opcode.append((values[taken] >> 8) & 0xFF)
            taken += 1
    state = values[taken:]
    if len(state) != 9:
        raise ValueError("%s: %d machine state slots" % (macro, len(state)))
    memop, iy, ix, hl, de, bc, f, a, sp = state
    out = list(opcode)
    for word in (memop, iy, ix, hl, de, bc):
        out.append(word & 0xFF)
        out.append((word >> 8) & 0xFF)
    out.append(f & 0xFF)
    out.append(a & 0xFF)
    out.append(sp & 0xFF)
    out.append((sp >> 8) & 0xFF)
    return out


def read_order(lines, branch):
    """The .dw list of one branch of the Tests table (zexall.sms.asm:530-536)."""
    for i, line in enumerate(lines):
        if line.strip() != "Tests:":
            continue
        seen = None
        for follow in lines[i + 1:i + 12]:
            stripped = follow.strip()
            if stripped.startswith(".ifdef"):
                seen = "ifdef"
            elif stripped.startswith(".else"):
                seen = "else"
            elif stripped.startswith(".endif"):
                seen = None
            elif stripped.startswith(".dw") and seen == branch:
                names = split_args(stripped[3:])
                if names != ["0"]:
                    return names
        break
    raise ValueError("could not read the %s branch of the Tests table" % branch)


def parse(path, mode):
    """Reads every descriptor for one mode."""
    conf = MODES[mode]
    flag_mask = conf["flag_mask"]

    with open(path, "r") as handle:
        lines = handle.read().split("\n")

    order = read_order(lines, conf["branch"])

    tests = {}
    current = None
    in_macro = False
    label = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*):\s*$")
    for line in lines:
        stripped = line.split(";", 1)[0].strip()
        if not stripped:
            continue
        # The macro bodies use \1, \2 ... as arguments: they define the shape
        # read from MACROS above, they are not descriptors.
        if stripped.startswith(".macro"):
            in_macro = True
            continue
        if stripped.startswith(".endm"):
            in_macro = False
            continue
        if in_macro:
            continue
        matched = label.match(stripped)
        if matched:
            current = {"rows": [], "notes": [], "crcs": None,
                       "mask": None, "name": None}
            tests[matched.group(1)] = current
            continue
        if current is None:
            continue
        head = stripped.split(None, 1)[0]
        if head in MACROS:
            # A fourth row would be silently dropped by a "< 3" guard, and a
            # dropped row is a wrong descriptor that still compiles.
            if len(current["rows"]) >= 3:
                raise ValueError("a descriptor carries more than three "
                                 "TestData rows: %s" % stripped[:60])
            current["rows"].append(pack(head, split_args(stripped[len(head):]),
                                        flag_mask))
            # The assembler states what each mask is worth in a trailing
            # comment; that figure is the independent check of the engine.
            current["notes"].append(line.split(";", 1)[1] if ";" in line else "")
        elif head == "CRCs":
            values = stripped[4:].replace(",", " ").split()
            if len(values) != 2:
                raise ValueError("CRCs wants two values: %s" % stripped)
            current["crcs"] = [evaluate(v, flag_mask) for v in values]
        elif head == ".db" and current["crcs"] is not None \
                and current["mask"] is None:
            # sccf is the one descriptor with a mask of its own: the assembler
            # writes ".db FlagMask & %11010111", so it stays $D7 in both modes.
            current["mask"] = evaluate(stripped[3:], flag_mask) & 0xFF
        elif head == "MessageString":
            quoted = re.search(r'"([^"]*)"', stripped)
            if quoted:
                current["name"] = quoted.group(1)

    out = []
    for name in order:
        test = tests.get(name)
        if test is None:
            raise ValueError("test %s is in the table but has no descriptor" % name)
        if len(test["rows"]) != 3 or test["crcs"] is None \
                or test["mask"] is None or test["name"] is None:
            raise ValueError("test %s is incomplete" % name)
        test["label"] = name
        test["base"], test["counter"], test["shifter"] = test["rows"]
        test["crc"] = test["crcs"][conf["crc_index"]]
        out.append(test)

    if len(out) != TEST_COUNT:
        raise ValueError("expected %d descriptors, read %d"
                         % (TEST_COUNT, len(out)))
    return out


def popcount(mask):
    return sum(bin(byte).count("1") for byte in mask)


STATED = re.compile(r"->\s*(\d+)(?:\s*/\s*(\d+))?")


def stated_figure(note, which):
    """The count the assembler states, "a/b" resolved for this mode."""
    found = STATED.search(note or "")
    if not found:
        return None
    pair = found.groups()
    if which == 1 and pair[1] is not None:
        return int(pair[1])
    return int(pair[0])


def mode_dependent(tables):
    """Which mask rows the build actually changes, by (label, field)."""
    by_label = {}
    for mode, tests in tables.items():
        for test in tests:
            by_label.setdefault(test["label"], {})[mode] = test
    out = set()
    for label, pair in by_label.items():
        for field in ("counter", "shifter"):
            if pair["doc"][field] != pair["undoc"][field]:
                out.add((label, field))
    return out


def cross_check(tables):
    """Checks 2^c and d+1 against the assembler's own comments, strictly.

    Three cases, and the third is why this is not one line:

      - the comment states a pair ("70/72 bits -> 71/73"): both builds are
        written down, both are checked;
      - the comment states one figure for a mask the build does not change:
        that one figure checks both builds;
      - the comment states one figure for a mask the build DOES change: the
        figure is the documented build's, the undocumented one was never
        written down, and there is nothing to check it against.  Those are
        frozen in UNSTATED_UNDOC so that a reformat of the exerciser trips
        this check instead of quietly shrinking it.

    A comment that cannot be read at all is an error, not something to skip:
    skipping turns the only independent check of the permutation engine into
    a number that looks reassuring and proves nothing.
    """
    varies = mode_dependent(tables)
    unstated = []
    checked = {"doc": 0, "undoc": 0}

    for mode, tests in tables.items():
        which = MODES[mode]["figure"]
        for test in tests:
            for field, note, want in (
                    ("counter", test["notes"][1],
                     1 << popcount(test["counter"])),
                    ("shifter", test["notes"][2],
                     popcount(test["shifter"]) + 1)):
                found = STATED.search(note or "")
                if not found:
                    raise ValueError("%s: the %s comment states no count (%r)"
                                     % (test["label"], field, note))
                pair = found.groups()
                if mode == "undoc" and pair[1] is None \
                        and (test["label"], field) in varies:
                    unstated.append((test["label"], field))
                    continue
                got = stated_figure(note, which)
                erratum = COMMENT_ERRATA.get((test["label"], mode, field))
                if erratum is not None and got == erratum[0]:
                    got = erratum[1]
                if got != want:
                    raise ValueError("%s/%s: the %s mask is worth %d, the "
                                     "assembler says %d"
                                     % (test["label"], mode, field, want, got))
                checked[mode] += 1

    if checked["doc"] != 2 * TEST_COUNT:
        raise ValueError("documented cross-check covered %d figures, "
                         "expected %d" % (checked["doc"], 2 * TEST_COUNT))
    if sorted(unstated) != sorted(UNSTATED_UNDOC):
        raise ValueError(
            "the set of figures the assembler leaves unstated for the "
            "undocumented build has moved.\nnow: %s" % sorted(unstated))
    if checked["undoc"] + len(unstated) != 2 * TEST_COUNT:
        raise ValueError("undocumented cross-check covered %d + %d figures, "
                         "expected %d"
                         % (checked["undoc"], len(unstated), 2 * TEST_COUNT))
    return checked, unstated


def stated_cases(test, mode, varies):
    """The case total the assembler states, or 0 when it states none.

    0 travels into the table on purpose: the bench compares its own count
    against this figure and skips -- loudly -- the descriptors where the
    exerciser never wrote one down.  Recomputing it from the masks would make
    the comparison a tautology, which is exactly the defect being fixed.
    """
    which = MODES[mode]["figure"]
    total = 1
    for field, note in (("counter", test["notes"][1]),
                        ("shifter", test["notes"][2])):
        found = STATED.search(note or "")
        pair = found.groups()
        if mode == "undoc" and pair[1] is None \
                and (test["label"], field) in varies:
            return 0
        figure = stated_figure(note, which)
        erratum = COMMENT_ERRATA.get((test["label"], mode, field))
        if erratum is not None and figure == erratum[0]:
            figure = erratum[1]
        total *= figure
    return total


def computed_cases(test):
    """2^c x (d+1) from the masks themselves -- the figure the bench will
    actually run.  Used for the totals a reader wants; never for the check,
    which must come from the assembler's independent word."""
    return (1 << popcount(test["counter"])) * (popcount(test["shifter"]) + 1)


def emit(tables, digest, source, out_dir, varies):
    totals = {}
    for mode, tests in tables.items():
        totals[mode] = sum(computed_cases(t) for t in tests)

    header = os.path.join(out_dir, "zexall_tests.h")
    with open(header, "w") as handle:
        handle.write("""/* Generated by tests/z80/gen_tests.py -- do not edit by hand.
 *
 * The %d instruction descriptors of ZEXALL, in both of the exerciser's two
 * builds.  See zexall_tests.c for the digest of the assembler file they were
 * read from and the command that rebuilds them; run_z80.sh checks that digest
 * by regenerating whenever docs/ is present.
 */
#ifndef SMS3DO_ZEXALL_TESTS_H
#define SMS3DO_ZEXALL_TESTS_H

/* Four opcode bytes then the sixteen of a MachineState, little endian:
 * memop, iy, ix, hl, de, bc, f, a, sp (zexall.sms.asm:128-147). */
#define ZEX_CASE_BYTES 20

typedef struct
{
  const char    *label;        /* the assembler label, e.g. "cplop" */
  const char    *name;         /* what the exerciser prints, e.g. "cpl" */
  unsigned char  base[ZEX_CASE_BYTES];
  unsigned char  counter[ZEX_CASE_BYTES];
  unsigned char  shifter[ZEX_CASE_BYTES];
  unsigned long  crc;          /* the CRC published for this build */
  unsigned long  stated_cases; /* the case total the ASSEMBLER'S COMMENT
                                * states -- an independent figure, not one
                                * recomputed from the masks beside it */
  unsigned char  fmask;        /* this descriptor's own flag mask */
} zex_test_t;

#define ZEX_TEST_COUNT %d

/* The documented build: flag mask $D7, the first of each pair of published
 * CRCs.  This is the verdict. */
extern const zex_test_t zex_tests_doc[ZEX_TEST_COUNT];

/* The undocumented build: flag mask $FF, the second CRC, and the exerciser's
 * own ordering for that build.  The only thing here that exercises bits 3
 * and 5 of F. */
extern const zex_test_t zex_tests_undoc[ZEX_TEST_COUNT];

#endif /* SMS3DO_ZEXALL_TESTS_H */
""" % (TEST_COUNT, TEST_COUNT))

    body = os.path.join(out_dir, "zexall_tests.c")
    with open(body, "w") as handle:
        handle.write("""/* Generated by tests/z80/gen_tests.py -- do not edit by hand.
 *
 * Source:  %s
 * SHA-256: %s
 * Rebuild: python3 tests/z80/gen_tests.py \\
 *              %s tests/z80
 *
 * Committed on purpose.  The repository does not track docs/, so a bench that
 * parsed the assembler at start-up would not run on a fresh clone -- which is
 * how the first host bench was lost.  Only the generator needs docs/; the
 * bench needs this file.
 *
 * Editing a CRC here by hand is the cheapest way to make a red bench go green
 * for ever, so the digest above is not decoration: run_z80.sh regenerates
 * into a temporary directory and diffs against this file whenever docs/ is
 * present, and says so plainly when it is not.
 *
 * Case totals: documented %d, undocumented %d.
 */
#include "zexall_tests.h"

""" % (source, digest, source, totals["doc"], totals["undoc"]))

        for mode, symbol in (("doc", "zex_tests_doc"),
                             ("undoc", "zex_tests_undoc")):
            handle.write("const zex_test_t %s[ZEX_TEST_COUNT] =\n{\n" % symbol)
            tests = tables[mode]
            for index, test in enumerate(tests):
                handle.write("  /* %s -- %d cases */\n  {\n"
                             % (test["name"], computed_cases(test)))
                handle.write('    "%s", "%s",\n' % (test["label"], test["name"]))
                for field in ("base", "counter", "shifter"):
                    handle.write("    { %s },\n" % ", ".join(
                        "0x%02X" % byte for byte in test[field]))
                handle.write("    0x%08XUL, %dUL, 0x%02X\n"
                             % (test["crc"], stated_cases(test, mode, varies),
                                test["mask"]))
                handle.write("  }%s\n" % ("," if index + 1 < len(tests) else ""))
            handle.write("};\n\n")

    return totals


def main(argv):
    if len(argv) != 3:
        sys.stderr.write("usage: gen_tests.py <zexall.sms.asm> <output dir>\n")
        return 2
    source, out_dir = argv[1], argv[2]
    if not os.path.isfile(source):
        sys.stderr.write(
            "gen_tests.py: cannot read %s\n"
            "The assembler lives under docs/, which git does not track, so a\n"
            "fresh clone does not have it.  Only this generator needs it: the\n"
            "bench compiles against the committed tests/z80/zexall_tests.c.\n"
            "Fetch ZEXALL-SMS 0.21 and point this script at its source file.\n"
            % source)
        return 1

    with open(source, "rb") as handle:
        digest = hashlib.sha256(handle.read()).hexdigest()

    # The name written into the generated files must not depend on how this
    # script was invoked: run_z80.sh regenerates into a temporary directory
    # and diffs, so an absolute path here would make the two differ for a
    # reason that has nothing to do with the descriptors.
    shown = source.replace(os.sep, "/")
    marker = "docs/"
    if marker in shown:
        shown = shown[shown.index(marker):]

    tables = {}
    for mode in ("doc", "undoc"):
        tables[mode] = parse(source, mode)

    varies = mode_dependent(tables)
    checked, unstated = cross_check(tables)

    totals = emit(tables, digest, shown, out_dir, varies)

    for mode in ("doc", "undoc"):
        carried = sum(1 for t in tables[mode]
                      if stated_cases(t, mode, varies) != 0)
        sys.stdout.write(
            "%-5s %d descriptors, %d cases, %d/%d comment figures checked, "
            "%d/%d case totals carried\n"
            % (mode, len(tables[mode]),
               sum(computed_cases(t) for t in tables[mode]), checked[mode],
               2 * TEST_COUNT, carried, TEST_COUNT))

    sys.stdout.write("sha256=%s\n" % digest)
    sys.stdout.write("assembler comments: %d erratum, %d figures never stated "
                     "for the undocumented build\n"
                     % (len(COMMENT_ERRATA), len(unstated)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
