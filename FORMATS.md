# CyClones (Raven / SSI, 1994): technical notes for the native port

These notes come from reverse-engineering the 1994 CD release (the image at
SHA-256 `D3943E27…CF68`). **Confirmed** means it was decoded and checked
visually. **Partial** means the structure is known but not every field.

## 1. Disc layout (raw image, 2352-byte sectors, 213,902 sectors)

| Track | Type | Start (LBA) | Notes |
|---|---|---|---|
| 1 | MODE1/2352 data | 0 | ISO 9660 volume "CYCLONES", 45,142 sectors |
| 2 | Audio | 45,292 | Pregap 45,142–45,291 is encoded as mode-1 sectors |
| 3–30 | Audio | pregap + 150 | Pregaps start at 55953, 63371, 70707, 78327, 85363, 92698, 100290, 107889, 115480, 122560, 129588, 136715, 143892, 151167, 158539, 165911, 173505, 181306, 182268, 183293, 183895, 184479, 185384, 186751, 194155, 201491, 209128, 210849 |

Tracks 2–30 hold about 37.5 minutes of Red Book CD audio: the music, plus
short stingers in tracks 19–24. `tools/make_cue.py` writes the matching CUE
sheet.

## 2. Files

Data track root:
- `DATA.A00` is an ARJ archive holding the hard-disk install: 176 files, 6.1 MB unpacked
  (`CYCLONE.HD`, `SOUND\*.RAW` the effects, the executables, drivers and the manual), 171 of
  them packed with method 1 and 5 stored (method 0); one volume, nothing garbled. 7-Zip extracts
  it. Since 1.0 the engine reads `CYCLONE.HD` and the effects straight from it (`data/arj.cpp`:
  the headers, CRC-32 checked, and methods 1–3 decoded as Robert K. Jung's public-domain UNARJ
  does), so it needs nothing but the disc image; `--hd` still takes an installed copy.
- `GAME\` holds the CD-resident data:
  - `*.MAP` / `*.GLB`: 22 levels
  - `CYCLONE.CD`: textures and monsters
  - `*.SMK`: Smacker videos
  - `*.SSD`: video sound sync
  - `SOUND\2xx.RAW`: speech for the videos
- `ACROBAT\` holds Acrobat Reader 2 and the manual, `CYCLONES.PDF`.

Hard-disk install (`C:\CYCLONE`):

| File | What it is |
|---|---|
| `CYCLONES.EXE` | Borland C++ 16-bit front end: plays videos with `SMKPLAY.EXE` and runs `CYCLONE.EXE 8335791`, then `-restart` between missions (section 5f). |
| `SMKPLAY.EXE` | The video player ("cycplay", Watcom/DOS4G, with symbols): Smacker, HMI SOS for speech, MSCDEX for the CD music. |
| `CYCLONE.EXE` | The game: Watcom C/C++32, DOS/4GW 1.95, LE executable, 700 KB. |
| `CYCLONE.HD` | BOL archive: UI, HUD, items, player sprites, palettes, math tables. |
| `PIC.BOL` | BOL archive: palette and fade table. |
| `TUT.DAT` | Tutorial script data (format not decoded yet). |
| `SOUND\*.RAW` | Sound effects `1.RAW`–`152.RAW`: 8-bit unsigned mono PCM, no header, played at 11,025 Hz (see section 5e). |
| `KEYMAP.BIN`, `KEYSAVER.BIN` | Key bindings (the CONFIGURE screen writes them). `KEYMAP.BIN` at 0xb68 also holds the MUSIC and SOUND volumes (u32 each, 0–100; shipped as 99 and 70). |
| `CYCLONE.CFG` | Text file: CD drive letter, HD path, CD game path, HD sound path, CD sound path (CRLF). |
| `SOUNDHMI.CFG` | 50-byte HMI SOS driver config (see below). |

`CYCLONE.EXE` command line (developer switches, only after the magic number):
`8335791 [-D(E|M|H)] [-W(1-8)] [-L<lvl#>] [<mapname>] [-A(1-3)] [-K] [x y] [z]`

`SOUNDHMI.CFG`, as written by `SSETUP.EXE` for a Sound Blaster 16 at
A220 I7 D1:
`u32 device_id = 0xE015, u32 port = 0x220, u32 irq = 7, u32 dma = 1, u32 0,
char name[30] = "Sound Blaster 16 …"`. "No sound" is `device_id = 0xFFFFFFFF`.

### The demo ("Version 2", January 1995)

A folder, not a CD: the full game's programs (`CYCLONE.EXE` of 3 January 1995) set up to read the
"CD" from `.\ODATA\` (its `CYCLONE.CFG`: drive `Z`, then `.\`, `.\odata\`, `.\sound\`,
`.\odata\`). `ODATA` holds `CYCLONE.CD`, one level of its own (`DEMO.MAP`/`.GLB`, "Code Name:
Stalag": 75 creatures, 27 items, 35 doors) and `RAVEN.SMK`/`.SSD` (the logos); `CYCLONE.HD` and
`SOUND\*.RAW` (30 effects) sit at its top. Both BOL archives keep the full game's lump order and
count, the lumps the demo doesn't use left as empty entries named `\n` (HD, 232 of them) or with no name
(CD, 1,098), so every index the code uses still holds; only the first three weapons' art is there
(`i_gaussp`, `i_chaing`, `i_alpstl`). Its texts: `introtxt` (the welcome on the options screen
at start), `level1` (the briefing), `finaltxt` (the congratulations at the end), and the same as
`introtext`, `brieftext`, `endtext` at the end of `CYCLONE.CD`; their lines run wider than the
full game's. `DEMO.MAP`'s one way out is action 25, the end of the game. The demo's program can
play HMI MIDI music (`.hmp`, with `MELODIC.BNK`/`DRUM.BNK` for FM cards) but no music files come
with it, and there is no CD audio, so it plays in silence.

The engine (1.1) opens such a folder in place of a disc image (`DiscImage` folder mode: `GAME/x`
is looked for in `ODATA`, anything else at the top, any case), looking in its own folder, the
folders inside it, the folder above and the folders inside that when there is no disc image, or
taking `--disc <folder>`. It starts at the folder's level when there is no BASE; shows `introtxt`
on the options screen at the start and `level1` for a level outside the mission table; sets a
text again to the screen's width, a paragraph at a time, when a line is wider than it (only the
demo's are); gives with START WEAPONS: ALL only weapons whose art is there, and leaves the others
off the weapon strip; and marks EXTRAS playlists whose videos aren't there.

## 3. BOL lump archive (confirmed)

The format is WAD-like and little-endian:

```
header (16 bytes): u16 lump_count; u32 dir_offset; u32 dir_size; u8 pad[6]
directory at dir_offset:
  lump_count × { u32 offset; u32 size; u32 name_offset }   // name_offset is relative to dir_offset
  then NUL-terminated names
```

Zero-size lumps are section markers: `startflats`/`endflats`, `startwalls`,
`startmonsters`, `startobjects`, `startitems`, `startsprites`, and so on.

- `CYCLONE.CD` has 1,307 entries: 347 walls, 167 flats, 714 monster frames,
  61 objects, plus `options2` (a 320×200 raw image) and its palette.
- `CYCLONE.HD` has 806 entries: 204 items, 129 player and weapon sprites,
  266 font glyphs, HUD and option-screen art, `play_pal`, `automap_pal`,
  `sintable`, `costable`, `tantable`, `TINT_MAP` and `surfanim`.

Reader: `tools/bol.py`.

## 4. Graphics (confirmed)

- **Palettes** (`play_pal`, `palette`, `options_pal`, `automap_pal`): 768
  bytes of 6-bit VGA RGB (0–63). Convert to 8-bit with `v*4 + v/16`.
- **Flats** (floors and ceilings): 64×64 raw, row-major, 4,096 bytes.
- **Walls**: column-major.
  - `u16 height/4`, then `u16 colofs[64]`, then 64 columns × height bytes
    of pixels starting at byte 130.
  - Heights are 32, 64, 96 or 128 (other widths exist too). Many textures
    stack two materials vertically.
  - `colofs[i]` is the column start plus a per-texture vertical bias of
    ±32 bytes. It's used for wrap alignment, and its exact meaning is
    still to be confirmed against the renderer.
- **Sprites** (monsters, objects, items, sprites):
  - `u16 origin_x; u16 width; u16 colofs[width]`. A `colofs` of 0 means an
    empty column.
  - Each column is `u8 y_end (inclusive); u8 y_start; pixels[y_end-y_start+1]`. The pixels are
    stored **from the top of the post down**: pixel `j` is at height `y_end - j`. Heights count up
    from the floor, so `y_start = 0` touches the ground. Previews 1 to 3 read the posts upside down,
    which scrambled every sprite; it was found by comparing against the original running in DOSBox.
  - Palette index 0 is transparent.
- **Full-screen images**: 320×200 raw, 64,000 bytes.
- **Videos**: Smacker 2 (`SMK2`), 320×200 at 8-bit palette, video only (the frame rate in
  the header is overridden by the `.SSD` script). Music and speech come from the `.SSD` script
  (section 5f). The engine has its own decoder (`engine/src/data/smacker.cpp`), checked
  pixel-for-pixel against ffmpeg on every frame tested.

- **Decals** (`decalstart`..`decalend` in `CYCLONE.HD`: `smoke1a-d`, `smoke2a-d`,
  `blood1-3 a-c`, `aph0-2`): `u16 w, u16 h, u16 anchor_x, u16 anchor_y`, then `w*h` pixels row
  by row from the top. `smoke1a-d` is a bullet-impact flash and puff, `smoke2c/d` a smoke puff,
  and `blood1/2/3` are red, green and purple blood spurts. `nanodth2*` and `nanohit1*` use a
  different, still unknown layout.
- **Effect sprites** (normal sprite format, `sprites` section of `CYCLONE.HD`): missile
  explosion `rcktexp0-3`, grenade explosion `grnaded1-6`, flaming debris `expbit0`/`expchk0`
  (they burn out through `expbitd0-1`/`expchkd0-2`), projectiles (`gbolt` and `rktprj` come in
  5 rotations × 3 frames; `lasprj`, `rdplsm`, `grenade`, `approj` and `dblproj` are single
  views) and their impact animations (`gboltd`, `lasprjd`, `rdplsmd`, `approjd`, `dblprjd`).
  Barrel wrecks are `barreld1-3` in `CYCLONE.CD`.

Decoders: `tools/sprite.py`, `tools/dump_tex.py`, `tools/dump_sprites.py`.

## 5. Levels (confirmed, from the loader `FUN_00014179` in CYCLONE.EXE)

A `.MAP` file is a stream of chunks: `char tag[4]; u16 type; u32 size; u8 data[size]`.
All 22 levels contain the same 17 chunks, in this order:

| Chunk | Size | Contents |
|---|---|---|
| `MAP ` | 20,484 | `u16 width, height` (playable area, e.g. 40×40), then five u8 layers: **shape**, **static light**, unused, **light type**, unused |
| `MAPC` | 20,480 | Ceiling surface layer (see below) |
| `MAPF` | 20,480 | Floor surface layer |
| `MAPN` | 49,152 | Wall on the **north** edge of each cell (see below) |
| `MAPW` | 49,152 | Wall on the **west** edge of each cell |
| `MAP1`, `MAP2` | 20,480 each | Two extra surface layers (catwalks, ledges, bridges) |
| `CUBE` | 56 × n | Free-standing boxes (crates, tables, shelves, consoles) |
| `OBJ `, `DOOR`, `ITEM`, `MON `, `TRIG` | varies | Objects, doors, items, monsters, triggers (see section 5b) |
| `OTXT`, `DTXT`, `ITXT`, `MTXT` | small | Text for the chunk above: `u16 index, u16 length, chars` repeated |

Every grid layer covers 64×64 cells, stored row by row: u8 layers take 4,096
bytes and u16 layers 8,192. A cell is 64 world units square, and heights use
the same units (1 unit = 1 texel).

**Surface layer** (floor, ceiling, extra 1, extra 2):
- `u16 tex[64×64]`: the low 14 bits are a flat number counted from the
  `startflats` lump, and the top 2 bits are flags.
- `u16 height[64×64]`: the surface height of each cell.
- `u8 light[64×64]`: a signed light offset.

**Shape**: bits 0–2 of `MAP ` layer 0 give the floor shape, and bits 3–5 the
ceiling shape. The corner heights of cell (x,y) are taken from cells (x,y),
(x+1,y), (x+1,y+1) and (x,y+1):

| Shape | Surface |
|---|---|
| 0 | Flat, at the cell's own height (the neighbour values are ignored) |
| 1 | Sloped quad using all four corner heights |
| 2 | Two triangles split on the (x,y)–(x+1,y+1) diagonal |
| 3 | Two triangles split on the (x+1,y)–(x,y+1) diagonal |

**Edge layer** (north/west walls), in this order:
- `u16 tex_front`: a wall number counted from the `startwalls` lump. 0 means
  no wall.
- `u16 tex_back`: 0 means the back uses `tex_front`.
- `u16 base`: the world height of the wall's bottom.
- `u8 flags`: bit 3 (`0x08`) means the wall is not solid (the collision test `FUN_00020105`
  skips it, for north and west walls alike); bit 1 (`0x02`) lets sight through
  (`FUN_00020f42`: windows, grates; textures 8, 12, 14, 18 and 21–24 get it from a table at
  load) and makes the renderer draw it masked (texel 0 clear); bit 4 (`0x10`) lets through
  physics objects flagged `0x20` (`FUN_0004ec41`) and, without bit 1, makes the wall **glass**:
  `FUN_0001cc24` hands it to `FUN_0001746b` in mode 2, whose columns are drawn later with the
  sprites by the unrolled loop at 0x27986, `out = TINT_MAP[dst × 256 + shade[texel]]` (texel 0
  clear). `TINT_MAP` (65,536 bytes in `CYCLONE.HD`) is close to 62 % of the pixel behind plus
  38 % of the glass (a least-squares fit per channel: 0.61 dst + 0.37 src); the engine draws
  such walls last with that blend. LAB2's glass room (cells 13–15, 15–16, textures 321 and 322,
  the second with bullet holes that are holes) and others in nine levels; bit 0
  (`0x01`, on about 6,000 walls across the levels, crates and windows among them) only matters
  to the renderer, which skips such walls when `0xc8016` is set. Up to 4.2 the engine read bit 0
  as "not solid", so crates, windows and many walls could be walked through.
- `u8 light`: a signed light offset.
- `u16 v_start`, `u16 v_end`: the texture rows shown, so the wall is
  `v_end - v_start + 1` units tall and the texture tiles. When both are 0, the
  wall is exactly one texture tall.

The wall spans `base` to `base + span`. In the Subway level, for example:
base 99 + one 128-px texture = 227, which is the ceiling height.

**CUBE record** (u16 fields, from `FUN_00014106` → `FUN_00013b14`):
- [1] x
- [2] z
- [3] base height
- [4] size x (low byte)
- [5] size z (low byte)
- [6] top flat
- [7] bottom flat
- [8..11] side walls (north, east, south, west)
- [13]−[12] height; the sides show texture rows from [12] down (as a wall's `vStart`). When the
  height is 0 or less, the cube takes the height of its side texture (the last non-zero of
  [8..11]) from row 0: that is how the levels make floor-to-ceiling pillars (68 such cubes in 15
  levels, missing from the engine up to 4.6). A block also makes its cell ground: HIVE1's stairs
  at (36, 27)–(31, 29) are blocks 11–17 (base 9, tops 24 to 110) in cells with no floor texture in
  any layer, which the engine took for the outside of the level (`World::hasGround`, 0.5.2). The
  blocks are 63 units wide in 64-unit cells, so a unit-wide seam lies between neighbouring steps
  with nothing under it; `hasGround` takes a block's footprint 2 units wider all round. Without that
  the player's body (whose floor test samples nine points) stopped at a seam whenever one of the
  points landed in it, the likelier the smaller each frame's move: at 60 frames a second a walk
  straight up the middle happened to step over every seam, at 144 and up it stopped on the second
  step of each flight.
  A cube of height 0 or less with no side texture either keeps a height of 0, and is still a box for collision: flat, at its base, which creatures and the player stand on
  1 unit over (the top + 1 of `FUN_00047ed3`). BIOSTOR2's lift at (1, 29) is the one such block
  with a footprint (id 255, 63 × 63, no flats, level with the 300 floor; five more in BASE,
  BIOLAB, HEAVY, HIVE1 and HIVE3 are 0 × 0): in DOSBox (5i) the player standing in that cell is
  at 301 and is carried up to 439–443 by script 38 (block 255 to 442 at speed 4, 1 unit a tick,
  back to 300 at speed 1). The engine gives such a block a height of 1; up to 0.5.2 it ignored
  it, and the lift went up without the player.
- The engine tucks a side lying on a cell edge half a unit into the block, so a wall on that edge
  stays in front of it from the far side (the game's renderer never shows it through the wall).
  Only the side's plane moves; a side keeps its full length unless its end meets a wall, so blocks
  set end to end (the gates below) meet without a gap.
- The top and bottom flats start at the block's own corner ([1], [2]), not at the grid's: the
  desk tops `flat249` / `flat250` hold their picture in the flat's first 32 × 32 texels with
  black round it, and the desks built of them stand off the grid (BIOLAB at (20, 22) and
  (22, 47), LAB2 at (27, 7)). Mapped from the grid (the engine up to 0.5.2), those desks showed a
  black square; from the block's corner no block top in any level shows a black texel.
- [0] is an id (scripts move blocks by it, action 33).
- [14] (low byte) holds the block's **flags**, kept at +0x12 of the block (`FUN_00013b14`) and read
  like a wall's by the block renderer (`FUN_0001e51c`, 0x1eb28): `0x02` draws the sides masked
  (mode 0), otherwise `0x10` draws them as **glass** (mode 2: the columns go to the sprite list and
  are drawn by the TINT_MAP loop at 0x27986, as glass walls are), otherwise solid (mode 1). The
  flags don't change collision: a block stops you whatever they say (`FUN_00020105`, type 8,
  which tests the player's 40 × 40 box against the block's own box). The engine tests the body's
  circle against each block's outline; up to 0.5.2 it only sampled nine points round the body, about
  14 units apart, and MAINTEN2's exit bars (5-unit posts 10 apart, raised by the switch) let you
  through before the switch was thrown.
  46 blocks in 8 levels are glass, nearly all of them energy curtains: the red gates of BIOLAB,
  LAB2 and BIOSTOR2 (`wall234`, 5 units thick; LAB2's and BIOSTOR2's rise into the ceiling by
  action 33) and five small posts in BIOLAB, CLONE's blue laser beams (2 units tall, `wall267`),
  FAILSAF2's white and orange fields, HIVE1's and HIVE2's, MOTHER3's violet ones (`wall236`), and
  two concrete-faced ones in BIOSTOR2. Up to 0.5.2 the engine drew every block solid, so the red
  gates showed as opaque slabs of red.
  Without either flag the sides are drawn solid (mode 1, `FUN_000242db`: the jump table at
  0x72c10 enters an unrolled loop ending at 0x242da that writes every texel through the shade row,
  with no test), so texel 0 comes out as palette 0, black. (The masked loop, 0x25620 on, skips it
  with `jcxz`.) The light table maps colour 0 like any colour: black from row 70 on, dark grey at
  row 55, white at row 0. The engine now draws walls and block sides masked only with 0x02 (and
  the loader's table below) or 0x10; up to 0.5.2 it discarded texel 0 on all of them. 109 solid
  sides show empty texels in the rows they display: 84 crate sides (`wall73`, the gaps between
  planks; `wall74`, the "SSI RAVEN" stencil) in BASE, LAB2, MUSEUM2, SDILAB and CORE, now black
  as in the game; 14 tank walls in MOTHER3 (`wall215`-`219`, the animated range 161-165, an empty
  top half), sealed outside the map at (29-32, 0-2); BIOLAB's desk block at (22, 47) and four
  LAB2 blocks of `wall43` (their empty texels lie outside the columns the sides show); and SDILAB's
  two security-scan monitors. `Renderer::holeAt` (shots and "use" through a texture's holes) now
  counts only masked triangles. Door panels (`wall279`, `wall280` and `door1` have empty texels)
  are drawn as before. The loader (`FUN_00014179`) ORs a per-texture table into each wall's flags
  by its front texture: 2 for textures 8, 12, 14, 18 and 21-24 (no block uses them). It matters
  most for SDILAB's two security-scan monitors
  (blocks 11 and 13 at (4, 25) and (4, 37), flags 0x01, their west side rows 25-60 of `wall287`,
  whose screen, columns 7-57 and rows 28-55, is texel 0): up to 0.5.2 they showed the wall behind;
  now the screen shows colour 0 as the game draws it, until a scan starts (`Renderer::scanFrame`). No other block or wall uses
  `wall287`. As an addition the screen then shows the scan (`game/scan.cpp`): each such side gets
  its own texture number, `Renderer::kScanScreen` + i (i counting the sides block by block), painted
  from the room's script: the scripts that print "Conducting Security Scan" (36 and 37: on the
  player stepping on (3, 25) or (3, 37), "...DANGER... Remain Motionless", counter 7 set to 100,
  waits of 400/900/900 and 300/800/900 eighths of a tick, i.e. 0.5 s, about 1 s and 1 s, then
  "APPROVED" and the way on opens). While counter 7 is 100, script 25 kills a player standing in
  map area 3, the room round the tile: the game's "remain motionless".
  SDI1's nine missile consoles are also walls (cells (16, 17), (25, 17), (41, 17), (16, 33),
  (40, 33), (50, 33), (8, 49), (24, 55) and (40, 55), their north walls, `wall360`, number 303),
  each with a cell script used from that wall (condition 4): sound 92, "CAUTION", a wait of 500,
  "Missile Now Armed", counter 1 up 15 and a creature brought in (action 14); when counter 1
  reaches 105 (seven of them) script 3 opens the silo bay. As an addition, `game/scan.cpp` finds
  such walls (condition 4 or 5, a message starting "Missile", the wall showing `wall360`) and
  `Renderer::setScreenWalls` gives each its own texture, `Renderer::kConsoleScreen` + i, whose
  glass (columns 12-49, rows 53-64) `Renderer::missileFrame` paints for 2.2 s from the script's
  start (power on, a red missile outline traced and filled, ARMING) and then still (ARMED).
  `wall360` also stands on two walls in SDI2 and one in BIOLAB, which stay as they are.

**Lighting**. The renderer picks a row of the 127-row `fadetob` table, where
row 0 is brightest, row 63 neutral and row 126 black:

```
shade = ambient(5) + 4 × (dynamic + animated[light type] + static light) + depth
row   = 63 + clamp(shade, −640, 639) × 63 / 640
```

Light values are signed bytes, and positive values darken. Depth is
camera-space distance in world units, so surfaces fade to black about 10 cells
away. The `TINT_MAP` lump (256×256) is the translucency table.

**Sky** (`FUN_0003e6d9`): `sky1` and `sky2` are 320×160 raw images that sit side by side as one
640×160 panorama for the whole turn, drawn as a flat backdrop before the walls (not mapped on a
dome). The scroll is the 0–255 heading (0 east, counter-clockwise) × 2.5 pixels, and screen column
c of the 320-wide view shows panorama column `(320 − scroll + c) mod 640`: one sky pixel per
column, so the 90° view shows half the panorama and the sky turns at half the walls' rate. The
panorama's bottom row sits on the horizon row (rows above it show the sky's lower rows); rows
below the horizon are cleared to black. The horizon moves with looking up and down between 20 %
and 80 % of the view height (`FUN_00012fa5`: rows 40–160 of 200, about ±20.6°), so the sky's top
reaches the screen's top exactly at the game's highest look; the engine, which can look further,
keeps it there. Up to 4.6 the engine wrapped `sky1` alone four times round a dome, so looking up
showed four moons pinched together at the zenith.

## 5b. Entities (confirmed: all 22 levels and GLB files parse to the exact chunk size)

The game reads these chunks field by field with `fread`, so records have no padding and their
length varies. Tags are compared as strings (`FUN_0001023f`), which is why they never appear as
32-bit constants in the code. The loaders are in `FUN_0001231b` (map) and `FUN_000121a2` (GLB).
Parser: `tools/entities.py`; engine: `src/data/entities.cpp`.

**Common header** (`FUN_00010010`), 21 bytes:
`u16 id; u16 x; u16 z; s16 y; u8 skill; u16; u16; u32; u32`. Positions are world units (a cell is
64). `y` is the height of the surface the entity stands on. `skill` is a difficulty mask: the game
maps easy, normal, hard and "all" to 1, 2, 4 and 7, and skips the entity when `skill & mask == 0`.
When the chunk header's `type` field is non-zero, the header continues with `u32 script`. If that is
non-zero, a script record follows (`FUN_00039f07`):
`u16, u32, u32, u8, u32 next, u32, u8[16], u32 head`, then while `head` is set, actions of
`u16 op, u8[20] args, u32 next`; then `u32 head2, u32`, then while `head2` is set,
`u16, u8[20], u32 next, u32 prev`.

| Record | After the header |
|---|---|
| `OBJ ` (`FUN_0001029b`) | `u16 kind; char sprite[10]; u16; u16; u8[7]; u16; u8[19]`. `sprite` is a lump in the CD's `objects` section (`barrel`, `chair00`, `fan10`). |
| `ITEM` (`FUN_000108dc`) | `u16 kind; u16; char sprite[10]; u16; u16; u16; u16; u8[40]; u16; u8[19]`. `sprite` is an `s_*` lump in `CYCLONE.HD`. |
| `DOOR` (`FUN_00010577`) | `u16 kind; char texture[10]; u16; u16; u8 flags; u16; u8 b[6]; u16 v0; u16 v1; u8[10]` |
| `MON ` (`FUN_00010d05`) | `u16 template; u16 type; u8 m20[20]; u8 b19[19]; u32 carried[8]; u8[8]; u8 b6[6]; u8[97]`, then one `ITEM` record for each non-zero `carried` entry. **`type` (the second u16) is the `BMON` id**; the first is the editor template it was made from and differs in 619 of the 1,254 placements, so reading it gives the wrong creature about half the time (previews up to 3.3 did). See the field table below. |
| `TRIG` | A 64×64 grid of `u32` (16 KB). Each non-zero cell starts a linked list of script records. |

**`MON ` fields** (`FUN_00010d05`, checked against the live monster records at `0xec3xx`):

| Field | Meaning |
|---|---|
| header byte 9 (`u16` at record +0x09, low byte; stored at +0x6f) | **AI class**: 1 chaser, 2 deployer, 3 exploding object, 4 turret, 6 queen, 0 none |
| `m20[0]` (stored at +0x70) | start mode: 1 and 2 active, 13 frozen (every barrel: its AI never runs, it only goes off when killed) |
| `b19[1]` | sight radius in cells (the "noticing" distance, usually 10) |
| `b19[7]`, `b19[10]` | trigger and group numbers used by level scripts |
| `b19[14]` | **hit points** of this placement (the `BMON` `health` field is not used for placed monsters) |
| `b6[0..3]` | armour against the damage types, subtracted through the resist tables at `0x73c0c` |
| `b6[4]`, `b6[5]` | range bands of attacks A and B, in cells (reach = band × 64 + 20) |
| `carried[k]` | an `ITEM` record follows for each non-zero entry; entry 2 is dropped when the monster dies |

**`ITEM` fields**: `kind` indexes the item-name text; `u16` at +0x31 in memory (`i15` after the 40
parameter bytes) is the amount or charge count; the `u16` before the parameters (+0x4d) is the
**item use**: 1 hitscan weapon (weapon index `p[0x19]`), 2 projectile weapon (`p[0x1d]`), 3 ammo
clip (weapon `p[0]`; leftovers stay in the clip), 5 activated (the Razor Saw suit, jet packs), 6
"readied" quest item, 7 demo pack, 8 key (id `p[0]`), 9 bio-kit (+48 health, cap 104), 10 mech-kit
(+5 armour, cap at the suit maximum 10 / 18 / 32), and inside `BMON` records 11 melee and 12 area
blast. Items are not picked up by walking over them: the player points at one and presses "use",
within ±128 units on each axis ("ITEM OUT OF RANGE."), so never through a wall or a shut door.
The engine also takes an item you walk within 48 units of (an option, on by default), but only
with a clear level line to it, at the item's height or at your feet, past walls, blocks and doors
(floors and catwalks don't count). Up to 0.5.2 it didn't check, so BIOSTOR2's red card could be
taken through its locked hatch by standing against it. With the check, every item in the game is
still within reach. Items stay at the height they are placed
at: some sit on props, 20–65 units above the floor (the BIOSTOR2 pedestals, LAB2 tables,
FAILSAF2 shelves, a SDI2 key card), and a few are placed up to 7 units under it (the engine lifts
those onto the floor).

**Monster weapons** are the `ITEM` records embedded in `BMON` (use 1 hitscan, 2 projectile, 11
melee, 12 area blast). Their 40 parameter bytes `p`:

| Bytes | Hitscan and melee | Projectile |
|---|---|---|
| 0–7 | accuracy bonus per range band, 4 × s16 | 2: speed (`u16`, capped at 80, then /8 per tick); 4: rotation mode; 5: lob; 6: fuse (ticks) |
| 8–15 | minimum damage per band, 4 × u16 | 8: direct-hit minimum; 10: splash damage (when `p[18]` is set) |
| 16–23 | maximum damage per band, 4 × u16 | 16: direct-hit maximum |
| 24 | damage type + 1 | 24: impact sprite offset; 26: impact frames; 27: tics per frame; 28: damage type + 1; 30–37: projectile sprite name; 39: flight frames |

Hitscan bands are ≤ 96, 224, 352 and 608 units (2-D distance); melee bands ≤ 80 and 160. The
player's hitscan (`FUN_00031ef2`) does no damage beyond 608 units: the shot still counts as a hit
(blood, the creature is alerted) but no damage is rolled. A hit is
`rand() % 100 < type accuracy (+0x39e) + p[band]` (unsigned compare: a negative chance always
hits). A band with maximum 0 does nothing. A miss still counts as a shot with line of sight, so the
monster keeps attacking.

**The player** (`FUN_0004d35d`, `FUN_0004d5e0`, `FUN_0004dc9a`): a 40 × 40 × 55 box, eye 52 above
the feet, steps up 17 units onto a floor (surface + 52 ≤ eye + 17) and onto the top of a wall it
runs into when that top is no more than 18 above the feet (`FUN_0004dc9a`: base + height + 51 ≤
eye + 17). A jump sets the vertical speed (`0xef405`, 16.16, shifted down 5) to 3 units a tick;
every following tick in the air takes 0.1465 off it before moving, so a jump peaks 32.2 units
up after 21 ticks (a continuous curve with the same numbers would peak at 30.7). The 48-unit
ledges in LAB2 (cells 34, 44–51: a riser 49 tall) are within reach only this way: 332.2 ≥ 349
− 18. A creature stands in the player's way however short it is: up to 0.5.2 the engine let you
walk over anything under a step's height, and MAINTEN2's and SDI2's rats (no weapon, sight 0 in the
level data, so they never attack in the game either) ended up under your feet, stuck, since no
creature steps into the player's cell. Speeds per second: forward 344 (running 594), back 219 (469), strafe
281 (500); velocity closes 1/8 of the gap every tick; gravity 1,465 units/s²; jump 300 units/s.
A level starts with health 104, armour 10 (suit 0, maximum 10) and the gauss pistol with 50
rounds. Damage to the player (`FUN_00032cf2`, `FUN_00032f0e`): on easy `d −= d >> 2`; then,
while armour > 0, armour absorbs by damage type (0: all of it; 1, 2, 3, 5: `armour >> shift` with
the suit's shifts {3,8,8,8} / {2,2,8,2} / {1,1,1,2}; 4: none) and wears by
`raw × {10,10,20,5,30,0}[type] / 100`. So plain type-0 bullets weaker than your armour do
nothing at all until other hits wear it down.

**Doors** (loader `FUN_00010577`, set up by `FUN_00013199`): `x, z` is the cell's corner and
`b[2]` is the panel's position across the cell (32 is the middle). Flag `0x10` means the panel
runs east–west, so `z += b[2]`; otherwise `x += b[2]`. The panel is `v1 - v0` units tall (one
texture tall when 0), starting at texture row `v0`. Flag `0x80` makes the panel slide along its
run; without it the panel moves up and down (runtime flag `0x02`). `b[1]` is the speed in units
per tick, and a negative speed reverses the motion (runtime `0x04`): a sliding panel goes toward
−x or −z instead of +x or +z, a vertical one sinks instead of rising. Flags `0x02`, `0x20` and
`0x40` give the starting state: open, opening, closing (otherwise closed). Flag `0x01` locks the
door (runtime `0x08`) until the player holds key `b[0]` (`FUN_00042bda` looks it up among eight
held keys; key cards give keys, see "Keys and scripts on doors" below). In the u16 after
the flags, `0x8000` keeps the door open once opened (runtime `0x20`). A door's panel is only seen
and only blocks inside its own cell, between its base and top (`FUN_0001fe65`, `FUN_0001ffbe`):
a rising panel disappears into its lintel, a sliding one into the wall beside it. Open, a panel
has risen its height less 1 or slid 63 units (`FUN_000132a5`: the counter at +0x19 stops at 0x3f
or height − 1), and it stops blocking once less than 5 units of it are left in the doorway (the
engine stopped 4 and 6 units short up to 0.5.2, leaving a solid post between double doors). Wide
doorways are several door records side by side with no link between them; the level scripts open
them together. In 59 of the 70 such pairs in the game each panel's own script toggles its partner
(op 2) when it is used. In BIOSTOR2 the two panels of each double door slide toward each other and
vanish at the seam (the left one at +2, the right one at −2), as the clipping to the cell makes
them. The engine also opens neighbouring panels in one line together, but only where no script
moves either (from before it ran the scripts; doing it for script-linked pairs made the script's
toggle shut the partner again, up to 0.5.2). The engine draws a panel as a slab whose two faces
each read left to right from their own side, as walls do (up to 4.6 the slab was inside out: from
either side the far face showed, mirrored and set back). The slab is 6 units thick, but no thicker
than keeps it inside its own cell, so a panel on the cell's edge is flat and flush with the walls
there (BIOSTOR2's key-card hatch at (27, 9), offset 0: up to 0.5.2 it stood 3 units out of the wall
as a little box): 2 × the distance to the nearer edge, less 1, between 0.5 and 6. MUSEUM2's sliding
marble walls (`wall30`, offsets 1, 2 and 62) sit just behind the brick wall of the next room, and
the 6-unit slab came through that wall up to 0.5.2. Up to 4.3 the engine read
`0x02`/`0x20`/`0x40` as slide styles and lifted everything else, so sliding doors (BASE's house
door, for one) went up or down. The slab has no top (bottom) where a floor, ceiling or extra
surface of its cell lies within 1.5 units of its top (bottom): SDI2's sliding doors at (4, 35)
and (1, 35), 100 to 228 on the floor below the launch room, met that room's floor (`MAP2`, 228)
with their tops, which showed through it as two dark streaks up to 0.5.2. Along its line the slab
stops half a unit short of the cell's edges (`Renderer::buildDoors`; collision and shots still use
the whole panel): its thin ends lay in the plane of the walls there, and where such a wall faces
another room the end showed through it, a light strip the door's height and 6 units wide (HIVE3:
door 17 at (8, 12), north-south, seen end on through the north wall of (8, 12) from the corridor
(8, 7)–(8, 11), up to 0.5.2).

**Keys and scripts on doors.** Picking up a key item (use 8) puts its `params[0]` into one of the
eight key slots at `0xe43cc` (cleared to 0xff on every level change, `FUN_0003733b`, and on a new
game, `FUN_000410b9`), so keys never carry over. Key id 0 is an ordinary key (BASE's red card is
key 0). Script actions (see `ScriptAction`: a u16 op and 20 argument bytes, in every entity
script and in the `TRIG` chunk: a 64×64 grid of links, then each linked cell's chain of scripts,
the fixed part's u32 at offset 11 linking to the next) act on the door in cell (arg[0], arg[1]):
op 2 toggles it (`FUN_0003789e`), 3 toggles its lock (`FUN_000378e9`), 18 opens it, 19 closes it
(`FUN_000379d6`, `FUN_00037a2e`), 20 locks and 21 unlocks it (`FUN_0003794a`, `FUN_00037990`).
The engine treats a locked door as openable when the player holds its key, or when a script the
engine can't run yet names its cell with op 2, 3, 18 or 21 (a stand-in); scripts it runs open
and unlock doors themselves (section 5k). Otherwise the door is sealed, as in the game (TUTORIAL
has two such doors, MUSEUM2 three). A door whose `DOOR` u16 after the flags has bit 0x08 (runtime
0x40) says nothing when it won't open: its script prints why instead of "ACCESS DENIED.".

**Using doors** (`FUN_0003fed1`, the "use" handler, case 5 of the screen pick): the door must be
within 128 units on each axis ("DOOR OUT OF RANGE."); a locked door (flag `0x08`) without its key
(`FUN_00042bda`, key id at door +0x2d) shows "ACCESS DENIED.". `FUN_000134cb` toggles the door:
closed (state 0) or closing (2) starts opening (3), open (1) or opening starts closing. The player
opens doors only this way; creatures of species 0 push them (`FUN_00047ed3`), and scripts call the
same toggle (`FUN_0003789e`). `FUN_000132a5` runs them every tick: the panel position moves by
the speed per tick; an open door starts closing 200 ticks after it opened unless its runtime flag
`0x20` is set, and a closing door that meets anything in the doorway opens again.
With a readied item, case 5 first hands the item's script the door (`FUN_0004032d(5)`), then runs
the door's own script (bit 0x08), then tries the key, so a door's "you need X" message shows even
as the right item opens it. MUSEUM2's maintenance-pass door at (36, 36) and the wall panel at
(36, 37) beside it are both locked, speed 6 (the level's fastest: the 127-unit lift takes 21
ticks); script 15 (condition 14, the pass used on (36, 36)) opens both with op 18, and scripts 62
and 63 close them behind you at (37, 36) and (37, 37). MUSEUM2's three-panel door at (19–21, 16)
opens only from the middle panel (`door14`, its script opens all three); the side panels are
locked with no script.

**The screen pick and the cursor** (`FUN_00013524` finds the object drawn under the cursor; no
range limit). `FUN_000358a1` colours the cursor by it: palette `0x90` (pink) on a door of any
kind, so hidden doors give themselves away; `0xbc` (green) on an item; `100` (orange) on a
creature that isn't flagged inanimate or dying; `0x9c` otherwise.

**The player's hitscan guns** (`FUN_00030f9e`, weapon record type 1: gauss pistol, chaingun,
plasma accelerator) draw no projectile: `smoke1a` at a wall or prop, `blood1a`–`3a` on a
creature by its blood type, and for the plasma accelerator (+0x5e = 4) `FUN_00044744`'s beam.
The gauss record has no projectile sprite (the alien pistol's, `approj00`, is at its +0x46); the
`gbolt` sprites are fired only by TUT.DAT's creatures. Up to 0.5.2 the engine drew a green
`gbolt` after each gauss shot.

**Projectiles** (`FUN_00031342`, player; `DAT_000ef7a6` the weapon, `DAT_000ef3f6` the heading):
weapons 0–4 start 13 units ahead and 24 below the eye (the alien rifle's two bolts 13 ahead and
10 to either side, heading ± 0x40); the twin laser's two start 13 to either side of the player
(heading ± 0x40, none ahead) and the rocket 13 ahead, both 5 above the eye: the suit's shoulder
guns. The twin bolts get the same heading and speeds, so they fly parallel. The engine starts
them where the game does (up to 0.5.2 it started the suit weapons 13 ahead and 18 below the eye),
and aims each at what is under the crosshair. Each is a physics object with a 10 × 10 box. On hitting a
creature or the player a plain projectile stops and goes off; one with the bounce flags
(`0x10 | 0x20`, the grenades) reverses at a quarter of its speed (`FUN_0004ec41`) and goes on
until its fuse runs out or it comes to rest. The impact handler (0x3354f) gives the direct-hit
roll to whatever the projectile touched last (+0x25), then the blast.

**GLB files** (one per level; `cyclone.glb` is the fallback) use the same chunk format. The game
only reads `GITX`, `GDTX` and `GMTX` (item, door and monster names), plus `BMON` and `MBTX`. The
`GOBJ`, `GDOR`, `GITM` and `GMON` chunks are editor templates, and the game skips them.

**`BMON` monster type** (`FUN_000115e2`):
`u16 id; char sprite[10]; u16 health; u8 stats[8]; u32 flags6[6]; u32 attack[3]; u32 attackTics[3];
s16 accuracy; s16 evasion; u8; u32 hasSpecial; u32 hasSpecialTics; u32 items[3]; u8[3]; u16 mirror;
u8 eye; u8[27]; u32; u32`. `health` is a default only: a placed monster's hit points come from its
`MON` record. `accuracy` (+0x39e) is added to every hitscan and melee roll, `evasion` (+0x3a0) is
subtracted from the player's rolls against it, `eye` is the gun height (0: half the box), and
`stats[5..7]` are the collision box.
`stats[0]` (stored at type + 0x0e) is the walking speed: when a walk frame ends, the animation
player (`FUN_000473b1`, state 3) hands its tic count to `FUN_00047abe`, which moves the creature
`tics × stats[0] / 32` units in one jump (nominally `stats[0] × 3.125` units a second: scientist 20,
Cyclone 60–80, berserker 110, wasp 115; 0 for gunposts, ceiling guns, barrels and the queen). A jump
that would cross a cell centre stops on it (`+0x76` is set so the creature thinks at once) and the
rest of the jump is lost, so the speed actually reached is 3–31 % lower and depends on the walk
tics: a berserker nominally walks 344 units/s but covers 237, the same as a Cyclone 2 (250 → 237). `stats[1]` is never
read after loading. `stats[2]` is a flier's hover height and `stats[4]` the species (0 = creatures
that open doors; it also picks blood and death effects). `stats[3]` (type + 0x12) holds flags: bit 1
makes the creature see-through. `FUN_00047010` copies it to bit 0x02 of the creature's `+0x18`,
and `FUN_0001c694` then draws the creature in mode 2, through `TINT_MAP` as glass is drawn. Only
the camouflaged Cyclone 3 (`cyc300`) of SUBWAY and MAINTEN2 sets it. The engine draws such a
creature at 38 % over what lies behind, front surface only (a depth pre-pass, so overlapping parts
of the body don't add up). Then come up to three embedded `ITEM`
records, then animation sequences.
Each sequence is `u8 count` followed by `count` bytes, ending with 255. The game stores them in
seven slots of 64 bytes (frames at type + 0x1e + slot·0x40, tics at type + 0x1de + slot·0x40).
In file order:

1. slot 0 frames: the **ready** move (unfolds before attacking: the gunball opening, the gunpost
   rising). Played forward before an attack and backward at double speed (`tics / 2 + 1`) after it.
2. slot 0 tics
3. slot 1 frames: **death**
4. slot 1 tics (if `flags6[3]` is set)
5. slot 2 frames: **walk**
6. slot 2 tics (if `flags6[5]` is set)
7. slots 4, 5, 6 frames: attack A, attack B, and attack-A fire flags (each if `attack[i]` is set)
8. slots 4, 5, 6 tics: attack A, attack B, and attack-B fire flags (each if `attackTics[i]` is set)
9. slot 3 frames and tics (if `hasSpecial` / `hasSpecialTics`): the **hit reaction**, usually one
   recoil frame (often the first death frame). The damage code (`FUN_0004848a`, mode 3) plays it
   when a hit doesn't kill, but only when the tick counter is a multiple of 4, so about one hit in
   four.

A sequence holding a lone frame 0 with tic 0 is a placeholder. BASE (`grenc01`, `cyc300`), TUTOR
(the same two) and MOTHER3 (`Alsci00`, `alwrk00`) define creatures that way; MOTHER3 spawns 31
alien scientists with it. The engine borrows the full sequences of the same sprite from another
level's GLB.

Frame numbers are lump offsets from `sprite`. Walk frames step by the rotation count: `0,5,10,15`
for mirrored 5-view sets (`alwrk00..04`), and `0,8,16,24` for full 8-view sets (`cyc201..208`).
Pain, attack and death frames are single views facing the player.

**Animation timing.** The game logic runs on a 100 Hz timer (`FUN_00040d78(100)` programs the
PIT at 1,193,180 / 100 and hooks interrupt 8, which counts `0x74df0`; the main loop in
`FUN_00036b53` steps the world once per tick, at most 20 ticks a frame), and a monster's next frame is due at
`tick + tics[i]` (`FUN_000473b1`). So one tic is 10 ms. The animation player's states, set by
`FUN_0002af83`:

| State | Sequence | Timing |
|---|---|---|
| 3 | walk (slot 2) | `tics[i]`, loops |
| 1 / 7 | ready (slot 0) / hit reaction (slot 3) | `tics[i]` |
| 2 | ready, backward | `tics[i] / 2 + 1` |
| 4 / 5 | attack A / B (slots 4 / 5) | `tics[i]`; a value of 129+ means 5 tics plus a light flash |
| 8 | death (slot 1) | `tics[i]`; a value of 129+ means 5 tics plus a light flash |
| 9 | stun: the first hit-reaction frame | 6 × 5 tics |

A light flash of `v − 128` darkens the cell's fade row by that much (brighter), and by half in the
8 neighbouring cells, while the frame shows. Exploding machines and aliens use it for every death
frame (156–236, i.e. flashes of 28–108), and some attacks for their muzzle flash (the gunballs'
`balla1` at 160: a flash of 32 for 5 ticks). The engine lit only death frames up to 0.5.2; attack
frames light up the same way now. The same gunball art is timed two ways: BASE, MAINTEN2 and HEAVY
define its attack (B) with `balla1` at a plain 9 ticks, BIOSTOR2, CORE and CLONE (attack A) at
160, so there the flash is on screen 5 ticks instead of 9, and the engine's glide between frames
left about half of that at full strength: a blink that read as no flash at all. The engine keeps
the shots on the game's ticks but draws (and lights) a light-coded muzzle flash for 8 ticks,
popping in and out without the glide (`kFlashHold`).

**One creature, many definitions.** Each level's creature types (`BMON`) carry their own
animation sequences, fire flags and tics, and the same sprite turns up with different ones from
level to level (the gunball above; the CyClones, pit bulls and barrels; even within MOTHER1, where
the Alien Elite Flamer and the Alien Gunner share a sprite and walk). The engine builds a
creature's pictures, animations and hit areas once and keeps them across levels. Up to 0.5.2 it
found them again by the sprite and walk frames alone, so in a game played through, 58 creature
types from LAB2 on were drawn and timed with an earlier level's animations, shots included
(BIOSTOR2's gunballs, after BASE's, had no attack A at all: no flash, red-lit ports while firing).
The key now holds every sequence the build reads.

When the death ends (mode 5, `FUN_000481f2`): if the **last death tic is 0**, the last frame stays
as a corpse. Otherwise the creature is removed (`FUN_0002062e`): barrels, gunballs, gunposts and
other machines leave nothing behind.

**Sprite rows count up from the ground.** In a column, `y_start = 0` is the pixel touching the
floor. Every monster frame has `min(y_start) = 0`. Rotation 0 is the front view, and each next
rotation is the viewer 45° further around toward the monster's left. In 5-view sets, views 5–7 are
views 3–1 mirrored.

**Monster AI** (traced in the decompilation). It is simple:

- *Classes.* The placed monster's byte +0x6f picks the AI (`FUN_0002aea7`): 1 chaser
  (`FUN_0002a17b`), 2 "deployer" that unfolds its ready sequence before attacking (`FUN_0002a539`),
  3 proximity mine / exploding object (`FUN_0002a8b2`), 4 stationary turret (`FUN_0002a9c6`),
  6 queen (`FUN_0002ad53`). Script ops 0x10 and 0x24 can change a monster's mode and class.
- *Activity.* Only monsters within (10 + 1) × 64 = 704 units of the player along each axis are
  active (the larger of |dx| and |dz|, so a square; 10 is the view distance at `0x72a90`), recomputed
  when the player changes cell (`0x7525b`, `FUN_00047206`); others neither animate nor think. A
  monster thinks every 75 ticks (`FUN_0002aea7` sets `+0x72` = tick + 75), or sooner when its
  `+0x76` flag is set: an animation ended, a step reached a cell centre, or a step bumped into
  something. A blocked creature therefore walks on the spot and re-thinks after every walk frame
  (about 9 ticks), trying a new axis each time.
- *Noticing.* Purely distance: within the placed sight radius (usually 10 cells) it heads for
  the player, otherwise it wanders in a random 8-way direction (`FUN_0002ae76`). No line of
  sight, field of view or noise is involved; being shot only activates it until the player moves
  to another cell. There is no memory: inside the radius it homes on the player through walls,
  outside it forgets at once.
- *Movement.* `FUN_0002ae02` picks a cardinal direction toward the player's cell (an axis at
  random, with a parity quirk), facing snaps to 8 directions, monsters move cell centre to cell
  centre, step up 17 units at most and re-think when blocked. No pathfinding: a wall in the same
  row or a U-shaped obstacle traps them. Species-0 creatures open unlocked doors they bump into.
  Fliers hover at `stats[2]` and dive to the player's height within 192 units (Chebyshev):
  the eye height minus half of `stats[6]`. The dive is worked out as a walk step ends
  (`FUN_00047abe`), so a creature that never walks never dives: ceiling guns (class 4, speed
  0) hang at the floor + 2 + `stats[2]` they are placed at (LAB2: 300 + 65, under a 427
  ceiling). Up to 0.5.2 the engine dived them to eye height.
- *Fliers' moves.* `FUN_00047abe(creature, tics)` is called as each walk frame ends. It moves the
  creature `tics × stats[0] / 32` along its heading, and a flier (`stats[2]` ≠ 0) `tics` units up
  or down (1 a tick, 100 a second) toward `+0x7a`: the player's height − `stats[6]` / 2 within
  192 (Chebyshev), else `+0x7c`, which it then sets to the surface under it at 17 over its feet
  (`FUN_00012a74`, `FUN_00012b30`) + 2 + `stats[2]` (within a step it snaps there). A walker
  instead falls (`+0x77`) to that surface + 2. `FUN_00047ed3` then puts the creature at the new
  place and height and tests its box (`FUN_0004e0d2`: x ± `stats[5]`/2, y..y + `stats[6]`, z ±
  `stats[7]`/2) with `FUN_00012e13`, which runs `FUN_0002049a` (mode 1) over the collision rows
  at `0xc9060` (64 rows of 0x109 bytes, one list per row of cells, sorted by x) for its row and
  the rows either side; `FUN_00020105` overlaps the box with each object (`FUN_0001fd4c`, an
  axis-aligned box test in x, y and z) and notes what it meets: type 1 walls (0x3e bytes at
  `0xcdb3c`, `0xc905c`), type 2 cells (0x52 bytes at `0xcdb38`, `0xc9038`, the layer in
  `0xc9048`), type 5 doors and type 8 blocks (`0xc903c`), types 3 and 4 (`0xc9050`). Every cell
  with a floor or a ceiling is a type 2 object holding four surfaces, each a thin box (its corner
  heights' range + 1): layer 0 the floor (`MAPF`: texture +0x1a, corner heights +0x22), 1
  `MAP1` (+0x1c, +0x2a), 2 `MAP2` (+0x1e, +0x32), 3 the ceiling (`MAPC`: +0x20, +0x3a); slope
  shapes at +0x42 (`FUN_00014099`/`FUN_000140cd` put the walls and cells into the rows as the
  level loads). For what it met: a surface (its height + 2), a wall's or a block's top (+ 1)
  that is no more than 17 over the creature's new feet, it is set on; a door, or anything
  higher, refuses the move, and the creature goes back to where it was, height included, and
  thinks again (`+0x76`). So a catwalk (`MAP1` underside and `MAP2` deck) is solid to a flier
  from above and below, a flier stops under one it rises to and can't fly in under one at a
  height where it would cut through its box, and the ceiling is solid too. A creature placed
  with its box through a surface stays put for good: SDI2's Flybots 18, 25, 26 and 31 start at
  100 + 100 + 1 = 201 under catwalks at 250/266, their box reaching 260; DOSBox (5i) shows them
  trying a step now and then (a few units' move and y 202 for an instant) and being put back
  at once, for minutes. SDI1's Flybot 18 (record y 300: 401) and Wasps 16 and 17 (451) start
  above its 394 ceiling, out of sight. A flier starts at its record's y + `stats[2]` + 1
  (`FUN_00010d05`; SDI1's Wasps 24–28 read 251 over the 100 floor), and SDI1's Flybots 21–23,
  at 336, touch the 394 ceiling with their 59-unit box but get away by sinking.
  The engine (`ai.cpp`, *Fliers*) moves fliers the same way on its smooth path from cell to cell:
  a step to a neighbouring cell is judged at the height its first walk frame takes it to (the
  type's walk tics toward the height it makes for: 5 for a Flybot, 2 for a Wasp); while it moves
  it climbs or sinks 100 units a second, set on any surface its box meets up to 15 units over its
  feet and stopped under any higher one, over its box's middle and corners and the cell ahead;
  walls between cells are tested over its box's span above 16 units. Its routes follow the floor
  under it rather than its own height. The enhanced AI differs on purpose: a flier whose step
  would cut a catwalk or lintel sinks under it first when there is room (`Monster::duck`), and the
  seven fliers above start just under the catwalk or ceiling. Up to 0.5.2 fliers walked the floors
  like other creatures (judging steps at the floor) and eased their drawn height to the hover or
  dive height every frame, also while standing or attacking, inside a circle of 192, through
  catwalks, floors and ceilings; and they started at the floor under their record + `stats[2]`. The placement is the record's own
  height: `FUN_00010d05` sets the creature's y to the record's y + the type's hover byte + 1,
  not the floor under it, and only a walk step brings a creature down to the floor. So a
  creature that never walks stays at its record's height + hover: BIOLAB's ceiling laser at
  (51, 51), placed at 300 over the 220 floor of the pit room, hangs at 360 from the 419 ceiling
  (the engine had put it at 220 + 60, halfway down), as do MAINTEN2's two gunballs at (11, 4)
  and (9, 5) and HEAVY's at (45, 11), 8 units above the sloping floor's height.
- *Sight* (`FUN_00020f42`, a grid tracer from one point to another, at the heights in between):
  a cell edge's wall (type 1, `FUN_00020587`) blocks when it has a texture, the line crosses it
  between its base and top, and either the caller's last argument is 1 or the wall lacks flag
  `0x02` (the file's flags OR the loader's per-texture table: windows, grates, BASE's fence
  `wall18`). Glass (`0x10` alone) blocks. Floors and ceilings (type 2, `FUN_00012a74` via
  `FUN_00020e79`) block where the line passes through them; a door (type 5, `FUN_00020d47`) only
  while fully shut (+0x18 = 0), within its height. Blocks (crates, desks) are not looked at: they
  never stop sight. Callers and that argument: the attack decision `FUN_00030e0e` 0, hitscan
  `FUN_00032419` 0, blasts `FUN_00032991` and `FUN_00033977` (demo packs) 0, the melee blow
  `FUN_00032295` and `FUN_0003219b` 1. The engine's `lineOfSight` now follows all of it (per-triangle
  flags, `Renderer::sightFlags`: walls flagged 0x02 pass unless the caller is the melee blow,
  blocks always pass, a door stops it only while shut and still); up to 0.5.2 every wall, block
  and door panel in the way stopped it. In BIOLAB a Cyclone at (15, 29) now sees the player at
  (13, 33) over the 100-unit block at (14, 31) and fires at once.
  BASE has 124 such walls (the yard's fence and the south buildings' windows, `wall12`, `wall7a`,
  `wall8a`), HEAVY 6, SUBWAY 4; no other level has any.
- *Attacks.* Line of sight (`FUN_00030e0e`, grid tracer `FUN_00020f42`) is only checked before an
  attack starts. Melee reach is 96 units (square distance), ranged reach the placed band radius.
  Attack frames fire on the flags in slot 6. The creature keeps its heading while it attacks:
  the chaser and the deployer set `+0x1a` to the exact angle when they choose an attack, but
  `FUN_0002aea7` then overwrites it with `(+0x88 + 0x10) & 0xe0`, the axis heading `FUN_0002ae02`
  chose at the start of the same think. Hitscan (`FUN_00032419`) and melee (`FUN_00032295`) need
  `|(s8)(angle to player − +0x1a)| < 64` (exactly side-on fails), then line of sight, then roll
  type accuracy (+0xec432) plus a per-range bonus against `rand % 100`; a failed facing or sight
  check returns 0, a miss returns 1, a hit returns 1 unless the band does no damage. Projectiles
  (`FUN_00032607`) aim at the player's current position (±1 step of jitter on ticks with bit 1
  set), with no leading, and return the line of sight. The result goes to `+0x7e`.
  There is no cooldown. When a chaser's attack animation ends (states 8 and 9 in `FUN_0002a17b`)
  with `+0x7e` set it attacks again at once, without a new sight check: attack A again if A is
  ranged and the player is beyond 96 units (band 3 or 5, so even out of weapon range), B if A is
  ranged and B is melee at close range; if A is melee, A at close range (or B at random when B is
  melee too) and B in band 3 when B is ranged; and the same with A and B swapped after attack B.
  Otherwise, or with `+0x7e` clear, it walks, and thinks again 75 ticks later.
  Measured in the running game (section 5i): the BASE Cyclone's plasma attack repeats exactly
  every 47 ticks (its frames 9 + 9 + 5 + 10 + 5 + 9), and a gunpost alternates bursts with
  75-tick pauses as its random axis heading turns it toward the player or away.
- *None of:* memory, search, pathfinding, noise or group alerts, retreat, infighting (stray shots
  can still hurt other monsters), or any effect of difficulty.

**Difficulty** is the u16 at `0x73c95` (0 easy, 1 normal, 2 hard; a value of 3 would load every
entity). It does exactly three things:

1. **Which entities exist.** `FUN_0001231b` turns it into a mask (1, 2, 4; 7 for 3) and the
   entity header loader `FUN_00010010` skips any monster, item, object or door whose skill byte
   doesn't contain it. Across the 22 levels that gives 894 / 1,061 / 1,243 monsters (BIOLAB
   35 / 56 / 80). Supplies barely change: 169 / 166 / 162 biokits and 158 / 155 / 151 armour
   kits in total, and only ALIEN1 and BASE hold back a few on the harder settings. Ammunition
   barely changes either (189 / 187 / 187 clips).
2. **Easy only: damage you take** is cut by a quarter (`FUN_00032cf2` and
   `FUN_00032f0e`: `dmg -= dmg >> 2`).
3. **Easy only: damage you deal** to monsters: `FUN_0004848a` adds a quarter twice
   (`p += p >> 2; s = p + (p >> 2)`), about 1.56 times.

Normal and hard differ only in monster placement. Monster health, speed, accuracy and
behaviour are the same on every setting.

The player's position is at `0xef3e4` (x, u16), `0xef3e6` (eye height, s16) and `0xef3e8` (z, u16);
health is at `0xef7b4` (s16), armour at `0xef417` (s8) and the suit at `0xef418`.

**Deployers** (class 2 with a ready sequence: gunballs, gunposts, the aliens' workers, gunners and
flamers; `FUN_0002a539`). The attack flags
at monster +0x7f come from the weapon items (monster spawn `FUN_00047010`): 0x02 / 0x01 attack A is
ranged / melee, 0x20 / 0x10 attack B. In range, a ranged A is fired straight away (mode 8, no
unfolding); a ranged B first plays the ready move (mode 6), then fires (mode 9). At the end of an
attack the creature fires again while its last shot had line of sight and the player is still in
the band (modes 8 and 9 repeat, staying open); otherwise it folds up (mode 7: the ready move
backwards at `tics / 2 + 1`) and walks. So BIOSTOR2, CORE and CLONE gunballs, whose attack A
frames are the opened shell, pop open without unfolding; the engine unfolds them first. Their
unfolding (`ballb0`–`ballb6`) ends with the gun ports still shut (red lights); the attack frames
(`balla0`, `balla1`) show them open, then the muzzle flash. The game never waits open (it fires
again at once), but the engine's enhanced AI does between shots: it holds the first attack frame
there (up to 0.5.2 the last unfolding frame, so the gunballs seemed to fire closed). Hit areas
follow the frame drawn: the closed sphere's eight views, each unfolding and attack frame, and the
barrels' single view (checked against the pictures from all sides in BIOSTOR2).
The fold (mode 7) ends in walking (mode 1), and `FUN_0002aea7` sets the next think 75 ticks on;
the AI also runs when a walk step bumps (+0x76). Facing for the walk is a fresh axis heading
(`FUN_0002ae02`; the exact angle the attack branches store at +0x1a, `FUN_00020986`, is
overwritten with +0x88 by `FUN_0002aea7` straight after). In DOSBox (BASE, player at (10, 11),
gunball 2 at (10, 8) in the player's column): ticks 77–100 fire (9), fold (7), 139–214 walk west
from x 10.5 to 7.0 (16 units every 5 ticks), unfold (6), 277–421 fire, fold, walk 20 ticks south to
z 9.5, unfold. Gunballs have a single walk frame and a speed (stats[0] 70–100); up to 0.5.2 the
engine took every creature with one walk frame for a fixed gun and never moved them, in either AI.
Now only creatures with one walk frame and no hover height are fixed (gun posts; LAB2's
`CyClone3`, stats[0] 1, whose walk frame moves less than a unit and is lost to the integer
position). In the original AI a fold is followed by a step along a fresh axis heading
(`World::foldedWalk`), which is thought over again at the next cell centre. In the enhanced AI a
deployer that can move counts its bursts and after 2–4 folds and moves 1–2 cells across the line
of fire (`Monster::bursts`, `relocate`) before it opens up again.
The Alien Worker (`alwrk`, HIVE1–3, MOTHER1) is a deployer with only a melee attack B (weapon 11:
6 frames of 9 ticks, blows on frames 3 and 5; ready 4 × 9 ticks). At the end of mode 9, if the
last blow counted (+0x7e) and the player is within 96 (Chebyshev), mode 9 again at once (no line of
sight or height test); otherwise fold and walk. A blow needs the player within 90° of the facing,
which is the axis heading, so a worker in the player's row or column faces side-on half the time,
misses and folds. MOTHER1's Alien Gunners and Alien Elite Flamers share `algrd` (frames 0–31 walk,
32–35 death `algrdd1`–`4`, 36–42 `algrda10`–`16` the flamer, 43–52 `algrda20`–`29` the arm gun):
the gunner readies with 43, 44 and attacks with 45 46 46 47 48 48 49 50 51 52 at 12 ticks (45 its
arm thrown up, shots on 46 and 48, `aggun` plasma), the flamer readies with 42 and attacks with 36
37 37 38 38 39 39 40 40 41 42 at 8/4 ticks (shots on 37 and 41, `agflm` flames); both pain on 36.
In DOSBox the creature's sprite lump is at +0x1c (frame + 65 for `algrd`), its frame index in the
sequence at +0x68: gunners 6 and 37 and flamers 19 and 20 ran exactly these sequences and timings
(lumps 110–117, 101–107), repeating while their shots counted, the flamers folding (mode 7, lump
107) and walking after two. The engine's enhanced AI holds a deployer open between attacks; it
holds `World::openFrame`: the unfolding's last frame if the attack shows it (the flamer's 42),
else the first frame after the first shot that neither fires nor repeats a firing frame (the
gunner's 47, its arm gun levelled), else the first (gunballs). Up to 0.5.2 it held the first: the
gunner's arm thrown up, the flamer's flame.
The enhanced AI's pause after an attack (`coolTime` × 0.7–1.4: 1.1, 0.65 or
0.35 s by difficulty, counted only while walking) is 0.3 of that after a blow.

**Hit tests in the engine.** Bullets (hitscan) and the crosshair test one long ray from the eye.
Depth sprites (a creature walking in its 5 or 8 views) are marched through their cylinder.
Single-view pictures (barrels, attack, pain, unfolding and death frames) are tested where the ray
crosses an upright plane through the creature's axis. For the eye's ray that plane faces the eye,
as the picture does. A projectile is tested a frame's flight at a time, and up to 0.5.2 its plane
faced each step's start, so it turned as the projectile flew past and no step crossed it unless
the projectile passed within about half a step of the axis. Alien pistol, alien rifle, twin laser
and grenade shots went through barrels and open gunballs except down their middle (BIOSTOR2
barrel: hits within ±4 of 32 units; the twin laser's two bolts could both miss dead centre), and
the faster the frame rate the narrower it got. A projectile's plane now stands square to its
flight, which it crosses once: barrel shots hit across ±16 and miss beyond it, gunball shots
across about ±25.

**Death screen** (`FUN_00036b53`). Dying (`FUN_0004e120`) sets a 325-tick timer. For the first 125
ticks the world runs on, the eye sinks up to 45 units and the screen tears: each frame the tear
amount is `rand() % (326 − timer) & 0xf0`, and screen rows with `row & 15` in {5, 7, 8, 11} are
copied shifted sideways by a quarter of it, rows {6, 13} by half (the same tear `0xe41a6` that hits
add to). Then, with `u = 200 − timer`, the whole 320×200 screen (status bar included) is drawn
scaled into a shrinking box on black, through fade-table row `max(0, 64 − u/2)` (brighter): for
u < 100 the box is `(320 − u) × (200 − 2u)` around the centre, so it collapses to a line; for
u < 155 a one-pixel line `220 − 4(u − 100)` wide on row 99; then a single pixel at (159, 99) until
the timer runs out. Then the game leaves the level for its options screen (`FUN_000491d0`, below)
with RETURN and SAVE dimmed and the death report (`failtxt`) in the grid; the engine adds "RESTART LEVEL?"
YES / NO under the report as a shortcut (the game had LOAD and DIFFICULTY's "RESTART GAME?").

**Hit tear** (the same code): a hit adds the damage taken after armour to `0xe41a6`
(`FUN_00032cf2`, `FUN_00032f0e`), which also kicks the player's speeds. While it is non-zero the
3D view is copied to the screen row by row with rows {5, 7, 8, 11} of every 16 shifted by a
quarter of it, {6, 13} by half, rows {1, 2, 3, 9, 10, 15} as they are, and rows {0, 4, 12, 14}
not copied at all, so they keep the picture from before. After each frame the tear drops by the
ticks that passed and is held to 50.

**The view.** The game projects with one focal length for both axes, half the view's width
(for example `FUN_0001bb15`: `x = 160 + X·160/Z`, `y = 100 − Y·160/Z`), so the full 320×200 view spans 90°
across (the per-column angle tables `pang0`–`pang5`, 8,192 units to the turn, run from +1,020 to
−1,023) and 64° down. Shown on a 4:3 screen, its pixels and everything drawn with them are 1.2
times as tall as wide. The engine draws the same view (90° across 4:3, tall pixels) unless the
player changes it; up to 4.1 it used 75° with square pixels, about 1.3 times magnified.

**The status display** (320×200 coordinates): `statsicon` (29 × 42) at (287, 138): armour bars
in columns 2–13, health in 15–26, 13 segments of 2 rows every 3 rows from row 2. From the top,
`(max − armour) × 13 / max` armour segments and `(104 − health) / 8` health segments are painted
palette 4 (`FUN_00040523`, `FUN_000405e9`). `gunicon` (29 × 16) beneath it holds the ammunition
in the red seven-segment digits, 8 pixels apart. The bearing (`FUN_0004084e`) is printed in the
green font at (95, 184): a compass point from `" E NE N NW W SW S SE"` indexed by
`((angle + 16) & 255) >> 5` (angle 0 = east, counter-clockwise), and below it at (95, 192)
`"%3d_"` of `(255 − angle) × 141 / 100 + 90` (less 360 above 360; `_` is the degree sign). The
altimeters (`FUN_0003f2b9`, `FUN_0003f449`) are ticks every 2 rows from 26 to 122 at x = 3 and
x = 316, 8, 2, 4 and 2 pixels long in turn, fading inward from palette 20 by 2, 8, 4 per pixel;
the current one starts at palette 126 (red). Both show the same thing: the eye's height
(`0xef3e6`), clamped (0x3ef4d) between the level's low and high marks `0x74b52` and `0x74b50`,
at row `122 − (eye − low) / (high − low) × 96`, truncated (`FUN_00053884` rounds toward zero)
and made even. The marks are set at the level's start (`FUN_0003680a`, 0x36a30) from the surface
record of cell (0, 0): its floor height when it has a floor (else 0) and its ceiling height when
it has a ceiling (else 512). The designers used that corner cell for it: its floor and ceiling
bound every surface in the level (BIOLAB 0–700, ALIEN1 100–528, HIVE1 and HIVE3 10–500, most
levels 1–900; HEAVY, HIVE2 and TUTORIAL have none, so 0–512). So the red tick rises as you jump,
climb or ride a lift, and falls into pits. Up to 0.5.2 the engine took them for look scales and
moved the tick with the view's pitch.

**The options screen** (`FUN_000491d0` draws it; `FUN_0003b7fa` and friends write its text), in
320×200 coordinates and the `options_pal` palette:
- Background: `options` (raw 320×200), the framed monitor with the green grid. The arrow buttons
  at its foot (`hoptdown`/`hoptup` lit, at 117, 179 and 157, 179) light when a list scrolls.
- Buttons: `optbut1`–`optbut9` (72×15: RETURN, TUTORIAL, LOAD, SAVE, DIFFICULTY, QUIT,
  SETTINGS, CONFIGURE, NEW GAME) at (39, 29 + 16n); the one under the cursor is drawn from
  `hoptbut1`–`9`. With no game running (`0xef7b6` = 2: at start and after death) RETURN and SAVE
  are `disreturn`/`dissave`, dimmed by
  a stipple: about every other pixel near-black (palette 51–53), the rest a shade darker. LOAD is
  never dimmed. Every question starts with NO lit (`0xeeea0` = 1, `FUN_0004a6a0`; SETTINGS
  aside).
- With no button open: `option0` (80×63, the logo) at (159, 67), then a text lump written over it
  at (128, 30), a line every 8 pixels (`FUN_0003cac4`): `finaltxt` at start (`0x73c99` set),
  `failtxt` after death, otherwise `level<n>` for the mission number n. Text lumps are lines ending in NUL,
  stopped by the bytes 01 01 02 03 05 08 0D. Missions are numbered by the level table at 0x73d1e
  (0x80 bytes per entry): base, lab2, alien1, biolab, biostor2, failsaf2, museum2, mainten2,
  treat2, subway, sdilab, sdi1, sdi2, hive1–5, mother1–3, core, clone, heavy (1–24; treat2,
  hive4, hive5 and mother2 have placeholder briefings and no maps on the disc).
- Fonts (`0x74a20` selects): 1 = `c` (green, the default), 3 = `ag` (amber, also `FUN_0003c8eb`),
  5 = `gmred` (red), and `ar` (grey-green, `FUN_0003c986`). In `ag` and `ar`, '.' is a degree
  sign; `c` has its own 1-pixel shadow (palette 2) in its 8×7 cells.
- Questions (red) at y 145 with YES (`optyes`, 37×15) at (156, 158) and NO (`optno`) at
  (205, 158), the highlighted one from `hoptyes`/`hoptno`: QUIT "ARE YOU SURE?" at x 168,
  NEW GAME "START NEW GAME?" at 162.
- DIFFICULTY (`FUN_0004ae47`): "SELECT A DIFFICULTY LEVEL" (red) at (142, 46); " EASY",
  " MEDIUM", " HARD" at (180, 73 + 21n), the chosen one in green with '>' for its first
  character, the others amber; then "RESTART LEVEL?" (or "RESTART GAME?" after death) at
  (167, 145) and YES / NO.
- CONFIGURE (`FUN_0004ac5f`): the title (red) at (130, 36), "Select a function to reassign" or
  "Press a key for this function"; one row per function every 7 pixels from y 46: the name (12
  characters from `KEYMAP.BIN`) at x 115, its two keys at 183 and 238; the chosen row green with
  '>', the others amber and grey-green.
- SETTINGS (`FUN_0004a998`): sound and music volume (amber label at 117, green value at 122 below
  it, `optbar` 138×12 with the `optslide` knob at 167), a `switchon`/`switchoff` lever, the screen
  size picture `resize1`–`6` with `minusreg`/`plusreg` buttons, and `joystick` (calibration). The
  engine's SETTINGS shows its own options instead, in the same colours.
- LOAD (`FUN_0004b393`) and SAVE (`FUN_0004afb4`): six slots (`0xeee90`, 0–5, `0xeee94` = 6), one
  on show, turned by the arrows under the screen (`hoptdown` at 117, 179: the next slot;
  `hoptup` at 157, 179: the previous) or the arrow keys; the slot number (a digit, `ag`) at
  (280, 30); the slot's picture (144×90 palette indices) at (131, 25); in `ag` at (131, 120)
  `"LEVEL %d"` (`"Special Op 1-3"` for missions 22–24) + `" DIFFICULTY: EASY|MEDIUM|HARD"` and
  at (131, 127) `ctime()` of the save time; the description in green at (131, 134) (`"EMPTY SLOT"`
  when there is no file, with blank lines and a black picture). Then in red at (163, 147)
  "LOAD THIS GAME?", or at (153, 147) "SAVE TO THIS SLOT?" / "OVERWRITE THIS GAME?", and YES /
  NO. LOAD on an empty slot does nothing; otherwise "LOADING GAME....." (red, 163, 97) and the
  game resumes. SAVE: YES renders the view into the picture box and takes it as the picture,
  shows the new level and date lines, "ENTER GAME DESCRIPTION ABOVE" (red, 131, 165) and a text
  entry at (131, 134) (`FUN_0003cbd5`: starts empty, `$` is the cursor, characters 0x20–0x5f
  upper-cased, Backspace, Enter or 30 characters end it), then "SAVING GAME....." at (153, 147),
  writes the slot and returns to the options screen.
- The cursor is `autocursor` (19×19, a green crosshair).
- The engine stamps its title, "CYCLONES: REMASTERED", into the frame's top bezel (centred, letters
  at y 14–18 in the light band 12–20), and "VERSION 0.5.2" and the author's name into the lower
  bezel either side of the arrows (centred on x 68 and 248, y 182, in the band 180–188): the `c`
  font's letters (5 rows tall) without their shadow in `options_pal` grey 6, over the same letters
  in grey 29 one pixel down and right, so they read as cut into the metal. (The game's own
  chrome "CYCLONES" logo is in `waitpic`, a raw 320×200 picture with the 1994 copyright line.)

**The enhanced AI** (the engine's option, not the game's) keeps the 1994 movement, animations and
weapons and adds: sight with a view cone that becomes all-round once alerted, hearing (gunfire
reaches 12 cells, explosions 16), alerting by being shot, a memory of where the player was last
seen or heard with a 10–12 second search that lasts as long as the creature is still closing in,
A* pathfinding over per-height walk maps (stacked floors such as catwalks are separate nodes;
props and creatures that never move are obstacles; species 0 path through doors and open them),
a route to the nearest reachable spot when the player can't be reached, sidestepping for ranged
attackers, group alerts within 8 cells, reaction time and attack cooldown by difficulty
(0.8 / 0.5 / 0.3 s and 1.1 / 0.65 / 0.35 s), accuracy −15 / 0 / +10 by difficulty, and leading
moving targets with projectiles (none / 50 / 80 % of the player's smoothed velocity times the
flight time, at most 96 units, and only toward a point the shooter can see). The view cone is
all round within five cells, and within their sight range for creatures that never move.

**Noticing the player** (an engine addition to both AIs, from 0.5.2): the game starts every
creature facing east (`FUN_00047010` sets +0x1a to 0; the engine faces each a random way) and turns
it only when it thinks, every 75 ticks, so a creature could be caught facing away for up to 0.75 s
as the player came in. With the original AI a creature now thinks at once the first time it has
the player in line of sight within its sight range (it forgets once the player is out of range);
with the enhanced AI the view cone above lets it see the player coming.

## 5e. Sound and music (confirmed from the code)

**Playing a sound.** `FUN_0004fa92(id, a, b, c, mode)` plays `.\sound\<id>.raw` (ids 1–255;
`FUN_0004f86d` keeps the last six files in memory and starts them with the HMI SOS driver).
The driver runs at 11,025 Hz (`FUN_0004f457`: rate 0x2b11, buffer 0x1000) and every sample
starts with no pitch change, so the files play at 11,025 Hz. The master volume is the SETTINGS
SOUND value (0–100, `0x75263`); the final HMI volume is `master × v × 0x7fff / 255 / 100`.
- Mode 1 (the player's own sounds, menus, scripts): `v = a` (the game passes 200 for nearly
  everything, 128 for the death sound) and the pan comes from `b` (−127..127; 0 is the middle).
- Mode 0 (sounds at a place: `a`, `c` = x, z): the offset from the player is turned into the
  view (sin/cos of the heading); with `f` the forward part and `s` the sideways part,
  `d = |f| + |s| + min(|f|, |s|) / 2`, `v = 255 − min(255, d × 255 / 640)` (640 is `0x72a90`
  = 10, shifted left by 6), pan `= (s × 127 / d + 127) × 0xffff / 254`. So a sound fades out 640
  units (10 cells) straight ahead, about 360 on a diagonal, and one directly to the side is all
  in one ear. Volume and pan are set when the sound starts. The engine fades the sounds of
  gunballs and turrets (machines, species 1, that hover or never move: gunballs, flybots,
  gunposts, ceiling guns and lasers, chain guns) over 1,280 units instead (an addition: they fire
  from far down corridors in plain sight); everything else uses 640.

**What plays what.**

| Event | Sound | Where in the code |
|---|---|---|
| Player fires weapon 0–7 | weapon table +0x43: 26, 100, 59, 61, 101, 77, 78, 151 (gauss pistol … plasma accelerator), mode 1 at 200 | `FUN_0003308a` (table built by `FUN_000509ee`, 0x6e bytes per weapon at 0xef429) |
| Fire with no ammunition | +0x45: 103 | `FUN_0003308a` |
| Weapon readied (switch), weapon picked up | +0x42: 104 | `FUN_00052296`, `FUN_000420c0` |
| Ammunition picked up | +0x44: 102 | `FUN_000420c0` |
| Any other pickup (kits, keys, quest items) | 81 | `FUN_000420c0` |
| Glove swing | Sabre Glove 139, Razor Saw 108 (once per pull; it then runs on), Nanotechnology Hand 142 | `FUN_0003308a` |
| Door opens or closes (use, a creature, a script) | the door's own: `DOOR` record u16 at file +0x27 (low byte; door +0x20), e.g. 24, 62–69, 87–91; closing by itself is silent | `FUN_000134cb` |
| Creature hurt (also barrels) | placement `m20` u32 at 4 (instance +0x2c) | `FUN_0004848a` |
| Creature melee blow that lands | `m20` u32 at 8 (+0x30) | `FUN_00032295` |
| Creature fires (hitscan: before any range or sight test; projectile: as it leaves) | `m20` u32 at 12 (+0x34) | `FUN_00032419`, `FUN_00032607` |
| Creature killed; barrels and gas again when they go off | `m20` u32 at 16 (+0x38) | `FUN_0004848a`, `FUN_0002a8b2` |
| Creature hit by nanotech (hurt or killed: it is stunned to death) | 98 instead of the above | `FUN_0004848a` |
| Creature blown apart (explosive kill) | 33 (flesh), or 46 then 47 (machines), before its death sound | `FUN_000481f2` |
| Projectile impact or grenade fuse | by damage type (u16 table 0x755ec): explosive 47, nanotech 150, others none | impact handler 0x3354f (player and creature shots alike) |
| Player dies | 125, mode 1 at 128 | `FUN_0004e120` |
| Jet pack (every 16 ticks while it burns) | 84 | `FUN_0004d5e0` |
| Demo pack goes off | 46 at the pack | `FUN_00042a96` |
| Level script op 0x1c | `arg[0]` at volume `arg[1]`, pan `arg[2]`, mode 1 | `FUN_0004fa54` |

The `m20` field of each `MON` placement is five u32s: start mode, then the four sounds above
(the game plays ids 0–255 only). Typical sets (hurt, blow, fire, death): Cyclones (15, 0, 99,
46), gunballs (5, 3, 59, 35), barrels (0, 0, 0, 46 or 47).

**Music.** The CD audio tracks, one per mission, from the byte table at 0x74a06 indexed by
the mission number (`0xd4290`; 1 BASE … 24 HEAVY, names at 0x73c9e + 0x80 n): BASE 3, LAB2 26,
ALIEN1 4, BIOLAB 5, BIOSTOR2 6, FAILSAF2 7, MUSEUM2 8, MAINTEN2 9, TREAT2 3, SUBWAY 10, SDILAB
11, SDI1 12, SDI2 13, HIVE1 14, HIVE2 15, HIVE3 16, HIVE4 3, HIVE5 3, MOTHER1 17, MOTHER2 3,
MOTHER3 18, CORE 27, CLONE 28, HEAVY 3. Mission 0 (the tutorial: `TUT.DAT` was recorded on it)
has track 0, which `FUN_0004c93b` refuses, so the tutorial is silent. The track numbers are the
disc's own (the table of contents, read by `FUN_0004c5e0` through MSCDEX). A
level load plays its track from the start; when the track's length has passed (the game counts
it in frames drawn, 50 a second) the main loop plays it again. Reading the CD stops the music
and starts the track again afterwards (`FUN_00030b54` and its kin), as does joystick
calibration. Tracks 2, 19–25, 29 and 30 are not used by the levels (title, cut-scene and
stinger music). The CD volume is the SETTINGS MUSIC value (`FUN_0004cef6`, × 255 / 100).

**The SETTINGS screen** (`FUN_0004a998`): the JOYSTICK button at (176, 25); " MUSIC" (amber, green
with '>' when chosen) at (117, 49) with its value in green at (122, 56) and `optbar` at
(167, 56) with the `optslide` knob at (167 + value, 56); " SOUND" at (117, 77), (122, 84),
bar at (167, 84); " FLAT DETAIL" at (136, 103) with the `switchon`/`switchoff` lever at
(147, 112) and "ON"/"OFF" (grey-green) at (180, 112)/(180, 151); " RESIZE SCREEN" at (219, 103)
with `resize1`–`6` at (226, 112) and `minusreg`/`plusreg` at (226, 162)/(266, 162). Image lumps
carry an anchor after their size (u16 w, h, x, y; `optbar` 17, 6, `optslide` 10, 11) that the
blitter (`FUN_00038326`) subtracts.

## 5f. Cut-scenes (CYCLONES.EXE and SMKPLAY.EXE)

(The engine's Enhanced pictures and speech, and its EXTRAS player: section 5p.)

**The front end** (`CYCLONES.EXE`, 16-bit; DGROUP at paragraph 0x362): sets `DOS4G=QUIET`, runs
`SMKPLAY.EXE 1975338 I`, sets mode 13h and the `pic.bol` palette, runs `SMKPLAY.EXE 1975338 o`,
then `CYCLONE.EXE 8335791` (or with the player's own arguments). While the game's exit code is
between 1 and 49 it runs `SMKPLAY.EXE 1975338 <code>`, after code 11 also `36a` and `36b`, and
unless the code was 22 (the end) runs `CYCLONE.EXE 8335791 -restart` again; code 22 ends the
loop and returns to text mode. So the game itself has quit, its CD music and sounds stopped,
before any video between missions plays.

**Videos between missions** (the exit action 24 to another mission, its video number at
argument 8; action 25 the end): BASE 30, LAB2 31, ALIEN1 4 (then 32, 33), BIOSTOR2 34, FAILSAF2
7, SUBWAY 11 (then 36a, 36b), SDILAB 37, HIVE3 39, CORE 35, CLONE 38, MOTHER3 22 and 40 (the end);
BIOLAB, MUSEUM2, MAINTEN2, SDI1, SDI2, HIVE1, HIVE2 and MOTHER1 go on without one. Only 4, 7, 11
and 22 start CD music of their own (op 4, at or right after the first frame); the rest have
none. The engine plays the videos in the game's window, so up to 0.5.2 the last level's music
went on under them (for the whole 75 s of video 30); it stops the music and sounds as the videos
start now, and the next level's music begins when they end.

**The playlists** (`SMKPLAY.EXE`, main at 0x10cd6; the second argument must be `1975338`):
`I` = raven, int1, int2, int3; `o` = 1a2a, 1a2b, 1a3; `4` = 4, 32, 33; `7`; `11`; `22` = 22, 40;
anything else plays the video of that name. Each video is `GAME\<name>.SMK` with its script
`<name>.SSD`. **Any key** (the keyboard interrupt handler at 0x101cf takes any scancode but the
0xE0/0xE1 prefixes) ends the video and **the rest of its playlist**; the CD stops at the end of
every video, and the speech when the player quits. (The engine counts only the ordinary keys,
scancodes A to the ISO backslash without Alt or Windows held, and mouse clicks, so volume and
media keys and system shortcuts don't skip.)

**SSD scripts** (`FUN_00010608`): 12 bytes of header (unused, often stale bytes), then 5-byte
records, op (u8) and arg (i32). The script runs once before the first frame and then after each
frame whose number (1 = the first) has reached the last op 6's frame; it runs on until the next
op 6 or op 10.

| Op | Meaning |
|---|---|
| 0 | Play `GAME\SOUND\<b0>.RAW` (b0 = arg & 255; 8-bit 11,025 Hz) at volume b1, pan −(int8)b2: the briefing speech (225, 226, 202, 203, 204, 210–217, 221, 223, 230, 231, 250 at 190) |
| 1 | Hold the picture for arg frame times (a key ends it) |
| 2 / 3 | Fade the palette to black / up from black over arg vertical retraces (70 a second); while faded, new frames' palettes stay dark |
| 4 | CD audio: track arg, played once; 200 = track 2 from 1:13 for 35 s, 201 = track 2 from 1:48 for 48 s (`FUN_00011c37`, HSG start + seconds × 75) |
| 5 | Stop the CD |
| 6 | Wait for frame arg |
| 7 | Frame rate: (500 / arg) × 2 ms a frame (the timer counts milliseconds); most use 10, INT2 6 then 10, 4 10 then 17 |
| 10 | End of the script |
| 11 | Stores arg (unused) |

**Frame timing** (main loop at 0x11188): the 500 Hz timer (`DAT_000309cd`, 2 ms a tick) is
zeroed before each frame's wait and again after it; the frame is decoded and shown, the script
run, and the ticks since then are the frame's cost. The next wait is the frame time less that
cost; a cost over a whole frame (a hold or fade in the script) is carried as a debt and skips
the next wait, so frames keep to the clock on average. The speech is started by op 0 and then
plays on its own at 11,025 Hz through the HMI driver (a 4,096-byte DMA buffer, `0x30cca`), so
the game never checks one against the other. In the speech videos (1A2A … 40: one op 0 on the
first frame, then 10 frames a second) the voice runs as long as the picture to within a second
or so (34's ends at 63 s of 64; the click at 67 s is the file's end). The engine keeps the same
timing, but holds the picture to the speech as heard (`Audio::heardTime`: the mix's position
less what is still queued for the device), so a stall in its loop can't leave the lips behind
for the rest of the video (up to 0.5.2 each stall over 0.1 s did, for good).

The music: RAVEN track 29; INT1 track 2; INT2 and INT3 the two stretches of track 2; 4 tracks 19
and 20; 7 tracks 23 and 24; 11 track 21; 14 track 30; 22 track 25; the briefings have speech
instead.

## 5g. The weapon in hand (FUN_0005185c, FUN_00051ce6, FUN_000520aa)

The art (raw images with an anchor, `CYCLONE.HD` items section): `gaussp`, `chaing` (and
`chainf`, the barrels turned), `alpstl` (each 00–24: 3 raises × 5 turns), `dbgn01`…`29` (odd;
the even set is unused), `lnch01`–`15`; the suit-mounted weapons show `joyanim1`–`3`,
`joymidcl`, `joyfwd`/`cl`, `joyrgt`/`cl`, `joylft`/`cl` (a hand on a control stick); the gloves
`glove10`–`12`, `glove20`–`25`, `glove30`–`32`. All are drawn full bright (no shading), scaled
with the view (x × width / 320, y × height / 200).

Placement: the weapon record holds x shift `xs` (+0x3c), y shifts `y1`, `y2` (+0x3d, +0x3e) and
rest height `r` (+0x3f). With the reticle at (rx, ry) (it starts at 160, 100 and moves with the
mouse): `a = 200 − r`, `b = 200 − (r >> y2)`; `y = ry > a ? b + ((ry − a) >> y2) : b −
((a − ry) >> y1)`; for rx ≥ 160 `x = 160 + ((rx − 160) >> xs)`, drawn mirrored, else
`x = 160 − ((160 − rx) >> xs)`; the pose is `table[(y − ry) >> 3][|rx − x| >> 3]` (25 × 20
bytes at 0x755f8). Gauss pistol: xs 1, y1 3, y2 1, r 50; chaingun 3, 2, 2, 20; alien pistol 1,
3, 1, 64; alien rifle 1, 3, 2, 30; grenade launcher 1, 3, 2, 7; suit weapons 2, 3, 1, 0 (the
hand 30 lines lower, never mirrored; its pose by the mouse's movement that frame: right > 3 →
`joyfwd`, left → `joyrgt`, up → `joylft`, otherwise the three idle frames in turn; the `cl`
frames while fire is held). Recoil: each shot adds the record's +0x40 (18, 0, 25, 35, 50) to a
speed; each drawn frame the gun drops by speed × ticks / 32 (50 Hz ticks), clamped 0–32, and
the speed falls by 10; the drop also pulls x toward 160 by (160 − x) × drop / 64. On death the
gun sinks by the 50 Hz ticks.

Muzzle flashes (`FUN_00051ffd`, 8 slots, at the gun's anchor when it fired, drawn under the
gun): frames, mode, tics per frame, see-through after the first, from the record's +0x35..+0x3a:
gauss `gflash10`–`12` (3, step 1, 3 tics), chaingun `cflash10`/`20`/`11` (2 frames, step 2: the
first frame `cflash20` from the side when the pose is turned), alien pistol `apflash0`–`2`,
alien rifle `dgflash0`–`3`, grenade launcher `grflash0`–`3` (smoke). Gloves: at (160 + dx,
199 + dy), steps of 5 ticks; Sabre (frame, dx, dy) 0: (0, 0, 21), 1: (0, −30, 35), 2: (1, 0, 5),
3: (2, 0, 5), 4: (2, 60, 65); Nanotech Hand 0: (0, 0, 4), 1: (0, −30, 64), 2: (1, 0, 34),
3: (2, 0, 134), 4: (2, 60, 164); Razor Saw: frames 0/1 alternating (4/5 while cutting), dy 40.

## 5c. Reverse-engineering setup

- `tools/le2elf.py` converts `CYCLONE.EXE` (the bound DOS/4GW LE executable)
  into a relocated ELF32. It applies 15,322 internal fixups and loads object 1
  (code at 0x10000) and object 2 (data at 0x70000).
- Ghidra 11.4.2 analyzes it headless with a Watcom calling convention patched
  in as the x86 "gcc" default. Arguments go in EAX, EDX, EBX, ECX, the result
  in EAX, and every other register is preserved. See `re/README.md`.
- `re/PreWatcom.java` marks `__CHK` (0x5355b, the Watcom stack probe) as
  returning. Without it, 179 functions decompile truncated.
- `re/DecompAll.java` writes C for all 1,050 functions, plus a string
  cross-reference table.

## 5h. Saved games

**The game's** `SAVE<n>.DAT` (`FUN_00045592` writes, `FUN_00045c0c` reads; in the current
directory; slots 0–5 are the player's, 6 holds the mission start for `-restart`, 9 writes
`TUT.DAT`): `u32` time, `char[30]` description, `u16` mission number (`0xd4292`), `u16`
difficulty (`0x73c95`), 12,960 bytes of picture (144×90), then raw copies of the game's memory:
the flags at `0x74ab5`, the movers (`0xdebb4`, 50 × 0x27), the level's cells and scripts, the
player block (`0xe4176`), the clocks, the physics objects (`0xef7c4`, 200 × 0x29), the monster
types (`0xec094`, 0x3ac each) and monsters (`0xe95c4`, 0x89 each), the items, the explored map
and more, ending with a copy of the level file's tail. `FUN_0004691d` and `FUN_000469e0` read
only the header for the LOAD and SAVE screens.

**The engine's** `saves\save<n>.sav` (beside the program; the game's files are memory dumps it
can't use): `char[16]` "CYCLONES SAVE 1", `s64` time (Unix seconds), `char[32]` description,
`char[16]` level name, `u8` difficulty, `u8` enhanced AI, 2 spare bytes, the picture (144×90
RGB), `f32` view yaw and pitch, then `u32` size and the world's state (`World::saveState`,
`game/save.cpp`): clocks, a seed for the random numbers, the player (place, speeds, health, armour, suit,
weapons, ammunition, kits, keys), the explored map and the cell occupancy, every creature by its
place in the level's list (position, step, state and its timer, hit points, armour, AI fields,
route), the pickups (taken or not, then those creatures dropped), props and doors, the level's
scripts and what they changed, and the inventory (section 5k lists it all).
Loading reloads the level from the disc at the saved difficulty and puts the state over it; the
counts of creatures, props and doors must match. Shots in flight, explosions, sparks and lights
are not kept.

Additions: a seventh slot, Q (`saves\quick.sav`), written by F5 (description "QUICK SAVE", only
when SAVE is allowed) and loaded by F9 from play, the death screen or the options screen. LOAD and
SAVE open on the slot last saved to or loaded from (`last_slot` in `settings.ini`); without one,
LOAD on the newest save file and SAVE on the first empty slot of 0–5. Testing: `--inv
"frame:qs"` / `"frame:ql"` quick-save and quick-load in captures.

**The start** (engine): after the opening playlists, or at once without them, the options screen
comes up as the game's does before play (LOAD lit if a save exists, otherwise NEW GAME); not when
the command line names a level or a save. `CYCLONES_STARTMENU=1` does it in captures too.
When the window loses the focus outside the videos, the options screen comes up and the sound is
muted until it comes back; the EXTRAS player pauses. The window's size and place are kept in
`settings.ini` (`window=w h x y`) and used if the place is still on a display.

## 5i. Checking against the running game

The game runs in DOSBox Staging, started straight into a level (`CYCLONE 8335791 -DM base 29 22
188`: difficulty, map, cell x, cell z, floor height; the player faces west). Its memory can be
read from outside through `/proc/<pid>/mem`: the data object sits contiguously in DOSBox's
emulated RAM, so searching for its bytes (for example the strings at `0x722a0`, "i_gaussp",
which are at file offset address − 0x1a000 in `cyclone.elf`) gives the host address of data
address 0, and every variable above can then be read (and written: setting `0xeeea4` = 2,
`0xeeec4` = 1, `0xeee98` = 1 opens the LOAD screen). Polling every 2 ms gives each creature's
state, frame, next-frame tick, think tick and position against the game's tick counter
(`0x74ab0`). Note that a slow host makes DOSBox's emulated clock run slow (in the cloud test
machine it ran at 43 % of real time: 43 timer ticks a second though the game programs
1,193,180 / 100), so timings must be read in ticks, not from screenshots or wall-clock video.

Findings checked this way: the tick is 100 Hz (`0xe43c0` holds the divisor 11,931; the ISR at
0x40a8f counts `0x74df0` and `0x74de8`; the main loop runs `FUN_0003cdf2` once per tick, at most 20
a frame, and fire input is polled every 8 ticks); a Cyclone walks 22–23 units per 9-tick walk
frame, its step cut short at each cell centre (2.5 units a tick, about 237 a second as the engine
has it); attacks repeat as the AI above says; player and monster projectiles share one rule
(`FUN_0004e501`: speed components capped at 10 × 8 and moved by a 1/8 each tick, so the alien
pistol's 72 is 900 units a second and the Cyclone's 75 is 938).

## 5j. The keepsake in BASE (an addition, not in the 1994 game)

`src/game/keepsake.cpp` hangs a framed photograph on the south wall of BASE's wooden house
(z 2624, facing north, centred on x 2400, between the shelf pairs at x 2240–2303 and
2496–2559), dedicated "FOR MY SON, NICOLAS." by the port's author. It is a voxel model (the
renderer's cube instances, 1 voxel = 1 unit = 1 wall texel), built once when BASE first loads:

- a frame 96 × 60 (y 363–423, under the ceiling at 426): dark wood (palette 69–76) with a gilt
  liner (81/86), 2–3 voxels deep, mitred, lit on its top and left sides;
- the photograph inside it, 88 × 52 palette indices compiled into the engine (made for the port
  from the author's own photo; no game data), flat against the wall;
- a brass plate (78/86/88) on the wall's rail (y 354–361), its lettering the game's own small
  font (`c32`…`c95`, read from the player's CYCLONE.HD), a screw at each end.

It is lit like the wall it hangs on (the wall's MAPN light byte, plus 4), not by the cell's
floor light, which is far darker there (90 against the wall's 0). It has no collision and no
part in saved games.

## 5k. Level scripts (FUN_00039f07 loads them, FUN_0003a8e3 … FUN_0003ab56 run them)

**Where they are.** Up to 150 scripts of 0x2f bytes at `0xdc935`: the ones attached to entities
(a door, item, creature or prop record whose header has a script; the script keeps its owner's
index at +0x13, and a creature's at `+0x24` of the monster) and the trigger cells. `TRIG` is a
64 × 64 grid of u32 (`0xd4298`, index z × 64 + x) followed by each non-zero cell's chain of
scripts, **the cells taken column by column** (`FUN_0003a1fa`: x outer, z inner; read row by row,
the chains land on the wrong cells). In BASE: the switch (2, 4), the phase-shift pads (8, 45) and
(27, 1), the switch (21, 39), the secret wall (26, 1), the mission's end (34, 25), the gate (46, 8).

**A script** (file order): `u16 flags` (0x02: the actions repeat for ever; 0x08: it can run again
— and a creature's death sets it; runtime 0x01 running, 0x04 finished), `u32 timer`, `u32 owner`,
`u8 kind`, `u32 next` (the cell's chain), `u32`, `u8 params[16]` (+0x20: values conditions
compare), then the conditions (`u16 op, u8 arg[20], u32 next`, records of 0x1a at `0xdaae8`) and
the actions (`u16 op, u8 arg[20], u32 next, u32 prev`, 0x1e at `0xd8298`).

**When they are tried** (`FUN_0003a856`: not running nor finished, and every condition holds;
each condition check clears the script's event byte +2): the player's cell's scripts every tick
(`FUN_0004d327`), cell (0, 0)'s every 8 ticks, a cell's when the player uses one of its walls
(the use handler sets 0x20 for a north edge, 0x40 for a west edge, on the first script of the
cell; the wall's cell corner within 128 units on each axis), a door's when it is used (event
0x08, before the key test), an item's when it is taken (0x04), a prop's or creature's when it is
used (0x01), a creature's when it dies (flags |= 0x08). Creatures walking into a cell also try
its scripts (FUN_0002a17b and the others), which matters only to conditions not about the player.

**Running** (`FUN_0003aff5`, every 8 ticks with 64): a script that starts is queued (sorted by
timer) at timer 0; every 8 ticks 64 come off each queued timer and those below 1 run actions
until one changes the timer (action 1 adds its u16) or the list ends; past the end a flag-0x02
script starts over, others are finished (dropped from their cell) unless flag 0x08 is set.

**Conditions** (`FUN_0003a316`): 1 the player stands in cell (a0, a1); 4 / 5 / 6 the cell's north
wall / west wall / floor was used; 7 the entity was used or taken (or 0x01 from its own creature);
9 taken; 10 the creature was used; 8 a0 = params u32 (a prop's script: the slot of the prop used);
23 flag 0x08 (the creature died); 28 counter a0 (1–10) = a1, 29 counter a0 (0-based) = a1; 30 the
player's cell's area (the MAP chunk's third layer, `0x802d4` + 3 × cell) = a0; 31 the surface the
player last landed on (`0xef416`: its layer in the cell, 0 the floor; 0xff a block, a creature or a
wall top) = a0 — CLONE's spawns wait for the player on the floor, not on the 2-unit tripwire
blocks across their cells; 32 a0 = a1, which only a shot makes true: a hitscan shot that hits a
prop with a script (`FUN_00030f9e`) writes the prop's slot into a1 of the script's first
condition and tries it, so MOTHER3's generators (`32: 0x11` on prop 17, and so on) wake when shot.

The readied item's conditions (the inventory in mode 7, `0xe417a`; section 5l) compare what it was
used on, which `FUN_0004032d` writes into the item's own script's params and events: 11 and 12 a
wall in cell (a0, a1) (event 0x20; the cell is the edge's, the hit point's x and z over 64); 13 a
floor (0x10); 14 a door in cell (a0, a1) (0x08); 16 an item whose sprite is a0 (0x04); 17 the
creature with id a0 (its record's u16 +0x22) in group a1 (+0x47) (0x01); 25 a door whose key is
a0 (0x08); 26 any item readied at all — a script whose first condition is 26 runs the moment its
item is selected; 27 the demo packs are going off (`0x74f02`).

**Actions** (`FUN_0003ab56`; ai = arg byte i, u16(i) the u16 at byte i):

| Op | Action |
|---|---|
| 1 | Wait u16(0) (timer units; 64 = 8 ticks) |
| 2, 3, 18, 19, 20, 21 | The first door in cell (a0, a1): toggle (not if locked, with its sound), toggle the lock, open (even locked, no sound), close, lock, unlock; 17 toggles the lock of the door in the cell given by params |
| 4 | Health += s16(0), at most 104; at 0 the player dies (5) |
| 5 | The player dies (sound 125 at 128) |
| 8 | Move a wall up or down (`FUN_00022109`): cell (u16(0), u16(2)), side u16(6) (0 west, 1 north), distance s16(4), speed u16(8). The wall's base moves 4 units every `speed` ticks, \|distance\| / 4 steps (at speed 0 the whole distance at once). LAB2's lockers at (59, 10)–(59, 20) slide up 128 |
| 9 | The same for a cell's surface, u16(6) 0 the floor (`FUN_000222ea`; the record's four corners all move): a lift. Items within 4 of the new height ride along, and creatures |
| 10 | Put up a wall at (a0, a1) side a2 (0 west, 1 north): texture s16(4) both sides, base s16(6), solid |
| 11 | Take away the wall at (a1, a2) side a3 (`FUN_00013acd`) |
| 12, 13 | Label a0; go to label a0 |
| 14 | Bring in a copy of creature a0 (by slot, below) at the middle of cell (a1, a2) (`FUN_00048a30`): the creature as it is now (hit points, script, a copy of what it carries in slot 2), at its height, set going (mode 1). Not of a dead creature, nor past the table's 80; the copy is called off, its slot still used, if the player or another creature is in the way. Monster closets in MOTHER1, ambushes in MAINTEN2, SDI1, SUBWAY, SDILAB and CLONE, MOTHER3's generators |
| 16 | Creature a1's mode = a0 unless it is dead (`FUN_0002b31e` → `FUN_0002af83`): 0 idle, 1/2 going, 3–11 moments of its animation, 12/13 frozen. The levels use 1 (LAB2, the scientist) and 13 (CORE) |
| 22, 23 | Start or stop entry a0 of the level's animations (`FUN_0002208b`, `FUN_000220ca`; section 5m): ALIEN1's power coils stand still until the ship's power is back |
| 24 | Move the player (`FUN_0003733b`) to cell (u16(0), u16(2)), on the surface under s16(4), facing a16 (0 east, 0x40 north, 0x80 west, 0xc0 south); the screen fades out over 64 retraces first. With a mission number a6 that isn't the current one: that mission's level loads first (keys cleared; missions 4, 7, 11, 14 restore health 104 and full armour), slot 6 is saved, and the game exits with code u16(8) for CYCLONES.EXE, which plays that video (section 5f) and restarts the game. BASE ends at (34, 25): mission 2 at (31, 1), video 30 |
| 25 | The end of the game: exit code 22, for which CYCLONES.EXE plays videos 22 and 40 and stops (MOTHER3, cell (31, 3)). The engine fades out, plays them, and leaves the options screen with no game to return to (the opening text, `finaltxt`) |
| 26, 29, 31 | Not in the game's switch: nothing happens |
| 27 | Set a light group's flicker (`0xe8d18`): accepted, but the engine doesn't animate lights yet |
| 28 | Sound a0 at volume a1, pan a2 |
| 30 | A sweeping beam of light (`0xe8de8`, `FUN_00044333`, drawn by `FUN_00044149`): accepted, not drawn (the engine has no dynamic level lighting yet) |
| 32, 48, 38 | A message, green (`c` font) / red (`gmred`); 38 adds to the last one (69 characters at most). Message colour 3 is the amber `ag` font |
| 33 | Move a block (`FUN_0002284e`): every block (CUBE record) whose u16[0] is u16(0) heads for base s16(2), 4 units every u16(4) ticks, stopping within a step of it (`FUN_000229c4`). A block already moving ignores it; at speed 0 the base moves *by* s16(2) and the block never moves again. Going down it waits for whatever is under it (creatures, items, the player, other blocks and doors: `FUN_000126fb` doesn't look at floors, so a block sinks through them); going up it lifts what stands on it. LAB2's red gates (id 255, base 300) rise to 428, into the ceiling, once the four switches round them make counter 3 reach 100 ("Access Granted"). Three lift platforms are sent below the floor they started on: SDI2's at (8, 30) (block 20, up to 220, then to −220) and at (6, 11) (block 10, 117 up to 300, then 101, its top 4 under the 115 floor), SUBWAY's at (48, 17) (block 60, 110 up to 700, then −700). The engine (a fix of the data, not the game) stops a platform — a block 16 units high or less that began on the floor under it, or up to 16 over it — at where it began when it is sent below that floor (`World::startMover`, `cubeStartBase_`; `CYCLONES_SCRIPTS=1` logs `dbg lift`). |
| 34 | Put on suit a0 (`FUN_0004e1ff`: 1 the first, 2 the bio suit, 3 the nanotechnology suit), its armour at full (BIOLAB's suit, HIVE1's nanotech suit) |
| 35 | Armour += s16(0), held to 0..104 (`FUN_0004e2f0`), then as 43 |
| 36 | Creature a1's class (+0x6f) = a0 (`FUN_0002b362`; BIOSTOR2's lift doors) |
| 37 | A wall's flags (a3; bit 0 not solid) |
| 39, 40 | Counter a0 (1–10) += (s8) a1 (held to 0..255) / = a1 |
| 41 | Creature a0 drops what it carries (slot 2, `+0x5b`) where it stands, alive (`FUN_00048eeb`): LAB2's scientist hands over his key card |
| 42 | A wall's pictures: (a0, a1) side a2 gets u16(4) in front, u16(6) behind — BASE's switches turn from wall 266 to 267 |
| 43 | The script's own item is taken out of the game (`FUN_000412f1`): out of the inventory (a readied one is no longer), or off the floor |
| 44 | Move every west and north wall in x a0..a2, z a1..a3 by s16(6), 4 units every u16(8) ticks (`FUN_00022e84`, a mover of its own): MOTHER3's rows of panels |
| 46 | A demo pack goes off (`FUN_00033977`), radius a0 cells: every door within it that can be blown (its flags' 0x80: the u16 after the file's flags lacks 0x01) is blown open for good, and creatures and the player in sight of the pack take 60–120 of explosive damage; the pack is gone (MUSEUM2) |
| 47 | Give the player key a0 (into a free slot of `0xe43cc`) |
| 53 | The jump jets' fuel (`0xef7b2`) += the script's item's charge, then the item is gone (`FUN_00042c81`; SDILAB) |
| 54 | A hazard (`FUN_0004e3a0`): while the player stays in area a0, health += s16(2) every a1 checks of a1 ticks (`FUN_0004e443`, a mover with function 12; one at a time, `0x7525c`). BIOLAB's chemical pools: area 9 −16 every 0.36 s, area 10 −30 every 0.04 s; SDILAB's area 1, MOTHER3's |
| 55 | Prop a0 (its slot: every OBJ record has one, `+0x32`) shows picture `startobjects` + u16(2), animated if the picture is (`FUN_00049085`) |
| 56 | Prop a0 is taken away (`FUN_00049114`) |
| 57 | Prop a0's script stops (`FUN_000490e2` → `FUN_0003a9fb`): out of the queue |
| 6, 7, 15, 45, 49–52 | In the game's switch but used by no level: hurt creatures by id and group (6, 7), remove creature a0 (15), a mover of wall rows (45), copy an item's charge into the jets' gauges (49–52). The engine leaves a script with one of these idle |

**In the engine** (`src/game/script.cpp`, `inventory.cpp`, `surfanim.cpp`): every condition, and
every action any level uses — all 860 scripts of the 21 levels on the disc (the tutorial's
included) run.
What the engine doesn't show yet is the light: action 27's flickers and action 30's beams are
accepted and do nothing. (The jump jets fly since 0.5.2's last update: section 5n.) Walls the scripts change are
changed in the level and the level's geometry is rebuilt.

**Movers** (actions 8, 9, 33, 44 and 54). The game keeps up to 50 (`0xdebb4`, 0x27 bytes: +0
kind, +1 progress, +5 the ticks between steps, +9 progress a step, +0xd the tick of the last step,
+0x11 the tick it ended, +0x15 the end, +0x19.. cell, side or block, +0x25 the function, from the
table at `0x74ab8`: 1 a floor `FUN_0002243d`, 2 a wall `FUN_0002225a`, 3 a block `FUN_000229c4`,
a block of walls `FUN_00022fb1`, 12 a hazard `FUN_0004e443`), all run every 100 Hz tick by
`FUN_0003ce15`. The engine builds every block, and each wall and surface some script moves, apart
from the rest of the level, and builds just those again as they move (`Renderer::buildMoving`);
the walk maps are worked out again when a mover ends.

**Creature slots.** Scripts name creatures by their slot in the game's table (`0xe95c4`, 80 of
0x89 bytes; +0x6f class, +0x70 mode, 5 dead): the level's `MON` records the difficulty keeps, in
file order (`FUN_00010d05` skips the others without using a slot, and drops their scripts), then
the copies action 14 brings in. The engine keeps the same numbering (`Monster::slot`), counting a
creature it can't show too. Props are named the same way, by their place among the level's `OBJ`
records (all of them: every prop is on every difficulty).

**"Use"** (`FUN_0003fed1`) picks what the cursor is on from what the game drew (`FUN_00013524`
walks the frame's spans and skips a sprite's or a wall's see-through texels), so a creature in a
cage or behind a window can be used — SDI2's "Mated Pair" in their glass cases. The engine picks
along the line of sight the same way, through the holes of wall textures (`Renderer::holeAt`),
and so do the player's hitscan shots. Every target must be within 128 units on each axis
(`FUN_00040967`; "DOOR / ITEM / OBJECT OUT OF RANGE."). A door whose panel lies on a wall's own
line (BIOLAB's hidden door at (53, 46), offset 0, on its holographic wall) is in front of it: the
engine draws a panel 3 units either side of its line, and the pick takes a door within 3.5 units
behind a wall's hit.

**Saves** (version 5) keep every script's state and parameters, the queue, the counters, the
walls changed, where the moving parts are, the movers still going, the creatures scripts brought
in (each made again from the one it copied), every creature's slot and whether it still carries
its item, the surface the player last landed on, the props' pictures and those taken away, the
doors blown open, the inventory (its eight slots, the readied item, the highlight, whether it is
open), the placed demo packs, the jump jets' fuel, the hazard and the animations (each entry on or
off, its count, and the picture every number shows). Versions 1–4 still load (with an empty
inventory, the kits made into stacks). After the state the slot file (`main.cpp`, `writeSlot`)
now ends with "MAPZ" and two floats, the minimap's and the full map's zoom, which a load puts
back (older engines stop reading at the state's end; older saves keep the zoom you have). The
zoom also goes to `settings.ini` (`minimap_zoom`, `map_zoom`); the minimap zooms in steps of 1.25
from 0.8^4 (about 17 cells to its rim; up to 0.5.2 it stopped at 0.5) to 2.5. `CYCLONES_SCRIPTS=1` logs scripts starting, their actions,
movers, creatures brought in, wall changes, walls and things used and the readied item's uses.
Testing: `CYCLONES_COUNTERS=3=75,2=50` presets counters; `CYCLONES_KILL=35,36` kills those
creatures (by the engine's index, as `CYCLONES_LIST=1` lists them) a fifth of a second in;
`CYCLONES_GIVE="44,5;51,22"` takes the items lying in those cells at once, as "use" would.

## 5l. The inventory (FUN_000420c0 in, the slot handler at 0x41948, FUN_000412f1 out)

Eight slots (`0xef419`, an item's index each). What goes in depends on the item's use (`+0x4d`
of the item record): 4 and 5 activated items (selecting one switches it on or off, its
background yellow, and tries its script), 6 quest items that are *readied* (red; the inventory
goes to mode 7 and the next "use" hands the item to whatever the cursor is on, `FUN_0004032d`), 7
demo packs (selecting one sets it down at the player's feet showing its "set" picture,
`FUN_000428dc`, at most eight), 9 bio-kits and 10 mech-kits, which stack in one slot each (a kit
picked up adds to the stack and is gone; a stack's count shows from two). Keys (use 8) take no
slot. A full inventory leaves an item where it lies ("NOT ACQUIRED. ITEM WOULD EXCEED STRUCTURAL
CAPACITY."). With an item readied only its own slot does anything (it unreadies); closing the
inventory unreadies it; using it on nothing unreadies it; a script whose first condition is 26
runs as soon as its item is readied (HIVE1's nanotech kits). Between missions the inventory is
emptied but for the kit stacks. (Some key cards carry scripts waiting for them to be readied —
BIOSTOR2's — which can never happen: a key takes no slot. Their doors open by the key.)

The panel (`FUN_000414d1`, buttons 17 down to 10 of the table at `0x74b97`, 19 bytes each: flag,
x0, y0, x1, y1): slots up the left edge at (3, 156), (3, 125), (3, 94), (3, 63), (3, 32), (3, 1)
and along the top at (50, 1), (97, 1), 45 × 29 each — the `meshgrid` plate, a grid of lines every
9 pixels in palette 0xb5 (green), 0x7c when readied, 0x6b when switched on, the item's `i_...`
picture (by its own anchor), and the count; the DROP button (`dropicon`) at (3, 187). The Demo
Pack Icon (`demopakicon`, blinking with `demopakicon2`) shows above the graphs while packs wait;
selecting it (`FUN_00042a96`) sets `0x74f02` and tries every placed pack's script (condition 27;
MUSEUM2's packs then run action 46). The key ring (`invkeyr` / `invkeyb` / `invkeyg` for one,
two, three or more keys) shows at (52, 177).

The engine: the inventory key (I, as in the game) opens and closes it. While it is open the view
stands still and the mouse moves a cursor, as the game's reticle did — the manual's small purple
box, an orange down arrow (`arrowb`, tinted) once DROP is pressed. A click on a slot selects it
(1–8 too, or the weapon wheel and fire on the highlight); an item dragged out of its slot and let
go over the view is readied and used on what is under the cursor; a click in the view uses the
readied item there (with nothing readied: a door, switch or item there, the inventory staying
open); a right click is the game's "use" (it closes the inventory when nothing is readied); DROP
then a slot, or an item dragged onto DROP, or Backspace drops; the Demo Pack Icon detonates (so
does B, the Detonate control). What the cursor points at is picked along the line through that
pixel (`App::cursorCamera`, the renderer's projection). Esc closes the inventory before it opens
the menu. A dropped item isn't taken
again by walking over it until the player has stepped away, and a placed demo pack can't be
taken at all.

**Data transmitters** (an engine addition, not in the game): once a data stack is used on one of
BIOLAB's transmitters (wall texture 59, the lump `wall57`) and its script starts, that wall shows
wall numbers 1000–1007 instead, eight frames the renderer makes from the terminal's own pixels
(`Renderer::transmitterFrame`): the big screen's lines of text scroll and slide, the small screens'
rows cycle, the level bars rise and fall, the read-outs run, the lights blink and the red bar
fills; a new frame every 12 ticks. The change is an ordinary wall edit, so saves keep it.

**Computer screens** (an engine addition, not in the game): every wall and block side showing a
terminal's screens draws with one of four textures of its kind (`Renderer::kIdleScreen` 1200 +
kind × 4 + group; the group is a hash of the wall or block side, so neighbours differ): kind 0
`wall57` (the mainframes, consoles and wall monitors: SDI1's computer rooms, whose consoles and
monitors are blocks showing rows 50–128 and 10–48 of it, and CLONE, FAILSAF2, BIOSTOR2, SUBWAY and
SDI2), 1 `wall359` (SDI1's twin read-outs beside the missile consoles), 2 `wall150` (a keypad's
green display). The renderer repaints each texture ten times a second from the world's clock
(`Scene::time`, so it stands still while the game is paused) over the wall's own pixels, told
apart by colour (`Renderer::idleFrame`): `wall57`'s big screen is a log, a new line (a stretch of
one of its five text lines, slid along) typed in at the bottom every 1.6 s while the others move
up, a two-pixel cursor blinking after it; each small green screen's four rows change one at a
time every 1.3 s, staggered; the level bars drift; the red bar under the big screen rises and
falls; the read-outs on row 79 change; the red lights (row 74) blink at 1.1–2.5 s; the lit
buttons (rows 93 and 102) go dark one time in five. `wall359`'s read-outs scroll at 5 pixels a
second; `wall150`'s line changes every 1.4 s and its bar drifts. Each group runs at 90–111 % of the
pace and from another point. Walls a readied item is used on (script conditions 11 and 12: the cell in
their first two arguments) stay as they are (`Renderer::setStillWalls`): BIOLAB's nine data
transmitters, which come alive only when a data stack goes in.
Kinds 4–11 are MOTHER1's alien screens (MOTHER3 has the star ones too), each drawn as two walls,
left and right halves: 4–5 `wall221`/`wall222` (the big window on space), 6–7 `wall227`/`wall228`
(stars over a perspective grid with three poles), 8–9 `wall258`/`wall259` (a console: stars above,
stars and a red wireframe globe below, a blue display, two green lamps), 10–11 `wall224`/`wall225`
(a green tactical map). Their effects run by the world's clock without the groups' offsets, so a
screen's halves keep together. Stars: the screens are the patches of the texture's black (palette
1) over 120 texels, a star a cluster of up to 24 grey texels inside one (a point, or a cross whose
arms are dimmer than its middle), brightened and dimmed along the greys 0.55–1.15 at 1.2–3.8
radians a second from its own phase, its arms by the cube (a cross shrinks to a point below about
0.56); the dark-blue specks (palette 151) go out one time in four. The grid: a band 1.6 texels wide
(×1.4 brighter) moving down the green rows at 9 texels a second; the beacons (the brightest bluish
texel of each 3 × 3, ≥ 380 of 765) pulse 0.75–1.25, the red feet go dark 0.5 s in 1.2. The globe:
its red texels lit by a light 7 texels wide (×0.6–1.25) running across both halves at 14 texels a
second; the blue display's rows (blue texels over 60 brightness, 8 or more wide, below row 96) show
made-up letters scrolling at 6 a second; the green lamps alternate every 0.7 s. The map (no black:
its screen is the commonest green, 175): a scan line with a fading trail (×1.7) moving down at 11
texels a second, the blips (3 × 3 bluish, middle palette 58) pulsing 0.7–1.3, the brightest green
marks dimming to 0.6 one time in three per 0.9 s.
Kind 3 is `wall93`, the aliens' pod computer (HIVE1 2, HIVE3 1, ALIEN1 1, MOTHER3 2 walls): the red
lamp (rows 69–73, x 28–34) breathes (brightness 0.55–1 on a 2.6 s sine), the pilot lights at
(19, 71) and (43, 71) blink in turn every 1.1 s, a bright line sweeps the hand-print screen (rows
81–95, the blue texels) in 1.6 s every 4.2 s and the print then glows for 0.6 s, a line of
made-up letters runs along the blue display (rows 104–105, x 5–24) at 6 pixels a second over a
level bar on row 106, and the green lamp (rows 103–105, x 52–58) is out 0.45 s in every 1.6 s.
Colours are changed by brightness along the palette's own red, green and blue ramps
(`Renderer::idleRamp_`). A `wall93` in a cell a script takes an item at (HIVE1's (26, 11), the
Code Pod, script 22, "Force Shielding Lowered", and (9, 27), the Coordinate pod, script 23,
"Phase Shift Mechanism on-line") is not still but gets its own texture, `Renderer::kPodScreen`
1400 + i, painted from the scene's `ScanScreen` of kind 3 (`World::pods_`, scan.cpp): waiting
(green lamp out, a sweep every 3 s), taking the pod for 2 s from when its script runs (screen and
red lamp flashing, the text racing, the bar filling) and done (red lamp out, green lit, the screen
glowing). Its script is over in a few ticks, so it is mostly seen done rather than running: done
when first looked at after a level or saved game loads counts as long since.

**SDI2's launch computer** (an engine addition, not in the game): the west wall of the launch
room at (2, 30)–(2, 33): `wall323` and `wall324` (animated columns, surfanim) round `wall330` (2, 31)
and `wall329` (2, 32), which make one picture 128 wide: a monitor (glass 18–104 × 17–44 of the
pair) over three levers (slots at x 40, 52 and 64; handles 7 wide on rows 70–72, 65–67, 70–72).
The launch codes (item script 9, condition 12: used on the wall at (5, 32), `wall360`) print
"Missile Launch Sequence INITIATED" and open the emergency elevator. The engine gives the two
walls their own textures, `Renderer::kLaunchScreen` 1300 (`wall329`) and 1301 (`wall330`), and
`Renderer::launchFrame` paints them from when that script starts (found by its first message,
`World::launch_`; a game loaded with it done shows the end): the monitor switches on, LAUNCH
CODES typed in, ACCEPTED blinking; ARMING, the levers going up to the top of their slots 1.5,
1.9 and 2.3 s in (0.3 s each), a box lighting as each arrives; then SEQUENCE INITIATED, three
lit boxes, ARMED, and a flight path plotted from the silo (63, 41) over the top to the target
(100, 40) in 1.2 s, the target locking on at 4.7 s; from 5 s it holds that, the path's dashes
marching and the target blinking. 30 frames a second while it runs, 10 after. In the game the
monitor stays grey and the levers as they are.

## 5m. Animated textures (the `surfanim` lump, FUN_00021db2 loads it, FUN_00021de5 steps it)

CYCLONE.HD's `surfanim`: `s16 last`, then `last + 1` entries of 12 bytes — `s16 on`, `s16 kind`
(0 walls from `startwalls`, 1 flats, 2 items from `startitems`, 3 props from `startobjects`),
`s16 period` (ticks), `s16 count`, `s16 first`, `s16 last`. Every tick each entry that is on
counts down; at nothing it counts `period` again and every number in first..last shows the next
picture of the range, wrapping round (translation tables per kind: walls `0xc63d8`, flats
`0xb9640`). Screens flicker, fans turn, coils glow, a placed demo pack's light blinks. Level
scripts switch entries on and off (actions 22 and 23). The engine draws each batch of the level
with its texture's current frame, and animated items and props with the frame of their range.

## 5n. The jump jets (FUN_000409e5, FUN_0004d5e0, FUN_00042c81)

The jets are an item (`s_jmpjet`, use 5) whose script (condition 9, action 53) runs as it is
taken: the fuel (`0xef7b2`, s16, −1 = no jets) becomes the item's charge, or gains it, and the
item is gone — SDILAB's at (22, 6) holds 500, HIVE3's at (24, 1) 50. A new game starts at −1
(`FUN_0004cf2a` sets 15000 instead when its second argument is set: `0x73c9d`, which only one of
the developer command-line switches sets).

- **Switching** (`FUN_000409e5`, the key dispatcher's function 0x2a; the data card: "J = toggle
  jump jet on/off"): on → off; off → on only with fuel above 0. The flag (`0xe41a8`) is the
  HUD's, cleared at every level start.
- **Flying** (`FUN_0004d5e0`, every tick): a jump from the ground sets the speed to `0x600000`
  (3 units a tick) and a 25-tick delay (`0x75256`, counted down every tick). In the air, with the
  jets on, the jump key held (`0x75255`, or the Jump Jet Icon held with the mouse, `0x75254`) and
  the delay over, the speed gains `0x10000` (1/32 of a unit a tick) *instead of* gravity's
  `−0x4b000`; otherwise gravity. On every tick whose number is a multiple of 8 while they push,
  the fuel drops by 1, and when it is then odd sound 84 plays at 200; at 0 the jets switch off and
  the fuel becomes −1 (the icon goes). Height moves by speed / 32 each tick (16.16).
  The manual's "they constantly use fuel" is not what the code does: only thrust burns it.
- **HUD** (button 8 of the table at `0x74b97`: (3, 126)–(24, 134)): drawn while the fuel is 0 or
  more, `jumpjeticon` (green JJ) with the jets off, `jumpjeticon2` (red) on; the fuel "%03d" at
  (28, 129) in font 0 (the shadow) and at (27, 128) in font 3 (amber) off, font 5 (red) on.
  Button 9 at the foot of the screen, the jump button, shows `gjumpicon` / `rjumpicon` likewise.

**In the engine** (`player.cpp`, `main.cpp`): the same rules in the 100 Hz flight steps
(`kJetPush` 3.125 units/s a tick, `kJetDelay` 25); the fuel burns on every 8th tick of flight. The
JUMP JETS action (CONFIGURE; J by default) and a click on the icon switch them; the icon and fuel
sit under the left altimeter at the game's (3, 126), the key's name small over the number. The
jets are off after loading a game, as at a level's start. `--inv "frame:j"` switches them in
captures; `CYCLONES_LIST` lines end with `jets 0|1 fuel n`.

## 5o. The weapons cache (the drawing at 0x417e0; C, or the ammunition button)

Eight tiles, buttons 26 down to 19 of the table at `0x74b97` for weapons 0 to 7: (272, 156),
(272, 125), (272, 94), (272, 63), (272, 32), (272, 1), (225, 1), (178, 1), 45 × 29 each. Each is
the `meshgrid` plate, a grid of lines every 9 pixels (`FUN_0003920c`: x0 to x0+45, y0 to y0+27)
in palette 0x7c (red) for the readied weapon and 0xb5 (green) for the rest, then the picture from
the weapon table (`0xef428`, 0x6e bytes a weapon: +2 the gun's picture, `i_gaussp`, `i_chaing`,
`i_alpstl`, `i_dbgun`, `i_grnlch`, `i_twnlas`, `i_rktlch`, `i_plasma`; +6 its clip's, `i_gusclp` …
`i_plsmclp`, a red outline of the gun with the ammunition drawn in): the gun if you have it, the
clip if you only have rounds, nothing otherwise; and the rounds ("%d", font 1) at +2, +2. The glove
has no tile.

**In the engine** (additions): the readied weapon's tile stands left of the rounds box, the
weapon's name over it (the armour and health numbers moved above their bars to make room); the
glove's tile shows the suit's glove (`glove10`, `glove20`, `glove30`, shrunk by averaging, as
`wglove0`–`2`). Turning the wheel (next / previous weapon) shows all nine tiles in a row above
it for 1.6 seconds: the readied one red, a clip's outline for rounds without the gun, a weapon you
have nothing of dimmed. `--inv "frame:w+"` / `"w-"` turn the wheel in captures.

## 5p. The Enhanced cut-scenes and the EXTRAS player (additions)

A folder `HD Video` beside the program (or `--video-pack dir`) holds new pictures and speech made
from the game's own videos; with it, and SETTINGS, CUT-SCENES on ENHANCED (`cutscenes=enhanced`),
they are shown in the game's place on the game's own timing (frame rates, holds, palette fades,
CD music: everything still comes from the `.SSD` scripts and the `.SMK` frame count). Without it
nothing changes. Its `index.txt`:

    video NAME full FILE                       a new picture for every frame (the CGI)
    video NAME window X Y W H FILE RUNS        a picture for the live-action window at X,Y W×H
                                               of the game's frames; RUNS ("B170 .208 ...") say
                                               which frames show it (B) and which don't (.)
    speech ID FILE                             GAME\SOUND\ID.RAW cleaned (FLAC)

The pictures are AV1 (`.ivf`, 10-bit 4:2:0, BT.709 studio range), one per frame of the game's
video (window files: one per B frame), a key frame every 30 with the sequence header, decoded with
dav1d (`src/data/hdvideo.cpp`; 10 bits come down to 8 with a 2×2 ordered dither). A whole picture
replaces the frame, smoothly scaled; a window picture is drawn over the game's frame (its bezel,
maps and wireframes stay the game's pixels). The palette fade darkens them by the same amount.

How the pack is made (`tools/hdvideo/`): the briefings (1A2A, 1A2B, 1A3, 30–40) keep the game's frames; their video window
(77, 47, 164 × 106) in the frames that show the bezel is denoised over time (hqdn3d), debanded
and scaled 4× with Lanczos and a light sharpening (`live.py`) — no machine learning on the
actors' faces, which it would invent. The speech is declipped, denoised (FFT, 20 dB) and
resampled to 22,050 Hz (`encode.sh`). The CGI videos (RAVEN, INT1–3, 4, 7, 11, 14, 22) have no
entries: they play as the game's own. (A 4× upscale of them with Real-ESRGAN general-x4v3 —
`esrgan.py`, `dequant.py`, `cgi.py`, the credits kept pixel-sharp and the faint stars put back —
was in one build and taken out: it smoothed away detail the original pixels carry. The engine
still takes `full` entries.) Video 14 is on the disc but the game never plays it.

**The EXTRAS player** (SETTINGS, third page; `src/game/videoplayer.cpp`): a row for each
playlist the game plays (the opening, the first briefing, after levels 1, 2, 3, 5, 6, 10, 11, 16,
the end, after special ops 1 and 2) and video 14, with its length. To seek, a playlist is first
run through the way Cutscenes plays it, without pictures, giving a timeline: when each frame shows
(the last video's last frame until the next one's first), the palette fade at any moment, when
speech and CD music start and stop (the CD stops at the end of every video). Any moment can then be
shown (the Smacker frames decoded again from the start of their video when going back; the AV1
pictures from the key frame before) and the speech and music are started part-way to match;
playing keeps the picture to the speech as heard, as the game's cut-scenes do. Controls: Space
play / pause, Left / Right 5 s, Up / Down 30 s, comma / period a frame, Page Up / Down the video
before / next, Home / End, E Enhanced / Original, Esc or a right click back; the bar (shown while
paused, or for a moment after the mouse moves) can be clicked and dragged. Testing: `--extras n`
opens playlist n at the start, `--extras-do "frame:what;..."` (p play / pause, s<seconds> seek,
f / b a frame on / back, e Enhanced, m<x>,<y> the mouse in window pixels, x close).

**Subtitles** (SETTINGS, SUBTITLES): `subtitles ID FILE` lines in `index.txt` name a SubRip file
for `GAME\SOUND\ID.RAW`, its times from the start of that speech (`subtitles/*.srt`, plain text,
editable). The cue under way is drawn in the game's green lettering at the video's own pixel size,
centred on the bezel under the video window (row 178 of 200), on a dark band; in the EXTRAS player
above its bar while the bar shows. The time is the speech's as the picture is timed (Cutscenes:
the timeline since its op 0, which follows the speech as heard; the player: its clock less the
op 0's moment). Text in capitals; characters the font lacks are dropped (curly quotes and dashes
become plain ones). The drafts were made by speech recognition (`tools/subtitles/transcribe.py`:
Silero VAD for the cues' times, NVIDIA Parakeet TDT 0.6B v2 for the words, Whisper small.en as a
cross-check; `recheck.py`: Whisper large-v3-turbo on each stretch and on whole sentences of up to
25 s) and proofread from the three (`final.py`, which writes the shipped files): the code names
and terms as the game's mission texts and manual write them (Lost and Found, Fail-Safe,
Starfire, Bio-link, Hangar 18, Advanced Ideas, HAVOC); the characters' names by ear, as the game
never writes them.

## 5q. Other additions in the engine

- **BRIGHTNESS** (SETTINGS, SOUND AND PICTURE; `brightness=0..10`): after the 3D view is drawn,
  the screen goes through `pow(c, 1 / (1 + n / 10))` (`Renderer::brighten`); the status display,
  map, first-person weapon and screens are drawn after it, unchanged. At 0 nothing is done.
- **Help** (F1 or H): a panel over the paused game in the game's lettering: the actions with
  their keys as set, the fixed keys, three tips. Any key or click closes it.
- The testing keys that changed levels (Page Up / Page Down, [ and ]), flew through walls (G),
  set off explosions (X) and held the monsters still (M) are gone; `--level`, `--boom` and
  `--hold` remain for captures.

## 5d. Engine test switches

Captures (`--shot f.bmp --frames N --every K --burst C`) run at a fixed 60 Hz step and ignore
`settings.ini`, so they repeat exactly. `--fast` skips drawing the frames that aren't saved.
`--start x,z,yawDeg[,pitchDeg[,height]]` places the player (with a height, on the surface under it), `--keys "W:10-90;A:40-60"` holds keys over frame
ranges, `--fire`, `--use` and `--boom` take frame lists, and `--god`, `--peaceful`, `--allweapons`,
`--difficulty`, `--ai` and `--weapon n` set up the fight. Environment variables:
`CYCLONES_LIST=1` lists every creature and item at load and logs the player and the AI's cost twice
a second; `CYCLONES_TRACE=<index>` logs one creature's AI state, shots and blocked routes;
`CYCLONES_NAVDUMP=x0,z0,x1,z1,height` prints part of a walk map. `tools/play.sh` turns a burst
into a contact sheet; `CYCLONES_TRACE=-2` logs every hit on a creature. `--hurt` (frame list) deals 30 damage that ignores armour, `--kits n`
carries n kits of each kind, and `--fov n` / `--square` set the view. `--menu` opens the options
screen, `--panel n` opens button n's screen (0 RETURN … 8 NEW GAME; 6 SETTINGS, 7 CONFIGURE),
and `--menumouse x,y` places the cursor (320×200 coordinates); `--settingsrow n` chooses a SETTINGS
row (0 to 8 on the first page, 9 to 18 on the second, 19 and up on the third, EXTRAS).
`--brightness n` sets BRIGHTNESS (0–10) for a capture; `--inv "frame:help"` opens the help
screen. Captures make no sound but `--wav out.wav` writes what
they would have played (16-bit stereo, 44.1 kHz, in game time); `CYCLONES_SOUNDLOG=1` logs each
sound as it starts. `--inv "frame:what;..."` presses inventory keys (i open or close, 1–8 a slot, + / − move, s select, d drop, b detonate, u use; also j the jump jets, w+ / w− the weapon wheel). `--intro` / `--nointro` play or skip the opening (captures skip it unless
asked; `--fire` frames stand for a key there). `--save n` saves to slot n as a capture ends
(description "CAPTURE AT FRAME n"), `--load n` loads slot n at the start; `CYCLONES_TRACE_FAST=1`
traces every frame instead of four times a second (with the creature's position).
`CYCLONES_BEFORE=BASE,LAB2,...` loads those levels first, in order, before the one asked for, as a
game played from the start does: what the engine builds for one level (creature pictures and
animations) is kept for the next, and a capture of a lone level never shows that.
`CYCLONES_EXIT=1` takes the level's way out to the next mission after a second (its video too,
outside captures, which skip videos); `CYCLONES_LIST=1` also lists the level's ways out.
`CYCLONES_AVLOG=1` logs a playing video's picture against its speech once a second,
`CYCLONES_STALL=ms` stalls the loop that long every 3 s of video, and `CYCLONES_AVFREE=1` times
the picture on its own, as up to 0.5.2 (real time only: captures have no sound device and keep
the plain timing). `CYCLONES_SOUNDLOG=1` also gives each placed sound's fade distance. `CYCLONES_SOLIDSIGHT=1` makes every
wall, block and door panel stop creatures' sight, as up to 0.5.2. `CYCLONES_FLIERS=1` reports a
flier whose box has gone into a surface (`dbg flier into a surface`) and, every 10 s, how many
fliers have moved a cell or more; `CYCLONES_LIST=1` also gives each creature type's box and
sprite size (`dbg size`), the fliers placed without room for their first walk frame (`dbg
flier`, stuck as in the game or started lower), the walls whose screens stay still (`dbg
still screens`) and the pod computers that take a pod (`dbg pod computer`), and each creature's
line ends with `stationary` for the fixed ones and its walk frame count. The trace
(`CYCLONES_TRACE`) gives the creature's height too. `CYCLONES_STEPCHECK=1` walks the player's
body (without creatures or props) from every cell's floors to each neighbour's floors a jump can
reach, in half-unit steps, down the middle and 12 units to either side (`12 off the middle`),
stepping up and jumping where it must, and lists those it fits on but can't get onto,
other than across a wall standing over both floors (`dbg stepcheck`), and floors with 20–55
units of room over them (`no room`).
`CYCLONES_CAPFPS=n` runs captures at n frames a second instead of 60 (frame numbers count those
frames). `--warp "frame:x,z[,height];..."` puts the player in that cell at those frames (as a
script's teleport does). `--view "frame:yaw,pitch;..."` turns the view at those frames (degrees), to film what is behind
you after using something. `CYCLONES_LIST=2` logs the player twenty times a second, and `CYCLONES_LIST=3` also prints every
trigger cell with its scripts (`dbg trigger cell x,z: 5 6`) and every script of the level as it
loads, one line each: `dbg script i flags F: cond(8 args)... | op(8 args)...`
(the numbers are the codes of 5k; the texts of actions 32, 38 and 48, the messages, show as
text in quotes).

## 6. Native engine plan (Phase 2)

The target is a C++20 engine on SDL3 with OpenGL 4.5 or Vulkan. It loads
the original files from the player's own disc image, ships no game assets,
and builds as an x64 MSVC/MinGW binary.

Releases were numbered 0.x until the whole game played through: that is 1.0. The previews below,
1 to 5.2, are versions 0.1 to 0.5.2.

| Milestone | Deliverable |
|---|---|
| M1: data layer + level viewer (**preview 1 delivered**) | Disc-image reader (raw BIN, ISO, CUE), BOL, MAP (all geometry chunks), palette, fade table and sky loaders. OpenGL 3.3 renderer covering walls, floors, ceilings, slopes, extra surfaces and cubes, with the original fade-table lighting done in a shader. Hor+ widescreen, render scale and 320×200 retro mode. Still to do: doors, sprites, objects and monsters. |
| Preview 2: living levels (delivered, then replaced) | Entity decoding (monsters, items, objects, doors, monster types), wandering monsters, doors, explosions, dynamic lights. Its voxel models (built from the visual hull of the sprite views) didn't look like the original art, so preview 3 replaced them. |
| Preview 3.1: correct sprite decoding (delivered) | Fixed the post pixel order (top-down) found by side-by-side comparison with the original in DOSBox, and the animation clock (100 Hz). With the right pixels, the depth sprites, deaths, items and effects all match the 1994 art. |
| 1.1: the demo (**delivered**) | The CyClones demo ("Version 2", its level Stalag) plays from its folder (section 2, "The demo"): its texts on the options screen, its three weapons, its sounds; no music or briefings, as in the demo. |
| 1.0: the whole game (**delivered**) | Everything in 0.5.2 and its test builds, and the game played from BASE to the closing videos: every level's way out leads to the next mission (BASE, LAB2, ALIEN1, BIOLAB, BIOSTOR2, FAILSAF2, CORE, MUSEUM2, MAINTEN2, SUBWAY, SDILAB, SDI1, SDI2, CLONE, HIVE1–3, MOTHER1, MOTHER3, the end; HEAVY and the tutorial have none, as on the disc), and a game saved on any level loads back as it was. It needs nothing but the disc image (the installer's archive, section 2). The remaster's additions (sections 5n–5q): the jump jets, the Enhanced briefings and EXTRAS player, subtitles, the weapons cache on the status display, brightness, quick save and load, the help panel; the testing keys are gone. |
| 0.5.2: level scripts (**delivered**) | The game's script system (section 5k): trigger cells, entity scripts, conditions, timed actions; switches, teleporters, sliding walls, door scripts and their messages, sounds, counters, and the move to the next mission with its video. Every script in BASE runs. Then movers (actions 8, 9 and 33): blocks, walls and floors that scripts move — gates, lifts, lockers — built apart from the rest of the level so each step is cheap. Then creatures in scripts (actions 14, 16, 36, 41, condition 31): copies brought in, modes and classes changed, a carried item handed over, by the game's creature slots; LAB2's scientist is freed and gives his key card. Then the rest (sections 5k–5m): the inventory with quest items readied and used on walls, floors, doors, creatures and items, kit stacks, demo packs and their detonation, suits and armour, animated textures, props' pictures and scripts (shooting MOTHER3's generators), walls moved in blocks, hazard areas, the jump jets' fuel and the end of the game. All 860 scripts run. |
| Preview 5.2: saved games and the monsters' pace (**delivered**) | LOAD and SAVE on the game's own screens (six slots, picture, level and difficulty, date, typed description; the questions start on NO as in the game), with the engine's own save format; the original AI checked against the running game's memory tick by tick, and fixed where it was harsher: creatures attack along their 8-way axis heading (hitscan and melee miss a player who is side-on and they pause 75 ticks), chain attacks as the game does (even out of band), re-think after every blocked walk frame, and wake within a square, not a circle. |
| Preview 5.1: the opening and the weapon in hand (delivered) | A Smacker 2 decoder; the opening as `CYCLONES.EXE` and `SMKPLAY.EXE` play it (two playlists, each video run by its SSD script: frame rate, fades, holds, CD stretches, speech; a key skips the rest of the playlist); the first-person weapons from their art, poses picked by the game's placement rules (the reticle leaning with the view's turn), recoil, muzzle flashes, the suit's control-stick hand and the glove swings; SETTINGS in tabs, the game options first. |
| Preview 5: sound and music (delivered) | The game's sound effects from its `SOUND` folder, started by the same events with the same ids (weapon table, door records, the four sounds of each creature placement, impacts by damage type, gibs, death) and placed by the game's own distance and pan sums; each mission's CD track streamed from the disc image (a CUE sheet with one or several BIN or WAVE files, or for a bare CyClones BIN the disc's own layout, built in) and looped; SETTINGS in two pages, the first with MUSIC and SOUND on the game's own sliders (defaults 99 and 70) plus screen, vsync, rendering, lighting, field of view and picture shape. The game now starts at mission 1, BASE. |
| Preview 4.8: locked doors (delivered) | Key cards open the doors locked with their key ("ACCESS DENIED." otherwise); doors that a script can open or unlock stay openable until scripts run; doors nothing can open stay sealed, as in the game. Script actions are now read (entity scripts and the `TRIG` chunk). Block sides on a cell edge tucked behind the wall. |
| Preview 4.7: sky, pillars, doors (delivered) | The sky as the game draws it (sky1 + sky2 as one flat panorama scrolling at half the walls' rate, bottom on the horizon) instead of sky1 wrapped four times round a dome; cubes with no height take their side texture's height (the missing pillars) and their sides start at texture row [12]; door slabs right side out (they were inside out and mirrored). |
| Preview 4.6: stairs (delivered) | Creatures judge each step from the floor they stand on (`ground`), not from their drawn height, which eases toward each new floor and lagged a few units behind: on 16-unit stairs the 18-unit risers (MAPN/MAPW walls with flag 0x01) then blocked them, for good, since the height only settled while walking. Fliers no longer judge steps from their dive height. |
| Preview 4.5: the options screen (delivered) | Esc opens the game's own options screen, drawn from its art and fonts at its coordinates (rendered at 320×200 and scaled sharply, 4:3 with tall pixels): the nine buttons, the logo, the mission briefing (or the death report), the DIFFICULTY, QUIT and NEW GAME questions, CONFIGURE as the game's key screen, the engine's options under SETTINGS, the green cursor. Fixed: picked-up items stayed drawn. |
| Preview 4.4: doors and death (delivered) | Doors move as the game's loader sets them up (flag 0x80 slides, otherwise up or down, the speed's sign gives the direction, speeds in units per tick, starting states, stay-open flag), are seen only inside their doorway, and side-by-side panels open together; after death the menu opens (NEW GAME, RESTART LEVEL) instead of a "press fire" prompt. |
| Preview 4.3: fixes (delivered) | Wall flag 0x08 (not 0x01) marks walls you can walk through, so windows, crates and some 6,000 other walls are solid; ground higher than a step is solid even where it is above your head (no more standing on the BASE crates under a 20-unit ceiling); projectiles aim at the creature under the crosshair instead of the wall behind it; the "use" text cues are gone (the cursor colour tells); the stats panel and ammunition box drawn at about four fifths of the game's size. |
| Preview 4.2: the game's view and status display (delivered) | The game's projection (90° across 4:3, tall pixels; 75° square pixels had made everything look 1.3 times faster); the hit tear; the status display (stats panel, ammunition LCD, bearing, side scales, kit pictures); doors opened with "use" (or by walking into them, as an option), closing after 2 s, blocking shots and sight; the cursor colours (pink on doors, hidden ones too); projectile hits against depth sprites over the whole body (rockets and grenades hit directly), 10-unit projectile bodies and grenade last-contact hits; twin laser and rockets fired from below the view's centre. |
| Preview 4.1: fixes (delivered) | Walking speed as the game really reaches it (cell-centre clipping simulated per creature type); shot leading from the player's real velocity; the game's death screen; deployers stay open while firing and unfold before attack A too; rebindable keys, mouse buttons and wheel; pickups by "use" only as an option; jump/ceiling collision fixed (no standing on air); the game pauses in the menu. |
| Preview 4: the monsters fight back (delivered) | The player's body, health, armour, weapons, ammunition and pickups (walk-over or "use"); the HUD in the game's fonts; hit tests against each frame's own silhouette (row-filled and dilated; depth sprites marched through the view's depth); monster attacks from the embedded weapon records (hitscan, melee, projectiles, area blasts) with the game's hit rolls, bands, armour and damage types; the 1994 AI ported class by class; an optional enhanced AI; difficulty as the game does it. Fixed: the monster type is the second u16 of `MON` (half the creatures were wrong before), and hit points come from the placement. |
| Preview 3.3: options (delivered) | Automap markers option (none, pickups, pickups and creatures) and a settings file. |
| Preview 3.2: timing and automap (delivered) | Animation timing from the game's own animation player (table above): light-coded death frames run fast with a light flash, corpses stay only when the last tic is 0 (others fade out over 0.16 s), the true hit reaction (slot 3) replaces the ready move, and placeholder creature definitions borrow full sequences from other levels. Walk cycles are driven by distance (a cycle covers 0.8 × the creature's height) at the `stats[0]` speed (scaled down for the showcase); frames hold their pose and morph only in the last half of each frame. The view dissolve is gone (views switch outright, with hysteresis), as is the coarse occlusion test that hid creatures near corners. Barrel objects explode with `barreld1–3` at 70 ms per frame and leave nothing. New automap: heading-up minimap and north-up full map; floor art shrunk to 16×16 per cell and shaded by light and height; merged wall runs (edges that block at walking height, plus the floor boundary) and ledges (steps of 24+ units); doors, creatures, pickups, player arrow and view cone; fog of war from 360 grid rays (14 cells) that stop at blocking edges, closed doors and higher ground, sampled through a linear-filtered 64×64 mask. |
| Preview 3: faithful sprites (delivered) | **Depth sprites**: each original rotation view keeps its exact pixels, placed in 3D using the visual hull's front-surface depth (gaps filled from the nearest known depth, median and box blurred). Seen from one of the 8 original angles, a monster is pixel-identical to its sprite. In between, the view nearest the camera is shown with parallax, and when the nearest view changes it dissolves into the next over 0.12 s, with hysteresis so the view doesn't flicker back and forth. Walk, pain, death and effect frames morph pixel-by-pixel (nearest partner by position and colour). The original pain and death animations are used, and corpses stay. Items and props are upright billboards, as in 1994. The game's eight weapons are available, each with its own projectile and impact sprites. Explosions are built from `rcktexp`/`grnaded`, `expbit`/`expchk` and the smoke decals. |
| M2: renderer | GPU renderer for walls, flats, heights and slopes, cubes and sprites. True widescreen with Hor+ FOV, any resolution, uncapped frame rate with interpolation, 8-bit palette lighting emulated in a shader (so it stays faithful). |
| M3: gameplay | Movement and collision, doors and switches, triggers and events, items and weapons, monster AI, HUD and automap. Validated against DOSBox traces. |
| M4: modern layer | Mouselook plus WASD (with the classic free-reticle scheme kept as an option), rebinding, controller support, CD audio from the BIN, upscaled videos, save slots, settings UI. |
| M5: remaster extras | Optional high-res texture packs (made from the player's own files), CRT and sharp filters, achievements-style stats. |

The main reverse-engineering tool is Ghidra with an LE loader on
`CYCLONE.EXE`. Watcom register calling conventions and the strings above
give anchor points.
