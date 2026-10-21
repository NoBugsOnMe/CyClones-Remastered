// CyClones: Remastered — the native engine (Phase 2), version 1.0.
// Reads levels and art straight from the player's own CyClones CD image and renders them
// with a modern OpenGL renderer: any resolution, any aspect ratio (Hor+), uncapped frame rate.
// Monsters, items and effects are the original 1994 sprites (monsters given depth so they turn
// smoothly), with the original animation timing. You walk the levels with the game's own body,
// weapons, armour and pickups, and the monsters fight back: the 1994 AI ported from
// CYCLONE.EXE, or an enhanced AI (Esc menu), on three difficulty settings. The game's own sound
// effects (its SOUND folder) play as the game placed and mixed them, and each mission's music
// streams from the disc image's audio tracks. Games save to six slots and load from them on the
// game's own LOAD and SAVE screens (picture, level, date and description).
//
// Controls (rebindable, Esc > Configure): mouse look, WASD move, Shift run, Space jump, left click
//           fire, right click / Enter use (doors and items), wheel weapons, Q bio-kit, E mech-kit,
//           Tab automap, I inventory (a mouse cursor: click to select, drag onto a target to use;
//           1-8, Backspace drop), B detonate.
// Fixed:    1-8 weapons, 0 glove, G walk/fly, F show the whole map, +/- (or the wheel on the full
//           map) map zoom, K map markers (none / pickups / pickups and creatures),
//           X explosion at the crosshair, N smooth/classic animation, M monsters wander/hold,
//           PgUp/PgDn level, L lighting, R render scale, V vsync,
//           F11 or Alt+Enter fullscreen, F12 screenshot, F1 help, Esc menu.

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "audio/audio.h"
#include "data/arj.h"
#include "data/bol.h"
#include "data/disc.h"
#include "data/entities.h"
#include "game/world.h"
#include "input.h"
#include "data/level.h"
#include "game/cutscene.h"
#include "game/videoplayer.h"
#include "render/automap.h"
#include "render/firstperson.h"
#include "render/hud.h"
#include "render/gl.h"
#include "render/renderer.h"

namespace {

// The release number: 0.x until the whole game plays through (the "previews" 1-5.2 were 0.1-0.5.2).
const char* const kVersion = "1.0";
const char* const kVersionTag = "VERSION 1.0";   // as the options screen's frame shows it

struct Options {
    std::string disc, hd, level, shot, pos, fire, boom, focus, map, tour, keys, start, use, hurt;
    std::string view;  // --view "frame:yaw,pitch;...": testing, the view turned (degrees) at those frames
    std::string warp;  // --warp "frame:x,z[,height];...": testing, the player put in that cell at those frames
    std::string inv;   // --inv "frame:what;...": testing, inventory presses (i open/close, 1-8 a slot,
                       // + / - move, s select, d drop, b detonate, u use)
    bool hittest = false;
    int burst = 1, every = 10;
    bool hold = false;
    bool fullscreen = false;
    int display = -1;
    int vsync = 1;
    int width = 1280, height = 800;
    int winX = INT32_MIN, winY = INT32_MIN;   // the window's place last time (settings.ini)
    int frames = 3;
    int weapon = -1;   // --weapon n: testing, select a weapon after spawning
    int kits = -1;     // --kits n: testing, carry n bio-kits and n mech-kits
    bool fast = false; // --fast: captures skip drawing the frames they don't save
    std::string wav;   // --wav path: captures also write the sound they make (16-bit stereo WAV)
    int intro = -1;    // --intro / --nointro: play the opening videos or not (default: the setting; captures: no)
    int saveTo = -1;   // --save n: testing, save to slot n as the capture ends
    int loadFrom = -1; // --load n: testing, load slot n at the start
    std::string videoPack;   // --video-pack dir: the Enhanced cut-scenes (default: "HD Video" beside the program)
    int extras = -1;         // --extras n: testing, open the EXTRAS player on playlist n
    std::string extrasDo;    // --extras-do "frame:what;...": testing, player keys (p pause, s<sec> seek, f/b frame, e enhanced, m mouse x,y)
};

std::string baseDir() {
    const char* b = SDL_GetBasePath();
    return b ? std::string(b) : std::string("./");
}

bool exists(const std::string& p) {
    SDL_PathInfo info;
    return SDL_GetPathInfo(p.c_str(), &info) && info.type == SDL_PATHTYPE_FILE;
}

std::string firstExisting(const std::vector<std::string>& candidates) {
    for (const auto& c : candidates)
        if (exists(c)) return c;
    return std::string();
}

void fail(const std::string& msg) {
    SDL_Log("%s", msg.c_str());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "CyClones: Remastered", msg.c_str(), nullptr);
}

struct App {
    Options opt;
    DiscImage disc;
    GameData data;
    // Sound: the game's own effects and the disc's music, at the game's default levels
    // (KEYMAP.BIN as shipped: music 99, sound 70).
    Audio audio;
    int soundVolume = 70, musicVolume = 99;
    // The opening, as CYCLONES.EXE runs it before the first mission: "SMKPLAY I" (the Raven logo
    // and the introduction) and "SMKPLAY o" (the first briefing); a key skips the rest of each.
    Cutscenes cutscenes;
    bool introVideos = true;
    // The Enhanced cut-scenes (an addition): the HD pack beside the program, used unless the
    // setting says the game's own.
    HdPack hdPack;
    bool enhancedVideos = true;
    bool subtitles = true;      // the briefings' subtitles (an addition; the pack's subtitles\*.srt)
    int brightness = 0;         // BRIGHTNESS (an addition): 0 the game's, to 10 (the 3D view's gamma 1 + n / 10)
    void applyVideoSetting() { cutscenes.setEnhanced(&hdPack, enhancedVideos && hdPack.loaded()); }
    // EXTRAS: the cut-scene player.
    VideoPlayer player;
    int playerList = -1;
    int soundsFound = 0, tracksFound = 0;
    std::vector<int16_t> wavOut;  // --wav: what a capture played
    double wavClock = 0;
    Renderer renderer;
    RenderSettings rs;
    Camera cam;
    std::vector<std::string> levels;
    int levelIndex = 0;
    Level level;
    LevelEntities ents;
    World world;
    Scene scene;
    Automap automap;
    Hud hud;
    FirstPerson firstPerson;   // the weapon in your hands
    // Options menu (Esc): the game's options screen.
    bool menuOpen = false;
    // The inventory's cursor: with the inventory open the mouse moves a cursor instead of the view.
    bool invCursor = false;
    float curX = 0, curY = 0;        // pixels
    int dragSlot = -1;               // a slot pressed on (dragged once the mouse moves)
    float dragFromX = 0, dragFromY = 0;
    bool dragging = false;
    bool dropMode = false;           // DROP pressed: the next slot chosen is dropped
    float demoIcon[4] = {0, 0, 0, 0};   // where the Demo Pack Icon was drawn (x, y, w, h; w 0: not shown)
    float jetsIcon[4] = {0, 0, 0, 0};   // the Jump Jet Icon, the same way
    bool finished = false;   // the game is won (action 25): the last videos played, only the menu is left
    int menuBtn = 0;              // the button the keys are on
    int menuPanel = -1;           // the button whose screen is open; -1: the mission text
    bool panelFocus = false;      // the keys work in that screen rather than on the buttons
    int panelSel = 0;             // its row
    int menuYes = 0;              // 0: YES lit, 1: NO
    int pendingDifficulty = 1;
    // LOAD and SAVE: six slots, as the game has (its SAVE0-5.DAT), one on show at a time and
    // turned with the arrows under the screen. Each keeps a picture of the view when it was
    // saved, the level and difficulty, the date and a description typed in when saving.
    static constexpr int kSlots = 7;
    static constexpr int kQuick = 6;   // the quick save's slot (F5 / F9; an addition), "Q" on the LOAD and SAVE screens
    struct SlotInfo {
        bool used = false;              // there is a save in this slot
        bool readable = false;          // ...and this engine can load it
        std::string desc, where, when;  // "NEAR THE LIFT", "LEVEL 1 DIFFICULTY: MEDIUM", "MON OCT 17 22:51:00 1994"
        std::vector<uint8_t> thumb;     // Hud::kThumbW x kThumbH RGB
    };
    int saveSlot = 0;                   // the slot on show
    int lastSlot = -1;                  // the slot last saved to or loaded from (settings.ini: last_slot)
    int slotRead = -1;                  // the slot slotInfo describes (and whose picture the HUD has)
    SlotInfo slotInfo;
    enum { SlotAsk, SlotTyping, SlotSaving, SlotLoading };
    int slotStage = SlotAsk;
    std::string saveDesc;               // being typed
    int64_t saveTime = 0;               // when the save being made was begun
    std::vector<uint8_t> newThumb;      // the view just before saving
    bool grabThumb = false;             // take newThumb from the next frame drawn
    std::string slotNote;               // a red note in place of the question ("LOAD GAME FAILED")
    struct MenuHit { float x, y, w, h; int kind, index; };
    enum { HitButton, HitRow, HitYes, HitNo, HitSlider, HitPage, HitSlot, HitSlotPick };
    int settingsPage = 0;         // SETTINGS: 0 sound and picture, 1 the game
    int sliderDrag = -1;          // the slider the mouse is dragging
    std::vector<MenuHit> menuHits;   // where the last drawn menu's buttons and rows are (320x200)
    float menuCanvas[4] = {0, 0, 1, 1};   // where the 320x200 screen is in the window
    int bindWait = -1;            // configure: the action waiting for a key or button
    bool bindWaitRelease = false; // ...once the mouse button that chose it is released
    // Controls
    Bindings bindings;
    float mouseSens = 1.0f;
    bool invertMouse = false;
    bool pulse[kActionCount] = {};   // a wheel turn bound to a held action counts for one frame
    int ignoreButton = 0;            // the click that captured the mouse doesn't also fire
    float menuMouseX = -1, menuMouseY = -1;   // in the menu's 320x200 pixels
    bool quitRequested = false;
    Automap::Mode mapMode = Automap::Mini;
    float mapZoom[3] = {1, 1, 1};
    bool mapReveal = false;
    int mapMarkers = 0;   // 0 none, 1 pickups, 2 pickups and creatures (the original automap showed enemies)
    bool saveSettingsOnChange = true;
    float clock = 0;
    bool walk = true;     // walk with collision (G: free flight through everything)
    SDL_Window* window = nullptr;
    SDL_GLContext gl = nullptr;
    int renderMode = 0;  // 0 native, 1 half, 2 retro 320x200

    bool loadData() {
        std::string base = baseDir();
        std::string discPath = !opt.disc.empty() ? opt.disc
            : firstExisting({base + "CYCLONES.cue", base + "CYCLONES.bin", base + "../CYCLONES.cue", base + "../CYCLONES.bin",
                             base + "CYCLONES.iso", base + "../CYCLONES.iso"});
        if (discPath.empty()) { fail("Couldn't find your CyClones disc image.\nPut this folder next to CYCLONES.bin, or start with --disc <path>."); return false; }
        std::string err;
        if (!disc.open(discPath, err)) { fail(err); return false; }

        // CYCLONE.HD and the sound effects are what the 1994 installer copied to the hard disk out
        // of its archive on the disc (DATA.A00); they are read from there, so no installed copy of
        // the game is needed. --hd <path> takes an installed CYCLONE.HD instead (its SOUND folder
        // beside it), as does one found where CyClones Remastered's Classic launcher installs it,
        // if the archive can't be read.
        std::vector<uint8_t> hd;
        std::string hdPath = opt.hd;
        ArjArchive installer;
        bool fromDisc = false;
        if (hdPath.empty()) {
            std::vector<uint8_t> a;
            std::string aerr = "DATA.A00 isn't on the disc image";
            fromDisc = disc.readFile("DATA.A00", a) && installer.open(std::move(a), aerr) && installer.extract("CYCLONE.HD", hd, aerr);
            if (!fromDisc) {
                SDL_Log("The installer's archive: %s", aerr.c_str());
                hdPath = firstExisting({base + "CYCLONE.HD", base + "../CyClones Remastered/game/drive_c/CYCLONE/CYCLONE.HD",
                                        base + "../game/drive_c/CYCLONE/CYCLONE.HD"});
                if (hdPath.empty()) { fail("Couldn't read the game's data from your disc image (DATA.A00: " + aerr + ")."); return false; }
            }
        }
        if (!fromDisc) {
            size_t len = 0;
            void* hdBytes = SDL_LoadFile(hdPath.c_str(), &len);
            if (!hdBytes) { fail("Cannot read " + hdPath); return false; }
            hd.assign((uint8_t*)hdBytes, (uint8_t*)hdBytes + len);
            SDL_free(hdBytes);
        }
        if (!data.hd.load(std::move(hd), err)) { fail("CYCLONE.HD: " + err); return false; }

        // Sound: the effects (SOUND\<id>.RAW, from the archive or beside an installed CYCLONE.HD),
        // and the music tracks of the disc image.
        {
            std::string snd;
            if (fromDisc) {
                std::vector<std::vector<uint8_t>> bank(256);
                for (const std::string& n : installer.names()) {
                    if (n.size() < 11 || n.compare(0, 6, "SOUND/") != 0 || n.compare(n.size() - 4, 4, ".RAW") != 0) continue;
                    int id = std::atoi(n.c_str() + 6);
                    std::string serr;
                    if (id > 0 && id < 256 && !installer.extract(n, bank[id], serr)) SDL_Log("%s", serr.c_str());
                }
                soundsFound = audio.setSoundBank(std::move(bank));
                snd = "the disc's DATA.A00";
            } else {
                std::string dir = hdPath.substr(0, hdPath.find_last_of("/\\") + 1);
                SDL_PathInfo info;
                snd = dir + "SOUND";
                if (!SDL_GetPathInfo(snd.c_str(), &info) || info.type != SDL_PATHTYPE_DIRECTORY) snd = dir + "sound";
                soundsFound = audio.setSoundDir(snd);
            }
            tracksFound = audio.setDisc(discPath);
            SDL_Log("Sound: %d effects in %s, %d music tracks", soundsFound, snd.c_str(), tracksFound);
            applyVolumes();
        }

        {
            std::string pack = !opt.videoPack.empty() ? opt.videoPack : base + "HD Video/";
            if (hdPack.open(pack)) SDL_Log("Enhanced cut-scenes: %s", pack.c_str());
            applyVideoSetting();
        }

        std::vector<uint8_t> cd;
        if (!disc.readFile("GAME/CYCLONE.CD", cd)) { fail("GAME/CYCLONE.CD not found on the disc image."); return false; }
        if (!data.cd.load(std::move(cd), err)) { fail("CYCLONE.CD: " + err); return false; }

        int pal = data.hd.find("play_pal"), fade = data.hd.find("fadetob"), sky = data.hd.find("sky1");
        if (pal < 0 || fade < 0) { fail("Palette or fade table missing from CYCLONE.HD"); return false; }
        data.palette.assign(data.hd.data(pal), data.hd.data(pal) + 768);
        data.fade.assign(data.hd.data(fade), data.hd.data(fade) + 127 * 256);
        if (sky >= 0 && data.hd.size(sky) >= 320 * 160) data.sky = data.hd.data(sky);
        if (int sky2 = data.hd.find("sky2"); sky2 >= 0 && data.hd.size(sky2) >= 320 * 160) data.sky2 = data.hd.data(sky2);
        data.startWalls = data.cd.find("startwalls");
        data.startFlats = data.cd.find("startflats");
        if (data.startWalls < 0 || data.startFlats < 0) { fail("Texture sections missing from CYCLONE.CD"); return false; }

        for (const auto& n : disc.list("GAME"))
            if (n.size() > 4 && n.substr(n.size() - 4) == ".MAP") levels.push_back(n.substr(0, n.size() - 4));
        if (levels.empty()) { fail("No levels found on the disc image."); return false; }
        // The game starts at mission 1, BASE (0xd4290 = 1); the tutorial is on the options screen.
        auto it = std::find(levels.begin(), levels.end(), opt.level.empty() ? std::string("BASE") : opt.level);
        levelIndex = it == levels.end() ? 0 : (int)(it - levels.begin());
        return true;
    }

    float floorAt(float wx, float wz) const {
        int x = (int)std::floor(wx / kCellSize), y = (int)std::floor(wz / kCellSize);
        if (x < 0 || y < 0 || x >= kGrid || y >= kGrid) return 0;
        return (float)level.floor.height[y * kGrid + x];
    }

    // Every level's creature definitions, read once (used to complete placeholder entries).
    std::vector<MonsterType> library;
    bool libraryRead = false;
    const std::vector<MonsterType>& creatureLibrary() {
        if (libraryRead) return library;
        libraryRead = true;
        for (const auto& name : levels) {
            std::vector<uint8_t> glb;
            std::string err;
            LevelEntities e;
            if (disc.readFile("GAME/" + name + ".GLB", glb) && e.parseGlb(glb, err))
                library.insert(library.end(), e.types.begin(), e.types.end());
        }
        return library;
    }

    // The music for a level: the game's track table (0x74a06 in CYCLONE.EXE, by mission number;
    // FUN_0004c93b plays it and starts it again when it ends). The tutorial is mission 0, which
    // has no track, so it plays in silence; the missions without a track of their own use 3.
    static int musicTrack(const std::string& name) {
        static const struct { const char* name; int track; } kTracks[] = {
            {"BASE", 3},     {"LAB2", 26},    {"ALIEN1", 4},   {"BIOLAB", 5},  {"BIOSTOR2", 6}, {"FAILSAF2", 7},
            {"MUSEUM2", 8},  {"MAINTEN2", 9}, {"TREAT2", 3},   {"SUBWAY", 10}, {"SDILAB", 11},  {"SDI1", 12},
            {"SDI2", 13},    {"HIVE1", 14},   {"HIVE2", 15},   {"HIVE3", 16},  {"HIVE4", 3},    {"HIVE5", 3},
            {"MOTHER1", 17}, {"MOTHER2", 3},  {"MOTHER3", 18}, {"CORE", 27},   {"CLONE", 28},   {"HEAVY", 3}};
        for (const auto& t : kTracks)
            if (name == t.name) return t.track;
        return 0;
    }

    void applyVolumes() { audio.setVolumes(soundVolume, musicVolume); }

    // The sounds the world started this frame, heard from the camera.
    void playSounds(float dt) {
        static const bool logSounds = SDL_getenv("CYCLONES_SOUNDLOG") != nullptr;   // testing
        for (const auto& s : world.takeSounds()) {
            if (s.at) audio.playAt(s.id, s.x - cam.x, s.z - cam.z, cam.yaw, s.range);
            else audio.play(s.id, s.volume, s.pan);
            if (logSounds)
                SDL_Log("dbg sound %.2fs %d %s %.0f,%.0f (%.0f away, fades over %.0f)", world.time(), s.id, s.at ? "at" : "flat", s.x, s.z,
                        s.at ? std::hypot(s.x - cam.x, s.z - cam.z) : 0.0, s.at ? s.range : 0.0);
        }
        if (!opt.wav.empty()) {   // captures: the mix, frame by frame in game time
            wavClock += dt * Audio::kRate;
            int n = (int)wavClock;
            wavClock -= n;
            std::vector<float> buf((size_t)n * 2);
            audio.render(buf.data(), n);
            for (float v : buf) wavOut.push_back((int16_t)std::lround(std::clamp(v, -1.0f, 1.0f) * 32767));
        }
    }
    void writeWav() {
        if (opt.wav.empty()) return;
        uint32_t data = (uint32_t)(wavOut.size() * 2);
        std::vector<uint8_t> f(44 + data);
        auto u32 = [&](size_t o, uint32_t v) { for (int k = 0; k < 4; ++k) f[o + k] = (uint8_t)(v >> (8 * k)); };
        auto u16 = [&](size_t o, uint16_t v) { f[o] = (uint8_t)v; f[o + 1] = (uint8_t)(v >> 8); };
        std::memcpy(&f[0], "RIFF", 4); u32(4, 36 + data); std::memcpy(&f[8], "WAVEfmt ", 8);
        u32(16, 16); u16(20, 1); u16(22, 2); u32(24, Audio::kRate); u32(28, Audio::kRate * 4); u16(32, 4); u16(34, 16);
        std::memcpy(&f[36], "data", 4); u32(40, data);
        std::memcpy(&f[44], wavOut.data(), data);
        SDL_SaveFile(opt.wav.c_str(), f.data(), f.size());
    }

    void loadLevel(int index) {
        finished = false;
        levelIndex = (index + (int)levels.size()) % (int)levels.size();
        std::vector<uint8_t> bytes;
        std::string err;
        level = Level();
        level.name = levels[levelIndex];
        if (!disc.readFile("GAME/" + level.name + ".MAP", bytes) || !level.parse(bytes, err)) {
            SDL_Log("Failed to load %s: %s", level.name.c_str(), err.c_str());
            return;
        }
        ents = LevelEntities();   // (world.load builds the level's geometry, once it knows what moves)
        if (!ents.parseMap(bytes, err)) SDL_Log("%s entities: %s", level.name.c_str(), err.c_str());
        std::vector<uint8_t> glb;
        if (disc.readFile("GAME/" + level.name + ".GLB", glb) && !ents.parseGlb(glb, err))
            SDL_Log("%s.GLB: %s", level.name.c_str(), err.c_str());
        if (int n = ents.fillPlaceholders(creatureLibrary()))
            SDL_Log("%s: %d creature definitions completed from other levels", level.name.c_str(), n);
        world.mission = missionNumber(level.name);
        world.setEditableLevel(&level);   // scripts throw switches and move walls in this copy
        world.load(level, ents, 1994u + (unsigned)levelIndex);
        audio.stopSounds();
        if (!cutscenes.active()) audio.music(musicTrack(level.name));   // a level (re)starts its track from the top
        MapLevel plan;
        world.mapLevel(plan);
        automap.buildLevel(plan, data);
        // Start where the game starts a level (its level table in CYCLONE.EXE, at 0x7499e); levels
        // without an entry start in the most open floor cell. Face down the longest clear line.
        auto isFloor = [&](int x, int y) {
            return x >= 0 && y >= 0 && x < kGrid && y < kGrid && (level.floor.tex[y * kGrid + x] & 0x3fff) != 0;
        };
        static const struct { const char* name; int x, z; } kStarts[] = {
            {"BASE", 46, 8},      {"LAB2", 31, 1},     {"ALIEN1", 31, 58},  {"BIOLAB", 27, 2},   {"BIOSTOR2", 16, 17},
            {"FAILSAF2", 12, 29}, {"MUSEUM2", 20, 48}, {"MAINTEN2", 38, 38}, {"SUBWAY", 62, 1},  {"SDILAB", 1, 31},
            {"SDI1", 57, 62},     {"SDI2", 62, 61},    {"HIVE1", 37, 3},    {"HIVE2", 15, 41},   {"HIVE3", 1, 1},
            {"MOTHER1", 32, 32},  {"MOTHER3", 31, 58}, {"CORE", 60, 35},    {"CLONE", 17, 17},   {"HEAVY", 1, 1}};
        int bestScore = -1, bx = 0, by = 0;
        for (const auto& s : kStarts)
            if (level.name == s.name && isFloor(s.x, s.z)) { bx = s.x; by = s.z; bestScore = 0; }
        for (int y = 0; y < level.height && bestScore < 0; ++y)
            for (int x = 0; x < level.width; ++x) {
                if (!isFloor(x, y)) continue;
                int score = 0;
                for (int dy = -3; dy <= 3; ++dy)
                    for (int dx = -3; dx <= 3; ++dx) score += isFloor(x + dx, y + dy);
                if (score > bestScore) { bestScore = score; bx = x; by = y; }
            }
        const int dirs[4][2] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};
        int bestDir = 0, bestRun = -1;
        for (int d = 0; d < 4; ++d) {
            int run = 1;
            while (run < 40 && isFloor(bx + dirs[d][0] * run, by + dirs[d][1] * run)) ++run;
            if (run > bestRun) { bestRun = run; bestDir = d; }
        }
        cam.x = (bx + 0.5f) * kCellSize;
        cam.z = (by + 0.5f) * kCellSize;
        cam.y = level.floor.height[by * kGrid + bx] + 48.0f;
        cam.yaw = bestDir * 1.5707963f;
        cam.pitch = 0;
        // --start cx,cz,yawDeg[,pitchDeg[,height]]: begin the level in another cell (testing); with a
        // height, on the surface under it (an upper floor).
        float startH = -1e9f;
        if (!opt.start.empty()) {
            float sx = 0, sz = 0, sy = 0, sp = 0;
            if (std::sscanf(opt.start.c_str(), "%f,%f,%f,%f,%f", &sx, &sz, &sy, &sp, &startH) >= 2) {
                cam.x = (sx + 0.5f) * kCellSize;
                cam.z = (sz + 0.5f) * kCellSize;
                cam.yaw = sy * 3.14159265f / 180.0f;
                cam.pitch = sp * 3.14159265f / 180.0f;
            }
        }
        world.spawnPlayer(cam.x, cam.z, cam.yaw);
        if (startH > -1e8f) {   // (the middle of the cell)
            world.warpPlayer((int)std::floor(cam.x / kCellSize), (int)std::floor(cam.z / kCellSize), startH);
            cam.x = world.player().x;
            cam.z = world.player().z;
        }
        firstPerson.reset();
        if (opt.weapon >= 0) world.selectWeapon(opt.weapon);
        if (opt.kits >= 0) world.giveKits(opt.kits, opt.kits);
        cam.y = world.player().viewY;
        SDL_Log("Loaded %s: %llu triangles", level.name.c_str(), (unsigned long long)renderer.triangleCount());
    }

    bool init() {
        if (!SDL_Init(SDL_INIT_VIDEO)) { fail(std::string("SDL init failed: ") + SDL_GetError()); return false; }
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "CyClones: Remastered");
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, opt.width);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, opt.height);
        SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_OPENGL_BOOLEAN, true);
        SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
        SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN, true);
        if (opt.display < 0 && opt.winX != INT32_MIN) {
            // Where it was, if that is still on a screen (a monitor unplugged since puts it back in
            // the middle): the title bar's left end on some display's usable area.
            SDL_Rect r{opt.winX, opt.winY, opt.width, opt.height}, b{};
            SDL_DisplayID d = SDL_GetDisplayForRect(&r);
            if (d && SDL_GetDisplayUsableBounds(d, &b) && opt.winX >= b.x - opt.width / 2 && opt.winX + 120 <= b.x + b.w &&
                opt.winY >= b.y && opt.winY + 40 <= b.y + b.h) {
                SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, opt.winX);
                SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, opt.winY);
            }
        }
        if (opt.display >= 0) {
            int count = 0;
            SDL_DisplayID* ids = SDL_GetDisplays(&count);
            if (ids && opt.display < count) {
                SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(ids[opt.display]));
                SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(ids[opt.display]));
            }
            SDL_free(ids);
        }
        window = SDL_CreateWindowWithProperties(props);
        SDL_DestroyProperties(props);
        if (!window) { fail(std::string("Window creation failed: ") + SDL_GetError()); return false; }
        gl = SDL_GL_CreateContext(window);
        if (!gl) { fail(std::string("OpenGL 3.3 is required: ") + SDL_GetError()); return false; }
        if (const char* missing = glLoadFunctions()) { fail(std::string("Missing OpenGL function ") + missing); return false; }
        SDL_GL_SetSwapInterval(opt.vsync);
        SDL_Log("OpenGL: %s / %s", (const char*)glGetString(GL_RENDERER), (const char*)glGetString(GL_VERSION));
        if (opt.fullscreen) SDL_SetWindowFullscreen(window, true);
        audio.init(opt.shot.empty());   // captures make no sound (but can write it: --wav)

        if (!loadData()) return false;
        std::string err;
        if (!renderer.init(data, err)) { fail("Renderer: " + err); return false; }
        if (!automap.init(err)) { fail("Automap: " + err); return false; }
        if (!hud.init(data, err)) { fail("HUD: " + err); return false; }
        world.init(&data, &renderer);
        if (opt.hold) world.monstersMove = false;
        if (opt.intro > 0 || (opt.intro < 0 && introVideos && opt.shot.empty()))
            cutscenes.start({{"raven", "int1", "int2", "int3"}, {"1a2a", "1a2b", "1a3"}}, &disc, &audio);
        // As the game does ("before play begins, the Option Screen"), the options screen comes up
        // after the opening (or at once without it), unless the command line asked for a level or
        // a save.
        startMenu = (opt.shot.empty() || SDL_getenv("CYCLONES_STARTMENU")) && opt.level.empty() && opt.loadFrom < 0 && opt.extras < 0;
        if (opt.extras >= 0 && opt.extras < kExtras) {   // testing: straight into the EXTRAS player
            openMenu(true);
            openPanel(BSettings);
            turnSettingsPage(2);
            panelSel = SExtra0 + opt.extras;
            openPlayer(opt.extras);
        }
        // Testing: CYCLONES_BEFORE=BASE,LAB2 plays through those levels' loading first, as a game
        // played from the start does (what one level builds stays for the next).
        if (const char* before = SDL_getenv("CYCLONES_BEFORE")) {
            int target = levelIndex;
            std::string list = before;
            for (size_t p = 0; p < list.size();) {
                size_t q = list.find(',', p);
                std::string name = list.substr(p, q == std::string::npos ? std::string::npos : q - p);
                auto it = std::find(levels.begin(), levels.end(), name);
                if (it != levels.end()) loadLevel((int)(it - levels.begin()));
                if (q == std::string::npos) break;
                p = q + 1;
            }
            levelIndex = target;
        }
        loadLevel(levelIndex);
        if (opt.loadFrom >= 0 && !loadSlot(opt.loadFrom)) SDL_Log("--load %d: no game to load", opt.loadFrom);
        if (menuOpen && (menuPanel == BLoad || menuPanel == BSave)) saveSlot = defaultSlot(menuPanel);   // --panel 2 / 3
        if (!opt.pos.empty()) {
            float v[5] = {cam.x, cam.y, cam.z, 0, 0};
            std::sscanf(opt.pos.c_str(), "%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4]);
            cam.x = v[0]; cam.y = v[1]; cam.z = v[2];
            cam.yaw = v[3] * 3.14159265f / 180.0f;
            cam.pitch = v[4] * 3.14159265f / 180.0f;
        }
        focusCamera(true);
        if (opt.map == "mini") mapMode = Automap::Mini;
        else if (opt.map == "full") mapMode = Automap::Full;
        else if (opt.map == "off" || (!opt.shot.empty() && opt.map.empty())) mapMode = Automap::Off;  // captures: no map unless asked
        if (opt.shot.empty()) SDL_SetWindowRelativeMouseMode(window, true);
        if (startMenu && !cutscenes.active()) showStartMenu();
        return true;
    }
    bool startMenu = false;   // the options screen is still to come up at the start
    void showStartMenu() {
        startMenu = false;
        if (menuOpen) return;
        openMenu(true);
        // LOAD lit if there is a game to go on with, NEW GAME if not (RETURN starts mission 1 too).
        bool any = false;
        for (int k = 0; k < kSlots; ++k) {
            SDL_PathInfo pi;
            any |= SDL_GetPathInfo(slotPath(k).c_str(), &pi) && pi.type == SDL_PATHTYPE_FILE;
        }
        menuBtn = any ? BLoad : BNewGame;
    }

    void applyRenderMode() {
        rs.renderScale = renderMode == 0 ? 100 : renderMode == 1 ? 50 : -1;
    }

    void screenshot() {
        int w, h;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        std::string dir = baseDir() + "screenshots/";
        SDL_CreateDirectory(dir.c_str());
        char name[64];
        SDL_snprintf(name, sizeof(name), "%s_%llu.bmp", level.name.c_str(), (unsigned long long)SDL_GetTicks());
        renderer.screenshot(w, h, dir + name);
        SDL_Log("Saved %s%s", dir.c_str(), name);
    }

    // --focus monster[,distance,angleDeg,heightOffset[,follow[,orbitDegPerSec]]]: frame a monster for captures.
    float focusClock = 0;
    void focusCamera(bool first) {
        if (opt.focus.empty()) return;
        float f[6] = {0, 120, 0, 0, 0, 0}, info[5];
        std::string spec = opt.focus;
        char kind = 'm';
        if (spec.size() > 2 && spec[1] == ':') { kind = spec[0]; spec = spec.substr(2); }
        std::sscanf(spec.c_str(), "%f,%f,%f,%f,%f,%f", &f[0], &f[1], &f[2], &f[3], &f[4], &f[5]);
        if (!first && f[4] == 0) return;
        if (!world.entityInfo(kind, (int)f[0], info)) return;
        static float startYaw = 0;
        if (first) startYaw = info[4];
        if (f[5] != 0) info[4] = startYaw;                 // orbiting: angles stay in the world frame
        float a = info[4] + (f[2] + f[5] * focusClock) * 3.14159265f / 180.0f;   // angle around the monster, 0 = in front
        float target[3] = {info[0], info[1] + info[3] * 0.6f + f[3], info[2]};
        float dir[3] = {std::sin(a), 0, -std::cos(a)};
        float t, n[3], dist = f[1];
        if (world.raycast(target, dir, dist, t, n)) dist = std::max(20.0f, t - 12);
        cam.x = target[0] + dir[0] * dist;
        cam.z = target[2] + dir[2] * dist;
        cam.y = target[1];
        float dx = info[0] - cam.x, dz = info[2] - cam.z, dy = info[1] + info[3] * 0.5f - cam.y;
        cam.yaw = std::atan2(dx, -dz);
        cam.pitch = std::atan2(dy, std::sqrt(dx * dx + dz * dz));
    }

    void drawCrosshair(int w, int h) {
        if (world.inventoryOpen()) return;   // the inventory's cursor instead (drawInventoryCursor)
        if (!SDL_GetWindowRelativeMouseMode(window) && opt.shot.empty()) return;
        int s = std::max(1, h / 400), len = std::max(4, h / 90), cx = w / 2, cy = h / 2, gap = len / 2;
        // The game colours its cursor by what is under it (FUN_000358a1, palette entries 0xbc,
        // 100 and 0x90): green on an item, orange on a creature, pink on a door, hidden doors
        // too. Otherwise it stays white here (the game's own was dark blue).
        static const float cols[4][3] = {{0.85f, 0.85f, 0.85f}, {0, 227 / 255.0f, 0}, {243 / 255.0f, 121 / 255.0f, 0}, {215 / 255.0f, 36 / 255.0f, 247 / 255.0f}};
        int pick = std::clamp(world.cursorPick(cam), 0, 3);
        glEnable(GL_SCISSOR_TEST);
        auto rect = [&](int x, int y, int rw, int rh, const float* c) {
            glScissor(x, y, rw, rh);
            glClearColor(c[0], c[1], c[2], 1);
            glClear(GL_COLOR_BUFFER_BIT);
        };
        static const float black[3] = {0, 0, 0};
        for (int pass = 0; pass < 2; ++pass) {
            int o = pass == 0 ? s : 0;
            const float* c = pass == 0 ? black : cols[pick];
            rect(cx - gap - len - o, cy - s / 2 - o, len + 2 * o, s + 2 * o, c);
            rect(cx + gap - o, cy - s / 2 - o, len + 2 * o, s + 2 * o, c);
            rect(cx - s / 2 - o, cy + gap - o, s + 2 * o, len + 2 * o, c);
            rect(cx - s / 2 - o, cy - gap - len - o, s + 2 * o, len + 2 * o, c);
        }
        glDisable(GL_SCISSOR_TEST);
    }

    // --tour x,z;x,z;... (cells): walk the camera along waypoints at 200 units/s, for captures.
    void tourCamera() {
        if (opt.tour.empty()) return;
        std::vector<float> pts;
        for (size_t p = 0; p < opt.tour.size();) {
            size_t q = opt.tour.find(';', p);
            float x = 0, z = 0;
            std::sscanf(opt.tour.substr(p, q - p).c_str(), "%f,%f", &x, &z);
            pts.push_back((x + 0.5f) * kCellSize);
            pts.push_back((z + 0.5f) * kCellSize);
            if (q == std::string::npos) break;
            p = q + 1;
        }
        float d = focusClock * 200.0f;
        for (size_t i = 0; i + 3 < pts.size(); i += 2) {
            float dx = pts[i + 2] - pts[i], dz = pts[i + 3] - pts[i + 1], l = std::sqrt(dx * dx + dz * dz);
            if (d <= l || i + 4 >= pts.size()) {
                float t = l > 0 ? std::min(1.0f, d / l) : 1.0f;
                cam.x = pts[i] + dx * t;
                cam.z = pts[i + 1] + dz * t;
                cam.y = floorAt(cam.x, cam.z) + 48.0f;
                if (l > 0) cam.yaw = std::atan2(dx, -dz);
                cam.pitch = 0;
                return;
            }
            d -= l;
        }
    }

    // Settings live in settings.ini next to the program, so they survive restarts.
    std::string settingsPath() const { return baseDir() + "settings.ini"; }

    void loadSettings() {
        size_t size = 0;
        void* text = SDL_LoadFile(settingsPath().c_str(), &size);
        if (!text) return;
        std::string s((const char*)text, size);
        SDL_free(text);
        bool loaded[kActionCount] = {};
        size_t p = 0;
        while (p < s.size()) {
            size_t e = s.find('\n', p);
            std::string line = s.substr(p, e == std::string::npos ? std::string::npos : e - p);
            p = e == std::string::npos ? s.size() : e + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            size_t eq = line.find('=');
            if (line.empty() || line[0] == '#' || line[0] == ';' || eq == std::string::npos) continue;
            std::string key = line.substr(0, eq), val = line.substr(eq + 1);
            while (!key.empty() && key.back() == ' ') key.pop_back();
            while (!val.empty() && val.front() == ' ') val.erase(val.begin());
            int n = std::atoi(val.c_str());
            if (key == "map") mapMode = val == "full" ? Automap::Full : val == "off" ? Automap::Off : Automap::Mini;
            else if (key == "map_markers") mapMarkers = val == "creatures" ? 2 : val == "pickups" ? 1 : 0;
            else if (key == "minimap_zoom") mapZoom[Automap::Mini] = clampZoom(Automap::Mini, (float)std::atof(val.c_str()));
            else if (key == "map_zoom") mapZoom[Automap::Full] = clampZoom(Automap::Full, (float)std::atof(val.c_str()));
            else if (key == "look") world.smooth = val != "classic";
            else if (key == "lighting") rs.lighting = n != 0;
            // ("fov" from 4.1 and earlier is ignored: its default, 75, was narrower than the game's view)
            else if (key == "view_fov") rs.hFovDeg4x3 = std::clamp((float)std::atof(val.c_str()), 50.0f, 120.0f);
            else if (key == "tall_pixels") rs.tallPixels = val != "0";
            else if (key == "vsync") opt.vsync = n != 0;
            else if (key == "fullscreen") opt.fullscreen = n != 0;
            else if (key == "opening") introVideos = val != "skip";
            else if (key == "cutscenes") { enhancedVideos = val != "original"; applyVideoSetting(); }
            else if (key == "subtitles") subtitles = n != 0;
            else if (key == "brightness") brightness = std::clamp(n, 0, 10);
            else if (key == "music_volume") musicVolume = std::clamp(n, 0, 100);
            else if (key == "sound_volume") soundVolume = std::clamp(n, 0, 100);
            else if (key == "render") renderMode = val == "retro" ? 2 : val == "half" ? 1 : 0;
            else if (key == "difficulty") world.difficulty = val == "easy" ? 0 : val == "normal" ? 1 : 2;
            else if (key == "monster_ai") world.enhancedAI = val == "enhanced";
            else if (key == "monsters_attack") world.monstersAttack = n != 0;
            else if (key == "all_weapons") world.startWithAllWeapons = n != 0;
            else if (key == "pickups") world.autoPickup = val != "use";
            else if (key == "doors") world.bumpDoors = val == "walk";
            else if (key == "mouse_sensitivity") mouseSens = std::clamp((float)std::atof(val.c_str()), 0.1f, 5.0f);
            else if (key == "invert_mouse") invertMouse = n != 0;
            else if (key == "last_slot") lastSlot = std::clamp(n, -1, kSlots - 1);
            else if (key == "window") {   // the window's size and place when last left (not full screen)
                int ww = 0, wh = 0, wx = 0, wy = 0;
                if (std::sscanf(val.c_str(), "%d %d %d %d", &ww, &wh, &wx, &wy) == 4 && ww >= 320 && wh >= 200) {
                    opt.width = ww;
                    opt.height = wh;
                    opt.winX = wx;
                    opt.winY = wy;
                }
            }
            else if (key.rfind("bind_", 0) == 0) {
                for (int a = 0; a < kActionCount; ++a)
                    if (key.substr(5) == Bindings::key((Action)a)) { bindings.load((Action)a, val); loaded[a] = true; }
            }
        }
        // Actions newer than the file keep their default inputs unless the file gave them to another.
        for (int a = 0; a < kActionCount; ++a) {
            if (loaded[a]) continue;
            for (int& c : bindings.in[a])
                for (int b = 0; b < kActionCount && c; ++b)
                    if (b != a && loaded[b] && bindings.has((Action)b, c)) c = 0;
            if (!bindings.in[a][0]) std::swap(bindings.in[a][0], bindings.in[a][1]);
        }
    }

    void saveSettings() {
        if (!saveSettingsOnChange) return;
        const char* maps[] = {"off", "mini", "full"};
        const char* markers[] = {"none", "pickups", "creatures"};
        const char* renders[] = {"native", "half", "retro"};
        const char* diffs[] = {"easy", "normal", "hard"};
        char buf[4096];
        SDL_snprintf(buf, sizeof(buf),
                     "# CyClones Engine settings (saved automatically)\n"
                     "# music_volume, sound_volume: 0-100 (the game's own defaults: 99 and 70)\n"
                     "music_volume=%d\n"
                     "sound_volume=%d\n"
                     "fullscreen=%d\n"
                     "# opening: videos (the game's opening videos at start) or skip\n"
                     "opening=%s\n"
                     "# cutscenes: enhanced (the HD Video pack's pictures and speech, when present) or original\n"
                     "cutscenes=%s\n"
                     "# subtitles: 1 = the briefings' subtitles; brightness: 0 (the game's) to 10\n"
                     "subtitles=%d\n"
                     "brightness=%d\n"
                     "# map: mini, full or off\n"
                     "map=%s\n"
                     "# map_markers: none, pickups, or creatures (pickups and creatures)\n"
                     "map_markers=%s\n"
                     "# minimap_zoom, map_zoom: the zoom you left the minimap and the full map at (1 = as it opens)\n"
                     "minimap_zoom=%.3f\n"
                     "map_zoom=%.3f\n"
                     "# look: smooth or classic\n"
                     "look=%s\n"
                     "lighting=%d\n"
                     "# view_fov: degrees across a 4:3 screen (the game's: 90); tall_pixels: 1 = the game's 4:3 shape\n"
                     "view_fov=%.0f\n"
                     "tall_pixels=%d\n"
                     "vsync=%d\n"
                     "# render: native, half or retro\n"
                     "render=%s\n"
                     "# difficulty: easy, normal or hard\n"
                     "difficulty=%s\n"
                     "# monster_ai: original (1994) or enhanced\n"
                     "monster_ai=%s\n"
                     "monsters_attack=%d\n"
                     "# all_weapons: 1 = start each level with every weapon\n"
                     "all_weapons=%d\n"
                     "# pickups: walk (walking over an item takes it) or use (only the use key, as in the game)\n"
                     "pickups=%s\n"
                     "# doors: use (only the use key, as in the game) or walk (walking into a door opens it too)\n"
                     "doors=%s\n"
                     "mouse_sensitivity=%.2f\n"
                     "invert_mouse=%d\n"
                     "# last_slot: the save slot LOAD and SAVE open on (the one last used; 6 the quick save; -1 none yet)\n"
                     "last_slot=%d\n"
                     "# window: width height x y of the window as you left it (not full screen)\n"
                     "window=%d %d %d %d\n"
                     "# controls: up to two keys or mouse buttons each (Mouse Left/Right/Middle, Mouse 4, Mouse 5,\n"
                     "# Wheel Up, Wheel Down, or key names such as W, Left Shift, Space, Return)\n",
                     musicVolume, soundVolume, opt.fullscreen ? 1 : 0, introVideos ? "videos" : "skip",
                     enhancedVideos ? "enhanced" : "original", subtitles ? 1 : 0, brightness,
                     maps[mapMode], markers[std::clamp(mapMarkers, 0, 2)], mapZoom[Automap::Mini], mapZoom[Automap::Full],
                     world.smooth ? "smooth" : "classic", rs.lighting ? 1 : 0,
                     rs.hFovDeg4x3, rs.tallPixels ? 1 : 0, opt.vsync ? 1 : 0, renders[renderMode], diffs[std::clamp(world.difficulty, 0, 2)],
                     world.enhancedAI ? "enhanced" : "original", world.monstersAttack ? 1 : 0, world.startWithAllWeapons ? 1 : 0,
                     world.autoPickup ? "walk" : "use", world.bumpDoors ? "walk" : "use", mouseSens, invertMouse ? 1 : 0, lastSlot,
                     opt.width, opt.height, opt.winX == INT32_MIN ? -1 : opt.winX, opt.winY == INT32_MIN ? -1 : opt.winY);
        std::string out = buf;
        for (int a = 0; a < kActionCount; ++a)
            out += std::string("bind_") + Bindings::key((Action)a) + "=" + bindings.save((Action)a) + "\n";
        SDL_SaveFile(settingsPath().c_str(), out.data(), out.size());
    }

    // --hittest: writes which screen pixels would hit a creature (a grey image, white = hit),
    // to compare hit areas with the picture.
    void hitTestImage(int w, int h, const std::string& path) {
        float aspect = (float)w / h;
        float st = rs.tallPixels ? 1.2f : 1.0f;
        float tanV = std::tan(rs.hFovDeg4x3 * 3.14159265f / 360.0f) * 0.75f / st, tanH = tanV * aspect * st;
        float cy = std::cos(cam.yaw), sy = std::sin(cam.yaw), cp = std::cos(cam.pitch), sp = std::sin(cam.pitch);
        float fwd[3] = {sy * cp, sp, -cy * cp}, right[3] = {cy, 0, sy};
        float up[3] = {right[1] * fwd[2] - right[2] * fwd[1], right[2] * fwd[0] - right[0] * fwd[2], right[0] * fwd[1] - right[1] * fwd[0]};
        std::string out = "P5\n" + std::to_string(w) + " " + std::to_string(h) + "\n255\n";
        std::string px((size_t)w * h, '\0');
        float o[3] = {cam.x, cam.y, cam.z};
        for (int y = 0; y < h; y += 2)
            for (int x = 0; x < w; x += 2) {
                float sx = ((x + 1.0f) / w * 2 - 1) * tanH, syy = (1 - (y + 1.0f) / h * 2) * tanV;
                float d[3];
                for (int c = 0; c < 3; ++c) d[c] = fwd[c] + right[c] * sx + up[c] * syy;
                float l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                for (float& v : d) v /= l;
                if (world.rayHitsAnyMonster(o, d)) {
                    px[(size_t)y * w + x] = (char)255;
                    if (x + 1 < w) px[(size_t)y * w + x + 1] = (char)255;
                    if (y + 1 < h) { px[(size_t)(y + 1) * w + x] = (char)255; if (x + 1 < w) px[(size_t)(y + 1) * w + x + 1] = (char)255; }
                }
            }
        out += px;
        SDL_SaveFile(path.c_str(), out.data(), out.size());
    }

    // The HUD in the game's own fonts: health and armour (bottom left), weapon (bottom right),
    // messages (top left), damage flashes and the direction a hit came from.
    // The game's own status display (320 x 200 layout, scaled by u screen pixels per game pixel):
    // the stats panel (statsicon: armour green on the left, health blue on the right, a segment
    // going dark from the top for every 8 health or 13th of the suit's armour, as FUN_00040523
    // and FUN_000405e9 paint them), the ammunition counter in its LCD box (gunicon) beneath, the
    // bearing (FUN_0004084e) and the altimeters at both edges (FUN_0003f2b9 / FUN_0003f449).
    // Added: the numbers beside the bars, the weapon's name, and the kits you carry.
    void drawStatus(int w, int h) {
        const PlayerState& p = world.player();
        float u = std::max(1.0f, std::round(h / 225.0f));
        // The altimeters at both edges: a tick every 2 rows from 26 to 122, long / short / medium /
        // short, fading inward. The red tick is your eye's height (0xef3e6) between the level's
        // lowest and highest points, which its designers give as the floor and ceiling of cell
        // (0, 0) (0 and 512 where that cell has none; FUN_0003680a at 0x36a30); the game clamps it
        // there and draws 122 - (eye - low) / (high - low) * 96, truncated, even (0x3ef4d,
        // FUN_0003f2b9, FUN_0003f449). So it rises as you jump, climb or ride a lift. (Up to 0.5.2
        // the engine showed where you looked instead.)
        {
            int low = (level.floor.tex[0] & 0x3fff) ? (int16_t)level.floor.height[0] : 0;
            int high = (level.ceiling.tex[0] & 0x3fff) ? (int16_t)level.ceiling.height[0] : 512;
            float eye = std::clamp(p.viewY, (float)low, (float)std::max(low, high));
            int mark = high > low ? (int)(122 - (eye - low) / (float)(high - low) * 96) & ~1 : 122;
            int k = 0;
            for (int row = 26; row <= 122; row += 2, k = (k + 1) & 3) {
                int len = k == 0 ? 8 : k == 2 ? 4 : 2, step = k == 0 ? 2 : k == 2 ? 4 : 8;
                int c = row == mark ? 126 : 20;
                float y = std::round(row * h / 200.0f);
                for (int i = 0; i < len; ++i, c -= step) {
                    if (!world.inventoryOpen()) hud.palRect(3 * u + i * u, y, u, u, c, 0.9f);   // under the inventory
                    hud.palRect(w - 4 * u - i * u, y, u, u, c, 0.9f);
                }
            }
        }
        // Bearing: the point of the compass and the degrees (the game's own sums on its 0-255
        // angle, east = 0, counter-clockwise).
        {
            int a = ((int)std::lround(64 - cam.yaw * 128 / 3.14159265f) % 256 + 256) % 256;
            int deg = (255 - a) * 141 / 100 + 90;
            if (deg > 360) deg -= 360;
            static const char* points[] = {" E", "NE", " N", "NW", " W", "SW", " S", "SE"};
            char num[16];
            SDL_snprintf(num, sizeof(num), "%3d_", deg);
            float x = std::round(w * 0.5f - 65 * u);
            const char* pt = points[((a + 16) & 255) >> 5];
            // The game's font is fixed-width: a one-letter point stands over the second digit.
            float px = pt[0] == ' ' ? hud.textWidth(Hud::Green, u, "0") : 0;
            hud.text(Hud::Green, x + px, h - 16 * u, u, pt[0] == ' ' ? pt + 1 : pt);
            hud.text(Hud::Green, x, h - 8 * u, u, num);
        }
        // The Jump Jet Icon (the game's button 8 at (3, 126), under the left altimeter):
        // jumpjeticon green while the jets are off, jumpjeticon2 red while on, the fuel beside it
        // ("%03d" at (27, 128), amber or red). Shown once the jets have fuel; a click toggles
        // them as J does. Added: the key, small, over the number.
        jetsIcon[2] = 0;
        world.jetsKey = Bindings::inputName(bindings.in[(int)Action::JumpJets][0]);
        if (world.jetsKey.empty()) world.jetsKey = "THE JUMP JETS KEY";
        if (p.alive && world.jetFuel() >= 0 && !world.inventoryOpen()) {
            float ix = 3 * u, iy = std::round(126 * h / 200.0f);
            bool on = world.jetsOn();
            hud.sprite(on ? "jumpjeticon2" : "jumpjeticon", ix, iy, u, u);
            jetsIcon[0] = ix; jetsIcon[1] = iy; jetsIcon[2] = 22 * u; jetsIcon[3] = 9 * u;
            char fuel[16];
            SDL_snprintf(fuel, sizeof(fuel), "%03d", world.jetFuel());
            hud.text(on ? Hud::Red : Hud::Amber, ix + 24 * u, iy + 2 * u, u, fuel);
            std::string key = Bindings::inputName(bindings.in[(int)Action::JumpJets][0]);
            float ks = std::max(1.0f, u - 2);
            if (!key.empty() && key.size() <= 3) hud.text(Hud::Grey, ix + 24 * u, iy - 7 * ks, ks, key, 0.75f);
        }
        if (!p.alive) return;
        // Stats panel and the ammunition box, bottom right; the health number to its right. A
        // little smaller than the game's proportions (about four fifths).
        const float u0 = u;
        u = std::max(1.0f, std::round(h / 270.0f));
        float hw = hud.textWidth(Hud::Green, u, "104") + 3 * u;
        float sx = std::round(w - 4 * u - hw - 29 * u), gy = std::round(h - 17 * u), sy = gy - 44 * u;
        hud.image(Hud::StatsIcon, sx, sy, u);
        int armourDark = p.armorMax > 0 ? std::min(13, (int)((p.armorMax - p.armor) * 13 / p.armorMax)) : 13;
        int healthDark = std::min(13, std::max(0, (104 - (int)std::ceil(p.health)) / 8));
        for (int i = 0; i < 13; ++i) {
            if (i < armourDark) hud.palRect(sx + 2 * u, sy + (2 + 3 * i) * u, 12 * u, 2 * u, 4);
            if (i < healthDark) hud.palRect(sx + 15 * u, sy + (2 + 3 * i) * u, 12 * u, 2 * u, 4);
        }
        std::string av = std::to_string((int)std::ceil(p.armor)), hv = std::to_string((int)std::ceil(p.health));
        // Each number over its own bar (up to 0.5.2 beside their feet, where the weapon now is).
        float ny = sy - 9 * u;
        hud.text(Hud::Green, std::round(sx + 8 * u - hud.textWidth(Hud::Green, u, av) / 2), ny, u, av);
        hud.tinted(Hud::Grey, std::round(sx + 21 * u - hud.textWidth(Hud::Grey, u, hv) / 2), ny, u, hv, 0.8f, 1.0f, 2.2f);
        hud.image(Hud::GunIcon, sx, gy, u);
        hud.palRect(sx + u, gy + 2 * u, 27 * u, 13 * u, 5);
        if (p.weapon < 8) hud.digits(sx + 3 * u, gy + 3 * u, u, p.ammo[p.weapon], 3);
        // The readied weapon's tile from the game's weapons cache, left of the rounds, its name
        // over it (an addition: the game showed the tiles only with the cache open).
        std::string wn = world.weaponName();
        float tx = sx - 4 * u - 46 * u, ty = h - 30 * u;
        hud.weaponTile(weaponPicture(p.weapon), tx, ty, u, true, "");
        hud.text(Hud::Green, sx - 4 * u - hud.textWidth(Hud::Green, u, wn), ty - 9 * u, u, wn);
        // The Demo Pack Icon, blinking above the graphs while packs wait to go off (its key beside it).
        demoIcon[2] = 0;
        if (world.placedDemoPacks() > 0) {
            float dy = sy - 20 * u;   // over the numbers
            bool lit = std::fmod(clock, 0.5f) < 0.25f;
            hud.sprite(lit ? "demopakicon" : "demopakicon2", sx + 4 * u, dy, u, u);
            demoIcon[0] = sx + 4 * u; demoIcon[1] = dy; demoIcon[2] = 22 * u; demoIcon[3] = 9 * u;
            std::string key = Bindings::inputName(bindings.in[(int)Action::Detonate][0]);
            if (!key.empty()) hud.text(Hud::Grey, sx - 3 * u - hud.textWidth(Hud::Grey, u, key), dy + 2 * u, u, key, 0.8f);
        }
        // Kits, bottom left: the game's inventory pictures and how many you carry (the inventory
        // itself when it is open).
        u = u0;
        if (world.inventoryOpen()) { drawInventory(w, h); return; }
        {
            float k = std::max(1.0f, std::round(u * 0.75f)), x = 4 * u, base = h - 2 * u;
            float ih = 23 * k;   // both pictures stand on one line, the taller one's height
            auto kit = [&](Hud::Image img, int count, Action act) {
                float a = count > 0 ? 1.0f : 0.5f;
                hud.image(img, x, base - hud.imageH(img) * k, k, a);
                x += hud.imageW(img) * k + 2 * u;
                std::string n = std::to_string(count);
                hud.text(Hud::Green, x, base - 8 * u, u, n, a);
                float nw = hud.textWidth(Hud::Green, u, n);
                // Its key, small, above the count (if it has a short name).
                std::string key = Bindings::inputName(bindings.in[(int)act][0]);
                float ks = std::max(1.0f, u - 2);
                if (!key.empty() && key.size() <= 3) {
                    hud.text(Hud::Grey, x, base - ih, ks, key, 0.75f);
                    nw = std::max(nw, hud.textWidth(Hud::Grey, ks, key));
                }
                x += std::max(nw, 8 * u) + 8 * u;
            };
            kit(Hud::BioKit, p.bioKits, Action::BioKit);
            kit(Hud::MechKit, p.mechKits, Action::MechKit);
            // The key ring (FUN_0003eed1: invkeyr, invkeyb, invkeyg for one, two, three or more keys).
            int keys = 0;
            for (int b = 0; b < 31; ++b) keys += (p.keys >> b) & 1;
            if (keys > 0) {
                const char* ring[3] = {"invkeyr", "invkeyb", "invkeyg"};
                hud.sprite(ring[std::min(keys, 3) - 1], x, base - 20 * k, k, k);
                x += 39 * k + 4 * u;
            }
            // Activated items that are on, and the inventory's key.
            for (const auto& sl : world.inventory())
                if (sl.used && sl.active && hud.hasSprite(sl.icon)) {
                    hud.palRect(x, base - 22 * k, 45 * k, 22 * k, 0x6b, 0.35f);
                    hud.sprite(sl.icon, x, base - 26 * k, k, k);
                    x += 47 * k;
                }
            std::string key = Bindings::inputName(bindings.in[(int)Action::Inventory][0]);
            bool carrying = false;
            for (const auto& sl : world.inventory()) carrying |= sl.used && sl.use != 9 && sl.use != 10;
            if (carrying && !key.empty()) hud.text(Hud::Grey, x, base - 8 * u, std::max(1.0f, u - 1), "INVENTORY: " + key, 0.75f);
        }
    }

    // The open inventory (FUN_000414d1), where the game draws it: eight slots up the left edge and
    // along the top (its buttons 17 down to 10: 3,156 / 3,125 / 3,94 / 3,63 / 3,32 / 3,1 / 50,1 /
    // 97,1, each 45 x 29), each a dark plate (meshgrid) under a grid of lines every 9 pixels, green,
    // red for the readied item (yellow for an activated one), with the item's picture and, for a
    // kit stack, how many; the DROP button beneath (3,187). Added: the slot numbers, a frame
    // round the highlighted slot, and its name with what to do beside the first slot.
    static constexpr int kSlotXY[8][2] = {{3, 156}, {3, 125}, {3, 94}, {3, 63}, {3, 32}, {3, 1}, {50, 1}, {97, 1}};
    struct InvLayout { float q, ox, oy; };
    static InvLayout invLayout(int h) {
        float q = std::max(1.0f, std::floor(h / 200.0f));
        return {q, 0.0f, std::round((h - 200 * q) / 2)};
    }
    static int invSlotAt(float x, float y, int h) {
        InvLayout L = invLayout(h);
        for (int i = 0; i < 8; ++i) {
            float sx = L.ox + kSlotXY[i][0] * L.q, sy = L.oy + kSlotXY[i][1] * L.q;
            if (x >= sx && x < sx + 46 * L.q && y >= sy && y < sy + 30 * L.q) return i;
        }
        return -1;
    }
    static bool invDropAt(float x, float y, int h) {
        InvLayout L = invLayout(h);
        float dx = L.ox + 3 * L.q, dy = L.oy + 187 * L.q;
        return x >= dx && x < dx + 45 * L.q && y >= dy && y < dy + 13 * L.q;
    }
    void drawInventory(int w, int h) {
        (void)w;
        InvLayout L = invLayout(h);
        float q = L.q, ox = L.ox, oy = L.oy;
        auto slots = world.inventory();
        int cursor = world.inventoryCursor();
        int hover = invCursor ? invSlotAt(curX, curY, h) : -1;
        float ns = std::max(1.0f, q - 2);
        for (int i = 0; i < 8; ++i) {
            float x = ox + kSlotXY[i][0] * q, y = oy + kSlotXY[i][1] * q;
            const auto& sl = slots[i];
            hud.sprite("meshgrid", x, y, q, q, false, 0.85f);
            int c = sl.readied ? 0x7c : sl.active ? 0x6b : 0xb5;
            for (int gx = 0; gx <= 45; gx += 9) hud.palRect(x + gx * q, y, q, 28 * q, c, 0.9f);
            for (int gy = 0; gy <= 29; gy += 9) hud.palRect(x, y + gy * q, 45 * q, q, c, 0.9f);
            if (sl.used && !(dragging && i == dragSlot)) {
                if (hud.hasSprite(sl.icon)) hud.sprite(sl.icon, x, y, q, q);
                else hud.text(Hud::Green, x + 3 * q, y + 11 * q, std::max(1.0f, q - 1), sl.name.substr(0, 8));
                if (sl.count >= 2) hud.text(Hud::Amber, x + 2 * q, y + 2 * q, q, std::to_string(sl.count));
            }
            std::string n = std::to_string(i + 1);
            hud.text(Hud::Grey, x + 46 * q - hud.textWidth(Hud::Grey, ns, n) - ns, y + 29 * q - 7 * ns, ns, n, 0.8f);
            if (i == cursor || i == hover) {   // the highlight (the cursor's slot fainter)
                float t = std::max(1.0f, std::round(q / 2)), a = i == cursor ? 0.9f : 0.45f;
                hud.rect(x - t, y - t, 46 * q + 2 * t, t, 1, 1, 1, a);
                hud.rect(x - t, y + 29 * q, 46 * q + 2 * t, t, 1, 1, 1, a);
                hud.rect(x - t, y, t, 29 * q, 1, 1, 1, a);
                hud.rect(x + 46 * q, y, t, 29 * q, 1, 1, 1, a);
            }
        }
        hud.sprite("dropicon", ox + 3 * q, oy + 187 * q, q, q, false, dropMode ? 1.0f : 0.85f);
        if (dropMode) hud.rect(ox + 3 * q, oy + 187 * q, 45 * q, 13 * q, 1.0f, 0.55f, 0.1f, 0.35f);
        // Beside the first slot: the highlighted (or pointed at) item's name, what it is waiting
        // for, and what the mouse and keys do.
        float tx = ox + 52 * q, ty = oy + 157 * q, ts = std::max(1.0f, std::floor(q * 0.5f + 0.5f));
        int shown = hover >= 0 && slots[hover].used ? hover : cursor;
        if (shown >= 0 && shown < 8 && slots[shown].used) {
            const auto& sl = slots[shown];
            std::string name = sl.name;
            for (char& ch : name) ch = (char)std::toupper((unsigned char)ch);
            hud.text(sl.readied ? Hud::Red : Hud::Green, tx, ty, std::max(1.0f, q - 1), name);
            const char* state = sl.readied ? "READIED: CLICK ITS TARGET" : sl.active ? "ON" : nullptr;
            if (state) hud.text(sl.readied ? Hud::Red : Hud::Amber, tx, ty + 8 * q, ts, state);
        }
        auto key = [&](Action a) {
            std::string k = Bindings::inputName(bindings.in[(int)a][0]);
            return k.empty() ? std::string("-") : k;
        };
        const std::string hints[4] = {dropMode ? "CLICK THE ITEM TO DROP" : "CLICK OR 1-8: SELECT", "DRAG ONTO A TARGET: USE",
                                      "RIGHT CLICK: USE / CLOSE", key(Action::Inventory) + " OR ESC: CLOSE"};
        for (int i = 0; i < 4; ++i) hud.text(Hud::Grey, tx, ty + 14 * q + i * 8 * ts, ts, hints[i], 0.8f);
    }

    // The inventory's cursor, as the game has it: the manual's small purple box (an orange down
    // arrow once DROP is pressed), the dragged item's picture beside it.
    void drawInventoryCursor(int w, int h) {
        float x = invCursor ? curX : w * 0.5f, y = invCursor ? curY : h * 0.5f;
        float q = invLayout(h).q, u = std::max(1.0f, std::round(q * 0.6f));
        if (dragging && dragSlot >= 0) {
            auto slots = world.inventory();
            if (slots[dragSlot].used && hud.hasSprite(slots[dragSlot].icon))
                hud.sprite(slots[dragSlot].icon, x - 22 * q, y - 14 * q, q, q, false, 0.8f);
        }
        if (dropMode) {   // arrowb, its tip on the point
            hud.tintedSprite("arrowb", x, y - 10 * u, u, 1.7f, 0.9f, 0.15f);
            return;
        }
        float b = 4 * u, t = std::max(1.0f, std::round(u * 0.5f));
        for (int pass = 0; pass < 2; ++pass) {
            float o = pass == 0 ? 1 : 0, r = pass ? 0.66f : 0, g = pass ? 0.25f : 0, bl = pass ? 0.95f : 0;
            hud.rect(x - b - o, y - b - o, 2 * b + 2 * o, t + 2 * o, r, g, bl, 1);
            hud.rect(x - b - o, y + b - t - o, 2 * b + 2 * o, t + 2 * o, r, g, bl, 1);
            hud.rect(x - b - o, y - b - o, t + 2 * o, 2 * b + 2 * o, r, g, bl, 1);
            hud.rect(x + b - t - o, y - b - o, t + 2 * o, 2 * b + 2 * o, r, g, bl, 1);
        }
    }

    // The line of sight through a point of the screen: the camera turned to look through it (what
    // the cursor points at, for "use").
    Camera cursorCamera(float px, float py, int w, int h) const {
        float aspect = (float)w / std::max(1, h), st = rs.tallPixels ? 1.2f : 1.0f;
        float tanV = std::tan(rs.hFovDeg4x3 * 3.14159265f / 360.0f) * 0.75f / st, tanH = tanV * aspect * st;
        float cy = std::cos(cam.yaw), sy = std::sin(cam.yaw), cp = std::cos(cam.pitch), sp = std::sin(cam.pitch);
        float fwd[3] = {sy * cp, sp, -cy * cp}, right[3] = {cy, 0, sy};
        float up[3] = {right[1] * fwd[2] - right[2] * fwd[1], right[2] * fwd[0] - right[0] * fwd[2], right[0] * fwd[1] - right[1] * fwd[0]};
        float sx = (px / w * 2 - 1) * tanH, syy = (1 - py / h * 2) * tanV;
        float d[3];
        for (int c = 0; c < 3; ++c) d[c] = fwd[c] + right[c] * sx + up[c] * syy;
        float l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        Camera c = cam;
        c.yaw = std::atan2(d[0] / l, -d[2] / l);
        c.pitch = std::asin(std::clamp(d[1] / l, -1.0f, 1.0f));
        return c;
    }
    Camera cursorCamera() const {
        int w = 1, h = 1;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        return invCursor ? cursorCamera(curX, curY, w, h) : cam;
    }
    void cursorFromWindow(float x, float y) {
        int ww, wh, pw, ph;
        SDL_GetWindowSize(window, &ww, &wh);
        SDL_GetWindowSizeInPixels(window, &pw, &ph);
        curX = ww > 0 ? x * pw / ww : x;
        curY = wh > 0 ? y * ph / wh : y;
        if (dragSlot >= 0 && !dragging && std::abs(curX - dragFromX) + std::abs(curY - dragFromY) > 6) dragging = true;
    }
    // A mouse button with the inventory's cursor out. Left: a slot is selected (or, dragged out
    // onto the view, used there: readied and used on what it was let go over); DROP, then a slot,
    // drops that item (or drag the item onto DROP); the Demo Pack Icon detonates; in the view, the
    // readied item is used on what the cursor is on (with nothing readied, doors and switches are
    // used and items taken). Right: "use" at the cursor, which closes the inventory when nothing
    // is readied, as in the game.
    void inventoryClick(int button, bool down) {
        int w = 1, h = 1;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        if (button == SDL_BUTTON_LEFT) {
            if (down) {
                int sl = invSlotAt(curX, curY, h);
                if (sl >= 0) {
                    if (dropMode) { world.inventoryDrop(sl); dropMode = false; return; }
                    dragSlot = sl;
                    dragFromX = curX;
                    dragFromY = curY;
                    dragging = false;
                    return;
                }
                if (invDropAt(curX, curY, h)) { dropMode = !dropMode; return; }
                dropMode = false;
                if (demoIcon[2] > 0 && curX >= demoIcon[0] && curX < demoIcon[0] + demoIcon[2] && curY >= demoIcon[1] &&
                    curY < demoIcon[1] + demoIcon[3]) {
                    world.detonate();
                    return;
                }
                if (jetsIcon[2] > 0 && curX >= jetsIcon[0] && curX < jetsIcon[0] + jetsIcon[2] && curY >= jetsIcon[1] &&
                    curY < jetsIcon[1] + jetsIcon[3]) {
                    world.toggleJets();
                    return;
                }
                world.useAt(cursorCamera(curX, curY, w, h), true);
                return;
            }
            if (dragSlot < 0) return;
            int sl = dragSlot;
            bool dragged = dragging;
            dragSlot = -1;
            dragging = false;
            if (!dragged) { world.inventorySelect(sl); return; }
            if (invDropAt(curX, curY, h)) { world.inventoryDrop(sl); return; }
            InvLayout L = invLayout(h);
            if (invSlotAt(curX, curY, h) >= 0 || curX < L.ox + 49 * L.q) return;   // let go over the inventory: nothing
            auto slots = world.inventory();
            if (!slots[sl].used) return;
            if (slots[sl].use != 6) { world.inventorySelect(sl); return; }
            if (world.readiedSlot() != sl) {
                if (world.readiedSlot() >= 0) world.inventorySelect(world.readiedSlot());   // the other one unreadied
                world.inventorySelect(sl);
            }
            if (world.readiedSlot() == sl) world.useAt(cursorCamera(curX, curY, w, h), true);
            return;
        }
        if (!down) return;
        if (button == SDL_BUTTON_RIGHT) {
            dropMode = false;
            world.useAt(cursorCamera(curX, curY, w, h));
            return;
        }
        Action a = bindings.actionFor(kMouseBase + button);   // the other buttons do what they are bound to
        if (a != Action::Count && !inventoryAction(a) && !Bindings::held(a)) doAction(a);
    }
    // The cursor comes out while the inventory is open (the view stands still), and goes away as
    // it closes, however it closes.
    void syncInventoryCursor() {
        bool want = world.inventoryOpen() && !menuOpen && world.player().alive && !cutscenes.active();
        if (want) {
            if (!invCursor) {
                invCursor = true;
                int w = 1, h = 1, ww = 1, wh = 1;
                SDL_GetWindowSizeInPixels(window, &w, &h);
                SDL_GetWindowSize(window, &ww, &wh);
                curX = w * 0.5f;
                curY = h * 0.5f;
                dragSlot = -1;
                dragging = dropMode = false;
                if (opt.shot.empty()) {
                    SDL_SetWindowRelativeMouseMode(window, false);
                    SDL_SetWindowMouseGrab(window, true);   // the cursor stays in the window
                    SDL_WarpMouseInWindow(window, ww * 0.5f, wh * 0.5f);
                }
            }
            if (opt.shot.empty()) {
                if (SDL_GetWindowRelativeMouseMode(window)) SDL_SetWindowRelativeMouseMode(window, false);
                SDL_HideCursor();
            }
        } else if (invCursor) {
            invCursor = false;
            dragSlot = -1;
            dragging = dropMode = false;
            if (opt.shot.empty()) {
                SDL_SetWindowMouseGrab(window, false);
                if (!menuOpen) SDL_SetWindowRelativeMouseMode(window, true);
            }
        }
    }

    void drawHud(int w, int h) {
        hud.begin(w, h);
        const PlayerState& p = world.player();
        bool cleanCapture = !opt.shot.empty() && opt.map.empty() && opt.focus.size();
        if (mapMode != Automap::Full && !cleanCapture) firstPerson.draw(hud, w, h, rs.tallPixels);
        float s = std::max(2.0f, std::round(h / 240.0f));
        if (p.hurt > 0) hud.rect(0, 0, (float)w, (float)h, 0.75f, 0.05f, 0.02f, std::min(0.12f, p.hurt * 0.12f));
        if (p.bonus > 0) hud.rect(0, 0, (float)w, (float)h, 0.9f, 0.75f, 0.2f, p.bonus * 0.18f);
        // Where the last hit came from: a red arc round the crosshair, on the side it came from
        // (top: ahead, bottom: behind).
        if (p.hurtShow > 0 && p.alive) {
            float rel = p.hurtYaw - cam.yaw;
            float r = h * 0.16f;
            float alpha = std::min(1.0f, p.hurtShow * 1.5f) * 0.85f;
            for (int k = -6; k <= 6; ++k) {
                float a = rel + k * 0.045f;
                float x = w * 0.5f + std::sin(a) * r, y = h * 0.5f - std::cos(a) * r;
                float sz = s * (2.6f - std::abs(k) * 0.22f);
                hud.rect(x - sz / 2, y - sz / 2, sz, sz, 1.0f, 0.12f, 0.06f, alpha * (1.0f - std::abs(k) * 0.08f));
            }
        }
        if (!opt.shot.empty() && opt.map.empty() && opt.focus.size()) { hud.end(); return; }  // clean captures
        float m = std::round(h * 0.03f);
        drawStatus(w, h);
        if (mapMode != Automap::Full && !world.inventoryOpen()) drawWeaponStrip(w, h);
        if (world.inventoryOpen() && p.alive && mapMode != Automap::Full) drawInventoryCursor(w, h);
        // Messages, newest last; each fades out over its last second.
        float my = m, mx = m;
        if (world.inventoryOpen()) mx = std::max(m, 146 * std::max(1.0f, std::floor(h / 200.0f)));   // beside the slots along the top
        for (const auto& msg : world.messages()) {
            float a = std::clamp(4.0f - msg.age, 0.0f, 1.0f);
            hud.text((Hud::Font)msg.font, mx, my, s, msg.text, a);
            my += hud.lineHeight(Hud::Green, s) + s;
        }
        hud.end();
    }

    // Death, as the game shows it (FUN_00036b53): for 1.25 s the world runs on while the picture
    // tears sideways more and more; then the whole screen collapses like a television switching
    // off: squeezed to a bright line in a second, the line shrinks to a dot, and the dot goes out.
    static constexpr float kDeathScreen = 3.25f;
    void drawDeath(int w, int h) {
        const PlayerState& p = world.player();
        if (p.alive) return;
        float u = (p.deadTime * 100.0f) - 125.0f;   // the game's 200 - timer
        float rect[4] = {0, 0, 320, 200}, glitch = 0, bright = 0;
        if (u < 0) {
            int range = std::max(1, (int)(p.deadTime * 100.0f) + 1);
            glitch = (float)((int)(deathRng() % (unsigned)range) & 0xf0);
        } else if (u < 100) {   // squeeze to a line
            rect[0] = u * 0.5f; rect[2] = 320 - u * 0.5f;
            rect[1] = u; rect[3] = 200 - u;
        } else if (u < 155) {   // the line shrinks to a dot
            float v = u - 100;
            rect[0] = 50 + v * 2; rect[2] = 270 - v * 2;
            rect[1] = 99.5f; rect[3] = 100.5f;
        } else if (u < 200) {   // the dot
            rect[0] = 159.5f; rect[2] = 160.5f;
            rect[1] = 99.5f; rect[3] = 100.5f;
        } else {
            rect[0] = rect[2] = 160; rect[1] = rect[3] = -10;   // gone: all black
        }
        if (u > 0) bright = std::min(1.0f, u / 128.0f) * 0.85f;
        renderer.crt(w, h, rect, bright, glitch);
    }
    bool deathMenu = false;   // the menu has been opened for this death
    bool cutscenesWere = false;   // cut-scenes were playing last frame
    // A move a level script asked for (World::Warp). As the game does it (FUN_0003733b): the
    // screen fades out (64 retraces, about 0.9 s) while everything stands still, then the player
    // is somewhere else; between missions the video plays and the next level loads first.
    World::Warp warp;
    int warpStage = 0;            // 0 none, 1 fading out, 2 fading back in
    float warpDark = 0;           // 0..1

    // The videos CYCLONES.EXE plays for the code a mission ends with (SMKPLAY.EXE's playlists).
    static std::vector<std::vector<std::string>> missionVideos(int code) {
        switch (code) {
        case 4: return {{"4", "32", "33"}};
        case 11: return {{"11"}, {"36a"}, {"36b"}};
        case 22: return {{"22", "40"}};
        default: return {{std::to_string(code)}};
        }
    }
    void changeMission(const World::Warp& w) {
        static const char* names[24] = {"BASE", "LAB2", "ALIEN1", "BIOLAB", "BIOSTOR2", "FAILSAF2", "MUSEUM2", "MAINTEN2",
                                        "TREAT2", "SUBWAY", "SDILAB", "SDI1", "SDI2", "HIVE1", "HIVE2", "HIVE3",
                                        "HIVE4", "HIVE5", "MOTHER1", "MOTHER2", "MOTHER3", "CORE", "CLONE", "HEAVY"};
        if (w.mission < 1 || w.mission > 24) return;
        auto it = std::find(levels.begin(), levels.end(), names[w.mission - 1]);
        if (it == levels.end()) {
            SDL_Log("Mission %d (%s) isn't on the disc", w.mission, names[w.mission - 1]);
            return;
        }
        PlayerState kit = world.player();
        if (w.video > 0 && opt.shot.empty()) cutscenes.start(missionVideos(w.video), &disc, &audio);
        loadLevel((int)(it - levels.begin()));
        world.carryOver(kit, w.mission);
        world.warpPlayer(w.cellX, w.cellZ, w.y);
        world.selectWeapon(kit.weapon);
        firstPerson.reset();
        cam.yaw = w.yaw;
        cam.pitch = 0;
        cam.x = world.player().x;
        cam.y = world.player().viewY;
        cam.z = world.player().z;
    }
    void updateWarp(float dt) {
        if (warpStage == 0) {
            World::Warp w = world.takeWarp();
            if (!w.pending) return;
            warp = w;
            warpStage = 1;
            warpDark = 0;
        }
        if (warpStage == 1) {
            warpDark += dt / 0.9f;
            if (warpDark < 1) return;
            warpDark = 1;
            if (warp.end) {   // the end of the game (exit code 22): videos 22 and 40, then the game stops
                finished = true;
                if (opt.shot.empty()) cutscenes.start(missionVideos(warp.video), &disc, &audio);
                openMenu(true);
                warpStage = 0;
                warpDark = 0;
                return;
            }
            if (warp.mission) {
                changeMission(warp);
            } else {
                world.warpPlayer(warp.cellX, warp.cellZ, warp.y);
                cam.yaw = warp.yaw;
                cam.pitch = 0;
                cam.x = world.player().x;
                cam.y = world.player().viewY;
                cam.z = world.player().z;
            }
            warpStage = 2;
            return;
        }
        warpDark -= dt / 0.35f;
        if (warpDark <= 0) { warpDark = 0; warpStage = 0; }
    }
    std::minstd_rand deathRng{1994};

    // A cut-scene's picture in the window (from Cutscenes or the EXTRAS player): the Enhanced
    // picture when there is one, smoothly scaled; otherwise the game's, sharp, with the
    // live-action window's Enhanced picture over it.
    const void* videoSource = nullptr;
    uint64_t videoSerial = 0;
    template <class V> void drawVideoPicture(const V& v, int w, int h, float x, float y, float cw, float ch) {
        bool fresh = &v != videoSource || v.serial() != videoSerial;
        videoSource = &v;
        videoSerial = v.serial();
        int pw = 0, ph = 0;
        if (const auto* hd = v.hdPicture(pw, ph)) {
            hud.photo(0, fresh ? hd->data() : nullptr, pw, ph, w, h, x, y, cw, ch, v.fade(), true);
            return;
        }
        hud.picture(v.rgba().data(), 320, 200, w, h, x, y, cw, ch);
        int ox = 0, oy = 0, ow = 0, oh = 0;
        if (const auto* o = v.overlay(ox, oy, ow, oh, pw, ph))
            hud.photo(1, fresh ? o->data() : nullptr, pw, ph, w, h, x + ox * cw / 320, y + oy * ch / 200, ow * cw / 320,
                      oh * ch / 200, v.fade(), false);
    }

    // The weapons cache's pictures (the weapon table at 0xef428: the gun, or its clip when you have
    // rounds but not the gun), and the glove's (made from the suit's glove).
    std::string weaponPicture(int w, bool clip = false) const {
        static const char* guns[8] = {"i_gaussp", "i_chaing", "i_alpstl", "i_dbgun", "i_grnlch", "i_twnlas", "i_rktlch", "i_plasma"};
        static const char* clips[8] = {"i_gusclp", "i_chgclp", "i_alpclp", "i_dbgclip", "i_grnclp", "i_twnlclp", "i_rlchclp", "i_plsmclp"};
        if (w == 8) return "wglove" + std::to_string(std::clamp(world.player().suit, 0, 2));
        if (w < 0 || w > 7) return "";
        return clip ? clips[w] : guns[w];
    }
    // The weapon strip (an addition): turning the wheel (next / previous weapon) shows every
    // weapon's tile in a row at the foot of the view for a moment, the game's weapons cache laid
    // out flat: the readied one red, the rest green, the rounds in each; a clip's outline for rounds
    // without the gun; a weapon you don't have dimmed.
    float weaponStripUntil = -1;
    void drawWeaponStrip(int w, int h) {
        float left = weaponStripUntil - clock;
        if (left <= 0 || !world.player().alive) return;
        float a = std::min(1.0f, left / 0.35f);
        const PlayerState& p = world.player();
        float s = std::max(1.0f, std::round(h / 360.0f)), u = std::max(1.0f, std::round(h / 225.0f));
        float tw = 46 * s, gap = 3 * s, total = 9 * tw + 8 * gap;
        float su = std::max(1.0f, std::round(h / 270.0f));   // the status display's scale: above its weapon tile and name
        float x = std::round((w - total) / 2), y = std::round(h - 42 * su - 29 * s - 16 * s);
        (void)u;
        hud.rect(x - 4 * s, y - 4 * s, total + 8 * s, 29 * s + 8 * s + 10 * s, 0, 0, 0, 0.55f * a);
        for (int k = 0; k < 9; ++k) {
            bool has = p.owned[k], rounds = k < 8 && p.ammo[k] > 0;
            std::string pic = has ? weaponPicture(k) : rounds ? weaponPicture(k, true) : "";
            std::string count = k < 8 && (has || rounds) ? std::to_string(p.ammo[k]) : "";
            hud.weaponTile(pic, x + k * (tw + gap), y, s, k == p.weapon, count, a, !has && !rounds);
        }
        std::string wn = world.weaponName();
        hud.text(Hud::Green, std::round((w - hud.textWidth(Hud::Green, s, wn)) / 2), y + 29 * s + 2 * s, s, wn, a);
    }

    // ---- EXTRAS: the cut-scene player (an addition) ----
    // The picture fills the window as the cut-scenes do; a bar over its foot (shown while paused,
    // and for a while after the mouse moves or a key is pressed) holds play / pause, the timeline
    // (a mark where each video of the playlist starts; click or drag to go there), the time, the
    // video and frame, and ENHANCED / ORIGINAL. Keys: Space or K play / pause, Left / Right 5 s,
    // Up / Down 30 s, comma / period a frame back / on, Page Up / Page Down the video before / next,
    // Home / End, E enhanced or original, Esc back to EXTRAS.
    double playerShowUntil = 0;
    bool playerScrub = false, playerWasPlaying = false;
    float playerMouseX = -1, playerMouseY = -1;
    float pbBar[4] = {0, 0, 0, 0}, pbPlay[4] = {0, 0, 0, 0}, pbMode[4] = {0, 0, 0, 0}, pbBack[4] = {0, 0, 0, 0};
    void openPlayer(int list) {
        if (!player.open(extraList(list).videos, &disc, &audio, &hdPack, enhancedVideos && hdPack.loaded())) return;
        playerList = list;
        playerShowUntil = clock + 3;
        playerScrub = false;
        playerMouseX = playerMouseY = -1;
    }
    void closePlayer() {
        player.close();
        playerList = -1;
        audio.stopSounds();
        audio.music(musicTrack(level.name));   // the level's music again, behind the options screen
    }
    void playerPoke() { playerShowUntil = clock + 2.5; }
    static bool inRect(const float r[4], float x, float y) { return r[2] > 0 && x >= r[0] && x < r[0] + r[2] && y >= r[1] && y < r[1] + r[3]; }
    void playerSeekToMouse() {
        if (pbBar[2] <= 0) return;
        float f = std::clamp((playerMouseX - pbBar[0]) / pbBar[2], 0.0f, 1.0f);
        player.seek(f * player.length());
    }
    void playerKey(SDL_Keycode k, bool repeat) {
        playerPoke();
        double t = player.time();
        const auto& clips = player.clips();
        int c = player.clipAt(t);
        switch (k) {
        case SDLK_ESCAPE: if (!repeat) closePlayer(); break;
        case SDLK_SPACE: case SDLK_K: if (!repeat) player.setPlaying(!player.playing()); break;
        case SDLK_LEFT: case SDLK_J: player.seek(t - 5); break;
        case SDLK_RIGHT: case SDLK_L: player.seek(t + 5); break;
        case SDLK_UP: player.seek(t + 30); break;
        case SDLK_DOWN: player.seek(t - 30); break;
        case SDLK_COMMA: player.step(-1); break;
        case SDLK_PERIOD: player.step(1); break;
        case SDLK_HOME: player.seek(0); break;
        case SDLK_END: player.seek(player.length()); break;
        case SDLK_PAGEUP:   // the start of this video, or the one before if just past it
            if (!clips.empty()) player.seek(t - clips[c].start < 1.5 && c > 0 ? clips[c - 1].start : clips[c].start);
            break;
        case SDLK_PAGEDOWN:
            if (c + 1 < (int)clips.size()) player.seek(clips[c + 1].start);
            break;
        case SDLK_E: if (!repeat) { player.setEnhanced(!player.enhanced()); } break;
        case SDLK_F11: setFullscreen(!(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN)); break;
        case SDLK_F12: screenshot(); break;
        default: break;
        }
    }
    void playerMouse(const SDL_Event& e) {
        int ww, wh, pw, ph;
        SDL_GetWindowSize(window, &ww, &wh);
        SDL_GetWindowSizeInPixels(window, &pw, &ph);
        auto toPx = [&](float x, float y) {
            playerMouseX = ww > 0 ? x * pw / ww : x;
            playerMouseY = wh > 0 ? y * ph / wh : y;
        };
        if (e.type == SDL_EVENT_MOUSE_MOTION) {
            toPx(e.motion.x, e.motion.y);
            playerPoke();
            if (playerScrub) playerSeekToMouse();
        } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            toPx(e.button.x, e.button.y);
            playerPoke();
            if (e.button.button == SDL_BUTTON_RIGHT) { closePlayer(); return; }
            if (e.button.button != SDL_BUTTON_LEFT) return;
            if (inRect(pbBar, playerMouseX, playerMouseY)) {
                playerScrub = true;
                playerWasPlaying = player.playing();
                if (playerWasPlaying) player.setPlaying(false);
                playerSeekToMouse();
            } else if (inRect(pbPlay, playerMouseX, playerMouseY)) player.setPlaying(!player.playing());
            else if (inRect(pbMode, playerMouseX, playerMouseY)) player.setEnhanced(!player.enhanced());
            else if (inRect(pbBack, playerMouseX, playerMouseY)) closePlayer();
            else player.setPlaying(!player.playing());   // a click on the picture
        } else if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT && playerScrub) {
            playerScrub = false;
            if (playerWasPlaying) player.setPlaying(true);
        } else if (e.type == SDL_EVENT_MOUSE_WHEEL) {
            playerPoke();
            if (e.wheel.y != 0) player.seek(player.time() + (e.wheel.y > 0 ? 5 : -5));
        }
    }
    static std::string clockText(double t) {
        int s = (int)std::floor(std::max(0.0, t));
        char b[16];
        SDL_snprintf(b, sizeof(b), "%d:%02d", s / 60, s % 60);
        return b;
    }
    void drawPlayer(int w, int h) {
        float aspect = rs.tallPixels ? 4.0f / 3.0f : 1.6f;
        float cw = std::min((float)w, h * aspect), ch = cw / aspect;
        drawVideoPicture(player, w, h, std::round((w - cw) / 2), std::round((h - ch) / 2), cw, ch);
        bool show = !player.playing() || playerScrub || clock < playerShowUntil;
        {
            float u = std::max(1.0f, std::round(h / 270.0f));
            drawSubtitles(player, w, h, std::round((w - cw) / 2), std::round((h - ch) / 2), cw, ch, show ? h - (3 * 8 * u + 4 * 6 * u) - 4 * u : (float)h);
        }
        pbBar[2] = pbPlay[2] = pbMode[2] = pbBack[2] = 0;
        hud.begin(w, h);
        if (show) {
            const float u = std::max(1.0f, std::round(h / 270.0f));
            const float lh = 8 * u, pad = 6 * u;
            float barH = 3 * lh + 4 * pad;
            float y0 = h - barH;
            hud.rect(0, y0, (float)w, barH, 0, 0, 0, 0.62f);
            hud.rect(0, y0, (float)w, u, 0.2f, 0.75f, 0.25f, 0.5f);
            const auto& clips = player.clips();
            double t = player.time(), len = std::max(0.001, player.length());
            int c = player.clipAt(t);
            // first line: the playlist and the video, frame; ENHANCED / ORIGINAL; BACK
            float ty = y0 + pad;
            std::string title = std::string(extraList(playerList).label) + "  -  " + (clips.empty() ? "" : clips[c].name);
            if (clips.size() > 1) title += "  (" + std::to_string(c + 1) + " OF " + std::to_string(clips.size()) + ")";
            title += "   FRAME " + std::to_string(player.frameNumber()) + " / " + std::to_string(player.videoFrames());
            hud.text(Hud::Green, pad, ty, u, title);
            float bx = w - pad;
            std::string back = "BACK";
            float bw = hud.textWidth(Hud::Amber, u, back);
            bx -= bw;
            hud.text(inRect(pbBack, playerMouseX, playerMouseY) ? Hud::Green : Hud::Amber, bx, ty, u, back);
            pbBack[0] = bx - 3 * u; pbBack[1] = ty - 2 * u; pbBack[2] = bw + 6 * u; pbBack[3] = lh + 2 * u;
            if (player.hasEnhanced()) {
                std::string mode = player.enhanced() ? "ENHANCED" : "ORIGINAL";
                float mw = hud.textWidth(Hud::Amber, u, "ENHANCED");
                float mx = bx - 16 * u - mw;
                hud.rect(mx - 3 * u, ty - 2 * u, mw + 6 * u, lh + 2 * u, player.enhanced() ? 0.15f : 0.35f, player.enhanced() ? 0.45f : 0.3f,
                         0.15f, inRect(pbMode, playerMouseX, playerMouseY) ? 0.9f : 0.6f);
                hud.text(player.enhanced() ? Hud::Green : Hud::Amber, mx + (mw - hud.textWidth(Hud::Amber, u, mode)) / 2, ty, u, mode);
                pbMode[0] = mx - 3 * u; pbMode[1] = ty - 2 * u; pbMode[2] = mw + 6 * u; pbMode[3] = lh + 2 * u;
            }
            // second line: play / pause, the timeline, the time
            float ry = ty + lh + pad;
            float px = pad, ps = lh;   // the button, a square of the line's height
            bool hot = inRect(pbPlay, playerMouseX, playerMouseY);
            float br = hot ? 0.55f : 0.35f, bg = hot ? 1.0f : 0.85f, bb = hot ? 0.55f : 0.35f;
            if (player.playing()) {   // pause: two bars
                hud.rect(px + ps * 0.15f, ry, ps * 0.25f, ps, br, bg, bb, 1);
                hud.rect(px + ps * 0.6f, ry, ps * 0.25f, ps, br, bg, bb, 1);
            } else {                  // play: a triangle, a slice a pixel high
                for (float yy = 0; yy < ps; yy += 1) {
                    float k = 1 - std::abs((yy + 0.5f) / ps * 2 - 1);
                    hud.rect(px + ps * 0.1f, ry + yy, ps * 0.85f * k, 1, br, bg, bb, 1);
                }
            }
            pbPlay[0] = px - 2 * u; pbPlay[1] = ry - 2 * u; pbPlay[2] = ps + 4 * u; pbPlay[3] = ps + 4 * u;
            std::string tt = clockText(t) + " / " + clockText(len);
            float tw = hud.textWidth(Hud::Green, u, "00:00 / 00:00");
            float x0 = px + ps + 3 * pad, x1 = w - pad - tw - 2 * pad;
            hud.text(Hud::Green, w - pad - hud.textWidth(Hud::Green, u, tt), ry, u, tt);
            float by = ry + lh / 2 - 1.5f * u, bh = 3 * u;
            hud.rect(x0, by, x1 - x0, bh, 0.25f, 0.3f, 0.25f, 0.9f);
            float fx = x0 + (float)(t / len) * (x1 - x0);
            hud.rect(x0, by, fx - x0, bh, 0.85f, 0.15f, 0.1f, 1);
            for (size_t i = 1; i < clips.size(); ++i) {   // where each video starts
                float cx = x0 + (float)(clips[i].start / len) * (x1 - x0);
                hud.rect(cx - u * 0.5f, by - 2 * u, u, bh + 4 * u, 0.9f, 0.9f, 0.6f, 0.9f);
            }
            hud.rect(fx - 2 * u, by - 3 * u, 4 * u, bh + 6 * u, 1, 0.85f, 0.4f, 1);   // the knob
            pbBar[0] = x0; pbBar[1] = by - 6 * u; pbBar[2] = x1 - x0; pbBar[3] = bh + 12 * u;
            if (inRect(pbBar, playerMouseX, playerMouseY) || playerScrub) {   // the time under the mouse
                double mt = std::clamp((playerMouseX - x0) / (x1 - x0), 0.0f, 1.0f) * len;
                std::string ht = clockText(mt) + "  " + clips[player.clipAt(mt)].name;
                float hw = hud.textWidth(Hud::Grey, u, ht);
                float hx = std::clamp(playerMouseX - hw / 2, pad, w - pad - hw), hy = y0 - lh - 4 * u;
                hud.rect(hx - 3 * u, hy - 2 * u, hw + 6 * u, lh + 3 * u, 0, 0, 0, 0.7f);
                hud.text(Hud::Grey, hx, hy, u, ht);
            }
            // third line: the keys
            hud.text(Hud::Grey, pad, ry + lh + pad, std::max(1.0f, u - 1),
                     "SPACE PLAY/PAUSE   LEFT/RIGHT 5 S   UP/DOWN 30 S   , . FRAME   PGUP/PGDN VIDEO   E ENHANCED   ESC BACK", 0.85f);
            if (playerMouseX >= 0) hud.image(Hud::MenuCursor, std::round(playerMouseX) - 9 * u, std::round(playerMouseY) - 9 * u, u);
        }
        hud.end();
    }

    // The briefings' subtitles (an addition): the cue for the speech as far as it has got, in the
    // game's green lettering at its own pixel size, centred on the bezel under the video window
    // (178 of the 200 rows), on a dark band; never lower than maxBottom (the player's bar).
    template <class V> void drawSubtitles(const V& v, int w, int h, float x, float y, float cw, float ch, float maxBottom) {
        int id = -1;
        double t = 0;
        if (!subtitles || !v.speechAt(id, t)) return;
        const SubtitleCue* cue = hdPack.cueAt(id, t);
        if (!cue) return;
        float s = std::max(1.0f, std::floor(ch / 200.0f));
        std::vector<std::string> lines;
        size_t p = 0;
        while (p <= cue->text.size()) {
            size_t q = cue->text.find('\n', p);
            std::string part = cue->text.substr(p, q == std::string::npos ? std::string::npos : q - p);
            for (auto& l : wrap(Hud::Green, part, cw * 0.88f / s)) lines.push_back(l);
            if (q == std::string::npos) break;
            p = q + 1;
        }
        if (lines.empty()) return;
        float lh = 9 * s, blockH = lines.size() * lh;
        float cy = y + 178.0f * ch / 200.0f;
        float top = std::round(std::min(cy - blockH / 2, maxBottom - blockH - 2 * s));
        hud.begin(w, h);
        for (size_t i = 0; i < lines.size(); ++i) {
            float tw = hud.textWidth(Hud::Green, s, lines[i]);
            float tx = std::round(x + (cw - tw) / 2), ty = top + i * lh;
            hud.rect(tx - 3 * s, ty - 1 * s, tw + 6 * s, lh, 0, 0, 0, 0.6f);
            hud.text(Hud::Green, tx, ty, s, lines[i]);
        }
        hud.end();
    }

    // ---- Options menu ----
    // The game's options screen (FUN_000491d0 and the screens its buttons open), drawn on its own
    // 320x200 art: the buttons down the left at 39, 29 + 16 n; the logo at 159, 67; the mission
    // text (level<n>, finaltxt, or failtxt after death) at 128, 30, a line every 8 pixels; red
    // questions at 145 with YES at 156, 158 and NO at 205, 158.
    enum MenuButton { BReturn, BTutorial, BLoad, BSave, BDifficulty, BQuit, BSettings, BConfigure, BNewGame, BCount };
    // SETTINGS, two pages under tabs (the frame's arrows turn them too): the game options, then
    // sound and picture (the music and sound sliders as the game's own SETTINGS has them,
    // FUN_0004a998).
    // A third, EXTRAS (an addition): the game's cut-scenes to watch, a playlist a row.
    static constexpr int kExtras = 14;
    enum SettingRow { SAI, SAttack, SPickups, SDoors, SStart, SMarkers, SLook, SIntro, SSubtitles,
                      SMusic, SSound, SScreen, SVsync, SRender, SLighting, SBright, SFov, SPixels, SVideos,
                      SExtra0, SCount = SExtra0 + kExtras };
    static int settingPage(int row) { return row < SMusic ? 0 : row < SExtra0 ? 1 : 2; }
    static int pageFirst(int page) { return page == 0 ? (int)SAI : page == 1 ? (int)SMusic : (int)SExtra0; }
    struct ExtraList { const char* label; std::vector<std::string> videos; const char* about; };
    static const ExtraList& extraList(int i) {
        static const ExtraList lists[kExtras] = {
            {"OPENING", {"raven", "int1", "int2", "int3"}, "LOGOS, CREDITS AND THE INTRODUCTION."},
            {"FIRST BRIEFING", {"1a2a", "1a2b", "1a3"}, "BEFORE LEVEL 1, AFTER THE OPENING."},
            {"AFTER LEVEL 1", {"30"}, "BETWEEN LEVEL 1 (BASE) AND LEVEL 2."},
            {"AFTER LEVEL 2", {"31"}, "BETWEEN LEVEL 2 (LAB2) AND LEVEL 3."},
            {"AFTER LEVEL 3", {"4", "32", "33"}, "BETWEEN LEVEL 3 (ALIEN1) AND LEVEL 4."},
            {"AFTER LEVEL 5", {"34"}, "BETWEEN LEVEL 5 (BIOSTOR2) AND LEVEL 6."},
            {"AFTER LEVEL 6", {"7"}, "BETWEEN LEVEL 6 (FAILSAF2) AND LEVEL 7."},
            {"AFTER LEVEL 10", {"11", "36a", "36b"}, "BETWEEN LEVEL 10 (SUBWAY) AND LEVEL 11."},
            {"AFTER LEVEL 11", {"37"}, "BETWEEN LEVEL 11 (SDILAB) AND LEVEL 12."},
            {"AFTER LEVEL 16", {"39"}, "BETWEEN LEVEL 16 (HIVE3) AND LEVEL 19."},
            {"THE END", {"22", "40"}, "AFTER LEVEL 21 (MOTHER3)."},
            {"AFTER SPECIAL OP 1", {"35"}, "AFTER SPECIAL OP 1 (CORE)."},
            {"AFTER SPECIAL OP 2", {"38"}, "AFTER SPECIAL OP 2 (CLONE)."},
            {"NEVER SHOWN", {"14"}, "ON THE DISC, BUT THE GAME NEVER PLAYS IT."},
        };
        return lists[std::clamp(i, 0, kExtras - 1)];
    }
    double extraLength[kExtras] = {};   // seconds, measured the first time the page is shown
    // CONFIGURE: one row per action, then these.
    enum ControlsRow { CSens = kActionCount, CInvert, CReset, CCount };

    // ---- Saved games ----
    // The engine's own format (the game's SAVE?.DAT files are dumps of its memory): in saves\ next
    // to the program, save0.sav to save5.sav.
    std::string saveDir() const { return baseDir() + "saves/"; }
    std::string slotPath(int slot) const {
        return saveDir() + (slot == kQuick ? std::string("quick") : "save" + std::to_string(slot)) + ".sav";
    }
    static const char* saveMagic() { return "CYCLONES SAVE 1"; }   // 16 bytes with its NUL
    // The game numbers its missions 1-24 (its level table at 0x73c9e); a save shows "LEVEL n", or
    // "SPECIAL OP n" for the last three, then the difficulty (FUN_000469e0).
    static int missionNumber(const std::string& name) {
        static const char* m[] = {"BASE", "LAB2", "ALIEN1", "BIOLAB", "BIOSTOR2", "FAILSAF2", "MUSEUM2", "MAINTEN2",
                                  "TREAT2", "SUBWAY", "SDILAB", "SDI1", "SDI2", "HIVE1", "HIVE2", "HIVE3",
                                  "HIVE4", "HIVE5", "MOTHER1", "MOTHER2", "MOTHER3", "CORE", "CLONE", "HEAVY"};
        for (int i = 0; i < 24; ++i)
            if (name == m[i]) return i + 1;
        return 0;
    }
    static std::string whereLine(const std::string& levelName, int difficulty) {
        static const char* d[3] = {"EASY", "MEDIUM", "HARD"};
        int n = missionNumber(levelName);
        std::string w = n >= 22 ? "SPECIAL OP " + std::to_string(n - 21) : n > 0 ? "LEVEL " + std::to_string(n) : levelName;
        return w + " DIFFICULTY: " + d[std::clamp(difficulty, 0, 2)];
    }
    // The date as the game shows it (ctime(), in capitals): "MON OCT 17 22:51:00 1994".
    static std::string whenLine(int64_t seconds) {
        SDL_DateTime t;
        if (!SDL_TimeToDateTime((SDL_Time)seconds * SDL_NS_PER_SECOND, &t, true)) return "";
        static const char* days[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
        static const char* months[12] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
        char buf[48];
        SDL_snprintf(buf, sizeof(buf), "%s %s %2d %02d:%02d:%02d %d", days[std::clamp(t.day_of_week, 0, 6)],
                     months[std::clamp(t.month - 1, 0, 11)], t.day, t.hour, t.minute, t.second, t.year);
        return buf;
    }
    static int64_t nowSeconds() {
        SDL_Time t = 0;
        SDL_GetCurrentTime(&t);
        return (int64_t)(t / SDL_NS_PER_SECOND);
    }
    static constexpr size_t kThumbBytes = (size_t)Hud::kThumbW * Hud::kThumbH * 3;

    bool writeSlot(int slot, std::string& err) {
        std::vector<uint8_t> state, out;
        world.saveState(state);
        auto put = [&](const void* p, size_t n) { out.insert(out.end(), (const uint8_t*)p, (const uint8_t*)p + n); };
        char magic[16] = {};
        std::memcpy(magic, saveMagic(), std::strlen(saveMagic()));
        put(magic, 16);
        put(&saveTime, 8);
        char desc[32] = {}, lv[16] = {};
        std::memcpy(desc, saveDesc.data(), std::min<size_t>(saveDesc.size(), 30));
        std::memcpy(lv, level.name.data(), std::min<size_t>(level.name.size(), 15));
        put(desc, 32);
        put(lv, 16);
        uint8_t info[4] = {(uint8_t)std::clamp(world.difficulty, 0, 2), (uint8_t)world.enhancedAI, 0, 0};
        put(info, 4);
        std::vector<uint8_t> thumb = newThumb;
        thumb.resize(kThumbBytes, 0);
        put(thumb.data(), thumb.size());
        float view[2] = {cam.yaw, cam.pitch};
        put(view, 8);
        uint32_t n = (uint32_t)state.size();
        put(&n, 4);
        put(state.data(), state.size());
        // After the state (engines before 0.5.2aa stop reading there): the maps' zoom.
        float zoom[2] = {mapZoom[Automap::Mini], mapZoom[Automap::Full]};
        put("MAPZ", 4);
        put(zoom, 8);
        SDL_CreateDirectory(saveDir().c_str());
        if (!SDL_SaveFile(slotPath(slot).c_str(), out.data(), out.size())) {
            err = SDL_GetError();
            return false;
        }
        return true;
    }
    // Reads a slot's description and picture, and if asked what's needed to load it. False if the
    // slot is empty or can't be loaded (info.used tells which).
    bool readSlot(int slot, SlotInfo& info, std::vector<uint8_t>* state = nullptr, std::string* levelName = nullptr,
                  int* difficulty = nullptr, float* yaw = nullptr, float* pitch = nullptr, float* zoom = nullptr) {
        info = SlotInfo();
        size_t size = 0;
        void* raw = SDL_LoadFile(slotPath(slot).c_str(), &size);
        if (!raw) return false;
        std::vector<uint8_t> b((const uint8_t*)raw, (const uint8_t*)raw + size);
        SDL_free(raw);
        info.used = true;
        info.desc = "UNREADABLE SAVE";
        const size_t head = 16 + 8 + 32 + 16 + 4 + kThumbBytes + 8 + 4;
        if (b.size() < head || std::memcmp(b.data(), saveMagic(), std::strlen(saveMagic()) + 1) != 0) return false;
        size_t p = 16;
        int64_t t;
        std::memcpy(&t, &b[p], 8);
        p += 8;
        std::string desc((const char*)&b[p], strnlen((const char*)&b[p], 30));
        p += 32;
        std::string lv((const char*)&b[p], strnlen((const char*)&b[p], 15));
        p += 16;
        int diff = std::clamp((int)b[p], 0, 2);
        p += 4;
        info.thumb.assign(b.begin() + p, b.begin() + p + kThumbBytes);
        p += kThumbBytes;
        float view[2];
        std::memcpy(view, &b[p], 8);
        p += 8;
        uint32_t n;
        std::memcpy(&n, &b[p], 4);
        p += 4;
        if (p + n > b.size()) return false;
        info.desc = desc;
        info.where = whereLine(lv, diff);
        info.when = whenLine(t);
        if (std::find(levels.begin(), levels.end(), lv) == levels.end()) return false;   // a level this disc doesn't have
        info.readable = true;
        if (state) state->assign(b.begin() + p, b.begin() + p + n);
        if (zoom && p + n + 12 <= b.size() && std::memcmp(&b[p + n], "MAPZ", 4) == 0) {   // the maps' zoom (0.5.2aa on)
            std::memcpy(zoom, &b[p + n + 4], 8);
        } else if (zoom) {
            zoom[0] = zoom[1] = -1;
        }
        if (levelName) *levelName = lv;
        if (difficulty) *difficulty = diff;
        if (yaw) *yaw = view[0];
        if (pitch) *pitch = view[1];
        return true;
    }
    // The slot on show: its description, and its picture in the HUD's atlas.
    void showSlot() {
        if (slotRead == saveSlot) return;
        readSlot(saveSlot, slotInfo);
        hud.setThumb(slotInfo.thumb.size() == kThumbBytes ? slotInfo.thumb.data() : nullptr);
        slotRead = saveSlot;
    }
    // Quick save and load (an addition): F5 saves to the Q slot at once, with the view as its
    // picture and "QUICK SAVE" as its description, when SAVE could; F9 loads it at once (also from
    // the death screen and the options screen).
    bool quickSavePending = false;
    void quickSave() {
        if (!buttonEnabled(BSave)) { world.message("YOU CAN'T SAVE NOW.", 0); return; }
        quickSavePending = true;   // done once the frame is drawn (its picture)
    }
    void finishQuickSave(int w, int h) {
        quickSavePending = false;
        newThumb = renderer.thumbnail(w, h, Hud::kThumbW, Hud::kThumbH);
        saveTime = nowSeconds();
        saveDesc = "QUICK SAVE";
        std::string err;
        if (!writeSlot(kQuick, err)) {
            SDL_Log("Quick save failed: %s", err.c_str());
            world.message("QUICK SAVE FAILED.", 0);
            return;
        }
        lastSlot = kQuick;
        saveSettings();
        slotRead = -1;
        world.message("GAME SAVED (QUICK SAVE, F9 TO LOAD).", 0);
    }
    bool quickLoad() {
        SDL_PathInfo pi;
        if (!SDL_GetPathInfo(slotPath(kQuick).c_str(), &pi) || pi.type != SDL_PATHTYPE_FILE) {
            world.message("NO QUICK SAVE YET (F5 MAKES ONE).", 0);
            return false;
        }
        if (!loadSlot(kQuick)) { world.message("QUICK LOAD FAILED.", 0); return false; }
        slotRead = -1;
        world.message("QUICK SAVE LOADED.", 0);
        return true;
    }
    // Where LOAD and SAVE open (an addition; the game started at slot 0): the slot last used. With
    // none yet (or it has since been emptied, for LOAD), LOAD shows the newest saved game and SAVE
    // the first empty slot.
    int defaultSlot(int panel) {
        auto used = [&](int k) { SDL_PathInfo i; return SDL_GetPathInfo(slotPath(k).c_str(), &i) && i.type == SDL_PATHTYPE_FILE; };
        if (lastSlot >= 0 && lastSlot < kSlots && (panel == BSave || used(lastSlot))) return lastSlot;
        if (panel == BSave) {
            for (int k = 0; k < kQuick; ++k)
                if (!used(k)) return k;
            return 0;
        }
        int best = 0;
        SDL_Time newest = 0;
        for (int k = 0; k < kSlots; ++k) {
            SDL_PathInfo i;
            if (SDL_GetPathInfo(slotPath(k).c_str(), &i) && i.type == SDL_PATHTYPE_FILE && i.modify_time > newest) {
                newest = i.modify_time;
                best = k;
            }
        }
        return best;
    }
    void turnSlot(int d) {
        saveSlot = (saveSlot + d + kSlots) % kSlots;
        slotNote.clear();
    }
    void stopTyping() {
        if (slotStage == SlotTyping) SDL_StopTextInput(window);
    }
    // SAVE, once the description is in: write the slot, and back to the options screen as the game
    // does.
    void finishSave() {
        std::string err;
        slotStage = SlotAsk;
        slotRead = -1;
        if (!writeSlot(saveSlot, err)) {
            SDL_Log("Saving to %s failed: %s", slotPath(saveSlot).c_str(), err.c_str());
            slotNote = "SAVE GAME FAILED";
            return;
        }
        SDL_Log("Saved %s to %s", level.name.c_str(), slotPath(saveSlot).c_str());
        lastSlot = saveSlot;
        saveSettings();
        closePanel();
    }
    // LOAD: the level again from the disc, at the save's difficulty, then the save over it.
    bool loadSlot(int slot) {
        SlotInfo info;
        std::vector<uint8_t> state;
        std::string lv, err;
        int diff = 1;
        float yaw = 0, pitch = 0, zoom[2] = {-1, -1};
        if (!readSlot(slot, info, &state, &lv, &diff, &yaw, &pitch, zoom)) return false;
        // The maps come back at the zoom they were saved with (older saves keep the present one).
        if (zoom[0] > 0) mapZoom[Automap::Mini] = clampZoom(Automap::Mini, zoom[0]);
        if (zoom[1] > 0) mapZoom[Automap::Full] = clampZoom(Automap::Full, zoom[1]);
        int idx = (int)(std::find(levels.begin(), levels.end(), lv) - levels.begin());
        world.difficulty = diff;
        saveSettings();
        loadLevel(idx);
        if (!world.loadState(state, err)) {
            SDL_Log("Loading %s failed: %s", slotPath(slot).c_str(), err.c_str());
            return false;
        }
        const PlayerState& p = world.player();
        cam.x = p.x;
        cam.z = p.z;
        cam.y = p.viewY;
        cam.yaw = yaw;
        cam.pitch = pitch;
        firstPerson.reset();
        deathMenu = false;
        SDL_Log("Loaded %s from %s", lv.c_str(), slotPath(slot).c_str());
        lastSlot = slot;
        saveSettings();
        return true;
    }
    void finishLoad() {
        slotStage = SlotAsk;
        if (!loadSlot(saveSlot)) {
            slotNote = "LOAD GAME FAILED";
            menuYes = 1;
            return;
        }
        openMenu(false);
    }
    // The red messages stay up for a frame before the work is done, as the game draws them.
    int slotFrames = 0;
    void slotWork() {
        if (!menuOpen || (slotStage != SlotSaving && slotStage != SlotLoading)) return;
        if (++slotFrames < 2) return;
        slotFrames = 0;
        if (slotStage == SlotSaving) finishSave();
        else finishLoad();
    }
    // Typing the description: capitals, figures and the font's signs, up to 30; Enter ends it.
    void slotText(const char* text) {
        if (slotStage != SlotTyping) return;
        for (const char* c = text; *c; ++c) {
            int ch = (unsigned char)*c;
            if (ch >= 'a' && ch <= 'z') ch -= 32;
            if (ch < 0x20 || ch > 0x5f || ch == '$') continue;
            saveDesc += (char)ch;
            if (saveDesc.size() >= 30) { stopTyping(); slotStage = SlotSaving; slotFrames = 0; return; }
        }
    }

    bool buttonEnabled(int b) const {
        if (b == BSave) return world.player().alive && !finished && level.name != "TUTOR";   // not when dead, nor in the tutorial
        if (b == BReturn) return world.player().alive && !finished;
        if (b == BTutorial) return std::find(levels.begin(), levels.end(), std::string("TUTORIAL")) != levels.end();
        return true;
    }

    void openMenu(bool open) {
        menuOpen = open;
        menuPanel = -1;
        panelFocus = false;
        bindWait = -1;
        menuBtn = world.player().alive && !finished ? BReturn : BNewGame;
        menuMouseX = menuMouseY = -1;   // the game's cursor shows once the mouse moves
        SDL_SetWindowRelativeMouseMode(window, !open);
        if (open) SDL_HideCursor();   // the menu draws the game's own cursor
        else SDL_ShowCursor();
    }

    void openPanel(int b) {
        menuBtn = b;
        menuPanel = b;
        panelFocus = true;
        panelSel = 0;
        menuYes = 1;   // the game lights NO first (FUN_0004a6a0)
        bindWait = -1;
        pendingDifficulty = std::clamp(world.difficulty, 0, 2);
        stopTyping();
        slotStage = SlotAsk;
        slotNote.clear();
        slotRead = -1;
        settingsPage = 0;
        sliderDrag = -1;
        if (b == BLoad || b == BSave) saveSlot = defaultSlot(b);
    }
    void closePanel() {
        stopTyping();
        slotStage = SlotAsk;
        slotNote.clear();
        if (menuPanel >= 0) menuBtn = menuPanel;
        menuPanel = -1;
        panelFocus = false;
        bindWait = -1;
    }

    void activate(int b) {
        if (!buttonEnabled(b)) return;
        if (b == BReturn) { openMenu(false); return; }
        openPanel(b);
    }

    void loadNamed(const char* name) {
        auto it = std::find(levels.begin(), levels.end(), std::string(name));
        loadLevel(it == levels.end() ? 0 : (int)(it - levels.begin()));
    }

    // YES or NO on the panel that asks (or on the restart question after death).
    void confirm(bool yes) {
        switch (menuPanel) {
        case -1:   // dead
            if (yes) { loadLevel(levelIndex); openMenu(false); }
            else { panelFocus = false; menuBtn = BNewGame; }
            return;
        case BQuit:
            if (yes) quitRequested = true;
            else closePanel();
            return;
        case BNewGame:
        case BTutorial:
            if (!yes) { closePanel(); return; }
            loadNamed(menuPanel == BNewGame ? "BASE" : "TUTORIAL");
            openMenu(false);
            return;
        case BLoad:
            if (!yes) { closePanel(); return; }
            showSlot();
            if (!slotInfo.used) return;   // an empty slot: nothing happens
            if (!slotInfo.readable) { slotNote = "LOAD GAME FAILED"; return; }
            slotStage = SlotLoading;
            slotFrames = 0;
            return;
        case BSave:
            if (!yes) { closePanel(); return; }
            // The view as it is now becomes the slot's picture; then the description is typed in.
            grabThumb = true;
            saveTime = nowSeconds();
            saveDesc.clear();
            slotNote.clear();
            slotStage = SlotTyping;
            SDL_StartTextInput(window);
            return;
        case BDifficulty:
            if (!yes) { closePanel(); return; }
            world.difficulty = pendingDifficulty;
            saveSettings();
            loadLevel(levelIndex);
            openMenu(false);
            return;
        }
    }
    bool asks() const {   // the open panel is a YES / NO question
        return (menuPanel == -1 && !world.player().alive) || menuPanel == BQuit || menuPanel == BNewGame ||
               menuPanel == BTutorial || menuPanel == BDifficulty ||
               ((menuPanel == BLoad || menuPanel == BSave) && slotStage == SlotAsk);
    }

    std::string settingLabel(int i) const {
        static const char* n[SExtra0] = {"MONSTER AI", "MONSTER ATTACKS", "PICK UP ITEMS", "OPEN DOORS", "START WEAPONS",
                                         "MAP MARKERS", "LOOK", "OPENING", "SUBTITLES", "MUSIC", "SOUND", "SCREEN", "VSYNC",
                                         "RENDERING", "LIGHTING", "BRIGHTNESS", "FIELD OF VIEW", "PICTURE", "CUT-SCENES"};
        if (i >= SExtra0) return extraList(i - SExtra0).label;
        return n[i];
    }
    std::string settingValue(int i) const {
        static const char* marks[] = {"NONE", "PICKUPS", "ALL"};
        static const char* renders[] = {"FULL RESOLUTION", "HALF RESOLUTION", "320X200"};
        switch (i) {
        case SMusic: return std::to_string(musicVolume);
        case SSound: return std::to_string(soundVolume);
        case SScreen: return opt.fullscreen ? "FULL SCREEN" : "WINDOW";
        case SVsync: return opt.vsync ? "ON" : "OFF";
        case SRender: return renders[std::clamp(renderMode, 0, 2)];
        case SLighting: return rs.lighting ? "ORIGINAL" : "FULL BRIGHT";
        case SAI: return world.enhancedAI ? "ENHANCED" : "ORIGINAL";
        case SAttack: return world.monstersAttack ? "ON" : "OFF";
        case SPickups: return world.autoPickup ? "WALK OVER" : "USE KEY ONLY";
        case SDoors: return world.bumpDoors ? "WALK INTO" : "USE KEY ONLY";
        case SStart: return world.startWithAllWeapons ? "ALL" : "GAUSS PISTOL";
        case SMarkers: return marks[std::clamp(mapMarkers, 0, 2)];
        case SLook: return world.smooth ? "SMOOTH" : "CLASSIC";
        case SIntro: return introVideos ? "VIDEOS" : "SKIP";
        case SFov: return std::to_string((int)std::lround(rs.hFovDeg4x3)) + " DEGREES";
        case SPixels: return rs.tallPixels ? "TALL PIXELS" : "SQUARE PIXELS";
        case SVideos: return !hdPack.loaded() ? "ORIGINAL" : enhancedVideos ? "ENHANCED" : "ORIGINAL";
        case SBright: return brightness == 0 ? "0 (THE GAME'S)" : std::to_string(brightness);
        case SSubtitles: return !hdPack.hasSubtitles() ? "NONE FOUND" : subtitles ? "ON" : "OFF";
        }
        if (i >= SExtra0) {
            double t = extraLength[i - SExtra0];
            int sec = (int)std::lround(t);
            char b[16];
            SDL_snprintf(b, sizeof(b), "%d:%02d", sec / 60, sec % 60);
            return t > 0 ? b : "";
        }
        return "";
    }
    std::string settingHint(int i) const {
        if (i >= SExtra0) {
            const ExtraList& l = extraList(i - SExtra0);
            std::string v;
            for (const auto& n : l.videos) v += (v.empty() ? "" : ", ") + Bindings::upper(n);
            return std::string(l.about) + " (" + v + ")";
        }
        switch (i) {
        case SMusic: return tracksFound ? "THE CD MUSIC, A TRACK FOR EACH MISSION (THE TUTORIAL HAS NONE). LEFT / RIGHT OR DRAG."
                                        : "NO MUSIC: THIS DISC IMAGE HAS NO AUDIO TRACKS (AN .ISO HAS NONE).";
        case SSound: return soundsFound ? "THE GAME'S SOUND EFFECTS. LEFT / RIGHT OR DRAG."
                                        : "NO SOUND EFFECTS: THEY COME FROM DATA.A00 ON THE DISC IMAGE.";
        case SScreen: return "FULL SCREEN AT THE DESKTOP'S RESOLUTION, OR A WINDOW. F11 OR ALT+ENTER SWITCH TOO.";
        case SVsync: return "ON: EACH FRAME WAITS FOR THE DISPLAY, NO TEARING. OFF: AS MANY FRAMES AS POSSIBLE.";
        case SRender: return "THE 3D VIEW AT THE SCREEN'S OWN RESOLUTION, AT HALF OF IT (FASTER), OR IN THE GAME'S 320X200 PIXELS.";
        case SLighting: return "ORIGINAL: THE GAME'S OWN SHADING, DARKER WITH DISTANCE. FULL BRIGHT: NO SHADING.";
        case SAI: return world.enhancedAI ? "SIGHT, HEARING, MEMORY, PATHFINDING AND TEAMWORK. ORIGINAL: AS IN 1994."
                                          : "THE 1994 BEHAVIOUR: DISTANCE ONLY, NO MEMORY, NO PATHFINDING.";
        case SAttack: return "OFF: MONSTERS WANDER AND NEVER ATTACK.";
        case SPickups: return "THE GAME TOOK ITEMS ONLY WITH USE (RIGHT CLICK), AIMED AT ONE UP TO TWO CELLS AWAY.";
        case SDoors: return "THE GAME OPENED DOORS ONLY WITH USE. THE CURSOR TURNS PINK ON DOORS, HIDDEN ONES TOO.";
        case SStart: return "THE GAME GAVE YOU THE GAUSS PISTOL. CHANGING THIS RESTARTS THE LEVEL.";
        case SMarkers: return "WHAT THE AUTOMAP SHOWS BESIDES WALLS AND DOORS. ALL: PICKUPS AND CREATURES.";
        case SLook: return "SMOOTH TURNING AND ANIMATION, OR THE GAME'S OWN 8 ANGLES.";
        case SIntro: return "AT START: THE GAME'S OPENING VIDEOS AND FIRST BRIEFING (A KEY SKIPS EACH PART), THEN THIS SCREEN; OR THIS SCREEN AT ONCE.";
        case SFov: return "DEGREES ACROSS A 4:3 SCREEN. THE GAME SHOWED 90. WIDER SCREENS SEE MORE.";
        case SPixels: return rs.tallPixels ? "AS ON A 1994 MONITOR: 320X200 SHOWN AT 4:3, EVERY PIXEL 1.2 TIMES AS TALL AS WIDE."
                                           : "GEOMETRICALLY TRUE PROPORTIONS, FLATTER THAN THE GAME LOOKED.";
        case SBright: return "LIFTS THE DARK PARTS OF THE 3D VIEW (0 TO 10). THE STATUS DISPLAY, MAP AND SCREENS STAY AS THEY ARE.";
        case SSubtitles: return !hdPack.hasSubtitles() ? "THE SUBTITLES ARE IN THE HD VIDEO FOLDER, WHICH ISN'T BESIDE THE PROGRAM."
                                                       : "WHAT IS SAID IN THE BRIEFINGS, UNDER THE PICTURE (IN THE GAME AND IN EXTRAS).";
        case SVideos: return !hdPack.loaded() ? "ENHANCED NEEDS THE HD VIDEO FOLDER BESIDE THE PROGRAM."
                             : enhancedVideos ? "THE BRIEFINGS' VIDEO AND SPEECH CLEANED UP. THE REST AS IN THE GAME."
                                              : "THE GAME'S OWN 320X200 VIDEOS AND 8-BIT SPEECH.";
        }
        return "";
    }
    void setFullscreen(bool on) {
        opt.fullscreen = on;
        SDL_SetWindowFullscreen(window, on);
    }
    // A volume slider to a value (0-100); the sound slider plays a sound at the new level.
    void setVolume(int row, int v, bool feedback) {
        v = std::clamp(v, 0, 100);
        int& vol = row == SMusic ? musicVolume : soundVolume;
        if (v == vol) return;
        vol = v;
        applyVolumes();
        if (row == SSound && feedback) audio.play(81);
    }
    void changeSetting(int i, int dir) {
        switch (i) {
        case SMusic:
        case SSound: {   // steps of 5 (to the nearest 5 first)
            int v = i == SMusic ? musicVolume : soundVolume;
            setVolume(i, dir < 0 ? (v - 1) / 5 * 5 : (v / 5 + 1) * 5, true);
            break;
        }
        case SScreen: setFullscreen(!opt.fullscreen); break;
        case SVsync: opt.vsync = !opt.vsync; SDL_GL_SetSwapInterval(opt.vsync); break;
        case SRender: renderMode = (renderMode + (dir < 0 ? 2 : 1)) % 3; applyRenderMode(); break;
        case SLighting: rs.lighting = !rs.lighting; break;
        case SAI: world.enhancedAI = !world.enhancedAI; break;
        case SAttack: world.monstersAttack = !world.monstersAttack; break;
        case SPickups: world.autoPickup = !world.autoPickup; break;
        case SDoors: world.bumpDoors = !world.bumpDoors; break;
        case SStart: world.startWithAllWeapons = !world.startWithAllWeapons; loadLevel(levelIndex); break;
        case SMarkers: mapMarkers = (mapMarkers + (dir < 0 ? 2 : 1)) % 3; break;
        case SLook: world.smooth = !world.smooth; break;
        case SIntro: introVideos = !introVideos; break;
        case SFov: rs.hFovDeg4x3 = std::clamp(rs.hFovDeg4x3 + (dir < 0 ? -5.0f : 5.0f), 50.0f, 120.0f); break;
        case SPixels: rs.tallPixels = !rs.tallPixels; break;
        case SVideos: enhancedVideos = !enhancedVideos; applyVideoSetting(); break;
        case SBright: brightness = std::clamp(brightness + (dir < 0 ? -1 : 1), 0, 10); break;
        case SSubtitles: subtitles = !subtitles; break;
        default:
            if (i >= SExtra0 && dir > 0) { openPlayer(i - SExtra0); return; }
            return;
        }
        saveSettings();
    }
    void turnSettingsPage(int page) {
        settingsPage = std::clamp(page, 0, 2);
        panelSel = pageFirst(settingsPage);
        panelFocus = true;
    }

    static const char* actionName(int a) {
        static const char* n[] = {"FORWARD", "BACK", "STRAFE LEFT", "STRAFE RIGHT", "RUN", "JUMP", "FIRE",
                                  "USE", "BIO-KIT", "MECH-KIT", "NEXT WEAPON", "PREV WEAPON", "AUTOMAP",
                                  "INVENTORY", "DETONATE", "JUMP JETS"};
        return n[a];
    }
    void changeControl(int i, int dir) {
        if (i < kActionCount) { bindWait = i; return; }
        switch (i) {
        case CSens: mouseSens = std::clamp(std::round((mouseSens + (dir < 0 ? -0.1f : 0.1f)) * 10) / 10, 0.1f, 5.0f); break;
        case CInvert: invertMouse = !invertMouse; break;
        case CReset: bindings.defaults(); mouseSens = 1.0f; invertMouse = false; break;
        }
        saveSettings();
    }

    // A key, button or wheel turn while an action waits for its new input.
    void menuBind(int code) {
        if (bindWait < 0 || bindWaitRelease) return;
        bindings.bind((Action)bindWait, code);
        bindWait = -1;
        saveSettings();
    }

    void menuKey(SDL_Keycode k, SDL_Scancode sc) {
        if (k == SDLK_F9 && bindWait < 0 && slotStage == SlotAsk) {   // the quick save, from here too
            if (quickLoad()) openMenu(false);
            return;
        }
        if (bindWait >= 0) {
            if (k == SDLK_ESCAPE) bindWait = -1;
            else if (k == SDLK_BACKSPACE || k == SDLK_DELETE) { bindings.clear((Action)bindWait); bindWait = -1; saveSettings(); }
            else menuBind((int)sc);
            return;
        }
        if (slotStage == SlotTyping) {   // the description (letters come as text input)
            if (k == SDLK_ESCAPE) { stopTyping(); slotStage = SlotAsk; slotRead = -1; }
            else if (k == SDLK_BACKSPACE) { if (!saveDesc.empty()) saveDesc.pop_back(); }
            else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) { stopTyping(); slotStage = SlotSaving; slotFrames = 0; }
            return;
        }
        if (slotStage != SlotAsk) return;   // saving or loading
        bool up = k == SDLK_UP || k == SDLK_W, down = k == SDLK_DOWN || k == SDLK_S;
        bool left = k == SDLK_LEFT || k == SDLK_A, right = k == SDLK_RIGHT || k == SDLK_D;
        bool enter = k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE;
        bool alive = world.player().alive;
        if (!panelFocus) {   // the buttons
            if (k == SDLK_ESCAPE) {
                if (alive && !finished) openMenu(false);
                else if (!alive) panelFocus = menuPanel == -1;   // back to the restart question
            } else if (up || down) {
                for (int n = 0; n < BCount; ++n) {
                    menuBtn = (menuBtn + (up ? BCount - 1 : 1)) % BCount;
                    if (buttonEnabled(menuBtn)) break;
                }
            } else if (right || enter) activate(menuBtn);
            return;
        }
        if (k == SDLK_ESCAPE) {
            if (menuPanel >= 0) closePanel();
            else panelFocus = false;
            return;
        }
        if (asks()) {
            if (k == SDLK_Y) { confirm(true); return; }
            if (k == SDLK_N) { confirm(false); return; }
            if (menuPanel == BDifficulty && (up || down)) pendingDifficulty = (pendingDifficulty + (up ? 2 : 1)) % 3;
            else if ((menuPanel == BLoad || menuPanel == BSave) && (up || down || k == SDLK_PAGEUP || k == SDLK_PAGEDOWN))
                turnSlot(up || k == SDLK_PAGEUP ? -1 : 1);
            else if (left || right || up || down) menuYes ^= 1;
            else if (enter) confirm(menuYes == 0);
            return;
        }
        int rows = menuPanel == BSettings ? (int)SCount : (int)CCount;
        if (menuPanel == BSettings && (k == SDLK_PAGEDOWN || k == SDLK_PAGEUP || k == SDLK_TAB))
            turnSettingsPage(k == SDLK_PAGEUP ? settingsPage - 1 : k == SDLK_PAGEDOWN ? settingsPage + 1 : (settingsPage + 1) % 3);
        else if (up || down) {
            panelSel = (panelSel + (up ? rows - 1 : 1)) % rows;
            if (menuPanel == BSettings) settingsPage = settingPage(panelSel);   // the list runs on over both pages
        } else if (menuPanel == BSettings) {
            if (left) changeSetting(panelSel, -1);
            else if (right || enter) changeSetting(panelSel, 1);
        } else if (menuPanel == BConfigure) {
            if (panelSel < kActionCount && (k == SDLK_BACKSPACE || k == SDLK_DELETE)) { bindings.clear((Action)panelSel); saveSettings(); }
            else if (left && panelSel >= kActionCount) changeControl(panelSel, -1);
            else if (right || enter) changeControl(panelSel, 1);
        }
    }

    // The mouse (window coordinates), in the 320x200 screen's pixels.
    void menuPoint(float x, float y) {
        int ww, wh, pw, ph;
        SDL_GetWindowSize(window, &ww, &wh);
        SDL_GetWindowSizeInPixels(window, &pw, &ph);
        float wx = ww > 0 ? x * pw / ww : x, wy = wh > 0 ? y * ph / wh : y;
        menuMouseX = (wx - menuCanvas[0]) * 320.0f / menuCanvas[2];
        menuMouseY = (wy - menuCanvas[1]) * 200.0f / menuCanvas[3];
    }
    const MenuHit* menuHitAt() const {
        for (const auto& h : menuHits)
            if (menuMouseX >= h.x && menuMouseX < h.x + h.w && menuMouseY >= h.y && menuMouseY < h.y + h.h) return &h;
        return nullptr;
    }
    void menuHover() {
        const MenuHit* h = menuHitAt();
        if (!h || bindWait >= 0) return;
        if (h->kind == HitButton) {
            if (menuPanel < 0 && buttonEnabled(h->index)) { menuBtn = h->index; panelFocus = false; }
        } else if (h->kind == HitYes || h->kind == HitNo) {
            menuYes = h->kind == HitNo;
            panelFocus = true;
        } else if ((h->kind == HitRow && menuPanel != BDifficulty) || h->kind == HitSlider) {
            panelSel = h->index;
            panelFocus = true;
        }
    }
    void menuClick(int button) {
        if (bindWait >= 0) { menuBind(kMouseBase + button); return; }
        const MenuHit* h = menuHitAt();
        if (!h) return;
        int dir = button == SDL_BUTTON_RIGHT ? -1 : 1;
        switch (h->kind) {
        case HitButton: activate(h->index); break;
        case HitYes: confirm(true); break;
        case HitNo: confirm(false); break;
        case HitPage: turnSettingsPage(h->index >= 100 ? settingsPage + (h->index == 101 ? 1 : -1) : h->index); break;
        case HitSlot: if (slotStage == SlotAsk) turnSlot(h->index); break;
        case HitSlotPick: if (slotStage == SlotAsk) turnSlot(h->index - saveSlot); break;
        case HitSlider:
            panelSel = h->index;
            panelFocus = true;
            if (menuMouseX >= 150 && button == SDL_BUTTON_LEFT) {   // on the bar: the knob goes there, and follows
                setVolume(h->index, (int)std::lround(menuMouseX - 167), false);
                sliderDrag = h->index;
            } else {
                changeSetting(h->index, dir);
            }
            break;
        case HitRow:
            panelFocus = true;
            if (menuPanel == BDifficulty) pendingDifficulty = h->index;
            else if (menuPanel == BSettings) { panelSel = h->index; changeSetting(h->index, dir); }
            else if (menuPanel == BConfigure) {
                panelSel = h->index;
                changeControl(h->index, dir);
                if (bindWait >= 0) bindWaitRelease = true;   // don't take this very click as the new button
            }
            break;
        }
    }

    // A text lump (level<n>, finaltxt, failtxt): lines ending in NUL, up to the 01 01 02 03... tail.
    std::vector<std::string> textLump(const std::string& name) {
        std::vector<std::string> lines;
        int i = data.hd.find(name);
        if (i < 0) return lines;
        const uint8_t* d = data.hd.data(i);
        size_t n = data.hd.size(i), p = 0;
        while (p < n && d[p] >= 0x20) {
            size_t q = p;
            while (q < n && d[q]) ++q;
            lines.emplace_back((const char*)d + p, q - p);
            p = q + 1;
        }
        return lines;
    }
    // What the screen says: the mission briefing for this level (the game's level table, at
    // 0x73d1e in CYCLONE.EXE, numbers the missions), the opening text, or the report after death.
    std::vector<std::string> menuStory() {
        if (!world.player().alive) return textLump("failtxt");
        if (finished) return textLump("finaltxt");
        static const char* missions[] = {"BASE", "LAB2", "ALIEN1", "BIOLAB", "BIOSTOR2", "FAILSAF2", "MUSEUM2", "MAINTEN2",
                                         "TREAT2", "SUBWAY", "SDILAB", "SDI1", "SDI2", "HIVE1", "HIVE2", "HIVE3",
                                         "HIVE4", "HIVE5", "MOTHER1", "MOTHER2", "MOTHER3", "CORE", "CLONE", "HEAVY"};
        for (int m = 0; m < 24; ++m)
            if (level.name == missions[m]) {
                auto lines = textLump("level" + std::to_string(m + 1));
                if (!lines.empty()) return lines;
            }
        return textLump("finaltxt");
    }

    // Words wrapped to a width in one of the game's fonts.
    std::vector<std::string> wrap(Hud::Font f, const std::string& s, float width) {
        std::vector<std::string> out;
        std::string line, word;
        auto flush = [&]() {
            if (word.empty()) return;
            std::string t = line.empty() ? word : line + " " + word;
            if (!line.empty() && hud.textWidth(f, 1, t) > width) { out.push_back(line); line = word; }
            else line = t;
            word.clear();
        };
        for (char c : s) {
            if (c == ' ') flush();
            else word += c;
        }
        flush();
        if (!line.empty()) out.push_back(line);
        return out;
    }
    std::string fit(Hud::Font f, std::string s, float width) {
        while (s.size() > 1 && hud.textWidth(f, 1, s) > width) s.pop_back();
        return s;
    }

    // Lettering stamped into the options screen's metal frame, centred on x.
    void engrave(const std::string& t, float cx, float y) {
        float x = std::round(cx - hud.textWidth(Hud::Engraved, 1, t) / 2);
        hud.text(Hud::EngravedEdge, x + 1, y + 1, 1, t);
        hud.text(Hud::Engraved, x, y, 1, t);
    }
    void drawYesNo(float y) {
        hud.image((Hud::Image)(menuYes == 0 ? Hud::MenuLit + 10 : Hud::MenuYes), 156, y, 1);
        hud.image((Hud::Image)(menuYes == 1 ? Hud::MenuLit + 9 : Hud::MenuNo), 205, y, 1);
        menuHits.push_back({156, y, 37, 15, HitYes, 0});
        menuHits.push_back({205, y, 37, 15, HitNo, 0});
    }
    void drawQuestion(const std::string& q, float x, float y) {
        if (x < 0) x = std::round(199 - hud.textWidth(Hud::Red, 1, q) / 2);   // centred over YES and NO
        hud.text(Hud::Red, x, y, 1, q);
    }

    void drawMenu(int w, int h) {
        // The 320x200 screen fills the window's height at 4:3 (or 16:10 with square pixels).
        float aspect = rs.tallPixels ? 4.0f / 3.0f : 1.6f;
        float cw = std::min((float)w, h * aspect), ch = cw / aspect;
        menuCanvas[0] = std::round((w - cw) / 2);
        menuCanvas[1] = std::round((h - ch) / 2);
        menuCanvas[2] = cw;
        menuCanvas[3] = ch;
        hud.begin(w, h);
        hud.rect(0, 0, (float)w, (float)h, 0, 0, 0, 0.86f);
        hud.end();
        hud.beginCanvas(320, 200, (int)std::ceil(std::max(cw / 320, ch / 200)));
        hud.setShadow(false);
        hud.image(Hud::MenuScreen, 0, 0, 1);
        // The title, stamped into the top of the monitor's frame like a maker's name; the version
        // and the port's author on the frame below the screen, either side of the arrows.
        engrave("CYCLONES: REMASTERED", 160, 14);
        engrave(kVersionTag, 68, 182);
        engrave("MAURICE V. DE CASTRO", 248, 182);
        menuHits.clear();
        bool alive = world.player().alive;

        int lit = menuPanel >= 0 ? menuPanel : panelFocus ? -1 : menuBtn;
        for (int b = 0; b < BCount; ++b) {
            float y = 29 + 16.0f * b;
            int img = Hud::MenuButton + b;
            if (b == BReturn && !buttonEnabled(b)) img = Hud::MenuOffReturn;
            else if (b == BSave && !buttonEnabled(b)) img = Hud::MenuOffSave;
            else if (b == lit) img = Hud::MenuLit + b;
            hud.image((Hud::Image)img, 39, y, 1);
            menuHits.push_back({39, y, 72, 15, HitButton, b});
        }

        const Hud::Font green = Hud::Green, amber = Hud::Amber, grey = Hud::Grey, red = Hud::Red;
        switch (menuPanel) {
        case -1: {
            hud.image(Hud::MenuLogo, 159, 67, 1);
            auto lines = menuStory();
            for (size_t i = 0; i < lines.size(); ++i) hud.text(green, 128, 30 + 8.0f * i, 1, lines[i]);
            if (!alive) {   // the game had saved games for this; here the level starts again
                drawQuestion("RESTART LEVEL?", -1, 152);
                drawYesNo(160);
            }
            break;
        }
        case BLoad:
        case BSave: {
            // The game's LOAD and SAVE screens (FUN_0004b393, FUN_0004afb4): the slot's number, its
            // picture, where and when it was saved, its description, and the question.
            bool saving = menuPanel == BSave, fresh = saving && slotStage != SlotAsk;
            if (fresh) {
                if (slotRead != -2 && !grabThumb) { hud.setThumb(newThumb.size() == kThumbBytes ? newThumb.data() : nullptr); slotRead = -2; }
            } else {
                showSlot();
            }
            // The slot's number, as the game shows it, here in a column of all six and Q, the quick
            // save (an addition): the
            // one on show amber, the others green if there is a game in them, grey if not. A click
            // picks one; up / down, page up / down and the arrows under the screen turn to the next.
            for (int k = 0; k < kSlots; ++k) {
                SDL_PathInfo pi;
                bool used = SDL_GetPathInfo(slotPath(k).c_str(), &pi) && pi.type == SDL_PATHTYPE_FILE;
                hud.text(k == saveSlot ? amber : used ? green : grey, 280, 30 + 10.0f * k, 1, k == kQuick ? "Q" : std::to_string(k));
                menuHits.push_back({277, 28 + 10.0f * k, 10, 10, HitSlotPick, k});
            }
            hud.image(Hud::MenuThumb, 131, 25, 1);
            hud.text(amber, 131, 120, 1, fresh ? whereLine(level.name, world.difficulty) : slotInfo.where);
            hud.text(amber, 131, 127, 1, fresh ? whenLine(saveTime) : slotInfo.when);
            if (slotStage == SlotTyping) {
                hud.text(green, 131, 134, 1, saveDesc + "$");   // the game's cursor
                hud.text(red, 131, 165, 1, "ENTER GAME DESCRIPTION ABOVE");
            } else if (slotStage == SlotSaving) {
                hud.text(green, 131, 134, 1, saveDesc);
                drawQuestion("SAVING GAME.....", 153, 147);
            } else {
                hud.text(green, 131, 134, 1, slotInfo.used ? slotInfo.desc : "EMPTY SLOT");
                if (slotStage == SlotLoading) drawQuestion("LOADING GAME.....", 163, 97);
                if (!slotNote.empty()) drawQuestion(slotNote, -1, 147);
                else if (saving) drawQuestion(slotInfo.used ? "OVERWRITE THIS GAME?" : "SAVE TO THIS SLOT?", 153, 147);
                else drawQuestion("LOAD THIS GAME?", 163, 147);
                drawYesNo(158);
                menuHits.push_back({117, 179, 40, 14, HitSlot, 1});    // the arrows under the screen
                menuHits.push_back({157, 179, 40, 14, HitSlot, -1});
            }
            break;
        }
        case BQuit:
            hud.image(Hud::MenuLogo, 159, 67, 1);
            drawQuestion("ARE YOU SURE?", 168, 145);
            drawYesNo(158);
            break;
        case BNewGame:
            hud.image(Hud::MenuLogo, 159, 67, 1);
            drawQuestion("START NEW GAME?", 162, 145);
            drawYesNo(158);
            break;
        case BTutorial:
            hud.image(Hud::MenuLogo, 159, 67, 1);
            drawQuestion("START TUTORIAL?", -1, 145);
            drawYesNo(158);
            break;
        case BDifficulty: {
            static const char* names[] = {"EASY", "MEDIUM", "HARD"};
            hud.text(red, 142, 46, 1, "SELECT A DIFFICULTY LEVEL");
            for (int i = 0; i < 3; ++i) {
                float y = 73 + 21.0f * i;
                bool on = i == pendingDifficulty;
                hud.text(on ? green : amber, 180, y, 1, std::string(on ? ">" : " ") + names[i]);
                menuHits.push_back({170, y - 4, 70, 15, HitRow, i});
            }
            drawQuestion("RESTART LEVEL?", 167, 145);
            drawYesNo(158);
            break;
        }
        case BSettings: {
            // Two tabs at the top (the chosen one red); the arrows under the screen, PgUp / PgDn and
            // Tab turn the page too, and the list runs on from one page into the other.
            {
                static const char* tabs[3] = {"GAME", "SOUND AND PICTURE", "EXTRAS"};
                float tx = 119;
                for (int t = 0; t < 3; ++t) {
                    bool on = t == settingsPage;
                    float tw = hud.textWidth(on ? red : grey, 1, tabs[t]);
                    hud.text(on ? red : grey, tx, 32, 1, tabs[t]);
                    menuHits.push_back({tx - 3, 29, tw + 6, 11, HitPage, t});
                    if (on) hud.rect(tx, 40, tw, 1, 0.85f, 0.15f, 0.1f, 1);   // underlined
                    tx += tw + 10;
                }
            }
            menuHits.push_back({117, 179, 40, 14, HitPage, 101});   // the down arrows: the next page
            menuHits.push_back({157, 179, 40, 14, HitPage, 100});   // the up arrows: the one before
            for (int k = 0; k < 2; ++k) {   // the arrow that leads somewhere lights up under the cursor
                float ax = k == 0 ? 117.0f : 157.0f;
                bool there = k == 0 ? settingsPage < 2 : settingsPage > 0;
                if (there && menuMouseX >= ax && menuMouseX < ax + 40 && menuMouseY >= 179 && menuMouseY < 193)
                    hud.rect(ax + 2, 181, 36, 10, 0.55f, 1.0f, 0.55f, 0.18f);
            }
            auto row = [&](int i, float y) {
                bool on = panelFocus && i == panelSel;
                hud.text(on ? green : amber, 117, y, 1, std::string(on ? ">" : " ") + settingLabel(i));
                hud.text(on ? green : grey, 203, y, 1, fit(on ? green : grey, settingValue(i), 88));
                menuHits.push_back({114, y - 1, 178, 8, HitRow, i});
            };
            auto hint = [&](float y, int lines) {
                if (!panelFocus) return;
                auto l = wrap(grey, settingHint(panelSel), 172);
                for (size_t i = 0; i < l.size() && (int)i < lines; ++i) hud.text(grey, 118, y + 8.0f * i, 1, l[i]);
            };
            if (settingsPage == 1) {
                // The game's sliders: the label at 117, its value below at 122, the bar beside
                // it (optbar, drawn from 17, 6 left and above its point) with the knob (optslide,
                // centred) at 167 + the value.
                const int vols[2] = {musicVolume, soundVolume};
                for (int k = 0; k < 2; ++k) {
                    int i = k == 0 ? SMusic : SSound;
                    float y = k == 0 ? 49.0f : 77.0f;
                    bool on = panelFocus && i == panelSel;
                    hud.text(on ? green : amber, 117, y, 1, std::string(on ? ">" : " ") + settingLabel(i));
                    hud.text(green, 122, y + 7, 1, std::to_string(vols[k]));
                    hud.image(Hud::MenuBar, 167 - 17, y + 7 - 6, 1);
                    hud.image(Hud::MenuSlide, 167 + vols[k] - 10, y + 7 - 11, 1);
                    menuHits.push_back({114, y - 3, 178, 24, HitSlider, i});
                }
                for (int i = SScreen; i <= SVideos; ++i) row(i, 100 + 7.0f * (i - SScreen));
                hint(162, 2);
            } else if (settingsPage == 2) {
                for (int k = 0; k < kExtras; ++k)
                    if (extraLength[k] <= 0) extraLength[k] = VideoPlayer::measure(extraList(k).videos, &disc);
                for (int i = SExtra0; i < SCount; ++i) row(i, 46 + 8.0f * (i - SExtra0));
                hint(161, 2);
            } else {
                for (int i = SAI; i < SMusic; ++i) row(i, 48 + 9.0f * (i - SAI));
                hint(134, 4);
            }
            break;
        }
        case BConfigure: {
            hud.text(red, 130, 36, 1, bindWait >= 0 ? "PRESS A KEY FOR THIS FUNCTION" : "SELECT A FUNCTION TO REASSIGN");
            const float rowH = CCount > 18 ? 6.0f : CCount > 16 ? 6.5f : 7.0f;   // every row on the panel
            for (int i = 0; i < CCount; ++i) {
                float y = 45 + rowH * i;
                bool on = panelFocus && i == panelSel;
                std::string name, k1, k2;
                if (i < kActionCount) {
                    name = actionName(i);
                    k1 = Bindings::inputName(bindings.in[i][0]);
                    k2 = Bindings::inputName(bindings.in[i][1]);
                    if (k1.empty()) k1 = "-";
                    if (bindWait == i) k1 = "...", k2 = "";
                } else if (i == CSens) {
                    name = "MOUSE SPEED";
                    k1 = std::to_string((int)std::lround(mouseSens * 100)) + "%";
                } else if (i == CInvert) {
                    name = "INVERT MOUSE";
                    k1 = invertMouse ? "ON" : "OFF";
                } else name = "RESET TO DEFAULTS";
                hud.text(on ? green : amber, 115, y, 1, std::string(on ? ">" : " ") + name);
                hud.text(on ? green : grey, 180, y, 1, fit(grey, k1, 54));
                hud.text(on ? green : grey, 236, y, 1, fit(grey, k2, 54));
                menuHits.push_back({112, y - 1, 180, rowH, HitRow, i});
            }
            std::string hint = bindWait >= 0 ? "ESC: CANCEL   BACKSPACE: CLEAR"
                             : !panelFocus || panelSel < kActionCount ? "ENTER: REASSIGN   BACKSPACE: CLEAR"
                             : panelSel == CSens ? "LEFT / RIGHT: CHANGE" : "";
            hud.text(grey, std::round(203 - hud.textWidth(grey, 1, hint) / 2), 164, 1, hint);
            break;
        }
        }
        // The game's cursor while the mouse is about.
        if (menuMouseX >= 0 && menuMouseY >= 0 && menuMouseX < 320 && menuMouseY < 200)
            hud.image(Hud::MenuCursor, std::round(menuMouseX) - 9, std::round(menuMouseY) - 9, 1);
        hud.setShadow(true);
        hud.endCanvas(w, h, menuCanvas[0], menuCanvas[1], menuCanvas[2], menuCanvas[3]);
    }

    // One press of an action that acts once (use, kits, weapons, map).
    void doAction(Action a) {
        switch (a) {
        case Action::Use: world.useAt(cursorCamera()); break;
        case Action::BioKit: world.useKit(0); break;
        case Action::MechKit: world.useKit(1); break;
        case Action::NextWeapon: world.cycleWeapon(1); weaponStripUntil = clock + 1.6f; break;
        case Action::PrevWeapon: world.cycleWeapon(-1); weaponStripUntil = clock + 1.6f; break;
        case Action::Map:
            mapMode = mapMode == Automap::Mini ? Automap::Full : mapMode == Automap::Full ? Automap::Off : Automap::Mini;
            saveSettings();
            break;
        case Action::Inventory: world.toggleInventory(); break;
        case Action::Detonate: world.detonate(); break;
        case Action::JumpJets: world.toggleJets(); break;
        default: break;
        }
    }
    // With the inventory open, fire selects the highlighted slot and the weapon keys move the
    // highlight. True: the press was the inventory's.
    bool inventoryAction(Action a) {
        if (!world.inventoryOpen()) return false;
        switch (a) {
        case Action::Fire: world.inventorySelect(-1); return true;
        case Action::NextWeapon: world.inventoryMove(1); return true;
        case Action::PrevWeapon: world.inventoryMove(-1); return true;
        default: return false;
        }
    }

    // Is an action held? Keys from the keyboard state, mouse buttons while the game has the mouse.
    bool held(Action a, const bool* ks) const {
        for (int c : bindings.in[(int)a]) {
            if (!c) continue;
            if (c == kWheelUp || c == kWheelDown) { if (pulse[(int)a]) return true; continue; }
            if (c > kMouseBase) {
                int b = c - kMouseBase;
                if (b == ignoreButton || !SDL_GetWindowRelativeMouseMode(window)) continue;
                if (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_MASK(b)) return true;
                continue;
            }
            if (c < SDL_SCANCODE_COUNT && ks[c]) return true;
        }
        return false;
    }

    // The map's zoom, in steps of 1.25 (the wheel, + and -): the full map from the whole level to
    // 9.3 times as close; the minimap from about 17 cells to its rim (0.8^4, one step farther out
    // than up to 0.5.2, where it stopped at 0.5) to 2.5 times as close.
    static float clampZoom(int mode, float z) {
        if (!(z > 0)) return 1;
        z = mode == Automap::Full ? std::clamp(z, 1.0f, 9.3f) : std::clamp(z, 0.4096f, 2.5f);
        return std::abs(z - 1) < 0.05f ? 1 : z;
    }
    void zoomMap(int dir) {
        if (!dir || mapMode == Automap::Off) return;
        float& z = mapZoom[mapMode];
        z = clampZoom(mapMode, z * (dir > 0 ? 1.25f : 0.8f));
        saveSettings();
    }

    void drawMap(int w, int h) {
        if (mapMode == Automap::Off) return;
        MapFrame f;
        world.mapFrame(f, cam, mapReveal, mapMarkers);
        float aspect = (float)w / std::max(1, h);
        f.fov = 2 * std::atan(std::tan(rs.hFovDeg4x3 * 3.14159265f / 360.0f) * aspect / (4.0f / 3.0f));
        automap.draw(w, h, f, mapMode, mapZoom[mapMode], mapReveal, clock);
    }

    std::string controlsText() const {
        std::string t;
        for (int a = 0; a < kActionCount; ++a) {
            std::string l = Bindings::label((Action)a);
            for (char& c : l) c = (char)std::tolower((unsigned char)c);
            t += "   " + l + " - " + bindings.describe((Action)a) + "\n";
        }
        return t;
    }

    bool windowMoved = false;     // the window's size or place changed: saved on the way out

    // F1 / H: the controls, on a panel over the paused game in the game's lettering (up to 0.5.2 a
    // message box). Your controls as they are now on the left, the fixed keys on the right, a few
    // tips below. Any key or click closes it (F11 and F12 still work).
    bool helpOpen = false;
    void drawHelp(int w, int h) {
        const int nFixed = 14;
        const float tipsY = 36 + 8.0f * std::max(kActionCount, nFixed) + 8, panelH = tipsY + 3 * 8 + 22;
        const float s = std::max(1.0f, std::floor(std::min(w / 410.0f, h / (panelH + 10))));
        const float pw = 400 * s, ph = panelH * s, x0 = std::round((w - pw) / 2), y0 = std::round((h - ph) / 2);
        const Hud::Font green = Hud::Green, amber = Hud::Amber, grey = Hud::Grey, red = Hud::Red;
        auto at = [&](float x, float y) { return std::pair<float, float>(x0 + x * s, y0 + y * s); };
        auto text = [&](Hud::Font f, float x, float y, const std::string& t, float a = 1) {
            auto p = at(x, y);
            hud.text(f, p.first, p.second, s, t, a);
        };
        auto fitTo = [&](Hud::Font f, std::string t, float width) {
            while (t.size() > 1 && hud.textWidth(f, 1, t) > width) t.pop_back();
            return t;
        };
        hud.begin(w, h);
        hud.rect(0, 0, (float)w, (float)h, 0, 0, 0, 0.45f);
        hud.rect(x0, y0, pw, ph, 0.02f, 0.05f, 0.03f, 0.92f);
        hud.rect(x0, y0, pw, s, 0.2f, 0.75f, 0.25f, 0.8f);
        hud.rect(x0, y0 + ph - s, pw, s, 0.2f, 0.75f, 0.25f, 0.8f);
        hud.rect(x0, y0, s, ph, 0.2f, 0.75f, 0.25f, 0.8f);
        hud.rect(x0 + pw - s, y0, s, ph, 0.2f, 0.75f, 0.25f, 0.8f);
        std::string title = "CYCLONES: REMASTERED - CONTROLS";
        text(red, (400 - hud.textWidth(red, 1, title)) / 2, 8, title);
        // left: the actions and their keys (CONFIGURE sets them)
        text(amber, 12, 24, "YOUR CONTROLS (ESC, CONFIGURE)");
        for (int a = 0; a < kActionCount; ++a) {
            float y = 36 + 8.0f * a;
            text(green, 12, y, actionName(a));
            text(grey, 100, y, fitTo(grey, bindings.describe((Action)a), 96));
        }
        // right: the keys that stay put
        static const char* fixed[][2] = {
            {"MOUSE", "LOOK AROUND"},           {"1 - 8, 0", "WEAPONS, THE GLOVE"},
            {"F5 / F9", "QUICK SAVE / LOAD"},   {"ESC", "OPTIONS (LOAD, SAVE...)"},
            {"+ / -", "ZOOM THE MAP"},          {"F", "WHOLE MAP / EXPLORED"},
            {"K", "MAP MARKERS"},               {"N", "SMOOTH / ORIGINAL LOOK"},
            {"L", "LIGHTING ON / OFF"},         {"R", "RENDER SCALE"},
            {"V", "VSYNC ON / OFF"},            {"F11", "FULL SCREEN / WINDOW"},
            {"F12", "SCREENSHOT"},              {"F1 OR H", "THIS SCREEN"},
        };
        text(amber, 212, 24, "OTHER KEYS");
        const int n = nFixed;
        static_assert(sizeof(fixed) / sizeof(fixed[0]) == 14, "nFixed");
        for (int i = 0; i < n; ++i) {
            float y = 36 + 8.0f * i;
            text(grey, 212, y, fixed[i][0]);
            text(green, 270, y, fixed[i][1]);
        }
        // below: what is worth knowing
        static const char* tips[] = {
            "CROSSHAIR: GREEN ON AN ITEM, ORANGE ON A CREATURE, PINK ON A DOOR. AIM, THEN USE.",
            "JUMP JETS (ONCE FOUND): THEIR KEY SWITCHES THEM ON, THEN HOLD JUMP IN THE AIR TO FLY.",
            "INVENTORY: CLICK A SLOT (OR 1-8); DRAG AN ITEM ONTO WHAT IT IS FOR; DROP, THEN A SLOT.",
        };
        float ty = tipsY;
        hud.rect(x0 + 12 * s, y0 + (ty - 5) * s, 376 * s, s, 0.2f, 0.75f, 0.25f, 0.35f);
        for (int i = 0; i < 3; ++i) text(grey, 12, ty + 8.0f * i, fitTo(grey, tips[i], 376));
        std::string close = "ANY KEY CLOSES";
        text(amber, (400 - hud.textWidth(amber, 1, close)) / 2, panelH - 13, close, 0.8f);
        hud.end();
    }

    // Whether a key press counts as "a key" for skipping a video: the ordinary keys (letters,
    // digits, Enter, Esc, Space, Backspace, Tab, punctuation, F1-F10, the arrows and the editing
    // block, the keypad), not system and media keys, and not with Alt or the Windows key held.
    static bool skipsVideo(const SDL_KeyboardEvent& k) {
        if (k.mod & (SDL_KMOD_ALT | SDL_KMOD_GUI)) return false;
        int sc = (int)k.scancode;
        if (sc < SDL_SCANCODE_A || sc > SDL_SCANCODE_NONUSBACKSLASH) return false;   // modifiers, media, volume, F13+, ...
        switch (sc) {
        case SDL_SCANCODE_CAPSLOCK:
        case SDL_SCANCODE_PRINTSCREEN:
        case SDL_SCANCODE_SCROLLLOCK:
        case SDL_SCANCODE_PAUSE:
        case SDL_SCANCODE_NUMLOCKCLEAR:
            return false;
        default:
            return true;
        }
    }
    uint64_t focusGainedAt = 0;

    void run() {
        bool running = true;
        uint64_t last = SDL_GetTicksNS(), fpsTimer = last;
        int frames = 0, shotFrames = 0;
        bool scriptedFire = false;
        while (running) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                // During the cut-scenes a key (or a click) skips the rest of the playlist, as the
                // game's video player did; full screen and screenshots still work. Keys meant for
                // the computer rather than the game don't skip: volume and media keys, the
                // modifiers and the Windows key on their own, Alt or Windows shortcuts (Alt+Tab),
                // Print Screen and the lock keys; nor does the click that brings the window back.
                if (e.type == SDL_EVENT_WINDOW_FOCUS_GAINED) focusGainedAt = SDL_GetTicks();
                // In the background (an addition): play stops (the options screen comes up, as
                // Esc brings it) and the sound goes quiet until the window is back, except under the
                // opening and the videos between missions, which play on as they were.
                if (opt.shot.empty() && (e.type == SDL_EVENT_WINDOW_FOCUS_LOST || e.type == SDL_EVENT_WINDOW_FOCUS_GAINED)) {
                    bool lost = e.type == SDL_EVENT_WINDOW_FOCUS_LOST;
                    if (lost && !cutscenes.active()) {
                        if (player.isOpen()) player.setPlaying(false);
                        else if (!menuOpen) openMenu(true);
                        audio.setVolumes(0, 0);
                    } else if (!lost) {
                        applyVolumes();
                    }
                }
                if (e.type == SDL_EVENT_WINDOW_RESIZED || e.type == SDL_EVENT_WINDOW_MOVED) {   // remembered for next time
                    SDL_WindowFlags f = SDL_GetWindowFlags(window);
                    if (!(f & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED)) && opt.shot.empty()) {
                        SDL_GetWindowSize(window, &opt.width, &opt.height);
                        SDL_GetWindowPosition(window, &opt.winX, &opt.winY);
                        windowMoved = true;
                    }
                }
                if (helpOpen) {   // the help screen: any key or click closes it
                    if (e.type == SDL_EVENT_QUIT) { running = false; continue; }
                    if (e.type == SDL_EVENT_KEY_DOWN) {
                        SDL_Keycode k = e.key.key;
                        if (k == SDLK_F11 || (k == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT)))
                            setFullscreen(!(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
                        else if (k == SDLK_F12) screenshot();
                        else if (!e.key.repeat) helpOpen = false;
                        continue;
                    }
                    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) { helpOpen = false; continue; }
                    if (e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_WHEEL || e.type == SDL_EVENT_MOUSE_BUTTON_UP ||
                        e.type == SDL_EVENT_KEY_UP || e.type == SDL_EVENT_TEXT_INPUT) continue;
                }
                if (player.isOpen()) {   // EXTRAS: the cut-scene player takes the keys and the mouse
                    if (e.type == SDL_EVENT_QUIT) { running = false; continue; }
                    if (e.type == SDL_EVENT_KEY_DOWN) { playerKey(e.key.key, e.key.repeat); continue; }
                    if (e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                        e.type == SDL_EVENT_MOUSE_BUTTON_UP || e.type == SDL_EVENT_MOUSE_WHEEL) {
                        if (opt.shot.empty()) playerMouse(e);
                        continue;
                    }
                    if (e.type == SDL_EVENT_KEY_UP || e.type == SDL_EVENT_TEXT_INPUT) continue;
                }
                if (cutscenes.active() && (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN)) {
                    if (e.type == SDL_EVENT_KEY_DOWN) {
                        SDL_Keycode k = e.key.key;
                        if (e.key.repeat) continue;
                        if (k == SDLK_F11 || (k == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT))) {
                            setFullscreen(!(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
                            continue;
                        }
                        if (k == SDLK_F12) { screenshot(); continue; }
                        if (!skipsVideo(e.key)) continue;
                    } else if (SDL_GetTicks() - focusGainedAt < 300) {
                        continue;
                    }
                    cutscenes.skip();
                    continue;
                }
                switch (e.type) {
                case SDL_EVENT_QUIT: running = false; break;
                case SDL_EVENT_MOUSE_MOTION:
                    if (menuOpen) {
                        if (!opt.shot.empty()) break;   // captures place the cursor themselves
                        menuPoint(e.motion.x, e.motion.y);
                        if (sliderDrag >= 0) setVolume(sliderDrag, (int)std::lround(menuMouseX - 167), false);
                        else menuHover();
                    } else if (invCursor) {
                        cursorFromWindow(e.motion.x, e.motion.y);
                    } else if (SDL_GetWindowRelativeMouseMode(window)) {
                        cam.yaw += e.motion.xrel * 0.0025f * mouseSens;
                        cam.pitch = std::clamp(cam.pitch - e.motion.yrel * 0.0025f * mouseSens * (invertMouse ? -1.0f : 1.0f), -1.5f, 1.5f);
                    }
                    break;
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                    if (menuOpen) {
                        menuPoint(e.button.x, e.button.y);
                        menuClick(e.button.button);
                        if (quitRequested) running = false;
                        break;
                    }
                    if (invCursor) {
                        cursorFromWindow(e.button.x, e.button.y);
                        inventoryClick(e.button.button, true);
                        break;
                    }
                    if (!SDL_GetWindowRelativeMouseMode(window)) {
                        SDL_SetWindowRelativeMouseMode(window, true);
                        ignoreButton = e.button.button;   // this click only takes the mouse
                    } else {
                        Action a = bindings.actionFor(kMouseBase + e.button.button);
                        if (inventoryAction(a)) break;
                        if (a != Action::Count && !Bindings::held(a)) doAction(a);
                    }
                    break;
                case SDL_EVENT_MOUSE_WHEEL: {
                    int code = e.wheel.y > 0 ? kWheelUp : e.wheel.y < 0 ? kWheelDown : 0;
                    if (!code) break;
                    if (menuOpen) { menuBind(code); break; }
                    Action a = bindings.actionFor(code);
                    if (inventoryAction(a)) break;
                    if (mapMode == Automap::Full) { zoomMap(code == kWheelUp ? 1 : -1); break; }   // the open map takes the wheel
                    if (a == Action::Count) { if (mapMode != Automap::Off) zoomMap(code == kWheelUp ? 1 : -1); }
                    else if (Bindings::held(a)) pulse[(int)a] = true;
                    else doAction(a);
                    break;
                }
                case SDL_EVENT_MOUSE_BUTTON_UP:
                    if (invCursor && !menuOpen) {
                        cursorFromWindow(e.button.x, e.button.y);
                        inventoryClick(e.button.button, false);
                    }
                    if (e.button.button == ignoreButton) ignoreButton = 0;
                    bindWaitRelease = false;
                    if (sliderDrag >= 0) {
                        if (sliderDrag == SSound) audio.play(81);   // how loud it is now
                        sliderDrag = -1;
                        saveSettings();
                    }
                    break;
                case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
                case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
                    if (opt.fullscreen != (e.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN)) {
                        opt.fullscreen = e.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN;
                        saveSettings();
                    }
                    break;
                case SDL_EVENT_TEXT_INPUT:
                    if (menuOpen) slotText(e.text.text);
                    break;
                case SDL_EVENT_KEY_DOWN: {
                    SDL_Keycode k = e.key.key;
                    if (menuOpen) {
                        if (!e.key.repeat || bindWait < 0) menuKey(k, e.key.scancode);
                        if (quitRequested) running = false;
                        break;
                    }
                    Action bound = bindings.actionFor(e.key.scancode);
                    bool inv = world.inventoryOpen();
                    if (k == SDLK_ESCAPE) {
                        if (inv) world.toggleInventory();   // Esc closes the inventory first
                        else openMenu(true);
                    } else if (bound != Action::Count) {   // your controls come before the fixed keys
                        if (!e.key.repeat && !inventoryAction(bound) && !Bindings::held(bound)) doAction(bound);
                    } else if (inv && k >= SDLK_1 && k <= SDLK_8) {
                        if (!e.key.repeat) world.inventorySelect((int)(k - SDLK_1));
                    } else if (inv && (k == SDLK_BACKSPACE || k == SDLK_DELETE)) {
                        if (!e.key.repeat) world.inventoryDrop(-1);
                    } else if (k == SDLK_L) rs.lighting = !rs.lighting;
                    else if (k == SDLK_R) { renderMode = (renderMode + 1) % 3; applyRenderMode(); }
                    else if (k >= SDLK_1 && k <= SDLK_8) world.selectWeapon((int)(k - SDLK_1));
                    else if (k == SDLK_0 || k == SDLK_9) world.selectWeapon(8);
                    else if (k == SDLK_N) world.smooth = !world.smooth;
                    else if (k == SDLK_V) { opt.vsync = !opt.vsync; SDL_GL_SetSwapInterval(opt.vsync); }
                    else if (k == SDLK_F) mapReveal = !mapReveal;
                    else if (k == SDLK_K) mapMarkers = (mapMarkers + 1) % 3;
                    else if (k == SDLK_EQUALS || k == SDLK_KP_PLUS) zoomMap(1);
                    else if (k == SDLK_MINUS || k == SDLK_KP_MINUS) zoomMap(-1);
                    else if (k == SDLK_F11 || (k == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT)))
                        setFullscreen(!(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
                    else if (k == SDLK_F12) screenshot();
                    else if (k == SDLK_F5) { if (!e.key.repeat) quickSave(); }
                    else if (k == SDLK_F9) { if (!e.key.repeat) quickLoad(); }
                    else if (k == SDLK_F1 || k == SDLK_H) { if (!e.key.repeat) helpOpen = true; }
                    if (bound == Action::Count && (k == SDLK_K || k == SDLK_N || k == SDLK_L || k == SDLK_R || k == SDLK_V))
                        saveSettings();
                    break;
                }
                default: break;
                }
            }
            syncInventoryCursor();
            uint64_t now = SDL_GetTicksNS();
            float dt = std::min(0.1f, (now - last) / 1e9f);
            last = now;
            if (!opt.shot.empty()) {
                // deterministic captures (CYCLONES_CAPFPS: another frame rate, for testing)
                static const float capDt = SDL_getenv("CYCLONES_CAPFPS") ? 1.0f / std::max(10.0f, (float)std::atof(SDL_getenv("CYCLONES_CAPFPS"))) : 1.0f / 60.0f;
                dt = capDt;
                auto at = [&](const std::string& list) {
                    for (size_t p = 0; p < list.size();) {
                        size_t q = list.find(',', p);
                        if (std::atoi(list.substr(p, q - p).c_str()) == shotFrames) return true;
                        if (q == std::string::npos) break;
                        p = q + 1;
                    }
                    return false;
                };
                scriptedFire = at(opt.fire);
                if (at(opt.boom)) world.explosionAtAim(cam);
                if (at(opt.use)) world.useAt(cam);
                for (size_t p = 0; p < opt.warp.size();) {   // --warp: the player put in a cell at a frame
                    size_t q = opt.warp.find(';', p);
                    std::string item = opt.warp.substr(p, q == std::string::npos ? std::string::npos : q - p);
                    p = q == std::string::npos ? opt.warp.size() : q + 1;
                    int f = -1, wx = 0, wz = 0;
                    float wy = 1e6f;
                    if (std::sscanf(item.c_str(), "%d:%d,%d,%f", &f, &wx, &wz, &wy) >= 3 && f == shotFrames) {
                        world.warpPlayer(wx, wz, wy);
                        cam.x = world.player().x;
                        cam.z = world.player().z;
                    }
                }
                for (size_t p = 0; p < opt.view.size();) {   // --view: the camera turned at a frame
                    size_t q = opt.view.find(';', p);
                    std::string item = opt.view.substr(p, q == std::string::npos ? std::string::npos : q - p);
                    p = q == std::string::npos ? opt.view.size() : q + 1;
                    int f = -1;
                    float yd = 0, pd = 0;
                    if (std::sscanf(item.c_str(), "%d:%f,%f", &f, &yd, &pd) >= 2 && f == shotFrames) {
                        cam.yaw = yd * 3.14159265f / 180.0f;
                        cam.pitch = pd * 3.14159265f / 180.0f;
                    }
                }
                for (size_t p = 0; p < opt.inv.size();) {
                    size_t q = opt.inv.find(';', p);
                    std::string item = opt.inv.substr(p, q == std::string::npos ? std::string::npos : q - p);
                    p = q == std::string::npos ? opt.inv.size() : q + 1;
                    size_t c = item.find(':');
                    if (c == std::string::npos || std::atoi(item.c_str()) != shotFrames) continue;
                    std::string what = item.substr(c + 1);
                    if (what == "i") world.toggleInventory();
                    else if (what == "+" || what == "-") world.inventoryMove(what == "+" ? 1 : -1);
                    else if (what == "s") world.inventorySelect(-1);
                    else if (what == "d") world.inventoryDrop(-1);
                    else if (what == "b") world.detonate();
                    else if (what == "j") world.toggleJets();
                    else if (what == "qs") quickSave();
                    else if (what == "help") helpOpen = true;
                    else if (what == "ql") quickLoad();
                    else if (what == "w+" || what == "w-") doAction(what == "w+" ? Action::NextWeapon : Action::PrevWeapon);
                    else if (what == "u") world.useAt(cursorCamera());
                    else if (what[0] == 'm') std::sscanf(what.c_str() + 1, "%f,%f", &curX, &curY);   // the cursor to x,y
                    else if (what == "l" || what == "r") {                                      // a click there
                        inventoryClick(what == "l" ? SDL_BUTTON_LEFT : SDL_BUTTON_RIGHT, true);
                        inventoryClick(what == "l" ? SDL_BUTTON_LEFT : SDL_BUTTON_RIGHT, false);
                    } else if (what[0] == 'g' && what.size() == 2) {                             // slot n dragged to it
                        dragSlot = what[1] - '1';
                        dragging = true;
                        inventoryClick(SDL_BUTTON_LEFT, false);
                    }
                    else if (what.size() == 1 && what[0] >= '1' && what[0] <= '8') world.inventorySelect(what[0] - '1');
                }
                if (at(opt.hurt)) world.damagePlayer(30, nullptr, 4);
            }

            // EXTRAS: the cut-scene player, over everything while it is open.
            if (player.isOpen()) {
                for (size_t p = 0; p < opt.extrasDo.size();) {   // testing: --extras-do "frame:what;..."
                    size_t q = opt.extrasDo.find(';', p);
                    std::string item = opt.extrasDo.substr(p, q == std::string::npos ? std::string::npos : q - p);
                    p = q == std::string::npos ? opt.extrasDo.size() : q + 1;
                    size_t c = item.find(':');
                    if (c == std::string::npos || std::atoi(item.c_str()) != shotFrames) continue;
                    std::string what = item.substr(c + 1);
                    if (what == "p") player.setPlaying(!player.playing());
                    else if (what == "f") player.step(1);
                    else if (what == "b") player.step(-1);
                    else if (what == "e") player.setEnhanced(!player.enhanced());
                    else if (what == "x") { closePlayer(); break; }
                    else if (what[0] == 's') player.seek(std::atof(what.c_str() + 1));
                    else if (what[0] == 'm') { std::sscanf(what.c_str() + 1, "%f,%f", &playerMouseX, &playerMouseY); playerPoke(); }
                }
                if (player.isOpen()) {
                    player.update(dt);
                    clock += dt;
                    int w, h;
                    SDL_GetWindowSizeInPixels(window, &w, &h);
                    drawPlayer(w, h);
                    ++shotFrames;
                    if (!opt.shot.empty() && shotFrames >= opt.frames && (shotFrames - opt.frames) % opt.every == 0) {
                        int k = (shotFrames - opt.frames) / opt.every;
                        std::string path = opt.shot;
                        if (opt.burst > 1) {
                            size_t dot = path.rfind('.');
                            path = path.substr(0, dot) + "_" + std::to_string(k) + path.substr(dot);
                        }
                        renderer.screenshot(w, h, path);
                        if (k + 1 >= opt.burst) running = false;
                    }
                    SDL_GL_SwapWindow(window);
                    continue;
                }
            }

            // The cut-scenes play before the game starts (it waits meanwhile). However they end
            // (played out, or the last part skipped with a key), the mission's music starts, and at
            // the start of the game the options screen comes up. (A key skips in the event loop, so
            // the videos are already over here; before 1.0 the options screen was missed then.)
            if (cutscenesWere && !cutscenes.active()) {
                audio.music(musicTrack(level.name));
                if (startMenu) showStartMenu();
            }
            cutscenesWere = cutscenes.active();
            if (cutscenes.active()) {
                if (scriptedFire) cutscenes.skip();   // captures: --fire frames stand for a key
                scriptedFire = false;
                cutscenes.update(dt);
                if (!cutscenes.active()) {
                    audio.music(musicTrack(level.name));   // the mission begins
                    cutscenesWere = false;
                    if (startMenu) showStartMenu();
                } else {
                    int w, h;
                    SDL_GetWindowSizeInPixels(window, &w, &h);
                    float aspect = rs.tallPixels ? 4.0f / 3.0f : 1.6f;
                    float cw = std::min((float)w, h * aspect), ch = cw / aspect;
                    drawVideoPicture(cutscenes, w, h, std::round((w - cw) / 2), std::round((h - ch) / 2), cw, ch);
                    drawSubtitles(cutscenes, w, h, std::round((w - cw) / 2), std::round((h - ch) / 2), cw, ch, (float)h);
                    ++shotFrames;
                    if (!opt.shot.empty() && shotFrames >= opt.frames && (shotFrames - opt.frames) % opt.every == 0) {
                        int k = (shotFrames - opt.frames) / opt.every;
                        std::string path = opt.shot;
                        if (opt.burst > 1) {
                            size_t dot = path.rfind('.');
                            path = path.substr(0, dot) + "_" + std::to_string(k) + path.substr(dot);
                        }
                        renderer.screenshot(w, h, path);
                        if (k + 1 >= opt.burst) running = false;
                    }
                    if (!opt.wav.empty()) playSounds(dt);
                    SDL_GL_SwapWindow(window);
                    continue;
                }
            }

            // Keyboard state, plus scripted keys for captures: --keys "W:0-120;A:60-90;LSHIFT:0-50"
            // (key names as SDL knows them, frame ranges).
            static bool keyState[SDL_SCANCODE_COUNT];
            {
                int n = 0;
                const bool* real = SDL_GetKeyboardState(&n);
                for (int i = 0; i < SDL_SCANCODE_COUNT; ++i) keyState[i] = i < n && real[i];
                for (size_t p = 0; p < opt.keys.size();) {
                    size_t q = opt.keys.find(';', p);
                    std::string item = opt.keys.substr(p, q == std::string::npos ? std::string::npos : q - p);
                    p = q == std::string::npos ? opt.keys.size() : q + 1;
                    size_t c = item.find(':');
                    int f0 = 0, f1 = 0;
                    if (c == std::string::npos || std::sscanf(item.c_str() + c + 1, "%d-%d", &f0, &f1) != 2) continue;
                    SDL_Scancode sc = SDL_GetScancodeFromName(item.substr(0, c).c_str());
                    if (sc != SDL_SCANCODE_UNKNOWN && shotFrames >= f0 && shotFrames <= f1) keyState[sc] = true;
                }
            }
            const bool* ks = keyState;
            bool fwd = held(Action::Forward, ks), back = held(Action::Back, ks), left = held(Action::StrafeLeft, ks),
                 right = held(Action::StrafeRight, ks), run = held(Action::Run, ks), jump = held(Action::Jump, ks);
            bool paused = (menuOpen || helpOpen) && opt.shot.empty();   // the game stands still while the menu (or help) is open
            bool warping = warpStage == 1;                 // and while a script's fade-out runs
            if (paused || warping) {
            } else if (walk) {
                PlayerInput in;
                in.forward = (fwd ? 1.0f : 0.0f) - (back ? 1.0f : 0.0f);
                in.strafe = (right ? 1.0f : 0.0f) - (left ? 1.0f : 0.0f);
                in.run = run;
                in.jump = jump;
                world.movePlayer(in, dt, cam);
            } else {
                // Free flight through everything; moving forward/back follows the view pitch.
                float speed = (run ? 900.0f : 300.0f) * dt;
                float fx = std::sin(cam.yaw), fz = -std::cos(cam.yaw);
                float mx = 0, mz = 0, my = 0;
                if (fwd) { mx += fx; mz += fz; }
                if (back) { mx -= fx; mz -= fz; }
                if (right) { mx -= fz; mz += fx; }
                if (left) { mx += fz; mz -= fx; }
                if (jump) my += 1;
                if (ks[SDL_SCANCODE_C]) my -= 1;
                float len = std::sqrt(mx * mx + mz * mz);
                if (len > 0) { cam.x += mx / len * speed; cam.z += mz / len * speed; }
                if (fwd) cam.y += std::sin(cam.pitch) * speed;
                if (back) cam.y -= std::sin(cam.pitch) * speed;
                cam.y += my * speed;
                world.placePlayer(cam);
            }

            bool firing = scriptedFire || (!menuOpen && !world.inventoryOpen() && held(Action::Fire, ks));
            scriptedFire = false;
            for (bool& b : pulse) b = false;
            // Dead: once the screen has gone out, the game goes to its menu (FUN_000491d0), and
            // so does this: restart the level, start a new game, or change the options.
            if (!world.player().alive) {
                if (world.player().deadTime > kDeathScreen && !deathMenu) {
                    deathMenu = true;
                    openMenu(true);
                    panelFocus = true;   // on the restart question
                    menuYes = 0;
                }
                firing = false;
            } else {
                deathMenu = false;
            }
            if (!paused && !warping) {
                world.update(dt, cam, firing);
                firstPerson.update(dt, world.player(), cam.yaw, cam.pitch, world.time());
            }
            if (!paused) updateWarp(dt);
            if (world.takeLevelChanged()) {   // a script moved walls: the map draws them anew
                MapLevel plan;
                world.mapLevel(plan);
                automap.buildLevel(plan, data);
            }
            slotWork();
            playSounds(dt);
            focusClock += dt;
            focusCamera(false);
            tourCamera();
            int w, h;
            SDL_GetWindowSizeInPixels(window, &w, &h);
            if (opt.fast && !opt.shot.empty()) {
                // Testing: simulate without drawing the frames that aren't saved.
                int next = shotFrames + 1;
                if (!(next >= opt.frames && (next - opt.frames) % opt.every == 0)) {
                    world.wearTear(dt);   // as if drawn
                    ++shotFrames;
                    continue;
                }
            }
            Camera view = cam;
            if (world.shake > 0) {
                view.x += std::sin(now * 1e-7f * 7.3f) * world.shake * 0.3f;
                view.y += std::sin(now * 1e-7f * 9.1f) * world.shake * 0.3f;
            }
            world.buildScene(scene, view);
            renderer.draw(view, w, h, rs, &scene);
            if (brightness > 0) renderer.brighten(w, h, 1.0f + brightness * 0.1f);
            if (grabThumb) {   // SAVE: the slot's picture, before anything is drawn over the view
                newThumb = renderer.thumbnail(w, h, Hud::kThumbW, Hud::kThumbH);
                grabThumb = false;
                slotRead = -1;
            }
            if (quickSavePending) finishQuickSave(w, h);
            // Hits tear the picture as in the game, before the status display is drawn over it.
            if (world.player().alive && world.player().tear >= 1) {
                float full[4] = {0, 0, 320, 200};
                renderer.crt(w, h, full, 0, world.player().tear);
            }
            renderer.keepFrame(w, h);
            if (!paused) world.wearTear(dt);
            clock += dt;
            if (mapMode != Automap::Full && world.player().alive) drawCrosshair(w, h);
            drawMap(w, h);
            drawHud(w, h);
            drawDeath(w, h);
            if (warpDark > 0) {   // a script's fade (the game fades the whole palette)
                hud.begin(w, h);
                hud.rect(0, 0, (float)w, (float)h, 0, 0, 0, std::min(1.0f, warpDark));
                hud.end();
            }
            if (helpOpen && !menuOpen) drawHelp(w, h);
            if (menuOpen) drawMenu(w, h);
            ++shotFrames;
            if (!opt.shot.empty() && shotFrames >= opt.frames && (shotFrames - opt.frames) % opt.every == 0) {
                int k = (shotFrames - opt.frames) / opt.every;
                std::string path = opt.shot;
                if (opt.burst > 1) {
                    size_t dot = path.rfind('.');
                    path = path.substr(0, dot) + "_" + std::to_string(k) + path.substr(dot);
                }
                renderer.screenshot(w, h, path);
                if (opt.hittest) hitTestImage(w, h, path.substr(0, path.rfind('.')) + "_hit.pgm");
                if (k + 1 >= opt.burst) {
                    running = false;
                    if (opt.saveTo >= 0) {   // testing: the state the capture ends in
                        std::string err;
                        renderer.draw(view, w, h, rs, &scene);
                        newThumb = renderer.thumbnail(w, h, Hud::kThumbW, Hud::kThumbH);
                        saveTime = nowSeconds();
                        saveDesc = "CAPTURE AT FRAME " + std::to_string(shotFrames);
                        if (!writeSlot(opt.saveTo, err)) SDL_Log("--save: %s", err.c_str());
                    }
                }
            }
            SDL_GL_SwapWindow(window);

            ++frames;
            if (now - fpsTimer > 1000000000ULL) {
                char title[256];
                const char* modes[] = {"native", "half-res", "320x200"};
                SDL_snprintf(title, sizeof(title), "CyClones: Remastered v%s — %s — %d fps — %dx%d %s — %s — %s%s%s — F1 help",
                             kVersion, level.name.c_str(), frames, w, h, modes[renderMode], world.status().c_str(),
                             rs.lighting ? "lit" : "fullbright", world.smooth ? "" : " — classic look",
                             walk ? " — walk" : "");
                SDL_SetWindowTitle(window, title);
                frames = 0;
                fpsTimer = now;
            }
        }
    }
};

}  // namespace

int main(int argc, char** argv) {
    App app;
    app.bindings.defaults();
    // Captures (--shot) ignore and never touch the saved settings, so they're repeatable.
    bool capture = false;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--shot") capture = true;
    if (capture) app.saveSettingsOnChange = false;
    else app.loadSettings();
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--disc") app.opt.disc = next();
        else if (a == "--hd") app.opt.hd = next();
        else if (a == "--video-pack") app.opt.videoPack = next();
        else if (a == "--brightness") app.brightness = std::clamp(std::atoi(next().c_str()), 0, 10);
        else if (a == "--extras") app.opt.extras = std::atoi(next().c_str());
        else if (a == "--extras-do") app.opt.extrasDo = next();
        else if (a == "--level") app.opt.level = next();
        else if (a == "--fullscreen") app.opt.fullscreen = true;
        else if (a == "--display") app.opt.display = std::atoi(next().c_str());
        else if (a == "--novsync") app.opt.vsync = 0;
        else if (a == "--size") std::sscanf(next().c_str(), "%dx%d", &app.opt.width, &app.opt.height);
        else if (a == "--shot") app.opt.shot = next();
        else if (a == "--frames") app.opt.frames = std::atoi(next().c_str());
        else if (a == "--pos") app.opt.pos = next();
        else if (a == "--retro") app.renderMode = 2;
        else if (a == "--fov") app.rs.hFovDeg4x3 = std::clamp((float)std::atof(next().c_str()), 50.0f, 120.0f);
        else if (a == "--square") app.rs.tallPixels = false;
        else if (a == "--fullbright") app.rs.lighting = false;
        else if (a == "--walk") app.walk = true;
        else if (a == "--fire") app.opt.fire = next();
        else if (a == "--boom") app.opt.boom = next();
        else if (a == "--use") app.opt.use = next();
        else if (a == "--inv") app.opt.inv = next();
        else if (a == "--view") app.opt.view = next();
        else if (a == "--warp") app.opt.warp = next();
        else if (a == "--kits") app.opt.kits = std::atoi(next().c_str());
        else if (a == "--hurt") app.opt.hurt = next();   // testing: 30 damage (ignores armour) at these frames
        else if (a == "--fast") app.opt.fast = true;
        else if (a == "--burst") app.opt.burst = std::max(1, std::atoi(next().c_str()));
        else if (a == "--every") app.opt.every = std::max(1, std::atoi(next().c_str()));
        else if (a == "--hold") app.opt.hold = true;
        else if (a == "--focus") app.opt.focus = next();
        else if (a == "--classic") app.world.smooth = false;
        else if (a == "--map") app.opt.map = next();
        else if (a == "--tour") app.opt.tour = next();
        else if (a == "--hittest") app.opt.hittest = true;
        else if (a == "--keys") app.opt.keys = next();
        else if (a == "--start") app.opt.start = next();
        else if (a == "--god") app.world.godMode = true;
        else if (a == "--difficulty") { std::string v = next(); app.world.difficulty = v == "easy" ? 0 : v == "normal" ? 1 : 2; }
        else if (a == "--ai") app.world.enhancedAI = next() == "enhanced";
        else if (a == "--allweapons") app.world.startWithAllWeapons = true;
        else if (a == "--peaceful") app.world.monstersAttack = false;
        else if (a == "--menu") app.menuOpen = true;
        else if (a == "--controls") { app.menuOpen = true; app.menuPanel = 7; app.panelFocus = true; app.panelSel = 2; }
        else if (a == "--panel") { app.menuOpen = true; app.menuPanel = std::atoi(next().c_str()); app.panelFocus = app.menuPanel >= 0; }
        else if (a == "--settingsrow") { app.panelSel = std::atoi(next().c_str()); app.settingsPage = App::settingPage(app.panelSel); }
        else if (a == "--menumouse") { app.menuOpen = true; std::sscanf(next().c_str(), "%f,%f", &app.menuMouseX, &app.menuMouseY); }
        else if (a == "--reveal") app.mapReveal = true;
        else if (a == "--markers") { std::string v = next(); app.mapMarkers = v == "creatures" ? 2 : v == "pickups" ? 1 : 0; }
        else if (a == "--mapzoom") { float z = (float)std::atof(next().c_str()); app.mapZoom[1] = app.mapZoom[2] = z; }
        else if (a == "--weapon") app.opt.weapon = std::atoi(next().c_str()) - 1;
        else if (a == "--wav") app.opt.wav = next();
        else if (a == "--intro") app.opt.intro = 1;
        else if (a == "--save") app.opt.saveTo = std::clamp(std::atoi(next().c_str()), 0, App::kSlots - 1);
        else if (a == "--load") app.opt.loadFrom = std::clamp(std::atoi(next().c_str()), 0, App::kSlots - 1);
        else if (a == "--nointro") app.opt.intro = 0;
    }
    if (!app.opt.focus.empty() || !app.opt.tour.empty() || !app.opt.pos.empty()) app.walk = false;  // scripted cameras fly
    app.applyRenderMode();
    if (!app.init()) return 1;
    app.run();
    if (app.windowMoved) app.saveSettings();
    app.writeWav();
    app.audio.shutdown();
    SDL_Quit();
    return 0;
}
