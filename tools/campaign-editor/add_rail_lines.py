"""Add named OSM railway lines to an existing rail.json, keeping the lines already there.

    python add_rail_lines.py --out <dir>                      # the default set (below), to a test folder
    python add_rail_lines.py --out <dir> --line "OSM name=Label" --area "Label=s,w,n,e"
    python add_rail_lines.py --write                          # into the theater's terrain folder

osm_rail.py built Korea's rail.json one named line at a time (`--line`, English names only), so
nothing beyond the nine Korean lines was ever fetched: no Chinese or Russian track, though the
theater covers Manchuria and the Russian corner. This adds lines without refetching or refitting
the old ones:

  * --line "NAME=Label": every main-line way whose name:en or name is NAME (Chinese lines often
    have only a Chinese name), shown as Label (rail.txt is ASCII).
  * --area "Label=s,w,n,e": every main-line way in that lat/lon box with no name of its own
    (the Russian track at Khasan is unnamed in OSM).

New lines are projected with the affine airbase fit, not the stored cubic one: the cubic fit is
tuned to Korea's coast and is 14 km out at Shenyang (affine 4 km; both under 3 km at the Yalu
and Manpo bridges). A new route end within JOIN_KM of an existing route end snaps onto it, so the
border bridges join the networks. The routes, airfield detours and rail.txt are then rebuilt for
all lines (keep_on_land), as osm_rail.py --resnap does.

--write backs up rail.json and rail.txt as *.bak-pre-addlines (once). Without --write and --out
it only reports.
"""

import argparse
import hashlib
import json
import math
import os
import shutil
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from ffcamp import rail  # noqa: E402
from ffcamp.workspace import TheaterWorkspace  # noqa: E402
import osm_rail  # noqa: E402

# Cross-border lines for Korea: China at Sinuiju-Dandong and Manpo-Ji'an, Russia at
# Tumangang-Khasan, and the North Korean lines that reach those bridges.
DEFAULT_LINES = [
    "Shenyang-Dandong Railway=Shendan Line (Shenyang-Dandong)",
    "Shenyang-Jilin Railway=Shenji Line (Shenyang-Meihekou)",
    "\u6885\u96c6\u7ebf=Meiji Line (Meihekou-Ji'an)",
    "Hambuk Line=Hambuk Line",
    "Pukbunaeryuk Line=Pukbunaeryuk Line",
]
DEFAULT_AREAS = ["Khasan Line=42.25,130.45,43.10,131.60"]

MIRRORS = ["https://overpass-api.de/api/interpreter",
           "https://overpass.kumi.systems/api/interpreter",
           "https://overpass.private.coffee/api/interpreter"]
CACHE = os.path.join(osm_rail.CACHE, "addlines")
SNAP_KM = 8.0   # a new line end this close to an old route end is the same station: the Shendan Line
                # stops 7.3 km short of Sinuiju (Dandong sits on the Yalu estuary, which the land check
                # treats as sea)
MAIN = '["railway"~"^(rail|narrow_gauge)$"]["service"!~"."]'


def overpass(query, log=print, tries=9):
    data = urllib.parse.urlencode({"data": query}).encode()
    for attempt in range(tries):
        url = MIRRORS[attempt % len(MIRRORS)]
        try:
            req = urllib.request.Request(url, data=data, headers={
                "User-Agent": "FreeFalcon campaign editor (rail overlay)"})
            with urllib.request.urlopen(req, timeout=600) as r:
                doc = json.loads(r.read().decode("utf-8"))
            remark = (doc.get("remark") or "").lower()
            if "error" in remark or "timed out" in remark:
                raise ValueError(doc["remark"])
            return doc
        except (urllib.error.URLError, ValueError, OSError) as exc:
            log("  %s: %s" % (url.split("/")[2], exc))
            time.sleep(60 if "429" in str(exc) else 15)
    raise SystemExit("Overpass: gave up after %d tries" % tries)


def cached(key, query, refresh, log):
    os.makedirs(CACHE, exist_ok=True)
    slug = "".join(c if c.isalnum() else "_" for c in key)[:80]
    path = os.path.join(CACHE, "%s_%s.json" % (slug, hashlib.sha1(query.encode("utf-8")).hexdigest()[:10]))
    if refresh or not os.path.isfile(path):
        log("fetching %s" % key)
        doc = overpass(query, log)
        with open(path + ".part", "w", encoding="utf-8") as f:
            json.dump(doc, f)
        os.replace(path + ".part", path)
    return path


def fetch_named(name, bbox, refresh, log):
    b = "%.4f,%.4f,%.4f,%.4f" % bbox
    q = ('[out:json][timeout:600];(way%s["name:en"="%s"](%s);way%s["name"="%s"](%s););out tags geom;'
         % (MAIN, name, b, MAIN, name, b))
    return rail.load_ways(cached("line_" + name, q, refresh, log))


def fetch_area(box, refresh, log):
    b = "%.4f,%.4f,%.4f,%.4f" % box
    q = "[out:json][timeout:600];way%s(%s);out tags geom;" % (MAIN, b)
    ways = rail.load_ways(cached("area_" + b, q, refresh, log))
    return [(t, p) for t, p in ways if not (t.get("name:en") or t.get("name"))]


def relabel(ways, label):
    out = []
    for tags, pts in ways:
        t = dict(tags)
        t["name:en"] = label
        out.append((t, pts))
    return out


def route_ends(doc):
    return [(r["name"], tuple(r["pts"][0]), tuple(r["pts"][-1])) for r in doc.get("routes", ())]


def snap_ends(lines, old_ends, log):
    """Move each new line's end points onto an old route end within SNAP_KM (the shared station)."""
    targets = [p for _n, a, b in old_ends for p in (a, b)]
    moved = 0
    for ln in lines:
        for k in (0, -1):
            x, y = ln["pts"][k]
            best = min(targets, key=lambda t: math.hypot(t[0] - x, t[1] - y)) if targets else None
            if best and math.hypot(best[0] - x, best[1] - y) <= SNAP_KM:
                ln["pts"][k] = [best[0], best[1]]
                moved += 1
    log("snapped %d new line ends onto existing route ends (within %.0f km)" % (moved, SNAP_KM))


def close_gaps(lines, log):
    """Join pieces of the same new line whose ends are within SNAP_KM but not touching.

    OSM's Hambuk Line has a 6 km hole near Chongjin; the route builder joins piece ends only
    within rail.JOIN_KM (0.5 km), so the route kept one 145 km piece and lost the link to the
    Pyongra Line and to Tumangang. Each piece end moves onto the nearest end of another piece of
    the same name when nothing of that name already meets it there.
    """
    by = {}
    for ln in lines:
        by.setdefault(ln["name"], []).append(ln)
    joined = 0
    for name, pieces in by.items():
        ends = [(i, k) for i in range(len(pieces)) for k in (0, -1)]
        for i, k in ends:
            x, y = pieces[i]["pts"][k]
            others = [(j, m) for j, m in ends if j != i]
            if not others:
                continue
            dist = lambda e: math.hypot(pieces[e[0]]["pts"][e[1]][0] - x, pieces[e[0]]["pts"][e[1]][1] - y)
            j, m = min(others, key=dist)
            d = dist((j, m))
            if rail.JOIN_KM < d <= SNAP_KM:
                pieces[i]["pts"][k] = list(pieces[j]["pts"][m])
                joined += 1
                log("  %s: closed a %.1f km gap at (%.0f, %.0f)" % (name, d, x, y))
    return joined


def _off_route(piece, route, tol_km=1.0):
    """True if most of the piece is more than tol_km from the route polyline."""
    r = np.asarray(route, float)
    a, b = r[:-1], r[1:]
    d = b - a
    l2 = np.maximum((d ** 2).sum(1), 1e-12)
    far = 0
    for p in piece:
        t = np.clip(((p[0] - a[:, 0]) * d[:, 0] + (p[1] - a[:, 1]) * d[:, 1]) / l2, 0, 1)
        q = a + d * t[:, None]
        far += np.hypot(q[:, 0] - p[0], q[:, 1] - p[1]).min() > tol_km
    return far > len(piece) / 2


def _next_branch(name):
    """"X" -> "X branch", "X branch" -> "X branch 2", "X branch 2" -> "X branch 3"."""
    base, sep, n = name.rpartition(" branch")
    if not sep:
        return name + " branch"
    return "%s branch %d" % (base, int(n) + 1 if n.strip() else 2)


MIN_BRANCH_KM = 10.0


def split_branches(lines, names, log, rounds=3):
    """A route is one track between the two points of a line furthest apart on the map, so a
    branch is dropped: the Hambuk Line's route is Chongjin-Namyang and loses Namyang-Tumangang,
    where the Khasan track joins. New-line pieces off their route become "<name> branch"."""
    for _ in range(rounds):
        routes = {r["name"]: r["pts"] for r in rail.build_routes([ln for ln in lines if ln["name"] in names])}
        moved = 0
        for ln in lines:
            if ln["name"] in names and ln["name"] in routes and _off_route(ln["pts"], routes[ln["name"]]):
                ln["name"] = _next_branch(ln["name"])
                moved += 1
        if not moved:
            return
        names = names | {_next_branch(n) for n in names}
        log("  %d piece(s) off their route became branch routes" % moved)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--gamedir", default=r"C:\FreeFalcon6")
    ap.add_argument("--theater", default=r"terrdata\theaterdefinition\korea.tdf")
    ap.add_argument("--line", action="append", help='"OSM name=Label" (repeat); default: the Korea border set')
    ap.add_argument("--area", action="append", help='"Label=s,w,n,e" (repeat)')
    ap.add_argument("--out", help="write rail.json + rail.txt into this folder")
    ap.add_argument("--write", action="store_true", help="write into the theater's terrain folder")
    ap.add_argument("--refresh", action="store_true")
    a = ap.parse_args()

    ws = TheaterWorkspace(a.gamedir, a.theater)
    terr = ws.terrain()
    src = os.path.join(terr.dir, rail.FILENAME)
    with open(src, encoding="utf-8") as f:
        doc = json.load(f)
    w = h = terr.size_km

    objs = osm_rail.any_objectives(ws)
    pairs = rail.airbase_pairs(objs, ws.name_table(), rail.AIRBASES["korea"])
    proj, control = rail.fit_airbases(pairs, float(np.mean([p[1] for p in pairs])),
                                      float(np.mean([p[2] for p in pairs])), log=lambda *x: None)
    # Check point north of the border: Shenyang Airbase (Dongta) in the campaign.
    for o in objs:
        if ws.name_table().get(o["nameId"], "").strip() == "Shenyang Airbase":
            sx, sy = proj(41.784, 123.496)
            print("fit check: Shenyang Airbase %.1f km from the campaign's objective"
                  % math.hypot(sx - (o["x"] + 0.5), sy - (o["y"] + 0.5)))

    corners = [proj.inverse(x, y) for x in (0, w) for y in (0, h)]
    bbox = (min(c[0] for c in corners) - 0.2, min(c[1] for c in corners) - 0.2,
            max(c[0] for c in corners) + 0.2, max(c[1] for c in corners) + 0.2)
    old_names = {ln["name"] for ln in doc["lines"]}
    old_ends = route_ends(doc)

    ways = []
    for spec in (a.line or DEFAULT_LINES):
        name, _, label = spec.partition("=")
        label = label or name
        if label in old_names:
            print("%-40s already in rail.json -- skipped" % label)
            continue
        got = fetch_named(name, bbox, a.refresh, print)
        print("%-40s %d ways" % (label, len(got)))
        ways += relabel(got, label)
    for spec in (a.area if a.area is not None else DEFAULT_AREAS):
        label, _, box = spec.partition("=")
        if label in old_names:
            print("%-40s already in rail.json -- skipped" % label)
            continue
        got = fetch_area(tuple(float(v) for v in box.split(",")), a.refresh, print)
        print("%-40s %d unnamed ways in the box" % (label, len(got)))
        ways += relabel(got, label)

    new = rail.build_lines(ways, proj, w, h)
    km = {}
    for ln in new:
        km[ln["name"]] = km.get(ln["name"], 0.0) + rail.line_km(ln)
    for n, v in sorted(km.items()):
        print("  new line %-36s %6.1f km on the map" % (n, v))
    close_gaps(new, print)
    split_branches(new, set(km), print)
    snap_ends(new, old_ends, print)
    doc["lines"] += new
    doc.setdefault("added", []).append({"when": time.strftime("%Y-%m-%dT%H:%M:%S"),
                                        "lines": sorted(km), "projection": "affine airbase fit"})
    rail.keep_on_land(doc, terr, fields=osm_rail.airfields(a.gamedir, terr.dir))
    stubs = [r for r in doc["routes"] if " branch" in r["name"] and r["km"] < MIN_BRANCH_KM]
    if stubs:
        drop = {r["name"] for r in stubs}
        doc["routes"] = [r for r in doc["routes"] if r["name"] not in drop]
        print("dropped %d branch stub(s) under %.0f km: %s" % (len(stubs), MIN_BRANCH_KM, ", ".join(sorted(drop))))
    for r in doc["routes"]:
        print("  route %-38s %6.1f km" % (r["name"], r["km"]))

    out_dir = terr.dir if a.write else a.out
    if not out_dir:
        print("(report only: pass --out <dir> or --write)")
        return
    os.makedirs(out_dir, exist_ok=True)
    if a.write:
        for f in (rail.FILENAME, rail.GAME_FILENAME):
            p = os.path.join(terr.dir, f)
            if os.path.exists(p) and not os.path.exists(p + ".bak-pre-addlines"):
                shutil.copy2(p, p + ".bak-pre-addlines")
    with open(os.path.join(out_dir, rail.FILENAME), "w", encoding="utf-8") as f:
        json.dump(doc, f, separators=(",", ":"))
    rail.write_game_file(doc, os.path.join(out_dir, rail.GAME_FILENAME))
    print("wrote %s" % out_dir)


if __name__ == "__main__":
    main()
