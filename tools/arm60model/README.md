# ARM60 cycle model

Analysis tool of the project: it runs the code of the ISO, as compiled by
`armcc`, on the PC and predicts its timings on the 3DO before an ISO is
built. It is not part of the ISO and `make` does not use it.

## How it works

- `mk.sh` links the objects of the last `make clean && make` (`build/*.o`)
  with `harness.c` (the driver, compiled by `armcc`) and `hstub.s` (host
  services) into a plain binary (`armlink -bin` at 0x8000, symbols in
  `sim.sym`), and builds `sim` with `gcc`.
- `sim` interprets the binary as an ARMv3 big-endian CPU and counts the S,
  N and I cycles of every instruction with the timings of the ARM60
  datasheet (data processing 1S, `LDR` 1S+1N+1I, `STR` 2N, `LDM` nS+1N+1I,
  `STM` (n-1)S+2N, branch 2S+1N, multiply 1S+mI, unexecuted 1S). The time
  is S x 1.05 + N x 3.2 + I x 1 cycles of 80 ns, weights fitted on Phoenix.
- The OS calls are replaced by host services: `printf`, memory allocation,
  the microsecond clock (modelled time), the ROM image, dumps of the debug
  view (`view_NNNNNN.ppm`) and of the code buffer (`code.bin`).

Accuracy against Phoenix: within 0.3 % on the benchmarks of steps 0-1 and
on the ROM translation, within about 4 % on the cartridge frames (see
`project-context.MD`, step 2). Not modelled: the interrupts of the 3DO OS,
the CEL engine.

## Use

Build the ISO first (`make clean && make` at the root), then:

```
tools/arm60model/mk.sh
cd <a work directory>
S=<repository>/tools/arm60model
$S/sim -img $S/sim.bin -sym $S/sim.sym -bench -frames 0           # benchmarks of steps 0-1
$S/sim -img $S/sim.bin -sym $S/sim.sym -frames 3000 -dump 250 \
       -rom <repository>/takeme/roms/rom.sms                      # cartridge run
```

Options: `-frames n` frames of the cartridge run (after the translation of
the reachable code), `-bench` benchmarks first, `-dump n` debug view every n
frames, `-from n` profile only from frame n, `-rom file`, `-interp` run
everything through the interpreter (nothing translated), `-budget f:c`
override the budget of translations at once (f microseconds per frame
plus c/16 microseconds per interpreted instruction), `-hot q:s` override
the entry counts at which an address is queued (q) and translated at once
(s), `-seed` translate the reachable code into the hot area, `-hotzones n`
give n of the 16 zones to the hot area.

Environment variables:
- `SIMPAD="1600:10,1606:0,2100:8"`: buttons held from each frame on
  (`SMS_PAD_*` bits of `sms.h`, 0x100 for PAUSE);
- `SIMPROF=1`: cycles per function of the image, and of the generated code;
- `SIMDIS=symbol`: executions of each instruction of a function.

Scripts on `code.bin` (written at the end of a cartridge run):
- `hotblocks.py code.bin [rom] [count]`: generated code time per Z80 block;
- `anacode.py code.bin [sim.sym]`: composition of the generated code;
- `disblock.py code.bin pc [key]` or `disblock.py code.bin rank N`: the
  generated code of a block, disassembled, with the cycles spent on each
  instruction.

Pictures are PPM files; any image tool converts them.

Requirements: `gcc`, `python3`, and the compiler tools of the SDK in
`bin/compiler/linux`.

`../z80int_test/` holds the reference Z80 interpreter in C (the assembly
one of `src/z80int_a.s` follows it) with a native ZEXDOC harness; see the
comment at the top of `zex.c` for building and running it.
