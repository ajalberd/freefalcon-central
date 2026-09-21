#!/usr/bin/env python3
"""Local web server for the FreeFalcon campaign editor.

Python standard library only -- no pip, no virtualenv. It binds to loopback,
serves web/ and a small JSON API over the modules in ffcamp/, and opens a
browser at itself.

    python server.py [--gamedir C:\\FreeFalcon6] [--port 8765] [--no-browser]

Everything it touches lives under the game directory. Files are backed up into
<campaign dir>/_editor-backup/<timestamp>/ the first time a session writes
them.
"""

import argparse
import json
import mimetypes
import os
import posixpath
import sys
import threading
import webbrowser
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse, parse_qs, quote

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from ffcamp import (campfile, camptext, entities, objectives,  # noqa: E402
                    records, terrain, theater, triggers, uiart,
                    workspace)

# CountryListEnum, campaign/include/team.h. classInfo_[VU_OWNER] carries this,
# and in the shipped campaigns the country index and the team index coincide.
COUNTRIES = ["-", "US", "South Korea", "Japan", "Russia", "China",
             "North Korea", "Gorn"]

# DEFAULT.wch uses "Nowhere" as its null place name; treat it as unnamed and
# fall back to the class-table name.
NULL_PLACE_NAMES = {"", "Nowhere", "New"}


def place_name(nametab, names, obj):
    got = nametab.get(obj["nameId"], "").strip()
    if got in NULL_PLACE_NAMES:
        return names.get(obj["classIndex"], "")
    return got

WEB = os.path.join(HERE, "web")
SESSION = None


class ApiError(Exception):
    def __init__(self, message, status=400):
        super().__init__(message)
        self.status = status


# --- API ---------------------------------------------------------------------

def api_state(_q, _body):
    return {
        "gamedir": SESSION.gamedir,
        "gamedir_ok": os.path.isfile(
            os.path.join(SESSION.gamedir, theater.THEATER_LIST)),
        "theaters": SESSION.theaters(),
        "pending": SESSION.pending(),
    }


def api_set_gamedir(_q, body):
    global SESSION
    path = body.get("gamedir", "")
    if not os.path.isdir(path):
        raise ApiError("%s is not a directory" % path)
    if SESSION.pending():
        raise ApiError("There are unsaved changes. Save or discard them first.")
    SESSION = workspace.Session(path)
    return api_state(None, None)


def _ws(q):
    tdf = (q.get("theater") or [""])[0]
    if not tdf:
        raise ApiError("no theater given")
    try:
        return SESSION.workspace(tdf)
    except ValueError as exc:
        raise ApiError(str(exc), 404)


def api_theater(q, _body):
    ws = _ws(q)
    db = ws.db
    tables = []
    for name in db.present():
        tbl = db.table(name)
        tables.append({
            "name": name,
            "title": tbl.spec.title,
            "file": os.path.basename(tbl.path),
            "rows": len(tbl.rows),
            "bytes": len(tbl.rows) * tbl.spec.size,
            "dirty": tbl.dirty,
        })
    return {
        "tdf": ws.tdf_rel,
        "name": ws.tdf.name,
        "desc": ws.tdf.get("desc"),
        "values": {k: ws.tdf.get(k) for k in theater.KEYS + theater.EXTRA_KEYS
                   if ws.tdf.get(k)},
        "campaign_dir": os.path.relpath(ws.campaign_dir, SESSION.gamedir),
        "db_dir": os.path.relpath(ws.db.dir, SESSION.gamedir),
        "tables": tables,
        "campaigns": ws.campaign_files(),
        "pending": ws.pending(),
    }


def _row_view(db, spec, row, index):
    out = {"_i": index}
    for col in workspace.SUMMARY_COLUMNS.get(spec.name, spec.names[:6]):
        if col in row:
            out[col] = row[col]
    if spec.name == "class":
        out["_name"] = db.class_name(index)
        out["_dtype"] = records.DTYPE_NAMES.get(row["dataType"], "?")
        info = row["classInfo_"]
        out["_domain"] = records.DOMAIN_NAMES.get(info[0], str(info[0]))
        out["_class"] = records.CLASS_NAMES.get(info[1], str(info[1]))
    elif spec.label:
        out["_name"] = row.get(spec.label, "")
    else:
        out["_name"] = "%s %d" % (spec.title.rstrip("s"), index)
    return out


def api_table(q, _body):
    ws = _ws(q)
    name = (q.get("table") or [""])[0]
    if name not in records.BY_NAME:
        raise ApiError("unknown table %r" % name, 404)
    tbl = ws.db.table(name)
    if tbl is None:
        raise ApiError("%s has no %s file" % (ws.tdf.name, name), 404)

    needle = (q.get("q") or [""])[0].strip().lower()
    offset = int((q.get("offset") or ["0"])[0])
    limit = min(500, int((q.get("limit") or ["200"])[0]))

    views = [_row_view(ws.db, tbl.spec, row, i)
             for i, row in enumerate(tbl.rows)]
    if needle:
        def hit(v):
            if needle in str(v.get("_name", "")).lower():
                return True
            return any(needle in str(val).lower() for val in v.values())
        views = [v for v in views if hit(v)]

    changed = set()
    for i, row in enumerate(tbl.rows):
        if tbl.raw[i] is None:
            changed.add(i)
        elif tbl.spec.pack(row, tbl.raw[i]) != tbl.raw[i]:
            changed.add(i)
    for v in views:
        v["_changed"] = v["_i"] in changed

    return {
        "table": name,
        "title": tbl.spec.title,
        "file": os.path.basename(tbl.path),
        "size": tbl.spec.size,
        "total": len(tbl.rows),
        "matched": len(views),
        "columns": workspace.SUMMARY_COLUMNS.get(name, tbl.spec.names[:6]),
        "rows": views[offset:offset + limit],
        "dirty": tbl.dirty,
        "changed": len(changed),
    }


def api_row(q, _body):
    ws = _ws(q)
    name = (q.get("table") or [""])[0]
    index = int((q.get("index") or ["0"])[0])
    tbl = ws.db.table(name)
    if tbl is None or not (0 <= index < len(tbl.rows)):
        raise ApiError("no row %s[%d]" % (name, index), 404)

    row = dict(tbl.rows[index])
    original = tbl.spec.unpack(tbl.raw[index]) if tbl.raw[index] else None
    resolved = {}
    if name == "class":
        tname, drow = ws.db.data_row(index)
        resolved["dataTable"] = tname
        resolved["dataName"] = ws.db.class_name(index)
    if name in ("unit",):
        names = ws.db.name_index()
        resolved["VehicleType"] = [names.get(v, "") for v in row["VehicleType"]]
    if name == "vehicle":
        # Weapon[hp] is a *row* index: into FALCON4.WLD when Weapons[hp] is
        # 255 (the "this is a weapon list" marker LoadoutWeapons checks), into
        # FALCON4.WCD otherwise. It is not a class-table id.
        wtbl = ws.db.table("weapon")
        ltbl = ws.db.table("weaponlist")
        out = []
        for hp, wid in enumerate(row["Weapon"]):
            count = row["Weapons"][hp]
            if not wid:
                out.append("")
            elif count == 255:
                nm = (ltbl.rows[wid]["Name"]
                      if ltbl and 0 <= wid < len(ltbl.rows) else "?")
                out.append("list: " + nm)
            else:
                out.append(wtbl.rows[wid]["Name"]
                           if wtbl and 0 <= wid < len(wtbl.rows) else "?")
        resolved["Weapon"] = out

    return {
        "table": name,
        "index": index,
        "title": _row_view(ws.db, tbl.spec, row, index).get("_name", ""),
        "columns": tbl.spec.describe(),
        "labels": workspace.ARRAY_LABELS,
        "values": row,
        "original": original,
        "resolved": resolved,
    }


def api_edit(_q, body):
    ws = SESSION.workspace(body["theater"])
    name = body["table"]
    index = int(body["index"])
    tbl = ws.db.table(name)
    if tbl is None or not (0 <= index < len(tbl.rows)):
        raise ApiError("no row %s[%d]" % (name, index), 404)

    spec = tbl.spec
    row = tbl.rows[index]
    for field, value in body.get("values", {}).items():
        f = spec.by_name.get(field)
        if f is None:
            raise ApiError("%s has no field %r" % (name, field))
        row[field] = _coerce(f, value)

    tbl.dirty = tbl.spec.pack(row, tbl.raw[index]) != tbl.raw[index] or tbl.dirty
    return {"ok": True, "values": row, "dirty": tbl.dirty}


def _coerce(f, value):
    if f.kind == "str":
        return str(value)
    if f.count > 1:
        vals = list(value)
        if f.kind == "f32":
            return [float(x or 0) for x in vals]
        return [int(x or 0) for x in vals]
    if f.kind == "f32":
        return float(value or 0)
    return int(value or 0)


def api_add_row(_q, body):
    """Append a row, optionally copied from an existing one."""
    ws = SESSION.workspace(body["theater"])
    name = body["table"]
    tbl = ws.db.table(name)
    if tbl is None:
        raise ApiError("no %s table" % name, 404)
    src = body.get("copyFrom")
    if src is None:
        row = tbl.blank_row()
    else:
        src = int(src)
        if not (0 <= src < len(tbl.rows)):
            raise ApiError("no row to copy at %d" % src)
        row = dict(tbl.rows[src])
        if tbl.spec.label and row.get(tbl.spec.label):
            row[tbl.spec.label] = (str(row[tbl.spec.label]) + " copy")
    tbl.rows.append(row)
    tbl.raw.append(None)
    tbl.dirty = True
    return {"ok": True, "index": len(tbl.rows) - 1, "total": len(tbl.rows)}


def api_revert_row(_q, body):
    ws = SESSION.workspace(body["theater"])
    tbl = ws.db.table(body["table"])
    index = int(body["index"])
    if tbl is None or not (0 <= index < len(tbl.rows)):
        raise ApiError("no such row", 404)
    if tbl.raw[index] is None:
        raise ApiError("that row was added in this session; it has nothing to "
                       "revert to")
    tbl.rows[index] = tbl.spec.unpack(tbl.raw[index])
    tbl.dirty = any(
        tbl.raw[i] is None or tbl.spec.pack(r, tbl.raw[i]) != tbl.raw[i]
        for i, r in enumerate(tbl.rows))
    return {"ok": True, "values": tbl.rows[index]}


def api_campaign(q, _body):
    ws = _ws(q)
    name = (q.get("file") or [""])[0]
    cam = ws.campaign(name)
    hdr = cam.header
    if hdr is None:
        raise ApiError("%s has no campaign header section" % name)
    return {
        "file": name,
        "version": cam.version,
        "members": cam.summary(),
        "fields": {k: v for k, v in hdr.fields.items()
                   if not k.startswith("_")},
        "editable": sorted(campfile.EDITABLE),
        "docs": campfile.FIELD_DOC,
        "teamDocs": campfile.TEAM_FIELD_DOC,
        "teams": hdr.teams,
        "squadrons": hdr.squadrons[:400],
        "squadronCount": len(hdr.squadrons),
        "events": [len(hdr.events[0]), len(hdr.events[1])],
        "dirty": getattr(cam, "dirty", False),
    }


def api_campaign_edit(_q, body):
    ws = SESSION.workspace(body["theater"])
    cam = ws.campaign(body["file"])
    hdr = cam.header
    for key, value in body.get("fields", {}).items():
        if key not in campfile.EDITABLE:
            raise ApiError("%s is derived by the engine and is not editable"
                           % key)
        current = hdr.fields[key]
        hdr.fields[key] = str(value) if isinstance(current, str) else int(value)
    for entry in body.get("teams", []):
        t = hdr.teams[int(entry["index"])]
        for key in ("flag", "colour"):
            if key in entry:
                t[key] = int(entry[key])
        for key in ("name", "motto"):
            if key in entry:
                t[key] = str(entry[key])
    cam.header_dirty = True
    cam.dirty = True
    return {"ok": True}


# --- map ---------------------------------------------------------------------

# Grid coordinates are kilometres. ConvertGridToSim in campaign/camplib/find.cpp
# maps grid y onto sim north and grid x onto sim east, so x runs west->east and
# y runs south->north. Kneemap.gif is one pixel per grid km, top row north.
def api_map(q, _body):
    ws = _ws(q)
    name = (q.get("file") or [""])[0]
    cam = ws.units(name)
    ws.objectives(name)
    hdr = cam.header
    nametab = ws.name_table()
    names = ws.db.name_index()

    art = ws.ui_art()
    utbl = ws.db.table("unit")
    otbl = ws.db.table("objective")

    def icon_for(class_index, table):
        """The IconIndex of a class-table row, resolved to an icon name."""
        if not art:
            return ""
        rows = ws.db.class_rows()
        if not (0 <= class_index < len(rows)):
            return ""
        ptr = rows[class_index]["dataPtr"]
        if not table or not (0 <= ptr < len(table.rows)):
            return ""
        return art.icon_name(table.rows[ptr].get("IconIndex", 0))

    units = []
    for u in cam.units:
        units.append({
            "n": u["_n"],
            "x": u["x"], "y": u["y"],
            "owner": u["owner"],
            "kind": u["kind"],
            "type": u["classIndex"],
            "name": names.get(u["classIndex"], ""),
            "icon": icon_for(u["classIndex"], utbl),
            "campId": u["campId"],
            "wp": len(u.get("waypoints", [])),
        })

    stations = ws.tacan()
    objs = []
    for o in cam.objectives:
        objs.append({
            "n": o["_n"],
            "x": o["x"], "y": o["y"],
            "owner": o["owner"],
            "cat": o["category"],
            "t": o["objType"],
            "type": o["typeName"],
            "name": place_name(nametab, names, o),
            "icon": icon_for(o["classIndex"], otbl),
            "tacan": stations.get(o["campId"], {}).get("label", ""),
        })

    teams = []
    for i, t in enumerate(hdr.teams if hdr else []):
        teams.append({"index": i, "name": t["name"], "flag": t["flag"],
                      "colour": t["colour"]})

    return {
        "file": name,
        "width": (hdr.fields["TheaterSizeX"] if hdr else 1024) or 1024,
        "height": (hdr.fields["TheaterSizeY"] if hdr else 1024) or 1024,
        "bullseye": [hdr.fields["BullseyeX"], hdr.fields["BullseyeY"]] if hdr
                    else [0, 0],
        "hasImage": ws.map_image() is not None,
        "hasTerrain": ws.terrain() is not None,
        "tacanCount": sum(1 for o in cam.objectives
                          if o["campId"] in stations),
        "teams": teams,
        "units": units,
        "objectives": objs,
        "objectivesFrom": cam.objectives_from,
        "canEditObjectives": cam.member("obj") is not None,
        "categories": objectives.CATEGORY_LABELS,
        "orders": entities.GROUND_ORDERS,
        "placeable": list(entities.PLACEABLE),
        "error": cam.units_error,
        "objectivesError": cam.objectives_error,
        "dirty": bool(getattr(cam, "units_dirty", False)),
    }


def api_map_image(q, _body):
    """Handled specially in do_GET -- it returns an image, not JSON."""
    raise ApiError("internal", 500)


def api_unit(q, _body):
    ws = _ws(q)
    name = (q.get("file") or [""])[0]
    n = int((q.get("n") or ["0"])[0])
    cam = ws.units(name)
    if not (0 <= n < len(cam.units)):
        raise ApiError("no unit %d" % n, 404)
    u = cam.units[n]

    names = ws.db.name_index()
    utbl = ws.db.table("unit")
    class_row = ws.db.class_rows()[u["classIndex"]]
    _tname, drow = ws.db.data_row(u["classIndex"])

    composition = []
    if drow and "VehicleType" in drow:
        for i, vt in enumerate(drow["VehicleType"]):
            count = drow["NumElements"][i]
            if vt and count:
                composition.append({"slot": i, "type": vt, "count": count,
                                    "name": names.get(vt, "")})

    return {
        "n": n,
        "kind": u["kind"],
        "name": names.get(u["classIndex"], ""),
        "editable": sorted(entities.PATCHABLE),
        "values": {k: v for k, v in u.items()
                   if k not in ("waypoints", "stores", "pilots", "_span")},
        "waypoints": u.get("waypoints", []),
        "composition": composition,
        "unitRow": drow,
        "classInfo": class_row["classInfo_"],
        "span": u["_span"],
    }


def api_unit_edit(_q, body):
    ws = SESSION.workspace(body["theater"])
    name = body["file"]
    cam = ws.units(name)
    n = int(body["n"])
    if not (0 <= n < len(cam.units)):
        raise ApiError("no unit %d" % n, 404)
    u = cam.units[n]

    raw = cam.units_raw
    for field, value in body.get("values", {}).items():
        if field not in entities.PATCHABLE:
            raise ApiError("%s is not editable from the map" % field)
        raw = entities.patch_unit(raw, u, cam.version, field, value)
        u[field] = int(value)
    cam.units_raw = raw
    cam.units_dirty = True
    cam.dirty = True
    return {"ok": True, "values": {k: u[k] for k in entities.PATCHABLE}}


def api_objective(q, _body):
    ws = _ws(q)
    name = (q.get("file") or [""])[0]
    n = int((q.get("n") or ["0"])[0])
    cam = ws.objectives(name)
    if not (0 <= n < len(cam.objectives)):
        raise ApiError("no objective %d" % n, 404)
    o = cam.objectives[n]
    nametab = ws.name_table()
    names = ws.db.name_index()
    _t, drow = ws.db.data_row(o["classIndex"])

    features = []
    if drow:
        ftbl = ws.db.table("featureentry")
        first, count = drow["FirstFeature"], drow["Features"]
        if ftbl and count:
            for i in range(first, min(first + count, len(ftbl.rows))):
                fid = ftbl.rows[i]["Index"]
                features.append({"slot": i - first, "index": fid,
                                 "name": names.get(fid, ""),
                                 "value": ftbl.rows[i]["Value"]})

    return {
        "n": n,
        "name": place_name(nametab, names, o),
        "typeName": o["typeName"],
        "category": o["category"],
        "className": names.get(o["classIndex"], ""),
        "editable": sorted(objectives.PATCHABLE),
        "canEdit": cam.member("obj") is not None,
        "values": {k: v for k, v in o.items()
                   if k not in ("_span", "featureStatus", "links")},
        "tacan": ws.tacan().get(o["campId"]),
        "featureCount": len(features),
        "features": features[:64],
        "links": [{"id": l["id"], "costs": l["costs"]} for l in o["links"]],
        "objRow": drow,
    }


def api_objective_edit(_q, body):
    ws = SESSION.workspace(body["theater"])
    name = body["file"]
    cam = ws.objectives(name)
    n = int(body["n"])
    if not (0 <= n < len(cam.objectives)):
        raise ApiError("no objective %d" % n, 404)
    if cam.member("obj") is None:
        raise ApiError(
            "%s carries no objective list of its own -- it borrows the one in "
            "%s. Edit that file instead." % (name, cam.objectives_from))
    o = cam.objectives[n]
    raw = cam.objectives_raw
    for field, value in body.get("values", {}).items():
        if field not in objectives.PATCHABLE:
            raise ApiError("%s is not editable" % field)
        raw = objectives.patch_objective(raw, o, cam.version, field, value)
        o[field] = int(value)
    cam.objectives_raw = raw
    cam.objectives_dirty = True
    cam.dirty = True
    return {"ok": True, "values": {k: o[k] for k in objectives.PATCHABLE}}


def api_placeable(q, _body):
    """Unit types that can be dropped on the map, grouped by kind."""
    ws = _ws(q)
    rows = ws.db.class_rows()
    names = ws.db.name_index()
    utbl = ws.db.table("unit")
    out = []
    for i, r in enumerate(rows):
        info = r["classInfo_"]
        if info[entities.VU_CLASS] != 6:          # CLASS_UNIT
            continue
        table = entities.DISPATCH.get(info[entities.VU_DOMAIN])
        entry = table.get(info[entities.VU_TYPE]) if table else None
        if not entry or entry[0] not in entities.PLACEABLE:
            continue
        name = names.get(i, "")
        if not name:
            continue
        drow = None
        if utbl and 0 <= r["dataPtr"] < len(utbl.rows):
            drow = utbl.rows[r["dataPtr"]]
        # classInfo_[VU_OWNER] is 0 for every unit row, so a unit type carries
        # no country. What separates the several "AAA" or "Infantry" rows is
        # their sub-type and, in practice, the vehicles they are made of --
        # which is also the only version of that difference a human can read.
        makeup, groups = [], 0
        if drow:
            for slot, vt in enumerate(drow["VehicleType"]):
                if not vt or not drow["NumElements"][slot]:
                    continue
                groups += 1
                vn = names.get(vt, "")
                if vn and vn not in makeup:
                    makeup.append(vn)
        out.append({
            "classIndex": i,
            "kind": entry[0],
            "name": name,
            "subType": info[3],
            "specific": info[4],
            "makeup": makeup[:3],
            "role": drow["Role"] if drow else 0,
            "moveType": drow["MovementType"] if drow else 0,
            "vehicles": groups,
        })
    out.sort(key=lambda e: (e["kind"], e["name"].lower(), e["specific"]))
    return {"items": out, "orders": entities.GROUND_ORDERS,
            "moveTypes": records.MOVE_NAMES}


def _redecode_units(ws, cam):
    """Re-walk the stream after a structural change, so spans stay right."""
    cam.units = entities.walk_units(
        cam.units_raw, cam.version, ws.db.class_rows())


def api_unit_add(_q, body):
    ws = SESSION.workspace(body["theater"])
    name = body["file"]
    cam = ws.units(name)
    ws.objectives(name)
    if cam.member("uni") is None:
        raise ApiError("%s has no unit list" % name)
    if cam.units_error:
        raise ApiError("this file's unit list did not decode, so it cannot be "
                       "edited: %s" % cam.units_error)

    class_index = int(body["classIndex"])
    rows = ws.db.class_rows()
    if not (0 <= class_index < len(rows)):
        raise ApiError("no class-table row %d" % class_index)
    info = rows[class_index]["classInfo_"]
    table = entities.DISPATCH.get(info[entities.VU_DOMAIN])
    entry = table.get(info[entities.VU_TYPE]) if table else None
    if not entry or entry[0] not in entities.PLACEABLE:
        raise ApiError("that class-table row is not a placeable unit")
    kind = entry[0]

    utbl = ws.db.table("unit")
    drow = None
    if utbl and 0 <= rows[class_index]["dataPtr"] < len(utbl.rows):
        drow = utbl.rows[rows[class_index]["dataPtr"]]

    try:
        vu_id, camp_id = entities.next_ids(cam.units, cam.objectives)
        record = entities.build_unit(
            kind, cam.version,
            type_id=class_index + entities.VU_LAST_ENTITY_TYPE,
            x=int(body["x"]), y=int(body["y"]),
            owner=int(body.get("owner", 0)),
            vu_id=vu_id, camp_id=camp_id,
            roster=entities.roster_from_class(drow),
            orders=int(body.get("orders", 6)),
            supply=int(body.get("supply", 100)),
            morale=int(body.get("morale", 100)))
    except ValueError as exc:
        raise ApiError(str(exc))

    cam.units_raw = entities.append_unit(cam.units_raw, record)
    _redecode_units(ws, cam)
    cam.units_dirty = True
    cam.dirty = True
    return {"ok": True, "n": len(cam.units) - 1, "total": len(cam.units),
            "id": vu_id, "campId": camp_id,
            "name": names_for(ws, class_index)}


def names_for(ws, class_index):
    return ws.db.name_index().get(class_index, "")


def api_unit_delete(_q, body):
    ws = SESSION.workspace(body["theater"])
    cam = ws.units(body["file"])
    n = int(body["n"])
    if not (0 <= n < len(cam.units)):
        raise ApiError("no unit %d" % n, 404)
    cam.units_raw = entities.delete_unit(cam.units_raw, cam.units[n])
    _redecode_units(ws, cam)
    cam.units_dirty = True
    cam.dirty = True
    return {"ok": True, "total": len(cam.units)}


def api_campaign_new(_q, body):
    """Create a new campaign file by copying an existing one.

    A campaign is only partly authored data: the objective graph, the terrain
    links, the team records and the weather all have to be internally
    consistent, and the engine will not rebuild them from nothing. So a "new"
    campaign starts as a byte copy of one that already works, with its member
    names, scenario name and UI name changed -- then everything in it is
    editable from here.
    """
    ws = SESSION.workspace(body["theater"])
    source = body["source"]
    raw_name = (body.get("name") or "").strip()
    if not raw_name:
        raise ApiError("the new campaign needs a name")

    stem = "".join(c for c in raw_name if c.isalnum() or c in " _-").strip()
    if not stem:
        raise ApiError("that name has no usable characters in it")
    ext = ".tac" if source.lower().endswith(".tac") else ".cam"
    target = stem + ext
    path = os.path.join(ws.campaign_dir, target)
    if os.path.exists(path):
        raise ApiError("%s already exists" % target)

    src = ws.campaign(source)
    old_stem = src.stem
    members, order = {}, []
    for name in src.order:
        suffix = name.rsplit(".", 1)[-1]
        renamed = stem + "." + suffix
        members[renamed] = src.members[name]
        order.append(renamed)

    fresh = campfile.CampaignFile(path, members, order, stem)
    fresh.version = src.version
    cmp_name = fresh._member_name("cmp")
    if cmp_name:
        fresh.header = campfile.decode_header(members[cmp_name], src.version)
        fresh.header.fields["Scenario"] = stem
        fresh.header.fields["SaveFile"] = stem
        if body.get("uiName"):
            fresh.header.fields["UIName"] = str(body["uiName"])[:39]
        fresh.header_dirty = True
    fresh.save(path)

    SESSION.forget(body["theater"])
    return {"ok": True, "file": target, "from": source, "stem": stem,
            "renamedFrom": old_stem}


def api_campaign_delete(_q, body):
    ws = SESSION.workspace(body["theater"])
    name = body["file"]
    path = os.path.join(ws.campaign_dir, name)
    if not os.path.isfile(path):
        raise ApiError("no such campaign file: %s" % name, 404)
    if name.lower() in ("save0.cam", "save1.cam", "save2.cam",
                        "instant.cam", "te_new.tac"):
        raise ApiError("%s is one of the campaigns the theater ships with. "
                       "Copy it and edit the copy instead." % name)
    SESSION.backup.keep(path)
    os.remove(path)
    SESSION.forget(body["theater"])
    return {"ok": True, "removed": name}


def api_icons(q, _body):
    """Manifest for the game's own campaign-map icons.

    One atlas per team colour, plus the frame table and the IconIndex ->
    name map, so the map can blit the real symbols instead of stand-in shapes.
    """
    ws = _ws(q)
    art = ws.ui_art()
    if not art or not art.available():
        return {"available": False,
                "reason": "no art/main/imageids.id under this game directory"}

    colours = {}
    for colour in uiart.TEAM_COLOURS:
        atlas = ws.icon_atlas(colour)
        if not atlas:
            continue
        colours[colour] = {
            "width": atlas.width,
            "height": atlas.height,
            "url": "/api/icons/atlas?theater=%s&colour=%s"
                   % (quote(ws.tdf_rel), colour),
            "frames": atlas.frames,
        }

    return {
        "available": bool(colours),
        "teamColours": uiart.TEAM_COLOURS,
        "colours": colours,
    }


def api_icons_atlas(q, _body):
    """Handled specially in do_GET -- it returns a PNG, not JSON."""
    raise ApiError("internal", 500)


def api_terrain(q, _body):
    """Whether this theater's ground imagery can be served, and at what zooms."""
    ws = _ws(q)
    t = ws.terrain()
    if not t:
        return {"available": False,
                "reason": ("numpy is not installed" if not terrain.available()
                           else "no terrain directory for this theater")}
    lo, hi = t.zoom_range()
    return {
        "available": True,
        "sizeKm": t.size_km,
        "tilePx": terrain.TILE_PX,
        "minZoom": lo,
        "maxZoom": hi,
        "feetPerPost": t.feet_per_post,
        "url": "/api/terrain/tile?theater=%s&z={z}&x={x}&y={y}"
               % quote(ws.tdf_rel),
    }


def _text_shared_with(ws):
    """Other theaters whose artdir resolves to the same lcktxtrc.irc.

    Korea 1980s inherits artKorea2012, so its campaign text is Korea 2012's
    campaign text -- the same three lines, one file. Worth saying out loud
    before someone rewrites Rolling Fire's blurb in two theaters at once.
    """
    text = ws.campaign_text()
    if not text:
        return []
    mine = os.path.normcase(os.path.abspath(text.path))
    out = []
    for t in SESSION.theaters():
        if t["name"] == ws.tdf.name:
            continue
        other = camptext.locate(SESSION.gamedir, t.get("artdir"))
        if other and os.path.normcase(os.path.abspath(other)) == mine:
            out.append(t["name"].strip())
    return out


def _script_context(ws, name):
    """Everything needed to render or edit a trigger script for one campaign."""
    cam = ws.objectives(name)
    nametab = ws.name_table()
    names_by_class = ws.db.name_index()
    by_camp = {o["campId"]: o for o in cam.objectives}
    teams = [t["name"] for t in (cam.header.teams if cam.header else [])]

    def place(camp_id):
        o = by_camp.get(camp_id)
        if not o:
            return "objective %d" % camp_id
        label = place_name(nametab, names_by_class, o)
        return "%s (%d)" % (label or ("objective %d" % camp_id), camp_id)

    return cam, by_camp, teams, place


def api_triggers(q, _body):
    """The campaign's .tri script, with objective ids resolved to places.

    This is where a campaign's victory conditions actually live. The header's
    EndgameResult only records the outcome; nothing in the engine writes it
    except a `#END_GAME` in this script (or a tactical-engagement win).
    """
    ws = _ws(q)
    name = (q.get("file") or [""])[0]
    sc = ws.script(name)
    if not sc:
        return {"available": False,
                "reason": "no %s.tri next to this campaign"
                          % os.path.splitext(name)[0]}

    cam, by_camp, teams, place = _script_context(ws, name)

    def point(camp_id):
        o = by_camp.get(camp_id)
        if not o:
            return None
        return {"campId": camp_id, "n": o["_n"], "x": o["x"], "y": o["y"],
                "owner": o["owner"],
                "name": place_name(ws.name_table(), ws.db.name_index(), o),
                "type": o["typeName"]}

    watched, seen = [], set()

    def collect(nodes):
        for node in nodes:
            if node.verb == "IF_CONTROLLED" and len(node.args) >= 3:
                for raw in node.args[2:]:
                    if not raw.lstrip("-").isdigit():
                        continue
                    cid = int(raw)
                    if cid in seen:
                        continue
                    seen.add(cid)
                    p = point(cid)
                    if p:
                        watched.append(p)
            collect(node.children)
            if node.orelse:
                collect(node.orelse)
    collect(sc.body)

    ends = triggers.endgames(sc.body, teams, place)

    # The editable directives behind each endgame, so the UI can offer real
    # controls rather than a text box over the raw file.
    def editable_chain(line_no):
        rows = []

        def walk(nodes, guards):
            for node in nodes:
                if node.line == line_no:
                    rows.extend(guards + [node])
                    return True
                if node.verb in triggers.CONDITIONS:
                    if walk(node.children, guards + [node]):
                        return True
                    if node.orelse and walk(node.orelse, guards + [node]):
                        return True
            return False
        walk(sc.body, [])
        out = []
        for node in rows:
            if node.verb not in triggers.EDITABLE:
                continue
            entry = {"line": node.line, "verb": node.verb,
                     "text": triggers.describe(node, teams, place),
                     "args": list(node.args)}
            if node.verb == "IF_CONTROLLED" and len(node.args) >= 2:
                entry["team"] = int(node.args[0]) if node.args[0].isdigit() else 0
                entry["mode"] = node.args[1].upper()
                entry["ids"] = [int(a) for a in node.args[2:]
                                if a.lstrip("-").isdigit()]
                entry["places"] = [{"campId": cid, "name": place(cid)}
                                   for cid in entry["ids"]]
            elif node.verb == "IF_CAMPAIGN_DAY" and len(node.args) >= 2:
                entry["cmp"] = node.args[0].upper()
                entry["value"] = int(node.args[1]) if node.args[1].isdigit() else 0
            elif node.args and node.args[0].lstrip("-").isdigit():
                entry["value"] = int(node.args[0])
            out.append(entry)
        return out

    for e in ends:
        e["editable"] = editable_chain(e["line"])

    slot = camptext.slot_of(name)
    text = ws.campaign_text()
    blurb = camptext.blurb_from_script(ends)

    return {
        "available": True,
        "file": os.path.basename(sc.path),
        "totalEvents": sc.total,
        "dirty": sc.dirty,
        "init": [{"verb": n.verb, "text": triggers.describe(n, teams, place),
                  "comment": n.comment} for n in sc.init],
        "endgames": ends,
        "outline": triggers.outline(sc.body, teams, place),
        "watched": watched,
        "teams": [{"index": i, "name": n} for i, n in enumerate(teams)],
        "suggestedBlurb": blurb,
        "text": ({"available": True, "file": os.path.basename(text.path),
                  "path": os.path.relpath(text.path, SESSION.gamedir),
                  "dirty": text.dirty,
                  "sharedWith": _text_shared_with(ws),
                  **text.campaign_text(slot)} if (text and slot)
                 else {"available": False,
                       "reason": ("this campaign is not one of the three the "
                                  "theater advertises"
                                  if text else
                                  "no lcktxtrc.irc for this theater")}),
    }


def api_trigger_edit(_q, body):
    """Change one directive in a campaign's trigger script."""
    ws = SESSION.workspace(body["theater"])
    name = body["file"]
    sc = ws.script(name)
    if not sc:
        raise ApiError("no trigger script for %s" % name, 404)

    _cam, by_camp, _teams, _place = _script_context(ws, name)
    fields = dict(body.get("fields", {}))

    if "ids" in fields:
        ids = [int(v) for v in fields["ids"]]
        missing = [i for i in ids if i not in by_camp]
        if missing:
            raise ApiError(
                "no objective in this campaign has campaign id %s. The engine "
                "skips an id it cannot find, so the condition would read as "
                "satisfied in an 'all' list and never fire in an 'any' list."
                % ", ".join(str(m) for m in missing))
        fields["ids"] = ids

    try:
        sc.edit(int(body["line"]), fields)
    except (ValueError, IndexError) as exc:
        raise ApiError(str(exc))
    return {"ok": True}


def api_campaign_text_edit(_q, body):
    """Change a campaign's name or its victory-conditions blurb."""
    ws = SESSION.workspace(body["theater"])
    text = ws.campaign_text()
    if not text:
        raise ApiError("this theater has no lcktxtrc.irc to edit", 404)
    slot = camptext.slot_of(body["file"])
    if not slot:
        raise ApiError("%s is not one of the three campaigns the theater "
                       "advertises, so it has no selection text" % body["file"])
    try:
        if "name" in body:
            text.set("TXT_SCENARIO_%d" % slot, body["name"])
        if "blurb" in body:
            text.set("TXT_SC_%d" % slot, body["blurb"])
    except KeyError as exc:
        raise ApiError(str(exc))
    return {"ok": True, **text.campaign_text(slot)}


def api_create_theater(_q, body):
    name = (body.get("name") or "").strip()
    if not name:
        raise ApiError("the new theater needs a name")
    try:
        info = theater.create_theater(
            SESSION.gamedir,
            body["base"],
            name,
            desc=body.get("desc", ""),
            campaign_folder=body.get("folder") or None,
            copy_art=bool(body.get("copyArt")),
        )
    except (FileExistsError, ValueError) as exc:
        raise ApiError(str(exc))
    SESSION.forget()
    return {"ok": True, "theater": info, "state": api_state(None, None)}


def api_delete_theater(_q, body):
    try:
        removed = theater.delete_theater(
            SESSION.gamedir, body["tdf"],
            remove_campaign_dir=bool(body.get("removeCampaign")))
    except ValueError as exc:
        raise ApiError(str(exc))
    SESSION.forget()
    return {"ok": True, "removed": removed, "state": api_state(None, None)}


def api_discard(_q, _body):
    """Drop every in-memory edit and re-read from disk."""
    n = len(SESSION.pending())
    SESSION.forget()
    return {"ok": True, "discarded": n}


def api_save(_q, _body):
    result = SESSION.save_all()
    result["ok"] = True
    return result


def api_lookup(q, _body):
    """id -> name maps the UI uses to turn raw indices into pickers."""
    ws = _ws(q)
    kind = (q.get("kind") or ["class"])[0]
    if kind == "class":
        items = [{"id": i, "name": n}
                 for i, n in sorted(ws.db.name_index().items())]
    else:
        tbl = ws.db.table(kind)
        if tbl is None:
            raise ApiError("no %s table" % kind, 404)
        label = tbl.spec.label or "Name"
        items = [{"id": r.get("Index", i), "name": r.get(label, "")}
                 for i, r in enumerate(tbl.rows)]
    return {"kind": kind, "items": items}


ROUTES_GET = {
    "/api/state": api_state,
    "/api/theater": api_theater,
    "/api/table": api_table,
    "/api/row": api_row,
    "/api/campaign": api_campaign,
    "/api/lookup": api_lookup,
    "/api/map": api_map,
    "/api/unit": api_unit,
    "/api/objective": api_objective,
    "/api/placeable": api_placeable,
    "/api/icons": api_icons,
    "/api/terrain": api_terrain,
    "/api/triggers": api_triggers,
}

ROUTES_POST = {
    "/api/gamedir": api_set_gamedir,
    "/api/edit": api_edit,
    "/api/row/add": api_add_row,
    "/api/row/revert": api_revert_row,
    "/api/campaign/edit": api_campaign_edit,
    "/api/triggers/edit": api_trigger_edit,
    "/api/campaign/text": api_campaign_text_edit,
    "/api/unit/edit": api_unit_edit,
    "/api/unit/add": api_unit_add,
    "/api/unit/delete": api_unit_delete,
    "/api/objective/edit": api_objective_edit,
    "/api/campaign/new": api_campaign_new,
    "/api/campaign/delete": api_campaign_delete,
    "/api/theater/create": api_create_theater,
    "/api/theater/delete": api_delete_theater,
    "/api/save": api_save,
    "/api/discard": api_discard,
}


# --- HTTP --------------------------------------------------------------------

class Handler(BaseHTTPRequestHandler):
    server_version = "FFCampaignEditor/1.0"

    def log_message(self, fmt, *args):
        if "--verbose" in sys.argv:
            sys.stderr.write("  %s\n" % (fmt % args))

    def _send_json(self, obj, status=200):
        blob = json.dumps(obj, default=_jsonable).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(blob)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(blob)

    def do_GET(self):
        parsed = urlparse(self.path)
        if parsed.path == "/api/map/image":
            return self._serve_map(parse_qs(parsed.query))
        if parsed.path == "/api/icons/atlas":
            return self._serve_atlas(parse_qs(parsed.query))
        if parsed.path == "/api/terrain/tile":
            return self._serve_terrain(parse_qs(parsed.query))
        route = ROUTES_GET.get(parsed.path)
        if route:
            return self._run(route, parse_qs(parsed.query), None)
        return self._serve_static(parsed.path)

    def _serve_terrain(self, query):
        """One 256x256 map tile of the theater's ground imagery."""
        try:
            ws = _ws(query)
        except ApiError as exc:
            return self._send_json({"error": str(exc)}, exc.status)
        t = ws.terrain()
        if not t:
            return self._send_json({"error": "no terrain for this theater"}, 404)
        try:
            z = int((query.get("z") or ["0"])[0])
            x = int((query.get("x") or ["0"])[0])
            y = int((query.get("y") or ["0"])[0])
        except ValueError:
            return self._send_json({"error": "bad tile coordinates"}, 400)
        try:
            blob = t.render_png(z, x, y)
        except Exception as exc:
            import traceback
            traceback.print_exc()
            return self._send_json({"error": str(exc)}, 500)
        if blob is None:
            return self._send_json({"error": "tile out of range"}, 404)
        self.send_response(200)
        self.send_header("Content-Type", "image/png")
        self.send_header("Content-Length", str(len(blob)))
        self.send_header("Cache-Control", "max-age=86400")
        self.end_headers()
        self.wfile.write(blob)

    def _serve_atlas(self, query):
        """One team colour's icons, packed into a single PNG."""
        try:
            ws = _ws(query)
        except ApiError as exc:
            return self._send_json({"error": str(exc)}, exc.status)
        colour = (query.get("colour") or ["red"])[0]
        atlas = ws.icon_atlas(colour)
        if not atlas:
            return self._send_json({"error": "no icons for %r" % colour}, 404)
        blob = atlas.png
        self.send_response(200)
        self.send_header("Content-Type", "image/png")
        self.send_header("Content-Length", str(len(blob)))
        self.send_header("Cache-Control", "max-age=600")
        self.end_headers()
        self.wfile.write(blob)

    def _serve_map(self, query):
        """The campaign's Kneemap.gif, served as the map background."""
        try:
            ws = _ws(query)
            path = ws.map_image()
        except ApiError as exc:
            return self._send_json({"error": str(exc)}, exc.status)
        if not path:
            return self._send_json({"error": "this campaign has no Kneemap.gif"}, 404)
        with open(path, "rb") as fp:
            blob = fp.read()
        self.send_response(200)
        self.send_header("Content-Type", "image/gif")
        self.send_header("Content-Length", str(len(blob)))
        self.send_header("Cache-Control", "max-age=300")
        self.end_headers()
        self.wfile.write(blob)

    def do_POST(self):
        parsed = urlparse(self.path)
        route = ROUTES_POST.get(parsed.path)
        if not route:
            return self._send_json({"error": "no such endpoint"}, 404)
        length = int(self.headers.get("Content-Length") or 0)
        try:
            body = json.loads(self.rfile.read(length) or b"{}")
        except ValueError as exc:
            return self._send_json({"error": "bad JSON: %s" % exc}, 400)
        return self._run(route, parse_qs(parsed.query), body)

    def _run(self, route, query, body):
        try:
            return self._send_json(route(query, body))
        except ApiError as exc:
            return self._send_json({"error": str(exc)}, exc.status)
        except Exception as exc:  # surfaced in the UI rather than the console
            import traceback
            traceback.print_exc()
            return self._send_json(
                {"error": "%s: %s" % (type(exc).__name__, exc)}, 500)

    def _serve_static(self, path):
        rel = posixpath.normpath(path).lstrip("/")
        if rel in ("", "."):
            rel = "index.html"
        target = os.path.normpath(os.path.join(WEB, rel))
        if not target.startswith(WEB) or not os.path.isfile(target):
            self.send_error(404)
            return
        ctype = mimetypes.guess_type(target)[0] or "application/octet-stream"
        with open(target, "rb") as fp:
            blob = fp.read()
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(blob)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(blob)


def _jsonable(obj):
    if isinstance(obj, (bytes, bytearray)):
        return obj.hex()
    if isinstance(obj, float) and obj != obj:
        return None
    raise TypeError(repr(obj))


def main():
    global SESSION
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--gamedir", default=r"C:\FreeFalcon6",
                    help="FreeFalcon install directory (default %(default)s)")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--no-browser", action="store_true")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    SESSION = workspace.Session(args.gamedir)
    url = "http://127.0.0.1:%d/" % args.port

    if not os.path.isfile(os.path.join(SESSION.gamedir, theater.THEATER_LIST)):
        print("Warning: no %s under %s -- set the game directory in the UI."
              % (theater.THEATER_LIST, SESSION.gamedir))

    server = HTTPServer(("127.0.0.1", args.port), Handler)
    print("FreeFalcon campaign editor")
    print("  game directory : %s" % SESSION.gamedir)
    print("  listening on   : %s" % url)
    print("  stop with Ctrl-C")
    if not args.no_browser:
        threading.Timer(0.4, lambda: webbrowser.open(url)).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nbye")


if __name__ == "__main__":
    main()
