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
import math
import mimetypes
import os
import posixpath
import re
import sys
import threading
import webbrowser
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse, parse_qs, quote

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from ffcamp import (campfile, camptext, entities, objectives,  # noqa: E402
                    records, teams as teamdata, terrain, theater,
                    triggers, uiart, weather, workspace)

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


_NUM_FILTER = re.compile(r"^(>=|<=|>|<|=)\s*(-?\d+(?:\.\d+)?)$")


def _match_filter(value, want):
    """One column's filter: a substring, or a numeric comparison like "> 40"."""
    m = _NUM_FILTER.match(want)
    if m:
        try:
            n = float(value)
        except (TypeError, ValueError):
            return False
        v = float(m.group(2))
        op = m.group(1)
        if op == ">":
            return n > v
        if op == "<":
            return n < v
        if op == ">=":
            return n >= v
        if op == "<=":
            return n <= v
        return n == v
    return want in str(value).lower()


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

    # Per-column filters and the sort are applied here rather than in the
    # browser: the table pages, so a client-side sort would only reorder the
    # rows that happen to be on this page.
    raw_filters = (q.get("filters") or [""])[0]
    if raw_filters:
        try:
            filters = {str(k): str(v).strip().lower()
                       for k, v in json.loads(raw_filters).items()
                       if str(v).strip()}
        except (ValueError, AttributeError):
            filters = {}
        if filters:
            views = [v for v in views
                     if all(_match_filter(v.get(col, ""), want)
                            for col, want in filters.items())]

    sort = (q.get("sort") or [""])[0]
    if sort:
        reverse = (q.get("dir") or ["1"])[0] == "-1"

        def sort_key(v):
            val = v.get(sort)
            if isinstance(val, (int, float)) and not isinstance(val, bool):
                return (0, float(val), "")
            return (1, 0.0, str(val).lower())

        views.sort(key=sort_key, reverse=reverse)

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
        if not isinstance(value, (list, tuple)):
            raise ApiError("%s takes %d values, not one" % (f.name, f.count))
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
        "squadrons": [dict(header_squadron_view(ws, ws.db.name_index(), s),
                           isPlayer=is_player_squadron(player_squadron_id(cam),
                                                       s.get("id")))
                      for s in hdr.squadrons[:400]],
        "playerSquadron": player_squadron_id(cam),
        "squadronCount": len(hdr.squadrons),
        "specialties": SPECIALTIES,
        "countries": COUNTRIES,
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
        if key == "Scenario" and str(value) != str(current):
            _check_scenario_change(ws, cam, str(value))
            ws.forget_script(body["file"])
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


# --- weather -----------------------------------------------------------------
#
# The game keeps its fronts as a field in sim feet and campaign milliseconds
# (ffcamp/weather.py, src/graphics/include/weatherfronts.h). The browser gets
# them in the terms the map uses -- grid km, east and north, knots, hours of
# age -- and sends them back the same way, so it never needs the model.

def _weather_ctx(ws, name):
    cam = ws.campaign(name)
    hdr = cam.header
    f = hdr.fields if hdr else {}
    return cam, int(f.get("CurrentTime", 0)), \
        (int(f.get("TheaterSizeX", 1024)) or 1024, int(f.get("TheaterSizeY", 1024)) or 1024)


def _clock(ms):
    day = ms // 86400000 + 1
    rest = ms % 86400000
    return "Day %d %02d:%02d" % (day, rest // 3600000, rest // 60000 % 60)


def _front_view(f, now):
    north, east = weather.core(f, now)
    return {
        "kind": weather.kind_of(f),
        "eastKm": round(east / weather.KM, 1),
        "northKm": round(north / weather.KM, 1),
        "headingDeg": round(math.degrees(f["heading"]) % 360, 1),
        "speedKts": round(f["speed"] / weather.KTS, 1),
        "widthKm": round(f["halfWidth"] / weather.KM, 1),
        "lengthKm": round(f["halfLength"] / weather.KM, 1),
        "severity": round(f["severity"], 2),
        "windBoost": round(f.get("windBoost", 0.0), 1),
        "tempDelta": round(f.get("tempDelta", 0.0), 1),
        "ageHours": round((now - f["born"]) / 3600000.0, 2),
        "lifeHours": round(f["life"] / 3600000.0, 2),
    }


def _front_from_view(v, now):
    age = int(float(v.get("ageHours", 0)) * 3600000)
    f = {
        "heading": math.radians(float(v["headingDeg"])),
        "speed": float(v["speedKts"]) * weather.KTS,
        "halfWidth": max(1.0, float(v["widthKm"])) * weather.KM,
        "halfLength": max(0.0, float(v.get("lengthKm", 0))) * weather.KM,
        "severity": float(v["severity"]),
        "windBoost": float(v.get("windBoost", 0)),
        "tempDelta": float(v.get("tempDelta", 0)),
        "life": max(1, int(float(v["lifeHours"]) * 3600000)),
        "born": now - min(age, now),
        "x": 0.0, "y": 0.0,
    }
    return weather.place(f, now, float(v["northKm"]), float(v["eastKm"]))


def _state_from_view(fv, w, now):
    """The fronts block from what the browser sent, drift from the wind."""
    kph = float(w["windKnots"]) * 1.852
    to = math.radians((float(w["windFromDeg"]) + 180.0) % 360)
    return {
        "active": bool(fv.get("active", True)),
        "prevailing": min(4.0, max(1.0, float(fv["prevailing"]))),
        "noiseAmp": max(0.0, float(fv.get("noiseAmp", 0.8))),
        "noiseScale": 400000.0,
        "seed": int(fv.get("seed", 1)) & 0xFFFFFFFF,
        "driftX": math.cos(to) * kph * 0.9113,
        "driftY": math.sin(to) * kph * 0.9113,
        "fronts": [_front_from_view(v, now)
                   for v in (fv.get("items") or [])][:weather.FRONTS_MAX],
    }


def _weather_view(w):
    return {
        "condition": w.get("condition") or 1,
        # The game keeps the direction the clouds go; a pilot wants where the
        # wind comes from.
        "windFromDeg": round((math.degrees(w["windHeading"]) + 180) % 360),
        "windKnots": int(w["windKnots"]),
        "temperature": int(w["temperature"]),
        "cumulusBase": int(round(w["cumulusBase"])),
        "stratusBase": int(round(w["stratusBase"])),
        "stratus2Base": int(round(w["stratus2Base"])),
        "contrail": int(w["contrail"]),
        "overcastDepth": int(w["overcastDepth"]),
    }


def _weather_from_view(v, old):
    out = dict(old)
    out.update(
        condition=min(4, max(1, int(v["condition"]))),
        windHeading=math.radians((float(v["windFromDeg"]) + 180.0) % 360),
        windKnots=max(0, min(255, int(v["windKnots"]))),
        temperature=max(-60, min(60, int(v["temperature"]))),
        cumulusBase=float(v["cumulusBase"]),
        stratusBase=float(v["stratusBase"]),
        stratus2Base=float(v["stratus2Base"]),
        contrail=max(0, min(255, int(v["contrail"]))),
        overcastDepth=max(0, min(255, int(v["overcastDepth"]))),
    )
    return out


def _preview(state, now, size, hours):
    at = now + int(float(hours) * 3600000)
    step = 16
    items = []
    for f in state["fronts"]:
        v = _front_view(f, at)
        v["strength"] = round(weather.life_strength(f, at), 2)
        items.append(v)
    return {
        "at": at, "atText": _clock(at), "stepKm": step,
        "grid": weather.grid(state, at, size[0], size[1], step,
                             state.get("active", True)),
        "items": items,
    }


def api_weather(q, _body):
    ws = _ws(q)
    name = (q.get("file") or [""])[0]
    cam, now, size = _weather_ctx(ws, name)
    raw = cam.member("wth")
    if raw is None:
        raise ApiError("%s has no weather member" % name)
    w = weather.parse(raw)
    if w["layout"] == "unknown":
        raise ApiError("the weather member of %s is not a layout this editor "
                       "reads (%d bytes)" % (name, len(raw)))
    fr = w.get("fronts")
    if w["condition"] is None:
        w["condition"] = int(round(fr["prevailing"])) if fr else 1
    view = {
        "file": name,
        "isCampaign": name.lower().endswith(".cam"),
        "layout": w["layout"],
        "now": now, "nowText": _clock(now),
        "sizeKm": list(size),
        "weather": _weather_view(w),
        "fronts": None,
        "kinds": list(weather.KINDS),
        "dirty": getattr(cam, "dirty", False),
    }
    if fr:
        view["fronts"] = {
            "active": fr["active"], "prevailing": round(fr["prevailing"], 2),
            "noiseAmp": round(fr["noiseAmp"], 2), "seed": fr["seed"],
            "items": [_front_view(f, now) for f in fr["fronts"]],
        }
        view["preview"] = _preview(fr, now, size, 0)
    return view


def api_weather_preview(_q, body):
    ws = SESSION.workspace(body["theater"])
    _cam, now, size = _weather_ctx(ws, body["file"])
    state = _state_from_view(body["fronts"], body["weather"], now)
    return _preview(state, now, size, body.get("hours", 0))


def api_weather_generate(_q, body):
    """Fronts the way the game seeds them at load, for the browser to edit."""
    import random as _random
    ws = SESSION.workspace(body["theater"])
    _cam, now, size = _weather_ctx(ws, body["file"])
    rng = _random.Random()
    w = body["weather"]
    to = math.radians((float(w["windFromDeg"]) + 180.0) % 360)
    count = max(0, min(weather.FRONTS_MAX, int(body.get("count", 3))))
    kind = body.get("kind")
    fronts = []
    for _ in range(count):
        k = kind or rng.choices(weather.KINDS, [35, 20, 17, 16, 12])[0]
        fronts.append(weather.spawn(k, to, size[0], size[1], now, rng,
                                    in_progress=True))
    return {"items": [_front_view(f, now) for f in fronts],
            "seed": rng.randrange(1, 1 << 31)}


def api_weather_edit(_q, body):
    ws = SESSION.workspace(body["theater"])
    name = body["file"]
    cam, now, _size = _weather_ctx(ws, name)
    member = cam._member_name("wth")
    if member is None:
        raise ApiError("%s has no weather member" % name)
    old = weather.parse(cam.members[member])
    w = _weather_from_view(body["weather"], old)
    fv = body.get("fronts")
    if fv:
        w["fronts"] = _state_from_view(fv, body["weather"], now)
        w["condition"] = weather.condition(w["fronts"]["prevailing"])
    else:
        w["fronts"] = None
    w["time"] = old.get("time", now)
    cam.members[member] = weather.encode(w)
    cam.dirty = True
    return {"ok": True}


# --- what fits on which aircraft ----------------------------------------------
#
# The loadout screen offers, per hardpoint, what the aircraft's vehicle class
# lists (FALCON4.VCD Weapon/Weapons): either one weapon and how many, or -- when
# the count is 255 -- a row of the weapon-list table naming several, each with
# its own quantity. That is per jet. It has nothing to do with the squadron
# stores maximum, which is how much of a weapon a squadron keeps on the shelf.

def _aircraft_rows(ws):
    """Every vehicle class that a squadron class flies, once each."""
    rows = ws.db.class_rows()
    utbl = ws.db.table("unit")
    names = ws.db.name_index()
    seen = {}
    for ci, ent in enumerate(rows):
        info = ent["classInfo_"]
        entry = entities.DISPATCH.get(info[entities.VU_DOMAIN], {}).get(
            info[entities.VU_TYPE])
        if not entry or entry[0] != "squadron" or not utbl:
            continue
        ptr = ent["dataPtr"]
        if not (0 <= ptr < len(utbl.rows)):
            continue
        vci = utbl.rows[ptr]["VehicleType"][0]
        if not vci or vci in seen or not (0 <= vci < len(rows)):
            continue
        # Some squadron classes point at ground vehicles (a 2S6, an AK47):
        # placeholders, not aircraft. DOMAIN_AIR is 2.
        if rows[vci]["classInfo_"][entities.VU_DOMAIN] != 2:
            continue
        _t, vrow = ws.db.data_row(vci)
        if vrow and "Weapon" in vrow:
            seen[vci] = (vrow, names.get(vci, "") or vrow.get("Name", "").strip())
    return seen


def aircraft_hardpoints(ws, vrow):
    wl = ws.db.table("weaponlist")
    wcd = ws.db.table("weapon")
    wname = lambda i: ((wcd.rows[i].get("Name") or "").strip()
                       if wcd and 0 <= i < len(wcd.rows) else "weapon %d" % i)
    out = []
    for hp in range(len(vrow["Weapon"])):
        w, n = vrow["Weapon"][hp], vrow["Weapons"][hp]
        if not w or not n:
            continue
        options = []
        if n == 255:
            if wl is not None and 0 <= w < len(wl.rows):
                lr = wl.rows[w]
                for wid, qty in zip(lr["WeaponID"], lr["Quantity"]):
                    if wid and wname(wid).upper() != "NO WEAPON":
                        options.append({"index": wid, "name": wname(wid),
                                        "qty": qty})
        elif wname(w).upper() != "NO WEAPON":
            options.append({"index": w, "name": wname(w), "qty": n})
        if not options:
            continue
        out.append({"hardpoint": hp, "gun": hp == 0, "options": options})
    return out


def _aircraft_in_campaign(ws, name):
    """Squadrons and airframes per aircraft class in one campaign file.

    A squadron's aircraft class is its unit class's first vehicle type; its
    airframes are what its roster still holds (two bits a slot, as for any
    unit -- what GetTotalVehicles counts).
    """
    out = {}
    if not name:
        return out
    cam = ws.units(name)
    rows = ws.db.class_rows()
    utbl = ws.db.table("unit")
    teams = [t["name"] for t in (cam.header.teams if cam.header else [])]
    for u in cam.units:
        if u["kind"] != "squadron" or not utbl:
            continue
        ptr = rows[u["classIndex"]]["dataPtr"]
        if not (0 <= ptr < len(utbl.rows)):
            continue
        vci = utbl.rows[ptr]["VehicleType"][0]
        r = u.get("roster", 0)
        have = sum((r >> (2 * i)) & 3 for i in range(16))
        rec = out.setdefault(vci, {"squadrons": 0, "aircraft": 0, "teams": {}})
        rec["squadrons"] += 1
        rec["aircraft"] += have
        tname = teams[u["owner"]] if u["owner"] < len(teams) else             "team %d" % u["owner"]
        t = rec["teams"].setdefault(tname, {"squadrons": 0, "aircraft": 0})
        t["squadrons"] += 1
        t["aircraft"] += have
    return out


def api_aircraft(q, _body):
    ws = _ws(q)
    name = (q.get("file") or [""])[0]
    try:
        flying = _aircraft_in_campaign(ws, name)
        flying_error = None
    except Exception as exc:          # a broken unit stream: list still useful
        flying, flying_error = {}, str(exc)
    out = []
    for vci, (vrow, acname) in sorted(_aircraft_rows(ws).items(),
                                      key=lambda kv: kv[1][1].lower()):
        hps = aircraft_hardpoints(ws, vrow)
        weapons = {}
        for h in hps:
            for o in h["options"]:
                w = weapons.setdefault(o["index"], {"index": o["index"],
                                                    "name": o["name"],
                                                    "hardpoints": [], "max": 0})
                w["hardpoints"].append(h["hardpoint"])
        # How many one jet can take in total: each hardpoint's best quantity.
        for w in weapons.values():
            w["max"] = sum(max((o["qty"] for o in h["options"]
                                if o["index"] == w["index"]), default=0)
                           for h in hps)
        inuse = flying.get(vci, {"squadrons": 0, "aircraft": 0, "teams": {}})
        out.append({"vehicle": vci, "name": acname,
                    "squadrons": inuse["squadrons"],
                    "inService": inuse["aircraft"],
                    "teams": inuse["teams"],
                    "hardpoints": hps,
                    "weapons": sorted(weapons.values(),
                                      key=lambda w: w["name"].lower())})
    return {"aircraft": out, "file": name, "flyingError": flying_error}


# --- campaign progress ------------------------------------------------------
#
# Each team's own war statistics, from the .tea member (ffcamp/teams.py):
# what it has now against what it started with, and the national supply,
# fuel and replacement pools. One file is a snapshot; the saves of one
# scenario, ordered by campaign time, are the war so far.

STAT_LABELS = {
    "aircraft": "Aircraft", "groundVehs": "Ground vehicles",
    "airDefenseVehs": "Air defence", "ships": "Ships", "airbases": "Airbases",
    "supplyLevel": "Supply level %", "fuelLevel": "Fuel level %",
}


def _team_rows(cam):
    names = [t["name"] for t in (cam.header.teams if cam.header else [])]
    out = []
    for t in teamdata.decode(cam.member("tea")):
        st, cur = t["start"], t["current"]
        # A team that started with nothing is not in this war.
        if not any(st[k] for k in ("aircraft", "groundVehs", "airDefenseVehs",
                                   "ships")):
            continue
        out.append({
            "team": t["who"],
            "name": names[t["who"]] if t["who"] < len(names) else
                    "team %d" % t["who"],
            "current": cur, "start": st,
            "initiative": t["initiative"],
            "supplyAvail": t["supplyAvail"], "fuelAvail": t["fuelAvail"],
            "replacementsAvail": t["replacementsAvail"],
            "experience": t["experience"],
            "stance": t["stance"],
        })
    return out


def _unit_rollup(ws, name):
    """Averages over each team's ground units, and objectives held."""
    cam = ws.units(name)
    ws.objectives(name)
    rows = ws.db.class_rows()
    utbl = ws.db.table("unit")
    per = {}
    for u in cam.units:
        if u["kind"] not in ("battalion", "taskforce", "squadron"):
            continue
        drow = None
        ptr = rows[u["classIndex"]]["dataPtr"] if 0 <= u["classIndex"] < len(rows) else -1
        if utbl and 0 <= ptr < len(utbl.rows):
            drow = utbl.rows[ptr]
        p = per.setdefault(u["owner"], {"battalions": 0, "taskforces": 0,
                                        "squadrons": 0, "_h": [], "_s": [],
                                        "_m": [], "_f": []})
        p[u["kind"] + ("s" if not u["kind"].endswith("s") else "")] += 1
        h = unit_health(u, drow)
        if h is not None and u["kind"] == "battalion":
            p["_h"].append(h)
        if u["kind"] == "battalion":
            for key, bucket in (("supply", "_s"), ("morale", "_m"),
                                ("fatigue", "_f")):
                if key in u:
                    p[bucket].append(u[key])
    out = {}
    avg = lambda v: round(sum(v) / len(v)) if v else None
    for team, p in per.items():
        out[team] = {"battalions": p["battalions"], "taskforces": p["taskforces"],
                     "squadrons": p["squadrons"], "health": avg(p["_h"]),
                     "supply": avg(p["_s"]), "morale": avg(p["_m"]),
                     "fatigue": avg(p["_f"])}
    held = {}
    for o in cam.objectives:
        cat = objectives.CATEGORY_LABELS.get(o["category"], str(o["category"])) \
            if isinstance(objectives.CATEGORY_LABELS, dict) else \
            (objectives.CATEGORY_LABELS[o["category"]]
             if 0 <= o["category"] < len(objectives.CATEGORY_LABELS)
             else str(o["category"]))
        held.setdefault(o["owner"], {}).setdefault(cat, 0)
        held[o["owner"]][cat] += 1
    return out, held


def api_progress(q, _body):
    ws = _ws(q)
    name = (q.get("file") or [""])[0]
    cam = ws.campaign(name)
    hdr = cam.header
    snap = _team_rows(cam)
    try:
        units, held = _unit_rollup(ws, name)
        units_error = None
    except Exception as exc:          # a broken unit stream still has stats
        units, held, units_error = {}, {}, str(exc)
    # Who is actually fighting: a team at war (stance 5) with another team in
    # this war. The rest -- China and the CIS in Korea -- are here but neutral.
    present = {t["team"] for t in snap}
    for t in snap:
        t["atWarWith"] = [o for o in present if o != t["team"] and
                          o < len(t["stance"]) and t["stance"][o] == 5]
    for t in snap:
        t["units"] = units.get(t["team"])
        t["held"] = held.get(t["team"], {})

    # The war so far: every save of the same scenario, by campaign time.
    scenario = hdr.fields.get("Scenario", "") if hdr else ""
    series, skipped = [], []
    for f in ws.campaign_files():
        if f["kind"] != "campaign" and not name.lower().endswith(".tac"):
            continue
        try:
            other = ws.campaign(f["file"])
        except Exception:
            continue
        oh = other.header
        if not oh or oh.fields.get("Scenario", "") != scenario:
            continue
        rows = _team_rows(other)
        if not rows:
            continue
        # A save whose forces add up to a sliver of what they started with
        # was written from a broken or half-loaded campaign (an Auto Save made
        # right after loading one), not by a war that lost 90% overnight.
        have = sum(r["current"][k] for r in rows
                   for k in ("aircraft", "groundVehs", "airDefenseVehs"))
        start = sum(r["start"][k] for r in rows
                    for k in ("aircraft", "groundVehs", "airDefenseVehs"))
        if start and have < start * 0.1:
            skipped.append(f["file"])
            continue
        series.append({"file": f["file"], "time": oh.fields["CurrentTime"],
                       "clock": _clock(oh.fields["CurrentTime"]),
                       "teams": {r["team"]: {
                           **{k: r["current"][k] for k in STAT_LABELS},
                           "initiative": r["initiative"],
                           "supplyAvail": r["supplyAvail"],
                           "fuelAvail": r["fuelAvail"],
                           "replacementsAvail": r["replacementsAvail"]}
                           for r in rows}})
    series.sort(key=lambda p: p["time"])

    return {
        "file": name, "scenario": scenario,
        "clock": _clock(hdr.fields["CurrentTime"]) if hdr else "",
        "teams": snap, "series": series, "labels": STAT_LABELS,
        "skipped": skipped,
        "unitsError": units_error,
    }


# --- map ---------------------------------------------------------------------

def script_targets(ws, cam, campaign_file):
    """The places the trigger script watches, joined to objective indices.

    `#IF_CONTROLLED` is the only condition that names a place; events, ratios,
    initiative and the rest have nothing to draw. The result is one entry per
    objective so the map can ring it, plus the counts it needs to explain what
    it is not showing.
    """
    sc, from_scenario = ws.script(campaign_file)
    if sc is None:
        return {"available": False, "targets": [], "dead": 0,
                "conditions": 0, "otherConditions": 0}

    by_camp = {}
    for o in cam.objectives:
        by_camp.setdefault(o["campId"], o)

    targets, dead = {}, 0
    for t in triggers.controlled_targets(sc.body):
        o = by_camp.get(t["campId"])
        if o is None:
            dead += 1
            continue
        rec = targets.setdefault(t["campId"], {
            "campId": t["campId"], "n": o["_n"], "endgame": False,
            "conditions": []})
        rec["endgame"] = rec["endgame"] or t["endgame"]
        rec["conditions"].append({"team": t["team"], "mode": t["mode"],
                                  "negated": t["negated"], "count": t["count"],
                                  "line": t["line"]})

    counts = triggers.condition_counts(sc.body)
    return {
        "available": True,
        "file": os.path.basename(sc.path),
        "fromScenario": from_scenario,
        "targets": list(targets.values()),
        "dead": dead,
        "conditions": sum(counts.values()),
        "otherConditions": sum(n for verb, n in counts.items()
                               if verb != "IF_CONTROLLED"),
    }


# Grid coordinates are kilometres. ConvertGridToSim in campaign/camplib/find.cpp
# maps grid y onto sim north and grid x onto sim east, so x runs west->east and
# y runs south->north. Kneemap.gif is one pixel per grid km, top row north.
# Kinds whose roster is a strength: vehicles left per slot. A flight's roster
# is its aircraft for one mission, packages and brigades have none.
HEALTH_KINDS = ("battalion", "taskforce", "squadron")


def unit_health(u, drow):
    """Percent of the class's full establishment still on the roster.

    The roster packs two bits of vehicle count per slot, 16 slots
    (entities.roster_from_class); full strength is the class's NumElements,
    each capped at the 3 two bits can hold. This is what the engine counts
    when it asks a unit how many vehicles it has left.
    """
    if u["kind"] not in HEALTH_KINDS or not drow or "roster" not in u:
        return None
    full = sum(min(3, int(n)) for n in drow.get("NumElements", [])[:16])
    if not full:
        return None
    r = u["roster"]
    have = sum((r >> (2 * i)) & 3 for i in range(16))
    return min(100, round(100 * have / full))


def naval_state(u, objs, nametab, names):
    """What a task force is doing, the way TaskForceClass::MoveUnit decides.

    In port means the nearest objective within 1 km is a port, and then with
    no waypoints it stays put. At sea with no waypoints it patrols 20 km
    north and back for ever. With waypoints it follows them. The campaign AI
    never gives a ship waypoints (NavalTaskingManagerClass::Task is empty).
    """
    best, bd = None, 1.0001
    for o in objs:
        d = ((o["x"] - u["x"]) ** 2 + (o["y"] - u["y"]) ** 2) ** 0.5
        if d <= bd:
            best, bd = o, d
    wp = u.get("waypoints", [])
    # MoveUnit's own patrol is three waypoints all flagged WPF_REPEAT (0x40):
    # that is "patrolling", not a route anyone gave it.
    patrol = bool(wp) and all(w["flags"] & 0x40 for w in wp)
    in_port = bool(best and best["typeName"] == "Port")
    if in_port and not wp:
        state = "In port at " + (place_name(nametab, names, best) or "a port")
    elif not wp or patrol:
        state = "At sea, patrolling"
    else:
        state = "Sailing: %d waypoint%s" % (len(wp), "" if len(wp) == 1 else "s")
    return {"inPort": in_port, "patrol": patrol, "waypoints": len(wp),
            "state": state}


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
    rows = ws.db.class_rows()

    def icon_for(class_index, table):
        """The IconIndex of a class-table row, resolved to an icon name."""
        if not art:
            return ""
        if not (0 <= class_index < len(rows)):
            return ""
        ptr = rows[class_index]["dataPtr"]
        if not table or not (0 <= ptr < len(table.rows)):
            return ""
        return art.icon_name(table.rows[ptr].get("IconIndex", 0))

    # A squadron's airbase is the objective its airbase_id points at; the
    # header's flyable records carry the printable name as a fallback, and
    # they are also where currentStrength lives.
    objectives_by_vuid = {}
    for o in cam.objectives:
        objectives_by_vuid.setdefault(tuple(o["id"]), o)
    header_by_vuid = {}
    for srec in (hdr.squadrons if hdr else []):
        header_by_vuid.setdefault(tuple(srec["id"]), srec)

    player_id = player_squadron_id(cam)
    units = []
    for u in cam.units:
        kind = u["kind"]
        drow = None
        if utbl and 0 <= rows[u["classIndex"]]["dataPtr"] < len(utbl.rows):
            drow = utbl.rows[rows[u["classIndex"]]["dataPtr"]]

        entry = {
            "n": u["_n"],
            "x": u["x"], "y": u["y"],
            "owner": u["owner"],
            "kind": kind,
            "type": u["classIndex"],
            "name": names.get(u["classIndex"], ""),
            "icon": icon_for(u["classIndex"], utbl),
            "campId": u["campId"],
            # The VU_ID is the only key a unit shares with the campaign
            # header's flyable-squadron records, so the Squadrons tab joins
            # on it.
            "id": u.get("id"),
            "isPlayer": (u["kind"] == "squadron" and
                         is_player_squadron(player_id, u.get("id"))),
            "wp": len(u.get("waypoints", [])),
            "makeup": unit_makeup(names, drow),
            # U_INACTIVE (0x20000, "generally reinforcements"): the unit exists in the
            # save but is off the map until the team's reinforcement counter reaches
            # `arrives` (campaign hours; one point per hour, see WIP-NOTES.md).
            "inactive": bool(u.get("unitFlags", 0) & 0x20000),
            "arrives": u.get("reinforcement") or 0,
        }

        # The fields the map's condition panel and quick actions edit.
        for key in ("supply", "morale", "fatigue", "losses", "orders", "fuel"):
            if key in u:
                entry[key] = u[key]

        health = unit_health(u, drow)
        if health is not None:
            entry["health"] = health

        if kind == "taskforce":
            entry["naval"] = naval_state(u, cam.objectives, nametab, names)

        if kind == "squadron":
            srec = header_by_vuid.get(tuple(u.get("id") or (0, 0)))
            airbase = objectives_by_vuid.get(tuple(u.get("airbaseId") or (0, 0)))
            patch = u.get("squadronPatch")
            entry.update({
                "sqName": composed_name(names, u["classIndex"], u.get("nameId")),
                "aircraft": first_vehicle(names, drow),
                "patch": patch,
                "patchName": patch_entry_name(ws, patch),
                "home": (place_name(nametab, names, airbase) if airbase else
                         (srec.get("airbaseName", "").strip() if srec else "")),
                "strength": srec.get("currentStrength") if srec else None,
                "selectable": bool(srec),
            })

        units.append(entry)

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
            "campId": o["campId"],
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
        "script": script_targets(ws, cam, name),
        "error": cam.units_error,
        "objectivesError": cam.objectives_error,
        "dirty": bool(getattr(cam, "units_dirty", False)),
    }


def api_map_image(q, _body):
    """Handled specially in do_GET -- it returns an image, not JSON."""
    raise ApiError("internal", 500)


def _squadrons_sharing(ws, special):
    """How many squadron CLASSES point at this .SSD row.

    The maximum column is class data, not squadron data: change it and every
    squadron built from a class with this SpecialIndex changes with it. The UI
    says so before letting anyone touch it.
    """
    utbl = ws.db.table("unit")

    if utbl is None:
        return 0

    return sum(1 for r in utbl.rows if r.get("SpecialIndex") == special)


# Falcon's own labels for the two enums a squadron carries. Both are small and
# neither is in a data file, so they live here rather than being guessed at in
# the browser. campaign/include/squadron.h and unit.h.
SPECIALTIES = ["General", "Air Superiority", "Ground Attack", "SEAD",
               "Reconnaissance", "Close Air Support", "Interdiction",
               "Strategic", "Naval", "Support"]

PILOT_STATUS = ["Available", "In Use", "MIA", "KIA", "Rescued", "Captured"]


def ordinal(n):
    """`GetNumberName` in campaign/campui/campstr.cpp, without the string table.

    A unit's nameId is a NUMBER, not an index into the place names -- 120 is
    the 120th, and resolving it against DEFAULT.idx the way an objective's
    nameId resolves gives a town in Korea instead of a squadron.
    """
    n = int(n or 0)

    if n % 10 == 1 and n != 11:
        suffix = "st"
    elif n % 10 == 2 and n != 12:
        suffix = "nd"
    elif n % 10 == 3 and n != 13:
        suffix = "rd"
    else:
        suffix = "th"

    return "%d%s" % (n, suffix)


def composed_name(names, class_index, name_id, size="Squadron"):
    """`UnitClass::GetName`: "<ordinal> <class name> <size>".

    This is the only thing that tells 126 squadrons apart -- the unit table
    row's own Name is the role ("Fighter"), which they all share.
    """
    cls = (names.get(class_index) or "").strip()
    return " ".join(x for x in (ordinal(name_id), cls, size) if x)


def first_vehicle_index(drow):
    """Class-table index of the first vehicle group a unit actually has."""
    if not drow:
        return None
    for vt, n in zip(drow.get("VehicleType") or [], drow.get("NumElements") or []):
        if vt and n:
            return vt
    return None


def first_vehicle(names, drow):
    vt = first_vehicle_index(drow)
    return names.get(vt, "") if vt is not None else ""


def unit_makeup(names, drow, limit=6):
    """What a unit is made of, deduped: [{"name": "T-72", "count": 3}, ...].

    A squadron lists the same aeroplane once per vehicle group, so the counts
    are summed by name; a battalion made of three types reads as three rows.
    """
    out, index = [], {}
    if not drow:
        return out
    for vt, n in zip(drow.get("VehicleType") or [], drow.get("NumElements") or []):
        if not vt or not n:
            continue
        nm = names.get(vt, "")
        if not nm:
            continue
        if nm in index:
            out[index[nm]]["count"] += int(n)
        else:
            index[nm] = len(out)
            out.append({"name": nm, "count": int(n)})
    return out[:limit]


def patch_entry_name(ws, index):
    """The art name behind a squadron patch index, or ""."""
    if index is None:
        return ""
    art = ws.ui_art()
    sheet = art.patches() if art else None
    if sheet is None:
        return ""
    entry = sheet.at(index)
    return entry.strip("_").replace("_", " ") if entry else ""


def resolve_weapon(ws, wid, count):
    """One hardpoint's weapon, resolved. Returns (name, is_list).

    `Weapon[hp]` is a row index: into FALCON4.WLD when `Weapons[hp]` is 255
    (the "this is a weapon list" marker LoadoutWeapons checks), into
    FALCON4.WCD otherwise. It is not a class-table id.
    """
    if not wid:
        return "", False
    if count == 255:
        tbl = ws.db.table("weaponlist")
        name = (tbl.rows[wid].get("Name") or "").strip() \
            if tbl and 0 <= wid < len(tbl.rows) else "?"
        return "list: " + name, True
    tbl = ws.db.table("weapon")
    name = (tbl.rows[wid].get("Name") or "").strip() \
        if tbl and 0 <= wid < len(tbl.rows) else "?"
    return name, False


def aircraft_loadout(ws, drow, names):
    """A squadron's aeroplane and its hardpoints, resolved for the sidebar."""
    aircraft = first_vehicle_index(drow)
    if aircraft is None:
        return None
    _tname, vrow = ws.db.data_row(aircraft)
    if vrow is None:
        return None

    rows = ws.db.class_rows()
    ptr = rows[aircraft]["dataPtr"] if 0 <= aircraft < len(rows) else None
    utbl = ws.db.table("unit")

    hardpoints = []
    for i, (wid, shots) in enumerate(zip(vrow["Weapon"], vrow["Weapons"])):
        nm, is_list = resolve_weapon(ws, wid, shots)
        hardpoints.append({"hp": i, "weapon": wid, "name": nm,
                           "isList": is_list, "shots": shots})

    return {
        "aircraft": names.get(aircraft, ""),
        "classIndex": aircraft,
        "vehicleRow": ptr,
        # The hardpoints are class data: editing them moves every squadron
        # flying this type. Say how many that is before anyone touches it.
        "sharedWith": sum(1 for r in utbl.rows
                          if first_vehicle_index(r) == aircraft) if utbl else 0,
        "hardpoints": hardpoints,
    }


def player_squadron_id(cam):
    """The VU_ID number of the squadron the player last flew, or None.

    The header's PlayerSquadronID is written at save time from the local
    session (`CampaignClass::Encode`), and the save masks it to 16 bits --
    `PlayerSquadronID.num_ &= 0x0000ffff` -- before renumbering it the same way
    the units are renumbered. So match on the low 16 bits. A fresh scenario or
    Instant.cam carries a stale default that matches nothing, which is the
    honest answer for a file nobody has flown yet.
    """
    hdr = cam.header
    pid = (hdr.fields.get("PlayerSquadronID") if hdr else None) or [0, 0]
    return (pid[0] & 0xFFFF) or None


def is_player_squadron(pid, vu_id):
    return bool(pid) and bool(vu_id) and (vu_id[0] & 0xFFFF) == pid


def header_squadron_view(ws, names, srec):
    """One campaign-header squadron record, with its name and aircraft joined.

    The header record and the unit share only a VU_ID; the name is the ordinal
    from `nameId` plus the class name `dIndex` points at, and the aircraft is
    that class's first vehicle group -- the same joins the campaign-select
    screen does (`SelectScenarioCB`).
    """
    _tname, drow = ws.db.data_row(srec.get("dIndex"))
    spec = srec.get("specialty") or 0
    country = srec.get("country") or 0
    return {
        "x": srec["x"], "y": srec["y"], "id": srec["id"],
        "dIndex": srec["dIndex"], "nameId": srec["nameId"],
        "airbaseIcon": srec["airbaseIcon"],
        "squadronPatch": srec["squadronPatch"],
        "specialty": spec,
        "specialtyName": SPECIALTIES[spec] if 0 <= spec < len(SPECIALTIES) else "",
        "currentStrength": srec["currentStrength"],
        "country": country,
        "countryName": COUNTRIES[country] if 0 <= country < len(COUNTRIES) else "",
        "airbaseName": srec["airbaseName"],
        "name": composed_name(names, srec.get("dIndex"), srec.get("nameId")),
        "aircraft": first_vehicle(names, drow),
        "patchName": patch_entry_name(ws, srec.get("squadronPatch")),
    }


def squadron_detail(ws, cam, unit, drow):
    """The things Mission Commander puts at the top of a squadron page.

    Most of it is on the squadron entity already. Two pieces are not:

    - the AIRCRAFT is the unit class's first vehicle type, the same join the
      composition list does;
    - the PATCH is an index into `art/resource/patches`, whose file order is the
      order of `imageids.id` and of the engine's SquadronMatchIDs -- all three
      agree, so the index resolves straight to an entry.

    The campaign header also keeps a parallel record for every squadron the
    player may fly. It is matched here by VU_ID, which is the only key the two
    lists share, and it is where the airbase's printable name lives.
    """
    names = ws.db.name_index()

    # `UnitClass::GetName` composes "<ordinal> <class name> <size>", which is
    # how "120th F-16C-52 ROKAF Squadron" is built. The class name is the same
    # one the unit list shows.
    composed = composed_name(names, unit.get("classIndex"), unit.get("nameId"))

    out = {
        "nameId": unit.get("nameId"),
        "ordinal": ordinal(unit.get("nameId")),
        "name": composed,
        "aircraft": "",
        "aircraftType": None,
        "specialty": unit.get("specialty"),
        "specialtyName": "",
        "patch": unit.get("squadronPatch"),
        "patchName": "",
        "airbase": "",
        "selectable": False,
        "kills": {"aa": unit.get("aaKills", 0), "ag": unit.get("agKills", 0),
                  "as": unit.get("asKills", 0), "an": unit.get("anKills", 0)},
        "missionsFlown": unit.get("missionsFlown", 0),
        "missionScore": unit.get("missionScore", 0),
        "totalLosses": unit.get("totalLosses", 0),
        "pilotLosses": unit.get("pilotLosses", 0),
        "fuel": unit.get("fuel", 0),
        "pilots": [],
    }

    spec = unit.get("specialty")

    if spec is not None and 0 <= spec < len(SPECIALTIES):
        out["specialtyName"] = SPECIALTIES[spec]

    # The aeroplane. VehicleType[0] is the squadron's aircraft; the rest of the
    # composition slots are empty for an air unit.
    if drow and drow.get("VehicleType"):
        for i, vt in enumerate(drow["VehicleType"]):
            if vt and drow["NumElements"][i]:
                out["aircraftType"] = vt
                out["aircraft"] = names.get(vt, "")
                break

    art = ws.ui_art()
    sheet = art.patches() if art else None

    if sheet is not None and unit.get("squadronPatch") is not None:
        entry = sheet.at(unit["squadronPatch"])
        if entry:
            out["patchName"] = entry.strip("_").replace("_", " ")

    # The header's player-selectable list, joined on VU_ID.
    if cam.header:
        mine = unit.get("id")
        for srec in cam.header.squadrons:
            if srec.get("id") == mine:
                out["selectable"] = True
                out["airbase"] = srec.get("airbaseName", "").strip()
                break

    for p in unit.get("pilots") or []:
        pid, skill_rating, status, aa, ag, as_, an, flown = p
        if not pid and not flown and not status:
            continue
        out["pilots"].append({
            "id": pid,
            "skill": skill_rating & 0x0F,
            "rating": (skill_rating >> 4) & 0x0F,
            "status": status,
            "statusName": (PILOT_STATUS[status]
                           if 0 <= status < len(PILOT_STATUS) else str(status)),
            "kills": {"aa": aa, "ag": ag, "as": as_, "an": an},
            "missions": flown,
        })

    return out


def patch_png(ws, index):
    """One squadron patch as PNG bytes, by its index in the patch sheet."""
    art = ws.ui_art()
    sheet = art.patches() if art else None

    if sheet is None:
        return None, "this theater has no art/resource/patches"

    entry = sheet.at(index)

    if not entry:
        return None, ("patch %d is outside the %d-entry sheet"
                      % (index, len(sheet.order)))

    decoded = sheet.rgba(entry)

    if not decoded:
        return None, "patch %s did not decode" % entry

    w, h, px = decoded
    return uiart.write_png(w, h, px), entry


def aircraft_weapons(ws, drow):
    """Every weapon the squadron's aircraft can hang on a hardpoint.

    This is the list the loadout screen shows (ui/src/campaign/munition.cpp):
    the aircraft's vehicle class, hardpoints 1..n, each either one weapon or,
    when its count is 255, a row of the weapon-list table naming several.
    Whether each one reads OUT or HIGH is then the squadron's stock over the
    stores maximum -- so a weapon here with a maximum of 0 is the "OUT" that
    no amount of waiting fixes.
    """
    wl = ws.db.table("weaponlist")
    try:
        _tname, vrow = ws.db.data_row(drow["VehicleType"][0])
    except Exception:
        return set()
    if not vrow or "Weapon" not in vrow:
        return set()
    got = set()
    for i in range(1, len(vrow["Weapon"])):
        w, n = vrow["Weapon"][i], vrow["Weapons"][i]
        if not w or not n:
            continue
        if n == 255:
            if wl is not None and 0 <= w < len(wl.rows):
                got.update(x for x in wl.rows[w]["WeaponID"] if x)
        else:
            got.add(w)
    return got


def squadron_stores(ws, unit, drow, show_all=False):
    """What a squadron is armed with, joined against its class maximums.

    Two tables meet here. The squadron entity carries `stores[i]` -- how many
    supply points of weapon i it actually holds -- and its unit class points at
    a row of FALCON4.SSD through `SpecialIndex`, which holds the maximum for
    each weapon plus the three weapons this squadron never runs out of.
    `SquadronClass::GetAvailableStores` divides one by the other:

        avail = (have * 4) / max        0..4, and 4 for an infinite weapon

    That quotient is what the loadout screen colours by, so it is what the
    editor shows. With a maximum of 0 the engine returns the raw stock instead
    (and asserts), and campaign resupply never adds any -- so in practice a 0
    maximum is a weapon the aircraft can carry but the squadron is never
    issued: OUT for the whole war. The default view is what the loadout
    screen lists (the aircraft's weapons) plus anything else with a maximum;
    Show all adds the other 500-odd slots.
    """
    stores = unit.get("stores")

    if stores is None or not drow or "SpecialIndex" not in drow:
        return {"available": False,
                "reason": "this unit has no squadron stores"}

    ssd = ws.db.table("squadstores")
    wcd = ws.db.table("weapon")

    if ssd is None or wcd is None:
        return {"available": False,
                "reason": "FALCON4.SSD or FALCON4.WCD is missing"}

    special = drow["SpecialIndex"]

    if not (0 <= special < len(ssd.rows)):
        return {"available": False,
                "reason": "SpecialIndex %d is outside the %d-row .SSD"
                          % (special, len(ssd.rows))}

    srow = ssd.rows[special]
    maxima = srow["Stores"]
    infinite = {"ag": srow["infiniteAG"], "aa": srow["infiniteAA"],
                "gun": srow["infiniteGun"]}
    never_out = set(v for v in infinite.values() if v)
    loadable = aircraft_weapons(ws, drow)

    rows = []

    for i in range(min(len(stores), len(maxima))):
        count, cap = stores[i], maxima[i]
        forever = i in never_out

        # A maximum of 0 means the class cannot arm with this weapon at all --
        # whatever stock happens to be sitting in the entity's array, and there
        # is a lot of it, because the array is not cleared when a squadron is
        # created. Those rows are noise unless explicitly asked for.
        if not show_all and not cap and not forever and i not in loadable:
            continue

        name = ""

        if i < len(wcd.rows):
            name = (wcd.rows[i].get("Name") or "").strip()

        rows.append({
            "index": i,
            "name": name or ("weapon %d" % i),
            "count": count,
            "max": cap,
            "infinite": forever,
            "carryable": bool(cap) or forever,
            # On the aircraft's hardpoints: the loadout screen lists it.
            "loadable": i in loadable,
            # Listed there, and OUT for good until the maximum is raised.
            "locked": i in loadable and not cap and not forever and not count,
            # Max 0 but stock on the shelf: GetAvailableStores returns the raw
            # stock then, so it can be loaded until it runs out -- and never
            # comes back, because resupply skips a 0 maximum.
            "stockOnly": i in loadable and not cap and not forever and bool(count),
            # The engine's own 0..4, so the editor colours the way the game does.
            "avail": 4 if forever else ((count * 4) // cap if cap else 0),
        })

    rows.sort(key=lambda r: (r["name"].lower(), r["index"]))

    return {
        "available": True,
        "specialIndex": special,
        "infinite": infinite,
        "infiniteNames": {k: ((wcd.rows[v].get("Name") or "").strip()
                              if 0 < v < len(wcd.rows) else "")
                          for k, v in infinite.items()},
        "length": unit.get("_storesLen", len(stores)),
        "shown": len(rows),
        "carryable": sum(1 for i in range(min(len(stores), len(maxima)))
                         if maxima[i]) + len(never_out - set(
                             i for i in range(len(maxima)) if maxima[i])),
        "sharedWith": _squadrons_sharing(ws, special),
        "locked": sum(1 for r in rows if r["locked"]),
        "rows": rows,
    }


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

    show_all = (q.get("allstores") or ["0"])[0] not in ("0", "", "false")

    return {
        "n": n,
        "kind": u["kind"],
        "name": names.get(u["classIndex"], ""),
        # The pane header: a squadron's real name is composed, not the class
        # name, which is the role they all share ("Fighter").
        "title": (composed_name(names, u["classIndex"], u.get("nameId"))
                  if u["kind"] == "squadron" else names.get(u["classIndex"], "")),
        "editable": sorted(k for k in entities.PATCHABLE if k in u),
        "values": {k: v for k, v in u.items()
                   if k not in ("waypoints", "stores", "pilots")
                   and not k.startswith("_")},
        "stores": (squadron_stores(ws, u, drow, show_all)
                   if u["kind"] == "squadron" else None),
        "squadron": (squadron_detail(ws, cam, u, drow)
                     if u["kind"] == "squadron" else None),
        "loadout": (aircraft_loadout(ws, drow, names)
                    if u["kind"] == "squadron" else None),
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
        try:
            # patch_unit clamps to the range the engine expects; record the
            # effective value so the response is what is really on disk.
            raw = entities.patch_unit(raw, u, cam.version, field, value)
        except ValueError as exc:
            raise ApiError(str(exc))
        u[field] = entities.clamp_value(field, value)
    cam.units_raw = raw
    cam.units_dirty = True
    cam.dirty = True
    return {"ok": True, "values": {k: u[k] for k in entities.PATCHABLE
                                   if k in u}}


def api_unit_stores(_q, body):
    """Set one weapon's stock on a squadron, or refill/strip the whole rack."""
    ws = SESSION.workspace(body["theater"])
    name = body["file"]
    cam = ws.units(name)
    n = int(body["n"])

    if not (0 <= n < len(cam.units)):
        raise ApiError("no unit %d" % n, 404)

    u = cam.units[n]

    if u["kind"] not in ("squadron",):
        raise ApiError("%s units carry no stores" % u["kind"])

    _tname, drow = ws.db.data_row(u["classIndex"])
    info = squadron_stores(ws, u, drow, True)

    if not info["available"]:
        raise ApiError(info["reason"])

    ssd = ws.db.table("squadstores")
    maxima = ssd.rows[info["specialIndex"]]["Stores"]

    action = body.get("action")
    raw = cam.units_raw
    changed = 0

    if action == "unlock":
        # Every weapon the aircraft can load that the stores row never issues:
        # give it a maximum, and this squadron a full stock of it. Class data,
        # like any maximum -- the note on the panel says who else moves.
        cap = max(1, min(255, int(body.get("max", 100))))
        srow = ssd.rows[info["specialIndex"]]
        for r in info["rows"]:
            if not r["locked"]:
                continue
            srow["Stores"][r["index"]] = cap
            raw = entities.patch_stores(raw, u, r["index"], cap)
            changed += 1
        if changed:
            ssd.dirty = True
        maxima = srow["Stores"]
    elif action in ("resupply", "clear"):
        # Only touch what the class can actually carry. Writing stock for a
        # weapon whose maximum is 0 does nothing in game and only makes the
        # file differ from what the engine would ever produce.
        for i in range(min(len(u["stores"]), len(maxima))):
            want = maxima[i] if action == "resupply" else 0

            if not maxima[i] or u["stores"][i] == want:
                continue

            raw = entities.patch_stores(raw, u, i, want)
            changed += 1
    elif "max" in body:
        # Class data. This is the real "can this squadron carry it at all"
        # switch, and it moves for every squadron class sharing the row.
        index = int(body["index"])
        cap = max(0, min(255, int(body["max"])))

        if not 0 <= index < len(maxima):
            raise ApiError("weapon %d is outside the %d-entry stores table"
                           % (index, len(maxima)))

        srow = ssd.rows[info["specialIndex"]]
        srow["Stores"][index] = cap
        ssd.dirty = (ssd.spec.pack(srow, ssd.raw[info["specialIndex"]])
                     != ssd.raw[info["specialIndex"]]) or ssd.dirty
        maxima = srow["Stores"]

        # Stock above the new ceiling would read as more than full, so bring it
        # down with the ceiling rather than leaving the file inconsistent.
        if cap and u["stores"][index] > cap:
            raw = entities.patch_stores(raw, u, index, cap)
        elif not cap and u["stores"][index]:
            # With a maximum of 0 the engine reports the raw stock as the
            # availability, so stock left behind would still arm the jet;
            # switching a weapon off has to take the stock away too.
            raw = entities.patch_stores(raw, u, index, 0)
        elif cap and body.get("fill"):
            # Enabling a weapon: stock it now rather than waiting for the
            # next supply run, or it still reads OUT on the loadout screen.
            raw = entities.patch_stores(raw, u, index, cap)

        changed = 1
    else:
        index = int(body["index"])
        count = int(body["count"])

        if 0 <= index < len(maxima) and maxima[index] and count > maxima[index]:
            raise ApiError(
                "%d is more than this squadron class can hold (max %d). The "
                "engine divides by that maximum to get availability, so a "
                "larger number does not mean more than full -- raise the "
                "maximum if that is what you want."
                % (count, maxima[index]))

        try:
            raw = entities.patch_stores(raw, u, index, count)
        except ValueError as exc:
            raise ApiError(str(exc))

        changed = 1

    cam.units_raw = raw
    cam.units_dirty = True
    cam.dirty = True

    return {"ok": True, "changed": changed,
            "stores": squadron_stores(ws, u, drow,
                                      bool(body.get("showAll")))}


# falclib/include/f4vu.h. Only 0..3 fit in the two bits a feature gets on
# disk; the engine's 4..6 (left/right/both destroyed) never reach the file.
FEATURE_STATUS = ["Normal", "Repaired", "Damaged", "Destroyed"]

# campaign/include/feature.h
FEAT_PREV_CRIT = 0x01
FEAT_NEXT_CRIT = 0x02


def objective_features(ws, o, drow):
    """Every feature of an objective, with its name, value and damage state.

    The status is two bits in the record's feature-status block; the engine
    recomputes the objective's condition from those bits on load, so they are
    the real repair/destroy lever. Returns (features, slots-in-the-block).
    """
    features = []
    names = ws.db.name_index()
    ftbl = ws.db.table("featureentry")
    first, count = (drow["FirstFeature"], drow["Features"]) if drow else (0, 0)
    slots = len(o.get("featureStatus") or b"") * 4

    if ftbl and count:
        for i in range(first, min(first + count, len(ftbl.rows))):
            slot = i - first
            # The block on disk can be shorter than the class table implies;
            # the engine reads the smaller of the two, and so do we.
            if slot >= slots:
                break
            fid = ftbl.rows[i]["Index"]
            st = objectives.feature_status(o, slot)
            features.append({
                "slot": slot, "index": fid,
                "name": names.get(fid, ""),
                "value": ftbl.rows[i]["Value"],
                "status": st,
                "statusName": FEATURE_STATUS[st] if st is not None else "",
            })

    return features, slots


def _fed_flags(ftbl, first, feature):
    if ftbl is None:
        return 0
    i = first + feature
    if not (0 <= i < len(ftbl.rows)):
        return 0
    return ftbl.rows[i].get("Flags", 0) or 0


def _critical_chain(ftbl, first, total, feature, status, came_from=None):
    """The features ObjectiveClass::SetFeatureStatus would set with this one.

    Destroying or repairing a feature walks its FEAT_PREV_CRIT /
    FEAT_NEXT_CRIT neighbours; `came_from` stops the walk doubling back on
    itself. Damaged and normal states do not propagate.
    """
    out = []
    if status in (1, 3):
        if came_from != feature - 1 and feature > 0 and \
                (_fed_flags(ftbl, first, feature) & FEAT_PREV_CRIT):
            out += _critical_chain(ftbl, first, total, feature - 1, status,
                                   feature)
        if came_from != feature + 1 and feature + 1 < total and \
                (_fed_flags(ftbl, first, feature) & FEAT_NEXT_CRIT):
            out += _critical_chain(ftbl, first, total, feature + 1, status,
                                   feature)
    out.append(feature)
    return out


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

    features, slots = objective_features(ws, o, drow)

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
        "featureSlots": slots,
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
        try:
            # patch_objective clamps to the range the engine expects; record
            # the effective value so the response is what is on disk.
            raw = objectives.patch_objective(raw, o, cam.version, field, value)
        except ValueError as exc:
            raise ApiError(str(exc))
        o[field] = objectives.clamp_value(field, value)
    cam.objectives_raw = raw
    cam.objectives_dirty = True
    cam.dirty = True
    return {"ok": True, "values": {k: o[k] for k in objectives.PATCHABLE}}


def api_objective_feature(_q, body):
    """Damage or repair features on one objective.

    `{feature, status}` sets one, propagating across critical links the way
    `ObjectiveClass::SetFeatureStatus` does; `{action: "repair-all" |
    "destroy-all"}` moves every feature at once.
    """
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
    _t, drow = ws.db.data_row(o["classIndex"])
    features, slots = objective_features(ws, o, drow)

    counts = [x for x in (len(features), slots) if x]
    total = min(counts) if counts else 0
    if not total:
        raise ApiError("this objective has no features to damage")

    action = body.get("action")
    if action in ("repair-all", "destroy-all"):
        status = 0 if action == "repair-all" else 3
        targets = list(range(total))
    else:
        feature = int(body["feature"])
        status = int(body["status"])
        if not 0 <= feature < total:
            raise ApiError("feature %d is outside this objective's %d features"
                           % (feature, total))
        if not 0 <= status <= 3:
            raise ApiError("status must be 0..3 (normal, repaired, damaged, "
                           "destroyed), not %d" % status)
        # Match the engine: destroy/repair propagates across critical links.
        ftbl = ws.db.table("featureentry")
        first = drow["FirstFeature"] if drow else 0
        targets = []
        for f in _critical_chain(ftbl, first, total, feature, status):
            if f not in targets:
                targets.append(f)

    raw = cam.objectives_raw
    for f in targets:
        raw = objectives.patch_feature_bits(raw, o, cam.version, f, status)
    objectives.refresh_feature_status(raw, o, cam.version)
    cam.objectives_raw = raw
    cam.objectives_dirty = True
    cam.dirty = True

    features, slots = objective_features(ws, o, drow)
    return {"ok": True, "changed": len(targets), "status": status,
            "statusName": FEATURE_STATUS[status],
            "featureCount": len(features), "featureSlots": slots,
            "features": features[:64]}


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

    Two things live outside the .cam and have to come across too: the trigger
    script (`<scenario>.tri`, which is what actually ends a campaign) and, if
    the source is a mid-campaign save, the base objective list the engine
    reads from the header's Scenario field. Without those a copy would be a
    campaign that cannot end and has no objectives.
    """
    ws = SESSION.workspace(body["theater"])
    source = body["source"]
    raw_name = (body.get("name") or "").strip()
    if not raw_name:
        raise ApiError("the new campaign needs a name")

    stem = "".join(c for c in raw_name if c.isalnum() or c in " _-").strip()
    if not stem:
        raise ApiError("that name has no usable characters in it")

    try:
        info = workspace.copy_campaign(ws, source, stem,
                                       body.get("uiName") or "")
    except FileExistsError as exc:
        raise ApiError(str(exc))
    except ValueError as exc:
        raise ApiError("cannot copy %s: %s" % (source, exc))

    SESSION.forget(body["theater"])
    return {"ok": True, **info}


def _base_scenario_file(ws, scenario):
    """The campaign file a save's header names as its scenario, or None."""
    scenario = (scenario or "").strip()
    if not scenario:
        return None
    if scenario.lower().endswith((".cam", ".tac")):
        candidates = [scenario]
    else:
        candidates = [scenario + ".cam", scenario + ".tac"]
    for candidate in candidates:
        try:
            cam = ws.campaign(candidate)
        except ValueError:
            continue
        if cam.member("obj") is not None:
            return candidate, cam
    return None


def _check_scenario_change(ws, cam, scenario):
    """A save's base objectives come from the scenario it names.

    Changing the field on a file that has an objective list of its own is
    free -- it is a scenario. Changing it on a save repoints where the base
    list is read from, so the target has to exist and carry one.
    """
    if cam.member("obj") is not None:
        return
    if not (scenario or "").strip():
        raise ApiError("a save needs a scenario name: its base objectives and "
                       "its trigger script are read from it")
    if _base_scenario_file(ws, scenario) is None:
        raise ApiError(
            "no campaign file here called %r carries an objective list, so a "
            "save cannot use it as its base scenario. Use New campaign to "
            "make a copy of the scenario first." % scenario)


def api_save_ending(_q, body):
    """Give a mid-campaign save victory conditions of its own.

    A save has no script of its own: the engine reads `<Scenario>.tri` and the
    base objective list from `<Scenario>.cam`, both named by the header's
    Scenario field. So the way to change a save's ending without touching the
    scenario it came from is to make it a scenario of its own -- a copy of the
    base scenario, carrying the same objective list and a script you can edit
    -- and point the save at it. Units, teams, pilots and the objective deltas
    still come from the save; only the base list and the script move.
    """
    ws = SESSION.workspace(body["theater"])
    name = body["file"]
    try:
        cam = ws.campaign(name)
    except ValueError as exc:
        raise ApiError(str(exc), 404)
    if cam.member("obj") is not None:
        raise ApiError("%s carries its own objective list, so it already has "
                       "an ending of its own" % name)
    if cam.header is None:
        raise ApiError("%s has no campaign header" % name)

    scenario = (cam.header.fields.get("Scenario") or "").strip()
    found = _base_scenario_file(ws, scenario)
    if found is None:
        raise ApiError("this save names %r as its scenario, but no campaign "
                       "file here carries that objective list" % scenario)
    source, _base_cam = found

    raw = (body.get("name") or "").strip()
    if not raw:
        raise ApiError("the new scenario needs a name")
    stem = "".join(c for c in raw if c.isalnum() or c in " _-").strip()
    if not stem:
        raise ApiError("that name has no usable characters in it")
    if stem.lower() == os.path.splitext(name)[0].lower():
        raise ApiError("the scenario copy cannot have the save's own name")

    try:
        info = workspace.copy_campaign(ws, source, stem,
                                       body.get("uiName") or "")
    except FileExistsError as exc:
        raise ApiError(str(exc))
    except ValueError as exc:
        raise ApiError("cannot copy %s: %s" % (source, exc))

    # Point the save at the copy. Its objective list is the one the save's
    # deltas were made against, so the deltas still apply.
    cam.header.fields["Scenario"] = stem
    cam.header_dirty = True
    cam.dirty = True
    cam.objectives = None           # re-resolve against the new base
    cam.objectives_from = name
    ws.forget_script(name)

    return {"ok": True, "save": name, "scenario": info["file"],
            "script": info["script"], "from": source}


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
    sc, from_scenario = ws.script(name)
    if not sc:
        cam = ws.objectives(name)
        scenario = ""
        if cam.header:
            scenario = (cam.header.fields.get("Scenario") or "").strip()
        stem = os.path.splitext(name)[0]
        if scenario and scenario.lower() != stem.lower():
            reason = ("no %s.tri for the scenario this file names (%s), and no "
                      "%s.tri beside it" % (scenario, scenario, stem))
        else:
            reason = "no %s.tri next to this campaign" % stem
        return {"available": False, "reason": reason,
                "ownObjectives": cam.member("obj") is not None}

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

    # Annotate the listing with the three things a reader cannot see from the
    # tokens: what a movie id actually plays, which actions this build ignores,
    # and which events are written but never read.
    art = ws.ui_art()
    movies = art.movies() if art else {}
    write_only = triggers.write_only_events(sc.init, sc.body)
    outline = triggers.outline(sc.body, teams, place)
    for row in outline:
        row["note"] = _script_row_note(row, movies, write_only)

    # Which scripted events have fired in THIS file (a scenario has none; a save
    # carries the flags in its .evt member).
    fired = triggers.fired_events(cam.member("evt"))
    titles = triggers.event_titles(sc.lines)
    for row in outline:
        if row.get("verb") in ("DO_EVENT", "RESET_EVENT", "IF_EVENT_PLAYED",
                               "SET_EVENT") and row.get("args"):
            try:
                ev = int(row["args"][0])
            except ValueError:
                continue
            row["event"] = ev
            row["fired"] = ev in fired
    event_ids = sorted(set(titles) | set(fired))
    hist = triggers.event_history(cam.member("evt"))
    day_zero = (cam.header.fields.get("DayZero", 0) if cam.header else 0) or 0
    when = {}
    for h in hist:
        if h["kind"] == "event":
            when[h["id"]] = h["time"]          # the last time it fired
    events = []
    for i in event_ids:
        e = {"id": i, "title": titles.get(i, ""), "fired": i in fired}
        if i in when:
            e["day"], e["clock"] = triggers.describe_time(when[i], day_zero)
            e["time"] = when[i]
        events.append(e)
    # Which branch did it: the save's own record of the #IF chain behind each action, or, for
    # saves from before that, a re-evaluation against the team stats in the .frc beside it.
    frc_hist = []
    frc_path = os.path.splitext(getattr(cam, "path", "") or "")[0] + ".frc"
    if os.path.exists(frc_path):
        with open(frc_path, "rb") as fp:
            frc_hist = triggers.force_history(fp.read())
    conds = triggers.condition_history(cam.member("evt"))
    taken = triggers.branch_report(sc.body, hist, conds, frc_hist)
    nodes = {}

    def index(ns):
        for n in ns:
            nodes[n.line] = n
            index(n.children)
            if n.orelse:
                index(n.orelse)
    index(sc.body)
    for row in outline:
        for t in taken.get(row.get("line"), []):
            day, clock = triggers.describe_time(t["time"], day_zero)
            node = nodes.get(row["line"])
            t2 = dict(t, day=day, clock=clock,
                      measured=triggers.describe_measure(node, t["a"], t["b"], teams) if node else "")
            row.setdefault("taken", []).append(t2)
    # The #ELSE divider rows carry no line; tag them from their #IF so the listing can light
    # the branch that ran rather than every DO_EVENT of a fired event.
    for k, row in enumerate(outline):
        if row.get("kind") == "else":
            for j in range(k - 1, -1, -1):
                r = outline[j]
                if r.get("kind") == "condition" and r["depth"] == row["depth"]:
                    row["elseOf"] = r["line"]
                    row["taken"] = [t for t in r.get("taken", []) if t["branch"] == "else"]
                    break
    history = []
    for h in sorted(hist, key=lambda x: x["time"]):
        day, clock = triggers.describe_time(h["time"], day_zero)
        history.append({"kind": h["kind"], "id": h["id"], "day": day, "clock": clock,
                        "title": titles.get(h["id"], "") if h["kind"] == "event" else
                                 (movies.get(h["id"]) or {}).get("title", "") if isinstance(movies, dict) else ""})
    slot = camptext.slot_of(name)
    text = ws.campaign_text()
    blurb = camptext.blurb_from_script(ends)

    return {
        "available": True,
        "file": os.path.basename(sc.path),
        # Set when the script belongs to the scenario this file was started
        # from -- a mid-campaign save has no script of its own.
        "fromScenario": from_scenario,
        # A file with its own objective list is a scenario; one without is a
        # save, and only a save can be given an ending of its own.
        "ownObjectives": cam.member("obj") is not None,
        "totalEvents": sc.total,
        "dirty": sc.dirty,
        "init": [{"verb": n.verb, "text": triggers.describe(n, teams, place),
                  "comment": n.comment} for n in sc.init],
        "endgames": ends,
        "outline": outline,
        "events": events,
        "eventHistory": history,
        "eventsFromSave": cam.member("evt") is not None,
        "writeOnlyEvents": write_only,
        "deadActions": sorted(triggers.DEAD_ACTIONS),
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


def _script_row_note(row, movies, write_only):
    """What one outline row does not say for itself.

    A `#PLAY_MOVIE 104` is a number until MOVIES.ID and movies.irc name it; a
    `#SET_TEMPO` reads like an order but this build ignores it; and an event
    write whose flag nothing tests is a latch with no reader.
    """
    verb = row.get("verb")
    args = row.get("args") or []

    if verb == "PLAY_MOVIE" and args:
        try:
            movie = movies.get(int(args[0]))
        except ValueError:
            movie = None
        if movie:
            return "%s \u00b7 %s" % (movie["title"], movie["file"])

    if verb in triggers.DEAD_ACTIONS:
        return "no effect in this build"

    if verb in ("DO_EVENT", "RESET_EVENT", "SET_EVENT") and args:
        try:
            event = int(args[0])
        except ValueError:
            return ""
        if event in write_only:
            return "nothing tests event %d" % event

    return ""


def api_trigger_edit(_q, body):
    """Change one directive in a campaign's trigger script."""
    ws = SESSION.workspace(body["theater"])
    name = body["file"]
    sc, _from = ws.script(name)
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
        items = [{"id": i, "row": i, "name": n}
                 for i, n in sorted(ws.db.name_index().items())]
    else:
        tbl = ws.db.table(kind)
        if tbl is None:
            raise ApiError("no %s table" % kind, 404)
        label = tbl.spec.label or "Name"
        # `row` is the position in the table, which is what the .cam streams
        # index by (a hardpoint's Weapon is a row in .WCD or .WLD, not a
        # class-table id); `id` stays the class-table index for other callers.
        items = [{"id": r.get("Index", i), "row": i, "name": r.get(label, "")}
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
    "/api/weather": api_weather,
    "/api/progress": api_progress,
    "/api/aircraft": api_aircraft,
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
    "/api/unit/stores": api_unit_stores,
    "/api/weather/edit": api_weather_edit,
    "/api/weather/preview": api_weather_preview,
    "/api/weather/generate": api_weather_generate,
    "/api/unit/add": api_unit_add,
    "/api/unit/delete": api_unit_delete,
    "/api/objective/edit": api_objective_edit,
    "/api/objective/feature": api_objective_feature,
    "/api/campaign/new": api_campaign_new,
    "/api/campaign/delete": api_campaign_delete,
    "/api/save/ending": api_save_ending,
    "/api/theater/create": api_create_theater,
    "/api/theater/delete": api_delete_theater,
    "/api/save": api_save,
    "/api/discard": api_discard,
}


# --- HTTP --------------------------------------------------------------------

class Handler(BaseHTTPRequestHandler):
    server_version = "FFCampaignEditor/1.0"
    # Keep-alive, so a screenful of tiles reuses one connection; every
    # response here sets Content-Length, which 1.1 requires.
    protocol_version = "HTTP/1.1"

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
        if parsed.path == "/api/patch":
            return self._serve_patch(parse_qs(parsed.query))
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
        # First tile of the session: start decoding the texture library in the
        # background so later pans are cache hits.
        t.warm()
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

    def _serve_patch(self, query):
        """A squadron patch, straight out of the game's own art."""
        try:
            ws = _ws(query)
        except ApiError as exc:
            return self._send_json({"error": str(exc)}, exc.status)

        try:
            index = int((query.get("i") or ["0"])[0])
        except ValueError:
            return self._send_json({"error": "patch index must be a number"},
                                   400)

        blob, why = patch_png(ws, index)

        if blob is None:
            return self._send_json({"error": why}, 404)

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

    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    # Tile requests render concurrently; keep-alive connections hold a thread
    # each, so do not let them delay interpreter exit either way.
    server.daemon_threads = True
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
