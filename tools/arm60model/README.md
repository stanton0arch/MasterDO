# ARM60 cycle model

Analysis tool of the project: it runs the code of the ISO, as compiled by
`armcc`, on the PC and predicts its timings on the 3DO before an ISO is
built. It is not part of the ISO and `make` does not use it.

## How it works

- `mk.sh` links the objects of the last `make clean && make` (`build/*.o`)
  with `harness.c` (the driver), `dectest.c`, `anatest.c` and `emittest.c`
  (the checks of the assembly decoder, analysis passes and code emitter
  against their C references), all compiled by `armcc`, and `hstub.s` (host services) into a plain binary (`armlink -bin` at 0x8000, symbols in
  `sim.sym`), and builds `sim` with `gcc`.
- `sim` interprets the binary as an ARMv3 big-endian CPU and counts the S,
  N and I cycles of every instruction with the timings of the ARM60
  datasheet (data processing 1S, `LDR` 1S+1N+1I, `STR` 2N, `LDM` nS+1N+1I,
  `STM` (n-1)S+2N, branch 2S+1N, multiply 1S+mI, unexecuted 1S). The time
  is S x 1.05 + N x 3.2 + I x 1 cycles of 80 ns, weights fitted on Phoenix.
- The OS calls are replaced by host services: `printf`, memory allocation,
  the microsecond clock (modelled time), the ROM image, dumps of the debug
  view (`view_NNNNNN.ppm`) and of the code buffer (`code.bin`). The
  allocations are counted against the memory Phoenix leaves to a
  cartridge's run (1 232 640 bytes of DRAM, 571 392 bytes of VRAM less the
  image): an allocation past it fails as `AllocMem` does, and the end of a
  run prints what is left.

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
(s), `-force n` the entry count from which an address is translated
whatever the budget, `-forcelimit us` the elapsed time of a frame past
which that rule waits for the next frame (0: no limit), `-zones n` the
zones of the code buffer (default: as many as the DRAM left allows),
`-dramfree n` the DRAM left to the run, `-seed` translate the reachable
code into the hot area, `-hotzones n`
(kept for older scripts: the zones are no longer split between the areas,
any nonzero value gives the hot area its own zone), `-check` verify the
translator's block table after each frame (hashed blocks with their code,
RAM entry headers, zone counts, lookup entries) and report the first
inconsistencies; the check runs in the model and shifts its clock, so a
timing-dependent run may not reproduce with it. `-hash` prints a hash of
the machine state (Z80 RAM and registers, VDP registers, video and colour
RAM) every 100 frames, `-hashevery n` every n frames: two builds that
should behave alike are compared with it frame by frame (the run's own
timing decides what is translated when, and translated and interpreted
code take interrupts at different boundaries, so a game can take another
course without any fault; a change of course right from the first frames
is a fault).

The frames compared with the reference view are those at multiples of
the dump period, those with a scroll, display or colour band, and the
first frame with the display on after frames without it (the picture
caught up in their spare time). A frame with the display off is charged
no drawing time, as on the console, so its spare time goes to the
picture's catch-up and to translation like the ISO's.

A `.gg` image runs in the Game Gear mode (12-bit colours, the 160 x 144
LCD window, ports $00-$06, START on the PAUSE bit of the pad script), as
the ISO decides it from the file name; `-gg` and `-sms` force the mode.
In that mode the pictures compared and dumped are the LCD window, and the
drawing time charged to a frame is the 3.5 ms scaled by the pixels of
the window (an estimate until measured on the console).

`-framelog` prints one `Frame:` line per frame (emulation, update and
drawing times, spare time used, translations at once and their time,
forced ones, refusals, blocks translated in spare time, interpreted
instructions, display on, VDP data writes, dirty tiles left at the end
of the frame, code buffer evictions, those made ahead in spare time and
their time); `present.py` replays those lines through the queued
presentation of the ISO.

`-dumpdiff n` with `-dump`: the frames compared because of a scroll or
display band are only written when they differ, the first n of them; the
pictures at multiples of the dump period are always written (a raster game
compares thousands of frames per run).

`-cels n` prints the sprites of frame n as the VDP evaluated them, the
register 8 and colour RAM writes of the frame with their lines, the
cel chain (position, size, bits per pixel) and, when the priority layer
was drawn under the sprites, the strips it replaced. `-rasterlog`
prints, for every frame that has some, the writes made during the
active display (scroll, name table, display enable, colours) with the
first line they affect (`Raster:` lines). `-celhash` prints a hash of the
cel chain of every frame with the display on (`Cels:` lines: every field of
every cel, in the order of the chain, the source and PLUT pointers taken as
offsets into the renderer's buffers so that two builds allocating them
elsewhere compare; then a hash of the picture the chain draws through the
software cel engine, which compares the pixels of the bitmaps it shows), to
compare the cels two builds make of the same machine state frame by frame
(with `-interp`, so that the translation decisions cannot change the course
of the game). The harness also checks the assembly sprite
evaluation of every frame against a C reference and reports the frames
with differences (`Sprites:` lines).

`-dectest` (no cartridge needed) checks the instruction decoder in assembly
(`src/z80jit_dec.s`) against the C decoder it replaced, kept in
`dectest.c`: `jit_decode` on every pair of opcode bytes (operand bytes,
page ends and the wrap at $FFFF included), then `jit_decode_block` on
60 000 random blocks over pages of every kind (fixed, paged slots, RAM
with a mirror), comparing the number of instructions, the next address,
the flags, every word set per instruction and the instruction map. It
prints two `Decoder test:` lines with the differences found (0 expected).

`-anatest` (no cartridge needed) checks the analysis passes of the
translator in assembly (`src/z80jit_ana.s`) against the C passes they
replaced, kept in `anatest.c`: 60 000 random blocks built from instruction
templates (jumps and calls inside and outside the block, flag producers
before conditional jumps, OUTI / OUTD / OUT (C),r runs and loops, busy-wait
loops reading memory or a line port, EI, BIT, block instructions) over
pages of every kind are decoded by `jit_decode_block` and analysed both
ways from the same records; the fields the analysis writes (internal
targets, busy-wait loops, runs, segment starts, fused tests, flag
liveness, segment T-states) and the counts are compared. It prints one
`Analysis test:` line with the differences found (0 expected).

`-emittest` (no cartridge needed) checks the code emitter in assembly
(`src/z80jit_emt.s`) against the C emitter it replaced, kept in
`emittest.c`: 60 000 random blocks (the instruction templates of the
analysis test, with random bytes sprinkled in for the other opcode forms)
over pages of every kind (fixed, paged slots with large bank numbers, RAM
with a mirror, mapped near the context or out of reach of the fixed
offsets) are decoded and analysed once, then emitted twice at the same
address, by the C and by the assembly; the generated words, the code
address of each instruction, the body address, the number of stubs, the
error and the count of full flag computations are compared. It prints one
`Emitter test:` line with the differences found (0 expected); the first
differing blocks are dumped word by word.

A hang is located without a debugger: `kill -USR1 <pid of sim>` prints the
simulated pc and lr with their symbols, the stack pointer, r0 and r1.

Environment variables:
- `SIMPAD="1600:10,1606:0,2100:8"`: buttons held from each frame on
  (`SMS_PAD_*` bits of `sms.h`, 0x100 for PAUSE, or START held on the Game
  Gear), up to 1 024 entries.
  The ISO records the pad changes of a run and writes them at the pause
  or the end of the run as a `Pad script:` line holding a `panel.txt`
  entry (cartridge, frames, script), so that a run played on the console
  or on Phoenix is replayed here;
- `SIMPROF=1`: cycles per function of the image, and of the generated code
  (the 40 largest; `SIMPROFALL=1` lists every function with cycles);
- `SIMDIS=symbol`: executions of each instruction of a function.

Scripts on `code.bin` (the whole code buffer and the block descriptors,
written at the end of a cartridge run) and `codecnt.bin` (executions of
each code word since the `-from` frame):
- `hotblocks.py code.bin [rom] [count]`: generated code time per Z80 block;
- `anacode.py code.bin [sim.sym]`: composition of the generated code;
- `disblock.py code.bin pc [key]` or `disblock.py code.bin rank N`: the
  generated code of a block, disassembled, with the cycles spent on each
  instruction;
- `entries.py code.bin codecnt.bin [frames] [count]`: block entries (the
  executions of the first word of each block's body, where the links of
  other blocks lead) in all and per frame, and the most entered blocks.

Pictures are PPM files; any image tool converts them.

## Batch runner

`batch.py` runs every cartridge of `takeme/roms` in the model, in parallel,
with the frame count and the pad script of `panel.txt` (cartridges absent
from it get 3 000 frames and button 1 tapped every 300 frames), and writes a
one-page report in `reports/` (`<date>-<commit>.txt`, with the data in a
`.json` next to it): a table with one line per cartridge (status, emulation,
update and total times per frame, frames over budget, worst frame, pictures
identical to the reference view, sprite evaluation checks, the mechanisms the
game uses), then per cartridge the ROM translation, the times over the run
and across the 100-frame windows, the busiest window, the worst frames and
what they did, the per-frame VDP and translator counters, the picture
counters and the comparison with the reference view; and at the end the
changes against the previous report of the directory (or `-against FILE`).
Each run's log, pictures, a contact sheet of the reference pictures
(`sheet.png`, to check that the pad script reaches the game) and the first
differing frames side by side (`diff_NNNNNN.png`) are kept in
`batch/<cartridge>/`.

```
tools/arm60model/batch.py                 # the whole panel, about 3 minutes
tools/arm60model/batch.py -only sonic2,ys -frames 1800 -label "trial"
tools/arm60model/batch.py -roms takeme/roms/gg -panel tools/arm60model/panel_gg.txt \
        -reports tools/arm60model/reports/gg   # the Game Gear panel
```

The Game Gear images live in `takeme/roms/gg` (the ISO lists them after
those of `takeme/roms`) and have their own panel file and report
directory, so that each panel's report is compared with the previous one
of the same panel.

Options: `-roms DIR`, `-panel FILE`, `-frames N`, `-jobs N`, `-only a,b`,
`-out DIR`, `-reports DIR`, `-against REPORT.json`, `-timeout SECONDS` (a
hung run is interrupted with SIGUSR1 so that its location is logged),
`-dump N`, `-nosheets`, `-label TEXT`. The model's wall time is dominated by
the reference view, drawn for every frame compared: a raster game with a
band on every line takes about two minutes, the others about ten seconds.
Images with a 512-byte copier header are loaded past it, as on the console.

Requirements: `gcc`, `python3`, and the compiler tools of the SDK in
`bin/compiler/linux`.

`../z80int_test/` holds the reference Z80 interpreter in C (the assembly
one of `src/z80int_a.s` follows it) with a native ZEXDOC harness; see the
comment at the top of `zex.c` for building and running it. `smsref.c`
there runs a cartridge on it one instruction at a time, the maskable
interrupt taken at the exact instruction boundary, with the VDP's
ports, flags, line counter and V counter modelled as in `src/vdp.c`, and
prints the register 8 writes of each frame in the `Raster:` format of
`-rasterlog`: a game whose raster timing differs between the two runs
depends on the interrupts the translated code takes at segment
boundaries.
