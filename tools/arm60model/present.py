#!/usr/bin/env python3
"""Presentation model: what a run of the model looks like on the screen.

Usage: present.py [-overhead us] [-windows] log.txt [log2.txt ...]

The model does not present frames. With -framelog it writes one line per
frame (emulation, picture update, drawing, spare time), and this script
replays those times through the frame loop of the ISO in its queued mode:
three screens, the main task drawing a frame into a free screen and
queueing it, a presenter showing at most one queued screen per VBL. The
main task works ahead as long as a screen is free; it waits for the next
VBL when the two others are shown and queued. Each frame also costs a
fixed overhead outside the measured parts (the pad read, the presentation,
the system), 900 us by default.

Printed per run: the frames, the wall clock time per frame (16 683 us at
60 fps on NTSC), the VBLs that showed no new picture (each one a stutter,
visible unless the screen was blank, the display being off), the presents
that came 25 ms or more after the previous one (the "long presents" of
the ISO's log), and the longest time a picture stayed on the screen; with
-windows, the same per 100-frame window.
"""
import re
import sys

P = 16683               # NTSC display period, us
OVERHEAD = 900          # us per frame outside the measured parts
LONG = 25000            # a "long present", as the ISO counts them

LINE = re.compile(r'^Frame: (\d+) emu (\d+) upd (\d+) draw (\d+) spare (\d+)')
SHOWN = re.compile(r' shown (\d+)')


def frames_of(path):
    """(frame, emulation, update, drawing, spare time, display on) per frame."""
    out = []
    for line in open(path, errors='replace'):
        m = LINE.match(line)
        if m:
            s = SHOWN.search(line)
            out.append(tuple(int(x) for x in m.groups()) + (int(s.group(1)) if s else 1,))
    return out


def present(frames, overhead):
    """Times (us) at which each frame is shown."""
    t = 0                   # main task clock
    vbl = P                 # next VBL
    queue = []              # ready times of the screens queued, in order
    shown = []

    def vbls_until(limit):
        nonlocal vbl
        while vbl <= limit:
            if queue and queue[0] <= vbl:
                queue.pop(0)
                shown.append(vbl)
            vbl += P

    for (_, emu, upd, draw, spare, _on) in frames:
        vbls_until(t)
        while len(queue) >= 2:          # no free screen: wait for a VBL
            t = vbl
            vbls_until(t)
        t += emu + upd + draw + spare + overhead
        queue.append(t)
    while queue:
        vbls_until(vbl)
    return shown


def stats(shown, a, b, frames=None):
    """Figures for the frames a..b-1 (b > a + 1). With the frames, the
    VBLs missed while a picture was on the screen ("visible") are told
    apart from those missed while the screen was blank (display off: a
    black screen held longer, which shows nothing); "worst" is then the
    longest time a picture stayed on the screen."""
    ks = range(max(a, 1), b)
    gaps = [shown[k] - shown[k - 1] for k in ks]
    if not gaps:
        return None
    span = sum(gaps)
    vis = [shown[k] - shown[k - 1] for k in ks if frames is None or frames[k - 1][5]]
    return {
        'real': span / len(gaps),
        'missed': round(span / P) - len(gaps),
        'visible': sum(round(g / P) - 1 for g in vis),
        'long': sum(1 for g in gaps if g >= LONG),
        'worst': max(vis) if vis else P,
    }


def main():
    args = sys.argv[1:]
    overhead = OVERHEAD
    windows = False
    paths = []
    k = 0
    while k < len(args):
        if args[k] == '-overhead':
            overhead = int(args[k + 1])
            k += 2
        elif args[k] == '-windows':
            windows = True
            k += 1
        else:
            paths.append(args[k])
            k += 1
    for path in paths:
        fr = frames_of(path)
        if len(fr) < 2:
            print('%s: no Frame: lines (run the model with -framelog)' % path)
            continue
        shown = present(fr, overhead)
        s = stats(shown, 1, len(shown), fr)
        print('%s: %d frames, real %.0f us per frame, %d VBLs without a new picture '
              '(%d of them with a picture on the screen), %d long presents, '
              'a picture at most %.0f ms on the screen' %
              (path, len(fr), s['real'], s['missed'], s['visible'], s['long'], s['worst'] / 1000))
        if windows:
            for a in range(0, len(shown), 100):
                w = stats(shown, a, min(a + 100, len(shown)), fr)
                if w:
                    print('  frames %5d-%5d: real %6.0f us, %3d missed (%3d visible), %3d long, '
                          'longest %6.1f ms' %
                          (a, min(a + 100, len(shown)) - 1, w['real'], w['missed'], w['visible'],
                           w['long'], w['worst'] / 1000))


if __name__ == '__main__':
    main()
