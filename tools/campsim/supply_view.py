"""supply_view.py <run> [days, default 2,4,6,8] [out.png]: the supply picture a player would need, one panel per day.

Battalions: Blue circles, the North squares, China triangles; green >= 75% supply, yellow 50-74, red < 50 (a red Blue spearhead is an offensive
that has outrun its supply). Depots / army bases: white diamonds (Blue's, outlined blue; the North's / China's, outlined red). The caption gives
each side's battalions under 50% and the share of Blue's front battalions (within 40 km of its northern edge) under 50%.
Reads tools/campsim/runs/<run>.jsonl on the theater backdrop (backdrop.png, 2 px per km, north up)."""
import json
import os
import sys

from PIL import Image, ImageDraw, ImageEnhance, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
STORES = (3, 10)  # army base, depot


def font(n):
    for f in ("arial.ttf", "segoeui.ttf", "DejaVuSans.ttf"):
        try:
            return ImageFont.truetype(f, n)
        except OSError:
            pass
    return ImageFont.load_default()


def main():
    run = sys.argv[1]
    days = [int(d) for d in (sys.argv[2] if len(sys.argv) > 2 else "2,4,6,8").split(",")]
    out = sys.argv[3] if len(sys.argv) > 3 else os.path.join(HERE, run + "-supply.png")
    lines = open(os.path.join(HERE, "runs", run + ".jsonl")).readlines()
    meta = json.loads(lines[0])
    objs = {o[0]: o for o in meta["objs"]}
    owner = {o[0]: o[4] for o in meta["objs"]}
    frames, snaps = [json.loads(l) for l in lines[1:] if l.startswith('{"t"')], {}
    for f in frames:  # ownership as of each frame (change rows only)
        for row in f.get("o") or []:
            owner[row[0]] = row[1]
        for d in days:
            if d not in snaps and f["t"] >= d * 24 * 60:
                snaps[d] = (f, dict(owner))
    x0, x1, y0, y1 = 200, 820, 380, 880  # km window: the DMZ to the Yalu and Dandong
    S = 1.6  # px per km
    bd = Image.open(os.path.join(HERE, "backdrop.png")).convert("RGB")
    base = ImageEnhance.Brightness(bd.crop((x0 * 2, (1024 - y1) * 2, x1 * 2, (1024 - y0) * 2))).enhance(0.5)
    base = base.resize((int((x1 - x0) * S), int((y1 - y0) * S)))
    W, H = base.size
    P = lambda x, y: ((x - x0) * S, (y1 - y) * S)
    col = lambda s: (60, 200, 60, 255) if s >= 75 else (240, 200, 40, 255) if s >= 50 else (230, 50, 40, 255)
    panels = []
    for d in days:
        if d not in snaps:
            continue
        f, own = snaps[d]
        im = base.copy()
        dr = ImageDraw.Draw(im, "RGBA")
        for oid, t in own.items():
            o = objs[oid]
            if o[3] in STORES and x0 <= o[1] <= x1 and y0 <= o[2] <= y1:
                px, py = P(o[1], o[2])
                dr.polygon([(px, py - 5), (px + 5, py), (px, py + 5), (px - 5, py)], fill=(255, 255, 255, 200),
                           outline=(60, 120, 255, 255) if t in (1, 2, 3) else (255, 60, 60, 255))
        bl = [u for u in f["u"] if u[1] == 0 and u[2] in (1, 2, 3)]
        nk = [u for u in f["u"] if u[1] == 0 and u[2] == 6]
        cn = [u for u in f["u"] if u[1] == 0 and u[2] == 5]
        for u in bl:
            px, py = P(u[3], u[4])
            dr.ellipse((px - 3, py - 3, px + 3, py + 3), fill=col(u[11]))
        for u in nk:
            px, py = P(u[3], u[4])
            dr.rectangle((px - 3, py - 3, px + 3, py + 3), fill=col(u[11]))
        for u in cn:
            px, py = P(u[3], u[4])
            dr.polygon([(px, py - 4), (px + 4, py + 3), (px - 4, py + 3)], fill=col(u[11]))
        ys = sorted(u[4] for u in bl)
        edge = sum(ys[-10:]) / 10 if len(ys) >= 10 else 0
        front = [u for u in bl if abs(u[4] - edge) <= 40]
        low = lambda us: sum(1 for u in us if u[11] < 50)
        cap = "day %d  |  Blue <50%%: %d (front %d of %d)  North: %d  China: %d  |  Blue objectives %d" % (
            d, low(bl), low(front), len(front), low(nk), low(cn), f["s"][2][4])
        dr.rectangle((0, 0, W, 22), fill=(0, 0, 0, 190))
        dr.text((6, 3), cap, fill=(255, 255, 255, 255), font=font(14))
        panels.append(im)
    cols = 2
    rows = (len(panels) + cols - 1) // cols
    sheet = Image.new("RGB", (W * cols, H * rows + 26), (20, 20, 20))
    for i, p in enumerate(panels):
        sheet.paste(p, ((i % cols) * W, 26 + (i // cols) * H))
    ImageDraw.Draw(sheet).text((6, 5), "%s -- circles Blue, squares North, triangles China; green >=75%% supply, yellow 50-74, red <50; "
                                         "diamonds depots / army bases (blue / red outline = owner)" % run, fill=(230, 230, 230), font=font(14))
    sheet.save(out)
    print("wrote", out)


if __name__ == "__main__":
    main()
