#include <array>
// Monster behaviour: the 1994 AI ported from CYCLONE.EXE (think FUN_0002aea7, chaser FUN_0002a17b,
// deployer FUN_0002a539, turret FUN_0002a9c6, queen FUN_0002ad53, heading FUN_0002ae02, attacks
// FUN_0003219b...), and an enhanced AI with sight, hearing, memory, pathfinding and teamwork.
// Both share the movement, the animation states and the weapons.
#include <queue>

#include "world_internal.h"

namespace {

int irandAI(std::mt19937& r, int lo, int hi) {
    if (hi < lo) std::swap(lo, hi);
    return lo + (int)(r() % (unsigned)(hi - lo + 1));
}

// Original headings in the game's 256-step angles: 0 east, 0x40 north, 0x80 west, 0xc0 south.
void headingToDir(int a, int& dx, int& dz) {
    static const int kDx[8] = {1, 1, 0, -1, -1, -1, 0, 1}, kDz[8] = {0, -1, -1, -1, 0, 1, 1, 1};
    int i = ((a + 16) & 255) / 32;
    dx = kDx[i];
    dz = kDz[i];
}

float yawOf(float dx, float dz) { return std::atan2(dx, -dz); }

}  // namespace

// ---------------------------------------------------------------------------------------------
// Shared: attacks

void World::startAttack(Monster& m, int slot) {
    const SpriteAnim& a = slot == 0 ? m.vis->attackA : m.vis->attackB;
    m.attackSlot = slot;
    m.stateTime = 0;
    m.attackFrame = 0;   // frame 0 shows without checking its fire flag, as in the game
    m.lastFire = true;
    if (a.empty()) {
        // No attack frames: fire once straight away.
        m.lastFire = monsterFire(m, slot);
        m.state = Monster::Walk;
        m.thinkTimer = enhancedAI ? 0.4f : 0.75f;
        return;
    }
    m.state = Monster::Attack;
}

// Deployers (class 2 with a ready move: gunballs, gunposts) open up before they fire and stay open
// while they keep firing; the game folds them back when it stops (FUN_0002a539, modes 6, 9, 7).
// The game only opens them for attack B; gunballs whose attack A shows the opened shell (BIOSTOR2,
// CORE, CLONE) popped open without the move, so here they open first as well.
void World::beginAttack(Monster& m, int slot) {
    if (m.moving) { occupied_[m.ncz * kGrid + m.ncx]--; m.moving = false; }
    if (m.aiClass == 2 && !m.vis->ready.empty() && !m.unfolded) {
        m.state = Monster::Ready;
        m.stateTime = 0;
        m.attackSlot = slot;
        m.readySlot = slot;
        return;
    }
    startAttack(m, slot);
}

void World::foldUp(Monster& m) {
    if (!m.unfolded) return;
    m.unfolded = false;
    m.state = Monster::Unready;   // back up, at double speed
    m.stateTime = 0;
}

// Folded (the original AI): the game walks on (mode 7 -> 1) along a fresh heading towards the
// player, and thinks again at the next cell centre, or 75 ticks later (FUN_0002a539). So a
// gunball whose shot went wide floats on a cell before it opens up again, and an Alien Worker
// whose blow went wide steps round.
void World::foldedWalk(Monster& m) {
    const PlayerState& p = player_;
    float dist = std::max(std::abs(p.x - m.x), std::abs(p.z - m.z));
    if (!p.alive || dist > m.sight * 64 + 20 || !m.active) return;   // the next think picks a heading
    int hx, hz;
    headingToDir(axisHeading(m), hx, hz);
    m.yaw = yawOf((float)hx, (float)hz);
    m.thinkTimer = 0.75f;
    if (!m.moving && !chooseStep(m, hx, hz)) blockedRetry(m);
}

bool World::monsterFire(Monster& m, int slot) {
    const MonsterWeapon& w = m.weapon[slot];
    if (traceMonster_ == m.index)
        SDL_Log("dbg fire %d slot %d kind %d acc %d/%d/%d/%d dmg %d-%d %d-%d %d-%d %d-%d accuracy %d yaw %.2f", m.index, slot, w.kind,
                w.acc[0], w.acc[1], w.acc[2], w.acc[3], w.dmgMin[0], w.dmgMax[0], w.dmgMin[1], w.dmgMax[1], w.dmgMin[2],
                w.dmgMax[2], w.dmgMin[3], w.dmgMax[3], m.accuracy, m.yaw);
    if (!w.kind || !player_.alive) return false;
    const PlayerState& p = player_;
    float eye[3] = {m.x, m.y + m.eye, m.z}, target[3] = {p.x, playerEye(), p.z};
    float dx = p.x - m.x, dz = p.z - m.z;
    float d = std::sqrt(dx * dx + dz * dz);
    if (w.kind == 12) { areaBlast(m); return true; }
    if (w.kind == 1) creatureSound(m, m.sndFire);   // hitscan: the shot sounds first (FUN_00032419)
    // Hitscan and melee need the player within 90 degrees of the facing, and in sight.
    if (w.kind == 1 || w.kind == 11) {
        // In the game's 256-step angles: |angle to the player - facing| < 64, so a player exactly
        // side-on (common with the axis headings) is out of reach.
        auto steps = [](float yaw) { return (int)std::lround((kPi / 2 - yaw) * 128.0f / kPi) & 255; };
        if (std::abs((int)(int8_t)(uint8_t)(steps(yawOf(dx, dz)) - steps(m.yaw))) >= 64) return false;
        // (A blow needs a clear way: walls see-through to sight still stop it, FUN_00032295.)
        if (!lineOfSight(eye, target, w.kind == 11)) {
            if (traceMonster_ == m.index)
                SDL_Log("dbg fire: no line of sight from %.0f,%.0f,%.0f to %.0f,%.0f,%.0f", eye[0], eye[1], eye[2], target[0], target[1], target[2]);
            return false;
        }
        int band;
        if (w.kind == 1) band = d <= 96 ? 0 : d <= 224 ? 1 : d <= 352 ? 2 : d <= 608 ? 3 : -1;
        else band = d <= 80 ? 0 : d <= 160 ? 1 : -1;
        if (band < 0) return false;
        int chance = m.accuracy + w.acc[band];
        if (enhancedAI) chance += difficulty == 0 ? -15 : difficulty == 2 ? 10 : 0;
        lights_.push_back({eye[0] + dx / std::max(1.0f, d) * 16, eye[1], eye[2] + dz / std::max(1.0f, d) * 16, 140, 110,
                           0.3f, 0.25f, 0.1f, 0, 0.06f});
        bool hit = chance < 0 || (int)(rng_() % 100) < chance;   // the game compares unsigned
        if (!hit) {
            if (w.kind == 1) {   // a miss: a puff near the player shows where it went
                float a = frand(rng_, 0, 2 * kPi), r = frand(rng_, 18, 40);
                spawnEffect("puff", p.x + std::cos(a) * r, playerEye() - frand(rng_, 10, 40), p.z + std::sin(a) * r, 0.6f, 0.8f, 1,
                            true, 0, 0);
            }
            return true;    // a miss keeps the monster shooting
        }
        // Melee: a blow that lands sounds (even one whose band does no damage, FUN_00032295).
        if (w.kind == 11) creatureSound(m, m.sndMelee);
        if (w.dmgMax[band] == 0) return false;
        damagePlayer((float)irandAI(rng_, w.dmgMin[band], w.dmgMax[band]), eye, w.type);
        return true;
    }
    if (w.kind == 2) {
        bool los = lineOfSight(eye, target);
        // Aim: the player's eye now (the game), or where the player will be (enhanced).
        float tx = target[0], ty = target[1], tz = target[2];
        if (enhancedAI && w.speed > 1 && difficulty > 0) {
            // Lead a moving target: part of the way to where the player would be when the shot
            // arrives (half on normal, most of it on hard), never more than a cell and a half,
            // and only if that spot can be seen; otherwise straight at the player.
            float lead = difficulty == 1 ? 0.5f : 0.8f;
            float vx = playerVelX_, vz = playerVelZ_;
            float t = d / w.speed;
            for (int k = 0; k < 2; ++k) {   // refine the time to intercept
                float ix = p.x + vx * t * lead - eye[0], iz = p.z + vz * t * lead - eye[2];
                t = std::sqrt(ix * ix + iz * iz) / w.speed;
            }
            float ox = vx * t * lead, oz = vz * t * lead, ol = std::sqrt(ox * ox + oz * oz);
            if (ol > 96) { ox *= 96 / ol; oz *= 96 / ol; }
            float aimAt[3] = {p.x + ox, ty, p.z + oz};
            if (lineOfSight(eye, aimAt)) { tx = aimAt[0]; tz = aimAt[2]; }
            if (traceMonster_ == m.index)
                SDL_Log("dbg lead: player v %.0f,%.0f  lead offset %.0f,%.0f (%.0f units) visible %d", vx, vz, ox, oz,
                        std::min(ol, 96.0f), tx == aimAt[0]);
        }
        Shot s;
        s.x = eye[0];
        s.y = eye[1];
        s.z = eye[2];
        float ax = tx - s.x, ay = ty - s.y, az = tz - s.z;
        float hd = std::sqrt(ax * ax + az * az);
        float yaw = std::atan2(ax, -az) + ((ticks_ & 2) ? ((ticks_ & 1) ? 1 : -1) * (2 * kPi / 256) : 0.0f);
        float hs = w.speed;
        s.vx = std::sin(yaw) * hs;
        s.vz = -std::cos(yaw) * hs;
        // The game aims vertically with the uncapped speed, so very fast shots aim a little high.
        float aim = w.aimSpeed > 0 ? w.aimSpeed : hs;
        s.vy = hd > 1 ? ay / (hd / aim) * (hs / aim) * (aim / hs) : 0;
        s.owner = m.index;
        s.visual = w.visual;
        s.impact = w.impact;
        s.dmgMin = w.dmgMin[0];
        s.dmgMax = w.dmgMax[0];
        s.splash = w.splash;
        s.type = w.type;
        s.lr = w.lr; s.lg = w.lg; s.lb = w.lb;
        if (w.lob) { s.gravity = 312.5f; s.fuse = w.fuse > 0 ? w.fuse : 3.0f; }
        else if (w.fuse > 0) s.fuse = w.fuse;
        shots_.push_back(s);
        creatureSound(m, m.sndFire);
        return los;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// Shared: walking from cell centre to cell centre

// Starts a step to the neighbouring cell (dx, dz) if a creature of this size can make it.
bool World::chooseStep(Monster& m, int dx, int dz) {
    if (!dx && !dz) return false;
    int nx = m.cx + dx, nz = m.cz + dz;
    if (nx < 0 || nz < 0 || nx >= kGrid || nz >= kGrid || occupied_[nz * kGrid + nx]) return false;
    // Don't walk into the player.
    int px = (int)std::floor(player_.x / kCellSize), pz = (int)std::floor(player_.z / kCellSize);
    if (nx == px && nz == pz && player_.alive) return false;
    for (auto& d : doors_)
        if (d.cellX == nx && d.cellZ == nz && d.open < 0.95f) {
            // Creatures (species 0) open the doors they bump into; machines and aliens can't.
            if (m.species == 0 && doorOpens(d, false)) openDoor(d);
            return false;
        }
    float ny, duck = -1e9f;
    if (m.hover > 0 && !m.stationary) {
        if (!flierStep(m, nx, nz, ny, duck)) return false;
    } else if (!stepTarget(m.cx, m.cz, nx, nz, m.ground, m.vis->height, ny)) {
        return false;
    }
    m.duck = duck;
    occupied_[nz * kGrid + nx]++;
    m.ncx = nx;
    m.ncz = nz;
    m.tx = (nx + 0.5f) * kCellSize;
    m.tz = (nz + 0.5f) * kCellSize;
    m.ty = ny;
    m.moving = true;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Fliers
//
// The game moves a flier as it moves any creature: each walk frame (FUN_00047abe) picks a new place,
// `tics` x speed / 32 along, and a new height, `tics` units toward the one it makes for (1 unit a
// tick: 100 a second); the step stands only if the creature's collision box there (FUN_00047ed3, on
// what FUN_00012e13 finds in the rows of cells around it) meets no wall, door, block or floor
// surface more than 15 units over its feet (a catwalk is two such surfaces, its deck and its
// underside, and the ceiling is one too). It is set on one it meets lower down, 2 units over it.
// A step that fails is undone, height and all. So fliers cross drops and fly over railings, but
// neither rise nor sink through a catwalk, and don't fly into one at their height; one placed with
// its box through a surface never gets out (SDI2's four Flybots under the catwalks at 250: their
// box reaches 260 and they never move, as the running game shows). Up to 0.5.2 the engine walked
// fliers along the floors like other creatures and only lifted the drawing, easing it to its
// height every frame, which took them up and down through catwalks, floors and ceilings.

// The height of a creature's collision box (type +0x15: FUN_0004e0d2 makes the box from type
// +0x14..0x16, wide, high, deep), or the drawing's for a type without one.
float World::boxHeight(const Monster& m) const {
    return m.type && m.type->stats[6] ? (float)m.type->stats[6] : m.vis->height;
}

// The middle and the corners of a creature's box (kept inside its cell when it stands in the
// middle of one), and while it moves the middle of the cell ahead.
int World::boxPoints(const Monster& m, float* px, float* pz) const {
    auto half = [&](int k) { return std::min(31.0f, (m.type && m.type->stats[k] ? m.type->stats[k] : m.vis->radius * 2) * 0.5f); };
    float hw = half(5), hd = half(7);
    int n = 0;
    px[n] = m.x; pz[n++] = m.z;
    for (int k = 0; k < 4; ++k) { px[n] = m.x + ((k & 1) ? hw : -hw); pz[n++] = m.z + ((k & 2) ? hd : -hd); }
    if (m.moving) { px[n] = m.tx; pz[n++] = m.tz; }
    return n;
}

// A creature's box with its feet at y, over the points (px, pz): `floor` gets the highest surface
// up to 15 units over its feet (one its box meets it is set on, 2 units over it), `room` the
// lowest surface higher up. False if that one is within the box.
bool World::flierRoom(const Monster& m, const float* px, const float* pz, int n, float y, float& floor, float& room) const {
    floor = -1e9f;
    room = 1e9f;
    for (int i = 0; i < n; ++i) {
        float ce;
        floor = std::max(floor, groundBelow(px[i], pz[i], y + 15, &ce));
        room = std::min(room, ce);
    }
    return room > y + boxHeight(m);
}

// The game's test of a flier's box with its feet at y (FUN_00047ed3): false if a surface more than
// 15 units over its feet is within the box; otherwise the box is set on the highest surface it
// meets lower down, 2 units over it (y changes). `room` gets the lowest surface over 15 units up.
bool World::flierFits(const Monster& m, const float* px, const float* pz, int n, float& y, float* room) const {
    float fl, rm;
    bool ok = flierRoom(m, px, pz, n, y, fl, rm);
    if (room) *room = rm;
    if (!ok) return false;
    if (fl >= y - 1) {   // a surface the box meets: onto it (if that leaves room over it)
        if (rm <= fl + 2 + boxHeight(m)) return false;
        y = std::max(y, fl + 2);
    }
    return true;
}

// The height a flier makes for (FUN_00047abe): within 192 units of the player on both axes (a
// square), the player's eye height less half its box; otherwise its hover height over the
// surface under it, found at 17 units over its feet (so one that has climbed over a catwalk
// cruises over it), plus 2.
float World::flierTarget(const Monster& m) const {
    if (std::max(std::abs(player_.x - m.x), std::abs(player_.z - m.z)) < 192.0f) return playerEye() - boxHeight(m) * 0.5f;
    return groundBelow(m.x, m.z, m.y + 17) + 2 + m.hover;
}

// A walk frame's length in ticks (the type's average): how far a flier climbs or sinks in one.
float World::walkFrameTics(const Monster& m) const {
    if (!m.type || m.type->walk.tics.empty()) return 5;
    int sum = 0;
    for (uint8_t t : m.type->walk.tics) sum += t;
    return std::max(1.0f, (float)sum / m.type->walk.tics.size());
}

// A flier's step to a neighbouring cell (diagonally, both cells beside it must let it by too),
// judged as the game judges its first walk frame: at the height that frame takes it to (its walk
// tics toward the height it makes for). newY: the floor it will be over, which its routes follow
// from there. The game's AI stops at a catwalk or lintel that would cut through it; the enhanced
// AI's fliers sink under it first if there is room (duck: that height; -1e9 when there is no need).
bool World::flierStep(const Monster& m, int nx, int nz, float& newY, float& duck) const {
    const Level& L = *level_;
    duck = -1e9f;
    if (nx < 0 || nz < 0 || nx >= L.width || nz >= L.height) return false;
    if (!hasGround((nx + 0.5f) * kCellSize, (nz + 0.5f) * kCellSize)) return false;   // no room at all
    const float h = boxHeight(m);
    const bool diag = nx != m.cx && nz != m.cz;
    float px[3], pz[3];
    int n = 0;
    auto add = [&](int x, int z) { px[n] = (x + 0.5f) * kCellSize; pz[n++] = (z + 0.5f) * kCellSize; };
    add(nx, nz);
    if (diag) { add(nx, m.cz); add(m.cx, nz); }
    // Fits at y (set on what it meets)? `room` gets the lowest thing in the way (a surface, or the
    // foot of a wall it could pass under; -1e9: a wall it can't).
    auto fits = [&](float& y, float& room) {
        bool ok = flierFits(m, px, pz, n, y, &room);
        auto edge = [&](int ax, int az, int bx, int bz) {
            float b = -1e9f;
            if (!edgeBlocks(ax, az, bx, bz, y + 16, y + h, &b)) return;
            ok = false;
            room = std::min(room, b > y + 16 ? b : -1e9f);
        };
        if (diag) { edge(m.cx, m.cz, nx, m.cz); edge(nx, m.cz, nx, nz); edge(m.cx, m.cz, m.cx, nz); edge(m.cx, nz, nx, nz); }
        else edge(m.cx, m.cz, nx, nz);
        return ok;
    };
    const float frame = walkFrameTics(m);
    float y = m.y + std::clamp(flierTarget(m) - m.y, -frame, frame);
    float room;
    if (!fits(y, room)) {
        if (!enhancedAI || room < -1e8f) return false;
        const float d = std::min(m.y, room - h - 1);
        float d2 = d, room2;
        if (!fits(d2, room2) || d2 > d + 0.5f) return false;   // no room under it there
        float qx[6], qz[6], dh = d;   // nor where it is
        int q = boxPoints(m, qx, qz);
        if (!flierFits(m, qx, qz, q, dh) || dh > d + 0.5f) return false;
        duck = y = d;
    }
    newY = groundBelow(px[0], pz[0], y + 17);
    return true;
}

// Moves toward the target cell centre; reports whether it walked and how far. The drawn height
// eases toward the floor underneath, also while the creature stands still; fliers climb and sink
// as they go (see above).
void World::monsterStep(Monster& m, float dt, bool& walking, float& moved) {
    walking = false;
    moved = 0;
    float go = 0;
    if (m.moving && m.duck > -1e8f && m.y > m.duck && m.hover > 0 && !m.stationary) {
        // Sinking under what is ahead before it sets off (100 units a second, as fliers climb).
        m.y = std::max(m.duck, m.y - 100 * dt);
        walking = true;
        return;
    }
    if (m.moving) {
        float dx = m.tx - m.x, dz = m.tz - m.z;
        float dist = std::sqrt(dx * dx + dz * dz);
        float want = std::atan2(dx, -dz);
        float d = angleDiff(want, m.yaw);
        // The original snaps its facing to the heading; the enhanced AI turns quickly but smoothly.
        if (enhancedAI) m.yaw += std::clamp(d, -8.0f * dt, 8.0f * dt);
        else m.yaw = want;
        go = (enhancedAI ? std::max(0.0f, std::cos(d)) : 1.0f) * m.speed * dt;
        if (dist <= go || dist < 1.0f) {
            m.x = m.tx;
            m.z = m.tz;
            m.moving = false;
            occupied_[m.cz * kGrid + m.cx]--;
            m.cx = m.ncx;
            m.cz = m.ncz;
            m.ground = m.ty;
            moved = dist;
            m.thinkTimer = 0;   // reached a cell centre: think again now (the game's +0x76 flag)
        } else {
            m.x += dx / dist * go;
            m.z += dz / dist * go;
            moved = go;
        }
    }
    if (m.stationary && m.hover > 0) return;   // hung from the ceiling: it stays at its height (see World::load)
    walking = go > 0.001f;
    if (m.hover > 0) {
        // Fliers climb and sink only while they move, at 1 unit a tick (see above), and only as
        // far as their box has room: they settle on a surface they sink onto and stop under one
        // they rise to.
        if (!walking && moved <= 0) return;
        const float h = boxHeight(m);
        float ny = m.y + std::clamp(flierTarget(m) - m.y, -100 * dt, 100 * dt), y = ny, room;
        float px[6], pz[6];
        int n = boxPoints(m, px, pz);
        if (flierFits(m, px, pz, n, y, &room)) m.y = y;
        else if (ny > m.y) m.y = std::max(m.y, std::min(ny, room - h - 1));   // up against what is over it
        else if (ny < m.y) m.y = ny;   // already into something (a step cut short by a save): out downward
        if (!m.moving) m.duck = -1e9f;
        return;
    }
    float g = groundBelow(m.x, m.z, std::max(m.ground, m.ty) + 24);
    m.y += (g - m.y) * std::min(1.0f, dt * 10);
}

// Random strolling (monsters not fighting, and the original AI out of sight).
void World::wanderMonster(Monster& m, float dt) {
    if (m.moving || m.stationary) return;
    m.wait -= dt;
    if (m.wait > 0) return;
    float fx = std::sin(m.yaw), fz = -std::cos(m.yaw);
    int bestX = 0, bestZ = 0;
    float bestScore = -1e9f;
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx) {
            if (!dx && !dz) continue;
            float l = std::sqrt((float)(dx * dx + dz * dz));
            float score = (dx * fx + dz * fz) / l + frand(rng_, -0.9f, 0.9f);
            if (score > bestScore) { bestScore = score; bestX = dx; bestZ = dz; }
        }
    if (!chooseStep(m, bestX, bestZ)) {
        // try the others in random order
        for (int k = 0; k < 8 && !m.moving; ++k) {
            int a = (int)(rng_() & 0xe0);
            int dx, dz;
            headingToDir(a, dx, dz);
            chooseStep(m, dx, dz);
        }
    }
    if (!m.moving) m.wait = frand(rng_, 0.8f, 2.5f);
    else m.wait = (rng_() % 10) < 6 ? 0.0f : frand(rng_, 0.5f, 2.0f);
}

// ---------------------------------------------------------------------------------------------
// The 1994 AI

// The heading toward the player's cell, one axis at a time (FUN_0002ae02): along the row when the
// player is in the creature's row, otherwise a random axis; with the game's parity quirk (an odd
// coordinate turns it north or west instead).
int World::axisHeading(const Monster& m) {
    const PlayerState& p = player_;
    int pcx = (int)std::floor(p.x / kCellSize), pcz = (int)std::floor(p.z / kCellSize);
    bool xAxis = pcz == m.cz || (rng_() & 1) == 0;
    int mx = (int)m.x, mz = (int)m.z;
    if (xAxis) return m.cx < pcx ? ((mz & 1) ? 0x40 : 0x00) : ((mz & 1) ? 0x40 : 0x80);
    return m.cz < pcz ? ((mx & 1) ? 0x80 : 0xc0) : ((mx & 1) ? 0x80 : 0x40);
}

void World::updateOriginalAI(Monster& m, float dt) {
    if (!m.active) return;   // outside the activity bubble: frozen, as in the game
    // Added (not in the game): a creature that has the player in sight for the first time (within
    // its sight range, in line of sight) thinks at once instead of at its next think, up to 75
    // ticks later, so it turns to face the player as they come into the room rather than being
    // caught facing away. Out of its sight range it forgets, and notices again next time.
    if (m.state == Monster::Walk && player_.alive && m.aiClass != 3) {
        float ddx = player_.x - m.x, ddz = player_.z - m.z;
        if (std::max(std::abs(ddx), std::abs(ddz)) > m.sight * 64 + 20) {
            m.alerted = false;
        } else if (!m.alerted && (m.perceiveTimer -= dt) <= 0) {
            m.perceiveTimer = 0.1f;
            float eye[3] = {m.x, m.y + m.eye, m.z}, target[3] = {player_.x, playerEye(), player_.z};
            if (lineOfSight(eye, target)) {
                m.alerted = true;
                m.thinkTimer = 0;
            }
        }
    }
    m.thinkTimer -= dt;
    if (m.state != Monster::Walk || m.thinkTimer > 0) return;
    m.thinkTimer = 0.75f;    // 75 ticks to the next think
    bool walkOnly = m.walkOnly;
    m.walkOnly = false;
    const PlayerState& p = player_;
    float dx = p.x - m.x, dz = p.z - m.z;
    float dist = std::max(std::abs(dx), std::abs(dz));   // Chebyshev, as the game measures
    float dy = playerEye() - (m.y + m.eye);
    bool melee = m.weapon[0].melee() || m.weapon[1].melee();
    bool ranged = m.weapon[0].ranged() || m.weapon[1].ranged();
    float eye[3] = {m.x, m.y + m.eye, m.z}, target[3] = {p.x, playerEye(), p.z};
    auto attack = [&](int slot) { beginAttack(m, slot); };

    if (m.aiClass == 4) {   // turret: exact angle, ranged only, band B
        if (!p.alive) return;
        m.yaw = yawOf(dx, dz);
        if (dist <= m.bandB * 64 + 20 && ranged && lineOfSight(eye, target)) attack(m.weapon[0].ranged() ? 0 : 1);
        return;
    }
    if (m.aiClass == 6) {   // the Queen: attacks on odd ticks while the player is in sight range
        if (!p.alive || dist > m.sight * 64 + 20) return;
        m.yaw = yawOf(dx, dz);
        if (ticks_ & 1) attack(0);
        return;
    }
    if (m.aiClass != 1 && m.aiClass != 2) return;
    if (!p.alive || dist > m.sight * 64 + 20) {
        if (m.unfolded) { foldUp(m); return; }
        // Out of sight range: a random 8-way heading (FUN_0002ae76), and keep walking.
        int dx8, dz8;
        headingToDir((int)(rng_() & 0xe0), dx8, dz8);
        m.yaw = yawOf((float)dx8, (float)dz8);
        if (!m.moving && !m.stationary && !chooseStep(m, dx8, dz8)) blockedRetry(m);
        return;
    }
    // In range: the creature turns to the axis heading first, and keeps that facing while it
    // attacks (the game sets +0x1a from the heading after every think, FUN_0002aea7). So it may
    // stand side-on to the player: its projectiles still fly straight at the player, but a shot or
    // a blow needs the player within 90 degrees of where it faces.
    int heading = axisHeading(m);
    int hx, hz;
    headingToDir(heading, hx, hz);
    m.yaw = yawOf((float)hx, (float)hz);
    int band = (dist < 97 && (m.aiClass == 2 || std::abs(dy) < 49)) ? 1
             : (dist > m.bandA * 64 + 20 && dist > m.bandB * 64 + 20) ? 5 : 3;
    if (walkOnly) {
        // An attack has just ended without a shot: it gives up and walks (state 8/9 -> 1).
    } else if (band <= 1 && melee) {
        if (dist <= 96 && std::abs(dy) <= 64 && lineOfSight(eye, target)) {
            int slot = m.weapon[0].melee() ? ((m.weapon[1].melee() && (rng_() & 1)) ? 1 : 0) : 1;
            attack(slot);
            return;
        }
        m.lastFire = false;
    } else if (band <= 3 && ranged) {
        if (lineOfSight(eye, target)) {
            attack(m.weapon[0].ranged() ? 0 : 1);
            return;
        }
        m.lastFire = false;
    }
    if (m.unfolded) { foldUp(m); return; }   // not firing: fold up before moving on
    if (m.moving || m.stationary) return;
    if (!chooseStep(m, hx, hz)) blockedRetry(m);
}

// Blocked: the game's creature walks on the spot, bumps at its next walk frame and thinks again
// straight after (FUN_00047abe sets +0x76), so it tries a new heading every walk frame.
void World::blockedRetry(Monster& m) {
    const std::vector<float>& secs = !m.vis->rot.empty() ? m.vis->rot.seconds : m.vis->flat.seconds;
    float frame = secs.empty() ? 0.09f : secs[0];
    m.thinkTimer = std::min(m.thinkTimer, frame + 0.01f);
}

// A chaser's attack has ended (FUN_0002a17b, states 8 and 9 with the animation done). If its last
// shot or blow counted (+0x7e) it goes again at once, by range and by what its two attacks are,
// with no new line-of-sight check (a ranged attacker keeps firing even beyond its weapon bands);
// otherwise it walks, and thinks again 75 ticks later.
void World::originalAttackEnded(Monster& m) {
    const PlayerState& p = player_;
    float dx = p.x - m.x, dz = p.z - m.z;
    float dist = std::max(std::abs(dx), std::abs(dz));
    float dy = playerEye() - (m.y + m.eye);
    m.state = Monster::Walk;
    m.thinkTimer = 0.75f;
    auto walk = [&](int heading) {
        int hx, hz;
        headingToDir(heading, hx, hz);
        m.yaw = yawOf((float)hx, (float)hz);
        if (!m.moving && !m.stationary && !chooseStep(m, hx, hz)) blockedRetry(m);
    };
    if (!p.alive || dist > m.sight * 64 + 20) { walk((int)(rng_() & 0xe0)); return; }
    int heading = axisHeading(m);
    int band = (dist < 97 && std::abs(dy) < 49) ? 1 : (dist > m.bandA * 64 + 20 && dist > m.bandB * 64 + 20) ? 5 : 3;
    if (!m.lastFire) { walk(heading); return; }
    int hx, hz;
    headingToDir(heading, hx, hz);
    m.yaw = yawOf((float)hx, (float)hz);
    bool aMelee = m.weapon[0].melee(), bMelee = m.weapon[1].melee(), bRanged = m.weapon[1].ranged(), aRanged = m.weapon[0].ranged();
    int next = -1;
    if (m.attackSlot == 0) {
        if (!aMelee) next = band > 1 ? 0 : bMelee ? 1 : -1;
        else if (band < 3) next = (bMelee && (rng_() & 1)) ? 1 : 0;
        else if (band < 4 && bRanged) next = 1;
    } else {
        if (!bMelee) next = band > 1 ? 1 : aMelee ? 0 : -1;
        else if (band < 3) next = (aMelee && (rng_() & 1)) ? 0 : 1;
        else if (band < 4 && aRanged) next = 0;
    }
    if (next < 0) { walk(heading); return; }
    startAttack(m, next);
}

// ---------------------------------------------------------------------------------------------
// Enhanced AI

bool World::findPath(const Monster& m, int gx, int gz, float gy, std::vector<int>& out, bool* partial) const {
    // A* over the walk map's floors (several per cell where floors are stacked); closed doors only
    // for creatures that open them. With no way through, the route leads as close as it can get.
    out.clear();
    if (partial) *partial = false;
    if (gx < 0 || gz < 0 || gx >= kGrid || gz >= kGrid) return false;
    auto navIt = nav_.find(navHeight(m.vis->height));
    if (navIt == nav_.end()) return false;
    const std::vector<uint32_t>& nav = navIt->second;
    int startCell = m.cz * kGrid + m.cx, goalCell = gz * kGrid + gx;
    if (startCell == goalCell) return true;
    int sl = navLevel(startCell, m.ground), gl = navLevel(goalCell, gy);
    if (sl < 0) return false;
    int start = startCell * kNavLevels + sl;
    const int nodes = kGrid * kGrid * kNavLevels;
    // Scratch arrays live across calls; a node's entries count only when its stamp is this search's.
    static thread_local std::vector<float> g;
    static thread_local std::vector<int> from;
    static thread_local std::vector<uint32_t> seen;   // stamp * 2 (+1 when closed)
    static thread_local uint32_t stamp = 0;
    if ((int)seen.size() != nodes) { g.assign(nodes, 0); from.assign(nodes, -1); seen.assign(nodes, 0); stamp = 0; }
    if (++stamp >= 0x7fffffffu) { std::fill(seen.begin(), seen.end(), 0u); stamp = 1; }
    const uint32_t open1 = stamp * 2, closed1 = stamp * 2 + 1;
    auto known = [&](int n) { return seen[n] == open1 || seen[n] == closed1; };
    auto isClosed = [&](int n) { return seen[n] == closed1; };
    using Node = std::pair<float, int>;
    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> open;
    auto h = [&](int node) {
        int c = node / kNavLevels, x = c % kGrid, z = c / kGrid;
        float ddx = (float)std::abs(x - gx), ddz = (float)std::abs(z - gz);
        return std::max(ddx, ddz) + 0.41f * std::min(ddx, ddz);
    };
    g[start] = 0;
    from[start] = -1;
    seen[start] = open1;
    open.push({h(start), start});
    int expanded = 0, reached = -1, nearest = start;
    float nearestH = h(start);
    static const int kDx[8] = {1, -1, 0, 0, 1, 1, -1, -1}, kDz[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    while (!open.empty() && expanded < 2500) {
        int node = open.top().second;
        open.pop();
        if (isClosed(node)) continue;
        seen[node] = closed1;
        ++expanded;
        int c = node / kNavLevels;
        if (c == goalCell && (gl < 0 || node % kNavLevels == gl)) { reached = node; break; }
        if (h(node) < nearestH) { nearestH = h(node); nearest = node; }
        int x = c % kGrid, z = c / kGrid;
        uint32_t links = nav[node];
        for (int k = 0; k < 8; ++k) {
            if (!(links & (1u << k))) continue;
            int nc = (z + kDz[k]) * kGrid + x + kDx[k];
            int n = nc * kNavLevels + (int)((links >> (8 + 2 * k)) & 3);
            if (isClosed(n)) continue;
            float cost = k < 4 ? 1.0f : 1.41f;
            if (doorCell_[nc]) {
                const Door& d = doors_[doorCell_[nc] - 1];
                if (d.open < 0.95f) {
                    if (m.species != 0 || !doorOpens(d, false)) continue;   // can't open doors, or locked
                    cost += 1.0f;
                }
            }
            if (nc != goalCell && fixed_[nc]) continue;   // a prop or a creature that never moves
            if (occupied_[nc] && nc != goalCell) cost += 2.5f;
            float ng = g[node] + cost;
            if (!known(n) || ng < g[n]) {
                g[n] = ng;
                from[n] = node;
                seen[n] = open1;
                open.push({ng + h(n), n});
            }
        }
    }
    if (traceMonster_ == m.index)
        SDL_Log("dbg findPath %d,%d(l%d y%.0f) -> %d,%d(l%d y%.0f): expanded %d reached %d", m.cx, m.cz, sl, m.ground, gx, gz, gl, gy,
                expanded, reached);
    if (reached < 0) {
        if (partial) *partial = true;
        if (nearest == start) return false;
        reached = nearest;   // as near as it can get
    }
    for (int node = reached; node != start; node = from[node]) out.push_back(node / kNavLevels);
    std::reverse(out.begin(), out.end());
    return true;
}

void World::updateEnhancedAI(Monster& m, float dt) {
    const PlayerState& p = player_;
    float dx = p.x - m.x, dz = p.z - m.z;
    float dist = std::sqrt(dx * dx + dz * dz);
    float eye[3] = {m.x, m.y + m.eye, m.z}, target[3] = {p.x, playerEye(), p.z};
    const float reactionTime = difficulty == 0 ? 0.8f : difficulty == 1 ? 0.5f : 0.3f;
    const float coolTime = difficulty == 0 ? 1.1f : difficulty == 1 ? 0.65f : 0.35f;
    m.reaction -= dt;
    m.cooldown -= dt;
    m.repath -= dt;
    // Perception, a few times a second: sight (a wide view cone that becomes all-round once
    // alerted), hearing (gunfire and explosions), and being shot (handled in damageMonster).
    m.perceiveTimer -= dt;
    if (m.perceiveTimer <= 0) {
        m.perceiveTimer = frand(rng_, 0.12f, 0.2f);
        bool see = false;
        if (p.alive && dist < std::max(m.sight, 10) * 64 * 1.5f) {
            // A wide view cone, but all round within five cells, and for creatures that never
            // move (turrets, ceiling guns) all round within their sight range: none of them is
            // caught facing away as the player comes in.
            float facing = std::abs(angleDiff(yawOf(dx, dz), m.yaw));
            bool allRound = dist < 320 || (m.stationary && dist < m.sight * 64 + 20);
            if (m.alerted || facing < kPi * 0.4f || allRound) see = lineOfSight(eye, target);
        }
        if (see && !m.seesPlayer && m.cooldown < reactionTime) m.reaction = reactionTime * frand(rng_, 0.8f, 1.3f);
        m.seesPlayer = see;
        if (see) {
            if (!m.alerted) m.reaction = reactionTime * frand(rng_, 0.8f, 1.3f);
            m.alerted = true;
            m.lastSeenX = p.x;
            m.lastSeenZ = p.z;
            m.lastSeenY = p.y;
            m.lastSeenTime = time_;
            m.searchUntil = time_ + 12;
            // Call nearby creatures that can see this one (or are very close).
            for (auto& o : monsters_) {
                if (o.get() == &m || o->alerted || o->state == Monster::Dying || o->state == Monster::Dead || o->aiClass == 3) continue;
                float ox = o->x - m.x, oz = o->z - m.z, od = ox * ox + oz * oz;
                if (od > 8.0f * 64 * 8.0f * 64) continue;
                float oe[3] = {o->x, o->y + o->eye, o->z};
                if (od < 3.0f * 64 * 3.0f * 64 || lineOfSight(eye, oe)) {
                    o->alerted = true;
                    o->lastSeenX = p.x;
                    o->lastSeenZ = p.z;
                    o->lastSeenTime = time_;
                    o->searchUntil = time_ + 12;
                    o->reaction = reactionTime * frand(rng_, 1.0f, 1.6f);
                }
            }
        } else if (noiseTime_ > m.lastSeenTime && time_ - noiseTime_ < 0.5f) {
            float nx = noiseX_ - m.x, nz = noiseZ_ - m.z;
            if (nx * nx + nz * nz < noiseRange_ * noiseRange_) {
                // Heard it: go and look.
                m.alerted = true;
                m.lastSeenX = noiseX_;
                m.lastSeenZ = noiseZ_;
                m.lastSeenTime = time_;
                m.searchUntil = time_ + 10;
            }
        }
        if (m.alerted && !m.seesPlayer && time_ > m.searchUntil) m.alerted = false;   // gave up
    }
    if (m.state != Monster::Walk) return;
    if (!m.alerted) {
        if (!m.stationary) wanderMonster(m, dt * 0.6f);
        return;
    }
    bool melee = m.weapon[0].melee() || m.weapon[1].melee();
    bool ranged = m.weapon[0].ranged() || m.weapon[1].ranged();
    float rangedReach = std::max(std::max(m.bandA, m.bandB) * 64.0f + 20, ranged ? 600.0f : 0.0f);
    float dy = playerEye() - (m.y + m.eye);
    if (m.relocate > 0 && time_ > m.relocateUntil) m.relocate = 0;
    const bool moveOn = m.unfolded && !m.stationary && m.burstGoal > 0 && m.bursts >= m.burstGoal;
    // Attack when it can (not when it is to move on to another spot).
    if (m.seesPlayer && p.alive && m.reaction <= 0 && m.cooldown <= 0 && m.relocate <= 0 && !moveOn) {
        float cheb = std::max(std::abs(dx), std::abs(dz));
        int slot = -1;
        if (melee && cheb <= 96 && std::abs(dy) <= 64) slot = m.weapon[0].melee() ? ((m.weapon[1].melee() && (rng_() & 1)) ? 1 : 0) : 1;
        else if (ranged && dist <= rangedReach) slot = m.weapon[0].ranged() ? 0 : 1;
        if (m.aiClass == 4 && slot >= 0 && dist > m.bandB * 64 + 20) slot = -1;
        if (slot >= 0) {
            if (m.moving) {   // stop at once: give the cell back
                occupied_[m.ncz * kGrid + m.ncx]--;
                m.moving = false;
            }
            m.yaw = yawOf(dx, dz);
            beginAttack(m, slot);
            if (m.aiClass == 2 && !m.stationary && !m.vis->ready.empty() && !m.weapon[slot].melee()) {
                if (m.burstGoal <= 0) m.burstGoal = irandAI(rng_, 2, 4);
                ++m.bursts;
            }
            // The pause counts once the attack is over. Blows follow each other closely, as in
            // the game, where a creature whose blow landed swings again at once (FUN_0002a17b,
            // FUN_0002a539): a melee attacker stands and keeps swinging while the player is in reach.
            m.cooldown = (m.weapon[slot].melee() ? coolTime * 0.3f : coolTime) * frand(rng_, 0.7f, 1.4f);
            return;
        }
    }
    if (m.unfolded) {
        // Open and firing: hold position while the target stays in sight and reach; fold up to move.
        // One that can move (gunballs, the aliens' gunners) moves on after a few bursts, as the
        // game's do: there it is a shot that goes wide that folds it, and it floats or walks on a
        // cell before it opens up again (FUN_0002a539).
        if (m.seesPlayer && p.alive && dist <= rangedReach && !moveOn) {
            m.yaw += std::clamp(angleDiff(yawOf(dx, dz), m.yaw), -5 * dt, 5 * dt);
            return;
        }
        if (moveOn) {
            m.bursts = 0;
            m.burstGoal = irandAI(rng_, 2, 4);
            m.relocate = irandAI(rng_, 1, 2);
            m.relocateUntil = time_ + 3;
        }
        foldUp(m);
        return;
    }
    if (m.stationary) {
        if (m.seesPlayer) m.yaw += std::clamp(angleDiff(yawOf(dx, dz), m.yaw), -5 * dt, 5 * dt);
        return;
    }
    if (m.moving) return;
    if (m.relocate > 0) {
        // Moving on: a cell across the line of fire (either side), else nearer or further back.
        float ux = dx / std::max(1.0f, dist), uz = dz / std::max(1.0f, dist), side = (rng_() & 1) ? 1.0f : -1.0f;
        const float dirs[6][2] = {{-uz * side, ux * side}, {uz * side, -ux * side}, {(ux - uz * side) * 0.7f, (uz + ux * side) * 0.7f},
                                  {(-ux - uz * side) * 0.7f, (-uz + ux * side) * 0.7f}, {ux, uz}, {-ux, -uz}};
        for (const auto& d : dirs) {
            int sx = (int)std::lround(d[0]), sz = (int)std::lround(d[1]);
            if ((sx || sz) && chooseStep(m, sx, sz)) { --m.relocate; return; }
        }
        m.relocate = 0;   // nowhere to go: open up here again
        return;
    }
    // Where to go: the player (melee, or to get in range), a flanking cell next to it (ranged
    // creatures in range strafe), or the last place it was seen or heard.
    int gx, gz;
    int pcx = (int)std::floor(p.x / kCellSize), pcz = (int)std::floor(p.z / kCellSize);
    if (m.seesPlayer) {
        if (!melee && ranged && dist < rangedReach * 0.75f && dist > 160) {
            // In range: sidestep to a neighbouring cell across the line of fire, now and then.
            if (m.strafe <= 0) {
                m.strafe = frand(rng_, 0.6f, 1.6f);
                float sx = -dz / std::max(1.0f, dist), sz = dx / std::max(1.0f, dist);
                if (rng_() & 1) { sx = -sx; sz = -sz; }
                int ddx = (int)std::lround(sx), ddz = (int)std::lround(sz);
                if (chooseStep(m, ddx, ddz) || chooseStep(m, -ddx, -ddz)) return;
            }
            m.strafe -= dt;
            m.yaw += std::clamp(angleDiff(yawOf(dx, dz), m.yaw), -6 * dt, 6 * dt);
            return;
        }
        gx = pcx;
        gz = pcz;
    } else {
        gx = (int)std::floor(m.lastSeenX / kCellSize);
        gz = (int)std::floor(m.lastSeenZ / kCellSize);
        if (gx == m.cx && gz == m.cz) {
            // At the last known place: search round it.
            int ddx = irandAI(rng_, -1, 1), ddz = irandAI(rng_, -1, 1);
            m.lastSeenX = (m.cx + ddx * 3 + 0.5f) * kCellSize;
            m.lastSeenZ = (m.cz + ddz * 3 + 0.5f) * kCellSize;
            m.repath = 0;
            return;
        }
    }
    // Plan a route now and then (or at once for a new goal), a few creatures per frame at most.
    int goalCell = gz * kGrid + gx;
    if ((m.repath <= 0 || (m.path.empty() && m.pathGoal != goalCell)) && pathBudget_ > 0) {
        --pathBudget_;
        m.repath = frand(rng_, 0.4f, 0.7f);
        m.pathGoal = goalCell;
        uint64_t f0 = SDL_GetTicksNS();
        bool partial = false;
        bool found = findPath(m, gx, gz, m.seesPlayer ? p.y : m.lastSeenY, m.path, &partial);
        pathNs_ += SDL_GetTicksNS() - f0;
        if (partial) m.repath = frand(rng_, 1.5f, 2.5f);   // no way through: don't search again for a while
        if (!found) {
            m.path.clear();
            // No route: head straight for it as far as it can.
            int ddx = (gx > m.cx) - (gx < m.cx), ddz = (gz > m.cz) - (gz < m.cz);
            if (!chooseStep(m, ddx, ddz) && !chooseStep(m, ddx, 0)) chooseStep(m, 0, ddz);
            return;
        }
    }
    while (!m.path.empty() && m.path.front() == m.cz * kGrid + m.cx) m.path.erase(m.path.begin());
    if (m.path.empty()) return;
    int next = m.path.front();
    int ddx = next % kGrid - m.cx, ddz = next / kGrid - m.cz;
    if (std::abs(ddx) > 1 || std::abs(ddz) > 1) { m.path.clear(); return; }
    if (chooseStep(m, ddx, ddz)) {
        m.path.erase(m.path.begin());
        if (!m.seesPlayer) m.searchUntil = std::max(m.searchUntil, time_ + 4);   // still on the trail: keep looking
    } else {
        // Blocked (another creature or a closed door): try round it, then plan again soon.
        if (traceMonster_ == m.index)
        {
            float ny = 0;
            bool ok = stepTarget(m.cx, m.cz, m.cx + ddx, m.cz + ddz, m.ground, m.vis->height, ny);
            SDL_Log("dbg path blocked at %d,%d -> %d,%d (goal %d,%d, %d steps) occupied %d fixed %d door %d y %.0f h %.0f step %d ny %.0f",
                    m.cx, m.cz, m.cx + ddx, m.cz + ddz, gx, gz, (int)m.path.size(), occupied_[next], fixed_[next], doorCell_[next],
                    m.y, m.vis->height, ok, ny);
        }
        if (!chooseStep(m, ddx, 0) && !chooseStep(m, 0, ddz)) m.repath = std::min(m.repath, 0.25f);
    }
}

// ---------------------------------------------------------------------------------------------
// Every creature, every frame

void World::updateMonsters(float dt, const Camera& cam) {
    const PlayerState& p = player_;
    // The game's activity bubble: recomputed when the player moves into another cell.
    int pcx = (int)std::floor(p.x / kCellSize), pcz = (int)std::floor(p.z / kCellSize);
    bool newCell = pcx != playerCellX_ || pcz != playerCellZ_;
    playerCellX_ = pcx;
    playerCellZ_ = pcz;
    pathBudget_ = 3;
    if (SDL_getenv("CYCLONES_FLIERS")) {   // testing: how far the fliers have gone
        static float next = 10;
        static std::map<const Monster*, std::array<float, 3>> track;   // x, z, distance
        int n = 0, went = 0, alerted = 0, dead = 0;
        for (auto& mp : monsters_) {
            const Monster& m = *mp;
            if (m.hover <= 0 || m.stationary) continue;
            auto& t = track.try_emplace(&m, std::array<float, 3>{m.x, m.z, 0}).first->second;
            t[2] += std::hypot(m.x - t[0], m.z - t[1]);
            t[0] = m.x;
            t[1] = m.z;
            ++n;
            if (t[2] > 64) ++went;
            if (m.alerted) ++alerted;
            if (m.state == Monster::Dead || m.state == Monster::Dying) ++dead;
        }
        if (time_ >= next) {
            next += 10;
            SDL_Log("dbg fliers at %.0f s: %d, %d have gone a cell or more, %d alerted, %d dead", time_, n, went, alerted, dead);
        }
    }
    for (auto& mp : monsters_) {
        Monster& m = *mp;
        m.flash = std::max(0.0f, m.flash - dt * 6);
        if (newCell) {
            float ddx = p.x - m.x, ddz = p.z - m.z;   // the larger of the two distances (FUN_00047206)
            m.active = std::max(std::abs(ddx), std::abs(ddz)) <= 704.0f;
            m.shotActive = false;
        } else if (m.shotActive) {
            m.active = true;
        }
        if (m.state == Monster::Dead) {
            m.stateTime += dt;
            continue;
        }
        if (m.state == Monster::Dying) {
            m.stateTime += dt;
            if (m.vis->death.empty() || m.stateTime >= m.vis->death.total()) {
                m.state = Monster::Dead;
                m.gone = m.vis->death.empty() || !m.vis->death.hold;
                m.stateTime = 0;
                dropItem(m);
                if (m.aiClass == 3 || m.barrel) {
                    // Barrels and gas go off when their death animation ends, with their death
                    // sound once more (FUN_0002a8b2).
                    creatureSound(m, m.sndDeath);
                    explode(m.x, m.y + m.vis->height * 0.5f, m.z, 1.0f, "");
                    areaBlast(m);
                }
            }
            continue;
        }
        float dxp = cam.x - m.x, dzp = cam.z - m.z;
        // Which of the 8 original views faces the camera. It switches outright (never through a
        // blend) with a little hysteresis, so it doesn't flicker between two views.
        {
            float X[3] = {-std::cos(m.yaw), 0, -std::sin(m.yaw)}, Z[3] = {std::sin(m.yaw), 0, -std::cos(m.yaw)};
            float phi = std::atan2(dxp * X[0] + dzp * X[2], dxp * Z[0] + dzp * Z[2]);
            if (phi < 0) phi += 2 * kPi;
            float f = phi / (kPi / 4);
            int nearest = ((int)std::lround(f)) % 8;
            if (m.view < 0) m.view = nearest;
            float off = std::abs(angleDiff(f * kPi / 4, m.view * kPi / 4)) / (kPi / 4);
            if (nearest != m.view && off > 0.56f) m.view = nearest;
        }
        bool fighting = monstersAttack && m.aiClass != 0 && m.startMode != 13 && m.startMode != 0;
        if (traceMonster_ == m.index) {   // testing (CYCLONES_TRACE=index)
            static float tt = 0;
            if ((tt += dt) >= (SDL_getenv("CYCLONES_TRACE_FAST") ? 0.0f : 0.25f)) {
                tt = 0;
                SDL_Log("dbg trace %d t %.2f state %d time %.2f alerted %d sees %d react %.2f cool %.2f active %d moving %d cell %d,%d pos %.0f,%.0f y %.0f hp %.0f",
                        m.index, time_, (int)m.state, m.stateTime, m.alerted, m.seesPlayer, m.reaction, m.cooldown, m.active,
                        m.moving, m.cx, m.cz, m.x, m.z, m.y, m.hp);
            }
        }
        static const bool checkFliers = SDL_getenv("CYCLONES_FLIERS") != nullptr;
        if (checkFliers && m.hover > 0 && !m.stationary && m.state != Monster::Dying && m.state != Monster::Dead &&
            !(m.moving && m.y > m.duck + 0.5f && m.duck > -1e8f)) {
            // Testing: a flier whose box has gone into a surface (once it has been clear).
            static std::map<const Monster*, int> was;   // 1 clear, 2 reported
            float px[6], pz[6], fl, room;
            int n = std::min(5, boxPoints(m, px, pz));   // the box itself (not the cell ahead)
            flierRoom(m, px, pz, n, m.y, fl, room);
            bool bad = room < m.y + boxHeight(m) - 1 || fl > m.y + 1;   // a surface over 15 units up in its box, or one lower down not under its feet
            int& w = was[&m];
            if (!bad && w == 0) w = 1;
            if (bad && w == 1) {
                w = 2;
                SDL_Log("dbg flier into a surface: %d at %.0f,%.0f y %.0f box to %.0f, room %.0f floor %.0f moving %d duck %.0f",
                        m.index, m.x, m.z, m.y, m.y + boxHeight(m), room, fl, m.moving, m.duck);
            }
        }
        // Animation states
        if (m.state == Monster::Pain) {
            m.stateTime += dt;
            if (m.stateTime >= m.vis->pain.total()) {
                m.painReady = time_ + 0.3f;
                m.state = Monster::Walk;
                m.thinkTimer = 0;
                if (m.resumeAttack && fighting) { m.resumeAttack = false; m.unfolded = m.unfolded || !m.vis->ready.empty(); startAttack(m, m.readySlot); }
                m.resumeAttack = false;
            }
            continue;
        }
        if (m.state == Monster::Ready) {
            m.stateTime += dt;
            if (m.stateTime >= m.vis->ready.total()) {
                m.unfolded = true;
                if (fighting) startAttack(m, m.readySlot);
                else foldUp(m);
            }
            continue;
        }
        if (m.state == Monster::Unready) {
            m.stateTime += dt;
            if (m.stateTime >= m.vis->ready.total() * 0.5f + 0.02f) {
                m.state = Monster::Walk;
                m.thinkTimer = 0;
                if (!enhancedAI && fighting && !m.stationary) foldedWalk(m);
            }
            continue;
        }
        if (m.state == Monster::Attack) {
            const SpriteAnim& a = m.attackSlot == 0 ? m.vis->attackA : m.vis->attackB;
            const std::vector<uint8_t>& flags = m.attackSlot == 0 ? m.vis->fireA : m.vis->fireB;
            m.stateTime += dt;
            int f;
            float t;
            animFrame(a.seconds, m.stateTime, false, f, t);
            bool ended = m.stateTime >= a.total();
            if (ended) f = (int)a.still.size();   // every frame has been shown
            // Keep facing the target while shooting (turrets exactly, others as they turned).
            if (enhancedAI || m.aiClass == 4) m.yaw = yawOf(p.x - m.x, p.z - m.z);
            for (int k = m.attackFrame + 1; k <= std::min(f, (int)a.still.size() - 1); ++k) {
                if (k >= 1 && k < (int)flags.size() && flags[k]) m.lastFire = monsterFire(m, m.attackSlot);
                // A muzzle flash with a light code shows for the game's 5 ticks, about half as
                // long as the same frame where it has none (BASE's gunballs, 9): BIOSTOR2's,
                // CORE's and CLONE's flashed for a blink. Drawn (and lit) for 8 ticks here, and
                // without gliding in or out; the shots keep the game's timing.
                if (k < (int)a.light.size() && a.light[k] > 0) {
                    m.flashUntil = time_ + kFlashHold;
                    m.flashFrame = k;
                    m.flashSlot = m.attackSlot;
                }
            }
            m.attackFrame = std::max(m.attackFrame, std::min(f, (int)a.still.size() - 1));
            if (ended) {
                float over = m.stateTime - a.total();   // carried into a follow-up attack
                m.attackFrame = -1;
                m.state = Monster::Walk;
                if (m.unfolded && !m.lastFire) {
                    foldUp(m);   // lost sight: the game folds up at once (mode 7)
                } else if (!enhancedAI && fighting && m.aiClass == 1) {
                    originalAttackEnded(m);   // again at once, or give up and walk
                    if (m.state == Monster::Attack) m.stateTime = std::max(0.0f, over);
                } else if (!enhancedAI && !m.lastFire && m.aiClass == 2) {
                    m.walkOnly = true;        // no shot: it walks, and thinks again 75 ticks later
                    m.thinkTimer = 0;
                } else {
                    // The game decides again at once; it keeps attacking while it can see the player.
                    m.thinkTimer = m.lastFire && !enhancedAI ? 0 : m.thinkTimer;
                }
            }
            continue;
        }
        // Walking
        bool walking = false;
        float moved = 0;
        if (m.stagger > 0) m.stagger -= dt;
        if (m.stagger <= 0) {
            if (!fighting) {
                if (monstersMove && m.startMode != 13) wanderMonster(m, dt);   // 13: frozen (a script's action 16)
            } else if (enhancedAI) {
                updateEnhancedAI(m, dt);
            } else {
                updateOriginalAI(m, dt);
            }
            if (m.state == Monster::Walk && (monstersMove || fighting)) monsterStep(m, dt, walking, moved);
        }
        // Walk cycle: legs keep pace with the floor; when stopping, finish the stride.
        const std::vector<float>& secs = !m.vis->rot.empty() ? m.vis->rot.seconds : m.vis->flat.seconds;
        int steps = (int)secs.size();
        if (steps > 1) {
            int s = (int)m.step % steps;
            bool atRest = s == 0 && m.step - std::floor(m.step) < 1e-4f;
            if (walking || !atRest) {
                float before = m.step;
                if (walking && m.vis->rot.byDistance) {
                    m.step += moved / (m.stride / steps);
                } else {
                    float rate = 1.0f / std::max(0.02f, secs[s]);
                    if (!walking && m.vis->rot.byDistance) rate *= 0.6f;
                    m.step += dt * rate;
                }
                if (!walking && std::floor(m.step) != std::floor(before) && (int)std::floor(m.step) % steps == 0)
                    m.step = std::floor(m.step);
                if (m.step >= steps) m.step -= steps;
            }
        }
    }
}
