"""Repaint the sea in a theatre's campaign cover grid (<theatre>.thr) from its own 3D terrain.

    python paint_sea_from_3d.py --theater C:\\FreeFalcon6\\Theaters\\Taiwan\\terrdata \
                                --thr C:\\FreeFalcon6\\Theaters\\Taiwan\\campaign\\Taiwan.thr [--dry-run]

cover_compare.py showed the Taiwan grid paints 656,218 km2 of open sea as land (water agreement with the
3D world: 31%, and only a 2.3% improvement from any global shift, so it is a painting error, not an
offset). The AI paths on this grid, so units drove across the strait and no objective link graph could
mean anything.

The grid is one byte per km: cover type in the low nibble (0 = water), 0x40 = road (Korea's sea cells are
48 = 0x30, its roads 86 = 0x56). This sets cells whose 3D tile is at least --threshold sea to the theatre's
sea type and leaves everything else alone. Read the 3D tiles through cover_compare, so it uses the same
water test as the audit. Writes a .bak-pre-sea backup first and refuses to run twice on the same file.
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

SEA_BYTE = 0x30          # Korea's open-sea cover type: low nibble 0 = water, no road bit


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--theater", required=True, help="the theatre's terrdata directory (contains terrain/)")
    ap.add_argument("--thr", required=True)
    ap.add_argument("--threshold", type=float, default=0.5, help="3D sea fraction at which a cell becomes water")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    cells, mx, my = cc.load_thr(a.thr)          # [y][x]
    water, sea = cc.load_3d(a.theater)          # [y][x] fraction, post count
    print("grid %dx%d, 3D %dx%d" % (mx, my, water.shape[1], water.shape[0]))

    was_water = (cells & 0x0F) == 0
    is_sea = water >= a.threshold
    new = cells.copy()
    new[is_sea] = (new[is_sea] & 0xF0) | SEA_BYTE     # keep any road bit, set the water type
    changed = int((new != cells).sum())
    print("water cells before %d (%.2f%%), 3D sea cells %d (%.2f%%), repainted %d"
          % (int(was_water.sum()), 100.0 * was_water.mean(), int(is_sea.sum()), 100.0 * is_sea.mean(), changed))
    print("roads (0x40) before %d, after %d"
          % (int(((cells & 0x40) != 0).sum()), int(((new & 0x40) != 0).sum())))

    if a.dry_run or not changed:
        print("dry run / nothing to do" if a.dry_run else "nothing to do")
        return

    bak = a.thr + ".bak-pre-sea"
    if os.path.exists(bak):
        print("refusing: %s already exists (this file has been repainted once)" % bak)
        return

    shutil.copy2(a.thr, bak)
    raw = new.T.copy()                            # back to [x][y]
    with open(a.thr, "wb") as fh:
        fh.write(struct.pack("<hh", mx, my))
        fh.write(raw.tobytes())
    print("wrote %s (%d bytes), backup %s" % (a.thr, os.path.getsize(a.thr), bak))


if __name__ == "__main__":
    main()
