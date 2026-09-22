#!/usr/bin/env python3
"""Project a menu model's REAL vertices with the viewer's camera.

Bounding-box corners overstate cropping -- a corner can sit where no geometry is,
and one stray surface stretches the box.  This walks every surface of the LOD and
projects every vertex that is rasterised, so "cropped" means real geometry off the
pane, and the 1%..99% band says where the visual mass actually sits.

Shares its projection with menuview.py (which projects bboxes); see that file for
the camera conventions, and ConfineGpuViewportToPane in c3dview.cpp for what the
device viewport does with these numbers.

Usage:
    python menuverts.py <KoreaObj-basename> <LOD> <heading> <pitch> <dist> <vp_w> <vp_h>
    python menuverts.py            # both stock menu cases, and a distance sweep
"""

import struct
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import objsurvey as o
import menuview


def lod_vertices(basename, lod):
    """Every vertex index used by a surface of one LOD, in model space."""
    blob = o.read_model(basename, lod)
    header = o.read_model_header(basename, lod)
    pts = []
    for node in o.walk_nodes(blob, header):
        if node.type != 1 or not node.body:
            continue
        count = node.body[1]
        idx_at = node.offset + o.SURFACE_NODE_SIZE
        pool = header["pVPool"]
        for k in range(count):
            at = idx_at + k * 2
            if at + 2 > len(blob):
                break
            index = struct.unpack_from("<H", blob, at)[0]
            vat = pool + index * o.VERTEX_STRIDE
            if vat + o.VERTEX_STRIDE > len(blob):
                continue
            v = o.VERTEX.unpack_from(blob, vat)
            pts.append((v[0], v[1], v[2]))
    return pts


def report(label, pts, heading, pitch, dist, vp_w, vp_h):
    T, move = menuview.projection(heading, pitch, dist, vp_w, vp_h)
    xs, ys = [], []
    inside = 0
    for p in pts:
        nx, ny, sz = menuview.project(T, move, p)
        xs.append(nx)
        ys.append(ny)
        if -1.0 <= nx <= 1.0 and -1.0 <= ny <= 1.0:
            inside += 1
    xs.sort()
    ys.sort()
    n = len(xs)
    lo = n // 100
    hi = n - 1 - n // 100

    print(f"{label}: {n} verts  dist {dist:.1f}  H {heading}  P {pitch}"
          f"  pane {vp_w}x{vp_h}")
    print(f"  NDC x [{xs[0]:+.3f}, {xs[-1]:+.3f}]  1%..99% "
          f"[{xs[lo]:+.3f}, {xs[hi]:+.3f}]")
    print(f"  NDC y [{ys[0]:+.3f}, {ys[-1]:+.3f}]  1%..99% "
          f"[{ys[lo]:+.3f}, {ys[hi]:+.3f}]")
    print(f"  vertices inside pane: {100.0 * inside / n:.1f}%")
    print()


def main():
    basename = r"C:\FreeFalcon6\terrdata\objects\KoreaObj"
    lods, parents, meta = o.read_dxh(basename)

    if len(sys.argv) >= 8:
        lid = int(sys.argv[2])
        heading, pitch, dist, vp_w, vp_h = (float(a) for a in sys.argv[3:8])
        report(lods[lid].name, lod_vertices(basename, lods[lid]), heading,
               pitch, dist, int(vp_w), int(vp_h))
        return

    # F-16CG (loadout) and CF-18 (tacref) at their stock defaults, then a sweep.
    f16 = lod_vertices(basename, lods[2718])
    f18 = lod_vertices(basename, lods[2801])

    report("loadout stock (D = Radius*3)", f16, 180.0, -10.0, 29.25 * 3,
           800, 275)
    report("tacref  stock (D = Radius*4)", f18, -200.0, 10.0, 27.875 * 4,
           659, 410)

    for d in (60, 80, 100, 120, 150, 180, 220):
        report(f"loadout D={d}", f16, 180.0, -10.0, d, 800, 275)
    for d in (80, 100, 120, 150, 180, 220):
        report(f"tacref  D={d}", f18, -200.0, 10.0, d, 659, 410)


if __name__ == "__main__":
    main()
