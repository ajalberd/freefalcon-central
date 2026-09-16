#!/usr/bin/env python3
"""Survey a theater's terrain posts and the ground tiles their texIDs name.

Answers, without a build and without launching the game, the questions that decide
whether the campaign map can be drawn from ground imagery instead of post colours:

  * what the post grid is (spacing, extent, TdiskPost vs TNewdiskPost)
  * how much ground one texture tile covers, and at how many feet per pixel
  * how many distinct tiles the theater actually references
  * how much is lost by quantising tile pixels into the theater's own 256-entry
    ColorTable -- i.e. whether the map can stay 8-bit paletted, which is what
    C_ScaleBitmap's blended overlay palettes require

Everything is read straight off the shipped files, the same way BuildTerrainMapImage
(src/ui/src/campaign/cmap.cpp) reads them. Nothing here links against the game.

Usage:
    python tilesurvey.py [--theater DIR] [--sample N] [--compare BC,BR] [--out DIR]

--theater defaults to the theaterDir value under
HKLM\\SOFTWARE\\WOW6432Node\\MicroProse\\Falcon\\4.1 on Windows.
--sample limits the texID scan to every Nth block (the full scan reads the whole
Theater.l0, ~100 MB for Korea).
--compare renders a side-by-side PNG of post colours vs tile imagery for the 4x4
block region starting at block column,row -- the picture of what the change buys.

Needs numpy. No other third-party packages: the PNG writer is stdlib zlib.
"""

import argparse
import collections
import os
import struct
import sys
import zlib

import numpy as np

POST_OFFSET_BITS = 4
POSTS_ACROSS_BLOCK = 1 << POST_OFFSET_BITS          # 16
POSTS_PER_BLOCK = 1 << (POST_OFFSET_BITS << 1)      # 256
POSTS_ACROSS_TILE = 4        # DiskblockToMemblock puts 4 post-quads across a tile at LOD 0
FEET_PER_NM = 6076.115

TMAP_LARGETERRAIN = 0x1
TMAP_LARGEUIMAP = 0x2

COVERAGE = {
    0: "NODATA", 1: "WATER", 2: "RIVER", 3: "SWAMP", 4: "PLAINS", 5: "BRUSH",
    6: "THINFOREST", 7: "THICKFOREST", 8: "ROCKY", 9: "URBAN", 10: "ROAD",
    11: "RAIL", 12: "BRIDGE", 13: "RUNWAY", 14: "STATION", 15: "OBJECT",
}


# --------------------------------------------------------------------------- paths

def default_theater():
    if sys.platform != "win32":
        return None
    try:
        import winreg
        key = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                             r"SOFTWARE\WOW6432Node\MicroProse\Falcon\4.1")
        return winreg.QueryValueEx(key, "theaterDir")[0]
    except OSError:
        return None


def find(dirpath, name):
    """Case-insensitive lookup -- the shipped data is upper case, the code is not."""
    if os.path.exists(os.path.join(dirpath, name)):
        return os.path.join(dirpath, name)
    low = name.lower()
    for f in os.listdir(dirpath):
        if f.lower() == low:
            return os.path.join(dirpath, f)
    raise FileNotFoundError(os.path.join(dirpath, name))


# ------------------------------------------------------------------- Theater.map

class TheaterMap:
    """Theater.map, in the order TMap::Setup reads it."""

    def __init__(self, path):
        d = open(path, "rb").read()
        o = 0
        (self.feet_per_post,) = struct.unpack_from("<f", d, o); o += 4
        self.mea_w, self.mea_h = struct.unpack_from("<ii", d, o); o += 8
        (self.ft_to_mea,) = struct.unpack_from("<f", d, o); o += 4
        (self.nlevels,) = struct.unpack_from("<i", d, o); o += 4
        self.last_near_tex, self.last_far_tex = struct.unpack_from("<ii", d, o); o += 8

        packed = np.frombuffer(d[o:o + 1024], dtype="<u4"); o += 1024
        self.color_table = np.stack([packed & 0xFF,
                                     (packed >> 8) & 0xFF,
                                     (packed >> 16) & 0xFF], -1).astype(np.int32)

        self.levels = []
        for _ in range(self.nlevels):
            self.levels.append(struct.unpack_from("<ii", d, o)); o += 8

        (self.flags,) = struct.unpack_from("<i", d, o); o += 4
        self.longitude, self.latitude = struct.unpack_from("<ff", d, o); o += 8
        self.trailing = len(d) - o

    @property
    def large_terrain(self):
        return bool(self.flags & TMAP_LARGETERRAIN)

    @property
    def post_size(self):
        """sizeof(TNewdiskPost) or sizeof(TdiskPost), both #pragma pack(1)."""
        return 9 if self.large_terrain else 7

    def report(self):
        print("=== Theater.map ===")
        print(f"  FeetPerPost        {self.feet_per_post:.3f}")
        print(f"  levels             {self.nlevels}"
              f"   lastNearTexLOD {self.last_near_tex}"
              f"   lastFarTexLOD {self.last_far_tex}")
        print(f"  flags              0x{self.flags:08x}"
              f"  (LARGETERRAIN {self.large_terrain},"
              f" LARGEUIMAP {bool(self.flags & TMAP_LARGEUIMAP)})")
        print(f"  post on disk       {'TNewdiskPost' if self.large_terrain else 'TdiskPost'}"
              f", {self.post_size} bytes")
        print(f"  lon/lat            {self.longitude} {self.latitude}")
        for i, (w, h) in enumerate(self.levels):
            ft = self.feet_per_post * (1 << i)
            print(f"    LOD {i}: {w:4d}x{h:<4d} blocks"
                  f" = {w * POSTS_ACROSS_BLOCK:5d}x{h * POSTS_ACROSS_BLOCK:<5d} posts"
                  f"  {ft:8.1f} ft/post"
                  f"  {w * POSTS_ACROSS_BLOCK * ft / FEET_PER_NM:.0f} nm across")


# ------------------------------------------------------------------- texture.bin

def load_tile_table(texture_dir):
    """texture.bin, in the order TextureDB::Setup reads it.

    Returns ([(terrainType, [filename, ...]), ...], totalTiles).
    """
    d = open(find(texture_dir, "texture.bin"), "rb").read()
    o = 0
    num_sets, total_tiles = struct.unpack_from("<ii", d, o); o += 8
    sets = []
    for _ in range(num_sets):
        (n_tiles,) = struct.unpack_from("<i", d, o); o += 4
        terrain_type = d[o]; o += 1
        tiles = []
        for _ in range(n_tiles):
            fn = d[o:o + 20].split(b"\0")[0].decode("latin1"); o += 20
            n_areas, n_paths = struct.unpack_from("<ii", d, o); o += 8
            o += n_areas * 16 + n_paths * 24      # TexArea / TexPath
            tiles.append(fn)
        sets.append((terrain_type, tiles))
    if o != len(d):
        print(f"  WARNING: texture.bin parse consumed {o} of {len(d)} bytes")
    return sets, total_tiles


# -------------------------------------------------------------------------- DXT1

def decode_dxt1(data, w, h):
    """Raw DXT1 surface bytes -> (h, w, 3) uint8 RGB."""
    bw, bh = w // 4, h // 4
    blocks = np.frombuffer(data[:bw * bh * 8], dtype=np.uint8).reshape(bh, bw, 8)
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
    pal = np.stack([a, b, c2, c3], axis=2)                      # (bh, bw, 4, 3)

    words = (blocks[:, :, 4].astype(np.uint32)
             | (blocks[:, :, 5].astype(np.uint32) << 8)
             | (blocks[:, :, 6].astype(np.uint32) << 16)
             | (blocks[:, :, 7].astype(np.uint32) << 24))
    shifts = (np.arange(16, dtype=np.uint32) * 2).reshape(4, 4)
    idx = (words[:, :, None, None] >> shifts) & 3               # (bh, bw, 4, 4)

    out = np.take_along_axis(pal[:, :, None, None, :, :],
                             idx[..., None, None], axis=4)[..., 0, :]
    return out.transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 3).astype(np.uint8)


def read_dds(path):
    """-> (rgb, w, h, fourcc). rgb is None for anything that is not the DXT1 the tiles use."""
    with open(path, "rb") as f:
        head = f.read(128)
        if head[:4] != b"DDS ":
            raise ValueError(f"{path}: not a DDS")
        h, w = struct.unpack_from("<2I", head, 12)
        fourcc = head[84:88].decode("latin1")
        if fourcc != "DXT1":
            return None, w, h, fourcc
        return decode_dxt1(f.read(), w, h), w, h, fourcc


# ------------------------------------------------------------------ quantisation

def build_lut(color_table):
    """RGB555 -> nearest ColorTable index. 32 KB, built once; one lookup per pixel."""
    grid = np.arange(32, dtype=np.int32) * 255 // 31
    r, g, b = np.meshgrid(grid, grid, grid, indexing="ij")
    cube = np.stack([r.ravel(), g.ravel(), b.ravel()], -1)
    d2 = ((cube[:, None, :] - color_table[None, :, :]) ** 2).sum(-1)
    return d2.argmin(1).astype(np.uint8), np.sqrt(d2.min(1))


def quantise(rgb, lut):
    q = rgb.astype(np.uint32) >> 3
    return lut[(q[..., 0] << 10) | (q[..., 1] << 5) | q[..., 2]]


# ------------------------------------------------------------------- theater data

class Theater:
    def __init__(self, root, lod=0):
        self.root = root
        self.lod = lod
        self.terrain_dir = find(root, "terrain")
        self.texture_dir = find(root, "texture")
        self.tile_dir = find(self.texture_dir, "texture")
        self.map = TheaterMap(find(self.terrain_dir, "Theater.map"))
        self.bw, self.bh = self.map.levels[lod]
        self.offsets = np.frombuffer(
            open(find(self.terrain_dir, f"Theater.o{lod}"), "rb").read(), dtype="<u4")
        self.posts = np.memmap(find(self.terrain_dir, f"Theater.l{lod}"),
                               dtype=np.uint8, mode="r")
        self.sets, self.total_tiles = load_tile_table(self.texture_dir)
        self.lut, self.luterr = build_lut(self.map.color_table)
        self._tiles = {}

    @property
    def feet_per_post(self):
        return self.map.feet_per_post * (1 << self.lod)

    def block(self, bc, br):
        """-> (texID, colour) as two (16, 16) arrays."""
        psz = self.map.post_size
        o = int(self.offsets[br * self.bw + bc])
        buf = np.array(self.posts[o:o + psz * POSTS_PER_BLOCK]).reshape(POSTS_PER_BLOCK, psz)
        if self.map.large_terrain:
            tid = (buf[:, 0].astype(np.uint32) | (buf[:, 1].astype(np.uint32) << 8)
                   | (buf[:, 2].astype(np.uint32) << 16) | (buf[:, 3].astype(np.uint32) << 24))
            col = buf[:, 4]
        else:
            tid = (buf[:, 0].astype(np.uint32) | (buf[:, 1].astype(np.uint32) << 8))
            col = buf[:, 4]
        return (tid.reshape(POSTS_ACROSS_BLOCK, POSTS_ACROSS_BLOCK),
                col.reshape(POSTS_ACROSS_BLOCK, POSTS_ACROSS_BLOCK))

    def tile_name(self, texid):
        s, t = (texid >> 4) & 0xFF, texid & 0xF
        if s >= len(self.sets) or t >= len(self.sets[s][1]):
            return None
        return self.sets[s][1][t]

    def tile_path(self, texid):
        fn = self.tile_name(texid)
        return None if fn is None else os.path.join(self.tile_dir, fn.split(".")[0] + ".dds")

    def tile_indexed(self, texid):
        """Decoded and quantised into the theater ColorTable -> (n, n) uint8 indices."""
        if texid not in self._tiles:
            rgb, _w, _h, fourcc = read_dds(self.tile_path(texid))
            if rgb is None:
                raise ValueError(f"tile 0x{texid:x} is {fourcc}, not DXT1")
            self._tiles[texid] = quantise(rgb, self.lut)
        return self._tiles[texid]


# ---------------------------------------------------------------------- reporting

def report_tiles(th):
    print("\n=== texture.bin ===")
    print(f"  sets {len(th.sets)}   tiles {th.total_tiles}")
    types = collections.Counter(COVERAGE.get(t, "?") for t, _ in th.sets)
    print("  set coverage types:", dict(types.most_common()))

    dims, missing, fourccs = collections.Counter(), [], collections.Counter()
    bytes8 = 0
    for _tt, tiles in th.sets:
        for fn in tiles:
            p = os.path.join(th.tile_dir, fn.split(".")[0] + ".dds")
            if not os.path.exists(p):
                missing.append(fn)
                continue
            with open(p, "rb") as f:
                head = f.read(128)
            h, w = struct.unpack_from("<2I", head, 12)
            (mips,) = struct.unpack_from("<I", head, 28)
            dims[(w, h, mips)] += 1
            fourccs[head[84:88].decode("latin1")] += 1
            bytes8 += w * h

    print("\n=== tile images ===")
    print(f"  present {sum(dims.values())}   missing {len(missing)}"
          + (f"  {missing[:4]}" if missing else ""))
    print("  formats:", dict(fourccs))
    ground_ft = th.feet_per_post * POSTS_ACROSS_TILE
    print(f"  one tile covers {POSTS_ACROSS_TILE}x{POSTS_ACROSS_TILE} posts"
          f" = {ground_ft:.0f} ft ({ground_ft / FEET_PER_NM:.2f} nm) of ground")
    for (w, h, mips), n in sorted(dims.items()):
        print(f"    {w:4d}x{h:<4d} mips {mips}  x{n:<5d}"
              f"  -> {ground_ft / w:5.1f} ft/pixel"
              f"  ({th.feet_per_post / (ground_ft / w):.0f}x the post map)")
    print(f"  whole library decoded to 8-bit indices: {bytes8 / 1e6:.0f} MB")


def report_posts(th, sample):
    print(f"\n=== post scan (LOD {th.lod}, every {sample} blocks) ===")
    nb = th.bw * th.bh
    print(f"  blocks {th.bw}x{th.bh} = {nb}"
          f"   distinct offsets {len(np.unique(th.offsets))}"
          f"   (identical blocks are shared)")

    tex_hist = collections.Counter()
    bad = 0
    group_same = group_tot = 0
    for br in range(0, th.bh, sample):
        for bc in range(0, th.bw, sample):
            tid, _col = th.block(bc, br)
            for v, n in zip(*np.unique(tid, return_counts=True)):
                tex_hist[int(v)] += int(n)
                if th.tile_name(int(v)) is None:
                    bad += int(n)
            step = POSTS_ACROSS_TILE
            for gr in range(POSTS_ACROSS_BLOCK // step):
                for gc in range(POSTS_ACROSS_BLOCK // step):
                    cell = tid[gr * step:(gr + 1) * step, gc * step:(gc + 1) * step]
                    group_tot += 1
                    group_same += int(len(np.unique(cell)) == 1)

    total = sum(tex_hist.values())
    print(f"  posts sampled            {total}")
    print(f"  texIDs with no tile      {bad}")
    print(f"  distinct texIDs          {len(tex_hist)}")
    print(f"  res fields               {sorted({(t >> 12) & 0xF for t in tex_hist})}"
          f"   (0=L 1=M 2=H)")
    print(f"  bits above 16            {sorted({t >> 16 for t in tex_hist})}")
    print(f"  {POSTS_ACROSS_TILE}x{POSTS_ACROSS_TILE} post groups sharing one texID:"
          f" {group_same}/{group_tot}"
          f" ({100.0 * group_same / max(group_tot, 1):.1f}%)")

    order = sorted(tex_hist.items(), key=lambda kv: -kv[1])
    print("  most common tiles:")
    for tid, n in order[:6]:
        print(f"    0x{tid:04x} set {(tid >> 4) & 0xFF:3d} tile {tid & 0xF:2d}"
              f"  {str(th.tile_name(tid)):<14s} {100.0 * n / total:5.2f}%")
    for k in (50, 200):
        if len(order) > k:
            share = sum(n for _t, n in order[:k]) / total
            print(f"  top {k:3d} tiles cover {100.0 * share:.1f}% of the sampled ground")
    return tex_hist


def report_quantisation(th, texids):
    """How much is lost by forcing tile pixels into the theater's own ColorTable."""
    print("\n=== quantising tile pixels into the theater ColorTable ===")
    tot = 0
    err_sum = 0.0
    err_max = 0.0
    hist = np.zeros(512, np.int64)
    used = 0
    for texid in texids:
        p = th.tile_path(texid)
        if not p or not os.path.exists(p):
            continue
        rgb, _w, _h, _fourcc = read_dds(p)
        if rgb is None:
            continue
        q = rgb.astype(np.uint32) >> 3
        e = th.luterr[(q[..., 0] << 10) | (q[..., 1] << 5) | q[..., 2]].ravel()
        err_sum += e.sum()
        err_max = max(err_max, float(e.max()))
        tot += e.size
        used += 1
        hist += np.bincount(np.minimum(e.astype(int), 511), minlength=512)

    if not tot:
        print("  no tiles read")
        return
    cum = np.cumsum(hist) / tot
    pct = lambda p: int(np.searchsorted(cum, p))
    print(f"  tiles {used}   pixels {tot / 1e6:.1f} M")
    print("  RGB euclidean error (0..441, 441 = black vs white)")
    print(f"    mean {err_sum / tot:5.1f}   median {pct(.5)}"
          f"   p90 {pct(.9)}   p99 {pct(.99)}   max {err_max:.0f}")
    print(f"    within  8: {100 * cum[8]:5.1f}%"
          f"    within 16: {100 * cum[16]:5.1f}%"
          f"    within 32: {100 * cum[32]:5.1f}%")


# -------------------------------------------------------------------- comparison

def write_png(path, rgb):
    h, w, _ = rgb.shape
    raw = b"".join(b"\0" + rgb[y].astype(np.uint8).tobytes() for y in range(h))

    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    open(path, "wb").write(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 6))
        + chunk(b"IEND", b""))


def render_compare(th, bc, br, out_dir, nblocks=4, out_px=1024):
    """Side by side: the map we draw today, and the same ground from tile imagery."""
    nposts = nblocks * POSTS_ACROSS_BLOCK
    cells_per_block = POSTS_ACROSS_BLOCK // POSTS_ACROSS_TILE
    ncell = nposts // POSTS_ACROSS_TILE
    post_col = np.zeros((nposts, nposts), np.uint8)
    cell_tid = np.zeros((ncell, ncell), np.uint32)
    for j in range(nblocks):
        for i in range(nblocks):
            tid, col = th.block(bc + i, br + j)
            post_col[j * POSTS_ACROSS_BLOCK:(j + 1) * POSTS_ACROSS_BLOCK,
                     i * POSTS_ACROSS_BLOCK:(i + 1) * POSTS_ACROSS_BLOCK] = col
            cell_tid[j * cells_per_block:(j + 1) * cells_per_block,
                     i * cells_per_block:(i + 1) * cells_per_block] \
                = tid[::POSTS_ACROSS_TILE, ::POSTS_ACROSS_TILE]

    across_nm = nposts * th.feet_per_post / FEET_PER_NM
    print(f"\n=== comparison render: blocks ({bc},{br}) +{nblocks} ===")
    print(f"  {nposts}x{nposts} posts = {across_nm:.1f} nm across,"
          f" {len(np.unique(cell_tid))} distinct tiles")

    ct = th.map.color_table
    rep = out_px // nposts
    left = np.repeat(np.repeat(ct[post_col].astype(np.uint8), rep, 0), rep, 1)

    ts = min(th.tile_indexed(int(t)).shape[0] for t in np.unique(cell_tid))
    big = np.zeros((ncell * ts, ncell * ts), np.uint8)
    for r in range(ncell):
        for c in range(ncell):
            t = th.tile_indexed(int(cell_tid[r, c]))
            if t.shape[0] != ts:
                t = t[::t.shape[0] // ts, ::t.shape[0] // ts]
            big[r * ts:(r + 1) * ts, c * ts:(c + 1) * ts] = t
    f = big.shape[0] // out_px
    right = (ct[big].astype(np.float64)
             .reshape(out_px, f, out_px, f, 3).mean((1, 3)).astype(np.uint8))

    gap = np.full((out_px, 12, 3), 40, np.uint8)
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, f"zoom-compare-{bc}-{br}.png")
    write_png(path, np.concatenate([left, gap, right], axis=1))
    print(f"  left: post colours ({th.feet_per_post:.0f} ft/pixel)"
          f"   right: tile imagery ({th.feet_per_post * POSTS_ACROSS_TILE / ts:.1f} ft/pixel)")
    print(f"  wrote {path}")


# --------------------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--theater", default=default_theater(),
                    help="theater data directory (holding terrain/ and texture/)")
    ap.add_argument("--lod", type=int, default=0)
    ap.add_argument("--sample", type=int, default=8,
                    help="scan every Nth block; 1 reads the whole level (default 8)")
    ap.add_argument("--quantise", type=int, default=64, metavar="N",
                    help="measure ColorTable quantisation over the N most used tiles"
                         " (0 to skip, -1 for every tile)")
    ap.add_argument("--compare", metavar="BC,BR",
                    help="render a post-colour vs tile-imagery PNG at this block")
    ap.add_argument("--out", default=".", help="where --compare writes its PNG")
    args = ap.parse_args()

    if not args.theater:
        ap.error("no theater directory: pass --theater")
    print(f"theater: {args.theater}\n")

    th = Theater(args.theater, args.lod)
    th.map.report()
    report_tiles(th)
    hist = report_posts(th, max(1, args.sample))

    if args.quantise:
        order = [t for t, _n in sorted(hist.items(), key=lambda kv: -kv[1])]
        if args.quantise > 0:
            order = order[:args.quantise]
        report_quantisation(th, order)

    if args.compare:
        bc, br = (int(v) for v in args.compare.split(","))
        render_compare(th, bc, br, args.out)


if __name__ == "__main__":
    main()
