"""One editing session over one game installation.

Holds the loaded CampaignDB and campaign files per theater, tracks what has
been changed, and writes only the files that need writing -- after taking a
backup, because the files being edited are the ones the game reads at startup
and a bad write is a reinstall.
"""

import os
import shutil
import threading
import time

from . import (campdb, campfile, camptext, entities, names, objectives,
               records, tacan, terrain, theater, triggers, uiart)


class Backup:
    """One timestamped backup folder per campaign directory per session."""

    def __init__(self):
        self._made = {}

    def keep(self, path):
        """Copy `path` aside once, the first time it is about to be written."""
        if not os.path.isfile(path):
            return None
        camp_dir = os.path.dirname(path)
        if os.path.basename(camp_dir).lower() == "campaigndb":
            camp_dir = os.path.dirname(camp_dir)
        key = camp_dir
        if key not in self._made:
            stamp = time.strftime("%Y%m%d-%H%M%S")
            self._made[key] = os.path.join(camp_dir, "_editor-backup", stamp)
        root = self._made[key]
        rel = os.path.relpath(path, camp_dir)
        dest = os.path.join(root, rel)
        if os.path.exists(dest):
            return dest
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        shutil.copy2(path, dest)
        return dest

    def folders(self):
        return sorted(self._made.values())


class TheaterWorkspace:
    def __init__(self, gamedir, tdf_rel):
        self.gamedir = gamedir
        self.tdf_rel = tdf_rel
        self.tdf = theater.find_theater(gamedir, tdf_rel)
        if self.tdf is None:
            raise ValueError("no theater matches %r" % tdf_rel)
        self.campaign_dir = self.tdf.campaign_dir(gamedir)
        self.db = campdb.CampaignDB(theater.db_dir(gamedir, self.tdf))
        self._camps = {}
        # The HTTP server answers on several threads at once, so the lazy
        # caches below (and the edit buffers they hold) get one lock.
        self._lock = threading.RLock()

    # -- campaign files -------------------------------------------------------

    def campaign_files(self):
        out = []
        if not os.path.isdir(self.campaign_dir):
            return out
        for name in sorted(os.listdir(self.campaign_dir)):
            low = name.lower()
            if not low.endswith((".cam", ".tac")):
                continue
            path = os.path.join(self.campaign_dir, name)
            out.append({
                "file": name,
                "bytes": os.path.getsize(path),
                "kind": "tactical" if low.endswith(".tac") else "campaign",
                "derived": low in theater.DERIVED,
            })
        return out

    def campaign(self, name):
        with self._lock:
            if name not in self._camps:
                path = os.path.join(self.campaign_dir, name)
                if not os.path.isfile(path):
                    raise ValueError("no such campaign file: %s" % name)
                cam = campfile.CampaignFile.load(path)
                cam.units = None
                cam.units_raw = b""
                cam.units_error = ""
                cam.objectives = None
                cam.objectives_raw = b""
                cam.objectives_error = ""
                cam.objectives_from = name
                self._camps[name] = cam
            return self._camps[name]

    def _stream_error(self, exc):
        """Explain a decode failure when there is no CampaignDB at all.

        `class-table index N out of range` is only the first symptom of a
        missing class table, and the plain message sends you looking at the
        .cam instead of at the database directory that was searched.
        """
        if self.db.present():
            return str(exc)
        return ("no CampaignDB found in %s, so this file's class-table "
                "references cannot be resolved (%s)"
                % (os.path.relpath(self.db.dir, self.gamedir), exc))

    def units(self, name):
        """Decode the unit list on first use, and remember any desync."""
        with self._lock:
            cam = self.campaign(name)
            if cam.units is None and not cam.units_error:
                section = cam.member("uni")
                if section is None:
                    cam.units, cam.units_raw = [], b""
                else:
                    try:
                        cam.units, cam.units_raw = entities.decode_units(
                            section, cam.version, self.db.class_rows())
                    except Exception as exc:
                        cam.units, cam.units_raw = [], b""
                        cam.units_error = self._stream_error(exc)
            return cam

    def objectives(self, name):
        """Decode the objective list, falling back to the base scenario.

        A mid-campaign save stores only objective *deltas* (`.obd`) against
        the scenario it was started from, and carries no `.obj` of its own.
        The header names that scenario, so borrow its objective list rather
        than showing an empty map.
        """
        with self._lock:
            cam = self.campaign(name)
            if cam.objectives is not None or cam.objectives_error:
                return cam

            section = cam.member("obj")
            source = name
            if section is None and cam.header is not None:
                base = (cam.header.fields.get("Scenario") or "").strip()
                if base:
                    candidate = base
                    if not candidate.lower().endswith((".cam", ".tac")):
                        candidate += ".cam"
                    path = os.path.join(self.campaign_dir, candidate)
                    if os.path.isfile(path) and candidate.lower() != name.lower():
                        try:
                            other = self.campaign(candidate)
                            section = other.member("obj")
                            source = candidate
                        except ValueError:
                            section = None

            cam.objectives_from = source
            if section is None:
                cam.objectives, cam.objectives_raw = [], b""
                return cam
            try:
                cam.objectives, cam.objectives_raw = objectives.decode_objectives(
                    section, cam.version, self.db.class_rows())
            except Exception as exc:
                cam.objectives, cam.objectives_raw = [], b""
                cam.objectives_error = self._stream_error(exc)
            return cam

    def name_table(self):
        if not hasattr(self, "_names"):
            self._names = names.load(self.campaign_dir)
        return self._names

    def script(self, campaign_file):
        """The .tri that decides how this campaign ends, open for editing."""
        with self._lock:
            if not hasattr(self, "_scripts"):
                self._scripts = {}
            key = campaign_file.lower()
            if key not in self._scripts:
                path = triggers.script_path(self.campaign_dir, campaign_file)
                if not path:
                    self._scripts[key] = None
                else:
                    try:
                        self._scripts[key] = triggers.Script(path)
                    except OSError:
                        self._scripts[key] = None
            return self._scripts[key]

    def triggers(self, campaign_file):
        """Back-compat shape: ((init, body, total), path)."""
        sc = self.script(campaign_file)
        if not sc:
            return None
        return (sc.init, sc.body, sc.total), sc.path

    def campaign_text(self):
        """The theater's campaign-selection text, open for editing."""
        with self._lock:
            if not hasattr(self, "_text"):
                path = camptext.locate(self.gamedir, self.tdf.get("artdir"))
                try:
                    self._text = camptext.TextFile(path) if path else None
                except OSError:
                    self._text = None
            return self._text

    def tacan(self):
        """Airbase TACAN stations, keyed by the objective's campaign id."""
        if not hasattr(self, "_tacan"):
            self._tacan = tacan.load(self.campaign_dir)
        return self._tacan

    def terrain(self):
        """The theater's ground imagery, or None if it cannot be rendered.

        `terraindir` in the .tdf points at the shared terrain, so several
        theaters usually resolve to the same directory -- and to the same
        cached overview.
        """
        with self._lock:
            if hasattr(self, "_terrain"):
                return self._terrain
            self._terrain = None
            rel = self.tdf.get("terraindir")
            if rel and terrain.available():
                path = os.path.join(
                    self.gamedir, *rel.replace("\\", "/").split("/"))
                if os.path.isdir(path):
                    try:
                        self._terrain = terrain.Terrain(path)
                    except Exception:
                        self._terrain = None
            return self._terrain

    def ui_art(self):
        """The game's UI art, for the campaign map's own icon set.

        A theater can override art with its own directory (`artdir` in the
        .tdf), so look there before the game root -- that is the same order
        the engine's resource manager searches.
        """
        with self._lock:
            if not hasattr(self, "_uiart"):
                roots = []
                art = self.tdf.get("artdir")
                if art:
                    roots.append(os.path.join(
                        self.gamedir, *art.replace("\\", "/").split("/")))
                try:
                    self._uiart = uiart.UiArt(self.gamedir, roots)
                except Exception:
                    self._uiart = None
            return self._uiart

    def icon_atlas(self, colour):
        """One team colour's icons -- ground set and air set in one sheet.

        Ground, naval and objective symbols live in `<colour>dark`; aircraft
        silhouettes live in the directional `<colour>air_*` sets, and the two
        never share a name, so a single atlas can serve both.
        """
        with self._lock:
            if not hasattr(self, "_atlases"):
                self._atlases = {}
            if colour in self._atlases:
                return self._atlases[colour]

            art = self.ui_art()
            if not art:
                self._atlases[colour] = None
                return None

            ground = art.icon_set(colour)
            if not ground:
                self._atlases[colour] = None
                return None

            merged = uiart.IconSet.__new__(uiart.IconSet)
            merged.base = ground.base
            merged.icons = dict(ground.icons)
            merged._data = ground._data

            air_name = {"white": "WHITE", "green": "GREEN", "blue": "BLUE",
                        "brown": "BROWN", "orange": "ORANGE", "yellow": "YELLOW",
                        "red": "RED", "grey": "GREY"}.get(colour)
            rel = art.resources.get("%s_AIR_NORTH" % air_name) if air_name else None
            base = art._resolve(rel) if rel else None
            if base:
                try:
                    air = uiart.IconSet(base)
                except Exception:
                    air = None
                if air:
                    # Two sets, two data buffers: pack the air icons separately
                    # and merge the frames, rather than pretending one buffer
                    # holds both.
                    self._atlases[colour] = _merge_atlas(ground, air)
                    return self._atlases[colour]

            self._atlases[colour] = uiart.IconAtlas(ground)
            return self._atlases[colour]

    def map_image(self):
        """The kneeboard map for this campaign, used as the map background."""
        for name in ("Kneemap.gif", "kneemap.gif"):
            path = os.path.join(self.campaign_dir, name)
            if os.path.isfile(path):
                return path
        try:
            for f in os.listdir(self.campaign_dir):
                if f.lower() == "kneemap.gif":
                    return os.path.join(self.campaign_dir, f)
        except OSError:
            pass
        return None

    # -- dirty tracking -------------------------------------------------------

    def pending(self):
        out = []
        for name, sc in getattr(self, "_scripts", {}).items():
            if sc and sc.dirty:
                out.append({"kind": "script", "name": name,
                            "file": os.path.basename(sc.path)})
        text = getattr(self, "_text", None)
        if text and text.dirty:
            out.append({"kind": "text", "name": "campaign text",
                        "file": os.path.basename(text.path)})
        for tname, tbl in list(self.db._tables.items()):
            if tbl.dirty:
                out.append({"kind": "table", "name": tname,
                            "file": os.path.basename(tbl.path)})
        for name, cam in self._camps.items():
            if getattr(cam, "dirty", False):
                out.append({"kind": "campaign", "name": name, "file": name})
        return out

    def save(self, backup):
        written = []
        for _name, sc in getattr(self, "_scripts", {}).items():
            if not sc or not sc.dirty:
                continue
            backup.keep(sc.path)
            sc.save()
            written.append(os.path.relpath(sc.path, self.gamedir))
        text = getattr(self, "_text", None)
        if text and text.dirty:
            backup.keep(text.path)
            text.save()
            written.append(os.path.relpath(text.path, self.gamedir))
        for tname, tbl in list(self.db._tables.items()):
            if not tbl.dirty:
                continue
            backup.keep(tbl.path)
            tbl.save()
            written.append(os.path.relpath(tbl.path, self.gamedir))
        for name, cam in self._camps.items():
            if not getattr(cam, "dirty", False):
                continue
            backup.keep(cam.path)
            if getattr(cam, "units_dirty", False):
                member = cam._member_name("uni")
                cam.members[member] = entities.encode_units(
                    cam.units_raw, len(cam.units))
                cam.units_dirty = False
            if getattr(cam, "objectives_dirty", False):
                member = cam._member_name("obj")
                if member:
                    cam.members[member] = objectives.encode_objectives(
                        cam.objectives_raw, len(cam.objectives))
                cam.objectives_dirty = False
            cam.save()
            cam.dirty = False
            written.append(os.path.relpath(cam.path, self.gamedir))
        return written


class Session:
    def __init__(self, gamedir):
        self.gamedir = os.path.abspath(gamedir)
        self.backup = Backup()
        self._theaters = {}
        # Requests arrive on several threads: one thread per tile means the
        # workspaces below must be created and iterated under a lock, or two
        # requests can end up editing two copies of the same workspace.
        self._lock = threading.Lock()

    def theaters(self):
        return [t.as_dict(self.gamedir)
                for t in theater.load_theaters(self.gamedir)]

    def workspace(self, tdf_rel):
        key = tdf_rel.replace("/", "\\").lower()
        with self._lock:
            if key not in self._theaters:
                self._theaters[key] = TheaterWorkspace(self.gamedir, tdf_rel)
            return self._theaters[key]

    def forget(self, tdf_rel=None):
        with self._lock:
            if tdf_rel is None:
                self._theaters.clear()
            else:
                self._theaters.pop(tdf_rel.replace("/", "\\").lower(), None)

    def pending(self):
        with self._lock:
            workspaces = list(self._theaters.values())
        out = []
        for ws in workspaces:
            for item in ws.pending():
                item = dict(item)
                item["theater"] = ws.tdf.name
                item["tdf"] = ws.tdf_rel
                out.append(item)
        return out

    def save_all(self):
        with self._lock:
            workspaces = list(self._theaters.values())
        written = []
        for ws in workspaces:
            written.extend(ws.save(self.backup))
        return {"written": written, "backups": [
            os.path.relpath(p, self.gamedir) for p in self.backup.folders()]}


def _merge_atlas(*sets):
    """Pack several IconSets into one atlas, first set wins on a name clash."""
    class _Combined:
        def __init__(self):
            self.icons = {}
            self._owner = {}

        def names(self):
            return sorted(self.icons)

        def rgba(self, name):
            return self._owner[name].rgba(name)

    c = _Combined()
    for st in sets:
        for nm, e in st.icons.items():
            if nm not in c.icons:
                c.icons[nm] = e
                c._owner[nm] = st
    return uiart.IconAtlas(c)


# --- presentation helpers ----------------------------------------------------

ARRAY_LABELS = {
    "HitChance": records.MOVE_NAMES,
    "Strength": records.MOVE_NAMES,
    "Range": records.MOVE_NAMES,
    "Detection": records.MOVE_NAMES,
    "DamageMod": records.DAMAGE_NAMES,
}

# Per table, the handful of columns worth showing in the list view.
SUMMARY_COLUMNS = {
    "unit": ["Index", "Name", "MovementType", "MovementSpeed", "MaxRange",
             "Role", "SpecialIndex"],
    "vehicle": ["Index", "Name", "NCTR", "HitPoints", "MaxSpeed", "RadarType",
                "MaxWt", "RCSfactor"],
    "weapon": ["Index", "Name", "Strength", "DamageType", "Range", "Weight",
               "BlastRadius", "MaxAlt"],
    "objective": ["Index", "Name", "DataRate", "Features", "FirstFeature",
                  "IconIndex"],
    "feature": ["Index", "Name", "HitPoints", "RepairTime", "Priority",
                "RadarType"],
    "weaponlist": ["Name"],
    "class": ["id_", "dataType", "dataPtr", "vehicleDataIndex", "hitpoints_"],
    "featureentry": ["Index", "Value", "Facing"],
    "squadstores": ["infiniteAG", "infiniteAA", "infiniteGun"],
    "simweapon": ["mnemonic", "weight", "weaponClass", "domain", "weaponType"],
}
