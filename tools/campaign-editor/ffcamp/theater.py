"""Theater definitions, and cloning one into a new theater.

A theater is a .tdf file -- plain "key value" lines, parsed by
ParseSimlibFile against theaterdesc[] in src/falclib/theaterdef.cpp -- listed
in theater.lst at the game root. The keys the engine actually reads are the
ones in KEYS below; anything else in the file is ignored by the parser and is
preserved verbatim when we rewrite.

Cloning is the whole point of the tool: a new theater is a copy of an existing
campaign directory (its own CampaignDB, so the class table can diverge) plus a
.tdf pointing at it and a line in theater.lst. Terrain, objects and art can
stay shared with the parent -- that is what makes "Korea 1980s" a few hundred
MB of campaign data rather than a whole new map.
"""

import os
import re
import shutil

KEYS = [
    "name", "desc", "campaigndir", "terraindir", "artdir", "moviedir",
    "uisounddir", "objectdir", "3ddatadir", "misctexdir", "bitmap",
    "mintacan", "sounddir", "cockpitdir", "zipsdir", "tacrefdir", "splashdir",
]

# Not in theaterdesc[], but every shipped .tdf carries it and other tools read
# it, so keep writing it.
EXTRA_KEYS = ["specialdbdir"]

THEATER_LIST = "theater.lst"
TDF_DIR = os.path.join("terrdata", "theaterdefinition")


def _norm(p):
    return str(p).replace("/", "\\")


def _under(root, rel):
    return os.path.join(root, *str(rel).replace("\\", "/").split("/"))


def _slug(name):
    s = re.sub(r"[^A-Za-z0-9]+", "_", str(name)).strip("_").lower()
    return s or "theater"


class TheaterDef:
    def __init__(self, path, lines, values):
        self.path = path
        self.lines = lines       # original file, for round-tripping comments
        self.values = values

    @property
    def name(self):
        return self.values.get("name", os.path.basename(self.path))

    def get(self, key, default=""):
        return self.values.get(key, default)

    def campaign_dir(self, gamedir):
        rel = self.get("campaigndir")
        return os.path.join(gamedir, *rel.replace("\\", "/").split("/")) if rel else ""

    def as_dict(self, gamedir):
        camp = self.campaign_dir(gamedir)
        return {
            "file": os.path.relpath(self.path, gamedir).replace("\\", "/"),
            "name": self.name,
            "desc": self.get("desc"),
            "campaigndir": self.get("campaigndir"),
            "terraindir": self.get("terraindir"),
            "artdir": self.get("artdir"),
            "objectdir": self.get("objectdir"),
            "campaign_exists": bool(camp) and os.path.isdir(camp),
            "has_campaigndb": bool(camp) and os.path.isdir(
                os.path.join(camp, "CampaignDB")),
        }


def parse_tdf(path):
    with open(path, "r", encoding="latin-1") as fp:
        lines = fp.read().splitlines()
    values = {}
    for line in lines:
        s = line.strip()
        if not s or s[0] in "#;":
            continue
        parts = s.split(None, 1)
        key = parts[0].lower()
        values[key] = parts[1].strip() if len(parts) > 1 else ""
    return TheaterDef(path, lines, values)


def load_theaters(gamedir):
    """Every theater listed in theater.lst that has a readable .tdf."""
    listing = os.path.join(gamedir, THEATER_LIST)
    out = []
    if not os.path.isfile(listing):
        return out
    with open(listing, "r", encoding="latin-1") as fp:
        for line in fp:
            rel = line.strip()
            if not rel or rel[0] in "#;":
                continue
            path = os.path.join(gamedir, *rel.replace("\\", "/").split("/"))
            if os.path.isfile(path):
                try:
                    out.append(parse_tdf(path))
                except OSError:
                    pass
    return out


def find_theater(gamedir, tdf_rel):
    want = _norm(tdf_rel).lower()
    for t in load_theaters(gamedir):
        if _norm(os.path.relpath(t.path, gamedir)).lower() == want:
            return t
    return None


def db_dir(gamedir, tdf):
    """The directory holding this theater's CampaignDB tables.

    Tried in order, first existing directory wins:

      specialdbdir            the per-campaign copy the Korea-family .tdf
                              files point at, and what the DB tools edit
      <campaigndir>\\CampaignDB  what a theater created here gets
      objectdir               where the engine reads the tables from:
                              OpenCampFile maps .ct and every .*CD extension
                              onto FalconObjectDataDir, which theaterdef.cpp
                              sets from `objectdir`. The Israel theaters keep
                              their only copy there, with no specialdbdir.

    If none exist the conventional <campaigndir>\\CampaignDB is returned so
    callers can report the path that was searched.
    """
    camp = tdf.get("campaigndir")
    local = _under(gamedir, camp.rstrip("\\/") + "/CampaignDB") if camp else ""
    candidates = []
    for rel in (tdf.get("specialdbdir"), tdf.get("objectdir")):
        if rel:
            candidates.append(_under(gamedir, rel))
    if local:
        candidates.insert(1, local)
    for path in candidates:
        if os.path.isdir(path):
            return path
    return local


def list_entries(gamedir):
    """The raw theater.lst lines, so a rewrite keeps comments and order."""
    listing = os.path.join(gamedir, THEATER_LIST)
    if not os.path.isfile(listing):
        return []
    with open(listing, "r", encoding="latin-1") as fp:
        return fp.read().splitlines()


def write_tdf(path, values, template=None):
    """Write a .tdf, keeping the comment layout of `template` where possible."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    written = set()
    out = []

    if template is not None:
        for line in template.lines:
            s = line.strip()
            if not s or s[0] in "#;":
                out.append(line)
                continue
            key = s.split(None, 1)[0].lower()
            if key in values:
                out.append("%s %s" % (key, values[key]))
                written.add(key)
            else:
                out.append(line)

    for key in KEYS + EXTRA_KEYS:
        if key in values and key not in written:
            out.append("%s %s" % (key, values[key]))
            written.add(key)

    with open(path, "w", encoding="latin-1", newline="\r\n") as fp:
        fp.write("\n".join(out) + "\n")


def register(gamedir, tdf_rel):
    """Append a .tdf to theater.lst if it is not already listed."""
    listing = os.path.join(gamedir, THEATER_LIST)
    rel = _norm(tdf_rel)
    lines = list_entries(gamedir)
    for line in lines:
        if _norm(line.strip()).lower() == rel.lower():
            return False
    lines.append(rel)
    with open(listing, "w", encoding="latin-1", newline="\r\n") as fp:
        fp.write("\n".join(lines) + "\n")
    return True


def unregister(gamedir, tdf_rel):
    listing = os.path.join(gamedir, THEATER_LIST)
    rel = _norm(tdf_rel).lower()
    lines = list_entries(gamedir)
    kept = [l for l in lines if _norm(l.strip()).lower() != rel]
    if len(kept) == len(lines):
        return False
    with open(listing, "w", encoding="latin-1", newline="\r\n") as fp:
        fp.write("\n".join(kept) + "\n")
    return True


# Campaign-directory files that are per-campaign state rather than authored
# content. They are copied anyway (the game expects them present), but the UI
# flags them so nobody hand-edits a derived file and wonders why saves differ.
DERIVED = {"auto save.cam", "flist.txt", "default.idx", "default.wch"}


def clone_campaign_dir(src, dst, progress=None):
    """Copy a campaign directory, including its own CampaignDB."""
    if os.path.exists(dst):
        raise FileExistsError("%s already exists" % dst)
    os.makedirs(dst)
    total = 0
    for root, _dirs, files in os.walk(src):
        rel = os.path.relpath(root, src)
        target = dst if rel == "." else os.path.join(dst, rel)
        os.makedirs(target, exist_ok=True)
        for f in files:
            shutil.copy2(os.path.join(root, f), os.path.join(target, f))
            total += 1
            if progress:
                progress(total, f)
    return total


def dir_size(path):
    total = 0
    for root, _dirs, files in os.walk(path):
        for f in files:
            try:
                total += os.path.getsize(os.path.join(root, f))
            except OSError:
                pass
    return total


def create_theater(gamedir, base_tdf_rel, new_name, desc="", campaign_folder=None,
                   copy_art=False, progress=None):
    """Clone `base_tdf_rel` into a new theater and register it.

    Returns a dict describing what was created. Terrain, objects and misc
    textures keep pointing at whatever the base theater used -- only the
    campaign directory (and optionally the art directory) is copied.
    """
    base = find_theater(gamedir, base_tdf_rel)
    if base is None:
        raise ValueError("no theater matches %r" % base_tdf_rel)

    slug = _slug(campaign_folder or new_name)
    camp_rel = os.path.join("campaign", slug)
    camp_abs = os.path.join(gamedir, camp_rel)
    src_camp = base.campaign_dir(gamedir)
    if not os.path.isdir(src_camp):
        raise ValueError("base theater campaign directory %r is missing" % src_camp)
    if os.path.exists(camp_abs):
        raise FileExistsError("campaign\\%s already exists" % slug)

    tdf_rel = os.path.join(TDF_DIR, slug + ".tdf")
    tdf_abs = os.path.join(gamedir, tdf_rel)
    if os.path.exists(tdf_abs):
        raise FileExistsError("%s already exists" % tdf_rel)

    copied = clone_campaign_dir(src_camp, camp_abs, progress)

    # The source may keep its only DB outside the campaign directory (the
    # Israel theaters' objectdir). The new .tdf points specialdbdir at the new
    # campaign directory, so copy the resolved tables in rather than let the
    # clone edit the parent's shared copy.
    dst_db = os.path.join(camp_abs, "CampaignDB")
    src_db = db_dir(gamedir, base)
    if not os.path.isdir(dst_db) and os.path.isdir(src_db):
        shutil.copytree(src_db, dst_db)
        copied += sum(len(files) for _r, _d, files in os.walk(dst_db))

    values = dict(base.values)
    values["name"] = new_name
    values["desc"] = desc or new_name
    values["campaigndir"] = _norm(camp_rel)
    values["specialdbdir"] = _norm(os.path.join(camp_rel, "campaigndb"))

    art_rel = base.get("artdir")
    if copy_art and art_rel:
        src_art = os.path.join(gamedir, *art_rel.replace("\\", "/").split("/"))
        dst_art_rel = "art" + slug
        dst_art = os.path.join(gamedir, dst_art_rel)
        if os.path.isdir(src_art) and not os.path.exists(dst_art):
            shutil.copytree(src_art, dst_art)
            values["artdir"] = dst_art_rel

    write_tdf(tdf_abs, values, template=base)
    register(gamedir, tdf_rel)

    return {
        # Forward slashes so this matches the `file` key in as_dict() and the
        # UI can select the new theater straight away.
        "name": new_name,
        "tdf": tdf_rel.replace("\\", "/"),
        "campaigndir": _norm(camp_rel),
        "artdir": values.get("artdir", ""),
        "files_copied": copied,
        "bytes": dir_size(camp_abs),
    }


def delete_theater(gamedir, tdf_rel, remove_campaign_dir=False):
    """Remove a theater. Refuses to touch the three that ship with the game."""
    protected = {"korea.tdf", "korea_2012.tdf", "eurowar.tdf"}
    if os.path.basename(_norm(tdf_rel)).lower() in protected:
        raise ValueError("%s ships with the game and will not be deleted"
                         % os.path.basename(tdf_rel))
    t = find_theater(gamedir, tdf_rel)
    unregister(gamedir, tdf_rel)
    path = os.path.join(gamedir, *_norm(tdf_rel).replace("\\", "/").split("/"))
    removed = []
    if os.path.isfile(path):
        os.remove(path)
        removed.append(_norm(tdf_rel))
    if remove_campaign_dir and t is not None:
        camp = t.campaign_dir(gamedir)
        if camp and os.path.isdir(camp) and "campaign" in camp.replace("\\", "/"):
            shutil.rmtree(camp)
            removed.append(t.get("campaigndir"))
    return removed
