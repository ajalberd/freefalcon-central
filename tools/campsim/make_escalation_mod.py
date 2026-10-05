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
  C:/FreeFalcon6/MODS/Korea Escalation/campaign/SAVE/{save0.cam, save0.tri, Falcon4.AII} + "Korea Escalation README.txt" (lands in the game folder when enabled)
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
import saves  # noqa: E402

GAME = r"C:\FreeFalcon6"
TDF = make_ally_mod.TDF
MOD = os.path.join(GAME, "MODS", "Korea Escalation")
GW = os.path.join(HERE, "gamework", "campaign", "SAVE")
U_PARENT = 0x20
U_INACTIVE = 0x20000
PRC = 5
# (donor H-6A squadron campId, DPRK airbase, reinforcement hour; 0 = active from the start) -- vanilla F4's Tu-16s
BOMBERS = [(4304, "Sunan Airbase", 0), (4306, "Sunan Airbase", 48), (4306, "Toksan Airbase", 72)]
# Russia's Pacific Fleet: (class index, x, y, naval orders) -- NORD_ATTACK 1, the carrier as the US one (3)
KUZNETSOV, KIEV_GROUP, NAKHIMOV, KILO, OSA = 2158, 3584, 3261, 2793, 828
# air defence first (Andrew: "boats with great anti-air capabilities"): the Kiev group (Kiev, 2 Admiral Nakhimov,
# 6 Najin) and 3 Admiral Nakhimov cruisers reach 64 km at full hit chance; the Osa II's reach is 4 km
FLEET = [(KUZNETSOV, 600, 660, 3), (KIEV_GROUP, 620, 640, 1),
         (NAKHIMOV, 580, 635, 1), (NAKHIMOV, 560, 620, 1), (NAKHIMOV, 590, 688, 1), (KILO, 565, 600, 1),
         (OSA, 530, 615, 1), (OSA, 545, 640, 1), (OSA, 560, 665, 1), (OSA, 540, 590, 1)]
SU33, SU27_DONOR = 2169, 4783  # Su-33 squadron class; Russia's Su-27 squadron record it is cloned from


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

    # Russia's Pacific Fleet (save0 gives Russia no ships at all): a Kuznetsov carrier with an Su-33 squadron
    # aboard, an Admiral Nakhimov missile cruiser, a Kilo submarine and 8 Osa II missile boats, 44-127 km off
    # Wonsan (open water checked against KOREA.THR). Russian (neutral) until the trigger brings Russia in at
    # Wonsan, then folded into DPRK like the rest of Russia's forces.
    nship = 0
    carrier_vu = None
    for cls, x, y, orders in FLEET:
        u = saves.load_tables(saves.SOURCES["ff6"])[1][cls]
        roster = 0
        for g in range(16):
            roster |= min(3, int(u["NumElements"][g])) << (2 * g)
        vu, c = new_ids()
        added.append(entities.build_unit("taskforce", cam.version, cls + entities.VU_LAST_ENTITY_TYPE, x, y, 4, vu,
                                         c, roster=roster, orders=orders, supply=100, unit_flags=U_PARENT))
        nship += 1
        if cls == KUZNETSOV:
            carrier_vu, cx, cy = vu, x, y

    # Su-33 squadron on the carrier: Russia's Su-27 squadron record (nearest stores) switched to the Su-33 class
    u = camp[SU27_DONOR]
    s0, s1 = u["_span"]
    at = u["_at"]
    rel = lambda p: p - s0
    rec = bytearray(raw[s0:s1])
    vu, c = new_ids()
    t = SU33 + entities.VU_LAST_ENTITY_TYPE
    struct.pack_into("<h", rec, 0, t)                 # dispatch type
    struct.pack_into("<II", rec, 2, vu, u["id"][1])
    struct.pack_into("<H", rec, 10, t)                # entityType
    struct.pack_into("<h", rec, base["x"], cx)
    struct.pack_into("<h", rec, base["y"], cy)
    struct.pack_into("<h", rec, base["campId"], c)
    struct.pack_into("<hh", rec, rel(at["destX"]), cx, cy)
    struct.pack_into("<II", rec, rel(at["airbaseId"]), carrier_vu, 0)
    struct.pack_into("<h", rec, rel(at["nameId"]), name)
    struct.pack_into("<h", rec, rel(at["nameId"]) + 2, 0)   # no reinforcement delay
    struct.pack_into("<I", rec, base["campId"] + 2 + 4 + 4, u["unitFlags"] & ~U_INACTIVE)
    added.append(bytes(rec))
    return raw + b"".join(added), nb, ns + nbomb + 1, nship


README = """Korea Escalation (JSGME) - Korea theater, save0 only. Applies to NEW campaigns.
- Blue wins only when it holds Pyongyang AND Wonsan (stock: either one ends the war).
- Russia joins when Blue takes Wonsan (stock: DPRK supply <= 20%% or air ratio <= 3).
- China's and Russia's armies start staged 100-170 km behind the front (stock ~340 km for China, ~500 km
  for Russia), their squadrons on DPRK airbases. Every PRC battalion and active squadron is doubled
  (%d battalions, %d squadrons added); Russia is staged only.
- DPRK gets bombers like vanilla Falcon 4.0's Tu-16s: H-6A (China's Tu-16) squadrons, 1 active at Sunan,
  reinforcements at Sunan (h48) and Toksan (h72). The squadron count above includes these 3.
- Russia's Pacific Fleet off Wonsan (stock: no Russian ships), built for air defence: Kuznetsov carrier with an
  Su-33 squadron aboard, the Kiev battle group (Kiev, 2 Admiral Nakhimov, 6 Najin), 3 more Admiral Nakhimov
  cruisers (SAMs to 64 km), a Kilo submarine and 4 Osa II missile-boat groups (12 boats). It joins with Russia.
- Falcon4.AII: ObjGroundPathMaxCost 2000 (stock 500), so rear units can be given orders at all.
Play it with FFViper-ai.exe and, in FFViper.cfg: set g_bAlertScramble 1 / set g_bInitTrueLosses 1 /
set g_nCounterAttackInitiative 15 / set g_nCaptureInitiative 2 (campsim: war ends ~h81 instead of h34-45).
Same files as campsim's gamework escal.cam/escal.tri (tools/campsim/make_escalation_mod.py);
details in tools/campsim/KOREA-ESCALATION.md.
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
    stream, nb, ns, nship = clone_prc(cam, ws, objs, clones)
    units = entities.walk_units(stream, cam.version, ws.db.class_rows())
    assert len(units) == len(cam.units) + nb + ns + nship, (len(units), len(cam.units), nb, ns, nship)
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
        with open(os.path.join(MOD, "Korea Escalation README.txt"), "w") as f:
            f.write(README % (nb, ns))
    print("staged: %s; cloned %d battalions, %d squadrons; %d Russian ships -> %s" % (report, nb, ns, nship, ", ".join(d for d, _ in outs)))


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--clones", type=int, default=1)
    ap.add_argument("--no-install", action="store_true")
    a = ap.parse_args()
    build(a.clones, not a.no_install)
