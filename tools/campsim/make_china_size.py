"""Make test campaigns with fewer Chinese battalions: python make_china_size.py <N> [src=chnb] [out=chnb<N>]

Deletes cloned PRC battalions (the 43 added by the Escalation mod, no brigade) evenly until N PRC battalions are left (86 -> N, N >= 43) and writes
Campaign/SAVE/<out>.cam beside the source, for the spread (`b72` etc. in supply_spread.py).  A continuous dial for how strong China is."""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "campaign-editor"))
from ffcamp import entities, workspace
N = int(sys.argv[1]); src = sys.argv[2] if len(sys.argv) > 2 else "chnb"; out = sys.argv[3] if len(sys.argv) > 3 else "chnb%d" % N
ws = workspace.Session(r"C:\FreeFalcon6").workspace("terrdata/theaterdefinition/korea.tdf")
cam = ws.units(src + ".cam")
prc = [u for u in cam.units if u["owner"] == 5 and u["kind"] == "battalion"]
clones = [u for u in prc if u["unitFlags"] & entities.U_PARENT]
drop = len(prc) - N
if drop < 0 or drop > len(clones):
    raise SystemExit("cannot go from %d to %d (clones: %d)" % (len(prc), N, len(clones)))
step = len(clones) / float(drop) if drop else 0
victims = [clones[int(i * step)] for i in range(drop)]
raw = cam.units_raw
for u in sorted(victims, key=lambda u: -u["_span"][0]):     # highest offset first, spans stay valid
    raw = entities.delete_unit(raw, u)
cam.units_raw = raw
cam.units = entities.walk_units(raw, cam.version, ws.db.class_rows())
cam.members[cam._member_name("uni")] = entities.encode_units(raw, len(cam.units))
cam.path = os.path.join(ws.campaign_dir, out + ".cam")
cam.save()
left = sum(1 for u in cam.units if u["owner"] == 5 and u["kind"] == "battalion")
print("%s: %d PRC battalions -> %s (%d left)" % (src, len(prc), cam.path, left))
