"""Add rail-bridge objectives where the railway crosses a big river with no road bridge near.

Korea has no rail bridges of its own, so railnet.cpp binds each OSM rail bridge to the
nearest bridge objective within RailBridgeBindKm -- and most long rail crossings have none
(56 of the 68 of 300 m or more). This gives each such crossing a bridge objective of its own,
so the ATM can plan strikes on it, bombs can drop it, repair rebuilds it, and the rail tick
binds to it (it lies right on the track) and cuts the line while it is down.

Per crossing (rail.json routes, a bridge run of --min-m or more with no TYPE_BRIDGE objective
within --clear-km of its middle in any campaign):

  * a new objective CLASS: a class-table row and an OCD row copied from "624 Bridge 5" (a
    straight multi-span bridge), and feature entries (FED) laid out the same way -- ramp,
    600 ft spans, ramp -- along the crossing's real heading and length. Appended to the
    engine's tables (terrdata\\objects) and to the editor's per-campaign copies, which must
    stay row-for-row equal. Named "Rail Bridge NN".
  * a new OBJECTIVE of that class in every base campaign file of every theater on this terrain
    (save0-2.cam and te_new.tac with an objective list of their own; files that borrow one get
    it from those): fixed VU id 7000+k, and campaign ids from the first block free in every file
    (saves included), owner and parent
    of the nearest objective, no links (ground units do not route over a railway bridge).

Saves only store deltas against these lists, keyed by id, so existing saves keep working and
pick the bridges up with their default state.

    python add_rail_bridges.py              # report only
    python add_rail_bridges.py --write      # back up, then add
    python add_rail_bridges.py --rollback   # restore what --write changed
"""

import argparse
import datetime
import glob
import json
import math
import os
import shutil
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from ffcamp import campdb, objectives  # noqa: E402
from ffcamp.workspace import TheaterWorkspace  # noqa: E402

FT_PER_KM = 3279.98
VU_LAST_ENTITY_TYPE = 100
TEMPLATE_NAME = "624 Bridge 5"      # straight N-S bridge: ramp, 4 x 600 ft span, ramp
SPAN_FT = 600.0                     # span centre spacing in the template
RAMP_FT = 440.0                     # ramp centre past the last span centre
NAME_FMT = "Rail Bridge %02d"
VU_ID_BASE = 7000                   # objective namespace is 4..8004 (MAX_NUMBER_OF_OBJECTIVES)
CAMP_ID_BASE = 3800                 # lowest campaign id tried; must stay below 28000 (MAX_CAMP_ENTITIES)
STYPE_BASE = 150                    # classInfo SType for the new classes (unused for bridges)
BACKUP = "_railbridge-backup"

DB_DIRS = [r"terrdata\objects", r"campaign\save\campaigndb",
           r"campaign\Korea2012\campaigndb", r"campaign\korea1980s\campaigndb"]
THEATERS = [r"terrdata\theaterdefinition\korea.tdf",
            r"terrdata\theaterdefinition\korea1980s.tdf",
            r"terrdata\theaterdefinition\korea_2012.tdf"]
BASE_FILES = ("save0.cam", "save1.cam", "save2.cam", "te_new.tac")


# ------------------------------------------------------------------ crossings

def crossings(gamedir, rail_json, min_m, clear_km):
    doc = json.load(open(rail_json, encoding="utf-8"))
    bridges = []
    for rel in THEATERS:
        ws = TheaterWorkspace(gamedir, rel)
        for f in BASE_FILES:
            try:
                cam = ws.objectives(f)
            except Exception:
                continue
            bridges += [(o["x"] + 0.5, o["y"] + 0.5) for o in cam.objectives
                        if o.get("typeName") == "Bridge" and o["id"][0] < VU_ID_BASE]
    out = []
    for r in doc["routes"]:
        p, seg = r["pts"], r.get("seg", "")
        i = 0
        while i < len(seg):
            if seg[i] != "b":
                i += 1
                continue
            j, length = i, 0.0
            while j < len(seg) and seg[j] == "b":
                length += math.dist(p[j], p[j + 1])
                j += 1
            a, b = p[i], p[j]
            mid = ((a[0] + b[0]) / 2, (a[1] + b[1]) / 2)
            near = min((math.dist(mid, q) for q in bridges), default=99.0)
            if length * 1000 >= min_m and near > clear_km:
                # Compass heading from the run's start to its end (0 north, 90 east).
                heading = math.degrees(math.atan2(b[0] - a[0], b[1] - a[1])) % 360
                out.append({"route": r["name"], "mid": mid, "km": length, "heading": heading})
            i = j
    out.sort(key=lambda c: (c["route"], c["mid"]))
    return out


def layout(c, template_fed):
    """FED rows for one crossing: the template's ramp / span / ramp sequence, enough spans
    to cover the run, centred on its middle, rotated to its heading. Offsets are from the
    objective's cell centre (Offset[0] east, Offset[1] north, as GetFeatureOffset reads)."""
    ramp, span = template_fed[0], template_fed[1]
    length_ft = c["km"] * FT_PER_KM
    n = max(1, int(round((length_ft - 2 * RAMP_FT) / SPAN_FT)) + 1)
    cx, cy = math.floor(c["mid"][0]), math.floor(c["mid"][1])
    ox = (c["mid"][0] - (cx + 0.5)) * FT_PER_KM
    oy = (c["mid"][1] - (cy + 0.5)) * FT_PER_KM
    h = math.radians(c["heading"])
    ue, un = math.sin(h), math.cos(h)
    along = [-(n - 1) / 2.0 * SPAN_FT - RAMP_FT] + \
            [(k - (n - 1) / 2.0) * SPAN_FT for k in range(n)] + \
            [(n - 1) / 2.0 * SPAN_FT + RAMP_FT]
    rows = []
    for k, s in enumerate(along):
        src = ramp if k in (0, len(along) - 1) else span
        row = dict(src)
        row["eClass"] = list(src["eClass"])
        # Feature links, as the template chains them: first ramp NEXT_NORM, first span
        # PREV_CRIT|NEXT_CRIT, the other spans NEXT_CRIT|PREV_NORM, last ramp PREV_NORM.
        row["Flags"] = 8 if k == 0 else (4 if k == len(along) - 1 else (3 if k == 1 else 6))
        row["Offset"] = [round(ox + s * ue, 1), round(oy + s * un, 1), 0.0]
        # As the template (laid north, heading 0): the near ramp faces along the heading,
        # the spans and the far ramp face back (heading + 180).
        face = c["heading"] if k == 0 else c["heading"] + 180
        row["Facing"] = int(round(((face + 180) % 360) - 180))
        rows.append(row)
    return (cx, cy), rows


# ------------------------------------------------------------------ objective records

def record(version, type_id, vu, cell, owner, camp_id, obj_flags, base_flags, nfeat, priority,
           name_id, parent, supply, fuel):
    b = bytearray()
    b += struct.pack("<h", type_id)
    b += struct.pack("<II", vu, 0)                    # VU_ID: num, creator
    b += struct.pack("<H", type_id)                   # entityType
    b += struct.pack("<hh", cell[0], cell[1])
    if version >= 70:
        b += struct.pack("<f", 0.0)                   # z
    b += struct.pack("<I", 0)                         # spotTime
    b += struct.pack("<hh", 0, base_flags)            # spotted, baseFlags
    b += struct.pack("<B", owner)
    b += struct.pack("<h", camp_id)
    b += struct.pack("<I", 0)                         # lastRepair
    b += struct.pack("<I" if version > 1 else "<h", obj_flags)
    b += struct.pack("<BBB", supply, fuel, 0)         # supply, fuel, losses
    fsize = (nfeat + 3) // 4
    b += struct.pack("<B", fsize) + bytes(fsize)      # every feature intact
    b += struct.pack("<B", priority)
    b += struct.pack("<h", name_id)
    b += struct.pack("<II", parent[0], parent[1])
    b += struct.pack("<B", owner)                     # firstOwner
    b += struct.pack("<B", 0)                         # no links
    if version >= 20:
        b += struct.pack("<B", 0)                     # no radar ranges
    return bytes(b)


# ------------------------------------------------------------------ backup / rollback

def backup_dir(gamedir):
    return os.path.join(gamedir, "campaign", BACKUP)


def rollback(gamedir):
    runs = sorted(glob.glob(os.path.join(backup_dir(gamedir), "*", "manifest.json")))
    if not runs:
        raise SystemExit("nothing to roll back")
    man = json.load(open(runs[-1]))
    base = os.path.dirname(runs[-1])
    for i, rel in enumerate(man["files"]):
        shutil.copy2(os.path.join(base, "%03d_%s" % (i, os.path.basename(rel))),
                     os.path.join(gamedir, rel))
        print("  restored %s" % rel)
    os.rename(runs[-1], runs[-1] + ".rolled-back")
    print("rail bridges removed")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--gamedir", default=r"C:\FreeFalcon6")
    ap.add_argument("--rail", default=r"terrdata\korea\rail.json")
    ap.add_argument("--min-m", type=float, default=300.0)
    ap.add_argument("--clear-km", type=float, default=2.0)
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--rollback", action="store_true")
    args = ap.parse_args()
    g = args.gamedir

    if args.rollback:
        rollback(g)
        return

    # Class tables: all copies must agree before anything is appended.
    dbs = []
    for rel in DB_DIRS:
        d = os.path.join(g, rel)
        if os.path.isdir(d):
            db = campdb.CampaignDB(d)
            if db.table("class") and db.table("objective") and db.table("featureentry"):
                dbs.append((rel, db))
    sizes = {(len(db.table("class").rows), len(db.table("objective").rows),
              len(db.table("featureentry").rows)) for _r, db in dbs}
    if len(sizes) != 1:
        raise SystemExit("class tables differ between %s: %s" % ([r for r, _ in dbs], sizes))
    db0 = dbs[0][1]
    if any(r["Name"].startswith("Rail Bridge") for r in db0.table("objective").rows):
        raise SystemExit("rail bridges are already in the class tables (--rollback first)")
    ocd = db0.table("objective").rows
    t_ocd = [i for i, r in enumerate(ocd) if r["Name"] == TEMPLATE_NAME][0]
    t_cls = ocd[t_ocd]["Index"]
    fed = db0.table("featureentry").rows
    t_fed = fed[ocd[t_ocd]["FirstFeature"]:ocd[t_ocd]["FirstFeature"] + ocd[t_ocd]["Features"]]
    # The template is listed far end first; take a ramp and a span from it.
    template_fed = [t_fed[-1], t_fed[1]]
    used_stypes = {r["classInfo_"][3] for r in db0.table("class").rows if r["classInfo_"][:3] == [3, 4, 6]}

    cs = crossings(g, os.path.join(g, args.rail), args.min_m, args.clear_km)
    print("%d rail crossings of %d m or more with no bridge objective within %.1f km:"
          % (len(cs), args.min_m, args.clear_km))
    for k, c in enumerate(cs):
        print("  %-16s %-14s at (%.1f E, %.1f N) km  %4.0f m  heading %3.0f"
              % (NAME_FMT % (k + 1), c["route"], c["mid"][0], c["mid"][1], c["km"] * 1000, c["heading"]))
    if len(cs) > 99 or VU_ID_BASE + len(cs) > 8004:
        raise SystemExit("too many crossings for the id ranges")
    stypes = [s for s in range(STYPE_BASE, 256) if s not in used_stypes][:len(cs)]
    if len(stypes) < len(cs):
        raise SystemExit("not enough free bridge STypes")

    # The VU ids (objective namespace) must be free in every objective list, and the
    # campaign ids free in every file, saves included: units take campaign ids all over
    # the range during play. A block free today is free for good -- the engine's
    # FindUniqueID skips ids that loaded entities hold, and these bridges are loaded.
    files, camp_used = [], set()
    for rel in THEATERS:
        ws = TheaterWorkspace(g, rel)
        for f in ws.campaign_files():
            try:
                cam = ws.objectives(f["file"])
            except Exception:
                continue
            for o in cam.objectives:
                if VU_ID_BASE <= o["id"][0] < VU_ID_BASE + len(cs):
                    raise SystemExit("%s: objective id %s already used" % (f["file"], o["id"]))
                camp_used.add(o["campId"])
            try:
                camp_used |= {u.get("campId", 0) for u in ws.units(f["file"]).units}
            except Exception:
                pass
            if f["file"] in BASE_FILES and cam.member("obj") is not None and cam.objectives:
                files.append((rel, ws, f["file"], cam))
    camp_base = next((b for b in range(CAMP_ID_BASE, 28000 - len(cs))
                      if not any(i in camp_used for i in range(b, b + len(cs)))), None)
    if camp_base is None:
        raise SystemExit("no free block of %d campaign ids" % len(cs))
    print("campaign ids %d..%d (free in every file), objective ids %d..%d"
          % (camp_base, camp_base + len(cs) - 1, VU_ID_BASE, VU_ID_BASE + len(cs) - 1))
    print("objective lists to extend: %s" % ", ".join("%s/%s" % (os.path.basename(r), f) for r, _w, f, _c in files))

    if not args.write:
        print("dry run: nothing written. Add --write to back up and add them.")
        return

    # Backups of everything this touches, with a restore script.
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    bdir = os.path.join(backup_dir(g), stamp)
    os.makedirs(bdir)
    touched = []
    for _rel, db in dbs:
        for t in ("class", "objective", "featureentry"):
            touched.append(os.path.relpath(db.table(t).path, g))
    for _rel, _ws, _f, cam in files:
        touched.append(os.path.relpath(cam.path, g))
    for i, rel in enumerate(touched):
        shutil.copy2(os.path.join(g, rel), os.path.join(bdir, "%03d_%s" % (i, os.path.basename(rel))))
    with open(os.path.join(bdir, "restore.bat"), "w", newline="\r\n") as f:
        f.write("@echo off\nrem Removes the rail-bridge objectives. Close the game and the campaign editor first.\n")
        f.write("cd /d \"%~dp0\"\n")
        for i, rel in enumerate(touched):
            f.write("copy /y \"%03d_%s\" \"..\\..\\..\\%s\" >nul || goto fail\n" % (i, os.path.basename(rel), rel))
        f.write("echo Restored.\npause\nexit /b 0\n:fail\necho Restore FAILED.\npause\nexit /b 1\n")
    json.dump({"when": stamp, "files": touched, "crossings": cs}, open(os.path.join(bdir, "manifest.json"), "w"), indent=1)

    # Class tables.
    n_cls = len(db0.table("class").rows)
    n_ocd = len(ocd)
    n_fed = len(fed)
    plan = []
    first = n_fed
    for k, c in enumerate(cs):
        cell, rows = layout(c, template_fed)
        plan.append((cell, rows, n_cls + k, n_ocd + k, first))
        first += len(rows)
    for _rel, db in dbs:
        ct, ot, ft = db.table("class"), db.table("objective"), db.table("featureentry")
        for k, (cell, rows, ci, oi, fi) in enumerate(plan):
            crow = dict(ct.rows[t_cls])
            crow["classInfo_"] = list(crow["classInfo_"])
            crow["classInfo_"][3] = stypes[k]
            crow["dataPtr"] = oi
            ct.rows.append(crow)
            ct.raw.append(None)
            orow = dict(ocd[t_ocd])
            orow["Index"] = ci
            orow["Name"] = NAME_FMT % (k + 1)
            orow["Features"] = len(rows)
            orow["FirstFeature"] = fi
            ot.rows.append(orow)
            ot.raw.append(None)
            for r in rows:
                ft.rows.append(dict(r))
                ft.raw.append(None)
        ct.dirty = ot.dirty = ft.dirty = True
        db.save_dirty()

    # Objective records.
    for rel, ws, fname, cam in files:
        names = ws.name_table()
        objs = cam.objectives
        raw = cam.objectives_raw
        tmpl = [o for o in objs if o.get("typeName") == "Bridge"]
        flags = max(set(o["objFlags"] for o in tmpl), key=[o["objFlags"] for o in tmpl].count)
        add = b""
        for k, (cell, rows, ci, oi, fi) in enumerate(plan):
            near = min(objs[:len(objs)], key=lambda o: (o["x"] - cell[0]) ** 2 + (o["y"] - cell[1]) ** 2)
            parent = near["parent"] if near["parent"][0] else near["id"]
            add += record(cam.version, ci + VU_LAST_ENTITY_TYPE, VU_ID_BASE + k, cell, near["owner"],
                          camp_base + k, flags, 2, len(rows), 20, 0, parent, 5, 10)
        cam.objectives_raw = raw + add
        cam.objectives = objs + [{"_added": True}] * len(plan)
        cam.objectives_dirty = True
        cam.dirty = True

        class NoBackup:
            def keep(self, path):
                return None
        for w in ws.save(NoBackup()):
            print("  wrote %s" % w)

    # Read everything back.
    for _rel, db in dbs:
        back = campdb.CampaignDB(db.dir)
        o = back.table("objective").rows
        assert o[-1]["Name"] == NAME_FMT % len(cs) and o[-1]["FirstFeature"] + o[-1]["Features"] == len(back.table("featureentry").rows)
    for rel, _ws, fname, _cam in files:
        ws2 = TheaterWorkspace(g, rel)
        objs = ws2.objectives(fname).objectives
        got = [o for o in objs if VU_ID_BASE <= o["id"][0] < VU_ID_BASE + len(cs)]
        if len(got) != len(cs) or any(o["typeName"] != "Bridge" for o in got):
            raise SystemExit("read-back failed for %s -- restore with %s" % (fname, bdir))
        print("  check %s/%s: %d rail bridges, %d objectives" % (os.path.basename(rel), fname, len(got), len(objs)))
    print("added. Undo: python %s --rollback   (or run %s\\restore.bat)" % (os.path.basename(__file__), bdir))


if __name__ == "__main__":
    main()
