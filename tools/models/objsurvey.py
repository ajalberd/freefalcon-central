#!/usr/bin/env python3
"""Read a theater's 3D object database offline -- no build, no game.

Parses <basename>.DXH (the object header: colour/palette/texture banks, the LOD
table, the parent-object table) and, on request, the model records inside
<basename>.DXL.  Companion to tools/terrain/tilesurvey.py: re-run this instead
of re-deriving any object-database number by reading the loaders.

The on-disk layouts below are the 32-bit ones.  They are what the file actually
contains regardless of the build's word size -- see the "x64 serialization fix"
comments in src/graphics/bsplib/{palbank,texbank,objectparent}.cpp.

Usage:
    python objsurvey.py <KoreaObj-basename> [--parent N] [--lod N] [--find STR]
"""

import argparse
import os
import struct
import sys

FORMAT_VERSION = 0x03087000
DXVER = 0xFEEF

# --- on-disk record sizes (bytes) -------------------------------------------
PCOLOR = 16           # float r,g,b,a
DISK_PALETTE = 1032   # DWORD paletteData[256] + UInt32 palHandle + int refCount
DISK_TEXENTRY = 40    # int fileOffset,fileSize + DiskTexture(24) + int palID,refCount
LOD_HEADER = 20       # char spare[12] + UInt32 fileoffset + UInt32 filesize
PARENT_RECORD = 48    # see ParentFileRecord in graphics/include/objectparent.h
LOD_NAME = 32         # char LODName[32], present only when g_bUse_DX_Engine
DISK_LODREF = 8       # UInt32 objLOD offset + float maxRange

DXDBHEADER = struct.Struct(
    "<8I"      # Version Id VBClass ModelSize dwNVertices dwPoolSize pVPool dwNodesNr
    "8I"       # Scripts[2] = {ScriptType, DWORD Arguments[3]} each
    "3I"       # dwLightsNr pLightsPool dwTexNr
)
assert DXDBHEADER.size == 76

# --- .DXL node stream (graphics/dxengine/dxdefines.h) ------------------------
# Every node starts with DXNodeHeadType {dwNodeSize, dwNodeID, ItemType} and the
# stream is walked by adding dwNodeSize, exactly as CDXEngine::DrawModel does.
NODE_HEAD = struct.Struct("<3I")
ITEM_TYPES = {
    0: "ROOT", 1: "SURFACE", 2: "MATERIAL", 3: "TEXTURE", 4: "DOF",
    5: "ENDDOF", 6: "SLOT", 7: "SWITCH", 8: "LIGHT", 9: "MODELEND",
}
DOF_TYPES = {
    0: "NO_DOF", 1: "ROTATE", 2: "XROTATE", 3: "TRANSLATE", 4: "SCALE",
    5: "SWITCH", 6: "XSWITCH",
}
# DxSurfaceType after the 12-byte head.
SURFACE_BODY = struct.Struct("<IIIIIf2I2II")
# DxDofType after the 12-byte head: dwDOFTotalSize, Type, dofNumber/SwitchNumber,
# min, max, multiplier, future, flags/SwitchBranch, scale[3], rotation[16],
# translation[3].
DOF_BODY = struct.Struct("<II i 4f i 3f 16f 3f")
SURFACE_NODE_SIZE = NODE_HEAD.size + SURFACE_BODY.size  # indices follow this
# D3DVERTEXEX: vx,vy,vz, nx,ny,nz, dwColour, dwSpecular, tu,tv
VERTEX_STRIDE = 40
VERTEX = struct.Struct("<6f2I2f")
assert VERTEX.size == VERTEX_STRIDE
# DXLightType: Switch, SwitchMask, Argument, Flags, D3DLIGHT7
LIGHT_HEAD = struct.Struct("<4I")


class Reader:
    """Sequential reader over an in-memory buffer."""

    def __init__(self, data):
        self.data = data
        self.pos = 0

    def take(self, n):
        if self.pos + n > len(self.data):
            raise EOFError(f"want {n} bytes at {self.pos}, file has {len(self.data)}")
        chunk = self.data[self.pos:self.pos + n]
        self.pos += n
        return chunk

    def u32(self):
        return struct.unpack_from("<I", self.take(4))[0]

    def i32(self):
        return struct.unpack_from("<i", self.take(4))[0]


class Parent:
    __slots__ = ("index", "radius", "bbox", "radar_sign", "ir_sign",
                 "n_texture_sets", "n_dynamic_coords", "n_lods",
                 "n_switches", "n_dofs", "n_slots", "lods", "positions")

    def extent(self):
        (x0, x1, y0, y1, z0, z1) = self.bbox
        return (x1 - x0, y1 - y0, z1 - z0)


class Lod:
    __slots__ = ("index", "file_offset", "file_size", "name", "max_range")


def read_dxh(basename):
    """Parse <basename>.DXH.  Returns (lods, parents, meta)."""
    path = basename + ".DXH"
    if not os.path.exists(path):
        # The loader uppercases the extension; be forgiving about case on disk.
        for cand in (basename + ".dxh", basename + ".Dxh"):
            if os.path.exists(cand):
                path = cand
                break
    with open(path, "rb") as handle:
        r = Reader(handle.read())

    version = r.u32()
    if version != FORMAT_VERSION:
        raise SystemExit(
            f"{path}: object format 0x{version:08X}, expected 0x{FORMAT_VERSION:08X}")

    # --- colour bank ---
    n_colors = r.i32()
    n_darkened = r.i32()
    r.take(n_colors * PCOLOR)

    # --- palette bank ---
    n_palettes = r.i32()
    r.take(n_palettes * DISK_PALETTE)

    # --- texture bank ---
    n_textures = r.i32()
    max_compressed = 0
    if n_textures:
        max_compressed = r.i32()
        r.take(n_textures * DISK_TEXENTRY)
    # TextureBankClass::ReadPool stores maxCompressedSize in nVer; when it equals
    # DXver the parent records carry 16-bit nSwitches/nDOFs instead of 8-bit.
    new_counts = (max_compressed == DXVER)

    # --- LOD table ---
    max_tag_list = r.i32()
    n_lods = r.i32()
    lods = []
    for i in range(n_lods):
        lod = Lod()
        lod.index = i
        r.take(12)  # spare
        lod.file_offset = r.u32()
        lod.file_size = r.u32()
        lod.name = None
        lod.max_range = None
        lods.append(lod)

    # --- parent table ---
    n_parents = r.i32()
    parents = []
    for i in range(n_parents):
        rec = r.take(PARENT_RECORD)
        (radius, min_x, max_x, min_y, max_y, min_z, max_z,
         radar_sign, ir_sign) = struct.unpack_from("<9f", rec, 0)
        (n_texture_sets, n_dynamic_coords) = struct.unpack_from("<2h", rec, 36)
        (n_lods_b, n_switch_b, n_dof_b, n_slots_b) = struct.unpack_from("<4B", rec, 40)
        (n_switches_s, n_dofs_s) = struct.unpack_from("<2h", rec, 44)

        p = Parent()
        p.index = i
        p.radius = radius
        p.bbox = (min_x, max_x, min_y, max_y, min_z, max_z)
        p.radar_sign = radar_sign
        p.ir_sign = ir_sign
        p.n_texture_sets = n_texture_sets
        p.n_dynamic_coords = n_dynamic_coords
        p.n_lods = n_lods_b
        p.n_slots = n_slots_b
        p.n_switches = n_switches_s if new_counts else n_switch_b
        p.n_dofs = n_dofs_s if new_counts else n_dof_b
        p.lods = []
        p.positions = []
        parents.append(p)

    # --- per-parent reference arrays ---
    for p in parents:
        if p.n_lods == 0:
            continue
        n_pos = p.n_slots + p.n_dynamic_coords
        if n_pos:
            raw = r.take(n_pos * 12)
            p.positions = [struct.unpack_from("<3f", raw, k * 12) for k in range(n_pos)]
        for _ in range(p.n_lods):
            name = r.take(LOD_NAME).split(b"\0")[0].decode("latin-1")
            (offset, max_range) = struct.unpack("<If", r.take(DISK_LODREF))
            lod_index = offset >> 1  # the low bit is a marker, not data
            if 0 <= lod_index < len(lods):
                lods[lod_index].name = name
                lods[lod_index].max_range = max_range
            p.lods.append((lod_index, name, max_range))

    meta = {
        "path": path,
        "version": version,
        "n_colors": n_colors,
        "n_darkened": n_darkened,
        "n_palettes": n_palettes,
        "n_textures": n_textures,
        "max_tag_list": max_tag_list,
        "new_counts": new_counts,
        "trailing_bytes": len(r.data) - r.pos,
    }
    return lods, parents, meta


def read_model_header(basename, lod):
    """Read the DxDbHeader of one LOD record out of <basename>.DXL."""
    path = basename + ".DXL"
    if not os.path.exists(path):
        for cand in (basename + ".dxl", basename + ".Dxl"):
            if os.path.exists(cand):
                path = cand
                break
    with open(path, "rb") as handle:
        handle.seek(lod.file_offset)
        raw = handle.read(DXDBHEADER.size)
    if len(raw) < DXDBHEADER.size:
        return None
    f = DXDBHEADER.unpack(raw)
    return {
        "Version": f[0], "Id": f[1], "VBClass": f[2], "ModelSize": f[3],
        "dwNVertices": f[4], "dwPoolSize": f[5], "pVPool": f[6],
        "dwNodesNr": f[7],
        "Scripts": [(f[8], f[9:12]), (f[12], f[13:16])],
        "dwLightsNr": f[16], "pLightsPool": f[17], "dwTexNr": f[18],
    }


def read_model(basename, lod):
    """Read one LOD's whole record out of <basename>.DXL."""
    path = basename + ".DXL"
    if not os.path.exists(path):
        for cand in (basename + ".dxl", basename + ".Dxl"):
            if os.path.exists(cand):
                path = cand
                break
    with open(path, "rb") as handle:
        handle.seek(lod.file_offset)
        return handle.read(lod.file_size)


class Node:
    __slots__ = ("offset", "size", "node_id", "type", "depth", "body")


def walk_nodes(blob, header):
    """Yield the model's nodes in stream order, tracking DOF nesting depth.

    Mirrors CDXEngine::DrawModel: start at the node area, step by dwNodeSize,
    stop at DX_MODELEND.  Depth is bookkeeping for the reader only -- the engine
    itself jumps over a DOF subtree using dwDOFTotalSize.
    """
    start = DXDBHEADER.size + header["dwTexNr"] * 4
    pos = start
    depth = 0
    seen = 0
    limit = header["dwNodesNr"] + 16
    while pos + NODE_HEAD.size <= len(blob):
        (size, node_id, type_id) = NODE_HEAD.unpack_from(blob, pos)
        n = Node()
        n.offset = pos
        n.size = size
        n.node_id = node_id
        n.type = type_id
        n.body = None
        if type_id == 5:  # DX_ENDDOF closes a subtree
            depth = max(0, depth - 1)
        n.depth = depth
        if type_id == 1 and pos + SURFACE_NODE_SIZE <= len(blob):
            n.body = SURFACE_BODY.unpack_from(blob, pos + NODE_HEAD.size)
        elif type_id == 4 and pos + NODE_HEAD.size + DOF_BODY.size <= len(blob):
            n.body = DOF_BODY.unpack_from(blob, pos + NODE_HEAD.size)
        yield n
        if type_id == 4:
            depth += 1
        if type_id == 9:  # DX_MODELEND
            break
        seen += 1
        if size == 0 or seen > limit:
            break
        pos += size


def surface_bounds(blob, node, header):
    """Bounding box of one surface node's vertices, in model space.

    A surface node is followed by dwVCount 16-bit indices into the vertex pool
    (CDXEngine draws them from m_NODE.BYTE + sizeof(DxSurfaceType)).  Triangle
    lists routinely end in a degenerate triangle with a repeated index, whose
    vertices can sit anywhere in the model and are never rasterised -- include
    them and a 5 mm switch lever measures 2.6 units across.  Skip them.
    """
    count = node.body[1]
    prim_type = node.body[3]
    idx_at = node.offset + SURFACE_NODE_SIZE
    pool = header["pVPool"]

    indices = []
    for k in range(count):
        at = idx_at + k * 2
        if at + 2 > len(blob):
            break
        indices.append(struct.unpack_from("<H", blob, at)[0])

    if prim_type == 4:  # D3DPT_TRIANGLELIST
        keep = []
        for k in range(0, len(indices) - 2, 3):
            tri = indices[k:k + 3]
            if len(set(tri)) == 3:
                keep.extend(tri)
        indices = keep

    lo = [float("inf")] * 3
    hi = [float("-inf")] * 3
    used = 0
    for index in indices:
        vat = pool + index * VERTEX_STRIDE
        if vat + VERTEX_STRIDE > len(blob):
            continue
        v = VERTEX.unpack_from(blob, vat)
        for axis in range(3):
            lo[axis] = min(lo[axis], v[axis])
            hi[axis] = max(hi[axis], v[axis])
        used += 1
    if not used:
        return None
    return (lo, hi, used)


def dump_nodes(blob, header, args):
    counts = {}
    switch_nodes = {}   # switch number -> set of branches
    switch_surfaces = {}  # switch number -> list of (mask, node)
    for node in walk_nodes(blob, header):
        name = ITEM_TYPES.get(node.type, f"?{node.type}")
        counts[name] = counts.get(name, 0) + 1

        if node.type == 4 and node.body:
            dof_type = node.body[1]
            number = node.body[2]
            branch = node.body[7]
            if DOF_TYPES.get(dof_type) in ("SWITCH", "XSWITCH"):
                switch_nodes.setdefault(number, set()).add(branch)

        if node.type == 1 and node.body:
            flags = node.body[0]
            sw_emissive = (flags >> 5) & 1
            if sw_emissive:
                switch_surfaces.setdefault(node.body[8], []).append(
                    (node.body[9], node))

        if args.nodes:
            pad = "  " * node.depth
            extra = ""
            if node.type == 4 and node.body:
                dof_type = DOF_TYPES.get(node.body[1], node.body[1])
                extra = (f"  {dof_type} num={node.body[2]} branch={node.body[7]}"
                         f" total={node.body[0]}")
            elif node.type == 1 and node.body:
                extra = (f"  flags=0x{node.body[0]:08x} verts={node.body[1]}"
                         f" tex={node.body[6]} sw={node.body[8]}"
                         f" mask=0x{node.body[9]:x}")
            print(f"  @{node.offset:08d} {pad}{name}{extra}")

    print("  node counts:", ", ".join(f"{k}={v}" for k, v in sorted(counts.items())))
    print()
    print(f"  switch numbers present in the node tree ({len(switch_nodes)}):")
    for number in sorted(switch_nodes):
        branches = sorted(switch_nodes[number])
        print(f"    switch {number:4d}  branches {branches}")
    print()
    if switch_surfaces:
        print(f"  switchable-emissive surfaces ({len(switch_surfaces)} switches):")
        for number in sorted(switch_surfaces):
            entries = switch_surfaces[number]
            masks = sorted({m for (m, _) in entries})
            print(f"    switch {number:4d}  {len(entries)} surface(s)"
                  f"  masks {[hex(m) for m in masks]}")
        print()

    n_lights = header["dwLightsNr"]
    if n_lights:
        print(f"  lights pool ({n_lights}) at {header['pLightsPool']}:")
        at = header["pLightsPool"]
        for k in range(n_lights):
            if at + LIGHT_HEAD.size > len(blob):
                break
            (switch, mask, argument, flags) = LIGHT_HEAD.unpack_from(blob, at)
            sw = "always on" if switch == 0xFFFFFFFF else str(switch)
            print(f"    [{k:3d}] switch {sw}  mask 0x{mask:x}"
                  f"  arg {argument}  flags 0x{flags:x}")
            at += LIGHT_HEAD.size + 104  # + D3DLIGHT7
        print()
    return switch_nodes, switch_surfaces


def describe_parent(p, basename, show_lods=True):
    (dx, dy, dz) = p.extent()
    print(f"parent {p.index}")
    print(f"  radius        {p.radius:.4f}")
    print(f"  bbox          x [{p.bbox[0]:.4f}, {p.bbox[1]:.4f}]"
          f"  y [{p.bbox[2]:.4f}, {p.bbox[3]:.4f}]"
          f"  z [{p.bbox[4]:.4f}, {p.bbox[5]:.4f}]")
    print(f"  extent        {dx:.4f} x {dy:.4f} x {dz:.4f}")
    print(f"  nLODs         {p.n_lods}")
    print(f"  nSwitches     {p.n_switches}")
    print(f"  nDOFs         {p.n_dofs}")
    print(f"  nSlots        {p.n_slots}")
    print(f"  nDynamicCoord {p.n_dynamic_coords}")
    print(f"  nTextureSets  {p.n_texture_sets}")
    if p.positions:
        print(f"  positions     {len(p.positions)}")
        for k, pos in enumerate(p.positions):
            kind = "slot" if k < p.n_slots else "dyn "
            print(f"    [{k:3d}] {kind} {pos[0]:10.4f} {pos[1]:10.4f} {pos[2]:10.4f}")
    if show_lods:
        for (lod_index, name, max_range) in p.lods:
            print(f"  LOD {lod_index:5d}  range {max_range:12.1f}  {name!r}")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("basename",
                    help="path without extension, e.g. .../objects/KoreaObj")
    ap.add_argument("--parent", type=int, action="append", default=[],
                    help="describe this parent object id (repeatable)")
    ap.add_argument("--lod", type=int, action="append", default=[],
                    help="dump this LOD's DxDbHeader (repeatable)")
    ap.add_argument("--find", metavar="STR",
                    help="list LODs whose name contains STR (case-insensitive)")
    ap.add_argument("--switches-over", type=int, metavar="N",
                    help="list parents declaring more than N switches")
    ap.add_argument("--tree", type=int, action="append", default=[],
                    help="summarise this LOD's node stream (repeatable)")
    ap.add_argument("--nodes", action="store_true",
                    help="with --tree, print every node")
    ap.add_argument("--switch", type=int, action="append", default=[],
                    help="with --tree, report the geometry under this switch")
    args = ap.parse_args(argv)

    lods, parents, meta = read_dxh(args.basename)

    print(f"{meta['path']}")
    print(f"  format version 0x{meta['version']:08X}")
    print(f"  colours        {meta['n_colors']} (+{meta['n_darkened']} darkened)")
    print(f"  palettes       {meta['n_palettes']}")
    print(f"  textures       {meta['n_textures']}")
    print(f"  LOD records    {len(lods)}")
    print(f"  parent records {len(parents)}")
    print(f"  maxTagList     {meta['max_tag_list']}")
    print(f"  16-bit switch/DOF counts: {meta['new_counts']}")
    print(f"  unread trailing bytes: {meta['trailing_bytes']}")
    print()

    if args.find:
        needle = args.find.lower()
        for lod in lods:
            if lod.name and needle in lod.name.lower():
                print(f"LOD {lod.index:5d}  offset {lod.file_offset:10d}"
                      f"  size {lod.file_size:8d}  {lod.name!r}")
        owners = {}
        for p in parents:
            for (lod_index, name, _) in p.lods:
                if name and needle in name.lower():
                    owners.setdefault(p.index, []).append(lod_index)
        for pid, ids in sorted(owners.items()):
            print(f"  used by parent {pid}: LODs {ids}")
        print()

    if args.switches_over is not None:
        for p in parents:
            if p.n_switches > args.switches_over:
                names = [n for (_, n, _) in p.lods]
                print(f"parent {p.index:5d}  nSwitches {p.n_switches:4d}"
                      f"  nDOFs {p.n_dofs:4d}  {names}")
        print()

    for pid in args.parent:
        if not 0 <= pid < len(parents):
            print(f"parent {pid} out of range (0..{len(parents) - 1})")
            continue
        describe_parent(parents[pid], args.basename)
        print()

    for lid in args.lod:
        if not 0 <= lid < len(lods):
            print(f"LOD {lid} out of range (0..{len(lods) - 1})")
            continue
        lod = lods[lid]
        print(f"LOD {lid}  name {lod.name!r}  offset {lod.file_offset}"
              f"  size {lod.file_size}")
        header = read_model_header(args.basename, lod)
        if header is None:
            print("  (could not read model header)")
            continue
        for key, value in header.items():
            print(f"  {key:12s} {value}")
        print()

    for lid in args.tree:
        if not 0 <= lid < len(lods):
            print(f"LOD {lid} out of range (0..{len(lods) - 1})")
            continue
        lod = lods[lid]
        header = read_model_header(args.basename, lod)
        blob = read_model(args.basename, lod)
        print(f"LOD {lid}  name {lod.name!r}  size {lod.file_size}"
              f"  nodes {header['dwNodesNr']}  verts {header['dwNVertices']}"
              f"  pVPool {header['pVPool']}  textures {header['dwTexNr']}")
        dump_nodes(blob, header, args)

        for want in args.switch:
            print(f"  --- geometry under switch {want} ---")
            report_switch(blob, header, want)
            print()

    return 0


def report_switch(blob, header, want):
    """Print each branch of one switch and the extent of the geometry in it."""
    nodes = list(walk_nodes(blob, header))
    for (i, node) in enumerate(nodes):
        if node.type != 4 or not node.body:
            continue
        if DOF_TYPES.get(node.body[1]) not in ("SWITCH", "XSWITCH"):
            continue
        if node.body[2] != want:
            continue
        branch = node.body[7]
        total = node.body[0]
        end = node.offset + total
        lo = [float("inf")] * 3
        hi = [float("-inf")] * 3
        surfaces = 0
        verts = 0
        for inner in nodes[i + 1:]:
            if inner.offset >= end:
                break
            if inner.type != 1 or not inner.body:
                continue
            bounds = surface_bounds(blob, inner, header)
            surfaces += 1
            verts += inner.body[1]
            if bounds:
                for axis in range(3):
                    lo[axis] = min(lo[axis], bounds[0][axis])
                    hi[axis] = max(hi[axis], bounds[1][axis])
        if surfaces and lo[0] != float("inf"):
            print(f"    branch {branch}: {surfaces} surface(s), {verts} verts,"
                  f" x [{lo[0]:.4f}, {hi[0]:.4f}]"
                  f" y [{lo[1]:.4f}, {hi[1]:.4f}]"
                  f" z [{lo[2]:.4f}, {hi[2]:.4f}]")
        else:
            print(f"    branch {branch}: {surfaces} surface(s), {verts} verts")


if __name__ == "__main__":
    sys.exit(main())
