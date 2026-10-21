# CyClones sprite lumps (monsters/objects/items/sprites sections):
#   u16 origin_x, u16 width, width x u16 column offsets (0 = empty column)
#   column: u8 y_end (inclusive), u8 y_start, then (y_end - y_start + 1) palette indices stored from
#   the top of the post (y_end) downward; y counts up from the ground; index 0 = transparent
from PIL import Image

def decode(d, pal):
    ox = int.from_bytes(d[0:2], 'little')
    w = int.from_bytes(d[2:4], 'little')
    cols = []
    h = 1
    for i in range(w):
        o = int.from_bytes(d[4 + 2 * i:6 + 2 * i], 'little')
        if o == 0 or o >= len(d):
            cols.append(None)
            continue
        ye, ys = d[o], d[o + 1]
        if ye < ys:
            cols.append(None)
            continue
        cols.append((ye, d[o + 2:o + 2 + ye - ys + 1]))
        h = max(h, ye + 1)
    im = Image.new('RGBA', (max(1, w), h), (0, 0, 0, 0))
    px = im.load()
    for x, c in enumerate(cols):
        if not c:
            continue
        ye, pixels = c
        for j, p in enumerate(pixels):
            row = h - 1 - (ye - j)          # image row 0 is the top; sprite y 0 is the ground
            if p != 0 and 0 <= row < h:
                px[x, row] = (pal[p * 3], pal[p * 3 + 1], pal[p * 3 + 2], 255)
    return im, ox
