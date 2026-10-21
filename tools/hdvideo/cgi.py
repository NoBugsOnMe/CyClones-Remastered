#!/usr/bin/env python3
"""Upscale the CGI videos' frames 4x (320x200 -> 1280x800).

Per frame: the game's flat text and lines (exact palette colours) are lifted out and the holes
inpainted; the 256-colour banding is smoothed within half a step (dequant); Real-ESRGAN
general-x4v3 (denoise 0.5) upscales; the text goes back on top, pixel-sharp at 4x.

  cgi.py VIDEO...      (frames from src/VIDEO/NNNN.png to hd/VIDEO/NNNN.png, resumable)
"""
import os, sys, time
import numpy as np, cv2
from PIL import Image
import esrgan, dequant

TEXT = {
    # the credits (red names, blue titles, two tones each) and the blue wipe lines
    'RAVEN': dict(colours=[(255, 0, 0), (223, 4, 28), (0, 0, 255), (0, 0, 199)], rows=(0, 200)),
    # the copyright line under the logo
    'INT3': dict(colours=[(255, 44, 69), (223, 4, 28)], rows=(184, 197)),
}


def text_mask(a, cfg):
    m = np.zeros(a.shape[:2], bool)
    for c in cfg['colours']:
        m |= (a == np.array(c, np.uint8)).all(axis=2)
    r0, r1 = cfg['rows']
    m[:r0] = False
    m[r1:] = False
    return m


def stars(a, y):
    """Put back the faint single-pixel stars (and pin-point lights) the denoising upscale drops:
    a pixel brighter than its 5x5 median by 12+ on a dark ground becomes a small soft dot at least
    as bright as it was."""
    f = a.astype(np.float32)
    med = cv2.medianBlur(a, 5).astype(np.float32)
    r = f - med
    lum = r.max(axis=2)
    star = (lum > 12) & (med.max(axis=2) < 48)
    if not star.any():
        return y
    s = np.zeros((800, 1280, 3), np.float32)
    ys, xs = np.where(star)
    s[ys * 4 + 2, xs * 4 + 2] = r[ys, xs]
    s = cv2.GaussianBlur(s, (0, 0), 1.3) * (2 * np.pi * 1.3 * 1.3)
    base = cv2.resize(med, (1280, 800), interpolation=cv2.INTER_LINEAR)
    return np.maximum(y, base + s)


def main():
    up = esrgan.Upscaler('/home/claude/hdvid/models/x4v3_d05.onnx', threads=int(os.environ.get('THREADS', '2')))
    rng = np.random.default_rng(1)
    for v in sys.argv[1:]:
        os.makedirs(f'hd/{v}', exist_ok=True)
        files = sorted(os.listdir(f'src/{v}'))
        t0 = time.time()
        done = 0
        for fn in files:
            out = f'hd/{v}/{fn}'
            if os.path.exists(out):
                continue
            a = np.array(Image.open(f'src/{v}/{fn}').convert('RGB'))
            key = v.split('_')[0]
            m = text_mask(a, TEXT[key]) if key in TEXT else None
            src = a
            if m is not None and m.any():
                src = cv2.inpaint(a, m.astype(np.uint8) * 255, 3, cv2.INPAINT_TELEA)
            dq = dequant.dequant(src)
            x = (dq / 255.0).transpose(2, 0, 1)[None].astype(np.float32)
            y = up.s.run(None, {'input': x})[0][0].transpose(1, 2, 0) * 255.0
            y = stars(a if m is None else src, y)
            if m is not None and m.any():
                m4 = np.kron(m, np.ones((4, 4), bool))
                t4 = np.kron(a, np.ones((4, 4, 1), np.uint8)).astype(np.float32)
                # soften only the outermost ring of each text pixel a touch, so the edges sit in
                # the smooth picture without fringes
                alpha = cv2.GaussianBlur(m4.astype(np.float32), (0, 0), 0.6)
                alpha = np.maximum(alpha, m4.astype(np.float32) * 0.0)
                alpha = np.where(m4, 1.0, alpha * 0.8)[..., None]
                y = y * (1 - alpha) + t4 * alpha
            # 8-bit with a little triangular dither so the smooth gradients don't band again
            y = y + (rng.random(y.shape, np.float32) - rng.random(y.shape, np.float32))
            Image.fromarray(np.clip(y + 0.5, 0, 255).astype(np.uint8)).save(out, compress_level=1)
            done += 1
            if done % 25 == 0:
                print(f'{v} {fn} {done} frames {(time.time() - t0) / done:.2f}s/frame', flush=True)
        print(f'{v} done', flush=True)


if __name__ == '__main__':
    main()
