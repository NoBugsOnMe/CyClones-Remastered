// Combat, following CYCLONE.EXE: the player's weapons (FUN_000509ee, FUN_0003308a), hitscan
// (FUN_00030f9e / FUN_00031ef2), projectiles and their impacts (FUN_00031342, handler 0x3354f),
// melee (FUN_00031bf2), damage to monsters (FUN_0004848a) and to the player (FUN_00032cf2 /
// FUN_00032f0e), area blasts (FUN_00032991) and pickups (FUN_000420c0).
#include "world_internal.h"

namespace {

int irand(std::mt19937& r, int lo, int hi) {
    if (hi < lo) std::swap(lo, hi);
    return lo + (int)(r() % (unsigned)(hi - lo + 1));
}

// Range band by horizontal distance: 96, 224, 352, 608 units; -1 beyond.
int rangeBand(float d) { return d <= 96 ? 0 : d <= 224 ? 1 : d <= 352 ? 2 : d <= 608 ? 3 : -1; }

// Monster armour (FUN_0004848a): per armour byte, damage soaked per damage type and wear per hit.
const int kMonAbsorb[4][6] = {{10, 0, 0, 0, 0, 0}, {0, 10, 0, 10, 0, 0}, {3, 0, 10, 0, 0, 0}, {0, 0, 0, 5, 0, 10}};
const int kMonWear[4][6] = {{10, 10, 20, 5, 30, 0}, {10, 10, 50, 20, 30, 0}, {10, 0, 10, 0, 30, 0}, {10, 10, 20, 50, 20, 20}};
// Player armour: wear per damage type, and the suit's absorption shift for types 1, 2, 3 and 5.
const int kWear[6] = {10, 10, 20, 5, 30, 0};
const int kSuitShift[3][4] = {{3, 8, 8, 8}, {2, 2, 8, 2}, {1, 1, 1, 2}};
const char* kSuitName[3] = {"SABRE GLOVE", "RAZOR SAW", "NANOTECHNOLOGY HAND"};

// Sounds (SOUND\<id>.RAW). The weapon table (FUN_000509ee) gives each weapon an "armed" sound
// (+0x42, 104 for all), a firing sound (+0x43), an ammunition pickup sound (+0x44, 102) and an
// empty click (+0x45, 103); the glove's swing depends on the suit (FUN_0003308a).
const int kFireSound[8] = {26, 100, 59, 61, 101, 77, 78, 151};
const int kArmedSound = 104, kAmmoSound = 102, kEmptySound = 103;
const int kGloveSound[3] = {139, 108, 142};
// Impacts (handler 0x3354f, u16 table at 0x755ec) by damage type: explosive 47, nanotech 150.
int impactSound(int type) { return type == 2 ? 47 : type == 4 ? 150 : 0; }

const char* bitKey(int species) { return species == 1 ? "expbit" : species == 2 ? "gblbit" : species == 3 ? "pblbit" : "rblbit"; }
const char* chunkKey(int species) { return species == 1 ? "expchk" : species == 2 ? "gblchk" : species == 3 ? "pblchk" : "rblchk"; }

}  // namespace

// ---------------------------------------------------------------------------------------------
// Player weapons

void World::selectWeapon(int index) {
    if (index < 0 || index > 8 || !player_.owned[index]) return;
    if (index != 8 && player_.ammo[index] <= 0) { message("NO AMMUNITION FOR THE " + std::string(kWeapons[index].name), 0); return; }
    if (player_.weapon != index) {
        player_.weapon = index;
        cooldown_ = std::max(cooldown_, 0.25f);   // a short switch delay
        if (index < 8) sound(kArmedSound);        // FUN_00052296
    }
}

void World::cycleWeapon(int dir) {
    for (int k = 1; k <= 9; ++k) {
        int w = ((player_.weapon + dir * k) % 9 + 9) % 9;
        if (!player_.owned[w] || (w != 8 && player_.ammo[w] <= 0)) continue;
        selectWeapon(w);
        return;
    }
}

std::string World::weaponName() const { return kWeapons[player_.weapon].name; }

void World::makeNoise(float x, float y, float z, float range) {
    noiseX_ = x;
    noiseY_ = y;
    noiseZ_ = z;
    noiseTime_ = time_;
    noiseRange_ = range;
}

void World::updateFiring(float dt, const Camera& cam, bool firing) {
    cooldown_ -= dt;
    PlayerState& p = player_;
    if (meleeTimer_ > 0) {
        meleeTimer_ -= dt;
        if (meleeTimer_ <= 0 && p.alive) playerMelee(cam);
    }
    p.firing = p.alive && firing;
    if (!p.alive || !firing || cooldown_ > 0) return;
    const WeaponDef& w = kWeapons[p.weapon];
    if (w.kind != 2 && p.ammo[p.weapon] <= 0) {
        // Out of ammunition: switch to the best weapon that has some, or the glove.
        message("AMMUNITION FULLY DEPLETED.", 3);
        sound(kEmptySound);
        int best = 8;
        for (int k = 7; k >= 0; --k)
            if (p.owned[k] && p.ammo[k] > 0) { best = k; break; }
        p.weapon = best;
        cooldown_ = 0.3f;
        return;
    }
    // The game looks at the fire button every 8 ticks (FUN_0003308a runs when the tick count is a
    // multiple of 8), so a weapon's period is rounded up to 8 ticks: only the glove's 25 changes (32).
    cooldown_ = (float)((w.period + 7) / 8 * 8) / kTicRate;
    if (w.kind != 2) p.ammo[p.weapon]--;
    ++p.shots;
    if (w.kind == 2) p.swingAt = time_;
    sound(w.kind == 2 ? kGloveSound[std::clamp(p.suit, 0, 2)] : kFireSound[p.weapon]);
    float cy = std::cos(cam.yaw), sy = std::sin(cam.yaw), cp = std::cos(cam.pitch), sp = std::sin(cam.pitch);
    float fwd[3] = {sy * cp, sp, -cy * cp}, right[3] = {cy, 0, sy};
    if (w.kind == 0) {
        playerHitscan(cam, p.weapon);
        makeNoise(cam.x, cam.y, cam.z, 12 * kCellSize);
        lights_.push_back({cam.x + fwd[0] * 20, cam.y, cam.z + fwd[2] * 20, 160, 130, 0.3f, 0.25f, 0.08f, 0, 0.06f});
        return;
    }
    if (w.kind == 2) {
        meleeTimer_ = 0.15f;   // the swing lands at tick 15 of 25
        return;
    }
    // Projectiles fly toward whatever is under the crosshair: a creature or a barrel if one is
    // there, otherwise the wall. (Aimed at the wall behind, a shot started below the eye passed
    // under small creatures such as gunballs.)
    makeNoise(cam.x, cam.y, cam.z, 12 * kCellSize);
    float o[3] = {cam.x, cam.y, cam.z}, t, n[3];
    float aim = raycast(o, fwd, 6000, t, n) ? t : 6000.0f;
    for (const auto& m : monsters_) {
        if (m->state == Monster::Dying || m->state == Monster::Dead) continue;
        float tt;
        if (rayHitsMonsterThin(*m, o, fwd, aim, tt) && tt < aim) aim = tt;
    }
    for (const auto& ob : objects_) {
        if (!ob.barrel || !ob.intact || ob.removed) continue;
        float ox = o[0] - ob.x, oz = o[2] - ob.z, r = std::max(10.0f, ob.vis->radius);
        float a = fwd[0] * fwd[0] + fwd[2] * fwd[2], b = 2 * (ox * fwd[0] + oz * fwd[2]), c = ox * ox + oz * oz - r * r;
        float disc = b * b - 4 * a * c;
        if (a < 1e-6f || disc < 0) continue;
        float tt = (-b - std::sqrt(disc)) / (2 * a);
        float yy = o[1] + fwd[1] * tt;
        if (tt > 0 && tt < aim && yy >= ob.y && yy <= ob.y + ob.vis->height) aim = tt;
    }
    aim = std::max(aim, 24.0f);
    float tx = cam.x + fwd[0] * aim, ty = cam.y + fwd[1] * aim, tz = cam.z + fwd[2] * aim;
    float hf[3] = {sy, 0, -cy};
    for (int b = 0; b < w.bolts; ++b) {
        float side = w.bolts > 1 ? (b == 0 ? -w.side : w.side) : 0.0f;
        Shot s;
        s.x = cam.x + hf[0] * w.ahead + right[0] * side;
        s.y = cam.y + w.up;
        s.z = cam.z + hf[2] * w.ahead + right[2] * side;
        float d[3] = {tx - s.x, ty - s.y, tz - s.z};
        float l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (l < 1e-3f) continue;
        s.vx = d[0] / l * w.speed;
        s.vy = d[1] / l * w.speed;
        s.vz = d[2] / l * w.speed;
        s.owner = -1;
        s.visual = w.shot;
        s.impact = w.impact;
        s.dmgMin = w.dmgMin[0];
        s.dmgMax = w.dmgMax[0];
        s.splash = w.dmgMax[1] ? w.dmgMin[1] : 0;
        s.type = w.type;
        if (w.fuse > 0) { s.fuse = w.fuse; s.gravity = 312.5f; s.vy += 60; }
        if (w.type == 2) { s.lr = 0.3f; s.lg = 0.2f; s.lb = 0.06f; }
        else { s.lr = 0.12f; s.lg = 0.2f; s.lb = 0.3f; }
        shots_.push_back(s);
    }
    lights_.push_back({cam.x + fwd[0] * 20, cam.y, cam.z + fwd[2] * 20, 140, 110, 0.25f, 0.2f, 0.1f, 0, 0.06f});
}

void World::playerHitscan(const Camera& cam, int weaponIndex) {
    const WeaponDef& w = kWeapons[weaponIndex];
    float cy = std::cos(cam.yaw), sy = std::sin(cam.yaw), cp = std::cos(cam.pitch), sp = std::sin(cam.pitch);
    float d[3] = {sy * cp, sp, -cy * cp};
    float o[3] = {cam.x, cam.y, cam.z}, t, n[3];
    float best = 6000;
    bool wall = raycast(o, d, best, t, n, nullptr, true);   // (the game's hit test is its pick: through holes)
    if (wall) best = t;
    Monster* hit = nullptr;
    for (auto& m : monsters_) {
        if (m->state == Monster::Dying || m->state == Monster::Dead) continue;
        float tt;
        if (rayHitsMonster(*m, o, d, best, tt) && tt < best) { best = tt; hit = m.get(); wall = false; }
    }
    Object* barrel = nullptr;
    for (auto& ob : objects_) {
        if (!ob.barrel || !ob.intact || ob.removed) continue;
        float ox = o[0] - ob.x, oz = o[2] - ob.z, r = std::max(10.0f, ob.vis->radius);
        float a = d[0] * d[0] + d[2] * d[2], b = 2 * (ox * d[0] + oz * d[2]), c = ox * ox + oz * oz - r * r;
        float disc = b * b - 4 * a * c;
        if (a < 1e-6f || disc < 0) continue;
        float tt = (-b - std::sqrt(disc)) / (2 * a);
        if (tt < 0 || tt >= best) continue;
        float yy = o[1] + d[1] * tt;
        if (yy < ob.y || yy > ob.y + ob.vis->height) continue;
        best = tt;
        barrel = &ob;
        hit = nullptr;
        wall = false;
    }
    // Other props stop the shot too, with a puff of smoke; one with a script learns it was shot
    // (FUN_00030f9e: its first condition's second argument becomes the prop's slot, so condition
    // 32 holds for the prop it names, and the script is tried).
    Object* prop = nullptr;
    {
        float pt = 0;
        Object* po = propInSight(cam, best, &pt);
        if (po && !po->barrel && pt < best) {
            prop = po;
            best = pt;
            barrel = nullptr;
            hit = nullptr;
            wall = false;
        }
    }
    float p[3] = {o[0] + d[0] * best, o[1] + d[1] * best, o[2] + d[2] * best};
    if (prop) {
        spawnEffect("puff", p[0] - d[0] * 3, p[1] - d[1] * 3, p[2] - d[2] * 3, 0.9f, 1.05f, 1, true, 0, 0);
        if (prop->script >= 0 && prop->script < (int)scripts_.size() && !scripts_[prop->script].conds.empty()) {
            scripts_[prop->script].conds.front().arg[1] = (uint8_t)prop->slot;
            if (logScripts()) SDL_Log("dbg prop %d shot: script %d", prop->slot, prop->script);
            tryScript(prop->script);
        }
        return;
    }
    // The plasma accelerator's bolt, so the beam reads. The gauss pistol shows none: in the game
    // it hits at once, with its muzzle flash and a puff where it lands (FUN_00030f9e); its weapon
    // record has no projectile sprite. (Up to 0.5.2 the engine sent a green `gbolt` after it, a
    // sprite only the tutorial's creatures fire.)
    if (weaponIndex == 7) {
        Shot s;
        s.tracer = true;
        s.visual = "rdplsm";
        float hf = std::sqrt(d[0] * d[0] + d[2] * d[2]);
        float side = hf > 1e-3f ? 5.0f : 0.0f;
        s.x = cam.x + d[0] * 14 + (hf > 1e-3f ? -d[2] / hf * side : 0);
        s.y = cam.y - 6;
        s.z = cam.z + d[2] * 14 + (hf > 1e-3f ? d[0] / hf * side : 0);
        float e[3] = {p[0] - s.x, p[1] - s.y, p[2] - s.z};
        float l = std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
        if (l > 20) {
            float speed = 3200;
            s.vx = e[0] / l * speed;
            s.vy = e[1] / l * speed;
            s.vz = e[2] / l * speed;
            s.range = l;
            shots_.push_back(s);
        }
    }
    if (barrel) {
        barrel->intact = false;
        barrel->wreckTime = 0;
        float by = barrel->y + barrel->vis->height * 0.5f;
        explode(barrel->x, by, barrel->z, 1.0f, "barreld");
        splashDamage(barrel->x, by, barrel->z, irand(rng_, 40, 80), 2, nullptr, false);
        return;
    }
    if (hit) {
        float dx = hit->x - cam.x, dz = hit->z - cam.z;
        int band = rangeBand(std::sqrt(dx * dx + dz * dz));
        spawnEffect("blood" + std::to_string(hit->blood), p[0] - d[0] * 3, p[1] - d[1] * 3, p[2] - d[2] * 3, 1.0f, 1.0f, 1,
                    hit->blood == 2, 0, 0);
        if (weaponIndex == 7) spawnEffect("rdplsmd", p[0], p[1], p[2], 0.8f, 1.0f, 2, true, 0, 0);
        if (band < 0) {                             // beyond 608 units: blood, but no damage
            hit->active = hit->shotActive = true;   // (the game still marks the creature as shot at)
            return;
        }
        int chance = 10 + w.acc[band] - hit->evasion;
        if ((int)(rng_() % 100) >= chance) return;  // a miss
        float from[3] = {cam.x, cam.y, cam.z};
        damageMonster(*hit, (float)irand(rng_, w.dmgMin[band], w.dmgMax[band]), from, w.type, true);
        return;
    }
    if (wall) {
        float x = p[0] + n[0] * 3, y = p[1] + n[1] * 3, z = p[2] + n[2] * 3;
        spawnEffect(weaponIndex == 7 ? "rdplsmd" : "puff", x, y, z, 0.9f, 1.05f, weaponIndex == 7 ? 2 : 1, true, 0, 0);
        for (int k = 0; k < 4; ++k) {
            Particle sp2;
            sp2.kind = Particle::Spark;
            sp2.x = x; sp2.y = y; sp2.z = z;
            float v = frand(rng_, 80, 240);
            sp2.vx = (n[0] + frand(rng_, -0.7f, 0.7f)) * v;
            sp2.vy = (n[1] + frand(rng_, -0.3f, 0.9f)) * v;
            sp2.vz = (n[2] + frand(rng_, -0.7f, 0.7f)) * v;
            sp2.life = frand(rng_, 0.15f, 0.35f);
            sp2.size0 = sp2.size1 = 0.6f;
            sp2.gravity = kGravity;
            sp2.drag = 1.5f;
            addParticle(sp2);
        }
    }
}

void World::playerMelee(const Camera& cam) {
    // A 20-unit box 10 to 30 units ahead, from the eye down 27 units (FUN_00031bf2).
    float fx = std::sin(cam.yaw), fz = -std::cos(cam.yaw);
    float bx = cam.x + fx * 20, bz = cam.z + fz * 20, by = cam.y - 27;
    static const int kMin[3] = {10, 10, 50}, kMax[3] = {20, 20, 100}, kType[3] = {0, 0, 4};
    for (auto& m : monsters_) {
        if (m->state == Monster::Dying || m->state == Monster::Dead) continue;
        float r = std::max(10.0f, m->vis->radius) + 10;
        if (std::abs(m->x - bx) > r || std::abs(m->z - bz) > r) continue;
        if (by + 20 < m->y || by > m->y + m->vis->height) continue;
        int chance = 10 + 100 - m->evasion;
        if ((int)(rng_() % 100) >= chance) return;
        float from[3] = {cam.x, cam.y, cam.z};
        int s = std::clamp(player_.suit, 0, 2);
        damageMonster(*m, (float)irand(rng_, kMin[s], kMax[s]), from, kType[s], true);
        spawnEffect("blood" + std::to_string(m->blood), m->x, m->y + m->vis->height * 0.6f, m->z, 1.0f, 1.0f, 1, m->blood == 2, 0, 0);
        return;
    }
}

// ---------------------------------------------------------------------------------------------
// Damage

void World::damageMonster(Monster& m, float amount, const float* from, int type, bool byPlayer) {
    if (m.state == Monster::Dying || m.state == Monster::Dead) return;
    if (traceMonster_ == -2) SDL_Log("dbg damage monster %d: %.0f (type %d)", m.index, amount, type);
    // The Queen and other creatures with no death animation can't be killed here.
    if (m.vis->death.empty() && !m.barrel && m.aiClass != 3) amount = 0;
    int p = (int)amount, dmg = p;
    if (difficulty == 0) {   // easy: a quarter more, twice (as the game does)
        p += p >> 2;
        dmg = p + (p >> 2);
    }
    type = std::clamp(type, 0, 5);
    for (int k = 0; k < 4; ++k) {
        if (m.armour[k] <= 0) continue;
        dmg -= (int)m.armour[k] * kMonAbsorb[k][type];
        m.armour[k] = std::max(0.0f, m.armour[k] - kMonWear[k][type] * p / 10.0f);
    }
    m.flash = 1;
    if (byPlayer) {
        m.active = m.shotActive = true;
        if (from) {
            m.lastSeenX = player_.x;
            m.lastSeenZ = player_.z;
            m.lastSeenY = player_.y;
            m.lastSeenTime = time_;
            m.alerted = true;
            if (!m.barrel && m.aiClass != 3) m.yaw = std::atan2(from[0] - m.x, -(from[2] - m.z));  // turns to face the shooter
        }
    }
    if (dmg <= 0) return;
    m.hp -= dmg;
    int bits = std::min(9, dmg >> (m.hp > 0 ? 3 : 2));
    if (type == 4) bits = 0;
    for (int k = 0; k < bits; ++k) {
        Effect e;
        bool chunk = (k % 3) == 2;
        e.vis = fx(chunk ? chunkKey(m.species) : bitKey(m.species));
        if (!e.vis) break;
        float th = frand(rng_, 0, 2 * kPi), up = frand(rng_, 0.3f, 1.0f), sp = frand(rng_, 80, 220);
        e.x = m.x; e.y = m.y + m.vis->height * 0.55f; e.z = m.z;
        e.vx = std::cos(th) * sp * (1 - up * 0.5f);
        e.vy = up * sp;
        e.vz = std::sin(th) * sp * (1 - up * 0.5f);
        e.gravity = kGravity;
        e.drag = 0.5f;
        e.life = frand(rng_, 0.4f, 0.8f);
        e.mode = 1;
        e.bounce = true;
        e.fullbright = false;
        e.then = std::string(chunk ? chunkKey(m.species) : bitKey(m.species)) + "d";
        effects_.push_back(e);
    }
    if (m.hp > 0) {
        // Sounds: the creature's hurt sound, or for nanotech (which never makes it recoil) 98.
        creatureSound(m, type == 4 && m.aiClass != 3 && !m.barrel ? 98 : m.sndPain);
        if (m.aiClass == 3 || m.barrel) return;
        if (type == 4) return;   // nanotech: a flash, no recoil
        // The game recoils on about one hit in four; the enhanced AI on every hit, with a gap.
        bool recoil = enhancedAI ? time_ >= m.painReady : (ticks_ & 3) == 0;
        if (!recoil) return;
        if (!m.vis->pain.empty()) {
            if (m.state == Monster::Walk || m.state == Monster::Attack || m.state == Monster::Ready) {
                if (m.state == Monster::Ready || (m.state == Monster::Attack && m.aiClass == 2)) {
                    m.resumeAttack = true;   // the game fires again after the recoil (mode 0xb)
                    if (m.state == Monster::Attack) m.readySlot = m.attackSlot;
                }
                m.state = Monster::Pain;
                m.stateTime = 0;
                m.attackFrame = -1;
            }
        } else {
            m.stagger = 0.25f;
        }
        return;
    }
    // Killed.
    m.hp = 0;
    ++kills_;
    if (m.script >= 0 && m.script < (int)runs_.size()) {   // its script (FUN_000481f2): "it died" holds
        runs_[m.script].flags |= 0x08;
        tryScript(m.script);
    }
    if (m.moving) { occupied_[m.ncz * kGrid + m.ncx]--; m.moving = false; }
    occupied_[m.cz * kGrid + m.cx]--;
    if (m.stationary && fixed_[m.cz * kGrid + m.cx]) fixed_[m.cz * kGrid + m.cx]--;
    m.stateTime = 0;
    m.attackFrame = -1;
    if (type == 2 && m.aiClass != 3 && !m.barrel) {
        // Blown apart by an explosion: the game's gib animation (FUN_000481f2) and its sounds,
        // then nothing.
        if (m.species == 1) { creatureSound(m, 46); creatureSound(m, 47); }
        else creatureSound(m, 33);
    }
    // The death sound (nanotech: 98, the creature stunned to death).
    creatureSound(m, type == 4 ? 98 : m.sndDeath);
    if (type == 2 && m.aiClass != 3 && !m.barrel) {
        const char* key = m.species == 1 ? "mechdth" : m.species == 2 || m.species == 3 ? "gbiodth" : "rbiodth";
        size_t before = effects_.size();
        spawnEffect(key, m.x, m.y, m.z, 1.0f, 1.0f, 1, m.species == 1, 0, 0);
        if (effects_.size() > before) effects_.back().grounded = true;
        m.state = Monster::Dead;
        m.gone = true;
        m.stateTime = 1;
        dropItem(m);
        return;
    }
    m.state = Monster::Dying;
}

void World::dropItem(const Monster& m) {
    if (!m.hasDrop) return;
    Item it;
    const Visual* v = itemVisual(m.dropSprite, it);
    if (!v || v->flat.empty()) return;   // (needs to be built at load; see World::load)
    it.vis = v;
    it.x = m.x;
    it.z = m.z;
    it.y = groundBelow(m.x, m.z, m.y + 8);
    it.light = cellLight(m.x, m.z);
    it.use = m.dropUse;
    it.amount = m.dropAmount;
    std::memcpy(it.params, m.dropParams, sizeof(it.params));
    it.sprite = lower(m.dropSprite);
    it.script = m.dropScript;
    nameItem(it, m.dropKind, m.dropInvPic, 0);
    items_.push_back(it);
}

void World::damagePlayer(float rawIn, const float* from, int type) {
    PlayerState& p = player_;
    if (!p.alive) return;
    int raw = (int)rawIn, d = raw;
    if (difficulty == 0) d -= d >> 2;   // easy: a quarter less
    if (d < 1) return;
    type = std::clamp(type, 0, 5);
    if (p.armor >= 1) {
        int a = (int)p.armor, absorb = 0;
        int s = std::clamp(p.suit, 0, 2);
        if (type == 0) absorb = a;
        else if (type == 4) absorb = 0;
        else absorb = a >> kSuitShift[s][type == 5 ? 3 : type - 1];
        d = std::max(0, d - absorb);
        int wear = raw * kWear[type] / 100;
        p.armor = wear < a ? (float)(a - wear) : 0.0f;
    }
    if (godMode) d = 0;
    p.health -= d;
    p.hurt = std::min(1.0f, p.hurt + 0.12f + d / 25.0f);
    p.tear = std::min(255.0f, p.tear + d);
    if (from) {
        p.hurtYaw = std::atan2(from[0] - p.x, -(from[2] - p.z));
        p.hurtShow = 1.0f;
    }
    shake = std::max(shake, std::min(8.0f, 1.5f + d * 0.3f));
    if (d > 47) message("HEAVY DAMAGE SUSTAINED.", 3);
    if (p.health < 1) {
        p.health = 0;
        p.alive = false;
        p.deadTime = 0;
        sound(125, 128);   // FUN_0004e120
    } else if (p.health < 25 && d > 0) {
        message("DAMAGE CRITICAL.", 3);
    }
}

bool World::useKit(int which) {
    PlayerState& p = player_;
    if (!p.alive) return false;
    if (which == 0) {
        if (p.bioKits <= 0) { message("NO BIO-KITS.", 0); return false; }
        if (p.health >= 104) { message("ALREADY AT FULL STRENGTH.", 0); return false; }
        p.bioKits--;
        p.health = std::min(104.0f, p.health + 48);
        p.bonus = 1;
        message("BIO-KIT USED.", 0);
        if (p.bioKits <= 0) kitStackEmpty(9);
        return true;
    }
    if (p.mechKits <= 0) { message("NO MECH-KITS.", 0); return false; }
    if (p.armor >= p.armorMax) { message("ARMOR ALREADY AT MAXIMUM.", 0); return false; }
    p.mechKits--;
    p.armor = std::min(p.armorMax, p.armor + 5);
    p.bonus = 1;
    message("MECH-KIT USED.", 0);
    if (p.mechKits <= 0) kitStackEmpty(10);
    return true;
}

// Splash and blasts: a 3x3 block of 64-unit probes round the point (a 192-unit square), from 32
// below to 64 above; each target once, at most nine, and only with line of sight.
void World::splashDamage(float x, float y, float z, int amount, int type, const Monster* skip, bool skipPlayer) {
    float c[3] = {x, y + 8, z};
    int victims = 0;
    for (auto& m : monsters_) {
        if (victims >= 9) break;
        if (m.get() == skip || m->state == Monster::Dying || m->state == Monster::Dead) continue;
        if (std::abs(m->x - x) > 96 || std::abs(m->z - z) > 96) continue;
        if (m->y + m->vis->height < y - 32 || m->y > y + 64) continue;
        float t[3] = {m->x, m->y + m->vis->height * 0.5f, m->z};
        if (!lineOfSight(c, t)) continue;
        float from[3] = {x, y, z};
        damageMonster(*m, (float)amount, from, type, false);
        ++victims;
    }
    const PlayerState& p = player_;
    if (!skipPlayer && p.alive && victims < 9 && std::abs(p.x - x) <= 96 && std::abs(p.z - z) <= 96 && p.y + 55 >= y - 32 &&
        p.y <= y + 64) {
        float t[3] = {p.x, p.viewY, p.z};
        if (lineOfSight(c, t)) {
            float from[3] = {x, y, z};
            damagePlayer((float)amount, from, type);
        }
    }
}

void World::areaBlast(const Monster& m) {
    // Barrels and gas (weapon kind 12): every target near takes its own roll, damage type 2.
    const MonsterWeapon* w = nullptr;
    for (const auto& k : m.weapon)
        if (k.kind == 12) { w = &k; break; }
    int lo = w ? w->dmgMin[0] : 40, hi = w ? w->dmgMax[0] : 80;
    float c[3] = {m.x, m.y + 16, m.z};
    int victims = 0;
    for (auto& o : monsters_) {
        if (victims >= 9) break;
        if (o.get() == &m || o->state == Monster::Dying || o->state == Monster::Dead) continue;
        if (std::abs(o->x - m.x) > 96 || std::abs(o->z - m.z) > 96) continue;
        if (o->y + o->vis->height < m.y - 32 || o->y > m.y + 64) continue;
        float t[3] = {o->x, o->y + o->vis->height * 0.5f, o->z};
        if (!lineOfSight(c, t)) continue;
        float from[3] = {m.x, m.y, m.z};
        damageMonster(*o, (float)irand(rng_, lo, hi), from, 2, false);
        ++victims;
    }
    const PlayerState& p = player_;
    if (p.alive && std::abs(p.x - m.x) <= 96 && std::abs(p.z - m.z) <= 96) {
        float t[3] = {p.x, p.viewY, p.z};
        if (lineOfSight(c, t)) {
            float from[3] = {m.x, m.y, m.z};
            damagePlayer((float)irand(rng_, lo, hi), from, 2);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Explosions (visuals) and chain reactions between barrel props

void World::explode(float x, float y, float z, float power, const char* key) {
    lights_.push_back({x, y, z, 380 * power, 420 * power, 0.45f, 0.25f, 0.08f, 0, 0.4f});
    makeNoise(x, y, z, 16 * kCellSize);
    if (std::string(key) == "barreld") {
        soundAt(46, x, z);   // the barrels' explosion (the creature barrels' death sound)
        // A barrel's own explosion stands on the floor where the barrel was.
        size_t before = effects_.size();
        spawnEffect(key, x, groundBelow(x, z, y + 4), z, 1.0f, 1.08f, 1, true, 0, 0);
        if (effects_.size() > before) effects_.back().grounded = true;
    } else if (*key) {
        spawnEffect(key, x, y, z, 1.0f, 1.15f, 1, true, 0, 0);
        for (int k = 0; k < 2; ++k)
            spawnEffect(key, x + frand(rng_, -14, 14), y + frand(rng_, -6, 12), z + frand(rng_, -14, 14), 0.5f, 0.65f, 2,
                        true, frand(rng_, 0.02f, 0.07f), 0);
    }
    // Flaming debris bits (the game's own expbit/expchk sprites), burning out where they land.
    for (int k = 0; k < 10; ++k) {
        Effect e;
        bool chunk = k & 1;
        e.vis = fx(chunk ? "expchk" : "expbit");
        if (!e.vis) break;
        float th = frand(rng_, 0, 2 * kPi), up = frand(rng_, 0.3f, 1.0f), sp = frand(rng_, 160, 380) * power;
        e.x = x; e.y = y; e.z = z;
        e.vx = std::cos(th) * sp * (1 - up * 0.5f);
        e.vy = up * sp;
        e.vz = std::sin(th) * sp * (1 - up * 0.5f);
        e.gravity = kGravity;
        e.drag = 0.4f;
        e.life = frand(rng_, 0.3f, 0.55f);
        e.mode = 1;
        e.bounce = true;
        e.then = chunk ? "expchkd" : "expbitd";
        effects_.push_back(e);
    }
    for (int k = 0; k < 2; ++k) {
        Effect e;
        e.vis = fx("smoke");
        if (!e.vis) break;
        e.x = x + frand(rng_, -10, 10); e.y = y + frand(rng_, 0, 10); e.z = z + frand(rng_, -10, 10);
        e.vx = frand(rng_, -10, 10); e.vy = frand(rng_, 20, 35); e.vz = frand(rng_, -10, 10);
        e.delay = frand(rng_, 0.1f, 0.16f);
        e.life = frand(rng_, 0.35f, 0.5f);
        e.scale0 = 1.2f; e.scale1 = 1.7f;
        e.alpha = 0.35f;
        e.mode = 1;
        e.fullbright = false;
        effects_.push_back(e);
    }
    for (int k = 0; k < 28; ++k) {
        Particle p;
        p.kind = k < 18 ? Particle::Spark : Particle::Ember;
        float th = frand(rng_, 0, 2 * kPi), ph = std::acos(frand(rng_, -0.5f, 1)), sp = frand(rng_, 180, 600) * power;
        p.vx = std::sin(ph) * std::cos(th) * sp; p.vy = std::cos(ph) * sp * 0.9f + 100; p.vz = std::sin(ph) * std::sin(th) * sp;
        p.x = x; p.y = y; p.z = z;
        p.life = p.kind == Particle::Spark ? frand(rng_, 0.3f, 0.8f) : frand(rng_, 0.8f, 1.6f);
        p.size0 = p.size1 = p.kind == Particle::Spark ? 0.8f : 1.3f;
        p.gravity = kGravity * (p.kind == Particle::Spark ? 0.9f : 0.5f);
        p.drag = p.kind == Particle::Spark ? 1.2f : 2.0f;
        addParticle(p);
    }
    // Barrel props caught in the blast go off in turn.
    for (auto& o : objects_) {
        if (!o.barrel || !o.intact || o.removed) continue;
        float dx = o.x - x, dz = o.z - z, dy = o.y + o.vis->height * 0.5f - y;
        if (std::abs(dx) > 96 || std::abs(dz) > 96 || std::abs(dy) > 64) continue;
        o.intact = false;
        float delay = frand(rng_, 0.08f, 0.18f);
        o.wreckTime = -delay;
        pending_.push_back({o.x, o.y + o.vis->height * 0.5f, o.z, 1.0f, delay, "barreld", true});
    }
    shake = std::max(shake, 6.0f * power);
}

void World::explosionAtAim(const Camera& cam) {
    float cy = std::cos(cam.yaw), sy = std::sin(cam.yaw), cp = std::cos(cam.pitch), sp = std::sin(cam.pitch);
    float fwd[3] = {sy * cp, sp, -cy * cp};
    float o[3] = {cam.x, cam.y, cam.z}, t, n[3];
    if (!raycast(o, fwd, 4000, t, n)) return;
    float x = cam.x + fwd[0] * (t - 8), y = cam.y + fwd[1] * (t - 8), z = cam.z + fwd[2] * (t - 8);
    explode(x, y, z, 1.0f, "rcktexp");
    soundAt(impactSound(2), x, z);
    splashDamage(x, y, z, 60, 2, nullptr, true);
}

// ---------------------------------------------------------------------------------------------
// Projectiles in flight

void World::updateShots(float dt) {
    for (size_t i = 0; i < shots_.size();) {
        Shot& s = shots_[i];
        s.age += dt;
        s.vy -= s.gravity * dt;
        float d[3] = {s.vx * dt, s.vy * dt, s.vz * dt};
        float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (s.tracer) {
            s.x += d[0]; s.y += d[1]; s.z += d[2];
            s.travelled += len;
            if (s.travelled >= s.range || s.age > 1) { shots_.erase(shots_.begin() + i); continue; }
            ++i;
            continue;
        }
        bool expired = s.fuse > 0 && s.age >= s.fuse;
        bool outside = s.x < 64 || s.z < 64 || s.x > 4032 || s.z > 4032 || s.y < -64 || s.y > 1100 || s.age > 6;
        if (len < 1e-4f && !expired && !outside) { ++i; continue; }
        float dir[3] = {0, 0, 0};
        if (len > 1e-4f) { dir[0] = d[0] / len; dir[1] = d[1] / len; dir[2] = d[2] / len; }
        float o[3] = {s.x, s.y, s.z};
        float tHit = len, nrm[3] = {0, 1, 0};
        bool wall = len > 1e-4f && raycast(o, dir, len + 5, tHit, nrm);
        if (wall) tHit = std::max(0.0f, tHit - 5);
        float hitT = wall ? tHit : len;
        Monster* hitM = nullptr;
        bool hitPlayer = false;
        for (auto& m : monsters_) {
            if (m->state == Monster::Dying || m->state == Monster::Dead || m->index == s.owner) continue;
            float t;
            if (len > 1e-4f && rayHitsMonster(*m, o, dir, hitT, t, 5.0f) && t < hitT) { hitT = t; hitM = m.get(); wall = false; }
        }
        // The player (a 40 x 55 box) for monster shots.
        if (s.owner >= 0 && player_.alive && len > 1e-4f) {
            const PlayerState& p = player_;
            float ox = o[0] - p.x, oz = o[2] - p.z, r = 20 + 5;
            float a = dir[0] * dir[0] + dir[2] * dir[2], b = 2 * (ox * dir[0] + oz * dir[2]), c = ox * ox + oz * oz - r * r;
            float disc = b * b - 4 * a * c;
            if (a > 1e-6f && disc >= 0) {
                float t = (-b - std::sqrt(disc)) / (2 * a);
                if (c <= 0) t = 0;
                float yy = o[1] + dir[1] * t;
                if (t >= 0 && t < hitT && yy >= p.y - 5 && yy <= p.y + 60) { hitT = t; hitPlayer = true; hitM = nullptr; wall = false; }
            }
        }
        bool barrelHit = false;
        Object* barrel = nullptr;
        for (auto& ob : objects_) {
            if (!ob.barrel || !ob.intact || ob.removed || len < 1e-4f) continue;
            float ox = o[0] - ob.x, oz = o[2] - ob.z, r = std::max(10.0f, ob.vis->radius);
            float a = dir[0] * dir[0] + dir[2] * dir[2], b = 2 * (ox * dir[0] + oz * dir[2]), c = ox * ox + oz * oz - r * r;
            float disc = b * b - 4 * a * c;
            if (a < 1e-6f || disc < 0) continue;
            float t = std::max(0.0f, (-b - std::sqrt(disc)) / (2 * a));
            float yy = o[1] + dir[1] * t;
            if (t < hitT && yy >= ob.y && yy <= ob.y + ob.vis->height) { hitT = t; barrelHit = true; barrel = &ob; hitM = nullptr; hitPlayer = false; wall = false; }
        }
        float p[3] = {o[0] + dir[0] * hitT, o[1] + dir[1] * hitT, o[2] + dir[2] * hitT};
        if (s.fuse > 0 && (wall || hitM || hitPlayer) && !expired) {
            // Grenades bounce: walls reflect and halve, floors reflect vy and halve (xz x 3/4),
            // creatures send it back at a quarter speed. The game remembers the last thing a
            // grenade touched; if that is a creature (or you) when the fuse runs out, it takes the
            // direct hit as well as the blast (the impact handler at 0x3354f).
            s.lastMonster = hitM ? hitM->index : -1;
            s.lastPlayer = hitPlayer;
            if (hitM || hitPlayer) { s.vx *= -0.25f; s.vy *= -0.25f; s.vz *= -0.25f; }
            else if (std::abs(nrm[1]) > 0.7f) { s.vy = -s.vy * 0.5f; s.vx *= 0.75f; s.vz *= 0.75f; }
            else {
                float vn = s.vx * nrm[0] + s.vy * nrm[1] + s.vz * nrm[2];
                s.vx -= 1.5f * vn * nrm[0];
                s.vy -= 1.5f * vn * nrm[1];
                s.vz -= 1.5f * vn * nrm[2];
            }
            s.x = p[0] + nrm[0] * 2; s.y = p[1] + nrm[1] * 2; s.z = p[2] + nrm[2] * 2;
            if (std::abs(s.vx) + std::abs(s.vy) + std::abs(s.vz) < 30) expired = true;   // came to rest
            ++s.bounces;
            if (!expired) { ++i; continue; }
        }
        if (expired && !hitM && !hitPlayer) {
            if (s.lastPlayer && player_.alive) hitPlayer = true;
            else if (s.lastMonster >= 0)
                for (auto& m : monsters_)
                    if (m->index == s.lastMonster && m->state != Monster::Dying && m->state != Monster::Dead) hitM = m.get();
        }
        if (wall || hitM || hitPlayer || barrelHit || expired || outside) {
            float ix = expired ? s.x : p[0] - dir[0] * 2, iy = expired ? s.y : p[1] - dir[1] * 2, iz = expired ? s.z : p[2] - dir[2] * 2;
            // Impact animation, light and damage. A hit on the player shows as damage, not as a
            // sprite in the player's face.
            soundAt(impactSound(s.type), ix, iz);
            bool big = s.splash > 0 || s.type == 2;
            if (hitPlayer) lights_.push_back({ix, iy, iz, 120, 120, s.lr + 0.1f, s.lg + 0.05f, s.lb, 0, 0.12f});
            else if (big) explode(ix, iy, iz, 1.0f, s.impact.c_str());
            else {
                spawnEffect(s.impact, ix, iy, iz, 0.9f, 1.05f, 2, true, 0, 0);
                lights_.push_back({ix, iy, iz, 120, 160, s.lr + 0.05f, s.lg + 0.05f, s.lb + 0.05f, 0, 0.15f});
            }
            int dmg = irand(rng_, s.dmgMin, s.dmgMax);
            float from[3] = {s.x - s.vx * 0.05f, s.y, s.z - s.vz * 0.05f};
            if (hitM) {
                spawnEffect("blood" + std::to_string(hitM->blood), ix, iy, iz, 1.0f, 1.0f, 1, hitM->blood == 2, 0, 0);
                damageMonster(*hitM, (float)dmg, from, s.type, s.owner < 0);
            } else if (hitPlayer) {
                damagePlayer((float)dmg, from, s.type);
                // Knock-back along the shot's heading.
                float hl = std::sqrt(s.vx * s.vx + s.vz * s.vz);
                if (hl > 1e-3f) {
                    float push = dmg * 0.5f;
                    float nx = player_.x + s.vx / hl * push, nz = player_.z + s.vz / hl * push;
                    if (playerFits(nx, nz, player_.y, nullptr)) { player_.x = nx; player_.z = nz; }
                }
            }
            if (barrelHit && barrel) {
                barrel->intact = false;
                barrel->wreckTime = 0;
                float by = barrel->y + barrel->vis->height * 0.5f;
                explode(barrel->x, by, barrel->z, 1.0f, "barreld");
                splashDamage(barrel->x, by, barrel->z, irand(rng_, 40, 80), 2, nullptr, false);
            }
            if (s.splash > 0) splashDamage(ix, iy, iz, s.splash, s.type, hitM, hitPlayer);
            shots_.erase(shots_.begin() + i);
            continue;
        }
        s.x += d[0]; s.y += d[1]; s.z += d[2];
        if (s.visual == "rktprj") {  // a thin exhaust trail behind missiles
            Particle t;
            t.kind = Particle::Trail;
            t.x = s.x - dir[0] * 12; t.y = s.y - dir[1] * 12; t.z = s.z - dir[2] * 12;
            t.vy = 10;
            t.life = 0.5f;
            t.size0 = 1.5f; t.size1 = 5;
            t.alpha = 0.35f;
            t.drag = 2;
            addParticle(t);
        }
        ++i;
    }
}

// ---------------------------------------------------------------------------------------------
// Pickups. The game takes an item when you point at it and press "use" (within two cells); here
// walking over it takes it as well.

void World::updatePickups() {
    PlayerState& p = player_;
    if (!p.alive) return;
    for (auto& it : items_) {
        if (it.taken || it.placed) continue;
        if (it.denied > 0) it.denied -= 1.0f / 60.0f;
        float dx = it.x - p.x, dz = it.z - p.z;
        if (it.dropped) {
            if (dx * dx + dz * dz > 64.0f * 64.0f) it.dropped = false;
            continue;
        }
        if (dx * dx + dz * dz > 48.0f * 48.0f || std::abs(it.y - p.y) > 48) continue;
        // Only with nothing in between: the game takes an item only by "use" on it, so never
        // through a wall or a shut door (BIOSTOR2's red card behind its locked hatch could be
        // taken by standing against the hatch, up to 0.5.2).
        // Level lines, at the item's height or at your feet, so floors and catwalks don't count.
        auto clear = [&](float y) {
            float from[3] = {p.x, y, p.z}, to[3] = {it.x, y, it.z};
            return lineOfSight(from, to);
        };
        if (!clear(it.y + 8) && !clear(p.y + 8)) continue;
        takeItem(it, false);
    }
}

// The item nearest the line of sight, within 15 degrees of it (more for items close by).
const World::Item* World::itemInSight(const Camera& cam) const {
    float fx = std::sin(cam.yaw) * std::cos(cam.pitch), fy = std::sin(cam.pitch), fz = -std::cos(cam.yaw) * std::cos(cam.pitch);
    const Item* best = nullptr;
    float bestAngle = 0.26f;
    for (const auto& it : items_) {
        if (it.taken || it.placed) continue;
        float dx = it.x - cam.x, dy = it.y + 8 - cam.y, dz = it.z - cam.z;
        float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d < 1 || d > 600) continue;
        float c = (dx * fx + dy * fy + dz * fz) / d;
        float a = std::acos(std::clamp(c, -1.0f, 1.0f));
        if (a > bestAngle + 0.4f * std::max(0.0f, 1.0f - d / 128.0f)) continue;   // close items are big targets
        float from[3] = {cam.x, cam.y, cam.z}, to[3] = {it.x, it.y + 8, it.z};
        if (!lineOfSight(from, to)) continue;
        best = &it;
        bestAngle = a;
    }
    return best;
}

// The door whose panel the crosshair is on (index, or -1), nothing in front of it.
int World::doorInSight(const Camera& cam, float* dist) const {
    float d[3] = {std::sin(cam.yaw) * std::cos(cam.pitch), std::sin(cam.pitch), -std::cos(cam.yaw) * std::cos(cam.pitch)};
    float o[3] = {cam.x, cam.y, cam.z}, t, n[3];
    int door = -1;
    if (!raycast(o, d, 6000, t, n, &door) || door < 0) return -1;
    for (const auto& m : monsters_) {
        if (m->state == Monster::Dying || m->state == Monster::Dead) continue;
        float tt;
        if (rayHitsMonsterThin(*m, o, d, t, tt)) return -1;
    }
    if (dist) *dist = t;
    return door;
}

// Whether a door gives way: unlocked, or the player holds its key (FUN_00042bda), or a level
// script could open it (scripts aren't run yet; a locked door that no key in the level and no
// script can open stays shut for good, as in the game). Creatures carry no keys.
bool World::doorOpens(const Door& d, bool player) const {
    if (!d.locked || d.scripted) return true;
    return player && (player_.keys & (1 << (d.spec.key & 7)));
}

// Gunballs and turrets: machines that hover (gunballs, flybots, ceiling guns and lasers) or never
// move (gunposts, chain guns); not barrels, gas mines or the walking robots.
void World::creatureSound(const Monster& m, int id) {
    bool gun = m.species == 1 && m.aiClass != 3 && !m.barrel && (m.hover > 0 || m.stationary);
    soundAt(id, m.x, m.z, gun ? 1280.0f : 640.0f);
}

// Use on a door toggles it, as FUN_000134cb does: a closed or closing door opens, an open or
// opening one closes.
void World::openDoor(Door& d) {
    bool moved = false;
    for (auto& o : doors_)
        if (o.group == d.group && !o.opening) { o.opening = true; o.openFor = 0; moved = true; }
    if (moved) soundAt(d.spec.sound, d.cx, d.cz);   // the door's own sound (FUN_000134cb)
}

// Anything standing in the doorway (the player or a creature) stops a door closing.
bool World::doorBlocked(const Door& d) const {
    float x0 = d.cellX * (float)kCellSize, z0 = d.cellZ * (float)kCellSize, x1 = x0 + kCellSize, z1 = z0 + kCellSize;
    // Only the strip the panel sweeps through, widened by a body's radius.
    if (d.ew) { z0 = d.spec.z + d.spec.offset - 24.0f; z1 = z0 + 48; }
    else { x0 = d.spec.x + d.spec.offset - 24.0f; x1 = x0 + 48; }
    const PlayerState& p = player_;
    if (p.alive && p.x > x0 - 4 && p.x < x1 + 4 && p.z > z0 && p.z < z1 && p.y < d.base + d.height && p.y + 55 > d.base) return true;
    for (const auto& m : monsters_) {
        if (m->state == Monster::Dying || m->state == Monster::Dead) continue;
        if ((m->cx == d.cellX && m->cz == d.cellZ) || (m->ncx == d.cellX && m->ncz == d.cellZ)) return true;
    }
    return false;
}

int World::cursorPick(const Camera& cam) const {
    if (!level_) return 0;
    float d[3] = {std::sin(cam.yaw) * std::cos(cam.pitch), std::sin(cam.pitch), -std::cos(cam.yaw) * std::cos(cam.pitch)};
    float o[3] = {cam.x, cam.y, cam.z}, t, n[3];
    float best = 6000;
    int door = -1, kind = 0;
    if (raycast(o, d, best, t, n, &door, true)) { best = t; kind = door >= 0 ? 3 : 0; }
    for (const auto& m : monsters_) {
        if (m->state == Monster::Dying || m->state == Monster::Dead) continue;
        float tt;
        if (rayHitsMonsterThin(*m, o, d, best, tt) && tt < best) { best = tt; kind = m->barrel ? 0 : 2; }
    }
    // Items: their upright sprite, facing you.
    for (const auto& it : items_) {
        if (it.taken || it.placed || !it.vis) continue;
        float fx = it.x - o[0], fz = it.z - o[2], fl = std::sqrt(fx * fx + fz * fz);
        if (fl < 1) continue;
        fx /= fl; fz /= fl;
        float denom = d[0] * fx + d[2] * fz;
        if (denom < 1e-3f) continue;
        float tt = fl / denom;
        if (tt >= best) continue;
        float px = o[0] + d[0] * tt, py = o[1] + d[1] * tt, pz = o[2] + d[2] * tt;
        float u = (px - it.x) * -fz + (pz - it.z) * fx, h = py - it.y;
        if (std::abs(u) > std::max(5.0f, it.vis->radius) || h < 0 || h > std::max(6.0f, it.vis->height)) continue;
        best = tt;
        kind = 1;
    }
    return kind;
}

std::string World::aimedItem(const Camera& cam) const {
    const PlayerState& p = player_;
    if (!p.alive || !level_) return "";
    float doorDist = 0;
    int di = doorInSight(cam, &doorDist);
    if (const Item* it = itemInSight(cam)) {
        float dx = it->x - cam.x, dy = it->y + 8 - cam.y, dz = it->z - cam.z;
        if (di >= 0 && dx * dx + dy * dy + dz * dz < doorDist * doorDist) di = -1;
    }
    if (di >= 0) {
        const Door& d = doors_[di];
        if (std::abs(d.cx - p.x) >= 128 || std::abs(d.cz - p.z) >= 128) return "";
        return d.opening ? "CLOSE DOOR" : "OPEN DOOR";
    }
    const Item* it = itemInSight(cam);
    if (!it || std::abs(it->x - p.x) >= 128 || std::abs(it->z - p.z) >= 128) return "";
    switch (it->use) {
    case 1: case 2: {
        int w = it->use == 1 ? it->params[0x19] : it->params[0x1d];
        return w >= 0 && w < 8 ? kWeapons[w].name : "WEAPON";
    }
    case 3: {
        int w = it->params[0];
        return w >= 0 && w < 8 ? std::string(kWeapons[w].name) + " AMMUNITION" : "AMMUNITION";
    }
    case 5: return it->sprite == "suit2" ? "SUIT" : "EQUIPMENT";
    case 8: return "ACCESS KEY";
    case 9: return "BIO-KIT";
    case 10: return "MECH-KIT";
    default: return "";
    }
}

// "Use" (FUN_0003fed1): on whatever the crosshair is on first, a door, a wall or floor, a
// creature, a prop or an item, within 128 units on each axis. A readied item is used on it first
// (its script learns what it was used on); pointing at nothing uses it on nothing.
bool World::useAt(const Camera& cam, bool keepInventory) {
    PlayerState& p = player_;
    if (!p.alive || !level_) return false;
    // The inventory open and nothing readied: "use" closes it (FUN_000404c8).
    if (invOpen_ && readied_ < 0 && !keepInventory) {
        toggleInventory();
        return true;
    }
    // A readied item that waits for nothing is used at once, and is no longer readied.
    if (readied_ >= 0 && readiedWaitsForNothing()) {
        useReadied(UseNothing, 0, 0, 0);
        readied_ = -1;
    }
    float cp = std::cos(cam.pitch);
    float o[3] = {cam.x, cam.y, cam.z};
    float dir[3] = {std::sin(cam.yaw) * cp, std::sin(cam.pitch), -std::cos(cam.yaw) * cp};
    float wallT = 1e9f, n[3] = {0, 0, 0};
    int doorHit = -1;
    bool hitLevel = raycast(o, dir, 4096, wallT, n, &doorHit, true);
    if (!hitLevel) wallT = 4096;
    enum { None, DoorT, WallT, CreatureT, PropT, ItemT } kind = None;
    float best = 1e9f;
    if (hitLevel) { kind = doorHit >= 0 ? DoorT : WallT; best = wallT; }
    float ct = 0, pt = 0;
    Monster* cm = creatureInSight(cam, best, &ct);
    if (cm && ct < best) { kind = CreatureT; best = ct; }
    if (kind == DoorT) cm = nullptr;
    Object* po = propInSight(cam, best, &pt);
    if (po && pt < best) { kind = PropT; best = pt; }
    Item* item = const_cast<Item*>(itemInSight(cam));
    if (item) {
        float dx = item->x - cam.x, dy = item->y + 8 - cam.y, dz = item->z - cam.z;
        float id = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (id < best || kind == None) { kind = ItemT; best = id; }
    }
    auto near = [&](float x, float z) { return std::abs(x - p.x) < 128 && std::abs(z - p.z) < 128; };
    if (logScripts()) SDL_Log("dbg use: %s at %.0f", kind == None ? "nothing" : kind == DoorT ? "a door" : kind == WallT ? "a wall" :
                              kind == CreatureT ? "a creature" : kind == PropT ? "a prop" : "an item", best);
    switch (kind) {
    case None:
        if (readied_ >= 0) {
            useReadied(UseNothing, 0, 0, 0);
            readied_ = -1;
        }
        return false;
    case DoorT: {
        Door& d = doors_[doorHit];
        if (!near(d.cx, d.cz)) {
            message("DOOR OUT OF RANGE.", 0);
            return false;
        }
        useReadied(UseDoor, d.cellX, d.cellZ, d.spec.key);
        entityScript(d.spec.script, 0x08);   // the door's script learns it was used
        if (!doorOpens(d, true)) {
            // Doors whose script says why (flag 0x08 of the u16 after the flags) don't add this.
            if (!(d.spec.flags2 & 0x08)) message("ACCESS DENIED.", 0);
            return false;
        }
        if (d.blown) return true;   // blown open for good
        if (d.opening) {
            for (auto& od : doors_)
                if (od.group == d.group) od.opening = false;
            soundAt(d.spec.sound, d.cx, d.cz);
        } else {
            openDoor(d);
        }
        return true;
    }
    case WallT: {
        float hx = o[0] + dir[0] * wallT, hz = o[2] + dir[2] * wallT;
        if (std::abs(n[1]) > 0.7f) {   // a floor (or a platform): its cell (FUN_0003fed1, case 2)
            int cx = (int)std::floor(hx / kCellSize), cz = (int)std::floor(hz / kCellSize);
            if (cx < 0 || cz < 0 || cx >= kGrid || cz >= kGrid || !near(cx * (float)kCellSize, cz * (float)kCellSize)) return false;
            useReadied(UseFloor, cx, cz, 0);
            auto& list = cellScripts_[cz * kGrid + cx];
            if (list.empty()) return false;
            runs_[list.front()].events |= 0x10;
            runCellScripts(cx, cz);
            return true;
        }
        return useWall(cam);
    }
    case CreatureT:
        if (!near(cm->x, cm->z)) return false;
        useCreature(*cm);
        return true;
    case PropT:
        if (!near(po->x, po->z)) {
            message("OBJECT OUT OF RANGE.", 0);
            return false;
        }
        useProp(*po);
        return true;
    case ItemT:
        if (!near(item->x, item->z)) {
            message("ITEM OUT OF RANGE.", 0);
            return false;
        }
        useReadied(UseItem, data_->hd.find(item->sprite), 0, 0);
        item->denied = 0;
        return takeItem(*item, true);
    }
    return false;
}

bool World::takeItem(Item& it, bool asked) {
    bool done = takeItemNow(it, asked);
    if (done || asked) entityScript(it.script, 0x04);   // the item's script learns it was taken
    return done;
}

bool World::takeItemNow(Item& it, bool asked) {
    PlayerState& p = player_;
    bool done = false;
    // FUN_000420c0: weapons play their "armed" sound, ammunition its pickup sound, the rest 81.
    auto take = [&](const std::string& text, int snd = kPickupSound) {
        it.taken = true;
        p.bonus = 1;
        message(text, 0);
        sound(snd);
        done = true;
    };
    switch (it.use) {
    case 1:
    case 2: {
        int w = it.use == 1 ? it.params[0x19] : it.params[0x1d];
        if (w < 0 || w > 7) break;
        bool had = p.owned[w];
        p.owned[w] = true;
        p.ammo[w] = std::min(kWeapons[w].maxAmmo, p.ammo[w] + it.amount);
        take(std::string(kWeapons[w].name) + (had ? " AMMUNITION ACQUIRED." : " ACQUIRED."), kArmedSound);
        break;
    }
    case 3: {
        int w = it.params[0];
        if (w < 0 || w > 7) break;
        if (p.ammo[w] >= kWeapons[w].maxAmmo) {
            if (it.denied <= 0) { message("ONBOARD SUPPLIES ALREADY AT MAXIMUM.", 0); it.denied = 3; }
            break;
        }
        int add = std::min(it.amount, kWeapons[w].maxAmmo - p.ammo[w]);
        if (it.amount <= 0) add = 0;
        p.ammo[w] += add;
        it.amount -= add;
        p.bonus = 1;
        message(std::string(kWeapons[w].name) + " AMMUNITION +" + std::to_string(add), 0);
        sound(kAmmoSound);
        if (it.amount <= 0) it.taken = true;
        done = true;
        break;
    }
    case 5:
        // The suit (an activated item): BIOLAB's has a script that puts it on (action 34) and takes
        // it out of the inventory; without one it is put on at once.
        if (it.sprite == "suit2" && it.script < 0) {
            p.suit = 1;
            p.armorMax = kSuitMax[1];
            p.armor = p.armorMax;
            take(std::string(kSuitName[1]) + " SUIT ACQUIRED.");
            break;
        }
        done = packItem(it);
        break;
    case 8:
        p.keys |= 1 << (it.params[0] & 7);
        take("ACCESS KEY ACQUIRED.");
        break;
    case 4: case 6: case 7: case 9: case 10:   // into the inventory (kits onto their stack)
        done = packItem(it);
        break;
    default:
        break;
    }
    return done;
}
