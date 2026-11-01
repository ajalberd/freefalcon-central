"""Rebuild the relief class (bits 4-5) of a theatre's cover grid from its own 3D terrain heights.

    python relief_from_3d.py --theater C:\\FreeFalcon6\\Theaters\\Taiwan\\terrdata \
                             --thr C:\\FreeFalcon6\\Theaters\\Taiwan\\campaign\\Taiwan.thr \
                             [--like C:\\FreeFalcon6\\campaign\\SAVE\\KOREA.THR] [--dry-run]

The engine multiplies ground cost by ReliefCost = 1.0 / 1.41 / 1.73 / 2.0 for relief class 0..3 (path.cpp), so
a column prefers low, flat ground. Korea's classes follow the 3D terrain: mean elevation by class is
671 / 2,439 / 2,398 / 3,975 ft, correlation with elevation +0.64. Taiwan's are *inverted*: 4,543 / 2,131 /
1,408 / 261 ft (correlation -0.35) -- the western coastal plain is class 3 (slowest) and the Central Range
class 0 -- so the cheap way across the island is over the mountains.

Cells are ranked by their 3D mean elevation among the theatre's land cells and cut into four classes in the
same proportions as --like (default Korea's grid, 32 / 44 / 12 / 11 %). Only bits 4-5 change; cover type,
road and water are left alone. Writes <thr>.bak-pre-relief first and refuses to run twice (--force repaints
from that backup).
"""
import argparse
import os
import shutil
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cover_compare as cc  # noqa: E402
import tilesurvey as ts  # noqa: E402


def mean_elevation(root):
    """Mean post height (ft) per campaign km cell, [y][x]."""
    th = ts.Theater(root, 0)
    psz = th.map.post_size
    zoff = 4 if th.map.large_terrain else 2
    bw, bh = th.bw, th.bh
    zm = np.zeros((bh * 4, bw * 4), np.float32)
    for br in range(bh):
        for bc in range(bw):
            o = int(th.offsets[br * bw + bc])
            buf = np.array(th.posts[o:o + psz * 256]).reshape(256, psz)
            z = (buf[:, zoff].astype(np.int16) | (buf[:, zoff + 1].astype(np.int16) << 8)).astype(np.int16)
            zz = z.reshape(4, 4, 4, 4).transpose(0, 2, 1, 3).reshape(4, 4, 16).astype(np.float32)
            zm[br * 4:br * 4 + 4, bc * 4:bc * 4 + 4] = zz.mean(2)
    return zm


def report(tag, cells, zm):
    land = (cells & 0x0F) != 0
    rel = (cells >> 4) & 3
    by = {k: round(float(zm[land & (rel == k)].mean())) for k in range(4) if (land & (rel == k)).any()}
    share = {k: round(100.0 * float((land & (rel == k)).sum()) / land.sum(), 1) for k in range(4)}
    print("%s: mean elevation ft by relief class %s, share %% %s, corr %.2f"
          % (tag, by, share, np.corrcoef(rel[land], zm[land])[0, 1]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--theater", required=True, help="terrdata directory of the theatre")
    ap.add_argument("--thr", required=True)
    ap.add_argument("--like", default=r"C:\FreeFalcon6\campaign\SAVE\KOREA.THR",
                    help="a grid whose relief shares to copy")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--force", action="store_true")
    a = ap.parse_args()

    bak = a.thr + ".bak-pre-relief"
    src = a.thr
    if os.path.exists(bak):
        if not a.force and not a.dry_run:
            sys.exit("refusing: %s exists; --force repaints from it" % bak)
        src = bak
    cells, mx, my = cc.load_thr(src)
    zm = mean_elevation(a.theater)
    land = (cells & 0x0F) != 0
    ref, _, _ = cc.load_thr(a.like)
    rl = (ref & 0x0F) != 0
    share = np.array([float(((ref >> 4) & 3)[rl].tolist().count(k)) for k in range(4)])
    share /= share.sum()
    print("target shares from %s: %s" % (os.path.basename(a.like), np.round(100 * share, 1).tolist()))

    report("before", cells, zm)
    order = np.argsort(zm[land], kind="stable")
    cls = np.empty(order.size, np.uint8)
    cut = np.round(np.cumsum(share) * order.size).astype(int)
    lo = 0
    for k in range(4):
        cls[order[lo:cut[k]]] = k
        lo = cut[k]
    new = cells.copy()
    new[land] = (cells[land] & 0xCF) | (cls << 4)
    report("after ", new, zm)
    if a.dry_run:
        return
    if src == a.thr:
        shutil.copy2(a.thr, bak)
    with open(a.thr, "wb") as fh:
        fh.write(struct.pack("<hh", mx, my))
        fh.write(new.T.copy().tobytes())
    print("wrote %s, backup %s" % (a.thr, bak))


if __name__ == "__main__":
    main()
