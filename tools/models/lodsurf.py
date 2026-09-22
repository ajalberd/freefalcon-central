#!/usr/bin/env python3
"""Per-surface bounds of one LOD, so the geometry at a model's extremes can be named.

A bounding box says a model is 75 ft long; this says which surfaces make it so
(a probe? a plume? a welded-on weapon?), and where the vertices actually cluster.
Used to check whether a viewer's framing is chasing real visual mass or a stray
surface that only the bounds know about.

Surface bounds honour the degenerate-triangle rule from objsurvey.surface_bounds:
a triangle list's trailing repeated index can name a vertex anywhere in the
model, and including it turns a 5 mm lever into a 2.6 unit surface.

Usage:
    python lodsurf.py <KoreaObj-basename> <LOD>
"""

import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import objsurvey as o


def survey(basename, lid):
    lods, parents, meta = o.read_dxh(basename)
    lod = lods[lid]
    blob = o.read_model(basename, lod)
    header = o.read_model_header(basename, lod)
    rows = []
    for node in o.walk_nodes(blob, header):
        if node.type != 1 or not node.body:
            continue
        b = o.surface_bounds(blob, node, header)
        if b:
            rows.append((b[0], b[1], b[2], node.node_id, node.body[6]))
    return lod, rows


def print_surface(tag, rows):
    for (lo, hi, used, node_id, tex) in rows:
        print(f"  {tag} node {node_id:6d} verts {used:5d} tex {tex:5d} "
              f"x [{lo[0]:9.3f},{hi[0]:9.3f}] y [{lo[1]:8.3f},{hi[1]:8.3f}]"
              f" z [{lo[2]:8.3f},{hi[2]:8.3f}]")


def main():
    basename, lid = sys.argv[1], int(sys.argv[2])
    lod, rows = survey(basename, lid)
    print(f"LOD {lid} {lod.name!r}: {len(rows)} surfaces")
    print("lowest min-x:")
    print_surface("", sorted(rows, key=lambda r: r[0][0])[:8])
    print("highest max-x:")
    print_surface("", sorted(rows, key=lambda r: -r[1][0])[:8])

    print("vertex-count histogram by surface-centre x:")
    buckets = {}
    for (lo, hi, used, node_id, tex) in rows:
        b = int((lo[0] + hi[0]) * 0.5 // 5) * 5
        buckets[b] = buckets.get(b, 0) + used
    for b in sorted(buckets):
        print(f"  x {b:+4d}..{b + 5:+4d}: {'#' * min(60, buckets[b] // 10)}"
              f" ({buckets[b]})")


if __name__ == "__main__":
    main()
