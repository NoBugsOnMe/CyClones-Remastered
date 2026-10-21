# Generates an original launcher icon: a targeting reticle on a dark rounded tile.
import sys
from PIL import Image, ImageDraw

def draw(size):
    s = 4  # supersample
    W = size * s
    im = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    r = int(W * 0.18)
    d.rounded_rectangle([0, 0, W - 1, W - 1], radius=r, fill=(12, 20, 34, 255))
    # subtle inner border
    d.rounded_rectangle([int(W*0.03)]*2 + [W - 1 - int(W*0.03)]*2, radius=int(r*0.85),
                        outline=(40, 64, 92, 255), width=max(1, W // 64))
    cx = cy = W / 2
    acc = (64, 196, 255, 255)
    lw = max(2, int(W * 0.06))
    R = W * 0.30
    d.ellipse([cx - R, cy - R, cx + R, cy + R], outline=acc, width=lw)
    # crosshair ticks crossing the ring
    t0, t1 = W * 0.12, W * 0.38
    for a, b in [((cx, t0), (cx, t1)), ((cx, W - t1), (cx, W - t0)),
                 ((t0, cy), (t1, cy)), ((W - t1, cy), (W - t0, cy))]:
        d.line([a, b], fill=acc, width=lw)
    # centre dot
    c = W * 0.045
    d.ellipse([cx - c, cy - c, cx + c, cy + c], fill=(255, 92, 72, 255))
    return im.resize((size, size), Image.LANCZOS)

sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
imgs = [draw(s) for s in sizes]
imgs[-1].save(sys.argv[1], format="ICO", sizes=[(s, s) for s in sizes], append_images=imgs[:-1])
imgs[-1].save(sys.argv[1].replace(".ico", "_256.png"))
