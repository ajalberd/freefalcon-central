#!/usr/bin/env python3
"""Where a menu-viewer model lands on screen -- the viewer's projection, offline.

Reimplements the exact CPU projection the menu 3D viewers use (Render3D::
SetCamera + TransformPoint, fed by FindCameraDeltas in viewer.cpp) so a model's
framing can be checked without launching the game.  The camera is aimed at the
object's origin by construction, so anything this reports off-centre is the
model's own geometry; anything that lands at the TARGET centre instead of the
pane centre is a device-viewport problem, not a camera one (see the note on
ConfineGpuViewportToPane in src/ui/src/general/c3dview.cpp).

The FOV argument is the HORIZONTAL field of view in degrees (what
C_3dViewer::Init3d passes as ViewAngle); the vertical is derived from the pane's
pixel aspect, exactly as Render3D::SetFOV does.

Usage:
    python menuview.py            # the two stock menu viewports, from the log

The numbers in main() are the panes and defaults measured on a 1024x768 run:
    loadout/munition  client rect (120,10)-(920,285)  F-16CG  D = Radius*3
    tactical reference (355,37)-(1014,447)            F/A-18  D = Radius*4
"""

import math
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import objsurvey as o
import lodbounds

DTR = math.pi / 180.0


def camera_rot(heading_deg, pitch_deg):
    """PositandOrientSetData's rotation rows: X forward, Y right, Z down."""
    psi = heading_deg * DTR
    th = pitch_deg * DTR
    return [
        [math.cos(psi) * math.cos(th), -math.sin(psi),
         math.cos(psi) * math.sin(th)],
        [math.sin(psi) * math.cos(th), math.cos(psi),
         math.sin(psi) * math.sin(th)],
        [-math.sin(th), 0.0, math.cos(th)],
    ]


def projection(heading, pitch, dist, vp_w, vp_h, fov_h_deg=30.0):
    """Render3D's camera transform T and move vector for one viewer pose.

    heading/pitch are Info->Heading / Info->Pitch in degrees; the camera itself
    gets -Info->Pitch (PositionCamera negates it), which is folded in here.
    """
    half_h = fov_h_deg * 0.5 * DTR
    scale_x = vp_w * 0.5
    scale_y = vp_h * 0.5
    k_h = 1.0 / math.tan(half_h)
    k_v = 1.0 / math.tan(math.atan2(scale_y * math.tan(half_h), scale_x))

    rot = camera_rot(heading, -pitch)  # SetCamera receives -Info->Pitch
    cr = [[rot[j][i] for j in range(3)] for i in range(3)]  # cameraRot = rot^T

    T = [
        [cr[0][0], cr[0][1], cr[0][2]],
        [cr[1][0] * k_h, cr[1][1] * k_h, cr[1][2] * k_h],
        [cr[2][0] * k_v, cr[2][1] * k_v, cr[2][2] * k_v],
    ]

    # FindCameraDeltas: the camera sits on the opposite side of the aim point.
    p = pitch * DTR
    h = heading * DTR
    cam = (-dist * math.cos(h) * math.cos(p),
           -dist * math.sin(h) * math.cos(p),
           -dist * math.sin(p))

    move = [-sum(cam[i] * T[r][i] for i in range(3)) for r in range(3)]
    return T, move


def project(T, move, pt):
    """TransformPoint: NDC x/y (right, down) and camera-space depth."""
    sz = sum(T[0][i] * pt[i] for i in range(3)) + move[0]
    sx = sum(T[1][i] * pt[i] for i in range(3)) + move[1]
    sy = sum(T[2][i] * pt[i] for i in range(3)) + move[2]
    return (sx / sz, sy / sz, sz)


def report(label, lo, hi, heading, pitch, dist, vp_w, vp_h):
    """Project a bounding box's corners; |NDC| > 1 means it is off the pane."""
    T, move = projection(heading, pitch, dist, vp_w, vp_h)
    xs, ys = [], []
    for x in (lo[0], hi[0]):
        for y in (lo[1], hi[1]):
            for z in (lo[2], hi[2]):
                nx, ny, sz = project(T, move, (x, y, z))
                xs.append(nx)
                ys.append(ny)
    cropped = []
    if min(xs) < -1.0:
        cropped.append("left")
    if max(xs) > 1.0:
        cropped.append("right")
    if min(ys) < -1.0:
        cropped.append("top")
    if max(ys) > 1.0:
        cropped.append("bottom")
    print(f"{label}: dist {dist:.1f} H {heading} P {pitch} pane {vp_w}x{vp_h}")
    print(f"  NDC x [{min(xs):+.3f}, {max(xs):+.3f}]  "
          f"y [{min(ys):+.3f}, {max(ys):+.3f}]  "
          f"cropped: {', '.join(cropped) if cropped else 'none'}")


def main():
    basename = r"C:\FreeFalcon6\terrdata\objects\KoreaObj"
    lods, parents, meta = o.read_dxh(basename)

    # F-16CG (loadout) and CF-18 (tacref), the first model each screen shows.
    cases = {}
    for lid, label in ((2718, "loadout"), (2801, "tacref")):
        r = lodbounds.lod_bounds(basename, lods[lid])
        if not r:
            continue
        cases[label] = r
        lo, hi, used = r
        print(f"{label}: LOD {lid} {lods[lid].name!r}  "
              f"x [{lo[0]:.2f},{hi[0]:.2f}] y [{lo[1]:.2f},{hi[1]:.2f}] "
              f"z [{lo[2]:.2f},{hi[2]:.2f}]")
    print()

    if "loadout" in cases:
        report("loadout (stock D = Radius*3)", cases["loadout"][0],
               cases["loadout"][1], heading=180.0, pitch=-10.0,
               dist=29.25 * 3, vp_w=800, vp_h=275)

    if "tacref" in cases:
        report("tacref  (stock D = Radius*4)", cases["tacref"][0],
               cases["tacref"][1], heading=-200.0, pitch=10.0,
               dist=27.875 * 4, vp_w=659, vp_h=410)


if __name__ == "__main__":
    main()
