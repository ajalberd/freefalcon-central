"""Load a campaign save from FreeFalcon, vanilla Falcon 4.0 or BMS 4.x into one shape.

FF/vanilla ship binary class tables (FALCON4.ct/.UCD); BMS ships XML
(Falcon4_CT.xml / Falcon4_UCD.xml).  The save itself is the same container, so
tools/campaign-editor/ffcamp decodes it once it is handed class rows.
"""
import os
import sys
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "campaign-editor"))
from ffcamp import campfile, entities, objectives, records, campdb  # noqa: E402

SOURCES = {
    "vanilla": dict(
        label="Falcon 4.0 (GOG)",
        cam=r"D:\GOG Games\Falcon 4.0\campaign\SAVE\save0.cam",
        tables=r"D:\GOG Games\Falcon 4.0\terrdata\objects", kind="bin"),
    "bms": dict(
        label="BMS 4.37",
        cam=r"C:\Falcon BMS 4.37\Data\Campaign\Save0.cam",
        tables=r"C:\Falcon BMS 4.37\Data\Terrdata\Objects", kind="xml"),
    "ff6": dict(
        label="FreeFalcon6",
        cam=r"C:\FreeFalcon6\campaign\SAVE\save0.cam",
        tables=r"C:\FreeFalcon6\campaign\SAVE\CampaignDB", kind="bin"),
}


def _xml_rows(path, tag):
    out = []
    for _, el in ET.iterparse(path):
        if el.tag == tag:
            out.append({c.tag: (c.text or "").strip() for c in el})
            el.clear()
    return out


def load_tables(src):
    """Return (class_rows, ucd_by_class_index)."""
    d = src["tables"]
    if src["kind"] == "bin":
        rows = campdb.Table.load(records.CLASS_ENTRY, os.path.join(d, "FALCON4.ct")).rows
        ucd = campdb.Table.load(records.UNIT, os.path.join(d, "FALCON4.UCD")).rows
        by = {}
        for ci, e in enumerate(rows):
            if e["dataType"] == 4 and 0 <= e["dataPtr"] < len(ucd):
                by[ci] = ucd[e["dataPtr"]]
        return rows, by
    ct = _xml_rows(os.path.join(d, "Falcon4_CT.xml"), "CT")
    ucdx = _xml_rows(os.path.join(d, "Falcon4_UCD.xml"), "UCD")
    rows = []
    for r in ct:
        info = [int(r[k]) for k in ("Domain", "Class", "Type", "SubType",
                                    "Specific", "Owner", "Class_6", "Class_7")]
        et = int(r.get("EntityType", 0))
        rows.append({"classInfo_": info, "dataType": et,
                     "dataPtr": int(r.get("EntityIdx", -1)),
                     "id_": int(r["Id"]), "visType": [0] * 7,
                     "hitpoints_": int(r.get("HitPoints", 0))})
    by = {}
    for ci, e in enumerate(rows):
        if e["dataType"] == 4 and 0 <= e["dataPtr"] < len(ucdx):
            u = ucdx[e["dataPtr"]]
            by[ci] = {
                "NumElements": [int(u.get("ElementCount_%d" % i, 0)) for i in range(16)],
                "Name": u.get("Name", ""), "Role": int(u.get("MainRole", 0)),
                "MovementSpeed": int(u.get("MoveSpeed", 0)),
                "Strength": [int(u.get(k, 0)) for k in (
                    "Str_NoMove", "Str_Foot", "Str_Wheeled", "Str_Tracked",
                    "Str_LowAir", "Str_Air", "Str_Naval", "Str_Rail")],
                "HitChance": [int(u.get(k, 0)) for k in (
                    "Hit_NoMove", "Hit_Foot", "Hit_Wheeled", "Hit_Tracked",
                    "Hit_LowAir", "Hit_Air", "Hit_Naval", "Hit_Rail")],
                "Range": [int(u.get(k, 0)) for k in (
                    "Rng_NoMove", "Rng_Foot", "Rng_Wheeled", "Rng_Tracked",
                    "Rng_LowAir", "Rng_Air", "Rng_Naval", "Rng_Rail")],
            }
    return rows, by


def _decode_lite(cam, rows):
    """BMS's unit records are longer than ffcamp knows (a squadron is 1696
    bytes, not 1272) and carry no length prefix.  Every record starts
    `type, VU_ID(num, 0), type` though, so anchor on that and read only the
    CampBase + UnitClass head, which is unchanged."""
    import struct
    from ffcamp import lzss
    sec = cam.member("uni")
    s = entities.Stream(sec, 4)
    count = s.i16()
    size = s.i32()
    raw = lzss.expand(sec[s.pos:], size)
    starts = []
    for i in range(len(raw) - 14):
        t = struct.unpack_from("<H", raw, i)[0]
        if (struct.unpack_from("<H", raw, i + 10)[0] == t
                and struct.unpack_from("<I", raw, i + 6)[0] == 0
                and 100 <= t < 100 + len(rows)):
            num = struct.unpack_from("<I", raw, i + 2)[0]
            if 1 <= num < 60000:
                starts.append(i)
    if len(starts) != count:
        raise ValueError("anchored %d records, header says %d" % (len(starts), count))
    units = []
    for i in starts:
        st = entities.Stream(raw, i)
        type_id = st.i16()
        idx = type_id - entities.VU_LAST_ENTITY_TYPE
        info = rows[idx]["classInfo_"]
        r = entities._camp_base(st, 108)
        r["classIndex"] = idx
        r["lastCheck"] = st.u32()
        r["roster"] = st.u32()
        r["unitFlags"] = st.u32()
        dom, typ = info[0], info[2]
        r["kind"] = {(2, 1): "flight", (2, 2): "package", (2, 3): "squadron",
                     (3, 1): "battalion", (3, 2): "brigade",
                     (4, 1): "taskforce"}.get((dom, typ), "other")
        units.append(r)
    return units


def load(key, cam_path=None):
    """cam_path overrides the source's save (e.g. a live Auto Save with the same class tables)."""
    src = SOURCES[key]
    cam = campfile.CampaignFile.load(cam_path or src["cam"])
    rows, by = load_tables(src)
    if key == "bms":
        units = _decode_lite(cam, rows)
    else:
        units = entities.decode_units(cam.member("uni"), cam.version, rows)[0]
    for u in units:
        row = by.get(u["classIndex"])
        u["veh"] = sum((u["roster"] >> (2 * i)) & 3 for i in range(16))
        u["full"] = int(sum(row["NumElements"])) if row else 0
        u["name"] = (row.get("Name") or "") if row else ""
        u["role"] = int(row.get("Role", 0)) if row else 0
        info = rows[u["classIndex"]]["classInfo_"]
        u["stype"], u["sptype"] = info[3], info[4]
        u["ucd"] = row
    try:
        objs, _ = objectives.decode_objectives(cam.member("obj"), cam.version, rows)
    except Exception as exc:  # BMS objectives are not decoded yet
        objs = []
        print("objectives not decoded for %s: %s" % (key, exc), file=sys.stderr)
    return dict(key=key, label=src["label"], cam=cam, units=units, objs=objs,
                rows=rows, ucd=by)
