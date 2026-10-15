"""Draw the objective link network that supply (and ground units) travel along, from a campaign file at the start of the war.
    python supply_network.py [save0.cam] [out.png]
Every line is a link between two objectives (the engine's roads, rail and track network as the campaign sees it): red = both ends DPRK,
blue = both ROK, yellow = a link between the two sides.  Markers: factories, ports, depots, army bases (the supply makers and stores).
Thick white line: the route a delivery from one DPRK factory to the front would follow (fewest links)."""
import os, sys, math, collections
from PIL import Image, ImageDraw, ImageFont, ImageEnhance
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "campaign-editor"))
from ffcamp import workspace
cam_name = sys.argv[1] if len(sys.argv) > 1 else "save0.cam"
out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, "supply-network.png")
ws = workspace.Session(r"C:\FreeFalcon6").workspace("terrdata/theaterdefinition/korea.tdf")
cam = ws.units(cam_name); ws.objectives(cam_name); objs = cam.objectives
by = {tuple(o["id"]): o for o in objs}
nbr = collections.defaultdict(list)
for o in objs:
    for l in o["links"]:
        p = by.get(tuple(l["id"]))
        if p: nbr[tuple(o["id"])].append(tuple(p["id"]))
# the stage: Korea only
sel = [o for o in objs if 330 <= o["x"] <= 640 and 400 <= o["y"] <= 640]
x0, x1, y0, y1 = 330, 640, 400, 640
PX = 5
bd = Image.open(os.path.join(HERE, "backdrop.png")).convert("RGB")
crop = bd.crop((x0 * 2, (1024 - y1) * 2, x1 * 2, (1024 - y0) * 2)).resize(((x1 - x0) * PX, (y1 - y0) * PX))
im = ImageEnhance.Brightness(crop).enhance(0.5); d = ImageDraw.Draw(im, "RGBA")
P = lambda o: ((o["x"] - x0) * PX, (y1 - o["y"]) * PX)
def font(n):
    for f in ("arial.ttf", "segoeui.ttf", "DejaVuSans.ttf"):
        try: return ImageFont.truetype(f, n)
        except Exception: pass
    return ImageFont.load_default()
F, FB = font(16), font(22)
seen = set()
for o in objs:
    for q in nbr[tuple(o["id"])]:
        k = tuple(sorted((tuple(o["id"]), q)))
        if k in seen: continue
        seen.add(k); p = by[q]
        if not (x0 <= o["x"] <= x1 and y0 <= o["y"] <= y1) and not (x0 <= p["x"] <= x1 and y0 <= p["y"] <= y1): continue
        a, b = o["owner"], p["owner"]
        c = (255, 90, 80, 120) if a == b == 6 else (90, 150, 255, 120) if a == b == 2 else (255, 225, 60, 200) if {a, b} == {2, 6} else (170, 170, 170, 70)
        d.line((*P(o), *P(p)), fill=c, width=1)
MK = {11: ("factory", (255, 170, 0), 7), 19: ("port", (80, 220, 230), 7), 10: ("depot", (255, 255, 255), 8), 3: ("army base", (255, 255, 255), 5)}
# sample route: BFS over links from the DPRK factory nearest (x=380, y=560) to the DPRK objective nearest the front (south-most DPRK secondary)
fact = min((o for o in objs if o["objType"] == 11 and o["owner"] == 6), key=lambda o: (o["x"] - 380) ** 2 + (o["y"] - 580) ** 2)
fronts = [o for o in objs if o["owner"] == 6 and o["objType"] in (8, 6, 3, 10) and any(by[q]["owner"] == 2 for q in nbr[tuple(o["id"])])]
tgt = min(fronts, key=lambda o: (o["x"] - fact["x"]) ** 2 + (o["y"] - fact["y"]) ** 2) if fronts else None
route = []
if tgt:
    prev = {tuple(fact["id"]): None}; dq = collections.deque([tuple(fact["id"])])
    while dq:
        u = dq.popleft()
        if u == tuple(tgt["id"]): break
        for q in nbr[u]:
            if q not in prev and by[q]["owner"] == 6: prev[q] = u; dq.append(q)
    u = tuple(tgt["id"])
    while u in prev and u is not None: route.append(by[u]); u = prev[u]
    route.reverse()
for a, b in zip(route, route[1:]): d.line((*P(a), *P(b)), fill=(255, 255, 255, 255), width=4)
for o in sel:
    if o["objType"] in MK:
        name, col, r = MK[o["objType"]]; x, y = P(o)
        d.rectangle((x - r, y - r, x + r, y + r), fill=col + (255,), outline=(0, 0, 0, 255), width=2)
    elif o["objType"] == 8:
        x, y = P(o); d.ellipse((x - 3, y - 3, x + 3, y + 3), fill=(220, 220, 220, 220))
if route:
    x, y = P(fact); d.text((x + 10, y - 22), "factory (start of route)", font=F, fill=(255, 255, 255, 255))
    x, y = P(tgt); d.text((x + 10, y - 8), "front-line town (end)", font=F, fill=(255, 255, 255, 255))
d.rounded_rectangle((12, 12, 760, 168), 10, fill=(10, 14, 22, 225))
d.text((26, 20), "The link network supply travels along (Korea, start of the war)", font=FB, fill=(255, 255, 255, 255))
for i, (t, c) in enumerate([("red line: link between two DPRK objectives", (255, 90, 80)), ("blue line: link between two ROK objectives", (90, 150, 255)), ("yellow line: link crossing the front", (255, 225, 60)), ("white line: one sample route, factory to front (fewest links, %d links)" % max(0, len(route) - 1), (255, 255, 255))]):
    d.line((26, 62 + i * 24, 56, 62 + i * 24), fill=c + (255,), width=4); d.text((66, 53 + i * 24), t, font=F, fill=(235, 235, 235, 255))
d.text((26, 150), "squares: orange factory, cyan port, white depot (big) / army base (small); grey dot = city", font=F, fill=(190, 200, 215, 255))
im.convert("RGB").save(out); print(out, im.size, "route links", len(route) - 1, "links drawn", len(seen))
