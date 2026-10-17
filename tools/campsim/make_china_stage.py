"""Put China's ground battalions (not its air defence) at the North Korean rail gate: python make_china_stage.py <src=chnb> <out=chdd> [spots=8]

Tan-tung (Dandong, objective 1074) is the Chinese end of the Pyongbu line's bridge over the Yalu, 5 km from Sinuiju.  Every PRC ground battalion
(unit-table role not 5) is moved onto Dandong and the nearest Chinese towns / cities / villages, round robin; PRC air-defence battalions and
squadrons stay where the source scenario has them.  Only the unit positions change (the same patch make_ally_mod.py uses)."""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "campaign-editor"))
from ffcamp import entities, workspace  # noqa: E402

GAME = r"C:\FreeFalcon6"
TDF = "terrdata/theaterdefinition/korea.tdf"
DANDONG = 1074
PRC = 5


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else "chnb"
    out = sys.argv[2] if len(sys.argv) > 2 else "chdd"
    nspots = int(sys.argv[3]) if len(sys.argv) > 3 else 8
    mode = sys.argv[4] if len(sys.argv) > 4 else "dandong"      # "front": instead onto North Korean towns 70-150 km north of the starting front (y 560-640)
    ws = workspace.Session(GAME).workspace(TDF)
    cam = ws.units(src + ".cam")
    objs = ws.objectives(src + ".cam").objectives or []
    rows = ws.db.class_rows()
    utbl = ws.db.table("unit")
    base = entities.base_offsets(cam.version)
    raw = bytearray(cam.units_raw)
    dd = [o for o in objs if o["campId"] == DANDONG][0]
    towns = sorted((o for o in objs if o["owner"] == PRC and any(t in o["typeName"] for t in ("Town", "City", "Village")) and o["campId"] != DANDONG),
                   key=lambda o: (o["x"] - dd["x"]) ** 2 + (o["y"] - dd["y"]) ** 2)
    spots = [dd] + towns[: nspots - 1]
    if mode == "front":
        spots = sorted((o for o in objs if o["owner"] == 6 and any(w in o["typeName"] for w in ("Town", "City", "Village")) and 560 <= o["y"] <= 640),
                       key=lambda o: o["x"])
        spots = spots[:: max(1, len(spots) // nspots)][:nspots]
    moved = 0
    for u in cam.units:
        if u["kind"] != "battalion" or u["owner"] != PRC:
            continue
        if utbl.rows[rows[u["classIndex"]]["dataPtr"]]["Role"] == 5:
            continue
        o = spots[moved % len(spots)]
        s0 = u["_span"][0]
        struct.pack_into("<h", raw, s0 + base["x"], o["x"])
        struct.pack_into("<h", raw, s0 + base["y"], o["y"])
        moved += 1
    stream = bytes(raw)
    cam.units_raw = stream
    cam.units = entities.walk_units(stream, cam.version, rows)
    cam.members[cam._member_name("uni")] = entities.encode_units(stream, len(cam.units))
    cam.path = os.path.join(ws.campaign_dir, out + ".cam")
    cam.save()
    far = max(((o["x"] - dd["x"]) ** 2 + (o["y"] - dd["y"]) ** 2) ** 0.5 for o in spots) if mode != "front" else 0.0
    print("%s -> %s: %d PRC ground battalions on %d objectives around Dandong (farthest %.0f km); %s" % (
        src, cam.path, moved, len(spots), far, ", ".join(str(o["campId"]) for o in spots)))


if __name__ == "__main__":
    main()
