// Internals shared by the world's source files (world.cpp, player.cpp, ai.cpp): helpers, the
// weapon table and the live entity records.
#pragma once
#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "world.h"

namespace {

const float kPi = 3.14159265358979f;
const int kDataTransmitter = 59;   // BIOLAB's data transmitters (wall texture 59, lump wall57)
const float kTicRate = 100.0f;     // the game logic runs at 100 ticks per second (timer at 100 Hz)
const float kFlashHold = 8 / kTicRate;   // a light-coded muzzle flash stays drawn this long (the game: 5 ticks)
const float kGravity = 800.0f;

inline std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

inline float frand(std::mt19937& r, float a, float b) { return std::uniform_real_distribution<float>(a, b)(r); }

inline void yawMatrix(float yaw, float* m) {  // model x/y/z axes in world space (column-major)
    float c = std::cos(yaw), s = std::sin(yaw);
    float v[9] = {-c, 0, -s, 0, 1, 0, s, 0, -c};
    std::memcpy(m, v, sizeof(v));
}

inline void dirMatrix(const float* f, float* m) {  // model +z along f, +y as close to up as possible
    float z[3] = {f[0], f[1], f[2]};
    float l = std::sqrt(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
    if (l < 1e-6f) { yawMatrix(0, m); return; }
    for (float& v : z) v /= l;
    float x[3] = {z[2], 0, -z[0]};  // X = up x Z (matches yawMatrix for level directions)
    float xl = std::sqrt(x[0] * x[0] + x[2] * x[2]);
    if (xl < 1e-5f) { x[0] = 1; x[2] = 0; xl = 1; }
    x[0] /= xl; x[2] /= xl;
    float y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};  // Z x X
    float v[9] = {x[0], x[1], x[2], y[0], y[1], y[2], z[0], z[1], z[2]};
    std::memcpy(m, v, sizeof(v));
}

inline float angleDiff(float a, float b) {  // shortest signed a - b
    float d = std::fmod(a - b + kPi, 2 * kPi);
    if (d < 0) d += 2 * kPi;
    return d - kPi;
}

inline float smoothstep(float a, float b, float x) {
    float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}

// The player's weapons, as CYCLONE.EXE builds them (FUN_000509ee; table at 0xef429). Periods are
// the effective ones (the game checks held fire every 8 ticks). Hitscan damage and accuracy are
// per range band (horizontal distance up to 96, 224, 352 and 608 units); projectiles list the
// direct-hit damage in band 0 and the fixed splash damage in band 1.
struct WeaponDef {
    const char* name;
    const char* shot;      // projectile sprite ("" = hitscan or melee)
    const char* impact;    // impact animation
    int kind;              // 0 hitscan, 1 projectile, 2 melee
    int bolts;             // projectiles per pull (one ammo per pull)
    int period;            // ticks between shots
    int maxAmmo;
    int dmgMin[4], dmgMax[4];
    int acc[4];
    int type;              // damage type: 0 kinetic, 1 energy, 2 explosive, 4 nanotech (ignores armour)
    float speed;           // projectile units per second
    float fuse;            // seconds; a fuse also means gravity and bouncing (grenade)
    float ahead, side, up; // spawn point: ahead of the eye, sideways (pairs: either side), height from the eye,
                           // as the game has them (FUN_00031342): 13 ahead and 24 below for the hand
                           // weapons (the alien rifle's pair 10 to either side), and the suit weapons
                           // from the shoulders, 5 above the eye: the twin laser's pair 13 to either
                           // side of you (none ahead), the rocket 13 ahead.
};
const WeaponDef kWeapons[] = {
    {"GAUSS PISTOL", "", "puff", 0, 1, 24, 200, {12, 10, 8, 6}, {24, 20, 16, 12}, {90, 80, 65, 40}, 0, 0, 0, 0, 0, 0},
    {"PHOTON CHAINGUN", "", "puff", 0, 1, 8, 400, {8, 6, 6, 5}, {20, 15, 12, 10}, {80, 60, 30, 10}, 0, 0, 0, 0, 0, 0},
    {"ALIEN PISTOL", "approj", "approjd", 1, 1, 40, 50, {20, 0, 0, 0}, {30, 0, 0, 0}, {0, 0, 0, 0}, 1, 900, 0, 13, 0, -24},
    {"ALIEN RIFLE", "dblproj", "dblprjd", 1, 2, 32, 100, {25, 0, 0, 0}, {35, 0, 0, 0}, {0, 0, 0, 0}, 1, 800, 0, 13, 10, -24},
    {"GRENADE LAUNCHER", "grenade", "grnaded", 1, 1, 56, 25, {60, 45, 0, 0}, {80, 45, 0, 0}, {0, 0, 0, 0}, 2, 800, 2.0f, 13, 0, -24},
    {"SUIT-MOUNTED TWIN LASER", "lasprj", "lasprjd", 1, 2, 16, 400, {25, 0, 0, 0}, {40, 0, 0, 0}, {0, 0, 0, 0}, 1, 1000, 0, 0, 13, 5},
    {"SUIT-MOUNTED ROCKET LAUNCHER", "rktprj", "rcktexp", 1, 1, 56, 25, {75, 40, 0, 0}, {100, 40, 0, 0}, {0, 0, 0, 0}, 2, 900, 0, 13, 0, 5},
    {"SUIT-MOUNTED PLASMA ACCELERATOR", "", "rdplsmd", 0, 1, 104, 50, {100, 90, 70, 55}, {150, 125, 100, 75}, {120, 120, 120, 120}, 4, 0, 0, 0, 0, 0},
    {"SABRE GLOVE", "", "", 2, 1, 25, 0, {10, 0, 0, 0}, {20, 0, 0, 0}, {100, 0, 0, 0}, 0, 0, 0, 0, 0, 0},
};
const int kWeaponCount = (int)(sizeof(kWeapons) / sizeof(kWeapons[0]));

}  // namespace

// Picks the frame and morph position for an animation clock (defined in world.cpp).
void animFrame(const std::vector<float>& seconds, float time, bool loop, int& frame, float& t);

// ---------------------------------------------------------------------------------------------

struct Visual::Staged {
    struct Anim {
        std::vector<std::vector<SplatMorph>> still, morph;
        std::vector<float> seconds, light;
        std::vector<int> height, width, anchor;
        bool loop = false, hold = true;
    };
    struct Rot {
        std::vector<std::array<std::vector<SplatMorph>, 8>> still, morph;
        std::vector<float> seconds;
    };
    Rot rot;
    Anim flat, pain, death, attackA, attackB, ready;
};

// A monster type's weapon, decoded from the ITEM record embedded in its BMON type.
struct MonsterWeapon {
    int kind = 0;              // 1 hitscan, 2 projectile, 11 melee, 12 area blast; 0 none
    int acc[4] = {};           // hitscan / melee: accuracy bonus per range band
    int dmgMin[4] = {}, dmgMax[4] = {};
    int type = 1;              // damage type
    float speed = 0;           // projectile: units per second (capped at 1000, as in the game)
    float aimSpeed = 0;        // the uncapped speed the game aims with
    bool lob = false;          // projectile falls and bounces
    float fuse = 0;            // seconds (0: none)
    int splash = 0;            // fixed splash damage (0: none)
    std::string visual, impact;// effect keys for the flight and the impact
    float lr = 0, lg = 0, lb = 0;  // flight light colour
    bool melee() const { return kind == 11; }
    bool ranged() const { return kind == 1 || kind == 2; }
};

// The suits' armour at full (0xf2cea per suit), and the pickup sound (FUN_000420c0: 81).
inline constexpr float kSuitMax[3] = {10, 18, 32};
inline constexpr int kPickupSound = 81;

// Testing: CYCLONES_SCRIPTS=1 logs the level scripts, movers, creatures and items they touch.
inline bool logScripts() {
    static const bool on = SDL_getenv("CYCLONES_SCRIPTS") != nullptr;
    return on;
}

struct World::Monster {
    enum State { Walk, Pain, Dying, Dead, Attack, Ready, Unready, Stun };
    const Visual* vis = nullptr;
    const MonsterType* type = nullptr;
    bool barrel = false, stationary = false;
    float x = 0, y = 0, z = 0, yaw = 0;
    float tx = 0, tz = 0, ty = 0;
    float ground = 0;       // the floor it stands on: steps are judged from here, not from the drawn
                            // height, which eases toward each new floor and lags a step behind
    bool moving = false;
    int cx = 0, cz = 0, ncx = 0, ncz = 0;
    float wait = 0, step = 0, speed = 56, flash = 0, look = 0, sightTimer = 0;
    State state = Walk;
    float stateTime = 0, hp = 100;
    int blood = 0;  // 0 red, 1 green, 2 machine
    int view = -1;          // which original rotation faces the camera (switches with hysteresis)
    float stagger = 0;      // brief stop when hit (creatures without pain frames)
    float painReady = 0;    // world time when the next hit reaction may start
    bool gone = false;      // death animation ended without a corpse
    // Placement and type data
    int index = 0;
    int slot = -1;          // its place in the game's creature table (0xe95c4): the level's creatures
                            // for this difficulty in file order, then those scripts bring in; scripts
                            // name creatures by it
    int copyOf = -1;        // brought in by a script: the index of the creature it was copied from
    int aiClass = 1;        // 1 chaser, 2 deployer, 3 exploding object, 4 turret, 6 queen, 0 none
    int startMode = 1;      // 13: frozen (barrels)
    int sight = 10, trigger = 0, bandA = 0, bandB = 0;
    float armour[4] = {};
    int species = 0;        // 0 red-blooded, 1 machine, 2 green, 3 purple, 10 the queen
    float eye = 32;         // gun height above the feet
    float hover = 0;        // fliers hover this high (and dive within 192 units)
    float duck = -1e9f;     // a flier of the enhanced AI sinks to this height before it sets off
                            // under a catwalk or lintel ahead (-1e9: no need)
    int evasion = 0, accuracy = 0;
    MonsterWeapon weapon[2];
    int sndPain = 0, sndMelee = 0, sndFire = 0, sndDeath = 0;   // from the placement
    bool hasDrop = false;
    int dropUse = 0, dropAmount = 0;
    std::string dropSprite;
    int dropScript = -1;
    uint8_t dropParams[40] = {};
    int script = -1;        // its script: runs when it dies (FUN_000481f2)
    int recordId = 0;       // its record's id (+0x22; a copy's is its slot), and its group (+0x47):
    int group = 0;          // what a readied item used on it tells the item's script
    int dropKind = 0;       // what it carries: the item's name and inventory picture
    int dropInvPic = 0;
    // Attack in progress
    int attackSlot = 0;     // 0 A, 1 B
    int attackFrame = -1;   // last frame whose fire flag was handled
    bool lastFire = false;  // the last shot had line of sight (the game keeps attacking while true)
    bool unfolded = false;  // class 2 (gunballs, gunposts): opened up by the ready move
    // A light-coded attack frame (a muzzle flash) held on screen a little longer than the game's 5
    // ticks: until this time (world seconds), frame flashFrame of attack flashSlot is drawn.
    float flashUntil = -1;
    int flashFrame = -1, flashSlot = 0;
    int readySlot = 1;      // the attack to make once open
    bool resumeAttack = false;  // hit while opening or firing: fire again once the recoil ends
    // AI
    bool active = false;    // original: within the activity bubble, or shot since the player moved
    bool shotActive = false;
    float thinkTimer = 0;
    int headX = 0, headZ = 0;   // original: current move direction (cells)
    bool walkOnly = false;      // original: the next think only walks (an attack ended without a shot)
    // Enhanced AI
    bool alerted = false;
    float lastSeenX = 0, lastSeenZ = 0, lastSeenY = 0, lastSeenTime = -100;
    bool seesPlayer = false;
    float perceiveTimer = 0, reaction = 0, cooldown = 0, searchUntil = 0, repath = 0, strafe = 0;
    std::vector<int> path;  // cells to walk through (index z*64+x), next first
    int pathGoal = -1;      // the cell the current route leads to
    // A deployer that can move (gunballs, the aliens' gunners) moves on after a few bursts.
    int bursts = 0, burstGoal = 0, relocate = 0;   // bursts fired here, before it moves on; cells left to go
    float relocateUntil = 0;

    float stride = 48;      // walk cycle length (units)
};

struct World::Item {
    const Visual* vis = nullptr;
    float x = 0, y = 0, z = 0, light = 0;
    int animNumber = -1, animFirst = 0;   // an animated sprite (surfanim, kind 2): its number and its range's first
    int use = 0, amount = 0;
    uint8_t params[40] = {};
    std::string sprite;
    bool taken = false;
    float denied = 0;       // "supplies already at maximum" shown recently
    int script = -1;        // its script: runs when it is taken
    // The inventory (inventory.cpp)
    int kind = 0;           // its name: the level's item names (GITX), entry kind - 1
    std::string name;
    std::string icon;       // its inventory picture (i_...), "" when it has none
    std::string setSprite;  // a demo pack's picture once placed (s1dmoset...)
    bool carried = false;   // in the player's inventory (taken stays true)
    bool active = false;    // an activated item switched on (+0x59 / +0x5a)
    bool placed = false;    // a demo pack set down to go off: it stays where it is
    bool dropped = false;   // put down at the player's feet: walking over it doesn't take it until they have stepped away
};

struct World::Object {
    const Visual* vis;
    const Visual* wreck;   // barrels: their explosion ("barreld"), played as an effect
    float x, y, z, light, step;
    bool barrel, intact, grounded;
    float wreckTime;       // < 0: hit by a blast, still standing until its own explosion
    int animNumber = -1, animFirst = 0;   // an animated sprite (surfanim, kind 3): its number and its range's first
    int slot = -1;         // its place in the level's props (scripts name props by it; +0x32)
    int script = -1;       // its script: runs when it is used (FUN_0003fed1, case 3)
    bool removed = false;  // taken away by a script (action 56)
    int picture = -1;      // the picture action 55 gave it ('startobjects' + picture; -1 its own)
};

struct World::Door {
    DoorSpawn spec;
    float cx = 0, cz = 0, base = 0, height = 0;
    float open = 0, rate = 100;
    bool opening = false;      // heading open (false: closed, or closing)
    float openFor = 0;         // seconds fully open; the game starts closing a door after 2
    bool vertical = true;      // the panel rises or sinks (false: slides along its run)
    float dir = 1;             // +1 rises / slides toward +x or +z; -1 sinks / slides the other way
    bool staysOpen = false;    // never closes by itself once open
    int group = -1;            // panels side by side that open together (a door two cells wide)
    int cellX = 0, cellZ = 0, style = 0;
    bool ew = false;
    bool locked = false;       // flag 0x01: needs key spec.key (key cards give keys)
    bool scripted = false;     // a level script can open or unlock it (scripts aren't run yet, so
                               // such a door opens without its key, as a stand-in)
    bool blown = false;        // a demo pack blew it open for good (FUN_00033977)
};

struct World::Shot {
    float x = 0, y = 0, z = 0, vx = 0, vy = 0, vz = 0, age = 0;
    int bounces = 0;
    int owner = -1;            // -1 the player; otherwise the monster that fired it (never hits itself)
    std::string visual, impact;
    int dmgMin = 0, dmgMax = 0, splash = 0, type = 1;
    float gravity = 0, fuse = 0;   // a fuse: falls and bounces until it runs out (grenades)
    int lastMonster = -1;      // grenades: the creature (index) or the player it last bounced off; a
    bool lastPlayer = false;   // fuse that runs out still touching it hits it directly, as in the game
    bool tracer = false;       // only shows a hitscan shot that has already landed
    float range = 1e9f;        // tracers: distance to travel
    float travelled = 0;
    float lr = 0.25f, lg = 0.18f, lb = 0.08f;  // flight light
};

struct World::Effect {
    const Visual* vis = nullptr;
    float x = 0, y = 0, z = 0, vx = 0, vy = 0, vz = 0, gravity = 0, drag = 0;
    float age = 0, delay = 0, life = 0;   // life 0 = the animation's own length
    float scale0 = 1, scale1 = 1, roll = 0, spin = 0, alpha = 1;
    uint8_t mode = 1;                     // 1 alpha, 2 additive
    bool fullbright = true, centred = true, bounce = false;
    bool grounded = false;                // stands on the floor (pivot at its feet) instead of centred
    std::string then;                     // animation to continue with when this one ends
};

struct World::Particle {
    enum Kind : uint8_t { Spark, Ember, Trail } kind = Spark;
    float x = 0, y = 0, z = 0, vx = 0, vy = 0, vz = 0;
    float age = 0, life = 1, size0 = 1, size1 = 1;
    float drag = 0, gravity = 0, alpha = 1;
};

struct World::Light {
    float x, y, z, radius, intensity, r, g, b, age, life;
};

struct World::Pending {
    float x, y, z, power, delay;
    const char* sprite;     // main explosion sprite ("" = the dying thing animates its own)
    bool blast = false;     // also hurts what's near (barrel props)
};

