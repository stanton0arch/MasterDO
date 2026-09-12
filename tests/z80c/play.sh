#!/bin/sh
#
# The functions the two scripts of this directory share: how the
# side-by-side runner is compiled, how a table of translated code is
# linked with the core into one binary, and how that binary is played
# twice -- recorded with the translated code armed, then replayed by the
# interpreter on the recorded quotas (tests/z80c/sidebyside.c says how).
#
# Sourced, never run: `. tests/z80c/play.sh` from the repository root,
# after CC, S (src/), H (the SDK stubs), B (this directory), FRAMES and
# EVERY are set.

# The same pins as the picture check: the host has no assembler for the
# recompiled cores, the interrupt test source is off, and the counters
# the video part and the translated code keep are on -- the runner
# records them -- plus the hits per block the generated code counts for
# the runner alone (Z80C_HITS, src/z80c.h).
Z80C_PINS="-DLOG_LEVEL=2 -DSMS_IRQ_TEST_SOURCE=0 -DSMS_TELEMETRY=1 \
      -DSMS_DYNAREC_J0=0 -DSMS_DYNAREC_J1=0 -DSMS_DYNAREC_J2=0 -DZ80C_HITS=1"

# The runner compiled on its own: its warnings are kept, the core's are
# not (they are the SDK headers' and are counted elsewhere). The dialect
# is gnu89 and not overridable: the runner formats its states with
# snprintf, which C89 does not declare (tests/z80/run_z80.sh says the
# same).
#
#   z80c_runner <sidebyside.o>
z80c_runner() {
  $CC -O1 -std=gnu89 -Wall -Wextra $Z80C_PINS -I"$H" -I"$S" -c -o "$1" "$B/sidebyside.c"
}

# The core linked as it stands with a named table of translated code.
# A fourth argument names a file that takes the place of the core file of
# the same name, and of that one only: it is how a check breaks the core
# itself rather than the emitted C, and a name that matches none of them
# is refused rather than compiled as if it had been used.
#
#   z80c_build <rom_code.c> <binary> <sidebyside.o> [core file]
z80c_build() {
  z80c_srcs="$S/cart.c $S/sms.c $S/vdp.c $S/z80.c $S/z80c.c"
  if [ -n "${4:-}" ]; then
    z80c_base=$(basename "$4")
    z80c_kept=
    z80c_swapped=0
    for z80c_f in $z80c_srcs; do
      if [ "$(basename "$z80c_f")" = "$z80c_base" ]; then
        z80c_kept="$z80c_kept $4"
        z80c_swapped=1
      else
        z80c_kept="$z80c_kept $z80c_f"
      fi
    done
    if [ "$z80c_swapped" -eq 0 ]; then
      echo "z80c_build: $4 takes the place of no core file"
      return 2
    fi
    z80c_srcs=$z80c_kept
  fi
  $CC -O1 -std=gnu89 -w $Z80C_PINS -I"$H" -I"$S" -o "$2" "$3" $z80c_srcs "$1"
}

# One binary played twice: recorded with the translated code armed, then
# replayed by the interpreter on the recorded quotas. Prints the runner's
# lines; returns its status -- 0 the same, 1 a difference, 2 nothing
# proved, 3 the table is not the ROM's. The replay mode is the caller's:
# "replay" judges, "replay-poke" is the self-check of the frame path.
# A sixth argument names where the record writes the hits per block.
# t_rec and t_rep hold the seconds of the two runs afterwards.
#
#   z80c_play <binary> <rom> <dir> <replay mode> [counts]
z80c_play() {
  rc=0; t_rec=; t_rep=; start=
  set +e
  start=$(date +%s)
  "$1" "$2" "$FRAMES" "$EVERY" record "$3/trace.bin" ${5:+"$5"} >"$3/record.out" 2>"$3/record.log"
  rc=$?
  t_rec=$(( $(date +%s) - start ))
  cat "$3/record.out"
  if [ "$rc" -ne 0 ]; then
    set -e
    grep -E 'ERR|WARN' "$3/record.log" || true
    return "$rc"
  fi
  start=$(date +%s)
  "$1" "$2" "$FRAMES" "$EVERY" "$4" "$3/trace.bin" >"$3/replay.out" 2>"$3/replay.log"
  rc=$?
  t_rep=$(( $(date +%s) - start ))
  set -e
  cat "$3/replay.out"
  [ "$rc" -eq 0 ] || grep -E 'ERR|WARN' "$3/replay.log" || true
  return "$rc"
}
