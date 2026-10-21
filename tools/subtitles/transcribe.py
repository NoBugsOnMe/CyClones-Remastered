#!/usr/bin/env python3
"""Draft subtitles for the briefings' speech.

Each cleaned speech file (16 kHz) is cut into spoken stretches by Silero VAD; each stretch is
recognised by NVIDIA Parakeet TDT 0.6B v2 (punctuated, with word times) and, as a cross-check,
by Whisper small.en. The stretches become SubRip cues (long ones split at word boundaries, about
two lines of the game's font at most). A report lists where the two recognisers differ.

  transcribe.py OUTDIR ID...
"""
import os, subprocess, sys, json
import numpy as np
import sherpa_onnx

ASR = '/home/claude/asr'
SPEECH = '/home/claude/hdvid/speech'
P = f'{ASR}/sherpa-onnx-nemo-parakeet-tdt-0.6b-v2-int8'
W = f'{ASR}/sherpa-onnx-whisper-small.en'
MAX_CHARS = 84      # about two lines of subtitle
MAX_SECONDS = 6.5


def load16k(id_):
    raw = subprocess.run(['ffmpeg', '-v', 'error', '-i', f'{SPEECH}/{id_}.wav', '-ac', '1', '-ar', '16000',
                          '-af', 'aresample=resampler=soxr', '-f', 's16le', '-'], capture_output=True, check=True).stdout
    return np.frombuffer(raw, np.int16).astype(np.float32) / 32768.0


def segments(x):
    c = sherpa_onnx.VadModelConfig()
    c.silero_vad.model = f'{ASR}/silero_vad.onnx'
    c.silero_vad.threshold = 0.45
    c.silero_vad.min_silence_duration = 0.30
    c.silero_vad.min_speech_duration = 0.20
    c.silero_vad.max_speech_duration = 15
    c.sample_rate = 16000
    vad = sherpa_onnx.VoiceActivityDetector(c, buffer_size_in_seconds=200)
    out = []
    win = c.silero_vad.window_size
    for i in range(0, len(x), win):
        vad.accept_waveform(x[i:i + win])
        while not vad.empty():
            out.append((vad.front.start, np.array(vad.front.samples, np.float32)))
            vad.pop()
    vad.flush()
    while not vad.empty():
        out.append((vad.front.start, np.array(vad.front.samples, np.float32)))
        vad.pop()
    return out


def recognisers():
    par = sherpa_onnx.OfflineRecognizer.from_transducer(
        encoder=f'{P}/encoder.int8.onnx', decoder=f'{P}/decoder.int8.onnx', joiner=f'{P}/joiner.int8.onnx',
        tokens=f'{P}/tokens.txt', num_threads=2, model_type='nemo_transducer')
    wh = sherpa_onnx.OfflineRecognizer.from_whisper(
        encoder=f'{W}/small.en-encoder.int8.onnx', decoder=f'{W}/small.en-decoder.int8.onnx',
        tokens=f'{W}/small.en-tokens.txt', language='en', task='transcribe', num_threads=2)
    return par, wh


def run(rec, samples):
    s = rec.create_stream()
    # a little silence each side helps both models with the first and last words
    pad = np.zeros(int(0.15 * 16000), np.float32)
    s.accept_waveform(16000, np.concatenate([pad, samples, pad]))
    rec.decode_stream(s)
    r = s.result
    ts = [t - 0.15 for t in getattr(r, 'timestamps', [])]
    return r.text.strip(), list(r.tokens), ts


def words(tokens, ts):
    """Parakeet's sentencepiece tokens ('▁' starts a word) to (word, start) pairs."""
    out = []
    for tok, t in zip(tokens, ts):
        if tok.startswith('▁') or not out:
            out.append([tok.lstrip('▁'), t])
        else:
            out[-1][0] += tok
    return [(w, t) for w, t in out if w]


def fmt(t):
    t = max(0.0, t)
    h = int(t // 3600); m = int(t // 60) % 60; s = int(t) % 60; ms = int(round((t - int(t)) * 1000))
    if ms == 1000:
        s += 1; ms = 0
    return f'{h:02d}:{m:02d}:{s:02d},{ms:03d}'


def cues_for(seg_t0, seg_t1, text, ws):
    """One cue, or several split at word boundaries (prefer after punctuation)."""
    if len(text) <= MAX_CHARS and seg_t1 - seg_t0 <= MAX_SECONDS or len(ws) < 2:
        return [(seg_t0, seg_t1, text)]
    n = max(2, int(np.ceil(max(len(text) / MAX_CHARS, (seg_t1 - seg_t0) / MAX_SECONDS))))
    target = len(text) / n
    pieces, cur, start = [], [], seg_t0
    for i, (w, t) in enumerate(ws):
        cur.append(w)
        joined = ' '.join(cur)
        last = i == len(ws) - 1
        nxt_t = ws[i + 1][1] if not last else seg_t1
        punct = w[-1:] in '.,?!;:'
        if not last and (len(joined) >= target and (punct or len(joined) >= target * 1.25) or len(joined) >= MAX_CHARS - 8):
            pieces.append((start, seg_t0 + nxt_t - 0.02 if False else nxt_t, joined))
            cur, start = [], nxt_t
    if cur:
        pieces.append((start, seg_t1, ' '.join(cur)))
    return pieces


def main():
    outdir = sys.argv[1]
    os.makedirs(outdir, exist_ok=True)
    par, wh = recognisers()
    report = []
    for id_ in sys.argv[2:]:
        x = load16k(id_)
        cues = []
        for start, samp in segments(x):
            t0 = start / 16000.0
            t1 = t0 + len(samp) / 16000.0
            ptext, toks, ts = run(par, samp)
            wtext, _, _ = run(wh, samp)
            if not ptext and not wtext:
                continue
            ws = [(w, t0 + t) for w, t in words(toks, ts)] if ts else []
            for a, b, t in cues_for(t0, t1, ptext or wtext, ws):
                cues.append((a, b, t))
            report.append({'id': id_, 't0': round(t0, 2), 't1': round(t1, 2), 'parakeet': ptext, 'whisper': wtext})
        # small gaps between cues close up, so the text doesn't flicker off between phrases
        fixed = []
        for i, (a, b, t) in enumerate(cues):
            if i + 1 < len(cues) and cues[i + 1][0] - b < 0.6:
                b = cues[i + 1][0]
            fixed.append((a, max(b, a + 0.8), t))
        with open(f'{outdir}/{id_}.srt', 'w', encoding='utf-8') as f:
            for i, (a, b, t) in enumerate(fixed, 1):
                f.write(f'{i}\n{fmt(a)} --> {fmt(b)}\n{t}\n\n')
        print(id_, len(fixed), 'cues', flush=True)
    with open(f'{outdir}/report.json', 'w') as f:
        json.dump(report, f, indent=1)


if __name__ == '__main__':
    main()
