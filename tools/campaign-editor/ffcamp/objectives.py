"""Decode the objective list inside a .cam / .tac.

The `.obj` member is the theater's static furniture: airbases, cities, towns,
villages, bridges, factories, power plants, ports, radars, SAM sites — every
place the campaign can own, damage or fly to. It is a VU entity stream like
`.uni`, but simpler, because there is only one class.

    short  count
    int32  raw size
    int32  compressed size (informational; LZSS_Expand uses the raw size)
    LZSS(raw)

and each record is `short type` followed by ObjectiveClass's own fields
(`src/campaign/camplib/objectiv.cpp`, the `ObjectiveClass(VU_BYTE**, long*)`
constructor):

    CampBaseClass        id, entityType, x, y, z, spot state, owner, campId
    CampaignTime         last_repair
    ulong                obj_flags
    uchar                supply, fuel, losses
    uchar size + size    per-feature status bits, two bits per feature
    uchar                priority
    short                nameid          index into DEFAULT.idx/.wch
    VU_ID                parent
    Control              first_owner
    uchar links + links x CampObjectiveLinkDataType(16)
    uchar has_radar + RadarRangeClass(32) when set

The feature-status block is the one awkward part: the writer stored `size`
bytes, and the reader recomputes what it *should* be from the current class
table. When they disagree it reads the smaller of the two and skips the rest,
so the stream always advances by exactly `size` either way.
"""

import struct

from . import entities, lzss

VU_LAST_ENTITY_TYPE = 100
LINK_SIZE = 16            # uchar costs[8] + VU_ID, natural alignment
RADAR_RANGE_SIZE = 32     # float detect_ratio[NUM_RADAR_ARCS], 8 arcs

# Classtable_Types, restricted to the values an objective actually uses.
# Cross-checked against the shipped korea2012 campaign: every objective in
# save0.cam falls in this set.
TYPE_NAMES = {
    1: "Airbase", 2: "Airstrip", 3: "Army base", 4: "Beach", 5: "Border",
    6: "Bridge", 7: "Chemical plant", 8: "City", 9: "Command and control",
    10: "Depot", 11: "Factory", 12: "Ford", 13: "Fortification",
    14: "Hill top", 15: "Intersection", 16: "Nav beacon", 17: "Nuclear plant",
    18: "Pass", 19: "Port", 20: "Power plant", 21: "Radar",
    22: "Radio tower", 23: "Rail terminal", 24: "Railroad", 25: "Refinery",
    26: "Road", 27: "Sea", 28: "Town", 29: "Village", 30: "HARTS",
    31: "SAM site", 58: "Artillery site",
}

# Coarse buckets, so the map can offer sensible layer toggles instead of two
# dozen checkboxes.
CATEGORY_OF_TYPE = {
    1: "airbase", 2: "airbase",
    3: "military", 13: "military", 30: "airdefence", 31: "airdefence",
    21: "airdefence", 58: "military", 10: "military", 9: "military",
    7: "industry", 11: "industry", 17: "industry", 20: "industry",
    25: "industry", 22: "infrastructure",
    6: "infrastructure", 12: "infrastructure", 15: "infrastructure",
    18: "infrastructure", 24: "infrastructure", 26: "infrastructure",
    16: "infrastructure", 23: "infrastructure",
    8: "political", 28: "political", 29: "political",
    19: "port", 4: "terrain", 5: "terrain", 14: "terrain", 27: "terrain",
}

CATEGORY_LABELS = {
    "airbase": "Airbases",
    "airdefence": "Air defence",
    "military": "Military",
    "industry": "Industry",
    "infrastructure": "Infrastructure",
    "political": "Cities and towns",
    "port": "Ports",
    "terrain": "Terrain",
    "other": "Other",
}


class ObjectiveStreamError(Exception):
    pass


def decode_objectives(section, version, class_rows):
    """Decode a `.obj` member into (objectives, raw)."""
    if section is None or len(section) < 10:
        return [], b""
    s = entities.Stream(section, 0)
    count = s.i16()
    size = s.i32()
    s.i32()                       # compressed size, not needed
    if size <= 0 or count <= 0:
        return [], b""
    raw = lzss.expand(section[s.pos:], size)

    out = []
    body = entities.Stream(raw)
    for n in range(count):
        start = body.pos
        try:
            obj = _decode_one(body, version, class_rows)
        except (EOFError, struct.error) as exc:
            raise ObjectiveStreamError(
                "objective %d of %d desynced at byte %d: %s"
                % (n, count, start, exc)) from exc
        obj["_span"] = [start, body.pos]
        obj["_n"] = n
        out.append(obj)

    if body.pos != len(raw):
        raise ObjectiveStreamError(
            "decoded %d objectives but consumed %d of %d bytes"
            % (count, body.pos, len(raw)))
    return out, raw


def _decode_one(s, version, class_rows):
    type_id = s.i16()
    idx = type_id - VU_LAST_ENTITY_TYPE
    if not (0 <= idx < len(class_rows)):
        raise ObjectiveStreamError(
            "class-table index %d out of range (class table has %d rows)"
            % (idx, len(class_rows)))

    r = {"typeId": type_id, "classIndex": idx}
    r["id"] = s.vuid()
    r["entityType"] = s.u16()
    r["x"] = s.i16()
    r["y"] = s.i16()
    r["z"] = 0.0 if version < 70 else s.f32()
    r["spotTime"] = s.u32()
    r["spotted"] = s.i16()
    r["baseFlags"] = s.i16()
    r["owner"] = s.u8()
    r["campId"] = s.i16()

    r["lastRepair"] = s.u32()
    r["objFlags"] = s.u32() if version > 1 else s.i16()
    r["supply"] = s.u8()
    r["fuel"] = s.u8()
    r["losses"] = s.u8()
    fsize = s.u8()
    r["featureStatus"] = s.bytes_(fsize)
    r["priority"] = s.u8()
    r["nameId"] = s.i16()
    r["parent"] = s.vuid()
    r["firstOwner"] = s.u8()
    n = s.u8()
    r["links"] = [{"costs": s.bytes_(8), "id": s.vuid()} for _ in range(n)]
    if version >= 20 and s.u8():
        r["radarRange"] = list(struct.unpack("<8f", s.take(RADAR_RANGE_SIZE)))

    info = class_rows[idx]["classInfo_"]
    r["objType"] = info[2]
    r["typeName"] = TYPE_NAMES.get(info[2], "Type %d" % info[2])
    r["category"] = CATEGORY_OF_TYPE.get(info[2], "other")
    return r


# CampBaseClass is fixed-size and comes first, so these offsets from the start
# of a record are constant for a given version -- the same trick the unit
# stream uses.
PATCHABLE = {"x": "<h", "y": "<h", "owner": "<B", "priority": "<B"}


def _offsets(version):
    base = 2 + 8 + 2                      # type, VU_ID, entityType
    off = {"x": base, "y": base + 2}
    after = base + 4 + (4 if version >= 70 else 0)
    off["owner"] = after + 8              # spotTime, spotted, baseFlags
    off["campId"] = after + 9
    return off


def patch_objective(raw, obj, version, field, value):
    if field not in PATCHABLE:
        raise ValueError("%s is not patchable" % field)
    if field == "priority":
        # priority sits after the variable-length feature-status block, so it
        # is located relative to the end of that block rather than the start.
        at = obj["_span"][0] + _priority_offset(obj, version)
    else:
        at = obj["_span"][0] + _offsets(version)[field]
    out = bytearray(raw)
    struct.pack_into(PATCHABLE[field], out, at, int(value))
    return bytes(out)


def _priority_offset(obj, version):
    at = _offsets(version)["campId"] + 2   # past campId
    at += 4                                # lastRepair
    at += 4 if version > 1 else 2          # objFlags
    at += 3                                # supply, fuel, losses
    at += 1 + len(obj["featureStatus"])    # size byte + the block
    return at


def encode_objectives(raw, count):
    """Re-wrap a (possibly patched) stream as a `.obj` member."""
    packed = lzss.compress(raw)
    return struct.pack("<hii", count, len(raw), len(packed)) + packed
