// The player's body: walking with collision against the level's walls, floors, ceilings, doors,
// creatures and solid props; health, armour and damage.
#include "world_internal.h"

namespace {

// The game's player (FUN_0004d35d / FUN_0004d5e0 / FUN_0004dc9a): a 40 x 40 x 55 box with the eye
// 52 above the floor, stepping up 17 units. Speeds are per second (the game's per-tick x 100).
const float kRadius = 19.0f;       // body radius (the game's box is 40 wide)
const float kHeight = 55.0f;       // body height
const float kEye = 52.0f;          // eye above the feet
const float kStep = 17.0f;         // highest step walked up without jumping
const float kForward[2] = {344.0f, 594.0f};   // walk, run
const float kBack[2] = {219.0f, 469.0f};
const float kStrafe[2] = {281.0f, 500.0f};
const float kFall = 1465.0f;       // gravity, units/s^2 (0.1465 a tick, per tick)
const float kJump = 300.0f;        // take-off speed (3 units a tick: 32 units high, in whole ticks)
const float kJetPush = 3.125f;     // the jump jets' thrust: speed added a tick instead of gravity (1/32 unit a tick)
const float kJetDelay = 25.0f;     // ticks after a take-off before the jets fire
const float kWallStep = 18.0f;     // a wall's top this far above the feet is stepped onto (FUN_0004dc9a: top + 51 <= eye + 17)

}  // namespace

void World::spawnPlayer(float x, float z, float yaw) {
    player_ = PlayerState();
    player_.x = x;
    player_.z = z;
    // Stand on the cell's own floor (not on whatever is stacked highest above it), unless there
    // is no room to stand there.
    {
        int cx = std::clamp((int)std::floor(x / kCellSize), 0, kGrid - 1), cz = std::clamp((int)std::floor(z / kCellSize), 0, kGrid - 1);
        float ceil;
        float g = bodyGround(x, z, (float)(int16_t)level_->floor.height[cz * kGrid + cx], &ceil);
        float top = groundBelow(x, z, 1e6f);
        player_.y = (ceil - g >= kHeight && playerFits(x, z, g, nullptr)) || !playerFits(x, z, top, nullptr) ? g : top;
    }
    player_.viewY = player_.y + kEye;
    lastPX_ = x;
    lastPZ_ = z;
    playerVelX_ = playerVelZ_ = 0;
    (void)yaw;
    if (startWithAllWeapons) {   // the game's -W cheat: every weapon, full
        for (int k = 0; k < 8; ++k)
            if (!weaponAbsent[k]) { player_.owned[k] = true; player_.ammo[k] = kWeapons[k].maxAmmo; }
    }
    cooldown_ = 0.3f;
    meleeTimer_ = 0;
}

void World::carryOver(const PlayerState& k, int newMission) {
    PlayerState& p = player_;
    p.health = k.health;
    p.armor = k.armor;
    p.armorMax = k.armorMax;
    p.suit = k.suit;
    for (int i = 0; i < 9; ++i) { p.owned[i] = k.owned[i]; p.ammo[i] = k.ammo[i]; }
    p.weapon = k.weapon;
    p.bioKits = k.bioKits;
    p.mechKits = k.mechKits;
    p.jetFuel = k.jetFuel;
    p.keys = 0;
    // The kit stacks come along (each a slot of the new level's inventory); the rest of the
    // inventory, the mission's own things, is purged.
    ensureKitSlots();
    if (newMission == 4 || newMission == 7 || newMission == 11 || newMission == 14) {
        p.health = 104;
        p.armor = p.armorMax;
    }
}

void World::message(const std::string& text, int font) {
    for (auto& m : messages_)
        if (m.text == text) { m.age = 0; return; }   // repeated: refresh instead of stacking
    messages_.push_back({text, font, 0});
    if (messages_.size() > 4) messages_.erase(messages_.begin());
}

// Is there room for the body at (x, z) standing at `feet`? Reports the highest floor under it.
bool World::playerFits(float x, float z, float feet, float* groundOut) const {
    const Level& L = *level_;
    const float r = kRadius, lo = feet + kStep, hi = feet + kHeight;
    // Walls on cell edges.
    auto edgeSolid = [&](const EdgeLayer& e, int cx, int cz) {
        if (cx < 0 || cz < 0 || cx >= kGrid || cz >= kGrid) return true;
        int i = cz * kGrid + cx;
        if (!e.texFront[i] || (e.flags[i] & 8)) return false;
        float span = e.vEnd[i] ? (float)(e.vEnd[i] - e.vStart[i] + 1) : (float)renderer_->wallHeight(e.texFront[i]);
        float b = (float)(int16_t)e.base[i], t = b + span;
        return b < hi && t > feet + kWallStep;
    };
    auto segHit = [&](float ax, float az, float bx, float bz) {
        float dx = bx - ax, dz = bz - az, l2 = dx * dx + dz * dz;
        float t = l2 > 0 ? std::clamp(((x - ax) * dx + (z - az) * dz) / l2, 0.0f, 1.0f) : 0.0f;
        float px = ax + dx * t - x, pz = az + dz * t - z;
        return px * px + pz * pz < r * r;
    };
    int cx0 = (int)std::floor((x - r) / kCellSize), cx1 = (int)std::floor((x + r) / kCellSize);
    int cz0 = (int)std::floor((z - r) / kCellSize), cz1 = (int)std::floor((z + r) / kCellSize);
    for (int cz = cz0; cz <= cz1 + 1; ++cz)
        for (int cx = cx0; cx <= cx1 + 1; ++cx) {
            float x0 = (float)cx * kCellSize, z0 = (float)cz * kCellSize;
            if (cx <= cx1 && edgeSolid(L.north, cx, cz) && segHit(x0, z0, x0 + kCellSize, z0)) return false;
            if (cz <= cz1 && edgeSolid(L.west, cx, cz) && segHit(x0, z0, x0, z0 + kCellSize)) return false;
        }
    // Blocks taller than a step stand in the way with their whole outline. The floor test below
    // only samples nine points round the body, about 14 units apart at its edge, and a thin block
    // between two of them was walked through: MAINTEN2's exit bars (5-unit posts 10 apart, raised
    // by the switch) let you leave without the key, up to 0.5.2. (The game tests a 40 x 40 box
    // against each block's own box, FUN_00020105 type 8.)
    {
        int bx0 = std::max(0, cx0), bx1 = std::min(kGrid - 1, cx1), bz0 = std::max(0, cz0), bz1 = std::min(kGrid - 1, cz1);
        for (int cz = bz0; cz <= bz1; ++cz)
            for (int cx = bx0; cx <= bx1; ++cx)
                for (int ci : cubesInCell_[cz * kGrid + cx]) {
                    const auto& c = L.cubes[ci];
                    auto u16 = [&](int k) { return (int)(c[k * 2] | c[k * 2 + 1] << 8); };
                    float bxl = (float)u16(1), bzl = (float)u16(2), base = (float)(int16_t)u16(3);
                    float bxh = bxl + c[8], bzh = bzl + c[10], hgt = cubeHeight_[ci];
                    if (hgt <= 0 || base + hgt <= lo + 0.5f || base >= hi) continue;   // a step, or overhead
                    float nx = std::clamp(x, bxl, bxh) - x, nz = std::clamp(z, bzl, bzh) - z;
                    if (nx * nx + nz * nz < r * r) return false;
                }
    }
    // Floors, platforms and ceilings under and over the body (centre and eight points round it).
    float ground = -1e9f;
    for (int k = 0; k < 9; ++k) {
        float px = x, pz = z;
        if (k) {
            float a = (k - 1) * kPi / 4;
            px += std::cos(a) * r * 0.92f;
            pz += std::sin(a) * r * 0.92f;
        }
        int cx = (int)std::floor(px / kCellSize), cz = (int)std::floor(pz / kCellSize);
        if (cx < 0 || cz < 0 || cx >= L.width || cz >= L.height) return false;
        int i = cz * kGrid + cx;
        if (!hasGround(px, pz)) return false;  // outside
        // The ground itself higher than a step is solid (a crate stack taller than you are is
        // not a ceiling to walk under).
        if ((L.floor.tex[i] & 0x3fff) && surfaceY(L.floor, 0, px, pz) > lo + 0.5f) return false;
        float ceil;
        float g = groundBelow(px, pz, lo, &ceil);
        if (ceil < hi) return false;
        ground = std::max(ground, g);
    }
    // Doors that aren't open.
    for (const auto& d : doors_) {
        if (d.open > 0.85f) continue;
        float dx0, dz0, dx1, dz1;
        if (d.ew) { dx0 = (float)d.spec.x; dx1 = dx0 + kCellSize; dz0 = dz1 = (float)(d.spec.z + d.spec.offset); }
        else { dz0 = (float)d.spec.z; dz1 = dz0 + kCellSize; dx0 = dx1 = (float)(d.spec.x + d.spec.offset); }
        if (d.base > hi || d.base + d.height < lo) continue;
        if (segHit(dx0, dz0, dx1, dz1)) { bumpedDoor_ = (int)(&d - doors_.data()); return false; }
    }
    // Creatures and solid props. A creature blocks however short it is: it isn't a step to climb
    // onto. (Up to 0.5.2 anything under a step's height was walked over, so MAINTEN2's and SDI2's
    // rats ended up under your feet, where they can't move, as no creature steps into your cell.)
    for (const auto& m : monsters_) {
        if (m->state == Monster::Dead || m->state == Monster::Dying) continue;
        float rr = kRadius + std::clamp(m->vis->radius * 0.8f, 8.0f, 28.0f);
        float dx = m->x - x, dz = m->z - z;
        if (dx * dx + dz * dz < rr * rr && m->y < hi && m->y + m->vis->height > feet + 1) return false;
    }
    for (const auto& o : objects_) {
        if (!o.intact || o.removed || !o.grounded || o.vis->height < 20) continue;
        float rr = kRadius + std::clamp(o.vis->radius * 0.7f, 6.0f, 24.0f);
        float dx = o.x - x, dz = o.z - z;
        if (dx * dx + dz * dz < rr * rr && o.y < hi && o.y + o.vis->height > lo) return false;
    }
    if (groundOut) *groundOut = ground;
    return true;
}

bool World::hasGround(float x, float z) const {
    const Level& L = *level_;
    int cx = (int)std::floor(x / kCellSize), cz = (int)std::floor(z / kCellSize);
    if (cx < 0 || cz < 0 || cx >= kGrid || cz >= kGrid) return false;
    int i = cz * kGrid + cx;
    if ((L.floor.tex[i] & 0x3fff) || (L.extra1.tex[i] & 0x3fff) || (L.extra2.tex[i] & 0x3fff)) return true;
    // A block's footprint, and the seams round it: the levels lay blocks 63 units wide in 64-unit
    // squares, so a unit's gap runs between neighbouring steps, where no floor lies under it either.
    const float seam = 2;
    for (int ci : cubesInCell_[i]) {
        const auto& c = L.cubes[ci];
        auto u16 = [&](int k) { return (int)(c[k * 2] | c[k * 2 + 1] << 8); };
        float x0 = (float)u16(1), z0 = (float)u16(2);
        if (cubeHeight_[ci] > 0 && x >= x0 - seam && x <= x0 + c[8] + seam && z >= z0 - seam && z <= z0 + c[10] + seam) return true;
    }
    return false;
}

// The highest floor under the body (anything up to a step above the feet) and the lowest
// ceiling over it, sampled at the centre and eight points round the edge.
float World::bodyGround(float x, float z, float feet, float* ceilingOut) const {
    float ground = -1e9f, ceiling = 1e9f;
    for (int k = 0; k < 9; ++k) {
        float px = x, pz = z;
        if (k) {
            float a = (k - 1) * kPi / 4;
            px += std::cos(a) * kRadius * 0.92f;
            pz += std::sin(a) * kRadius * 0.92f;
        }
        float ceil;
        float g = groundBelow(px, pz, feet + kStep, &ceil);
        ground = std::max(ground, g);
        ceiling = std::min(ceiling, ceil);
    }
    if (ceilingOut) *ceilingOut = ceiling;
    return ground;
}

void World::movePlayer(const PlayerInput& in, float dt, Camera& cam) {
    PlayerState& p = player_;
    if (!level_) return;
    if (!p.alive) {
        // Sink to the floor and look up a little.
        p.deadTime += dt;
        float target = p.y + 10;
        p.viewY += (target - p.viewY) * std::min(1.0f, dt * 4);
        cam.x = p.x;
        cam.z = p.z;
        cam.y = p.viewY;
        return;
    }
    int run = in.run ? 1 : 0;
    float fwdSpeed = in.forward > 0 ? kForward[run] * in.forward : kBack[run] * in.forward;
    float sideSpeed = kStrafe[run] * in.strafe;
    float fx = std::sin(cam.yaw), fz = -std::cos(cam.yaw);
    // Velocity closes 1/8 of the gap to the target every tick, as in the game.
    float tvx = fx * fwdSpeed - fz * sideSpeed, tvz = fz * fwdSpeed + fx * sideSpeed;
    float k = 1.0f - std::pow(7.0f / 8.0f, dt * 100.0f);
    p.vx += (tvx - p.vx) * k;
    p.vz += (tvz - p.vz) * k;
    if (in.jump && p.onGround) { p.vy = kJump; p.onGround = false; p.takeoff = true; p.jetCool = kJetDelay; }
    float dx = p.vx * dt, dz = p.vz * dt;
    float len = std::sqrt(dx * dx + dz * dz);
    int steps = std::max(1, (int)std::ceil(len / 6.0f));
    float sx = dx / steps, sz = dz / steps;
    for (int i = 0; i < steps; ++i) {
        if (playerFits(p.x + sx, p.z + sz, p.y, nullptr)) { p.x += sx; p.z += sz; continue; }
        if (std::abs(sx) > 1e-4f && playerFits(p.x + sx, p.z, p.y, nullptr)) { p.x += sx; p.vz = 0; continue; }
        if (std::abs(sz) > 1e-4f && playerFits(p.x, p.z + sz, p.y, nullptr)) { p.z += sz; p.vx = 0; continue; }
        p.vx = p.vz = 0;
        break;
    }
    // What is under and over the body where it now stands. (Blocked moves used to leave the old
    // height as the "ground", so pushing against a crate in mid-jump stood you on thin air and a
    // second jump climbed higher, up into the ceiling.)
    float ceiling;
    float ground = bodyGround(p.x, p.z, p.y, &ceiling);
    // Steps up are taken at once (the view glides); drops are fallen with gravity, and a jump
    // stops at the ceiling.
    if (ground > p.y && p.vy <= 0) {
        p.y = ground;
        p.vy = 0;
        p.onGround = true;
    } else if (ground < p.y - 0.5f || p.vy > 0) {
        // In the air, in whole 100 Hz ticks as the game does it (FUN_0004d5e0): each tick the
        // speed drops by 0.1465 units a tick (except the tick of take-off) and then the body
        // moves by it. A jump so peaks 32.2 units up; a smooth curve with the same numbers peaks
        // at 30.7, which left the 48-unit ledges the game lets you jump onto out of reach.
        p.fallTicks += dt * 100.0f;
        int n = (int)p.fallTicks;
        p.fallTicks -= n;
        p.onGround = false;
        for (int t = 0; t < n; ++t) {
            ++airTicks_;
            if (p.takeoff) p.takeoff = false;
            else {
                // The jump jets (FUN_0004d5e0): switched on, with the jump key held and the jump's
                // first 25 ticks over, they push up by 1/32 of a unit a tick instead of falling,
                // and burn a unit of fuel every 8 ticks (sound 84 every other one). Out of fuel,
                // they go off and their icon goes (the fuel is -1 again).
                if (p.jetCool > 0) p.jetCool -= 1;
                if (in.jump && p.jets && p.jetCool <= 0) {
                    p.vy += kJetPush;
                    if ((airTicks_ & 7) == 0) {
                        --p.jetFuel;
                        if (p.jetFuel & 1) sound(84, 200);
                        if (p.jetFuel < 1) {
                            p.jets = false;
                            p.jetFuel = -1;
                        }
                    }
                } else p.vy -= kFall / 100.0f;
            }
            float ny = p.y + p.vy / 100.0f;
            if (p.vy > 0 && ny + kHeight > ceiling) {
                ny = std::max(p.y, ceiling - kHeight);
                p.vy = 0;
            }
            p.y = ny;
            if (p.y <= ground) { p.y = ground; p.vy = 0; p.onGround = true; p.fallTicks = 0; break; }
        }
    } else {
        p.y = std::max(p.y, ground);
        p.onGround = true;
    }
    if (p.onGround && p.jetCool > 0) p.jetCool = std::max(0.0f, p.jetCool - dt * 100.0f);
    if (len > 0.01f && p.onGround) p.bob += len / 64.0f;
    float target = p.y + kEye;
    p.viewY += (target - p.viewY) * std::min(1.0f, dt * 18);
    if (std::abs(target - p.viewY) > 40) p.viewY = target;
    cam.x = p.x;
    cam.z = p.z;
    cam.y = p.viewY;
}

void World::toggleJets() {
    PlayerState& p = player_;
    if (p.jets) p.jets = false;
    else if (p.jetFuel > 0 && p.alive) p.jets = true;
}

void World::placePlayer(const Camera& cam) {
    player_.x = cam.x;
    player_.z = cam.z;
    player_.y = cam.y - kEye;
    player_.viewY = cam.y;
    player_.vy = 0;
}

// Testing (CYCLONES_STEPCHECK): from each cell's floor to each neighbour's floors a jump can
// reach, walk (and jump where it takes one) from centre to centre without creatures or props,
// and report the neighbours the body fits on but can't get onto.
void World::stepCheck() {
    const Level& L = *level_;
    std::vector<std::unique_ptr<Monster>> keepM;
    std::vector<Object> keepO;
    std::swap(keepM, monsters_);
    std::swap(keepO, objects_);
    int found = 0;
    for (int cz = 0; cz < kGrid; ++cz)
        for (int cx = 0; cx < kGrid; ++cx) {
            float ax = (cx + 0.5f) * kCellSize, az = (cz + 0.5f) * kCellSize;
            int i = cz * kGrid + cx;
            if (doorCell_[i]) continue;
            // Each floor of A the body stands on.
            std::vector<float> floorsA;
            for (float from = 4000; floorsA.size() < 3;) {
                float g = groundBelow(ax, az, from);
                if (g < -1e8f || g > from) break;
                if (playerFits(ax, az, g, nullptr)) floorsA.push_back(g);
                from = g - 1;
            }
            static const int kDx[4] = {1, -1, 0, 0}, kDz[4] = {0, 0, 1, -1};
            for (float fA : floorsA)
                for (int d = 0; d < 4; ++d) {
                    int nx = cx + kDx[d], nz = cz + kDz[d];
                    if (nx < 0 || nz < 0 || nx >= kGrid || nz >= kGrid || doorCell_[nz * kGrid + nx]) continue;
                    float bx = (nx + 0.5f) * kCellSize, bz = (nz + 0.5f) * kCellSize;
                    float gB = bodyGround(bx, bz, fA + 32, nullptr);   // the highest a jump lands on
                    if (gB < fA - 17 || gB > fA + 49) continue;
                    {   // a wall between them that stands over both floors: a wall, not a step
                        int ek = d == 0 ? nz * kGrid + nx : d == 1 ? i : d == 2 ? nz * kGrid + nx : i;
                        const EdgeLayer& e = d < 2 ? L.west : L.north;
                        if (e.texFront[ek] && !(e.flags[ek] & 8)) {
                            float span = e.vEnd[ek] ? (float)(e.vEnd[ek] - e.vStart[ek] + 1) : (float)renderer_->wallHeight(e.texFront[ek]);
                            float b = (float)(int16_t)e.base[ek], t = b + span;
                            if (b < std::max(fA, gB) + kHeight && t > std::max(fA, gB) + kWallStep) continue;
                        }
                    }
                    if (!playerFits(bx, bz, gB, nullptr)) {
                        float ceil;
                        bodyGround(bx, bz, gB, &ceil);
                        if (ceil - gB > 20 && ceil - gB < kHeight)   // a floor with a little room over it
                            SDL_Log("dbg stepcheck %d,%d (floor %.0f) -> %d,%d (floor %.0f, %.0f up): no room, %.0f under the ceiling",
                                    cx, cz, fA, nx, nz, gB, gB - fA, ceil - gB);
                        continue;
                    }
                    // Walk across in half-unit steps (a fast frame rate moves you about that far a
                    // frame), stepping up, jumping when blocked; down the middle and 12 units to
                    // either side of it.
                    float feet = fA, x = ax, z = az;
                    bool ok = true;
                    float bad = 0;
                    int lane = 0;
                    for (int side = -1; side <= 1 && ok; ++side) {
                        const float ox = kDx[d] ? 0.0f : side * 12.0f, oz = kDz[d] ? 0.0f : side * 12.0f;
                        if (side && !playerFits(ax + ox, az + oz, fA, nullptr)) continue;
                        feet = fA;
                        for (int s = 1; s <= 128; ++s) {
                            x = ax + ox + (bx - ax) * s / 128.0f;
                            z = az + oz + (bz - az) * s / 128.0f;
                            float g = bodyGround(x, z, feet, nullptr);
                            if (g > feet) feet = g;
                            if (playerFits(x, z, feet, nullptr)) continue;
                            bool fit = false;
                            for (float f = feet + 2; f <= fA + 32; f += 2)
                                if (playerFits(x, z, f, nullptr)) { feet = std::max(f, bodyGround(x, z, f, nullptr)); fit = true; break; }
                            if (!fit) {
                                ok = false;
                                bad = s / 1.28f;
                                lane = side;
                                break;
                            }
                        }
                    }
                    if (ok) continue;
                    ++found;
                    auto tex = [&](const EdgeLayer& e, int k) { return e.texFront[k] ? (int)e.texFront[k] : 0; };
                    int ek = d == 0 ? nz * kGrid + nx : d == 1 ? i : d == 2 ? nz * kGrid + nx : i;
                    const EdgeLayer& e = d < 2 ? L.west : L.north;
                    SDL_Log("dbg stepcheck %d,%d (floor %.0f) -> %d,%d (floor %.0f, %.0f up)%s: stuck %.0f%% of the way at %.2f,%.2f feet %.0f; edge tex %d base %d span %d-%d flags %02x",
                            cx, cz, fA, nx, nz, gB, gB - fA, lane ? " 12 off the middle" : "", bad, x / kCellSize, z / kCellSize, feet, tex(e, ek),
                            (int)(int16_t)e.base[ek], (int)e.vStart[ek], (int)e.vEnd[ek], (int)e.flags[ek]);
                }
        }
    std::swap(keepM, monsters_);
    std::swap(keepO, objects_);
    SDL_Log("dbg stepcheck: %d", found);
}
