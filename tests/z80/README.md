# The Z80 core's net, on the PC

## What it checks

The 79 instruction tests of ZEXALL, replayed against `src/z80.c` **without
running the ROM**, in **both** of the builds the exerciser publishes CRCs for.

Each ZEXALL test is an exhaustive permutation of one instruction family, and
its CRC-32 fingerprint is published in the exerciser's own assembler source.
This bench generates the same cases, runs each one against the real core,
fingerprints the resulting machine state and compares with the published
value. About a tenth of a second — against the hours a console run costs.

    sh tests/z80/run_z80.sh          # or: make test-z80

Expected, on the core as it stands:

    write path + documented flags : failed=0   <- the verdict
    undocumented flags            : failed=0   <- reported

Exit status is non-zero as soon as the **verdict** falls. Nothing here is on
the path of a plain `make`, and nothing here needs the ARM cross compiler.

## The three things it watches, and they are not the same kind of thing

### 1. The state fingerprint — a real external oracle

Sixteen bytes per case (`memop | iy | ix | hl | de | bc | f & mask | a | sp`)
into a reflected CRC-32, compared with a number somebody else published. This
is the part that can say the core is *wrong*.

It runs twice, because ZEXALL publishes two CRCs per test:

| pass | flag mask | which CRC | status |
|---|---|---|---|
| documented | `$D7` | the first | **the verdict** — non-zero exit if it falls |
| undocumented | `$FF` | the second | **reported** — named, not fatal |

The undocumented pass is the only thing in this repository that exercises
**bits 3 and 5 of F**. With the documented table alone, deleting nineteen
deliberate sites of the core — every `Z80_SZ53_MASK` and every `Z80_53_MASK` —
left the bench completely green. It now fails twenty-six descriptors.

`sccf` keeps its own `$D7` mask in **both** passes: the exerciser writes
`.db FlagMask & %11010111`, because SCF/CCF's undocumented flags vary by part
(`zexall.sms.asm:891-899`).

*If the undocumented pass falls, that is a finding to carry up, not something
to fix here — see "Ask First" in the spec.*

### 2. The length fingerprint — a freeze, **not** an oracle

**Read this before trusting it.** Nobody published these numbers. Each is a
CRC-32 over `(uint8)(PC - ZEX_IUT)` for every case of a descriptor — how far
this core moved the program counter — taken **from this core**, on the day
`pclen_baseline.h` was written. It does **not** say the instruction lengths
are right. It says they have not moved.

It exists because the state fingerprint is completely blind to length. The
bench reloads PC for every case, so the final position is never observed. Two
reviewers independently proved the hole by adding a stray PC increment to an
opcode and watching the bench stay green. On a console that is the loudest
failure there is — every following fetch derails; here it was the quietest.

It is kept in its own file, its own CRC, and the documented pass only. It is
never mixed into the ZEXALL fingerprint, which must stay comparable to the
published values.

To re-freeze — and only after deciding, with reasons, that a length change is
*correct*:

    tests/z80/z80_bench --emit-pclen    # paste into pclen_baseline.h

### 3. The write path of the shipped build

The bench is built with **no `-DSMS_MAPPER`**, so it gets `MAPPER_SEGA`, which
is what `src/common.h` gives a build that says nothing — and therefore what
the shipped binary carries. `Z80_WR8` is then the store **followed by the
mapper trigger** (`src/z80_ops.h:119-130`), the macro every emulated write of
the game goes through.

An earlier version of this bench passed `-DSMS_MAPPER=2` and so compiled the
*other* branch — the plain store (`:108-117`) — and announced green on a
variant of the macro nothing ships. `mapper_path_check` now exercises the
trigger directly: a `Z80_WR16` at `$FFFE` must fire it **twice**, low address
first, exactly as `z80_ops.h:79-82` states; and a write at `$FFFB` must not
fire it at all.

## What it does NOT cover

Written without softening, because this project has already paid for a net
presented as wider than it was.

**Covered**, from the 79 labels: 8-bit arithmetic and logic; 8- and 16-bit
loads; 16-bit `ADD`/`ADC`/`SBC`; `INC`/`DEC` in 8 and 16 bits; rotates and
shifts, indexed forms included; `BIT`/`SET`/`RES`; `DAA`, `CPL`, `NEG`,
`SCF`, `CCF`; `RLD`/`RRD`; and the four block families `LDI`/`LDD`/`LDIR`/
`LDDR` and `CPI`/`CPD`/`CPIR`/`CPDR`.

**Not covered at all.** A census over every case the descriptors generate, in
both passes, confirms each of these is *never executed*:

- **the alternate register set** — `EXX` (`$D9`) and `EX AF,AF'` (`$08`) never
  run, and the bench neither seeds nor checks `AF'`/`BC'`/`DE'`/`HL'`;
- **the other exchanges** — `EX DE,HL` (`$EB`), `EX (SP),HL` (`$E3`);
- **`I` and `R`** — neither the registers nor `LD A,I` / `LD A,R`
  (`ED 57`/`ED 5F`);
- **the interrupt flip-flops** `IFF1`/`IFF2`, and `DI`/`EI` (`$F3`/`$FB`);
- **`IM 0/1/2`** (`ED 46`/`ED 56`/`ED 5E`);
- **the stack** — `PUSH`/`POP`;
- **control flow** — `CALL`, `RET`, `RST`, `JP`, `JR`, `DJNZ`;
- **I/O** — `IN`/`OUT`, `IN/OUT (C)`, and the block forms `INI`/`INIR`/
  `IND`/`INDR`/`OUTI`/… ;
- **`HALT`** — deliberately counted and skipped by the exerciser itself.

And three properties rather than instructions:

- **instruction length** — only the freeze above, which is not an oracle;
- **timing** — nothing checks T-state counts. The one exception is the
  documented bound on what `z80_run` hands back (never negative, never above
  22), asserted on every one of the million instructions;
- **interrupts and paging** — beyond the write-path check above, nothing.

It is a real net over instruction semantics. It is not "the net of the Z80
core".

## What it compiles

The real files of the repository — `src/z80.c`, `src/z80_ops.h`, `src/sms.c` —
never a copy. `#include "z80_ops.h"` from `src/z80.c` resolves in `src/`
first, so no `-I` can substitute a header that lives there. Only the four 3DO
SDK headers `src/common.h` pulls in are replaced, from `3do/`, whose `-I`
comes first — and the script checks that none of them shares a name with a
real header of `src/`.

Built with `-Wall -Wextra`, not `-w`: this is the only place in the project
where a host compiler ever sees `src/z80.c` and `src/sms.c`, and that is a
second compiler's opinion for free. All four units are clean today.

Every build switch the bench's claims depend on is **pinned on the command
line**, and `bench_z80.c` carries a matching `#error` for each, so a build
that loses one fails loudly instead of measuring something else:
`SMS_MAPPER` (by absence), `LOG_LEVEL=0`, `SMS_IRQ_TEST_SOURCE=0`,
`SMS_TELEMETRY=1`, and the three `SMS_DYNAREC_J*=0`. Two of those would
silently change the measurement: `z80_run` samples both interrupt lines on
**every** call — here, every instruction — so an armed test source would
inject NMIs in the middle of a descriptor; and a dynarec flag would mean the
thing measured is no longer `src/z80.c` as it stands.

`LOG_LEVEL` is **0**, errors only. An earlier version said level 3 was needed
to read the core's "unimplemented opcode" line; that was wrong — the line is
`LOG_LVL_ERR`, which level 0 carries. What level 3 added was `LOG_INFO`, and
`z80_reset` emits one per stopped case, which this bench triggers by design:
measured, the trace ring then held sixty-three copies of `reset pc=… sp=…` and
the useful line had scrolled out.

## Diagnosing a failure

- The trace ring keeps the **last** lines, not the first: the end is what
  diagnoses.
- The stopping opcode is decoded and printed **by the bench, per descriptor**.
  It does not rely on the core's own trace: `LOG_ONCE` gives that line to the
  first test of the whole process and to no other, so a bench that leaned on
  it would diagnose exactly one descriptor per run.
- When the core stopped, the fingerprint is printed as `crc=(void)` and said
  to be void — the state after a stop is whatever `z80_reset` left behind, and
  presenting it as evidence would be a lie.
- A runaway — an instruction that never leaves its address — names its case
  and its opcode, resets the core, and **abandons the descriptor** instead of
  burning the repeat bound on every remaining case.

## The committed tables, and why they are checked

`zexall_tests.c` holds **both** tables and is generated and committed. That is
deliberate: the repository does not track `docs/`, so a bench that parsed the
assembler at start-up would not run on a fresh clone — which is how the first
host bench of this core was lost.

Editing a CRC there by hand is the cheapest way to make a red bench go green
for ever, and the SHA-256 in the file header is only a comment. So
`run_z80.sh` **regenerates into a temporary directory and diffs** whenever
`docs/` is present, and says plainly when it is not.

    python3 tests/z80/gen_tests.py \
        docs/sms_gg/ZEXALL-SMS-0.21/source/zexall.sms.asm tests/z80

The generator also re-derives `2^c` and `d+1` for every mask and checks them
against the counts the assembler states in its own comments — **strictly**: a
comment it cannot read is an error, not something to skip. Three cases arise,
and the third is why this is not one line:

- the comment states a pair (`70/72 bits -> 71/73`): both builds checked;
- it states one figure for a mask the build does not change: that figure
  checks both builds;
- it states one figure for a mask the build **does** change: that figure is
  the documented build's, the undocumented one was never written down. Those
  32 are frozen in `UNSTATED_UNDOC` so a reformat of the exerciser trips the
  check instead of quietly shrinking it.

One comment of the exerciser is simply wrong — `adc16` reads
`70/72 bits -> 71/74`, and 72 bits is 73 passes, not 74. It is the only one,
and it is listed in `COMMENT_ERRATA` rather than accommodated silently.

Those stated counts then travel **into the table** and are compared against
the bench's own case count at run time. The previous check compared the count
against a product recomputed from the very masks the engine had just walked,
which could never fail.

## Sweeping candidate rules — and the trap that voids the sweep

The procedure that isolated the `daa` rule in one session: keep a variant of
`z80_ops.h`, build the bench against it, read which tests move.

**`-I` cannot do this.** The compiler resolves `#include "z80_ops.h"` in the
directory of the including file first — `src/`. A `-I` pointing at a variant
header is silently ignored, and the sweep measures the repository's own file
while believing it measures the variant.

So the variant header has to sit **beside a copy of `z80.c`**:

    mkdir -p /tmp/sweep
    cp src/z80.c src/z80_ops.h /tmp/sweep/     # then edit /tmp/sweep/z80_ops.h
    gcc -O2 -std=gnu89 -Wall -Wextra -DLOG_LEVEL=0 -DSMS_IRQ_TEST_SOURCE=0 \
        -DSMS_TELEMETRY=1 -DSMS_DYNAREC_J0=0 -DSMS_DYNAREC_J1=0 \
        -DSMS_DYNAREC_J2=0 \
        -Itests/z80/3do -I/tmp/sweep -Isrc -Itests/z80 -o /tmp/sweep/bench \
        tests/z80/bench_z80.c tests/z80/zexall_tests.c \
        /tmp/sweep/z80.c src/sms.c
    /tmp/sweep/bench

Verified in both directions: with that same `-I`, compiling `src/z80.c` gives
a green bench while compiling the **copy** beside the mutated header fails.
The copy is the net, not the `-I`.

## The five things that make a replay wrong in silence

All five were found by reproducing the fingerprints, none was guessed. They
are written out at the head of `bench_z80.c`; in short:

1. A counter bit **flips** the base state's bit, it does not overwrite it.
2. The shifter is a single bit walking left, not a count — its last pass flips
   nothing, which is where the `+1` of `2^c × (d+1)` comes from.
3. The very first case of a test is asymmetric: the base state is played as it
   stands, counter at zero **and** shifter not applied.
4. `$C070` is inside the fingerprints: several descriptors use the address of
   the input structure as the value of `hl`/`ix`/`iy`.
5. The core's stop flag only falls on `z80_reset`, so a case that meets an
   unimplemented opcode falsifies every case after it unless the core is
   reset.

And a sixth, which belongs to this core rather than to ZEXALL: an index prefix
with nothing to substitute is **absorbed on its own dispatch turn**
(`src/z80.c:1735-1770`), so one call of `z80_run(1)` returns between the prefix
and the instruction behind it. That is deliberate — it is what stops the core
swallowing a displacement byte that was never emitted — and the `ld8rrx`
descriptor sweeps that range on purpose. The bench therefore runs a case until
PC has left the instruction's bytes having consumed something other than a
prefix. Stopping at the prefix reports a correct core as broken.

## Files

| File | What it is |
|---|---|
| `bench_z80.c` | the bench: memory, permutation engine, both fingerprints, stubs |
| `zexall_tests.c` / `.h` | the 79 descriptors in both builds, generated and committed |
| `pclen_baseline.h` | the instruction-length freeze — a freeze, not an oracle |
| `gen_tests.py` | rebuilds the tables from the assembler source |
| `run_z80.sh` | one command: check, build, run |
| `3do/` | the four SDK header substitutes `src/common.h` pulls in |
