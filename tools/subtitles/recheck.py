#!/usr/bin/env python3
"""Third opinion: Whisper turbo (large-v3-turbo) on each speech file, the VAD stretches grouped
into windows of up to ~25 s so the model hears whole sentences, plus each stretch alone, and the
first seconds before the first stretch."""
import sys
import numpy as np
import sherpa_onnx
sys.path.insert(0, '/home/claude/asr')
from transcribe import load16k, segments, run

ASR = '/home/claude/asr'
T = f'{ASR}/sherpa-onnx-whisper-turbo'

tur = sherpa_onnx.OfflineRecognizer.from_whisper(
    encoder=f'{T}/turbo-encoder.int8.onnx', decoder=f'{T}/turbo-decoder.int8.onnx',
    tokens=f'{T}/turbo-tokens.txt', language='en', task='transcribe', num_threads=4)

for id_ in sys.argv[1:]:
    x = load16k(id_)
    segs = [(s / 16000.0, s / 16000.0 + len(a) / 16000.0) for s, a in segments(x)]
    print(f'===== {id_}  ({len(x)/16000:.2f} s)', flush=True)
    # windows
    win = []
    for a, b in segs:
        if win and b - win[0][0] > 25:
            t0, t1 = win[0][0], win[-1][1]
            print(f'  [{t0:6.2f}-{t1:6.2f}] {run(tur, x[int(t0*16000):int(t1*16000)])[0]}', flush=True)
            win = []
        win.append((a, b))
    if win:
        t0, t1 = win[0][0], win[-1][1]
        print(f'  [{t0:6.2f}-{t1:6.2f}] {run(tur, x[int(t0*16000):int(t1*16000)])[0]}', flush=True)
    for a, b in segs:
        print(f'   {a:6.2f}-{b:6.2f} T: {run(tur, x[int(a*16000):int(b*16000)])[0]}', flush=True)
