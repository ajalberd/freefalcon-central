"""Build the two hidden Israel 2012 campaigns (save0 = 2004, save1 = 2012) as modern scenarios.

python build_modern.py [--write]

Each is a copy of save2 (the 2007 fictional war: modern map ownership, units and objectives) with the squadrons'
aircraft swapped for the year and the title / trigger script of its own.  Originals are kept as *.bak-orig.
Without --write nothing in the install is touched: the result goes to ./out/.
"""
import os, shutil, struct, sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "campaign-editor"))
from ffcamp import campfile, entities, workspace  # noqa: E402

GAME = r"C:\FreeFalcon6"
TDF = "Theaters/Israel2007/Israel2007.tdf"
CAMP = os.path.join(GAME, "Theaters", "Israel2007", "campaign")
HERE = os.path.dirname(os.path.abspath(__file__))

# class-table index of the aircraft squadron classes used below (names from the CampaignDB)
F16I, F16CG, F16C, F16CP, AH64A, MIRAGE5, J7B, MIG21EAF = 2678, 372, 2650, 2638, 417, 2578, 3288, 3269
MIG29A, MIG29SM, MIG23ML, SU22, SU24, MI24 = 360, 2546, 3289, 2580, 380, 421
F5E_RSAF, F15C_RSAF, F14D, F15C = 3279, 3575, 2648, 356

# squadron VU id (number) -> new class, per scenario
SWAPS = {
    "save0": {   # 2004: the first Sufa, Apache A, no Rafale, a MiG-29 Syria, an F-5/F-15 Saudi Arabia
        7221: F16CG, 7222: F16C,                    # Ramon squadrons: only one F-16I squadron exists yet
        7182: AH64A,
        4232: MIRAGE5, 4168: AH64A, 4170: AH64A, 4172: MIG21EAF, 4173: MIG21EAF,
        7191: MIG29A, 7193: MIG23ML, 7194: MIG29A, 7269: SU22, 7270: SU24, 7203: MI24,
        4033: F5E_RSAF, 4036: F15C_RSAF,
        4029: F14D,
    },
    "save1": {   # 2012: Sufa in numbers, Egypt's F-16 fleet, Syria's MiG-29SM, Typhoon in Saudi Arabia
        4232: F16CP, 4172: J7B, 4173: J7B,
        7191: MIG29SM, 7193: MIG23ML, 7194: MIG29A, 7269: SU22, 7270: SU24, 7203: MI24,
        4029: F15C,
    },
}
# fraction of the ground vehicles removed from every battalion of a team (the 2007 save has ~6,500 Arab vehicles to
# Israel's ~2,900; campsim's strip= knob found 25% still lost for Israel, 40% a draw every time)
STRIP = {"save0": {5: 0.30, 6: 0.28}, "save1": {5: 0.30, 6: 0.28}}
TITLE = {"save0": "2004 Fictional War", "save1": "2012 Fictional War"}


def build(stem, outdir, write):
    ws = workspace.TheaterWorkspace(GAME, TDF)
    src = ws.units("save2.cam")
    base = entities.base_offsets(src.version)
    roster_off = base["campId"] + 2 + 4        # span start -> campId(h), lastCheck(I), roster(I)
    rows, utbl, ssd = ws.db.class_rows(), ws.db.table("unit"), ws.db.table("squadstores")
    names = ws.db.name_index()

    # a copy of save2 under the new name: copy_campaign writes <stem>.cam (+ the 2007 script, replaced later)
    work = os.path.join(outdir, "campaign")
    os.makedirs(work, exist_ok=True)
    members, order = {}, []
    for name in src.order:
        n = stem + "." + name.rsplit(".", 1)[-1]
        members[n] = src.members[name]
        order.append(n)
    path = os.path.join(work, stem + ".cam")
    new = campfile.CampaignFile(path, members, order, stem)
    new.version = src.version
    cmp_name = new._member_name("cmp")
    new.header = campfile.decode_header(members[cmp_name], src.version)

    raw = bytearray(src.units_raw)
    units = src.units
    changed = []
    for u in units:
        if u["kind"] != "squadron":
            continue
        dr = utbl.rows[rows[u["classIndex"]]["dataPtr"]]
        at = u["_span"][0] + roster_off
        have = struct.unpack_from("<I", raw, at)[0]
        if have != entities.roster_from_class(dr):
            print("   note: roster of", u["id"][0], "was", have, "class says", entities.roster_from_class(dr))
        new_cls = SWAPS[stem].get(u["id"][0])
        if new_cls is None:
            continue
        nr = rows[new_cls]
        ndr = utbl.rows[nr["dataPtr"]]
        tid = new_cls + entities.VU_LAST_ENTITY_TYPE
        struct.pack_into("<h", raw, u["_span"][0], tid)                  # the leading dispatch type
        struct.pack_into("<H", raw, u["_span"][0] + 2 + 8, tid)          # entityType
        struct.pack_into("<I", raw, at, entities.roster_from_class(ndr))
        stores = ssd.rows[ndr["SpecialIndex"]]["Stores"]                 # what SquadronClass(type) starts with
        s0 = u["_storesAt"]
        raw[s0:s0 + len(stores)] = bytes(stores)[:u["_storesLen"]].ljust(u["_storesLen"], b"\0")[:u["_storesLen"]]
        for h in new.header.squadrons:                                   # the player's squadron list holds the class too
            if tuple(h["id"]) == tuple(u["id"]):
                h["dIndex"] = new_cls
        changed.append((u["id"][0], names.get(u["classIndex"]), "->", names.get(utbl.rows[nr["dataPtr"]]["VehicleType"][0])))
    cut = 0
    for u in units:
        p = STRIP[stem].get(u["owner"])
        if u["kind"] != "battalion" or not p:
            continue
        at = u["_span"][0] + roster_off
        r = struct.unpack_from("<I", raw, at)[0]
        cnt = [(r >> (2 * g)) & 3 for g in range(16)]
        tot = sum(cnt)
        want = max(1, tot - int(round(tot * p)))
        g = u["id"][0] % 16
        while sum(cnt) > want:
            if cnt[g] > 0 and sum(cnt) > 1:
                cnt[g] -= 1
            g = (g + 1) % 16
        r2 = sum(c << (2 * i) for i, c in enumerate(cnt))
        struct.pack_into("<I", raw, at, r2)
        cut += tot - sum(cnt)
    print("   ground vehicles removed:", cut)
    new.members[new._member_name("uni")] = entities.encode_units(bytes(raw), len(units))
    f = new.header.fields
    f["UIName"], f["Scenario"], f["SaveFile"] = TITLE[stem], stem, stem
    new.header_dirty = True
    new.save(path)
    shutil.copy2(os.path.join(HERE, "tri_" + stem + ".tri"), os.path.join(work, stem + ".tri"))
    return changed, path


def main():
    write = "--write" in sys.argv
    out = os.path.join(HERE, "out")
    shutil.rmtree(out, ignore_errors=True)
    for stem in ("save0", "save1"):
        changed, path = build(stem, os.path.join(out, stem), write)
        print(stem, len(changed), "squadrons changed ->", path)
        for c in changed:
            print("   ", c)
    if write:
        for stem in ("save0", "save1"):
            for ext in (".cam", ".tri"):
                dst = os.path.join(CAMP, stem + ext)
                if os.path.exists(dst) and not os.path.exists(dst + ".bak-orig"):
                    shutil.copy2(dst, dst + ".bak-orig")
                shutil.copy2(os.path.join(out, stem, "campaign", stem + ext), dst)
            print("installed", stem)


if __name__ == "__main__":
    main()
