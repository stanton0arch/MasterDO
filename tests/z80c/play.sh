#!/bin/sh
#
# The functions the scripts of this directory share: how the runners are
# compiled, how a table of translated code is linked with the core, and
# how the check of the translated code is played -- the ROM run free
# twice by the picture runner (tests/cel8/romrun.c, compiled as
# tests/cel8/check_picture.sh compiles it, write mode, one picture every
# frame, the colours of every frame taken beside it by
# tests/z80c/colour_tap.c), translated code armed then the interpreter
# alone, and every frame of the first held, rows and colours, to the frame
# of the same rank of the second, or to the one before or after it, never
# going backwards (tests/z80c/sidebyside.c says why and what that does not
# prove). A frame that matches none of them is redrawn in colour by both
# sides and the screens are compared byte for byte, and the colours with
# them: the same screen and the same colours are counted apart, as a
# change of palette number the eye cannot see; anything else is a
# mismatch.
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
# free translated run (Z80C_HITS, src/z80c.h). The picture's runner is
# linked with the same objects: the hits cost it nothing but a counter.
Z80C_PINS="-DLOG_LEVEL=2 -DSMS_IRQ_TEST_SOURCE=0 -DSMS_TELEMETRY=1 \
      -DSMS_DYNAREC_J0=0 -DSMS_DYNAREC_J1=0 -DSMS_DYNAREC_J2=0 -DZ80C_HITS=1"

# The core files, as they stand, in the order they are linked.
Z80C_CORE="cart sms vdp z80 z80c"

# How the picture runner is linked: with the colour tap in front of
# vdp_line.
Z80C_TAP="-Wl,--wrap=vdp_line"

# The frames of one judgement that may be found the same screen under
# other palette numbers. They are redrawn spread over the run and the
# first frame that differs ends the judgement red; past this count of
# frames found the same, with frames still to redraw, the judgement
# proves nothing and says so -- never a pass: a redraw costs a run of the
# ROM up to the frame on each side. Raise it with Z80C_REDRAW_MAX=<n>.
Z80C_REDRAW_MAX=${Z80C_REDRAW_MAX:-64}
case "$Z80C_REDRAW_MAX" in
  ''|*[!0-9]*|0) echo "Z80C_REDRAW_MAX must be a positive integer, not '$Z80C_REDRAW_MAX'"; exit 2;;
esac

# The runners, the core and the interpreter's picture runner compiled
# once into a directory: their own warnings are kept, the core's are not
# (they are the SDK headers' and are counted elsewhere). The dialect is
# gnu89 and not overridable: the runners format their lines with
# snprintf, which C89 does not declare (tests/z80/run_z80.sh says the
# same). Linked there as well: <objects dir>/reference, the picture runner
# with the core as it stands and the empty table, and <objects dir>/romid,
# which prints what a take's header says of a ROM.
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
# is refused rather than compiled as if it had been used.
#
#   z80c_build <rom_code.c> <binary> <objects dir> [core file]
z80c_build() {
  z80c_objs=
  z80c_swapped=0
  for z80c_f in $Z80C_CORE; do
    if [ -n "${4:-}" ] && [ "$(basename "$4")" = "$z80c_f.c" ]; then
      $CC -O1 -std=gnu89 -w $Z80C_PINS -I"$H" -I"$S" -c -o "$2.$z80c_f.o" "$4" || return 2
      z80c_objs="$z80c_objs $2.$z80c_f.o"
      z80c_swapped=1
    else
      z80c_objs="$z80c_objs $3/$z80c_f.o"
    fi
  done
  if [ -n "${4:-}" ] && [ "$z80c_swapped" -eq 0 ]; then
    echo "z80c_build: $4 takes the place of no core file"
    return 2
  fi
  $CC -O1 -std=gnu89 -w $Z80C_PINS -I"$H" -I"$S" -c -o "$2.table.o" "$1" || return 2
  $CC -o "$2" "$3/sidebyside.o" "$3/host.o" $z80c_objs "$2.table.o" || return 2
  $CC -o "$2.picture" "$3/romrun.o" "$3/colour_tap.o" $z80c_objs "$2.table.o" $Z80C_TAP || return 2
}

# The digest of the colours of one frame in a colours file, or nothing.
#
#   z80c_colour_of_frame <colours> <frame>
z80c_colour_of_frame() {
  sed -n "s/^$2 \([0-9a-f]\{8\}\)\$/\1/p" "$1" | head -n 1
}

# The digests of one frame of a take, the whole line, or nothing.
#
#   z80c_frame_line <take> <frame>
z80c_frame_line() {
  grep "^frame=$2 " "$1" || true
}

# Whether a take and its colours are this run's: the header names FRAMES
# frames, one picture every frame, the ROM as <romid> prints it and the
# side that took it; the colours hold at least FRAMES records and no stop.
#
#   z80c_take_ok <take> <taken> <romid line>
z80c_take_ok() {
  [ -f "$1" ] && [ -f "$1.colours" ] || return 1
  case "$(head -n 1 "$1")" in
    "cel8-picture-reference frames=$FRAMES every=1 width=256 lines=192 pictures=$FRAMES $3 taken=$2") ;;
    *) return 1;;
  esac
  if grep -q '^stopped ' "$1.colours"; then
    return 1
  fi
  [ "$(grep -c '^[0-9][0-9]* [0-9a-f]\{8\}$' "$1.colours")" -ge "$FRAMES" ] || return 1
  return 0
}

# The picture the interpreter draws of every frame of a ROM, and its
# colours, written to <take> and <take>.colours by
# <objects dir>/reference, and kept: every table of the ROM, and every
# broken copy of one, is held to them. A take already there is reused only
# when its header and its colours are this run's (z80c_take_ok); otherwise
# it is taken again. A take is written under <take>.part and moved into
# place once it has been checked, so that a run stopped half way never
# leaves a file the next run would trust. A take whose core stopped is
# refused: nothing is proved against it.
#
#   z80c_reference <objects dir> <rom> <take>
z80c_reference() {
  [ -f "$2" ] || { echo "FAIL: no rom at $2"; return 2; }
  if z80c_id=$("$1/romid" "$2" romid </dev/null 2>/dev/null); then :; else
    echo "FAIL: $(basename "$2") does not boot"
    return 2
  fi
  if [ -f "$3" ] || [ -f "$3.colours" ]; then
    if z80c_take_ok "$3" interpreter "$z80c_id"; then
      return 0
    fi
    echo "z80c: the take at $3 is not the interpreter's $FRAMES frames of this rom ($z80c_id): taken again"
    rm -f "$3" "$3.colours"
  fi
  rm -f "$3.part" "$3.part.tmp" "$3.part.colours"
  if Z80C_COLOURS="$3.part.colours" "$1/reference" "$2" "$FRAMES" 1 write "$3.part" "" interpreter \
       </dev/null >"$3.out" 2>"$3.log"; then z80c_rrc=0; else z80c_rrc=$?; fi
  if [ "$z80c_rrc" -ne 0 ] || [ ! -f "$3.part" ] || [ ! -f "$3.part.colours" ] \
     || ! grep -q "^pictures=$FRAMES written$" "$3.out"; then
    cat "$3.out"
    grep -E 'ERR|WARN|FAIL|nothing|cannot' "$3.log" || true
    echo "FAIL: the interpreter's picture of $(basename "$2") was not taken (status $z80c_rrc)"
    rm -f "$3.part" "$3.part.colours"
    return 2
  fi
  if ! grep -q '^z80c exec=0 fallback=0$' "$3.out"; then
    grep '^z80c ' "$3.out" || true
    echo "FAIL: the interpreter's picture ran translated code"
    rm -f "$3.part" "$3.part.colours"
    return 2
  fi
  if grep -q '^stopped ' "$3.part.colours"; then
    echo "FAIL: the interpreter's core stopped on $(basename "$2") ($(grep -m1 '^stopped ' "$3.part.colours")): nothing to hold the pictures to"
    rm -f "$3.part" "$3.part.colours"
    return 2
  fi
  if mv -f "$3.part.colours" "$3.colours" && mv -f "$3.part" "$3" \
     && z80c_take_ok "$3" interpreter "$z80c_id"; then :; else
    echo "FAIL: the interpreter's take of $(basename "$2") is not what was asked ($z80c_id, $FRAMES frames)"
    rm -f "$3" "$3.colours" "$3.part" "$3.part.colours"
    return 2
  fi
  return 0
}

# One frame that matched no allowed neighbour, redrawn in colour: the
# translated side draws frame <n>, the interpreter draws every frame from
# max(n-1, <from>) to n+1 that exists, each redraw a run of the ROM up to
# that frame with a single picture at its end, and each redraw's digests
# and colours held to its take's, so that the screen compared is the frame
# judged. Returns 0 when the translated screen is byte for byte one of the
# interpreter's and its colours are that frame's (z80c_colour_of holds
# which), 1 when it is none, 2 when nothing could be redrawn. With
# Z80C_KEEP naming a directory, a screen that is none of them is copied
# there with the interpreter's screen of the same rank, under the ROM's
# name and the binary's; a copy that fails is a warning, not a verdict.
#
#   z80c_redraw <binary> <objects dir> <rom> <frame> <from> <dir> <translated take> <interpreter take>
z80c_redraw() {
  z80c_d=$6/redraw.$4
  rm -rf "$z80c_d"
  mkdir -p "$z80c_d/t" "$z80c_d/i" || { echo "FAIL: cannot make $z80c_d"; return 2; }
  z80c_ppm=f$(printf '%05d' "$4").ppm
  z80c_tcol=$(z80c_colour_of_frame "$7.colours" "$4")
  if Z80C_COLOURS="$z80c_d/t.colours" "$1.picture" "$3" $(( $4 + 1 )) 100000 write "$z80c_d/t.fnv" "$z80c_d/t" translated \
       </dev/null >"$z80c_d/t.out" 2>"$z80c_d/t.log"; then z80c_rrc=0; else z80c_rrc=$?; fi
  if [ "$z80c_rrc" -ne 0 ] || [ ! -f "$z80c_d/t/$z80c_ppm" ] || [ -z "$z80c_tcol" ] \
     || [ "$(z80c_frame_line "$z80c_d/t.fnv" "$4")" != "$(z80c_frame_line "$7" "$4")" ] \
     || [ "$(z80c_colour_of_frame "$z80c_d/t.colours" "$4")" != "$z80c_tcol" ]; then
    echo "FAIL: frame $4 redrawn with the translated code is not the frame of the take (status $z80c_rrc)"
    return 2
  fi
  z80c_k=$(( $4 - 1 ))
  if [ "$z80c_k" -lt "$5" ]; then
    z80c_k=$5
  fi
  while [ "$z80c_k" -le $(( $4 + 1 )) ]; do
    if [ "$z80c_k" -ge 0 ] && [ "$z80c_k" -lt "$FRAMES" ]; then
      z80c_kppm=f$(printf '%05d' "$z80c_k").ppm
      z80c_kcol=$(z80c_colour_of_frame "$8.colours" "$z80c_k")
      if Z80C_COLOURS="$z80c_d/i$z80c_k.colours" "$2/reference" "$3" $(( z80c_k + 1 )) 100000 write \
           "$z80c_d/i$z80c_k.fnv" "$z80c_d/i" interpreter </dev/null >"$z80c_d/i$z80c_k.out" 2>"$z80c_d/i$z80c_k.log"; then
        z80c_rrc=0
      else
        z80c_rrc=$?
      fi
      if [ "$z80c_rrc" -ne 0 ] || [ ! -f "$z80c_d/i/$z80c_kppm" ] || [ -z "$z80c_kcol" ] \
         || [ "$(z80c_frame_line "$z80c_d/i$z80c_k.fnv" "$z80c_k")" != "$(z80c_frame_line "$8" "$z80c_k")" ] \
         || [ "$(z80c_colour_of_frame "$z80c_d/i$z80c_k.colours" "$z80c_k")" != "$z80c_kcol" ]; then
        echo "FAIL: frame $z80c_k redrawn by the interpreter is not the frame of the take (status $z80c_rrc)"
        return 2
      fi
      if cmp -s "$z80c_d/t/$z80c_ppm" "$z80c_d/i/$z80c_kppm" && [ "$z80c_tcol" = "$z80c_kcol" ]; then
        z80c_colour_of=$z80c_k
        rm -rf "$z80c_d"
        return 0
      fi
    fi
    z80c_k=$(( z80c_k + 1 ))
  done
  if [ -n "${Z80C_KEEP:-}" ]; then
    z80c_n=$(basename "$3" | sed 's/\./_/g')-$(basename "$(dirname "$1")")-$(basename "$1")
    if mkdir -p "$Z80C_KEEP" 2>/dev/null \
       && cp "$z80c_d/t/$z80c_ppm" "$Z80C_KEEP/$z80c_n-translated-$z80c_ppm" 2>/dev/null \
       && { [ ! -f "$z80c_d/i/$z80c_ppm" ] || cp "$z80c_d/i/$z80c_ppm" "$Z80C_KEEP/$z80c_n-interpreter-$z80c_ppm" 2>/dev/null; }; then
      echo "z80c: the screens of frame $4 kept in $Z80C_KEEP as $z80c_n-*-$z80c_ppm"
    else
      echo "WARN: the screens of frame $4 could not be kept in $Z80C_KEEP"
    fi
  fi
  rm -rf "$z80c_d"
  return 1
}

# One table judged: the free translated run (the table paired, blocks run,
# chains followed, and the hits per block written when a file is named),
# then the translated picture of every frame and its colours taken and
# held to the interpreter's take, then every frame left unmatched redrawn
# in colour. Prints the runners' lines and the verdict:
#
#   z80c: pictures PASS <n>/<n> same=<n> shifted=<n> colour_only=<n>
#   z80c: picture MISMATCH frame <n> rows=<rows that differ at the same rank>
#   z80c: MISMATCH frame <n>: the core stopped with translated code armed
#
# stopping at the first mismatch. Returns 0 the pictures agree, 1 a
# mismatch, 2 nothing proved, 3 the table is not the ROM's. t_tr, t_pic
# and t_redraw hold the seconds of the three steps afterwards.
#
#   z80c_play <binary> <rom> <dir> <interpreter take> <objects dir> [counts]
z80c_play() {
  t_tr=; t_pic=; t_redraw=
  z80c_start=$(date +%s)
  if "$1" "$2" "$FRAMES" translated ${6:+"$6"} </dev/null >"$3/translated.out" 2>"$3/translated.log"; then
    z80c_rc=0
  else
    z80c_rc=$?
  fi
  t_tr=$(( $(date +%s) - z80c_start ))
  cat "$3/translated.out"
  if [ "$z80c_rc" -ne 0 ]; then
    grep -E 'ERR|WARN' "$3/translated.log" || true
    # A stop with translated code armed: the takes below say whether the
    # interpreter stops too, and only then is it named.
    if ! grep -q '^FAIL: the core stopped at frame ' "$3/translated.out"; then
      return "$z80c_rc"
    fi
  fi

  # The picture runner prints many figures of its own (windows, cels,
  # scenes, flags); they judge the list of cels, not the translation, and
  # the picture check holds them. What is held here is the rows, the
  # colours, and that the table linked ran blocks while drawing them.
  rm -f "$3/translated.fnv" "$3/translated.fnv.colours"
  z80c_start=$(date +%s)
  if Z80C_COLOURS="$3/translated.fnv.colours" "$1.picture" "$2" "$FRAMES" 1 write "$3/translated.fnv" "" translated \
       </dev/null >"$3/picture.out" 2>"$3/picture.log"; then z80c_rc=0; else z80c_rc=$?; fi
  t_pic=$(( $(date +%s) - z80c_start ))
  if [ "$z80c_rc" -ne 0 ] || ! grep -q "^pictures=$FRAMES written$" "$3/picture.out" \
     || [ ! -f "$3/translated.fnv.colours" ]; then
    grep -E 'FAIL|ERR|WARN|nothing|cannot|pictures=' "$3/picture.out" "$3/picture.log" || true
    echo "FAIL: the translated picture of every frame was not taken (status $z80c_rc)"
    return 2
  fi
  if ! grep -q '^z80c exec=[1-9]' "$3/picture.out"; then
    grep '^z80c ' "$3/picture.out" || true
    echo "FAIL: no translated block ran while the picture was drawn"
    return 2
  fi

  if "$1" pictures "$3/translated.fnv" "$3/translated.fnv.colours" "$4" "$4.colours" \
       </dev/null >"$3/pictures.out"; then z80c_rc=0; else z80c_rc=$?; fi
  if [ "$z80c_rc" -eq 1 ] && grep -q '^z80c: MISMATCH frame [0-9]*: the core stopped' "$3/pictures.out"; then
    grep -m1 '^z80c: MISMATCH' "$3/pictures.out"
    return 1
  fi
  if [ "$z80c_rc" -gt 1 ]; then
    cat "$3/pictures.out"
    return 2
  fi
  z80c_sum=$(grep '^z80c: pictures frames=' "$3/pictures.out" || true)
  z80c_same=$(echo "$z80c_sum" | sed -n 's/^.* same=\([0-9]*\) .*$/\1/p')
  z80c_shifted=$(echo "$z80c_sum" | sed -n 's/^.* shifted=\([0-9]*\) .*$/\1/p')
  z80c_unmatched=$(echo "$z80c_sum" | sed -n 's/^.* unmatched=\([0-9]*\)$/\1/p')
  if [ -z "$z80c_same" ] || [ -z "$z80c_shifted" ] || [ -z "$z80c_unmatched" ] \
     || [ "$z80c_sum" != "z80c: pictures frames=$FRAMES same=$z80c_same shifted=$z80c_shifted unmatched=$z80c_unmatched" ] \
     || [ $(( z80c_same + z80c_shifted + z80c_unmatched )) -ne "$FRAMES" ] \
     || { [ "$z80c_rc" -eq 0 ] && [ "$z80c_unmatched" -ne 0 ]; } \
     || { [ "$z80c_rc" -eq 1 ] && [ "$z80c_unmatched" -eq 0 ]; }; then
    cat "$3/pictures.out"
    echo "FAIL: the pictures were not compared whole (status $z80c_rc)"
    return 2
  fi
  echo "$z80c_sum"

  # The free run stopped, and the interpreter's take did not: the pictures
  # above must have said so already.
  if grep -q '^FAIL: the core stopped at frame ' "$3/translated.out"; then
    echo "FAIL: the free translated run stopped but its picture did not: nothing proved"
    return 2
  fi

  z80c_start=$(date +%s)
  z80c_colour=0
  # Redrawn spread over the run -- one frame in every (unmatched / cap)
  # first, then the next such pass -- so that the frames redrawn before
  # the cap cover the whole run and not its first seconds alone, where a
  # broken program may still show the same black screen under another
  # number. In order when they fit under the cap. The first frame that
  # differs ends the judgement. Read from a file rather than a pipe: a
  # pipe would run the loop in a subshell, and its verdict would never
  # reach this function; every runner inside reads /dev/null, not the
  # list.
  grep '^unmatched frame ' "$3/pictures.out" \
    | awk -v m="$Z80C_REDRAW_MAX" '{ l[NR] = $0 }
        END { s = int((NR + m - 1) / m); if(s < 1) s = 1;
              for(r = 0; r < s; r++) for(i = 1 + r; i <= NR; i += s) print l[i] }' \
    >"$3/unmatched" || true
  while read -r z80c_w1 z80c_w2 z80c_fr z80c_from z80c_rows; do
    if z80c_redraw "$1" "$5" "$2" "$z80c_fr" "${z80c_from#from=}" "$3" "$3/translated.fnv" "$4"; then
      z80c_rc=0
    else
      z80c_rc=$?
    fi
    if [ "$z80c_rc" -eq 0 ]; then
      z80c_colour=$(( z80c_colour + 1 ))
      echo "z80c: frame $z80c_fr colour only: its screen and colours are the interpreter's frame $z80c_colour_of, other palette numbers ($z80c_rows)"
      if [ "$z80c_colour" -ge "$Z80C_REDRAW_MAX" ] && [ "$z80c_colour" -lt "$z80c_unmatched" ]; then
        t_redraw=$(( $(date +%s) - z80c_start ))
        echo "FAIL: $z80c_colour frames of the same screen under other palette numbers and $(( z80c_unmatched - z80c_colour )) more to redraw, past Z80C_REDRAW_MAX=$Z80C_REDRAW_MAX: nothing proved (raise Z80C_REDRAW_MAX to redraw more)"
        return 2
      fi
    elif [ "$z80c_rc" -eq 1 ]; then
      t_redraw=$(( $(date +%s) - z80c_start ))
      echo "z80c: picture MISMATCH frame $z80c_fr $z80c_rows"
      return 1
    else
      t_redraw=$(( $(date +%s) - z80c_start ))
      return 2
    fi
  done <"$3/unmatched"
  t_redraw=$(( $(date +%s) - z80c_start ))
  if [ "$z80c_colour" -ne "$z80c_unmatched" ]; then
    echo "FAIL: $z80c_unmatched frames unmatched, $z80c_colour redrawn: the list was not read whole"
    return 2
  fi
  rm -f "$3/translated.fnv" "$3/translated.fnv.colours"
  echo "z80c: pictures PASS $FRAMES/$FRAMES same=$z80c_same shifted=$z80c_shifted colour_only=$z80c_colour"
  return 0
}
