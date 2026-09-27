#!/usr/bin/env python3
"""Build JSGME mods that swap a BMS theater's terrain tiles into FreeFalcon.

Written for BMS 4.32 Korea, scanned 2026-09-26 (see the terrain-tiles memory / WIP notes):

  * BMS's tiles are a strict superset of FF6's by name, and its texture.bin is FF6's set list in the
    same order plus appended sets -- so FF6's terrain map still indexes the right tiles.
  * FF's tile loader (graphics/texture/terrtex.cpp) tags EVERY tile as DXT1 ("MUST BE DXT1"). BMS
    ships some tiles as DXT5/DXT3; copied as-is they would render as garbage. They are converted here,
    losslessly for colour: a DXT3/DXT5 block is 8 bytes of alpha followed by a DXT1-style colour block,
    and terrain has no alpha. The one trap is that DXT1 treats a block whose endpoints are c0 <= c1 as
    3-colour + transparent, while DXT3/5 always read it as 4-colour. Such blocks get their endpoints
    swapped and indices remapped (0<->1, 2<->3), or -- when c0 == c1, a flat block -- all indices set
    to 0. Every mip level is converted, and the header's linear size is the DXT1 top level.
  * Mipmaps and 128/1024 sizes load fine as they are, so everything else is copied untouched.

Two mods, because the far tiles are a separate risk (BMS regenerated all of them and the count
changed, so their order against FF's terrain far-tile ids is unverified):

    MODS/<name> Tiles/terrdata/<theater>/texture/texture.bin + texture/*.dds
    MODS/<name> FarTiles/terrdata/<theater>/texture/farTiles.dds + FArtILES.PAL

Usage:
    python bms_tiles_mod.py --bms "L:/Falcon BMS 4.32/Data/Terrdata/korea/texture"
                            --ff C:/FreeFalcon6 --theater korea --name "BMS 4.32 Korea"

BMS 4.34+ (the "Polak" Korea) reorganised the set list: its Texture_Polak.bin shares only 9 of FF6's 113
sets by position, so it would scramble FF's terrain map. Its tile FILES still carry every FF6 name, drawn to
the same shapes (coastlines, towns, rivers line up against FF's map), so for those builds keep FF's own
texture.bin and swap the images only, skipping blank placeholders in favour of a fallback folder:

    python bms_tiles_mod.py --bms "C:/Falcon BMS 4.37/Data/TerrData/Korea/Texture"
                            --tiles-dir Texture_Polak --keep-bin --no-far
                            --fallback "C:/FreeFalcon6/MODS/BMS 4.32 Korea Tiles/terrdata/korea/texture/texture"
                            --ff C:/FreeFalcon6 --theater korea --name "BMS 4.37 Korea"
"""

import argparse
import os
import shutil
import struct
import sys

import numpy as np

DDS_MAGIC = b"DDS "


def dxt_blocks(w, h):
    return max(1, (w + 3) // 4) * max(1, (h + 3) // 4)


def colour_blocks_to_dxt1(blocks16):
    """(N,16) DXT3/DXT5 blocks -> (N,8) DXT1 blocks with identical colours."""
    col = blocks16[:, 8:16].copy()
    c0 = col[:, 0].astype(np.uint16) | (col[:, 1].astype(np.uint16) << 8)
    c1 = col[:, 2].astype(np.uint16) | (col[:, 3].astype(np.uint16) << 8)
    idx = (col[:, 4].astype(np.uint32) | (col[:, 5].astype(np.uint32) << 8) |
           (col[:, 6].astype(np.uint32) << 16) | (col[:, 7].astype(np.uint32) << 24))

    swap = c0 < c1
    flat = c0 == c1

    # swap endpoints; remap each 2-bit index 0<->1, 2<->3 (i.e. flip the low bit)
    n0 = np.where(swap, c1, c0)
    n1 = np.where(swap, c0, c1)
    idx = np.where(swap, idx ^ np.uint32(0x55555555), idx)
    # flat block: every palette entry is the same colour -- point all texels at c0 (4-colour needs c0 > c1,
    # which cannot hold when they are equal, so index 0 is the only safe choice)
    idx = np.where(flat, np.uint32(0), idx)

    out = np.empty((len(col), 8), dtype=np.uint8)
    out[:, 0] = n0 & 0xFF
    out[:, 1] = n0 >> 8
    out[:, 2] = n1 & 0xFF
    out[:, 3] = n1 >> 8
    out[:, 4] = idx & 0xFF
    out[:, 5] = (idx >> 8) & 0xFF
    out[:, 6] = (idx >> 16) & 0xFF
    out[:, 7] = idx >> 24
    return out


def convert_to_dxt1(data):
    """Return (bytes, action) for one DDS file: action is 'copy' or 'DXT5->DXT1' etc."""
    if data[:4] != DDS_MAGIC:
        return data, "copy (no DDS magic)"
    four = data[84:88]
    if four not in (b"DXT3", b"DXT5"):
        return data, "copy"
    h, w = struct.unpack_from("<II", data, 12)
    mips = max(1, struct.unpack_from("<I", data, 28)[0])
    hdr = bytearray(data[:128])
    hdr[84:88] = b"DXT1"
    struct.pack_into("<I", hdr, 20, dxt_blocks(w, h) * 8)  # linear size = DXT1 top level

    out = [bytes(hdr)]
    o = 128
    lw, lh = w, h
    for _ in range(mips):
        n = dxt_blocks(lw, lh)
        chunk = data[o:o + n * 16]
        if len(chunk) < n * 16:
            break  # truncated chain: keep the levels we have
        blocks = np.frombuffer(chunk, dtype=np.uint8).reshape(n, 16)
        out.append(colour_blocks_to_dxt1(blocks).tobytes())
        o += n * 16
        lw, lh = max(1, lw // 2), max(1, lh // 2)
    return b"".join(out), "%s->DXT1" % four.decode()


def is_blank(dxt1):
    """True for a flat or all-white DXT1 tile (4.37 ships one white placeholder, HFARMD99)."""
    h, w = struct.unpack_from("<II", dxt1, 12)
    o, lw, lh = 128, w, h
    while lw > 16 and lh > 16:           # sample a small mip: cheap and enough
        o += dxt_blocks(lw, lh) * 8
        lw, lh = lw // 2, lh // 2
    blocks = np.frombuffer(dxt1[o:o + dxt_blocks(lw, lh) * 8], dtype=np.uint8).reshape(-1, 8)
    ends = np.concatenate([blocks[:, 0:2], blocks[:, 2:4]]).view("<u2").ravel()
    r, g, b = (ends >> 11) & 31, (ends >> 5) & 63, ends & 31
    # near-white on average (the placeholder is white with a faint grid), or one flat colour
    white = r.mean() / 31 > 0.92 and g.mean() / 63 > 0.92 and b.mean() / 31 > 0.92
    return bool(white or len(np.unique(ends)) <= 1)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bms", required=True, help="BMS .../Terrdata/<theater>/texture folder")
    ap.add_argument("--ff", required=True, help="FreeFalcon install root (holds MODS/)")
    ap.add_argument("--theater", default="korea")
    ap.add_argument("--name", required=True, help='mod name prefix, e.g. "BMS 4.32 Korea"')
    ap.add_argument("--tiles-dir", default="texture", help="tile folder under --bms (4.37: Texture_Polak)")
    ap.add_argument("--keep-bin", action="store_true",
                    help="keep FF's texture.bin and copy only tiles FF already has (for reordered BMS set lists)")
    ap.add_argument("--fallback", help="folder to take a tile from when the BMS one is a blank placeholder")
    ap.add_argument("--no-far", action="store_true", help="do not build the FarTiles mod")
    args = ap.parse_args(argv)

    rel = os.path.join("terrdata", args.theater, "texture")
    ff_tex = os.path.join(args.ff, rel)
    mods = os.path.join(args.ff, "MODS")

    # FF's own file names, so the mod overwrites them with the same spelling
    ff_names = {n.lower(): n for n in os.listdir(ff_tex)}
    ff_tiles = {n.lower(): n for n in os.listdir(os.path.join(ff_tex, "texture"))}

    # --- mod 1: tiles + texture.bin
    tiles_root = os.path.join(mods, args.name + " Tiles", rel)
    os.makedirs(os.path.join(tiles_root, "texture"), exist_ok=True)
    if not args.keep_bin:
        shutil.copy2(os.path.join(args.bms, "texture.bin"),
                     os.path.join(tiles_root, ff_names.get("texture.bin", "texture.bin")))
    counts = {}
    src_dir = os.path.join(args.bms, args.tiles_dir)
    fallback = {n.lower(): n for n in os.listdir(args.fallback)} if args.fallback else {}
    for n in sorted(os.listdir(src_dir)):
        if not n.lower().endswith(".dds"):
            continue
        if args.keep_bin and n.lower() not in ff_tiles:
            continue  # FF's texture.bin never names it
        data = open(os.path.join(src_dir, n), "rb").read()
        out, action = convert_to_dxt1(data)
        if out[84:88] == b"DXT1" and is_blank(out):
            if n.lower() in fallback:
                out, action = convert_to_dxt1(open(os.path.join(args.fallback, fallback[n.lower()]), "rb").read())
                action = "blank -> fallback " + action
                print("   blank placeholder, from fallback:", n)
            else:
                ff_src = os.path.join(ff_tex, "texture", ff_tiles[n.lower()]) if n.lower() in ff_tiles else None
                if ff_src:
                    out, action = open(ff_src, "rb").read(), "blank -> FF's own"
                    print("   blank placeholder, kept FF's:", n)
        counts[action] = counts.get(action, 0) + 1
        with open(os.path.join(tiles_root, "texture", ff_tiles.get(n.lower(), n)), "wb") as fh:
            fh.write(out)
    print("tiles mod:", tiles_root)
    for k, v in sorted(counts.items()):
        print("   %-20s %d" % (k, v))

    if args.no_far:
        return 0

    # --- mod 2: far tiles
    far_root = os.path.join(mods, args.name + " FarTiles", rel)
    os.makedirs(far_root, exist_ok=True)
    for bms_name, key in (("fartiles.dds", "fartiles.dds"), ("fartiles.pal", "fartiles.pal")):
        src = next((os.path.join(args.bms, n) for n in os.listdir(args.bms) if n.lower() == bms_name), None)
        if src:
            shutil.copy2(src, os.path.join(far_root, ff_names.get(key, bms_name)))
    print("far tiles mod:", far_root, sorted(os.listdir(far_root)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
