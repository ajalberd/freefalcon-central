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

def _team(idx, teams):
    try:
        name = teams[int(idx)]
    except (ValueError, IndexError, TypeError):
        return "team %s" % idx
    return name or ("team %s" % idx)


def describe(node, teams, place):
    """One node as a sentence. `place(campId)` names an objective."""
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
        op = "above" if a[1].upper() == "G" else "below"
        return "%s supply is %s %s%%" % (_team(a[0], teams), op, a[2])
    if v == "IF_FORCE_RATIO" and len(a) >= 5:
        kind = {"A": "air", "G": "ground", "N": "naval"}.get(a[0].upper(), a[0])
        op = "above" if a[3].upper() == "G" else "below"
        return ("%s %s strength against %s is %s %s"
                % (_team(a[1], teams), kind, _team(a[2], teams), op, a[4]))
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
