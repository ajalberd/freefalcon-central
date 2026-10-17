"""Campaign Lab: launch campsim runs, watch them, compare them, replay them.

    python serve.py [port]          ->  http://localhost:8770/   (lab.html; viewer.html?run=NAME)

Runs land in runs/<save>[-tag]-s<seed>.jsonl/.log exactly as run_batch.py writes them, so
runs started from the command line show up here too. Summaries are cached next to each
timeline (<name>.sum.json) because a 10-day timeline is 20-50 MB.
"""
import glob
import gzip
import http.server
import json
import os
import re
import sys
import threading
import time
import types
from concurrent.futures import ThreadPoolExecutor
from urllib.parse import parse_qs

HERE = os.path.dirname(os.path.abspath(__file__))
os.chdir(HERE)
os.makedirs("runs", exist_ok=True)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "campaign-editor"))
import run_batch  # noqa: E402

GAMEWORK = os.path.join(HERE, "gamework")
GAME = GAMEWORK if os.path.isdir(GAMEWORK) else run_batch.GAME
SAVE_DIR = os.path.join(GAME, "campaign", "SAVE")
TRI = os.path.join(run_batch.GAME, "campaign", "SAVE", "save0.tri")
BLUE, RED = (1, 2, 3), (5, 6)
EVENT_RE = re.compile(r"EVENT (\d+) FIRED at min (\d+) \(day \d+\) \| DPRK supply=(-?\d+) ac=(-?\d+) gnd=(-?\d+)")
PROG_RE = re.compile(r"\[day (\d+) (\d\d):(\d\d)\] ([\d.]+)/([\d.]+) h \(\s*([\d.]+)%\)")
RESULT_RE = re.compile(r"RESULT seed=\d+ endgame=(-?\d+).*wall_s=(\d+)")

SUMMARY_VERSION = 4      # bump when summarise() output changes (invalidates .sum.json caches)
POOL = ThreadPoolExecutor(int(os.environ.get("CAMPSIM_JOBS", "4")))
JOBS = {}            # name -> {"state", "rc", "started", "args"}
LOCK = threading.Lock()


def event_titles():
    try:
        from ffcamp import triggers
        with open(TRI, encoding="latin-1") as f:
            return {str(k): v for k, v in triggers.event_titles(f.read().splitlines()).items()}
    except Exception:
        return {}


TITLES = event_titles()


def side(f, teams, k):
    return sum(f["s"][t][k] for t in teams)


FRAME_T_RE = re.compile(rb'^\{"t":(-?\d+)')
_TAIL_KEYS = (b',"st":[', b',"s":[')       # TimelineFrame writes ... "ev", "st", "s", "sp" last


def frame_tail(ln):
    """The few keys of a timeline frame the summary needs (t, st, s), without parsing its unit list.

    A frame line is 20-80 KB, almost all of it "u"; the keys after it are a few hundred bytes."""
    for key in _TAIL_KEYS:
        i = ln.rfind(key)
        if i > 0:
            try:
                o = json.loads(b"{" + ln[i + 1:])
                m = FRAME_T_RE.match(ln)
                o["t"] = int(m.group(1)) if m else json.loads(ln)["t"]
                return o
            except ValueError:
                break
    return json.loads(ln)


class RunState:
    """What summarise() has read of one run so far: growing files are read from where it stopped."""

    def __init__(self):
        self.tl_off, self.tl_head = 0, b""
        self.meta = self.first = self.last = self.fin = None
        self.frames, self.curve = 0, []        # per frame: t, blue objs, red objs, DPRK supply, DPRK ac, ROK ac, ...
        self.log_off, self.log_head = 0, b""
        self.events, self.knobs, self.crash, self.stack, self.in_crash, self.wall = [], [], None, [], False, None


STATES = {}           # name -> RunState
SUMS = {}             # name -> (stamp, summary) in memory, so a cached summary is not re-read from disk every poll


def _new_bytes(path, off, head):
    """(bytes appended since off up to the last newline, new offset, head) or None when the file was replaced."""
    try:
        with open(path, "rb") as fh:
            h = fh.read(256)
            if off and h[:len(head)] != head:
                return None                    # a rerun of the same name rewrote the file
            fh.seek(0, 2)
            size = fh.tell()
            if size < off:
                return None
            fh.seek(off)
            b = fh.read(size - off)
    except OSError:
        return b"", off, head
    cut = b.rfind(b"\n") + 1                   # a run still writing its last line: keep that line for next time
    return b[:cut], off + cut, h


def _read_timeline(st, tl):
    got = _new_bytes(tl, st.tl_off, st.tl_head)
    if got is None:
        return False
    b, st.tl_off, st.tl_head = got
    for ln in b.split(b"\n"):
        if not ln.strip():
            continue
        try:
            if ln.startswith(b'{"meta"'):
                st.meta = json.loads(ln)
                continue
            if ln.startswith(b'{"final"'):
                st.fin = json.loads(ln)
                continue
            o = frame_tail(ln)
        except ValueError:
            continue
        st.frames += 1
        st.first = st.first or o
        st.last = o
        # st = the statistics the .tri triggers read: [supply%, aircraft, groundVehs] per team.
        # China: DPRK supply < 40 or DPRK/ROK aircraft x10 < 6; Russia: < 20 or < 3.
        s = o.get("st") or []
        has = len(s) > 6
        dac, rac = (s[6][1], s[2][1]) if has else (None, None)
        st.curve.append([o["t"], side(o, BLUE, 4), side(o, RED, 4),
                         s[6][0] if has else None, dac, rac,
                         side(o, BLUE, 0), side(o, RED, 0),
                         round(10.0 * dac / rac, 2) if has and rac else None])
    return True


def _read_log(st, log):
    got = _new_bytes(log, st.log_off, st.log_head)
    if got is None:
        return False
    b, st.log_off, st.log_head = got
    for ln in b.decode("latin-1").splitlines(True):
        if ln.startswith("CRASH at"):            # campsim's crash filter (main.cpp)
            st.crash, st.in_crash = ln.strip(), True
            continue
        if st.in_crash:
            if ln.startswith("CRASH END"):
                st.in_crash = False
            elif ln.startswith("  ") and len(st.stack) < 12:
                st.stack.append(ln.strip())
            continue
        if "EVENT" in ln:
            m = EVENT_RE.search(ln)
            if m:
                ev = int(m.group(1))
                st.events.append({"id": ev, "min": int(m.group(2)), "supply": int(m.group(3)),
                                  "ac": int(m.group(4)), "title": TITLES.get(str(ev), "")})
                continue
        if ln.startswith("KNOB"):
            st.knobs.append(ln.strip()[5:])
        elif ln.startswith("ERR"):
            st.crash = st.crash or ln.strip()[:200]
        if "RESULT" in ln:
            m = RESULT_RE.search(ln)
            if m:
                st.wall = int(m.group(2))
    return True


_ENDLAB = {}


def end_label(name, code):
    parts = run_batch.run_parts(run_batch.GAME, name)
    key = parts[:2] if parts else name
    if key not in _ENDLAB:
        _ENDLAB[key] = run_batch.end_labels(run_batch.GAME, name)
    return _ENDLAB[key].get(code, "")


def summarise(name, stats=None):
    """Small JSON describing one run; cached by timeline/log mtime+size, in memory and in <name>.sum.json.

    stats: {ext: os.stat_result} from list_runs' directory scan, to skip the stat calls."""
    tl, log = os.path.join("runs", name + ".jsonl"), os.path.join("runs", name + ".log")
    cache = os.path.join("runs", name + ".sum.json")
    if stats is None:
        stats = {}
        for ext, p in ((".jsonl", tl), (".log", log)):
            try:
                stats[ext] = os.stat(p)
            except OSError:
                pass
    # the stamp the old format used (mtimes), so existing .sum.json caches stay valid, plus sizes for memory
    stamp = [stats[e].st_mtime if e in stats else 0 for e in (".jsonl", ".log")] + [SUMMARY_VERSION]
    full = stamp + [stats[e].st_size if e in stats else 0 for e in (".jsonl", ".log")]
    hit = SUMS.get(name)
    if hit and hit[0] == full:
        return hit[1]
    if name not in STATES and os.path.exists(cache):
        try:
            with open(cache) as fh:
                c = json.load(fh)
            if c.get("_stamp") == stamp:
                SUMS[name] = (full, c)
                return c
        except ValueError:
            pass
    st = STATES.get(name) or RunState()
    if not _read_timeline(st, tl) or not _read_log(st, log):
        st = RunState()                          # replaced by a rerun: start over
        _read_timeline(st, tl)
        _read_log(st, log)
    out = build_summary(name, st, stamp)
    if st.fin:
        STATES.pop(name, None)                   # finished: the summary is all that is needed from now on
    else:
        STATES[name] = st
    SUMS[name] = (full, out)
    if out["done"] or not live(name, stats):
        try:
            with open(cache, "w") as fh:
                json.dump(out, fh)
        except OSError:
            pass
    return out


def build_summary(name, st, stamp):
    out = {"name": name, "_stamp": stamp}
    meta, first, last, fin, curve = st.meta, st.first, st.last, st.fin, st.curve
    if meta:
        out["seed"] = meta.get("seed")
        out["daysPlanned"] = meta.get("days")
        out["scenario"] = meta.get("scenario")
    if first and last:
        out["days"] = round(last["t"] / 1440.0, 2)
        out["obj"] = [[side(first, BLUE, 4), side(first, RED, 4)], [side(last, BLUE, 4), side(last, RED, 4)]]
        out["gnd"] = [[side(first, BLUE, 0), side(first, RED, 0)], [side(last, BLUE, 0), side(last, RED, 0)]]
        out["air"] = [[side(first, BLUE, 2), side(first, RED, 2)], [side(last, BLUE, 2), side(last, RED, 2)]]
        # per team (any theater): name, objectives / ground / aircraft first -> last frame; teams with nothing are left out
        names = (meta or {}).get("teams") or []
        out["teams"] = [{"n": n, "obj": [first["s"][t][4], last["s"][t][4]], "gnd": [first["s"][t][0], last["s"][t][0]],
                         "air": [first["s"][t][2], last["s"][t][2]]}
                        for t, n in enumerate(names) if n and t < len(first["s"]) and any(first["s"][t][k] for k in (0, 2, 4))]
        step = max(1, len(curve) // 240)
        out["curve"] = curve[::step]
    out["done"] = bool(fin)
    out["endgame"] = fin.get("endgame") if fin else None
    out["endLabel"] = end_label(name, out["endgame"]) if out["endgame"] else ""
    if st.wall is not None:
        out["wall"] = st.wall
    out["events"], out["knobs"], out["crash"], out["stack"] = list(st.events), list(st.knobs), st.crash, list(st.stack)
    return out


LITE_DROP = ("curve", "_stamp")


def lite(s):
    """A summary without its curve: what the run table needs (the curve is ~90% of the bytes)."""
    return {k: v for k, v in s.items() if k not in LITE_DROP}


def running(name):
    j = JOBS.get(name)
    return bool(j and j["state"] in ("queued", "running"))


LIVE_S = 300          # a batch-script run counts as running while its files changed this recently


def live(name, stats=None):
    """Running here, or by a batch script: run_batch.py keeps runs/_work_<name> while campsim runs, but a
    killed batch leaves the dir behind, so the run's files must also have been written in the last LIVE_S."""
    if running(name):
        return True
    if not os.path.isdir(os.path.join("runs", "_work_" + name)):
        return False
    if stats is None:
        stats = {}
        for ext in (".jsonl", ".log"):
            try:
                stats[ext] = os.stat(os.path.join("runs", name + ext))
            except OSError:
                pass
    return any(time.time() - stats[e].st_mtime < LIVE_S for e in (".jsonl", ".log") if e in stats)


def progress(name):
    log = os.path.join("runs", name + ".log")
    try:
        with open(log, "rb") as fh:
            fh.seek(0, 2)
            fh.seek(max(0, fh.tell() - 4096))
            tail = fh.read().decode("latin-1")
    except OSError:
        return None
    ms = PROG_RE.findall(tail)
    if not ms:
        return {"pct": 0, "clock": "starting"}
    d, hh, mm, _, _, pct = ms[-1]
    return {"pct": float(pct), "clock": "Day %s %s:%s" % (d, hh, mm)}


def start_runs(req):
    save = os.path.splitext(os.path.basename(req["save"]))[0]
    tag = re.sub(r"[^A-Za-z0-9_+.]", "", req.get("tag", ""))
    knobs = [k for k in req.get("knobs", []) if re.match(r"^[a-z]+=[A-Za-z0-9_.:\\/ -]*$", k)]
    a = types.SimpleNamespace(save=save, tag=tag, game=GAME, days=int(req.get("days", 3)),
                              every=int(req.get("every", 60)), set=knobs)
    started = []
    for seed in req.get("seeds", [1]):
        seed = int(seed)
        name = "%s%s-s%d" % (save, ("-" + tag) if tag else "", seed)
        with LOCK:
            if running(name):
                continue
            JOBS[name] = {"state": "queued", "rc": None, "started": time.time(),
                          "args": {"save": save, "days": a.days, "seed": seed, "knobs": knobs}}
        POOL.submit(job, a, seed, name)
        started.append(name)
    return started


def job(a, seed, name):
    if JOBS[name]["state"] == "cancelled":
        return
    JOBS[name]["state"] = "running"
    JOBS[name]["started"] = time.time()
    try:
        run_batch.run_one(a, seed)
        log = open(os.path.join("runs", name + ".log"), errors="replace").read()
        JOBS[name]["state"] = "done" if "RESULT seed=" in log else "failed"
    except Exception as e:  # noqa: BLE001
        JOBS[name]["state"] = "failed"
        JOBS[name]["error"] = str(e)


RUN_EXTS = (".jsonl", ".log", ".sum.json", ".ffdebug.log")
_LABELS = {}


def scan_runs():
    """{name: {ext: os.stat_result}} and the set of runs/_work_* dirs, from one directory listing.

    (One glob per run made /api/runs O(runs x files): 40 s for 1600 runs.) On Windows scandir's
    entries carry their stat data, so this costs one directory read."""
    files, work = {}, set()
    with os.scandir("runs") as it:
        for e in it:
            n = e.name
            if n.startswith("_"):
                if n.startswith("_work_") and e.is_dir():
                    work.add(n[6:])
                continue
            for ext in RUN_EXTS:
                if n.endswith(ext):
                    files.setdefault(n[:-len(ext)], {})[ext] = e.stat()
                    break
    return files, work


def list_runs():
    files, work = scan_runs()
    now = time.time()
    names = {n for n, f in files.items() if ".jsonl" in f} | set(JOBS)
    newest = {n: max([s.st_mtime for s in files.get(n, {}).values()] or [now]) for n in names}
    out = []
    for n in sorted(names, key=lambda n: -newest[n]):
        if n.startswith("_"):
            continue
        j, f = JOBS.get(n), files.get(n, {})
        if n not in _LABELS:
            _LABELS[n] = run_batch.run_label(run_batch.GAME, n)
        busy = n in work and any(now - f[e].st_mtime < LIVE_S for e in (".jsonl", ".log") if e in f)
        state = j["state"] if j else ("running" if busy else "done")
        row = {"name": n, "state": state, "label": _LABELS[n]}
        tl, lg = f.get(".jsonl"), f.get(".log")
        row["mtime"] = tl.st_mtime if tl else None
        row["size"] = tl.st_size if tl else 0
        row["started"] = lg.st_ctime if lg else row["mtime"]  # when the run was launched: the log is created at the start
        if state in ("running", "queued"):
            row["progress"] = progress(n)
            if j:
                row["args"] = j["args"]
        out.append(row)
    return out


def finish_anim(req):
    """Turn the PNG frames the viewer uploaded into one animated WebP or GIF (Pillow) under runs/_export/."""
    try:
        from PIL import Image
    except ImportError:
        return {"error": "nopillow"}
    import shutil
    d = os.path.join("runs", "_export", os.path.basename(str(req.get("id", ""))))
    files = sorted(glob.glob(os.path.join(d, "*.png")))
    if not files:
        return {"error": "no frames"}
    fmt = "gif" if req.get("fmt") == "gif" else "webp"
    fps = max(1, min(60, int(req.get("fps", 10))))
    name = "".join(c for c in str(req.get("name", "replay")) if c.isalnum() or c in "_-") or "replay"
    out = os.path.join("runs", "_export", "%s.%s" % (name, fmt))
    imgs = [Image.open(f).convert("RGB") for f in files]
    if fmt == "webp":
        q = int(req.get("quality", 90))
        opts = {"lossless": True} if q >= 100 else {"quality": q, "method": 4}
        imgs[0].save(out, "WEBP", save_all=True, append_images=imgs[1:], duration=int(1000 / fps), loop=0, **opts)
    else:
        pal = [im.quantize(256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE) for im in imgs]
        pal[0].save(out, "GIF", save_all=True, append_images=pal[1:], duration=int(1000 / fps), loop=0, disposal=1)
    shutil.rmtree(d, ignore_errors=True)
    return {"url": "/runs/_export/" + os.path.basename(out), "bytes": os.path.getsize(out), "frames": len(files), "size": list(imgs[0].size)}


def tail(name, off):
    """(bytes of runs/<name>.jsonl from off up to its last complete line, next offset, reset).

    The viewer loads a timeline with from=0 and then, while the run is still going, asks only for
    what was appended. reset: the file is shorter than off (a rerun replaced it): served from 0."""
    p = os.path.join("runs", os.path.basename(name) + ".jsonl")
    with open(p, "rb") as fh:
        fh.seek(0, 2)
        size = fh.tell()
        reset = off > size
        if reset:
            off = 0
        fh.seek(off)
        b = fh.read(size - off)
    cut = b.rfind(b"\n") + 1
    return b[:cut], off + cut, reset


class H(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        # static files (viewer.html, lab.html, backdrops): revalidate every time (a 304 when unchanged), so an
        # edited page is never served stale from the browser's heuristic cache
        if not getattr(self, "_cc", False):
            self.send_header("Cache-Control", "no-cache")
        self._cc = False
        super().end_headers()

    def send_bytes(self, b, ctype, code=200, extra=(), compress=True):
        # gzip JSON for the browser (the run list and summaries shrink ~8x); level 5 costs ~1 ms per 100 KB
        if compress and len(b) > 2048 and "gzip" in self.headers.get("Accept-Encoding", ""):
            b = gzip.compress(b, 5)
            extra = list(extra) + [("Content-Encoding", "gzip")]
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(b)))
        self.send_header("Cache-Control", "no-store")
        self._cc = True
        for k, v in extra:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(b)

    def send_json(self, obj, code=200):
        self.send_bytes(json.dumps(obj, separators=(",", ":")).encode(), "application/json", code)

    def do_GET(self):
        p = self.path.split("?", 1)
        path, q = p[0], (p[1] if len(p) > 1 else "")
        if path == "/runs.json":                       # viewer.html's run list
            return self.send_json([n + ".jsonl" for n in (r["name"] for r in list_runs())])
        if path == "/api/runs":
            return self.send_json(list_runs())
        if path == "/api/summary":                     # name=A&name=B[&lite=1]: lite leaves out the curves
            qs = parse_qs(q)
            files = scan_runs()[0] if len(qs.get("name", [])) > 20 else None
            sums = [summarise(n, files.get(n, {}) if files is not None else None) for n in qs.get("name", [])]
            return self.send_json([lite(s) for s in sums] if qs.get("lite") == ["1"] else sums)
        if path == "/api/tail":                        # name=A&from=N: timeline bytes appended since offset N
            qs = parse_qs(q)
            try:
                b, nxt, reset = tail(qs["name"][0], int(qs.get("from", ["0"])[0]))
            except (KeyError, ValueError, OSError):
                return self.send_json({"error": "no timeline"}, 404)
            return self.send_bytes(b, "text/plain", extra=[("X-Next", str(nxt)), ("X-Reset", "1" if reset else "0"),
                                                           ("X-Live", "1" if live(qs["name"][0]) else "0")],
                                   compress=False)
        if path == "/api/saves":
            saves = sorted(os.path.basename(p) for p in glob.glob(os.path.join(SAVE_DIR, "*.cam")))
            kor = run_batch.campaign_labels(run_batch.GAME).get("korea", ("Korea", ["", "", ""]))
            names = {"save%d.cam" % i: "%s - %s" % (kor[0], t) for i, t in enumerate(kor[1]) if t}   # as the campaign screen names them
            return self.send_json({"saves": saves, "game": GAME, "titles": TITLES, "names": names})
        if path in ("/", ""):
            self.path = "/lab.html"
        return super().do_GET()

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(n)
        path, _, qs = self.path.partition("?")
        if path == "/api/anim/frame":                      # one PNG of a replay export (viewer.html)
            q = parse_qs(qs)
            d = os.path.join("runs", "_export", os.path.basename(q.get("id", ["x"])[0]))
            os.makedirs(d, exist_ok=True)
            with open(os.path.join(d, "%06d.png" % int(q.get("n", ["0"])[0])), "wb") as fh:
                fh.write(raw)
            return self.send_json({"ok": True})
        req = json.loads(raw or b"{}")
        if path == "/api/anim/finish":
            return self.send_json(finish_anim(req))
        if self.path == "/api/run":
            return self.send_json({"started": start_runs(req)})
        if self.path == "/api/cancel":                  # queued runs only; a running one finishes
            j = JOBS.get(req.get("name", ""))
            if j and j["state"] == "queued":
                j["state"] = "cancelled"
                return self.send_json({"ok": True})
            return self.send_json({"ok": False}, 400)
        if self.path == "/api/delete":
            name = os.path.basename(req.get("name", ""))
            if not name or running(name):
                return self.send_json({"ok": False}, 400)
            for ext in (".jsonl", ".log", ".sum.json"):
                try:
                    os.remove(os.path.join("runs", name + ext))
                except OSError:
                    pass
            JOBS.pop(name, None)
            SUMS.pop(name, None)
            STATES.pop(name, None)
            return self.send_json({"ok": True})
        self.send_json({"error": "unknown"}, 404)

    def log_message(self, *a):
        pass


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8770
    print("Campaign Lab: http://localhost:%d/   (game dir %s)" % (port, GAME))
    http.server.ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()
