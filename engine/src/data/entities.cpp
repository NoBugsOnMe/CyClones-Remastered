#include "entities.h"

#include <cctype>
#include <cstring>
#include <map>
#include <stdexcept>

namespace {

struct Short : std::runtime_error {
    Short() : std::runtime_error("record runs past the end of its chunk") {}
};

struct R {
    const uint8_t* p;
    size_t n, o = 0;
    void need(size_t k) const { if (o + k > n) throw Short(); }
    int u8() { need(1); return p[o++]; }
    int u16() { need(2); int v = p[o] | p[o + 1] << 8; o += 2; return v; }
    int s16() { return (int16_t)u16(); }
    uint32_t u32() { need(4); uint32_t v = p[o] | p[o + 1] << 8 | p[o + 2] << 16 | (uint32_t)p[o + 3] << 24; o += 4; return v; }
    void skip(size_t k) { need(k); o += k; }
    std::string name(size_t k) {
        need(k);
        size_t len = 0;
        while (len < k && p[o + len]) ++len;
        std::string s((const char*)p + o, len);
        o += k;
        return s;
    }
    std::vector<uint8_t> bytes(size_t k) { need(k); std::vector<uint8_t> v(p + o, p + o + k); o += k; return v; }
    bool done() const { return o >= n; }
};

// Where the script actions read go (the LevelEntities being parsed), or nowhere; and where whole
// scripts go (a map's own: its entities' and its trigger cells'), or nowhere (a .GLB's).
std::vector<ScriptAction>* gActions = nullptr;
std::vector<Script>* gScripts = nullptr;

// A script (FUN_00039f07): u16 flags, u32 timer, u32 creature, u8 kind, u32 next (trigger cells
// chain several), u32, u8[16] parameters, then its conditions (u16 op, u8[20] arguments, u32 next)
// and its actions (the same with a u32 back link as well). Returns the link to the next script;
// *index is where it went in gScripts (-1: not kept).
uint32_t readScript(R& r, int* index) {
    Script s;
    s.flags = r.u16();
    r.skip(4 + 4 + 1);
    uint32_t next = r.u32();
    r.skip(4);
    for (auto& b : s.params) b = (uint8_t)r.u8();
    auto action = [&](std::vector<ScriptAction>& list) {
        ScriptAction a;
        a.op = r.u16();
        for (auto& b : a.arg) b = (uint8_t)r.u8();
        list.push_back(a);
        if (gActions) gActions->push_back(a);
    };
    if (r.u32()) {
        for (;;) { action(s.conds); if (!r.u32()) break; }
    }
    uint32_t head2 = r.u32();
    r.skip(4);
    if (head2) {
        for (;;) { action(s.acts); uint32_t more = r.u32(); r.skip(4); if (!more) break; }
    }
    if (index) *index = -1;
    if (gScripts) {
        if (index) *index = (int)gScripts->size();
        gScripts->push_back(std::move(s));
    }
    return next;
}

void header(R& r, int chunkType, EntityBase& e) {
    e.id = r.u16();
    e.x = r.u16();
    e.z = r.u16();
    e.y = r.s16();
    e.skill = r.u8();
    e.tag = r.u16();
    r.skip(2 + 4 + 4);
    if (chunkType) {
        if (r.u32()) readScript(r, &e.script);
    }
}

ItemSpawn readItem(R& r, int ct) {
    ItemSpawn it;
    header(r, ct, it);
    it.kind = r.u16();
    r.skip(2);
    it.sprite = r.name(10);
    r.skip(2);
    it.setPic = r.u16();   // +0x48: a placed demo pack's picture, from the sprite
    it.invPic = r.u16();   // +0x4a: the inventory picture, from the sprite (i_...)
    it.use = r.u16();
    for (auto& b : it.params) b = (uint8_t)r.u8();
    it.amount = r.u16();
    r.skip(19);
    return it;
}

ObjectSpawn readObject(R& r, int ct) {
    ObjectSpawn o;
    header(r, ct, o);
    o.kind = r.u16();
    o.sprite = r.name(10);
    r.skip(2 + 2 + 7 + 2 + 19);
    return o;
}

DoorSpawn readDoor(R& r, int ct) {
    DoorSpawn d;
    header(r, ct, d);
    d.kind = r.u16();
    d.texture = r.name(10);
    r.skip(2);
    d.sound = r.u16() & 0xff;   // the game keeps the low byte (door +0x20)
    d.flags = r.u8();
    d.flags2 = r.u16();
    int b[6];
    for (int& v : b) v = r.u8();
    d.key = b[0];
    d.speed = (int8_t)b[1];
    d.offset = b[2];
    int v0 = r.u16(), v1 = r.u16();
    d.vStart = v0;
    d.height = v1 > v0 ? v1 - v0 : 0;
    r.skip(10);
    return d;
}

MonsterSpawn readMonster(R& r, int ct) {
    MonsterSpawn m;
    header(r, ct, m);
    m.templ = r.u16();
    m.type = r.u16();
    auto m20 = r.bytes(20);
    m.startMode = m20[0];
    auto snd = [&](int o) {   // the game plays ids 1-255 only
        int v = m20[o] | m20[o + 1] << 8;
        return v <= 255 ? v : 0;
    };
    m.sndPain = snd(4);
    m.sndMelee = snd(8);
    m.sndFire = snd(12);
    m.sndDeath = snd(16);
    auto b19 = r.bytes(19);
    m.sight = b19[1];
    m.trigger = b19[7];
    m.group = b19[10];
    m.hp = b19[14];
    uint32_t links[8];
    for (auto& l : links) l = r.u32();
    r.skip(8);
    auto b6 = r.bytes(6);
    for (int k = 0; k < 4; ++k) m.armour[k] = b6[k];
    m.bandA = b6[4];
    m.bandB = b6[5];
    r.skip(97);
    for (int k = 0; k < 8; ++k) {
        if (!links[k]) continue;
        ItemSpawn it = readItem(r, ct);   // carried items; slot 2 is dropped on death
        if (k == 2) {
            m.hasDrop = true;
            m.dropKind = it.kind;
            m.dropInvPic = it.invPic;
            m.dropUse = it.use;
            m.dropAmount = it.amount;
            m.dropSprite = it.sprite;
            m.dropScript = it.script;
            std::memcpy(m.dropParams, it.params, sizeof(m.dropParams));
        }
    }
    return m;
}

AnimSeq seq(R& r) {
    AnimSeq s;
    int n = r.u8();
    auto v = r.bytes(n);
    for (uint8_t b : v) {
        if (b == 255) break;
        s.frames.push_back(b);
    }
    return s;
}

std::vector<uint8_t> seqBytes(R& r) { return seq(r).frames; }

MonsterType readType(R& r, int ct) {
    MonsterType t;
    t.id = r.u16();
    t.sprite = r.name(10);
    t.health = r.u16();
    for (auto& s : t.stats) s = (uint8_t)r.u8();
    uint32_t l6[6], attP[3], attQ[3];
    for (auto& v : l6) v = r.u32();
    for (auto& v : attP) v = r.u32();
    for (auto& v : attQ) v = r.u32();
    t.accuracy = (int16_t)r.u16();
    t.evasion = (int16_t)r.u16();
    r.skip(1);
    uint32_t l78 = r.u32(), l74 = r.u32();
    uint32_t items[3];
    for (auto& v : items) v = r.u32();
    r.skip(3);
    t.mirror = r.u16();
    t.eye = r.u8();
    r.skip(27 + 4 + 4);
    for (int k = 0; k < 3; ++k) {
        if (!items[k]) continue;
        ItemSpawn it = readItem(r, ct);
        t.hasWeapon[k] = true;
        t.weaponUse[k] = it.use;
        std::memcpy(t.weaponParams[k], it.params, sizeof(it.params));
    }
    t.ready = seq(r);
    t.ready.tics = seqBytes(r);
    t.death = seq(r);
    if (l6[3]) t.death.tics = seqBytes(r);
    t.walk = seq(r);
    if (l6[5]) t.walk.tics = seqBytes(r);
    AnimSeq att[3];
    for (int i = 0; i < 3; ++i)
        if (attP[i]) att[i] = seq(r);
    std::vector<uint8_t> attTics[3];
    for (int i = 0; i < 3; ++i)
        if (attQ[i]) attTics[i] = seqBytes(r);
    t.attackA = att[0];
    t.attackA.tics = attTics[0];
    t.attackB = att[1];
    t.attackB.tics = attTics[1];
    t.fireA = att[2].frames;
    t.fireB = attTics[2];
    if (l78) t.pain = seq(r);
    if (l74) t.pain.tics = seqBytes(r);
    return t;
}

std::map<int, std::string> texts(R& r) {
    std::map<int, std::string> out;
    while (!r.done()) {
        int i = r.u16(), n = r.u16();
        out[i] = r.name(n);
    }
    return out;
}

template <typename F>
bool chunks(const std::vector<uint8_t>& file, std::string& error, F&& f) {
    size_t o = 0;
    while (o + 10 <= file.size()) {
        std::string tag((const char*)&file[o], 4);
        int type = file[o + 4] | file[o + 5] << 8;
        uint32_t size = file[o + 6] | file[o + 7] << 8 | file[o + 8] << 16 | (uint32_t)file[o + 9] << 24;
        if (o + 10 + size > file.size()) { error = "truncated chunk " + tag; return false; }
        R r{&file[o + 10], size};
        try {
            f(tag, type, r);
        } catch (const Short& e) {
            error = tag + ": " + e.what();
            return false;
        }
        o += 10 + size;
    }
    return true;
}

}  // namespace

bool LevelEntities::parseMap(const std::vector<uint8_t>& file, std::string& error) {
    gActions = &actions;
    gScripts = &scripts;
    bool ok = chunks(file, error, [&](const std::string& tag, int ct, R& r) {
        if (tag == "MON ") while (!r.done()) monsters.push_back(readMonster(r, ct));
        else if (tag == "OBJ ") while (!r.done()) objects.push_back(readObject(r, ct));
        else if (tag == "ITEM") while (!r.done()) items.push_back(readItem(r, ct));
        else if (tag == "DOOR") while (!r.done()) doors.push_back(readDoor(r, ct));
        else if (tag == "TRIG") {
            // Trigger cells (FUN_0003a1fa): a 64x64 grid of links (z * 64 + x), then each linked
            // cell's chain of scripts, the cells taken column by column (x outer, z inner).
            uint32_t grid[64 * 64];
            for (auto& g : grid) g = r.u32();
            cellScripts.assign(64 * 64, {});
            for (int x = 0; x < 64; ++x)
                for (int z = 0; z < 64; ++z) {
                    if (!grid[z * 64 + x]) continue;
                    int index;
                    uint32_t more;
                    do {
                        more = readScript(r, &index);
                        if (index >= 0) cellScripts[z * 64 + x].push_back(index);
                    } while (more);
                }
        }
    });
    gActions = nullptr;
    gScripts = nullptr;
    return ok;
}

bool LevelEntities::parseGlb(const std::vector<uint8_t>& file, std::string& error) {
    std::map<int, std::string> names;
    gActions = &actions;
    bool ok = chunks(file, error, [&](const std::string& tag, int ct, R& r) {
        if (tag == "BMON") while (!r.done()) types.push_back(readType(r, ct));
        else if (tag == "MBTX") names = texts(r);
        else if (tag == "GITX") itemNames = texts(r);   // item names: kind k is entry k - 1
    });
    gActions = nullptr;
    for (auto& t : types) {
        auto it = names.find(t.id - 1);
        if (it != names.end()) t.name = it->second;
    }
    return ok;
}

int LevelEntities::fillPlaceholders(const std::vector<MonsterType>& library) {
    auto same = [](const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
        return true;
    };
    int fixed = 0;
    for (auto& t : types) {
        if (!t.walk.placeholder() && !t.death.placeholder()) continue;
        for (const auto& o : library) {
            if (!same(o.sprite, t.sprite) || o.walk.placeholder() || o.death.placeholder() || o.death.frames.empty()) continue;
            t.walk = o.walk;
            t.death = o.death;
            t.pain = o.pain;
            t.ready = o.ready;
            t.attackA = o.attackA;
            t.attackB = o.attackB;
            t.fireA = o.fireA;
            t.fireB = o.fireB;
            for (int k = 0; k < 3; ++k) {
                t.hasWeapon[k] = o.hasWeapon[k];
                t.weaponUse[k] = o.weaponUse[k];
                std::memcpy(t.weaponParams[k], o.weaponParams[k], sizeof(t.weaponParams[k]));
            }
            ++fixed;
            break;
        }
    }
    return fixed;
}
