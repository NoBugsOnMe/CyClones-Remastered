// Saved games: what a level has come to since it was loaded. A save holds the level's name and
// difficulty (main.cpp keeps those with the slot's picture and description); loading rebuilds the
// level with World::load() exactly as it was first built, then puts this state over it: the
// player's body and kit, every creature (by its place in the level's list), the pickups (taken,
// and those creatures dropped), the props, the doors, the level's scripts and what they moved, the
// inventory and the placed demo packs, the animated surfaces, the automap, the clocks and the
// random numbers. Shots in flight, explosions and sparks are not kept.
#include <type_traits>

#include "world_internal.h"

namespace {

const uint32_t kStateMagic = 0x53594343;   // "CCYS"
const uint32_t kStateVersion = 5;   // 2: level scripts; 3: blocks, walls and floors scripts move; 4: creatures scripts bring
                                    // in; 5: the inventory, demo packs, animated surfaces, props scripts change (1-4 still load)

struct Writer {
    std::vector<uint8_t>& out;
    template <typename T> void put(const T& v) {
        static_assert(std::is_trivially_copyable<T>::value, "plain values only");
        const uint8_t* p = (const uint8_t*)&v;
        out.insert(out.end(), p, p + sizeof(T));
    }
    void str(const std::string& s) {
        put((uint32_t)s.size());
        out.insert(out.end(), s.begin(), s.end());
    }
    template <typename T> void vec(const std::vector<T>& v) {
        put((uint32_t)v.size());
        for (const T& x : v) put(x);
    }
};

struct Reader {
    const std::vector<uint8_t>& in;
    size_t pos = 0;
    bool ok = true;
    template <typename T> T get() {
        T v{};
        if (pos + sizeof(T) > in.size()) { ok = false; return v; }
        std::memcpy(&v, in.data() + pos, sizeof(T));
        pos += sizeof(T);
        return v;
    }
    template <typename T> void get(T& v) { v = get<T>(); }
    std::string str() {
        uint32_t n = get<uint32_t>();
        if (!ok || pos + n > in.size()) { ok = false; return ""; }
        std::string s((const char*)in.data() + pos, n);
        pos += n;
        return s;
    }
    template <typename T> void vec(std::vector<T>& v, size_t limit) {
        uint32_t n = get<uint32_t>();
        if (!ok || n > limit) { ok = false; return; }
        v.resize(n);
        for (auto& x : v) get(x);
    }
};

}  // namespace

void World::saveState(std::vector<uint8_t>& out) const {
    out.clear();
    Writer w{out};
    w.put(kStateMagic);
    w.put(kStateVersion);
    // Clocks and chance
    w.put(time_);
    w.put(ticks_);
    w.put(tickAcc_);
    w.put(kills_);
    w.put(cooldown_);
    w.put(meleeTimer_);
    w.put(noiseX_); w.put(noiseY_); w.put(noiseZ_); w.put(noiseTime_); w.put(noiseRange_);
    w.put(playerCellX_);
    w.put(playerCellZ_);
    {   // the dice carry on from a seed drawn from a copy (the generator itself stays as it is)
        std::mt19937 dice = rng_;
        w.put((uint32_t)dice());
    }
    // The player
    const PlayerState& p = player_;
    w.put(p.x); w.put(p.y); w.put(p.z);
    w.put(p.vx); w.put(p.vy); w.put(p.vz);
    w.put(p.viewY);
    w.put(p.health); w.put(p.armor); w.put(p.armorMax);
    w.put(p.alive); w.put(p.onGround);
    for (bool b : p.owned) w.put(b);
    for (int a : p.ammo) w.put(a);
    w.put(p.weapon);
    w.put(p.bioKits); w.put(p.mechKits);
    w.put(p.suit);
    w.put(p.keys);
    // The ground
    w.vec(explored_);
    w.vec(occupied_);
    w.vec(fixed_);
    // Creatures: first which of them scripts brought in (copies, after the level's own), then each one
    {
        std::vector<int32_t> copies;
        for (const auto& mp : monsters_)
            if (mp->copyOf >= 0) copies.push_back(mp->copyOf);
        w.vec(copies);
    }
    w.put((uint32_t)monsters_.size());
    for (const auto& mp : monsters_) {
        const Monster& m = *mp;
        w.put(m.x); w.put(m.y); w.put(m.z); w.put(m.yaw);
        w.put(m.tx); w.put(m.tz); w.put(m.ty); w.put(m.ground);
        w.put(m.moving);
        w.put(m.cx); w.put(m.cz); w.put(m.ncx); w.put(m.ncz);
        w.put(m.wait); w.put(m.step); w.put(m.look); w.put(m.sightTimer);
        w.put((int32_t)m.state);
        w.put(m.stateTime); w.put(m.hp);
        w.put(m.stagger); w.put(m.painReady); w.put(m.gone);
        w.put(m.aiClass); w.put(m.startMode); w.put(m.stationary);
        for (float a : m.armour) w.put(a);
        w.put(m.attackSlot); w.put(m.attackFrame); w.put(m.lastFire); w.put(m.unfolded);
        w.put(m.readySlot); w.put(m.resumeAttack);
        w.put(m.active); w.put(m.shotActive); w.put(m.thinkTimer);
        w.put(m.headX); w.put(m.headZ); w.put(m.walkOnly);
        w.put(m.alerted);
        w.put(m.lastSeenX); w.put(m.lastSeenZ); w.put(m.lastSeenY); w.put(m.lastSeenTime);
        w.put(m.seesPlayer);
        w.put(m.perceiveTimer); w.put(m.reaction); w.put(m.cooldown); w.put(m.searchUntil);
        w.put(m.repath); w.put(m.strafe);
        w.vec(m.path);
        w.put(m.pathGoal);
        w.put(m.slot); w.put(m.hasDrop);
    }
    w.vec(slotMonster_);
    // Pickups: those the level placed, then those creatures dropped
    w.put((uint32_t)items_.size());
    for (const Item& it : items_) {
        w.str(it.sprite);
        w.put(it.x); w.put(it.y); w.put(it.z); w.put(it.light);
        w.put(it.use); w.put(it.amount);
        for (uint8_t b : it.params) w.put(b);
        w.put(it.taken);
        w.put(it.script);
        w.put(it.kind); w.str(it.name); w.str(it.icon); w.str(it.setSprite);   // 5
        w.put(it.carried); w.put(it.active);
    }
    // Props
    w.put((uint32_t)objects_.size());
    for (const Object& o : objects_) {
        w.put(o.y);
        w.put(o.intact);
        w.put(o.wreckTime);
        w.put(o.step);
        w.put(o.removed); w.put(o.picture);   // 5
    }
    // Doors
    w.put((uint32_t)doors_.size());
    for (const Door& d : doors_) {
        w.put(d.open);
        w.put(d.opening);
        w.put(d.openFor);
        w.put(d.locked);
        w.put(d.blown);   // 5
    }
    // The inventory, the placed demo packs, the jump jets' fuel
    for (int k : inv_) w.put(k);
    w.put(readied_); w.put(invCursor_); w.put(invOpen_);
    w.vec(demoPlaced_);
    w.put(p.jetFuel);
    w.put(hazardOn_);
    // Animated surfaces: each entry on or off and its count, and the picture every number shows
    w.put((uint32_t)surfAnims_.size());
    for (const SurfAnim& a : surfAnims_) { w.put(a.on); w.put(a.count); }
    for (int k = 0; k < 4; ++k) w.vec(animFrame_[k]);
    // Level scripts: each one's state, the running ones in order, the counters, the walls moved
    w.put((uint32_t)runs_.size());
    for (const ScriptRun& r : runs_) {
        w.put(r.flags); w.put(r.events); w.put(r.timer); w.put(r.pc);
        for (int32_t v : r.p) w.put(v);   // 5
    }
    {   // 5: the prop a shot last told each script about (its first condition's second argument)
        std::vector<uint8_t> shot;
        for (const Script& sc : scripts_) shot.push_back(sc.conds.empty() ? 0 : sc.conds.front().arg[1]);
        w.vec(shot);
    }
    w.vec(scriptQueue_);
    for (uint8_t v : scriptVars_) w.put(v);
    w.put(standLayer_);
    w.put((uint32_t)wallEdits_.size());
    for (const WallEdit& e : wallEdits_) {
        w.put(e.x); w.put(e.z); w.put(e.side); w.put(e.front); w.put(e.back); w.put(e.flags); w.put(e.base); w.put(e.remove);
    }
    // Where the moving parts are (every block's base, each moving wall's base and floor's height,
    // cell by cell), and the movers still going
    std::vector<int16_t> heights;
    movingHeights(heights, false);
    w.vec(heights);
    w.vec(cubeMoving_);
    w.vec(movers_);
}

// The bases and heights of the moving parts in a fixed order: the blocks', then cell by cell the
// moving walls' (west, north) and surfaces'. put: write `h` into the level instead of reading it.
bool World::movingHeights(std::vector<int16_t>& h, bool put) const {
    if (!levelEdit_) return false;
    Level& L = *levelEdit_;
    size_t k = 0;
    auto at = [&](uint8_t* lo, uint8_t* hi) {
        if (!put) { h.push_back((int16_t)(*lo | *hi << 8)); return; }
        if (k < h.size()) { *lo = (uint8_t)h[k]; *hi = (uint8_t)((uint16_t)h[k] >> 8); }
        ++k;
    };
    auto at16 = [&](uint16_t& v) {
        if (!put) { h.push_back((int16_t)v); return; }
        if (k < h.size()) v = (uint16_t)h[k];
        ++k;
    };
    for (auto& c : L.cubes) at(&c[6], &c[7]);
    for (int i = 0; i < kGrid * kGrid; ++i) {
        uint8_t wb = (size_t)i < movingWalls_.size() ? movingWalls_[i] : 0;
        uint8_t sb = (size_t)i < movingSurfaces_.size() ? movingSurfaces_[i] : 0;
        if (wb & 1) at16(L.west.base[i]);
        if (wb & 2) at16(L.north.base[i]);
        if (sb & 1) at16(L.floor.height[i]);
        if (sb & 2) at16(L.ceiling.height[i]);
        if (sb & 4) at16(L.extra1.height[i]);
        if (sb & 8) at16(L.extra2.height[i]);
    }
    return !put || k == h.size();
}

bool World::loadState(const std::vector<uint8_t>& in, std::string& err) {
    Reader r{in};
    uint32_t magic = r.get<uint32_t>(), version = r.get<uint32_t>();
    if (magic != kStateMagic || version < 1 || version > kStateVersion) {
        err = "not a saved game this version can read";
        return false;
    }
    r.get(time_);
    // The computers that show what their scripts have done look again (scan.cpp): what is done
    // already was done long since.
    for (auto& m : consoles_) m.seen = false;
    for (auto& m : pods_) { m.seen = false; m.checked = false; }
    launch_.seen = false;
    r.get(ticks_);
    r.get(tickAcc_);
    r.get(kills_);
    r.get(cooldown_);
    r.get(meleeTimer_);
    r.get(noiseX_); r.get(noiseY_); r.get(noiseZ_); r.get(noiseTime_); r.get(noiseRange_);
    r.get(playerCellX_);
    r.get(playerCellZ_);
    rng_.seed(r.get<uint32_t>());
    PlayerState& p = player_;
    r.get(p.x); r.get(p.y); r.get(p.z);
    r.get(p.vx); r.get(p.vy); r.get(p.vz);
    r.get(p.viewY);
    r.get(p.health); r.get(p.armor); r.get(p.armorMax);
    r.get(p.alive); r.get(p.onGround);
    for (bool& b : p.owned) r.get(b);
    for (int& a : p.ammo) r.get(a);
    r.get(p.weapon);
    r.get(p.bioKits); r.get(p.mechKits);
    r.get(p.suit);
    r.get(p.keys);
    p.deadTime = 0;
    p.hurt = p.tear = p.bonus = p.hurtShow = 0;
    p.firing = false;
    p.swingAt = -100;
    const size_t cells = (size_t)kGrid * kGrid;
    r.vec(explored_, cells);
    r.vec(occupied_, cells);
    r.vec(fixed_, cells);
    if (r.ok && (explored_.size() != cells || occupied_.size() != cells || fixed_.size() != cells)) r.ok = false;
    if (version >= 4) {   // the creatures scripts brought in, copied again from those they came from
        std::vector<int32_t> copies;
        r.vec(copies, 80);
        for (int32_t from : copies) {
            if (!r.ok || from < 0 || from >= (int)monsters_.size()) { r.ok = false; break; }
            auto m = std::make_unique<Monster>(*monsters_[from]);
            m->index = (int)monsters_.size();
            m->copyOf = from;
            m->path.clear();
            monsters_.push_back(std::move(m));
        }
    }
    uint32_t nm = r.get<uint32_t>();
    if (!r.ok || nm != monsters_.size()) {
        err = "the level's creatures don't match this save";
        return false;
    }
    for (auto& mp : monsters_) {
        Monster& m = *mp;
        r.get(m.x); r.get(m.y); r.get(m.z); r.get(m.yaw);
        r.get(m.tx); r.get(m.tz); r.get(m.ty); r.get(m.ground);
        r.get(m.moving);
        r.get(m.cx); r.get(m.cz); r.get(m.ncx); r.get(m.ncz);
        r.get(m.wait); r.get(m.step); r.get(m.look); r.get(m.sightTimer);
        int32_t st = r.get<int32_t>();
        m.state = (Monster::State)std::clamp(st, 0, (int)Monster::Stun);
        r.get(m.stateTime); r.get(m.hp);
        r.get(m.stagger); r.get(m.painReady); r.get(m.gone);
        r.get(m.aiClass); r.get(m.startMode); r.get(m.stationary);
        for (float& a : m.armour) r.get(a);
        r.get(m.attackSlot); r.get(m.attackFrame); r.get(m.lastFire); r.get(m.unfolded);
        r.get(m.readySlot); r.get(m.resumeAttack);
        r.get(m.active); r.get(m.shotActive); r.get(m.thinkTimer);
        r.get(m.headX); r.get(m.headZ); r.get(m.walkOnly);
        r.get(m.alerted);
        r.get(m.lastSeenX); r.get(m.lastSeenZ); r.get(m.lastSeenY); r.get(m.lastSeenTime);
        r.get(m.seesPlayer);
        r.get(m.perceiveTimer); r.get(m.reaction); r.get(m.cooldown); r.get(m.searchUntil);
        r.get(m.repath); r.get(m.strafe);
        r.vec(m.path, cells);
        r.get(m.pathGoal);
        if (version >= 4) { r.get(m.slot); r.get(m.hasDrop); }
        m.flash = 0;
        m.view = -1;
        m.attackSlot = std::clamp(m.attackSlot, 0, 1);
        m.cx = std::clamp(m.cx, 0, kGrid - 1); m.cz = std::clamp(m.cz, 0, kGrid - 1);
        m.ncx = std::clamp(m.ncx, 0, kGrid - 1); m.ncz = std::clamp(m.ncz, 0, kGrid - 1);
    }
    if (version >= 4) {
        r.vec(slotMonster_, 80);
        for (int& k : slotMonster_)
            if (k < -1 || k >= (int)monsters_.size()) r.ok = false;
    }
    uint32_t ni = r.get<uint32_t>();
    if (!r.ok || ni < items_.size() || ni > items_.size() + 4096) {
        err = "the level's pickups don't match this save";
        return false;
    }
    size_t placed = items_.size();
    for (uint32_t i = 0; i < ni && r.ok; ++i) {
        Item it = i < placed ? items_[i] : Item();
        it.sprite = r.str();
        r.get(it.x); r.get(it.y); r.get(it.z); r.get(it.light);
        r.get(it.use); r.get(it.amount);
        for (uint8_t& b : it.params) r.get(b);
        r.get(it.taken);
        if (version >= 2) r.get(it.script);
        if (version >= 5) {
            r.get(it.kind);
            it.name = r.str(); it.icon = r.str(); it.setSprite = r.str();
            r.get(it.carried); r.get(it.active);
        }
        it.denied = 0;
        if (i >= placed) {   // dropped by a creature (its picture was made when the level loaded), or a kit
            const Visual* v = itemVisual(it.sprite, it);   // stack brought from another mission
            if (!v || v->flat.empty()) {
                if (version < 5) continue;
                it.vis = nullptr;
                it.taken = true;   // (kept, so the inventory's slots still point at the right things)
            } else {
                it.vis = v;
            }
            items_.push_back(it);
        } else {
            items_[i] = it;
        }
    }
    uint32_t no = r.get<uint32_t>();
    if (!r.ok || no != objects_.size()) {
        err = "the level's props don't match this save";
        return false;
    }
    for (Object& o : objects_) {
        r.get(o.y);
        r.get(o.intact);
        r.get(o.wreckTime);
        r.get(o.step);
        if (version >= 5) {
            r.get(o.removed);
            int pic = r.get<int>();
            float step = o.step;
            if (r.ok && pic >= 0 && pic < 4096) setPropPicture(o, pic);
            o.step = step;
        }
    }
    uint32_t nd = r.get<uint32_t>();
    if (!r.ok || nd != doors_.size()) {
        err = "the level's doors don't match this save";
        return false;
    }
    for (Door& d : doors_) {
        r.get(d.open);
        r.get(d.opening);
        r.get(d.openFor);
        r.get(d.locked);
        if (version >= 5) {
            r.get(d.blown);
            if (d.blown) d.staysOpen = true;
        }
    }
    if (version >= 5) {
        for (int& k : inv_) {
            r.get(k);
            if (k < -1 || k >= (int)items_.size()) r.ok = false;
        }
        r.get(readied_); r.get(invCursor_); r.get(invOpen_);
        if (readied_ < -1 || readied_ > 7 || (readied_ >= 0 && inv_[readied_] < 0)) readied_ = -1;
        invCursor_ = std::clamp(invCursor_, 0, 7);
        r.vec(demoPlaced_, 8);
        for (int k : demoPlaced_) {
            if (k < 0 || k >= (int)items_.size()) { r.ok = false; break; }
            Item& it = items_[k];
            it.placed = true;
            if (!it.setSprite.empty())
                if (const Visual* v = itemVisual(it.setSprite, it); v && !v->flat.empty()) it.vis = v;
        }
        r.get(p.jetFuel);
        p.jets = false;   // the jets start off, as at a level's start (the game's HUD state isn't saved)
        p.jetCool = 0;
        r.get(hazardOn_);
        uint32_t na = r.get<uint32_t>();
        if (!r.ok || na != surfAnims_.size()) {
            err = "the level's animations don't match this save";
            return false;
        }
        for (SurfAnim& a : surfAnims_) { r.get(a.on); r.get(a.count); }
        for (int k = 0; k < 4; ++k) {
            std::vector<uint16_t> f;
            r.vec(f, 4096);
            if (r.ok && f.size() == animFrame_[k].size()) animFrame_[k] = f;
        }
        renderer_->setAnimFrames(animFrame_[0], animFrame_[1]);
    } else {
        inv_.fill(-1);   // (older saves kept no inventory: the kits come back as stacks)
        readied_ = -1;
        invOpen_ = false;
        demoPlaced_.clear();
        for (Item& it : items_) it.carried = false;
        ensureKitSlots();
    }
    if (version >= 2) {
        uint32_t ns = r.get<uint32_t>();
        if (!r.ok || ns != runs_.size()) {
            err = "the level's scripts don't match this save";
            return false;
        }
        for (ScriptRun& s : runs_) {
            r.get(s.flags); r.get(s.events); r.get(s.timer); r.get(s.pc);
            if (version >= 5) for (int32_t& v : s.p) r.get(v);
            if (s.pc >= 0 && (size_t)(&s - runs_.data()) < scripts_.size() &&
                s.pc >= (int)scripts_[&s - runs_.data()].acts.size()) s.pc = -1;
        }
        if (version >= 5) {
            std::vector<uint8_t> shot;
            r.vec(shot, scripts_.size());
            if (r.ok && shot.size() == scripts_.size())
                for (size_t k = 0; k < shot.size(); ++k)
                    if (!scripts_[k].conds.empty()) scripts_[k].conds.front().arg[1] = shot[k];
        }
        r.vec(scriptQueue_, runs_.size());
        for (int& q : scriptQueue_) if (q < 0 || q >= (int)runs_.size()) r.ok = false;
        for (uint8_t& v : scriptVars_) r.get(v);
        if (version >= 4) r.get(standLayer_);
        uint32_t ne = r.get<uint32_t>();
        if (!r.ok || ne > 100000) {
            err = "the saved game is cut short";
            return false;
        }
        wallEdits_.clear();
        for (uint32_t k = 0; k < ne && r.ok; ++k) {
            WallEdit e;
            r.get(e.x); r.get(e.z); r.get(e.side); r.get(e.front); r.get(e.back); r.get(e.flags); r.get(e.base); r.get(e.remove);
            if (r.ok) editWall(e, true);
        }
        if (version >= 3) {
            std::vector<int16_t> heights;
            r.vec(heights, 100000);
            r.vec(cubeMoving_, 100000);
            r.vec(movers_, 50);
            if (!r.ok || !movingHeights(heights, true) || cubeMoving_.size() != levelEdit_->cubes.size()) {
                err = "the level's moving parts don't match this save";
                return false;
            }
            for (const Mover& m : movers_)
                if (m.kind < 1 || m.kind > 6 || m.kind == 4 || m.speed < 1 || m.x < 0 || m.z < 0 || m.x >= kGrid || m.z >= kGrid ||
                    m.cube >= (int)cubeMoving_.size()) {
                    err = "the level's moving parts don't match this save";
                    return false;
                }
            geometryDirty_ = true;
        }
        applyLevelEdits();
        if (version >= 3) buildNav();
    }
    if (!r.ok) {
        err = "the saved game is cut short";
        return false;
    }
    shots_.clear();
    effects_.clear();
    particles_.clear();
    lights_.clear();
    pending_.clear();
    sounds_.clear();
    messages_.clear();
    shake = 0;
    lastPX_ = p.x;
    lastPZ_ = p.z;
    playerVelX_ = playerVelZ_ = 0;
    return true;
}
