"""Supply hubs and production sources, FreeFalcon 6 vs vanilla Falcon 4.0 vs BMS 4.37 (Korea, campaign start).

    python supply_hubs.py

Counts the objectives that make or distribute supply (the types ProduceSupplies and IsSupplySource look at) per side, with the
class DataRate that scales their output.  FF6 / vanilla objectives decode with the campaign editor's ffcamp; BMS's objective
records are longer than ffcamp knows, so they are anchored the way saves._decode_lite anchors units (type, VU_ID, type) and only
the CampBase head is read (position, owner), plus the per-class XML for the DataRate.
"""
import collections
import os
import struct
import sys
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import saves  # noqa: E402
from ffcamp import campdb, campfile, entities, lzss, objectives, records  # noqa: E402

# classtbl.h objective types
TYPES = {1: "Airbase", 2: "Airstrip", 3: "Army base", 6: "Bridge", 8: "City", 10: "Depot", 11: "Factory", 17: "Nuclear plant",
         19: "Port", 20: "Power plant", 21: "Radar", 25: "Refinery", 15: "Intersection", 26: "Road", 24: "Railroad"}
PRODUCERS = {"Factory": "supply+repl", "Army base": "supply+repl", "Depot": "supply+repl", "Port": "supply+repl", "Refinery": "fuel"}
SOURCES = ("City", "Port", "Depot", "Army base")           # IsSupplySource, when not front line
POWER = ("Power plant", "Nuclear plant")
SIDES = {"north": (4, 5, 6), "south": (1, 2, 3, 7)}


def ocd_rates_bin(tables):
    t = campdb.Table.load(records.OBJECTIVE, os.path.join(tables, "FALCON4.OCD")).rows
    return {i: r["DataRate"] for i, r in enumerate(t)}


def ocd_rates_xml(tables):
    out = {}
    root = os.path.join(tables, "ObjectiveRelatedData")
    for d in os.listdir(root):
        p = os.path.join(root, d, d.replace("OCD", "OCD") + ".XML")
        if d.startswith("OCD_") and os.path.exists(p):
            el = ET.parse(p).getroot().find("OCD")
            out[int(el.get("Num"))] = int(el.findtext("DataRate", "0"))
    return out


def bms_objectives(cam, rows):
    """[(classIndex, owner)] for every objective record in a BMS .obj member."""
    sec = cam.member("obj")
    s = entities.Stream(sec, 4)
    s.i16()
    size = s.i32()
    raw = lzss.expand(sec[s.pos:], size)
    ok = {i for i, r in enumerate(rows) if r["classInfo_"][1] == 4}      # CLASS_OBJECTIVE
    out = []
    i = 0
    while i < len(raw) - 40:
        t = struct.unpack_from("<H", raw, i)[0]
        idx = t - entities.VU_LAST_ENTITY_TYPE
        if (idx in ok and struct.unpack_from("<H", raw, i + 10)[0] == t and struct.unpack_from("<I", raw, i + 6)[0] == 0
                and 1 <= struct.unpack_from("<I", raw, i + 2)[0] < 60000):
            st = entities.Stream(raw, i)
            st.i16()
            base = entities._camp_base(st, 108)
            if 0 <= base["owner"] < 8:
                out.append((idx, base["owner"]))
                i += 60
                continue
        i += 1
    return out


def census(key):
    src = saves.SOURCES[key]
    cam = campfile.CampaignFile.load(src["cam"])
    rows, _ = saves.load_tables(src)
    if key == "bms":
        objs = bms_objectives(cam, rows)
        rate_of = ocd_rates_xml(src["tables"])
        pairs = [(ci, ow) for ci, ow in objs]
        ocd_ptr = lambda ci: rows[ci]["dataPtr"]
    else:
        objs, _ = objectives.decode_objectives(cam.member("obj"), cam.version, rows)
        rate_of = ocd_rates_bin(src["tables"])
        pairs = [(o["classIndex"], o["owner"]) for o in objs]
        ocd_ptr = lambda ci: rows[ci]["dataPtr"]
    cnt = collections.defaultdict(lambda: collections.defaultdict(lambda: [0, 0]))   # type -> side -> [count, rate sum]
    for ci, owner in pairs:
        t = TYPES.get(rows[ci]["classInfo_"][2])
        if not t:
            continue
        side = next((s for s, tm in SIDES.items() if owner in tm), None)
        if not side:
            continue
        c = cnt[t][side]
        c[0] += 1
        c[1] += rate_of.get(ocd_ptr(ci), 0)
    return len(pairs), cnt


def main():
    res = {}
    for key in ("ff6", "vanilla", "bms"):
        try:
            res[key] = census(key)
        except Exception as e:                                    # a missing install is not fatal
            print("%s: %s %s" % (key, type(e).__name__, str(e)[:100]))
    keys = [k for k in ("ff6", "vanilla", "bms") if k in res]
    print("Objectives per side at campaign start: count (sum of class DataRate)\n")
    print("%-14s %-6s | %s" % ("type", "side", " | ".join("%-18s" % saves.SOURCES[k]["label"] for k in keys)))
    for t in ("Factory", "Army base", "Depot", "Port", "Refinery", "Power plant", "Nuclear plant", "City"):
        for side in ("north", "south"):
            cells = []
            for k in keys:
                c = res[k][1].get(t, {}).get(side, [0, 0])
                cells.append("%4d (%6d)       " % (c[0], c[1]))
            print("%-14s %-6s | %s" % (t, side, " | ".join(cells)))
    print()
    for side in ("north", "south"):
        for label, types in (("producers (factory, army base, depot, port)", ("Factory", "Army base", "Depot", "Port")),
                             ("supply sources (city, port, depot, army base)", SOURCES)):
            cells = []
            for k in keys:
                n = sum(res[k][1].get(t, {}).get(side, [0, 0])[0] for t in types)
                r = sum(res[k][1].get(t, {}).get(side, [0, 0])[1] for t in types)
                cells.append("%4d (%6d)       " % (n, r))
            print("%-6s %-46s | %s" % (side, label, " | ".join(cells)))


if __name__ == "__main__":
    main()
