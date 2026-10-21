// Live level contents for the engine preview, drawn from the original 1994 sprite art.
//
// Monsters are "depth sprites": every original rotation view keeps its exact pixels, placed in 3D
// with depth estimated from the shape all the views agree on. Seen from one of the 8 original
// angles a monster is pixel-for-pixel the 1994 sprite; in between it turns with real parallax
// instead of snapping. Each animation frame holds its original pose and glides briefly into the
// next; walking legs keep pace with the ground covered. Items, props, pain and death frames stay
// classic upright sprites, with the original timing (100 Hz ticks). Weapons, projectiles, impacts
// and explosions all use the game's own sprites, animated smoothly, with sparks and light on top.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "../data/entities.h"
#include "../data/level.h"
#include "../render/automap.h"
#include "../render/renderer.h"
#include "../render/voxel.h"

struct SpriteAnim {                       // single-view frames
    std::vector<ModelRange> still, morph; // morph[i]: frame i gliding into frame i+1
    std::vector<float> seconds;
    std::vector<float> light;             // per frame: light flash from the original data (0 = none)
    std::vector<int> height, width, anchor;
    bool loop = false;
    bool hold = true;                     // last frame stays (a corpse) instead of fading away
    bool empty() const { return still.empty(); }
    float total() const {
        float t = 0;
        for (float s : seconds) t += s;
        return t;
    }
};

struct RotAnim {                          // multi-view frames: 8 views per step
    std::vector<std::array<ModelRange, 8>> still, morph;
    std::vector<float> seconds;
    bool byDistance = true;               // legs: advance with distance walked; wings: with time
    float stride = 48;                    // world units per full walk cycle
    bool empty() const { return still.empty(); }
};

// What a shot can hit: each sprite frame's silhouette with every row filled from its leftmost to
// its rightmost pixel (so gaps between legs and arms still count) and grown by a pixel.
struct HitMask {
    int w = 0, h = 0, ox = 0;
    std::vector<uint8_t> on;              // w*h, row 0 at the ground
    std::vector<float> depth;             // depth sprites: each pixel's distance toward the viewer
    bool test(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h && on[(size_t)y * w + x]; }
};

struct Visual {
    RotAnim rot;                          // monsters / projectiles drawn from rotation sets
    SpriteAnim flat;                      // single-view: items, objects, effects, 1-view monsters
    SpriteAnim pain, death;
    SpriteAnim attackA, attackB, ready;   // single-view attack frames and the "unfold" before attacking
    std::vector<uint8_t> fireA, fireB;    // per attack frame: fire on this frame
    float height = 16, radius = 8;
    // Hit silhouettes (monsters): per walk step and view, and per flat/pain/death frame.
    std::vector<std::array<HitMask, 8>> rotHit;
    std::vector<HitMask> flatHit, painHit, deathHit, attackAHit, attackBHit, readyHit;
    float hitRadius = 8, hitHeight = 16;  // bounds of every silhouette around the axis
    // Built on worker threads at level load, then uploaded on the main thread.
    std::function<void(Visual&)> build;
    struct Staged;
    std::shared_ptr<Staged> staged;
};

// The player's body and status.
struct PlayerState {
    float x = 0, y = 0, z = 0;      // feet
    float vx = 0, vy = 0, vz = 0;
    float viewY = 0;                // smoothed eye height (steps up/down glide)
    float health = 104, armor = 10;
    bool alive = true;
    bool onGround = true;
    float deadTime = 0;
    float hurt = 0;                 // red flash, 0..1
    float tear = 0;                 // the game's hit tear (0xe41a6): grows by the damage taken
    float bonus = 0;                // pickup flash, 0..1
    float hurtYaw = 0, hurtShow = 0;// direction of the last hit (world yaw) and how long to show it
    float bob = 0;                  // walking phase
    float fallTicks = 0;            // part of a 100 Hz tick of flight not yet taken
    bool takeoff = false;           // the next tick of flight is the jump's first (no gravity yet)
    // Kit (a new game: the gauss pistol with 50 rounds, health 104, armour 10).
    bool owned[9] = {true, false, false, false, false, false, false, false, true};
    int ammo[9] = {50, 0, 0, 0, 0, 0, 0, 0, 0};
    int weapon = 0;
    int bioKits = 0, mechKits = 0;
    int suit = 0;                   // 0 sabre glove (armour up to 10), 1 razor saw (18), 2 nanotech hand (32)
    int jetFuel = -1;               // the jump jets' fuel (0xef7b2; -1 none yet: action 53 fills them)
    bool jets = false;              // the jump jets switched on (0xe41a8; off at every level start and load)
    float jetCool = 0;              // ticks after a jump before the jets fire (0x75256: 25 at take-off)
    float armorMax = 10;
    int keys = 0;                   // bit per key id
    // For the first-person view: shots fired so far (recoil and muzzle flashes), when the glove's
    // swing began (world seconds; -100: none), and whether fire is held (the chaingun spins).
    int shots = 0;
    float swingAt = -100;
    bool firing = false;
};

struct PlayerInput {
    float forward = 0, strafe = 0;  // -1..1
    bool run = false;
    bool jump = false;
};

struct HudMessage {
    std::string text;
    int font = 0;                   // Hud::Font
    float age = 0;
};

// A sound the game starts (SOUND\<id>.RAW, FUN_0004fa92): at a place (its "mode 0": the listener
// decides volume and pan), or at a set volume (0-255) and pan (-127..127) ("mode 1").
struct SoundEvent {
    int id = 0;
    bool at = false;
    float x = 0, z = 0;
    int volume = 200, pan = 0;
    float range = 640;   // placed sounds: the distance (the game's measure) over which they fade out
};

struct World {
    World();
    ~World();
    // Settings the viewer can toggle.
    bool smooth = true;             // smooth turning and frame blending (off = original snapping)
    bool monstersMove = true;
    int difficulty = 1;             // 0 easy, 1 normal, 2 hard: which monsters and items a level places
    bool enhancedAI = false;        // false: the 1994 AI, ported; true: the enhanced AI
    bool monstersAttack = true;
    bool godMode = false;

    void init(GameData* data, Renderer* renderer);
    void load(const Level& level, const LevelEntities& ents, unsigned seed);
    void update(float dt, const Camera& cam, bool firing);
    void buildScene(Scene& scene, const Camera& cam) const;

    void selectWeapon(int index);
    void cycleWeapon(int dir);     // next (+1) or previous (-1) weapon you have, with ammunition
    void explosionAtAim(const Camera& cam);
    // pick: what "use" points at, as the game picks it (FUN_00013524, from what it drew): through the
    // holes of a wall's texture (a cage's bars, a window) to whatever is behind.
    // sight: as a creature's sight goes (FUN_00020f42): 1 through walls flagged 0x02 (windows, grates),
    // 2 stopped by every wall; either way blocks never stop it, and doors only while fully shut.
    bool raycast(const float o[3], const float d[3], float maxT, float& t, float n[3], int* door = nullptr,
                 bool pick = false, int sight = 0) const;
    bool rayHitsAnyMonster(const float o[3], const float d[3]) const;  // before any wall (testing aid)

    std::string status() const;
    std::string weaponName() const;
    bool monsterInfo(int index, float out[5]) const;            // x, y, z, height, yaw (for --focus)
    bool entityInfo(char kind, int index, float out[5]) const;  // kind: m monster, o object, i item, d door
    float shake = 0;

    // Player
    void spawnPlayer(float x, float z, float yaw);
    void movePlayer(const PlayerInput& in, float dt, Camera& cam);   // walking with collision; sets cam x/y/z
    void placePlayer(const Camera& cam);                             // free flight: the body follows the camera
    const PlayerState& player() const { return player_; }
    float time() const { return time_; }
    void damagePlayer(float amount, const float* from, int type = 1);
    // After a frame is drawn: the tear wears off by the ticks that passed, and is held to 50
    // (so only the first frame of a big hit tears more).
    void wearTear(float dt) {
        float t = dt * 100.0f;
        player_.tear = player_.tear < t ? 0.0f : std::min(50.0f, player_.tear - t);
    }
    bool useKit(int which);
    void giveKits(int bio, int mech) { player_.bioKits = bio; player_.mechKits = mech; ensureKitSlots(); }   // testing                                          // 0 bio-kit (+48 health), 1 mech-kit (+5 armour)
    // keepInventory: a click in the view with the inventory open (it doesn't close it, as "use" with
    // nothing readied does).
    bool useAt(const Camera& cam, bool keepInventory = false);
    std::string aimedItem(const Camera& cam) const;   // what "use" would do now ("" if nothing in reach)
    // What the crosshair is on, as the game colours its cursor (FUN_000358a1): 0 nothing,
    // 1 an item, 2 a creature, 3 a door (hidden ones too).
    int cursorPick(const Camera& cam) const;
    bool startWithAllWeapons = false;
    bool weaponAbsent[8] = {};   // the demo has only the first three weapons' art: the others aren't given
    bool autoPickup = true;          // walking over an item takes it (false: only "use" does, as in the game)
    bool bumpDoors = false;          // walking into a door opens it (false: only "use" does, as in the game)
    const std::vector<HudMessage>& messages() const { return messages_; }
    void message(const std::string& text, int font = 0);
    // The sounds started since the last call.
    std::vector<SoundEvent> takeSounds() {
        std::vector<SoundEvent> s;
        s.swap(sounds_);
        return s;
    }

    // Saved games (save.cpp): the state the level has reached since load(); loadState() goes over a
    // fresh load() of the same level at the same difficulty.
    void saveState(std::vector<uint8_t>& out) const;
    bool loadState(const std::vector<uint8_t>& in, std::string& err);

    // Level scripts (script.cpp). The mission now playing (the game's 0xd4290), set before load():
    // a script moving the player to another mission number changes level.
    int mission = 0;
    // A move a script asked for (FUN_0003733b): within the level (a teleporter: the screen fades
    // out first), or on to another mission (its video, then the new level). The caller carries it out.
    struct Warp {
        bool pending = false;
        int mission = 0;         // 0: this level
        int cellX = 0, cellZ = 0;
        float y = 0;             // the surface under this height is the one to stand on
        float yaw = 0;           // the way to face (radians, 0 north)
        int video = 0;           // between missions: the video to play first (0: none)
        bool end = false;        // the end of the game (action 25): the last videos, then the menu
    };
    Warp takeWarp() { Warp w = warp_; warp_ = Warp(); return w; }
    // Puts the player in a cell, on the surface under y (a teleporter, or a new mission).
    void warpPlayer(int cellX, int cellZ, float y);
    // A new mission keeps the player's kit from the last (FUN_0003733b): health, armour, suit,
    // weapons, ammunition and kits, but no keys; missions 4, 7, 11 and 14 begin with health 104
    // and full armour.
    void carryOver(const PlayerState& from, int newMission);
    // Walls a script changed since the last call (the automap draws them again).
    bool takeLevelChanged() { bool c = levelChanged_; levelChanged_ = false; return c; }
    // The level as loaded, which scripts may change (switches, sliding walls): the caller's own copy.
    void setEditableLevel(Level* level) { levelEdit_ = level; }

    // The inventory (inventory.cpp): eight slots of quest items, activated items, demo packs and
    // the kit stacks.
    struct InventorySlot {
        bool used = false;
        std::string icon, name;   // icon: its picture (i_... in CYCLONE.HD), "" if it has none
        int use = 0, count = 0;   // count: a kit stack's kits
        bool active = false, readied = false;
    };
    std::array<InventorySlot, 8> inventory() const;
    bool inventoryOpen() const { return invOpen_; }
    int inventoryCursor() const { return invCursor_; }
    int readiedSlot() const { return readied_; }
    void toggleInventory();
    void inventoryMove(int dir);
    void inventorySelect(int slot);   // -1: the highlighted slot
    void inventoryDrop(int slot);     // -1: the highlighted slot
    int placedDemoPacks() const { return (int)demoPlaced_.size(); }
    void detonate();                  // the Demo Pack Icon: every placed pack goes off
    int jetFuel() const { return player_.jetFuel; }
    bool jetsOn() const { return player_.jets; }
    std::string jetsKey = "J";        // the jets' key's name, for the pickup message
    void toggleJets();                // the J key and the Jump Jet Icon (FUN_000409e5)

    // Automap
    void mapLevel(MapLevel& out) const;                                  // static plan, after load()
    // Markers and explored ground. markers: 0 none, 1 pickups, 2 pickups and creatures.
    void mapFrame(MapFrame& out, const Camera& cam, bool all, int markers) const;

private:
    struct Monster;
    struct Item;
    struct Object;
    struct Door;
    struct Shot;
    struct Effect;
    struct Particle;
    struct Light;
    struct Pending;

    // Level queries
    float surfaceY(const SurfaceLayer& s, int shapeShift, float wx, float wz) const;
    float groundBelow(float wx, float wz, float y, float* ceiling = nullptr) const;
    bool stepTarget(int cx, int cz, int nx, int nz, float y, float height, float& newY) const;
    bool edgeBlocks(int cx, int cz, int nx, int nz, float y0, float y1, float* bottom = nullptr) const;
    float cellLight(float wx, float wz) const;
    // A creature's sight, through windows and grates (FUN_00020f42 with its last argument 0: sight,
    // hitscan shots, blasts); solid: every wall in the way (1: the melee blow, FUN_00032295). Blocks
    // (crates, desks, pillars) never stop it, and a door only while fully shut.
    bool lineOfSight(const float a[3], const float b[3], bool solid = false) const;
    void buildCollision();

    // Art
    int lump(const std::string& name, bool cdFirst, const Bol** bol) const;
    bool sprite(const std::string& name, bool cdFirst, SpriteImage& out) const;
    const Visual* monsterVisual(const MonsterType& t);
    const Visual* spriteVisual(const std::string& key, std::vector<std::string> frames, bool cdFirst, bool loop,
                               float frameSeconds, bool decal = false);
    const Visual* rotVisual(const std::string& key, const std::vector<std::string>& firstViewOfEachFrame, bool cdFirst,
                            float frameSeconds);
    const Visual* fx(const std::string& key) const;
    void requestEffects();
    void buildPending();

    // Gameplay showcase
    void fire(const Camera& cam);
    void impact(const Shot& s, const float p[3], const float n[3], Monster* hitMonster);
    void explode(float x, float y, float z, float power, const char* mainSprite);
    void damageMonster(Monster& m, float amount, const float* from, int type = 0, bool byPlayer = true);
    void playerHitscan(const Camera& cam, int weapon);
    void playerMelee(const Camera& cam);
    void updateShots(float dt);
    void updateFiring(float dt, const Camera& cam, bool firing);
    void makeNoise(float x, float y, float z, float range);
    void dropItem(const Monster& m);
    // Monsters (ai.cpp)
    void updateMonsters(float dt, const Camera& cam);
    void updateOriginalAI(Monster& m, float dt);
    int axisHeading(const Monster& m);           // FUN_0002ae02
    void originalAttackEnded(Monster& m);        // the chaser's decision when an attack ends
    void blockedRetry(Monster& m);
    void updateEnhancedAI(Monster& m, float dt);
    void wanderMonster(Monster& m, float dt);
    bool monsterFire(Monster& m, int slot);
    void startAttack(Monster& m, int slot);
    void monsterStep(Monster& m, float dt, bool& walking, float& moved);
    bool chooseStep(Monster& m, int dx, int dz);
    // Fliers (ai.cpp): moved as the game moves them, a box at a height that surfaces, walls and
    // doors stop (FUN_00047abe, FUN_00047ed3), not along the floors.
    float boxHeight(const Monster& m) const;
    int boxPoints(const Monster& m, float* px, float* pz) const;
    bool flierRoom(const Monster& m, const float* px, const float* pz, int n, float y, float& floor, float& room) const;
    bool flierFits(const Monster& m, const float* px, const float* pz, int n, float& y, float* room = nullptr) const;
    float flierTarget(const Monster& m) const;
    float walkFrameTics(const Monster& m) const;
    bool flierStep(const Monster& m, int nx, int nz, float& newY, float& duck) const;
    float playerEye() const { return player_.viewY; }
    void areaBlast(const Monster& m);
    bool findPath(const Monster& m, int goalX, int goalZ, float goalY, std::vector<int>& out, bool* partial = nullptr) const;
    void decodeWeapons(const MonsterType& t, struct MonsterWeapon* out);
    void updatePickups();
    bool takeItem(Item& it, bool asked);
    bool takeItemNow(Item& it, bool asked);
    const Item* itemInSight(const Camera& cam) const;
    float bodyGround(float x, float z, float feet, float* ceiling) const;
    // Ground at (x, z) at all: a floor layer with a texture in its cell, or a block over it (HIVE1's
    // stairs at (36, 27-29) are blocks over cells that have no floor of their own).
    bool hasGround(float x, float z) const;
    void beginAttack(Monster& m, int slot);
    void foldUp(Monster& m);
    void foldedWalk(Monster& m);
    int openFrame(const Monster& m) const;   // the attack frame a deployer holds while it waits open
    static float originalWalkSpeed(const MonsterType& t);
    float playerVelX_ = 0, playerVelZ_ = 0, lastPX_ = 0, lastPZ_ = 0;   // the player's smoothed ground velocity
    int pathBudget_ = 0;      // route searches left this frame
    uint64_t aiNs_ = 0, pathNs_ = 0, pathNsMax_ = 0, losNsMax_ = 0;
    mutable uint64_t losNs_ = 0;       // testing: slowest creature update since the last report
    int traceMonster_ = -1;   // testing: log one creature's AI (CYCLONES_TRACE=index)
    void splashDamage(float x, float y, float z, int amount, int type, const Monster* skip, bool skipPlayer);
    // Ray against what the monster looks like from the ray's origin; d must be normalised.
    bool rayHitsMonster(const Monster& m, const float o[3], const float d[3], float maxT, float& t, float pad = 0) const;
    bool rayHitsMonsterThin(const Monster& m, const float o[3], const float d[3], float maxT, float& t, bool flight = false) const;
    bool doorPanel(const Door& d, float& x0, float& z0, float& x1, float& z1, float& y0, float& y1) const;
    bool rayHitsDoor(const Door& d, const float o[3], const float dir[3], float maxT, float& t, float n[3]) const;
    int doorInSight(const Camera& cam, float* dist = nullptr) const;
    void openDoor(Door& d);
    bool doorOpens(const Door& d, bool player) const;
    bool doorBlocked(const Door& d) const;
    mutable int bumpedDoor_ = -1;         // the door that stopped the player's last move
    void spawnEffect(const std::string& key, float x, float y, float z, float scale0, float scale1, uint8_t mode,
                     bool fullbright, float delay = 0, float roll = 0);
    void addParticle(const Particle& p);
    void addMonsterDraw(Scene& sc, const Monster& m, const Camera& cam) const;
    // A keepsake in BASE (keepsake.cpp): a framed photograph on the wooden house's wall.
    void buildKeepsake();
    void addKeepsakeDraw(Scene& sc, const Camera& cam) const;

    // SDILAB's security scan rooms (scan.cpp; an addition, not in the game): a script that prints
    // "Conducting Security Scan" when the player steps on its cell, the monitor on the room's wall,
    // and the scan's light on the floor. What they show follows the script's progress, so a saved
    // game picks up where it was.
    struct ScanRoom {
        int script = -1, iScan = -1, iDone = -1, iOk = -1;   // its "Conducting", "Scan Complete", "APPROVED"
        int monitor = -1;                    // its screen: Renderer::kScanScreen + monitor
        float cx = 0, cz = 0, floorY = 0;    // the scan cell's middle and its floor
        float durStand = 0.5f, durScan = 1, durDone = 1;   // how long each part lasts (its waits)
        int phase = 0;                       // as ScanScreen::phase
        float since = 0;                     // time_ when the phase began
    };
    std::vector<ScanRoom> scans_;
    // SDI1's missile consoles (scan.cpp; an addition): a wall showing wall360 whose cell script (used
    // from that wall) prints "Missile ..."; its screen shows the arming, then the armed missile.
    struct MissileConsole {
        int script = -1, index = 0;
        int phase = 0;      // 0 not used yet, 1 arming, 2 armed
        float since = 0;    // when it was used (time_)
        bool seen = false;  // its script seen running or done
        bool checked = false;   // looked at since the level (or a saved game) was loaded
    };
    std::vector<MissileConsole> consoles_;
    // SDI2's launch computer (scan.cpp; an addition): the walls showing wall329 and wall330; its
    // screen runs from when the script printing "Missile Launch ..." starts (the launch codes going
    // into the console across the room), and holds the running sequence after.
    MissileConsole launch_;
    // The aliens' pod computers a pod goes into (scan.cpp; an addition): the walls showing wall93 in
    // a cell a script takes an item at (HIVE1's two); waiting, taking the pod (phase 1, kPodTake
    // seconds from when the script runs), done.
    std::vector<MissileConsole> pods_;
    static constexpr float kPodTake = 2.0f;
    static constexpr float kArming = 2.2f;   // seconds the arming shows
    void initScans();
    void scanTick();
    void addScanDraw(Scene& sc, std::vector<LightDraw>& lights) const;
    ModelRange keepsake_;
    bool keepsakeHere_ = false;

    GameData* data_ = nullptr;
    Renderer* renderer_ = nullptr;
    const Level* level_ = nullptr;
    std::mt19937 rng_;
    float time_ = 0;

    std::map<std::string, std::unique_ptr<Visual>> visuals_;
    std::vector<std::unique_ptr<Monster>> monsters_;
    std::vector<Item> items_;
    std::vector<Object> objects_;
    std::vector<Door> doors_;
    std::vector<Shot> shots_;
    std::vector<Effect> effects_;
    std::vector<Particle> particles_;
    std::vector<Light> lights_;
    std::vector<Pending> pending_;
    std::vector<std::vector<int>> cubesInCell_;
    std::vector<float> cubeHeight_;   // per cube: [13] - [12], or its side texture's height when that is 0
    std::vector<uint8_t> occupied_;
    std::vector<uint8_t> fixed_;      // cells held for good: props and creatures that never move

    std::vector<float> tris_;
    std::vector<std::vector<int>> triCells_;

    float cooldown_ = 0, meleeTimer_ = 0;
    // The last loud noise (gunfire, explosions), for monsters that listen (enhanced AI).
    float noiseX_ = 0, noiseY_ = 0, noiseZ_ = 0, noiseTime_ = -100, noiseRange_ = 0;
    int ticks_ = 0;                    // 100 Hz tick counter (the game's random rules use it)
    int airTicks_ = 0;                 // ticks of flight (the jump jets burn fuel every 8th)
    float tickAcc_ = 0;
    int kills_ = 0;
    float pal_[256][3];
    std::vector<float> sparkRamp_;

    // Player
    bool playerFits(float x, float z, float feet, float* ground) const;
    void stepCheck();   // testing (CYCLONES_STEPCHECK): steps the player can stand on but not get onto
    PlayerState player_;
    std::vector<HudMessage> messages_;

    // Sounds
    std::vector<SoundEvent> sounds_;
    void soundAt(int id, float x, float z, float range = 640) {
        if (id > 0 && id < 256 && sounds_.size() < 64) sounds_.push_back({id, true, x, z, 0, 0, range});
    }
    // A creature's own sound (its shots, pain, death, blows). The game fades every placed sound out
    // over 640 units; gunballs and turrets carry twice as far here (an addition): they keep firing
    // from far down a corridor, in plain sight, and went quiet long before they were out of view.
    void creatureSound(const Monster& m, int id);
    void sound(int id, int volume = 200) {
        if (id > 0 && id < 256 && sounds_.size() < 64) sounds_.push_back({id, false, 0, 0, volume, 0});
    }

    // Level scripts (script.cpp)
    struct ScriptRun {
        int flags = 0;        // the script's own (0x02 repeat, 0x08 can run again) + 0x01 running, 0x04 finished
        uint8_t events = 0;   // what the player just did to it: 0x01 used (a creature or prop), 0x04 took
                              // (an item), 0x08 used (a door), 0x20 used the north wall, 0x40 the west wall
        int timer = 0;        // its wait, in eighths of a tick (64 come off every 8 ticks)
        int pc = -1;          // the next action (-1: none)
        int32_t p[4] = {};    // its parameters (+0x20, the u32s conditions 8, 11-17 and 25 compare): from
                              // the file, then what a readied item was used on, or a prop's slot
    };
    std::vector<Script> scripts_;
    std::vector<ScriptRun> runs_;
    std::vector<std::vector<int>> cellScripts_;   // per cell (z * 64 + x): its trigger scripts, in order
    std::vector<int> scriptQueue_;                // running scripts, soonest first
    uint8_t scriptVars_[10] = {};                 // the game's counters (0xde4c0)
    struct WallEdit { int x, z, side, front, back, flags, base; bool remove; };
    std::vector<WallEdit> wallEdits_;             // every wall a script changed, in order (for saves)
    bool geometryDirty_ = false, levelChanged_ = false;
    Warp warp_;
    Level* levelEdit_ = nullptr;
    std::vector<uint8_t> scriptUnsupported_;      // per script: uses an op the engine doesn't run yet
    void initScripts(const LevelEntities& E);
    static bool scriptOpsSupported(const Script& s);
    void runCellScripts(int x, int z);            // FUN_0003a8e3
    void tryScript(int i);                        // FUN_0003a856
    void entityScript(int i, uint8_t event);      // an entity's script, after the player acted on it
    bool scriptCondition(int i, const ScriptAction& c);   // FUN_0003a316
    void queueScript(int i);                      // FUN_0003a95f
    void advanceScripts(int amount);              // FUN_0003aff5
    void scriptAction(int i);                     // FUN_0003ab56
    void scriptTick();                            // once per 100 Hz tick
    int scriptDoor(int cx, int cz) const;         // FUN_00037a86: the first door in a cell (-1: none)
    void editWall(const WallEdit& e, bool record);
    void applyLevelEdits();
    // Blocks, walls and floors the scripts move (actions 33, 8 and 9), 4 units a step, a step
    // every `speed` ticks (the game's movers, 0xdebb4: 50 of 0x27 bytes, run by FUN_0003ce15).
    struct Mover {
        int kind = 0;             // 1 a floor (action 9), 2 a wall (8), 3 a block (33), 5 walls (44), 6 a hazard (54)
        int x = 0, z = 0, which = 0;   // a wall or floor: its cell, and the wall's side (0 west, 1 north) or the surface (0 floor);
                                       // a hazard (kind 6): its area
        int x1 = 0, z1 = 0;       // a block of walls (kind 5, action 44): the far corner
        int id = 0, cube = -1;    // a block: its id, and the first block with it (which keeps the "moving" mark)
        int step = 4;             // +4 or -4 a step
        int steps = 0, done = 0;  // a wall or floor: the steps to take, and taken
        int target = 0;           // a block: the base it moves to
        int speed = 1;            // ticks between steps
        int last = 0;             // the tick of the last step
    };
    std::vector<Mover> movers_;
    std::vector<int16_t> cubeStartBase_;  // per block: its base as the level began
    std::vector<uint8_t> cubeMoving_;   // per block: a mover has it, so other moves of it are ignored (+0x40)
    std::vector<uint8_t> movingWalls_, movingSurfaces_;   // per cell: what scripts move (Renderer::setMovingParts)
    std::vector<int> slotMonster_;      // per slot of the game's creature table: index in monsters_ (-1: none)
    std::map<int, std::string> itemNames_;   // the level's item names (GITX)
    std::array<int, 8> inv_{{-1, -1, -1, -1, -1, -1, -1, -1}};   // per slot: an index in items_ (-1: empty)
    int readied_ = -1, invCursor_ = 0;
    bool invOpen_ = false;
    std::vector<int> demoPlaced_;       // the placed demo packs (0xe43d4, at most 8)
    bool detonating_ = false;           // 0x74f02: the packs are going off (condition 27)
    bool hazardOn_ = false;             // 0x7525c: an action 54 hazard is running
    enum UseTarget { UseNothing, UseWall, UseFloor, UseCreature, UseItem, UseProp, UseDoor };
    void nameItem(Item& it, int kind, int invPic, int setPic);
    int packSlot(int item) const;
    int packSlotOfUse(int use) const;
    int freePackSlot() const;
    bool packItem(Item& it);
    void removeItem(int item);          // FUN_000412f1
    void kitStackEmpty(int use);
    void ensureKitSlots();
    bool readiedWaitsForNothing() const;
    bool useReadied(UseTarget what, int p0, int p1, int p2);   // FUN_0004032d; true: the item's script started
    void placeDemoPack(int slot);
    void dropFromPack(int slot);
    void demoBlast(int radius, int item);   // action 46 (FUN_00033977)
    std::vector<int> slotObject_;       // per prop slot: index in objects_ (-1: none)
    Monster* creatureInSight(const Camera& cam, float maxT, float* dist);
    Object* propInSight(const Camera& cam, float maxT, float* dist);
    void useCreature(Monster& m);
    void useProp(Object& o);
    int scriptItem(int script) const;   // the item a script belongs to (-1: none)
    Object* slotProp(int slot);
    void setPropPicture(Object& o, int picture);   // action 55: 'startobjects' + picture
    // Animated walls, flats, items and props (surfanim.cpp)
    struct SurfAnim { bool on = false; int kind = 0, period = 1, count = 0, first = 0, last = 0; };
    std::vector<SurfAnim> surfAnims_;
    std::vector<uint16_t> animFrame_[5];   // per kind (0 walls, 1 flats, 2 items, 3 props): the picture each number shows
    struct AnimRange { int number = -1, first = 0, last = 0; std::vector<std::string> frames; };
    void initSurfAnims();
    void surfAnimTick();
    void setSurfAnim(int entry, bool on);
    AnimRange spriteAnim(int kind, const std::string& lump) const;
    const Visual* itemVisual(const std::string& sprite, Item& it);
    int animFrameOf(int kind, int number, int first) const {
        return number < 0 || number >= (int)animFrame_[kind].size() ? 0 : animFrame_[kind][number] - first;
    }
    int standLayer_ = 0;                // the surface layer the player last stood on (0xef416; 255: a block, a creature)
    Monster* slotCreature(int slot);    // the creature in a slot, if it is there
    void spawnCopy(int from, int cx, int cz);    // action 14 (FUN_00048a30)
    void setCreatureMode(int slot, int mode);    // action 16 (FUN_0002b31e -> FUN_0002af83)
    void dropCarried(int slot);                  // action 41 (FUN_00048eeb)
    size_t staticTris_ = 0;             // tris_ past this many are the moving parts'
    std::vector<int> movingTriCells_;   // the cells whose triangle lists hold them
    bool movedGeometry_ = false, navDirty_ = false;
    void findMovingParts();             // which walls and surfaces the scripts move (before the level is built)
    void startMover(int op, const uint8_t* a);   // actions 8 (FUN_00022109), 9 (FUN_000222ea), 33 (FUN_0002284e)
    bool moverStep(Mover& m);           // false once it has finished
    bool moveCubes(int id, int delta, int* lastBase);   // FUN_000126fb: false if the last one was in the way
    void moverTick();
    void refreshMoving();               // the moving parts' geometry and collision, built again
    void indexTriangles(size_t from);
    bool movingHeights(std::vector<int16_t>& h, bool put) const;   // for saves
    bool useWall(const Camera& cam);              // FUN_0003fed1 case 1: "use" on a wall

    void explore(const Camera& cam);      // marks the ground in view as explored for the automap
    float topSurface(int cx, int cz, int* flat = nullptr) const;
    std::vector<uint8_t> explored_;
    // Walk maps. A cell can hold up to three floors one above another (a catwalk over a pit); each
    // is a node. Per node, for each body height (rounded up to 16) of the creatures on the level:
    // bits 0-7 = which of the 8 neighbours it can step to, bits 8+2k = onto which of their floors.
    static constexpr int kNavLevels = 3;
    std::vector<float> navY_;             // floor height per node (-1e9: none)
    std::map<int, std::vector<uint32_t>> nav_;
    static int navHeight(float h) { return std::max(16, ((int)std::ceil(h) + 15) / 16 * 16); }
    int navLevel(int cell, float y) const;   // the floor of a cell nearest a height (-1: none)
    std::vector<int> doorCell_;           // per cell: door index + 1 (0 = none)
    int playerCellX_ = -1, playerCellZ_ = -1;
    void buildNav();
    float exploreTimer_ = 0;
};
