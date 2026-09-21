"""The campaign-selection text: names, and the victory-condition blurb.

The three campaigns a theater advertises get their on-screen text from
`<artdir>/art/Main/lcktxtrc.irc`, a plain list of

    [ADDTEXT] TXT_SCENARIO_1 "Rolling Fire by FF5.5"
    [ADDTEXT] TXT_SC_1 "Victory Conditions: Allies; Control P'Yongyang ..."

`SelectScenarioCB` (`src/ui/src/campaign/cpselect.cpp`) maps `save0` to
`TXT_SCENARIO_1`, `save1` to `_2` and `save2` to `_3`; the campaign-select
window (`art/campaign/select/cs_pua.scf`) puts `TXT_SC_<n>` in the SitRep box
alongside each.

Nothing checks that the blurb agrees with the campaign's trigger script --
they are separate files written by hand -- so a theater can and does ship with
text that describes conditions its script does not implement. That is exactly
what this module exists to let you fix.

Writing preserves every other line of the file byte for byte; only the values
that changed are rewritten, and the file keeps its CRLF endings.
"""

import os
import re

# save0 is scenario 1, and so on.
SLOTS = ["save0", "save1", "save2"]

ENTRY = re.compile(r'^(\s*\[ADDTEXT\]\s+)(\S+)(\s+)"(.*)"(\s*)$', re.IGNORECASE)


def slot_of(campaign_file):
    """`save0.cam` -> 1, or 0 when the file is not one of the three."""
    stem = os.path.splitext(os.path.basename(campaign_file))[0].lower()
    return SLOTS.index(stem) + 1 if stem in SLOTS else 0


def _find(dirpath, name):
    want = name.lower()
    try:
        for f in os.listdir(dirpath):
            if f.lower() == want:
                return os.path.join(dirpath, f)
    except OSError:
        pass
    return None


def _descend(root, *parts):
    for part in parts:
        if not root:
            return None
        root = _find(root, part)
    return root


def locate(gamedir, artdir):
    """The lcktxtrc.irc this theater's `artdir` resolves to.

    An override theater's artdir is a root that *contains* an `art` tree
    (`artKorea2012/art/Main/...`), but stock Korea's artdir is `art` itself,
    so it is the art tree. Try both, then fall back to the stock art at the
    game root for a theater that names no artdir at all.
    """
    roots = []
    if artdir:
        roots.append(os.path.join(gamedir, *artdir.replace("\\", "/").split("/")))
    roots.append(os.path.join(gamedir, "art"))
    for root in roots:
        if not os.path.isdir(root):
            continue
        for path in (_descend(root, "art", "main", "lcktxtrc.irc"),
                     _descend(root, "main", "lcktxtrc.irc")):
            if path:
                return path
    return None


class TextFile:
    """One lcktxtrc.irc, editable a key at a time."""

    def __init__(self, path):
        self.path = path
        with open(path, "r", encoding="latin-1", newline="") as fp:
            self.lines = fp.read().splitlines(True)
        self.index = {}
        for i, line in enumerate(self.lines):
            m = ENTRY.match(line.rstrip("\r\n"))
            if m:
                self.index.setdefault(m.group(2), i)
        self.dirty = False

    def get(self, key, default=""):
        i = self.index.get(key)
        if i is None:
            return default
        m = ENTRY.match(self.lines[i].rstrip("\r\n"))
        return m.group(4) if m else default

    def set(self, key, value):
        i = self.index.get(key)
        if i is None:
            raise KeyError("%s is not in %s" % (key, os.path.basename(self.path)))
        stripped = self.lines[i].rstrip("\r\n")
        eol = self.lines[i][len(stripped):] or "\r\n"
        m = ENTRY.match(stripped)
        if not m:
            raise KeyError(key)
        # A double quote would end the string early; the format has no escape.
        value = str(value).replace('"', "'")
        self.lines[i] = "%s%s%s\"%s\"%s%s" % (
            m.group(1), m.group(2), m.group(3), value, m.group(5), eol)
        self.dirty = True

    def save(self, path=None):
        path = path or self.path
        tmp = path + ".tmp"
        with open(tmp, "w", encoding="latin-1", newline="") as fp:
            fp.write("".join(self.lines))
        os.replace(tmp, path)
        self.dirty = False

    def campaign_text(self, slot):
        """The name and blurb a campaign slot (1..3) shows on the select page."""
        return {
            "slot": slot,
            "nameKey": "TXT_SCENARIO_%d" % slot,
            "name": self.get("TXT_SCENARIO_%d" % slot),
            "blurbKey": "TXT_SC_%d" % slot,
            "blurb": self.get("TXT_SC_%d" % slot),
        }


# --- generating a blurb from the script --------------------------------------

def _join(names):
    names = list(names)
    if len(names) <= 1:
        return "".join(names)
    return ", ".join(names[:-1]) + " and " + names[-1]


def blurb_from_script(endgames, allied_teams=(1, 2, 3)):
    """Write the SitRep line the script actually implements.

    Groups the endgames the way the shipped text does: what the allies have to
    hold, what the opposition has to hold, and how long the campaign runs.
    `place_short` strips the "(id)" the editor shows so the sentence reads
    like the game's own.
    """
    allied, opfor, days = [], [], None

    for e in endgames:
        for cond in e.get("conditions", []):
            if cond.startswith("NOT ("):
                continue
            m = re.match(r"^(.+?) controls (.+)$", cond)
            if m:
                team, what = m.group(1), m.group(2)
                what = re.sub(r"\s*\(\d+\)", "", what)
                (allied if _is_allied(team, allied_teams) else opfor).append(what)
                continue
            m = re.match(r"^the campaign day is at least (\d+)$", cond)
            if m:
                day = int(m.group(1))
                days = day if days is None else min(days, day)

    parts = []
    if allied:
        parts.append("Allies; Control %s" % _join(allied))
    if opfor:
        parts.append("OPFOR; Control %s" % _join(opfor))
    text = "Victory Conditions: " + (", ".join(parts) if parts
                                     else "none set in the trigger script")
    if days is not None:
        text += ". Campaign Length %d days" % days
    return text + "."


# Team names the shipped campaigns use for the allied side. Falls back to the
# team index when a campaign renames them.
_ALLIED_NAMES = {"u.s.", "us", "usa", "rok", "south korea", "japan", "allies"}


def _is_allied(team_name, allied_teams):
    name = str(team_name).strip().lower()
    if name in _ALLIED_NAMES:
        return True
    m = re.match(r"team (\d+)$", name)
    if m:
        return int(m.group(1)) in allied_teams
    return False
