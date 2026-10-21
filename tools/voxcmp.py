# voxcmp.py out.png bol base rot vox : original rotations (top) vs model rendered from the same angles (bottom)
import sys, math, struct
import numpy as np
from PIL import Image
sys.path.insert(0, '/home/claude/cyclones/tools')
sys.path.insert(0, '/home/claude/cyclones/tools/proto')
from bol import Bol
from carve import sprite
HD = Bol('/home/claude/cyclones/install/CYCLONE.HD')
PAL = np.array([(v * 4 + v // 16) for v in HD.get('play_pal')], np.uint8).reshape(256, 3)
out, bolp, base, rot, voxf = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), sys.argv[5]
B = Bol(bolp); names = [l[0] for l in B.lumps]; bi = names.index(base)
vox = np.loadtxt(voxf, dtype=np.int32).reshape(-1, 4)
S = 4
def spr(i):
    img, ox = sprite(names[i], B)
    h, w = img.shape
    rgb = np.zeros((h, w, 3), np.uint8) + 40
    m = img > 0; rgb[m] = PAL[img[m]]
    return rgb[::-1], ox
def render(yaw, W, H, ox):
    xs, ys, zs, cs = vox.T
    rx = xs + 0.5; rz = zs + 0.5
    # viewer at angle yaw (CCW): right = (cos a, -sin a), toward viewer = (sin a, cos a)
    u = rx * math.cos(yaw) - rz * math.sin(yaw)
    d = rx * math.sin(yaw) + rz * math.cos(yaw)
    img = np.zeros((H, W, 3), np.uint8) + 40
    zb = np.full((H, W), -1e9)
    for i in np.argsort(d):
        px = int(math.floor(u[i] + ox)); py = H - 1 - ys[i]
        if 0 <= px < W and 0 <= py < H: img[py, px] = PAL[cs[i]]
    return img
tiles = []
n = 8
for k in range(n):
    if rot == 5:
        src = k if k <= 4 else 8 - k
        rgb, ox = spr(bi + src)
        if k > 4: rgb = rgb[:, ::-1]; ox = rgb.shape[1] - ox
    else:
        rgb, ox = spr(bi + k)
    H, W = rgb.shape[:2]
    pad = 20
    a = Image.fromarray(rgb).resize((W * S, H * S), Image.NEAREST)
    bimg = render(k * math.pi / 4, W + 2 * pad, H, ox + pad)
    b = Image.fromarray(bimg).resize(((W + 2 * pad) * S, H * S), Image.NEAREST)
    t = Image.new('RGB', ((W + 2 * pad) * S, 2 * H * S + 8), (0, 0, 0))
    t.paste(a, (pad * S, 0)); t.paste(b, (0, H * S + 8))
    tiles.append(t)
Wt = sum(t.width + 6 for t in tiles); Ht = max(t.height for t in tiles)
sheet = Image.new('RGB', (Wt, Ht), (0, 0, 0)); x = 0
for t in tiles: sheet.paste(t, (x, 0)); x += t.width + 6
sheet = sheet.resize((sheet.width // 2, sheet.height // 2), Image.NEAREST)
sheet.save(out); print(out, sheet.size)
