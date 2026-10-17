"""Where North Korea's air defence sits: python sam_depth.py [cam=chnb.cam]

Counts air-defence battalions (unit-table role 5) by owner and by depth north of the front (y 490, the line the war starts on), by the SAM
type of the battalion's first vehicle group, and how many of them stand within 25 km of a factory / power plant / refinery.  Static: read from the
.cam, no simulation.  The loss side (what Blue loses to them, by depth) is the AIRLOSSDEPTH line campsim prints."""
import collections
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "campaign-editor"))
from ffcamp import entities, workspace  # noqa: E402

FRONT_Y = 490
GAME = r"C:\FreeFalcon6"
name = sys.argv[1] if len(sys.argv) > 1 else "chnb.cam"
ws = workspace.Session(GAME).workspace("terrdata/theaterdefinition/korea.tdf")
cam = ws.units(name)
objs = ws.objectives(name).objectives or []
rows = ws.db.class_rows()
utbl = ws.db.table("unit")
vtbl = ws.db.table("vehicle")
names = ws.db.name_index()


def unit_row(u):
    ci = u["classIndex"]
    if not (0 <= ci < len(rows)):
        return ci, None
    p = rows[ci]["dataPtr"]
    return ci, (utbl.rows[p] if utbl and 0 <= p < len(utbl.rows) else None)


def vehicle_name(row):
    if not row:
        return "?"
    for vt in row["VehicleType"][:16]:
        if vt and vt != 65535 and vt < len(rows):
            return names.get(vt, "veh%d" % vt)
    return "?"


ad = []
for u in cam.units:
    if u["kind"] != "battalion":
        continue
    ci, row = unit_row(u)
    if row and row["Role"] == 5:
        ad.append((u, ci, vehicle_name(row)))

print("%s: %d air-defence battalions" % (name, len(ad)))
by_owner = collections.Counter(u["owner"] for u, _, _ in ad)
print("by owner:", dict(by_owner))
print("\nclass (index, vehicle) by owner:")
cls = collections.Counter((u["owner"], ci, vn) for u, ci, vn in ad)
for k, n in sorted(cls.items()):
    print("  owner %d class %4d %-14s x%d" % (k[0], k[1], k[2], n))

print("\ndepth north of the front (y - %d, 50 km bands), DPRK(6) + PRC(5):" % FRONT_Y)
band = collections.Counter()
for u, ci, vn in ad:
    if u["owner"] in (5, 6):
        band[max(-1, (u["y"] - FRONT_Y) // 50)] += 1
for b in sorted(band):
    print("  %4d..%4d : %d" % (b * 50, b * 50 + 49, band[b]))

targets = [o for o in objs if o["owner"] == 6 and any(t in o["typeName"] for t in ("Factory", "Power", "Refinery", "Chemical", "Nuclear"))]
print("\nDPRK factories/power/refineries: %d" % len(targets))
kinds = collections.Counter(o["typeName"] for o in targets)
print(dict(kinds))
near = 0
rows_out = []
for o in targets:
    n = sum(1 for u, _, _ in ad if u["owner"] in (5, 6) and ((u["x"] - o["x"]) ** 2 + (u["y"] - o["y"]) ** 2) ** 0.5 <= 25)
    rows_out.append((o["y"] - FRONT_Y, o["typeName"], n))
    near += 1 if n else 0
print("with an AD battalion within 25 km: %d of %d" % (near, len(targets)))
for d, t, n in sorted(rows_out):
    print("  depth %4d %-12s AD within 25 km: %d" % (d, t, n))
