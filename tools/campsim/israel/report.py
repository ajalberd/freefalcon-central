import sys, glob, json, os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import run_batch as r
runs = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "runs")
for f in sorted(glob.glob(runs + r"\save?-Israel*-s?.jsonl")):
    n = os.path.basename(f)[:-6]
    s = r.summarise(f)
    if not s: print(n, "no output"); continue
    lab = r.end_labels(r.GAME, n).get(s["endgame"], "") if s["endgame"] else ""
    tm = " ".join("%s %d>%d" % (t[0][:3], t[1], t[2]) for t in s["teams"] if t[1] > 100)
    print("%-26s day %5.1f %s %-3s %s | %s" % (n, s["days"], "END" if s["endgame"] else "   ", s["endgame"] or "", lab[:46], tm))
