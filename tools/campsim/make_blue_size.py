"""Add Blue ground battalions to a Korea campaign save, for the Iron Fortress balance tests.

    python make_blue_size.py <copies> [--src save1] [--out save1b] [--step 4]

Clones every <step>th ROK battalion <copies> times, placing the clones on Blue-held objectives in
the south (y <= 400, the Pusan pocket).  Same record-clone technique as
make_escalation_mod.clone_prc: the donor record is copied byte for byte, then the VU id, camp id,
position, destination and name id are repointed; parentId and lastObj are zeroed and U_PARENT is
set, because the brigade's element list does not contain the clone and the ground tasking manager
walks AllParentList.

The write lands in the game's campaign/SAVE beside the source save (the pattern the other builders
use); the .cam's own Scenario field still says "save1", so the engine reads save1.tri for triggers.
"""
import argparse
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "campaign-editor"))
from ffcamp import entities, workspace  # noqa: E402

GAME = r"C:\FreeFalcon6"
TDF = "terrdata/theaterdefinition/korea.tdf"

U_PARENT = 0x20
BLUE = 2


def build(copies, src="save1", out="save1b", step=4, verbose=True):
    ws = workspace.Session(GAME).workspace(TDF)
    cam = ws.units(src + ".cam")
    objs = ws.objectives(src + ".cam").objectives or []

    base = entities.base_offsets(cam.version)
    raw = cam.units_raw
    used_vu = {u["id"][0] for u in cam.units}
    used_camp = {u["campId"] for u in cam.units} | {o["campId"] for o in objs}

    def new_ids():
        vu = entities.FIRST_NON_VOLATILE_ID
        while vu in used_vu:
            vu += 1
        c = 1
        while c in used_camp:
            c += 1
        used_vu.add(vu)
        used_camp.add(c)
        return vu, c

    # Blue-held objectives in the south, where the campaign starts with Blue's army
    spots = sorted((o for o in objs if o["owner"] == BLUE and o["y"] <= 400),
                   key=lambda o: (o["y"], o["x"]))
    if not spots:
        raise SystemExit("no Blue-held objectives south of y=400 in %s" % src)

    bns = sorted((u for u in cam.units if u["owner"] == BLUE and u["kind"] == "battalion"),
                 key=lambda u: (u["y"], u["x"]))
    donors = bns[::max(1, step)]
    if not donors:
        raise SystemExit("no ROK battalions to clone in %s" % src)

    added = []
    name = 900
    for k in range(copies):
        for i, u in enumerate(donors):
            s0, s1 = u["_span"]
            at = u["_at"]
            rel = lambda p: p - s0

            rec = bytearray(raw[s0:s1])
            vu, c = new_ids()
            o = spots[(i * 7 + k * 13) % len(spots)]

            struct.pack_into("<II", rec, 2, vu, u["id"][1])
            struct.pack_into("<h", rec, base["x"], o["x"])
            struct.pack_into("<h", rec, base["y"], o["y"])
            struct.pack_into("<h", rec, base["campId"], c)
            struct.pack_into("<hh", rec, rel(at["destX"]), o["x"], o["y"])
            struct.pack_into("<h", rec, rel(at["nameId"]), name)
            name += 1
            struct.pack_into("<II", rec, rel(at["supply"]) - 16, 0, 0)   # parentId: no brigade
            struct.pack_into("<II", rec, rel(at["supply"]) - 8, 0, 0)    # lastObj: unassigned
            struct.pack_into("<I", rec, base["campId"] + 2 + 4 + 4, u["unitFlags"] | U_PARENT)
            added.append(bytes(rec))

    stream = bytes(raw) + b"".join(added)
    units2 = entities.walk_units(stream, cam.version, ws.db.class_rows())
    assert len(units2) == len(cam.units) + len(added), (len(units2), len(cam.units), len(added))

    cam.units_raw = stream
    cam.units = units2
    member = cam._member_name("uni")
    cam.members[member] = entities.encode_units(stream, len(units2))

    out_path = os.path.join(ws.campaign_dir, out + ".cam")
    cam.path = out_path
    cam.save()

    if verbose:
        print("%s: %d units -> %d (%d battalions cloned from %d donors, %d start objectives)"
              % (out, len(units2) - len(added), len(units2), len(added), len(donors), len(spots)))
        print(out_path)
    return out_path


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("copies", type=int)
    ap.add_argument("--src", default="save1")
    ap.add_argument("--out", default="save1b")
    ap.add_argument("--step", type=int, default=4)
    a = ap.parse_args()
    build(a.copies, a.src, a.out, a.step)
