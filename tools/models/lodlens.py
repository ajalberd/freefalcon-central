#!/usr/bin/env python3
"""Author lamp LENS geometry into a model LOD of a theater object database.

Why this exists: the models carry a lamp's colour in the vertex COLOR2
(dwSpecular) -- see LIGHT-SOURCES.md -- and the port now uses that as the
emissive material on every surface (D3D7 parity).  That lights every lamp the
model authored a lens for (the F-16's intake fans, the landing-light cone, the
afterburner).  The wingtip nav lights are the exception: the model has only a
4-vertex POINTLIST at each wingtip, with the colour in the DIFFUSE and COLOR2
left at zero, and no lens surface at all -- so there is nothing for the engine
to make glow.  This tool adds one.

The added surface is authored exactly like the model's own working lamps
(node 55576, the intake fan pair):

  * an untextured Alpha triangle fan, VColor + Gouraud + Poly,
  * centre vertex: opaque black diffuse, COLOR2 = the lamp colour (the glow),
  * rim vertices: transparent black diffuse, COLOR2 = the model's dim 0x696969,
    so the fan fades to nothing at its edge,
  * SwEmissive + SwitchNumber/SwitchMask = the lamp's own switch, so it glows
    only while that lamp is on (the same gate the dynamic lights use).

Nothing in the original record is modified.  The patched record is APPENDED to
the .DXL and the .DXH LOD-table entry for that LOD is repointed at it; the
record's own header fields (dwNVertices, dwPoolSize, pVPool, dwNodesNr,
ModelSize) are rebuilt so the loader sees a normal model.  Reverting is
therefore one 8-byte write: `--revert` restores the original entry from the
JSON log this tool writes next to the .DXH.

Usage (note the `--lens=...` form -- a spec starts with a minus sign, so argparse
needs the `=` to tell it from an option):

    python lodlens.py <basename> --lod 2719 \
        --lens="-4.55,-15.55,-0.235:0,-1,0:0.30:0xff0000:8:0x1" \
        --lens="-4.55,15.55,-0.235:0,1,0:0.30:0x00ff00:8:0x1"

    SPEC = px,py,pz : nx,ny,nz : radius : color : switch : mask
           [: segments (default 8) : bulge (default 0.10)]

    --dry-run   build and verify the record, print it, write nothing.
    --revert    put the original LOD-table entry back (from the JSON log).

The F-16CJ wing light (the reason this was written): the diagram's
"POSITION/FORMATION LIGHT ... (TOP AND BOTTOM)" sits on the wing's own skin,
inboard of the wingtip missile rail -- NOT on the wingtip's outboard end plate
(y = +/-15.52), which is where the rail and its missile sit, so a lens there is
inside the store.  At y = +/-14.3 the skin is at z = -0.381 (upper) and -0.181
(lower), normals ~(0,0,-1) / (0,0,+1) -- a flat, almost axis-aligned patch.  Red
is the LEFT wing, green the RIGHT, matching the model's own lamp points and
light nodes:

    python lodlens.py C:/FreeFalcon6/terrdata/objects/KoreaObj --lod 2719 \
        --lens="-4.0,-14.3,-0.401:0,0,-1:0.35:0xff0000:8:0x1" \
        --lens="-4.0,-14.3,-0.161:0,0,1:0.35:0xff0000:8:0x1" \
        --lens="-4.0,14.3,-0.401:0,0,-1:0.35:0x00ff00:8:0x1" \
        --lens="-4.0,14.3,-0.161:0,0,1:0.35:0x00ff00:8:0x1"

Read back with `python objsurvey.py <basename> --lod 2719 --tree 2719`.
"""

import argparse
import json
import math
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import objsurvey as O  # noqa: E402

# DxSurfaceType flags (dxdefines.h DXFlagsType): Alpha, Lite, VColor, SwEmissive,
# Poly, Gouraud.  The intake fan is 0x3009 (no SwEmissive); the model's
# switch-gated lamps add 0x20.
FLAG_ALPHA = 0x0001
FLAG_LITE = 0x0002
FLAG_VCOLOR = 0x0008
FLAG_SWEMISSIVE = 0x0020
FLAG_POLY = 0x1000
FLAG_GOURAUD = 0x2000
LENS_FLAGS = (FLAG_ALPHA | FLAG_VCOLOR | FLAG_SWEMISSIVE | FLAG_POLY |
              FLAG_GOURAUD)

# D3DPT_TRIANGLELIST (dxdefines.h D3DPRIMITIVETYPE)
PRIM_TRIANGLELIST = 4

# The lamp fan convention, copied from the working intake lamps (node 55576).
LENS_SPECULAR_INDEX = 6.0
LENS_DEFAULT_SPECULARITY = 0xFF404040
LENS_RIM_EMISSIVE = 0x00696969      # dim grey, as authored on the intake rims
LENS_CENTRE_DIFFUSE = 0xFF000000    # opaque black
LENS_RIM_DIFFUSE = 0x00000000       # transparent black

NODE_HEAD = struct.Struct("<3I")
SURFACE_BODY = O.SURFACE_BODY        # 44 bytes after the head (56 with it)
SURFACE_NODE = O.SURFACE_NODE_SIZE   # 56


class Lens:
    __slots__ = ("pos", "normal", "radius", "color", "switch", "mask",
                 "segments", "bulge")

    def __init__(self, spec):
        fields = spec.split(":")
        if len(fields) < 6:
            raise SystemExit(f"--lens spec needs 6 fields, got {len(fields)}: {spec}")
        self.pos = [float(v) for v in fields[0].split(",")]
        self.normal = [float(v) for v in fields[1].split(",")]
        self.radius = float(fields[2])
        self.color = int(fields[3], 0)
        self.switch = int(fields[4], 0)
        self.mask = int(fields[5], 0)
        self.segments = int(fields[6]) if len(fields) > 6 else 8
        self.bulge = float(fields[7]) if len(fields) > 7 else 0.10
        if len(self.pos) != 3 or len(self.normal) != 3:
            raise SystemExit(f"--lens needs a 3-vector position and normal: {spec}")

    def vertices(self):
        """Centre + rim, in model space, wound like the model's own lamp fans.

        The winding matters: the object pass culls (g_nObjCullMode), and the
        convention that shows in game is the intake fan's -- the right-hand-rule
        normal of each triangle points along the surface normal.
        """
        (px, py, pz) = self.pos
        (nx, ny, nz) = self.normal
        ln = math.sqrt(nx * nx + ny * ny + nz * nz)
        (nx, ny, nz) = (nx / ln, ny / ln, nz / ln)
        # an in-plane basis: u is the world up unless the normal is vertical
        if abs(nz) < 0.9:
            (ux, uy, uz) = (0.0, 0.0, 1.0)
        else:
            (ux, uy, uz) = (1.0, 0.0, 0.0)
        # v = n x u
        (vx, vy, vz) = (ny * uz - nz * uy, nz * ux - nx * uz, nx * uy - ny * ux)

        out = []
        # centre: pushed out along the normal (a shallow dome, like the intake fan)
        out.append(((px + nx * self.bulge, py + ny * self.bulge,
                     pz + nz * self.bulge), (nx, ny, nz),
                    LENS_CENTRE_DIFFUSE, self.color))
        for k in range(self.segments):
            a = 2.0 * math.pi * k / self.segments
            (ca, sa) = (math.cos(a), math.sin(a))
            out.append(((px + self.radius * (ca * vx + sa * ux),
                         py + self.radius * (ca * vy + sa * uy),
                         pz + self.radius * (ca * vz + sa * uz)),
                        (nx + 0.5 * (ca * vx + sa * ux),
                         ny + 0.5 * (ca * vy + sa * uy),
                         nz + 0.5 * (ca * vz + sa * uz)),
                        LENS_RIM_DIFFUSE, LENS_RIM_EMISSIVE))
        return out

    def indices(self):
        """Fan indices, wound like the model's own lamp fans.

        The winding matters: the object pass culls (g_nObjCullMode), and the
        convention that shows in game is the intake fan's -- the right-hand-rule
        normal of each triangle points ALONG the surface normal.  With the rim
        built as p = c + r(cos a * v + sin a * u) and v = n x u, the triple
        (centre, rim[k+1], rim[k]) gives that; (centre, rim[k], rim[k+1]) is the
        reverse and gets culled (that is what the first wingtip attempt shipped:
        two invisible lenses).
        """
        out = []
        for k in range(self.segments):
            out.extend((0, 1 + (k + 1) % self.segments, 1 + k))
        return out

    def surface_flags(self):
        return LENS_FLAGS

    def specular_index(self):
        return LENS_SPECULAR_INDEX

    def default_specularity(self):
        return LENS_DEFAULT_SPECULARITY


class Panel:
    """An opaque, untextured quad -- a cover plate.

    Written for the F-16's wingtip end plate: the model's flat vertical facet at
    y = +/-15.5212 (a 7.4 x 0.6 rectangle) is lit by the nav light sitting right
    next to it, so at night the light's spill turns it into a bright grey BOX
    that only appears while the lamp is on.  A dark cover quad a few centimetres
    outboard hides it and stays dark under any light (dark vertex colour x light
    = dark, and specular index 0 kills the highlight).

    SPEC = px,py,pz : nx,ny,nz : size_u,size_v : color
    """

    __slots__ = ("pos", "normal", "size_u", "size_v", "color", "switch", "mask")

    def __init__(self, spec):
        fields = spec.split(":")
        if len(fields) < 4:
            raise SystemExit(f"--panel spec needs 4 fields, got {len(fields)}: {spec}")
        self.pos = [float(v) for v in fields[0].split(",")]
        self.normal = [float(v) for v in fields[1].split(",")]
        (su, sv) = fields[2].split(",")
        self.size_u = float(su)
        self.size_v = float(sv)
        self.color = int(fields[3], 0)
        self.switch = 0
        self.mask = 0
        if len(self.pos) != 3 or len(self.normal) != 3:
            raise SystemExit(f"--panel needs a 3-vector position and normal: {spec}")

    def _basis(self):
        (px, py, pz) = self.pos
        (nx, ny, nz) = self.normal
        ln = math.sqrt(nx * nx + ny * ny + nz * nz)
        (nx, ny, nz) = (nx / ln, ny / ln, nz / ln)
        if abs(nz) < 0.9:
            (ux, uy, uz) = (0.0, 0.0, 1.0)
        else:
            (ux, uy, uz) = (1.0, 0.0, 0.0)
        (vx, vy, vz) = (ny * uz - nz * uy, nz * ux - nx * uz, nx * uy - ny * ux)
        return ((px, py, pz), (nx, ny, nz), (ux, uy, uz), (vx, vy, vz))

    def vertices(self):
        ((px, py, pz), n, u, v) = self._basis()
        hu = self.size_u / 2.0
        hv = self.size_v / 2.0
        out = []
        for (su, sv) in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
            out.append(((px + su * hu * u[0] + sv * hv * v[0],
                         py + su * hu * u[1] + sv * hv * v[1],
                         pz + su * hu * u[2] + sv * hv * v[2]),
                        n, self.color, 0x00000000))
        return out

    def indices(self):
        # (c0,c1,c2) + (c0,c2,c3): the right-hand-rule normal is u x v = n.
        return [0, 1, 2, 0, 2, 3]

    def surface_flags(self):
        return FLAG_LITE | FLAG_VCOLOR | FLAG_POLY | FLAG_GOURAUD

    def specular_index(self):
        return 0.0

    def default_specularity(self):
        return 0x00000000


def vertex_bytes(pos, normal, diffuse, specular):
    return O.VERTEX.pack(pos[0], pos[1], pos[2], normal[0], normal[1], normal[2],
                         diffuse, specular, 0.0, 0.0)


def surface_node_bytes(node_id, shape, first_vertex):
    """A DX_SURFACE node: head + DxSurfaceType + the 16-bit index list."""
    verts = shape.vertices()
    indices = [first_vertex + i for i in shape.indices()]
    size = SURFACE_NODE + 2 * len(indices)
    head = NODE_HEAD.pack(size, node_id, 1)          # 1 = DX_SURFACE
    body = SURFACE_BODY.pack(shape.surface_flags(), len(indices),
                             O.VERTEX_STRIDE, PRIM_TRIANGLELIST, 0,
                             shape.specular_index(),
                             0xFFFFFFFF, 0,                     # TexID[0..1]
                             shape.switch, shape.mask,
                             shape.default_specularity())
    idx = struct.pack("<" + "H" * len(indices), *indices)
    return head + body + idx


def dxh_lod_entry_offset(meta, lod_index):
    """Byte offset in the .DXH of LOD `lod_index`'s table entry.

    ObjectLOD::ReadTable reads it as 12 spare bytes + UInt32 fileoffset +
    UInt32 filesize, right after maxTagList/nLODs.  The two fields are therefore
    at entry + 12; use dxh_lod_ref_offset() for the write.
    """
    pos = 4                                       # format version
    pos += 8 + meta["n_colors"] * O.PCOLOR        # colour bank
    pos += 4 + meta["n_palettes"] * O.DISK_PALETTE
    pos += 4                                      # nTextures
    if meta["n_textures"]:
        pos += 4 + meta["n_textures"] * O.DISK_TEXENTRY
    pos += 4 + 4                                  # maxTagList + nLODs
    return pos + lod_index * O.LOD_HEADER


# offset of the fileoffset/filesize pair inside a LOD table entry (the entry
# starts with 12 spare bytes)
DXH_LODREF_AT = 12


def dxh_lod_ref_offset(meta, lod_index):
    return dxh_lod_entry_offset(meta, lod_index) + DXH_LODREF_AT


def build_record(blob, header, shapes, next_node_id):
    """Return (record_bytes, added_vertex_count) with the shapes appended."""
    node_start = O.DXDBHEADER.size + header["dwTexNr"] * 4
    nodes_end = header["pLightsPool"]
    vertices_at = header["pVPool"]
    lights_at = header["pLightsPool"]

    # Find MODELEND -- the new nodes go just before it, at the top level, so no
    # DOF subtree total has to change.
    model_end_at = None
    for node in O.walk_nodes(blob, header):
        if node.type == 9:                        # DX_MODELEND
            model_end_at = node.offset
            break
    if model_end_at is None:
        raise SystemExit("no DX_MODELEND node found -- refusing to touch this LOD")

    new_nodes = []
    new_verts = []
    for (k, shape) in enumerate(shapes):
        first_vertex = header["dwNVertices"] + len(new_verts)
        new_nodes.append(surface_node_bytes(next_node_id + k, shape, first_vertex))
        for v in shape.vertices():
            new_verts.append(vertex_bytes(*v))

    nodes = (blob[node_start:model_end_at] +
             b"".join(new_nodes) +
             blob[model_end_at:nodes_end])
    lights = blob[lights_at:vertices_at]
    pool = blob[vertices_at:] + b"".join(new_verts)

    pVPool = O.DXDBHEADER.size + header["dwTexNr"] * 4 + len(nodes) + len(lights)
    n_vertices = header["dwNVertices"] + len(new_verts)
    fields = list(O.DXDBHEADER.unpack_from(blob, 0))
    fields[3] = pVPool + len(pool)                # ModelSize
    fields[4] = n_vertices                        # dwNVertices
    fields[5] = n_vertices * O.VERTEX_STRIDE      # dwPoolSize
    fields[6] = pVPool                            # pVPool
    fields[7] = header["dwNodesNr"] + len(new_nodes)
    fields[17] = O.DXDBHEADER.size + header["dwTexNr"] * 4 + len(nodes)
    record = (O.DXDBHEADER.pack(*fields) +
              blob[O.DXDBHEADER.size:node_start] +
              nodes + lights + pool)
    return (record, len(new_verts))


def verify_record(record, first_node_id):
    """Re-parse the built record the way the engine will and report the lenses.

    Filter by node id, not by switch: the model already has other SwEmissive
    surfaces on the same switch (the F-16 has seven on switch 8), and those are
    not ours.
    """
    header = O.DXDBHEADER.unpack_from(record, 0)
    hdr = {"dwTexNr": header[18], "pVPool": header[6], "dwNVertices": header[4],
           "dwLightsNr": header[16], "pLightsPool": header[17],
           "dwNodesNr": header[7]}
    if header[3] != len(record):
        raise SystemExit(f"ModelSize {header[3]} != record length {len(record)}")
    found = []
    for node in O.walk_nodes(record, hdr):
        if node.type != 1 or not node.body or node.node_id < first_node_id:
            continue
        bounds = O.surface_bounds(record, node, hdr)
        found.append((node.node_id, node.body[1], bounds))
    return found


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("basename", help="path without extension, e.g. .../objects/KoreaObj")
    ap.add_argument("--lod", type=int, required=True, help="LOD index to patch")
    ap.add_argument("--lens", action="append", default=[], metavar="SPEC",
                    help="lens to add: p:n:r:color:switch:mask[:segments[:bulge]]")
    ap.add_argument("--panel", action="append", default=[], metavar="SPEC",
                    help="opaque cover quad to add: p:n:size_u,size_v:color")
    ap.add_argument("--dry-run", action="store_true",
                    help="build and verify, print the result, write nothing")
    ap.add_argument("--revert", action="store_true",
                    help="restore the original LOD-table entry from the JSON log")
    ap.add_argument("--node-id", type=int, default=None,
                    help="first node id for the added surfaces (default max+1)")
    args = ap.parse_args(argv)

    lods, parents, meta = O.read_dxh(args.basename)
    if not 0 <= args.lod < len(lods):
        raise SystemExit(f"LOD {args.lod} out of range (0..{len(lods) - 1})")
    lod = lods[args.lod]
    entry_at = dxh_lod_entry_offset(meta, args.lod)
    ref_at = dxh_lod_ref_offset(meta, args.lod)
    dxh_path = meta["path"]
    dxl_path = dxh_path[:-4] + ".Dxl"
    log_path = os.path.splitext(dxh_path)[0] + ".lodlens.json"

    if args.revert:
        if not os.path.exists(log_path):
            raise SystemExit(f"no log at {log_path} -- nothing to revert")
        with open(log_path, "r") as fh:
            log = json.load(fh)
        original = log["original"]
        with open(dxh_path, "r+b") as fh:
            fh.seek(ref_at)
            fh.write(struct.pack("<II", original["offset"], original["size"]))
        print(f"reverted LOD {args.lod}: offset {original['offset']} "
              f"size {original['size']}")
        print("(the appended record is still in the .DXL; it is now unreferenced)")
        return 0

    if not args.lens and not args.panel:
        raise SystemExit("nothing to do: pass --lens/--panel or --revert")

    shapes = ([Lens(spec) for spec in args.lens] +
              [Panel(spec) for spec in args.panel])
    if args.node_id is not None:
        next_node_id = args.node_id
    else:
        next_node_id = 1 + max(n.node_id for n in O.walk_nodes(
            O.read_model(args.basename, lod), O.read_model_header(args.basename, lod)))

    blob = O.read_model(args.basename, lod)
    header = O.read_model_header(args.basename, lod)
    (record, added) = build_record(blob, header, shapes, next_node_id)
    found = verify_record(record, next_node_id)

    print(f"LOD {args.lod} {lod.name!r}  {lod.file_size} -> {len(record)} bytes"
          f"  (+{added} vertices, +{len(shapes)} surfaces)")
    for (node_id, count, bounds) in found:
        if bounds:
            (lo, hi, used) = bounds
            print(f"  added node {node_id}: {count} verts,"
                  f" x [{lo[0]:.3f},{hi[0]:.3f}] y [{lo[1]:.3f},{hi[1]:.3f}]"
                  f" z [{lo[2]:.3f},{hi[2]:.3f}]")
        else:
            print(f"  added node {node_id}: {count} verts (no bounds)")
    if len(found) != len(shapes):
        raise SystemExit(f"verification found {len(found)} added surfaces,"
                         f" expected {len(shapes)}")

    if args.dry_run:
        print("dry run: nothing written")
        return 0

    backup = dxh_path + ".bak-lodlens"
    if not os.path.exists(backup):
        with open(dxh_path, "rb") as src, open(backup, "wb") as dst:
            dst.write(src.read())
        print(f"backed up the header to {backup}")

    with open(dxl_path, "rb") as fh:
        fh.seek(0, os.SEEK_END)
        new_offset = fh.tell()
    with open(dxl_path, "ab") as fh:
        fh.write(record)
    with open(dxh_path, "r+b") as fh:
        fh.seek(ref_at)
        fh.write(struct.pack("<II", new_offset, len(record)))

    log = {
        "time": time.strftime("%Y-%m-%d %H:%M:%S"),
        "lod": args.lod, "name": lod.name,
        "entry_at": entry_at, "ref_at": ref_at,
        "original": {"offset": lod.file_offset, "size": lod.file_size},
        "patched": {"offset": new_offset, "size": len(record)},
        "lenses": args.lens,
        "panels": args.panel,
    }
    if os.path.exists(log_path):
        with open(log_path, "r") as fh:
            old = json.load(fh)
        log["previous"] = old.get("previous", []) + [{
            "time": old.get("time"), "patched": old.get("patched"),
            "lenses": old.get("lenses"), "panels": old.get("panels")}]
    with open(log_path, "w") as fh:
        json.dump(log, fh, indent=2)
    print(f"appended the patched record at .DXL offset {new_offset}")
    print(f"repointed the .DXH LOD ref at {ref_at} (log: {log_path})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
