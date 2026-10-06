"""Make the campaign cells under coastal objectives land, where the 3D terrain shows land.

The campaign's 1 km cover grid (campaign/SAVE/KOREA.THR) counts the boundary cell of most coastlines
as sea, while the 3D tile there is mostly land (cover_compare.py: a 21,095-cell coastal fringe). Ports,
Togwon-ni Nuclear Power Plant (#615) and two airbases sit in that fringe, so the campaign AI thinks they
are at sea: ground units could not path to them and retried forever, and capture (which needs the
unit in the objective's own cell) was impossible.

This changes ONLY the cells under such objectives -- not bridges (crossed by road over real rivers),
not PORTS (ships dock by sailing onto the port's own cell, navunit.cpp TaskForceOrderStation/NavalRoute,
and the naval path search rejects a land destination -- ground units reach ports through the
WaterObjectiveFix code instead), not the rest of the fringe (that would move shipping lanes) -- and
only where the 3D tile there is under half water. The cover becomes Urban for plants and airbases
(Plain for anything else); relief, road and rail bits are kept.

Safety, as flatten_airstrips.py: dry run by default; --write first copies the original next to it in
backup-thr-objectives/ with a SHA-256 manifest and a rollback .bat (plain copy, no Python); refuses to
patch twice; --rollback restores.

    python patch_thr_objectives.py              # list what would change
    python patch_thr_objectives.py --write      # back up, then patch
    python patch_thr_objectives.py --rollback
"""

import argparse
import datetime
import hashlib
import json
import os
import shutil
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "campaign-editor"))

import cover_compare as cc  # noqa: E402
import tilesurvey as ts  # noqa: E402
from ffcamp import theater  # noqa: E402
from ffcamp.workspace import TheaterWorkspace  # noqa: E402

BACKUP_DIR = "backup-thr-objectives"
MANIFEST = "manifest.json"
TYPE_BRIDGE = 6
TYPE_PORT = 19
URBAN_TYPES = {1: "airbase", 2: "airstrip", 17: "nuclear", 20: "power plant", 25: "refinery"}
COVER_URBAN, COVER_PLAIN = 7, 3


def sha256(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()


def candidates(game, thr_path, campaign, theater_name):
    t = [t for t in theater.load_theaters(game) if t.name == theater_name][0]
    ws = TheaterWorkspace(game, os.path.relpath(t.path, game))
    cam = ws.objectives(campaign)
    cells, mx, my = cc.load_thr(thr_path)
    frac, _sea = cc.load_3d(ts.default_theater())
    out = []
    for o in cam.objectives:
        x, y = o["x"], o["y"]
        if not (0 <= x < mx and 0 <= y < my) or (cells[y, x] & 0x0F) != 0:
            continue
        otype = ws.db.class_rows()[o["classIndex"]]["classInfo_"][2]
        if otype in (TYPE_BRIDGE, TYPE_PORT) or frac[y, x] >= 0.5:
            continue
        _t, row = ws.db.data_row(o["classIndex"])
        out.append({"campId": o["campId"], "x": x, "y": y, "type": int(otype),
                    "class": (row or {}).get("Name", "").strip(), "water3d": round(float(frac[y, x]), 2),
                    "cover": COVER_URBAN if otype in URBAN_TYPES else COVER_PLAIN})
    return out, mx, my


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", default=r"C:\FreeFalcon6")
    ap.add_argument("--thr", default=r"C:\FreeFalcon6\campaign\SAVE\KOREA.THR")
    ap.add_argument("--campaign", default="save0.cam")
    ap.add_argument("--theater-name", default="Korea")
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--rollback", action="store_true")
    a = ap.parse_args()

    bdir = os.path.join(os.path.dirname(a.thr), BACKUP_DIR)
    man_path = os.path.join(bdir, MANIFEST)
    name = os.path.basename(a.thr)

    if a.rollback:
        man = json.load(open(man_path))
        src = os.path.join(bdir, name)
        if sha256(src) != man["original_sha256"]:
            raise SystemExit("backup does not match the manifest -- not restoring")
        shutil.copy2(src, a.thr)
        os.remove(man_path)
        print("restored %s from %s" % (a.thr, src))
        return

    cand, mx, my = candidates(a.game, a.thr, a.campaign, a.theater_name)
    print("%d objective cells the campaign calls sea but the 3D terrain shows as mostly land:" % len(cand))
    for c in cand:
        print("  #%-5d (%d,%d) type %-3d %-22s 3D water %3.0f%% -> %s" % (
            c["campId"], c["x"], c["y"], c["type"], c["class"], 100 * c["water3d"],
            "Urban" if c["cover"] == COVER_URBAN else "Plain"))
    if not a.write:
        print("\ndry run; --write to back up and patch")
        return
    if os.path.exists(man_path):
        raise SystemExit("already patched (%s exists); --rollback first" % man_path)

    os.makedirs(bdir, exist_ok=True)
    shutil.copy2(a.thr, os.path.join(bdir, name))
    data = bytearray(open(a.thr, "rb").read())
    for c in cand:
        i = 4 + c["x"] * my + c["y"]            # TheaterCells[x * Map_Max_Y + y]
        data[i] = (data[i] & 0xF0) | c["cover"]
    open(a.thr, "wb").write(data)
    json.dump({"created": datetime.datetime.now().isoformat(timespec="seconds"), "file": a.thr,
               "original_sha256": sha256(os.path.join(bdir, name)), "patched_sha256": sha256(a.thr),
               "cells": cand}, open(man_path, "w"), indent=1)
    with open(os.path.join(bdir, "rollback-thr.bat"), "w") as f:
        f.write('@echo off\r\ncopy /Y "%%~dp0%s" "%%~dp0..\\%s"\r\ndel "%%~dp0%s"\r\necho restored %s\r\npause\r\n'
                % (name, name, MANIFEST, name))
    print("\nbacked up to %s, patched %d cells, rollback: %s" % (bdir, len(cand), os.path.join(bdir, "rollback-thr.bat")))


if __name__ == "__main__":
    main()
