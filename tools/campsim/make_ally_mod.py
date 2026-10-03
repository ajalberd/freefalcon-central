"""Stage China's (and optionally Russia's) forces forward so they can reach the war.

    python make_ally_mod.py <variant> <out-dir> [--src save0.cam]

Variants:  prc      PRC battalions + squadrons
           prc+cis  PRC and CIS

In save0 the PRC's 43 battalions start ~340 km behind the front (mean grid y 822, front ~490)
and campsim shows they never come south after China joins: the ground AI rejects any objective
whose path cost exceeds ObjGroundPathMaxCost. This moves every battalion to a DPRK-held
objective 100-170 km behind the front (spread west to east) and rehomes every squadron to a DPRK
airbase in roughly the same band. Units are not touched otherwise: they stay PRC/CIS (neutral)
until the .tri script brings their country into the war.
"""
import argparse
import os
import shutil
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "campaign-editor"))
from ffcamp import entities, workspace  # noqa: E402

GAME = r"C:\FreeFalcon6"
TDF = "terrdata/theaterdefinition/korea.tdf"
FRONT_Y = 490
BAND = (FRONT_Y + 100, FRONT_Y + 170)          # where battalions go
AIR_BAND = (FRONT_Y + 60, FRONT_Y + 230)       # airbases for squadrons
GROUND_TYPES = ("Town", "City", "Village", "Road", "Intersection", "Army base", "Depot")
TEAMS = {"prc": (5,), "prc+cis": (5, 4)}


def build(variant, out_dir, src="save0.cam"):
    S = workspace.Session(GAME)
    ws = S.workspace(TDF)
    cam = ws.units(src)
    objs = ws.objectives(src).objectives or []
    base = entities.base_offsets(cam.version)
    raw = bytearray(cam.units_raw)

    spots = sorted((o for o in objs if o["owner"] == 6 and BAND[0] <= o["y"] <= BAND[1]
                    and any(t in o["typeName"] for t in GROUND_TYPES)), key=lambda o: o["x"])
    bases = sorted((o for o in objs if o["owner"] == 6 and o["typeName"] in ("Airbase", "Airstrip")
                    and AIR_BAND[0] <= o["y"] <= AIR_BAND[1]), key=lambda o: o["x"])
    if not spots or not bases:
        raise SystemExit("no DPRK objectives/airbases in the band (%d spots, %d bases)" % (len(spots), len(bases)))

    moved_bn = moved_sq = 0
    bns = [u for u in cam.units if u["owner"] in TEAMS[variant] and u["kind"] == "battalion"]
    sqs = [u for u in cam.units if u["owner"] in TEAMS[variant] and u["kind"] == "squadron"]
    bns.sort(key=lambda u: u["x"])
    for i, u in enumerate(bns):
        # spread evenly across the band's objectives, west to east, like the units were
        o = spots[int(i * len(spots) / max(1, len(bns)))]
        s0 = u["_span"][0]
        struct.pack_into("<h", raw, s0 + base["x"], o["x"])
        struct.pack_into("<h", raw, s0 + base["y"], o["y"])
        moved_bn += 1
    for i, u in enumerate(sqs):
        ab = bases[i % len(bases)]
        s0, at = u["_span"][0], u["_at"]
        struct.pack_into("<h", raw, s0 + base["x"], ab["x"])
        struct.pack_into("<h", raw, s0 + base["y"], ab["y"])
        struct.pack_into("<hh", raw, at["destX"], ab["x"], ab["y"])
        if "airbaseId" in at:
            struct.pack_into("<II", raw, at["airbaseId"], ab["id"][0], ab["id"][1])
        if "hotSpot" in at:
            struct.pack_into("<II", raw, at["hotSpot"], ab["id"][0], ab["id"][1])
        moved_sq += 1

    stream = bytes(raw)
    cam.units_raw = stream
    cam.units = entities.walk_units(stream, cam.version, ws.db.class_rows())
    cam.members[cam._member_name("uni")] = entities.encode_units(stream, len(cam.units))
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, src)
    cam.path = out
    cam.save()
    tri = os.path.splitext(os.path.join(ws.campaign_dir, src))[0] + ".tri"
    if os.path.isfile(tri):
        shutil.copy(tri, os.path.splitext(out)[0] + ".tri")
    print("%s: moved %d battalions onto %d objectives (y %d-%d), rehomed %d squadrons to %d airbases -> %s" % (
        variant, moved_bn, len(spots), BAND[0], BAND[1], moved_sq, len(bases), out))
    return out


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("variant", choices=sorted(TEAMS))
    ap.add_argument("out_dir")
    ap.add_argument("--src", default="save0.cam")
    a = ap.parse_args()
    build(a.variant, a.out_dir, a.src)
