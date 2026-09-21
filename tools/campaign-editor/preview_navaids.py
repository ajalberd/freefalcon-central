"""Render the NAVAIDS kneeboard page offline, from the data the sim reads.

The in-game page walks the objective list, the TACAN list and the point-header
chain hanging off each objective's class. This does the same from the files, so
the columns can be checked against something real before they reach a cockpit.
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from ffcamp import campdb, campfile, names, objectives, tacan  # noqa: E402

GAME = r"C:\FreeFalcon6"
RUNWAY_PT = 1


def runway_pair(headers, pt_index):
    """Both ends of strip 0, walking the chain the way the sim does."""
    first = second = -1
    rw, seen = pt_index, set()

    while rw and rw not in seen and 0 <= rw < len(headers):
        seen.add(rw)
        h = headers[rw]
        if h["type"] == RUNWAY_PT and h["runwayNum"] == 0:
            if first < 0:
                first = h["data"]
            elif h["data"] != first:
                second = h["data"]
                break
        rw = h["nextHeader"]

    if second >= 0 and second < first:
        first, second = second, first
    return first, second


def designator(hdg):
    d = ((hdg % 360) + 5) // 10
    return 36 if (d == 0 or d > 36) else d


def main(camp_rel, cam_name):
    camp = os.path.join(GAME, camp_rel)
    db = campdb.CampaignDB(os.path.join(camp, "CampaignDB"))
    phd = db.table("ptheader")
    headers = phd.rows if phd else []
    nametab = names.load(camp)
    cls_names = db.name_index()
    stations = tacan.load(camp)

    cam = campfile.CampaignFile.load(os.path.join(camp, cam_name))
    objs, _raw = objectives.decode_objectives(
        cam.member("obj"), cam.version, db.class_rows())

    hdr = cam.header
    bx = getattr(hdr, "bullseye_x", None)
    by = getattr(hdr, "bullseye_y", None)
    if bx is None:
        fields = getattr(hdr, "fields", {}) or {}
        bx = fields.get("BullseyeX", 0)
        by = fields.get("BullseyeY", 0)
    teams = [t["name"] for t in (hdr.teams if hdr else [])]

    rows = []
    for o in objs:
        if o["typeName"] not in ("Airbase", "Airstrip"):
            continue

        name = (nametab.get(o["nameId"], "") or "").strip()
        if name in ("", "Nowhere", "New"):
            name = cls_names.get(o["classIndex"], "")

        st = stations.get(o["campId"])
        _t, cls_row = db.data_row(o["classIndex"])
        pt = (cls_row or {}).get("PtDataIndex", 0)
        rw1, rw2 = runway_pair(headers, pt) if pt else (-1, -1)

        dx, dy = o["x"] - bx, o["y"] - by
        rows.append(dict(
            name=name,
            tcn=(st or {}).get("label"),
            ils=(st or {}).get("ils", 0.0),
            rw1=rw1, rw2=rw2,
            brg=int(math.degrees(math.atan2(dy, dx))) % 360,
            rng=int(math.hypot(dx, dy)),
            owner=o["owner"]))

    rows.sort(key=lambda r: r["rng"])

    print("%s / %s   bullseye %s,%s" % (camp_rel, cam_name, bx, by))
    print("BASE          TCN   ILS     RWY  BULLS   NM")
    for r in rows[:26]:
        if r["rw1"] >= 0 and r["rw2"] >= 0:
            rwy = "%02d/%02d" % (designator(r["rw1"]), designator(r["rw2"]))
        elif r["rw1"] >= 0:
            rwy = "   %02d" % designator(r["rw1"])
        else:
            rwy = "    -"
        print("%-12.12s %4s %6s %5s %03d/%-3d %4d  %s"
              % (r["name"], r["tcn"] or "  - ",
                 ("%6.2f" % r["ils"]) if r["ils"] else "     -",
                 rwy, r["brg"], r["rng"], r["rng"],
                 teams[r["owner"]] if r["owner"] < len(teams) else r["owner"]))

    print("\n%d airbases: %d TACAN, %d ILS, %d runway headings, %d with both ends"
          % (len(rows),
             sum(1 for r in rows if r["tcn"]),
             sum(1 for r in rows if r["ils"]),
             sum(1 for r in rows if r["rw1"] >= 0),
             sum(1 for r in rows if r["rw2"] >= 0)))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else r"campaign\SAVE",
         sys.argv[2] if len(sys.argv) > 2 else "save0.cam")
