import sys, struct, math
import numpy as np
sys.path.insert(0, '/home/claude/cyclones/tools')
from bol import Bol
from PIL import Image

CD = Bol('/home/claude/cyclones/iso/GAME/CYCLONE.CD')
HD = Bol('/home/claude/cyclones/install/CYCLONE.HD')
PAL = np.array([(v * 4 + v // 16) for v in HD.get('play_pal')], np.uint8).reshape(256, 3)

def sprite(name, bol=CD):
    d = bol.get(name)
    ox, w = struct.unpack_from('<HH', d)
    h = 1; cols = []
    for i in range(w):
        o = struct.unpack_from('<H', d, 4 + 2 * i)[0]
        if not o: cols.append(None); continue
        ye, ys = d[o], d[o + 1]
        cols.append((ys, np.frombuffer(d[o + 2:o + 3 + ye - ys], np.uint8))); h = max(h, ye + 1)
    img = np.zeros((h, w), np.uint8)  # row 0 = ground
    for x, c in enumerate(cols):
        if c is None: continue
        img[c[0]:c[0] + len(c[1]), x] = c[1][::-1]   # stored top-down
    return img, ox

def carve(views, tol=0):
    """views: list of (img, ox, angle_rad, mirror). angle = viewer direction around the model (CCW from front).
    Returns occupancy grid occ[y, z, x] and color grid (palette index)."""
    R = max(max(ox, img.shape[1] - ox) for img, ox, a, m in views) + 1
    H = max(img.shape[0] for img, *_ in views)
    xs = np.arange(-R, R) + 0.5
    X, Z = np.meshgrid(xs, xs)          # [z, x]
    occ = np.ones((H, 2 * R, 2 * R), bool)
    votes = np.zeros((H, 2 * R, 2 * R), np.int16)
    for img, ox, a, mirror in views:
        # viewer at direction (sin a, cos a) in (x,z) plane from model; model front faces +z (toward viewer at a=0)
        # screen-right vector for viewer looking at model:
        rx, rz = math.cos(a), -math.sin(a)
        u = X * rx + Z * rz
        if mirror: u = -u
        col = np.floor(u + ox).astype(int)
        w = img.shape[1]
        valid = (col >= 0) & (col < w)
        colc = np.clip(col, 0, w - 1)
        sil = np.zeros((H, w), bool); sil[:img.shape[0]] = img > 0
        if tol:
            from scipy.ndimage import binary_dilation
            sil = binary_dilation(sil, iterations=tol)
        inside = sil[:, colc] & valid[None]   # [y, z, x]
        votes += inside
    occ = votes >= len(views) - (0 if tol >= 0 else -tol)
    return occ, R, votes

def render(occ, R, views_for_color, yaw, scale=3):
    """Quick orthographic render with per-voxel color from best-facing view (front projection)."""
    H = occ.shape[0]
    ys, zs, xs = np.nonzero(occ)
    px = xs - R + 0.5; pz = zs - R + 0.5
    # color: pick view whose direction best matches voxel position direction (approx normal)
    ang = np.arctan2(px, pz)
    best = None; cols = np.zeros(len(xs), np.uint8)
    bestd = np.full(len(xs), 9.0)
    for img, ox, a, mirror in views_for_color:
        d = np.abs((ang - a + np.pi) % (2 * np.pi) - np.pi)
        rx, rz = math.cos(a), -math.sin(a)
        u = px * rx + pz * rz
        if mirror: u = -u
        c = np.clip(np.floor(u + ox).astype(int), 0, img.shape[1] - 1)
        yy = np.clip(ys, 0, img.shape[0] - 1)
        v = img[yy, c]
        sel = (d < bestd) & (v > 0)
        cols[sel] = v[sel]; bestd[sel] = d[sel]
    # project for viewer at yaw
    rx, rz = math.cos(yaw), -math.sin(yaw)
    fx, fz = math.sin(yaw), math.cos(yaw)   # toward viewer
    u = px * rx + pz * rz; depth = px * fx + pz * fz
    W = 2 * R
    img = np.zeros((H, W, 3), np.uint8) + np.array([255, 0, 255], np.uint8)
    zb = np.full((H, W), -1e9)
    ui = np.floor(u + R).astype(int)
    order = np.argsort(depth)
    for i in order:
        if 0 <= ui[i] < W:
            img[ys[i], ui[i]] = PAL[cols[i]]
    im = Image.fromarray(img[::-1])
    return im.resize((W * scale, H * scale), Image.NEAREST)

def rotset(base_names, n_unique, step_sign=1):
    """Build view list for a rotation set. 5 unique -> 8 views (mirrored), 8 unique -> 8 views."""
    views = []
    imgs = [sprite(n) for n in base_names]
    if n_unique == 5:
        for k in range(8):
            if k <= 4: img, ox = imgs[k]; m = False
            else: img, ox = imgs[8 - k]; m = True
            views.append((img, (ox if not m else img.shape[1] - ox), step_sign * k * math.pi / 4, m))
    else:
        for k in range(8):
            img, ox = imgs[k]
            views.append((img, ox, step_sign * k * math.pi / 4, False))
    return views

def optimize_offsets(views, tol=1, rng=3, passes=2):
    views = list(views)
    for p in range(passes):
        for k in range(len(views)):
            best = None
            for dx in range(-rng, rng + 1):
                trial = list(views)
                img, ox, a, m = views[k]
                trial[k] = (img, ox + dx, a, m)
                occ, R, votes = carve(trial, tol)
                s = occ.sum()
                if best is None or s > best[0]: best = (s, dx)
            img, ox, a, m = views[k]
            views[k] = (img, ox + best[1], a, m)
    return views
