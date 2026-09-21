"""Decode the unit list inside a .cam / .tac.

The `.uni` member is a VU entity stream, not a table: a count, then that many
records written by each entity's own Save() chain. There are no per-record
lengths, so the only way to find record N+1 is to decode record N exactly --
every optional field, every version gate. Get one byte wrong and everything
after it is garbage.

The reference is the constructor chain in the tree:

    CampBaseClass(stream)           campaign/camplib/campbase.cpp
      UnitClass(stream)             campaign/camplib/unit.cpp  (+ waypoints)
        GroundUnitClass             campaign/camptask/gndunit.cpp
          BattalionClass            campaign/camptask/battalio.cpp
          BrigadeClass              campaign/camptask/brigade.cpp
        AirUnitClass                campaign/camptask/airunit.cpp (adds nothing)
          SquadronClass             campaign/camptask/squadron.cpp
          FlightClass               campaign/camptask/flight.cpp
          PackageClass              campaign/camptask/package.cpp
        TaskForceClass              campaign/camptask/navunit.cpp

Which subclass to use comes from the class table, exactly as NewUnit() does:
classInfo_[VU_DOMAIN] picks air/land/sea and classInfo_[VU_TYPE] picks the
kind within it.

Records keep the byte span they were decoded from, so a field can be patched
in place and everything around it comes back out untouched -- the same
approach the flat tables use, and the only safe one for a format with no
length prefixes.
"""

import struct

from . import lzss

VU_LAST_ENTITY_TYPE = 100
MAX_UNIT_CHILDREN = 5
PILOTS_PER_SQUADRON = 48
PILOTS_PER_FLIGHT = 4
HARDPOINT_MAX = 16
MAXIMUM_WEAPTYPES = 600
VEHICLE_GROUPS_PER_UNIT = 16
ARO_OTHER = 16
PILOT_RECORD = 10            # PilotClass: short, 6 x uchar, short -> 10
MISSION_REQUEST = 76         # sizeof(MissionRequestClass), natural alignment
LOADOUT_STRUCT = 48          # short[16] + uchar[16]

U_FINAL = 0x100000

# classInfo_ slots
VU_DOMAIN, VU_CLASS, VU_TYPE = 0, 1, 2

DOMAIN_AIR, DOMAIN_LAND, DOMAIN_SEA = 2, 3, 4
TYPE_FLIGHT, TYPE_PACKAGE, TYPE_SQUADRON = 1, 2, 3
TYPE_BATTALION, TYPE_BRIGADE = 1, 2
TYPE_TASKFORCE = 1

# campaign/camplib/campwp.cpp, lines 9-10. Note the order: DEPTIME is bit 0.
WP_HAVE_DEPTIME = 0x01
WP_HAVE_TARGET = 0x02


class Stream:
    """Sequential reader with the same field vocabulary as memcpychk."""

    def __init__(self, buf, pos=0):
        self.buf = buf
        self.pos = pos

    def take(self, n):
        end = self.pos + n
        if end > len(self.buf):
            raise EOFError("stream wants %d bytes at %d, only %d left"
                           % (n, self.pos, len(self.buf) - self.pos))
        chunk = self.buf[self.pos:end]
        self.pos = end
        return chunk

    def u8(self):
        return self.take(1)[0]

    def i8(self):
        return struct.unpack("<b", self.take(1))[0]

    def i16(self):
        return struct.unpack("<h", self.take(2))[0]

    def u16(self):
        return struct.unpack("<H", self.take(2))[0]

    def i32(self):
        return struct.unpack("<i", self.take(4))[0]

    def u32(self):
        return struct.unpack("<I", self.take(4))[0]

    def f32(self):
        return struct.unpack("<f", self.take(4))[0]

    def vuid(self):
        num, creator = struct.unpack("<II", self.take(8))
        return [num, creator]

    def bytes_(self, n):
        return list(self.take(n))


def _waypoint(s, version):
    haves = s.u8()
    wp = {
        "x": s.i16(), "y": s.i16(), "z": s.i16(),
        "arrive": s.u32(),
        "action": s.u8(), "routeAction": s.u8(), "formation": s.u8(),
    }
    wp["flags"] = s.i16() if version < 72 else s.u32()
    if haves & WP_HAVE_TARGET:
        wp["targetId"] = s.vuid()
        wp["targetBuilding"] = s.u8()
    if haves & WP_HAVE_DEPTIME:
        wp["depart"] = s.u32()
    return wp


def _waypoint_list(s, version, count):
    return [_waypoint(s, version) for _ in range(count)]


def _camp_base(s, version):
    r = {}
    r["id"] = s.vuid()
    r["entityType"] = s.u16()
    r["x"] = s.i16()
    r["y"] = s.i16()
    r["z"] = 0.0 if version < 70 else s.f32()
    r["spotTime"] = s.u32()
    r["spotted"] = s.i16()
    r["baseFlags"] = s.i16()
    r["owner"] = s.u8()          # Control, a uchar -- the owning team
    r["campId"] = s.i16()
    return r


def _unit(s, version, r):
    r["lastCheck"] = s.u32()
    r["roster"] = s.u32()
    r["unitFlags"] = s.u32()
    r["destX"] = s.i16()
    r["destY"] = s.i16()
    r["targetId"] = s.vuid()
    if version > 1:
        r["cargoId"] = s.vuid()
    r["moved"] = s.u8()
    r["losses"] = s.u8()
    r["tactic"] = s.u8()
    r["currentWp"] = s.u16() if version >= 71 else s.u8()
    r["nameId"] = s.i16()
    r["reinforcement"] = s.i16()
    count = s.u16() if version >= 71 else s.u8()
    r["waypoints"] = _waypoint_list(s, version, count)
    return r


def _ground_unit(s, version, r):
    r["orders"] = s.u8()
    r["division"] = s.i16()
    r["aobj"] = s.vuid()
    return r


def _battalion(s, version, r):
    r["lastMove"] = s.u32()
    r["lastCombat"] = s.u32()
    r["parentId"] = s.vuid()
    r["lastObj"] = s.vuid()
    # USE_FLANKS is not defined in this tree, so the four flank grid indices
    # are absent from the stream.
    r["supply"] = s.u8()
    r["fatigue"] = s.u8()
    r["morale"] = s.u8()
    r["heading"] = s.u8()
    r["finalHeading"] = s.u8()
    if version < 15:
        s.u8()
    r["position"] = s.u8()
    return r


def _brigade(s, version, r):
    n = s.u8()
    r["elements"] = n
    r["element"] = [s.vuid() for _ in range(n)]
    return r


def _taskforce(s, version, r):
    r["orders"] = s.u8()
    r["supply"] = s.u8()
    return r


def _squadron(s, version, r):
    r["fuel"] = s.i32()
    r["specialty"] = s.u8()
    if version < 69:
        r["stores"] = s.bytes_(200)
    elif version < 72:
        r["stores"] = s.bytes_(220)
    else:
        r["stores"] = s.bytes_(MAXIMUM_WEAPTYPES)

    if version < 47:
        s.take(8 * (PILOTS_PER_SQUADRON if version >= 29 else 36))
        r["pilots"] = []
    else:
        r["pilots"] = [list(struct.unpack("<hBBBBBBh", s.take(PILOT_RECORD)))
                       for _ in range(PILOTS_PER_SQUADRON)]

    r["schedule"] = [s.u32() for _ in range(VEHICLE_GROUPS_PER_UNIT)]
    r["airbaseId"] = s.vuid()
    r["hotSpot"] = s.vuid()
    if 6 <= version < 16:
        s.vuid()
    r["rating"] = s.bytes_(ARO_OTHER)
    r["aaKills"] = s.i16()
    r["agKills"] = s.i16()
    r["asKills"] = s.i16()
    r["anKills"] = s.i16()
    r["missionsFlown"] = s.i16()
    r["missionScore"] = s.i16()
    r["totalLosses"] = s.u8()
    if version >= 9:
        r["pilotLosses"] = s.u8()
    if version >= 45:
        r["squadronPatch"] = s.u8()
    return r


def _flight(s, version, r):
    r["z"] = s.f32()
    r["fuelBurnt"] = s.i32()
    r["lastMove"] = s.u32()
    r["lastCombat"] = s.u32()
    r["timeOnTarget"] = s.u32()
    r["missionOverTime"] = s.u32()
    r["missionTarget"] = s.i16()

    n = s.u8()
    r["loadouts"] = n
    loads = []
    for _ in range(n):
        if version <= 72:
            # Old flights stored both halves as bytes, not shorts.
            ids = s.bytes_(HARDPOINT_MAX)
            counts = s.bytes_(HARDPOINT_MAX)
        else:
            raw = s.take(LOADOUT_STRUCT)
            ids = list(struct.unpack("<%dh" % HARDPOINT_MAX,
                                     raw[:2 * HARDPOINT_MAX]))
            counts = list(raw[2 * HARDPOINT_MAX:])
        loads.append({"WeaponID": ids, "WeaponCount": counts})
    r["loadout"] = loads

    r["mission"] = s.u8()
    if version > 65:
        r["oldMission"] = s.u8()
    r["lastDirection"] = s.u8()
    r["priority"] = s.u8()
    r["missionId"] = s.u8()
    if version < 14:
        s.u8()
    r["evalFlags"] = s.u8()
    if version > 65:
        r["missionContext"] = s.u8()
    r["package"] = s.vuid()
    r["squadron"] = s.vuid()
    if version > 65:
        r["requester"] = s.vuid()
    r["slots"] = s.bytes_(PILOTS_PER_FLIGHT)
    r["pilotSlots"] = s.bytes_(PILOTS_PER_FLIGHT)
    r["planeStats"] = s.bytes_(PILOTS_PER_FLIGHT)
    r["playerSlots"] = s.bytes_(PILOTS_PER_FLIGHT)
    r["lastPlayerSlot"] = s.u8()
    r["callsignId"] = s.u8()
    r["callsignNum"] = s.u8()
    if version >= 72:
        r["refuel"] = s.u32()
    return r


def _package(s, version, r):
    n = s.u8()
    r["elements"] = n
    r["element"] = [s.vuid() for _ in range(n)]
    r["interceptor"] = s.vuid()
    if version >= 7:
        r["awacs"] = s.vuid()
        r["jstar"] = s.vuid()
        r["ecm"] = s.vuid()
        r["tanker"] = s.vuid()
    r["waitCycles"] = s.u8()

    # A planned package writes a short form; an unplanned one writes the lot.
    if (r["unitFlags"] & U_FINAL) and not r["waitCycles"]:
        r["requests"] = s.i16()
        if version < 35:
            s.i16()
        r["responses"] = s.i16()
        r["misMission"] = s.i16()
        r["misContext"] = s.i16()
        r["misRequester"] = s.vuid()
        r["misTarget"] = s.vuid()
        if version >= 16:
            r["misTot"] = s.u32()
        if version >= 35:
            r["misActionType"] = s.u8()
        if version >= 41:
            r["misPriority"] = s.i16()
    else:
        r["flights"] = s.u8()
        r["waitFor"] = s.i16()
        r["iax"] = s.i16()
        r["iay"] = s.i16()
        r["eax"] = s.i16()
        r["eay"] = s.i16()
        r["bpx"] = s.i16()
        r["bpy"] = s.i16()
        r["tpx"] = s.i16()
        r["tpy"] = s.i16()
        r["takeoff"] = s.u32()
        r["tpTime"] = s.u32()
        r["packageFlags"] = s.u32()
        r["caps"] = s.i16()
        r["requests"] = s.i16()
        if version < 35:
            s.i16()
        r["responses"] = s.i16()
        r["ingress"] = _waypoint_list(s, version, s.u8())
        r["egress"] = _waypoint_list(s, version, s.u8())
        s.take(64 if version < 35 else MISSION_REQUEST)
    return r


# domain -> type -> (kind, [decoder chain after UnitClass])
DISPATCH = {
    DOMAIN_AIR: {
        TYPE_FLIGHT: ("flight", [_flight]),
        TYPE_SQUADRON: ("squadron", [_squadron]),
        TYPE_PACKAGE: ("package", [_package]),
    },
    DOMAIN_LAND: {
        TYPE_BRIGADE: ("brigade", [_ground_unit, _brigade]),
        TYPE_BATTALION: ("battalion", [_ground_unit, _battalion]),
    },
    DOMAIN_SEA: {
        TYPE_TASKFORCE: ("taskforce", [_taskforce]),
    },
}


class UnitStreamError(Exception):
    pass


def decode_units(section, version, class_rows):
    """Decode a `.uni` member.

    Returns (units, raw) where `raw` is the decompressed stream each unit's
    `_span` indexes into.
    """
    if len(section) < 4:
        return [], b""
    # Outer int32 length, then the DecodeUnitData header.
    s = Stream(section, 4)
    count = s.i16()
    size = s.i32()
    if size == 0:
        return [], b""
    raw = lzss.expand(section[s.pos:], size)

    units = []
    body = Stream(raw)
    for n in range(count):
        start = body.pos
        try:
            unit = _decode_one(body, version, class_rows)
        except (EOFError, struct.error) as exc:
            raise UnitStreamError(
                "unit %d of %d desynced at byte %d: %s"
                % (n, count, start, exc)) from exc
        unit["_span"] = [start, body.pos]
        unit["_n"] = n
        units.append(unit)

    if body.pos != len(raw):
        raise UnitStreamError(
            "decoded %d units but consumed %d of %d bytes"
            % (count, body.pos, len(raw)))
    return units, raw


def walk_units(raw, version, class_rows):
    """Decode a decompressed unit stream without being told how many there are.

    Used after a structural edit: adding or removing a record changes the
    count, and the count is what the framing carries, so the only honest way
    to recount is to walk the records again.
    """
    out = []
    body = Stream(raw)
    while body.pos < len(raw):
        start = body.pos
        try:
            unit = _decode_one(body, version, class_rows)
        except (EOFError, struct.error) as exc:
            raise UnitStreamError(
                "unit %d desynced at byte %d: %s" % (len(out), start, exc)
            ) from exc
        unit["_span"] = [start, body.pos]
        unit["_n"] = len(out)
        out.append(unit)
    return out


def _decode_one(s, version, class_rows):
    type_id = s.i16()
    idx = type_id - VU_LAST_ENTITY_TYPE
    if not (0 <= idx < len(class_rows)):
        raise UnitStreamError(
            "class-table index %d out of range (class table has %d rows)"
            % (idx, len(class_rows)))
    info = class_rows[idx]["classInfo_"]

    table = DISPATCH.get(info[VU_DOMAIN])
    entry = table.get(info[VU_TYPE]) if table else None
    if entry is None:
        raise UnitStreamError(
            "no unit class for domain %d type %d (class-table row %d)"
            % (info[VU_DOMAIN], info[VU_TYPE], idx))
    kind, chain = entry

    r = _camp_base(s, version)
    r["typeId"] = type_id
    r["classIndex"] = idx
    r["kind"] = kind
    _unit(s, version, r)
    for step in chain:
        step(s, version, r)
    return r


# --- in-place patching -------------------------------------------------------

# Fields the map view can move or retag, with their offset from the start of
# the record and their format. All of them live in CampBaseClass, which is
# fixed-size and comes first, so the offsets are constant per version.
def base_offsets(version):
    #  short type | VU_ID (8) | ushort entityType | x | y | [float z]
    off = {"x": 2 + 8 + 2, "y": 2 + 8 + 2 + 2}
    after_xy = 2 + 8 + 2 + 2 + 2
    if version >= 70:
        after_xy += 4
    off["spotTime"] = after_xy
    off["spotted"] = after_xy + 4
    off["baseFlags"] = after_xy + 6
    off["owner"] = after_xy + 8
    off["campId"] = after_xy + 9
    return off


PATCHABLE = {
    "x": "<h", "y": "<h", "owner": "<B", "campId": "<h", "baseFlags": "<h",
}


def patch_unit(raw, unit, version, field, value):
    """Write one CampBaseClass field back into the decompressed stream."""
    if field not in PATCHABLE:
        raise ValueError("%s is not patchable" % field)
    off = base_offsets(version)[field]
    at = unit["_span"][0] + off
    out = bytearray(raw)
    struct.pack_into(PATCHABLE[field], out, at, int(value))
    return bytes(out)


def encode_units(raw, count):
    """Re-wrap a (possibly patched) stream as a `.uni` member."""
    packed = lzss.compress(raw)
    inner = struct.pack("<hi", count, len(raw)) + packed
    return struct.pack("<i", len(inner)) + inner


# --- creating and removing units ---------------------------------------------

# VU_ID number ranges, from campaign/camplib/campbase.cpp. Objectives and the
# non-volatile units (battalions, brigades, squadrons, task forces) live in
# fixed bands; packages and flights are allocated above them at runtime.
VU_FIRST_ENTITY_ID = 4
MAX_NUMBER_OF_OBJECTIVES = 8000
MAX_NUMBER_OF_UNITS = 4000
FIRST_OBJECTIVE_ID = VU_FIRST_ENTITY_ID
LAST_OBJECTIVE_ID = VU_FIRST_ENTITY_ID + MAX_NUMBER_OF_OBJECTIVES
FIRST_NON_VOLATILE_ID = LAST_OBJECTIVE_ID + 1
LAST_NON_VOLATILE_ID = FIRST_NON_VOLATILE_ID + MAX_NUMBER_OF_UNITS
MAX_CAMP_ENTITIES = MAX_NUMBER_OF_OBJECTIVES + MAX_NUMBER_OF_UNITS + 16000

# U_ flags worth naming here (campaign/include/unit.h).
U_ASSIGNED = 0x04
U_ORDERED = 0x08
U_PARENT = 0x20

# GORD_*, campaign/include/gndunit.h
GROUND_ORDERS = [
    "Reserve", "Capture", "Secure", "Assault", "Airborne", "Commando",
    "Defend", "Support", "Repair", "Air defence", "Recon", "Radar",
]

# Only these can be placed by hand. Flights and packages are transient objects
# the campaign AI creates and destroys as it plans; authoring one makes no
# sense and a half-built one would confuse the planner.
PLACEABLE = ("battalion", "brigade", "taskforce")


def roster_from_class(unit_row):
    """The packed roster BuildElements() would compute for a unit type.

    Two bits per vehicle group, 16 groups. The engine builds this with XOR, so
    a group count above 3 would bleed into its neighbour; mask instead.
    """
    if not unit_row:
        return 0
    r = 0
    for i, n in enumerate(unit_row["NumElements"][:VEHICLE_GROUPS_PER_UNIT]):
        r |= (int(n) & 0x03) << (i * 2)
    return r & 0xFFFFFFFF


def next_ids(units, objectives=()):
    """A free VU_ID number and camp id for a new non-volatile unit."""
    used_vu = {u["id"][0] for u in units}
    used_camp = {u["campId"] for u in units}
    used_camp |= {o["campId"] for o in objectives}

    vu = FIRST_NON_VOLATILE_ID
    while vu in used_vu and vu <= LAST_NON_VOLATILE_ID:
        vu += 1
    if vu > LAST_NON_VOLATILE_ID:
        raise ValueError("no free unit id left (the %d-unit ceiling is full)"
                         % MAX_NUMBER_OF_UNITS)

    camp = 1
    while camp in used_camp and camp < MAX_CAMP_ENTITIES:
        camp += 1
    if camp >= MAX_CAMP_ENTITIES:
        raise ValueError("no free campaign id left")
    return vu, camp


def _w(out, fmt, *vals):
    out.extend(struct.pack(fmt, *vals))


def build_unit(kind, version, type_id, x, y, owner, vu_id, camp_id,
               roster=0, orders=6, supply=100, morale=100,
               unit_flags=U_PARENT, name_id=0, division=0):
    """Serialise a fresh unit record, ready to append to the stream.

    Mirrors the Save() chain rather than cloning an existing record, so every
    byte is accounted for: no inherited parent, objective assignment or
    waypoint list to go stale. Waypoint count is zero — a placed unit sits
    where it is put until the campaign gives it orders.
    """
    if kind not in PLACEABLE:
        raise ValueError("%s units cannot be created by hand" % kind)
    if version < 70 or version < 71:
        raise ValueError("creating units needs campaign data version 71+, "
                         "this file is %d" % version)

    out = bytearray()
    _w(out, "<h", type_id)                       # the leading dispatch type

    # CampBaseClass
    _w(out, "<II", vu_id, 0)                     # VU_ID: number, creator
    _w(out, "<H", type_id)                       # entityType
    _w(out, "<hh", int(x), int(y))
    _w(out, "<f", 0.0)                           # z
    _w(out, "<I", 0)                             # spotTime
    _w(out, "<hh", 0, 0)                         # spotted, baseFlags
    _w(out, "<B", int(owner) & 0xFF)
    _w(out, "<h", int(camp_id))

    # UnitClass
    _w(out, "<I", 0)                             # lastCheck
    _w(out, "<I", roster & 0xFFFFFFFF)
    _w(out, "<I", unit_flags & 0xFFFFFFFF)
    _w(out, "<hh", int(x), int(y))               # destX, destY
    _w(out, "<II", 0, 0)                         # targetId
    _w(out, "<II", 0, 0)                         # cargoId
    _w(out, "<BBB", 0, 0, 0)                     # moved, losses, tactic
    _w(out, "<H", 0)                             # currentWp
    _w(out, "<hh", int(name_id), 0)              # nameId, reinforcement
    _w(out, "<H", 0)                             # waypoint count

    if kind in ("battalion", "brigade"):
        # GroundUnitClass
        _w(out, "<B", int(orders) & 0xFF)
        _w(out, "<h", int(division))
        _w(out, "<II", 0, 0)                     # aobj
        if kind == "battalion":
            _w(out, "<I", 0)                     # lastMove
            _w(out, "<I", 0)                     # lastCombat
            _w(out, "<II", 0, 0)                 # parentId
            _w(out, "<II", 0, 0)                 # lastObj
            _w(out, "<BBB", int(supply) & 0xFF, 0, int(morale) & 0xFF)
            _w(out, "<BB", 0, 0)                 # heading, finalHeading
            _w(out, "<B", 0)                     # position
        else:
            _w(out, "<B", 0)                     # elements, and no children
    else:                                        # taskforce
        _w(out, "<B", int(orders) & 0xFF)
        _w(out, "<B", int(supply) & 0xFF)

    return bytes(out)


def append_unit(raw, record):
    """Add a record to the end of a decompressed unit stream."""
    return raw + record


def delete_unit(raw, unit):
    """Cut one record out of a decompressed unit stream."""
    start, end = unit["_span"]
    return raw[:start] + raw[end:]


def replace_unit(raw, unit, record):
    start, end = unit["_span"]
    return raw[:start] + record + raw[end:]
