"""Flatten the terrain under airstrips whose ground is not flat.

Runway pieces are flat models, and each is placed at the ground height under its
own centre (DrawableBuilding::Draw, GetGroundLevelApproximation). Where the
theater's terrain rises and falls across an airstrip -- Kumch'on varies by about
450 ft in the original 2011 data -- the ground pokes up through the runway, and
which pieces vanish changes with the terrain detail level. The old renderer hid
it by clearing depth before drawing objects; one coherent depth buffer shows it.

This levels the ground under each such airstrip's flat surfaces (the features
flagged FEAT_FLAT_CONTAINER: runways, ramps, taxiways, aprons) to one height --
the median of the ground there -- and blends back to the original terrain over
a ring around them. Every terrain detail level (THEATER.L0..L5) gets the same
blend, so the levels still agree where their posts coincide. Sea posts (height
0 or below) are never raised. Post normals (lighting) are recomputed for every
post whose neighbourhood changed, with an encoder checked against the untouched
data first.

The .L files share identical blocks between map positions; an edited block is
written as a new copy at the end of its file and only that position's offset
(.O file) is repointed, so nothing else on the map changes.

Safety:
  * Before the first write, the original THEATER.L*/O* files are copied to
    <terrain>/backup-pre-flatten/ with a manifest of SHA-256 hashes.
  * --rollback puts them back (after checking the backup against the manifest).
  * rollback-flatten.bat is written next to the backup: it restores the
    originals with plain copy commands, no Python needed.

    python flatten_airstrips.py                 # dry run: report only
    python flatten_airstrips.py --write         # back up, then flatten
    python flatten_airstrips.py --rollback      # restore the originals
"""

import argparse
import datetime
import hashlib
import json
import math
import os
import shutil
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "campaign-editor"))

import tilesurvey as ts  # noqa: E402
from ffcamp.workspace import TheaterWorkspace  # noqa: E402

FEET_PER_KM = 3279.98
POSTS_ACROSS = 16
POSTS_PER_BLOCK = 256
FEAT_FLAT_CONTAINER = 0x100
LEVELS = 6
BACKUP_DIR = "backup-pre-flatten"
MANIFEST = "manifest.json"

CORE_FT = 1000.0    # ground within this of the flat surfaces is set to the target
BLEND_FT = 2000.0   # then blended back to the original over this much more
SPREAD_FT = 20      # only airstrips whose ground varies more than this under the core
MAX_SPREAD_FT = 300 # more than this is an airstrip sitting on a cliff: listed, not touched


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def level_files(terrain_dir):
    out = []
    for lod in range(LEVELS):
        for ext in ("L", "O"):
            out.append(ts.find(terrain_dir, "Theater.%s%d" % (ext.lower(), lod)))
    return out


# ------------------------------------------------------------------ backup and rollback

def backup(terrain_dir, log=print):
    bdir = os.path.join(terrain_dir, BACKUP_DIR)
    man_path = os.path.join(bdir, MANIFEST)
    if os.path.exists(man_path):
        log("backup already there: %s (originals kept from the first run)" % bdir)
        return json.load(open(man_path))
    os.makedirs(bdir, exist_ok=True)
    files = {}
    for path in level_files(terrain_dir):
        name = os.path.basename(path)
        shutil.copy2(path, os.path.join(bdir, name))
        files[name] = {"original": sha256(path)}
        log("  backed up %s" % name)
    man = {"created": datetime.datetime.now().isoformat(timespec="seconds"),
           "terrain": terrain_dir, "files": files, "flattened": []}
    json.dump(man, open(man_path, "w"), indent=1)
    with open(os.path.join(bdir, "rollback-flatten.bat"), "w", newline="\r\n") as f:
        f.write("@echo off\n")
        f.write("rem Restores the terrain files saved before flatten_airstrips.py changed them.\n")
        f.write("rem Run it from this folder (double-click is fine). The game must be closed.\n")
        f.write("cd /d \"%~dp0\"\n")
        for name in files:
            f.write("copy /y \"%s\" \"..\\%s\" >nul || goto fail\n" % (name, name))
        f.write("echo Terrain restored to the originals.\n")
        f.write("pause\nexit /b 0\n:fail\necho Restore FAILED -- is the game still running?\npause\nexit /b 1\n")
    log("backup written: %s (rollback-flatten.bat restores it without Python)" % bdir)
    return man


def rollback(terrain_dir, log=print):
    bdir = os.path.join(terrain_dir, BACKUP_DIR)
    man_path = os.path.join(bdir, MANIFEST)
    if not os.path.exists(man_path):
        raise SystemExit("no backup at %s -- nothing to roll back" % bdir)
    man = json.load(open(man_path))
    for name, info in man["files"].items():
        src = os.path.join(bdir, name)
        if sha256(src) != info["original"]:
            raise SystemExit("backup copy %s does not match its manifest hash; not restoring" % src)
    for name in man["files"]:
        shutil.copy2(os.path.join(bdir, name), os.path.join(terrain_dir, name))
        log("  restored %s" % name)
    for name, info in man["files"].items():
        if sha256(os.path.join(terrain_dir, name)) != info["original"]:
            raise SystemExit("restored %s does not match the original hash" % name)
    log("terrain restored to the originals (all %d files verified)" % len(man["files"]))


# ------------------------------------------------------------------ one terrain level

class Level:
    """One THEATER.Ln / .On pair, in memory, with copy-on-write block edits."""

    def __init__(self, terrain_dir, lod, post_size, zoff):
        self.lod = lod
        self.lpath = ts.find(terrain_dir, "Theater.l%d" % lod)
        self.opath = ts.find(terrain_dir, "Theater.o%d" % lod)
        self.data = bytearray(open(self.lpath, "rb").read())
        self.offsets = np.frombuffer(open(self.opath, "rb").read(), dtype="<u4").copy()
        self.bw = int(round(math.sqrt(len(self.offsets))))
        self.bh = len(self.offsets) // self.bw
        self.ps = post_size
        self.zoff = zoff
        self.spacing = 819.995 * (1 << lod)
        self.posts_w = self.bw * POSTS_ACROSS
        self.posts_h = self.bh * POSTS_ACROSS
        self.new_z = {}          # (px, py) -> new height
        self.copied = {}         # block index -> new offset
        self.orig_len = len(self.data)

    def _addr(self, px, py):
        b = (py // POSTS_ACROSS) * self.bw + px // POSTS_ACROSS
        k = (py % POSTS_ACROSS) * POSTS_ACROSS + (px % POSTS_ACROSS)
        return b, int(self.offsets[b]) + k * self.ps

    def z(self, px, py):
        """Height after the edits so far (clamped at the map edge)."""
        px = min(max(px, 0), self.posts_w - 1)
        py = min(max(py, 0), self.posts_h - 1)
        if (px, py) in self.new_z:
            return self.new_z[(px, py)]
        _b, a = self._addr(px, py)
        return int.from_bytes(self.data[a + self.zoff:a + self.zoff + 2], "little", signed=True)

    def orig_z(self, px, py):
        px = min(max(px, 0), self.posts_w - 1)
        py = min(max(py, 0), self.posts_h - 1)
        _b, a = self._addr(px, py)
        return int.from_bytes(self.data[a + self.zoff:a + self.zoff + 2], "little", signed=True)

    def normal_bytes(self, px, py):
        _b, a = self._addr(px, py)
        return self.data[a + self.zoff + 3], self.data[a + self.zoff + 4]

    def _own(self, b):
        """Give block b its own copy at the end of the file (once)."""
        if b in self.copied:
            return
        old = int(self.offsets[b])
        size = POSTS_PER_BLOCK * self.ps
        new = len(self.data)
        if new >= 0x80000000:
            raise SystemExit("L%d would exceed 2 GB" % self.lod)
        self.data += self.data[old:old + size]
        self.offsets[b] = new
        self.copied[b] = new

    def write_post(self, px, py, z=None, theta=None, phi=None):
        b, _a = self._addr(px, py)
        self._own(b)
        _b, a = self._addr(px, py)
        if z is not None:
            self.data[a + self.zoff:a + self.zoff + 2] = int(z).to_bytes(2, "little", signed=True)
        if theta is not None:
            self.data[a + self.zoff + 3] = theta
            self.data[a + self.zoff + 4] = phi

    def save(self):
        with open(self.lpath, "wb") as f:
            f.write(self.data)
        with open(self.opath, "wb") as f:
            f.write(self.offsets.astype("<u4").tobytes())


# ------------------------------------------------------------------ normals

def encode_theta(di, dj):
    """Direction byte, as the shipped data has it: MapDice's atan2(Ny, Nx) over 256
    steps with Nx = -(rise along the post rows) and Ny = -(rise along the columns),
    and its special case when Nx is zero (flat ground comes out 63, as stored)."""
    nx, ny = -dj, -di
    if abs(nx) < 1e-6:
        t = -math.pi / 2 if ny < 0 else math.pi / 2
    else:
        t = math.atan2(ny, nx)
    if t < 0:
        t += 2 * math.pi
    return min(int(255.99 * t / (2 * math.pi)), 255)


class NormalCodec:
    """Theta/phi bytes from neighbouring heights, calibrated on untouched posts.

    theta: fixed formula (matches the shipped data exactly). phi: a tilt scale
    fitted per level, with floor or round, whichever reproduces more posts.
    """

    def __init__(self, lvl, sample_box, log=print):
        self.lvl = lvl
        px0, py0, px1, py1 = sample_box
        tilts, phis, th_ok, n = [], [], 0, 0
        for py in range(max(py0, 1), min(py1, lvl.posts_h - 1)):
            for px in range(max(px0, 1), min(px1, lvl.posts_w - 1)):
                dj = lvl.orig_z(px, py + 1) - lvl.orig_z(px, py - 1)
                di = lvl.orig_z(px + 1, py) - lvl.orig_z(px - 1, py)
                t_b, p_b = lvl.normal_bytes(px, py)
                n += 1
                th_ok += abs(encode_theta(di, dj) - t_b) <= 1 or (di == 0 and dj == 0 and t_b == 63)
                if abs(di) + abs(dj) > 10:
                    tilts.append(math.atan(math.hypot(di, dj) / (2 * lvl.spacing)))
                    phis.append(p_b)
        self.theta_ok = th_ok / max(n, 1)
        tilts, phis = np.array(tilts), np.array(phis, float)
        self.k, self.mode, self.phi_ok = 0.0, "floor", 0.0
        if len(tilts) > 50:
            k0 = float((phis * tilts).sum() / max((tilts ** 2).sum(), 1e-9))
            for k in np.linspace(k0 * 0.97, k0 * 1.03, 61):
                for mode in ("floor", "round"):
                    enc = np.floor(k * tilts) if mode == "floor" else np.round(k * tilts)
                    ok = float((np.abs(np.minimum(enc, 63) - phis) <= 0).mean())
                    if ok > self.phi_ok:
                        self.k, self.mode, self.phi_ok = float(k), mode, ok
        within1 = 0.0
        if len(tilts):
            enc = np.floor(self.k * tilts) if self.mode == "floor" else np.round(self.k * tilts)
            within1 = float((np.abs(np.minimum(enc, 63) - phis) <= 1).mean())
        self.phi_within1 = within1
        self.usable = self.theta_ok >= 0.95 and within1 >= 0.95
        log("  L%d normals: theta %.1f%% exact, phi %.1f%% exact / %.1f%% within 1 (k=%.2f %s) -> %s"
            % (lvl.lod, 100 * self.theta_ok, 100 * self.phi_ok, 100 * within1, self.k, self.mode,
               "recomputed" if self.usable else "LEFT AS THEY WERE (encoder does not match)"))

    def encode(self, px, py):
        lvl = self.lvl
        dj = lvl.z(px, py + 1) - lvl.z(px, py - 1)
        di = lvl.z(px + 1, py) - lvl.z(px - 1, py)
        tilt = math.atan(math.hypot(di, dj) / (2 * lvl.spacing))
        p = math.floor(self.k * tilt) if self.mode == "floor" else round(self.k * tilt)
        return encode_theta(di, dj), int(min(max(p, 0), 63))


# ------------------------------------------------------------------ airstrips

def flat_footprints(ws, campaign):
    """[(name, type, centre_east_ft, centre_north_ft, [(east, north) of flat features])]"""
    names = ws.name_table()
    fed = ws.db.table("featureentry").rows
    out = []
    for o in ws.objectives(campaign).objectives:
        if o.get("typeName") not in ("Airbase", "Airstrip"):
            continue
        _t, row = ws.db.data_row(o["classIndex"])
        if not row:
            continue
        cx = (o["x"] + 0.5) * FEET_PER_KM     # east
        cy = (o["y"] + 0.5) * FEET_PER_KM     # north
        pts = []
        for k in range(row["Features"]):
            e = fed[row["FirstFeature"] + k]
            _ft, frow = ws.db.data_row(e["Index"])
            if frow and frow["Flags"] & FEAT_FLAT_CONTAINER:
                # GetFeatureOffset(f, &y, &x, &z): Offset[0] is east, Offset[1] north.
                pts.append((cx + e["Offset"][0], cy + e["Offset"][1]))
        if pts:
            out.append((names.get(o["nameId"], "?").strip(), o["typeName"], cx, cy, pts))
    return out


class Rect:
    """Oriented box round a set of points (principal axis), for distances."""

    def __init__(self, pts):
        p = np.array(pts, float)
        self.c = p.mean(axis=0)
        q = p - self.c
        if len(p) >= 2 and np.abs(q).sum() > 1:
            _u, _s, vt = np.linalg.svd(q, full_matrices=False)
            self.ax = vt[0]
        else:
            self.ax = np.array([1.0, 0.0])
        self.pe = np.array([-self.ax[1], self.ax[0]])
        a, b = q @ self.ax, q @ self.pe
        self.a0, self.a1, self.b0, self.b1 = a.min(), a.max(), b.min(), b.max()

    def dist(self, e, n):
        q = np.array([e, n]) - self.c
        a, b = q @ self.ax, q @ self.pe
        da = max(self.a0 - a, 0.0, a - self.a1)
        db = max(self.b0 - b, 0.0, b - self.b1)
        return math.hypot(da, db)

    def reach(self):
        return max(abs(self.a0), abs(self.a1)) + max(abs(self.b0), abs(self.b1))


def weight(d, core, blend):
    if d <= core:
        return 1.0
    if d >= core + blend:
        return 0.0
    t = (d - core) / blend
    return 1.0 - t * t * (3.0 - 2.0 * t)   # smoothstep down


def survey(lvl0, fp):
    """Target height and spread of the L0 ground under an airstrip's core."""
    name, typ, cx, cy, pts = fp
    rect = Rect(pts)
    r = rect.reach() + CORE_FT
    zs = []
    for py in range(int((cy - r) / lvl0.spacing), int((cy + r) / lvl0.spacing) + 2):
        for px in range(int((cx - r) / lvl0.spacing), int((cx + r) / lvl0.spacing) + 2):
            if rect.dist(px * lvl0.spacing, py * lvl0.spacing) <= CORE_FT:
                zs.append(lvl0.orig_z(px, py))
    land = [z for z in zs if z > 0] or zs
    return rect, (int(np.median(land)) if land else 0), (max(zs) - min(zs) if zs else 0), len(zs), \
        sum(1 for z in zs if z <= 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--gamedir", default=r"C:\FreeFalcon6")
    ap.add_argument("--theater", default=r"terrdata\theaterdefinition\korea.tdf")
    ap.add_argument("--campaign", default="save0.cam",
                    help="campaign file whose objectives give the airstrip positions")
    ap.add_argument("--spread", type=int, default=SPREAD_FT,
                    help="only airstrips whose ground varies more than this many feet (default %d)"
                         % SPREAD_FT)
    ap.add_argument("--only", action="append", metavar="NAME",
                    help="only airstrips whose name contains this (repeatable)")
    ap.add_argument("--write", action="store_true", help="back up, then change the terrain files")
    ap.add_argument("--rollback", action="store_true", help="restore the original terrain files")
    args = ap.parse_args()

    ws = TheaterWorkspace(args.gamedir, args.theater)
    terr_root = os.path.join(args.gamedir, ws.tdf.get("terraindir", r"terrdata\korea")) \
        if isinstance(ws.tdf, dict) else None
    if not terr_root or not os.path.isdir(terr_root):
        terr_root = os.path.join(args.gamedir, r"terrdata\korea")
    terrain_dir = ts.find(terr_root, "terrain")

    if args.rollback:
        rollback(terrain_dir)
        return

    bdir = os.path.join(terrain_dir, BACKUP_DIR)
    if args.write and os.path.exists(os.path.join(bdir, MANIFEST)):
        man = json.load(open(os.path.join(bdir, MANIFEST)))
        if man.get("flattened"):
            raise SystemExit("already flattened (%s). Roll back first to run it again."
                             % ", ".join(man["flattened"]))

    tmap = ts.TheaterMap(ts.find(terrain_dir, "Theater.map"))
    zoff = 4 if tmap.large_terrain else 2
    post_size = tmap.post_size
    levels = [Level(terrain_dir, lod, post_size, zoff) for lod in range(LEVELS)]
    lvl0 = levels[0]

    picks, cliffs = [], []
    for fp in flat_footprints(ws, args.campaign):
        if args.only and not any(s.lower() in fp[0].lower() for s in args.only):
            continue
        rect, target, spread, nposts, sea = survey(lvl0, fp)
        if spread > MAX_SPREAD_FT:
            cliffs.append((fp, spread))
        elif spread > args.spread:
            picks.append((fp, rect, target, spread, nposts, sea))
    picks.sort(key=lambda p: -p[3])
    for fp, spread in cliffs:
        print("  SKIPPED %s: ground varies %d ft under its runways -- levelling it would build a "
              "plateau; it needs moving or hand editing" % (fp[0], spread))
    if not picks:
        print("no airstrip varies by more than %d ft under its runways -- nothing to do" % args.spread)
        return

    print("%d airstrips to flatten (ground spread over %d ft under the runways):" % (len(picks), args.spread))
    for fp, rect, target, spread, nposts, sea in picks:
        print("  %-28s %-8s spread %4d ft -> level at %4d ft  (%d posts under the core%s)"
              % (fp[0], fp[1], spread, target, nposts, ", %d of them sea, kept" % sea if sea else ""))

    # Normal encoders, checked on untouched ground near the first airstrip.
    print("checking the normal encoding against the untouched data:")
    fp0 = picks[0][0]
    codecs = []
    for lvl in levels:
        cxp, cyp = int(fp0[2] / lvl.spacing), int(fp0[3] / lvl.spacing)
        r = max(12, int(30000 / lvl.spacing))
        codecs.append(NormalCodec(lvl, (cxp - r, cyp - r, cxp + r, cyp + r)))

    # Heights, level by level: the same blend everywhere. L1 and L2 (500 m and 1 km
    # posts, the levels seen from a few km out) are widened by half a post spacing so
    # their bigger triangles are covered too. Not L3 and up: a post there is 6-26 km
    # apart, and widening would drag whole mountains down; their posts are exact
    # samples of L0, so with the L0 blend they agree with L0 where they coincide.
    report = []
    for lvl, codec in zip(levels, codecs):
        widen = 0.5 * lvl.spacing if lvl.lod in (1, 2) else 0.0
        core = CORE_FT + widen
        blend = BLEND_FT + widen
        changed = 0
        cut = fill = 0
        for fp, rect, target, spread, nposts, sea in picks:
            name, typ, cx, cy, pts = fp
            r = rect.reach() + core + blend
            for py in range(int((cy - r) / lvl.spacing), int((cy + r) / lvl.spacing) + 2):
                for px in range(int((cx - r) / lvl.spacing), int((cx + r) / lvl.spacing) + 2):
                    if not (0 <= px < lvl.posts_w and 0 <= py < lvl.posts_h):
                        continue
                    w = weight(rect.dist(px * lvl.spacing, py * lvl.spacing), core, blend)
                    if w <= 0.0:
                        continue
                    old = lvl.z(px, py)
                    if old <= 0 and target > 0:
                        continue                   # sea: never raised
                    new = int(round(old + (target - old) * w))
                    if new != old:
                        lvl.new_z[(px, py)] = new
                        changed += 1
                        cut = max(cut, old - new)
                        fill = max(fill, new - old)
        # Write heights, then normals for every post whose neighbourhood changed.
        for (px, py), z in lvl.new_z.items():
            lvl.write_post(px, py, z=z)
        renorm = set()
        for (px, py) in lvl.new_z:
            for dx, dy in ((0, 0), (1, 0), (-1, 0), (0, 1), (0, -1)):
                qx, qy = px + dx, py + dy
                if 1 <= qx < lvl.posts_w - 1 and 1 <= qy < lvl.posts_h - 1:
                    renorm.add((qx, qy))
        if codec.usable:
            for (px, py) in renorm:
                t, p = codec.encode(px, py)
                lvl.write_post(px, py, theta=t, phi=p)
        report.append((lvl.lod, changed, cut, fill, len(lvl.copied), len(renorm) if codec.usable else 0))

    print("per level: posts changed, deepest cut / highest fill, blocks copied, normals redone")
    for lod, changed, cut, fill, copied, renorm in report:
        print("  L%d  %6d posts  cut %4d ft / fill %4d ft  %4d blocks (+%.1f KB)  %6d normals"
              % (lod, changed, cut, fill, copied, copied * POSTS_PER_BLOCK * post_size / 1024.0, renorm))

    if not args.write:
        print("dry run: nothing written. Add --write to back up and apply.")
        return

    # Windows will not let a file that another process has open be rewritten -- the game, or
    # the campaign editor's server (it maps the terrain). Find out before touching anything.
    busy = []
    for lvl in levels:
        if lvl.copied:
            for path in (lvl.lpath, lvl.opath):
                try:
                    with open(path, "r+b"):
                        pass
                except OSError:
                    busy.append(os.path.basename(path))
    if busy:
        raise SystemExit("cannot write %s: another program has it open. Close the game and the "
                         "campaign editor (tools/campaign-editor/server.py), then run this again. "
                         "Nothing was changed." % ", ".join(busy))

    man = backup(terrain_dir)
    for lvl in levels:
        if lvl.copied:
            lvl.save()
    man["flattened"] = [p[0][0] for p in picks]
    man["applied"] = datetime.datetime.now().isoformat(timespec="seconds")
    for name in man["files"]:
        man["files"][name]["flattened"] = sha256(os.path.join(terrain_dir, name))
    json.dump(man, open(os.path.join(terrain_dir, BACKUP_DIR, MANIFEST), "w"), indent=1)
    print("written. To undo: python %s --rollback   (or run %s\\rollback-flatten.bat)"
          % (os.path.basename(__file__), os.path.join(terrain_dir, BACKUP_DIR)))


if __name__ == "__main__":
    main()
