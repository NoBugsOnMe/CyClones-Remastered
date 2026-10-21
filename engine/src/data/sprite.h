// CyClones sprite lumps (monsters, objects, items, projectiles).
//   u16 origin_x; u16 width; u16 column_offset[width]   (0 = empty column)
//   column: u8 y_end (inclusive); u8 y_start; (y_end - y_start + 1) palette indices, stored from
//   the top of the post (y_end) downward.
// Rows count up from the ground: y = 0 is where the sprite touches the floor. Index 0 is transparent.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

struct SpriteImage {
    int w = 0, h = 0;
    int ox = 0;                    // column of the vertical axis (the sprite's "origin")
    int oy = 0;                    // anchor row (decals: the hit point), counted from the bottom
    std::vector<uint8_t> px;       // w*h palette indices, px[y*w + x], y = 0 at the ground
    uint8_t at(int x, int y) const { return (x < 0 || y < 0 || x >= w || y >= h) ? 0 : px[(size_t)y * w + x]; }
    bool valid() const { return w > 0 && h > 0; }
};

bool decodeSprite(const uint8_t* data, size_t size, SpriteImage& out);
// Decals (smoke puffs, blood spurts): u16 w, u16 h, u16 anchor_x, u16 anchor_y, then w*h pixels,
// row by row from the top.
bool decodeDecal(const uint8_t* data, size_t size, SpriteImage& out);
