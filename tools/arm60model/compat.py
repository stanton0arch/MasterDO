#!/usr/bin/env python3
"""Compatibility survey of whole cartridge sets in the ARM60 cycle model.

Estimates, for every title of the Master System and Game Gear sets, how
fluid the emulator is on the console, without anyone playing the games:
each title runs in the model (sim) under a few generic pad sequences, and
the frame times of each run are replayed through the presentation of the
ISO (present.py) to find what a player would see.

One image per title: the sets hold several images of most titles
(regions, revisions, fan translations); the title is the file name up to
its first '-', and one image of it is measured (the panel's image when a
recorded play exists, else no translation first, then by region: ue, u,
w, e, b, j, k). The other images of a title are not run.

Runs per title (90 s each by default, `-frames`):
- `idle`: no button (title and attract mode, whose demos run the game:
  often heavier than a blind play, which may stand still);
- `a`, `b`, `c`: 30 s of menu navigation, then 60 s of blind play
  (directions in a cycle; `a` taps button 1 and now and then button 2,
  `b` holds button 1 most of the time, `c` button 2). Master System: the
  menus get button 1 (`a`), buttons 2 and 1 in turn (`b`) or 1 and 2 in
  turn (`c`) every 1.5 s. Game Gear set (its Master System mode cartridges
  included, whose PAUSE is the START button): START 7, 8 or 9 times every
  2.7 s, buttons 1 and 2 between (START pauses most Game Gear games:
  whichever press starts the game, one of the three counts leaves it
  running);
- `recorded`: the play of the model's panels (panel.txt, panel_gg.txt,
  panel_large.txt) when the title has one.

Figures of a run (100-frame windows with the display on):
- a held picture is one shown 50 ms or more (two VBLs missed) with the
  display on, caused by new code translated at once, an eviction of the
  code buffer, a disc read, or an isolated heavy frame (the frames around
  it on time); held pictures less than 2 s apart make one hitch, which
  counts when it lost three VBLs or more;
- the speed of a window is its frames over its VBLs, a held picture
  counting as one VBL (hitches are reported apart);
- the run's fluidity is the speed held in nine windows out of ten (the
  10th percentile), as a percentage of 60 pictures per second.

The title's fluidity is the lowest of its runs (the heaviest scenes any
run reached); the remarks gather what the runs showed, in a fixed
vocabulary (see REMARKS). The reference pictures (every 100 frames) are
compared with the cel engine's; sheet.png shows one in three.

Usage (after make and mk.sh):
  tools/arm60model/compat.py list   [-set ms|gg|both]
  tools/arm60model/compat.py run    [-set ...] [-only key,...] [-jobs N]
                                    [-frames N] [-force] [-limit N]
  tools/arm60model/compat.py report [-set ...]

`run` runs what is missing (a run is kept until sim.bin, the image, the
pad script or the frame count change; -force runs again), then writes the
report. `report` analyses the kept runs again (after a change of the
thresholds) and writes:
  compatibility_master_system.md, compatibility_game_gear.md  (repository root)
  tools/arm60model/reports/compat/<set>-<stamp>-<commit>.json (per-run figures)
Work directories: tools/arm60model/batch/compat/<set>/<title>/<run>/
(log.txt.gz, sim.err, sheet.png: the reference pictures every 300 frames).
"""
import argparse
import concurrent.futures
import datetime
import glob
import gzip
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import batch    # noqa: E402
import present  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
ROMS = os.path.join(ROOT, "takeme", "roms")
WORK = os.path.join(HERE, "batch", "compat")
REPORTS = os.path.join(HERE, "reports", "compat")
P = present.P

SETS = {
    "ms": {"folder": "Master_System", "md": "compatibility_master_system.md",
           "title": "Master System", "panels": ("panel.txt", "panel_large.txt")},
    "gg": {"folder": "Game_Gear", "md": "compatibility_game_gear.md",
           "title": "Game Gear", "panels": ("panel_gg.txt", "panel_large.txt")},
}

REGION_RANK = {"ue": 0, "u": 1, "w": 2, "ueb": 3, "e": 4, "ju": 5, "je": 6, "b": 7,
               "j": 8, "k": 9}

DUMP = 100              # reference pictures compared every DUMP frames
SHEET = 300             # and kept as thumbnails every SHEET frames
WINDOW = 100            # frames per window
HELD = 1.5 * P          # a picture shown this long or more is held (a VBL missed)
CLUSTER = 120           # held pictures closer than this (frames) make one hitch
HITCH_VBLS = 3          # VBLs a hitch must lose to count
FREQUENT = 3.0          # hitches per minute of display-on play: frequent...
FREQUENT_MIN = 4        # ...and at least this many in the run
SLOW_WINDOW = 0.90      # a window under this speed is a slowdown

# The remarks, in the order they are printed.
REMARKS = [
    ("not_loaded", "Does not load",
     "the image is refused (size or layout the emulator does not take)"),
    ("crash", "Crash",
     "a run ended on a fault of the emulator"),
    ("stops", "Emulation stops",
     "a run ended because the emulator could not go on (translation refused)"),
    ("hang", "Emulator freezes",
     "a run stopped making frames: the emulator itself loops without end"),
    ("mapper", "Unsupported mapper",
     "the image holds data past its first 48 KiB but the game never wrote the Sega or "
     "Codemasters paging registers (another mapper: pictures garbled or stuck)"),
    ("legacy", "Unsupported video mode",
     "the game draws in the TMS9918 modes of the SG-1000 (register 0 without M4 with the "
     "display on); the emulator draws mode 4 only"),
    ("black", "Black screen",
     "the display never showed anything but one colour in any run"),
    ("stuck", "Stuck on one screen",
     "the picture never changed in any run"),
    ("artifacts", "Graphical artifacts",
     "pictures drawn by the console's cel engine differ from the VDP's reference picture"),
    ("slowdown", "Slowdown in some scenes",
     "fluid most of the time, but some 100-frame windows run under 90% of full speed"),
    ("frequent", "Frequent hitches",
     "hitches %g times a minute or more" % FREQUENT),
    ("scene", "Hitches at scene starts",
     "the picture freezes for a moment while the new code of a scene is translated "
     "(game, level, race, fight or menu starts)"),
    ("occasional", "Occasional hitches",
     "the picture freezes for a moment in play: code buffer evictions, disc reads of "
     "large images, heavy picture rewrites"),
    ("timeout", "Not measured",
     "the model did not finish the runs in time"),
]
REMARK_TEXT = {k: t for k, t, _ in REMARKS}
BROKEN = ("not_loaded", "mapper", "legacy", "black", "hang")  # no fluidity shown

# ---------------------------------------------------------------- catalogue


def title_key(fname):
    return os.path.splitext(fname)[0].split("-", 1)[0]


def image_rank(fname):
    stem = os.path.splitext(fname)[0]
    parts = stem.split("-")
    translated = any(p.startswith("T_") for p in parts[1:])
    regions = [p for p in parts[1:] if not p.startswith("T_")]
    region = regions[0] if regions else ""
    return (1 if translated else 0, REGION_RANK.get(region, 10), fname)


def read_panels(names):
    panel = {}
    for n in names:
        panel.update(batch.read_panel(os.path.join(HERE, n)))
    return panel


def catalogue(set_name):
    """[(key, chosen image path, [other images], recorded (frames, pad) or None)]."""
    cfg = SETS[set_name]
    folder = os.path.join(ROMS, cfg["folder"])
    files = sorted(f for f in os.listdir(folder) if f.lower().endswith((".sms", ".gg")))
    panel = read_panels(cfg["panels"])
    groups = {}
    for f in files:
        groups.setdefault(title_key(f), []).append(f)
    out = []
    for key in sorted(groups):
        imgs = sorted(groups[key], key=image_rank)
        recorded = [f for f in imgs if f in panel and panel[f][1]]
        chosen = recorded[0] if recorded else imgs[0]
        rec = panel[chosen] if recorded else None
        out.append((key, os.path.join(folder, chosen), [f for f in imgs if f != chosen], rec))
    return out


def needs_banks(path):
    """True when the image holds data past the first 48 KiB that a game can
    only reach by switching banks: a 16 KiB bank from the fourth on that is
    neither one byte repeated nor a copy of an earlier bank (images padded
    to a larger size do not need a mapper)."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) % 16384 == 512:
        data = data[512:]                       # copier header
    banks = [data[i:i + 16384] for i in range(0, len(data), 16384)]
    for b in range(3, len(banks)):
        x = banks[b]
        if x.count(x[:1]) == len(x) or x in banks[:b]:
            continue
        return True
    return False


def display_title(key):
    """The title as the menu shows it, capitalised, with a number at the end
    of a word set apart (sonic2 -> Sonic 2)."""
    words = []
    for w in key.split("_"):
        if not w:
            continue
        if w == "gg":
            words.append("GG")
            continue
        m = re.match(r"^([a-z]{2,})([0-9]+)$", w)
        if m:
            words.extend([m.group(1).capitalize(), m.group(2)])
        else:
            words.append(w[:1].upper() + w[1:])
    return " ".join(words)

# ---------------------------------------------------------------- pad sequences

UP, DOWN, LEFT, RIGHT, B1, B2, START = 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x100
START_PHASE = 1800      # frames of menu navigation before the play sequence


def play_bits(t, variant):
    """Blind play: directions in a 10 s cycle (mostly right, a jump, back
    left, down) with the buttons of the variant: `a` button 1 tapped and
    button 2 now and then, `b` button 1 held most of the time, `c` button 2
    held most of the time (accelerators of the racing games)."""
    c = t % 600
    if c < 240:
        d = RIGHT
    elif c < 270:
        d = RIGHT | UP
    elif c < 330:
        d = LEFT
    elif c < 480:
        d = RIGHT
    elif c < 520:
        d = DOWN
    else:
        d = 0
    if variant == "a":
        b = (B1 if t % 24 < 8 else 0) | (B2 if 32 <= t % 64 < 40 else 0)
    elif variant == "b":
        b = (B1 if t % 40 < 28 else 0) | (B2 if 10 <= t % 50 < 16 else 0)
    else:
        b = (B2 if t % 40 < 28 else 0) | (B1 if 10 <= t % 50 < 16 else 0)
    return d | b


def profile_bits(name, gg, t):
    """Buttons held at frame t by the pad sequence `name`."""
    if name == "idle":
        return 0
    if t >= START_PHASE:
        return play_bits(t, name)
    if not gg:
        if t < 200:
            return 0
        k, r = divmod(t - 200, 90)
        if r >= 8:
            return 0
        if name == "a":
            return B1
        first, second = (B2, B1) if name == "b" else (B1, B2)
        return first if k % 2 == 0 else second
    # Game Gear: START every 160 frames from frame 240, 7, 8 or 9 times
    # (whichever press starts the game, one of the three counts leaves it
    # unpaused), buttons 1 and 2 halfway between for the menus.
    if t < 240:
        return 0
    k, r = divmod(t - 240, 80)
    if r >= 8:
        return 0
    if k % 2 == 0:
        return START if k // 2 < {"a": 7, "b": 8, "c": 9}[name] else 0
    return B2 if k % 6 == 5 else B1


def profile_script(name, gg, frames):
    out = []
    last = 0
    for t in range(frames):
        b = profile_bits(name, gg, t)
        if b != last:
            out.append("%d:%x" % (t, b))
            last = b
    return ",".join(out)


def profiles(gg, rec, frames):
    names = ["idle", "a", "b", "c"]
    out = [(n, frames, profile_script(n, gg, frames)) for n in names]
    if rec:
        out.append(("recorded", rec[0] or frames, rec[1]))
    return out

# ---------------------------------------------------------------- running


def sim_stamp():
    return int(os.path.getmtime(os.path.join(HERE, "sim.bin")))


def run_key(image, frames, pad):
    h = hashlib.sha1()
    h.update(("%s|%d|%d|%s" % (os.path.basename(image), os.path.getsize(image), frames, pad))
             .encode())
    return h.hexdigest()[:16] + "-%d" % sim_stamp()


def make_sheet(workdir):
    """Thumbnails of the reference pictures at multiples of SHEET; returns,
    for every reference picture at a multiple of DUMP, (frame, md5 of the
    file, 1 when the picture is a single colour)."""
    views = sorted(glob.glob(os.path.join(workdir, "view_*.ppm")))
    regular = [v for v in views if int(os.path.basename(v)[5:11]) % DUMP == 0]
    hashes = []
    for v in regular:
        with open(v, "rb") as f:
            data = f.read()
        # Binary PPM: three header fields, then the pixels.
        pix = data.split(b"\n", 3)[3] if data.startswith(b"P6") else data
        plain = 1 if pix[:3] * (len(pix) // 3) == pix[:len(pix) // 3 * 3] else 0
        hashes.append((int(os.path.basename(v)[5:11]), hashlib.md5(data).hexdigest(), plain))
    regular = [v for v in regular if int(os.path.basename(v)[5:11]) % SHEET == 0]
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        return hashes
    if regular:
        per_row, tw, th = 6, 128, 120
        rows = (len(regular) + per_row - 1) // per_row
        sheet = Image.new("RGB", (per_row * tw, rows * (th + 12)), (32, 32, 32))
        draw = ImageDraw.Draw(sheet)
        for i, v in enumerate(regular):
            try:
                im = Image.open(v)
                im = im.resize((im.width // 2, im.height // 2))
            except OSError:
                continue
            x, y = (i % per_row) * tw, (i // per_row) * (th + 12)
            sheet.paste(im, (x, y + 12))
            draw.text((x + 2, y), os.path.basename(v)[5:11].lstrip("0") or "0",
                      fill=(220, 220, 220))
        sheet.save(os.path.join(workdir, "sheet.png"))
    return hashes


def run_one(image, prof, frames, pad, workdir, timeout, force):
    meta_path = os.path.join(workdir, "run.json")
    key = run_key(image, frames, pad)
    if not force and os.path.exists(meta_path):
        try:
            meta = json.load(open(meta_path))
            if meta.get("key") == key:
                return meta, False
        except (OSError, ValueError):
            pass
    if os.path.isdir(workdir):
        shutil.rmtree(workdir)
    os.makedirs(workdir)
    cmd = [os.path.join(HERE, "sim"), "-img", os.path.join(HERE, "sim.bin"),
           "-sym", os.path.join(HERE, "sim.sym"), "-rom", image,
           "-frames", str(frames), "-dump", str(DUMP), "-dumponly", "-framelog"]
    env = dict(os.environ)
    for k in ("SIMPROF", "SIMDIS", "SIMTRLOG", "SIMBLOCKLOG", "SIMBANKLOG", "SIMWATCH"):
        env.pop(k, None)
    env["SIMPAD"] = pad
    t0 = time.time()
    timed_out = False
    with open(os.path.join(workdir, "log.txt"), "w") as out, \
            open(os.path.join(workdir, "sim.err"), "w") as err:
        proc = subprocess.Popen(cmd, stdout=out, stderr=err, env=env, cwd=workdir)
        try:
            proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            proc.kill()
            proc.wait()
    hashes = make_sheet(workdir)
    for f in glob.glob(os.path.join(workdir, "*.ppm")) + \
            [os.path.join(workdir, n) for n in ("code.bin", "codecnt.bin", "lookup.bin")]:
        if os.path.exists(f):
            os.remove(f)
    log = os.path.join(workdir, "log.txt")
    with open(log, "rb") as fi, gzip.open(log + ".gz", "wb", compresslevel=6) as fo:
        shutil.copyfileobj(fi, fo)
    os.remove(log)
    meta = {"key": key, "image": os.path.basename(image), "profile": prof, "frames": frames,
            "pad": pad, "returncode": proc.returncode, "timeout": timed_out,
            "wall": round(time.time() - t0, 1), "pictures": hashes}
    with open(meta_path, "w") as f:
        json.dump(meta, f)
    return meta, True

# ---------------------------------------------------------------- analysis




def ext_frames(path):
    """Per-frame fields of the Frame: lines that present.frames_of leaves out."""
    out = []
    keys = ("sync_us", "evict", "evict_ahead", "disc")
    with open(path, errors="replace") as f:
        for line in f:
            if not line.startswith("Frame: "):
                continue
            w = line.split()
            d = {}
            for k in keys:
                try:
                    d[k] = int(w[w.index(k) + 1])
                except (ValueError, IndexError):
                    d[k] = 0
            out.append(d)
    return out


def hitch_cause(fx, gaps, k, n):
    """Why the picture before frame k was held: new code translated at once,
    an eviction or a disc read in the frames that make it; else, for a
    picture held two VBLs or more among frames on time (a held picture
    in one frame of twenty at most around it), an isolated heavy frame;
    None otherwise (a slow scene, which the speed shows)."""
    lo, hi = max(0, k - 2), min(n, k + 1)
    if any(fx[i]["sync_us"] >= 3000 for i in range(lo, hi)):
        return "code"
    if any(fx[i]["evict"] or fx[i]["evict_ahead"] for i in range(lo, hi)):
        return "evict"
    if any(fx[i]["disc"] for i in range(lo, hi)):
        return "disc"
    if gaps[k] >= 2.5 * P:
        around = gaps[max(1, k - 60):k] + gaps[k + 1:min(n, k + 61)]
        if around and sum(1 for g in around if g >= HELD) * 20 <= len(around):
            return "work"
    return None


def analyse_run(workdir, meta):
    """The figures of one run, from its kept log."""
    gz = os.path.join(workdir, "log.txt.gz")
    tmp = os.path.join(workdir, "log.tmp")
    r = {"profile": meta["profile"], "image": meta["image"], "wall": meta["wall"]}
    loaded = False
    try:
        with gzip.open(gz, "rb") as fi, open(tmp, "wb") as fo:
            shutil.copyfileobj(fi, fo)
        parsed = batch.parse_log(tmp, os.path.join(workdir, "sim.err"))
        fr = present.frames_of(tmp)
        fx = ext_frames(tmp)
        legacy = 0
        with open(tmp, errors="replace") as f:
            for line in f:
                if line.startswith("ROM translation:"):
                    loaded = True
                elif line.startswith("Compare: ") and "display on" in line:
                    # Register 0 without M4: a TMS9918 mode.
                    w = line.split(" registers 0 ", 1)
                    if len(w) == 2 and not int(w[1][:2], 16) & 0x04:
                        legacy += 1
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)
    s = parsed["summary"]
    status = "ok"
    if meta["timeout"]:
        # Out of time with less than half of its frames made: the emulator
        # loops in one frame (a few seconds per frame at most otherwise).
        status = "hang" if len(fr) < meta["frames"] // 2 else "timeout"
    elif not loaded and not fr:
        status = "not_loaded"
    elif parsed["stopped"]:
        status = "stops"
    elif s is None or meta["returncode"] != 0:
        status = "crash"
    r["status"] = status
    r["detail"] = ""
    if parsed["stopped"]:
        r["detail"] = "frame %d, exit reason %d, PC $%04X" % (
            parsed["stopped"]["frame"], parsed["stopped"]["reason"], parsed["stopped"]["pc"])
    elif status in ("crash", "not_loaded"):
        r["detail"] = "; ".join(parsed["fatal"][:2] + parsed["anomalies"][:1])[:240]
    r["frames"] = len(fr)
    on = [f[5] for f in fr]
    r["on_frames"] = sum(on)
    r["emu_us"] = int(sum(f[1] for f in fr) / len(fr)) if fr else 0
    # Pictures compared with the reference view while the display was on.
    diffs = [c for c in parsed["compares"] if c["display"] and c["diff"]]
    r["pics_compared"] = sum(1 for c in parsed["compares"] if c["display"])
    r["legacy_pictures"] = legacy
    r["pics_differing"] = len(diffs)
    r["pics_worst"] = max((c["diff"] for c in diffs), default=0)
    r["pics_diff_frames"] = [c["frame"] for c in diffs[:6]]
    r["sprite_errors"] = (parsed["sprites"] or {}).get("errors", 0)
    pics = [p for p in meta.get("pictures", []) if 0 < p[0] <= len(fr) and fr[p[0] - 1][5]]
    r["distinct_pictures"] = len({p[1] for p in pics})
    r["plain_pictures"] = sum(1 for p in pics if len(p) > 2 and p[2])
    r["shown_pictures"] = len(pics)
    r["bank_switches"] = s["banks"] if s else 0
    r["held"] = []
    r["hitches"] = []
    r["windows"] = []
    r["fluidity"] = None
    r["min_window"] = None
    if len(fr) < 2 or len(fx) != len(fr):
        return r
    shown = present.present(fr, present.OVERHEAD)
    n = min(len(shown), len(fr))
    gaps = [0] + [shown[k] - shown[k - 1] for k in range(1, n)]
    for k in range(1, n):
        if gaps[k] < HELD or not fr[k - 1][5]:
            continue
        cause = hitch_cause(fx, gaps, k, n)
        if cause:
            r["held"].append((k, int(gaps[k]), cause))
    # Hitches: held pictures less than CLUSTER frames apart, with the VBLs
    # they lost together; the cause is new code if any of them was. Only
    # the hitches that lost HITCH_VBLS or more count, and only their VBLs
    # are left out of the speed (a lone missed VBL is a slower frame).
    clusters = []
    for (k, g, cause) in r["held"]:
        lost = int(round(g / float(P))) - 1
        h = clusters[-1] if clusters else None
        if h and k - h["last"] < CLUSTER:
            h["last"] = k
            h["lost"] += lost
            h["ms"] = max(h["ms"], g // 1000)
            h["frames"].append((k, lost))
            if cause == "code":
                h["cause"] = "code"
        else:
            clusters.append({"first": k, "last": k, "lost": lost, "ms": g // 1000,
                             "cause": cause, "frames": [(k, lost)]})
    hitch_extra = [0] * n
    for h in clusters:
        if h["lost"] >= HITCH_VBLS:
            for (k, lost) in h.pop("frames"):
                hitch_extra[k] = lost
            r["hitches"].append(h)
    speeds = []
    for a in range(1, n - WINDOW // 2, WINDOW):
        b = min(n, a + WINDOW)
        if sum(on[a - 1:b - 1]) < 0.9 * (b - a):
            continue
        vbls = (shown[b - 1] - shown[a - 1]) / float(P)
        adj = vbls - sum(hitch_extra[a:b])
        speed = min(1.0, (b - a) / adj) if adj > 0 else 1.0
        r["windows"].append((a, round(speed, 4)))
        speeds.append(speed)
    if speeds:
        ss = sorted(speeds)
        r["fluidity"] = ss[len(ss) // 10] if len(ss) >= 3 else ss[0]
        r["min_window"] = ss[0]
    return r

# ---------------------------------------------------------------- per title


def title_result(key, image, others, runs):
    """Fluidity and remarks of a title from the figures of its runs."""
    t = {"key": key, "title": display_title(key), "image": os.path.basename(image),
         "size": os.path.getsize(image), "others": others, "runs": runs}
    rem = set()
    ok = [r for r in runs if r["status"] == "ok"]
    bad = [r for r in runs if r["status"] != "ok"]
    for r in bad:
        rem.add(r["status"])
    if runs and all(r["status"] == "not_loaded" for r in runs):
        rem = {"not_loaded"}
    ran = [r for r in runs if r["status"] in ("ok", "crash", "stops", "hang") and r["frames"]]
    measured = [r for r in ran if r["fluidity"] is not None]
    legacy = max((r["legacy_pictures"] / float(r["pics_compared"])
                  for r in ran if r["pics_compared"]), default=0.0)
    if "hang" in rem:
        pass                    # what the screen showed before is not the point
    elif legacy >= 0.5:
        rem.add("legacy")
    elif ran and all(r["shown_pictures"] == r["plain_pictures"] for r in ran):
        rem.add("black")
    elif ran and all(r["bank_switches"] == 0 for r in ran) and needs_banks(image):
        rem.add("mapper")
    elif ran and all(r["distinct_pictures"] <= 1 for r in ran):
        rem.add("stuck")
    if any(r["pics_differing"] or r["sprite_errors"] for r in ran):
        rem.add("artifacts")
    t["fluidity"] = None
    t["worst_run"] = None
    if measured and not rem & set(BROKEN):
        worst = min(measured, key=lambda r: (r["fluidity"], -r["emu_us"]))
        t["fluidity"] = worst["fluidity"]
        t["worst_run"] = worst["profile"]
        if round_pct(worst["fluidity"]) >= 95 and \
                any(r["min_window"] is not None and r["min_window"] < SLOW_WINDOW
                    for r in measured):
            rem.add("slowdown")
        # Hitches: the run with the most of them per minute of display-on play.
        best = None
        for r in measured:
            minutes = r["on_frames"] / 3600.0
            if minutes < 0.25 or not r["hitches"]:
                continue
            rate = len(r["hitches"]) / minutes
            if best is None or rate > best[0]:
                best = (rate, r)
        if best:
            rate, r = best
            t["hitch_rate"] = round(rate, 2)
            if rate >= FREQUENT and len(r["hitches"]) >= FREQUENT_MIN:
                rem.add("frequent")
            else:
                causes = {h["cause"] for h in r["hitches"]}
                if "code" in causes:
                    rem.add("scene")
                if causes - {"code"}:
                    rem.add("occasional")
    if 0 < legacy < 0.5 and any(r["legacy_pictures"] >= 3 for r in ran):
        rem.add("legacy")        # some scenes only: the fluidity of the others stands
    t["remarks"] = [k for k, _, _ in REMARKS if k in rem]
    return t


def round_pct(x):
    """Fluidity as a percentage, to the nearest 5 (the model's accuracy is a
    few percent)."""
    return int(5 * round(100.0 * x / 5.0))

# ---------------------------------------------------------------- report


def git_head():
    try:
        return subprocess.check_output(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT,
                                       stderr=subprocess.DEVNULL).decode().strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def write_markdown(set_name, titles, frames, sha, stamp):
    cfg = SETS[set_name]
    n_imgs = sum(1 + len(t["others"]) for t in titles)
    measured = [t for t in titles if t["fluidity"] is not None]
    buckets = [(100, 100, "100%"), (90, 95, "90-95%"), (75, 85, "75-85%"),
               (0, 70, "70% or less")]
    lines = []
    lines.append("# %s compatibility" % cfg["title"])
    lines.append("")
    lines.append("Estimated fluidity of the %s set on the 3DO, measured in the ARM60 cycle "
                 "model of the emulator (commit %s, %s), not by playing the games. The model "
                 "predicts the emulator's frame times on the console within a few percent."
                 % (cfg["title"], sha, stamp))
    lines.append("")
    lines.append("- One image per title: %d titles for %d images (the regions, revisions and "
                 "translations of a title run alike). The image measured is the one with a "
                 "recorded play if any, else an untranslated one, US / European first; the "
                 "data file in `tools/arm60model/reports/compat/` names it." % (
                     len(titles), n_imgs))
    lines.append("- Each title runs %d s several times: without a button (title and attract "
                 "mode, whose demos run the game) and with three generic pad sequences that "
                 "start the game and play it blindly%s; the titles with a recorded play run "
                 "it too. Only the scenes these runs reach are measured." % (
                     frames // 60,
                     "" if set_name == "ms" else " (START 7, 8 or 9 times, so that one of "
                     "them leaves the game unpaused)"))
    lines.append("- **Fluidity**: the speed held nine tenths of the time with the display on, "
                 "in percent of 60 pictures per second (every game runs on a 60 Hz machine; "
                 "a PAL 3DO shows 50 of its 60 pictures), to the nearest 5%; the lowest of "
                 "the runs. Brief freezes are left out of it and given as remarks.")
    counts = []
    for lo, hi, label in buckets:
        n = sum(1 for t in measured if lo <= round_pct(t["fluidity"]) <= hi)
        counts.append("%s: %d" % (label, n))
    lines.append("- Titles: %s; not measured: %d." % ("; ".join(counts),
                                                     len(titles) - len(measured)))
    lines.append("")
    lines.append("| Game | Fluidity | Remarks |")
    lines.append("|------|----------|---------|")
    for t in sorted(titles, key=lambda t: t["title"].lower()):
        flu = "%d%%" % round_pct(t["fluidity"]) if t["fluidity"] is not None else "-"
        rem = ", ".join(REMARK_TEXT[k] for k in t["remarks"])
        lines.append("| %s | %s | %s |" % (t["title"], flu, rem))
    lines.append("")
    lines.append("## Remarks")
    lines.append("")
    for k, text, why in REMARKS:
        lines.append("- **%s**: %s." % (text, why))
    lines.append("")
    lines.append("Not seen by the model: sound, the feel of the pad, peripherals (light "
                 "phaser, paddle, sports pad, 3-D glasses), the YM2413 FM sound (the games "
                 "play their PSG music), wrong pictures that the emulated VDP itself makes "
                 "(the reference picture comes from it), the scenes the runs do not reach, "
                 "the console's own costs that the model does not run.")
    lines.append("")
    path = os.path.join(ROOT, cfg["md"])
    with open(path, "w") as f:
        f.write("\n".join(lines))
    return path


def gather(set_name, frames, jobs, only, force, limit, run_missing, timeout):
    cat = catalogue(set_name)
    if only:
        wanted = set(only.split(","))
        cat = [c for c in cat if c[0] in wanted or os.path.basename(c[1]) in wanted]
    if limit:
        cat = cat[:limit]
    tasks = []
    for key, image, others, rec in cat:
        # The pad of the set's console: a Game Gear cartridge run in the
        # Master System mode (a .sms image of the Game Gear set) is started
        # with START, which that mode turns into PAUSE.
        gg = set_name == "gg"
        for prof, nfr, pad in profiles(gg, rec, frames):
            workdir = os.path.join(WORK, set_name, key, prof)
            tasks.append((key, image, prof, nfr, pad, workdir))
    metas = {}
    if run_missing:
        t0 = time.time()
        done = [0]
        print("%s: %d titles, %d runs, %d jobs" % (set_name, len(cat), len(tasks), jobs))
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
            futs = {ex.submit(run_one, image, prof, nfr, pad, workdir, timeout, force):
                    (key, prof, workdir) for key, image, prof, nfr, pad, workdir in tasks}
            for fut in concurrent.futures.as_completed(futs):
                key, prof, workdir = futs[fut]
                meta, ran = fut.result()
                metas[(key, prof)] = meta
                done[0] += 1
                if ran:
                    print("  [%4d/%d %5.0f s] %-28s %-8s %5.0f s%s" % (
                        done[0], len(tasks), time.time() - t0, key[:28], prof, meta["wall"],
                        " TIMEOUT" if meta["timeout"] else
                        (" exit %d" % meta["returncode"] if meta["returncode"] else "")))
                    sys.stdout.flush()
    titles = []
    for key, image, others, rec in cat:
        runs = []
        for k2, image2, prof, nfr, pad, workdir in tasks:
            if k2 != key:
                continue
            meta = metas.get((key, prof))
            if meta is None:
                try:
                    meta = json.load(open(os.path.join(workdir, "run.json")))
                except (OSError, ValueError):
                    continue
            runs.append(analyse_run(workdir, meta))
        titles.append(title_result(key, image, others, runs))
    return titles


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("cmd", choices=("list", "run", "report"))
    ap.add_argument("-set", default="both", choices=("ms", "gg", "both"))
    ap.add_argument("-frames", type=int, default=5400, help="frames per run (5400: 90 s)")
    ap.add_argument("-jobs", type=int, default=0, help="parallel runs (default: the CPUs)")
    ap.add_argument("-only", default="", help="comma-separated titles (or image names)")
    ap.add_argument("-limit", type=int, default=0, help="the first n titles only")
    ap.add_argument("-force", action="store_true", help="run again what is kept")
    ap.add_argument("-timeout", type=int, default=900, help="seconds per run (900)")
    ap.add_argument("-nomd", action="store_true", help="no Markdown files (pilot runs)")
    args = ap.parse_args()
    sets = ["ms", "gg"] if args.set == "both" else [args.set]

    if args.cmd == "list":
        for s in sets:
            cat = catalogue(s)
            n = sum(1 + len(c[2]) for c in cat)
            print("%s: %d titles, %d images" % (s, len(cat), n))
            for key, image, others, rec in cat:
                print("  %-34s %-40s %s%s" % (key, os.path.basename(image),
                                               "recorded " if rec else "",
                                               "+%d" % len(others) if others else ""))
        return

    for f in ("sim", "sim.bin", "sim.sym", "sim.aif"):
        if not os.path.exists(os.path.join(HERE, f)):
            sys.exit("%s missing: run make, then mk.sh" % f)
    jobs = args.jobs or os.cpu_count() or 1
    sha = git_head()
    stamp = datetime.datetime.now()
    os.makedirs(REPORTS, exist_ok=True)
    for s in sets:
        titles = gather(s, args.frames, jobs, args.only, args.force, args.limit,
                        args.cmd == "run", args.timeout)
        base = "%s-%s-%s" % (s, stamp.strftime("%Y%m%d-%H%M"), sha)
        if not args.nomd and not args.only and not args.limit:
            # The windows and held pictures of each run stay in its log.
            slim = [dict(t, runs=[{k: v for k, v in r.items() if k not in ("windows", "held")}
                                  for r in t["runs"]]) for t in titles]
            data = {"set": s, "date": stamp.strftime("%Y-%m-%d %H:%M"), "commit": sha,
                    "frames": args.frames, "titles": slim}
            with open(os.path.join(REPORTS, base + ".json"), "w") as f:
                json.dump(data, f, indent=0, sort_keys=True)
            print("written: %s" % write_markdown(s, titles, args.frames, sha,
                                                stamp.strftime("%Y-%m-%d")))
        for t in titles:
            flu = "%3d%%" % round_pct(t["fluidity"]) if t["fluidity"] is not None else "   -"
            per_run = " ".join("%s:%s" % (r["profile"], "%d" % round_pct(r["fluidity"])
                                          if r["fluidity"] is not None else r["status"])
                               for r in t["runs"])
            print("%-30s %s  %-50s %s" % (t["title"][:30], flu,
                                         ", ".join(REMARK_TEXT[k] for k in t["remarks"])[:50],
                                         per_run))


if __name__ == "__main__":
    main()
