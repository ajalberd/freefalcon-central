"""The campaign's scripted trigger file -- what actually ends a campaign.

A `.cam` carries no victory condition of its own. `EndgameResult` in the
header only *records* who won; what decides it is a plain-text script sitting
next to the campaign, `<scenario>.tri`, read by `ReadScriptedTriggerFile`
(`src/campaign/campupd/cmpevent.cpp`) and evaluated on every campaign tick.

The script is a flat token stream with `#IF_...` / `#ELSE` / `#ENDIF` nesting.
Conditions set a flag on a small stack; actions run when the enclosing
conditions are all true. `#END_GAME <n>` posts `FM_CAMPAIGN_OVER`, which is
the only thing in the whole engine (outside a tactical engagement) that sets
`EndgameResult` -- so a campaign with no `#END_GAME` in its script literally
cannot end except by the player abandoning it.

The condition vocabulary below is transcribed from that evaluator, including
the two easy-to-invert details:

  * `#IF_CONTROLLED <team> A|O <campIds...>` -- `A` means the team must hold
    **all** of them, anything else means **any one**.
  * `#IF_CAMPAIGN_DAY G|L <n>` -- `G` is `>=`, not `>`, so "G 14" fires on
    day 14.

Objective ids in the script are **campaign ids**, the same `campId` the
objective stream carries, which is what lets them be resolved to real places.
"""

import os
import re

# verb -> how many leading words are part of the verb's own name. The file uses
# `#IF_CONTROLLED 2 O 680 ...`, so everything after the verb is an argument.
CONDITIONS = {
    "IF_CONTROLLED", "IF_EVENT_PLAYED", "IF_CAMPAIGN_DAY", "IF_BORDOM_HOURS",
    "IF_SUPPLY", "IF_FORCE_RATIO", "IF_ON_OFFENSIVE", "IF_INITIATIVE",
    "IF_RANDOM_CHANCE", "IF_PLAYER_DIFFICULTY", "IF_TROOPS_COMMITTED",
    "IF_PRI_CONTROLLED_LT", "IF_REINFORCEMENT", "IF_MAIN_TARGET", "IF",
}

_COMMENT = re.compile(r"^\s*//")


def _find(dirpath, filename):
    want = filename.lower()
    try:
        for name in os.listdir(dirpath):
            if name.lower() == want:
                return os.path.join(dirpath, name)
    except OSError:
        pass
    return None


def script_path(campaign_dir, campaign_file):
    """`save0.cam` -> `save0.tri` in the same directory."""
    stem = os.path.splitext(campaign_file)[0]
    return _find(campaign_dir, stem + ".tri")


class Node:
    __slots__ = ("verb", "args", "line", "children", "orelse", "comment")

    def __init__(self, verb, args, line, comment=""):
        self.verb = verb
        self.args = args
        self.line = line
        self.comment = comment
        self.children = []
        self.orelse = None      # the #ELSE branch, for conditions


def parse(path):
    """-> (init actions, top-level nodes, declared event count)."""
    with open(path, "r", encoding="latin-1") as fp:
        raw = fp.readlines()

    init, body, total = [], [], 0
    stack = [body]
    branch = []              # where to append for the current #IF: children or orelse
    pending_comment = []
    in_init = True

    for n, line in enumerate(raw, 1):
        text = line.strip()
        if not text:
            continue
        if _COMMENT.match(line):
            note = text.lstrip("/").strip()
            if note:
                pending_comment.append(note)
            continue
        if not text.startswith("#"):
            continue

        parts = text.split()
        verb = parts[0][1:]
        args = parts[1:]
        comment = " ".join(pending_comment)
        pending_comment = []

        if verb == "TOTAL_EVENTS":
            total = int(args[0]) if args else 0
            continue
        if verb == "ENDINIT":
            in_init = False
            continue
        if verb == "ENDSCRIPT":
            break

        if in_init:
            init.append(Node(verb, args, n, comment))
            continue

        if verb in CONDITIONS:
            node = Node(verb, args, n, comment)
            stack[-1].append(node)
            stack.append(node.children)
            branch.append(node)
        elif verb == "ELSE":
            if branch:
                node = branch[-1]
                node.orelse = []
                stack[-1] = node.orelse
        elif verb == "ENDIF":
            if len(stack) > 1:
                stack.pop()
                if branch:
                    branch.pop()
        else:
            stack[-1].append(Node(verb, args, n, comment))

    return init, body, total


# --- turning the script into English -----------------------------------------

# Actions whose handler in `ReadScriptedTriggerFile` is commented out in this
# tree, so a script that uses them gets no effect at all. Worth saying out
# loud, because they read like real orders.
DEAD_ACTIONS = {
    "SET_TEMPO": "the handler is commented out in cmpevent.cpp",
    "CHANGE_PRIORITIES": "the handler is commented out in cmpevent.cpp",
}


def _team(idx, teams):
    try:
        name = teams[int(idx)]
    except (ValueError, IndexError, TypeError):
        return "team %s" % idx
    return name or ("team %s" % idx)


def describe(node, teams, place):
    """One node as a sentence. `place(campId)` names an objective."""
    text = _describe(node, teams, place)
    if node.verb in DEAD_ACTIONS:
        text += " \u2014 no effect in this build"
    return text


def _describe(node, teams, place):
    v, a = node.verb, node.args

    if v == "IF_CONTROLLED" and len(a) >= 3:
        joiner = " and " if a[1].upper() == "A" else " or "
        ids = [x for x in a[2:] if x.lstrip("-").isdigit()]
        return "%s controls %s" % (_team(a[0], teams),
                                   joiner.join(place(int(i)) for i in ids))
    if v == "IF_EVENT_PLAYED" and a:
        return "event %s has fired" % a[0]
    if v == "IF_CAMPAIGN_DAY" and len(a) >= 2:
        # 'G' is >= in the evaluator, despite the name.
        op = "is at least" if a[0].upper() == "G" else "is at most"
        return "the campaign day %s %s" % (op, a[1])
    if v == "IF_BORDOM_HOURS" and a:
        h = int(a[0]) if a[0].isdigit() else 0
        return ("nothing significant has happened for %s hours (%.0f days)"
                % (a[0], h / 24.0))
    if v == "IF_SUPPLY" and len(a) >= 3:
        op = "at least" if a[1].upper() == "G" else "at most"
        return "%s supply is %s %s%%" % (_team(a[0], teams), op, a[2])
    if v == "IF_FORCE_RATIO" and len(a) >= 5:
        # ratio = own * 10 / theirs, integer division, then >= or <=: "L 6" holds below
        # 70%, "G 12" at 120% and up.
        kind = {"A": "air", "G": "ground", "N": "naval"}.get(a[0].upper(), a[0])
        try:
            n = int(a[4])
            pct = ("under %d%%" % ((n + 1) * 10) if a[3].upper() != "G"
                   else "at least %d%%" % (n * 10))
        except ValueError:
            pct = a[4]
        return ("%s %s strength is %s of %s's"
                % (_team(a[1], teams), kind, pct, _team(a[2], teams)))
    if v == "IF_ON_OFFENSIVE" and a:
        return "%s is on the offensive" % _team(a[0], teams)
    if v == "IF_INITIATIVE" and a:
        return "%s has the initiative%s" % (_team(a[0], teams),
                                            " " + " ".join(a[1:]) if len(a) > 1 else "")
    if v == "IF_RANDOM_CHANCE" and a:
        return "a %s%% random roll succeeds" % a[0]
    if v == "IF_PLAYER_DIFFICULTY" and a:
        return "the difficulty setting is %s" % " ".join(a)
    if v == "IF_TROOPS_COMMITTED" and a:
        return "troops committed %s" % " ".join(a)
    if v == "IF_PRI_CONTROLLED_LT" and a:
        return "fewer than %s primary objectives are held" % " ".join(a)
    if v == "IF_REINFORCEMENT" and a:
        return "reinforcements %s" % " ".join(a)
    if v == "IF_MAIN_TARGET" and a:
        return "the main target is %s" % " ".join(a)

    # actions
    if v == "END_GAME" and a:
        return "the campaign ends, result %s" % a[0]
    if v == "DO_EVENT" and a:
        return "mark event %s as fired" % a[0]
    if v == "RESET_EVENT" and a:
        return "clear event %s" % a[0]
    if v == "PLAY_MOVIE" and a:
        return "play movie %s" % a[0]
    if v == "SET_TEMPO" and a:
        return "set the campaign tempo to %s" % a[0]
    if v == "SHIFT_INITIATIVE" and len(a) >= 3:
        return ("shift %s initiative from %s to %s"
                % (a[2], _team(a[0], teams), _team(a[1], teams)))
    if v == "CHANGE_RELATIONS" and len(a) >= 3:
        rel = {"0": "neutral to", "1": "at war with", "2": "allied with"}
        return ("%s becomes %s %s"
                % (_team(a[0], teams), rel.get(a[2], "relation " + a[2]),
                   _team(a[1], teams)))
    if v == "SET_PAK_PRIORITY" and len(a) >= 3:
        return "%s sets PAK %s priority to %s" % (_team(a[0], teams), a[1], a[2])
    if v == "CHANGE_PRIORITIES" and len(a) >= 2:
        return "%s switches to priority set %s" % (_team(a[0], teams), a[1])
    if v == "SET_EVENT" and a:
        return "start with event %s already fired" % a[0]
    if v == "RESET_BORDOM_TIMEOUT":
        return "reset the stalemate timer"

    return (v.replace("_", " ").lower() + (" " + " ".join(a) if a else "")).strip()


def controlled_targets(body):
    """Every place a `#IF_CONTROLLED` names, with its team, mode and polarity.

    `#IF_CONTROLLED` is the only condition in the vocabulary that names a
    place, so it is the only one that can be drawn on a map. `negated` marks a
    condition sitting in an `#ELSE` branch -- the team must *not* hold the
    place, which is how Korea's `save1` allied win is written -- and `endgame`
    marks one on the path to an `#END_GAME`, i.e. one that decides the campaign
    rather than firing a front-line event. `count` is how many ids that one
    condition listed, which is what makes "any of 3" meaningful.
    """
    out = []

    def walk(nodes, negated):
        reaches_end = False
        for node in nodes:
            if node.verb == "END_GAME":
                reaches_end = True
                continue
            if node.verb not in CONDITIONS:
                continue
            sub = walk(node.children, negated)
            sub_else = (walk(node.orelse, not negated)
                        if node.orelse is not None else False)
            if node.verb == "IF_CONTROLLED" and len(node.args) >= 3:
                team = int(node.args[0]) if node.args[0].isdigit() else 0
                mode = node.args[1].upper()
                ids = [int(a) for a in node.args[2:] if a.lstrip("-").isdigit()]
                for cid in ids:
                    out.append({"campId": cid, "team": team, "mode": mode,
                                "negated": negated, "count": len(ids),
                                "endgame": sub or sub_else,
                                "line": node.line})
            reaches_end = reaches_end or sub or sub_else
        return reaches_end

    walk(body, False)
    return out


def condition_counts(body):
    """How many of each condition verb a script uses, by verb."""
    out = {}

    def walk(nodes):
        for node in nodes:
            if node.verb in CONDITIONS:
                out[node.verb] = out.get(node.verb, 0) + 1
                walk(node.children)
                if node.orelse is not None:
                    walk(node.orelse)

    walk(body)
    return out


def event_usage(init, body):
    """How each campaign event is used: fired, reset, pre-set and tested.

    An event that is written but never tested by an `#IF_EVENT_PLAYED` does
    nothing except play whatever movie sits beside the write: the flag is a
    latch nothing reads, which is easy to miss when authoring a script. The
    endgame events are exempt -- they are fired immediately before an
    `#END_GAME`, so nothing is meant to test them.
    """
    out = {}
    keys = {"DO_EVENT": "fired", "RESET_EVENT": "reset", "SET_EVENT": "set",
            "IF_EVENT_PLAYED": "tested"}

    def has_endgame(nodes):
        for n in nodes:
            if n.verb == "END_GAME":
                return True
            if n.children and has_endgame(n.children):
                return True
            if n.orelse and has_endgame(n.orelse):
                return True
        return False

    def walk(nodes, endgame):
        for n in nodes:
            key = keys.get(n.verb)
            if key and n.args and n.args[0].lstrip("-").isdigit():
                rec = out.setdefault(int(n.args[0]),
                                     {"fired": 0, "reset": 0, "set": 0,
                                      "tested": 0, "endgame": 0})
                rec[key] += 1
                if endgame and key == "fired":
                    rec["endgame"] += 1
            if n.children:
                walk(n.children, endgame or has_endgame(n.children))
            if n.orelse is not None:
                walk(n.orelse, endgame or has_endgame(n.orelse))

    walk(init, False)
    walk(body, False)
    return out


def write_only_events(init, body):
    """Event ids that are set or cleared but never tested, endgames aside."""
    return sorted(i for i, u in event_usage(init, body).items()
                  if not u["tested"] and not u["endgame"]
                  and (u["fired"] or u["reset"] or u["set"]))


def endgames(body, teams, place):
    """Every way the campaign can end, with the conditions that reach it.

    Walks the tree and collects the guard chain above each `#END_GAME`, so the
    result reads as "these conditions, together, end the campaign".
    """
    out = []

    def walk(nodes, guards, notes):
        for node in nodes:
            if node.verb == "END_GAME":
                # The author's own comment sits above the guarding #IF, not the
                # #END_GAME, so carry the outermost one down.
                label = next((n for n in notes + [node.comment] if n), "")
                out.append({
                    "result": node.args[0] if node.args else "",
                    "line": node.line,
                    "comment": label,
                    "conditions": list(guards),
                })
            elif node.verb in CONDITIONS:
                walk(node.children,
                     guards + [describe(node, teams, place)],
                     notes + [node.comment])
                if node.orelse is not None:
                    walk(node.orelse,
                         guards + ["NOT (%s)" % describe(node, teams, place)],
                         notes + [node.comment])
            else:
                continue
    walk(body, [], [])
    return out


def outline(body, teams, place, depth=0):
    """A flat, readable listing of the whole script."""
    rows = []
    for node in body:
        is_cond = node.verb in CONDITIONS
        rows.append({
            "depth": depth,
            "kind": "condition" if is_cond else (
                "endgame" if node.verb == "END_GAME" else "action"),
            "verb": node.verb,
            "args": list(node.args),
            "text": describe(node, teams, place),
            "comment": node.comment,
            "line": node.line,
        })
        if is_cond:
            rows.extend(outline(node.children, teams, place, depth + 1))
            if node.orelse is not None:
                rows.append({"depth": depth, "kind": "else", "verb": "ELSE",
                             "text": "otherwise", "comment": "", "line": 0})
                rows.extend(outline(node.orelse, teams, place, depth + 1))
    return rows


# --- editing ------------------------------------------------------------------

# Which verbs the editor will rewrite, and the shape of their arguments. Every
# other line in the script is left exactly as it was found -- comments, blank
# lines, indentation and all -- because these files are hand-written and their
# layout is the only documentation they have.
EDITABLE = {
    "IF_CONTROLLED": ("team", "mode", "ids"),
    "IF_CAMPAIGN_DAY": ("cmp", "value"),
    "IF_BORDOM_HOURS": ("value",),
    "END_GAME": ("value",),
}


class Script:
    """A .tri file that can be read, edited a line at a time, and written."""

    def __init__(self, path):
        self.path = path
        with open(path, "r", encoding="latin-1", newline="") as fp:
            self.lines = fp.read().splitlines(True)
        self.init, self.body, self.total = parse(path)
        self.dirty = False

    def _rewrite(self, line_no, verb, args):
        """Replace one directive, keeping its indentation and line ending."""
        i = line_no - 1
        if not (0 <= i < len(self.lines)):
            raise IndexError("line %d is outside %s" % (line_no, self.path))
        raw = self.lines[i]
        stripped = raw.rstrip("\r\n")
        eol = raw[len(stripped):] or "\r\n"
        indent = stripped[:len(stripped) - len(stripped.lstrip())]
        body = "#" + verb + ("".join(" " + str(a) for a in args) if args else "")
        self.lines[i] = indent + body + eol
        self.dirty = True

    def edit(self, line_no, fields):
        """Change one condition or action. `fields` is keyed by EDITABLE."""
        node = self._node_at(line_no)
        if node is None:
            raise ValueError("no directive on line %d" % line_no)
        if node.verb not in EDITABLE:
            raise ValueError("%s is not editable" % node.verb)

        args = list(node.args)
        if node.verb == "IF_CONTROLLED":
            if "team" in fields:
                args[0] = int(fields["team"])
            if "mode" in fields:
                mode = str(fields["mode"]).upper()
                if mode not in ("A", "O"):
                    raise ValueError("mode must be A (all) or O (any)")
                args[1] = mode
            if "ids" in fields:
                ids = [int(v) for v in fields["ids"]]
                if not ids:
                    raise ValueError("an #IF_CONTROLLED needs at least one "
                                     "objective, or it can never fire")
                args = args[:2] + ids
        elif node.verb == "IF_CAMPAIGN_DAY":
            if "cmp" in fields:
                c = str(fields["cmp"]).upper()
                if c not in ("G", "L"):
                    raise ValueError("comparison must be G or L")
                args[0] = c
            if "value" in fields:
                args[1] = max(1, int(fields["value"]))
        else:                                   # IF_BORDOM_HOURS, END_GAME
            if "value" in fields:
                args = [max(0, int(fields["value"]))]

        self._rewrite(node.line, node.verb, args)
        # Re-parse so spans, descriptions and the tree all follow the edit.
        self.init, self.body, self.total = self._reparse()

    def _reparse(self):
        import tempfile
        fd, tmp = tempfile.mkstemp(suffix=".tri")
        os.close(fd)
        try:
            with open(tmp, "w", encoding="latin-1", newline="") as fp:
                fp.write("".join(self.lines))
            return parse(tmp)
        finally:
            try:
                os.remove(tmp)
            except OSError:
                pass

    def _node_at(self, line_no):
        found = []

        def walk(nodes):
            for n in nodes:
                if n.line == line_no:
                    found.append(n)
                walk(n.children)
                if n.orelse:
                    walk(n.orelse)
        walk(self.body)
        walk(self.init)
        return found[0] if found else None

    def save(self, path=None):
        path = path or self.path
        tmp = path + ".tmp"
        with open(tmp, "w", encoding="latin-1", newline="") as fp:
            fp.write("".join(self.lines))
        os.replace(tmp, path)
        self.dirty = False


# --- which scripted events have fired ----------------------------------------

CE_FIRED = 0x08          # EventClass flag, src/campaign/include/cmpevent.h


def fired_events(evt):
    """`.evt` member -> {event id: flags} for every event with its fired bit set.

    The member is a short count followed by (short event, short flags) pairs
    (`SaveCampaignEvents`). It records only the state, not when an event fired.
    """
    import struct
    if not evt or len(evt) < 2:
        return {}
    count = struct.unpack_from("<h", evt, 0)[0]
    out = {}
    for i in range(max(0, count)):
        at = 2 + 4 * i
        if at + 4 > len(evt):
            break
        ev, flags = struct.unpack_from("<hh", evt, at)
        if flags & CE_FIRED:
            out[ev] = flags
    return out


def event_titles(lines):
    """{event id: "China joins the war"} from the `// Event #N` comment blocks."""
    import re
    titles = {}
    cur = None
    for line in lines:
        text = line.strip()
        m = re.match(r"//\s*Event\s*#\s*(\d+)\s*$", text, re.I)
        if m:
            cur = int(m.group(1))
            continue
        if cur is not None:
            if text.startswith("//"):
                note = text.lstrip("/").strip()
                if note:
                    titles[cur] = note
                    cur = None
            elif text:
                cur = None
    return titles


def event_history(evt):
    """The history trailer of a `.evt` member -> [{"kind": "event"|"movie", "id", "time"}].

    Written by the game after the flag table (`EVT2`, count, then kind/id/time entries,
    `cmpevent.cpp`): which events fired and which news clips played, with the campaign
    time in ms. Saves from before this existed have no trailer, and give [].
    """
    import struct
    if not evt or len(evt) < 2:
        return []
    count = struct.unpack_from("<h", evt, 0)[0]
    at = 2 + 4 * max(0, count)
    if at + 6 > len(evt) or evt[at:at + 4] != b"EVT2":
        return []
    n = struct.unpack_from("<h", evt, at + 4)[0]
    out = []
    for i in range(max(0, n)):
        off = at + 6 + 8 * i
        if off + 8 > len(evt):
            break
        kind, ident, t = struct.unpack_from("<hhI", evt, off)
        out.append({"kind": "movie" if kind else "event", "id": ident, "time": t})
    return out


def condition_history(evt):
    """The condition trailer of a `.evt` member -> [{"line", "branch", "depth", "time", "a", "b"}].

    Written after the event history (`CND1`, count, entries; `cmpevent.cpp`): when an action that
    changes the war ran, every `#IF` around it -- its line in the .tri, `branch` 0 if its condition
    held or 1 if it was taken through its `#ELSE`, and what it measured (`a`, `b`; None if
    nothing: supply % in a, both sides' strength in a and b, the roll, the objective decided...).
    Saves from before this existed give [].
    """
    import struct
    if not evt or len(evt) < 2:
        return []
    count = struct.unpack_from("<h", evt, 0)[0]
    at = 2 + 4 * max(0, count)
    if at + 6 > len(evt) or evt[at:at + 4] != b"EVT2":
        return []
    n = struct.unpack_from("<h", evt, at + 4)[0]
    at += 6 + 8 * max(0, n)
    if at + 6 > len(evt) or evt[at:at + 4] != b"CND1":
        return []
    m = struct.unpack_from("<h", evt, at + 4)[0]
    out = []
    none = -0x80000000
    for i in range(max(0, m)):
        off = at + 6 + 20 * i
        if off + 20 > len(evt):
            break
        line, branch, depth, t, a, b = struct.unpack_from("<ihhIii", evt, off)
        out.append({"line": line, "branch": branch, "depth": depth, "time": t,
                    "a": None if a == none else a, "b": None if b == none else b})
    return out


STAT_FIELDS = ("airDefenseVehs", "aircraft", "groundVehs", "ships", "supply", "fuel",
               "airbases", "supplyLevel", "fuelLevel")


def force_history(frc):
    """A save's `.frc` file -> [(time ms, [per-team stats])], oldest first.

    `RecalculateStatistics` (team.cpp) appends each team's current stats, the same numbers the
    script's #IF_SUPPLY and #IF_FORCE_RATIO read, every time it recounts (hourly), so the last
    record at or before a trigger's time is what that trigger saw.
    """
    import struct
    out = []
    p = 0
    while frc and p + 6 <= len(frc):
        t, n = struct.unpack_from("<Ih", frc, p)
        p += 6
        if n <= 0 or p + 16 * n > len(frc):
            break
        out.append((t, [dict(zip(STAT_FIELDS, struct.unpack_from("<7H2B", frc, p + 16 * k)))
                        for k in range(n)]))
        p += 16 * n
    return out


def _measure(node, stats, fired_before):
    """Re-evaluate one condition from saved numbers -> (holds: True/False/None, a, b).

    Only what a save records can be re-evaluated: team stats (from the .frc) and which events
    had fired. Everything else (who held an objective then, a random roll) gives None.
    """
    v, a = node.verb, node.args
    try:
        if v == "IF_SUPPLY" and len(a) >= 3 and stats:
            lvl = stats[int(a[0])]["supplyLevel"]
            return ((lvl >= int(a[2])) if a[1].upper() == "G" else (lvl <= int(a[2]))), lvl, None
        if v == "IF_FORCE_RATIO" and len(a) >= 5 and stats:
            key = {"A": ("aircraft",), "G": ("groundVehs",), "N": ("ships",)}.get(
                a[0].upper(), ("groundVehs", "aircraft"))
            os_ = sum(stats[int(a[1])][k] for k in key)
            ts = sum(stats[int(a[2])][k] for k in key)
            ratio = os_ * 10 // ts if ts else (1 << 30 if os_ else 0)
            i = int(a[4])
            return ((ratio >= i) if a[3].upper() == "G" else (ratio <= i)), os_, ts
        if v == "IF_EVENT_PLAYED" and a:
            held = int(a[0]) in fired_before
            return held, int(held), None
    except (ValueError, IndexError, KeyError):
        pass
    return None, None, None


def branch_report(body, history, conds, frc_hist):
    """Which `#IF` branches led to what happened -> {tri line: [taken, ...]}.

    `taken` is {"time", "branch": "if"|"else", "a", "b", "source"}. From the save's own record
    (`condition_history`) when it has one -- source "recorded". Otherwise reconstructed: for each
    event in the history, every `#DO_EVENT` of it whose guard chain re-evaluates true at that
    moment from the .frc stats -- source "reconstructed", or "possible" when some guard cannot be
    re-evaluated from a save.
    """
    out = {}
    for c in conds:
        out.setdefault(c["line"], []).append({
            "time": c["time"], "branch": "else" if c["branch"] else "if",
            "a": c["a"], "b": c["b"], "source": "recorded"})
    recorded_times = {c["time"] for c in conds}

    fires = [h for h in history if h["kind"] == "event"]

    def chains(nodes, guards, ev, acc):
        for n in nodes:
            if n.verb == "DO_EVENT" and n.args and n.args[0].lstrip("-").isdigit() \
                    and int(n.args[0]) == ev:
                acc.append(list(guards))
            elif n.verb in CONDITIONS:
                chains(n.children, guards + [(n, True)], ev, acc)
                if n.orelse is not None:
                    chains(n.orelse, guards + [(n, False)], ev, acc)
        return acc

    for f in fires:
        t = f["time"]
        if t in recorded_times:
            continue
        before = [s for s in frc_hist if s[0] <= t]
        stats = before[-1][1] if before else None
        fired_before = {h["id"] for h in fires if h["time"] < t}
        found = []
        for guards in chains(body, [], f["id"], []):
            verdicts = []
            for node, want in guards:
                holds, a, b = _measure(node, stats, fired_before)
                ok = None if holds is None else (holds == want)
                verdicts.append((node, want, ok, a, b))
            if all(v[2] is not False for v in verdicts):
                found.append(verdicts)
        sure = [g for g in found if all(v[2] for v in g)]
        pick = sure if len(sure) == 1 else found
        for g in pick:
            src = "reconstructed" if all(v[2] for v in g) else "possible"
            for node, want, _ok, a, b in g:
                out.setdefault(node.line, []).append({
                    "time": t, "branch": "if" if want else "else", "a": a, "b": b,
                    "source": src})
    return out


def describe_measure(node, a, b, teams):
    """What a condition measured, in words ("DPRK 258 vs ROK 391 = 6.6")."""
    v, args = node.verb, node.args
    if a is None:
        return ""
    if v == "IF_SUPPLY":
        return "supply was %d%%" % a
    if v == "IF_FORCE_RATIO" and b is not None and len(args) >= 3:
        r = ("%.1f" % (a * 10.0 / b)) if b else "n/a"
        return "%s %d vs %s %d (ratio %s, compared as %s)" % (
            _team(args[1], teams), a, _team(args[2], teams), b, r,
            (a * 10 // b) if b else "max")
    if v == "IF_EVENT_PLAYED":
        return "it had fired" if a else "it had not fired yet"
    if v == "IF_CONTROLLED":
        return "objective %d held by %s" % (a, _team(b, teams)) if b is not None else ""
    if v == "IF_RANDOM_CHANCE":
        return "rolled %d" % a
    if v == "IF_CAMPAIGN_DAY":
        return "day %d" % a
    if v == "IF_BORDOM_HOURS":
        return "%d quiet hours" % a
    if v in ("IF_INITIATIVE", "IF_REINFORCEMENT", "IF_PLAYER_DIFFICULTY", "IF_PRI_CONTROLLED_LT"):
        return "value was %d" % a
    return ""


def describe_time(ms, day_zero):
    """Campaign ms -> (campaign day starting at 1, "HH:MM")."""
    day = ms // 86400000
    return int(day - day_zero + 1), "%02d:%02d" % ((ms // 3600000) % 24, (ms // 60000) % 60)
