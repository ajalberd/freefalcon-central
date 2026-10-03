"""Compare the campaign's 1 km cover grid (KOREA.THR) with the 3D terrain, cell by cell.

The campaign AI moves units, routes supply and plans paths on <theater>.THR: one byte per km cell,
cover type in the low nibble (0 = Water), drawn as its own raster (src/tools/makethr reads a
hand-made cover file) -- it is NOT derived from the 3D terrain. The 3D world is THEATER.L0: one ground
tile per campaign km (4x4 posts), indexed [y][x] (see ffcamp/terrain.py), each tile belonging to a
texture whose imagery shows how much of that km is water (tile_water_fraction; the set terrain types
and post heights both proved unreliable for this).

Read only. Reports how often the two disagree about water, whether the campaign grid is shifted as a
whole (best global offset), where the disagreements cluster, which objectives sit on cells the
campaign thinks are water but the 3D world shows as land, and writes difference maps.

    python cover_compare.py                       # report + cover_diff.png next to the THR
    python cover_compare.py --out DIR --crop 470:580:510:620   # also an enlarged crop

PNG colours: blue = both water, dark green = both land, RED = campaign water / 3D land,
YELLOW = campaign land / 3D water (sea or river), white dots = objectives.
"""

import argparse
import json
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "campaign-editor"))

import tilesurvey as ts  # noqa: E402

COVERAGE = {0: "nodata", 1: "water", 2: "river", 3: "swamp", 4: "plains", 5: "brush", 6: "thinforest",
            7: "thickforest", 8: "rocky", 9: "urban", 10: "road", 11: "rail", 12: "bridge", 13: "runway",
            14: "station"}
CAMP_COVER = ["Water", "Bog", "Barren", "Plain", "Brush", "LightForest", "HeavyForest", "Urban"]


def load_thr(path):
    d = open(path, "rb").read()
    mx, my = struct.unpack_from("<hh", d, 0)
    cells = np.frombuffer(d, dtype=np.uint8, offset=4, count=mx * my).reshape(mx, my)  # [x][y]
    return cells.T.copy(), mx, my  # -> [y][x]


def tile_water_fraction(th, texid, cache={}):
    """Share of a ground tile's pixels that are sea-coloured (blue over red and green). Neither the post
    heights (low coastal plains sit at 0 ft) nor the texture set's terrain type (the open-sea tile
    HCOST79F is typed 9) tell water from land; the imagery does: HCOST79F 1.00, HFARMD3C 0.00,
    HCOST7A6 (Togwon-ni's coast tile) 0.31."""
    if texid not in cache:
        frac = 0.0
        p = th.tile_path(texid)
        if p and os.path.exists(p):
            rgb, _w, _h, _f = ts.read_dds(p)
            if rgb is not None:
                r, g, b = (rgb[::4, ::4, k].astype(np.int16) for k in range(3))
                frac = float(((b > r + 15) & (b > g)).mean())
        cache[texid] = frac
    return cache[texid]


def load_3d(theater_root):
    """-> (water fraction 0..1 [y][x], sea-post count 0..16 [y][x]) at one value per campaign km."""
    th = ts.Theater(theater_root, 0)
    psz = th.map.post_size
    zoff = 4 if th.map.large_terrain else 2
    bw, bh = th.bw, th.bh
    km_w, km_h = bw * 4, bh * 4
    ttype = np.zeros((km_h, km_w), np.float32)
    sea = np.zeros((km_h, km_w), np.uint8)
    data = th.posts
    for br in range(bh):
        for bc in range(bw):
            o = int(th.offsets[br * bw + bc])
            buf = np.array(data[o:o + psz * 256]).reshape(256, psz)
            if th.map.large_terrain:
                tid = (buf[:, 0].astype(np.uint32) | (buf[:, 1].astype(np.uint32) << 8)
                       | (buf[:, 2].astype(np.uint32) << 16) | (buf[:, 3].astype(np.uint32) << 24))
            else:
                tid = buf[:, 0].astype(np.uint32) | (buf[:, 1].astype(np.uint32) << 8)
            z = (buf[:, zoff].astype(np.int16) | (buf[:, zoff + 1].astype(np.int16) << 8)).astype(np.int16)
            tid = tid.reshape(16, 16)
            z = z.reshape(16, 16)
            for r in range(4):
                for c in range(4):
                    y, x = br * 4 + r, bc * 4 + c
                    ttype[y, x] = tile_water_fraction(th, int(tid[r * 4, c * 4]))
                    sea[y, x] = int((z[r * 4:r * 4 + 4, c * 4:c * 4 + 4] <= 0).sum())
    return ttype, sea


def load_objectives(gamedir):
    """[(id, x, y, type, name)] from save0 via the campaign editor's reader, if it is available."""
    try:
        from ffcamp.workspace import TheaterWorkspace
        ws = TheaterWorkspace(gamedir)
        camp = ws.open_campaign("save0")
        out = []
        for o in camp.objectives:
            out.append((o.camp_id, o.x, o.y, o.type, ws.objective_name(o) if hasattr(ws, "objective_name") else ""))
        return out
    except Exception as e:  # noqa: BLE001 - optional
        print("  (objectives not listed: %s)" % e)
        return []


def best_shift(a, b, r=6):
    """Offset (dx, dy) of b against a that maximises agreement of two boolean masks."""
    best = None
    h, w = a.shape
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            aa = a[max(0, dy):h + min(0, dy), max(0, dx):w + min(0, dx)]
            bb = b[max(0, -dy):h + min(0, -dy), max(0, -dx):w + min(0, -dx)]
            mism = int((aa != bb).sum())
            if best is None or mism < best[0]:
                best = (mism, dx, dy)
    return best


def write_png(path, rgb):
    ts.write_png(path, rgb)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--theater", default=ts.default_theater())
    ap.add_argument("--thr", default=r"C:\FreeFalcon6\campaign\SAVE\KOREA.THR")
    ap.add_argument("--timeline", help="a campsim run .jsonl, for objective positions/types")
    ap.add_argument("--out", default=None)
    ap.add_argument("--crop", default=None, help="X0:Y0:X1:Y1 enlarged crop")
    ap.add_argument("--json", default=None, help="write the mismatch cells here")
    a = ap.parse_args()

    out = a.out or os.path.dirname(a.thr)
    cells, mx, my = load_thr(a.thr)
    cover = cells & 0x0F
    print("campaign grid %s: %dx%d" % (a.thr, mx, my))
    ttype, sea = load_3d(a.theater)
    print("3D terrain %s: %dx%d km" % (a.theater, ttype.shape[1], ttype.shape[0]))
    h = min(my, ttype.shape[0])
    w = min(mx, ttype.shape[1])
    cover, ttype, sea = cover[:h, :w], ttype[:h, :w], sea[:h, :w]

    camp_w = cover == 0
    seaw = ttype >= 0.5                        # 3D: half or more of the tile's imagery is water
    riverw = np.zeros_like(seaw)               # rivers are drawn inside land tiles, not measured here
    d3_w = seaw

    n = h * w
    both_w = int((camp_w & d3_w).sum())
    both_l = int((~camp_w & ~d3_w).sum())
    phantom = camp_w & ~d3_w                   # campaign water, 3D land
    missing = ~camp_w & d3_w
    print("\n=== water agreement over %d cells ===" % n)
    print("  both water %d, both land %d, agree %.2f%%" % (both_w, both_l, 100.0 * (both_w + both_l) / n))
    print("  campaign WATER but 3D land : %6d cells" % int(phantom.sum()))
    print("  campaign land but 3D water : %6d cells (sea %d, river %d)" % (
        int(missing.sum()), int((missing & seaw).sum()), int((missing & riverw).sum())))
    print("  3D water cells: sea %d, river %d; campaign water cells %d" % (
        int(seaw.sum()), int(riverw.sum()), int(camp_w.sum())))

    mism, dx, dy = best_shift(camp_w, seaw)
    mism0 = int((camp_w != seaw).sum())
    print("\n=== global alignment (campaign water vs 3D sea) ===")
    print("  mismatched cells at no shift: %d; best shift dx=%+d dy=%+d km: %d (%.1f%% better)" % (
        mism0, dx, dy, mism, 100.0 * (mism0 - mism) / max(1, mism0)))

    # where do the disagreements cluster? 32 km squares
    print("\n=== worst 32x32 km squares (campaign water / 3D land) ===")
    sq = phantom[:h - h % 32, :w - w % 32].reshape(h // 32, 32, w // 32, 32).sum(axis=(1, 3))
    order = np.dstack(np.unravel_index(np.argsort(-sq.ravel()), sq.shape))[0][:10]
    for sy, sx in order:
        if sq[sy, sx] == 0:
            break
        print("  x %4d-%4d y %4d-%4d: %4d cells" % (sx * 32, sx * 32 + 31, sy * 32, sy * 32 + 31, sq[sy, sx]))

    # objectives on mismatched cells
    objs = []
    if a.timeline:
        meta = json.loads(open(a.timeline).readline())
        objs = [(o[0], o[1], o[2], o[3]) for o in meta["objs"]]
    if objs:
        on_ph = [o for o in objs if 0 <= o[1] < w and 0 <= o[2] < h and phantom[o[2], o[1]]]
        on_wat = [o for o in objs if 0 <= o[1] < w and 0 <= o[2] < h and camp_w[o[2], o[1]]]
        print("\n=== objectives ===")
        print("  %d objectives on cells the campaign calls water; %d of those are LAND in 3D" % (
            len(on_wat), len(on_ph)))
        bytype = {}
        for o in on_ph:
            bytype[o[3]] = bytype.get(o[3], 0) + 1
        print("  by objective type (on campaign-water / 3D-land cells): %s" % dict(sorted(bytype.items())))
        for o in on_ph[:40]:
            print("    #%d type %d at (%d,%d): 3D tile %.0f%% water" % (
                o[0], o[3], o[1], o[2], 100 * ttype[o[2], o[1]]))

    # difference map, north up
    rgb = np.zeros((h, w, 3), np.uint8)
    rgb[~camp_w & ~d3_w] = (30, 60, 30)
    rgb[camp_w & d3_w] = (40, 80, 200)
    rgb[phantom] = (230, 40, 40)
    rgb[missing] = (240, 220, 40)
    for o in objs:
        if 0 <= o[1] < w and 0 <= o[2] < h:
            rgb[o[2], o[1]] = (255, 255, 255)
    img = rgb[::-1]
    path = os.path.join(out, "cover_diff.png")
    write_png(path, img)
    print("\nwrote %s (north up, 1 px = 1 km)" % path)
    if a.crop:
        x0, y0, x1, y1 = map(int, a.crop.split(":"))
        crop = rgb[y0:y1 + 1, x0:x1 + 1][::-1]
        k = max(1, 600 // max(1, crop.shape[1]))
        big = np.repeat(np.repeat(crop, k, 0), k, 1)
        cpath = os.path.join(out, "cover_diff_%d_%d.png" % (x0, y0))
        write_png(cpath, big)
        print("wrote %s (%dx enlarged)" % (cpath, k))
    if a.json:
        ys, xs = np.nonzero(phantom)
        ym, xm = np.nonzero(missing)
        json.dump({"campaign_water_3d_land": [[int(x), int(y)] for x, y in zip(xs, ys)],
                   "campaign_land_3d_water": [[int(x), int(y), "sea" if seaw[y, x] else "river"]
                                              for x, y in zip(xm, ym)]}, open(a.json, "w"))
        print("wrote %s" % a.json)


if __name__ == "__main__":
    main()
