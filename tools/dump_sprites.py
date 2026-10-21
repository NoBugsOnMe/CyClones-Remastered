import sys
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
# usage: python dump_x.py <CYCLONE.HD> <CYCLONE.CD> <out_dir>
HD, CD, OUT = sys.argv[1], sys.argv[2], sys.argv[3]
from bol import Bol
from sprite import decode
from PIL import Image
hd = Bol(HD)
cd = Bol(CD)
pal = [min(255, v * 4 + v // 16) for v in hd.get('play_pal')]
def sheet(arc, section, out, n=80, cell=96, cols=16):
    items = [(nm, arc.data[o:o+s]) for sec, nm, o, s in arc.sections() if sec == section][:n]
    rows = (len(items) + cols - 1) // cols
    img = Image.new('RGB', (cols * cell, rows * cell), (60, 60, 70))
    bad = 0
    for i, (nm, d) in enumerate(items):
        try:
            im, ox = decode(d, pal)
        except Exception:
            bad += 1
            continue
        im.thumbnail((cell - 4, cell - 4))
        img.paste(im, ((i % cols) * cell + 2, (i // cols) * cell + 2), im)
    img.save(out)
    print(section, len(items), 'bad', bad)
sheet(cd, 'monsters', OUT + '/monsters_sheet.png', n=128)
sheet(cd, 'objects', OUT + '/objects_sheet.png', n=61)
sheet(hd, 'items', OUT + '/items_sheet.png', n=64)
