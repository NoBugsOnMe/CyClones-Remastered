#include "firstperson.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "../game/world.h"

namespace {

// Per weapon, from its record in the weapon table (FUN_000509ee, +0x35..+0x41): the muzzle flash
// (frames, step mode, tics per frame, later frames see-through, the frame shown plain in step
// mode) and how the gun follows the reticle (x shift, the two y shifts, the height it rests at)
// and how hard it kicks.
struct GunDef {
    int flashN, mode, tics, tint, plain;
    const char* flash[5];
    int xsh, ysh1, ysh2, yoff, recoil;
};
const GunDef kGuns[8] = {
    {3, 0, 3, 1, 1, {"gflash10", "gflash11", "gflash12"}, 1, 3, 1, 50, 18},                 // gauss pistol
    {2, 1, 3, 1, 1, {"cflash10", "cflash20", "cflash11"}, 3, 2, 2, 20, 0},                  // photon chaingun
    {3, 0, 3, 1, 0, {"apflash0", "apflash1", "apflash2"}, 1, 3, 1, 64, 25},                 // alien pistol
    {4, 0, 3, 1, 1, {"dgflash0", "dgflash1", "dgflash2", "dgflash3"}, 1, 3, 2, 30, 35},     // alien rifle
    {4, 0, 3, 1, 1, {"grflash0", "grflash1", "grflash2", "grflash3"}, 1, 3, 2, 7, 50},      // grenade launcher
    {0, 0, 0, 0, 0, {}, 2, 3, 1, 0, 0},                                                     // twin laser (suit)
    {0, 0, 0, 0, 0, {}, 2, 3, 1, 0, 0},                                                     // rocket launcher (suit)
    {0, 0, 0, 0, 0, {}, 2, 3, 1, 0, 0},                                                     // plasma accelerator (suit)
};

// Which of the 15 poses (5 turns x 3 raises) shows a gun whose anchor is (dx, dy) / 8 pixels
// beside and below the reticle (byte table at 0x755f8, 25 rows of 20).
const unsigned char kAim[25][20] = {
    {0, 1, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4},
    {0, 1, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4},
    {0, 1, 2, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4},
    {0, 1, 2, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4},
    {0, 1, 1, 2, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4},
    {0, 1, 1, 2, 2, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4},
    {0, 1, 1, 1, 2, 2, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4},
    {5, 6, 6, 6, 7, 7, 8, 8, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9},
    {5, 5, 6, 6, 7, 7, 7, 8, 8, 8, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9},
    {5, 5, 6, 6, 6, 7, 7, 8, 8, 8, 8, 9, 9, 9, 9, 9, 9, 9, 9, 9},
    {5, 5, 6, 6, 6, 7, 7, 7, 8, 8, 8, 8, 9, 9, 9, 9, 9, 9, 9, 9},
    {5, 5, 6, 6, 6, 6, 7, 7, 7, 8, 8, 8, 8, 9, 9, 9, 9, 9, 9, 9},
    {5, 5, 6, 6, 6, 6, 7, 7, 7, 7, 8, 8, 8, 8, 9, 9, 9, 9, 9, 9},
    {10, 10, 11, 11, 11, 12, 12, 12, 13, 13, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14},
    {10, 10, 11, 11, 11, 12, 12, 12, 13, 13, 13, 14, 14, 14, 14, 14, 14, 14, 14, 14},
    {10, 10, 11, 11, 11, 12, 12, 12, 12, 13, 13, 13, 14, 14, 14, 14, 14, 14, 14, 14},
    {10, 10, 11, 11, 11, 11, 12, 12, 12, 13, 13, 13, 13, 14, 14, 14, 14, 14, 14, 14},
    {10, 10, 11, 11, 11, 11, 12, 12, 12, 12, 13, 13, 13, 13, 14, 14, 14, 14, 14, 14},
    {10, 10, 11, 11, 11, 11, 12, 12, 12, 12, 12, 13, 13, 13, 13, 14, 14, 14, 14, 14},
    {10, 10, 10, 11, 11, 11, 11, 12, 12, 12, 12, 13, 13, 13, 13, 14, 14, 14, 14, 14},
    {10, 10, 10, 11, 11, 11, 11, 12, 12, 12, 12, 12, 13, 13, 13, 13, 14, 14, 14, 14},
    {10, 10, 10, 11, 11, 11, 11, 12, 12, 12, 12, 12, 13, 13, 13, 13, 13, 14, 14, 14},
    {10, 10, 10, 11, 11, 11, 11, 11, 12, 12, 12, 12, 12, 13, 13, 13, 13, 13, 14, 14},
    {10, 10, 10, 11, 11, 11, 11, 11, 12, 12, 12, 12, 12, 12, 13, 13, 13, 13, 13, 14},
    {10, 10, 10, 11, 11, 11, 11, 11, 11, 12, 12, 12, 12, 12, 12, 13, 13, 13, 13, 13},
};

// The lump for a gun's pose (0-14): gaussp00-24 and the rest, in their lump order.
std::string pose(int weapon, int f, bool spin) {
    char b[32];
    f = std::clamp(f, 0, 14);
    switch (weapon) {
    case 0: std::snprintf(b, sizeof(b), "gaussp%d%d", f / 5, f % 5); break;
    case 1: std::snprintf(b, sizeof(b), "%s%d%d", spin ? "chainf" : "chaing", f / 5, f % 5); break;
    case 2: std::snprintf(b, sizeof(b), "alpstl%d%d", f / 5, f % 5); break;
    case 3: std::snprintf(b, sizeof(b), "dbgn%02d", f * 2 + 1); break;   // the lumps run dbgn01, 03 ... 29
    default: std::snprintf(b, sizeof(b), "lnch%02d", f + 1); break;
    }
    return b;
}

const char* kHand[10] = {"joyanim1", "joyanim2", "joyanim3", "joymidcl", "joyfwd", "joyfwdcl", "joyrgt", "joyrgtcl", "joylft", "joylftcl"};

// The gloves (FUN_00051ce6): per swing step, the frame and where it stands (x from 160, y from
// 199, both added to the frame's anchor). Steps change every 5 ticks; the blow lands at step 3.
struct GloveStep { int frame, dx, dy; };
const GloveStep kSabre[5] = {{0, 0, 21}, {0, -30, 35}, {1, 0, 5}, {2, 0, 5}, {2, 60, 65}};
const GloveStep kNano[5] = {{0, 0, 4}, {0, -30, 64}, {1, 0, 34}, {2, 0, 134}, {2, 60, 164}};

int truncDiv(int a, int b) { return a / b; }   // C division rounds toward zero, as the game's sar sums do

}  // namespace

void FirstPerson::reset() {
    shots_ = -1;
    recoil_ = recoilVel_ = 0;
    flashes_.clear();
    haveLast_ = false;
    rx_ = 160;
    ry_ = 100;
    yawRate_ = pitchRate_ = 0;
}

void FirstPerson::gunPlace(int weapon, float& x, float& y, int& aim) const {
    const GunDef& g = kGuns[std::clamp(weapon, 0, 7)];
    int rx = (int)std::lround(rx_), ry = (int)std::lround(ry_);
    int s7 = 200 - g.yoff, s11 = 200 - (g.yoff >> g.ysh2);
    int gy = s7 < ry ? s11 + ((ry - s7) >> g.ysh2) : s11 - ((s7 - ry) >> g.ysh1);
    bool mirror = rx > 159;
    int gx = mirror ? ((rx - 160) >> g.xsh) + 160 : 160 - ((160 - rx) >> g.xsh);
    int row = std::clamp((gy - ry) >> 3, 0, 24), col = std::clamp((mirror ? rx - gx : gx - rx) >> 3, 0, 19);
    aim = kAim[row][col] | (mirror ? 0x80 : 0);
    int r = (int)recoil_;
    x = (float)(gx + truncDiv((160 - gx) * r, 64));
    y = (float)gy + drop_ + r;
}

void FirstPerson::update(float dt, const PlayerState& p, float yaw, float pitch, float worldTime) {
    // The leaning reticle: how fast the view turns, smoothed, about 28 game pixels per radian a
    // second, settling back in a tenth of a second or so.
    if (haveLast_ && dt > 0) {
        float dy = std::remainder(yaw - lastYaw_, 6.2831853f), dp = pitch - lastPitch_;
        float k = std::min(1.0f, dt * 15);
        yawRate_ += (dy / dt - yawRate_) * k;
        pitchRate_ += (dp / dt - pitchRate_) * k;
    }
    lastYaw_ = yaw;
    lastPitch_ = pitch;
    haveLast_ = true;
    float tx = 160 + std::clamp(yawRate_ * 28, -110.0f, 110.0f), ty = 100 - std::clamp(pitchRate_ * 28, -60.0f, 60.0f);
    float e = std::min(1.0f, dt * 12);
    rx_ += (tx - rx_) * e;
    ry_ += (ty - ry_) * e;

    alive_ = p.alive;
    drop_ = p.alive ? 0 : p.deadTime * 50;   // FUN_00036b53: e4186 grows by the 50 Hz ticks
    weapon_ = std::clamp(p.weapon, 0, 8);
    suit_ = std::clamp(p.suit, 0, 2);
    firing_ = p.firing;
    swing_ = p.swingAt > -50 && worldTime - p.swingAt < 0.25f ? worldTime - p.swingAt : -1;

    // New shots: the kick (its speed grows by the weapon's recoil) and a flash at the muzzle.
    if (shots_ < 0 || p.shots < shots_) shots_ = p.shots;
    while (shots_ < p.shots) {
        ++shots_;
        if (weapon_ >= 8) continue;
        const GunDef& g = kGuns[weapon_];
        recoilVel_ += g.recoil;
        if (g.flashN > 0) {
            Flash f;
            f.weapon = weapon_;
            gunPlace(weapon_, f.x, f.y, f.aim);
            flashes_.push_back(f);
        }
    }
    // The kick, stepped as the game steps it each frame (taken here at 25 frames a second, two
    // 50 Hz ticks each): the gun drops by speed x 2 / 32, the speed falls by 10.
    recoilClock_ += dt;
    while (recoilClock_ >= 0.04f) {
        recoilClock_ -= 0.04f;
        if (recoil_ != 0 || recoilVel_ != 0) {
            recoil_ += (float)truncDiv((int)recoilVel_ * 2, 32);
            if (recoil_ > 32) { recoil_ = 32; recoilVel_ = 0; }
            if (recoil_ < 0) { recoil_ = 0; recoilVel_ = 0; }
            else recoilVel_ -= 10;
        }
        // Frame-by-frame animation: the hand's idle twitch and the saw's blade cycle, the
        // chaingun's barrels turn while fire is held.
        anim_ = (anim_ + 1) % 3;
        spin_ = weapon_ == 1 && firing_ ? !spin_ : false;
    }
    for (size_t i = 0; i < flashes_.size();) {
        Flash& f = flashes_[i];
        const GunDef& g = kGuns[f.weapon];
        f.clock += dt;
        while (f.clock >= g.tics / 100.0f) { f.clock -= g.tics / 100.0f; ++f.step; }
        if (f.step >= g.flashN || f.weapon != weapon_) flashes_.erase(flashes_.begin() + i);
        else ++i;
    }
}

void FirstPerson::draw(Hud& hud, int w, int h, bool tall) const {
    float sy = h / 200.0f, sx = tall ? h / 240.0f : sy;
    auto X = [&](float x) { return w * 0.5f + (x - 160) * sx; };
    auto Y = [&](float y) { return y * sy; };
    if (weapon_ == 8) {   // the glove
        int step = swing_ >= 0 ? std::min(4, (int)(swing_ * 100) / 5) : 0;
        int frame, dx = 0, dy;
        if (suit_ == 1) {   // the razor saw's blade turns, faster while it cuts
            bool cutting = firing_ || swing_ >= 0;
            frame = (cutting ? 4 : 0) + (anim_ & 1);
            dy = 40;
        } else {
            const GloveStep& s = (suit_ == 0 ? kSabre : kNano)[step];
            frame = s.frame;
            dx = s.dx;
            dy = s.dy;
        }
        char name[32];
        std::snprintf(name, sizeof(name), "glove%d%d", suit_ + 1, frame);
        hud.sprite(name, X(160.0f + dx), Y(199.0f + dy + drop_), sx, sy);
        return;
    }
    float x, y;
    int aim;
    gunPlace(weapon_, x, y, aim);
    if (weapon_ >= 5) {   // the suit's weapons: your hand on the control stick
        bool click = firing_;
        int f;
        if (yawRate_ > 0.8f) f = click ? 5 : 4;
        else if (yawRate_ < -0.8f) f = click ? 7 : 6;
        else if (pitchRate_ > 0.8f) f = click ? 9 : 8;
        else f = click ? 3 : anim_;
        hud.sprite(kHand[f], X(x), Y(y + 30), sx, sy);
        return;
    }
    // Muzzle flashes first (the gun is drawn over them), where the gun was when it fired.
    for (const Flash& f : flashes_) {
        const GunDef& g = kGuns[f.weapon];
        if (g.mode == 0) {
            hud.sprite(g.flash[f.step], X(f.x), Y(f.y), sx, sy, false, g.tint && f.step ? 0.55f : 1.0f);
        } else {
            int lump = f.step * 2, a = f.aim & 0x7f;
            bool side = (a >= 2 && a <= 4) || (a >= 7 && a <= 9) || a >= 12;
            if (f.step == g.plain) hud.sprite(g.flash[lump], X(f.x), Y(f.y), sx, sy, false, g.tint && f.step ? 0.55f : 1.0f);
            else hud.sprite(g.flash[lump + (side ? 1 : 0)], X(f.x), Y(f.y), sx, sy, (f.aim & 0x80) != 0);
        }
    }
    hud.sprite(pose(weapon_, aim & 0x7f, spin_), X(x), Y(y), sx, sy, (aim & 0x80) != 0);
}
