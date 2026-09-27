"""The weather member (.wth) and the weather-front field.

Two layouts are in the wild. Every file the game has written since Cobra is
the "Cobra" layout, 37 bytes:

    f32 windHeading   radians, the direction the clouds move TO, sim frame
    f32 cumulusBase   ft
    u32 time          campaign ms (unused on load)
    f32 stratusBase   ft
    i8  temperature   deg C
    u8  wind          knots
    u8  condition     1 Sunny, 2 Fair, 3 Poor, 4 Inclement
    u8  contrail      thousands of ft
    u8  overcastDepth hundreds of ft
    f32 stratus2Base  ft
    f32 COVersion     0.0077, marks the layout
    u32 mapW, mapH    always 0

The older Tacedit layout (Instant.cam) carries a weather map after that and
is read for display only; saving writes the Cobra layout.

Since 2026 the game appends a fronts block ("FRNT", see
src/graphics/include/weatherfronts.h) after the map dimensions. A campaign
reads nothing from this member *except* that block, so it is the only way to
give a campaign its starting weather. The field functions below are a port
of weatherfronts.cpp and must stay in step with it: selftest.py checks them
against values the C++ produces.
"""

import math
import random
import struct

CO_VERSION = struct.unpack("<f", struct.pack("<f", 0.0077))[0]
FRONTS_MAGIC = b"FRNT"
FRONTS_OFFSET = 37
FRONTS_VERSION = 1
FRONTS_MAX = 8

CONDITIONS = {1: "Sunny", 2: "Fair", 3: "Poor", 4: "Inclement"}
KINDS = ("cold", "warm", "squall", "cell", "high")

KM = 3279.98  # GRID_SIZE_FT
KTS = 1.68781  # knots to ft/s
DTR = math.pi / 180.0

_FRONT = struct.Struct("<9f2I")  # 44 bytes
_STATE_HEAD = struct.Struct("<3fI2fI")  # 28 bytes
STATE_SIZE = _STATE_HEAD.size + FRONTS_MAX * _FRONT.size  # 380


# -- the member ---------------------------------------------------------------

def _f32(v):
    return struct.unpack("<f", struct.pack("<f", v))[0]


def parse(data):
    """Decode a .wth member to a plain dict the web side can edit."""
    out = {"layout": "unknown", "bytes": len(data or b"")}
    if not data or len(data) < 29:
        return out

    co_a = struct.unpack_from("<f", data, 21)[0]
    co_b = struct.unpack_from("<f", data, 25)[0]
    if abs(co_a - CO_VERSION) < 1e-7 or abs(co_b - CO_VERSION) < 1e-7:
        # Tacedit swaps the last two floats when it saves.
        swapped = abs(co_a - CO_VERSION) < 1e-7
        stratus2 = co_b if swapped else co_a
        wh, cumulus, t, stratus = struct.unpack_from("<ffIf", data, 0)
        temp, wind, cond, contrail, depth = struct.unpack_from("<bBBBB", data, 16)
        out.update(layout="cobra", windHeading=wh, cumulusBase=cumulus,
                   time=t, stratusBase=stratus, temperature=temp,
                   windKnots=wind, condition=cond, contrail=contrail,
                   overcastDepth=depth, stratus2Base=stratus2,
                   swapped=swapped)
    else:
        wh, ws, t, temp = struct.unpack_from("<ffIf", data, 0)
        cumulus, clow, chigh = struct.unpack_from("<BBB", data, 18)
        out.update(layout="tacedit", windHeading=wh, windKnots=round(ws),
                   time=t, temperature=round(temp),
                   cumulusBase=max(cumulus, 100) * 100.0, contrail=clow,
                   condition=None, stratusBase=22000.0, stratus2Base=35000.0,
                   overcastDepth=20)

    out["fronts"] = read_fronts(data)
    # Whatever follows the 29 bytes of weather: two zero map dimensions from
    # the game, or Tacedit's 128x128 weather map, which nothing has read since
    # 2003 but which is kept as it was unless a fronts block replaces it.
    out["_tail"] = bytes(data[29:]) if out["layout"] == "cobra" else None
    return out


def read_fronts(data):
    need = FRONTS_OFFSET + 12 + STATE_SIZE + 12
    if not data or len(data) < need or \
            data[FRONTS_OFFSET:FRONTS_OFFSET + 4] != FRONTS_MAGIC:
        return None
    p = FRONTS_OFFSET + 4
    ver, active = struct.unpack_from("<II", data, p)
    if ver != FRONTS_VERSION:
        return None
    p += 8
    state = decode_state(data[p:p + STATE_SIZE])
    p += STATE_SIZE
    wh, ws, temp = struct.unpack_from("<3f", data, p)
    state.update(active=bool(active), windHeading=wh, windKph=ws,
                 temperature=temp)
    return state


def decode_state(b):
    prev, amp, scale, seed, dx, dy, count = _STATE_HEAD.unpack_from(b, 0)
    fronts = []
    for i in range(min(count, FRONTS_MAX)):
        v = _FRONT.unpack_from(b, _STATE_HEAD.size + i * _FRONT.size)
        fronts.append(dict(zip(("x", "y", "heading", "speed", "halfWidth",
                                "halfLength", "severity", "windBoost",
                                "tempDelta", "born", "life"), v)))
        fronts[-1]["kind"] = kind_of(fronts[-1])
    return {"prevailing": prev, "noiseAmp": amp, "noiseScale": scale,
            "seed": seed, "driftX": dx, "driftY": dy, "fronts": fronts}


def encode_state(st):
    fronts = list(st.get("fronts") or [])[:FRONTS_MAX]
    out = bytearray(_STATE_HEAD.pack(
        float(st["prevailing"]), float(st.get("noiseAmp", 0.0)),
        float(st.get("noiseScale", 400000.0)),
        int(st.get("seed", 0)) & 0xFFFFFFFF,
        float(st.get("driftX", 0.0)), float(st.get("driftY", 0.0)),
        len(fronts)))
    for f in fronts:
        out += _FRONT.pack(float(f["x"]), float(f["y"]), float(f["heading"]),
                           float(f["speed"]), float(f["halfWidth"]),
                           float(f["halfLength"]), float(f["severity"]),
                           float(f.get("windBoost", 0.0)),
                           float(f.get("tempDelta", 0.0)),
                           int(f["born"]) & 0xFFFFFFFF,
                           int(f["life"]) & 0xFFFFFFFF)
    out += b"\0" * (FRONTS_MAX - len(fronts)) * _FRONT.size
    assert len(out) == STATE_SIZE
    return bytes(out)


def encode(w):
    """The Cobra layout plus the fronts block, from a dict like parse's.

    Without fronts, the bytes after the weather are kept as they were read.
    With them, the block goes where the game looks -- straight after two zero
    map dimensions -- so a Tacedit map in the way is dropped.
    """
    wh = float(w["windHeading"])
    cond = int(w["condition"] or 1)
    # Tacedit swaps the last two floats when it saves; keep a file's order.
    s2 = float(w["stratus2Base"])
    last = (CO_VERSION, s2) if w.get("swapped") else (s2, CO_VERSION)
    head = struct.pack(
        "<ffIfbBBBB", wh, float(w["cumulusBase"]), int(w.get("time", 0)),
        float(w["stratusBase"]), max(-128, min(127, int(round(w["temperature"])))),
        max(0, min(255, int(round(w["windKnots"])))), cond,
        max(0, min(255, int(w["contrail"]))),
        max(0, min(255, int(w["overcastDepth"]))))
    assert len(head) == 21

    fr = w.get("fronts")
    tail = w.get("_tail")
    if not fr:
        if tail and len(tail) >= 8 and read_fronts(bytes(29) + tail) is None:
            # Keep the original dimensions and map, but not a stale fronts
            # block: removing the fronts is an edit too.
            return head + struct.pack("<ff", *last) + tail
        return head + struct.pack("<ff", *last) + struct.pack("<II", 0, 0)
    head += struct.pack("<ff", *last) + struct.pack("<II", 0, 0)
    assert len(head) == FRONTS_OFFSET
    # windSpeed inside the game is km/h; the Cobra byte is knots.
    tail = FRONTS_MAGIC + struct.pack("<II", FRONTS_VERSION,
                                      1 if fr.get("active", True) else 0)
    tail += encode_state(fr)
    tail += struct.pack("<3f", wh, float(w["windKnots"]) * 1.852,
                        float(w["temperature"]))
    return head + tail


# -- the field (port of weatherfronts.cpp) -----------------------------------

def kind_of(f):
    if f["severity"] < 0:
        return "high"
    if f["halfLength"] <= 0:
        return "cell"
    if f["halfWidth"] < 20 * KM:
        return "squall"
    return "warm" if f["tempDelta"] > 0 else "cold"


def life_strength(f, now):
    if f["life"] == 0 or now < f["born"]:
        return 0.0
    t = (now - f["born"]) / f["life"]
    if t >= 1:
        return 0.0
    if t < 0.15:
        return t / 0.15
    if t > 0.75:
        return (1 - t) / 0.25
    return 1.0


def core(f, now):
    secs = (now - f["born"]) * 0.001 if now > f["born"] else 0.0
    return (f["x"] + math.cos(f["heading"]) * f["speed"] * secs,
            f["y"] + math.sin(f["heading"]) * f["speed"] * secs)


def profile(f, x, y, now):
    life = life_strength(f, now)
    if life <= 0:
        return 0.0
    cx, cy = core(f, now)
    dx, dy = x - cx, y - cy
    ch, sh = math.cos(f["heading"]), math.sin(f["heading"])
    u = dx * ch + dy * sh
    v = -dx * sh + dy * ch
    w = max(f["halfWidth"], 1000.0)
    if f["halfLength"] <= 0:
        r2 = (u * u + v * v) / (w * w)
        return 0.0 if r2 > 9 else life * math.exp(-r2)
    ahead, behind = (0.6, 1.5) if f["tempDelta"] <= 0 else (1.6, 0.7)
    a = u / (w * (ahead if u >= 0 else behind))
    if a * a > 9:
        return 0.0
    across = math.exp(-a * a)
    along = 1.0
    e = abs(v) - f["halfLength"]
    if e > 0:
        b = e / w
        if b > 3:
            return 0.0
        along = math.exp(-b * b)
    return life * across * along


def _lattice(ix, iy, seed):
    h = (ix * 374761393 + iy * 668265263 + seed * 2246822519) & 0xFFFFFFFF
    h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF
    h ^= h >> 16
    return (h & 0xFFFF) / 32767.5 - 1.0


def _value_noise(x, y, seed):
    fx, fy = math.floor(x), math.floor(y)
    ix, iy = int(fx), int(fy)
    tx, ty = x - fx, y - fy
    tx = tx * tx * (3 - 2 * tx)
    ty = ty * ty * (3 - 2 * ty)
    a, b = _lattice(ix, iy, seed), _lattice(ix + 1, iy, seed)
    c, d = _lattice(ix, iy + 1, seed), _lattice(ix + 1, iy + 1, seed)
    ab = a + (b - a) * tx
    cd = c + (d - c) * tx
    return ab + (cd - ab) * ty


def noise(st, x, y, now):
    amp, scale = st.get("noiseAmp", 0.0), st.get("noiseScale", 0.0)
    if amp <= 0 or scale <= 0:
        return 0.0
    secs = now * 0.001
    px = (x - st.get("driftX", 0.0) * secs) / scale
    py = (y - st.get("driftY", 0.0) * secs) / scale
    cycle = secs / (6 * 3600.0)
    epoch = int(math.floor(cycle))
    m = cycle - math.floor(cycle)
    m = m * m * (3 - 2 * m)
    seed = int(st.get("seed", 0))
    s = lambda k: (seed + epoch + k) & 0xFFFFFFFF
    n0 = _value_noise(px, py, s(0)) * 0.7 + \
        _value_noise(px * 2.3, py * 2.3, s(101)) * 0.3
    n1 = _value_noise(px, py, s(1)) * 0.7 + \
        _value_noise(px * 2.3, py * 2.3, s(102)) * 0.3
    return (n0 + (n1 - n0) * m) * amp * 1.75  # see weatherfronts.cpp


def severity(st, x, y, now, active=True):
    sev = st["prevailing"]
    if active:
        sev += noise(st, x, y, now)
        up = down = 0.0
        for f in st.get("fronts", ()):
            p = profile(f, x, y, now)
            if p <= 0:
                continue
            add = f["severity"] * p
            up, down = max(up, add), min(down, add)
        sev += up + down
    return min(max(sev, 1.0), 4.49)


def condition(sev):
    return min(4, max(1, int(math.floor(sev + 0.5))))


def grid(st, now, size_x_km, size_y_km, step_km=16, active=True):
    """Severity sampled over the theater, rows north to south, columns west
    to east, for the map overlay. Grid x is east, y north; sim the reverse."""
    rows = []
    ny, nx = int(size_y_km // step_km), int(size_x_km // step_km)
    for j in range(ny - 1, -1, -1):
        north = (j + 0.5) * step_km * KM
        row = []
        for i in range(nx):
            east = (i + 0.5) * step_km * KM
            row.append(round(severity(st, north, east, now, active), 2))
        rows.append(row)
    return rows


# -- making fronts (mirror of WeatherClass::SpawnFront) ----------------------

def spawn(kind, wind_heading, size_x_km, size_y_km, now, rng=random,
          in_progress=True):
    """A random front of this kind, positioned as the game would place it."""
    sx, sy = size_y_km * KM, size_x_km * KM  # sim x is north
    cx, cy = sx / 2, sy / 2
    radius = 0.5 * math.hypot(sx, sy)
    R = rng.uniform
    f = {"heading": wind_heading + R(-35, 35) * DTR}
    if kind == "cold":
        f.update(halfWidth=R(25, 45) * KM, halfLength=R(150, 350) * KM,
                 severity=R(1.6, 2.6), windBoost=R(10, 20),
                 tempDelta=-R(3, 7), speed=R(20, 30) * KTS)
    elif kind == "warm":
        f.update(halfWidth=R(50, 80) * KM, halfLength=R(200, 400) * KM,
                 severity=R(1.2, 1.8), windBoost=R(5, 10), tempDelta=R(2, 4),
                 speed=R(10, 18) * KTS)
    elif kind == "squall":
        f.update(halfWidth=R(8, 15) * KM, halfLength=R(40, 100) * KM,
                 severity=R(2.2, 3.0), windBoost=R(20, 35), tempDelta=-2.0,
                 speed=R(25, 40) * KTS)
    elif kind == "cell":
        f.update(halfWidth=R(8, 20) * KM, halfLength=0.0,
                 severity=R(2.0, 3.0), windBoost=R(15, 25), tempDelta=-2.0,
                 speed=R(15, 30) * KTS)
    else:
        f.update(halfWidth=R(60, 120) * KM, halfLength=0.0,
                 severity=-R(1.5, 2.5), windBoost=0.0, tempDelta=1.0,
                 speed=R(8, 15) * KTS)

    ch, sh = math.cos(f["heading"]), math.sin(f["heading"])
    local = kind in ("cell", "squall")
    if local:
        px, py = R(0.1, 0.9) * sx, R(0.1, 0.9) * sy
        life = R(3, 8) * 3600.0
    else:
        travel = 2 * radius + 4 * f["halfWidth"]
        lateral = R(-0.6, 0.6) * radius
        px = cx - ch * (radius + 2 * f["halfWidth"]) - sh * lateral
        py = cy - sh * (radius + 2 * f["halfWidth"]) + ch * lateral
        life = travel / f["speed"]
    f["life"] = int(life * 1000)
    age = 0
    if in_progress:
        age = int(R(0.2, 0.6) * f["life"])
        if not local:
            px += ch * f["speed"] * age * 0.001
            py += sh * f["speed"] * age * 0.001
        if age > now:  # the clock is younger than the front; see SpawnFront
            f["life"] -= age - now
            age = now
    f["born"] = now - age
    f["x"] = px - ch * f["speed"] * age * 0.001
    f["y"] = py - sh * f["speed"] * age * 0.001
    f["kind"] = kind_of(f)
    return f


def place(f, now, north_km, east_km):
    """Move a front so its core is at this grid point now."""
    cx, cy = core(f, now)
    f["x"] += north_km * KM - cx
    f["y"] += east_km * KM - cy
    return f
