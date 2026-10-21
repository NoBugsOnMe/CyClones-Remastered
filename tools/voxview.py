# voxview.py out.png file.vox[,file2.vox...] : orthographic turntable previews of voxdump output (palette, face shading)
import sys, math
import numpy as np
from PIL import Image
sys.path.insert(0, '/home/claude/cyclones/tools')
from bol import Bol
HD = Bol('/home/claude/cyclones/install/CYCLONE.HD')
PAL = np.array([(v * 4 + v // 16) for v in HD.get('play_pal')], np.float32).reshape(256, 3)
S = 4
def render(vox, yaw, elev=0.25):
    xs, ys, zs, cs = vox.T
    # rotate around y; camera looks along -dir
    c, s = math.cos(yaw), math.sin(yaw)
    pts = []
    out_w, out_h = 100 * S, 110 * S
    img = np.zeros((out_h, out_w, 3), np.float32) + 40
    zb = np.full((out_h, out_w), -1e9, np.float32)
    # draw each voxel as a projected square splat with 3 visible faces approximated by shading
    rx = xs + 0.5; rz = zs + 0.5; ry = ys + 0.5
    u = rx * c - rz * s
    d = rx * s + rz * c
    v = ry * math.cos(elev) + d * math.sin(elev)
    depth = d * math.cos(elev) - ry * math.sin(elev)
    order = np.argsort(depth)
    shade = 0.8 + 0.25 * np.clip((rx * 0.3 + rz * 0.4) / 30, -1, 1)
    for i in order:
        px = int((u[i] + 50) * S); py = int((95 - v[i]) * S)
        if 0 <= px < out_w - S and 0 <= py < out_h - S:
            img[py:py + S + 1, px:px + S + 1] = PAL[cs[i]]
    return Image.fromarray(np.clip(img, 0, 255).astype(np.uint8))
out = sys.argv[1]
rows = []
for f in sys.argv[2:]:
    vox = np.loadtxt(f, dtype=np.int32).reshape(-1, 4)
    rows.append([render(vox, math.radians(a)) for a in (0, 45, 90, 135, 180, 270)])
W = sum(i.width for i in rows[0]); H = sum(r[0].height for r in rows)
sheet = Image.new('RGB', (W, H))
for ri, r in enumerate(rows):
    x = 0
    for im in r: sheet.paste(im, (x, ri * r[0].height)); x += im.width
sheet = sheet.resize((W // 2, H // 2), Image.LANCZOS)
sheet.save(out); print(out, sheet.size)
