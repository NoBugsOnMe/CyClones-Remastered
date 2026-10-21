#include "sprite.h"

bool decodeSprite(const uint8_t* d, size_t n, SpriteImage& out) {
    out = SpriteImage();
    if (n < 4) return false;
    int ox = d[0] | d[1] << 8, w = d[2] | d[3] << 8;
    if (w <= 0 || w > 1024 || 4 + (size_t)w * 2 > n) return false;
    int h = 0;
    for (int x = 0; x < w; ++x) {
        size_t o = d[4 + x * 2] | d[5 + x * 2] << 8;
        if (!o || o + 2 > n) continue;
        int ye = d[o], ys = d[o + 1];
        if (ye >= ys) h = h > ye + 1 ? h : ye + 1;
    }
    if (h <= 0) return false;
    out.w = w;
    out.h = h;
    out.ox = ox;
    out.px.assign((size_t)w * h, 0);
    for (int x = 0; x < w; ++x) {
        size_t o = d[4 + x * 2] | d[5 + x * 2] << 8;
        if (!o || o + 2 > n) continue;
        int ye = d[o], ys = d[o + 1];
        // Pixels run from the top of the post (y_end) down to y_start.
        for (int j = 0; j <= ye - ys && o + 2 + (size_t)j < n; ++j) out.px[(size_t)(ye - j) * w + x] = d[o + 2 + j];
    }
    return true;
}

bool decodeDecal(const uint8_t* d, size_t n, SpriteImage& out) {
    out = SpriteImage();
    if (n < 8) return false;
    int w = d[0] | d[1] << 8, h = d[2] | d[3] << 8, ax = d[4] | d[5] << 8, ay = d[6] | d[7] << 8;
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024 || 8 + (size_t)w * h > n) return false;
    out.w = w;
    out.h = h;
    out.ox = ax;
    out.oy = h - 1 - ay;
    out.px.assign((size_t)w * h, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) out.px[(size_t)(h - 1 - y) * w + x] = d[8 + (size_t)y * w + x];
    return true;
}
