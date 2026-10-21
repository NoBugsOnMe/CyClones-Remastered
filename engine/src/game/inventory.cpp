// The inventory, as the game keeps it (FUN_000420c0 puts items in, the slot handler at 0x41948
// and FUN_0004032d use them, FUN_000412f1 takes them out).
//
// Eight slots (0xef419). Bio-kits and mech-kits stack in one slot each (the count goes up, the
// new kit is gone); keys don't take a slot. Selecting a slot (with the inventory open) does what
// the item is for: a kit is used, an activated item (the jump jets, a suit) switches on or off and
// its script is told, a quest item is readied, a demo pack is set down at the player's feet.
// A readied item is used on whatever the crosshair is on the next time the player presses "use":
// its script learns what it was used on (a wall or floor and its cell, a door and its cell and
// key, a creature's id and group, an item) and is tried; an item whose script waits for nothing
// but being readied (condition 26) is used at once. Demo packs blow up together when the player
// detonates them (FUN_00042a96): each one's script runs with 0x74f02 set (condition 27).
#include "world_internal.h"

namespace {

std::string upperText(std::string s) {
    for (char& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

}  // namespace

// An item's name (the level's GITX texts) and its pictures relative to its sprite.
void World::nameItem(Item& it, int kind, int invPic, int setPic) {
    it.kind = kind;
    auto n = itemNames_.find(kind - 1);
    it.name = n == itemNames_.end() ? std::string() : n->second;
    while (!it.name.empty() && it.name.back() == ' ') it.name.pop_back();
    int i = data_->hd.find(it.sprite);
    auto rel = [&](int off) -> std::string {
        if (i < 0 || off <= 0 || i + off >= (int)data_->hd.lumps().size()) return "";
        return lower(data_->hd.lumps()[i + off].name);
    };
    it.icon = rel(invPic);
    it.setSprite = rel(setPic);
    if (it.icon.rfind("i_", 0) != 0) it.icon.clear();   // suits and jump jets have no picture of their own
}

int World::packSlot(int item) const {
    for (int k = 0; k < 8; ++k)
        if (inv_[k] == item) return k;
    return -1;
}

int World::packSlotOfUse(int use) const {
    for (int k = 0; k < 8; ++k)
        if (inv_[k] >= 0 && items_[inv_[k]].use == use) return k;
    return -1;
}

std::array<World::InventorySlot, 8> World::inventory() const {
    std::array<InventorySlot, 8> out;
    for (int k = 0; k < 8; ++k) {
        if (inv_[k] < 0 || inv_[k] >= (int)items_.size()) continue;
        const Item& it = items_[inv_[k]];
        InventorySlot& s = out[k];
        s.used = true;
        s.icon = it.icon;
        s.name = it.name;
        s.use = it.use;
        s.count = it.use == 9 ? player_.bioKits : it.use == 10 ? player_.mechKits : 0;
        s.active = it.active;
        s.readied = k == readied_;
    }
    return out;
}

// FUN_000420c0 for what goes in the inventory (uses 4-7, 9, 10). False: it stays where it is.
bool World::packItem(Item& it) {
    PlayerState& p = player_;
    int idx = (int)(&it - items_.data());
    std::string name = upperText(it.name.empty() ? "ITEM" : it.name);
    if (it.use == 9 || it.use == 10) {
        int& count = it.use == 9 ? p.bioKits : p.mechKits;
        const char* kit = it.use == 9 ? "BIO-KIT" : "MECH-KIT";
        if (packSlotOfUse(it.use) >= 0) {   // onto the stack: this kit is gone
            ++count;
            it.taken = true;
        } else {
            int s = freePackSlot();
            if (s < 0) {
                message(std::string(kit) + " NOT ACQUIRED.", 0);
                message("  ITEM WOULD EXCEED STRUCTURAL CAPACITY.", 0);
                return false;
            }
            inv_[s] = idx;
            it.taken = it.carried = true;
            count = std::max(count, 0) + 1;
        }
        p.bonus = 1;
        message(std::string(kit) + (it.use == 9 ? " ACQUIRED. PRESS Q TO USE." : " ACQUIRED. PRESS E TO USE."), 0);
        sound(kPickupSound);
        return true;
    }
    int s = freePackSlot();
    if (s < 0) {
        message(name + " NOT ACQUIRED.", 0);
        message("  ITEM WOULD EXCEED STRUCTURAL CAPACITY.", 0);
        return false;
    }
    inv_[s] = idx;
    it.taken = it.carried = true;
    it.active = false;
    p.bonus = 1;
    // The jump jets' pack goes at once (its script puts its charge in the fuel, action 53): the
    // hint is for the jets, not the inventory.
    bool jets = false;
    if (it.script >= 0 && it.script < (int)scripts_.size())
        for (const auto& a : scripts_[it.script].acts) jets |= a.op == 53;
    if (jets) message(name + " ACQUIRED. PRESS " + jetsKey + " FOR THE JETS, HOLD JUMP IN THE AIR TO FLY.", 0);
    else message(name + " ACQUIRED. PRESS I FOR THE INVENTORY.", 0);
    sound(kPickupSound);
    return true;
}

int World::freePackSlot() const {
    for (int k = 0; k < 8; ++k)
        if (inv_[k] < 0) return k;
    return -1;
}

// FUN_000412f1: an item taken out of the game. In the inventory its slot empties (readied, it is
// no longer); lying in the level it is gone from there.
void World::removeItem(int item) {
    if (item < 0 || item >= (int)items_.size()) return;
    Item& it = items_[item];
    int s = packSlot(item);
    if (s >= 0) {
        inv_[s] = -1;
        if (readied_ == s) readied_ = -1;
        if (it.use == 9) player_.bioKits = 0;
        if (it.use == 10) player_.mechKits = 0;
    }
    for (size_t k = 0; k < demoPlaced_.size(); ++k)
        if (demoPlaced_[k] == item) { demoPlaced_.erase(demoPlaced_.begin() + (long)k); break; }
    it.carried = false;
    it.active = false;
    it.taken = true;
}

// A kit stack used up (useKit): its slot empties.
void World::kitStackEmpty(int use) {
    int s = packSlotOfUse(use);
    if (s < 0) return;
    items_[inv_[s]].carried = false;
    inv_[s] = -1;
    if (readied_ == s) readied_ = -1;
}

void World::toggleInventory() {
    invOpen_ = !invOpen_;
    readied_ = -1;   // closing the inventory unreadies (and opening starts with nothing readied)
    if (invOpen_ && (invCursor_ < 0 || inv_[invCursor_] < 0)) {
        int s = 0;
        while (s < 8 && inv_[s] < 0) ++s;
        invCursor_ = s < 8 ? s : 0;
    }
}

void World::inventoryMove(int dir) {
    for (int step = 1; step <= 8; ++step) {
        int s = ((invCursor_ + dir * step) % 8 + 8) % 8;
        if (inv_[s] >= 0) { invCursor_ = s; return; }
    }
}

// The slot handler (0x41948): what selecting an inventory slot does.
void World::inventorySelect(int slot) {
    if (slot < 0) slot = invCursor_;
    if (slot < 0 || slot >= 8 || !player_.alive) return;
    invCursor_ = slot;
    int idx = inv_[slot];
    if (idx < 0) return;
    Item& it = items_[idx];
    if (readied_ >= 0) {   // with an item readied, only that item's slot does anything: it unreadies
        if (slot == readied_) readied_ = -1;
        return;
    }
    switch (it.use) {
    case 4:
    case 5:   // activated: on or off, and its script is told
        it.active = !it.active;
        message(upperText(it.name) + (it.active ? " ACTIVATED." : " DEACTIVATED."), 0);
        if (it.script >= 0) tryScript(it.script);
        break;
    case 6:   // readied, to be used on something
        readied_ = slot;
        message("USING " + upperText(it.name) + "...", 0);
        break;
    case 7:
        placeDemoPack(slot);
        break;
    case 9:
        useKit(0);
        break;
    case 10:
        useKit(1);
        break;
    default:
        break;
    }
    // A readied item whose script waits only for being readied (its first condition is 26) is
    // used at once.
    if (readied_ >= 0 && readiedWaitsForNothing()) {
        useReadied(UseNothing, 0, 0, 0);
        readied_ = -1;
    }
}

bool World::readiedWaitsForNothing() const {
    if (readied_ < 0 || inv_[readied_] < 0) return false;
    int sc = items_[inv_[readied_]].script;
    return sc >= 0 && sc < (int)scripts_.size() && !scripts_[sc].conds.empty() && scripts_[sc].conds.front().op == 26;
}

// FUN_0004032d: the readied item's script learns what it was used on, and is tried.
bool World::useReadied(UseTarget what, int p0, int p1, int p2) {
    if (readied_ < 0 || inv_[readied_] < 0) return false;
    int sc = items_[inv_[readied_]].script;
    if (sc < 0 || sc >= (int)runs_.size()) return false;
    ScriptRun& r = runs_[sc];
    switch (what) {
    case UseWall: r.events |= 0x20; r.p[0] = p0; r.p[1] = p1; break;
    case UseFloor: r.events |= 0x10; r.p[0] = p0; r.p[1] = p1; break;
    case UseCreature: r.events |= 0x01; r.p[0] = p0; r.p[1] = p1; break;
    case UseItem: r.events |= 0x04; r.p[0] = p0; break;
    case UseDoor: r.events |= 0x08; r.p[0] = p0; r.p[1] = p1; r.p[2] = p2; break;
    default: break;
    }
    if (logScripts()) SDL_Log("dbg readied %s used (%d) %d,%d,%d: script %d flags %x", items_[inv_[readied_]].name.c_str(), (int)what, p0, p1, p2, sc, r.flags);
    if (logScripts())
        for (const auto& c : scripts_[sc].conds)
            SDL_Log("dbg   cond %d %d,%d,%d,%d p %d,%d,%d ev %x", c.op, c.arg[0], c.arg[1], c.arg[2], c.arg[3], r.p[0], r.p[1], r.p[2], r.events);
    bool before = (r.flags & 0x01) != 0;
    tryScript(sc);
    return !before && (r.flags & 0x01);
}

// Selecting a demo pack (FUN_000428dc): it is set down at the player's feet, showing its "set"
// picture, among the packs waiting to go off (at most eight).
void World::placeDemoPack(int slot) {
    int idx = inv_[slot];
    if (idx < 0) return;
    Item& it = items_[idx];
    readied_ = -1;
    message("DEMO PACK PLACED.", 0);
    if (demoPlaced_.size() >= 8) {
        message("ABORT: TOO MANY DEMO PACKS PLACED FOR DETONATION", 0);
        return;
    }
    if (!it.setSprite.empty()) {
        if (const Visual* v = itemVisual(it.setSprite, it); v && !v->flat.empty()) it.vis = v;
    }
    demoPlaced_.push_back(idx);
    dropFromPack(slot);
    it.placed = true;
}

// The DROP button (FUN_00041e85): out of the inventory onto the floor at the player's feet; a kit
// leaves its stack one at a time.
void World::dropFromPack(int slot) {
    if (slot < 0 || slot >= 8 || inv_[slot] < 0) return;
    PlayerState& p = player_;
    Item& it = items_[inv_[slot]];
    if (!it.vis || it.vis->flat.empty()) {   // a kit brought from another mission: its picture, if this level has it
        const Visual* v = itemVisual(it.sprite, it);
        if (!v || v->flat.empty()) return;
        it.vis = v;
    }
    float y = groundBelow(p.x, p.z, p.y + 8);
    if (it.use == 9 || it.use == 10) {
        int& count = it.use == 9 ? p.bioKits : p.mechKits;
        if (count > 1) {   // one kit off the stack, a copy of the stack's
            Item one = it;
            one.carried = false;
            one.taken = false;
            one.script = -1;
            one.x = p.x;
            one.z = p.z;
            one.y = y;
            one.light = cellLight(p.x, p.z);
            one.dropped = true;
            --count;
            items_.push_back(one);
            return;
        }
        count = 0;
    }
    inv_[slot] = -1;
    if (readied_ == slot) readied_ = -1;
    it.carried = false;
    it.active = false;
    it.taken = false;
    it.x = p.x;
    it.z = p.z;
    it.y = y;
    it.light = cellLight(p.x, p.z);
    it.denied = 0;
    it.dropped = true;
}

void World::inventoryDrop(int slot) {
    if (slot < 0) slot = invCursor_;
    if (slot < 0 || slot >= 8 || inv_[slot] < 0) return;
    message(upperText(items_[inv_[slot]].name) + " DROPPED.", 0);
    dropFromPack(slot);
}

// The Demo Pack Icon (FUN_00042a96): every placed pack's script runs with 0x74f02 set (condition
// 27), and each goes off with sound 46 and the s1dmoexp blast.
void World::detonate() {
    if (demoPlaced_.empty()) return;
    std::vector<int> packs = demoPlaced_;
    detonating_ = true;
    for (int idx : packs) {
        if (idx < 0 || idx >= (int)items_.size()) continue;
        Item& it = items_[idx];
        if (it.script >= 0) tryScript(it.script);
        soundAt(46, it.x, it.z);
        explode(it.x, it.y + 16, it.z, 1.0f, "s1dmoexp");
    }
    detonating_ = false;
    demoPlaced_.clear();
}

// ---------------------------------------------------------------------------------------------
// What scripts and "use" do to props and creatures

int World::scriptItem(int script) const {
    if (script < 0) return -1;
    for (size_t k = 0; k < items_.size(); ++k)
        if (items_[k].script == script) return (int)k;
    return -1;
}

World::Object* World::slotProp(int slot) {
    if (slot < 0 || slot >= (int)slotObject_.size() || slotObject_[slot] < 0) return nullptr;
    return &objects_[slotObject_[slot]];
}

// Action 55: the prop shows 'startobjects' + picture (its animation, if the picture has one).
void World::setPropPicture(Object& o, int picture) {
    const Bol& cd = data_->cd;
    int start = cd.find("startobjects");
    if (start < 0 || start + picture >= (int)cd.lumps().size()) return;
    std::string name = cd.lumps()[start + picture].name;
    AnimRange r = spriteAnim(3, name);
    const Visual* v = spriteVisual(r.number >= 0 ? "obj:anim" + std::to_string(r.first) : "obj:" + lower(name), {}, true, false, 0.09f);
    if (!v || v->flat.empty()) return;   // (made at load: World::load asks for every picture action 55 shows)
    o.vis = v;
    o.animNumber = r.number;
    o.animFirst = r.first;
    o.step = 0;
    o.picture = picture;
}

// A demo pack going off (action 46, FUN_00033977): within `radius` cells of it each door that can
// be blown (0x80 of its flags: the u16 after the file's flags lacks 0x01) is blown open for good,
// and creatures and the player in sight of it are hurt; the pack is gone.
void World::demoBlast(int radius, int item) {
    if (item < 0 || item >= (int)items_.size()) return;
    Item& pk = items_[item];
    float x0 = pk.x - radius * kCellSize, x1 = pk.x + radius * kCellSize;
    float z0 = pk.z - radius * kCellSize, z1 = pk.z + radius * kCellSize;
    for (auto& d : doors_) {
        if (d.cx < x0 || d.cx > x1 || d.cz < z0 || d.cz > z1 || (d.spec.flags2 & 0x01)) continue;
        if (d.blown) continue;
        d.blown = true;
        d.locked = true;
        d.staysOpen = true;
        d.opening = true;
        d.openFor = 0;
        spawnEffect("mechdth", d.cx, d.base + 16, d.cz, 1.0f, 1.1f, 1, true, 0, 0);
    }
    float c[3] = {pk.x, pk.y + 16, pk.z};
    for (auto& m : monsters_) {
        if (m->state == Monster::Dead || m->state == Monster::Dying) continue;
        if (m->x < x0 || m->x > x1 || m->z < z0 || m->z > z1) continue;
        float to[3] = {m->x, m->y + m->vis->height * 0.5f, m->z};
        if (lineOfSight(c, to)) damageMonster(*m, (float)(60 + (int)(rng_() % 61u)), c, 2);
    }
    PlayerState& p = player_;
    if (p.alive && p.x >= x0 && p.x <= x1 && p.z >= z0 && p.z <= z1) {
        float eye[3] = {p.x, p.y + 32, p.z};
        if (lineOfSight(c, eye)) damagePlayer((float)(60 + (int)(rng_() % 61u)), c, 2);   // "seriously or fatally injured"
    }
    removeItem(item);
}

World::Monster* World::creatureInSight(const Camera& cam, float maxT, float* dist) {
    float d[3] = {std::sin(cam.yaw) * std::cos(cam.pitch), std::sin(cam.pitch), -std::cos(cam.yaw) * std::cos(cam.pitch)};
    float o[3] = {cam.x, cam.y, cam.z};
    Monster* best = nullptr;
    for (auto& m : monsters_) {
        if (m->state == Monster::Dying || m->state == Monster::Dead) continue;
        float tt;
        if (rayHitsMonsterThin(*m, o, d, maxT, tt) && tt < maxT) { maxT = tt; best = m.get(); }
    }
    if (dist) *dist = maxT;
    return best;
}

// A prop under the crosshair: its upright picture facing the camera, nearer than maxT.
World::Object* World::propInSight(const Camera& cam, float maxT, float* dist) {
    float d[3] = {std::sin(cam.yaw) * std::cos(cam.pitch), std::sin(cam.pitch), -std::cos(cam.yaw) * std::cos(cam.pitch)};
    Object* best = nullptr;
    for (auto& ob : objects_) {
        if (ob.removed || !ob.intact || !ob.vis) continue;
        float fx = ob.x - cam.x, fz = ob.z - cam.z, fl = std::sqrt(fx * fx + fz * fz);
        if (fl < 1) continue;
        fx /= fl; fz /= fl;
        float denom = d[0] * fx + d[2] * fz;
        if (denom < 1e-3f) continue;
        float tt = fl / denom;
        if (tt >= maxT) continue;
        float px = cam.x + d[0] * tt, py = cam.y + d[1] * tt, pz = cam.z + d[2] * tt;
        float u = (px - ob.x) * -fz + (pz - ob.z) * fx, h = py - ob.y;
        if (std::abs(u) > std::max(6.0f, ob.vis->radius) || h < 0 || h > std::max(8.0f, ob.vis->height)) continue;
        maxT = tt;
        best = &ob;
    }
    if (dist) *dist = maxT;
    return best;
}

// "Use" on a creature (FUN_0003fed1, case 3, 0xb): a readied item's script learns its id and
// group; the creature's own script is told it was used (event 0x01).
void World::useCreature(Monster& m) {
    useReadied(UseCreature, m.recordId, m.group, 0);
    if (m.script >= 0 && m.script < (int)runs_.size()) {
        runs_[m.script].events |= 0x01;
        tryScript(m.script);
    }
}

// "Use" on a prop (case 3, 0xd): its script's first parameter becomes the prop's slot, and the
// script is tried.
void World::useProp(Object& o) {
    useReadied(UseProp, 0, 0, 0);
    if (o.script >= 0 && o.script < (int)runs_.size()) {
        runs_[o.script].p[0] = o.slot;
        tryScript(o.script);
    }
}

// Kits carried without a slot (a new mission, the --kits test switch): each stack gets one, an
// item of its own that isn't in the level.
void World::ensureKitSlots() {
    for (int use : {9, 10}) {
        if ((use == 9 ? player_.bioKits : player_.mechKits) <= 0 || packSlotOfUse(use) >= 0) continue;
        int s = freePackSlot();
        if (s < 0) break;
        Item it;
        it.use = use;
        it.sprite = use == 9 ? "s_biokit" : "s_armork";
        it.name = use == 9 ? "Bio-kit" : "Mech-kit";
        it.icon = use == 9 ? "i_biokit" : "i_armork";
        it.vis = itemVisual(it.sprite, it);
        it.taken = it.carried = true;
        inv_[s] = (int)items_.size();
        items_.push_back(it);
    }
}
