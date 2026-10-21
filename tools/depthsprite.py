# Prototype: "depth sprites". Each original rotation view gets a per-pixel depth taken from the
# carved hull; rendering a view as 3D splats reproduces the sprite exactly at its own angle and
# lets it turn a little either side with parallax.
import sys, math
import numpy as np
from PIL import Image
sys.path.insert(0, '/home/claude/cyclones/tools/proto')
from carve import *
from scipy.ndimage import median_filter, distance_transform_edt

def view_depths(views, occ, R):
    H = occ.shape[0]
    ys, zs, xs = np.nonzero(occ)
    px = xs - R + 0.5; pz = zs - R + 0.5
    out = []
    for img, ox, a, m in views:
        rx, rz = math.cos(a), -math.sin(a); fx, fz = math.sin(a), math.cos(a)
        u = px * rx + pz * rz
        if m: u = -u
        col = np.floor(u + ox).astype(int)
        d = px * fx + pz * fz
        h, w = img.shape
        depth = np.full((h, w), np.nan)
        for c, y, dd in zip(col, ys, d):
            if 0 <= c < w and y < h and (np.isnan(depth[y, c]) or dd > depth[y, c]): depth[y, c] = dd
        mask = img > 0
        # fill pixels the hull missed from the nearest known depth, then smooth
        known = ~np.isnan(depth)
        if known.any():
            idx = distance_transform_edt(~known, return_distances=False, return_indices=True)
            depth = depth[tuple(idx)]
        depth = median_filter(depth, size=3)
        depth[~mask] = np.nan
        out.append(depth)
    return out

def render_splats(views, depths, k, yaw, S=4, W=110, H=100):
    img, ox, a, m = views[k]
    d = depths[k]
    h, w = img.shape
    rx, rz = math.cos(a), -math.sin(a); fx, fz = math.sin(a), math.cos(a)
    if m: rx, rz = -rx, -rz
    out = np.zeros((H * S, W * S, 3), np.uint8) + 40
    zb = np.full((H * S, W * S), -1e9)
    pts = []
    for y in range(h):
        for c in range(w):
            if img[y, c] == 0: continue
            u = c + 0.5 - ox
            X = u * rx + d[y, c] * fx; Z = u * rz + d[y, c] * fz
            pts.append((X, y + 0.5, Z, img[y, c]))
    cr, cs = math.cos(yaw), -math.sin(yaw); cf, cg = math.sin(yaw), math.cos(yaw)
    for X, Y, Z, col in pts:
        u = X * cr + Z * cs; dd = X * cf + Z * cg
        px0 = int((u + W / 2 - 0.5) * S); py0 = int((H - Y - 0.5) * S)
        for py in range(py0, py0 + S + 1):
            for px in range(px0, px0 + S + 1):
                if 0 <= px < W * S and 0 <= py < H * S and dd > zb[py, px]:
                    zb[py, px] = dd; out[py, px] = PAL[col]
    return Image.fromarray(out)

if __name__ == '__main__':
    base, n = sys.argv[1], int(sys.argv[2])
    names = [l[0] for l in CD.lumps]; bi = names.index(base)
    v = rotset(names[bi:bi + n], n, 1)
    v = optimize_offsets(v, 1)
    occ, R, votes = carve(v, 1)
    deps = view_depths(v, occ, R)
    tiles = []
    for ang in (0, 10, 20, 30, 40, 45):
        a = math.radians(ang)
        k = 0 if ang < 22.5 else 1
        tiles.append(render_splats(v, deps, k, a))
    W = sum(t.width for t in tiles)
    sheet = Image.new('RGB', (W, tiles[0].height))
    x = 0
    for t in tiles: sheet.paste(t, (x, 0)); x += t.width
    sheet = sheet.resize((W // 2, tiles[0].height // 2), Image.NEAREST)
    sheet.save(sys.argv[3]); print(sys.argv[3], sheet.size)
