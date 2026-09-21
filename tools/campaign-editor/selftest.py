#!/usr/bin/env python3
"""Check the editor's format code against the game's own data files.

Every table is read and written back in memory; the bytes must come out
identical. That is the only guarantee that matters here -- a layout that is
one byte off still "parses", it just silently shifts every field, and the
first sign would be the campaign engine reading garbage.

Usage:
    python selftest.py [game-dir]          default C:\\FreeFalcon6
"""

import os
import shutil
import sys
import tempfile
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from ffcamp import (campdb, campfile, entities, lzss,  # noqa: E402
                    names, objectives, records, tacan, terrain,
                    theater, triggers, uiart, camptext)

fails = []
checks = 0


def check(cond, msg):
    global checks
    checks += 1
    if not cond:
        fails.append(msg)
        print("  FAIL  " + msg)
    return cond


def test_lzss():
    print("LZSS round-trip")
    for blob in (b"", b"a", b"the quick brown fox " * 64, bytes(range(256)) * 9):
        enc = lzss.compress(blob)
        check(lzss.expand(enc, len(blob)) == blob,
              "literal round-trip failed for %d bytes" % len(blob))


def test_specs():
    print("Record layouts")
    for spec in records.ALL_SPECS:
        check(spec.struct.size == spec.size,
              "%s size %d != %d" % (spec.name, spec.struct.size, spec.size))


def test_campdb(dbdir):
    print("CampaignDB round-trip: %s" % dbdir)
    db = campdb.CampaignDB(dbdir)
    for name in db.present():
        tbl = db.table(name)
        with open(tbl.path, "rb") as fp:
            original = fp.read()
        rebuilt = tbl.to_bytes()
        ok = check(rebuilt == original,
                   "%s: %d bytes out vs %d in"
                   % (os.path.basename(tbl.path), len(rebuilt), len(original)))
        if ok:
            print("  ok    %-14s %5d rows x %3d bytes"
                  % (os.path.basename(tbl.path), len(tbl.rows), tbl.spec.size))
        elif len(rebuilt) == len(original):
            for i, (a, b) in enumerate(zip(original, rebuilt)):
                if a != b:
                    print("        first difference at byte %d (row %d, +%d)"
                          % (i, (i - 2) // tbl.spec.size,
                             (i - 2) % tbl.spec.size))
                    break

    # The class table must resolve into the per-type tables.
    named = db.name_index()
    check(len(named) > 1000,
          "only %d class-table entries resolved to a name" % len(named))
    print("  ok    class table resolves %d names, e.g. %s"
          % (len(named), ", ".join(list(named.values())[100:104])))


def test_campfile(path):
    print("Campaign file: %s" % os.path.basename(path))
    cam = campfile.CampaignFile.load(path)
    check(cam.version > 0, "no version in %s" % path)
    hdr = cam.header
    check(hdr is not None, "no .cmp section in %s" % path)
    if hdr is None:
        return
    check(hdr.trailing == 0,
          "%d bytes left over after decoding the header" % hdr.trailing)
    print("  ok    v%d  theater=%r scenario=%r teams=%d squadrons=%d"
          % (cam.version, hdr.fields["TheaterName"], hdr.fields["Scenario"],
             hdr.fields["ActiveTeams"], len(hdr.squadrons)))

    # Re-encode without editing anything: the decoded stream must be identical.
    raw = hdr.encode()
    check(raw == hdr.raw,
          "header re-encode differs (%d vs %d bytes)" % (len(raw), len(hdr.raw)))

    # And the container must rebuild with every member intact.
    rebuilt = campfile.build_container(cam.members, cam.order)
    again = campfile.read_container(rebuilt)
    check(list(again) == list(cam.members),
          "container rebuild lost or reordered members")
    check(all(again[k] == cam.members[k] for k in cam.members),
          "container rebuild changed member contents")


def test_units(path, class_rows):
    """Decode the unit stream, then prove a patched re-encode is exact."""
    cam = campfile.CampaignFile.load(path)
    section = cam.member("uni")
    if section is None:
        return
    name = os.path.basename(path)
    try:
        units, raw = entities.decode_units(section, cam.version, class_rows)
    except Exception as exc:
        check(False, "%s: unit stream: %s" % (name, exc))
        return

    kinds = {}
    for u in units:
        kinds[u["kind"]] = kinds.get(u["kind"], 0) + 1
    print("  ok    %-28s %5d units  %s"
          % (name, len(units), ", ".join("%s %d" % kv
                                         for kv in sorted(kinds.items()))))

    if not units:
        return

    # A re-encode with nothing changed must decode back to the same units.
    again, raw2 = entities.decode_units(
        entities.encode_units(raw, len(units)), cam.version, class_rows)
    check(raw2 == raw, "%s: re-encoded stream differs" % name)
    check(len(again) == len(units), "%s: re-encode changed the unit count" % name)

    # Patching one field must change that field and nothing else.
    target = units[len(units) // 2]
    moved = entities.patch_unit(raw, target, cam.version, "x", target["x"] + 3)
    check(sum(1 for a, b in zip(raw, moved) if a != b) <= 2,
          "%s: patching x touched more than two bytes" % name)
    after, _ = entities.decode_units(
        entities.encode_units(moved, len(units)), cam.version, class_rows)
    check(after[target["_n"]]["x"] == target["x"] + 3,
          "%s: patched x did not survive the round trip" % name)
    others_same = all(
        after[i]["x"] == units[i]["x"] and after[i]["y"] == units[i]["y"]
        and after[i]["owner"] == units[i]["owner"]
        for i in range(len(units)) if i != target["_n"])
    check(others_same, "%s: patching one unit disturbed another" % name)


def test_objectives(path, class_rows, nametab):
    """Decode the objective stream and prove a patched re-encode is exact."""
    cam = campfile.CampaignFile.load(path)
    section = cam.member("obj")
    if section is None:
        return
    name = os.path.basename(path)
    try:
        objs, raw = objectives.decode_objectives(
            section, cam.version, class_rows)
    except Exception as exc:
        check(False, "%s: objective stream: %s" % (name, exc))
        return

    cats = {}
    for o in objs:
        cats[o["category"]] = cats.get(o["category"], 0) + 1
    named = sum(1 for o in objs if nametab.get(o["nameId"]))
    print("  ok    %-24s %5d objectives, %d named  %s"
          % (name, len(objs), named,
             ", ".join("%s %d" % kv for kv in sorted(cats.items()))))
    if not objs:
        return

    again, raw2 = objectives.decode_objectives(
        objectives.encode_objectives(raw, len(objs)), cam.version, class_rows)
    check(raw2 == raw, "%s: objective re-encode differs" % name)
    check(len(again) == len(objs),
          "%s: objective re-encode changed the count" % name)

    target = objs[len(objs) // 2]
    moved = objectives.patch_objective(
        raw, target, cam.version, "x", target["x"] + 3)
    check(sum(1 for a, b in zip(raw, moved) if a != b) <= 2,
          "%s: patching an objective x touched more than two bytes" % name)
    after, _ = objectives.decode_objectives(
        objectives.encode_objectives(moved, len(objs)), cam.version, class_rows)
    check(after[target["_n"]]["x"] == target["x"] + 3,
          "%s: patched objective x did not survive the round trip" % name)
    check(all(after[i]["x"] == objs[i]["x"] and after[i]["y"] == objs[i]["y"]
              for i in range(len(objs)) if i != target["_n"]),
          "%s: patching one objective disturbed another" % name)


def test_place_unit(path, class_rows, db):
    """Build a unit from scratch, append it, and read it back."""
    cam = campfile.CampaignFile.load(path)
    section = cam.member("uni")
    if section is None or cam.version < 71:
        return
    name = os.path.basename(path)
    units, raw = entities.decode_units(section, cam.version, class_rows)
    if not units:
        return

    utbl = db.table("unit")
    target = None
    for i, r in enumerate(class_rows):
        info = r["classInfo_"]
        if info[1] != 6:
            continue
        table = entities.DISPATCH.get(info[0])
        entry = table.get(info[2]) if table else None
        if entry and entry[0] == "battalion":
            target = i
            break
    if target is None:
        return

    drow = utbl.rows[class_rows[target]["dataPtr"]] if utbl else None
    vu_id, camp_id = entities.next_ids(units)
    record = entities.build_unit(
        "battalion", cam.version, target + entities.VU_LAST_ENTITY_TYPE,
        x=123, y=456, owner=2, vu_id=vu_id, camp_id=camp_id,
        roster=entities.roster_from_class(drow))

    bigger = entities.append_unit(raw, record)
    grown = entities.walk_units(bigger, cam.version, class_rows)
    check(len(grown) == len(units) + 1,
          "%s: appending a unit did not add exactly one" % name)
    fresh = grown[-1]
    check((fresh["x"], fresh["y"], fresh["owner"], fresh["campId"]) ==
          (123, 456, 2, camp_id),
          "%s: the appended unit read back wrong" % name)
    check(fresh["kind"] == "battalion",
          "%s: the appended unit decoded as %s" % (name, fresh["kind"]))
    check(all(grown[i]["_span"] == units[i]["_span"] for i in range(len(units))),
          "%s: appending a unit moved an existing one" % name)

    back = entities.delete_unit(bigger, grown[-1])
    check(back == raw, "%s: append then delete did not restore the stream" % name)
    print("  ok    %-24s built a %d-byte battalion, appended and removed it"
          % (name, len(record)))


def test_icons(gamedir, class_rows, db):
    """The campaign map's own icon art, and whether the tables point into it."""
    print("Campaign-map icons")
    art = uiart.UiArt(gamedir)
    if not check(art.available(), "no imageids.id / imagerc.irc under %s" % gamedir):
        return

    ground = art.icon_set("red")
    if not check(ground is not None, "no red team icon set"):
        return
    check(len(ground.icons) > 50,
          "red icon set has only %d icons" % len(ground.icons))
    for want in ("ICON_INFANTRY", "ICON_ARMOR", "ICON_TOWN", "ICON_AIRDEFENSE"):
        check(want in ground.icons, "the icon set has no %s" % want)

    # A decoded icon must have a transparent background and coloured pixels:
    # reading the .rsc from byte zero instead of past its 8-byte header gives
    # an all-black palette, which still "works" and is silently wrong.
    got = ground.rgba("ICON_INFANTRY")
    if check(got is not None, "ICON_INFANTRY did not decode"):
        w, h, rgba = got
        check(w == 23 and h == 16,
              "ICON_INFANTRY is %dx%d, expected 23x16" % (w, h))
        alphas = set(rgba[3::4])
        check(0 in alphas, "ICON_INFANTRY has no transparent pixels")
        check(255 in alphas, "ICON_INFANTRY has no opaque pixels")
        lit = sum(1 for i in range(0, len(rgba), 4)
                  if rgba[i + 3] and (rgba[i] or rgba[i + 1] or rgba[i + 2]))
        check(lit > 50, "ICON_INFANTRY decoded to %d coloured pixels" % lit)
        print("  ok    ICON_INFANTRY %dx%d, %d coloured pixels" % (w, h, lit))

    for colour in uiart.TEAM_COLOURS:
        st = art.icon_set(colour)
        check(st is not None and len(st.icons) > 50,
              "team colour %r has no usable icon set" % colour)

    atlas = uiart.IconAtlas(ground)
    check(atlas.png[:8] == b"\x89PNG\r\n\x1a\n", "the atlas is not a PNG")
    check(len(atlas.frames) == len(ground.icons),
          "atlas packed %d of %d icons" % (len(atlas.frames), len(ground.icons)))
    for name, f in atlas.frames.items():
        check(f["x"] >= 0 and f["y"] >= 0 and
              f["x"] + f["w"] <= atlas.width and
              f["y"] + f["h"] <= atlas.height,
              "atlas frame %s falls outside the sheet" % name)
    print("  ok    atlas %dx%d, %d frames, %d byte PNG"
          % (atlas.width, atlas.height, len(atlas.frames), len(atlas.png)))

    # Every unit and objective type should resolve to an icon that exists.
    air = None
    rel = art.resources.get("RED_AIR_NORTH")
    if rel:
        base = art._resolve(rel)
        if base:
            air = uiart.IconSet(base)
    have = set(ground.icons) | (set(air.icons) if air else set())

    for table_name in ("unit", "objective"):
        tbl = db.table(table_name)
        if not tbl:
            continue
        total = resolved = 0
        for r in tbl.rows:
            icon = r.get("IconIndex", 0)
            if not icon:
                continue
            total += 1
            if art.icon_name(icon) in have:
                resolved += 1
        check(total and resolved * 100 // total >= 95,
              "%s: only %d of %d IconIndex values resolve"
              % (table_name, resolved, total))
        print("  ok    %-10s %d of %d rows with an IconIndex resolve"
              % (table_name, resolved, total))


def test_tacan(campaign_dir, class_rows, cam_path):
    """stations.dat, and whether its ids land on airbases."""
    stations = tacan.load(campaign_dir)
    name = os.path.basename(campaign_dir)
    if not stations:
        print("  --    %s has no stations.dat" % name)
        return
    for camp_id, st in stations.items():
        check(1 <= st["channel"] <= 126,
              "%s: TACAN channel %d out of range" % (name, st["channel"]))
        check(st["band"] in ("X", "Y"),
              "%s: TACAN band %r" % (name, st["band"]))

    cam = campfile.CampaignFile.load(cam_path)
    section = cam.member("obj")
    if section is None:
        return
    objs, _raw = objectives.decode_objectives(section, cam.version, class_rows)
    by_camp = {o["campId"]: o for o in objs}
    matched = [o for cid, o in by_camp.items() if cid in stations]
    airbases = sum(1 for o in matched if o["category"] == "airbase")
    check(len(matched) >= len(stations) * 0.9,
          "%s: only %d of %d stations match an objective"
          % (name, len(matched), len(stations)))
    check(airbases >= len(matched) * 0.9,
          "%s: only %d of %d matched stations are airbases"
          % (name, airbases, len(matched)))
    print("  ok    %-12s %3d stations, %3d match an objective, %3d of those "
          "are airbases" % (name, len(stations), len(matched), airbases))


def test_terrain(gamedir):
    """The ground-imagery pyramid: geometry, orientation and render cost."""
    print("Terrain")
    if not terrain.available():
        print("  --    numpy is not installed, terrain rendering is off")
        return
    root = os.path.join(gamedir, "terrdata", "korea")
    if not os.path.isdir(root):
        print("  --    no terrdata/korea to render")
        return

    t = terrain.Terrain(root)
    check(t.size_km == 1024,
          "theater is %d km across, expected 1024" % t.size_km)
    check(abs(t.feet_per_post - 820) < 1,
          "%.1f ft per post, expected ~820" % t.feet_per_post)
    # 4 posts to a ground tile and 820 ft to a post is 3280 ft, one kilometre:
    # that identity is what lets a campaign coordinate index a tile directly.
    km_ft = t.feet_per_post * 4
    check(abs(km_ft - 3280.84) < 2,
          "a ground tile covers %.0f ft, expected one km" % km_ft)

    g = t.grid()
    check(g.shape == (t.size_km, t.size_km),
          "texID grid is %s, expected one entry per km" % (g.shape,))
    missing = [int(v) for v in set(g.ravel().tolist()[:200000])
               if not t.tile_path(int(v))]
    check(not missing, "texIDs with no tile: %s" % missing[:4])

    import time
    for z in (0, 4, 6, 8, 10):
        span = 1 << z
        tx = ty = span // 2
        t0 = time.time()
        png = t.render_png(z, tx, ty)
        dt = time.time() - t0
        if not check(png is not None, "z=%d tile did not render" % z):
            continue
        check(png[:8] == b"\x89PNG\r\n\x1a\n", "z=%d tile is not a PNG" % z)
        check(dt < 5.0, "z=%d tile took %.1fs" % (z, dt))
        print("  ok    z=%-2d %5d bytes in %.2fs (one tile covers %4d km)"
              % (z, len(png), dt, t.size_km >> z))

    check(t.render(0, 1, 0) is None, "out-of-range tile was not rejected")
    check(t.render(3, 99, 0) is None, "out-of-range tile was not rejected")

    # The server answers tile requests on several threads at once, so the same
    # tile must come out byte for byte the same whichever thread asks for it,
    # and concurrent misses must not corrupt the shared caches. The baseline
    # comes from a second instance, leaving the tiles cold on `t`.
    want = {}
    probe = terrain.Terrain(root)
    for z in (5, 6, 9):
        span = 1 << z
        want[(z, span // 3, span // 4)] = probe.render_png(z, span // 3, span // 4)
    del probe

    got, errs = {}, []
    lock = threading.Lock()

    def work():
        for key in sorted(want):
            try:
                blob = t.render_png(*key)
            except Exception as exc:               # noqa: BLE001
                with lock:
                    errs.append(repr(exc))
            else:
                with lock:
                    got[key] = blob

    threads = [threading.Thread(target=work) for _ in range(8)]
    for th in threads:
        th.start()
    for th in threads:
        th.join()
    check(not errs, "concurrent terrain render failed: %s" % errs[:2])
    mismatched = [k for k, v in got.items() if v != want.get(k)]
    check(not mismatched,
          "concurrent render changed tiles: %s" % mismatched[:4])
    print("  ok    8 threads, %d tiles rendered identically"
          % (len(want) * 8))


def test_triggers(campaign_dir, campaign_file, class_rows, db, nametab):
    """The .tri script: does it parse, and does it actually end the campaign?"""
    path = triggers.script_path(campaign_dir, campaign_file)
    if not path:
        return
    label = "%s/%s" % (os.path.basename(campaign_dir), campaign_file)
    try:
        init, body, total = triggers.parse(path)
    except Exception as exc:
        check(False, "%s: trigger script: %s" % (label, exc))
        return

    cam = campfile.CampaignFile.load(os.path.join(campaign_dir, campaign_file))
    section = cam.member("obj")
    by_camp = {}
    if section is not None:
        objs, _raw = objectives.decode_objectives(
            section, cam.version, class_rows)
        by_camp = {o["campId"]: o for o in objs}
    cls = db.name_index()
    teams = [t["name"] for t in (cam.header.teams if cam.header else [])]

    def place(camp_id):
        o = by_camp.get(camp_id)
        if not o:
            return "objective %d" % camp_id
        nm = nametab.get(o["nameId"], "").strip()
        if nm in ("", "Nowhere", "New"):
            nm = cls.get(o["classIndex"], "")
        return "%s (%d)" % (nm or camp_id, camp_id)

    ends = triggers.endgames(body, teams, place)
    check(bool(ends),
          "%s: script has no #END_GAME, so the campaign cannot end" % label)
    for e in ends:
        check(e["conditions"],
              "%s: #END_GAME %s has no guarding condition"
              % (label, e["result"]))
        for text in e["conditions"]:
            check(text and not text.startswith("if "),
                  "%s: condition did not render: %r" % (label, text))

    # Campaign ids the script names that no objective carries. The engine
    # skips a missing id (`if (o and ...)`), so in an AND list it reads as
    # satisfied and in an OR list it can never fire. The shipped front-line
    # events do contain a few of these, so this is reported, not failed --
    # except in an endgame condition, where it would make a victory
    # unreachable.
    unknown = []

    def walk(nodes):
        for node in nodes:
            if node.verb == "IF_CONTROLLED" and len(node.args) >= 3:
                for raw in node.args[2:]:
                    if raw.lstrip("-").isdigit() and int(raw) not in by_camp:
                        unknown.append(int(raw))
            walk(node.children)
            if node.orelse:
                walk(node.orelse)
    walk(body)

    dead_endgame = []
    for e in ends:
        for text in e["conditions"]:
            for stray in set(unknown):
                if ("(%d)" % stray) in text:
                    dead_endgame.append((e["result"], stray))
    check(not dead_endgame,
          "%s: an endgame condition names a missing objective: %s"
          % (label, dead_endgame[:4]))

    rows = triggers.outline(body, teams, place)
    check(len(rows) > 10, "%s: script outline is only %d rows" % (label, len(rows)))
    note = ("   (%d ids name nothing in this campaign)" % len(set(unknown))
            if unknown else "")
    print("  ok    %-24s %2d events, %d endgames, %3d script rows%s"
          % (label, total, len(ends), len(rows), note))


def test_script_editing(campaign_dir, campaign_file):
    """An edit must touch exactly one line and survive a re-read.

    These files are hand-written and their layout is the only documentation
    they have, so anything but a surgical rewrite is a bug.
    """
    path = triggers.script_path(campaign_dir, campaign_file)
    if not path:
        return
    label = "%s/%s" % (os.path.basename(campaign_dir), campaign_file)

    tmp = tempfile.mkdtemp(prefix="ffcamp-tri-")
    try:
        copy = os.path.join(tmp, os.path.basename(path))
        shutil.copy2(path, copy)
        before = open(copy, "rb").read()

        sc = triggers.Script(copy)
        targets = []

        def walk(nodes):
            for n in nodes:
                if n.verb in triggers.EDITABLE:
                    targets.append(n)
                walk(n.children)
                if n.orelse:
                    walk(n.orelse)
        walk(sc.body)
        if not targets:
            return

        edited = 0
        for node in targets[:6]:
            args = list(node.args)
            if node.verb == "IF_CONTROLLED":
                fields = {"mode": "A" if args[1].upper() == "O" else "O"}
            elif node.verb == "IF_CAMPAIGN_DAY":
                fields = {"value": int(args[1]) + 1}
            else:
                fields = {"value": int(args[0]) + 1}
            sc.edit(node.line, fields)
            edited += 1
        sc.save()

        after = open(copy, "rb").read()
        a, b = before.split(b"\n"), after.split(b"\n")
        check(len(a) == len(b),
              "%s: editing changed the line count (%d -> %d)"
              % (label, len(a), len(b)))
        moved = [i + 1 for i, (x, y) in enumerate(zip(a, b)) if x != y]
        check(len(moved) == edited,
              "%s: %d edits rewrote %d lines" % (label, edited, len(moved)))

        # And it must still parse to the same shape.
        init2, body2, total2 = triggers.parse(copy)
        check(total2 == sc.total,
              "%s: event count drifted after editing" % label)
        check(len(triggers.endgames(body2, [], lambda i: str(i)))
              == len(triggers.endgames(sc.body, [], lambda i: str(i))),
              "%s: endgame count drifted after editing" % label)

        # A bad value must be refused, not written.
        stamp = open(copy, "rb").read()
        for node in targets:
            if node.verb == "IF_CONTROLLED":
                sc2 = triggers.Script(copy)
                try:
                    sc2.edit(node.line, {"ids": []})
                    check(False, "%s: an empty #IF_CONTROLLED was accepted"
                          % label)
                except ValueError:
                    pass
                try:
                    sc2.edit(node.line, {"mode": "X"})
                    check(False, "%s: mode X was accepted" % label)
                except ValueError:
                    pass
                break
        check(open(copy, "rb").read() == stamp,
              "%s: a refused edit still touched the file" % label)
        print("  ok    %-24s %d edits, %d lines rewritten"
              % (label, edited, len(moved)))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_campaign_text(gamedir, tdf):
    """The selection text: located, round-trips, and only one line moves."""
    path = camptext.locate(gamedir, tdf.get("artdir"))
    label = tdf.name.strip()
    if not path:
        print("  --    %-24s no lcktxtrc.irc" % label)
        return

    tmp = tempfile.mkdtemp(prefix="ffcamp-txt-")
    try:
        copy = os.path.join(tmp, "lcktxtrc.irc")
        shutil.copy2(path, copy)
        before = open(copy, "rb").read()

        tf = camptext.TextFile(copy)
        slots = [n for n in (1, 2, 3) if tf.get("TXT_SCENARIO_%d" % n)]
        check(bool(slots), "%s: no TXT_SCENARIO_n in %s" % (label, path))

        probe = "Victory Conditions: round-trip probe."
        tf.set("TXT_SC_%d" % slots[0], probe)
        tf.save()

        a, b = before.split(b"\n"), open(copy, "rb").read().split(b"\n")
        check(len(a) == len(b), "%s: text edit changed the line count" % label)
        moved = [i for i, (x, y) in enumerate(zip(a, b)) if x != y]
        check(len(moved) == 1,
              "%s: one text edit rewrote %d lines" % (label, len(moved)))

        back = camptext.TextFile(copy)
        check(back.get("TXT_SC_%d" % slots[0]) == probe,
              "%s: text did not survive the round trip" % label)
        orig = camptext.TextFile(path)
        for n in slots[1:]:
            for key in ("TXT_SCENARIO_%d" % n, "TXT_SC_%d" % n):
                check(back.get(key) == orig.get(key),
                      "%s: editing one slot disturbed %s" % (label, key))

        # A quote would end the string early; the format has no escape for it.
        back.set("TXT_SCENARIO_%d" % slots[0], 'a "quoted" name')
        check('"' not in back.get("TXT_SCENARIO_%d" % slots[0]),
              "%s: a double quote survived into the value" % label)

        check(camptext.slot_of("save1.cam") == 2,
              "slot_of(save1.cam) should be 2")
        check(camptext.slot_of("tanker.tac") == 0,
              "slot_of on a non-campaign should be 0")
        print("  ok    %-24s %d slots, %s"
              % (label, len(slots), os.path.relpath(path, gamedir)))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_theaters(gamedir):
    print("Theater list: %s" % gamedir)
    ths = theater.load_theaters(gamedir)
    check(len(ths) > 0, "no theaters found")
    for t in ths:
        print("  ok    %-22s campaigndir=%s" % (t.name, t.get("campaigndir")))
        check(os.path.isdir(t.campaign_dir(gamedir)),
              "%s: campaigndir %r does not exist"
              % (t.name, t.get("campaigndir")))


def main():
    gamedir = sys.argv[1] if len(sys.argv) > 1 else r"C:\FreeFalcon6"
    test_lzss()
    test_specs()
    if not os.path.isdir(gamedir):
        print("\n%s not found -- skipping the data-file checks." % gamedir)
    else:
        test_theaters(gamedir)

        # Walk the theaters, not the campaign/ folder: a theater may keep its
        # DB in objectdir (the Israel family does) or share tables with a
        # sibling, so resolve each one the way the server does.
        camps = []
        for t in theater.load_theaters(gamedir):
            camp = t.campaign_dir(gamedir)
            dbdir = theater.db_dir(gamedir, t)
            if os.path.isdir(camp) and os.path.isdir(dbdir):
                camps.append((camp, dbdir))
        check(bool(camps), "no theater with both a campaign dir and a CampaignDB")

        if camps:
            db = campdb.CampaignDB(camps[0][1])
            test_icons(gamedir, db.class_rows(), db)
        test_terrain(gamedir)

        for camp, dbdir in camps:
            print("Trigger scripts: %s" % os.path.relpath(camp, gamedir))
            db = campdb.CampaignDB(dbdir)
            rows = db.class_rows()
            nametab = names.load(camp)
            for f in sorted(os.listdir(camp)):
                if f.lower().endswith((".cam", ".tac")):
                    test_triggers(camp, f, rows, db, nametab)
                    test_script_editing(camp, f)

        print("Campaign selection text")
        for tdf in theater.load_theaters(gamedir):
            test_campaign_text(gamedir, tdf)

        print("TACAN stations")
        for camp, dbdir in camps:
            base = os.path.join(camp, "save0.cam")
            if os.path.isfile(base):
                rows = campdb.CampaignDB(dbdir).class_rows()
                test_tacan(camp, rows, base)
        dbs_done = set()
        for _camp, dbdir in camps:
            key = os.path.normcase(os.path.abspath(dbdir))
            if key in dbs_done:
                continue
            dbs_done.add(key)
            test_campdb(dbdir)
        for camp, _dbdir in camps:
            for f in sorted(os.listdir(camp)):
                if f.lower().endswith((".cam", ".tac")):
                    test_campfile(os.path.join(camp, f))

        camps_done = set()
        for camp, dbdir in camps:
            key = os.path.normcase(os.path.abspath(camp))
            if key in camps_done:
                continue
            camps_done.add(key)
            print("Streams: %s" % os.path.relpath(camp, gamedir))
            db = campdb.CampaignDB(dbdir)
            rows = db.class_rows()
            nametab = names.load(camp)
            check(len(nametab) > 0, "%s: no place-name table" % camp)
            for f in sorted(os.listdir(camp)):
                if f.lower().endswith((".cam", ".tac")):
                    test_units(os.path.join(camp, f), rows)
                    test_objectives(os.path.join(camp, f), rows, nametab)
            for f in sorted(os.listdir(camp)):
                if f.lower() in ("save0.cam", "te_new.tac"):
                    test_place_unit(os.path.join(camp, f), rows, db)

    print("\n%d checks, %d failures" % (checks, len(fails)))
    for f in fails:
        print("  - " + f)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
