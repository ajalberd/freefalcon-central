"""Survey air units in a campaign file: python survey_air.py [save0.cam]"""
import sys, os, collections
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "campaign-editor"))
from ffcamp import workspace
S = workspace.Session(r"C:\FreeFalcon6")
ws = S.workspace("terrdata/theaterdefinition/korea.tdf")
name = sys.argv[1] if len(sys.argv) > 1 else "save0.cam"
cam = ws.units(name)
print(name, "units", len(cam.units), "err", cam.units_error)
u0 = cam.units[0]
print({k: v for k, v in u0.items() if not k.startswith("_")})
kinds = collections.Counter((u.get("kind"), u.get("owner")) for u in cam.units)
print(kinds)

names = ws.db.name_index()
objs = ws.objectives(name).objectives or []
oby = {}
for o in objs:
    oby[tuple(o["id"])] = o
def bits(x): return bin(x & 0xffffffff).count("1")
rows = []
for u in cam.units:
    if u["kind"] != "squadron": continue
    tn, drow = ws.db.data_row(u["classIndex"])
    mk = []
    if drow:
        for vt, n in zip(drow["VehicleType"], drow["NumElements"]):
            if vt and n: mk.append((names.get(vt, "?"), n))
    ab = oby.get(tuple(u["airbaseId"]))
    rows.append((u["owner"], u["campId"], u["x"], u["y"], mk, u.get("roster"), ab and ab.get("x"), ab and ab.get("y"), u["airbaseId"], u.get("specialty"), u.get("unitFlags")))
rows.sort(key=lambda r: (r[0], str(r[4])))
for r in rows:
    print(r)
