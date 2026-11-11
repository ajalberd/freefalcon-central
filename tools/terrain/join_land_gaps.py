"""Join small land pieces that hold objectives or battalions to the land mass next to them, across a channel of
one or two water cells, in a theatre's cover grid.

    python join_land_gaps.py --tdf Theaters\\Taiwan\\taiwan.tdf --thr C:\\FreeFalcon6\\Theaters\\Taiwan\\campaign\\Taiwan.thr \
                             [--max-gap 3.2] [--dry-run]

The engine paths ground units cell by cell over the cover grid, so a piece cut off by a one-cell channel is an
island to it. Taiwan save2 has a China battalion on an 11-cell strip two cells off the Fujian coast, a depot on a
7-cell strip beside its army base, and a battalion pair on a 182-cell peninsula, none with an objective of its own:
the AI handed each the nearest objective across the channel and failed the grid path every tick.

A piece is joined when it holds an objective (not just a bridge) or a ground battalion (any save), is smaller than
--max-size cells, and its nearest cell is within --max-gap (Euclidean cells; 2.0 is one water cell between, 3.0 two
in a row) of a land mass of at least --min-target cells (islands are not joined to each other), unless a port objective lies within --port-keep cells of the new cells (a harbour channel
stays water for ships). The new cells take the cover/relief byte of the nearest land cell, road bit kept.
Land here is the engine's: a cover other than water, or a road bit (a bridge or port) on a water cell.

Writes <thr>.bak-pre-join first and refuses to run twice (--force redoes it from that backup).
"""
import argparse
import glob
import os
import shutil
import struct
import sys

import numpy as np
from scipy import ndimage as ndi

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "campaign-editor"))
import cover_compare as cc  # noqa: E402
from ffcamp.workspace import TheaterWorkspace  # noqa: E402

S8 = np.ones((3, 3), int)


def line_cells(a, b):
    """Cells strictly between a and b (y, x) on the straight line between them."""
    n = max(abs(b[0] - a[0]), abs(b[1] - a[1]))
    out = []
    for i in range(1, n):
        out.append((int(round(a[0] + (b[0] - a[0]) * i / n)), int(round(a[1] + (b[1] - a[1]) * i / n))))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gamedir", default=r"C:\FreeFalcon6")
    ap.add_argument("--tdf", default=r"Theaters\Taiwan\taiwan.tdf")
    ap.add_argument("--thr", required=True)
    ap.add_argument("--max-gap", type=float, default=3.2)
    ap.add_argument("--max-size", type=int, default=400)
    ap.add_argument("--port-keep", type=int, default=3)
    ap.add_argument("--min-target", type=int, default=5000, help="only join to a land mass at least this big (not island to island)")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--force", action="store_true")
    a = ap.parse_args()

    ws = TheaterWorkspace(a.gamedir, a.tdf)
    objs, units = {}, []
    for path in sorted(glob.glob(os.path.join(ws.campaign_dir, "save*.cam"))):
        n = os.path.basename(path)
        for o in ws.objectives(n).objectives:
            objs.setdefault((o["x"], o["y"], o["typeName"]), o)
        units += [u for u in ws.units(n).units if u["kind"] == "battalion"]
    objs = list(objs.values())
    ports = [(o["y"], o["x"]) for o in objs if o["typeName"] == "Port"]

    bak = a.thr + ".bak-pre-join"
    src = a.thr
    if os.path.exists(bak):
        if not a.force and not a.dry_run:
            sys.exit("refusing: %s exists; --force redoes it from it" % bak)
        src = bak
    cells, mx, my = cc.load_thr(src)
    land = ((cells & 0x0F) != 0) | ((cells & 0x40) != 0)
    lab, n = ndi.label(land, structure=S8)
    sizes = np.bincount(lab.ravel())
    holders = {}
    for o in objs:
        if o["typeName"] != "Bridge":          # a bridge's own sandbar is no reason to fill a river
            holders.setdefault(int(lab[o["y"], o["x"]]), []).append("obj %s %s" % (o["typeName"], o["id"]))
    for u in units:
        holders.setdefault(int(lab[u["y"], u["x"]]), []).append("bn %s" % (u["id"],))

    new = cells.copy()
    _, (iy, ix) = ndi.distance_transform_edt(~((cells & 0x0F) != 0), return_indices=True)
    joined = 0
    for p in sorted(holders):
        if p == 0 or sizes[p] >= a.max_size:
            continue
        mine = lab == p
        others = land & ~mine & (lab != 0)
        big = np.isin(lab, [i for i in range(1, n + 1) if sizes[i] >= a.min_target and i != p])
        if not big.any():
            continue
        d, (by, bx) = ndi.distance_transform_edt(~big, return_indices=True)
        ys, xs = np.where(mine)
        k = int(np.argmin(d[ys, xs]))
        gap = float(d[ys[k], xs[k]])
        if gap > a.max_gap or gap <= 1.5:
            continue
        a_cell = (int(ys[k]), int(xs[k]))
        b_cell = (int(by[a_cell]), int(bx[a_cell]))
        path = [c for c in line_cells(a_cell, b_cell) if not land[c]]
        near_port = any(abs(c[0] - q[0]) <= a.port_keep and abs(c[1] - q[1]) <= a.port_keep for c in path for q in ports)
        what = "; ".join(holders[p][:4])
        centre = (int(xs.mean()), int(ys.mean()))
        if not path or near_port:
            print("skip piece %d size %d centre %s gap %.1f (%s): %s" % (p, sizes[p], centre, gap, what,
                  "port in the channel" if near_port else "no water cells on the line"))
            continue
        for c in path:
            nb = cells[iy[c], ix[c]]
            new[c] = (nb & 0x3F) | (cells[c] & 0xC0)
        joined += 1
        print("join piece %d size %d centre %s gap %.1f: %d cells %s (%s)" % (p, sizes[p], centre, gap, len(path), path, what))

    print("joined %d pieces" % joined)
    if a.dry_run or not joined:
        return
    if src == a.thr:
        shutil.copy2(a.thr, bak)
    with open(a.thr, "wb") as fh:
        fh.write(struct.pack("<hh", mx, my))
        fh.write(new.T.copy().tobytes())
    print("wrote %s, backup %s" % (a.thr, bak))


if __name__ == "__main__":
    main()
