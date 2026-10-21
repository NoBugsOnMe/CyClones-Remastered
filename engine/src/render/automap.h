// The automap: a top-down plan of the level drawn over the game, as a corner minimap that turns
// with the player or as a full-screen map.
//
// The floor is the level's own floor art seen from above (each 64x64 flat shrunk to 16x16 and
// shaded by height and light), so rooms read by their real materials. Walls, ledges and doors
// are crisp vector lines that stay sharp at any zoom. Only explored ground is shown; it fades in
// softly as you see it. The player, creatures and pickups are drawn as markers on top.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "../data/level.h"
#include "gl.h"
#include "renderer.h"

// Static level description the map is built from (prepared by the game world).
struct MapLevel {
    int width = 0, height = 0;                  // playable cells
    std::vector<float> top;                     // kGrid*kGrid: top walkable height (or -1e9: no floor)
    std::vector<int> flat;                      // kGrid*kGrid: flat number of that surface
    std::vector<float> light;                   // kGrid*kGrid: signed cell light (negative = brighter)
    std::vector<float> walls;                   // x0, z0, x1, z1 per solid wall segment
    std::vector<float> ledges;                  // x0, z0, x1, z1, drop per height step without a wall
};

// What changes from frame to frame.
struct MapFrame {
    float px = 0, pz = 0, yaw = 0;              // player position and heading (yaw 0 = north)
    float fov = 1.3f;                           // horizontal field of view (radians), for the view cone
    std::vector<float> doors;                   // x0, z0, x1, z1, open (0..1) per door
    std::vector<float> enemies;                 // x, z, radius per living creature on explored ground
    std::vector<float> items;                   // x, z per pickup on explored ground
    const uint8_t* explored = nullptr;          // kGrid*kGrid, 0..255
};

class Automap {
public:
    enum Mode { Off, Mini, Full };
    bool init(std::string& error);
    void buildLevel(const MapLevel& level, const GameData& data);
    // Draws over the current (default) framebuffer.
    void draw(int width, int height, const MapFrame& f, Mode mode, float zoom, bool revealAll, float time);

private:
    struct V {
        float x, z, ox, oy, u, v, r, g, b, a, fog;
    };
    void quad(float x0, float z0, float x1, float z1, float u0, float v0, float u1, float v1, const float c[4], bool fog);
    void line(float x0, float z0, float x1, float z1, float px, const float c[4], bool fog, float scale);
    void disc(float x, float z, float radiusPx, const float c[4], bool fog, int segments = 14);
    void poly(float x, float z, const float* pts, int n, const float c[4], bool fog);  // pts: pixel offsets, map-aligned

    void buildStatic(float px1);
    GLuint prog_ = 0, vao_ = 0, vbo_ = 0, floorTex_ = 0, maskTex_ = 0;
    GLuint staticVao_ = 0, staticVbo_ = 0;
    int staticCount_ = 0;
    float staticPx_ = -1;
    std::vector<V> v_;
    MapLevel level_;
    std::vector<uint8_t> mask_;
    float minX_ = 0, minZ_ = 0, maxX_ = 0, maxZ_ = 0;  // extent of the level's floor (world units)
};
