"""Campaign Lab: launch campsim runs, watch them, compare them, replay them.

    python serve.py [port]          ->  http://localhost:8770/   (lab.html; viewer.html?run=NAME)

Runs land in runs/<save>[-tag]-s<seed>.jsonl/.log exactly as run_batch.py writes them, so
runs started from the command line show up here too. Summaries are cached next to each
timeline (<name>.sum.json) because a 10-day timeline is 20-50 MB.
"""
import glob
import http.server
import json
import os
import re
import sys
import threading
import time
import types
from concurrent.futures import ThreadPoolExecutor

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


def summarise(name):
    """Small JSON describing one run; cached by timeline/log mtime."""
    tl, log = os.path.join("runs", name + ".jsonl"), os.path.join("runs", name + ".log")
    cache = os.path.join("runs", name + ".sum.json")
    stamp = [os.path.getmtime(p) if os.path.exists(p) else 0 for p in (tl, log)]
    if os.path.exists(cache):
        try:
            c = json.load(open(cache))
            if c.get("_stamp") == stamp:
                return c
        except ValueError:
            pass
    out = {"name": name, "_stamp": stamp}
    meta, first, last, fin, frames = None, None, None, None, 0
    curve = []                                 # per frame: t, blue objs, red objs, DPRK supply, DPRK ac, ROK ac
    if os.path.exists(tl):
        with open(tl) as fh:
            for ln in fh:
                if not ln.strip():
                    continue
                try:
                    o = json.loads(ln)
                except ValueError:
                    break                      # a run still writing its last line
                if o.get("meta"):
                    meta = o
                elif o.get("final"):
                    fin = o
                else:
                    frames += 1
                    first = first or o
                    last = o
                    st = o.get("st") or []
                    curve.append([o["t"], side(o, BLUE, 4), side(o, RED, 4),
                                  st[6][0] if len(st) > 6 else None,
                                  side(o, RED, 2), side(o, BLUE, 2), side(o, BLUE, 0), side(o, RED, 0)])
    if meta:
        out["seed"] = meta.get("seed")
        out["daysPlanned"] = meta.get("days")
        out["scenario"] = meta.get("scenario")
    if first and last:
        out["days"] = round(last["t"] / 1440.0, 2)
        out["obj"] = [[side(first, BLUE, 4), side(first, RED, 4)], [side(last, BLUE, 4), side(last, RED, 4)]]
        out["gnd"] = [[side(first, BLUE, 0), side(first, RED, 0)], [side(last, BLUE, 0), side(last, RED, 0)]]
        out["air"] = [[side(first, BLUE, 2), side(first, RED, 2)], [side(last, BLUE, 2), side(last, RED, 2)]]
        step = max(1, len(curve) // 240)
        out["curve"] = curve[::step]
    out["done"] = bool(fin)
    out["endgame"] = fin.get("endgame") if fin else None
    events, knobs, crash = [], [], None
    if os.path.exists(log):
        with open(log, errors="replace") as fh:
            for ln in fh:
                m = EVENT_RE.search(ln)
                if m:
                    ev = int(m.group(1))
                    events.append({"id": ev, "min": int(m.group(2)), "supply": int(m.group(3)),
                                   "ac": int(m.group(4)), "title": TITLES.get(str(ev), "")})
                elif ln.startswith("KNOB"):
                    knobs.append(ln.strip()[5:])
                elif "ERR" in ln[:4] or "EXCEPTION" in ln or "0xC0000005" in ln:
                    crash = ln.strip()[:200]
                m = RESULT_RE.search(ln)
                if m:
                    out["wall"] = int(m.group(2))
    out["events"], out["knobs"], out["crash"] = events, knobs, crash
    if out["done"] or not running(name):
        try:
            json.dump(out, open(cache, "w"))
        except OSError:
            pass
    return out


def running(name):
    j = JOBS.get(name)
    return bool(j and j["state"] in ("queued", "running"))


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
    JOBS[name]["state"] = "running"
    JOBS[name]["started"] = time.time()
    try:
        run_batch.run_one(a, seed)
        log = open(os.path.join("runs", name + ".log"), errors="replace").read()
        JOBS[name]["state"] = "done" if "RESULT seed=" in log else "failed"
    except Exception as e:  # noqa: BLE001
        JOBS[name]["state"] = "failed"
        JOBS[name]["error"] = str(e)


def list_runs():
    names = sorted({os.path.basename(p)[:-6] for p in glob.glob("runs/*.jsonl")} | set(JOBS),
                   key=lambda n: -max([os.path.getmtime(p) for p in glob.glob("runs/" + glob.escape(n) + ".*")] or [time.time()]))
    out = []
    for n in names:
        if n.startswith("_"):
            continue
        j = JOBS.get(n)
        row = {"name": n, "state": j["state"] if j else "done"}
        tl = os.path.join("runs", n + ".jsonl")
        row["mtime"] = os.path.getmtime(tl) if os.path.exists(tl) else None
        row["size"] = os.path.getsize(tl) if os.path.exists(tl) else 0
        if running(n):
            row["progress"] = progress(n)
            row["args"] = j["args"]
        out.append(row)
    return out


class H(http.server.SimpleHTTPRequestHandler):
    def send_json(self, obj, code=200):
        b = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        p = self.path.split("?", 1)
        path, q = p[0], (p[1] if len(p) > 1 else "")
        if path == "/runs.json":                       # viewer.html's run list
            return self.send_json([n + ".jsonl" for n in (r["name"] for r in list_runs())])
        if path == "/api/runs":
            return self.send_json(list_runs())
        if path == "/api/summary":
            names = [n for n in q.split("&") if n.startswith("name=")]
            from urllib.parse import unquote
            return self.send_json([summarise(unquote(n[5:])) for n in names])
        if path == "/api/saves":
            saves = sorted(os.path.basename(p) for p in glob.glob(os.path.join(SAVE_DIR, "*.cam")))
            return self.send_json({"saves": saves, "game": GAME, "titles": TITLES})
        if path in ("/", ""):
            self.path = "/lab.html"
        return super().do_GET()

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        req = json.loads(self.rfile.read(n) or b"{}")
        if self.path == "/api/run":
            return self.send_json({"started": start_runs(req)})
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
            return self.send_json({"ok": True})
        self.send_json({"error": "unknown"}, 404)

    def log_message(self, *a):
        pass


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8770
    print("Campaign Lab: http://localhost:%d/   (game dir %s)" % (port, GAME))
    http.server.ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()
