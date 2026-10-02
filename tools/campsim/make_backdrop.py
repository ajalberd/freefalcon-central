"""Write the theater's ground imagery as a PNG for viewer.html.

    python make_backdrop.py [terrain_dir] [out.png]

Output is 2 px per campaign kilometre with north up (1024 km -> 2048 px).
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "campaign-editor"))
import numpy as np                      # noqa: E402
from ffcamp import terrain              # noqa: E402

src = sys.argv[1] if len(sys.argv) > 1 else r"C:\FreeFalcon6\terrdata\korea"
out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, "backdrop.png")
t = terrain.Terrain(src)
ov = t.overview().astype(np.float32)           # 4 px/km, row 0 = south
h, w, _ = ov.shape
half = ov.reshape(h // 2, 2, w // 2, 2, 3).mean(axis=(1, 3)).astype(np.uint8)
half = half[::-1]                               # north up
open(out, "wb").write(terrain.write_png(np.ascontiguousarray(half)))
print(out, half.shape, "size_km", t.size_km)
