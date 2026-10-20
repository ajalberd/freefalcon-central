"""Build campsim save chddf16.cam: chdd plus two US F-16 squadrons that arrive as reinforcements.

    python make_f16_wave.py [--level 12] [--count 2]

Each is a copy of the US F-16CG squadron (same aircraft, stores and pilots), on Blue's reinforcement clock at --level
hours. With ReinforceHold "2:12" (Blue's reinforcements wait for Russia, chn_appr2 event 12) that is --level hours
after Russia joins; with ReinforceForwardTypes "F-16" they are re-based on arrival to the most forward airbase Blue
holds. Writes C:/FreeFalcon6/campaign/SAVE/chddf16.cam (a new file; save0 and chdd are not touched).
"""
import argparse
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "campaign-editor"))
from ffcamp import entities, workspace  # noqa: E402

GAME = r"C:\FreeFalcon6"
TDF = r"terrdata\theaterdefinition\korea.tdf"
U_INACTIVE = 0x20000
US = 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--level", type=int, default=12, help="reinforcement level (hours on Blue's clock)")
    ap.add_argument("--count", type=int, default=2)
    a = ap.parse_args()

    S = workspace.Session(GAME)
    ws = S.workspace(TDF)
    cam = ws.units("chdd.cam")
    objs = ws.objectives("chdd.cam").objectives or []
    rows, ut, vt = ws.db.class_rows(), ws.db.table("unit"), ws.db.table("vehicle")

    def aircraft(u):
        v = ut.rows[rows[u["classIndex"]]["dataPtr"]]["VehicleType"][0]
        return vt.rows[rows[v]["dataPtr"]]["Name"]

    donor = next(u for u in cam.units if u["kind"] == "squadron" and u["owner"] == US and aircraft(u) == "F-16CG")
    base = entities.base_offsets(cam.version)
    raw = cam.units_raw
    used_vu = {u["id"][0] for u in cam.units}
    used_camp = {u["campId"] for u in cam.units} | {o["campId"] for o in objs}
    added = []

    for k in range(a.count):
        vu = entities.FIRST_NON_VOLATILE_ID
        while vu in used_vu:
            vu += 1
        c = 1
        while c in used_camp:
            c += 1
        used_vu.add(vu)
        used_camp.add(c)

        s0, s1 = donor["_span"]
        at = donor["_at"]
        rel = lambda p: p - s0  # noqa: E731
        rec = bytearray(raw[s0:s1])
        struct.pack_into("<II", rec, 2, vu, donor["id"][1])
        struct.pack_into("<h", rec, base["campId"], c)
        struct.pack_into("<h", rec, rel(at["nameId"]), 600 + k)
        struct.pack_into("<h", rec, rel(at["nameId"]) + 2, a.level)  # reinforcement level (follows nameId)
        struct.pack_into("<I", rec, base["campId"] + 2 + 4 + 4, donor["unitFlags"] | U_INACTIVE)
        added.append(bytes(rec))

    stream = raw + b"".join(added)
    units = entities.walk_units(stream, cam.version, ws.db.class_rows())
    assert len(units) == len(cam.units) + a.count, (len(units), len(cam.units))
    cam.units_raw, cam.units = stream, units
    cam.members[cam._member_name("uni")] = entities.encode_units(stream, len(units))
    cam.path = os.path.join(GAME, "campaign", "SAVE", "chddf16.cam")
    cam.save()
    print("chddf16.cam: %d x %s (campId %d's copy), reinforcement level %d -> %s"
          % (a.count, aircraft(donor), donor["campId"], a.level, cam.path))


if __name__ == "__main__":
    main()
