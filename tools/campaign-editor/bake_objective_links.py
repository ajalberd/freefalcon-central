"""Write a proper objective link graph into a theatre's campaign files (the one the engine loads), land only.

    python bake_objective_links.py --tdf Theaters\\Taiwan\\taiwan.tdf --thr C:\\FreeFalcon6\\Theaters\\Taiwan\\campaign\\Taiwan.thr
    python bake_objective_links.py ... --write [--files save0.cam save1.cam save2.cam]

Ground units do not path cell by cell: BuildGroundWP routes them objective to objective over the links stored
in each objective record (GetUnitObjectivePath) and only grid-paths the last leg. The converted Taiwan theatre
ships 1.08 links per objective with 58 % of objectives having none (Korea 2.31 and 3 %), so most villages,
bridges, cities and airbases cannot be reached or left by a column ("no waypoints").
cmpclass.cpp's BuildMissingObjectiveLinks patches that at load in builds that have it; this puts the links in
the data so every build, the campaign editor's Links layer and tools/terrain's road painter see them.

What it adds, per file, keeping every existing link:
  * every objective gets at least two links (angularly apart where it can), to its cheapest land neighbours
    within --radius km; cost is the grid path cost the engine itself would compute (Dijkstra on the .thr with
    path.cpp's cost tables: cover, relief, road halving, bridges entered straight on, diagonals x1.41),
  * pieces of the graph that are still apart join their nearest piece within --join km when a land path exists
    (Korea's longest link is 65 km). Islands stay islands: no link crosses water, and bridge / port objectives
    are the only water cells a path may use.
Costs are written per movement type like LinkCampaignObjectives does (NoMove = wheeled on roads, Foot, Wheeled,
Tracked from the path, LowAir/Air from the distance, Naval/Rail 255; ground types 254 if only road reaches).

Backs each file up as <file>.bak-prelinks before writing and reads it back.
"""
import argparse
import heapq
import math
import os
import shutil
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "terrain"))
import cover_compare as cc  # noqa: E402
from ffcamp.workspace import TheaterWorkspace  # noqa: E402
from ffcamp import objectives as objmod  # noqa: E402

# path.cpp CostTable columns NoMove, Foot, Wheeled, Tracked, LowAir, Air, Naval, Rail for cover 0..7
COST = np.array([
    [99, 99, 99, 99, 1, 1, 1, 99],
    [99, 3, 8, 6, 1, 1, 99, 99],
    [99, 1.5, 4, 3, 1, 1, 99, 99],
    [99, 1, 3, 2, 1, 1, 99, 99],
    [99, 1, 3, 2, 1, 1, 99, 99],
    [99, 2, 6, 4, 1, 1, 99, 99],
    [99, 2, 6, 4, 1, 1, 99, 99],
    [99, 2, 3, 3, 1, 1, 99, 99]], float)
RELIEF = np.array([1.0, 1.41, 1.73, 2.0])
MAXC = 90.0
NOMOVE, FOOT, WHEELED, TRACKED = 0, 1, 2, 3
VUID_BYTES = 8
S8 = [(-1, -1), (0, -1), (1, -1), (-1, 0), (1, 0), (-1, 1), (0, 1), (1, 1)]


class Grid:
    def __init__(self, thr):
        c, self.w, self.h = cc.load_thr(thr)
        self.cover = (c & 0x0F).astype(int)
        self.rel = RELIEF[(c >> 4) & 3]
        self.road = (c & 0x40) != 0
        self.land = self.cover != 0
        self.bridge = np.zeros(c.shape, bool)       # water + road at a bridge/port objective

    def mark_bridges(self, objs):
        for o in objs:
            if o["typeName"] in ("Bridge", "Port"):
                self.bridge[o["y"], o["x"]] = True
                self.road[o["y"], o["x"]] = True

    def dijkstra(self, a, mtype, radius, target=None, roadok=False):
        """Cost from cell a=(x, y) to every cell within `radius` (or just to `target`); dict {(x, y): cost}."""
        x0, y0 = a
        dist = {a: 0.0}
        pq = [(0.0, x0, y0)]
        col = {NOMOVE: WHEELED}.get(mtype, mtype)
        roadok = roadok or mtype == NOMOVE
        w, h = self.w, self.h
        while pq:
            d, x, y = heapq.heappop(pq)
            if d > dist.get((x, y), 1e18):
                continue
            if target and (x, y) == target:
                return dist
            for dx, dy in S8:
                nx, ny = x + dx, y + dy
                if not (0 <= nx < w and 0 <= ny < h) or max(abs(nx - x0), abs(ny - y0)) > radius:
                    continue
                diag = dx and dy
                if self.bridge[ny, nx]:
                    if diag:
                        continue                          # a bridge is entered straight on
                    c = 0.5
                else:
                    base = COST[self.cover[ny, nx], col]
                    if base > MAXC:
                        continue
                    c = base
                    if self.road[ny, nx] and not diag:
                        c = 0.5 if (roadok and self.road[y, x]) else base * 0.5
                    c *= self.rel[ny, nx]
                if diag:
                    c *= 1.41
                nd = d + c
                if nd < dist.get((nx, ny), 1e18):
                    dist[(nx, ny)] = nd
                    heapq.heappush(pq, (nd, nx, ny))
        return dist


def link_costs(grid, a, b, km):
    costs = [255] * 8
    for m in (NOMOVE, FOOT, WHEELED, TRACKED):
        d = grid.dijkstra(a, m, int(km * 2 + 8), target=b).get(b)
        if d is not None:
            costs[m] = int(min(254, max(1, round(d))))
    if costs[NOMOVE] < 255:
        for m in (FOOT, WHEELED, TRACKED):
            if costs[m] == 255:
                costs[m] = 254
    air = int(min(254, max(1, round(km))))
    costs[4] = costs[5] = air
    return costs


def angle(a, b):
    return math.atan2(b[1] - a[1], b[0] - a[0])


def plan_links(objs, grid, radius, join, log):
    """-> list of (i, j) object indices to link (new links only)."""
    grid.mark_bridges(objs)
    pos = [(o["x"], o["y"]) for o in objs]
    ok = [bool(grid.land[y, x] or grid.bridge[y, x]) for x, y in pos]
    by_id = {tuple(o["id"]): i for i, o in enumerate(objs)}
    adj = [set() for _ in objs]
    for i, o in enumerate(objs):
        for l in o["links"]:
            j = by_id.get(tuple(l["id"]))
            if j is not None:
                adj[i].add(j)
                adj[j].add(i)
    cell_obj = {}
    for i, p in enumerate(pos):
        cell_obj.setdefault(p, []).append(i)

    def reach(i, rad):
        """{j: cost} for objectives j reachable from i over land within rad."""
        d = grid.dijkstra(pos[i], TRACKED, rad)
        out = {}
        for c, cost in d.items():
            for j in cell_obj.get(c, ()):
                if j != i and ok[j]:
                    out[j] = cost
        return out

    new = []

    def add(i, j):
        adj[i].add(j)
        adj[j].add(i)
        new.append((i, j))

    isolated_cells = 0
    for i in range(len(objs)):
        if not ok[i]:
            isolated_cells += 1
            continue
        if len(adj[i]) >= 2:
            continue
        cand = sorted(reach(i, radius).items(), key=lambda kv: kv[1])
        for j, cost in cand:
            if len(adj[i]) >= 2:
                break
            if j in adj[i]:
                continue
            # spread the neighbours round the compass: skip one in the same direction as an existing link
            ang = angle(pos[i], pos[j])
            if any(abs((angle(pos[i], pos[k]) - ang + math.pi) % (2 * math.pi) - math.pi) < math.radians(35)
                   for k in adj[i]) and len(cand) > 4:
                continue
            add(i, j)
        if len(adj[i]) < 2:                                # nothing apart enough: take the cheapest left
            for j, cost in cand:
                if len(adj[i]) >= 2:
                    break
                if j not in adj[i]:
                    add(i, j)

    # join the pieces
    for _ in range(40):
        comp = [-1] * len(objs)
        comps = []
        for s in range(len(objs)):
            if comp[s] >= 0 or not ok[s] or not adj[s]:
                continue
            st, c = [s], [s]
            comp[s] = len(comps)
            while st:
                k = st.pop()
                for j in adj[k]:
                    if comp[j] < 0:
                        comp[j] = len(comps)
                        st.append(j)
                        c.append(j)
            comps.append(c)
        comps.sort(key=len, reverse=True)
        for n, c in enumerate(comps):
            for k in c:
                comp[k] = n
        joined = 0
        for n in range(1, len(comps)):
            best = None
            for i in comps[n]:
                for j, cost in reach(i, join).items():
                    if comp[j] != n and comp[j] >= 0 and (best is None or cost < best[0]):
                        best = (cost, i, j)
            if best:
                add(best[1], best[2])
                joined += 1
        if not joined:
            break
    sizes = sorted((len(c) for c in comps), reverse=True)
    km = sorted(math.hypot(pos[i][0] - pos[j][0], pos[i][1] - pos[j][1]) for i, j in new)
    if km:
        log("   new link length km: median %.1f, p90 %.1f, max %.0f" % (km[len(km) // 2], km[int(len(km) * 0.9)], km[-1]))
    log("   %d links added; %d objectives not on ground a column can use; graph pieces after: %s"
        % (len(new), isolated_cells, sizes[:8]))
    return new


def splice(cam, objs, new, grid, log):
    """Rebuild the objective stream with `new` links added; returns the new raw bytes."""
    raw = cam.objectives_raw
    ver = cam.version
    extra = {}
    for i, j in new:
        a, b = (objs[i]["x"], objs[i]["y"]), (objs[j]["x"], objs[j]["y"])
        km = math.hypot(a[0] - b[0], a[1] - b[1])
        costs = link_costs(grid, a, b, km)
        extra.setdefault(i, []).append(bytes(costs) + _vuid(objs[j]["id"]))
        extra.setdefault(j, []).append(bytes(costs) + _vuid(objs[i]["id"]))
    out = bytearray()
    for i, o in enumerate(objs):
        s, e = o["_span"]
        rec = raw[s:e]
        if i in extra:
            tail = (1 + (objmod.RADAR_RANGE_SIZE if "radarRange" in o else 0)) if ver >= 20 else 0
            n = len(o["links"])
            end = len(rec) - tail
            start = end - (1 + n * (8 + VUID_BYTES))
            assert rec[start] == n, "link count byte not where expected (objective %d)" % i
            rec = (rec[:start] + bytes([n + len(extra[i])]) + rec[start + 1:end]
                   + b"".join(extra[i]) + rec[end:])
        out += rec
    return bytes(out)


def _vuid(v):
    import struct
    return struct.pack("<II", v[0], v[1])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gamedir", default=r"C:\FreeFalcon6")
    ap.add_argument("--tdf", default=r"Theaters\Taiwan\taiwan.tdf")
    ap.add_argument("--thr", required=True)
    ap.add_argument("--files", nargs="*", default=["save0.cam", "save1.cam", "save2.cam"])
    ap.add_argument("--radius", type=int, default=25, help="km searched for the two nearest neighbours")
    ap.add_argument("--join", type=int, default=60, help="km searched to join separate pieces")
    ap.add_argument("--write", action="store_true")
    a = ap.parse_args()
    log = lambda s: print(s, flush=True)

    ws = TheaterWorkspace(a.gamedir, a.tdf)
    plans = []
    for name in a.files:
        cam = ws.objectives(name)
        objs = cam.objectives
        grid = Grid(a.thr)
        links0 = sum(len(o["links"]) for o in objs)
        log("%s: %d objectives, %d links (%.2f/objective), %d with none"
            % (name, len(objs), links0, links0 / len(objs), sum(1 for o in objs if not o["links"])))
        new = plan_links(objs, grid, a.radius, a.join, log)
        raw = splice(cam, objs, new, grid, log)
        plans.append((name, cam, objs, new, raw))
        log("   -> %.2f links/objective" % ((links0 + 2 * len(new)) / len(objs)))
    if not a.write:
        log("dry run (--write to write)")
        return

    class NoBackup:
        def keep(self, path):
            return None

    for name, cam, objs, new, raw in plans:
        path = os.path.join(ws.campaign_dir, name)
        bak = path + ".bak-prelinks"
        if not os.path.exists(bak):
            shutil.copy2(path, bak)
        cam.objectives_raw = raw
        cam.objectives_dirty = True
        cam.dirty = True
    for w in ws.save(NoBackup()):
        log("  wrote %s" % w)
    ws2 = TheaterWorkspace(a.gamedir, a.tdf)
    for name, _cam, objs, new, _raw in plans:
        back = ws2.objectives(name).objectives
        n = sum(len(o["links"]) for o in back)
        want = sum(len(o["links"]) for o in objs) + 2 * len(new)
        log("read back %s: %d objectives, %d links (expected %d)%s" % (name, len(back), n, want, "" if n == want else "  MISMATCH"))


if __name__ == "__main__":
    main()
