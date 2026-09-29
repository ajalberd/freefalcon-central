"""Railway lines from OpenStreetMap, placed on a theater's campaign grid.

No shipped theater has any rail: every .thr has the rail bit clear, there are
no Railroad objectives, and no terrain tile carries a COVERAGE_RAIL path. So
the lines come from OSM, and the hard part is not the download but where a
latitude/longitude lands on Falcon's map.

Falcon's own conversion (ApproxLatLong in sim/navaids/navsystem.cpp, origin
from Theater.map) is 45-195 km out in Korea: the theater was drawn stretched
about 1.25x east-west, and Japan is squeezed in. So the projection here is
*fitted*, in two steps:

  1. an affine fit from real airbase positions to the airbase objectives
     (about 5 km RMS in Korea -- airbases were placed by hand), then
  2. a low-order polynomial refined so the OSM coastline lies on the
     terrain's own shoreline (the land/water edge of the ground tiles),
     with the airbases kept as a weak pull so the fit cannot slide along a
     straight coast.

Grid coordinates are campaign kilometres: x east, y north, the same numbers
units and objectives use. OSM data is ODbL; the output carries the notice.
"""

import json
import math
import os
import time
import urllib.error
import urllib.parse
import urllib.request

try:
    import numpy as np
except ImportError:                                   # pragma: no cover
    np = None

OVERPASS_URL = "https://overpass-api.de/api/interpreter"
ATTRIBUTION = "Railway and coastline data (c) OpenStreetMap contributors, ODbL"

# Main and branch running lines only: `service` marks yards, sidings, spurs
# and crossovers, which would be noise at a kilometre per tile.
QUERIES = {
    "rail": """[out:json][timeout:600];
way["railway"~"^(rail|narrow_gauge)$"]["service"!~"."]({bbox});
out tags geom;""",
    "coast": """[out:json][timeout:600];
way["natural"="coastline"]({bbox});
out geom;""",
    # One named line, for trying the pipeline on a single route. A name
    # filter is light enough to run over the whole theater in one request.
    "line": """[out:json][timeout:600];
way["railway"~"^(rail|narrow_gauge)$"]["service"!~"."]["name:en"="{name}"]({bbox});
out tags geom;""",
}

# Real positions of airbases, keyed by the objective name with " Airbase"
# dropped. Only fields whose location is not in doubt; the fit drops any
# whose residual stands out, so a wrong entry costs a control point, not
# the projection.
AIRBASES = {
    "korea": {
        "Osan": (37.0906, 127.0297), "Suwon": (37.2394, 127.0071),
        "Kimpo": (37.5583, 126.7906), "Seoul": (37.4459, 127.1139),
        "Chongju": (36.7166, 127.4991), "Kunsan": (35.9038, 126.6158),
        "Kwangju": (35.1264, 126.8089), "Taegu": (35.8941, 128.6589),
        "Kimhae International": (35.1795, 128.9382),
        "Pohang": (35.9879, 129.4203), "Sachon": (35.0886, 128.0703),
        "Kangnung": (37.7536, 128.9440), "Choongwon": (37.0335, 127.8859),
        "Yechon": (36.6319, 128.3549), "Pyeongtaeg": (36.9621, 127.0311),
        "Seosan": (36.7040, 126.4863), "Sunan": (39.2241, 125.6703),
        "Wonsan": (39.1668, 127.4860), "Hwangju": (38.6532, 125.7897),
        "Uiju": (40.1506, 124.4986), "Orang": (41.4283, 129.6475),
        "Sondok": (39.7451, 127.4736), "Samjiyon-up": (41.907, 128.41),
    },
}


# -- download ----------------------------------------------------------------

TILE_DEG = 2.0      # one whole-theater query times out at the public server


MIN_TILE_DEG = 0.25


def _query(kind, bbox, log, tries=2, name=""):
    """One Overpass request; returns its element list."""
    query = QUERIES[kind].format(bbox="%.4f,%.4f,%.4f,%.4f" % tuple(bbox),
                                 name=name.replace('"', ''))
    data = urllib.parse.urlencode({"data": query}).encode()
    attempt = 0
    throttled = 0
    while True:
        req = urllib.request.Request(OVERPASS_URL, data=data, headers={
            "User-Agent": "FreeFalcon campaign editor (rail overlay)"})
        try:
            with urllib.request.urlopen(req, timeout=900) as r:
                doc = json.loads(r.read().decode("utf-8"))
            remark = doc.get("remark") or ""
            if "error" in remark.lower() or "timed out" in remark.lower():
                raise ValueError(remark)
            return doc.get("elements", [])
        except urllib.error.HTTPError as exc:
            # 429 is the server's rate limit, not a verdict on the query:
            # splitting would only send more requests. Wait for a slot.
            if exc.code == 429 and throttled < 20:
                throttled += 1
                log("  rate limited; waiting 60s")
                time.sleep(60)
                continue
            err = exc
        except (urllib.error.URLError, ValueError, OSError) as exc:
            err = exc
        attempt += 1
        log("  %s %.2f,%.2f..%.2f,%.2f: %s" % ((kind,) + tuple(bbox) + (err,)))
        if attempt >= tries:
            raise err
        time.sleep(20)


def _fetch_elements(kind, bbox, log):
    """Elements in `bbox`, quartering the box whenever the server gives up.

    The busy public server 504s on dense boxes (the south-west Korean
    coast is thousands of islets); four smaller queries usually go through.
    """
    s, w, n, e = bbox
    smallest = n - s <= MIN_TILE_DEG
    try:
        # Retry only at the smallest size; bigger boxes split at once.
        return _query(kind, bbox, log, tries=3 if smallest else 1)
    except (urllib.error.URLError, ValueError, OSError):
        if smallest:
            raise
        ms, mw = (s + n) / 2, (w + e) / 2
        log("  splitting %s into quarters" % kind)
        out = []
        for q in ((s, w, ms, mw), (s, mw, ms, e), (ms, w, n, mw), (ms, mw, n, e)):
            out += _fetch_elements(kind, q, log)
            time.sleep(2)
        return out


def _fetch_one(kind, bbox, path, log):
    elements = _fetch_elements(kind, bbox, log)
    tmp = path + ".part"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump({"elements": elements}, f)
    os.replace(tmp, path)


def fetch_line(name, bbox, cache_dir, refresh=False, log=print):
    """One named railway line over the whole box, cached; returns [path]."""
    os.makedirs(cache_dir, exist_ok=True)
    slug = "".join(c if c.isalnum() else "_" for c in name.lower())
    path = os.path.join(cache_dir, "line_%s_%.0f_%.0f_%.0f_%.0f.json"
                        % ((slug,) + tuple(bbox)))
    if refresh or not os.path.isfile(path):
        log("fetching line %r" % name)
        elements = _query("line", bbox, log, tries=8, name=name)
        with open(path + ".part", "w", encoding="utf-8") as f:
            json.dump({"elements": elements}, f)
        os.replace(path + ".part", path)
    return [path]


def fetch(kind, bbox, cache_dir, refresh=False, log=print):
    """Run an Overpass query over `bbox` in tiles; returns the cached paths.

    `bbox` is (south, west, north, east) in degrees. Each tile is cached on
    its own, so an interrupted download resumes where it stopped.
    """
    os.makedirs(cache_dir, exist_ok=True)
    s, w, n, e = bbox
    paths = []
    lat = s
    while lat < n:
        lon = w
        while lon < e:
            t = (lat, lon, min(lat + TILE_DEG, n), min(lon + TILE_DEG, e))
            path = os.path.join(cache_dir, "%s_%.2f_%.2f_%.2f_%.2f.json"
                                % ((kind,) + t))
            if refresh or not os.path.isfile(path):
                log("fetching %s %.1f..%.1fN %.1f..%.1fE"
                    % (kind, t[0], t[2], t[1], t[3]))
                _fetch_one(kind, t, path, log)
                time.sleep(2)                        # be polite to the server
            paths.append(path)
            lon += TILE_DEG
        lat += TILE_DEG
    return paths


def load_ways(paths):
    """[(tags, [(lat, lon), ...]), ...] from Overpass `out geom` files.

    A way crossing a tile edge comes back from both tiles, whole; keep one.
    """
    ways, seen = [], set()
    for path in ([paths] if isinstance(paths, str) else paths):
        with open(path, encoding="utf-8") as f:
            doc = json.load(f)
        for el in doc.get("elements", ()):
            geom = el.get("geometry")
            if el.get("type") != "way" or not geom or len(geom) < 2 \
                    or el["id"] in seen:
                continue
            seen.add(el["id"])
            ways.append((el.get("tags", {}),
                         [(p["lat"], p["lon"]) for p in geom]))
    return ways


# -- projection --------------------------------------------------------------

def _terms(u, v, order):
    """Polynomial terms of normalised plane coordinates, lowest order first."""
    cols = [np.ones_like(u), u, v]
    if order >= 2:
        cols += [u * u, u * v, v * v]
    if order >= 3:
        cols += [u * u * u, u * u * v, u * v * v, v * v * v]
    return np.stack(cols, -1)


N_TERMS = {1: 3, 2: 6, 3: 10}


class Projection:
    """lat/lon -> campaign grid km, through a local plane and a polynomial.

    The plane is equirectangular about (lat0, lon0), in units of `SCALE` km
    so the higher-order terms stay well conditioned.
    """

    SCALE = 500.0

    def __init__(self, lat0, lon0, order=1, coef=None):
        self.lat0, self.lon0, self.order = lat0, lon0, order
        self.coef = (np.zeros((N_TERMS[order], 2)) if coef is None
                     else np.asarray(coef, float))

    def plane(self, lat, lon):
        lat = np.asarray(lat, float)
        lon = np.asarray(lon, float)
        u = (lon - self.lon0) * 111.32 * math.cos(math.radians(self.lat0))
        v = (lat - self.lat0) * 110.57
        return u / self.SCALE, v / self.SCALE

    def terms(self, lat, lon):
        u, v = self.plane(lat, lon)
        return _terms(u, v, self.order)

    def __call__(self, lat, lon):
        """Grid (x, y) arrays for arrays of lat, lon."""
        p = self.terms(lat, lon) @ self.coef
        return p[..., 0], p[..., 1]

    def raise_order(self, order):
        c = np.zeros((N_TERMS[order], 2))
        c[:len(self.coef)] = self.coef
        self.order, self.coef = order, c

    def fit_points(self, lat, lon, gx, gy):
        T = self.terms(lat, lon)
        B = np.stack([gx, gy], -1).astype(float)
        self.coef = np.linalg.lstsq(T, B, rcond=None)[0]

    def inverse(self, x, y):
        """Grid -> lat/lon by Newton steps (for the download box only)."""
        lat, lon = self.lat0, self.lon0
        for _ in range(30):
            px, py = self(lat, lon)
            e = 1e-4
            ax, ay = self(lat + e, lon)
            bx, by = self(lat, lon + e)
            J = np.array([[(ax - px) / e, (bx - px) / e],
                          [(ay - py) / e, (by - py) / e]])
            d = np.linalg.solve(J, [x - px, y - py])
            lat, lon = lat + d[0], lon + d[1]
            if abs(d[0]) + abs(d[1]) < 1e-9:
                break
        return float(lat), float(lon)

    def to_json(self):
        return {"model": "poly%d" % self.order, "lat0": self.lat0,
                "lon0": self.lon0, "planeScaleKm": self.SCALE,
                "coef": [[round(float(a), 9) for a in r] for r in self.coef]}


def fit_airbases(pairs, lat0, lon0, log=print):
    """Affine fit to [(name, lat, lon, gx, gy)], dropping outliers.

    Returns (projection, [control dicts]).
    """
    names = [p[0] for p in pairs]
    lat, lon, gx, gy = (np.array([p[i] for p in pairs], float)
                        for i in range(1, 5))
    proj = Projection(lat0, lon0, 1)
    keep = np.ones(len(pairs), bool)
    for _ in range(4):
        proj.fit_points(lat[keep], lon[keep], gx[keep], gy[keep])
        fx, fy = proj(lat, lon)
        res = np.hypot(fx - gx, fy - gy)
        rms = math.sqrt(float(np.mean(res[keep] ** 2)))
        new = res < max(3 * rms, 4.0)
        if (new == keep).all():
            break
        keep = new
    log("airbase affine: %d of %d kept, %.1f km RMS"
        % (keep.sum(), len(pairs), rms))
    return proj, [{"name": n, "real": [float(a), float(b)],
                   "grid": [float(c), float(d)], "used": bool(k)}
                  for n, a, b, c, d, k in zip(names, lat, lon, gx, gy, keep)]


def control_residuals(proj, control):
    """Fill in each control point's fitted position; returns the RMS (km)."""
    res = []
    for c in control:
        fx, fy = proj(c["real"][0], c["real"][1])
        c["fit"] = [round(float(fx), 2), round(float(fy), 2)]
        c["errKm"] = round(math.hypot(fx - c["grid"][0], fy - c["grid"][1]), 2)
        if c["used"]:
            res.append(c["errKm"])
    return math.sqrt(sum(r * r for r in res) / max(1, len(res)))


# -- the terrain's shoreline ---------------------------------------------------

def _grow(m):
    g = m.copy()
    g[1:, :] |= m[:-1, :]
    g[:-1, :] |= m[1:, :]
    g[:, 1:] |= m[:, :-1]
    g[:, :-1] |= m[:, 1:]
    return g


def sea_mask(water):
    """Water cells connected to the edge of the map (drops lakes and rivers)."""
    sea = np.zeros_like(water)
    sea[0, :], sea[-1, :], sea[:, 0], sea[:, -1] = (
        water[0, :], water[-1, :], water[:, 0], water[:, -1])
    while True:
        grow = sea
        for _ in range(16):
            grow = _grow(grow) & water
        if (grow == sea).all():
            return sea
        sea = grow


def shore_distance(sea, cap=64):
    """Distance in km from each cell centre to the nearest land/sea edge.

    Exact Euclidean distance to edge cells, computed separably: nearest edge
    cell in each column first, then the best column within `cap` km.
    """
    land = ~sea
    edge = land & _grow(sea)
    h, _w = edge.shape
    col = np.where(edge, 0.0, float(cap * 4))
    for y in range(1, h):                      # down the columns...
        col[y] = np.minimum(col[y], col[y - 1] + 1)
    for y in range(h - 2, -1, -1):             # ...and back up
        col[y] = np.minimum(col[y], col[y + 1] + 1)
    sq = col * col
    best = sq.copy()
    for s in range(1, cap + 1):
        best[:, s:] = np.minimum(best[:, s:], sq[:, :-s] + s * s)
        best[:, :-s] = np.minimum(best[:, :-s], sq[:, s:] + s * s)
    return np.sqrt(best)


def _bilinear(f, x, y):
    """Sample f[y][x] at fractional km (cell centres sit at +0.5)."""
    h, w = f.shape
    x = np.clip(x - 0.5, 0, w - 1.001)
    y = np.clip(y - 0.5, 0, h - 1.001)
    x0, y0 = x.astype(int), y.astype(int)
    fx, fy = x - x0, y - y0
    return (f[y0, x0] * (1 - fx) * (1 - fy) + f[y0, x0 + 1] * fx * (1 - fy) +
            f[y0 + 1, x0] * (1 - fx) * fy + f[y0 + 1, x0 + 1] * fx * fy)


def resample(pts, step):
    """Points every `step` along a polyline ((n,2) array in, (m,2) out)."""
    d = np.hypot(*np.diff(pts, axis=0).T)
    s = np.concatenate([[0], np.cumsum(d)])
    if s[-1] < step:
        return pts[:1]
    t = np.arange(0, s[-1], step)
    return np.stack([np.interp(t, s, pts[:, 0]),
                     np.interp(t, s, pts[:, 1])], -1)


def coast_samples(coast_ways, step_km=0.5):
    """Coastline as lat/lon points about `step_km` apart."""
    lat, lon = [], []
    for _tags, pts in coast_ways:
        a = np.array(pts, float)
        k = math.cos(math.radians(float(a[:, 0].mean())))
        km = np.stack([a[:, 1] * 111.32 * k, a[:, 0] * 110.57], -1)
        r = resample(km, step_km)
        lon.append(r[:, 0] / (111.32 * k))
        lat.append(r[:, 1] / 110.57)
    return np.concatenate(lat), np.concatenate(lon)


def shore_score(proj, lat, lon, dist):
    h, w = dist.shape
    x, y = proj(lat, lon)
    inside = (x > 0) & (y > 0) & (x < w) & (y < h)
    d = _bilinear(dist, x[inside], y[inside])
    return float(np.median(d)), float(np.mean(d < 1.5))


def refine_to_shore(proj, lat, lon, dist, control, orders=(1, 2, 3),
                    cap=8.0, pull=0.05, log=print):
    """Refine `proj` so the projected coastline sits on the terrain's shore.

    Gauss-Newton on the polynomial, minimising the distance field sampled at
    coastline points. A point further than `cap` km from any shore counts as
    unmatched (an islet the terrain lacks) and is left out of that step. The
    airbases pull with total weight `pull` of the coastline's, which stops a
    long straight coast from letting the fit slide along it.
    """
    h, w = dist.shape
    gy_d, gx_d = np.gradient(dist)
    used = [c for c in control if c["used"]]
    a_lat = np.array([c["real"][0] for c in used])
    a_lon = np.array([c["real"][1] for c in used])
    a_xy = np.array([c["grid"] for c in used])

    med, frac = shore_score(proj, lat, lon, dist)
    log("shore, airbase fit: median %.2f km, %.0f%% of coast within 1.5 km"
        % (med, frac * 100))
    for order in orders:
        proj.raise_order(order)
        n = N_TERMS[order]
        for _it in range(40):
            x, y = proj(lat, lon)
            inside = (x > 1) & (y > 1) & (x < w - 1) & (y < h - 1)
            xi, yi = x[inside], y[inside]
            d = _bilinear(dist, xi, yi)
            m = d < cap
            if m.sum() < 100:
                break
            T = proj.terms(lat[inside][m], lon[inside][m])
            gx = _bilinear(gx_d, xi[m], yi[m])[:, None]
            gy = _bilinear(gy_d, xi[m], yi[m])[:, None]
            J = np.hstack([gx * T, gy * T])            # d(dist)/d(coef)
            r = d[m]
            Ta = proj.terms(a_lat, a_lon)
            fa = Ta @ proj.coef
            wa = pull * len(r) / max(1, len(used))
            Z = np.zeros_like(Ta)
            Ja = np.vstack([np.hstack([Ta, Z]), np.hstack([Z, Ta])])
            ra = np.concatenate([fa[:, 0] - a_xy[:, 0], fa[:, 1] - a_xy[:, 1]])
            A = J.T @ J + wa * (Ja.T @ Ja)
            b = J.T @ r + wa * (Ja.T @ ra)
            A += np.eye(2 * n) * 1e-3 * np.trace(A) / (2 * n)   # damping
            step = np.linalg.solve(A, b)
            proj.coef -= np.stack([step[:n], step[n:]], -1)
            if np.abs(step).max() < 1e-3:
                break
        med, frac = shore_score(proj, lat, lon, dist)
        log("shore after order %d: median %.2f km, %.0f%% within 1.5 km"
            % (order, med, frac * 100))
    return med, frac


# -- lines ------------------------------------------------------------------

def _seg_flag(tags):
    if tags.get("tunnel") not in (None, "no"):
        return "t"
    if tags.get("bridge") not in (None, "no"):
        return "b"
    return "-"


def _line_class(tags):
    return (tags.get("usage") or "", tags.get("railway") or "")


def merge_ways(ways):
    """Join ways end to end where exactly two of the same class meet.

    OSM cuts a line into a new way at every bridge, tunnel and tag change, so
    a main line arrives as hundreds of pieces; a train wants one polyline.
    Returns [{"cls", "names", "gauge", "ele", "pts": [(lat, lon)], "seg"}]
    where seg holds one flag per segment: '-' plain, 'b' bridge, 't' tunnel.
    """
    items = []
    for tags, pts in ways:
        items.append({"cls": _line_class(tags),
                      "names": [tags.get("name:en") or tags.get("name") or ""],
                      "gauge": tags.get("gauge", ""),
                      "ele": tags.get("electrified", "no") not in ("no", ""),
                      "pts": list(pts),
                      "seg": _seg_flag(tags) * (len(pts) - 1)})

    def key(p):
        return (round(p[0], 7), round(p[1], 7))

    ends = {}
    for i, it in enumerate(items):
        for e in (key(it["pts"][0]), key(it["pts"][-1])):
            ends.setdefault(e, []).append(i)
    alive = [True] * len(items)

    def partner(i, e):
        at = ends.get(e, ())
        if len(at) != 2:
            return None
        j = at[0] if at[1] == i else at[1]
        if j == i or not alive[j] or items[j]["cls"] != items[i]["cls"]:
            return None
        return j

    for i, it in enumerate(items):
        if not alive[i]:
            continue
        for _side in range(2):
            while True:
                e = key(it["pts"][-1])
                j = partner(i, e)
                if j is None:
                    break
                o = items[j]
                alive[j] = False
                if key(o["pts"][0]) != e:           # partner runs backwards
                    o["pts"].reverse()
                    o["seg"] = o["seg"][::-1]
                it["pts"] += o["pts"][1:]
                it["seg"] += o["seg"]
                it["names"] += o["names"]
                it["ele"] = it["ele"] or o["ele"]
                it["gauge"] = it["gauge"] or o["gauge"]
                ends[e] = []
                # the far end of the partner now belongs to this line
                fe = key(it["pts"][-1])
                ends[fe] = [i if x == j else x for x in ends.get(fe, [])]
            it["pts"].reverse()
            it["seg"] = it["seg"][::-1]
    return [it for i, it in enumerate(items) if alive[i]]


def _douglas_peucker(pts, tol):
    """Indices to keep of an (n,2) array."""
    keep = np.zeros(len(pts), bool)
    keep[0] = keep[-1] = True
    stack = [(0, len(pts) - 1)]
    while stack:
        a, b = stack.pop()
        if b <= a + 1:
            continue
        p, q = pts[a], pts[b]
        d = q - p
        n = math.hypot(*d)
        mid = pts[a + 1:b]
        if n < 1e-12:
            dist = np.hypot(*(mid - p).T)
        else:
            dist = np.abs(d[0] * (mid[:, 1] - p[1]) -
                          d[1] * (mid[:, 0] - p[0])) / n
        k = int(np.argmax(dist))
        if dist[k] > tol:
            keep[a + 1 + k] = True
            stack += [(a, a + 1 + k), (a + 1 + k, b)]
    return np.nonzero(keep)[0]


def simplify(xy, seg, tol):
    """Simplify, keeping every point where the bridge/tunnel flag changes."""
    cuts = ([0] + [i for i in range(1, len(seg)) if seg[i] != seg[i - 1]] +
            [len(xy) - 1])
    keep = []
    for a, b in zip(cuts, cuts[1:]):
        idx = list(_douglas_peucker(xy[a:b + 1], tol) + a)
        keep.extend(idx if not keep else idx[1:])
    out_seg = "".join(seg[keep[i]] for i in range(len(keep) - 1))
    return xy[keep], out_seg


def clip(xy, seg, w, h):
    """Split a line where it leaves the grid; returns [(xy, seg)]."""
    inside = ((xy[:, 0] >= 0) & (xy[:, 1] >= 0) &
              (xy[:, 0] <= w) & (xy[:, 1] <= h))
    out, start = [], None
    for i, ok in enumerate(list(inside) + [False]):
        if ok and start is None:
            start = i
        elif not ok and start is not None:
            if i - start >= 2:
                out.append((xy[start:i], seg[start:i - 1]))
            start = None
    return out


def build_lines(ways, proj, w, h, tol=0.05):
    """Merge, project, clip and simplify; returns the output line dicts."""
    lines = []
    for it in merge_ways(ways):
        a = np.array(it["pts"], float)
        x, y = proj(a[:, 0], a[:, 1])
        for part, seg in clip(np.stack([x, y], -1), it["seg"], w, h):
            part, seg = simplify(part, seg, tol)
            if len(part) < 2:
                continue
            names = [n for n in it["names"] if n]
            line = {
                "name": max(set(names), key=names.count) if names else "",
                "usage": it["cls"][0],
                "pts": [[round(float(px), 3), round(float(py), 3)]
                        for px, py in part],
            }
            if it["cls"][1] != "rail":
                line["railway"] = it["cls"][1]
            if it["gauge"]:
                line["gauge"] = it["gauge"]
            if it["ele"]:
                line["electrified"] = True
            if set(seg) != {"-"}:
                line["seg"] = seg
            lines.append(line)
    return lines


def line_km(line):
    p = np.array(line["pts"])
    return float(np.hypot(*np.diff(p, axis=0).T).sum())


# -- routes: one continuous polyline per named line, for trains ---------------

GAME_FILENAME = "rail.txt"
JOIN_KM = 0.5       # piece ends closer than this are the same place


def _path_len(pts):
    p = np.asarray(pts, float)
    return float(np.hypot(*np.diff(p, axis=0).T).sum())


def build_route(pieces, segs=None):
    """The longest end-to-end run through one named line's pieces.

    A line arrives as many pieces: OSM splits it at every junction, and a
    double-track line is two parallel sets. Treat piece ends as graph nodes
    (ends within JOIN_KM are one node), pieces as edges weighted by length,
    keep the biggest connected part, and walk its diameter -- farthest node
    from anywhere, then farthest from that. A train needs one track to run
    on, not the whole web. Returns [[x, y], ...] or None.

    With `segs` (one flag string per piece, one flag per segment: '-' plain,
    'b' bridge, 't' tunnel) returns (points, flags) instead, the flags walked
    along with the points.
    """
    import heapq
    ends = []                                   # node positions
    edges = []                                  # (a, b, pts, length)

    def node(p):
        for i, q in enumerate(ends):
            if math.hypot(p[0] - q[0], p[1] - q[1]) < JOIN_KM:
                return i
        ends.append(p)
        return len(ends) - 1

    for i, pts in enumerate(pieces):
        if len(pts) < 2:
            continue
        a, b = node(pts[0]), node(pts[-1])
        seg = segs[i] if segs and segs[i] else "-" * (len(pts) - 1)
        if a != b:
            edges.append((a, b, pts, _path_len(pts), seg))
    if not edges:
        return None
    adj = {}
    for k, (a, b, _p, ln, _s) in enumerate(edges):
        adj.setdefault(a, []).append((b, ln, k))
        adj.setdefault(b, []).append((a, ln, k))

    def dijkstra(src):
        dist, prev = {src: 0.0}, {}
        heap = [(0.0, src)]
        while heap:
            d, u = heapq.heappop(heap)
            if d > dist[u]:
                continue
            for v, ln, k in adj.get(u, ()):
                nd = d + ln
                if nd < dist.get(v, float("inf")):
                    dist[v], prev[v] = nd, (u, k)
                    heapq.heappush(heap, (nd, v))
        return dist, prev

    # Biggest component by track length: start from each unseen node.
    seen, best = set(), None
    for start in adj:
        if start in seen:
            continue
        dist, _ = dijkstra(start)
        seen.update(dist)
        comp_len = sum(ln for a, b, _p, ln, _s in edges if a in dist)
        if best is None or comp_len > best[0]:
            best = (comp_len, start)
    # The two nodes furthest apart on the map, then the shortest track
    # between them. Not the longest shortest path: a parallel track that
    # joins only at one end (Seoul, on the Gyeongbu Line) makes that path
    # run up to the junction and back down the spur. Not "dead ends" only
    # either: both tracks of a double line end on the Busan node, so the
    # real terminus has two pieces meeting at it.
    comp, _ = dijkstra(best[1])
    tips = list(comp)
    far, other = max(((a, b) for a in tips for b in tips),
                     key=lambda ab: math.hypot(ends[ab[0]][0] - ends[ab[1]][0],
                                               ends[ab[0]][1] - ends[ab[1]][1]))
    dist, prev = dijkstra(far)

    out, flags = [ends[other]], ""
    u = other
    while u != far:
        pu, k = prev[u]
        a, b, pts, _ln, seg = edges[k]
        fwd = a == pu
        run = pts if fwd else pts[::-1]         # walk from pu to u...
        out = [list(p) for p in run] + out[1:]  # ...prepending, so it ends at `other`
        flags = (seg if fwd else seg[::-1]) + flags
        u = pu
    return (out, flags) if segs is not None else out


def build_routes(lines):
    """{name: route points} for every named line."""
    by_name = {}
    for ln in lines:
        if ln.get("name"):
            pts = ln["pts"]
            by_name.setdefault(ln["name"], []).append(
                (pts, ln.get("seg") or "-" * (len(pts) - 1)))
    routes = []
    for name in sorted(by_name):
        got = build_route([p for p, _s in by_name[name]],
                          [sg for _p, sg in by_name[name]])
        if not got:
            continue
        pts, seg = got
        if len(pts) >= 2:
            route = {"name": name, "pts": pts, "km": round(_path_len(pts), 1)}
            if set(seg) != {"-"}:
                route["seg"] = seg
            routes.append(route)
    return routes


SHORE_INSET_KM = 0.3     # how far inside the land cell a point pulled off the sea lands
SEA_WINDOW = 2           # cells each side: a 5x5 km window...
SEA_BROAD = 0.4          # ...at least this much water = open sea, not a river crossing
SNAP_REACH = 5           # cells searched for land around a point at sea


class LandKeeper:
    """Moves route points that sit on open sea onto the nearest land.

    The fit is good to about a kilometre, and a line that hugs a coast (the
    Pyongra Line round Hamhung) keeps landing on sea tiles. Only broad water
    counts: sea_mask floods through anything joined to the map edge, river
    mouths included, and a real rail bridge crosses those -- so a point is
    moved only when at least SEA_BROAD of the 5x5 km around it is water.
    """

    def __init__(self, sea):
        self.sea = sea
        h, w = sea.shape
        pad = np.pad(sea.astype(np.float32), SEA_WINDOW)
        k = 2 * SEA_WINDOW + 1
        acc = np.zeros((h, w), np.float32)
        for dy in range(k):
            for dx in range(k):
                acc += pad[dy:dy + h, dx:dx + w]
        self.broad = sea & (acc / (k * k) >= SEA_BROAD)
        self.moved = 0
        self.worst = 0.0

    def at_sea(self, x, y):
        h, w = self.sea.shape
        ix, iy = int(x), int(y)
        return 0 <= ix < w and 0 <= iy < h and bool(self.broad[iy, ix])

    def to_land(self, x, y):
        """Nearest point just inside a land cell, or None if none in reach."""
        h, w = self.sea.shape
        ix, iy = int(x), int(y)
        best = None
        for cy in range(max(0, iy - SNAP_REACH), min(h, iy + SNAP_REACH + 1)):
            for cx in range(max(0, ix - SNAP_REACH), min(w, ix + SNAP_REACH + 1)):
                if self.sea[cy, cx]:
                    continue
                # Nearest point of that land cell, then pulled toward its centre.
                px = min(max(x, cx + SHORE_INSET_KM), cx + 1 - SHORE_INSET_KM)
                py = min(max(y, cy + SHORE_INSET_KM), cy + 1 - SHORE_INSET_KM)
                d = math.hypot(px - x, py - y)
                if best is None or d < best[0]:
                    best = (d, px, py)
        return best

    def fix(self, pts, densify_km=None, seg=None):
        """Points of one polyline, with any at sea moved ashore.

        With `seg` (one flag per segment) returns (points, flags): densified
        segments keep their flag, and a point with a bridge on both sides is
        never moved -- a bridge across a wide river mouth is meant to be over
        water.
        """
        out, flags = [], []
        for i, (x, y) in enumerate(pts):
            if densify_km and i:
                px, py = pts[i - 1]
                f = seg[i - 1] if seg else "-"
                n = int(math.hypot(x - px, y - py) / densify_km)
                for k in range(1, n + 1):
                    t = k / (n + 1.0)
                    out.append([px + (x - px) * t, py + (y - py) * t])
                    flags.append(f)
            if i:
                flags.append(seg[i - 1] if seg else "-")
            out.append([x, y])
        for i, p in enumerate(out):
            on_bridge = (0 < i < len(out) - 1 and flags[i - 1] == "b" and
                         flags[i] == "b")
            if not on_bridge and self.at_sea(p[0], p[1]):
                land = self.to_land(p[0], p[1])
                if land:
                    self.moved += 1
                    self.worst = max(self.worst, land[0])
                    p[0], p[1] = land[1], land[2]
        return (out, "".join(flags)) if seg is not None else out


def keep_on_land(doc, terr, log=print):
    """Pull lines and routes off open sea; rebuilds the routes. In place."""
    keeper = LandKeeper(sea_mask(water_mask(terr)))
    for ln in doc.get("lines", ()):
        # Vertices only: a line's `seg` flags are one per segment.
        ln["pts"] = [[round(x, 3), round(y, 3)] for x, y in keeper.fix(ln["pts"])]
    routes = build_routes(doc.get("lines", ()))
    for r in routes:
        # Densified, so a long straight segment cannot cut across a bay,
        # then simplified again.
        seg = r.get("seg") or "-" * (len(r["pts"]) - 1)
        pts, seg = keeper.fix(r["pts"], densify_km=0.25, seg=seg)
        pts, seg = simplify(np.array(pts, float), seg, 0.05)
        r["pts"] = [[round(float(x), 3), round(float(y), 3)] for x, y in pts]
        if set(seg) != {"-"}:
            r["seg"] = seg
        else:
            r.pop("seg", None)
        r["km"] = round(_path_len(r["pts"]), 1)
    doc["routes"] = routes
    log("kept on land: %d points moved off open sea, furthest %.2f km"
        % (keeper.moved, keeper.worst))
    return keeper.moved


def write_game_file(doc, path):
    """rail.txt: what the game reads (it has no JSON parser).

        ffrail 2
        route <npts> <name to end of line>
        <x km> <y km> <flag>     one per point, campaign km, x east, y north;
                                 flag of the segment starting here: - plain,
                                 b bridge, t tunnel (the last point's is -)

    Version 1 had no flag column; the game reads both.
    """
    with open(path, "w", encoding="ascii", errors="replace", newline="\n") as f:
        f.write("ffrail 2\n")
        f.write("# %s\n" % doc.get("attribution", ATTRIBUTION))
        for r in doc.get("routes", ()):
            pts = r["pts"]
            seg = r.get("seg") or "-" * (len(pts) - 1)
            f.write("route %d %s\n" % (len(pts), r["name"]))
            for i, (x, y) in enumerate(pts):
                f.write("%.3f %.3f %s\n" % (x, y, seg[i] if i < len(seg) else "-"))


# -- the whole pipeline --------------------------------------------------------

FORMAT = "ffrail/1"
FILENAME = "rail.json"


def water_mask(terr):
    """[y][x] bool: ground tiles whose average colour is water-blue.

    The texture sets' terrain-type byte does not mark the sea in these
    theaters, but every sea tile is unmistakably blue.
    """
    g = terr.grid()
    uniq = np.unique(g)
    lut = np.zeros((int(uniq.max()) + 1, 3))
    for v in uniq:
        lut[int(v)] = terr.mip(int(v), 4).reshape(-1, 3).mean(0)
    c = lut[g]
    return (c[..., 2] > c[..., 0] + 15) & (c[..., 2] > c[..., 1])


def airbase_pairs(objectives, name_table, table):
    """(name, lat, lon, x, y) for each airbase objective with a known spot.

    Grid indices name a cell; the engine puts the objective at the cell's
    centre (GridToSim adds OffsetToMiddle), hence the half kilometre.
    """
    pairs = []
    for o in objectives:
        if o.get("typeName") != "Airbase":
            continue
        name = name_table.get(o["nameId"], "").strip()
        if name.endswith(" Airbase"):
            name = name[:-len(" Airbase")]
        if name in table:
            lat, lon = table[name]
            pairs.append((name, lat, lon, o["x"] + 0.5, o["y"] + 0.5))
    return pairs


def debug_coast(coast_ways, proj, w, h, tol=0.3):
    """The projected coastline, coarsely simplified, for the editor overlay."""
    out = []
    for _tags, pts in coast_ways:
        a = np.array(pts, float)
        x, y = proj(a[:, 0], a[:, 1])
        for part, _seg in clip(np.stack([x, y], -1), "-" * (len(a) - 1), w, h):
            part, _ = simplify(part, "-" * (len(part) - 1), tol)
            if len(part) >= 2:
                out.append([[round(float(px), 2), round(float(py), 2)]
                            for px, py in part])
    return out


def build_theater(terr, objectives, name_table, airbases, cache_dir,
                  refresh=False, log=print, line=None):
    """Fit the projection, fetch OSM and build the track document.

    Returns (doc, debug): `doc` is what goes in rail.json; `debug` holds the
    projected coastline for the editor's fit overlay.
    """
    w = h = terr.size_km
    pairs = airbase_pairs(objectives, name_table, airbases)
    if len(pairs) < 4:
        raise ValueError("only %d airbases with known positions; need 4"
                         % len(pairs))
    lat0 = float(np.mean([p[1] for p in pairs]))
    lon0 = float(np.mean([p[2] for p in pairs]))
    proj, control = fit_airbases(pairs, lat0, lon0, log)

    # Download box: the grid's corners through the first fit, rounded out to
    # whole degrees so the cached tiles stay the same from run to run.
    corners = [proj.inverse(x, y) for x in (0, w) for y in (0, h)]
    bbox = (math.floor(min(c[0] for c in corners) - 0.3),
            math.floor(min(c[1] for c in corners) - 0.3),
            math.ceil(max(c[0] for c in corners) + 0.3),
            math.ceil(max(c[1] for c in corners) + 0.3))
    log("theater box %.0f..%.0fN %.0f..%.0fE" % (bbox[0], bbox[2], bbox[1], bbox[3]))

    if line:
        paths = []
        for name in ([line] if isinstance(line, str) else line):
            got = fetch_line(name, bbox, cache_dir, refresh, log)
            if not load_ways(got):
                raise ValueError("OSM has no railway ways named %r" % name)
            paths += got
        rail = load_ways(paths)
    else:
        rail = load_ways(fetch("rail", bbox, cache_dir, refresh, log))
    if not rail:
        raise ValueError("OSM returned no railway ways")
    coast = load_ways(fetch("coast", bbox, cache_dir, refresh, log))
    log("%d coastline ways, %d railway ways" % (len(coast), len(rail)))

    log("terrain shoreline...")
    dist = shore_distance(sea_mask(water_mask(terr)))
    lat, lon = coast_samples(coast)
    med, frac = refine_to_shore(proj, lat, lon, dist, control, log=log)
    rms = control_residuals(proj, control)
    log("airbases after refinement: %.1f km RMS" % rms)

    lines = build_lines(rail, proj, w, h)
    km = sum(line_km(ln) for ln in lines)
    log("%d lines, %.0f km of track" % (len(lines), km))

    doc = {
        "format": FORMAT,
        "attribution": ATTRIBUTION,
        "generated": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "units": ("campaign km: x east, y north, from the map's south-west "
                  "corner; sim X (north) = y * 3279.98 ft, sim Y (east) = "
                  "x * 3279.98 ft"),
        "sizeKm": [w, h],
        "projection": dict(proj.to_json(), shoreMedianKm=round(med, 2),
                           shoreWithin1_5Km=round(frac, 3),
                           airbaseRmsKm=round(rms, 2)),
        "control": control,
        "trackKm": round(km, 1),
        "lines": lines,
        "routes": [],
    }
    keep_on_land(doc, terr, log)
    return doc, {"coast": debug_coast(coast, proj, w, h)}
