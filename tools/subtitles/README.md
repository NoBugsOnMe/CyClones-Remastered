# Briefing subtitles

The subtitles in `HD Video/subtitles/*.srt` (one per `GAME\SOUND\<id>.RAW`, times from the start
of that speech) were made in three steps. The speech is the cleaned-up `HD Video/speech/<id>.flac`
(here read as `<id>.wav` from the folder named in `transcribe.py`).

1. `transcribe.py OUTDIR 225 226 202 ...` cuts each speech into spoken stretches (Silero VAD) and
   writes draft SubRip files from NVIDIA Parakeet TDT 0.6B v2, with Whisper small.en's reading of
   each stretch in `report.json` beside them.
2. `recheck.py 225 226 ...` prints Whisper large-v3-turbo's reading of each stretch and of whole
   sentences (up to 25 s at a time), which settles most places where the first two differ.
3. `final.py OUTDIR` writes the proofread cues (the drafts' times; ` | ` splits a long line into
   two cues timed by length).

Models (sherpa-onnx 1.13, `pip install sherpa-onnx`), from
`https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/`:
`sherpa-onnx-nemo-parakeet-tdt-0.6b-v2-int8`, `sherpa-onnx-whisper-small.en`,
`sherpa-onnx-whisper-turbo`, and `silero_vad.onnx`. Paths are set at the top of each script.

The files are plain text: edit a line or a time and the game picks it up the next time it starts.
The engine shows the text in capitals and drops characters its font lacks.
