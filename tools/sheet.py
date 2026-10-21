# sheet.py out.png cols crop(x,y,w,h|-) scale img1 img2 ... : tile screenshots into one contact sheet
import sys
from PIL import Image
out, cols, crop, scale = sys.argv[1], int(sys.argv[2]), sys.argv[3], float(sys.argv[4])
ims = []
for p in sys.argv[5:]:
    im = Image.open(p).convert('RGB')
    if crop != '-':
        x, y, w, h = map(int, crop.split(','))
        im = im.crop((x, y, x + w, y + h))
    if scale != 1:
        im = im.resize((int(im.width * scale), int(im.height * scale)), Image.LANCZOS)
    ims.append(im)
w, h = ims[0].size
rows = (len(ims) + cols - 1) // cols
sheet = Image.new('RGB', (cols * w + (cols - 1) * 4, rows * h + (rows - 1) * 4), (20, 20, 20))
for i, im in enumerate(ims):
    sheet.paste(im, ((i % cols) * (w + 4), (i // cols) * (h + 4)))
sheet.save(out)
print(out, sheet.size)
