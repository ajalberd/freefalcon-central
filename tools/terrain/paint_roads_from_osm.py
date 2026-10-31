"""Set the road bit (0x40) in a theatre's campaign cover grid (<theatre>.thr) from OpenStreetMap roads.

    python paint_roads_from_osm.py --tdf Theaters\\Taiwan\\taiwan.tdf --thr C:\\FreeFalcon6\\Theaters\\Taiwan\\campaign\\Taiwan.thr \
                                   [--classes motorway,trunk,primary] [--dry-run] [--png roads.png]

Taiwan.thr had no road cell at all (Korea: 9.6 % of its enterable cells, 57 % of its objectives on one).
The engine only reads the bit: a road cell halves the ground cost of entering it, and with PATH_ROADOK a move
along road costs 0.5. So the placement that matters is *connected* road between towns, on land, in the right
place -- a road painted onto a water cell would read as a bridge that needs a bridge objective, so those are
skipped.

Where the road is "right" comes from the same pipeline the rail lines use (campaign-editor/ffcamp/rail.py):
fit lat/lon -> grid km on the theatre's airbase objectives, then refine on the OSM coastline against the 3D
terrain's own shore, so the road lands where the 3D world draws the island, not where a 2011 conversion put
it. Reports how well that worked: coast median distance, and the distance from each city objective to the
nearest painted road cell.

Writes a .bak-pre-roads backup first and refuses to run twice on the same file (use --force to repaint from
that backup).
"""
import argparse
import math
import os
import shutil
import struct
import sys
import tempfile
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "campaign-editor"))
import cover_compare as cc  # noqa: E402
from ffcamp import rail  # noqa: E402
from ffcamp.workspace import TheaterWorkspace  # noqa: E402

CACHE = os.path.join(tempfile.gettempdir(), "ffcamp-rail")
ROAD_BIT = 0x40

# Real positions of airbases/airstrips, by the objective's name. Only fields whose spot is not in doubt; the
# fit drops any whose residual stands out. Taiwan, the offshore islands, the Ryukyus and the mainland coast.
AIRBASES = {
    "Chiang Kai Shek Intl": (25.0777, 121.2325), "Sungshan Intl.": (25.0694, 121.5522),
    "Ching Chuan Kang Airbase": (24.2647, 120.6208), "Chiayi Airbase": (23.4618, 120.3928),
    "Kao-Hsiung": (22.5771, 120.3500), "Pingtung North": (22.6722, 120.4619),
    "Hua-Lien": (24.0231, 121.6180), "Hsin-Chu Airbase": (24.8180, 120.9328),
    "Makung": (23.5687, 119.6283), "Wang An Strip": (23.3683, 119.5003),
    "Heng-Chjun Airstrip": (22.0357, 120.7339), "Orchid Island Airstrip": (22.0270, 121.5333),
    "Shatou": (24.4279, 118.3592), "Qingyang": (24.7964, 118.5902),
    "Xiamen": (24.5440, 118.1277), "Matsu South": (26.1578, 119.9583), "Ma Tsu": (26.2242, 119.9990),
    "Pratas Island": (20.7067, 116.7259), "Basco": (20.4513, 121.9803),
    "Yonaguni": (24.4669, 122.9779), "Ishigaki": (24.3446, 124.1867), "Hateruma Airbase": (24.0589, 123.8056),
    "Tarama Jima Airbase": (24.6539, 124.6753),
    "Wenzhou": (27.9122, 120.8520), "Meixian": (24.3500, 116.1333), "Liangcheng": (25.6747, 116.7472),
    "Luqiaq": (28.5622, 121.4294), "Quxian": (28.9658, 118.8997), "Yiwu": (29.3447, 120.0314),
}
AIRFIELD_TYPES = ("Airbase", "Airstrip")

QUERY = """[out:json][timeout:600];
way["highway"~"^(%s)$"]({bbox});
out tags geom;"""


def pairs_from_campaign(ws):
    """(name, lat, lon, x, y) for each uniquely named airfield in the table, at its cell centre."""
    names = ws.name_table()
    seen = {}
    for o in ws.objectives("save1.cam").objectives:
        if o.get("typeName") in AIRFIELD_TYPES:
            seen.setdefault(names.get(o["nameId"], "").strip(), []).append(o)
    out = []
    for name, objs in seen.items():
        if name in AIRBASES and len(objs) == 1:
            o = objs[0]
            out.append((name, AIRBASES[name][0], AIRBASES[name][1], o["x"] + 0.5, o["y"] + 0.5))
    return out


def fit_projection(ws, terr, log, refine=True):
    pairs = pairs_from_campaign(ws)
    log("%d airfields with a known position" % len(pairs))
    lat0 = float(np.mean([p[1] for p in pairs]))
    lon0 = float(np.mean([p[2] for p in pairs]))
    proj, control = rail.fit_airbases(pairs, lat0, lon0, log)
    w = h = terr.size_km
    corners = [proj.inverse(x, y) for x in (0, w) for y in (0, h)]
    bbox = (math.floor(min(c[0] for c in corners) - 0.3), math.floor(min(c[1] for c in corners) - 0.3),
            math.ceil(max(c[0] for c in corners) + 0.3), math.ceil(max(c[1] for c in corners) + 0.3))
    log("theater box %d..%dN %d..%dE" % (bbox[0], bbox[2], bbox[1], bbox[3]))
    if not refine:
        return proj, bbox, None, None
    coast = rail.load_ways(rail.fetch("coast", bbox, CACHE, log=log))
    log("%d coastline ways; terrain shoreline..." % len(coast))
    dist = rail.shore_distance(rail.sea_mask(rail.water_mask(terr)))
    lat, lon = rail.coast_samples(coast)
    med, frac = rail.refine_to_shore(proj, lat, lon, dist, control, log=log)
    rms = rail.control_residuals(proj, control)
    log("airfields after refinement: %.1f km RMS, coast median %.2f km, %.0f%% within 1.5 km"
        % (rms, med, frac * 100))
    return proj, bbox, med, frac


def fetch_roads(classes, bbox, log):
    rail.QUERIES["road_" + classes] = QUERY % classes.replace(",", "|")
    return rail.load_ways(rail.fetch("road_" + classes, bbox, CACHE, log=log))


def raster(ways, proj, w, h, land):
    """[y][x] bool: cells a road polyline passes through (dense sampling, 8-connected), on land cells."""
    road = np.zeros((h, w), bool)
    skipped = 0
    for _tags, pts in ways:
        a = np.array(pts, float)
        x, y = proj(a[:, 0], a[:, 1])
        for i in range(len(x) - 1):
            n = max(2, int(math.hypot(x[i + 1] - x[i], y[i + 1] - y[i]) * 4) + 1)
            t = np.linspace(0.0, 1.0, n)
            cx = np.floor(x[i] + (x[i + 1] - x[i]) * t).astype(int)
            cy = np.floor(y[i] + (y[i + 1] - y[i]) * t).astype(int)
            ok = (cx >= 0) & (cy >= 0) & (cx < w) & (cy < h)
            cx, cy = cx[ok], cy[ok]
            on = land[cy, cx]
            skipped += int((~on).sum())
            road[cy[on], cx[on]] = True
    return road, skipped


LINK_MAX_KM = 80.0       # a link longer than this is a sea or air hop, not a road
SPUR_MAX_KM = 15.0       # an unlinked objective further than this from every road stays off the network
DETOUR_MAX = 3.0         # a road more than this times the straight distance is not worth painting
RELIEF_COST = (1.0, 1.41, 1.73, 2.0)     # the engine's ReliefCost
ROAD_COST = 0.15         # entering a road cell: routes reuse the OSM roads where there are some
ROADLESS_TYPES = ("Hill top", "Pass", "Beach", "Radar", "SAM site", "Radio tower")


def route(road, cells, land, a, b, margin=12):
    """Cheapest 8-connected land path a -> b (cell (x, y) tuples) in a window round them, or None.

    Cost per cell: relief, a bonus for cells already road. Returns (path, straight-line km)."""
    import heapq
    h, w = land.shape
    x0, x1 = max(0, min(a[0], b[0]) - margin), min(w - 1, max(a[0], b[0]) + margin)
    y0, y1 = max(0, min(a[1], b[1]) - margin), min(h - 1, max(a[1], b[1]) + margin)
    relief = np.array(RELIEF_COST)[(cells[y0:y1 + 1, x0:x1 + 1] >> 4) & 3]
    step = np.where(road[y0:y1 + 1, x0:x1 + 1], ROAD_COST, relief)
    ok = land[y0:y1 + 1, x0:x1 + 1]
    sx, sy, tx, ty = a[0] - x0, a[1] - y0, b[0] - x0, b[1] - y0
    INF = 1e18
    dist = np.full(ok.shape, INF)
    prev = {}
    dist[sy, sx] = 0.0
    pq = [(0.0, sx, sy)]
    while pq:
        d, x, y = heapq.heappop(pq)
        if d > dist[y, x]:
            continue
        if (x, y) == (tx, ty):
            break
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                nx, ny = x + dx, y + dy
                if (dx or dy) and 0 <= nx < ok.shape[1] and 0 <= ny < ok.shape[0] and ok[ny, nx]:
                    nd = d + step[ny, nx] * (1.4142 if dx and dy else 1.0)
                    if nd < dist[ny, nx]:
                        dist[ny, nx] = nd
                        prev[(nx, ny)] = (x, y)
                        heapq.heappush(pq, (nd, nx, ny))
    if dist[ty, tx] >= INF:
        return None
    path, c = [], (tx, ty)
    while c != (sx, sy):
        path.append((c[0] + x0, c[1] + y0))
        c = prev[c]
    path.append((a[0], a[1]))
    return path


def nearest_land(land, x, y, reach=3):
    h, w = land.shape
    best = None
    for r in range(reach + 1):
        for dx in range(-r, r + 1):
            for dy in range(-r, r + 1):
                nx, ny = x + dx, y + dy
                if 0 <= nx < w and 0 <= ny < h and land[ny, nx] and (best is None or dx * dx + dy * dy < best[0]):
                    best = (dx * dx + dy * dy, nx, ny)
        if best:
            return best[1], best[2]
    return None


def link_roads(ws, cells, land, road, log):
    """Paint the campaign's own road network: a land path for every objective link in any of the theatre's saves,
    then a spur from every unlinked ground objective to the nearest road. Bridge and port objectives get the
    road bit on their own cell even over water: the engine reads road + water as a bridge/port."""
    import glob
    h, w = land.shape
    objs, edges = {}, set()
    for path in sorted(glob.glob(os.path.join(ws.campaign_dir, "save*.cam"))):
        cam = ws.objectives(os.path.basename(path))
        by = {tuple(o["id"]): o for o in cam.objectives}
        for o in cam.objectives:
            objs[(o["x"], o["y"])] = o["typeName"]
            for l in o["links"]:
                t = by.get(tuple(l["id"]))
                if t is not None:
                    a, b = (o["x"], o["y"]), (t["x"], t["y"])
                    edges.add((min(a, b), max(a, b)))
    wet = np.zeros_like(land)                       # bridge / port cells that are water
    for (x, y), tn in objs.items():
        if tn in ("Bridge", "Port") and 0 <= x < w and 0 <= y < h:
            road[y, x] = True
            wet[y, x] = not land[y, x]
    ok = land | wet            # a bridge cell is passable, so the roads on both banks join through it
    edges = sorted(edges, key=lambda e: math.hypot(e[0][0] - e[1][0], e[0][1] - e[1][1]))
    done = skipped_far = nopath = detour = 0
    for a, b in edges:
        km = math.hypot(a[0] - b[0], a[1] - b[1])
        if km > LINK_MAX_KM:
            skipped_far += 1
            continue
        pa, pb = (nearest_land(ok, a[0], a[1]), nearest_land(ok, b[0], b[1]))
        if not pa or not pb:
            nopath += 1
            continue
        p = route(road, cells, ok, pa, pb, margin=int(min(40, 8 + km * 0.5)))
        if p is None:
            nopath += 1
        elif len(p) > DETOUR_MAX * max(km, 1) + 6:
            detour += 1
        else:
            for x, y in p:
                road[y, x] = True
            done += 1
    log("links: %d edges, %d painted, %d longer than %.0f km, %d with no land path, %d too winding"
        % (len(edges), done, skipped_far, LINK_MAX_KM, nopath, detour))

    # spurs: ground objectives that no link reaches
    linked = {c for e in edges for c in e}
    ys, xs = np.nonzero(road & land)
    spurs = far = 0
    for (x, y), tn in sorted(objs.items()):
        if (x, y) in linked or tn in ROADLESS_TYPES or not (0 <= x < w and 0 <= y < h) or road[y, x]:
            continue
        if not land[y, x] or len(xs) == 0:
            continue
        d = np.hypot(xs - x, ys - y)
        i = int(d.argmin())
        if d[i] > SPUR_MAX_KM:
            far += 1
            continue
        p = route(road, cells, land, (x, y), (int(xs[i]), int(ys[i])), margin=int(6 + d[i]))
        if p is not None:
            for px, py in p:
                road[py, px] = True
            spurs += 1
    log("spurs: %d unlinked ground objectives joined to the nearest road, %d more than %.0f km from any" % (spurs, far, SPUR_MAX_KM))
    return road


def objective_report(ws, road, land, log):
    """Distance from each objective to the nearest road cell, by type, and the share exactly on one."""
    names = ws.name_table()
    ys, xs = np.nonzero(road)
    out = {}
    for o in ws.objectives("save1.cam").objectives:
        out.setdefault(o.get("typeName", "?"), []).append((o["x"], o["y"]))
    tot = on = 0
    log("objective type        count  on road  median km to road  within 3 km")
    for tn, cs in sorted(out.items(), key=lambda kv: -len(kv[1])):
        d = []
        for x, y in cs:
            d.append(float(np.sqrt(((xs - x) ** 2 + (ys - y) ** 2).min())) if len(xs) else 1e9)
        d = np.array(d)
        nroad = int(sum(1 for x, y in cs if road[y, x]))
        tot += len(cs)
        on += nroad
        if len(cs) >= 5:
            log("  %-18s %5d  %6d  %17.1f  %10.0f%%" % (tn, len(cs), nroad, float(np.median(d)),
                                                         100 * float((d <= 3).mean())))
    log("  all %d objectives, %d on a painted road cell (%.0f%%)" % (tot, on, 100.0 * on / max(1, tot)))


def write_png(path, cells, road):
    from PIL import Image
    land = (cells & 0x0F) != 0
    img = np.zeros(cells.shape + (3,), np.uint8)
    img[~land] = (20, 40, 90)
    img[land] = (60, 90, 55)
    img[road] = (255, 210, 60)
    Image.fromarray(img[::-1]).save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gamedir", default=r"C:\FreeFalcon6")
    ap.add_argument("--tdf", default=r"Theaters\Taiwan\taiwan.tdf")
    ap.add_argument("--thr", required=True)
    ap.add_argument("--classes", default="motorway,trunk,primary")
    ap.add_argument("--no-refine", action="store_true", help="airfield affine only; skip the coastline download")
    ap.add_argument("--no-osm", action="store_true", help="no download: roads from the campaign's own objective links only")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--force", action="store_true", help="repaint from the .bak-pre-roads backup")
    ap.add_argument("--png")
    a = ap.parse_args()
    log = lambda s: print(s, flush=True)

    ws = TheaterWorkspace(a.gamedir, a.tdf)
    terr = ws.terrain()
    bak = a.thr + ".bak-pre-roads"
    src = a.thr
    if os.path.exists(bak):
        if not a.force and not a.dry_run:
            sys.exit("refusing: %s exists (already painted); --force repaints from it" % bak)
        src = bak
    cells, mx, my = cc.load_thr(src)                      # [y][x]
    land = (cells & 0x0F) != 0
    log("grid %dx%d, land cells %d, road cells before %d" % (mx, my, int(land.sum()), int(((cells & ROAD_BIT) != 0).sum())))

    road = np.zeros((my, mx), bool)
    if not a.no_osm:
        proj, bbox, med, frac = fit_projection(ws, terr, log, not a.no_refine)
        ways = fetch_roads(a.classes, bbox, log)
        log("%d road ways" % len(ways))
        road, skipped = raster(ways, proj, mx, my, land)
        log("OSM road cells %d; %d samples fell on water and were dropped" % (int(road.sum()), skipped))
    road = link_roads(ws, cells, land, road, log)
    log("road cells %d (%.1f%% of %d enterable)" % (int(road.sum()), 100.0 * road.sum() / land.sum(), int(land.sum())))
    objective_report(ws, road, land, log)
    if a.png:
        write_png(a.png, cells, road)
        log("wrote " + a.png)
    if a.dry_run:
        return

    if src == a.thr:
        shutil.copy2(a.thr, bak)
    new = cells.copy()
    new[road] |= ROAD_BIT
    with open(a.thr, "wb") as fh:
        fh.write(struct.pack("<hh", mx, my))
        fh.write(new.T.copy().tobytes())
    log("wrote %s, backup %s" % (a.thr, bak))


if __name__ == "__main__":
    main()
