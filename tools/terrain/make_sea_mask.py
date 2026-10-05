"""Build <theater>.SEA: cells the campaign grid (KOREA.THR) calls water but the 3D terrain shows as land.

    python make_sea_mask.py [--thr C:\\FreeFalcon6\\campaign\\SAVE\\KOREA.THR] [--threshold 0.25] [--keep 3]
                            [--out C:\\FreeFalcon6\\campaign\\SAVE\\KOREA.SEA] [--also <dir> ...]

Ship routing (GetMovementCost, g_bNavalSeaMask) treats a marked cell as land. The 1 km campaign grid counts a
fringe of coastal cells as sea that the 3D tiles show as land (cover_compare.py), so ships routed by the grid
alone sailed over peninsulas and moored on airfields in Recon/3D (ship_debug.cam: the 65th Destroyer task force
on the Kalma peninsula at Wonsan, a cell 14% water in 3D).

A cell is marked when the grid says water and under `threshold` of its 3D tile is sea. Cells within `keep` km
of a port or beach objective are never marked, so harbours stay reachable; the engine's port/beach exception
covers the rest. The script refuses to write a mask that cuts a port off from the open sea.

File format (read by LoadTheaterTerrain, campterr.cpp): short X, short Y, then X*Y bytes in the engine's
TheaterCells order (index x * Y + y), 1 = treat as land for ships.
"""
import argparse
import os
import shutil
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "campsim"))
sys.path.insert(0, os.path.join(HERE, "..", "campaign-editor"))
import cover_compare as cc  # noqa: E402


def components(water):
    """4-connected labels of a [y][x] bool grid (iterative flood fill)."""
    lab = np.zeros(water.shape, np.int32)
    h, w = water.shape
    n = 0
    for sy, sx in zip(*np.nonzero(water)):
        if lab[sy, sx]:
            continue
        n += 1
        stack = [(sy, sx)]
        lab[sy, sx] = n
        while stack:
            y, x = stack.pop()
            for yy, xx in ((y + 1, x), (y - 1, x), (y, x + 1), (y, x - 1)):
                if 0 <= yy < h and 0 <= xx < w and water[yy, xx] and not lab[yy, xx]:
                    lab[yy, xx] = n
                    stack.append((yy, xx))
    return lab


def channel(water, mask, sea, start_box):
    """Unmark the grid-water route from start_box to `sea` that crosses the fewest marked cells (0-1 BFS).
    Returns how many cells it reopened."""
    from collections import deque
    h, w = water.shape
    INF = 1 << 30
    dist = np.full(water.shape, INF, np.int32)
    prev = {}
    dq = deque()
    ys, xs = np.nonzero(water[start_box])
    for y, x in zip(ys + start_box[0].start, xs + start_box[1].start):
        dist[y, x] = int(mask[y, x])
        dq.append((y, x))
    goal = None
    while dq:
        y, x = dq.popleft()
        if sea[y, x]:
            goal = (y, x)
            break
        for yy, xx in ((y + 1, x), (y - 1, x), (y, x + 1), (y, x - 1)):
            if 0 <= yy < h and 0 <= xx < w and water[yy, xx]:
                c = dist[y, x] + int(mask[yy, xx])
                if c < dist[yy, xx]:
                    dist[yy, xx] = c
                    prev[(yy, xx)] = (y, x)
                    (dq.append if mask[yy, xx] else dq.appendleft)((yy, xx))
    n = 0
    p = goal
    while p is not None:
        if mask[p]:
            mask[p] = False
            n += 1
        p = prev.get(p)
    return n


def reconnect_pockets(water, mask, min_cells):
    """Water the mask cuts off from the open sea (a bay behind tidal flats, an anchorage) keeps a channel:
    one 0-1 BFS outward from the open sea over grid water (marked cells cost 1), then each cut-off pocket of
    at least min_cells reopens its cheapest route back. Returns (pockets reconnected, cells reopened)."""
    from collections import deque
    lab = components(water & ~mask)
    sizes = np.bincount(lab.ravel())
    sizes[0] = 0
    main_lab = sizes.argmax()
    h, w = water.shape
    INF = 1 << 30
    dist = np.full(water.shape, INF, np.int32)
    prev_y = np.full(water.shape, -1, np.int32)
    prev_x = np.full(water.shape, -1, np.int32)
    dq = deque()
    for y, x in zip(*np.nonzero(lab == main_lab)):
        dist[y, x] = 0
        dq.append((y, x))
    while dq:
        y, x = dq.popleft()
        for yy, xx in ((y + 1, x), (y - 1, x), (y, x + 1), (y, x - 1)):
            if 0 <= yy < h and 0 <= xx < w and water[yy, xx]:
                c = dist[y, x] + int(mask[yy, xx])
                if c < dist[yy, xx]:
                    dist[yy, xx] = c
                    prev_y[yy, xx], prev_x[yy, xx] = y, x
                    (dq.append if mask[yy, xx] else dq.appendleft)((yy, xx))
    pockets = reopened = 0
    for p in range(1, len(sizes)):
        if p == main_lab or sizes[p] < min_cells:
            continue
        ys, xs = np.nonzero(lab == p)
        i = int(np.argmin(dist[ys, xs]))
        y, x = int(ys[i]), int(xs[i])
        if dist[y, x] >= INF:
            continue  # was never connected to the open sea (an inland lake)
        pockets += 1
        while dist[y, x] > 0:
            if mask[y, x]:
                mask[y, x] = False
                reopened += 1
            y, x = int(prev_y[y, x]), int(prev_x[y, x])
    return pockets, reopened


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--thr", default=r"C:\FreeFalcon6\campaign\SAVE\KOREA.THR")
    ap.add_argument("--threshold", type=float, default=0.25)
    ap.add_argument("--keep", type=int, default=3)
    ap.add_argument("--min-pocket", dest="min_pocket", type=int, default=4, help="smallest cut-off pocket kept connected")
    ap.add_argument("--out", default=None)
    ap.add_argument("--also", nargs="*", default=[], help="more campaign SAVE dirs to copy the mask into")
    a = ap.parse_args()
    out = a.out or os.path.splitext(a.thr)[0] + ".SEA"

    import saves  # tools/campsim: objectives of the stock scenario
    cells, mx, my = cc.load_thr(a.thr)          # [y][x]
    frac, _sea = cc.load_3d(cc.ts.default_theater())
    water = (cells & 15) == 0
    mask = water & (frac < a.threshold)

    objs = saves.load("ff6")["objs"]
    harbours = [o for o in objs if o["typeName"] in ("Port", "Beach")]
    k = a.keep
    for o in harbours:
        mask[max(0, o["y"] - k):o["y"] + k + 1, max(0, o["x"] - k):o["x"] + k + 1] = False

    # no stretch of water a ship can be in may be sealed off (ship_debug.cam: ROK cargo at (403,412) in a pocket
    # of Incheon bay could not reach the port 26 km away -- 149 failed routes, each a straight patrol over land)
    pk, ro = reconnect_pockets(water, mask, a.min_pocket)
    print("reconnected %d cut-off pockets (>= %d cells) with %d reopened cells" % (pk, a.min_pocket, ro))

    # every port that reached the main sea before must still reach it; where the mask cuts one off (a harbour
    # behind tidal flats the 3D tiles draw as land), reopen the grid-water route through the fewest marked cells
    before = components(water)
    main_before = np.bincount(before[before > 0]).argmax()
    ports = [o for o in objs if o["typeName"] == "Port"]
    box = lambda o: (slice(max(0, o["y"] - k), o["y"] + k + 1), slice(max(0, o["x"] - k), o["x"] + k + 1))
    reopened = []
    for _ in range(len(ports)):
        after = components(water & ~mask)
        main_after = np.bincount(after[after > 0]).argmax()
        cut = [o for o in ports if (before[box(o)] == main_before).any() and not (after[box(o)] == main_after).any()]
        if not cut:
            break
        o = cut[0]
        n = channel(water, mask, after == main_after, box(o))
        reopened.append((o["campId"], o["x"], o["y"], n))
    after = components(water & ~mask)
    main_after = np.bincount(after[after > 0]).argmax()
    lost = [(o["campId"], o["x"], o["y"]) for o in ports
            if (before[box(o)] == main_before).any() and not (after[box(o)] == main_after).any()]
    for r in reopened:
        print("port %d at (%d,%d): reopened a %d-cell channel to the open sea" % r)
    print("grid water %d cells; marked as land for ships %d (3D water < %.2f, %d km kept around %d ports/beaches)"
          % (water.sum(), mask.sum(), a.threshold, k, len(harbours)))
    if lost:
        raise SystemExit("refusing: these ports would be cut off from the open sea: %s" % lost)
    print("all ports that reached the open sea still do")

    blob = struct.pack("<hh", mx, my) + mask.T.astype(np.uint8).tobytes()  # [y][x] -> engine [x][y]
    with open(out, "wb") as f:
        f.write(blob)
    print("wrote", out, len(blob), "bytes")
    for d in a.also:
        shutil.copy(out, os.path.join(d, os.path.basename(out)))
        print("copied to", d)


if __name__ == "__main__":
    main()
