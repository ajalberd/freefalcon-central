"""Map of capture orders at one moment of a campsim run: where each ROK battalion with a CAPTURE order is,
and where it has been told to go. Plain SVG, no dependencies.

    python capture_map.py runs/save0-ss1-s101.jsonl 48 [out.svg]

Arrows run from the battalion (black square) to its target, coloured by distance (green near, red far);
the ground is the objective map (blue = ROK-held, red = DPRK-held); grey squares are ROK battalions on
other orders. North is up. Read-only.
"""
import json
import math
import sys

path, hour = sys.argv[1], int(sys.argv[2])
out = sys.argv[3] if len(sys.argv) > 3 else path.replace(".jsonl", "-capmap-h%d.svg" % hour)

owner, oloc = {}, {}
frame = None
for line in open(path):
    r = json.loads(line)
    if "meta" in r:
        for o in r["objs"]:
            owner[o[0]], oloc[o[0]] = o[4], (o[1], o[2])
        continue
    if "t" not in r:
        continue
    for o in r.get("o", []):
        owner[o[0]] = o[1]
    if r["t"] >= hour * 60 and "u" in r:
        frame = r
        break

bns = [u for u in frame["u"] if u[1] == 0 and u[2] == 2]
caps = [u for u in bns if u[9] == 1 and u[12]]
dist = lambda u: math.hypot(u[13] - u[3], u[14] - u[4])
ds = sorted(dist(u) for u in caps)
med = ds[len(ds) // 2] if ds else 0

# view: the area holding the battalions and their targets, plus a margin
xs = [u[3] for u in bns] + [u[13] for u in caps]
ys = [u[4] for u in bns] + [u[14] for u in caps]
x0, x1, y0, y1 = min(xs) - 25, max(xs) + 25, min(ys) - 25, max(ys) + 25
S = 2.2  # px per km
W, H = (x1 - x0) * S, (y1 - y0) * S
px = lambda x: (x - x0) * S
py = lambda y: (y1 - y) * S  # north up


def colour(d):
    t = min(d, 150) / 150.0  # green -> yellow -> red
    r = int(255 * min(1, 2 * t))
    g = int(200 * min(1, 2 * (1 - t)))
    return "rgb(%d,%d,40)" % (r, g)


svg = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d" font-family="Segoe UI, sans-serif">'
       % (W, H + 70, W, H + 70),
       '<rect width="100%" height="100%" fill="#fbfbf8"/>',
       '<defs>' + "".join('<marker id="a%d" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="5" markerHeight="5" '
                          'orient="auto"><path d="M0,0 L10,5 L0,10 z" fill="%s"/></marker>' % (k, colour(k * 15))
                          for k in range(11)) + '</defs>']
for i, t in owner.items():
    x, y = oloc[i]
    if x0 <= x <= x1 and y0 <= y <= y1 and t in (2, 6):
        svg.append('<circle cx="%.1f" cy="%.1f" r="1.6" fill="%s"/>' % (px(x), py(y), "#9ec5ff" if t == 2 else "#ffb3b3"))
for u in bns:
    if not (u[9] == 1 and u[12]):
        svg.append('<rect x="%.1f" y="%.1f" width="4" height="4" fill="#9a9a9a"/>' % (px(u[3]) - 2, py(u[4]) - 2))
for u in sorted(caps, key=dist):
    d = dist(u)
    k = min(10, int(d / 15))
    svg.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="%s" stroke-width="1.1" opacity="0.75" '
               'marker-end="url(#a%d)"><title>bn %d -> objective %d, %.0f km</title></line>'
               % (px(u[3]), py(u[4]), px(u[13]), py(u[14]), colour(d), k, u[0], u[12], d))
for u in caps:
    svg.append('<rect x="%.1f" y="%.1f" width="4" height="4" fill="#111"/>' % (px(u[3]) - 2, py(u[4]) - 2))
svg.append('<text x="10" y="%d" font-size="15" font-weight="600">Capture orders at hour %d - %d ROK battalions sent '
           'to capture, median %.0f km to target</text>' % (H + 25, hour, len(caps), med))
svg.append('<text x="10" y="%d" font-size="12" fill="#444">black = battalion with a capture order, arrow = its target '
           '(green near, red 150+ km); grey = other ROK battalions; blue/red dots = ROK/DPRK-held objectives</text>'
           % (H + 48))
svg.append('</svg>')
open(out, "w").write("\n".join(svg))
print("wrote %s: %d capture orders, median %.0f km" % (out, len(caps), med))
