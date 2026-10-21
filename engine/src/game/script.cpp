// Level scripts, as CYCLONE.EXE runs them (FUN_00039f07 loads them; FUN_0003a8e3, FUN_0003a856,
// FUN_0003a316, FUN_0003aff5 and FUN_0003ab56 run them).
//
// A script is a list of conditions that must all hold, then a list of actions run in order. Some
// belong to a trigger cell (the map's 'TRIG' grid) and are tried whenever the player stands in
// that cell (every tick) or uses a wall of it; cell (0, 0) holds scripts tried every 8 ticks. The
// rest belong to an entity: a door's runs when it is used, an item's when it is taken, a
// creature's when it dies. Once the conditions hold, the actions start at the next 8-tick step and
// run until one waits (action 1 adds to the script's timer, of which 64 come off every 8 ticks);
// the script then sleeps until its timer runs out. A finished script never runs again unless its
// flag 0x08 is set; flag 0x02 starts its actions over for ever.
//
// Supported here: the conditions and actions BASE uses, and those that only touch doors,
// messages, sounds, walls, counters and the flow of a script. A script that uses anything else
// stays idle (and a door it would open stays openable, the engine's stand-in until then).
#include <cstdio>

#include "world_internal.h"

namespace {

int u16At(const uint8_t* a, int k) { return a[k] | a[k + 1] << 8; }
int s16At(const uint8_t* a, int k) { return (int16_t)u16At(a, k); }

std::string scriptText(const uint8_t* a) {   // a message piece: up to 20 characters, NUL-ended
    size_t n = 0;
    while (n < 20 && a[n]) ++n;
    return std::string((const char*)a, n);
}

}  // namespace

// The conditions and actions run here. Every condition the game has is (those it doesn't know are
// false, as in the game); of the actions, those no level uses aren't: 6 and 7 (a creature hurt
// by id and group), 15 (a creature taken away), 45 (a mover of wall rows), 49-52 (an item's
// state into the player's: the jump jets' gauges). A script that uses one stays idle.
bool World::scriptOpsSupported(const Script& s) {
    for (const auto& c : s.conds)
        if (c.op < 1 || c.op > 32) return false;
    for (const auto& a : s.acts)
        switch (a.op) {
        case 6: case 7: case 15: case 45: case 49: case 50: case 51: case 52:
            return false;
        default:
            if (a.op < 1 || a.op > 57) return false;
            break;
        }
    return true;
}

void World::initScripts(const LevelEntities& E) {
    scripts_ = E.scripts;
    runs_.assign(scripts_.size(), ScriptRun());
    scriptUnsupported_.assign(scripts_.size(), 0);
    for (size_t i = 0; i < scripts_.size(); ++i) {
        runs_[i].flags = scripts_[i].flags & ~0x05;
        const uint8_t* pr = scripts_[i].params;
        for (int k = 0; k < 4; ++k) runs_[i].p[k] = (int32_t)(pr[k * 4] | pr[k * 4 + 1] << 8 | pr[k * 4 + 2] << 16 | (uint32_t)pr[k * 4 + 3] << 24);
        scriptUnsupported_[i] = !scriptOpsSupported(scripts_[i]);
        if (SDL_getenv("CYCLONES_LIST") && std::atoi(SDL_getenv("CYCLONES_LIST")) >= 3) {   // testing: every script
            std::string t = "dbg script " + std::to_string(i) + " flags " + std::to_string(scripts_[i].flags) + ":";
            auto add = [&](const ScriptAction& a) {
                t += " " + std::to_string(a.op) + "(";
                if (a.op == 32 || a.op == 38 || a.op == 48) {   // text: as text
                    t += "\"";
                    for (int k = 0; k < 20 && a.arg[k]; ++k) t += (char)a.arg[k];
                    t += "\")";
                    return;
                }
                for (int k = 0; k < 8; ++k) t += std::to_string(a.arg[k]) + (k < 7 ? "," : ")");
            };
            for (const auto& a : scripts_[i].conds) add(a);
            t += " |";
            for (const auto& a : scripts_[i].acts) add(a);
            SDL_Log("%s", t.c_str());
        }
        if (SDL_getenv("CYCLONES_LIST"))   // testing: the level's ways out (missions, videos)
            for (const auto& a : scripts_[i].acts) {
                if (a.op == 24 && a.arg[6]) SDL_Log("dbg exit script %d: to mission %d, video %d", (int)i, a.arg[6], u16At(a.arg, 8));
                if (a.op == 25) SDL_Log("dbg exit script %d: the end of the game, videos 22 and 40", (int)i);
            }
    }
    itemNames_ = E.itemNames;
    inv_.fill(-1);
    readied_ = -1;
    invOpen_ = false;
    invCursor_ = 0;
    demoPlaced_.clear();
    detonating_ = hazardOn_ = false;
    cellScripts_ = E.cellScripts;
    if (SDL_getenv("CYCLONES_LIST") && std::atoi(SDL_getenv("CYCLONES_LIST")) >= 3)   // testing: the trigger cells
        for (size_t c = 0; c < cellScripts_.size(); ++c)
            if (!cellScripts_[c].empty()) {
                std::string t;
                for (int si : cellScripts_[c]) t += " " + std::to_string(si);
                SDL_Log("dbg trigger cell %d,%d:%s", (int)(c % kGrid), (int)(c / kGrid), t.c_str());
            }
    cellScripts_.resize(kGrid * kGrid);
    scriptQueue_.clear();
    std::memset(scriptVars_, 0, sizeof(scriptVars_));
    wallEdits_.clear();
    geometryDirty_ = false;
    warp_ = Warp();
    movers_.clear();
    cubeMoving_.assign(level_ ? level_->cubes.size() : 0, 0);
    cubeStartBase_.clear();
    if (level_)
        for (const auto& c : level_->cubes) cubeStartBase_.push_back((int16_t)(c[6] | c[7] << 8));
    movedGeometry_ = navDirty_ = false;
    if (const char* v = SDL_getenv("CYCLONES_COUNTERS")) {   // testing: "3=75,2=50" presets counters
        for (const char* p = v; *p;) {
            int k = 0, n = 0, used = 0;
            if (std::sscanf(p, "%d=%d%n", &k, &n, &used) != 2) break;
            if (k >= 1 && k <= 10) scriptVars_[k - 1] = (uint8_t)n;
            p += used;
            if (*p == ',') ++p;
        }
    }
    if (logScripts()) {
        int cells = 0;
        for (const auto& c : cellScripts_) cells += !c.empty();
        int unsupported = 0;
        for (uint8_t u : scriptUnsupported_) unsupported += u;
        SDL_Log("dbg scripts %d (%d trigger cells), %d not run yet (they need what the engine lacks)", (int)scripts_.size(), cells, unsupported);
    }
}

// The scripts of a cell (FUN_0003a8e3): finished ones drop out of the cell's list, the others are tried.
void World::runCellScripts(int x, int z) {
    if (x < 0 || z < 0 || x >= kGrid || z >= kGrid) return;
    auto& list = cellScripts_[z * kGrid + x];
    for (size_t k = 0; k < list.size();) {
        int i = list[k];
        if (i < 0 || i >= (int)runs_.size()) { ++k; continue; }
        if (runs_[i].flags & 0x04) { list.erase(list.begin() + k); continue; }
        tryScript(i);
        ++k;
    }
}

// FUN_0003a856: a script that isn't running or finished starts if all its conditions hold.
void World::tryScript(int i) {
    if (i < 0 || i >= (int)runs_.size() || scriptUnsupported_[i]) return;
    ScriptRun& r = runs_[i];
    if (r.flags & 0x05) return;
    bool ok = true;
    for (const auto& c : scripts_[i].conds) {
        ok = scriptCondition(i, c);
        if (!ok) break;
    }
    r.events = 0;
    if (!ok) return;
    r.pc = scripts_[i].acts.empty() ? -1 : 0;
    r.timer = 0;
    if (logScripts()) SDL_Log("dbg script %d starts (tick %d)", i, ticks_);
    if (r.pc < 0) {   // nothing to do: it has run
        if (!(r.flags & 0x08)) r.flags |= 0x04;
        return;
    }
    queueScript(i);
}

void World::entityScript(int i, uint8_t event) {
    if (i < 0 || i >= (int)runs_.size()) return;
    runs_[i].events |= event;
    tryScript(i);
}

// FUN_0003a316. Every check clears what the player just did to the script.
bool World::scriptCondition(int i, const ScriptAction& c) {
    ScriptRun& r = runs_[i];
    const uint8_t* a = c.arg;
    uint8_t ev = r.events;
    r.events = 0;
    int px = (int)std::floor(player_.x / kCellSize), pz = (int)std::floor(player_.z / kCellSize);
    switch (c.op) {
    case 1: return a[0] == px && a[1] == pz;                 // the player stands in this cell
    case 4: return ev & 0x20;                                // the cell's north wall was used
    case 5: return ev & 0x40;                                // its west wall
    case 6: return ev & 0x10;                                // its floor
    case 7: return ev & (0x08 | 0x04 | 0x01);                // the door (item, creature) was used
    case 9: return ev & (0x04 | 0x01);
    case 10: return ev & 0x01;
    case 8: return (int32_t)a[0] == r.p[0];
    // A readied item used on something (FUN_0004032d set the item's own script's parameters):
    // 11/12 a wall in cell (a0, a1), 13 a floor, 14 a door in cell (a0, a1), 16 an item, 17 the
    // creature with id a0 and group a1, 25 a door with key a0; 26 an item readied at all.
    case 11: case 12: return r.p[0] == a[0] && r.p[1] == a[1] && readied_ >= 0 && (ev & 0x20);
    case 13: return r.p[0] == a[0] && r.p[1] == ((r.p[0] & ~0xff) | a[1]) && readied_ >= 0 && (ev & 0x10);
    case 14: return r.p[0] == a[0] && r.p[1] == a[1] && readied_ >= 0 && (ev & 0x08);
    case 16: return r.p[0] == a[0] && readied_ >= 0 && (ev & 0x04);
    case 17: return r.p[0] == a[0] && r.p[1] == a[1] && readied_ >= 0 && (ev & 0x01);
    case 25: return r.p[2] == a[0] && readied_ >= 0 && (ev & 0x08);
    case 26: return readied_ >= 0;
    case 27: return detonating_;                             // the demo packs are going off
    case 23: return r.flags & 0x08;                          // its creature died (or it runs again)
    case 28: return a[0] >= 1 && a[0] <= 10 && scriptVars_[a[0] - 1] == a[1];   // a counter's value
    case 29: return a[0] < 10 && scriptVars_[a[0]] == a[1];
    case 30:                                                 // the player is in an area of the map
        return px >= 0 && pz >= 0 && px < kGrid && pz < kGrid && level_->region[pz * kGrid + px] == a[0];
    case 31: return standLayer_ == a[0];                      // the player stands on that surface layer (0 the floor)
    case 32: return a[0] == a[1];   // a prop's script: a shot at the prop sets a1 to its slot (FUN_00030f9e)
    default: return false;
    }
}

// FUN_0003a95f: into the queue of running scripts, after those due no later.
void World::queueScript(int i) {
    auto it = scriptQueue_.begin();
    while (it != scriptQueue_.end() && runs_[*it].timer <= runs_[i].timer) ++it;
    scriptQueue_.insert(it, i);
    runs_[i].flags |= 0x01;
}

// FUN_0003aff5: time passes for the running scripts; those whose wait is over run their next
// actions, up to the next wait (FUN_0003af91).
void World::advanceScripts(int amount) {
    for (int i : scriptQueue_) runs_[i].timer -= amount;
    int guard = 0;
    while (!scriptQueue_.empty() && runs_[scriptQueue_.front()].timer < 1 && ++guard < 1000) {
        int i = scriptQueue_.front();
        scriptQueue_.erase(scriptQueue_.begin());
        ScriptRun& r = runs_[i];
        r.flags &= ~0x01;
        int start = r.timer, steps = 0;
        while (r.pc >= 0 && r.timer == start && ++steps < 256) scriptAction(i);
        if (r.pc >= 0) queueScript(i);
    }
}

// One action (FUN_0003ab56), then on to the next: past the last, a repeating script starts
// over and any other is finished, unless it can run again.
void World::scriptAction(int i) {
    ScriptRun& r = runs_[i];
    const auto& acts = scripts_[i].acts;
    if (r.pc < 0 || r.pc >= (int)acts.size()) { r.pc = -1; return; }
    int pc = r.pc;
    const ScriptAction& act = acts[pc];
    const uint8_t* a = act.arg;
    if (logScripts()) SDL_Log("dbg script %d action %d op %d (tick %d)", i, pc, act.op, ticks_);
    auto door = [&](int cx, int cz) -> Door* {
        int d = scriptDoor(cx, cz);
        return d < 0 ? nullptr : &doors_[d];
    };
    switch (act.op) {
    case 1:   // wait
        r.timer += u16At(a, 0);
        break;
    case 4: {  // health up or down, past armour (FUN_0004e1b8): at nothing the player dies
        PlayerState& p = player_;
        if (!p.alive) break;
        int dh = s16At(a, 0);
        p.health = std::min(104.0f, p.health + dh);
        if (dh < 0) { p.hurt = 1; p.tear += -dh; }
        if (p.health < 1) {
            p.health = 0;
            p.alive = false;
            p.deadTime = 0;
            sound(125, 128);
        }
        break;
    }
    case 5:   // the player dies (FUN_0004e120): a pit, a crusher
        if (player_.alive) {
            player_.health = 0;
            player_.alive = false;
            player_.deadTime = 0;
            sound(125, 128);
        }
        break;
    case 2:   // use the door in a cell (FUN_000134cb: a locked one doesn't move)
        if (Door* d = door(a[0], a[1]); d && !d->locked) {
            d->opening = !d->opening;
            d->openFor = 0;
            soundAt(d->spec.sound, d->cx, d->cz);
        }
        break;
    case 17:  // lock or unlock the door in the cell of the script's parameters
    case 3:   // lock or unlock a door
        if (Door* d = act.op == 17 ? door(r.p[0] & 0xff, r.p[1] & 0xff) : door(a[0], a[1])) d->locked = !d->locked;
        break;
    case 18:  // open a door (locked or not, without its sound)
        if (Door* d = door(a[0], a[1])) { d->opening = true; d->openFor = 0; }
        break;
    case 19:  // close it
        if (Door* d = door(a[0], a[1])) d->opening = false;
        break;
    case 20:  // lock it
        if (Door* d = door(a[0], a[1])) d->locked = true;
        break;
    case 21:  // unlock it
        if (Door* d = door(a[0], a[1])) d->locked = false;
        break;
    case 10:  // put up a wall (FUN_000225f5): a texture both sides, solid
        editWall({a[0], a[1], a[2], s16At(a, 4), s16At(a, 4), 0, s16At(a, 6), false}, true);
        break;
    case 11:  // take a wall away (FUN_00013acd: arguments 1-3 are its cell and side)
        editWall({a[1], a[2], a[3], 0, 0, 0, 0, true}, true);
        break;
    case 37:  // change a wall's flags (FUN_00022c41; bit 0: not solid)
        editWall({a[0], a[1], a[2], -1, -1, a[3], -1, false}, true);
        break;
    case 42:  // change a wall's pictures (FUN_00022d63): a switch thrown, a screen lit
        editWall({a[0], a[1], a[2], u16At(a, 4), u16At(a, 6), -1, -1, false}, true);
        break;
    case 8:   // move a wall up or down (FUN_00022109)
    case 9:   // move a floor up or down: a lift (FUN_000222ea)
    case 33:  // move a block up or down: a gate, a lift (FUN_0002284e)
        startMover(act.op, a);
        break;
    case 14:  // bring in a copy of creature a0 at the middle of cell (a1, a2) (FUN_00048a30)
        spawnCopy(a[0], a[1], a[2]);
        break;
    case 16:  // set creature a1's mode to a0, unless it is dead (FUN_0002b31e)
        setCreatureMode(a[1], a[0]);
        break;
    case 36:  // set creature a1's class to a0 (FUN_0002b362)
        if (Monster* m = slotCreature(a[1])) m->aiClass = a[0];
        break;
    case 41:  // creature a0 drops what it carries (FUN_00048eeb)
        dropCarried(a[0]);
        break;
    case 22:  // start an entry of the level's animations (surfanim) going (FUN_0002208b)
    case 23:  // stop it (FUN_000220ca): ALIEN1's power coils until the power is back
        setSurfAnim(a[0], act.op == 22);
        break;
    case 12:  // a label
        break;
    case 13: {  // go to the label with the same number
        for (size_t k = 0; k < acts.size(); ++k)
            if (acts[k].op == 12 && acts[k].arg[0] == a[0]) { r.pc = (int)k; break; }
        break;
    }
    case 24: {  // move the player (FUN_0003733b), within the level or on to another mission
        Warp w;
        w.pending = true;
        w.cellX = u16At(a, 0);
        w.cellZ = u16At(a, 2);
        w.y = (float)s16At(a, 4);
        w.mission = a[6] && a[6] != mission ? a[6] : 0;
        w.video = w.mission ? u16At(a, 8) : 0;
        // The facing byte: 0 east, 0x40 north, 0x80 west, 0xc0 south.
        w.yaw = (64 - (int)a[16]) * (2 * kPi / 256);
        warp_ = w;
        break;
    }
    case 27:  // set how a group of lights flickers: the engine doesn't animate the level's lights yet
        break;
    case 26: case 29: case 31:   // not in the game's switch: nothing
        break;
    case 25:  // the end of the game (exit code 22: the front end plays videos 22 and 40, and stops)
        warp_ = Warp();
        warp_.pending = true;
        warp_.end = true;
        warp_.video = 22;
        break;
    case 30:  // a sweeping beam of light (0xe8de8: FUN_00044333, drawn by FUN_00044149): the engine
              // doesn't light the level dynamically yet
        break;
    case 34: {  // put on suit a0 (FUN_0004e1ff): its armour at full
        PlayerState& p = player_;
        p.suit = std::clamp((int)a[0] - 1, 0, 2);
        p.armorMax = kSuitMax[p.suit];
        p.armor = p.armorMax;
        break;
    }
    case 35:  // armour + s16(0), held to 0..104 (FUN_0004e2f0), then the script's item is gone (as 43)
        player_.armor = (float)std::clamp((int)player_.armor + s16At(a, 0), 0, 104);
        player_.bonus = 1;
        [[fallthrough]];
    case 43:  // the script's item taken away (FUN_000412f1): out of the inventory, or off the floor
        removeItem(scriptItem(i));
        break;
    case 44: {  // a mover of walls: every west and north wall in x a0..a2, z a1..a3, s16(6) up or down
        startMover(44, a);
        break;
    }
    case 46:  // a demo pack going off (FUN_00033977): a0 cells round it
        demoBlast(a[0], scriptItem(i));
        break;
    case 53: {  // the jump jets' fuel + the script's item's charge, then the item is gone (FUN_00042c81)
        int it = scriptItem(i);
        int add = it >= 0 ? items_[it].amount : 0;
        player_.jetFuel = player_.jetFuel < 0 ? add : player_.jetFuel + add;
        removeItem(it);
        break;
    }
    case 54:  // a hazard: while the player is in area a0, health + s16(2) every a1 x a1 ticks (FUN_0004e3a0)
        if (!hazardOn_ && player_.alive) {
            hazardOn_ = true;
            startMover(54, a);
        }
        break;
    case 55:  // prop a0 shows picture u16(2) from 'startobjects' (FUN_00049085)
        if (Object* o = slotProp(u16At(a, 0))) setPropPicture(*o, u16At(a, 2));
        break;
    case 56:  // prop a0 taken away (FUN_00049114)
        if (Object* o = slotProp(u16At(a, 0))) o->removed = true;
        break;
    case 57:  // prop a0's script stopped (FUN_000490e2 -> FUN_0003a9fb): out of the queue
        if (Object* o = slotProp(a[0]); o && o->script >= 0) {
            auto q = std::find(scriptQueue_.begin(), scriptQueue_.end(), o->script);
            if (q != scriptQueue_.end()) scriptQueue_.erase(q);
            runs_[o->script].flags &= ~0x01;
        }
        break;
    case 47:  // give the player key a0 (FUN_00042c03: into a free key slot)
        player_.keys |= 1 << (a[0] & 7);
        break;
    case 28: {  // a sound (FUN_0004fa54): id, volume, pan
        int id = a[0];
        if (id > 0 && sounds_.size() < 64) sounds_.push_back({id, false, 0, 0, a[1], (int)(int8_t)a[2]});
        break;
    }
    case 32:  // a message (green)
        message(scriptText(a), 0);
        break;
    case 48:  // a message in red
        message(scriptText(a), 3);
        break;
    case 38:  // more of the last message (FUN_0003c30f: up to 69 characters in all)
        if (messages_.empty()) message(scriptText(a), 0);
        else {
            HudMessage& m = messages_.back();
            m.text = (m.text + scriptText(a)).substr(0, 69);
            m.age = 0;
        }
        break;
    case 39: {  // add to a counter, held to 0..255 (FUN_0003b557)
        int k = a[0] - 1;
        if (k >= 0 && k < 10) scriptVars_[k] = (uint8_t)std::clamp((int)scriptVars_[k] + (int)(int8_t)a[1], 0, 255);
        break;
    }
    case 40: {  // set a counter (FUN_0003b5c2)
        int k = a[0] - 1;
        if (k >= 0 && k < 10) scriptVars_[k] = a[1];
        break;
    }
    default:
        break;
    }
    if (r.pc == pc) {
        r.pc = pc + 1 < (int)acts.size() ? pc + 1 : -1;
        if (r.pc < 0) {
            if (r.flags & 0x02) r.pc = 0;            // repeats for ever
            else if (!(r.flags & 0x08)) r.flags |= 0x04;   // finished for good
        }
    }
}

// Once per tick: the movers (FUN_0003ce15), the player's cell (FUN_0004d327), and every 8 ticks
// cell (0, 0) and the clock.
void World::scriptTick() {
    if (ticks_ == 20)
        if (const char* v = SDL_getenv("CYCLONES_KILL")) {   // testing: "35,36" kills those creatures (by index)
            for (const char* q = v; *q;) {
                char* end = nullptr;
                long k = std::strtol(q, &end, 10);
                if (end == q) break;
                if (k >= 0 && k < (long)monsters_.size() && monsters_[k]->state != Monster::Dead && monsters_[k]->state != Monster::Dying)
                    damageMonster(*monsters_[k], 100000, nullptr);
                q = *end == ',' ? end + 1 : end;
            }
        }
    if (ticks_ == 2)
        if (const char* v = SDL_getenv("CYCLONES_GIVE")) {   // testing: "44,5;51,22" takes the items lying in those cells
            for (const char* q = v; *q;) {
                int cx = 0, cz = 0, n = 0;
                if (std::sscanf(q, "%d,%d%n", &cx, &cz, &n) != 2) break;
                for (Item& it : items_)
                    if (!it.taken && (int)std::floor(it.x / kCellSize) == cx && (int)std::floor(it.z / kCellSize) == cz) {
                        bool ok = takeItem(it, true);
                        if (logScripts()) SDL_Log("dbg give %s (use %d, script %d) at %d,%d: %s", it.name.c_str(), it.use, it.script, cx, cz, ok ? "taken" : "not taken");
                    }
                q += n;
                if (*q == ';') ++q;
            }
        }
    if (ticks_ == 100 && SDL_getenv("CYCLONES_EXIT"))   // testing: takes the level's way out to the next mission
        for (size_t i = 0; i < scripts_.size() && !warp_.pending; ++i)
            for (size_t k = 0; k < scripts_[i].acts.size(); ++k) {
                const ScriptAction& a = scripts_[i].acts[k];
                if ((a.op == 24 && a.arg[6]) || a.op == 25) {
                    ScriptRun saved = runs_[i];
                    runs_[i].pc = (int)k;
                    scriptAction((int)i);
                    runs_[i] = saved;
                    break;
                }
            }
    moverTick();
    if (scripts_.empty()) return;
    if (player_.onGround) {   // the surface underfoot (0xef416, set as the player lands)
        const Level& L = *level_;
        int c = std::clamp((int)std::floor(player_.z / kCellSize), 0, kGrid - 1) * kGrid +
                std::clamp((int)std::floor(player_.x / kCellSize), 0, kGrid - 1);
        float y = player_.y;
        auto on = [&](const SurfaceLayer& sl, float h) { return (sl.tex[c] & 0x3fff) && std::abs(h - y) < 1.5f; };
        if (on(L.floor, surfaceY(L.floor, 0, player_.x, player_.z))) standLayer_ = 0;
        else if (on(L.extra1, (float)(int16_t)L.extra1.height[c])) standLayer_ = 2;
        else if (on(L.extra2, (float)(int16_t)L.extra2.height[c])) standLayer_ = 3;
        else standLayer_ = 255;
    }
    if (player_.alive) runCellScripts((int)std::floor(player_.x / kCellSize), (int)std::floor(player_.z / kCellSize));
    if ((ticks_ & 7) == 0) {
        runCellScripts(0, 0);
        advanceScripts(64);
    }
    if (geometryDirty_) applyLevelEdits();
}

int World::scriptDoor(int cx, int cz) const {
    for (size_t k = 0; k < doors_.size(); ++k)
        if (doors_[k].cellX == cx && doors_[k].cellZ == cz) return (int)k;
    return -1;
}

// A wall a script changes (side 0: the cell's west edge, 1: its north edge). -1 keeps a field.
void World::editWall(const WallEdit& e, bool record) {
    if (!levelEdit_ || e.x < 0 || e.z < 0 || e.x >= kGrid || e.z >= kGrid || (e.side != 0 && e.side != 1)) return;
    EdgeLayer& L = e.side == 1 ? levelEdit_->north : levelEdit_->west;
    int c = e.z * kGrid + e.x;
    if (e.remove) {
        L.texFront[c] = L.texBack[c] = 0;
    } else {
        if (e.front >= 0) L.texFront[c] = (uint16_t)e.front;
        if (e.back >= 0) L.texBack[c] = e.back == L.texFront[c] ? 0 : (uint16_t)e.back;
        if (e.base >= 0) { L.base[c] = (uint16_t)e.base; L.vStart[c] = L.vEnd[c] = 0; }
        if (e.flags >= 0) L.flags[c] = (uint8_t)((L.flags[c] & ~8) | ((e.flags & 1) ? 8 : 0));
        if (e.front >= 0 && e.base >= 0) L.flags[c] &= ~8;   // a new wall is solid
    }
    if (record) wallEdits_.push_back(e);
    geometryDirty_ = true;
    if (logScripts()) SDL_Log("dbg wall %s %d,%d %s -> %d/%d", e.remove ? "removed" : "changed", e.x, e.z, e.side ? "north" : "west",
                              L.texFront[c], L.texBack[c]);
}

void World::applyLevelEdits() {
    if (!geometryDirty_ || !levelEdit_) return;
    geometryDirty_ = false;
    renderer_->buildLevel(*levelEdit_);
    buildCollision();
    levelChanged_ = true;
}

// "Use" on a wall (FUN_0003fed1, case 1): the wall under the crosshair, its cell's corner within
// 128 units of the player on each axis; the first script of the cell learns which side was used
// and the cell's scripts are tried.
bool World::useWall(const Camera& cam) {
    float cp = std::cos(cam.pitch);
    float o[3] = {cam.x, cam.y, cam.z};
    float d[3] = {std::sin(cam.yaw) * cp, std::sin(cam.pitch), -std::cos(cam.yaw) * cp};
    float t, n[3];
    int doorHit = -1;
    if (!raycast(o, d, 4096, t, n, &doorHit, true) || doorHit >= 0) return false;
    float hx = o[0] + d[0] * t, hz = o[2] + d[2] * t;
    int cx, cz, side;
    if (std::abs(n[2]) > 0.9f) {        // a north edge (an east-west wall)
        cz = (int)std::lround(hz / kCellSize);
        cx = (int)std::floor(hx / kCellSize);
        side = 1;
        if (std::abs(hz - cz * kCellSize) > 1.5f) return false;
    } else if (std::abs(n[0]) > 0.9f) {  // a west edge
        cx = (int)std::lround(hx / kCellSize);
        cz = (int)std::floor(hz / kCellSize);
        side = 0;
        if (std::abs(hx - cx * kCellSize) > 1.5f) return false;
    } else {
        return false;
    }
    if (cx < 0 || cz < 0 || cx >= kGrid || cz >= kGrid) return false;
    const EdgeLayer& e = side ? level_->north : level_->west;
    if (!e.texFront[cz * kGrid + cx]) return false;
    if (std::abs(cx * (float)kCellSize - player_.x) >= 128 || std::abs(cz * (float)kCellSize - player_.z) >= 128) return false;
    // A readied item used on the wall (FUN_0004032d, case 1). A data stack its transmitter takes
    // (BIOLAB's wall 59) sets the transmitter's screens going: an addition, not in the game, which
    // leaves the terminal as it was (the renderer draws the frames, Renderer::kActiveTerminal).
    if (useReadied(UseWall, cx, cz, 0) && e.texFront[cz * kGrid + cx] == kDataTransmitter)
        editWall({cx, cz, side, Renderer::kActiveTerminal, -1, -1, -1, false}, true);
    auto& list = cellScripts_[cz * kGrid + cx];
    if (list.empty()) return false;
    if (logScripts()) SDL_Log("dbg use wall %d,%d %s", cx, cz, side ? "north" : "west");
    runs_[list.front()].events |= side ? 0x20 : 0x40;
    runCellScripts(cx, cz);
    return true;
}

void World::warpPlayer(int cellX, int cellZ, float y) {
    PlayerState& p = player_;
    p.x = cellX * (float)kCellSize + 32;
    p.z = cellZ * (float)kCellSize + 32;
    // The surface under the given height (FUN_00012a74), the floor if there is none.
    float g = groundBelow(p.x, p.z, y + 1);
    if (g < -1e8f) g = groundBelow(p.x, p.z, 1e6f);
    p.y = g;
    p.viewY = g + 52;   // the eye (FUN_0003733b: the surface + 0x34)
    p.vx = p.vy = p.vz = 0;
    p.onGround = true;
    lastPX_ = p.x;
    lastPZ_ = p.z;
    playerVelX_ = playerVelZ_ = 0;
}

// ---------------------------------------------------------------------------------------------
// Movers: blocks, walls and floors that scripts move (the game's 0xdebb4, run every tick by
// FUN_0003ce15). Each takes a 4-unit step every `speed` ticks.

// The walls and floors the level's scripts move are built apart from the rest of the level
// (with every block), so that a step rebuilds only them.
void World::findMovingParts() {
    movingWalls_.assign(kGrid * kGrid, 0);
    movingSurfaces_.assign(kGrid * kGrid, 0);
    for (const auto& s : scripts_)
        for (const auto& act : s.acts) {
            if (act.op == 44) {   // every west and north wall of a block of cells
                for (int z = act.arg[1]; z <= act.arg[3] && z < kGrid; ++z)
                    for (int x = act.arg[0]; x <= act.arg[2] && x < kGrid; ++x) movingWalls_[z * kGrid + x] |= 3;
                continue;
            }
            if (act.op != 8 && act.op != 9) continue;
            int x = u16At(act.arg, 0), z = u16At(act.arg, 2), which = u16At(act.arg, 6);
            if (x >= kGrid || z >= kGrid) continue;
            if (act.op == 8 && which < 2) movingWalls_[z * kGrid + x] |= which ? 2 : 1;
            if (act.op == 9 && which < 4) movingSurfaces_[z * kGrid + x] |= 1 << which;
        }
    renderer_->setMovingParts(movingWalls_, movingSurfaces_);
}

// Action 8: u16 x, u16 z, s16 distance, u16 side (0 west, 1 north), u16 speed. Action 9: the same
// for a cell's surface (0 the floor; every lift in the game is a flat floor). Both go
// |distance| / 4 steps of 4; at speed 0 the whole distance at once. Action 33: u16 block id,
// s16 base to move to, u16 speed (at 0 the base moves BY that much, and the block never moves
// again); a block already moving ignores it. Every block with the id moves together.
void World::startMover(int op, const uint8_t* a) {
    if (!levelEdit_) return;
    Level& L = *levelEdit_;
    Mover m;
    if (op == 44) {   // FUN_00022e84: x a0..a2, z a1..a3, s16(6), speed u16(8)
        m.kind = 5;
        m.x = a[0];
        m.z = a[1];
        m.x1 = a[2];
        m.z1 = a[3];
        int d = s16At(a, 6);
        m.speed = std::max(1, u16At(a, 8));
        m.step = d < 0 ? -4 : 4;
        m.steps = std::abs(d) / 4;
        if (m.steps == 0) return;
    } else if (op == 54) {   // FUN_0004e3a0: area a0, every a1 checks of a1 ticks, health + s16(2)
        m.kind = 6;
        m.which = a[0];
        m.speed = std::max(1, (int)a[1]);
        m.target = s16At(a, 2);
        m.done = 0;
    } else if (op == 33) {
        m.kind = 3;
        m.id = u16At(a, 0);
        m.target = s16At(a, 2);
        m.speed = u16At(a, 4);
        for (size_t ci = 0; ci < L.cubes.size(); ++ci)
            if ((L.cubes[ci][0] | L.cubes[ci][1] << 8) == m.id) { m.cube = (int)ci; break; }
        if (m.cube < 0 || cubeMoving_[m.cube]) return;
        cubeMoving_[m.cube] = 1;
        int base = 0;
        if (m.speed == 0) { moveCubes(m.id, m.target, &base); return; }
        moveCubes(m.id, 0, &base);
        // A fix of the levels' data (not in the game): a lift platform (a block 16 units high or
        // less that began the level on the floor under it, or up to 16 over it) sent down below
        // that floor stops where it began. SDI2's lift at (8, 30) takes you up to 220 and is then
        // sent to -220, 320 units under the floor (and SUBWAY's at (48, 17) to -700), where the
        // game leaves it, through the floor, until you step into its cell again and it rises all
        // the way back up; SDI2's at (6, 11) comes back 16 units short, its top under the floor.
        if (m.cube < (int)cubeStartBase_.size() && cubeHeight_[m.cube] <= 16) {
            const auto& c = L.cubes[m.cube];
            const int start = cubeStartBase_[m.cube];
            float cx = (c[2] | c[3] << 8) + c[8] * 0.5f, cz = (c[4] | c[5] << 8) + c[10] * 0.5f;
            float floorY = groundBelow(cx, cz, start + 0.5f);
            if (start >= floorY - 0.5f && start <= floorY + 16 && m.target + cubeHeight_[m.cube] < floorY && m.target < start) {
                if (logScripts()) SDL_Log("dbg lift %d sent to %d, under its floor %.0f: stops at %d", m.id, m.target, floorY, start);
                m.target = start;
            }
        }
        m.step = m.target < base ? -4 : 4;
    } else {
        m.kind = op == 9 ? 1 : 2;
        m.x = u16At(a, 0);
        m.z = u16At(a, 2);
        int d = s16At(a, 4);
        m.which = u16At(a, 6);
        m.speed = u16At(a, 8);
        if (m.x >= kGrid || m.z >= kGrid || m.which >= (m.kind == 2 ? 2 : 4)) return;
        uint16_t& v = m.kind == 2 ? (m.which ? L.north : L.west).base[m.z * kGrid + m.x]
                                  : (m.which == 0 ? L.floor : m.which == 1 ? L.ceiling : m.which == 2 ? L.extra1 : L.extra2).height[m.z * kGrid + m.x];
        if (m.speed == 0) {
            v = (uint16_t)((int16_t)v + d);
            movedGeometry_ = navDirty_ = true;
            return;
        }
        m.step = d < 0 ? -4 : 4;
        m.steps = std::abs(d) / 4;
        if (m.steps == 0) return;
    }
    m.last = ticks_;
    if (logScripts()) SDL_Log("dbg mover kind %d (%d,%d side %d / block %d) step %d speed %d", m.kind, m.x, m.z, m.which, m.id, m.step, m.speed);
    movers_.push_back(m);
}

// FUN_000126fb: every block with the id moves by delta (0: just reports). A block going down
// stops for whatever is under it; one going up lifts what stands on it. lastBase: the base of
// the last block with the id. False if there is none or the last one was in the way.
bool World::moveCubes(int id, int delta, int* lastBase) {
    Level& L = *levelEdit_;
    bool ok = false;
    for (size_t ci = 0; ci < L.cubes.size(); ++ci) {
        auto& c = L.cubes[ci];
        if ((c[0] | c[1] << 8) != id) continue;
        auto u16 = [&](int k) { return (int)(c[k * 2] | c[k * 2 + 1] << 8); };
        int base = (int16_t)u16(3);
        *lastBase = base;
        ok = true;
        if (delta == 0) continue;
        float x0 = (float)u16(1), z0 = (float)u16(2), x1 = x0 + c[8], z1 = z0 + c[10];
        float hgt = cubeHeight_[ci], nb = (float)(base + delta), oldTop = base + hgt, newTop = nb + hgt;
        auto overlaps = [&](float x, float z, float r, float y0, float y1) {
            return x + r > x0 && x - r < x1 && z + r > z0 && z - r < z1 && y0 < newTop && y1 > nb;
        };
        // (The body's reach: the player's is sampled 0.92 of its radius out, so it can stand a
        // little closer than its radius; a creature's as in playerFits.)
        PlayerState& p = player_;
        if (delta < 0) {
            bool blocked = p.alive && overlaps(p.x, p.z, 16.0f, p.y, p.y + 55.0f);
            for (const auto& m : monsters_) {
                if (blocked) break;
                if (m->state == Monster::Dead || m->state == Monster::Dying || m->gone) continue;
                float r = std::clamp(m->vis->radius * 0.8f, 8.0f, 28.0f) * 0.8f;
                blocked = overlaps(m->x, m->z, r, m->y, m->y + m->vis->height);
            }
            if (blocked) { ok = false; continue; }
        }
        c[6] = (uint8_t)(base + delta);
        c[7] = (uint8_t)((base + delta) >> 8);
        *lastBase = base + delta;
        movedGeometry_ = true;
        if (delta > 0 && p.alive && p.y >= oldTop - 2 && overlaps(p.x, p.z, 16.0f, p.y, p.y + 55.0f)) {
            p.y = newTop;   // standing on it: lifted
            if (p.vy < 0) p.vy = 0;
        }
        // What stands on it rides along (FUN_000229c4): things within 4 of the top, creatures within 11.
        for (auto& it : items_)
            if (!it.taken && it.x >= x0 && it.x <= x1 && it.z >= z0 && it.z <= z1 && std::abs(it.y - oldTop) <= 4) it.y = newTop;
        for (auto& m : monsters_) {
            if (m->state == Monster::Dead || m->gone || m->hover > 0) continue;
            if (m->x < x0 || m->x > x1 || m->z < z0 || m->z > z1 || std::abs(m->y - oldTop) > 11) continue;
            m->y = newTop;
            m->ground = newTop;
            if (std::abs(m->ty - oldTop) <= 11) m->ty = newTop;
        }
    }
    return ok;
}

// One step of a mover; false once it has finished.
bool World::moverStep(Mover& m) {
    Level& L = *levelEdit_;
    if (m.kind == 3) {   // FUN_000229c4: on toward the target until within a step of it
        int cur = 0;
        moveCubes(m.id, 0, &cur);
        bool go = m.step > 0 ? cur <= m.target - 4 : cur >= m.target + 4;
        if (!go) {
            if (m.cube >= 0 && m.cube < (int)cubeMoving_.size()) cubeMoving_[m.cube] = 0;
            return false;
        }
        moveCubes(m.id, m.step, &cur);   // in the way: tried again next time
        return true;
    }
    if (m.kind == 5) {   // FUN_00022fb1: every wall of the block, a step
        for (int x = m.x; x <= m.x1 && x < kGrid; ++x)
            for (int z = m.z; z <= m.z1 && z < kGrid; ++z) {
                uint16_t& w = L.west.base[z * kGrid + x];
                uint16_t& n = L.north.base[z * kGrid + x];
                w = (uint16_t)((int16_t)w + m.step);
                n = (uint16_t)((int16_t)n + m.step);
            }
        movedGeometry_ = true;
        return ++m.done < m.steps;
    }
    if (m.kind == 6) {   // FUN_0004e443: on every speed-th call, the player in the area or not
        if (m.done == m.speed) {
            m.done = 0;
            PlayerState& p = player_;
            int px = std::clamp((int)std::floor(p.x / kCellSize), 0, kGrid - 1), pz = std::clamp((int)std::floor(p.z / kCellSize), 0, kGrid - 1);
            if (!p.alive || level_->region[pz * kGrid + px] != m.which) {
                hazardOn_ = false;
                return false;
            }
            p.health = std::min(104.0f, p.health + m.target);
            if (m.target < 0) { p.hurt = 1; p.tear += -m.target; }
            if (p.health < 1) {
                p.health = 0;
                p.alive = false;
                p.deadTime = 0;
                sound(125, 128);
            }
        }
        ++m.done;
        return true;
    }
    int c = m.z * kGrid + m.x;
    if (m.kind == 2) {   // FUN_0002225a: a wall's base
        uint16_t& b = (m.which ? L.north : L.west).base[c];
        b = (uint16_t)((int16_t)b + m.step);
    } else {             // FUN_0002243d: a floor, with what lies and stands on it
        SurfaceLayer& s = m.which == 0 ? L.floor : m.which == 1 ? L.ceiling : m.which == 2 ? L.extra1 : L.extra2;
        int h = (int16_t)s.height[c] + m.step;
        s.height[c] = (uint16_t)h;
        float x0 = (float)m.x * kCellSize, z0 = (float)m.z * kCellSize;
        auto inCell = [&](float x, float z) { return x >= x0 && x < x0 + kCellSize && z >= z0 && z < z0 + kCellSize; };
        for (auto& it : items_)
            if (!it.taken && inCell(it.x, it.z) && std::abs(it.y - h) <= 4) it.y = (float)h;
        for (auto& mo : monsters_) {
            if (mo->state == Monster::Dead || mo->gone || mo->hover > 0 || !inCell(mo->x, mo->z) || std::abs(mo->y - h) > 4) continue;
            mo->y = mo->ground = (float)h;
        }
    }
    movedGeometry_ = true;
    return ++m.done < m.steps;
}

void World::moverTick() {
    for (size_t k = 0; k < movers_.size();) {
        Mover& m = movers_[k];
        if (ticks_ < m.last + m.speed) { ++k; continue; }
        m.last = ticks_;
        if (moverStep(m)) { ++k; continue; }
        if (logScripts()) SDL_Log("dbg mover kind %d done (tick %d)", m.kind, ticks_);
        if (m.kind != 6) navDirty_ = true;   // where creatures can walk has changed
        movers_.erase(movers_.begin() + (long)k);
    }
    if (movedGeometry_) refreshMoving();
    if (navDirty_) {
        navDirty_ = false;
        buildNav();   // where creatures can walk has changed
    }
}

// ---------------------------------------------------------------------------------------------
// Creatures in scripts. Scripts name a creature by its slot in the game's table: the level's
// creatures this difficulty keeps, in file order, then those the scripts have brought in.

World::Monster* World::slotCreature(int slot) {
    if (slot < 0 || slot >= (int)slotMonster_.size()) return nullptr;
    int i = slotMonster_[slot];
    return i < 0 || i >= (int)monsters_.size() ? nullptr : monsters_[i].get();
}

// Action 14: a copy of a creature (as it is now: its hit points, what it carries, its script) in
// the middle of a cell, set going (mode 1). Not of a dead one, and not past the table's 80; the
// slot is used up even when the copy can't go there because something stands in the way.
void World::spawnCopy(int from, int cx, int cz) {
    Monster* src = slotCreature(from);
    if (!src || src->state == Monster::Dying || src->state == Monster::Dead || src->gone) return;
    if ((int)slotMonster_.size() >= 80 || cx >= kGrid || cz >= kGrid) return;
    slotMonster_.push_back(-1);
    float x = cx * (float)kCellSize + 32, z = cz * (float)kCellSize + 32;
    float ground = groundBelow(x, z, src->ground + 24);
    float r = std::clamp(src->vis->radius * 0.8f, 8.0f, 28.0f), y0 = ground + src->hover, y1 = y0 + src->vis->height;
    const PlayerState& p = player_;
    if (p.alive && std::abs(p.x - x) < r + 20 && std::abs(p.z - z) < r + 20 && p.y < y1 && p.y + 55 > y0) return;
    for (const auto& o : monsters_) {
        if (o->state == Monster::Dead || o->state == Monster::Dying || o->gone) continue;
        float rr = r + std::clamp(o->vis->radius * 0.8f, 8.0f, 28.0f);
        if (std::abs(o->x - x) < rr && std::abs(o->z - z) < rr && o->y < y1 && o->y + o->vis->height > y0) return;
    }
    auto m = std::make_unique<Monster>(*src);
    m->index = (int)monsters_.size();
    m->slot = (int)slotMonster_.size() - 1;
    m->copyOf = src->index;
    m->x = m->tx = x;
    m->z = m->tz = z;
    m->ground = m->ty = ground;
    m->y = y0;
    m->cx = m->ncx = cx;
    m->cz = m->ncz = cz;
    m->moving = false;
    m->startMode = 1;
    m->state = Monster::Walk;
    m->stateTime = 0;
    m->wait = 0;
    m->stagger = 0;
    m->flash = 0;
    m->view = -1;
    m->attackFrame = -1;
    m->lastFire = false;
    m->unfolded = false;
    m->resumeAttack = false;
    m->walkOnly = false;
    m->active = m->shotActive = false;
    m->thinkTimer = 0;
    m->alerted = m->seesPlayer = false;
    m->path.clear();
    m->pathGoal = -1;
    occupied_[cz * kGrid + cx]++;
    if (m->stationary) fixed_[cz * kGrid + cx]++;
    slotMonster_.back() = m->index;
    if (logScripts()) SDL_Log("dbg creature %d (slot %d) copied to %d,%d as slot %d", src->index, from, cx, cz, m->slot);
    monsters_.push_back(std::move(m));
}

// Action 16 (FUN_0002af83's modes as the game's data uses them: 1 and 2 going, 0 idle, 13
// frozen; 12 is frozen too, and the rest are moments of its own animation the engine keeps).
void World::setCreatureMode(int slot, int mode) {
    Monster* m = slotCreature(slot);
    if (!m || m->state == Monster::Dying || m->state == Monster::Dead || m->gone) return;
    if (mode == 0 || mode == 1 || mode == 2) {
        m->startMode = mode;
        m->thinkTimer = 0;
    } else if (mode == 12 || mode == 13) {
        m->startMode = 13;
        if (m->state == Monster::Attack || m->state == Monster::Ready || m->state == Monster::Unready) {
            m->state = Monster::Walk;
            m->attackFrame = -1;
        }
        m->path.clear();
        m->pathGoal = -1;
    }
    if (logScripts()) SDL_Log("dbg creature slot %d mode %d", slot, mode);
}

// Action 41: what a creature carries (its slot 2) falls where it stands; it no longer has it.
void World::dropCarried(int slot) {
    Monster* m = slotCreature(slot);
    if (!m || !m->hasDrop) return;
    dropItem(*m);
    m->hasDrop = false;
    if (logScripts() && !items_.empty())
        SDL_Log("dbg creature slot %d dropped %s at %.2f,%.2f y %.0f", slot, m->dropSprite.c_str(), items_.back().x / kCellSize,
                items_.back().z / kCellSize, items_.back().y);
}
