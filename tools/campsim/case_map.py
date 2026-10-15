"""Draw single failed store searches: python case_map.py <run name> [case numbers...] [out.png]
Reads SMAPCASE / SMAPNODE lines (campsim, stocks=1).  Per panel: the white diamond is the battalion; filled circles are the objectives the
search reached (owner colours: blue ROK, red DPRK), crossed circles are neighbours it refused (enemy-owned or a broken bridge), the gold
square is the nearest own depot / army base when the search is allowed to ignore owners and costs."""
import math, os, sys, collections
from PIL import Image, ImageDraw, ImageFont, ImageEnhance
HERE = os.path.dirname(os.path.abspath(__file__))
args = sys.argv[1:]; name = args[0]
out = args[-1] if args[-1].endswith(".png") else os.path.join(HERE, name + "-cases.png")
want = [int(a) for a in args[1:] if a.isdigit()]
cases = collections.defaultdict(list); unit = {}
for l in open(os.path.join(HERE, "runs", name + ".log"), errors="replace"):
    if l.startswith("SMAPCASE"): a = list(map(int, l.split()[1:])); unit[a[0]] = a[1:]
    elif l.startswith("SMAPNODE"): a = list(map(int, l.split()[1:])); cases[a[0]].append(a[1:])
want = want or sorted(cases)[:4]
bd = Image.open(os.path.join(HERE, "backdrop.png")).convert("RGB")
def font(n):
    for f in ("arial.ttf", "segoeui.ttf", "DejaVuSans.ttf"):
        try: return ImageFont.truetype(f, n)
        except Exception: pass
    return ImageFont.load_default()
F, FB, FS = font(15), font(18), font(13)
NAME = {3: "army base", 10: "depot", 8: "city", 6: "bridge", 11: "factory", 19: "port", 1: "airbase", 2: "airstrip"}
COL = {2: (80, 150, 255), 6: (255, 85, 75)}
PW = 640; panels = []
for k in want:
    ns = cases[k]; ux, uy, team, anchor = unit[k]
    xs = [n[3] for n in ns] + [ux]; ys = [n[4] for n in ns] + [uy]
    cx, cy = (min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2
    span = max(max(xs) - min(xs), max(ys) - min(ys), 16) + 16
    S = PW / span
    x0, y1 = cx - span / 2, cy + span / 2
    crop = bd.crop((int(x0 * 2), int((1024 - y1) * 2), int((x0 + span) * 2), int((1024 - y1 + span) * 2))).resize((PW, PW))
    im = ImageEnhance.Brightness(crop).enhance(0.55); d = ImageDraw.Draw(im, "RGBA")
    P = lambda x, y: ((x - x0) * S, (y1 - y) * S)
    store = [n for n in ns if n[5] == 2]
    if store:
        a, b = P(ux, uy), P(store[0][3], store[0][4]); km = math.hypot(ux - store[0][3], uy - store[0][4])
        for i in range(0, 20, 2):
            d.line((a[0] + (b[0] - a[0]) * i / 20, a[1] + (b[1] - a[1]) * i / 20, a[0] + (b[0] - a[0]) * (i + 1) / 20, a[1] + (b[1] - a[1]) * (i + 1) / 20), fill=(255, 215, 0, 220), width=3)
        d.text(((a[0] + b[0]) / 2 + 8, (a[1] + b[1]) / 2 - 18), "%.0f km" % km, font=F, fill=(255, 215, 0, 255))
    for oid, t, tm, x, y, kind in ns:
        px, py = P(x, y); c = COL.get(tm, (190, 190, 190)); r = 12
        if kind == 0:
            d.ellipse((px - r, py - r, px + r, py + r), fill=c + (255,), outline=(255, 255, 255, 255), width=3)
        elif kind == 1:
            d.ellipse((px - r, py - r, px + r, py + r), fill=c + (110,), outline=c + (255,), width=3)
            d.line((px - 7, py - 7, px + 7, py + 7), fill=(255, 255, 255, 255), width=3); d.line((px - 7, py + 7, px + 7, py - 7), fill=(255, 255, 255, 255), width=3)
        else:
            d.rectangle((px - 14, py - 14, px + 14, py + 14), fill=c + (255,), outline=(255, 215, 0, 255), width=4)
        if kind != 2 or True:
            d.text((px + 14, py + 8), NAME.get(t, "obj %d" % t), font=FS, fill=(255, 255, 255, 235))
    px, py = P(ux, uy); d.polygon([(px, py - 15), (px + 15, py), (px, py + 15), (px - 15, py)], fill=(255, 255, 255, 255), outline=(0, 0, 0, 255))
    reached = sum(1 for n in ns if n[5] == 0); ref = sum(1 for n in ns if n[5] == 1)
    d.rounded_rectangle((8, 8, PW - 8, 66), 8, fill=(10, 14, 22, 220))
    d.text((18, 14), "%s battalion at grid %d,%d: search reached %d objectives, refused %d" % ("DPRK" if team == 6 else "ROK", ux, uy, reached, ref), font=F, fill=(255, 255, 255, 255))
    d.text((18, 38), "gold square = nearest own store (%s), %.0f km away" % (NAME.get(store[0][1], "?"), km) if store else "no own store at all", font=F, fill=(255, 215, 0, 255))
    panels.append(im)
cols = 2; rows = (len(panels) + 1) // 2
sheet = Image.new("RGB", (cols * (PW + 10) + 10, rows * (PW + 10) + 84), (14, 18, 26)); sd = ImageDraw.Draw(sheet)
sd.text((14, 10), "Why a battalion cannot reach a supply store: real cases (Korea Rolling Fire, stores on, first 2 days)", font=FB, fill=(255, 255, 255))
sd.text((14, 38), "white diamond = battalion   filled circle = objective the search reached   crossed circle = refused neighbour (owned by the other side)   gold square = nearest own store", font=F, fill=(190, 200, 215))
for i, im in enumerate(panels): sheet.paste(im, (10 + (i % cols) * (PW + 10), 74 + (i // cols) * (PW + 10)))
sheet.save(out); print(out, sheet.size)
