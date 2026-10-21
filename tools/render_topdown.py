# Top-down render of a CyClones level: floor flats per cell + north/west wall edges.
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bol import Bol
from cymap import CyMap
from PIL import Image, ImageDraw
HD, CD, MAP, OUT = sys.argv[1:5]
hd, cd = Bol(HD), Bol(CD)
pal = [min(255, v * 4 + v // 16) for v in hd.get('play_pal')]
names = [n for n, o, s in cd.lumps]
sw, sf = names.index('startwalls'), names.index('startflats')
def lump(i):
    n, o, s = cd.lumps[i]; return cd.data[o:o + s]
flat_cache, wall_col = {}, {}
def flat(v):
    if v not in flat_cache:
        im = Image.frombytes('P', (64, 64), lump(sf + v)); im.putpalette(pal)
        flat_cache[v] = im.convert('RGB')
    return flat_cache[v]
def wallcolor(v):
    if v not in wall_col:
        d = lump(sw + v); px = d[130:]
        r = sum(pal[p * 3] for p in px) // len(px); g = sum(pal[p * 3 + 1] for p in px) // len(px); b = sum(pal[p * 3 + 2] for p in px) // len(px)
        wall_col[v] = (min(255, r + 60), min(255, g + 60), min(255, b + 60))
    return wall_col[v]
m = CyMap(MAP)
C = 32
img = Image.new('RGB', (m.width * C, m.height * C), (10, 10, 14))
dr = ImageDraw.Draw(img)
for y in range(m.height):
    for x in range(m.width):
        v = m.floor.tex[y * 64 + x] & 0x3fff
        if v:
            img.paste(flat(v).resize((C, C)), (x * C, y * C))
for y in range(m.height):
    for x in range(m.width):
        i = y * 64 + x
        n, w = m.north.tex_front[i], m.west.tex_front[i]
        if n:
            dr.line([(x * C, y * C), ((x + 1) * C, y * C)], fill=wallcolor(n), width=3)
        if w:
            dr.line([(x * C, y * C), (x * C, (y + 1) * C)], fill=wallcolor(w), width=3)
img.save(OUT)
print('walls/flats markers at', sw, sf, 'saved', OUT)
