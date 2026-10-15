"""Draw where battalions found no supply store: python store_map.py <run name, e.g. save0-smap-s1> [out.png]
Reads the SMAPOBJ / SMAPMISS lines campsim prints at the end of a stocks=1 run, on the theater backdrop (2 px per km, north up).
Stores (depots, army bases): blue ROK, red DPRK, solid = usable, ring only = on the front or second line (not allowed as a store).
A battalion's first failed resupply is a ring (orange: no store reachable, magenta: only front-line stores reachable), joined by a
dashed line to the nearest store of its own side, with the straight-line distance."""
import math, os, sys
from PIL import Image, ImageDraw, ImageFont, ImageEnhance
HERE = os.path.dirname(os.path.abspath(__file__))
name = sys.argv[1]; out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, name + "-stores.png")
objs, miss = [], []
for l in open(os.path.join(HERE, "runs", name + ".log"), errors="replace"):
    if l.startswith("SMAPOBJ"): objs.append(list(map(int, l.split()[1:])))
    elif l.startswith("SMAPMISS"): miss.append(list(map(int, l.split()[1:])))
stores = [o for o in objs if o[1] in (3, 10)]
TEAM = {2: "ROK", 6: "DPRK"}
korea = [s for s in stores if s[2] == 2] + [(0, 0, 0, m[0], m[1]) for m in miss]
x0 = min(k[3] for k in korea) - 40; x1 = max(k[3] for k in korea) + 40
y0 = min(k[4] for k in korea) - 30; y1 = max(k[4] for k in korea) + 90
K = 2.0                                    # output px per km
bd = Image.open(os.path.join(HERE, "backdrop.png")).convert("RGB")
crop = bd.crop((x0 * 2, (1024 - y1) * 2, x1 * 2, (1024 - y0) * 2))
crop = ImageEnhance.Brightness(crop).enhance(0.55)
PX = 6                                     # 3 px per km
im = crop.resize(((x1 - x0) * PX // 2 * 2 // 2 * 1, (y1 - y0) * PX // 2 * 1)) if False else crop.resize(((x1 - x0) * PX // 2, (y1 - y0) * PX // 2))
S = PX / 2.0                                # px per km in the output
W, H = im.size
P = lambda x, y: ((x - x0) * S, (y1 - y) * S)
d = ImageDraw.Draw(im, "RGBA")
def font(n):
    for f in ("arial.ttf", "segoeui.ttf", "DejaVuSans.ttf"):
        try: return ImageFont.truetype(f, n)
        except Exception: pass
    return ImageFont.load_default()
F, FB, FS = font(15), font(20), font(13)
COL = {2: (80, 150, 255), 6: (255, 85, 75)}
# front: objectives flagged as front line
for id_, t, team, x, y, fr, sec in objs:
    if fr and x0 <= x <= x1 and y0 <= y <= y1:
        px, py = P(x, y); d.ellipse((px - 2.5, py - 2.5, px + 2.5, py + 2.5), fill=(255, 255, 255, 150))
# failed battalions to nearest same-side store
for x, y, team, why, hr in miss:
    own = [s for s in stores if s[2] == team]
    if not own: continue
    s = min(own, key=lambda s: (s[3] - x) ** 2 + (s[4] - y) ** 2)
    a, b = P(x, y), P(s[3], s[4]); km = math.hypot(s[3] - x, s[4] - y)
    n = max(1, int(math.hypot(b[0] - a[0], b[1] - a[1]) / 9))
    c = COL.get(team, (200, 200, 200)) + (170,)
    for i in range(0, n, 2):
        d.line((a[0] + (b[0] - a[0]) * i / n, a[1] + (b[1] - a[1]) * i / n, a[0] + (b[0] - a[0]) * (i + 1) / n, a[1] + (b[1] - a[1]) * (i + 1) / n), fill=c, width=2)
for x, y, team, why, hr in miss:
    px, py = P(x, y); c = (255, 170, 0, 255) if why == 2 else (255, 0, 255, 255)
    d.ellipse((px - 7, py - 7, px + 7, py + 7), fill=(0, 0, 0, 120), outline=c, width=3)
for id_, t, team, x, y, fr, sec in sorted(stores, key=lambda s: s[1] == 10):
    if not (x0 <= x <= x1 and y0 <= y <= y1): continue
    px, py = P(x, y); c = COL.get(team, (200, 200, 200)); r = 11 if t == 10 else 7
    if fr or sec: d.rectangle((px - r, py - r, px + r, py + r), fill=(0, 0, 0, 90), outline=c + (255,), width=3)
    else: d.rectangle((px - r, py - r, px + r, py + r), fill=c + (255,), outline=(255, 255, 255, 255), width=2)
# legend + counts
cnt = {t: [sum(1 for s in stores if s[2] == t), sum(1 for s in stores if s[2] == t and not (s[5] or s[6])), sum(1 for m in miss if m[2] == t)] for t in TEAM}
box = (14, 14, 640, 214); d.rounded_rectangle(box, 10, fill=(10, 14, 22, 225), outline=(120, 130, 150, 255))
d.text((30, 24), "Supply stores and units that cannot reach one", font=FB, fill=(255, 255, 255, 255))
d.text((30, 52), "Korea Rolling Fire, first 2 days, stores on (SupplyStocks)", font=F, fill=(190, 200, 215, 255))
yy = 82
for t in TEAM:
    d.rectangle((30, yy + 2, 44, yy + 16), fill=COL[t] + (255,), outline=(255, 255, 255, 255))
    d.text((54, yy), "%s: %d stores (%d usable), %d battalions with no store" % (TEAM[t], cnt[t][0], cnt[t][1], cnt[t][2]), font=F, fill=(235, 235, 235, 255)); yy += 24
d.rectangle((30, yy + 2, 44, yy + 16), outline=(200, 200, 200, 255), width=3); d.text((54, yy), "ring only = front / second line, not allowed", font=F, fill=(235, 235, 235, 255)); yy += 24
d.ellipse((30, yy + 1, 46, yy + 17), outline=(255, 170, 0, 255), width=3); d.text((54, yy), "orange ring = no store reachable; dashed line to nearest own store", font=F, fill=(235, 235, 235, 255)); yy += 24
d.text((30, yy), "big square = depot, small = army base; white dots = front-line objectives", font=FS, fill=(170, 180, 195, 255))
d.text((W // 2 - 20, 40), "", font=F)
im.convert("RGB").save(out); print(out, im.size)
