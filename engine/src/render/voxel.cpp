#include "voxel.h"

#include <algorithm>
#include <array>
#include <unordered_map>
#include <cmath>
#include <cstring>

namespace {

const float kPi = 3.14159265358979f;
float gPal[256][3];
bool gHavePal = false;

// Nearest palette entry among the colours a model's art actually uses.
uint8_t nearest(const float rgb[3], const std::vector<uint8_t>& allowed) {
    float best = 1e30f;
    uint8_t bi = allowed.empty() ? 1 : allowed[0];
    for (uint8_t c : allowed) {
        float dr = gPal[c][0] - rgb[0], dg = gPal[c][1] - rgb[1], db = gPal[c][2] - rgb[2];
        float d = 0.3f * dr * dr + 0.59f * dg * dg + 0.11f * db * db;
        if (d < best) { best = d; bi = c; }
    }
    return bi;
}

SpriteImage mirrored(const SpriteImage& s) {
    SpriteImage m = s;
    for (int y = 0; y < s.h; ++y)
        for (int x = 0; x < s.w; ++x) m.px[(size_t)y * s.w + x] = s.px[(size_t)y * s.w + (s.w - 1 - x)];
    m.ox = s.w - s.ox;
    return m;
}

// Dense occupancy grid centred on the model axis: x, z in [-R, R), y in [0, H).
struct Grid {
    int R = 0, H = 0, S = 0;
    std::vector<uint8_t> v;
    void init(int r, int h) { R = r; H = h; S = 2 * r; v.assign((size_t)S * S * H, 0); }
    size_t idx(int x, int y, int z) const { return ((size_t)y * S + (z + R)) * S + (x + R); }
    bool in(int x, int y, int z) const { return x >= -R && x < R && z >= -R && z < R && y >= 0 && y < H; }
    uint8_t get(int x, int y, int z) const { return in(x, y, z) ? v[idx(x, y, z)] : 0; }
};

const int kN6[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};

bool isSurface(const Grid& g, int x, int y, int z) {
    for (const auto& d : kN6)
        if (!g.get(x + d[0], y + d[1], z + d[2])) return true;
    return false;
}

// Drop specks: keep 6-connected components with at least minSize voxels (always keep the largest).
void removeSpecks(Grid& g, int minSize) {
    std::vector<int> label(g.v.size(), 0);
    std::vector<size_t> stack, members;
    int next = 0;
    size_t largest = 0;
    std::vector<std::vector<size_t>> comps;
    for (size_t i = 0; i < g.v.size(); ++i) {
        if (!g.v[i] || label[i]) continue;
        ++next;
        members.clear();
        stack.push_back(i);
        label[i] = next;
        while (!stack.empty()) {
            size_t c = stack.back();
            stack.pop_back();
            members.push_back(c);
            int x = (int)(c % g.S) - g.R, z = (int)((c / g.S) % g.S) - g.R, y = (int)(c / ((size_t)g.S * g.S));
            for (const auto& d : kN6) {
                int nx = x + d[0], ny = y + d[1], nz = z + d[2];
                if (!g.in(nx, ny, nz)) continue;
                size_t n = g.idx(nx, ny, nz);
                if (g.v[n] && !label[n]) { label[n] = next; stack.push_back(n); }
            }
        }
        largest = std::max(largest, members.size());
        comps.push_back(members);
    }
    for (const auto& c : comps)
        if ((int)c.size() < minSize && c.size() != largest)
            for (size_t i : c) g.v[i] = 0;
}

// Outward normal estimate from the occupancy around a voxel.
void normalAt(const Grid& g, int x, int y, int z, float n[3]) {
    n[0] = n[1] = n[2] = 0;
    for (int dy = -2; dy <= 2; ++dy)
        for (int dz = -2; dz <= 2; ++dz)
            for (int dx = -2; dx <= 2; ++dx)
                if (!g.get(x + dx, y + dy, z + dz)) { n[0] += dx; n[1] += dy; n[2] += dz; }
    float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (l > 0) { n[0] /= l; n[1] /= l; n[2] /= l; }
}

// Gives uncoloured voxels (c == 0) the colour of a coloured neighbour, spreading until all are filled.
void fillColours(std::vector<Voxel>& vox, const Grid& g) {
    Grid cg;
    cg.init(g.R, g.H);
    for (const auto& v : vox) cg.v[cg.idx(v.x, v.y, v.z)] = v.c;
    for (int pass = 0; pass < 16; ++pass) {
        bool missing = false;
        for (auto& v : vox) {
            if (v.c) continue;
            for (int dy = -1; dy <= 1 && !v.c; ++dy)
                for (int dz = -1; dz <= 1 && !v.c; ++dz)
                    for (int dx = -1; dx <= 1 && !v.c; ++dx) {
                        uint8_t c = cg.get(v.x + dx, v.y + dy, v.z + dz);
                        if (c) v.c = c;
                    }
            if (v.c) cg.v[cg.idx(v.x, v.y, v.z)] = v.c;
            else missing = true;
        }
        if (!missing) break;
    }
    vox.erase(std::remove_if(vox.begin(), vox.end(), [](const Voxel& v) { return v.c == 0; }), vox.end());
}

VoxelModel finish(Grid& g, std::vector<Voxel> vox) {
    VoxelModel m;
    fillColours(vox, g);
    // Stand the model on the ground even if carving trimmed thin feet.
    int minY = 1 << 20;
    for (const auto& v : vox) minY = std::min(minY, (int)v.y);
    if (minY > 0 && minY < (1 << 20))
        for (auto& v : vox) v.y = (int16_t)(v.y - minY);
    m.vox = std::move(vox);
    for (const auto& v : m.vox) {
        m.height = std::max(m.height, v.y + 1);
        float r = std::sqrt((v.x + 0.5f) * (v.x + 0.5f) + (v.z + 0.5f) * (v.z + 0.5f));
        m.radius = std::max(m.radius, r);
    }
    return m;
}

}  // namespace

void setVoxelPalette(const uint8_t* pal) {
    for (int i = 0; i < 256; ++i)
        for (int c = 0; c < 3; ++c) {
            int v = pal[i * 3 + c] & 63;
            gPal[i][c] = (v * 4 + v / 16) / 255.0f;
        }
    gHavePal = true;
}

namespace {

struct View {
    SpriteImage img;
    std::vector<uint8_t> sil;    // silhouette grown by one pixel (tolerates small misalignments)
    float rx, rz, fx, fz;        // screen-right and toward-viewer directions in model space
    std::vector<int> col;        // projected column for every (x, z) cell, before the shift
    int shift = 0;
};

struct Carve {
    std::vector<View> views;
    Grid g;
    int R = 0, H = 0, S = 0;
    std::vector<Voxel> surface;
    std::vector<std::vector<float>> zbuf;  // per view: nearest surface depth per (row, column)
};

// Visual hull of the 8 views (5-view sets are completed by mirroring).
bool carveCore(const std::vector<const SpriteImage*>& rots, Carve& out) {
    std::vector<View>& views = out.views;
    int n = (int)rots.size();
    if (n != 5 && n != 8) return false;
    for (int k = 0; k < 8; ++k) {
        View v;
        if (n == 8) v.img = *rots[k];
        else v.img = k <= 4 ? *rots[k] : mirrored(*rots[8 - k]);
        if (!v.img.valid()) return false;
        float a = k * kPi / 4;
        v.rx = std::cos(a); v.rz = -std::sin(a);
        v.fx = std::sin(a); v.fz = std::cos(a);
        const SpriteImage& s = v.img;
        v.sil.assign((size_t)s.w * s.h, 0);
        for (int y = 0; y < s.h; ++y)
            for (int x = 0; x < s.w; ++x)
                v.sil[(size_t)y * s.w + x] = s.at(x, y) || s.at(x - 1, y) || s.at(x + 1, y) || s.at(x, y - 1) || s.at(x, y + 1);
        views.push_back(std::move(v));
    }
    int R = 0, H = 0;
    for (const auto& v : views) {
        R = std::max(R, std::max(v.img.ox, v.img.w - v.img.ox) + 4);
        H = std::max(H, v.img.h);
    }
    H = std::min(H, 255);
    Grid& g = out.g;
    g.init(R, H);
    const int S = g.S;
    out.R = R; out.H = H; out.S = S;
    for (auto& v : views) {
        v.col.resize((size_t)S * S);
        for (int z = -R; z < R; ++z)
            for (int x = -R; x < R; ++x)
                v.col[(size_t)(z + R) * S + (x + R)] = (int)std::floor((x + 0.5f) * v.rx + (z + 0.5f) * v.rz + v.img.ox);
    }
    auto inside = [&](const View& v, int y, size_t cell, int shift) -> int {
        int c = v.col[cell] + shift;
        return (c >= 0 && c < v.img.w && y < v.img.h) ? v.sil[(size_t)y * v.img.w + c] : 0;
    };

    // Votes per voxel; refine each view's horizontal shift to maximise the carved volume.
    std::vector<uint8_t> votes((size_t)S * S * H, 0);
    for (const auto& v : views)
        for (int y = 0; y < H; ++y)
            for (size_t cell = 0; cell < (size_t)S * S; ++cell) votes[(size_t)y * S * S + cell] += (uint8_t)inside(v, y, cell, 0);
    std::vector<uint32_t> cand;
    for (int pass = 0; pass < 2; ++pass)
        for (auto& v : views) {
            cand.clear();
            for (int y = 0; y < H; ++y)
                for (size_t cell = 0; cell < (size_t)S * S; ++cell) {
                    size_t i = (size_t)y * S * S + cell;
                    if (votes[i] - inside(v, y, cell, v.shift) == 7) cand.push_back((uint32_t)i);
                }
            int best = v.shift;
            long bestCount = -1;
            for (int s = -3; s <= 3; ++s) {
                long count = 0;
                for (uint32_t i : cand) count += inside(v, (int)(i / ((size_t)S * S)), i % ((size_t)S * S), s);
                if (count > bestCount || (count == bestCount && std::abs(s) < std::abs(best))) { bestCount = count; best = s; }
            }
            if (best != v.shift) {
                for (int y = 0; y < H; ++y)
                    for (size_t cell = 0; cell < (size_t)S * S; ++cell) {
                        size_t i = (size_t)y * S * S + cell;
                        votes[i] = (uint8_t)(votes[i] - inside(v, y, cell, v.shift) + inside(v, y, cell, best));
                    }
                v.shift = best;
            }
        }
    for (size_t i = 0; i < votes.size(); ++i) g.v[i] = votes[i] == 8;
    removeSpecks(g, 12);

    // Surface voxels, coloured from the view that faces them most squarely and actually sees them.
    std::vector<Voxel>& vox = out.surface;
    for (int y = 0; y < H; ++y)
        for (int z = -R; z < R; ++z)
            for (int x = -R; x < R; ++x)
                if (g.v[g.idx(x, y, z)] && isSurface(g, x, y, z)) vox.push_back({(int16_t)x, (int16_t)y, (int16_t)z, 0});
    std::vector<std::vector<float>>& zbuf = out.zbuf;
    zbuf.assign(8, {});
    for (int k = 0; k < 8; ++k) {
        const View& v = views[k];
        zbuf[k].assign((size_t)v.img.w * H, -1e9f);
        for (const auto& p : vox) {
            int c = v.col[(size_t)(p.z + R) * S + (p.x + R)] + v.shift;
            if (c < 0 || c >= v.img.w) continue;
            float d = (p.x + 0.5f) * v.fx + (p.z + 0.5f) * v.fz;
            float& zb = zbuf[k][(size_t)p.y * v.img.w + c];
            zb = std::max(zb, d);
        }
    }
    return true;
}

}  // namespace

VoxelModel carveRotations(const std::vector<const SpriteImage*>& rots) {
    Carve cv;
    if (!carveCore(rots, cv)) return VoxelModel();
    std::vector<View>& views = cv.views;
    Grid& g = cv.g;
    const int R = cv.R, S = cv.S;
    std::vector<Voxel> vox = cv.surface;
    std::vector<std::vector<float>>& zbuf = cv.zbuf;
    // Colours: blend what every view that sees the voxel says about it, weighted toward the views
    // that face it squarely, then snap back to the art's own palette entries. This keeps each
    // surface coherent where neighbouring rotations disagree by a pixel or two.
    std::vector<uint8_t> allowed;
    {
        std::vector<int> used(256, 0);
        for (const auto& v : views)
            for (uint8_t c : v.img.px) used[c]++;
        for (int c = 1; c < 256; ++c)
            if (used[c] > 1) allowed.push_back((uint8_t)c);
    }
    for (auto& p : vox) {
        float nrm[3];
        normalAt(g, p.x, p.y, p.z, nrm);
        float acc[3] = {0, 0, 0}, wsum = 0, bestW = -2;
        uint8_t bestC = 0;
        for (int k = 0; k < 8; ++k) {
            const View& v = views[k];
            float facing = nrm[0] * v.fx + nrm[2] * v.fz;
            int c = v.col[(size_t)(p.z + R) * S + (p.x + R)] + v.shift;
            if (c < 0 || c >= v.img.w) continue;
            float d = (p.x + 0.5f) * v.fx + (p.z + 0.5f) * v.fz;
            if (d < zbuf[k][(size_t)p.y * v.img.w + c] - 1.5f) continue;  // hidden in this view
            uint8_t col = v.img.at(c, p.y);
            if (!col) {
                const int nb[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
                for (const auto& o : nb)
                    if (!col) col = v.img.at(c + o[0], p.y + o[1]);
            }
            if (!col) continue;
            if (facing > bestW) { bestW = facing; bestC = col; }
            float w = std::max(0.0f, facing + 0.1f);
            w = w * w;
            w = w * w;
            if (w <= 0) continue;
            for (int a = 0; a < 3; ++a) acc[a] += gPal[col][a] * w;
            wsum += w;
        }
        if (gHavePal && wsum > 0) {
            for (float& a : acc) a /= wsum;
            p.c = nearest(acc, allowed);
        } else {
            p.c = bestC;
        }
    }
    return finish(g, std::move(vox));
}

VoxelModel inflateSprite(const SpriteImage& s, int maxHalf) {
    if (!s.valid()) return VoxelModel();
    // Chamfer (3-4) distance from each opaque pixel to the nearest transparent one.
    const int W = s.w + 2, Hh = s.h + 2;
    std::vector<int> d((size_t)W * Hh, 0);
    for (int y = 0; y < s.h; ++y)
        for (int x = 0; x < s.w; ++x)
            if (s.at(x, y)) d[(size_t)(y + 1) * W + x + 1] = 1 << 20;
    auto at = [&](int x, int y) -> int& { return d[(size_t)y * W + x]; };
    for (int y = 1; y < Hh - 1; ++y)
        for (int x = 1; x < W - 1; ++x)
            if (at(x, y)) at(x, y) = std::min({at(x, y), at(x - 1, y) + 3, at(x, y - 1) + 3, at(x - 1, y - 1) + 4, at(x + 1, y - 1) + 4});
    for (int y = Hh - 2; y >= 1; --y)
        for (int x = W - 2; x >= 1; --x)
            if (at(x, y)) at(x, y) = std::min({at(x, y), at(x + 1, y) + 3, at(x, y + 1) + 3, at(x + 1, y + 1) + 4, at(x - 1, y + 1) + 4});
    int R = std::max({s.ox, s.w - s.ox, maxHalf}) + 2;
    int H = std::min(s.h, 255);
    Grid g;
    g.init(R, H);
    Grid colour;
    colour.init(R, H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < s.w; ++x) {
            uint8_t c = s.at(x, y);
            if (!c) continue;
            float dist = at(x + 1, y + 1) / 3.0f;
            int half = std::clamp((int)std::lround(1.6f * std::sqrt(dist)), 1, maxHalf);
            int mx = x - s.ox;
            if (mx < -R || mx >= R) continue;
            for (int z = -half; z < half; ++z) {
                g.v[g.idx(mx, y, z)] = 1;
                colour.v[colour.idx(mx, y, z)] = c;
            }
        }
    std::vector<Voxel> vox;
    for (int y = 0; y < H; ++y)
        for (int z = -R; z < R; ++z)
            for (int x = -R; x < R; ++x)
                if (g.v[g.idx(x, y, z)] && isSurface(g, x, y, z))
                    vox.push_back({(int16_t)x, (int16_t)y, (int16_t)z, colour.v[colour.idx(x, y, z)]});
    return finish(g, std::move(vox));
}

bool looksSymmetric(const SpriteImage& s) {
    if (!s.valid()) return false;
    long both = 0, either = 0;
    for (int y = 0; y < s.h; ++y)
        for (int x = 0; x < s.w; ++x) {
            bool a = s.at(x, y) != 0;
            bool b = s.at(2 * s.ox - 1 - x, y) != 0;
            both += a && b;
            either += a || b;
        }
    return either > 0 && both >= 0.86 * either;
}

VoxelModel revolveSprite(const SpriteImage& s) {
    if (!s.valid()) return VoxelModel();
    int H = std::min(s.h, 255);
    int R = std::max(s.ox, s.w - s.ox) + 2;
    Grid g;
    g.init(R, H);
    std::vector<float> radius(H, 0);
    for (int y = 0; y < H; ++y) {
        int L = -1, Rr = -1;
        for (int x = 0; x < s.w; ++x)
            if (s.at(x, y)) { if (L < 0) L = x; Rr = x; }
        if (L < 0) continue;
        radius[y] = (Rr + 1 - L) * 0.5f;
        float r2 = radius[y] * radius[y];
        for (int z = -R; z < R; ++z)
            for (int x = -R; x < R; ++x)
                if ((x + 0.5f) * (x + 0.5f) + (z + 0.5f) * (z + 0.5f) <= r2) g.v[g.idx(x, y, z)] = 1;
    }
    std::vector<Voxel> vox;
    for (int y = 0; y < H; ++y)
        for (int z = -R; z < R; ++z)
            for (int x = -R; x < R; ++x) {
                if (!g.v[g.idx(x, y, z)] || !isSurface(g, x, y, z)) continue;
                // Every quarter turn shows the centre of the original art, the way the billboard
                // looked the same from every side in 1994.
                float px = x + 0.5f, pz = z + 0.5f;
                float r = std::sqrt(px * px + pz * pz);
                float th = std::atan2(px, pz);
                float q = std::fmod(th + kPi * 2.25f, kPi * 0.5f) - kPi * 0.25f;
                int c = (int)std::floor(s.ox + r * std::sin(q));
                uint8_t col = s.at(c, y);
                for (int k = 1; !col && k < 4; ++k) col = s.at(c - k * (c > s.ox ? 1 : -1), y);
                vox.push_back({(int16_t)x, (int16_t)y, (int16_t)z, col});
            }
    return finish(g, std::move(vox));
}

VoxelModel makeModel(const SpriteImage& s) {
    return looksSymmetric(s) ? revolveSprite(s) : inflateSprite(s);
}

namespace {

// Nearest-voxel lookup: voxels bucketed in 4x4x4 blocks, searched in growing shells.
struct NearIndex {
    int B = 4;
    int x0 = 0, y0 = 0, z0 = 0, sx = 0, sy = 0, sz = 0;
    std::vector<int> start, items;  // bucket -> [start[b], start[b+1]) in items
    const VoxelModel* m = nullptr;
    void build(const VoxelModel& model, int bucketSize = 4) {
        B = bucketSize;
        m = &model;
        int x1 = -(1 << 20), y1 = x1, z1 = x1;
        x0 = y0 = z0 = 1 << 20;
        for (const auto& v : model.vox) {
            x0 = std::min(x0, (int)v.x); y0 = std::min(y0, (int)v.y); z0 = std::min(z0, (int)v.z);
            x1 = std::max(x1, (int)v.x); y1 = std::max(y1, (int)v.y); z1 = std::max(z1, (int)v.z);
        }
        sx = (x1 - x0) / B + 1; sy = (y1 - y0) / B + 1; sz = (z1 - z0) / B + 1;
        std::vector<int> count((size_t)sx * sy * sz + 1, 0);
        auto bucket = [&](const Voxel& v) { return ((size_t)((v.y - y0) / B) * sz + (v.z - z0) / B) * sx + (v.x - x0) / B; };
        for (const auto& v : model.vox) ++count[bucket(v) + 1];
        for (size_t i = 1; i < count.size(); ++i) count[i] += count[i - 1];
        start = count;
        items.assign(model.vox.size(), 0);
        std::vector<int> fill(count.begin(), count.end() - 1);
        for (size_t i = 0; i < model.vox.size(); ++i) items[fill[bucket(model.vox[i])]++] = (int)i;
    }
    int maxSearch = 0;  // once something is found, stop looking further than this
    int find(int x, int y, int z, int colour = -1, float colourWeight = 0) const {
        int bx = (int)std::floor((float)(x - x0) / B), by = (int)std::floor((float)(y - y0) / B), bz = (int)std::floor((float)(z - z0) / B);
        int best = -1;
        float bestD = 1e30f;
        int maxR = std::max({sx, sy, sz}) + std::max({std::abs(bx), std::abs(by), std::abs(bz)}) + 1;
        for (int r = 0; r <= maxR; ++r) {
            if (best >= 0 && bestD <= (float)((r - 1) * B) * ((r - 1) * B)) break;
            if (best >= 0 && maxSearch > 0 && r * B > maxSearch) break;
            for (int dy = -r; dy <= r; ++dy) {
                int qy = by + dy;
                if (qy < 0 || qy >= sy) continue;
                for (int dz = -r; dz <= r; ++dz) {
                    int qz = bz + dz;
                    if (qz < 0 || qz >= sz) continue;
                    bool edge = std::abs(dy) == r || std::abs(dz) == r;
                    for (int dx = -r; dx <= r; dx += (edge ? 1 : 2 * r > 0 ? 2 * r : 1)) {
                        int qx = bx + dx;
                        if (qx >= 0 && qx < sx) {
                            size_t b = ((size_t)qy * sz + qz) * sx + qx;
                            for (int k = start[b]; k < start[b + 1]; ++k) {
                                const Voxel& v = m->vox[items[k]];
                                float ddx = (float)(v.x - x), ddy = (float)(v.y - y), ddz = (float)(v.z - z);
                                float d = ddx * ddx + 2 * ddy * ddy + ddz * ddz;  // prefer partners at the same height
                                if (colour >= 0 && d < bestD) {
                                    float cr = gPal[v.c][0] - gPal[colour][0], cg = gPal[v.c][1] - gPal[colour][1], cb = gPal[v.c][2] - gPal[colour][2];
                                    d += colourWeight * (cr * cr + cg * cg + cb * cb);
                                }
                                if (d < bestD) { bestD = d; best = items[k]; }
                            }
                        }
                        if (r == 0) break;
                    }
                }
            }
        }
        return best;
    }
};

MorphVoxel pack(const Voxel& f, const Voxel& t) {
    auto cx = [](int v) { return (uint8_t)std::clamp(v + 128, 0, 255); };
    auto cy = [](int v) { return (uint8_t)std::clamp(v, 0, 255); };
    return {cx(f.x), cy(f.y), cx(f.z), f.c, cx(t.x), cy(t.y), cx(t.z), t.c};
}

}  // namespace

std::vector<MorphVoxel> morph(const VoxelModel& a, const VoxelModel& b) {
    std::vector<MorphVoxel> out;
    if (a.empty() || b.empty()) return a.empty() ? still(b) : still(a);
    NearIndex ia, ib;
    ia.build(a);
    ib.build(b);
    std::vector<uint8_t> used(a.vox.size(), 0);
    out.reserve(a.vox.size() + b.vox.size());
    for (const auto& v : b.vox) {
        int i = ia.find(v.x, v.y, v.z);
        if (i < 0) continue;
        used[i] = 1;
        out.push_back(pack(a.vox[i], v));
    }
    for (size_t i = 0; i < a.vox.size(); ++i) {
        if (used[i]) continue;
        const Voxel& v = a.vox[i];
        int j = ib.find(v.x, v.y, v.z);
        if (j < 0) continue;
        out.push_back(pack(v, b.vox[j]));
    }
    return out;
}

VoxelModel downsample(const VoxelModel& m) {
    VoxelModel out;
    std::unordered_map<int, std::array<int, 2>> blocks;  // key -> (index in out, count)
    for (const auto& v : m.vox) {
        int bx = v.x >> 1, by = v.y >> 1, bz = v.z >> 1;
        int key = (bx + 512) | ((by + 512) << 10) | ((bz + 512) << 20);
        auto it = blocks.find(key);
        if (it == blocks.end()) {
            blocks[key] = {(int)out.vox.size(), 1};
            out.vox.push_back({(int16_t)bx, (int16_t)by, (int16_t)bz, v.c});
        }
    }
    out.height = (m.height + 1) / 2;
    out.radius = m.radius * 0.5f;
    return out;
}

std::vector<MorphVoxel> still(const VoxelModel& a) {
    std::vector<MorphVoxel> out;
    out.reserve(a.vox.size());
    for (const auto& v : a.vox) out.push_back(pack(v, v));
    return out;
}

// ---------------------------------------------------------------------------------------------
// Depth sprites

std::vector<SplatSet> depthSprites(const std::vector<const SpriteImage*>& rots, std::vector<int>* shifts) {
    std::vector<SplatSet> out;
    Carve cv;
    if (!carveCore(rots, cv)) return out;
    out.resize(8);
    if (shifts) {
        shifts->clear();
        for (int k = 0; k < 8; ++k) shifts->push_back(cv.views[k].shift);
    }
    for (int k = 0; k < 8; ++k) {
        const View& v = cv.views[k];
        const SpriteImage& img = v.img;
        const int w = img.w, h = std::min(img.h, cv.H);
        // Depth of the hull's front surface under each opaque pixel; gaps take the nearest known
        // depth (breadth-first), then a 3x3 median keeps neighbouring pixels on one surface.
        std::vector<float> depth((size_t)w * h, 0);
        std::vector<uint8_t> known((size_t)w * h, 0);
        std::vector<int> queue;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                size_t i = (size_t)y * w + x;
                float z = cv.zbuf[k][(size_t)y * w + x];
                if (img.at(x, y) && z > -1e8f) { depth[i] = z; known[i] = 1; queue.push_back((int)i); }
            }
        for (size_t qi = 0; qi < queue.size(); ++qi) {
            int i = queue[qi], x = i % w, y = i / w;
            const int nb[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (const auto& o : nb) {
                int nx = x + o[0], ny = y + o[1];
                if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                size_t j = (size_t)ny * w + nx;
                if (known[j] || !img.at(nx, ny)) continue;
                depth[j] = depth[i];
                known[j] = 1;
                queue.push_back((int)j);
            }
        }
        std::vector<float> smooth = depth;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                if (!img.at(x, y)) continue;
                float vals[9];
                int n = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                        if (img.at(x + dx, y + dy)) vals[n++] = depth[(size_t)(y + dy) * w + (x + dx)];
                std::nth_element(vals, vals + n / 2, vals + n);
                smooth[(size_t)y * w + x] = vals[n / 2];
            }
        // Two box-blur passes so neighbouring pixels sit at nearly the same depth: the sprite stays
        // a continuous surface (no cracks) when seen from above or from between two views.
        for (int pass = 0; pass < 3; ++pass) {
            std::vector<float> src = smooth;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    if (!img.at(x, y)) continue;
                    float sum = 0;
                    int n = 0;
                    for (int dy = -2; dy <= 2; ++dy)
                        for (int dx = -2; dx <= 2; ++dx)
                            if (img.at(x + dx, y + dy)) { sum += src[(size_t)(y + dy) * w + (x + dx)]; ++n; }
                    smooth[(size_t)y * w + x] = sum / n;
                }
        }
        SplatSet& set = out[k];
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                uint8_t c = img.at(x, y);
                if (!c) continue;
                float u = x + 0.5f - (float)(img.ox + v.shift);
                // Depth is softened (x0.7) so thin, spiky details don't scatter when the view turns.
                float d = smooth[(size_t)y * w + x] * 0.7f;
                float X = u * v.rx + d * v.fx, Z = u * v.rz + d * v.fz;
                set.s.push_back({(int16_t)std::lround(X * 4), (int16_t)std::lround((y + 0.5f) * 4), (int16_t)std::lround(Z * 4), c});
            }
    }
    return out;
}

SplatSet flatSprite(const SpriteImage& img) {
    SplatSet set;
    for (int y = 0; y < img.h; ++y)
        for (int x = 0; x < img.w; ++x) {
            uint8_t c = img.at(x, y);
            if (!c) continue;
            set.s.push_back({(int16_t)std::lround((x + 0.5f - img.ox) * 4), (int16_t)std::lround((y + 0.5f) * 4), 0, c});
        }
    return set;
}

namespace {
VoxelModel asModel(const SplatSet& s) {
    VoxelModel m;
    m.vox.reserve(s.s.size());
    for (const auto& p : s.s) m.vox.push_back({p.x, p.y, p.z, p.c});
    return m;
}
SplatMorph packSplat(const Splat& f, const Splat& t) { return {f.x, f.y, f.z, f.c, t.x, t.y, t.z, t.c}; }
}  // namespace

std::vector<SplatMorph> morphSplats(const SplatSet& a, const SplatSet& b) {
    std::vector<SplatMorph> out;
    if (a.s.empty() || b.s.empty()) return stillSplats(a.s.empty() ? b : a);
    VoxelModel ma = asModel(a), mb = asModel(b);
    NearIndex ia, ib;
    ia.build(ma, 8);
    ib.build(mb, 8);
    ia.maxSearch = ib.maxSearch = 40;
    std::vector<uint8_t> used(a.s.size(), 0);
    out.reserve(a.s.size() + b.s.size());
    const float cw = gHavePal ? 200.0f : 0.0f;  // colour similarity, in squared quarter-units
    for (const auto& p : b.s) {
        int i = ia.find(p.x, p.y, p.z, gHavePal ? p.c : -1, cw);
        if (i < 0) continue;
        used[i] = 1;
        out.push_back(packSplat(a.s[i], p));
    }
    for (size_t i = 0; i < a.s.size(); ++i) {
        if (used[i]) continue;
        int j = ib.find(a.s[i].x, a.s[i].y, a.s[i].z, gHavePal ? a.s[i].c : -1, cw);
        if (j < 0) continue;
        out.push_back(packSplat(a.s[i], b.s[j]));
    }
    return out;
}

std::vector<SplatMorph> stillSplats(const SplatSet& a) {
    std::vector<SplatMorph> out;
    out.reserve(a.s.size());
    for (const auto& p : a.s) out.push_back(packSplat(p, p));
    return out;
}
