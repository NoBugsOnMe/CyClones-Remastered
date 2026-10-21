import sys
from PIL import Image, ImageDraw
path = sys.argv[1]; out = sys.argv[2]
d = open(path, 'rb').read()
n = (len(d) - 16) // 4096
cell = 128
cols = 10
rows = (n + cols - 1) // cols
sheet = Image.new('RGB', (cols * cell, rows * (cell + 14)), (30, 30, 30))
dr = ImageDraw.Draw(sheet)
for i in range(n):
    p = d[16 + i * 4096: 16 + (i + 1) * 4096]
    vals = sorted(set(p))
    # map distinct values to spread-out grays/colors for visibility
    lut = {v: (0 if v == 0 else 60 + int(195 * (k / max(1, len(vals) - 1)))) for k, v in enumerate(vals)}
    im = Image.frombytes('L', (64, 64), bytes(lut[v] for v in p)).resize((cell, cell), Image.NEAREST)
    x, y = (i % cols) * cell, (i // cols) * (cell + 14)
    sheet.paste(im.convert('RGB'), (x, y + 14))
    dr.text((x + 2, y + 1), f'{i} d={len(vals)}', fill=(255, 220, 120))
sheet.save(out)
print(n, 'planes; trailing bytes', (len(d) - 16) % 4096)
