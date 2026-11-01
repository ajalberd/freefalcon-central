"""Turn the speckle that paint_sea_from_3d.py left inland back into land, in a theatre's cover grid.

    python fill_inland_water.py --tdf Theaters\\Taiwan\\taiwan.tdf --thr C:\\FreeFalcon6\\Theaters\\Taiwan\\campaign\\Taiwan.thr \
                                [--max-cells 400] [--dry-run]

paint_sea_from_3d.py calls a cell water when more than half its 3D tile is sea-coloured. Over land that is
often wrong: grey city roofs, shaded fields and river mud read blue-ish too. Taiwan after the repaint had
59,299 inland water cells in 15,298 pieces (9,397 of them a single cell) where Korea has 9,329 in 66; the
capital's basin is a lattice of water, and 112 villages, towns and cities stand on water cells. The engine
cannot path through or to a water cell, so a 2 km move across Taipei failed 150 times per battalion.

Here "sea" is water joined (8-connected) to the largest water body, i.e. the ocean; anything else is inland
water. Inland water pieces of at most --max-cells cells become land, taking the cover/relief byte of the
nearest land cell, except a piece that holds a bridge or port objective (that road + water cell is the
bridge). Then every other objective's own cell is made land the same way. The road bit is kept as it was.

Writes <thr>.bak-pre-fill first and refuses to run twice (--force repaints from that backup).
"""
import argparse
import collections
import math
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


def stats(cells, objs, units, log, tag):
    land = (cells & 0x0F) != 0
    lab, n = ndi.label(land, structure=S8)
    wat = ~land
    wl, _ = ndi.label(wat, structure=S8)
    sea = int(np.argmax(np.bincount(wl.ravel())[1:])) + 1
    inland = wat & (wl != sea)
    il, inn = ndi.label(inland, structure=S8)
    log("%s: land %d cells in %d pieces, inland water %d cells in %d pieces" % (tag, land.sum(), n, inland.sum(), inn))
    dry = [o for o in objs if o["typeName"] not in ("Bridge", "Port") and not land[o["y"], o["x"]]]
    log("%s: %d objectives other than bridges/ports on water; %d ground battalions on water"
        % (tag, len(dry), sum(1 for u in units if u["kind"] == "battalion" and not land[u["y"], u["x"]])))
    by = {tuple(o["id"]): o for o in objs}
    cross = 0
    for o in objs:
        for l in o["links"]:
            t = by.get(tuple(l["id"]))
            if t and land[o["y"], o["x"]] and land[t["y"], t["x"]] and lab[o["y"], o["x"]] != lab[t["y"], t["x"]]:
                cross += 1
    log("%s: %d links join land cells in different land pieces" % (tag, cross))


def fill(cells, objs, max_cells):
    land = (cells & 0x0F) != 0
    wat = ~land
    wl, _ = ndi.label(wat, structure=S8)
    sizes = np.bincount(wl.ravel())
    sea = int(np.argmax(sizes[1:])) + 1
    keep = {sea}
    for o in objs:
        if o["typeName"] in ("Bridge", "Port"):
            keep.add(int(wl[o["y"], o["x"]]))
    keep.discard(0)
    small = np.array([i != 0 and i not in keep and sizes[i] <= max_cells for i in range(len(sizes))])
    todo = small[wl] & wat
    n_piece = int(todo.sum())
    for o in objs:
        if o["typeName"] not in ("Bridge", "Port") and wat[o["y"], o["x"]]:
            todo[o["y"], o["x"]] = True
    # cover/relief of the nearest land cell that is not itself being filled
    src = land
    _, (iy, ix) = ndi.distance_transform_edt(~src, return_indices=True)
    new = cells.copy()
    nb = cells[iy, ix]
    new[todo] = (nb[todo] & 0x3F) | (cells[todo] & 0xC0)
    return new, n_piece, int(todo.sum()) - n_piece


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gamedir", default=r"C:\FreeFalcon6")
    ap.add_argument("--tdf", default=r"Theaters\Taiwan\taiwan.tdf")
    ap.add_argument("--thr", required=True)
    ap.add_argument("--max-cells", type=int, default=400)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--force", action="store_true")
    a = ap.parse_args()
    log = lambda s: print(s, flush=True)

    ws = TheaterWorkspace(a.gamedir, a.tdf)
    objs, units = [], []
    seen = set()
    import glob
    for path in sorted(glob.glob(os.path.join(ws.campaign_dir, "save*.cam"))):
        n = os.path.basename(path)
        objs_n = ws.objectives(n).objectives
        if objs_n and not objs:
            objs = objs_n                      # one save's objectives stand for the theatre (positions are shared)
        for o in objs_n:                       # ... but keep any objective at a cell no other save has
            if (o["x"], o["y"]) not in seen:
                seen.add((o["x"], o["y"]))
                if o not in objs:
                    objs.append(o)
        units += ws.units(n).units
    bak = a.thr + ".bak-pre-fill"
    src = a.thr
    if os.path.exists(bak):
        if not a.force and not a.dry_run:
            sys.exit("refusing: %s exists; --force repaints from it" % bak)
        src = bak
    cells, mx, my = cc.load_thr(src)
    stats(cells, objs, units, log, "before")
    new, n_piece, n_obj = fill(cells, objs, a.max_cells)
    log("filled %d cells of small inland water and %d objective cells" % (n_piece, n_obj))
    stats(new, objs, units, log, "after ")
    if a.dry_run:
        return
    if src == a.thr:
        shutil.copy2(a.thr, bak)
    with open(a.thr, "wb") as fh:
        fh.write(struct.pack("<hh", mx, my))
        fh.write(new.T.copy().tobytes())
    log("wrote %s, backup %s" % (a.thr, bak))


if __name__ == "__main__":
    main()
