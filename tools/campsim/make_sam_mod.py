"""Stronger SAMs for the war: python make_sam_mod.py <variant> [src=chnb] [out=chnb-<variant>]

Variants (comma-joined, e.g. sites,china):
  sites   one SA-10 battalion (class 799) and one SA-5 battalion (class 733) on every North Korean factory, chemical plant, nuclear/power
          plant and refinery -- 74 sites, 148 battalions -- owner DPRK, standing alone (U_PARENT), air-defence orders
  china   every Chinese air-defence battalion becomes SA-10 (6 of the 12 already are), and one more SA-10 battalion stands with every
          fourth Chinese ground battalion
  sa5     the same as sites but SA-5 only, half the number (every other site)
  dummy   the control for sites: two KS-19 anti-aircraft gun battalions (class 734, 100 mm guns the jets fly over) on every site -- the same
          number of extra battalions standing on the same objectives, with almost no air-defence value
Writes Campaign/SAVE/<out>.cam beside the source.  Nothing else about the scenario changes.  Used by the strike experiments in supply_spread.py."""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "campaign-editor"))
from ffcamp import entities, workspace  # noqa: E402

GAME = r"C:\FreeFalcon6"
TDF = "terrdata/theaterdefinition/korea.tdf"
SA10, SA5, KS19 = 799, 733, 734          # unit class-table rows: "Air Defense" with SA-10 / SA-5 vehicles (campsim class survey)
GORD_AIRDEFENSE = 9           # entities.GROUND_ORDERS index of "Air defence"
DPRK, PRC = 6, 5
SITE_WORDS = ("Factory", "Power", "Refinery", "Chemical", "Nuclear")


def unit_row(ws, rows, utbl, ci):
    p = rows[ci]["dataPtr"]
    return utbl.rows[p]


def main():
    variant = sys.argv[1]
    src = sys.argv[2] if len(sys.argv) > 2 else "chnb"
    out = sys.argv[3] if len(sys.argv) > 3 else "%s-%s" % (src, variant.replace(",", "-"))
    want = set(variant.split(","))
    ws = workspace.Session(GAME).workspace(TDF)
    cam = ws.units(src + ".cam")
    objs = ws.objectives(src + ".cam").objectives or []
    rows = ws.db.class_rows()
    utbl = ws.db.table("unit")
    base = entities.base_offsets(cam.version)
    raw = bytearray(cam.units_raw)

    used_vu = {u["id"][0] for u in cam.units}
    used_camp = {u["campId"] for u in cam.units} | {o["campId"] for o in objs}
    name = [700]

    def ids():
        vu = entities.FIRST_NON_VOLATILE_ID
        while vu in used_vu:
            vu += 1
        c = 1
        while c in used_camp:
            c += 1
        used_vu.add(vu)
        used_camp.add(c)
        return vu, c

    added = []

    def add(ci, x, y, owner):
        vu, c = ids()
        name[0] += 1
        rec = entities.build_unit("battalion", cam.version, ci + entities.VU_LAST_ENTITY_TYPE, x, y, owner, vu, c,
                                  roster=entities.roster_from_class(unit_row(ws, rows, utbl, ci)), orders=GORD_AIRDEFENSE,
                                  unit_flags=entities.U_PARENT, name_id=name[0])
        added.append(rec)

    n_site = n_up = n_china = 0
    sites = [o for o in objs if o["owner"] == DPRK and any(t in o["typeName"] for t in SITE_WORDS)]
    if "sites" in want:
        for o in sites:
            add(SA10, o["x"], o["y"], DPRK)
            add(SA5, o["x"], o["y"], DPRK)
            n_site += 2
    if "dummy" in want:
        for o in sites:
            add(KS19, o["x"], o["y"], DPRK)
            add(KS19, o["x"], o["y"], DPRK)
            n_site += 2
    if "sa5" in want:
        for i, o in enumerate(sites):
            if i % 2 == 0:
                add(SA5, o["x"], o["y"], DPRK)
                n_site += 1
    if "china" in want:
        # in place: the record keeps its id, place, parent and orders; only the class and the roster change
        for u in cam.units:
            if u["kind"] != "battalion" or u["owner"] != PRC:
                continue
            row = unit_row(ws, rows, utbl, u["classIndex"])
            if row["Role"] != 5 or u["classIndex"] == SA10:
                continue
            s0 = u["_span"][0]
            tid = SA10 + entities.VU_LAST_ENTITY_TYPE
            struct.pack_into("<h", raw, s0, tid)
            struct.pack_into("<H", raw, s0 + 10, tid)
            struct.pack_into("<I", raw, s0 + base["campId"] + 2 + 4, entities.roster_from_class(unit_row(ws, rows, utbl, SA10)))
            n_up += 1
        grd = [u for u in cam.units if u["kind"] == "battalion" and u["owner"] == PRC and unit_row(ws, rows, utbl, u["classIndex"])["Role"] != 5]
        for i, u in enumerate(grd):
            if i % 4 == 0:
                add(SA10, u["x"], u["y"], PRC)
                n_china += 1
    stream = bytes(raw) + b"".join(added)
    cam.units_raw = stream
    cam.units = entities.walk_units(stream, cam.version, rows)
    cam.members[cam._member_name("uni")] = entities.encode_units(stream, len(cam.units))
    cam.path = os.path.join(ws.campaign_dir, out + ".cam")
    cam.save()
    print("%s -> %s: +%d site battalions, %d Chinese AD battalions converted to SA-10, +%d SA-10 with Chinese ground units (%d units)"
          % (src, cam.path, n_site, n_up, n_china, len(cam.units)))


if __name__ == "__main__":
    main()
