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
                    names, objectives, rail, records, tacan, terrain,
                    theater, triggers, uiart, camptext, weather,
                    workspace)

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


def test_squadron_stores(campaign_dir, campaign_file, db):
    """A stores edit must move exactly one byte, and the right one.

    The array sits behind a variable-length run of fields, so the offset is
    computed rather than constant -- which is precisely the kind of thing that
    is wrong by four bytes and still looks plausible.
    """
    label = "%s/%s" % (os.path.basename(campaign_dir), campaign_file)
    cam = campfile.CampaignFile.load(os.path.join(campaign_dir, campaign_file))
    section = cam.member("uni")

    if section is None:
        return

    try:
        units, raw = entities.decode_units(section, cam.version,
                                           db.class_rows())
    except Exception:
        return

    squads = [u for u in units if u["kind"] == "squadron"]

    if not squads:
        return

    u = squads[0]
    check("_storesAt" in u and "_storesLen" in u,
          "%s: squadron record did not record where stores start" % label)

    if "_storesAt" not in u:
        return

    at, length = u["_storesAt"], u["_storesLen"]
    check(0 < at < len(raw),
          "%s: stores offset %d is outside the %d-byte stream"
          % (label, at, len(raw)))
    check(at + length <= len(raw),
          "%s: the %d-byte stores array runs off the end of the stream"
          % (label, length))
    check(list(raw[at:at + length]) == u["stores"],
          "%s: the bytes at the recorded offset are not the stores array"
          % label)

    # One edit, one byte.
    index = length // 2
    before = raw[at + index]
    want = (before + 7) % 256
    out = entities.patch_stores(raw, u, index, want)

    moved = [i for i, (a, b) in enumerate(zip(raw, out)) if a != b]
    check(moved == [at + index],
          "%s: patching one weapon changed %d byte(s)" % (label, len(moved)))
    check(out[at + index] == want,
          "%s: the patched byte does not hold the new value" % label)
    check(u["stores"][index] == want,
          "%s: the decoded record was not updated to match" % label)

    # And it must survive a decode of the patched stream.
    again, _raw2 = entities.decode_units(
        entities.encode_units(out, len(units)), cam.version, db.class_rows())
    same = [x for x in again if x.get("_storesAt") == at]
    check(bool(same) and same[0]["stores"][index] == want,
          "%s: the edit did not survive a re-encode" % label)

    # Out of range must be refused rather than silently clamped: the engine
    # indexes this array without a bounds check.
    for bad in (-1, length, length + 50):
        try:
            entities.patch_stores(raw, u, bad, 1)
            check(False, "%s: weapon index %d was accepted" % (label, bad))
        except ValueError:
            check(True, "")

    print("  ok    %-24s stores at %d, %d slots, one-byte edit clean"
          % (label, at, length))


def test_unit_state(camp, campaign_file, db):
    """The editable condition fields must move exactly the bytes they own.

    Supply, morale, fatigue, orders and losses sit behind variable-length
    waypoints and parent ids, so their offsets are recorded during decode; a
    wrong offset would still write *something* plausible, which is why every
    one is checked against a decode of the patched stream.
    """
    label = "%s/%s" % (os.path.basename(camp), campaign_file)
    path = os.path.join(camp, campaign_file)
    cam = campfile.CampaignFile.load(path)
    section = cam.member("uni")

    if section is None:
        return

    try:
        units, raw = entities.decode_units(section, cam.version, db.class_rows())
    except Exception:
        return

    wanted = {
        "battalion": ("supply", "morale", "fatigue", "losses", "orders"),
        "taskforce": ("supply", "orders"),
        "squadron": ("fuel", "losses"),
        "brigade": ("orders", "losses"),
    }
    checked = 0

    for kind, fields in wanted.items():
        u = next((x for x in units if x["kind"] == kind), None)
        if u is None:
            continue
        for field in fields:
            if field not in (u.get("_at") or {}):
                check(False, "%s: %s %s has no recorded offset"
                      % (label, kind, field))
                continue

            at = u["_at"][field]
            cur = u.get(field)
            probe = {"orders": 3, "fuel": 12345}.get(field, 33)
            if probe == cur:
                probe = (probe + 1) % len(entities.GROUND_ORDERS) \
                    if field == "orders" else probe + 1

            out = entities.patch_unit(raw, u, cam.version, field, probe)
            moved = [i for i, (a, b) in enumerate(zip(raw, out)) if a != b]
            width = 4 if field == "fuel" else 1
            check(1 <= len(moved) <= width,
                  "%s: patching %s on a %s touched %d bytes"
                  % (label, field, kind, len(moved)))
            check(all(at <= i < at + width for i in moved),
                  "%s: patching %s on a %s touched bytes outside its field"
                  % (label, field, kind))
            if width == 1:
                check(moved == [at],
                      "%s: patching %s on a %s did not land on its offset"
                      % (label, field, kind))

            again, _ = entities.decode_units(
                entities.encode_units(out, len(units)), cam.version,
                db.class_rows())
            same = [x for x in again if x.get("_span") == u.get("_span")]
            check(bool(same) and same[0][field] == probe,
                  "%s: the %s edit on a %s did not survive a re-encode"
                  % (label, field, kind))
            checked += 1

        # Fields a kind does not carry must be refused, not silently written.
        for field in ("fuel",):
            if field not in (u.get("_at") or {}) and field in entities.PATCHABLE:
                try:
                    entities.patch_unit(raw, u, cam.version, field, 1)
                    check(False, "%s: %s accepted a %s edit"
                          % (label, kind, field))
                except ValueError:
                    check(True, "")

    if checked:
        print("  ok    %-28s %d condition field(s) one-write clean"
              % (label, checked))


def test_objective_condition(camp, campaign_file, db):
    """supply/fuel/losses/priority, and the two-bit feature damage block."""
    label = "%s/%s" % (os.path.basename(camp), campaign_file)
    path = os.path.join(camp, campaign_file)
    cam = campfile.CampaignFile.load(path)
    section = cam.member("obj")

    if section is None:
        return

    try:
        objs, raw = objectives.decode_objectives(
            section, cam.version, db.class_rows())
    except Exception:
        return

    if not objs:
        return

    # The reader takes the smaller of the stored size and what the class table
    # says, so a disagreement is legal data -- Israel Classic ships plenty in
    # both directions. What matters here is that our decoder keeps the stored
    # size, because that is what advances the stream.
    mismatched = 0
    for o in objs:
        _t, drow = db.data_row(o["classIndex"])
        if drow and (drow["Features"] + 3) // 4 != len(o["featureStatus"]):
            mismatched += 1
    if mismatched:
        print("        note: %d objectives carry a feature block the class "
              "table does not predict" % mismatched)

    target = next((o for o in objs if o.get("featureStatus")), objs[0])

    for field, probe in (("supply", 41), ("fuel", 37), ("losses", 12),
                         ("priority", 9)):
        out = objectives.patch_objective(raw, target, cam.version, field, probe)
        moved = [i for i, (a, b) in enumerate(zip(raw, out)) if a != b]
        check(len(moved) == 1,
              "%s: patching objective %s touched %d bytes"
              % (label, field, len(moved)))
        again, _ = objectives.decode_objectives(
            objectives.encode_objectives(out, len(objs)), cam.version,
            db.class_rows())
        same = [x for x in again if x.get("_span") == target.get("_span")]
        check(bool(same) and same[0][field] == probe,
              "%s: objective %s did not survive a re-encode" % (label, field))

    # Feature damage: each of the four states, one byte, and it reads back.
    for status in (0, 1, 2, 3):
        out = objectives.patch_feature_bits(
            raw, target, cam.version, 0, status)
        moved = [i for i, (a, b) in enumerate(zip(raw, out)) if a != b]
        check(len(moved) <= 1,
              "%s: feature status %d touched %d bytes"
              % (label, status, len(moved)))
        again, _ = objectives.decode_objectives(
            objectives.encode_objectives(out, len(objs)), cam.version,
            db.class_rows())
        same = [x for x in again if x.get("_span") == target.get("_span")]
        check(bool(same) and objectives.feature_status(same[0], 0) == status,
              "%s: feature status %d did not survive a re-encode"
              % (label, status))

    slots = len(target["featureStatus"]) * 4
    for bad_feature in (-1, slots, slots + 3):
        try:
            objectives.patch_feature_bits(raw, target, cam.version,
                                          bad_feature, 3)
            check(False, "%s: feature %d was accepted"
                  % (label, bad_feature))
        except ValueError:
            check(True, "")
    for bad_status in (-1, 4):
        try:
            objectives.patch_feature_bits(raw, target, cam.version, 0,
                                          bad_status)
            check(False, "%s: feature status %d was accepted"
                  % (label, bad_status))
        except ValueError:
            check(True, "")

    print("  ok    %-28s condition and %d-slot damage block clean"
          % (label, slots))


def test_squadron_identity(camp, campaign_file, db):
    """Every flyable squadron record must join to a unit, and to a name.

    The header roster and the unit stream share only a VU_ID; `dIndex` is the
    unit's class-table row, which is where its name and aeroplane come from.
    A record that joins to nothing would show as a blank row in the UI and as
    a squadron the player can pick but never see on the map.
    """
    label = "%s/%s" % (os.path.basename(camp), campaign_file)
    path = os.path.join(camp, campaign_file)
    cam = campfile.CampaignFile.load(path)
    section = cam.member("uni")

    if section is None or cam.header is None or not cam.header.squadrons:
        return

    try:
        units, _raw = entities.decode_units(section, cam.version,
                                            db.class_rows())
    except Exception:
        return

    by_id = {}
    for u in units:
        if u["kind"] == "squadron" and u.get("id"):
            by_id[tuple(u["id"])] = u

    names = db.name_index()
    missing, mismatched, unnamed, no_aircraft = 0, 0, 0, 0
    for srec in cam.header.squadrons:
        u = by_id.get(tuple(srec["id"]))
        if u is None:
            missing += 1
            continue
        if u["classIndex"] != srec["dIndex"]:
            mismatched += 1
            continue
        if not names.get(u["classIndex"], "").strip():
            unnamed += 1
        drow = db.data_row(u["classIndex"])[1]
        has_aircraft = bool(drow) and any(
            vt and n for vt, n in zip(drow["VehicleType"], drow["NumElements"]))
        if not has_aircraft:
            no_aircraft += 1

    total = len(cam.header.squadrons)
    check(mismatched == 0, "%s: %d squadron records point at the wrong class"
                           % (label, mismatched))
    check(unnamed == 0, "%s: %d squadrons have no class name to compose from"
                        % (label, unnamed))
    check(no_aircraft <= total // 20,
          "%s: %d of %d squadrons resolve to no aircraft"
          % (label, no_aircraft, total))
    # A tactical-engagement template carries a roster that does not all exist
    # in its own unit stream, and a mid-campaign save references squadrons
    # from the scenario it was started from -- so a miss is not an error. A
    # wholesale failure of the join would still show, and everything that does
    # join is checked above.
    check(total - missing >= max(1, total // 4),
          "%s: only %d of %d squadron records join a unit"
          % (label, total - missing, total))
    print("  ok    %-28s %d squadron records, %d join a unit"
          % (label, total, total - missing))


def test_script_lookup(ws, camp, campaign_file):
    """The .tri the editor opens is the one the engine would read.

    `CheckTriggers(TheCampaign.Scenario)` reads `<Scenario>.tri`, always --
    never a script named after the campaign file that was loaded. Israel
    Classic is why that matters: its `save0.cam` names `save2` and its
    `save2.cam` names `save0`, so the two files play each other's ending and
    only the header can be trusted.
    """
    label = "%s/%s" % (os.path.basename(camp), campaign_file)
    sc, from_file = ws.script(campaign_file)

    cam = campfile.CampaignFile.load(os.path.join(camp, campaign_file))
    scenario = ""
    if cam.header:
        scenario = (cam.header.fields.get("Scenario") or "").strip()
    stem = os.path.splitext(campaign_file)[0]

    expected = (triggers.script_path(camp, scenario) if scenario
                else triggers.script_path(camp, campaign_file))
    if expected is None:
        check(sc is None,
              "%s: opened %s, but the engine would read no script"
              % (label, os.path.basename(sc.path) if sc else ""))
        return

    if not check(sc is not None, "%s: no script opened, expected %s"
                 % (label, os.path.basename(expected))):
        return
    check(os.path.normcase(sc.path) == os.path.normcase(expected),
          "%s: opened %s, the engine reads %s"
          % (label, os.path.basename(sc.path), os.path.basename(expected)))

    if scenario and scenario.lower() != stem.lower():
        check(from_file.lower() == scenario.lower(),
              "%s: borrowed script named %r, header says %r"
              % (label, from_file, scenario))
        own = triggers.script_path(camp, campaign_file)
        if own and os.path.normcase(own) != os.path.normcase(sc.path):
            print("        %s names %s; the engine reads %s, not its own %s"
                  % (label, scenario, os.path.basename(sc.path),
                     os.path.basename(own)))
    else:
        check(from_file == "",
              "%s: own script reported as borrowed from %r"
              % (label, from_file))


def test_script_targets(ws, camp, campaign_file):
    """The map's rings must match the script and the objectives it names.

    `#IF_CONTROLLED` is the only condition that names a place, so it is what
    can be drawn. Every endgame's places have to resolve to an objective -- a
    dead id in an endgame is a victory that can never fire -- and the polarity
    and endgame flags have to agree with an independent walk of the tree.
    """
    label = "%s/%s" % (os.path.basename(camp), campaign_file)
    sc, _from = ws.script(campaign_file)
    if sc is None:
        return

    cam = ws.objectives(campaign_file)
    ids = {o["campId"] for o in cam.objectives}
    targets = triggers.controlled_targets(sc.body)

    plain, negated = [], 0

    def walk(nodes, neg):
        nonlocal negated
        for n in nodes:
            if n.verb == "IF_CONTROLLED" and len(n.args) >= 3:
                for a in n.args[2:]:
                    if a.lstrip("-").isdigit():
                        plain.append(int(a))
                        if neg:
                            negated += 1
            walk(n.children, neg)
            if n.orelse is not None:
                walk(n.orelse, not neg)

    walk(sc.body, False)

    check(len(targets) == len(plain),
          "%s: controlled_targets returned %d, a plain walk finds %d"
          % (label, len(targets), len(plain)))
    check(len([t for t in targets if t["negated"]]) == negated,
          "%s: negated targets differ from a plain walk" % label)

    dead = sorted({t["campId"] for t in targets if t["campId"] not in ids})
    check(not [t for t in targets if t["endgame"] and t["campId"] not in ids],
          "%s: an endgame watches ids the campaign does not have: %s"
          % (label, dead[:4]))

    ends = {t["campId"] for t in targets if t["endgame"]}
    print("  ok    %-28s %d condition(s), %d target(s), %d endgame place(s), "
          "%d dead id(s)"
          % (label, sum(triggers.condition_counts(sc.body).values()),
             len(targets), len(ends), len(dead)))

    # The shipped Korea 2012 save0 endgames, checked by hand: the OPFOR win
    # holds all four of its places and the allied win any of its three.
    if campaign_file.lower() == "save0.cam" and \
            os.path.basename(camp).lower() == "korea2012":
        end_ids = {680, 260, 404, 229, 869, 415, 276}
        good = {t["campId"] for t in targets
                if t["endgame"] and not t["negated"]}
        check(end_ids <= good,
              "%s: endgame places not marked as such: %s"
              % (label, sorted(end_ids - good)))


def test_campaign_copy(gamedir):
    """A campaign copy must carry its ending and its base objective list.

    Both live outside the .cam: `<scenario>.tri` beside it, and -- for a save
    -- the objective list in the scenario its header names. A copy missing
    either is a campaign that cannot end, or one with no map.
    """
    print("Campaign copy")
    done = 0
    for tdf in theater.load_theaters(gamedir):
        camp = tdf.campaign_dir(gamedir)
        if not os.path.isfile(os.path.join(camp, "save0.cam")):
            continue
        tmp = tempfile.mkdtemp(prefix="ffcamp-copy-")
        try:
            game = os.path.join(tmp, "game")
            dst = os.path.join(game, "campaign", "test")
            os.makedirs(dst)
            os.makedirs(os.path.join(game, "terrdata", "theaterdefinition"))
            for f in os.listdir(camp):
                if f.lower().endswith((".cam", ".tac", ".tri")):
                    shutil.copy2(os.path.join(camp, f), os.path.join(dst, f))
            with open(os.path.join(game, "theater.lst"), "w",
                      encoding="latin-1", newline="\r\n") as fp:
                fp.write("terrdata\\theaterdefinition\\test.tdf\n")
            with open(os.path.join(game, "terrdata", "theaterdefinition",
                                   "test.tdf"), "w", encoding="latin-1",
                      newline="\r\n") as fp:
                fp.write("name Test\ndesc Test\ncampaigndir campaign\\test\n")

            ws = workspace.TheaterWorkspace(
                game, "terrdata/theaterdefinition/test.tdf")

            info = workspace.copy_campaign(ws, "save0.cam", "Copy Test")
            path = os.path.join(dst, "Copy Test.cam")
            check(os.path.isfile(path), "the copy was not written")
            cam = campfile.CampaignFile.load(path)
            check(cam.member("obj") is not None,
                  "the copy has no objective list")
            check(cam.header.fields["Scenario"] == "Copy Test",
                  "the copy does not name itself as its scenario")
            check(os.path.isfile(os.path.join(dst, "Copy Test.tri")),
                  "the copy has no trigger script")
            sc, frm = ws.script("Copy Test.cam")
            check(sc is not None and frm == "",
                  "the copy's script does not resolve as its own")
            check(info["baseObjectives"] is False,
                  "a scenario copy reported copying a base list")

            # A save has no .obj of its own; the copy has to bring the base
            # scenario's across or the deltas have nothing to apply to.
            saves = [f for f in sorted(os.listdir(dst))
                     if f.lower().endswith(".cam")
                     and f.lower() not in ("save0.cam", "save1.cam", "save2.cam",
                                           "instant.cam", "copy test.cam")]
            if saves:
                save = saves[0]
                info2 = workspace.copy_campaign(ws, save, "Copy From Save")
                check(info2["baseObjectives"] is True,
                      "%s: the base objective list was not copied" % save)
                cam2 = campfile.CampaignFile.load(
                    os.path.join(dst, "Copy From Save.cam"))
                check(cam2.member("obj") is not None,
                      "%s: the copy of a save has no objective list" % save)
                check(os.path.isfile(os.path.join(dst, "Copy From Save.tri")),
                      "%s: the copy of a save has no trigger script" % save)

            # Refuse to overwrite.
            try:
                workspace.copy_campaign(ws, "save0.cam", "Copy Test")
                check(False, "an existing campaign was overwritten")
            except FileExistsError:
                check(True, "")

            print("  ok    %-22s copy carries objectives and ending"
                  % os.path.basename(camp))
            done += 1
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
    if not done:
        print("        no theater with a save0.cam to copy")


def test_movies(gamedir, tdf):
    """`#PLAY_MOVIE` ids must resolve to a file and a title.

    The id is what a script names, and it only means something through
    MOVIES.ID and movies.irc -- both of which a theater can override with its
    own art directory. Every Israel theater points all seventeen ids at its
    own avi files, so this is also the check that the override is honoured.
    """
    print("Movie list: %s" % tdf.name)
    roots = []
    art = tdf.get("artdir")
    if art:
        roots.append(os.path.join(gamedir, *art.replace("\\", "/").split("/")))
    movies = uiart.UiArt(gamedir, roots).movies()
    check(len(movies) >= 17,
          "%s: only %d movies resolved" % (tdf.name, len(movies)))
    for mid in range(100, 117):
        check(mid in movies, "%s: movie %d is missing" % (tdf.name, mid))
    m = movies.get(104)
    if m:
        check("PUSHED BACK" in m["title"].upper(),
              "%s: movie 104 is titled %r" % (tdf.name, m["title"]))
        check(m["file"].lower().endswith(".avi"),
              "%s: movie 104 points at %r" % (tdf.name, m["file"]))
    print("  ok    %-22s %2d movies, 104 = %s"
          % (tdf.name, len(movies), m["file"] if m else "?"))


def test_dead_actions():
    """The actions this build ignores have to say so in the listing."""
    print("Actions this build ignores")
    for verb in ("SET_TEMPO", "CHANGE_PRIORITIES"):
        check(verb in triggers.DEAD_ACTIONS, "%s is not flagged" % verb)
        node = triggers.Node(verb, ["255"], 1)
        check("no effect" in triggers.describe(node, [], lambda i: str(i)),
              "%s is not annotated in the listing" % verb)
    print("  ok    %s" % ", ".join(sorted(triggers.DEAD_ACTIONS)))


def test_rail():
    """OSM ways merge end to end, keep bridge/tunnel runs, and the fit holds."""
    print("Rail (OSM) geometry")
    if rail.np is None:
        print("  skip  numpy missing")
        return
    np = rail.np
    main = {"railway": "rail", "usage": "main", "name": "Test Line"}
    bridge = dict(main, bridge="yes")
    # Three pieces of one line, the middle one a bridge and stored backwards,
    # plus a branch meeting at a junction (three ways at one node: no merge).
    ways = [(main, [(0.0, 0.0), (0.0, 1.0)]),
            (bridge, [(0.0, 2.0), (0.0, 1.0)]),
            (main, [(0.0, 2.0), (0.0, 3.0)]),
            (dict(main, usage="branch"), [(0.0, 3.0), (1.0, 3.0)]),
            (main, [(0.0, 3.0), (0.0, 4.0)])]
    merged = rail.merge_ways(ways)
    mains = [m for m in merged if m["cls"][0] == "main"]
    check(len(merged) == 3, "expected 3 lines after merging, got %d" % len(merged))
    longest = max(mains, key=lambda m: len(m["pts"]))
    check(len(longest["pts"]) == 4 and longest["seg"] in ("-b-", "-b-"[::-1]),
          "bridge run lost in merge: %r" % longest["seg"])

    xy = np.array([[0, 0], [1, 0.01], [2, 0], [3, 0], [4, 0]], float)
    out, seg = rail.simplify(xy, "--b-", 0.05)
    check(len(out) == 4 and seg == "-b-",
          "simplify must keep the bridge ends: %d pts %r" % (len(out), seg))
    parts = rail.clip(np.array([[-1, 5], [1, 5], [2, 5], [5, 5]], float),
                      "---", 3, 10)
    check(len(parts) == 1 and len(parts[0][0]) == 2, "clip kept %r" % parts)

    # A known affine map is recovered exactly from four points.
    proj = rail.Projection(37.0, 127.0, 1)
    lat = np.array([36.0, 36.0, 38.0, 38.0, 37.0])
    lon = np.array([126.0, 128.0, 126.0, 128.0, 127.0])
    u, v = proj.plane(lat, lon)
    gx, gy = 400 + 600 * u + 10 * v, 450 + 500 * v
    proj.fit_points(lat, lon, gx, gy)
    fx, fy = proj(lat, lon)
    check(float(np.abs(fx - gx).max() + np.abs(fy - gy).max()) < 1e-6,
          "affine fit does not reproduce its own points")
    back = proj.inverse(float(gx[4]), float(gy[4]))
    check(abs(back[0] - 37.0) + abs(back[1] - 127.0) < 1e-6,
          "inverse is off: %r" % (back,))

    # Distance field: a straight shore at x = 10 km.
    sea = np.zeros((32, 32), bool)
    sea[:, 10:] = True
    d = rail.shore_distance(rail.sea_mask(sea), cap=16)
    check(abs(float(d[5, 2]) - 7.0) < 1e-6, "shore distance %r" % d[5, 2])
    # Route stitching, with both traps the Gyeongbu Line set: a double track
    # whose two tracks end on one terminus node (so the terminus is not a
    # dead end), and a parallel spur that joins only at the far end (so the
    # longest shortest path runs out and back down it).
    pieces = [[[0, 0], [50, 0]], [[0, 0], [50, 0.004]],     # double track
              [[50, 0], [100, 0]],                             # single on to the end
              [[100, 0], [70, 0.3]]]                           # spur back from the end
    route = rail.build_route(pieces)
    ends = sorted([tuple(route[0]), tuple(route[-1])])
    check(ends == [(0, 0), (100, 0)], "route ends %r, want (0,0) and (100,0)" % ends)
    check(abs(rail._path_len(route) - 100) < 0.1,
          "route is %.1f km, want 100" % rail._path_len(route))
    # Bridge/tunnel flags ride along, reversed with a piece walked backwards.
    fl_pieces = [[[0, 0], [10, 0], [20, 0]], [[40, 0], [30, 0], [20, 0]]]
    fl_route, fl = rail.build_route(fl_pieces, ["b-", "t-"])
    if fl_route[0] != [0, 0]:
        fl_route, fl = fl_route[::-1], fl[::-1]
    check(fl == "b--t" and len(fl) == len(fl_route) - 1,
          "route flags %r, want 'b--t'" % fl)

    # Keeping lines off the sea: open sea east of x = 20, plus a one-cell river
    # at x = 8 joined to it (sea_mask would flood it). A point just offshore is
    # pulled ashore; a point on the river crossing is left alone.
    sea = np.zeros((40, 40), bool)
    sea[:, 20:] = True
    sea[:, 8] = True
    keeper = rail.LandKeeper(sea)
    moved = keeper.fix([[20.6, 10.5]])[0]
    check(moved[0] < 20.0 and not sea[int(moved[1]), int(moved[0])],
          "offshore point not moved ashore: %r" % (moved,))
    kept = keeper.fix([[8.5, 10.5]])[0]
    check(kept == [8.5, 10.5], "river crossing point was moved: %r" % (kept,))
    # A bridge point out over open sea stays put; densified segments keep
    # their flag.
    br_pts, br_fl = keeper.fix([[18.5, 10.5], [20.6, 10.5], [22.5, 10.5]],
                               densify_km=1.0, seg="bb")
    check(any(p == [20.6, 10.5] for p in br_pts),
          "bridge point over the sea was moved: %r" % (br_pts,))
    check(len(br_fl) == len(br_pts) - 1 and set(br_fl) == {"b"},
          "densified bridge flags %r for %d points" % (br_fl, len(br_pts)))
    # Airfields: a 3 km east-west runway at (10, 10). A track crossing it
    # north-south, and one running along it, both go round the box: no point
    # (or midpoint of a segment) inside, flags one per segment, ends kept.
    field = rail.Airfield("test", [(8.5, 10.0), (11.5, 10.0)], margin=0.35)
    for label, line in (("across", [[10.0, 5.0 + 0.25 * k] for k in range(41)]),
                        ("along", [[5.0 + 0.25 * k, 10.1] for k in range(41)])):
        fl_in = "-" * 10 + "b" * 5 + "-" * (len(line) - 16)
        got, got_fl, went = rail.keep_off_airfields(line, fl_in, [field])
        mids = [[(a[0] + b[0]) / 2, (a[1] + b[1]) / 2] for a, b in zip(got, got[1:])]
        check(went == ["test"], "%s: detour not taken (%r)" % (label, went))
        check(not any(field.inside(p) for p in got + mids),
              "%s: track still inside the airfield" % label)
        check(len(got_fl) == len(got) - 1, "%s: %d flags for %d points"
              % (label, len(got_fl), len(got)))
        check(got[0] == line[0] and got[-1] == line[-1], "%s: route ends moved" % label)
    print("  ok    merge, simplify, clip, fit, shore distance, route stitching, "
          "keep on land")


def test_script_annotations(ws, camp, campaign_file):
    """Events written but never tested, and the endgames that are exempt."""
    label = "%s/%s" % (os.path.basename(camp), campaign_file)
    sc, _from = ws.script(campaign_file)
    if sc is None:
        return

    usage = triggers.event_usage(sc.init, sc.body)
    write_only = triggers.write_only_events(sc.init, sc.body)

    for eid in write_only:
        u = usage[eid]
        check(u["tested"] == 0 and (u["fired"] or u["reset"] or u["set"]),
              "%s: event %d is flagged but is tested or never written"
              % (label, eid))

    ends = {int(e["result"]) for e in triggers.endgames(sc.body, [], str)
            if e["result"].isdigit()}
    check(not (ends & set(write_only)),
          "%s: an endgame event is flagged write-only: %s"
          % (label, sorted(ends & set(write_only))))

    # The Korea-family save0 scripts fire events 6 and 8 and never test them;
    # 5 and 7 are the ones that are tested.
    if campaign_file.lower() == "save0.cam" and \
            os.path.basename(camp).lower() in ("save", "korea2012",
                                               "korea1980s", "eurowar"):
        check(6 in write_only and 8 in write_only,
              "%s: events 6 and 8 should be write-only, got %s"
              % (label, write_only))
        check(5 not in write_only and 7 not in write_only,
              "%s: events 5 and 7 are tested" % label)

    if write_only:
        print("        %s write-only events: %s" % (label, write_only))


def theater_names(camp):
    """The place-name table the engine loads here -- names.theater_stem."""
    def theater_name_of(f):
        cam = campfile.CampaignFile.load(os.path.join(camp, f))
        return (cam.header.fields if cam.header else {}).get("TheaterName")
    return names.load(camp, names.theater_stem(camp, theater_name_of))


def test_airbase_names(camp, f, rows, nametab):
    """The engine wrote a printable airbase name into every flyable-squadron
    record, using its own name table and the objective at the squadron's grid
    square (`GetObjectiveByXY`). Reading the same square with our table must
    give the same string -- this is the check that caught the editor reading
    Korea's 2010 DEFAULT.idx instead of korea.idx.
    """
    label = "%s/%s" % (os.path.basename(camp), f)
    cam = campfile.CampaignFile.load(os.path.join(camp, f))
    section = cam.member("obj")
    if section is None or not cam.header or not cam.header.squadrons:
        return
    try:
        objs, _raw = objectives.decode_objectives(section, cam.version, rows)
    except Exception:
        return
    at = {}
    for o in objs:
        at.setdefault((o["x"], o["y"]), o)
    agree = total = 0
    for sq in cam.header.squadrons:
        want = (sq.get("airbaseName") or "").strip()
        # The record holds sim feet, and sim x is grid y (north): 774075 ft
        # is grid row 236. Same square GetObjectiveByXY looked in.
        gx = int(round(sq["y"] / 3280.84))
        gy = int(round(sq["x"] / 3280.84))
        o = at.get((gx, gy))
        if not want or o is None:
            continue
        total += 1
        got = nametab.get(o["nameId"], "").strip()
        if got.lower() == want.lower():
            agree += 1
    if total:
        # A few records are stale in the shipped files: the header is written
        # at save time and the squadron was rebased afterwards (eurowar save2
        # has Yechon's square labelled Choongwon). The wrong name TABLE shows
        # up as ~40% disagreement, so near-total agreement is the real test.
        check(agree * 100 >= total * 90,
              "%s: only %d of %d squadron airbase names match the engine's -- "
              "wrong place-name table?" % (label, agree, total))
        print("  ok    %-24s %d of %d airbase names match the engine's%s"
              % (label, agree, total,
                 "" if agree == total else
                 " (%d stale header records)" % (total - agree)))


def test_theaters(gamedir):
    print("Theater list: %s" % gamedir)
    ths = theater.load_theaters(gamedir)
    check(len(ths) > 0, "no theaters found")
    for t in ths:
        print("  ok    %-22s campaigndir=%s" % (t.name, t.get("campaigndir")))
        check(os.path.isdir(t.campaign_dir(gamedir)),
              "%s: campaigndir %r does not exist"
              % (t.name, t.get("campaigndir")))


# Severity the game's own weatherfronts.cpp produced for this state (built
# with g++ from the source, see the fronts harness in the weather notes). The
# Python port draws the editor's map; if it drifts from these, the editor
# shows weather the game will not fly.
WEATHER_REF_STATE = None
WEATHER_REF = [
    (32400000, 0.0, 0.0, 1.854028),
    (32400000, 655996.0, 2787983.0, 1.506036),
    (32400000, 1475991.0, 2295986.0, 1.524327),
    (32400000, 2295986.0, 1803989.0, 1.658539),
    (32400000, 3115981.0, 1311992.0, 2.500214),
    (40000000, 655996.0, 819995.0, 1.595303),
    (40000000, 1475991.0, 327998.0, 1.365068),
    (40000000, 2131987.0, 3115981.0, 2.579922),
    (40000000, 2951982.0, 2623984.0, 2.039654),
    (55000000, 491997.0, 2131987.0, 1.617610),
    (55000000, 1311992.0, 1639990.0, 3.925231),
    (55000000, 2131987.0, 1147993.0, 1.834167),
    (55000000, 2951982.0, 655996.0, 1.836669),
]


def _weather_ref_state():
    KM = 3279.98

    def F(x, y, h, sp, hw, hl, sev, wb, td, born, life):
        return dict(x=x * KM, y=y * KM, heading=h, speed=sp, halfWidth=hw * KM,
                    halfLength=hl * KM, severity=sev, windBoost=wb,
                    tempDelta=td, born=born, life=life)
    return dict(prevailing=1.8, noiseAmp=0.6, noiseScale=400000.0,
                seed=123456789, driftX=20.0, driftY=-12.0, fronts=[
                    F(600, 100, 1.9, 42, 35, 250, 2.2, 15, -5, 20000000, 80000000),
                    F(400, 500, 0.3, 30, 14, 0, 2.6, 20, -2, 30000000, 20000000),
                    F(300, 300, 2.4, 20, 90, 0, -2.0, 0, 1, 10000000, 90000000),
                    F(700, 600, 0.8, 25, 60, 300, 1.5, 8, 3, 25000000, 70000000)])


def test_weather_model():
    print("Weather fronts model")
    st = _weather_ref_state()
    for t, x, y, want in WEATHER_REF:
        got = weather.severity(st, x, y, t)
        check(abs(got - want) < 1e-4,
              "weather: severity at %.0f,%.0f t=%d is %.6f, the game says %.6f"
              % (x, y, t, got, want))

    # The state survives the byte layout the game reads (44-byte fronts,
    # 380-byte state), including the unsigned times.
    blob = weather.encode_state(st)
    check(len(blob) == 380, "weather: state encodes to %d bytes, not 380"
          % len(blob))
    back = weather.decode_state(blob)
    check(len(back["fronts"]) == 4, "weather: fronts lost in the round trip")
    for a, b in zip(st["fronts"], back["fronts"]):
        check(a["born"] == b["born"] and a["life"] == b["life"] and
              abs(a["x"] - b["x"]) < 1 and abs(a["severity"] - b["severity"]) < 1e-5,
              "weather: a front changed in the round trip")

    # A field with no fronts and no patches is the prevailing condition
    # everywhere; the clamp keeps any sum on the condition scale.
    flat = dict(prevailing=3.0, noiseAmp=0.0, fronts=[])
    check(weather.severity(flat, 1e6, 1e6, 5e7) == 3.0,
          "weather: an empty field is not flat")
    many = dict(prevailing=4.0, noiseAmp=2.0, noiseScale=400000.0, seed=9,
                fronts=st["fronts"])
    vals = [weather.severity(many, x * 1e5, y * 1e5, 4e7)
            for x in range(30) for y in range(30)]
    check(min(vals) >= 1.0 and max(vals) <= 4.49,
          "weather: severity left the 1..4.49 range")

    # Spawned fronts: every kind is recognised as itself from its shape (the
    # game stores no kind), and in-progress ones sit on the map with their
    # birth never before the campaign clock's zero.
    import random as _random
    rng = _random.Random(4)
    for kind in weather.KINDS:
        for now in (3600000, 36000000, 400000000):
            f = weather.spawn(kind, 1.0, 1024, 1024, now, rng, True)
            check(f["kind"] == kind, "weather: a spawned %s reads back as %s"
                  % (kind, f["kind"]))
            check(0 <= f["born"] <= now and f["life"] > now - f["born"],
                  "weather: spawned %s at t=%d has born %d life %d"
                  % (kind, now, f["born"], f["life"]))
            if kind in ("cell", "squall"):
                n, e = weather.core(f, now)
                check(0 <= n <= 1024 * weather.KM and 0 <= e <= 1024 * weather.KM,
                      "weather: a spawned %s is off the map" % kind)


def test_weather_member(path):
    """Every weather member reads; the Cobra ones write back byte for byte, and
    a fronts block added to one reads back as it was written."""
    cam = campfile.CampaignFile.load(path)
    raw = cam.member("wth")
    name = os.path.basename(path)
    if raw is None:
        return
    w = weather.parse(raw)
    if not check(w["layout"] in ("cobra", "tacedit"),
                 "%s: weather member not recognised (%d bytes)"
                 % (name, len(raw))):
        return
    if w["layout"] != "cobra":
        return
    if w["fronts"] is None:
        check(weather.encode(w) == raw,
              "%s: weather does not re-encode to the same bytes" % name)
    if w["fronts"] is None:
        w2 = dict(w)
        w2["fronts"] = _weather_ref_state()
        blob = weather.encode(w2)
        back = weather.parse(blob)
        check(back["fronts"] is not None and
              len(back["fronts"]["fronts"]) == 4 and
              back["condition"] == w["condition"] and
              abs(back["fronts"]["windKph"] - w["windKnots"] * 1.852) < 1e-3,
              "%s: fronts block did not read back" % name)
        check(blob[:29] == raw[:29],
              "%s: adding fronts changed the weather the old readers see"
              % name)
        w2["fronts"] = None
        back["fronts"] = None
        check(weather.encode(back)[:29] == raw[:29] and
              weather.read_fronts(weather.encode(back)) is None,
              "%s: removing the fronts again left a block behind" % name)


def test_weather_editor_view():
    """The editor sends fronts as km, degrees and knots; converting there and
    back must land the core where the map showed it."""
    import server
    import random as _random
    rng = _random.Random(11)
    now = 36326112
    for kind in weather.KINDS:
        f = weather.spawn(kind, 0.7, 1024, 1024, now, rng, True)
        v = server._front_view(f, now)
        g = server._front_from_view(v, now)
        v2 = server._front_view(g, now)
        check(all(abs(v[k] - v2[k]) < 0.2 for k in ("eastKm", "northKm",
                                                   "headingDeg", "speedKts",
                                                   "widthKm", "severity")),
              "weather: the editor's view of a %s does not round-trip: %r vs %r"
              % (kind, v, v2))
        check(v2["kind"] == kind, "weather: a %s came back as %s"
              % (kind, v2["kind"]))


def main():
    gamedir = sys.argv[1] if len(sys.argv) > 1 else r"C:\FreeFalcon6"
    test_lzss()
    test_specs()
    test_dead_actions()
    test_rail()
    test_weather_model()
    test_weather_editor_view()
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
            nametab = theater_names(camp)
            for f in sorted(os.listdir(camp)):
                if f.lower().endswith((".cam", ".tac")):
                    test_triggers(camp, f, rows, db, nametab)
                    test_airbase_names(camp, f, rows, nametab)
                    test_script_editing(camp, f)
                    test_squadron_stores(camp, f, db)

        print("Campaign selection text")
        for tdf in theater.load_theaters(gamedir):
            test_campaign_text(gamedir, tdf)

        for tdf in theater.load_theaters(gamedir):
            test_movies(gamedir, tdf)

        print("Trigger script lookup")
        for tdf in theater.load_theaters(gamedir):
            camp = tdf.campaign_dir(gamedir)
            if not os.path.isdir(camp):
                continue
            try:
                ws = workspace.TheaterWorkspace(gamedir,
                                               tdf.as_dict(gamedir)["file"])
            except Exception:
                continue
            for f in sorted(os.listdir(camp)):
                if f.lower().endswith((".cam", ".tac")):
                    test_script_lookup(ws, camp, f)
                    test_script_targets(ws, camp, f)
                    test_script_annotations(ws, camp, f)

        test_campaign_copy(gamedir)

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
                    test_weather_member(os.path.join(camp, f))

        camps_done = set()
        for camp, dbdir in camps:
            key = os.path.normcase(os.path.abspath(camp))
            if key in camps_done:
                continue
            camps_done.add(key)
            print("Streams: %s" % os.path.relpath(camp, gamedir))
            db = campdb.CampaignDB(dbdir)
            rows = db.class_rows()
            nametab = theater_names(camp)
            check(len(nametab) > 0, "%s: no place-name table" % camp)
            for f in sorted(os.listdir(camp)):
                if f.lower().endswith((".cam", ".tac")):
                    test_units(os.path.join(camp, f), rows)
                    test_objectives(os.path.join(camp, f), rows, nametab)
                    test_unit_state(camp, f, db)
                    test_objective_condition(camp, f, db)
                    test_squadron_identity(camp, f, db)
            for f in sorted(os.listdir(camp)):
                if f.lower() in ("save0.cam", "te_new.tac"):
                    test_place_unit(os.path.join(camp, f), rows, db)

    print("\n%d checks, %d failures" % (checks, len(fails)))
    for f in fails:
        print("  - " + f)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
