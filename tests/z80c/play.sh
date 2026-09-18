#!/bin/sh
#
# The functions the scripts of this directory share: how the runners are
# compiled, how a table of translated code is linked with the core, and
# how the check of the translated code is played.
#
# THE VERDICT. Once a table is armed a line of the picture is no longer a
# quota of T-states: the program runs until it waits, the video part
# counts the line, and the interrupt it raised is taken at the head of
# the next (src/z80.h, z80_run_events). The translated code is held to
# the interpreter LED BY THAT SAME CLOCK: the same table linked, its
# blocks not executed (src/z80c.h, z80c_no_exec), so that the two runs
# end every line on the same instruction. The picture runner
# (tests/cel8/romrun.c, compiled as tests/cel8/check_picture.sh compiles
# it, write mode, one picture every frame) takes both, with the colours
# of every frame line by line and the memory the program keeps at the
# end of every frame beside each (tests/z80c/colour_tap.c), and
# tests/z80c/sidebyside.c holds frame n to frame n: every row, the
# colours and the memory, no shift, no tolerance. The first frame that
# differs is the verdict.
#
# FOR INFORMATION, the translated take is also held to the take of the
# classic interpreter -- no table, quotas of T-states -- with one frame
# of shift allowed, and the counts are printed: the two clocks are not
# the same clock and nothing is judged on them (sidebyside.c says why).
#
# TWO REFUSALS, the program's and not the translation's, exit status 4:
# a line that never waits, and code executed from RAM. A refused program
# is named and counted, never passed.
#
# Sourced, never run: `. tests/z80c/play.sh` from the repository root,
# after CC, S (src/), H (the SDK stubs), B (this directory) and FRAMES are
# set.
#
# No function here turns the shell's exit-on-error on or off: a status is
# taken with `if cmd; then rc=0; else rc=$?; fi`, which holds whether the
# caller runs with set -e or not.

# The same pins as the picture check: the host has no assembler for the
# recompiled cores, the interrupt test source is off, and the counters
# the video part and the translated code keep are on -- the runners
# print them -- plus the hits per block the generated code counts for the
# free translated run (Z80C_HITS, src/z80c.h) and the switches of the
# host runners (Z80C_BENCH: the reference with the blocks not executed,
# the guard of a line that never waits, the seeds). The picture's runner
# is linked with the same objects.
Z80C_PINS="-DLOG_LEVEL=2 -DSMS_IRQ_TEST_SOURCE=0 -DSMS_TELEMETRY=1 \
      -DSMS_DYNAREC_J0=0 -DSMS_DYNAREC_J1=0 -DSMS_DYNAREC_J2=0 -DZ80C_HITS=1 -DZ80C_BENCH=1"

# The core files, as they stand, in the order they are linked.
Z80C_CORE="cart sms vdp z80 z80c"

# How the picture runner is linked: with the colour and memory tap in
# front of vdp_line.
Z80C_TAP="-Wl,--wrap=vdp_line"

# The runners, the core and the interpreter's picture runner compiled
# once into a directory: their own warnings are kept, the core's are not
# (they are the SDK headers' and are counted elsewhere). The dialect is
# gnu89 and not overridable: the runners format their lines with
# snprintf, which C89 does not declare (tests/z80/run_z80.sh says the
# same). Linked there as well: <objects dir>/reference, the picture runner
# with the core as it stands and the empty table -- the classic
# interpreter -- and <objects dir>/romid, which prints what a take's
# header says of a ROM.
#
#   z80c_runner <objects dir>
z80c_runner() {
  mkdir -p "$1" || return 2
  $CC -O1 -std=gnu89 -Wall -Wextra $Z80C_PINS -I"$H" -I"$S" -c -o "$1/sidebyside.o" "$B/sidebyside.c" || return 2
  $CC -O1 -std=gnu89 -Wall -Wextra $Z80C_PINS -I"$H" -I"$S" -c -o "$1/host.o" "$B/host.c" || return 2
  $CC -O1 -std=gnu89 -Wall -Wextra $Z80C_PINS -I"$H" -I"$S" -c -o "$1/colour_tap.o" "$B/colour_tap.c" || return 2
  $CC -O1 -std=gnu89 -Wall -Wextra $Z80C_PINS -I"$H" -I"$S" -c -o "$1/romrun.o" tests/cel8/romrun.c || return 2
  for z80c_f in $Z80C_CORE; do
    $CC -O1 -std=gnu89 -w $Z80C_PINS -I"$H" -I"$S" -c -o "$1/$z80c_f.o" "$S/$z80c_f.c" || return 2
  done
  $CC -O1 -std=gnu89 -w $Z80C_PINS -I"$H" -I"$S" -c -o "$1/rom_code_none.o" "$B/rom_code_none.c" || return 2
  z80c_core_objs=$(for z80c_f in $Z80C_CORE; do printf '%s ' "$1/$z80c_f.o"; done)
  $CC -o "$1/reference" "$1/romrun.o" "$1/colour_tap.o" $z80c_core_objs "$1/rom_code_none.o" $Z80C_TAP || return 2
  $CC -o "$1/romid" "$1/sidebyside.o" "$1/host.o" $z80c_core_objs "$1/rom_code_none.o" || return 2
}

# The core linked with a named table of translated code, twice: the free
# translated run at <binary>, the picture's runner at <binary>.picture. A
# fourth argument names a file that takes the place of the core file of
# the same name, and of that one only: it is how a check breaks the core
# itself rather than the emitted C, and a name that matches none of them
# is refused rather than compiled as if it had been used. A fourth
# argument that names z80_ops.h -- the one header a check may break,
# since the runners' own objects, built once above, do not read it --
# has no object to swap: the core's files are COPIED BESIDE IT and
# compiled from there, and so is the table read with that directory
# first. Copied, not pointed at by -I: for #include "z80_ops.h" the
# compiler looks in the including file's own directory before any -I,
# so a core file compiled from src/ would read src/z80_ops.h intact
# whatever the include path says. The core is thus compiled again, with
# the broken header, into the binary's own objects; a check that breaks
# an interpreter macro in it sees the interpreter's frames change.
#
#   z80c_build <rom_code.c> <binary> <objects dir> [core file]
z80c_build() {
  z80c_objs=
  z80c_swapped=0
  z80c_inc=
  z80c_src=$S
  case "${4:-}" in
    *.h)
      if [ "$(basename "$4")" != "z80_ops.h" ]; then
        echo "z80c_build: $4 takes the place of no core header the runners leave alone (z80_ops.h only)"
        return 2
      fi
      z80c_inc="-I$(dirname "$4")"
      z80c_src=$(dirname "$4")
      for z80c_f in $Z80C_CORE; do
        cp "$S/$z80c_f.c" "$z80c_src/$z80c_f.c" || return 2
      done
      z80c_swapped=1
      ;;
  esac
  for z80c_f in $Z80C_CORE; do
    if [ -n "${4:-}" ] && [ "$(basename "$4")" = "$z80c_f.c" ]; then
      $CC -O1 -std=gnu89 -w $Z80C_PINS -I"$H" -I"$S" -c -o "$2.$z80c_f.o" "$4" || return 2
      z80c_objs="$z80c_objs $2.$z80c_f.o"
      z80c_swapped=1
    elif [ -n "$z80c_inc" ]; then
      $CC -O1 -std=gnu89 -w $Z80C_PINS $z80c_inc -I"$H" -I"$S" -c -o "$2.$z80c_f.o" "$z80c_src/$z80c_f.c" || return 2
      z80c_objs="$z80c_objs $2.$z80c_f.o"
    else
      z80c_objs="$z80c_objs $3/$z80c_f.o"
    fi
  done
  if [ -n "${4:-}" ] && [ "$z80c_swapped" -eq 0 ]; then
    echo "z80c_build: $4 takes the place of no core file"
    return 2
  fi
  $CC -O1 -std=gnu89 -w $Z80C_PINS $z80c_inc -I"$H" -I"$S" -c -o "$2.table.o" "$1" || return 2
  $CC -o "$2" "$3/sidebyside.o" "$3/host.o" $z80c_objs "$2.table.o" || return 2
  $CC -o "$2.picture" "$3/romrun.o" "$3/colour_tap.o" $z80c_objs "$2.table.o" $Z80C_TAP || return 2
}

# Whether a take is this run's: the header names FRAMES frames, one
# picture every frame, the ROM as <romid> prints it and the side that
# took it; its colours -- and its memory, for a take of the reference --
# hold at least FRAMES records, and no stop unless the side is the
# translated one: a stop with translated code armed is the verdict's to
# name (sidebyside.c, events), not this function's to hide.
#
#   z80c_take_ok <take> <taken> <romid line>
z80c_take_ok() {
  [ -f "$1" ] && [ -f "$1.colours" ] || return 1
  case "$(head -n 1 "$1")" in
    "cel8-picture-reference frames=$FRAMES every=1 width=256 lines=192 pictures=$FRAMES $3 taken=$2") ;;
    *) return 1;;
  esac
  if [ "$2" != translated ] && grep -q '^stopped ' "$1.colours"; then
    return 1
  fi
  [ "$(grep -c '^[0-9][0-9]* [0-9a-f]\{8\}$' "$1.colours")" -ge "$FRAMES" ] || return 1
  if [ "$2" = reference ]; then
    [ -f "$1.memory" ] || return 1
    [ "$(grep -c '^[0-9][0-9]* [0-9a-f]\{8\}$' "$1.memory")" -ge "$FRAMES" ] || return 1
  fi
  return 0
}

# One take of the picture runner, written under <take>.part with its
# colours and memory beside it, checked, and moved into place -- so that
# a run stopped half way never leaves a file the next run would trust.
# The header must say what was asked, and a core that stopped is refused
# for the reference and the interpreter, whose takes are what the
# translated code is held to; a stopped TRANSLATED take is kept and
# handed to the verdict, which names it as a mismatch (sidebyside.c,
# events). Prints the runner's z80c lines on a failure. Returns 0, 2
# nothing taken, 4 the program refused (the runner's line printed).
#
#   z80c_take <picture runner> <rom> <take> <taken> <romid line> [env...]
z80c_take() {
  z80c_t_bin=$1; z80c_t_rom=$2; z80c_t_take=$3; z80c_t_taken=$4; z80c_t_id=$5
  shift 5
  rm -f "$z80c_t_take.part" "$z80c_t_take.part.tmp" "$z80c_t_take.part.colours" "$z80c_t_take.part.memory"
  if env "$@" Z80C_COLOURS="$z80c_t_take.part.colours" Z80C_MEMORY="$z80c_t_take.part.memory" \
       "$z80c_t_bin" "$z80c_t_rom" "$FRAMES" 1 write "$z80c_t_take.part" "" "$z80c_t_taken" \
       </dev/null >"$z80c_t_take.out" 2>"$z80c_t_take.log"; then z80c_rrc=0; else z80c_rrc=$?; fi
  if [ "$z80c_rrc" -eq 4 ] && grep -q '^z80c: REFUSED ' "$z80c_t_take.out"; then
    grep '^z80c: ' "$z80c_t_take.out"
    rm -f "$z80c_t_take.part" "$z80c_t_take.part.colours" "$z80c_t_take.part.memory"
    return 4
  fi
  if [ "$z80c_rrc" -ne 0 ] || [ ! -f "$z80c_t_take.part" ] || [ ! -f "$z80c_t_take.part.colours" ] \
     || [ ! -f "$z80c_t_take.part.memory" ] || ! grep -q "^pictures=$FRAMES written$" "$z80c_t_take.out"; then
    grep -E 'FAIL|ERR|WARN|nothing|cannot|pictures=|^z80c' "$z80c_t_take.out" "$z80c_t_take.log" || true
    echo "FAIL: the $z80c_t_taken picture of $(basename "$z80c_t_rom") was not taken (status $z80c_rrc)"
    rm -f "$z80c_t_take.part" "$z80c_t_take.part.colours" "$z80c_t_take.part.memory"
    return 2
  fi
  if [ "$z80c_t_taken" != translated ] && grep -q '^stopped ' "$z80c_t_take.part.colours"; then
    echo "FAIL: the core stopped while the $z80c_t_taken picture of $(basename "$z80c_t_rom") was taken ($(grep -m1 '^stopped ' "$z80c_t_take.part.colours"))"
    rm -f "$z80c_t_take.part" "$z80c_t_take.part.colours" "$z80c_t_take.part.memory"
    return 2
  fi
  if mv -f "$z80c_t_take.part.colours" "$z80c_t_take.colours" && mv -f "$z80c_t_take.part.memory" "$z80c_t_take.memory" \
     && mv -f "$z80c_t_take.part" "$z80c_t_take" && z80c_take_ok "$z80c_t_take" "$z80c_t_taken" "$z80c_t_id"; then :; else
    echo "FAIL: the $z80c_t_taken take of $(basename "$z80c_t_rom") is not what was asked ($z80c_t_id, $FRAMES frames)"
    rm -f "$z80c_t_take" "$z80c_t_take.colours" "$z80c_t_take.memory" "$z80c_t_take.part" "$z80c_t_take.part.colours" "$z80c_t_take.part.memory"
    return 2
  fi
  return 0
}

# The ROM as <romid> prints it, in z80c_id, or a failure.
#
#   z80c_romid <objects dir> <rom>
z80c_romid() {
  [ -f "$2" ] || { echo "FAIL: no rom at $2"; return 2; }
  if z80c_id=$("$1/romid" "$2" romid </dev/null 2>/dev/null); then return 0; fi
  echo "FAIL: $(basename "$2") does not boot"
  return 2
}

# The picture the CLASSIC interpreter draws of every frame of a ROM --
# no table, quotas of T-states -- written to <take> by
# <objects dir>/reference and kept, the information every table of the
# ROM is held beside. A take already there is reused only when it is
# this run's (z80c_take_ok); otherwise it is taken again.
#
#   z80c_reference <objects dir> <rom> <take>
z80c_reference() {
  z80c_romid "$1" "$2" || return 2
  if [ -f "$3" ] || [ -f "$3.colours" ]; then
    if z80c_take_ok "$3" interpreter "$z80c_id"; then
      return 0
    fi
    echo "z80c: the take at $3 is not the interpreter's $FRAMES frames of this rom ($z80c_id): taken again"
    rm -f "$3" "$3.colours" "$3.memory"
  fi
  z80c_take "$1/reference" "$2" "$3" interpreter "$z80c_id" Z80C_NO_EXEC=0 || return $?
  if ! grep -q '^z80c exec=0 fallback=0$' "$3.out"; then
    grep '^z80c ' "$3.out" || true
    echo "FAIL: the interpreter's picture ran translated code"
    rm -f "$3" "$3.colours" "$3.memory"
    return 2
  fi
  return 0
}

# THE REFERENCE of a table: the picture runner linked with that table,
# the blocks not executed, so that the interpreter runs every
# instruction on the clock the table's waits give. Written to <take>
# and kept; reused when it is this run's.
#
#   z80c_events_reference <binary> <objects dir> <rom> <take>
z80c_events_reference() {
  z80c_romid "$2" "$3" || return 2
  if [ -f "$4" ] || [ -f "$4.colours" ] || [ -f "$4.memory" ]; then
    if z80c_take_ok "$4" reference "$z80c_id"; then
      return 0
    fi
    echo "z80c: the take at $4 is not the reference's $FRAMES frames of this rom ($z80c_id): taken again"
    rm -f "$4" "$4.colours" "$4.memory"
  fi
  z80c_take "$1.picture" "$3" "$4" reference "$z80c_id" Z80C_NO_EXEC=1 || return $?
  if ! grep -q '^z80c blocks executed=no$' "$4.out" || ! grep -q '^z80c exec=0 fallback=[1-9]' "$4.out"; then
    grep '^z80c ' "$4.out" || true
    echo "FAIL: the reference ran translated code, or nothing"
    rm -f "$4" "$4.colours" "$4.memory"
    return 2
  fi
  return 0
}

# The screens of one frame, drawn in colour by both sides -- the
# translated code, and the reference -- and copied to Z80C_KEEP under the
# ROM's name and the binary's, for the human to look at a mismatch. Each
# is a run of the ROM up to that frame with a single picture at its end.
# A copy that fails is a warning, never a verdict.
#
#   z80c_keep_screens <binary> <rom> <frame> <dir>
z80c_keep_screens() {
  z80c_d=$4/keep.$3
  z80c_ppm=f$(printf '%05d' "$3").ppm
  z80c_n=$(basename "$2" | sed 's/\./_/g')-$(basename "$(dirname "$1")")-$(basename "$1")
  rm -rf "$z80c_d"
  mkdir -p "$z80c_d/t" "$z80c_d/r" "$Z80C_KEEP" 2>/dev/null || { echo "WARN: cannot make $z80c_d or $Z80C_KEEP"; return 0; }
  Z80C_NO_EXEC=0 "$1.picture" "$2" $(( $3 + 1 )) 100000 write "$z80c_d/t.fnv" "$z80c_d/t" translated </dev/null >/dev/null 2>&1 || true
  Z80C_NO_EXEC=1 "$1.picture" "$2" $(( $3 + 1 )) 100000 write "$z80c_d/r.fnv" "$z80c_d/r" reference </dev/null >/dev/null 2>&1 || true
  if cp "$z80c_d/t/$z80c_ppm" "$Z80C_KEEP/$z80c_n-translated-$z80c_ppm" 2>/dev/null \
     && cp "$z80c_d/r/$z80c_ppm" "$Z80C_KEEP/$z80c_n-reference-$z80c_ppm" 2>/dev/null; then
    echo "z80c: the screens of frame $3 kept in $Z80C_KEEP as $z80c_n-*-$z80c_ppm"
  else
    echo "WARN: the screens of frame $3 could not be kept in $Z80C_KEEP"
  fi
  rm -rf "$z80c_d"
  return 0
}

# One table judged: the free translated run (the table paired, blocks
# run, chains followed, the instructions a frame ran, and the hits per
# block and the seeds written when files are named), then the translated
# picture of every frame taken with its colours and memory and held to
# the reference's take -- taken here with this binary when <reference
# take> is not there or not this run's, reused when it is -- and, for
# information, to the classic interpreter's. Prints the runners' lines
# and the verdict:
#
#   z80c: events PASS <n>/<n> memory=same insns/frame=<n>
#   z80c: classic same=<n> shifted=<n> differ=<n>       (information)
#   z80c: events MISMATCH frame <n> rows=<rows> colours=same|differ memory=same|differ
#   z80c: MISMATCH frame <n>: the core stopped with translated code armed
#   z80c: REFUSED no wait at <pos> frame <n>
#   z80c: REFUSED ram code at <addr> reached from <pos> frame <n>
#
# Returns 0 the frames agree, 1 a mismatch, 2 nothing proved, 3 the table
# is not the ROM's, 4 the program is refused. t_tr and t_pic hold the
# seconds of the two steps afterwards; z80c_insns the instructions a
# frame ran.
#
#   z80c_play <binary> <rom> <dir> <reference take> <interpreter take> <objects dir> [counts [seeds]]
z80c_play() {
  t_tr=; t_pic=; z80c_insns=
  z80c_start=$(date +%s)
  if "$1" "$2" "$FRAMES" translated ${7:+"$7"} ${8:+"$8"} </dev/null >"$3/translated.out" 2>"$3/translated.log"; then
    z80c_rc=0
  else
    z80c_rc=$?
  fi
  t_tr=$(( $(date +%s) - z80c_start ))
  cat "$3/translated.out"
  if [ "$z80c_rc" -ne 0 ]; then
    grep -E 'ERR|WARN' "$3/translated.log" || true
    # A stop with translated code armed: the takes below say whether the
    # reference stops too, and only then is it named.
    if ! grep -q '^FAIL: the core stopped at frame ' "$3/translated.out"; then
      return "$z80c_rc"
    fi
  fi
  z80c_insns=$(sed -n 's/^z80c: insns\/frame=\([0-9]*\)$/\1/p' "$3/translated.out" | head -n 1)

  # The reference, validated when it is there (z80c_take_ok) and taken
  # again when it is not this run's.
  z80c_events_reference "$1" "$6" "$2" "$4" || return $?

  # The picture runner prints many figures of its own (windows, cels,
  # scenes, flags); they judge the list of cels, not the translation, and
  # the picture check holds them. What is held here is the rows, the
  # colours, the memory, and that the table linked ran blocks while
  # drawing them.
  z80c_romid "$6" "$2" || return 2
  rm -f "$3/translated.fnv" "$3/translated.fnv.colours" "$3/translated.fnv.memory"
  z80c_start=$(date +%s)
  z80c_take "$1.picture" "$2" "$3/translated.fnv" translated "$z80c_id" Z80C_NO_EXEC=0
  z80c_rc=$?
  t_pic=$(( $(date +%s) - z80c_start ))
  [ "$z80c_rc" -eq 0 ] || return "$z80c_rc"
  if ! grep -q '^z80c exec=[1-9]' "$3/translated.fnv.out"; then
    grep '^z80c ' "$3/translated.fnv.out" || true
    echo "FAIL: no translated block ran while the picture was drawn"
    return 2
  fi

  if "$1" events "$3/translated.fnv" "$4" </dev/null >"$3/events.out"; then z80c_rc=0; else z80c_rc=$?; fi
  if [ "$z80c_rc" -eq 1 ]; then
    grep -m1 'MISMATCH' "$3/events.out"
    if [ -n "${Z80C_KEEP:-}" ]; then
      z80c_fr=$(sed -n 's/^z80c: events MISMATCH frame \([0-9]*\) .*$/\1/p' "$3/events.out" | head -n 1)
      [ -n "$z80c_fr" ] && z80c_keep_screens "$1" "$2" "$z80c_fr" "$3"
    fi
    return 1
  fi
  if [ "$z80c_rc" -ne 0 ] || ! grep -q "^z80c: events frames=$FRAMES same=$FRAMES$" "$3/events.out"; then
    cat "$3/events.out"
    echo "FAIL: the frames were not compared whole (status $z80c_rc)"
    return 2
  fi

  # The free run stopped, and the reference did not: the frames above
  # must have said so already.
  if grep -q '^FAIL: the core stopped at frame ' "$3/translated.out"; then
    echo "FAIL: the free translated run stopped but its picture did not: nothing proved"
    return 2
  fi

  # For information: the classic interpreter's take, one frame of shift
  # allowed. Nothing is judged on it.
  if [ -f "$5" ] && [ -f "$5.colours" ]; then
    if "$1" pictures "$3/translated.fnv" "$3/translated.fnv.colours" "$5" "$5.colours" \
         </dev/null >"$3/classic.out"; then z80c_crc=0; else z80c_crc=$?; fi
    z80c_sum=$(grep '^z80c: pictures frames=' "$3/classic.out" || true)
    if [ "$z80c_crc" -le 1 ] && [ -n "$z80c_sum" ]; then
      echo "$z80c_sum" | sed 's/^z80c: pictures frames=[0-9]* same=\([0-9]*\) shifted=\([0-9]*\) unmatched=\([0-9]*\)$/z80c: classic same=\1 shifted=\2 differ=\3/'
      grep '^z80c: pictures elsewhere=' "$3/classic.out" | sed 's/^z80c: pictures /z80c: classic /' || true
    else
      echo "z80c: classic not compared: $(grep -m1 'FAIL' "$3/classic.out" || echo "status $z80c_crc")"
    fi
  else
    echo "z80c: classic not compared: no take of the classic interpreter"
  fi

  rm -f "$3/translated.fnv" "$3/translated.fnv.colours" "$3/translated.fnv.memory"
  echo "z80c: events PASS $FRAMES/$FRAMES memory=same insns/frame=${z80c_insns:-?}"
  return 0
}
