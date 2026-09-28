"""Build a theater's railway lines from OpenStreetMap.

Downloads rail and coastline from the Overpass API (cached, tiled), fits
lat/lon onto the campaign grid against the airbases and the terrain's own
shoreline, and writes rail.json into the theater's terrain folder, where the
campaign editor's Rail overlay reads it (and, later, the game will).

    python osm_rail.py
    python osm_rail.py --line "Gyeongbu Line"      # one line, to try it out
    python osm_rail.py --line "Honam Line" --line "Pyongui Line"   # several
    python osm_rail.py --gamedir C:\\FreeFalcon6 --theater terrdata\\theaterdefinition\\korea.tdf

See ffcamp/rail.py for how the fit works. OSM data is ODbL; the file keeps
the attribution line.
"""

import argparse
import glob
import hashlib
import json
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from ffcamp import rail  # noqa: E402
from ffcamp.workspace import TheaterWorkspace  # noqa: E402

CACHE = os.path.join(tempfile.gettempdir(), "ffcamp-rail")


def debug_path(terrain_dir):
    """Where the editor's fit overlay data lives (not in the game folder)."""
    tag = hashlib.sha1(os.path.abspath(terrain_dir).lower().encode()).hexdigest()
    return os.path.join(CACHE, "debug-%s.json" % tag[:12])


def any_objectives(ws):
    """Objectives and place names from the first campaign file that has them."""
    for path in sorted(glob.glob(os.path.join(ws.campaign_dir, "*.cam"))):
        cam = ws.objectives(os.path.basename(path))
        if cam.objectives:
            return cam.objectives
    raise SystemExit("no campaign in %s has objectives" % ws.campaign_dir)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--gamedir", default=r"C:\FreeFalcon6")
    ap.add_argument("--theater", default=r"terrdata\theaterdefinition\korea.tdf",
                    help="the theater's .tdf, relative to the game folder")
    ap.add_argument("--airbases", default="korea",
                    choices=sorted(rail.AIRBASES),
                    help="which table of real airbase positions to fit to")
    ap.add_argument("--refresh", action="store_true",
                    help="download again instead of using the cache")
    ap.add_argument("--line", metavar="NAME", action="append",
                    help='only this OSM line (its name:en), e.g. "Gyeongbu Line";'
                         ' repeat for several')
    ap.add_argument("--out", help="write here instead of <terrain>/rail.json")
    ap.add_argument("--resnap", action="store_true",
                    help="no download: take the rail.json already in the terrain "
                         "folder, pull it off open sea, rebuild the routes and "
                         "rail.txt")
    args = ap.parse_args()

    ws = TheaterWorkspace(args.gamedir, args.theater)
    terr = ws.terrain()
    if terr is None:
        raise SystemExit("no renderable terrain for %s (numpy missing?)"
                         % args.theater)
    out = args.out or os.path.join(terr.dir, rail.FILENAME)

    if args.resnap:
        src = os.path.join(terr.dir, rail.FILENAME)
        with open(src, encoding="utf-8") as f:
            doc = json.load(f)
        rail.keep_on_land(doc, terr)
        debug = None
    else:
        doc, debug = rail.build_theater(
            terr, any_objectives(ws), ws.name_table(),
            rail.AIRBASES[args.airbases], CACHE, refresh=args.refresh,
            line=args.line)
        doc["theater"] = args.theater

    with open(out, "w", encoding="utf-8") as f:
        json.dump(doc, f, separators=(",", ":"))
    if debug is not None:
        dbg = debug_path(terr.dir)
        os.makedirs(os.path.dirname(dbg), exist_ok=True)
        with open(dbg, "w", encoding="utf-8") as f:
            json.dump(debug, f, separators=(",", ":"))
    game = os.path.join(os.path.dirname(out), rail.GAME_FILENAME)
    rail.write_game_file(doc, game)
    for r in doc["routes"]:
        print("  route %-28s %6.1f km" % (r["name"], r["km"]))
    print("wrote %s (%.1f MB) and %s" % (out, os.path.getsize(out) / 1e6,
                                         rail.GAME_FILENAME))


if __name__ == "__main__":
    main()
