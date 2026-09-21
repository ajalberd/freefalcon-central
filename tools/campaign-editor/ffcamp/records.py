"""On-disk layouts for the per-theater CampaignDB tables.

Every one of these is a flat array of fixed-size C structs behind a 16-bit
count, in one of two framings (see LoadUnitData and friends in
src/falclib/entity.cpp):

  classic  <count:i16> <record * count>
  FFDBC    <0:i16>     <record * count> <count:i16>

FreeFalcon's "DB control" flag (g_bFFDBC) picks the second when the leading
count reads zero -- that is how the shipped tables get past the old 16-bit
entry ceiling.  Every table in korea2012 uses FFDBC framing except .PD and
.PHD, so both are supported and the framing is preserved on write.

The field lists below are transcribed from the engine headers, with the
compiler's padding written out explicitly as PAD entries.  Each Spec asserts
its own size at import, and selftest.py cross-checks those sizes against the
shipped files -- so if a struct in the tree ever changes, the assert fires
rather than silently shifting every field by a byte.
"""

import struct

# code -> (struct char, size in bytes)
_SCALARS = {
    "i8": ("b", 1), "u8": ("B", 1),
    "i16": ("h", 2), "u16": ("H", 2),
    "i32": ("i", 4), "u32": ("I", 4),
    "f32": ("f", 4),
}


class Field:
    __slots__ = ("name", "kind", "count", "size", "fmt", "doc", "offset",
                 "struct")

    def __init__(self, name, kind, count=1, doc=""):
        self.name = name
        self.kind = kind
        self.count = count
        self.doc = doc
        self.offset = 0
        if kind == "str":
            self.size = count
            self.fmt = "%ds" % count
        elif kind == "pad":
            self.size = count
            self.fmt = "%dx" % count
        else:
            ch, sz = _SCALARS[kind]
            self.size = sz * count
            self.fmt = ("%d%s" % (count, ch)) if count > 1 else ch
        self.struct = struct.Struct("<" + self.fmt) if kind != "pad" else None

    @property
    def is_pad(self):
        return self.kind == "pad"


def same(a, b):
    """Value equality that treats NaN as equal to itself.

    Some sensor tables hold NaN floats. Python's != would call every one of
    them a change on every save, which would then rewrite the field and quiet
    a signalling NaN into a different bit pattern.
    """
    if isinstance(a, (list, tuple)) or isinstance(b, (list, tuple)):
        if len(a) != len(b):
            return False
        return all(same(x, y) for x, y in zip(a, b))
    if isinstance(a, float) and isinstance(b, float):
        if a != a and b != b:
            return True
    return a == b


def F(name, kind, count=1, doc=""):
    return Field(name, kind, count, doc)


def PAD(n):
    return Field("_pad", "pad", n)


class Spec:
    """A fixed-size record layout: pack/unpack to and from plain dicts."""

    def __init__(self, name, ext, fields, size, label=None, key=None,
                 title=None):
        self.name = name
        self.ext = ext
        self.fields = fields
        self.size = size
        self.label = label        # field to show as the row title
        self.key = key            # field holding the class-table index, if any
        self.title = title or name
        self.struct = struct.Struct("<" + "".join(f.fmt for f in fields))
        assert self.struct.size == size, (
            "%s: layout is %d bytes, expected %d"
            % (name, self.struct.size, size))
        self.names = [f.name for f in fields if not f.is_pad]
        at = 0
        for f in fields:
            f.offset = at
            at += f.size
        self.by_name = {f.name: f for f in fields if not f.is_pad}

    def unpack(self, buf, offset=0):
        raw = self.struct.unpack_from(buf, offset)
        out = {}
        i = 0
        for f in self.fields:
            if f.is_pad:
                continue
            if f.kind == "str":
                out[f.name] = raw[i].split(b"\0")[0].decode("latin-1")
            elif f.count > 1:
                out[f.name] = list(raw[i:i + f.count])
                i += f.count - 1
            else:
                out[f.name] = raw[i]
            i += 1
        return out

    def _field_bytes(self, f, v):
        if f.kind == "str":
            raw = str(v).encode("latin-1", "replace")[:f.count]
            return raw + b"\0" * (f.count - len(raw))
        if f.count > 1:
            vals = list(v)[:f.count]
            vals = vals + [0] * (f.count - len(vals))
            if f.kind == "f32":
                return f.struct.pack(*(float(x) for x in vals))
            return f.struct.pack(*(int(x) for x in vals))
        if f.kind == "f32":
            return f.struct.pack(float(v))
        return f.struct.pack(int(v))

    def pack(self, rec, original=None):
        """Serialise `rec`, patching into `original` where one is supplied.

        Patching matters: these files carry bytes no struct field describes --
        compiler padding, and whatever followed the NUL in a fixed-size name
        array. A row nobody touched has to come back out identical, so only
        fields whose value actually changed are rewritten.
        """
        if original is None:
            out = bytearray(self.size)
            for f in self.fields:
                if f.is_pad:
                    continue
                blob = self._field_bytes(f, rec[f.name])
                out[f.offset:f.offset + f.size] = blob
            return bytes(out)

        out = bytearray(original)
        base = self.unpack(original)
        for f in self.fields:
            if f.is_pad:
                continue
            if same(rec[f.name], base[f.name]):
                continue
            blob = self._field_bytes(f, rec[f.name])
            out[f.offset:f.offset + f.size] = blob
        return bytes(out)

    def describe(self):
        """JSON-friendly column metadata for the UI."""
        cols = []
        for f in self.fields:
            if f.is_pad:
                continue
            cols.append({
                "name": f.name,
                "kind": f.kind,
                "count": f.count,
                "doc": f.doc,
                "type": ("text" if f.kind == "str"
                         else "float" if f.kind == "f32"
                         else "int"),
            })
        return cols


MOVEMENT_TYPES = 8      # NoMove..Rail, falclib/include/falcent.h
DAMAGE_TYPES = 11       # NoDamage..OtherDam, campaign/include/campweap.h
GROUPS = 16             # VEHICLE_GROUPS_PER_UNIT
ROLES = 16              # MAXIMUM_ROLES
HARDPOINTS = 16         # HARDPOINT_MAX
WEAPTYPES = 600         # MAXIMUM_WEAPTYPES
LIST_WEAPONS = 64       # MAX_WEAPONS_IN_LIST

MOVE_NAMES = ["NoMove", "Foot", "Wheeled", "Tracked",
              "LowAir", "Air", "Naval", "Rail"]
DAMAGE_NAMES = ["None", "Penetration", "HighExplosive", "Heave", "Incendiary",
                "Proximity", "Kinetic", "Hydrostatic", "Chemical", "Nuclear",
                "Other"]

# --- FALCON4.UCD : units ----------------------------------------------------
UNIT = Spec("unit", "UCD", [
    F("Index", "i16", 1, "Class-table index this row describes"),
    PAD(2),
    F("NumElements", "i32", GROUPS, "Vehicles in each of the 16 groups"),
    F("VehicleType", "i16", GROUPS, "Class-table index of each group's vehicle"),
    F("VehicleClass", "u8", GROUPS * 8, "8-byte class key per group"),
    F("Flags", "u16", 1, "VEH_ capability flags"),
    F("Name", "str", 20, "Unit name"),
    PAD(2),
    F("MovementType", "i32", 1, "MoveType"),
    F("MovementSpeed", "i16"),
    F("MaxRange", "i16", 1, "Movement range at full supply"),
    F("Fuel", "i32", 1, "Internal fuel, lbs"),
    F("Rate", "i16", 1, "Fuel burn, lbs/min at cruise"),
    F("PtDataIndex", "i16"),
    F("Scores", "u8", ROLES, "Score per mission role"),
    F("Role", "u8", 1, "Standard mission role"),
    F("HitChance", "u8", MOVEMENT_TYPES),
    F("Strength", "u8", MOVEMENT_TYPES),
    F("Range", "u8", MOVEMENT_TYPES, "Firing range per target movement type"),
    F("Detection", "u8", MOVEMENT_TYPES),
    F("DamageMod", "u8", DAMAGE_TYPES,
      "% of strength each damage type applies"),
    F("RadarVehicle", "u8"),
    PAD(1),
    F("SpecialIndex", "i16", 1, "Squadron stores index (.SSD)"),
    F("IconIndex", "i16"),
    PAD(2),
], 336, label="Name", key="Index", title="Units")

# --- FALCON4.VCD : vehicles -------------------------------------------------
VEHICLE = Spec("vehicle", "VCD", [
    F("Index", "i16"),
    F("HitPoints", "i16"),
    F("Flags", "u32", 1, "VEH_ flags"),
    F("Name", "str", 15),
    F("NCTR", "str", 5, "Non-cooperative target recognition tag"),
    F("RCSfactor", "f32", 1, "log2(1 + RCS relative to an F-16)"),
    F("MaxWt", "i32", 1, "Max loaded weight, lbs"),
    F("EmptyWt", "i32", 1, "Empty weight, lbs"),
    F("FuelWt", "i32", 1, "Max fuel weight, lbs"),
    F("FuelEcon", "i16", 1, "lbs/min"),
    F("EngineSound", "i16"),
    F("HighAlt", "i16", 1, "hundreds of feet"),
    F("LowAlt", "i16", 1, "hundreds of feet"),
    F("CruiseAlt", "i16", 1, "hundreds of feet"),
    F("MaxSpeed", "i16", 1, "kph"),
    F("RadarType", "i16", 1, "Index into .RCD"),
    F("NumberOfPilots", "i16"),
    F("RackFlags", "u16", 1, "Bit per hardpoint that needs a rack"),
    F("VisibleFlags", "u16", 1, "Bit per hardpoint whose store is drawn"),
    F("CallsignIndex", "u8"),
    F("CallsignSlots", "u8"),
    F("HitChance", "u8", MOVEMENT_TYPES),
    F("Strength", "u8", MOVEMENT_TYPES),
    F("Range", "u8", MOVEMENT_TYPES),
    F("Detection", "u8", MOVEMENT_TYPES),
    F("Weapon", "i16", HARDPOINTS, "Weapon or weapon-list id per hardpoint"),
    F("Weapons", "u8", HARDPOINTS, "Shots per hardpoint when fully supplied"),
    F("DamageMod", "u8", DAMAGE_TYPES),
    PAD(3),
], 160, label="Name", key="Index", title="Vehicles")

# --- FALCON4.WCD : weapons --------------------------------------------------
WEAPON = Spec("weapon", "WCD", [
    F("Index", "i16"),
    F("Strength", "u16", 1, "Damage dealt"),
    F("DamageType", "i32", 1, "DamType"),
    F("Range", "i16", 1, "km"),
    F("Flags", "u16"),
    F("Name", "str", 20),
    F("HitChance", "u8", MOVEMENT_TYPES),
    F("FireRate", "u8", 1, "Shots per barrage"),
    F("Rariety", "u8", 1, "% of a full supply actually provided"),
    F("GuidanceFlags", "u16"),
    F("Collective", "u8"),
    PAD(1),
    F("SimweapIndex", "i16"),
    F("Weight", "u16", 1, "lbs"),
    F("DragIndex", "i16"),
    F("BlastRadius", "u16", 1, "feet"),
    F("RadarType", "i16"),
    F("SimDataIdx", "i16", 1, "Index into .SWD"),
    F("MaxAlt", "i8", 1, "Max engagement altitude, thousands of feet"),
    PAD(1),
], 60, label="Name", key="Index", title="Weapons")

# --- FALCON4.OCD : objectives -----------------------------------------------
OBJECTIVE = Spec("objective", "OCD", [
    F("Index", "i16"),
    F("Name", "str", 20),
    F("DataRate", "i16", 1, "Sortie rate"),
    F("DeagDistance", "i16", 1, "Deaggregation distance"),
    F("PtDataIndex", "i16"),
    F("Detection", "u8", MOVEMENT_TYPES),
    F("DamageMod", "u8", DAMAGE_TYPES),
    PAD(1),
    F("IconIndex", "i16"),
    F("Features", "u8", 1, "Number of features in this objective"),
    F("RadarFeature", "u8"),
    F("FirstFeature", "i16", 1, "Index of first .FED entry"),
], 54, label="Name", key="Index", title="Objective types")

# --- FALCON4.FCD : features -------------------------------------------------
FEATURE = Spec("feature", "FCD", [
    F("Index", "i16"),
    F("RepairTime", "i16"),
    F("Priority", "u8", 1, "Display priority"),
    PAD(1),
    F("Flags", "u16", 1, "FEAT_ flags"),
    F("Name", "str", 20),
    F("HitPoints", "i16"),
    F("Height", "i16", 1, "Vehicle ramp height"),
    F("Angle", "f32", 1, "Vehicle ramp angle"),
    F("RadarType", "i16"),
    F("Detection", "u8", MOVEMENT_TYPES),
    F("DamageMod", "u8", DAMAGE_TYPES),
    PAD(3),
], 60, label="Name", key="Index", title="Features")

# --- FALCON4.FED : per-objective feature entries ----------------------------
FEATURE_ENTRY = Spec("featureentry", "FED", [
    F("Index", "i16", 1, "Class-table index of the feature"),
    F("Flags", "u16"),
    F("eClass", "u8", 8),
    F("Value", "u8", 1, "% operational status lost when destroyed"),
    PAD(3),
    F("Offset", "f32", 3, "Offset from the objective tile centre"),
    F("Facing", "i16"),
    PAD(2),
], 32, title="Feature entries")

# --- FALCON4.SSD : squadron stores ------------------------------------------
SQUAD_STORES = Spec("squadstores", "SSD", [
    F("Stores", "u8", WEAPTYPES, "Stock per weapon type"),
    F("infiniteAG", "u8", 1, "AG weapon this squadron never runs out of"),
    F("infiniteAA", "u8", 1, "AA weapon this squadron never runs out of"),
    F("infiniteGun", "u8", 1, "Gun this squadron never runs out of"),
], 603, title="Squadron stores")

# --- FALCON4.WLD : weapon lists ---------------------------------------------
WEAPON_LIST = Spec("weaponlist", "WLD", [
    F("Name", "str", 16),
    F("WeaponID", "i16", LIST_WEAPONS),
    F("Quantity", "u8", LIST_WEAPONS),
], 208, label="Name", title="Weapon lists")

# --- sensors and sim data ---------------------------------------------------
RADAR = Spec("radar", "RCD", [
    F("RWRsound", "i32"),
    F("RWRsymbol", "i16"),
    F("RDRDataInd", "i16"),
    F("Lethality", "f32", 2),
    F("NominalRange", "f32", 1, "Detection range against an F-16"),
    F("BeamHalfAngle", "f32", 1, "radians"),
    F("ScanHalfAngle", "f32", 1, "radians"),
    F("SweepRate", "f32", 1, "radians/sec"),
    F("CoastTime", "u32", 1, "ms of lock held on a faded target"),
    F("LookDownPenalty", "f32"),
    F("JammingPenalty", "f32"),
    F("NotchPenalty", "f32"),
    F("NotchSpeed", "f32", 1, "ft/sec"),
    F("ChaffChance", "f32"),
    F("flag", "i16", 1, "0x01 = NCTR capable"),
], 58, title="Radars")

RWR = Spec("rwr", "RWD", [
    F("nominalRange", "f32"),
    F("top", "f32"), F("bottom", "f32"), F("left", "f32"), F("right", "f32"),
    F("flag", "i16"),
], 22, title="RWR")

IRST = Spec("irst", "ICD", [
    F("NominalRange", "f32"), F("FOVHalfAngle", "f32"),
    F("GimbalLimitHalfAngle", "f32"), F("GroundFactor", "f32"),
    F("FlareChance", "f32"),
], 20, title="IRST")

VISUAL = Spec("visual", "VSD", [
    F("nominalRange", "f32"),
    F("top", "f32"), F("bottom", "f32"), F("left", "f32"), F("right", "f32"),
], 20, title="Visual sensors")

SIM_WEAPON = Spec("simweapon", "SWD", [
    F("flags", "i32"),
    F("cd", "f32", 1, "Drag coefficient"),
    F("weight", "f32"), F("area", "f32"),
    F("xEjection", "f32"), F("yEjection", "f32"), F("zEjection", "f32"),
    F("mnemonic", "str", 8),
    F("weaponClass", "i32"), F("domain", "i32"),
    F("weaponType", "i32"), F("dataIdx", "i32"),
], 52, label="mnemonic", title="SMS weapons")

SIM_ACDEF = Spec("simacdef", "ACD", [
    F("combatClass", "i32"), F("airframeIdx", "i32"), F("signatureIdx", "i32"),
    F("sensorType", "i32", 5), F("sensorIdx", "i32", 5),
], 52, title="Aircraft definitions")

ROCKET = Spec("rocket", "rkt", [
    F("weaponId", "i16"), F("nweaponId", "i16"), F("weaponCount", "i16"),
], 6, title="Rockets")

DIRTY_PRIORITY = Spec("dirtydata", "ddp", [F("priority", "i32")], 4,
                      title="DirtyData priorities")

PT_HEADER = Spec("ptheader", "PHD", [
    F("objID", "i16"), F("type", "u8"), F("count", "u8"),
    F("features", "u8", 5), PAD(1),
    F("data", "i16", 1, "Runway heading, for example"),
    F("sinHeading", "f32"), F("cosHeading", "f32"),
    F("first", "i16"), F("texIdx", "i16"),
    F("runwayNum", "i8"), F("ltrt", "i8"), F("nextHeader", "i16"),
], 28, title="Point headers")

PT_DATA = Spec("ptdata", "PD", [
    F("xOffset", "f32"), F("yOffset", "f32"),
    F("type", "u8"), F("flags", "u8"), PAD(2),
], 12, title="Points")

# --- FALCON4.ct : the class table -------------------------------------------
# Falcon4EntityClassType is #pragma pack(1), but the VuEntityType it embeds was
# compiled at natural alignment, so it keeps its own 3 bytes of tail padding.
# dataPtr is the 32-bit on-disk slot (see the x64 note in falclib/classtbl.cpp):
# on disk it holds a row index into the per-type table, not a pointer.
CLASS_ENTRY = Spec("class", "ct", [
    F("id_", "u16"),
    F("collisionType_", "u16"),
    F("collisionRadius_", "f32"),
    F("classInfo_", "u8", 8,
      "Domain, Class, Type, SType, SPType, Owner, c6, c7"),
    F("updateRate_", "u32"),
    F("updateTolerance_", "u32"),
    F("bubbleRange_", "f32"),
    F("fineUpdateForceRange_", "f32"),
    F("fineUpdateMultiplier_", "f32"),
    F("damageSeed_", "u32"),
    F("hitpoints_", "i32"),
    F("majorRevisionNumber_", "u16"),
    F("minorRevisionNumber_", "u16"),
    F("createPriority_", "u16"),
    F("managementDomain_", "u8"),
    F("transferable_", "u8"),
    F("private_", "u8"),
    F("tangible_", "u8"),
    F("collidable_", "u8"),
    F("global_", "u8"),
    F("persistent_", "u8"),
    PAD(3),
    F("visType", "i16", 7, "Normal, damaged, destroyed, ... model ids"),
    F("vehicleDataIndex", "i16"),
    F("dataType", "u8", 1,
      "1=Feature 2=None 3=Objective 4=Unit 5=Vehicle 6=Weapon"),
    F("dataPtr", "u32", 1, "Row index into the table named by dataType"),
], 81, title="Class table")

DTYPE_NAMES = {0: "-", 1: "feature", 2: "none", 3: "objective",
               4: "unit", 5: "vehicle", 6: "weapon"}

DTYPE_TABLE = {1: "feature", 3: "objective", 4: "unit",
               5: "vehicle", 6: "weapon"}

DOMAIN_NAMES = {1: "Abstract", 2: "Air", 3: "Land", 4: "Sea",
                5: "Space", 6: "Underground", 7: "Undersea"}

CLASS_NAMES = {0: "Abstract", 1: "Animal", 2: "Feature", 3: "Manager",
               4: "Objective", 5: "SFX", 6: "Unit", 7: "Vehicle",
               8: "Weapon", 9: "Weather", 10: "Session", 11: "Game",
               12: "Group", 13: "Dialog"}

# Tables the editor exposes, in the order the UI shows them.
ALL_SPECS = [
    CLASS_ENTRY, UNIT, VEHICLE, WEAPON, OBJECTIVE, FEATURE, FEATURE_ENTRY,
    WEAPON_LIST, SQUAD_STORES, RADAR, RWR, IRST, VISUAL, SIM_WEAPON,
    SIM_ACDEF, ROCKET, DIRTY_PRIORITY, PT_HEADER, PT_DATA,
]
BY_NAME = {s.name: s for s in ALL_SPECS}
