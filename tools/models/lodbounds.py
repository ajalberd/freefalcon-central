#!/usr/bin/env python3
"""Per-LOD model bounds, straight from a theater's .DXL.

Answers "where is the model's visual mass relative to its own origin" -- which is
what a menu 3D viewer aims its camera at, and what a parent record's radius/bbox
only claim to describe.  Needed because those records can be stale: parent 5's
F-16 bbox says y [-2.2, 2.2] (a 4 ft wingspan) and its radius is 38.3 while the
model actually spans 74.9 ft.  The LOD's own vertices do not lie.

Companion to lodsurf.py (per-surface detail) and objsurvey.py (the record-level
view); see menuview.py for what a viewer does with these numbers.

Usage:
    python lodbounds.py <KoreaObj-basename> <LOD> [LOD ...]
"""

import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import objsurvey as o


def lod_bounds(basename, lod):
    """Union bounds over every rasterised surface of one LOD: (lo, hi, surfaces)."""
    blob = o.read_model(basename, lod)
    header = o.read_model_header(basename, lod)
    if header is None:
        return None
    lo = [float("inf")] * 3
    hi = [float("-inf")] * 3
    used = 0
    for node in o.walk_nodes(blob, header):
        if node.type != 1 or not node.body:
            continue
        b = o.surface_bounds(blob, node, header)
        if not b:
            continue
        used += 1
        for axis in range(3):
            lo[axis] = min(lo[axis], b[0][axis])
            hi[axis] = max(hi[axis], b[1][axis])
    return lo, hi, used


def main():
    basename = sys.argv[1]
    lods, parents, meta = o.read_dxh(basename)
    for lid in (int(x) for x in sys.argv[2:]):
        lod = lods[lid]
        r = lod_bounds(basename, lod)
        if not r:
            print(f"LOD {lid} {lod.name!r}: bounds unavailable")
            continue
        lo, hi, used = r
        cx = [(lo[i] + hi[i]) * 0.5 for i in range(3)]
        print(f"LOD {lid:5d} {lod.name!r}")
        print(f"  surfaces {used}")
        print(f"  x [{lo[0]:10.4f}, {hi[0]:10.4f}]  centre {cx[0]:10.4f}")
        print(f"  y [{lo[1]:10.4f}, {hi[1]:10.4f}]  centre {cx[1]:10.4f}")
        print(f"  z [{lo[2]:10.4f}, {hi[2]:10.4f}]  centre {cx[2]:10.4f}")


if __name__ == "__main__":
    main()
