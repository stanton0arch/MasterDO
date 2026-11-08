#!/usr/bin/env python3
"""Regression check of a change on the recorded plays, at several timing
scales.

A play replayed in the model depends on its own timing: a few microseconds
more or less in a frame change which translations the budget lets through,
and the course of the run with them. A change is therefore judged on the
means over several timing scales (SIMSCALE), per cartridge, per burst
window and per 100-frame window, against the build it replaces.

Usage (after make and mk.sh):
  regress.py run OUT [-img DIR] [-scales a,b,...] [-only c1,c2] [-args "sim args"] [-jobs N]
      Runs every play of panel.txt and panel_gg.txt with -framelog at each
      scale and keeps the Frame: lines in OUT/<cartridge>@<scale>.txt (a
      run already there is kept). DIR holds the sim, sim.bin and sim.sym
      of the build to run (default: this directory); copy them aside to
      keep a build to compare with.
  regress.py compare BASE NEW [-v] [-only c]
      Compares two run directories over the scales both hold. Per
      cartridge and per window, the means over the scales of: vis (VBLs
      missed with a picture on the screen, as present.py counts them),
      long (long presents), worst (longest time a picture stays on the
      screen, ms), exc (time over budget, ms: frame time, its other field
      included, plus 900 us past 16 683 us), emu (emulation, ms) and sync
      (translations at once, ms).
      Windows: the whole play, each burst (frames with 4 ms or more of
      translations at once in either directory, merged when at most 10
      frames apart, from 5 frames before to 60 after) and, with -v, the
      100-frame windows. Then the regressions: a window where NEW's mean
      is worse by 0.25 VBL, 0.5 long present or 2 ms of worst time, marked
      with * when the difference exceeds twice its standard error. Last,
      the pictures (display on) NEW shows later than BASE, frame by frame,
      summed over the scales: a window marked while NEW shows every
      picture at the same VBL as BASE or earlier is a phase of the
      presentation (a picture shown earlier lengthens the hold before the
      next one), not a frame shown late.

Two runs of the same build at two sets of scales show only noise: a few
unmarked differences on the games that stay over budget (Road Rash).
"""
import concurrent.futures
import glob
import math
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import present  # noqa: E402

ROMS = os.path.join(ROOT, 'takeme', 'roms')
PANELS = [(os.path.join(HERE, 'panel.txt'), ROMS),
          (os.path.join(HERE, 'panel_gg.txt'), os.path.join(ROMS, 'gg'))]
SCALES = '0.9975,0.998,0.9985,0.999,0.9995,1,1.0005,1.001,1.0015,1.002,1.0025'
P = 16683
SYNC = re.compile(r' sync_us (\d+)')
KEYS = ['vis', 'long', 'worst', 'exc', 'emu', 'sync']
LIMITS = {'vis': 0.25, 'long': 0.5, 'worst': 2.0}

# ---------------------------------------------------------------- run


def plays():
    out = []
    for path, d in PANELS:
        for line in open(path):
            line = line.split('#', 1)[0].strip()
            if line:
                p = line.split()
                out.append((p[0], os.path.join(d, p[0]), int(p[1]), p[2] if len(p) > 2 else ''))
    return out


def run_one(img, out, cart, rom, frames, pad, scale, extra):
    path = os.path.join(out, '%s@%s.txt' % (cart, scale))
    if os.path.exists(path) and os.path.getsize(path) > 0:
        return
    wd = os.path.join(out, 'wd', cart + '@' + scale)
    os.makedirs(wd, exist_ok=True)
    env = dict(os.environ, SIMPAD=pad, SIMSCALE=scale)
    cmd = [os.path.join(img, 'sim'), '-img', os.path.join(img, 'sim.bin'),
           '-sym', os.path.join(img, 'sim.sym'), '-rom', rom, '-frames', str(frames),
           '-framelog'] + extra
    p = subprocess.run(cmd, env=env, cwd=wd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       errors='replace')
    keep = [l for l in p.stdout.splitlines() if l.startswith('Frame:') or l.startswith('sim:')]
    with open(path + '.tmp', 'w') as f:
        f.write('\n'.join(keep) + '\n')
    os.rename(path + '.tmp', path)
    for name in os.listdir(wd):
        os.remove(os.path.join(wd, name))
    os.rmdir(wd)


def cmd_run(a):
    out = a.pop(0)
    img, scales, only, extra, jobs = HERE, SCALES.split(','), None, [], os.cpu_count() or 4
    while a:
        k = a.pop(0)
        if k == '-img':
            img = os.path.abspath(a.pop(0))
        elif k == '-scales':
            scales = a.pop(0).split(',')
        elif k == '-only':
            only = set(a.pop(0).split(','))
        elif k == '-args':
            extra = a.pop(0).split()
        elif k == '-jobs':
            jobs = int(a.pop(0))
    os.makedirs(out, exist_ok=True)
    work = [(img, out, c, r, f, p, s, extra) for (c, r, f, p) in plays()
            if not only or c in only or c.split('.')[0] in only for s in scales]
    work.sort(key=lambda w: -w[4])
    t0 = time.time()
    with concurrent.futures.ThreadPoolExecutor(jobs) as ex:
        list(ex.map(lambda w: run_one(*w), work))
    if os.path.isdir(os.path.join(out, 'wd')):
        os.rmdir(os.path.join(out, 'wd'))
    print('%d runs in %.0f s' % (len(work), time.time() - t0))

# ---------------------------------------------------------------- compare


def load(d, cart, sc):
    path = os.path.join(d, '%s@%s.txt' % (cart, sc))
    fr = present.frames_of(path)
    sy = [int(SYNC.search(l).group(1)) for l in open(path) if l.startswith('Frame:')]
    return fr, sy, present.present(fr, present.OVERHEAD)


def scales_of(d, cart):
    return sorted(os.path.basename(p)[len(cart) + 1:-4] for p in glob.glob(os.path.join(d, cart + '@*.txt')))


def window(run, a, b):
    fr, sy, shown = run
    b = min(b, len(fr))
    s = present.stats(shown, a, b, fr)
    if s is None:
        return None
    return {'vis': s['visible'], 'long': s['long'], 'worst': s['worst'] / 1000,
            'exc': sum(max(0, e + u + d + o + 900 - P) for (_, e, u, d, _s, _n, o) in fr[a:b]) / 1000,
            'emu': sum(f[1] for f in fr[a:b]) / 1000, 'sync': sum(sy[a:b]) / 1000}


def mean(x):
    return sum(x) / len(x)


def var(x):
    m = mean(x)
    return sum((v - m) ** 2 for v in x) / max(1, len(x) - 1)


def compare_cart(base, new, cart, verbose):
    common = [s for s in scales_of(base, cart) if s in scales_of(new, cart)]
    runs = {d: [load(d, cart, s) for s in (common or scales_of(d, cart))] for d in (base, new)}
    n = min(len(r[0]) for d in runs for r in runs[d])
    marks = [any(r[1][f] >= 4000 for d in runs for r in runs[d]) for f in range(n)]
    wins, cur = [('play', 0, n)], None
    for f in range(n):
        if marks[f]:
            if cur and f - cur[1] <= 10:
                cur[1] = f
            else:
                cur = [f, f]
                wins.append(cur)
    wins = wins[:1] + [('burst %d-%d' % (a, b), max(0, a - 5), min(n, b + 60)) for a, b in wins[1:]]
    if verbose:
        wins += [('frames %d-%d' % (a, min(n, a + 100) - 1), a, min(n, a + 100)) for a in range(0, n, 100)]
    out = []
    for name, a, b in wins:
        st = {}
        for d in runs:
            vals = [v for v in (window(r, a, b) for r in runs[d]) if v]
            st[d] = {k: [v[k] for v in vals] for k in KEYS}
        if st[base]['vis'] and st[new]['vis']:
            out.append((name, st))
    return len(common), out


def cmd_compare(a):
    base, new = a[0], a[1]
    verbose = '-v' in a
    only = a[a.index('-only') + 1] if '-only' in a else None
    carts = sorted(set(os.path.basename(p).split('@')[0] for p in glob.glob(os.path.join(base, '*@*.txt'))))
    tot = {base: dict.fromkeys(KEYS, 0.0), new: dict.fromkeys(KEYS, 0.0)}
    regs = []
    for cart in carts:
        if only and not cart.startswith(only) or not scales_of(new, cart):
            continue
        nsc, res = compare_cart(base, new, cart, verbose)
        for name, st in res:
            text = '  '.join('%s %.2f/%.2f' % (k, mean(st[base][k]), mean(st[new][k])) for k in KEYS)
            if name == 'play':
                for k in KEYS:
                    tot[base][k] += mean(st[base][k])
                    tot[new][k] += mean(st[new][k])
                print('%-26s scales %2d  %s' % (cart, nsc, text))
            else:
                print('    %-22s %s' % (name, text))
            for k, lim in LIMITS.items():
                mb, mn = mean(st[base][k]), mean(st[new][k])
                if mn - mb >= lim:
                    se = math.sqrt(var(st[base][k]) / len(st[base][k]) + var(st[new][k]) / len(st[new][k]))
                    regs.append((cart, name, k, mb, mn, se))
    print('Total  ' + '  '.join('%s %.1f/%.1f' % (k, tot[base][k], tot[new][k]) for k in KEYS))
    print('Regressions (%s worse than %s):' % (new, base))
    for cart, name, k, mb, mn, se in regs:
        print('  %-26s %-22s %-5s %8.2f -> %8.2f  (se %.2f)%s' %
              (cart, name, k, mb, mn, se, '  *' if mn - mb > 2 * se else ''))
    print('%d marked, on %d cartridges' % (sum(1 for r in regs if r[4] - r[3] > 2 * r[5]),
                                          len(set(r[0] for r in regs if r[4] - r[3] > 2 * r[5]))))
    print('Pictures shown later by %s than by %s (display on, over the scales):' % (new, base))
    nlate = 0
    for cart in carts:
        if only and not cart.startswith(only) or not scales_of(new, cart):
            continue
        frames, worst, scales = 0, 0, 0
        for sc in [s for s in scales_of(base, cart) if s in scales_of(new, cart)]:
            fb, _, sb = load(base, cart, sc)
            fn, _, sn = load(new, cart, sc)
            late = [(sn[k] - sb[k]) // P for k in range(min(len(sb), len(sn), len(fn)))
                    if sn[k] > sb[k] and fn[k][5]]
            frames += len(late)
            scales += 1 if late else 0
            worst = max([worst] + late)
        if frames:
            nlate += 1
            print('  %-26s %d frames on %d scales, at most %d VBL(s) later' % (cart, frames, scales, worst))
    if nlate == 0:
        print('  none')


def main():
    a = sys.argv[1:]
    if not a or a[0] not in ('run', 'compare'):
        print(__doc__)
        sys.exit(1)
    (cmd_run if a[0] == 'run' else cmd_compare)(a[1:])


if __name__ == '__main__':
    main()
