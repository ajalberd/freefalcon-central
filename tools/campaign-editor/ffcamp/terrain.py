"""Render the theater's real ground texture as a map tile pyramid.

The kneeboard map is one 1024x1024 picture. The terrain underneath it is the
actual DXT1 ground imagery the sim flies over, and at full resolution it is
262144 x 262144 pixels -- so it has to be served as tiles.

How the pieces line up, from `tools/terrain/tilesurvey.py` and Theater.map:

    LOD 0 is 256x256 blocks of 16x16 posts = 4096x4096 posts
    820 ft per post, so 4 posts = 3280 ft = 1 km
    one ground tile covers exactly those 4x4 posts

which means **one terrain tile per campaign grid kilometre**, and the theater
is 1024x1024 km -- the same grid the units and objectives use, and the same
1024x1024 the kneemap is drawn at. Nothing needs rescaling; a campaign
coordinate is a tile index.

Orientation was settled against data already known to be right rather than
assumed: of the 913 ground battalions in korea2012's save0.cam, indexing the
texID grid as `[y][x]` puts 100% of them on land and 82.5% of the naval task
forces on water. Every other reflection scores far worse. So the array row is
the campaign y (north) and the column is the campaign x (east).

Needs numpy. It is the only part of the editor that does, and the map falls
back to the kneeboard image when it is missing.

The server is threaded, so several tile requests can be in flight at once.
Everything here is safe under that: caches are lock-guarded, a texture being
decoded is single-flighted (one thread decodes it, the rest wait for it), and
numpy releases the GIL, so decoding different textures actually uses several
cores. `warm()` runs that decode pass over the whole theater in the background
so a first pan is cache hits rather than a hundred decodes on the request path.
"""

import hashlib
import os
import struct
import tempfile
import threading
import zlib
from collections import OrderedDict
from concurrent.futures import ThreadPoolExecutor

try:
    import numpy as np
except ImportError:                                   # pragma: no cover
    np = None

POSTS_ACROSS_BLOCK = 16
POSTS_ACROSS_TILE = 4
TILE_PX = 256                 # served map tile size
Z_NATIVE = 10                 # at this zoom one served tile is one km
THEATER_KM = 1 << Z_NATIVE    # 1024

# Mip sizes kept for every distinct ground tile. 64px and below is 16 KB a
# tile, so the whole library fits in ~18 MB; the big two are cached per use.
SMALL_MIPS = (4, 8, 16, 32, 64)

# 256 and 128 are 196 KB and 49 KB a tile, so those get bounded LRU windows
# instead of the whole-library treatment. The 128 window is sized for a few
# screens of z=9 panning (the whole library of them would be ~74 MB).
FULL_CACHE = 220
MID_CACHE = 1024


def available():
    return np is not None


def _find(dirpath, name):
    p = os.path.join(dirpath, name)
    if os.path.exists(p):
        return p
    low = name.lower()
    for f in os.listdir(dirpath):
        if f.lower() == low:
            return os.path.join(dirpath, f)
    raise FileNotFoundError(p)


def write_png(arr):
    """(h, w, 3) uint8 -> PNG bytes."""
    h, w, _ = arr.shape
    rows = np.empty((h, w * 3 + 1), np.uint8)
    rows[:, 0] = 0                                    # filter: none
    rows[:, 1:] = arr.reshape(h, w * 3)

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload +
                struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows.tobytes(), 1))
            + chunk(b"IEND", b""))


def _decode_dxt1(data, w, h):
    """Raw DXT1 surface -> (h, w, 3) uint8. Same maths as tilesurvey.py."""
    bw, bh = w // 4, h // 4
    blocks = np.frombuffer(data[:bw * bh * 8], np.uint8).reshape(bh, bw, 8)
    c0 = blocks[:, :, 0].astype(np.uint16) | (blocks[:, :, 1].astype(np.uint16) << 8)
    c1 = blocks[:, :, 2].astype(np.uint16) | (blocks[:, :, 3].astype(np.uint16) << 8)

    def unpack565(c):
        r = (((c >> 11) & 0x1F).astype(np.int32) * 255 + 15) // 31
        g = (((c >> 5) & 0x3F).astype(np.int32) * 255 + 31) // 63
        b = ((c & 0x1F).astype(np.int32) * 255 + 15) // 31
        return np.stack([r, g, b], -1)

    a, b = unpack565(c0), unpack565(c1)
    gt = (c0 > c1)[..., None]
    c2 = np.where(gt, (2 * a + b) // 3, (a + b) // 2)
    c3 = np.where(gt, (a + 2 * b) // 3, np.zeros_like(a))
    pal = np.stack([a, b, c2, c3], axis=2)

    words = (blocks[:, :, 4].astype(np.uint32)
             | (blocks[:, :, 5].astype(np.uint32) << 8)
             | (blocks[:, :, 6].astype(np.uint32) << 16)
             | (blocks[:, :, 7].astype(np.uint32) << 24))
    shifts = (np.arange(16, dtype=np.uint32) * 2).reshape(4, 4)
    idx = (words[:, :, None, None] >> shifts) & 3
    out = np.take_along_axis(pal[:, :, None, None, :, :],
                             idx[..., None, None], axis=4)[..., 0, :]
    return out.transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 3).astype(np.uint8)


def _read_dds(path):
    with open(path, "rb") as f:
        head = f.read(128)
        if head[:4] != b"DDS ":
            return None
        h, w = struct.unpack_from("<2I", head, 12)
        if head[84:88] != b"DXT1":
            return None
        return _decode_dxt1(f.read(), w, h)


def _box(arr, size):
    """Box-filter a square RGB array down to size x size."""
    n = arr.shape[0]
    if n == size:
        return arr
    if n % size:
        step = max(1, n // size)
        arr = arr[:step * size:step, :step * size:step]
        n = arr.shape[0]
        if n != size:
            return np.ascontiguousarray(arr[:size, :size])
    k = n // size
    return (arr.reshape(size, k, size, k, 3)
               .mean(axis=(1, 3)).astype(np.uint8))


def _reduce(arr, size):
    """One step of `_box`, with the accumulator left in float64.

    Chaining uint8 reductions would round at every step and drift a little
    from filtering the full-size tile in one go. The mean of means over equal
    blocks is the same computation, so keeping it in float makes a prepared
    mip byte-identical to `_box(rgb, size)` while still only touching each
    pixel once per level.
    """
    n = arr.shape[0]
    k = n // size
    return arr.reshape(size, k, size, k, 3).mean(axis=(1, 3))


class Terrain:
    """One theater's ground imagery, as a tile pyramid."""

    def __init__(self, theater_dir, cache_dir=None):
        if np is None:
            raise RuntimeError("terrain rendering needs numpy")
        self.dir = theater_dir
        self.terrain_dir = _find(theater_dir, "terrain")
        self.texture_dir = _find(theater_dir, "texture")
        self.tile_dir = _find(self.texture_dir, "texture")
        self._read_map()
        self._sets = self._read_texture_bin()

        self.offsets = np.frombuffer(
            open(_find(self.terrain_dir, "Theater.o0"), "rb").read(), "<u4")
        self.posts = np.memmap(_find(self.terrain_dir, "Theater.l0"),
                               np.uint8, mode="r")

        self._grid = None
        self._mips = {size: {} for size in SMALL_MIPS}
        self._mid = OrderedDict()              # (texid, 128) -> array
        self._full_cache = OrderedDict()       # texid -> 256px array
        self._overview = None
        # The server is threaded: every request that wants a tile comes in on
        # its own thread, and two of them can want the same texture at once.
        self._lock = threading.RLock()
        self._overview_lock = threading.Lock()
        self._preparing = {}                   # texid -> Event while decoding
        self._warming = False
        self.cache_dir = cache_dir or os.path.join(
            tempfile.gettempdir(), "ffcamp-terrain",
            hashlib.sha1(os.path.abspath(theater_dir).encode()).hexdigest()[:12])

    # -- headers --------------------------------------------------------------

    def _read_map(self):
        d = open(_find(self.terrain_dir, "Theater.map"), "rb").read()
        o = 0
        (self.feet_per_post,) = struct.unpack_from("<f", d, o); o += 4
        o += 8 + 4 + 4 + 8                       # mea size, ft_to_mea, nlevels...
        o -= 4                                   # ...keep nlevels
        (self.nlevels,) = struct.unpack_from("<i", d, o - 8)
        o = 4 + 8 + 4
        (self.nlevels,) = struct.unpack_from("<i", d, o); o += 4
        o += 8                                   # lastNear/FarTex
        packed = np.frombuffer(d[o:o + 1024], "<u4"); o += 1024
        self.colour_table = np.stack([packed & 0xFF, (packed >> 8) & 0xFF,
                                      (packed >> 16) & 0xFF], -1).astype(np.uint8)
        self.levels = []
        for _ in range(self.nlevels):
            self.levels.append(struct.unpack_from("<ii", d, o)); o += 8
        (self.flags,) = struct.unpack_from("<i", d, o)
        self.large_terrain = bool(self.flags & 0x1)
        self.post_size = 9 if self.large_terrain else 7
        self.bw, self.bh = self.levels[0]

    def _read_texture_bin(self):
        d = open(_find(self.texture_dir, "texture.bin"), "rb").read()
        o = 0
        num_sets, _total = struct.unpack_from("<ii", d, o); o += 8
        sets = []
        for _ in range(num_sets):
            (n_tiles,) = struct.unpack_from("<i", d, o); o += 4
            o += 1                                          # terrain type
            tiles = []
            for _ in range(n_tiles):
                fn = d[o:o + 20].split(b"\0")[0].decode("latin-1"); o += 20
                n_areas, n_paths = struct.unpack_from("<ii", d, o); o += 8
                o += n_areas * 16 + n_paths * 24
                tiles.append(fn)
            sets.append(tiles)
        return sets

    def tile_path(self, texid):
        s, t = (texid >> 4) & 0xFF, texid & 0xF
        if s >= len(self._sets) or t >= len(self._sets[s]):
            return None
        return os.path.join(self.tile_dir,
                            self._sets[s][t].split(".")[0] + ".dds")

    # -- the km grid ----------------------------------------------------------

    @property
    def size_km(self):
        return self.bw * POSTS_ACROSS_BLOCK // POSTS_ACROSS_TILE

    def grid(self):
        """texID per campaign kilometre, indexed [y][x] with y running north."""
        with self._lock:
            if self._grid is not None:
                return self._grid
            psz = self.post_size
            offs = self.offsets.astype(np.int64).reshape(self.bh, self.bw)
            step = np.arange(0, POSTS_ACROSS_BLOCK, POSTS_ACROSS_TILE)
            rr, cc = np.meshgrid(step, step, indexing="ij")
            within = ((rr * POSTS_ACROSS_BLOCK + cc) * psz).astype(np.int64)
            base = offs[:, :, None, None] + within[None, None, :, :]
            b = np.asarray(self.posts)
            tex = (b[base].astype(np.uint32)
                   | b[base + 1].astype(np.uint32) << 8
                   | b[base + 2].astype(np.uint32) << 16
                   | b[base + 3].astype(np.uint32) << 24)
            n = POSTS_ACROSS_BLOCK // POSTS_ACROSS_TILE
            self._grid = tex.transpose(0, 2, 1, 3).reshape(self.bh * n,
                                                           self.bw * n)
            return self._grid

    # -- tile images ----------------------------------------------------------

    def _decode_full(self, texid):
        """Decode one 256x256 ground tile, or black where the file is absent."""
        path = self.tile_path(texid)
        rgb = _read_dds(path) if path and os.path.exists(path) else None
        if rgb is None:
            rgb = np.zeros((TILE_PX, TILE_PX, 3), np.uint8)
        elif rgb.shape[0] != TILE_PX:
            rgb = _box(rgb, TILE_PX)
        return rgb

    def _store_full(self, texid, rgb):
        with self._lock:
            self._full_cache[texid] = rgb
            self._full_cache.move_to_end(texid)
            while len(self._full_cache) > FULL_CACHE:
                self._full_cache.popitem(last=False)

    def _full(self, texid):
        """The 256px original, from a small LRU; decodes on a miss.

        Only the native zoom wants this size, and there the same texture keeps
        coming back as you pan, so an LRU beats holding all 1500-odd at 196 KB.
        """
        with self._lock:
            got = self._full_cache.get(texid)
            if got is not None:
                self._full_cache.move_to_end(texid)
                return got
        rgb = self._decode_full(texid)
        self._store_full(texid, rgb)
        return rgb

    def _prepare(self, texid):
        """Decode a tile once, and derive every mip zooms 5-8 draw from.

        The small sizes are 16 KB a tile together, so they live for the whole
        session; without them, each zoom level re-decoded every texture it
        touched. Single-flight per texid, because concurrent tile requests can
        want the same texture at the same moment.
        """
        while True:
            with self._lock:
                if texid in self._mips[SMALL_MIPS[-1]]:
                    return
                ev = self._preparing.get(texid)
                if ev is None:
                    ev = threading.Event()
                    self._preparing[texid] = ev
                    break
            ev.wait()

        try:
            rgb = self._decode_full(texid)
            small = {}
            cur = rgb.astype(np.float64)
            for size in sorted(SMALL_MIPS, reverse=True):    # 64 down to 4
                cur = _reduce(cur, size)
                small[size] = cur.astype(np.uint8)
            with self._lock:
                for size, arr in small.items():
                    self._mips[size][texid] = arr
            self._store_full(texid, rgb)
        finally:
            with self._lock:
                self._preparing.pop(texid, None)
            ev.set()

    def mip(self, texid, size):
        """One ground tile at `size` x `size`, 4..256."""
        if size >= TILE_PX:
            return self._full(texid)
        if size in SMALL_MIPS:
            self._prepare(texid)
            with self._lock:
                return self._mips[size][texid]
        # 128, for z=9: bounded, because panning keeps a screenful alive
        with self._lock:
            got = self._mid.get((texid, size))
            if got is not None:
                self._mid.move_to_end((texid, size))
                return got
        got = np.ascontiguousarray(_box(self._full(texid), size))
        with self._lock:
            self._mid[(texid, size)] = got
            self._mid.move_to_end((texid, size))
            while len(self._mid) > MID_CACHE:
                self._mid.popitem(last=False)
        return got

    def warm(self):
        """Decode the theater's whole ground library, off the request path.

        A tile at any zoom below native touches hundreds of distinct textures,
        so the first pan across a level otherwise pays a decode for every one
        of them. One parallel pass over the textures the grid actually uses
        turns the rest of the session into cache hits.
        """
        with self._lock:
            if self._warming:
                return
            self._warming = True

        def run():
            workers = max(1, min(8, (os.cpu_count() or 2) - 1))
            uniq = np.unique(self.grid())
            with ThreadPoolExecutor(workers,
                                    thread_name_prefix="terrain") as pool:
                try:
                    for _ in pool.map(self._prepare, uniq.tolist()):
                        pass
                except Exception:
                    # A malformed DDS should not stop the pass; the request
                    # path still reports it if that texture is ever drawn.
                    pass

        threading.Thread(target=run, name="terrain-warm", daemon=True).start()

    # -- the whole-theater overview -------------------------------------------

    OVERVIEW_PX_PER_KM = 4

    def overview(self):
        """The whole theater at 4 px/km, built once and cached on disk.

        Low zooms would otherwise have to decode a million tiles per request.
        """
        if self._overview is not None:
            return self._overview
        with self._overview_lock:
            if self._overview is not None:
                return self._overview
            path = os.path.join(self.cache_dir, "overview.npy")
            if os.path.isfile(path):
                try:
                    self._overview = np.load(path)
                    return self._overview
                except (OSError, ValueError):
                    pass

            g = self.grid()
            k = self.OVERVIEW_PX_PER_KM
            uniq = np.unique(g)
            lut = np.zeros((int(uniq.max()) + 1, k, k, 3), np.uint8)
            for v in uniq:
                lut[int(v)] = self.mip(int(v), k)
            # (ky, kx, k, k, 3) -> (ky*k, kx*k, 3)
            ov = lut[g].transpose(0, 2, 1, 3, 4).reshape(g.shape[0] * k,
                                                         g.shape[1] * k, 3)
            self._overview = np.ascontiguousarray(ov)
            try:
                os.makedirs(self.cache_dir, exist_ok=True)
                np.save(path, self._overview)
            except OSError:
                pass
            return self._overview

    # -- serving --------------------------------------------------------------

    def zoom_range(self):
        return 0, Z_NATIVE

    def render(self, z, tx, ty):
        """One 256x256 map tile. `ty` counts from the north, like a web map."""
        z = max(0, min(Z_NATIVE, int(z)))
        span = 1 << z
        if not (0 <= tx < span and 0 <= ty < span):
            return None
        km = self.size_km >> z                     # km covered by this tile
        x0 = tx * km
        # The grid's row 0 is the south edge, so a north-counting tile row has
        # to be flipped before it indexes the array.
        y1 = self.size_km - ty * km
        y0 = y1 - km

        if km * self.OVERVIEW_PX_PER_KM >= TILE_PX:
            ov = self.overview()
            k = self.OVERVIEW_PX_PER_KM
            crop = ov[y0 * k:y1 * k, x0 * k:(x0 + km) * k]
            return _box(np.ascontiguousarray(crop[::-1]), TILE_PX)

        px = TILE_PX // km                          # pixels per km tile
        g = self.grid()
        # Row 0 of the output is the north edge, so walk the grid backwards.
        cells = g[y0:y1, x0:x0 + km][::-1]
        # Assemble via a lookup table per distinct texture rather than a
        # Python loop over the km x km cells; same trick as the overview.
        uniq, inv = np.unique(cells, return_inverse=True)
        lut = np.stack([self.mip(int(v), px) for v in uniq])
        return (lut[inv.reshape(cells.shape)]
                   .transpose(0, 2, 1, 3, 4)
                   .reshape(km * px, km * px, 3))

    def render_png(self, z, tx, ty):
        arr = self.render(z, tx, ty)
        return None if arr is None else write_png(arr)
