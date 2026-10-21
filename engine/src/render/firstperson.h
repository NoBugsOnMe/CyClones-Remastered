// The weapon in your hands, as CYCLONE.EXE draws it (FUN_0005185c, FUN_00051ce6, FUN_000520aa):
// the gun art in 15 poses, picked by where the gun is and where it aims; the suit-mounted weapons'
// hand on its control stick; the three gloves' swings; the recoil kick; and the muzzle flashes.
//
// The game's reticle moved freely over the screen and the gun swung round to follow it. Here the
// crosshair stays in the middle, so a "reticle" that leans the way you turn and look (and settles
// back when you stop) drives the same rules: the gun swings and turns with your view, and stands
// as the game drew it when you hold still.
#pragma once
#include <string>
#include <vector>

#include "hud.h"

struct PlayerState;

class FirstPerson {
public:
    // Once per frame, before draw: yaw and pitch are the view's angles (radians), time the world's.
    void update(float dt, const PlayerState& p, float yaw, float pitch, float worldTime);
    // Draws into the HUD pass (w, h: the window; tall: 320x200 shown at 4:3).
    void draw(Hud& hud, int w, int h, bool tall) const;
    void reset();

private:
    struct Flash {
        int weapon = 0;
        float x = 0, y = 0;     // where the gun was (320x200 pixels)
        int aim = 0;            // the gun's pose then (bit 0x80: mirrored)
        int step = 0;           // frames shown so far
        float clock = 0;        // seconds into the current frame
    };
    int weapon_ = 0, suit_ = 0;
    int shots_ = -1;
    float recoil_ = 0, recoilVel_ = 0, recoilClock_ = 0;   // e4199 and its speed, stepped at 25 Hz
    float rx_ = 160, ry_ = 100;                              // the leaning reticle
    float lastYaw_ = 0, lastPitch_ = 0, yawRate_ = 0, pitchRate_ = 0;
    bool haveLast_ = false;
    float animClock_ = 0;
    int anim_ = 0;              // the suit hand's idle frames and the saw's blade, 0-2
    bool spin_ = false;         // the chaingun's barrels (the second set of poses)
    bool firing_ = false;
    float swing_ = -1;          // glove: seconds into the swing (-1: none)
    float drop_ = 0;            // dead: the weapon sinks out of view (50 lines a second)
    bool alive_ = true;
    std::vector<Flash> flashes_;

    void gunPlace(int weapon, float& x, float& y, int& aim) const;
};
