"""Build a DPRK-air-boost variant of a Korea campaign save.

    python make_air_mod.py <variant> <out-dir> [--src save0.cam] [--list]

Writes <out-dir>/<src name> (+ the matching .tri). Variants are cumulative sets of
clone/rehome operations on squadron units (see VARIANTS). Squadrons are cloned
byte for byte from a donor record and re-pointed at another airbase, so every field the
engine reads stays valid; ids come from entities.next_ids.
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
U_INACTIVE = 0x20000

# campId of the donor squadron (see survey_air.py) -> clone it to an airbase (by name).
#   ("clone", donor_campId, "Airbase name")
#   ("rehome", campId, "Airbase name")
HELI = [
    ("clone", 4294, "Koksan Airbase"),          # Mi-24 Hind
    ("clone", 4274, "Hwangju Airbase"),         # Mi-24 Hind
    ("clone", 4301, "Kwail Airbase"),           # Mi-8
]
BOMBER = [
    ("clone", 4096, "Onch'on Airbase"),         # H-5 (Il-28) bombers
    ("clone", 4097, "Mirim Airbase"),
    ("clone", 4470, "Ongjin Airbase"),          # Su-25BM attack
    ("clone", 5046, "Koksan Airbase"),
    ("clone", 4284, "Hwangju Airbase"),         # A-5 strike
]
FIGHTER = [
    ("clone", 5041, "Hwangju Airbase"),         # MiG-29A
    ("clone", 5038, "Taetan Airbase"),          # MiG-23ML
    ("clone", 5051, "Koksan Airbase"),          # MiG-21bis
    ("clone", 4272, "Ongjin Airbase"),          # MiG-21bis
]
REHOME = [
    ("rehome", 4098, "Taetan Airbase"),         # H-5 at Uiju -> near the front
    ("rehome", 4287, "Mirim Airbase"),          # H-5 at Orang
]
HALF = [
    ("clone", 4294, "Koksan Airbase"),          # Mi-24
    ("clone", 4096, "Onch'on Airbase"),         # H-5
    ("clone", 4470, "Ongjin Airbase"),          # Su-25BM
    ("clone", 5041, "Hwangju Airbase"),         # MiG-29A
    ("clone", 5051, "Koksan Airbase"),          # MiG-21bis
    ("clone", 5038, "Taetan Airbase"),          # MiG-23ML
]
VARIANTS = {
    "half": HALF,
    "none": [],
    "heli": HELI,
    "bomber": BOMBER,
    "fighter": FIGHTER,
    "rehome": REHOME,
    "air": HELI + BOMBER + FIGHTER,
    "air+rehome": HELI + BOMBER + FIGHTER + REHOME,
}


def pack_at(rec, at, fmt, *vals):
    struct.pack_into(fmt, rec, at, *vals)


def build(variant, out_dir, src="save0.cam", verbose=True):
    S = workspace.Session(GAME)
    ws = S.workspace(TDF)
    cam = ws.units(src)
    objs = ws.objectives(src).objectives or []
    nametab = ws.name_table() if hasattr(ws, "name_table") else None
    import server  # place names, same rule as the editor
    names = ws.db.name_index()
    by_name = {}
    for o in objs:
        by_name[server.place_name(nametab, names, o)] = o

    version = cam.version
    base = entities.base_offsets(version)
    raw = cam.units_raw
    units = cam.units
    camp = {u["campId"]: u for u in units}
    used_vu = {u["id"][0] for u in units}
    used_camp = set(camp) | {o["campId"] for o in objs}
    next_name = {}

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

    added, report = [], []
    patched = bytearray(raw)
    for op, cid, base_name in VARIANTS[variant]:
        ab = by_name.get(base_name)
        if ab is None:
            raise SystemExit("no objective named %r" % base_name)
        donor = camp[cid]
        s0, s1 = donor["_span"]
        at = donor["_at"]
        if op == "rehome":
            rec = patched
            off = s0
            vals = [(base["x"], ab["x"]), (base["y"], ab["y"])]
            for k, v in vals:
                struct.pack_into("<h", rec, s0 + k, v)
            struct.pack_into("<h", rec, at["destX"], ab["x"])
            struct.pack_into("<h", rec, at["destX"] + 2, ab["y"])
            struct.pack_into("<II", rec, at["airbaseId"], ab["id"][0], ab["id"][1])
            struct.pack_into("<II", rec, at["hotSpot"], ab["id"][0], ab["id"][1])
            report.append(("rehome", cid, base_name))
            continue
        rec = bytearray(raw[s0:s1])
        vu, c = new_ids()
        rel = lambda p: p - s0
        struct.pack_into("<II", rec, 2, vu, donor["id"][1])
        struct.pack_into("<h", rec, base["x"], ab["x"])
        struct.pack_into("<h", rec, base["y"], ab["y"])
        struct.pack_into("<h", rec, base["campId"], c)
        struct.pack_into("<h", rec, rel(at["destX"]), ab["x"])
        struct.pack_into("<h", rec, rel(at["destX"]) + 2, ab["y"])
        struct.pack_into("<II", rec, rel(at["airbaseId"]), ab["id"][0], ab["id"][1])
        struct.pack_into("<II", rec, rel(at["hotSpot"]), ab["id"][0], ab["id"][1])
        nid = next_name.get(donor["classIndex"], 60)
        next_name[donor["classIndex"]] = nid + 1
        struct.pack_into("<h", rec, rel(at["nameId"]), nid)
        struct.pack_into("<I", rec, base["campId"] + 2 + 4 + 4,
                         donor["unitFlags"] & ~U_INACTIVE)
        added.append(bytes(rec))
        report.append(("clone", cid, base_name, vu, c))

    # the donor spans are positions in the original stream: patch in place, then append
    stream = bytes(patched) + b"".join(added)
    units2 = entities.walk_units(stream, version, ws.db.class_rows())
    assert len(units2) == len(units) + len(added), (len(units2), len(units), len(added))
    cam.units_raw = stream
    cam.units = units2
    member = cam._member_name("uni")
    cam.members[member] = entities.encode_units(stream, len(units2))

    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, src)
    cam.path = out
    cam.save()
    tri = os.path.splitext(os.path.join(ws.campaign_dir, src))[0] + ".tri"
    if os.path.isfile(tri):
        shutil.copy(tri, os.path.splitext(out)[0] + ".tri")
    if verbose:
        for r in report:
            print(r)
        print("%s: %d units -> %d  (%s)" % (variant, len(units), len(units2), out))
    return out


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("variant", choices=sorted(VARIANTS))
    ap.add_argument("out_dir")
    ap.add_argument("--src", default="save0.cam")
    a = ap.parse_args()
    build(a.variant, a.out_dir, a.src)
