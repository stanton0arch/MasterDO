#!/usr/bin/env python3
"""Batch runner of the ARM60 cycle model.

Runs every cartridge of a directory in the model (sim), in parallel, each
with its own pad script and frame count from the panel file, and writes a
one-page report: per cartridge the status of the run, the emulation,
picture update and total times (over the run and across the 100-frame
windows), the frames over budget and the worst frames, the translator and
VDP counters that tell which hardware mechanisms the game uses, the
pictures compared with the CPU reference view and the sprite evaluation
check. The report ends with the differences against a previous report
(the newest one of the reports directory unless -against names one), so
that a change of the emulator is measured on the whole panel at once.

Usage (after make and mk.sh):
  tools/arm60model/batch.py [-roms DIR] [-panel FILE] [-frames N] [-jobs N]
                            [-only name,...] [-out DIR] [-reports DIR]
                            [-against REPORT.json] [-timeout SECONDS]
                            [-dump N] [-nosheets] [-label TEXT]

Outputs:
  <out>/<cartridge>/        log.txt (stdout of sim), sim.err, the PPM
                            pictures (view_ = reference, cels_ = cel
                            engine model) at every -dump frames and the
                            first differing ones, sheet.png (the reference
                            pictures of the run, to check that the pad
                            script reaches the game) and diff_NNNNNN.png
                            (reference, cels and their difference) for the
                            first differing frames.
  <reports>/<stamp>-<commit>.txt and .json   the report and its data.

The pad script format is that of SIMPAD: "frame:bits,..." with the buttons
held from each frame on, bits in hexadecimal (01 up, 02 down, 04 left,
08 right, 10 button 1, 20 button 2, 100 pause).
"""
import argparse
import concurrent.futures
import datetime
import glob
import json
import os
import re
import signal
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
FRAME_US = 16683
DRAW_US = 3500          # the DrawCels time the harness adds to each frame

# ---------------------------------------------------------------- panel

DEFAULT_PAD = ",".join("%d:10,%d:0" % (f, f + 6) for f in range(300, 1800, 300))


def read_panel(path):
    """Returns {file: (frames or None, pad script or '')}."""
    panel = {}
    if not path or not os.path.exists(path):
        return panel
    with open(path) as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split()
            name = parts[0]
            frames = int(parts[1]) if len(parts) > 1 and parts[1] != "-" else None
            pad = parts[2] if len(parts) > 2 and parts[2] != "-" else ""
            panel[name] = (frames, pad)
    return panel

# ---------------------------------------------------------------- parsing


def grab(line, pattern, default=None):
    """The integers of the first match of pattern in line (a tuple), or the
    single integer when the pattern has one group."""
    m = re.search(pattern, line)
    if not m:
        return default
    vals = tuple(int(g) for g in m.groups())
    return vals[0] if len(vals) == 1 else vals


def parse_window(line):
    w = {}
    w["first"], w["last"] = grab(line, r"frames (\d+)-(\d+)", (0, 0))
    w["emu"], w["emu_worst"], w["emu_worst_frame"] = grab(
        line, r"emulation (\d+) us average, (\d+) us worst \(frame (\d+)\)", (0, 0, 0))
    w["upd"], w["upd_worst"] = grab(line, r"update (\d+) us average, (\d+) us worst", (0, 0))
    w["total"], w["total_worst"], w["total_worst_frame"] = grab(
        line, r"total (\d+) us average, (\d+) us worst \(frame (\d+)\)", (0, 0, 0))
    w["over"] = grab(line, r"(\d+) over budget", 0)
    w["idle"] = grab(line, r"Z80 idle (\d+)%", 0)
    w["irq"], w["nmi"] = grab(line, r"(\d+) IRQ, (\d+) NMI", (0, 0))
    (w["data_w"], w["data_r"], w["ctrl_w"], w["stat_r"], w["counter_r"],
     w["psg"]) = grab(line, r"VDP (\d+) data writes, (\d+) data reads, (\d+) control writes, "
                      r"(\d+) status reads, (\d+) counter reads, PSG (\d+)", (0,) * 6)
    (w["banks"], w["blocks"], w["spare"], w["sync"], w["sync_us"], w["refused"],
     w["promoted"]) = grab(line, r"(\d+) bank switches, (\d+) blocks \((\d+) in spare time, "
                           r"(\d+) at once in (\d+) us, (\d+) refused, (\d+) promoted", (0,) * 7)
    w["interp"], w["stretches"], w["evictions"], w["evicted"] = grab(
        line, r"(\d+) interpreted instructions in (\d+) stretches, (\d+) evictions \((\d+) blocks",
        (0, 0, 0, 0))
    return w


def parse_picture(line):
    p = {}
    p["first"], p["last"] = grab(line, r"frames (\d+)-(\d+)", (0, 0))
    p["tiles"], p["cells"], p["rebuilds"] = grab(
        line, r"(\d+) tiles converted, (\d+) cells drawn, (\d+) rebuilds", (0, 0, 0))
    (p["sprites"], p["pieces"], p["strips"], p["patches"], p["bands"], p["dbands"],
     p["ntbands"], p["dropped"], p["layers"], p["spr_partial"], p["spr_dropped"],
     p["overflow"], p["collision"]) = grab(
        line, r"(\d+) sprite cels, (\d+) pieces, (\d+) priority strips, (\d+) priority patches, "
        r"(\d+) frames in (?:scroll )?bands, (\d+) frames partly blanked, (\d+) frames in name "
        r"table bands, (\d+) bands dropped, (\d+) frames short of layers, (\d+) frames with "
        r"sprites past the line limit, (\d+) sprite runs dropped, (\d+) frames with sprite "
        r"overflow, (\d+) with sprite collision", (0,) * 13)
    return p


def parse_log(path, err_path):
    r = {"windows": [], "pictures": [], "slow": [], "compares": [], "anomalies": [],
         "summary": None, "picture_summary": None, "vdp_summary": None, "rom": None,
         "stopped": None, "histogram": "", "sprites": None, "cart_ram": "",
         "display_changes": 0}
    try:
        lines = open(path, errors="replace").read().splitlines()
    except OSError:
        lines = []
    for line in lines:
        if line.startswith("Game: frames "):
            r["windows"].append(parse_window(line))
        elif line.startswith("Picture: frames "):
            r["pictures"].append(parse_picture(line))
        elif line.startswith("Game: frame ") and " took " in line:
            m = re.match(r"Game: frame (\d+) took (\d+) us \(emulation (\d+), update (\d+), "
                         r"draw (\d+)\): Z80 idle (\d+)%, (\d+) blocks translated( after an "
                         r"eviction)? \((\d+) us at once\), (\d+) interpreted instructions in "
                         r"(\d+) stretches, (\d+) VDP data writes, (\d+) tiles converted, "
                         r"(\d+) cells drawn", line)
            if m:
                g = m.groups()
                r["slow"].append({"frame": int(g[0]), "total": int(g[1]), "emu": int(g[2]),
                                  "upd": int(g[3]), "blocks": int(g[6]),
                                  "evicted": g[7] is not None, "sync_us": int(g[8]),
                                  "interp": int(g[9]), "data_w": int(g[11]),
                                  "tiles": int(g[12]), "cells": int(g[13])})
        elif line.startswith("Game: stopped at frame"):
            m = re.match(r"Game: stopped at frame (\d+), exit reason (-?\d+) at PC \$([0-9a-fA-F]+)",
                         line)
            if m:
                r["stopped"] = {"frame": int(m.group(1)), "reason": int(m.group(2)),
                                "pc": int(m.group(3), 16)}
        elif line.startswith("Game summary: picture:"):
            p = parse_picture(line)
            r["picture_summary"] = p
        elif line.startswith("Game summary: VDP"):
            r["vdp_summary"] = dict(zip(
                ("data_w", "data_r", "ctrl_w", "stat_r", "counter_r", "psg", "pad"),
                grab(line, r"VDP (\d+) data writes, (\d+) data reads, (\d+) control writes, "
                     r"(\d+) status reads, (\d+) counter reads, PSG (\d+) writes, pad (\d+) reads",
                     (0,) * 7)))
        elif line.startswith("Game summary: total frame time histogram"):
            r["histogram"] = line.split(")", 1)[1].strip()
        elif line.startswith("Game summary:"):
            s = {}
            s["frames"] = grab(line, r"Game summary: (\d+) frames", 0)
            s["emu"], s["emu_pct"] = grab(line, r"emulation (\d+) us average \((\d+)%", (0, 0))
            s["emu_worst"], s["emu_worst_frame"] = grab(line, r"worst (\d+) us \(frame (\d+)\), update",
                                                        (0, 0))
            s["upd"] = grab(line, r"update (\d+) us average", 0)
            s["total"], s["total_pct"] = grab(line, r"total (\d+) us average \((\d+)%\)", (0, 0))
            s["total_worst"], s["total_worst_frame"] = grab(
                line, r"\((\d+)%\), worst (\d+) us \(frame (\d+)\)", (0, 0, 0))[1:]
            s["over"] = grab(line, r"(\d+) frames over \d+ us", 0)
            s["idle"] = grab(line, r"Z80 idle (\d+)%", 0)
            s["irq"], s["nmi"], s["banks"] = grab(line, r"(\d+) IRQ, (\d+) NMI, (\d+) bank switches",
                                                  (0, 0, 0))
            (s["blocks"], s["spare"], s["sync"], s["sync_us"], s["refused"],
             s["promoted"]) = grab(line, r"(\d+) blocks translated while running \((\d+) in spare "
                                   r"time, (\d+) at once in (\d+) us, (\d+) refused, (\d+) promoted",
                                   (0,) * 6)
            s["interp"], s["stretches"], s["evictions"], s["evicted"] = grab(
                line, r"(\d+) interpreted instructions in (\d+) stretches, (\d+) evictions "
                r"\((\d+) blocks", (0, 0, 0, 0))
            s["busy_loops"], s["seams"], s["status"] = grab(
                line, r"(\d+) busy-wait loops found, (\d+) RAM blocks cut at a seam, status (-?\d+)",
                (0, 0, 0))
            r["summary"] = s
        elif line.startswith("ROM translation:"):
            r["rom"] = dict(zip(
                ("blocks", "insns", "bytes", "us", "us_per_insn", "busy", "scans", "evictions"),
                grab(line, r"(\d+) blocks, (\d+) instructions, (\d+) bytes of ARM code, (\d+) us "
                     r"\((\d+) us per instruction\), (\d+) busy-wait loops, (\d+) flag scans from "
                     r"the cache, (\d+) evictions", (0,) * 8)))
            if "out of memory" in line:
                r["anomalies"].append(line)
        elif line.startswith("Compare: frame"):
            m = re.match(r"Compare: frame (\d+): (\d+) pixels differ, (\d+) outside priority tiles, "
                         r"display (on|off), (\d+) sprites, .*?(\d+) bands", line)
            if m:
                r["compares"].append({"frame": int(m.group(1)), "diff": int(m.group(2)),
                                      "display": m.group(4) == "on", "sprites": int(m.group(5)),
                                      "bands": int(m.group(6))})
        elif line.startswith("Sprites: ") and "frames checked" in line:
            r["sprites"] = dict(zip(("checked", "errors", "overflow", "collision"),
                                    grab(line, r"(\d+) frames checked, (\d+) with differences, "
                                         r"(\d+) with overflow, (\d+) with collision", (0,) * 4)))
        elif line.startswith("Cartridge RAM:"):
            r["cart_ram"] = line.split(":", 1)[1].strip()
        elif line.startswith("Display: frame"):
            r["display_changes"] += 1
        elif (line.startswith(("ERROR", "WARNING", "Z80 translator:", "Check:", "compose:",
                               "Sprites: frame", "Picture: DrawCels"))):
            r["anomalies"].append(line)
    try:
        err = open(err_path, errors="replace").read().splitlines()
    except OSError:
        err = []
    r["sim_us"] = None
    r["fatal"] = []
    for line in err:
        m = re.match(r"sim: \d+ instructions, .*?, (\d+) us", line)
        if m:
            r["sim_us"] = int(m.group(1))
        elif not line.startswith("mark "):
            r["fatal"].append(line)
    return r

# ---------------------------------------------------------------- analysis


def analyse(name, size, frames_wanted, pad, run, parsed):
    """Reduces the parsed log to the figures of the report."""
    g = {"name": name, "size": size, "frames_wanted": frames_wanted, "pad": pad,
         "wall": run["wall"], "status": "ok", "detail": ""}
    s = parsed["summary"]
    if run["timeout"]:
        g["status"] = "timeout"
        g["detail"] = "; ".join(parsed["fatal"][:2])
    elif run["returncode"] != 0 and s is None:
        g["status"] = "crashed"
        g["detail"] = "exit %d; %s" % (run["returncode"], "; ".join(parsed["fatal"][:2]))
    elif parsed["stopped"]:
        st = parsed["stopped"]
        g["status"] = "stopped"
        g["detail"] = "at frame %d, exit reason %d, PC $%04X" % (st["frame"], st["reason"], st["pc"])
    elif s is None:
        g["status"] = "no summary"
        g["detail"] = "; ".join(parsed["fatal"][:2])
    elif s["frames"] != frames_wanted:
        g["status"] = "short"
        g["detail"] = "%d of %d frames" % (s["frames"], frames_wanted)
    g["frames"] = s["frames"] if s else 0
    g["rom"] = parsed["rom"]
    g["cart_ram"] = parsed["cart_ram"]
    g["summary"] = s
    g["vdp"] = parsed["vdp_summary"]
    g["picture"] = parsed["picture_summary"]
    g["histogram"] = parsed["histogram"]
    g["anomalies"] = parsed["anomalies"][:6]
    g["n_anomalies"] = len(parsed["anomalies"])
    g["sim_us"] = parsed["sim_us"]
    g["display_changes"] = parsed["display_changes"]

    ws = parsed["windows"]
    g["n_windows"] = len(ws)
    if ws:
        g["emu_min"] = min(w["emu"] for w in ws)
        g["emu_max"] = max(w["emu"] for w in ws)
        g["upd_min"] = min(w["upd"] for w in ws)
        g["upd_max"] = max(w["upd"] for w in ws)
        g["total_min"] = min(w["total"] for w in ws)
        g["total_max"] = max(w["total"] for w in ws)
        # The busiest window, the first one (start-up uploads) left out
        # when the run is long enough for it not to matter.
        busiest = max(ws[1:] if len(ws) >= 10 else ws, key=lambda w: w["total"])
        g["busiest"] = busiest
        g["windows_over"] = sum(1 for w in ws if w["over"])
        g["windows_clean"] = sum(1 for w in ws if w["over"] == 0)
        g["idle_min"] = min(w["idle"] for w in ws)
        g["idle_max"] = max(w["idle"] for w in ws)
        g["irq_max"] = max(w["irq"] for w in ws)
        g["irq_min"] = min(w["irq"] for w in ws)
    # Worst frames by total time, with what they did.
    slow = sorted(parsed["slow"], key=lambda x: -x["total"])
    g["worst"] = slow[:3]
    g["n_slow"] = len(parsed["slow"])
    # Per-frame picture features over the run.
    p = parsed["picture_summary"]
    n = g["frames"] or 1
    if p:
        g["per_frame"] = {k: p[k] / n for k in ("sprites", "pieces", "strips", "patches",
                                                  "tiles", "cells")}
    # Pictures compared with the reference view.
    cs = parsed["compares"]
    g["pics"] = len(cs)
    g["pics_on"] = sum(1 for c in cs if c["display"])
    diffs = [c for c in cs if c["diff"]]
    g["pics_diff"] = len(diffs)
    g["pics_diff_bands"] = sum(1 for c in diffs if c["bands"])
    g["pics_diff_frames"] = [c["frame"] for c in diffs[:8]]
    worst = max(diffs, key=lambda c: c["diff"]) if diffs else None
    g["pics_worst"] = (worst["frame"], worst["diff"]) if worst else None
    g["sprites"] = parsed["sprites"]
    return g

# ---------------------------------------------------------------- pictures


def make_sheets(workdir, dump_every, differing):
    """A contact sheet of the reference pictures at multiples of the dump
    period, and side-by-side pictures of the frames the harness found
    differing (its comparison treats the two blacks 000 and 001 as equal,
    so the PPM pairs themselves are not compared here)."""
    try:
        from PIL import Image, ImageChops, ImageDraw
    except ImportError:
        return
    views = sorted(glob.glob(os.path.join(workdir, "view_*.ppm")))
    regular = [v for v in views if int(os.path.basename(v)[5:11]) % dump_every == 0]
    if regular:
        per_row = 8
        tw, th = 128, 96
        rows = (len(regular) + per_row - 1) // per_row
        sheet = Image.new("RGB", (per_row * tw, rows * (th + 12)), (32, 32, 32))
        draw = ImageDraw.Draw(sheet)
        for i, v in enumerate(regular):
            try:
                im = Image.open(v).resize((tw, th))
            except OSError:
                continue
            x = (i % per_row) * tw
            y = (i // per_row) * (th + 12)
            sheet.paste(im, (x, y + 12))
            draw.text((x + 2, y), os.path.basename(v)[5:11].lstrip("0") or "0", fill=(220, 220, 220))
        sheet.save(os.path.join(workdir, "sheet.png"))
    for v in views:
        c = v.replace("view_", "cels_")
        if int(os.path.basename(v)[5:11]) not in differing or not os.path.exists(c):
            continue
        try:
            a = Image.open(v).convert("RGB")
            b = Image.open(c).convert("RGB")
        except OSError:
            continue
        d = ImageChops.difference(a, b)
        out = Image.new("RGB", (256 * 3 + 8, 192), (32, 32, 32))
        out.paste(a, (0, 0))
        out.paste(b, (260, 0))
        out.paste(d.point(lambda x: 255 if x else 0), (520, 0))
        out.save(os.path.join(workdir, "diff_%s.png" % os.path.basename(v)[5:11]))

# ---------------------------------------------------------------- running


def run_one(args, name, path, frames, pad, workdir):
    os.makedirs(workdir, exist_ok=True)
    for f in glob.glob(os.path.join(workdir, "*.ppm")) + glob.glob(os.path.join(workdir, "*.png")):
        os.remove(f)
    cmd = [os.path.join(HERE, "sim"), "-img", os.path.join(HERE, "sim.bin"),
           "-sym", os.path.join(HERE, "sim.sym"), "-rom", path,
           "-frames", str(frames), "-dump", str(args.dump), "-dumpdiff", "16"]
    env = dict(os.environ)
    env["SIMPAD"] = pad
    env.pop("SIMPROF", None)
    env.pop("SIMDIS", None)
    t0 = time.time()
    timeout = False
    with open(os.path.join(workdir, "log.txt"), "w") as out, \
            open(os.path.join(workdir, "sim.err"), "w") as err:
        proc = subprocess.Popen(cmd, stdout=out, stderr=err, env=env, cwd=workdir)
        try:
            proc.wait(timeout=args.timeout)
        except subprocess.TimeoutExpired:
            timeout = True
            # The model prints where it is on SIGUSR1.
            try:
                proc.send_signal(signal.SIGUSR1)
                proc.wait(timeout=5)
            except (subprocess.TimeoutExpired, OSError):
                proc.kill()
                proc.wait()
    run = {"wall": time.time() - t0, "timeout": timeout, "returncode": proc.returncode}
    parsed = parse_log(os.path.join(workdir, "log.txt"), os.path.join(workdir, "sim.err"))
    if not args.nosheets:
        make_sheets(workdir, args.dump, {c["frame"] for c in parsed["compares"] if c["diff"]})
    return analyse(name, os.path.getsize(path), frames, pad, run, parsed)

# ---------------------------------------------------------------- report


def ms(us):
    return "%.1f" % (us / 1000.0)


def kcount(n):
    if n >= 1000000:
        return "%.1fM" % (n / 1e6)
    if n >= 10000:
        return "%dk" % (n // 1000)
    return str(n)


def game_lines(g):
    out = []
    head = "%s (%d KiB): %s" % (g["name"], g["size"] // 1024, g["status"].upper())
    if g["detail"]:
        head += " " + g["detail"]
    head += ", %d frames, %.0f s on the PC" % (g["frames"], g["wall"])
    if g["pad"]:
        head += ", pad script %d entries" % len(g["pad"].split(","))
    out.append(head)
    if g["rom"]:
        r = g["rom"]
        line = "  ROM translation %d blocks, %d instructions, %s ms" % (
            r["blocks"], r["insns"], ms(r["us"]))
        if r["evictions"]:
            line += ", %d evictions during it" % r["evictions"]
        if g["cart_ram"]:
            line += "; cartridge RAM %s" % g["cart_ram"]
        out.append(line)
    s = g["summary"]
    if not s:
        for a in g["anomalies"][:3]:
            out.append("  " + a[:150])
        return out
    pct = 100.0 * s["over"] / (s["frames"] or 1)
    line = "  emulation %s ms (windows %s-%s), update %s ms (%s-%s), total %s ms with %s ms of drawing; %d over budget (%.1f%%)" % (
        ms(s["emu"]), ms(g.get("emu_min", 0)), ms(g.get("emu_max", 0)),
        ms(s["upd"]), ms(g.get("upd_min", 0)), ms(g.get("upd_max", 0)),
        ms(s["total"]), ms(DRAW_US), s["over"], pct)
    out.append(line)
    if g.get("busiest"):
        b = g["busiest"]
        out.append("  busiest window %d-%d: emulation %s, update %s, total %s ms, %d over budget, Z80 idle %d%%; idle over the run %d%% (windows %d-%d%%)" % (
            b["first"], b["last"], ms(b["emu"]), ms(b["upd"]), ms(b["total"]), b["over"], b["idle"],
            s["idle"], g["idle_min"], g["idle_max"]))
    if g["worst"]:
        parts = []
        for w in g["worst"]:
            why = []
            if w["blocks"]:
                why.append("%d blocks%s" % (w["blocks"], " after an eviction" if w["evicted"] else ""))
            if w["cells"] >= 100:
                why.append("%d cells" % w["cells"])
            if w["tiles"] >= 20:
                why.append("%d tiles" % w["tiles"])
            if w["interp"] >= 500:
                why.append("%d interpreted" % w["interp"])
            if w["data_w"] >= 1000:
                why.append("%d data writes" % w["data_w"])
            parts.append("%s ms at frame %d (%s)" % (ms(w["total"]), w["frame"],
                                                      ", ".join(why) or "emulation %s ms" % ms(w["emu"])))
        out.append("  worst frames: %s; %d frames over budget logged" % ("; ".join(parts), g["n_slow"]))
    n = s["frames"] or 1
    out.append("  per frame: %.1f IRQ, %.2f NMI, %.1f bank switches, %d VDP data writes, %d data reads, %d control writes, %d status reads, %d counter reads, %.1f PSG writes" % (
        s["irq"] / n, s["nmi"] / n, s["banks"] / n,
        (g["vdp"] or {}).get("data_w", 0) // n, (g["vdp"] or {}).get("data_r", 0) // n,
        (g["vdp"] or {}).get("ctrl_w", 0) // n, (g["vdp"] or {}).get("stat_r", 0) // n,
        (g["vdp"] or {}).get("counter_r", 0) // n, (g["vdp"] or {}).get("psg", 0) / n))
    out.append("  translator: %d blocks while running (%d at once in %s ms, %d promoted), %s interpreted instructions, %d evictions (%d blocks), %d busy-wait loops" % (
        s["blocks"], s["sync"], ms(s["sync_us"]), s["promoted"], kcount(s["interp"]),
        s["evictions"], s["evicted"], s["busy_loops"]))
    p = g["picture"]
    if p:
        pf = g["per_frame"]
        out.append("  picture: %.1f sprite cels, %.1f pieces, %.1f priority strips, %.1f patches, %.1f tiles, %.1f cells per frame; %d frames in scroll bands, %d in name table bands, %d partly blanked, %d bands dropped, %d short of layers; %d frames past the sprite limit (%d runs dropped), %d with overflow, %d with collision; %d display changes" % (
            pf["sprites"], pf["pieces"], pf["strips"], pf["patches"], pf["tiles"], pf["cells"],
            p["bands"], p["ntbands"], p["dbands"], p["dropped"], p["layers"], p["spr_partial"],
            p["spr_dropped"], p["overflow"], p["collision"], g["display_changes"]))
    line = "  pictures: %d compared (%d with the display on), %d identical, %d differing" % (
        g["pics"], g["pics_on"], g["pics"] - g["pics_diff"], g["pics_diff"])
    if g["pics_diff"]:
        line += " (%d in frames with scroll bands; worst frame %d: %d pixels; frames %s)" % (
            g["pics_diff_bands"], g["pics_worst"][0], g["pics_worst"][1],
            " ".join(str(f) for f in g["pics_diff_frames"]))
    if g["sprites"]:
        sp = g["sprites"]
        line += "; sprite evaluation %d frames checked, %d with differences" % (sp["checked"], sp["errors"])
    out.append(line)
    if g["anomalies"]:
        out.append("  %d anomalies: %s" % (g["n_anomalies"], " | ".join(a[:110] for a in g["anomalies"][:3])))
    return out


def table(games):
    hdr = "%-18s %-8s %6s %6s %6s %6s %6s %8s %7s %s" % (
        "cartridge", "status", "emu", "upd", "total", "over%", "worst", "pictures", "sprites", "notes")
    rows = [hdr, "-" * len(hdr)]
    for g in games:
        s = g["summary"]
        if s:
            notes = []
            p = g["picture"] or {}
            if g.get("irq_max", 0) >= 200:
                notes.append("line IRQ")
            if p.get("ntbands"):
                notes.append("nt bands")
            if p.get("spr_partial"):
                notes.append("spr>8")
            if (g["vdp"] or {}).get("data_r"):
                notes.append("VRAM reads")
            if (g["vdp"] or {}).get("counter_r", 0) >= 100:
                notes.append("counters")
            if s["evictions"]:
                notes.append("%d evict" % s["evictions"])
            if g["n_anomalies"]:
                notes.append("%d anomalies" % g["n_anomalies"])
            if g["pics_on"] == 0:
                notes.append("display never on")
            rows.append("%-18s %-8s %6s %6s %6s %5.1f%% %6s %8s %7s %s" % (
                g["name"][:18], g["status"][:8], ms(s["emu"]), ms(s["upd"]), ms(s["total"]),
                100.0 * s["over"] / (s["frames"] or 1), ms(s["total_worst"]),
                "%d/%d" % (g["pics"] - g["pics_diff"], g["pics"]),
                ("%d/%d" % (g["sprites"]["checked"] - g["sprites"]["errors"], g["sprites"]["checked"])
                 if g["sprites"] else "-"),
                ", ".join(notes)))
        else:
            rows.append("%-18s %-8s %s" % (g["name"][:18], g["status"][:8], g["detail"][:60]))
    return rows


def compare_reports(games, prev):
    """Lines describing what changed since the previous report."""
    out = []
    prev_games = {g["name"]: g for g in prev.get("games", [])}
    for g in games:
        o = prev_games.get(g["name"])
        if not o:
            out.append("%s: new in the panel" % g["name"])
            continue
        changes = []
        if o["status"] != g["status"] or (o["detail"] != g["detail"] and g["status"] != "ok"):
            changes.append("status %s %s -> %s %s" % (o["status"], o["detail"], g["status"], g["detail"]))
        s, so = g["summary"], o["summary"]
        if s and so and s["frames"] == so["frames"]:
            for key, label in (("emu", "emulation"), ("upd", "update"), ("total", "total")):
                a, b = so[key], s[key]
                if a and abs(b - a) / float(a) >= 0.01:
                    changes.append("%s %s -> %s ms (%+.1f%%)" % (label, ms(a), ms(b), 100.0 * (b - a) / a))
            if s["over"] != so["over"]:
                changes.append("over budget %d -> %d" % (so["over"], s["over"]))
            if s["total_worst"] // 1000 != so["total_worst"] // 1000:
                changes.append("worst frame %s -> %s ms" % (ms(so["total_worst"]), ms(s["total_worst"])))
            if s["evictions"] != so["evictions"]:
                changes.append("evictions %d -> %d" % (so["evictions"], s["evictions"]))
            if s["blocks"] != so["blocks"]:
                changes.append("blocks while running %d -> %d" % (so["blocks"], s["blocks"]))
        elif s and so:
            changes.append("frames %d -> %d, times not compared" % (so["frames"], s["frames"]))
        if g["pics_diff"] != o["pics_diff"] or g["pics"] != o["pics"]:
            changes.append("pictures differing %d/%d -> %d/%d" % (o["pics_diff"], o["pics"],
                                                                  g["pics_diff"], g["pics"]))
        if (g["sprites"] or {}).get("errors", 0) != (o["sprites"] or {}).get("errors", 0):
            changes.append("sprite evaluation differences %d -> %d" % (
                (o["sprites"] or {}).get("errors", 0), (g["sprites"] or {}).get("errors", 0)))
        if g["n_anomalies"] != o["n_anomalies"]:
            changes.append("anomalies %d -> %d" % (o["n_anomalies"], g["n_anomalies"]))
        if o["pad"] != g["pad"] or o["frames_wanted"] != g["frames_wanted"]:
            changes.append("pad script or frame count changed")
        if changes:
            out.append("%s: %s" % (g["name"], "; ".join(changes)))
    names = {g["name"] for g in games}
    for name in prev_games:
        if name not in names:
            out.append("%s: no longer in the panel" % name)
    if not out:
        out.append("no change")
    return out


def git_state():
    try:
        sha = subprocess.check_output(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT,
                                      stderr=subprocess.DEVNULL).decode().strip()
        dirty = subprocess.check_output(["git", "status", "--porcelain", "--", "src",
                                         "tools/arm60model/harness.c", "tools/arm60model/sim.c",
                                         "tools/arm60model/hstub.s"], cwd=ROOT,
                                        stderr=subprocess.DEVNULL).decode().strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown", False
    return sha, bool(dirty)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("-roms", default=os.path.join(ROOT, "takeme", "roms"))
    ap.add_argument("-panel", default=os.path.join(HERE, "panel.txt"))
    ap.add_argument("-frames", type=int, default=3000, help="default frame count (3000)")
    ap.add_argument("-jobs", type=int, default=0, help="parallel runs (default: the CPUs)")
    ap.add_argument("-only", default="", help="comma-separated cartridge names to run")
    ap.add_argument("-out", default=os.path.join(HERE, "batch"), help="work directories")
    ap.add_argument("-reports", default=os.path.join(HERE, "reports"))
    ap.add_argument("-against", default="", help="previous report (.json) to compare with")
    ap.add_argument("-timeout", type=int, default=900, help="seconds per cartridge (900)")
    ap.add_argument("-dump", type=int, default=100, help="compare the pictures every n frames (100)")
    ap.add_argument("-nosheets", action="store_true", help="no PNG contact sheets")
    ap.add_argument("-label", default="", help="a note written in the report header")
    args = ap.parse_args()

    for f in ("sim", "sim.bin", "sim.sym"):
        if not os.path.exists(os.path.join(HERE, f)):
            sys.exit("%s missing: run mk.sh after make" % f)
    img_time = os.path.getmtime(os.path.join(HERE, "sim.bin"))
    stale = [o for o in glob.glob(os.path.join(ROOT, "build", "*.o")) if os.path.getmtime(o) > img_time]
    if stale:
        print("warning: %d objects of build/ are newer than sim.bin (run mk.sh)" % len(stale))

    panel = read_panel(args.panel)
    roms = sorted(f for f in os.listdir(args.roms) if f.lower().endswith((".sms", ".gg")))
    if args.only:
        wanted = set(args.only.split(","))
        roms = [r for r in roms if r in wanted or os.path.splitext(r)[0] in wanted]
    if not roms:
        sys.exit("no cartridge found in %s" % args.roms)
    jobs = args.jobs or min(len(roms), os.cpu_count() or 1)
    sha, dirty = git_state()
    stamp = datetime.datetime.now()
    print("%d cartridges, %d jobs, commit %s%s, sim.bin of %s" % (
        len(roms), jobs, sha, " (uncommitted changes)" if dirty else "",
        datetime.datetime.fromtimestamp(img_time).strftime("%Y-%m-%d %H:%M")))

    t0 = time.time()
    games = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
        futures = {}
        for rom in roms:
            frames, pad = panel.get(rom, (None, DEFAULT_PAD))
            frames = frames or args.frames
            workdir = os.path.join(args.out, os.path.splitext(rom)[0])
            fut = ex.submit(run_one, args, rom, os.path.join(args.roms, rom), frames, pad, workdir)
            futures[fut] = rom
        for fut in concurrent.futures.as_completed(futures):
            g = fut.result()
            games.append(g)
            print("  %-20s %-8s %4d frames in %5.0f s %s" % (g["name"], g["status"], g["frames"],
                                                             g["wall"], g["detail"]))
    games.sort(key=lambda g: g["name"])
    wall = time.time() - t0

    os.makedirs(args.reports, exist_ok=True)
    base = "%s-%s%s" % (stamp.strftime("%Y%m%d-%H%M"), sha, "-dirty" if dirty else "")
    prev = None
    prev_name = ""
    if args.against:
        prev_name = args.against
    else:
        olds = sorted(glob.glob(os.path.join(args.reports, "*.json")))
        if olds:
            prev_name = olds[-1]
    if prev_name:
        try:
            prev = json.load(open(prev_name))
        except (OSError, ValueError):
            prev = None

    lines = []
    lines.append("Model batch report: %s, commit %s%s, %d cartridges, %d frames by default, %.0f s on %d jobs%s" % (
        stamp.strftime("%Y-%m-%d %H:%M"), sha, " with uncommitted changes" if dirty else "",
        len(games), args.frames, wall, jobs, (", " + args.label) if args.label else ""))
    lines.append("Times in ms per frame in the model (ARM60 cycle model, DrawCels taken as %s ms); the budget is %s ms. Pictures: cel engine model against the CPU reference view, every %d frames and at every frame with a scroll or display band." % (
        ms(DRAW_US), ms(FRAME_US), args.dump))
    lines.append("")
    lines.extend(table(games))
    lines.append("")
    for g in games:
        lines.extend(game_lines(g))
        lines.append("")
    if prev:
        lines.append("Changes since %s (commit %s, %s):" % (
            os.path.basename(prev_name), prev.get("commit", "?"), prev.get("date", "?")))
        lines.extend("  " + l for l in compare_reports(games, prev))
    else:
        lines.append("No previous report to compare with.")
    text = "\n".join(lines) + "\n"
    txt_path = os.path.join(args.reports, base + ".txt")
    with open(txt_path, "w") as f:
        f.write(text)
    with open(os.path.join(args.reports, base + ".json"), "w") as f:
        json.dump({"date": stamp.strftime("%Y-%m-%d %H:%M"), "commit": sha, "dirty": dirty,
                   "frames": args.frames, "label": args.label, "games": games}, f, indent=1)
    print()
    print(text)
    print("report: %s" % txt_path)


if __name__ == "__main__":
    main()
