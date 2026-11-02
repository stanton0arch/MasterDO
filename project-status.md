# Project status

Short summary of where the emulator stands. The detailed step log is
`project-context.MD` (read it on demand by searching for a step heading,
such as `### Step 9.3`, never whole: it is about 640 KB).

## Goal and constraints

- Sega Master System / Game Gear emulator for the 3DO, built with
  3do-devkit; 60 fps for NTSC games, 50 for PAL, no frame skipping.
- Generic: no game-specific code; Z80 code is translated to ARM on the
  console (translator in assembly, interpreter for cold code, zoned code
  buffer with eviction). Validated with ZEXDOC and a panel of 15 Master
  System and 9 Game Gear cartridges whose recorded plays replay in the
  ARM60 cycle model (`tools/arm60model`, `batch.py`, `panel.txt`,
  `panel_gg.txt`, reports in `tools/arm60model/reports/`).
- Build: `make clean && make` only. No automated tests: each step
  produces an ISO the tester runs on Phoenix and on a PAL FZ-1 console,
  and sends the logs back (ended with L or X, which print the `Pad script:`
  line used to replay the play in the model).
- The ARM60 model predicts Phoenix within a few percent; every change is
  measured there first (machine state hash, cel chain hash, pictures
  against the CPU reference view, both panels) before an ISO is built.

## Architecture (sources in `src/`)

- `z80jit*.c/.h/.s`, `z80jit_dec.s`, `z80jit_ana.s`, `z80jit_emt.s`,
  `z80jit_blk.s`, `z80int_a.s`: translator (decoder, analysis, emitter,
  block management in assembly), interpreter, glue.
- `smsmem*`: Sega and Codemasters mappers. `vdp.c`, `vdp_a.s`, `sms.c`,
  `sms_io.s`: VDP, machine, port handlers in assembly.
- `render.c`, `render_a.s`: picture as name table bitmaps drawn as cel
  pieces, priority patches, sprite cels (per-line limit, zoom, palette
  bands, Game Gear window), pictures identical to the previous one shown
  again instead of drawn.
- `psg.c`: SN76489 on the DSP through the audio folio (five voices,
  registers sent once per frame, Game Gear stereo).
- `game.c`, `main.c`, `platform.c`: frame loop with a presenter thread
  and three screens, binary log records printed at L or X, ROM menu,
  pad recorder, 320x240 bitmaps whatever the display type.

## State of the roadmap (2026-11-02)

Done and validated on Phoenix: steps 0 to 4, 5a-5d (5e dropped), 6a-6q,
7 (PSG), 9 (Game Gear mode), 9.1 (bounded forced translations, code
buffer sized from the DRAM left), 9.3 (sprite cels cut at palette
bands), plus the DRAM savings (benchmark state and reference view tables
on demand, port tables in the context padding).

Results: Sonic 2, Golden Axe, Space Harrier, Micro Machines' races,
Phantasy Star's dungeons and most of the panel run at 60 fps in calm
windows. Z80-bound scenes stay below: Road Rash's race (about 40 fps,
one line interrupt per line), OutRun's race at the edge of 60 fps. The
dips left are translation bursts at level, race, fight and match starts
(frames of 45-90 ms) and the scene change catch-ups.

Memory during a run: 27 040 bytes of DRAM with 512 KiB images (14 code
zones), 43 424 with images up to 256 KiB (16 zones). 1 MiB images do not
load (large image layout not done).

Done in the model, ISO to be tested: step 9.4a (one block context shared
by the decoder, the analysis and the emitter; the flag scan values kept
in the block descriptors; block management 715 to 582 cycles per block,
ROM translation 2-3 % faster; generated code identical word for word on
the 25 cartridges, checked with `tools/arm60model/cmpcode.py`). Section
`### Step 9.4a` of `project-context.MD`. On Phoenix the `ROM translation:`
line should read about 69 us per instruction (71 before), nothing else
changes.

## Next, on the tester's go (nothing started)

- Step 9.4b: the packed instruction record (29 to about 16 words) with
  the forward analysis walks folded into the decoder loop; then 9.4c, the
  emitter storing its sequences with `STM` (templates loaded once); 9.4d
  denser stubs only if needed. About 1.5x expected in all (71 to about 50
  us per instruction on Phoenix). Plan in `project-context.MD`, section
  `### Step 9.4`.
- Step 9.2: port input loops (`INI`/`IND` loops, `INIR`/`INDR` in one
  call; Defenders of Oasis's text windows).
- Step 8: input details (second pad, pause).
- Large image layout; a larger log arena (24 KiB fills on 6 000-frame
  Game Gear plays).
- Open decision, not to be settled before it is asked: PAL consoles run
  NTSC games at 50 fps (17 % slower) or 60 emulated frames with one in
  six not shown.

## Working rules worth remembering

- Never word a feature as game-specific: name the hardware mechanism,
  the game is the measurement case.
- Logs: nothing printed inside the frame loop (printf costs 35 us per
  character on Phoenix); check the banner line of every log to identify
  the ISO.
- Replay recipe: copy the `Pad script:` line into `panel.txt` or
  `panel_gg.txt`, run `batch.py -only <cartridge>` or `sim` with it as
  `SIMPAD`; compare the `Hash:` lines with `-hash -hashevery 16`.
- Commit messages: English, imperative, no step numbers, no names, no
  co-author lines, no emoji.
