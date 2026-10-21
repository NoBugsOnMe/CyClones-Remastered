#!/usr/bin/env python3
"""The live-action briefings: the video window (77,47 164x106) of every frame that shows the
briefing bezel, temporally denoised, debanded and scaled 4x smoothly (Lanczos, a light unsharp
mask). The bezel, the maps and the wireframes stay the original pixels (the engine draws them).

  live.py VIDEO...   -> live/VIDEO/NNNN.png (NNNN = the frame number, 1-based) + live/VIDEO.runs
"""
import os, subprocess, sys
import numpy as np
from PIL import Image

GAME = '/home/claude/cyclones/iso/GAME'
REF = 'src/30/0005.png'          # the bezel with an empty window
RECT = (77, 47, 164, 106)


def bezel_frames(v):
    ref = np.array(Image.open(REF).convert('RGB')).astype(np.float32)
    x, y, w, h = RECT
    mask = np.ones((200, 320), bool)
    mask[y:y + h, x:x + w] = False
    out = []
    for fn in sorted(os.listdir(f'src/{v}')):
        a = np.array(Image.open(f'src/{v}/{fn}').convert('RGB')).astype(np.float32)
        out.append(np.abs(a - ref)[mask].mean() < 2)
    return out


def runs(flags):
    r, i = [], 0
    while i < len(flags):
        j = i
        while j < len(flags) and flags[j] == flags[i]:
            j += 1
        r.append((flags[i], i, j - 1))
        i = j
    return r


def main():
    x, y, w, h = RECT
    for v in sys.argv[1:]:
        flags = bezel_frames(v)
        rr = runs(flags)
        os.makedirs(f'live/{v}', exist_ok=True)
        with open(f'live/{v}.runs', 'w') as f:
            f.write(' '.join(f"{'B' if b else '.'}{b1 - b0 + 1}" for b, b0, b1 in rr) + '\n')
        for b, a0, a1 in rr:
            if not b:
                continue
            vf = (f"select='between(n\\,{a0}\\,{a1})',crop={w}:{h}:{x}:{y},format=rgb24,"
                  f"hqdn3d=3:3:6:6,deband=1thr=0.02:2thr=0.02:3thr=0.02:range=8:blur=1,"
                  f"scale={w * 4}:{h * 4}:flags=lanczos,unsharp=5:5:0.35:5:5:0")
            subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', f'{GAME}/{v}.SMK', '-vf', vf, '-vsync', '0',
                            '-start_number', str(a0 + 1), f'live/{v}/%04d.png'], check=True)
        print(v, ' '.join(f"{'B' if b else '.'}{b1 - b0 + 1}" for b, b0, b1 in rr), flush=True)


if __name__ == '__main__':
    main()
