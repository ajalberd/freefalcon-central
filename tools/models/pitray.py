#!/usr/bin/env python3
"""Find where a ray from the pilot's eye meets the 3D cockpit model -- offline, no game.

Written to place the MRK BCN lamp: the pit has no geometry for it, and aiming the VR cursor at a
blank bit of panel draws a doubled cursor (the free cursor sits at a guessed depth), so the point
you line up differs between the eyes. The game's SimLogPitPoint (TEMPORARY) logs the head-centre
aim ray in MODEL units; give this tool one or more of those lines and it intersects each with the
pit's static triangles. Two rays aimed at the same spot, one lined up with each eye, err by about
half the eye spacing in opposite directions, so their average is the spot.

Only surfaces outside any DOF are tested: the panel itself is static, and the DOF subtrees
(switches, needles) carry transforms this tool does not apply.

Usage:
    python pitray.py <KoreaObj-basename> --lod 4105 --ray "ex ey ez dx dy dz" [--ray ...]
    python pitray.py <basename> --lod 4105 --log C:/FreeFalcon6/FFDebug.log   (every PITPOINT eyeModel line)
"""

import argparse
import re
import struct

import numpy as np

import objsurvey as O


def static_triangles(blob, header):
    """(N,3,3) triangle corners and a parallel texture-index array, DOF subtrees excluded."""
    tris = []
    for node in O.walk_nodes(blob, header):
        if node.type != 1 or node.depth != 0 or node.body is None:
            continue
        count = node.body[1]
        prim = node.body[3]
        at = node.offset + O.SURFACE_NODE_SIZE
        idx = [struct.unpack_from("<H", blob, at + 2 * k)[0] for k in range(count)]
        if prim == 4:  # triangle list
            faces = [idx[k:k + 3] for k in range(0, len(idx) - 2, 3)]
        elif prim == 5:  # strip
            faces = [idx[k:k + 3] if k % 2 == 0 else [idx[k + 1], idx[k], idx[k + 2]]
                     for k in range(len(idx) - 2)]
        elif prim == 6:  # fan
            faces = [[idx[0], idx[k], idx[k + 1]] for k in range(1, len(idx) - 1)]
        else:
            continue
        for f in faces:
            if len(set(f)) < 3:
                continue
            corners = []
            for i in f:
                v = O.VERTEX.unpack_from(blob, header["pVPool"] + i * O.VERTEX_STRIDE)
                corners.append(v[:3])
            tris.append(corners)
    return np.array(tris, dtype=np.float64)


def cast(tris, eye, d):
    """Nearest hit of eye + t*d with the triangles (Moller-Trumbore, both faces)."""
    d = d / np.linalg.norm(d)
    v0, v1, v2 = tris[:, 0], tris[:, 1], tris[:, 2]
    e1, e2 = v1 - v0, v2 - v0
    p = np.cross(d, e2)
    det = np.einsum("ij,ij->i", e1, p)
    ok = np.abs(det) > 1e-12
    inv = np.where(ok, 1.0 / np.where(ok, det, 1.0), 0.0)
    s = eye - v0
    u = np.einsum("ij,ij->i", s, p) * inv
    q = np.cross(s, e1)
    v = (q @ d) * inv
    t = np.einsum("ij,ij->i", e2, q) * inv
    hit = ok & (u >= 0) & (v >= 0) & (u + v <= 1) & (t > 1e-4)
    if not hit.any():
        return None
    k = np.where(hit, t, np.inf).argmin()
    n = np.cross(e1[k], e2[k])
    n /= np.linalg.norm(n)
    if n @ d > 0:
        n = -n  # face the viewer
    return eye + t[k] * d, n, t[k]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("basename")
    ap.add_argument("--lod", type=int, required=True)
    ap.add_argument("--ray", action="append", default=[], help='"ex ey ez dx dy dz" in model units')
    ap.add_argument("--log", help="read every 'PITPOINT eyeModel' line from this FFDebug.log")
    args = ap.parse_args(argv)

    rays = [[float(x) for x in r.split()] for r in args.ray]
    if args.log:
        pat = re.compile(r"PITPOINT eyeModel (\S+) (\S+) (\S+) dirModel (\S+) (\S+) (\S+)")
        for line in open(args.log, encoding="latin-1"):
            m = pat.search(line)
            if m:
                rays.append([float(g) for g in m.groups()])
    if not rays:
        raise SystemExit("no rays")

    lods, _, _ = O.read_dxh(args.basename)
    lod = lods[args.lod]
    blob = O.read_model(args.basename, lod)
    header = O.read_model_header(args.basename, lod)
    tris = static_triangles(blob, header)
    print(f"LOD {args.lod}: {len(tris)} static triangles")

    hits = []
    for r in rays:
        res = cast(tris, np.array(r[:3]), np.array(r[3:]))
        if res is None:
            print(f"ray {r}: no hit")
            continue
        p, n, t = res
        hits.append((p, n))
        print(f"ray {r}\n   hit {p.round(4).tolist()}  normal {n.round(3).tolist()}  dist {t:.3f}")
    if len(hits) > 1:
        p = np.mean([h[0] for h in hits], axis=0)
        n = np.mean([h[1] for h in hits], axis=0)
        n /= np.linalg.norm(n)
        print(f"average  {p.round(4).tolist()}  normal {n.round(3).tolist()}")


if __name__ == "__main__":
    main()
