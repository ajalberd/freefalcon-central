"""Build the "Korea Escalation" JSGME mod and the matching campsim variant from one set of files.

    python make_escalation_mod.py [--clones 1] [--no-install]

What changes (Korea save0 only, applies to NEW campaigns):
  - save0.tri: Blue wins only with Pyongyang AND Wonsan (stock: either), and Russia joins when Blue takes
    Wonsan (stock: DPRK supply <= 20% or air ratio <= 3). This is gamework's bothwr.tri.
  - save0.cam: China's and Russia's armies are staged forward (make_ally_mod "prc+cis": PRC's 43 and CIS's 8
    battalions 100-170 km behind the front, squadrons on DPRK airbases) and every PRC battalion and active
    squadron is cloned --clones times (Russia is staged, not cloned).
  - Falcon4.AII: ObjGroundPathMaxCost 2000 (stock 500), without which rear units are never picked for orders.

Writes:
  C:/FreeFalcon6/MODS/Korea Escalation/campaign/SAVE/{save0.cam, save0.tri, Falcon4.AII} + README.txt
  gamework/campaign/SAVE/escal.cam + escal.tri  (run campsim with --save escal --set tri=escal)
Units stay PRC (neutral) until the trigger script brings China in, like the originals.
"""
import argparse
import os
import shutil
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "campaign-editor"))
from ffcamp import entities, workspace  # noqa: E402
import make_ally_mod  # noqa: E402

GAME = r"C:\FreeFalcon6"
TDF = make_ally_mod.TDF
MOD = os.path.join(GAME, "MODS", "Korea Escalation")
GW = os.path.join(HERE, "gamework", "campaign", "SAVE")
U_PARENT = 0x20
U_INACTIVE = 0x20000
PRC = 5
# (donor H-6A squadron campId, DPRK airbase, reinforcement hour; 0 = active from the start) -- vanilla F4's Tu-16s
BOMBERS = [(4304, "Sunan Airbase", 0), (4306, "Sunan Airbase", 48), (4306, "Toksan Airbase", 72)]


def clone_prc(cam, ws, objs, clones):
    """Append `clones` copies of every PRC battalion and active squadron; returns (stream, n_bn, n_sq)."""
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

    spots = sorted((o for o in objs if o["owner"] == 6 and make_ally_mod.BAND[0] <= o["y"] <= make_ally_mod.BAND[1]
                    and any(t in o["typeName"] for t in make_ally_mod.GROUND_TYPES)), key=lambda o: o["x"])
    bns = sorted((u for u in cam.units if u["owner"] == PRC and u["kind"] == "battalion"), key=lambda u: u["x"])
    sqs = [u for u in cam.units if u["owner"] == PRC and u["kind"] == "squadron" and not u["unitFlags"] & U_INACTIVE]
    added, nb, ns, name = [], 0, 0, 500
    for k in range(clones):
        for i, u in enumerate(bns):
            s0, s1 = u["_span"]
            at = u["_at"]
            rel = lambda p: p - s0
            rec = bytearray(raw[s0:s1])
            vu, c = new_ids()
            # a different objective from the original's: half a step east along the band
            o = spots[int((i + 0.5 + k * 0.25) * len(spots) / max(1, len(bns))) % len(spots)]
            struct.pack_into("<II", rec, 2, vu, u["id"][1])
            struct.pack_into("<h", rec, base["x"], o["x"])
            struct.pack_into("<h", rec, base["y"], o["y"])
            struct.pack_into("<h", rec, base["campId"], c)
            struct.pack_into("<hh", rec, rel(at["destX"]), o["x"], o["y"])
            struct.pack_into("<h", rec, rel(at["nameId"]), name)
            name += 1
            # no brigade: the brigade's element list does not hold the clone, so it stands alone (U_PARENT puts
            # it on AllParentList, which is what the ground tasking manager walks)
            struct.pack_into("<II", rec, rel(at["supply"]) - 16, 0, 0)   # parentId
            struct.pack_into("<II", rec, rel(at["supply"]) - 8, 0, 0)    # lastObj
            struct.pack_into("<I", rec, base["campId"] + 2 + 4 + 4, u["unitFlags"] | U_PARENT)
            added.append(bytes(rec))
            nb += 1
        for u in sqs:
            s0, s1 = u["_span"]
            at = u["_at"]
            rel = lambda p: p - s0
            rec = bytearray(raw[s0:s1])
            vu, c = new_ids()
            struct.pack_into("<II", rec, 2, vu, u["id"][1])
            struct.pack_into("<h", rec, base["campId"], c)
            struct.pack_into("<h", rec, rel(at["nameId"]), name)
            name += 1
            added.append(bytes(rec))
            ns += 1

    # DPRK bombers, laid out as vanilla Falcon 4.0's Tu-16s (1 active squadron at Sunan, reinforcements at
    # Sunan h48 and Toksan h72). FF6 has no Tu-16 class; the H-6A is China's licence-built Tu-16, so China's
    # H-6A squadrons are cloned (4304 active, 4306 a reinforcement) and handed to DPRK.
    from ffcamp import names as ffnames
    nt = ffnames.load(os.path.join(GAME, "campaign", "SAVE"), "korea")
    by_name = {nt.get(o["nameId"], ""): o for o in objs if o["typeName"] in ("Airbase", "Airstrip")}
    camp = {u["campId"]: u for u in cam.units}
    nbomb = 0
    for donor_id, base_name, reinf in BOMBERS:
        u, ab = camp[donor_id], by_name[base_name]
        s0, s1 = u["_span"]
        at = u["_at"]
        rel = lambda p: p - s0
        rec = bytearray(raw[s0:s1])
        vu, c = new_ids()
        struct.pack_into("<II", rec, 2, vu, u["id"][1])
        struct.pack_into("<h", rec, base["x"], ab["x"])
        struct.pack_into("<h", rec, base["y"], ab["y"])
        struct.pack_into("<B", rec, base["owner"], 6)
        struct.pack_into("<h", rec, base["campId"], c)
        struct.pack_into("<hh", rec, rel(at["destX"]), ab["x"], ab["y"])
        struct.pack_into("<II", rec, rel(at["airbaseId"]), ab["id"][0], ab["id"][1])
        struct.pack_into("<II", rec, rel(at["hotSpot"]), ab["id"][0], ab["id"][1])
        struct.pack_into("<h", rec, rel(at["nameId"]), name)
        name += 1
        struct.pack_into("<h", rec, rel(at["nameId"]) + 2, reinf)  # reinforcement (follows nameId)
        flags = u["unitFlags"] & ~U_INACTIVE if reinf == 0 else u["unitFlags"] | U_INACTIVE
        struct.pack_into("<I", rec, base["campId"] + 2 + 4 + 4, flags)
        added.append(bytes(rec))
        nbomb += 1
    return raw + b"".join(added), nb, ns + nbomb


README = """Korea Escalation (JSGME) - Korea theater, save0 only. Applies to NEW campaigns.
- Blue wins only when it holds Pyongyang AND Wonsan (stock: either one ends the war).
- Russia joins when Blue takes Wonsan (stock: DPRK supply <= 20%% or air ratio <= 3).
- China's and Russia's armies start staged 100-170 km behind the front (stock ~340 km for China, ~500 km
  for Russia), their squadrons on DPRK airbases. Every PRC battalion and active squadron is doubled
  (%d battalions, %d squadrons added); Russia is staged only.
- DPRK gets bombers like vanilla Falcon 4.0's Tu-16s: H-6A (China's Tu-16) squadrons, 1 active at Sunan,
  reinforcements at Sunan (h48) and Toksan (h72). The squadron count above includes these 3.
- Falcon4.AII: ObjGroundPathMaxCost 2000 (stock 500), so rear units can be given orders at all.
Same files as campsim's gamework escal.cam/escal.tri (tools/campsim/make_escalation_mod.py).
Disable in JSGME to restore stock.
"""


def build(clones, install):
    gw_save0 = os.path.join(GW, "save0.cam")
    if open(os.path.join(GAME, "campaign", "SAVE", "save0.cam"), "rb").read() != open(gw_save0, "rb").read():
        raise SystemExit("the install's save0.cam is not stock (is the mod enabled?) -- disable it first")
    S = workspace.Session(GAME)
    ws = S.workspace(TDF)
    cam = ws.units("save0.cam")
    objs = ws.objectives("save0.cam").objectives or []
    stream, report = make_ally_mod.stage(cam, objs, "prc+cis")
    cam.units_raw = stream
    cam.units = entities.walk_units(stream, cam.version, ws.db.class_rows())
    stream, nb, ns = clone_prc(cam, ws, objs, clones)
    units = entities.walk_units(stream, cam.version, ws.db.class_rows())
    assert len(units) == len(cam.units) + nb + ns, (len(units), len(cam.units), nb, ns)
    cam.units_raw, cam.units = stream, units
    cam.members[cam._member_name("uni")] = entities.encode_units(stream, len(units))
    tri = os.path.join(GW, "bothwr.tri")
    aii_text = open(os.path.join(GAME, "campaign", "SAVE", "Falcon4.AII"), "rb").read()
    if b"ObjGroundPathMaxCost = 500" not in aii_text:
        raise SystemExit("expected the install's stock Falcon4.AII (ObjGroundPathMaxCost = 500)")
    aii_text = aii_text.replace(b"ObjGroundPathMaxCost = 500", b"ObjGroundPathMaxCost = 2000")

    outs = [(GW, "escal")]
    if install:
        outs.append((os.path.join(MOD, "campaign", "SAVE"), "save0"))
    for d, stem in outs:
        os.makedirs(d, exist_ok=True)
        cam.path = os.path.join(d, stem + ".cam")
        cam.save()
        shutil.copy(tri, os.path.join(d, stem + ".tri"))
    if install:
        with open(os.path.join(MOD, "campaign", "SAVE", "Falcon4.AII"), "wb") as f:
            f.write(aii_text)
        with open(os.path.join(MOD, "README.txt"), "w") as f:
            f.write(README % (nb, ns))
    print("staged: %s; cloned %d battalions, %d squadrons -> %s" % (report, nb, ns, ", ".join(d for d, _ in outs)))


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--clones", type=int, default=1)
    ap.add_argument("--no-install", action="store_true")
    a = ap.parse_args()
    build(a.clones, not a.no_install)
