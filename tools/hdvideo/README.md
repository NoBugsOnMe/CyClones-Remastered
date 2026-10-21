# Enhanced cut-scenes: how the HD Video pack is made

Run from a work folder holding `src/<VIDEO>/NNNN.png` (every frame of every `GAME\*.SMK`, from
`ffmpeg -i X.SMK -vsync 0 src/X/%04d.png`), `models/` (Real-ESRGAN's `realesr-general-x4v3.pth`
and `realesr-general-wdn-x4v3.pth`, from its GitHub releases) and `speech/<id>.wav` (the cleaned
speech, see `encode.sh`'s notes in FORMATS.md section 5p).

1. `python3 esrgan.py build models/x4v3_d05.onnx 0.5`: the network as ONNX (no torch needed).
2. (Not in the pack: the CGI play as the game's own.) `python3 cgi.py RAVEN INT1 INT2 INT3 4 7 11 14 22`
   upscales the CGI frames 4x into `hd/`; `CGI="..." ./encode.sh` puts them in.
3. `python3 live.py 1A2A 1A2B 1A3 30 31 32 33 34 35 36A 36B 37 38 39 40`: the briefings' video
   windows into `live/` (and `live/<V>.runs`).
4. `./encode.sh`: AV1 (SVT-AV1, 10-bit) and FLAC into `pack/` with `index.txt`. Copy `pack/` to
   `HD Video` beside the engine.

Needs Python 3 with numpy, Pillow, opencv-python-headless, onnx and onnxruntime, and ffmpeg built
with libsvtav1. Paths at the tops of the scripts are this machine's; change them to yours.
