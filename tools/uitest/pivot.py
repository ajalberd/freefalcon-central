# Recon zoom pivot: run recon_pivot.txt, copy uitest\out\pivot_*.bmp + the layout JSON into <dir>, then
#   python tools\uitest\pivot.py <dir> <range_before / range_after>
# B (zoomed in by s) = A magnified by s about the pivot c; grid-search c, then refine.
import json, struct, sys
import numpy as np

def bmp(path):
    d = open(path, 'rb').read()
    off, = struct.unpack_from('<I', d, 10)
    w, h = struct.unpack_from('<ii', d, 18)
    row = (w * 3 + 3) & ~3
    a = np.frombuffer(d, np.uint8, row * abs(h), off).reshape(abs(h), row)[:, :w * 3].reshape(abs(h), w, 3)
    if h > 0:
        a = a[::-1]
    return a.astype(np.float32).mean(axis=2)

folder, s = sys.argv[1], float(sys.argv[2])
A, B = bmp(folder + '/pivot_a.bmp'), bmp(folder + '/pivot_b.bmp')
lay = json.load(open(folder + '/pivot.json'))
sw, sh = lay['surface']
H, W = A.shape
kx, ky = W / sw, H / sh  # surface -> window pixels
win = {w['id']: w for w in lay['windows']}
rw = win['RECON_WIN']
c0 = rw['clients'][0]
pane = [(rw['rect'][0] + c0[0]) * kx, (rw['rect'][1] + c0[1]) * ky, c0[2] * kx, c0[3] * ky]
mask = np.zeros((H, W), bool)
px, py, pw, ph = [int(v) for v in pane]
mask[py + 4:py + ph - 4, px + 4:px + pw - 4] = True
for k, v in win.items():
    if k.startswith('RECON_LIST'):
        x, y, w_, h_ = v['rect']
        mask[int(y * ky):int((y + h_) * ky) + 2, int(x * kx):int((x + w_) * kx) + 2] = False

ys, xs = np.nonzero(mask)
sel = np.random.default_rng(1).choice(len(xs), min(40000, len(xs)), replace=False)
xs, ys = xs[sel], ys[sel]

def err(cx, cy):
    sx = cx + (xs - cx) / s
    sy = cy + (ys - cy) / s
    ok = (sx >= 0) & (sx < W - 1) & (sy >= 0) & (sy < H - 1) & mask[sy.astype(int).clip(0, H - 1), sx.astype(int).clip(0, W - 1)]
    if ok.sum() < 1000:
        return 1e18
    return float(np.mean((B[ys[ok], xs[ok]] - A[sy[ok].astype(int), sx[ok].astype(int)]) ** 2))

best = None
step = max(4, int(pw / 60))
for cy in range(py, py + ph, step):
    for cx in range(px, px + pw, step):
        e = err(cx, cy)
        if best is None or e < best[0]:
            best = (e, cx, cy)
e, bx, by = best
for d in (step // 2, step // 4, 1):
    for cy in range(by - 2 * d, by + 2 * d + 1, max(1, d)):
        for cx in range(bx - 2 * d, bx + 2 * d + 1, max(1, d)):
            ee = err(cx, cy)
            if ee < e:
                e, bx, by = ee, cx, cy
print(f'window {W}x{H} pane x{px}..{px+pw} y{py}..{py+ph} centre ({px+pw/2:.0f},{py+ph/2:.0f})')
print(f'pivot ({bx},{by}) err {e:.1f}  offset from pane centre ({bx-(px+pw/2):+.0f},{by-(py+ph/2):+.0f}) px '
      f'= ({(bx-(px+pw/2))/pw*100:+.1f}%, {(by-(py+ph/2))/ph*100:+.1f}%) of the pane')
