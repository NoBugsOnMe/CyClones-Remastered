# Dump CyClones wall + flat textures as PNG sheets (decoding hypothesis test).
import sys
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
# usage: python dump_x.py <CYCLONE.HD> <CYCLONE.CD> <out_dir>
HD, CD, OUT = sys.argv[1], sys.argv[2], sys.argv[3]
from bol import Bol
from PIL import Image

hd = Bol(HD)
cd = Bol(CD)
pal6 = hd.get('play_pal')
pal = [min(255, v * 4 + v // 16) for v in pal6]

def wall_image(d):
    h = int.from_bytes(d[0:2], 'little') * 4
    ncols = (len(d) - 130) // h
    im = Image.new('P', (ncols, h))
    im.putpalette(pal)
    px = im.load()
    for x in range(ncols):
        col = d[130 + x * h: 130 + (x + 1) * h]
        for y in range(h):
            px[x, y] = col[y]
    return im

def flat_image(d):
    im = Image.frombytes('P', (64, 64), d)
    im.putpalette(pal)
    return im

walls = [(n, cd.data[o:o+s]) for sec, n, o, s in cd.sections() if sec == 'walls']
flats = [(n, cd.data[o:o+s]) for sec, n, o, s in cd.sections() if sec == 'flats']

def sheet(items, fn, cell=(128, 128), cols=16, scale=1):
    rows = (len(items) + cols - 1) // cols
    out = Image.new('RGB', (cols * cell[0], rows * cell[1]), (40, 0, 40))
    for i, (n, d) in enumerate(items):
        try:
            im = fn(d).convert('RGB')
        except Exception as e:
            continue
        im.thumbnail(cell)
        out.paste(im, ((i % cols) * cell[0], (i // cols) * cell[1]))
    return out

sheet(walls[:64], wall_image).save(OUT + '/walls_sheet1.png')
sheet(flats[:64], flat_image, cell=(64, 64)).save(OUT + '/flats_sheet1.png')
wall_image(cd.get('wall1')).convert('RGB').resize((256, 512), Image.NEAREST).save(OUT + '/wall1_big.png')
print(len(walls), len(flats))
