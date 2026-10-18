"""supply_reach.py [--cities] [--pool] <run> [<run> ...]: battalion supply against distance to the nearest friendly supply source, for Blue
(teams 1-3), the North (6) and China (5), every 24 h of a campsim timeline. Answers: does supply depend on where a unit is?

Sources: depots, army bases, ports (the logistics hubs); --cities adds cities, which the engine also counts (IsSupplySource: city, port,
depot, army base not on the front / second line -- the front-line exclusion is not modelled here). --pool prints one table pooled over all
runs and days 1-5 instead of one per run. Objective ownership is the meta line's start owner plus each frame's "o" rows (changes only)."""
import json
import math
import sys

HUBS = {3, 10, 19}   # TYPE_ARMYBASE, TYPE_DEPOT, TYPE_PORT
CITY = 8             # TYPE_CITY
BINS = (0, 25, 50, 100, 150, 250, 9999)
SIDES = {"Blue": (1, 2, 3), "North": (6,), "China": (5,)}


def rows(run, sources):
    """yield (hour, side, [(km to nearest own source, supply)...]) every 24 h"""
    lines = open(run if run.endswith(".jsonl") else run + ".jsonl").readlines()
    meta = json.loads(lines[0])
    objs = {o[0]: o for o in meta["objs"]}
    owner = {o[0]: o[4] for o in meta["objs"]}
    nexth = 0
    for l in lines[1:]:
        if not l.startswith('{"t"'):
            continue
        f = json.loads(l)
        for row in f.get("o") or []:
            owner[row[0]] = row[1]
        h = f["t"] / 60
        if h < nexth:
            continue
        nexth += 24
        for sd, teams in SIDES.items():
            mine = [(objs[i][1], objs[i][2]) for i in owner if owner[i] in teams and objs[i][3] in sources]
            bns = [u for u in f["u"] if u[2] in teams and u[1] == 0]
            if bns and mine:
                yield h, sd, [(min(math.hypot(u[3] - x, u[4] - y) for x, y in mine), u[11]) for u in bns]


def table(pairs):
    cells = [[] for _ in range(len(BINS) - 1)]
    for d, s in pairs:
        for i in range(len(BINS) - 1):
            if BINS[i] <= d < BINS[i + 1]:
                cells[i].append(s)
    n = len(pairs)
    return " | ".join(("%3.0f%% of bns, supply %3.0f" % (100.0 * len(c) / n, sum(c) / len(c))) if c else "      -              " for c in cells), \
        100.0 * sum(1 for d, _ in pairs if d > 100) / n, 100.0 * sum(1 for _, s in pairs if s < 50) / n


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    sources = HUBS | ({CITY} if "--cities" in sys.argv else set())
    head = "km to nearest own source: " + " | ".join("%d-%d km" % (BINS[i], BINS[i + 1]) for i in range(len(BINS) - 1))
    if "--pool" in sys.argv:
        pooled = {}
        for run in args:
            for h, sd, pairs in rows(run, sources):
                if 24 <= h <= 120:
                    pooled.setdefault(sd, []).extend(pairs)
        print("%d runs, days 1-5 pooled, sources: %s" % (len(args), "hubs + cities" if CITY in sources else "hubs (depots, army bases, ports)"))
        print("  side  | " + head + " | >100 km | supply <50")
        for sd, pairs in pooled.items():
            t, far, low = table(pairs)
            print("  %-5s | %s | %4.1f%% | %4.1f%%" % (sd, t, far, low))
        return
    for run in args:
        print(run)
        print("  hour side  | " + head + " | >100 km | supply <50")
        for h, sd, pairs in rows(run, sources):
            t, far, low = table(pairs)
            print("  %4d %-5s | %s | %4.1f%% | %4.1f%%" % (h, sd, t, far, low))


if __name__ == "__main__":
    main()
