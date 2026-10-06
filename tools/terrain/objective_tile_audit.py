"""Audit objective placement against the 3D terrain tiles their layouts were built for.

Most objective types are designed for one terrain tile and say so in their name: "D3C Chernobyl II" is
a nuclear plant laid out on tile HFARMD3C, "79B Port" a port on HCOST79B. The tile's code is the last
three characters of its file name. If the campaign places the objective on a cell whose 3D tile has a
different code, its buildings are drawn over the wrong ground (a port off its quay, a plant whose
campaign cell is sea), and the campaign AI -- which plans on the campaign cell -- disagrees with what
the player sees.

For every objective whose type name starts with a three-character tile code, this finds the 3D tile
under its campaign cell and, if the code differs, the nearest cell (within --radius km) whose tile has
the code. Read only.

    python objective_tile_audit.py [--radius 4] [--json out.json]
"""

import argparse
import collections
import json
import os
import re
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "campaign-editor"))

import tilesurvey as ts  # noqa: E402
from ffcamp import theater  # noqa: E402
from ffcamp.workspace import TheaterWorkspace  # noqa: E402

CODE = re.compile(r"^\s*([0-9A-Fa-f]{3})[\s_]?")
TYPE_NAMES = {1: "airbase", 2: "airstrip", 3: "armybase", 4: "beach", 5: "border", 6: "bridge", 7: "chemical",
              8: "city", 9: "com", 10: "depot", 11: "factory", 12: "ford", 13: "fort", 14: "hilltop",
              15: "intersect", 16: "nav beacon", 17: "nuclear", 18: "pass", 19: "port", 20: "power",
              21: "radar", 22: "radio", 23: "rail term", 24: "railroad", 25: "refinery", 26: "road",
              27: "sea", 28: "town", 29: "village", 30: "hartsite", 31: "sam site"}


def tile_codes(th):
    """[y][x] three-character code of the 3D tile at each campaign km."""
    psz = th.map.post_size
    codes = np.empty((th.bh * 4, th.bw * 4), dtype=object)
    names = {}
    for br in range(th.bh):
        for bc in range(th.bw):
            o = int(th.offsets[br * th.bw + bc])
            buf = np.array(th.posts[o:o + psz * 256]).reshape(256, psz)
            if th.map.large_terrain:
                tid = (buf[:, 0].astype(np.uint32) | (buf[:, 1].astype(np.uint32) << 8)
                       | (buf[:, 2].astype(np.uint32) << 16) | (buf[:, 3].astype(np.uint32) << 24))
            else:
                tid = buf[:, 0].astype(np.uint32) | (buf[:, 1].astype(np.uint32) << 8)
            tid = tid.reshape(16, 16)
            for r in range(4):
                for c in range(4):
                    t = int(tid[r * 4, c * 4])
                    if t not in names:
                        n = (th.tile_name(t) or "").split(".")[0]
                        names[t] = (n[-3:].upper(), n)
                    codes[br * 4 + r, bc * 4 + c] = names[t]
    return codes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", default=r"C:\FreeFalcon6")
    ap.add_argument("--theater-name", default="Korea")
    ap.add_argument("--campaign", default="save0.cam")
    ap.add_argument("--radius", type=int, default=4)
    ap.add_argument("--json", default=None)
    a = ap.parse_args()

    t = [t for t in theater.load_theaters(a.game) if t.name == a.theater_name][0]
    ws = TheaterWorkspace(a.game, os.path.relpath(t.path, a.game))
    cam = ws.objectives(a.campaign)
    th = ts.Theater(ts.default_theater(), 0)
    codes = tile_codes(th)
    H, W = codes.shape

    names = {}
    try:
        from ffcamp import names as nm
        names = nm.load(ws.campaign_dir, "KOREA")
    except Exception:  # noqa: BLE001
        pass

    stats = collections.Counter()
    offsets = collections.Counter()
    by_type = collections.defaultdict(collections.Counter)
    rows = []
    for o in cam.objectives:
        _tname, row = ws.db.data_row(o["classIndex"])
        if row is None:
            continue
        m = CODE.match(row["Name"])
        typ = ws.db.class_rows()[o["classIndex"]].get("type", 0) if False else None
        if not m:
            stats["no tile code in type name"] += 1
            continue
        want = m.group(1).upper()
        x, y = o["x"], o["y"]
        if not (0 <= x < W and 0 <= y < H):
            continue
        have = codes[y, x][0]
        if have == want:
            stats["on its tile"] += 1
            continue
        best = None
        for r in range(1, a.radius + 1):
            for dy in range(-r, r + 1):
                for dx in range(-r, r + 1):
                    if max(abs(dx), abs(dy)) != r:
                        continue
                    xx, yy = x + dx, y + dy
                    if 0 <= xx < W and 0 <= yy < H and codes[yy, xx][0] == want:
                        d = dx * dx + dy * dy
                        if best is None or d < best[0]:
                            best = (d, dx, dy)
            if best:
                break
        if best:
            stats["tile found %d-%d km away" % (1, a.radius)] += 1
            offsets[(best[1], best[2])] += 1
        else:
            stats["tile not found within %d km" % a.radius] += 1
        rows.append({"campId": o["campId"], "x": x, "y": y, "class": row["Name"].strip(), "want": want,
                     "tileHere": codes[y, x][1],
                     "fix": None if not best else [x + best[1], y + best[2]],
                     "shift": None if not best else [best[1], best[2]]})

    total = sum(stats.values())
    print("objectives: %d" % total)
    for k, v in stats.most_common():
        print("  %-36s %5d  (%.1f%%)" % (k, v, 100.0 * v / total))
    print("\nshift to the matching tile (dx, dy km), most common:")
    for (dx, dy), v in offsets.most_common(12):
        print("  %+d,%+d  %4d" % (dx, dy, v))
    print("\nexamples:")
    for r in rows[:25]:
        print("  #%-5d (%d,%d) %-22s wants %s, tile here %-10s -> %s" % (
            r["campId"], r["x"], r["y"], r["class"], r["want"], r["tileHere"],
            "(%d,%d)" % tuple(r["fix"]) if r["fix"] else "not found"))
    if a.json:
        json.dump(rows, open(a.json, "w"), indent=1)
        print("\nwrote %s (%d misplaced objectives)" % (a.json, len(rows)))


if __name__ == "__main__":
    main()
