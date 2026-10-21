// Level entities: monsters, objects, items and doors from the .MAP file, plus the monster type
// definitions ('BMON') from the level's .GLB file. Record layouts follow the field-by-field reads
// in CYCLONE.EXE (common header FUN_00010010, OBJ FUN_0001029b, DOOR FUN_00010577,
// ITEM FUN_000108dc, MON FUN_00010d05, BMON FUN_000115e2, scripts FUN_00039f07).
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct EntityBase {
    int id = 0;            // type number within its class
    int x = 0, z = 0;      // world position (units; a cell is 64)
    int y = 0;             // height of the surface it stands on
    int skill = 7;         // difficulty mask: bit0 easy, bit1 normal, bit2 hard
    int tag = 0;           // header byte 0xd (monsters: the AI class)
    int script = -1;       // its script (an index into LevelEntities::scripts), or -1
};

struct ItemSpawn;

struct MonsterSpawn : EntityBase {
    int type = 0;          // the BMON type id (record field m2; instance +0x20)
    int templ = 0;         // the editor template (record field "kind"); not the type
    int startMode = 1;     // 1/2 active, 13 frozen (barrels)
    // Sound ids (SOUND\<id>.RAW; 0 none), the u32s after the start mode: hurt, melee hit, firing,
    // death (instance +0x2c, +0x30, +0x34, +0x38).
    int sndPain = 0, sndMelee = 0, sndFire = 0, sndDeath = 0;
    int sight = 10;        // cells: notices the player within sight*64+20 (Chebyshev)
    int trigger = 0;       // class 3: blows up when the player comes this close (units)
    int group = 0;         // tag that level scripts use
    int hp = 10;           // hit points
    uint8_t armour[4] = {};// damage soaked per damage type (see FORMATS.md)
    int bandA = 0, bandB = 0;  // ranged reach in cells (max(bandA, bandB)*64+20; turrets use bandB)
    bool hasDrop = false;  // carried slot 2: the pickup left behind on death
    int dropUse = 0, dropAmount = 0, dropKind = 0, dropInvPic = 0;
    std::string dropSprite;
    int dropScript = -1;
    uint8_t dropParams[40] = {};
};

struct ObjectSpawn : EntityBase {
    int kind = 0;
    std::string sprite;    // lump name in CYCLONE.CD, e.g. "barrel", "fan10"
};

struct ItemSpawn : EntityBase {
    int kind = 0;          // index of the item's name text
    std::string sprite;    // lump name in CYCLONE.HD, e.g. "s_grnclp"
    // What it does (FUN_000420c0): 1/2 weapon (index params[0x19] / params[0x1d]), 3 ammo clip
    // (weapon params[0]), 5 activated item, 6 quest item, 7 demo pack, 8 key (id params[0]),
    // 9 bio-kit, 10 mech-kit (armour).
    int use = 0;
    int setPic = 0, invPic = 0;   // pictures relative to the sprite: placed (demo packs), in the inventory
    uint8_t params[40] = {};
    int amount = 0;        // ammo in a clip or weapon, charges
};

struct DoorSpawn : EntityBase {
    int kind = 0;
    std::string texture;   // wall lump name, e.g. "wall187"
    int flags = 0;         // 0x10: panel runs east-west; 0x80: slides (else up / down); 0x02, 0x20,
                           // 0x40: starts open, opening, closing; 0x01: locked (needs `key`)
    int flags2 = 0;        // the u16 after the flags (bit 0x8000: stays open)
    int sound = 0;         // played as it opens or closes (the second u16 after the texture)
    int key = 0;           // key id for locked doors (flag 0x01)
    int offset = 32;       // panel position inside the cell, across its run
    int speed = 3;
    int height = 0;        // 0 = one texture tall
    int vStart = 0;        // first texture row shown
    int style() const { return (flags & 0x80) ? 1 : 0; }  // 0 = moves up or down, 1 = slides
    bool runsEastWest() const { return (flags & 0x10) != 0; }
};

struct AnimSeq {           // frame numbers are lump offsets from the type's base sprite
    std::vector<uint8_t> frames, tics;  // tics: 100 Hz ticks; values >= 128 also flash light (value - 128)
    bool placeholder() const { return frames.size() == 1 && frames[0] == 0 && (tics.empty() || tics[0] == 0); }
};

struct MonsterType {
    int id = 0;
    std::string sprite;    // base lump, e.g. "alwrk00"
    std::string name;      // display name from 'MBTX', e.g. "Alien Worker"
    int health = 0;
    int accuracy = 0;      // added to an attack's per-range accuracy (type + 0x39e)
    int evasion = 0;       // taken off the player's hit chance (type + 0x3a0)
    int mirror = 0;        // bits 0/1: attack A/B frames randomly mirrored
    int eye = 0;           // gun/eye height above the feet (0: half the collision box height)
    // The type's weapons: embedded ITEM records (slot 0 attack A, slot 1 attack B).
    bool hasWeapon[3] = {};
    int weaponUse[3] = {};             // 1 hitscan, 2 projectile, 11 melee, 12 area blast
    uint8_t weaponParams[3][40] = {};
    uint8_t stats[8] = {};  // [0] walking speed (x 3.125 units/s), [2] hover height for fliers, [4] species
    AnimSeq walk;          // frames step by the rotation count (5 = mirrored set, 8 = full set)
    AnimSeq death;         // the last frame stays as a corpse only if its tic value is 0
    AnimSeq pain;          // hit reaction (slot 3; the game plays it on about one hit in four)
    AnimSeq ready;         // slot 0: unfolds before attacking, plays back reversed at double speed after
    AnimSeq attackA, attackB;
    std::vector<uint8_t> fireA, fireB;  // per-frame "fire now" flags for the attacks
};

// One action of a level script (FUN_00039f07's lists): an op and its 20 argument bytes. Ops 2, 3,
// 18-21 act on the door in cell (arg[0], arg[1]): toggle, toggle the lock, open, close, lock,
// unlock (FUN_0003789e, FUN_000378e9, FUN_000379d6, FUN_00037a2e, FUN_0003794a, FUN_00037990).
struct ScriptAction {
    int op = 0;
    uint8_t arg[20] = {};
};

// A level script (FUN_00039f07): conditions that must all hold, then actions run in order, with
// waits between them. The game keeps up to 150 (0x2f bytes each at 0xdc935).
struct Script {
    int flags = 0;                     // 0x02: its actions repeat for ever; 0x08: it can run again
    uint8_t params[16] = {};           // values some conditions compare with (script + 0x20)
    std::vector<ScriptAction> conds;   // all must hold (FUN_0003a316)
    std::vector<ScriptAction> acts;    // run by FUN_0003ab56
};

struct LevelEntities {
    std::vector<ScriptAction> actions;   // every condition and action of every script in the map and its .GLB
    std::vector<Script> scripts;         // the map's scripts: its entities' and its trigger cells'
    std::vector<std::vector<int>> cellScripts;   // 'TRIG': per cell (z * 64 + x), its chain of scripts
    std::vector<MonsterSpawn> monsters;
    std::vector<ObjectSpawn> objects;
    std::vector<ItemSpawn> items;
    std::vector<DoorSpawn> doors;
    std::vector<MonsterType> types;
    std::map<int, std::string> itemNames;   // 'GITX' (the .GLB): an item of kind k is called itemNames[k - 1]
    std::string itemName(int kind) const {
        auto it = itemNames.find(kind - 1);
        return it == itemNames.end() ? std::string() : it->second;
    }
    const MonsterType* type(int id) const {
        for (const auto& t : types) if (t.id == id) return &t;
        return nullptr;
    }
    bool parseMap(const std::vector<uint8_t>& file, std::string& error);
    bool parseGlb(const std::vector<uint8_t>& file, std::string& error);
    // A few levels define a creature with placeholder sequences (a lone frame 0). Borrow the
    // complete definition of the same sprite from another level's .GLB.
    int fillPlaceholders(const std::vector<MonsterType>& library);
};
