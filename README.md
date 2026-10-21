# CyClones: Remastered 1.0 — developer kit

Everything behind the remaster except the game's own data, which is never included: the engine
and the tools read it from your own CyClones disc image.

| Folder | What it holds |
|---|---|
| `FORMATS.md` | The technical notes: the game's file formats and code as reverse-engineered, how the engine follows them, its additions (sections 5n–5q), the test switches (5d) and the plan (6). |
| `engine/` | The native engine (C++17, SDL3, OpenGL 3.3). `build.sh` builds a Linux binary (for testing) and the Windows x64 one with the MinGW-w64 cross compiler; it needs SDL3 3.4 and dav1d 1.5.1 (built static), at the paths set by `DEPS` at its top. `res/` holds the Windows icon, manifest and version; `third_party/dr_flac.h` decodes the briefings' speech. |
| `launcher/` | The Classic launcher of Phase 1 (Win32, C++): the original game on DOSBox Staging with modern display options. `build.sh` builds it. It isn't part of the 1.0 release. |
| `re/` | How `CYCLONE.EXE` was decompiled (Ghidra, with the Watcom calling convention). |
| `tools/` | Scripts for the formats (maps, sprites, textures, sounds, CD tracks), `hdvideo/` (how the "HD Video" pack is made) and `subtitles/` (how the briefings' subtitles were made). |

The engine's test switches (`--level`, `--shot`, `--frames`, `--fire`, `--save`, `--load` and
the rest) and environment variables (`CYCLONES_LIST`, `CYCLONES_EXIT`, `CYCLONES_GIVE`) are in
`FORMATS.md`, section 5d. A capture run never reads or writes `settings.ini`.

An unofficial fan project by Maurice V. de Castro, not affiliated with or endorsed by the makers
or publishers of CyClones.
