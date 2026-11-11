"""Give a bridge objective whose cell no ground unit can reach a road-bit causeway to the shore.

    python connect_bridges.py --tdf Theaters\\Taiwan\\taiwan.tdf --thr C:\\FreeFalcon6\\Theaters\\Taiwan\\campaign\\Taiwan.thr \
                              [--max-cells 6] [--dry-run]

The engine enters a water cell that carries the road bit (a bridge or port) only by an orthogonal step, and only
from a cell it can already stand on. Taiwan has 356 bridge objectives, 284 of them on water cells, and three
whose cell nothing reaches: #1647 and #556 sit on one-to-thirteen-cell sandbars that touch no real land, #430
(Hai-Kou) has open water two to three cells on every side. A unit ordered to such a bridge searched the whole
land mass for it and gave up (campsim save2: 17 km "no grid path" legs, 1,100 failures per 100 h).

For each such bridge this finds the shortest orthogonal chain of cells to land that is part of a big land mass
(>= --min-land cells, joined by road bits as the engine joins them) and sets the road bit on the water cells of
the chain. Land cells are left as they are.

Writes <thr>.bak-pre-bridges first and refuses to run twice (--force redoes it from that backup).
"""
import argparse
import glob
import os
import shutil
import struct
import sys
from collections import deque

import numpy as np
from scipy import ndimage as ndi

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "campaign-editor"))
import cover_compare as cc  # noqa: E402
from ffcamp.workspace import TheaterWorkspace  # noqa: E402

D8 = [(0, -1), (1, -1), (1, 0), (1, 1), (0, 1), (-1, 1), (-1, 0), (-1, -1)]
D4 = [(0, -1), (1, 0), (0, 1), (-1, 0)]


def reachable(cells, min_land):
    """Cells a ground unit can stand on and walk to from a big land mass, by the engine's step rule."""
    cov = (cells & 0x0F) != 0
    road = (cells & 0x40) != 0
    h, w = cells.shape
    lab, n = ndi.label(cov, structure=np.ones((3, 3)))
    sizes = np.bincount(lab.ravel())
    reach = np.isin(lab, [i for i in range(1, n + 1) if sizes[i] >= min_land])
    q = deque((int(x), int(y)) for y, x in zip(*np.where(reach)))
    while q:
        x, y = q.popleft()
        for k, (dx, dy) in enumerate(D8):
            nx, ny = x + dx, y + dy
            if 0 <= nx < w and 0 <= ny < h and not reach[ny, nx] and (cov[ny, nx] or (road[ny, nx] and k % 2 == 0)):
                reach[ny, nx] = True
                q.append((nx, ny))
    return reach


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gamedir", default=r"C:\FreeFalcon6")
    ap.add_argument("--tdf", default=r"Theaters\Taiwan\taiwan.tdf")
    ap.add_argument("--thr", required=True)
    ap.add_argument("--max-cells", type=int, default=6)
    ap.add_argument("--min-land", type=int, default=5000)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--force", action="store_true")
    a = ap.parse_args()

    ws = TheaterWorkspace(a.gamedir, a.tdf)
    objs = {}
    for path in sorted(glob.glob(os.path.join(ws.campaign_dir, "save*.cam"))):
        for o in ws.objectives(os.path.basename(path)).objectives:
            objs.setdefault((o["x"], o["y"], o["typeName"]), o)
    bridges = [o for o in objs.values() if o["typeName"] == "Bridge"]

    bak = a.thr + ".bak-pre-bridges"
    src = a.thr
    if os.path.exists(bak):
        if not a.force and not a.dry_run:
            sys.exit("refusing: %s exists; --force redoes it from it" % bak)
        src = bak
    cells, mx, my = cc.load_thr(src)
    h, w = cells.shape
    reach = reachable(cells, a.min_land)
    new = cells.copy()
    done = 0
    for o in bridges:
        x0, y0 = o["x"], o["y"]
        if reach[y0, x0]:
            continue
        prev = {(x0, y0): None}
        q = deque([(x0, y0, 0)])
        goal = None
        while q and goal is None:
            x, y, d = q.popleft()
            if d >= a.max_cells + 1:
                continue
            for dx, dy in D4:
                nx, ny = x + dx, y + dy
                if 0 <= nx < w and 0 <= ny < h and (nx, ny) not in prev:
                    prev[(nx, ny)] = (x, y)
                    if reach[ny, nx]:
                        goal = (nx, ny)
                        break
                    q.append((nx, ny, d + 1))
        if goal is None:
            print("no chain within %d cells for bridge #%s at (%d,%d)" % (a.max_cells, o["id"][0], x0, y0))
            continue
        chain = []
        c = prev[goal]
        while c is not None:
            chain.append(c)
            c = prev[c]
        painted = [(x, y) for x, y in chain if not (new[y, x] & 0x0F)]
        for x, y in painted:
            new[y, x] |= 0x40
        done += 1
        print("bridge #%s (%d,%d): chain of %d cells to land at %s, road bit set on %s"
              % (o["id"][0], x0, y0, len(chain), goal, painted))

    print("connected %d bridges" % done)
    if a.dry_run or not done:
        return
    if src == a.thr:
        shutil.copy2(a.thr, bak)
    with open(a.thr, "wb") as fh:
        fh.write(struct.pack("<hh", mx, my))
        fh.write(new.T.copy().tobytes())
    print("wrote %s, backup %s" % (a.thr, bak))


if __name__ == "__main__":
    main()
