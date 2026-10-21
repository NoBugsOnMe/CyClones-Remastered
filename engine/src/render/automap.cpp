#include "automap.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

const char* kVS = R"(#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aOff;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aCol;
layout(location = 4) in float aFog;
uniform vec2 uOrigin;    // world x/z at the centre of the view
uniform vec2 uCenter;    // that point on screen (pixels)
uniform float uScale;    // pixels per world unit
uniform vec2 uRot;       // cos/sin of the map's turn (minimap: heading up)
uniform vec2 uViewport;
out vec2 vUV;
out vec4 vCol;
out float vFog;
out vec2 vWorld;
void main() {
    vec2 d = (aPos - uOrigin) * uScale + aOff;
    vec2 s = vec2(uRot.x * d.x - uRot.y * d.y, uRot.y * d.x + uRot.x * d.y);
    vec2 p = uCenter + vec2(s.x, -s.y);
    gl_Position = vec4(p / uViewport * 2.0 - 1.0, 0.0, 1.0);
    vUV = aUV;
    vCol = aCol;
    vFog = aFog;
    vWorld = aPos;
}
)";

const char* kFS = R"(#version 330 core
in vec2 vUV;
in vec4 vCol;
in float vFog;
in vec2 vWorld;
uniform sampler2D uFloor;
uniform sampler2D uMask;
uniform int uReveal;
uniform vec3 uClip;      // circular window: centre and radius in pixels (radius 0 = none)
out vec4 oColor;
void main() {
    vec4 c = vCol;
    if (vUV.x >= 0.0) c *= texture(uFloor, vUV);
    if (vFog > 0.5 && uReveal == 0) {
        float m = texture(uMask, vWorld / 4096.0).r;
        c.a *= smoothstep(0.1, 0.6, m);
    }
    if (uClip.z > 0.0) {
        float d = length(gl_FragCoord.xy - uClip.xy);
        c.a *= 1.0 - smoothstep(uClip.z - 1.5, uClip.z, d);
    }
    if (c.a <= 0.004) discard;
    oColor = c;
}
)";

GLuint compileProgram(std::string& error) {
    auto stage = [&](GLenum type, const char* src) -> GLuint {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[2048];
            glGetShaderInfoLog(s, sizeof(log), nullptr, log);
            error += log;
            return 0;
        }
        return s;
    };
    GLuint v = stage(GL_VERTEX_SHADER, kVS), f = stage(GL_FRAGMENT_SHADER, kFS);
    if (!v || !f) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        error += log;
        return 0;
    }
    return p;
}

const int kTexel = 16;                    // floor texels per cell on the map
const int kFloorSize = kGrid * kTexel;    // 1024

}  // namespace

bool Automap::init(std::string& error) {
    prog_ = compileProgram(error);
    if (!prog_) return false;
    auto setup = [](GLuint& vao, GLuint& vbo) {
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        const GLsizei stride = sizeof(V);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(V, x));
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(V, ox));
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(V, u));
        glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(V, r));
        glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(V, fog));
        for (int i = 0; i < 5; ++i) glEnableVertexAttribArray(i);
        glBindVertexArray(0);
    };
    setup(vao_, vbo_);
    setup(staticVao_, staticVbo_);

    glGenTextures(1, &floorTex_);
    glGenTextures(1, &maskTex_);
    glBindTexture(GL_TEXTURE_2D, maskTex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return true;
}

void Automap::buildLevel(const MapLevel& L, const GameData& data) {
    level_ = L;
    staticPx_ = -1;  // rebuild the floor and wall geometry on the next draw
    float pal[256][3];
    for (int i = 0; i < 256; ++i)
        for (int c = 0; c < 3; ++c) {
            int v = data.palette[i * 3 + c] & 63;
            pal[i][c] = (v * 4 + v / 16) / 255.0f;
        }
    // Height range of the level's floors, for relief shading.
    float lo = 1e9f, hi = -1e9f;
    minX_ = minZ_ = 1e9f;
    maxX_ = maxZ_ = -1e9f;
    for (int z = 0; z < kGrid; ++z)
        for (int x = 0; x < kGrid; ++x) {
            float h = L.top[z * kGrid + x];
            if (h < -1e8f) continue;
            lo = std::min(lo, h);
            hi = std::max(hi, h);
            minX_ = std::min(minX_, (float)x * kCellSize);
            minZ_ = std::min(minZ_, (float)z * kCellSize);
            maxX_ = std::max(maxX_, (float)(x + 1) * kCellSize);
            maxZ_ = std::max(maxZ_, (float)(z + 1) * kCellSize);
        }
    if (minX_ > maxX_) { minX_ = minZ_ = 0; maxX_ = maxZ_ = kGrid * kCellSize; }
    float range = std::max(64.0f, hi - lo);

    std::vector<uint8_t> img((size_t)kFloorSize * kFloorSize * 4, 0);
    const Bol& cd = data.cd;
    for (int z = 0; z < kGrid; ++z)
        for (int x = 0; x < kGrid; ++x) {
            int i = z * kGrid + x;
            float h = L.top[i];
            if (h < -1e8f) continue;
            int lump = data.startFlats + L.flat[i];
            const uint8_t* px = (L.flat[i] > 0 && lump < (int)cd.lumps().size() && cd.size(lump) >= 4096) ? cd.data(lump) : nullptr;
            // Brightness: the cell's light, its height in the level, and a soft light from the
            // north-west so raised ground stands out.
            float lightK = std::clamp(1.0f - 0.022f * L.light[i], 0.5f, 1.25f);
            float heightK = 0.78f + 0.34f * (h - lo) / range;
            float nw = h;
            if (x > 0 && z > 0 && L.top[i - kGrid - 1] > -1e8f) nw = L.top[i - kGrid - 1];
            float relief = 1.0f + std::clamp((h - nw) / 48.0f, -1.0f, 1.0f) * 0.12f;
            float k = lightK * heightK * relief;
            for (int ty = 0; ty < kTexel; ++ty)
                for (int tx = 0; tx < kTexel; ++tx) {
                    float rgb[3] = {0.35f, 0.38f, 0.36f};
                    if (px) {
                        // 4x4 box average of the 64x64 flat.
                        float s[3] = {0, 0, 0};
                        for (int yy = 0; yy < 4; ++yy)
                            for (int xx = 0; xx < 4; ++xx) {
                                int c = px[(ty * 4 + yy) * 64 + tx * 4 + xx];
                                for (int ch = 0; ch < 3; ++ch) s[ch] += pal[c][ch];
                            }
                        for (int ch = 0; ch < 3; ++ch) rgb[ch] = s[ch] / 16.0f;
                    }
                    // Slightly muted so the lines and markers on top read clearly.
                    float grey = (rgb[0] + rgb[1] + rgb[2]) / 3.0f;
                    size_t o = (((size_t)z * kTexel + ty) * kFloorSize + (size_t)x * kTexel + tx) * 4;
                    for (int ch = 0; ch < 3; ++ch) {
                        float v = (grey + (rgb[ch] - grey) * 0.85f) * k * 0.92f;
                        img[o + ch] = (uint8_t)std::clamp((int)std::lround(v * 255), 0, 255);
                    }
                    img[o + 3] = 255;
                }
        }
    glBindTexture(GL_TEXTURE_2D, floorTex_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kFloorSize, kFloorSize, 0, GL_RGBA, GL_UNSIGNED_BYTE, img.data());
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

void Automap::quad(float x0, float z0, float x1, float z1, float u0, float v0, float u1, float v1, const float c[4], bool fog) {
    float f = fog ? 1.0f : 0.0f;
    V a{x0, z0, 0, 0, u0, v0, c[0], c[1], c[2], c[3], f}, b{x1, z0, 0, 0, u1, v0, c[0], c[1], c[2], c[3], f};
    V d{x1, z1, 0, 0, u1, v1, c[0], c[1], c[2], c[3], f}, e{x0, z1, 0, 0, u0, v1, c[0], c[1], c[2], c[3], f};
    v_.insert(v_.end(), {a, b, d, a, d, e});
}

// A line of constant pixel width between two world points, with round-ish caps.
void Automap::line(float x0, float z0, float x1, float z1, float px, const float c[4], bool fog, float scale) {
    float dx = x1 - x0, dz = z1 - z0, l = std::sqrt(dx * dx + dz * dz);
    if (l < 1e-4f) return;
    dx /= l;
    dz /= l;
    float h = px * 0.5f, nx = -dz * h, nz = dx * h, ex = dx * h, ez = dz * h;  // normal and cap offsets (pixels)
    float f = fog ? 1.0f : 0.0f;
    V a{x0, z0, nx - ex, nz - ez, -1, -1, c[0], c[1], c[2], c[3], f};
    V b{x1, z1, nx + ex, nz + ez, -1, -1, c[0], c[1], c[2], c[3], f};
    V d{x1, z1, -nx + ex, -nz + ez, -1, -1, c[0], c[1], c[2], c[3], f};
    V e{x0, z0, -nx - ex, -nz - ez, -1, -1, c[0], c[1], c[2], c[3], f};
    v_.insert(v_.end(), {a, b, d, a, d, e});
    (void)scale;
}

void Automap::disc(float x, float z, float r, const float c[4], bool fog, int n) {
    float f = fog ? 1.0f : 0.0f;
    for (int i = 0; i < n; ++i) {
        float a0 = 6.2831853f * i / n, a1 = 6.2831853f * (i + 1) / n;
        V o{x, z, 0, 0, -1, -1, c[0], c[1], c[2], c[3], f};
        V p{x, z, std::cos(a0) * r, std::sin(a0) * r, -1, -1, c[0], c[1], c[2], c[3], f};
        V q{x, z, std::cos(a1) * r, std::sin(a1) * r, -1, -1, c[0], c[1], c[2], c[3], f};
        v_.insert(v_.end(), {o, p, q});
    }
}

void Automap::poly(float x, float z, const float* pts, int n, const float c[4], bool fog) {
    float f = fog ? 1.0f : 0.0f;
    for (int i = 1; i + 1 < n; ++i) {
        V a{x, z, pts[0], pts[1], -1, -1, c[0], c[1], c[2], c[3], f};
        V b{x, z, pts[i * 2], pts[i * 2 + 1], -1, -1, c[0], c[1], c[2], c[3], f};
        V d{x, z, pts[i * 2 + 2], pts[i * 2 + 3], -1, -1, c[0], c[1], c[2], c[3], f};
        v_.insert(v_.end(), {a, b, d});
    }
}

void Automap::buildStatic(float px1) {
    v_.clear();
    const float white[4] = {1, 1, 1, 1};
    for (int z = 0; z < kGrid; ++z)
        for (int x = 0; x < kGrid; ++x) {
            if (level_.top[z * kGrid + x] < -1e8f) continue;
            float u0 = (float)x / kGrid, v0 = (float)z / kGrid, u1 = (float)(x + 1) / kGrid, v1 = (float)(z + 1) / kGrid;
            quad((float)x * kCellSize, (float)z * kCellSize, (float)(x + 1) * kCellSize, (float)(z + 1) * kCellSize, u0, v0, u1,
                 v1, white, true);
        }

    // Ledges: drops between floors without a wall, drawn as soft dark steps.
    for (size_t i = 0; i + 4 < level_.ledges.size(); i += 5) {
        const float* s = &level_.ledges[i];
        float a = std::clamp(s[4] / 96.0f, 0.25f, 0.7f);
        const float c[4] = {0.0f, 0.02f, 0.04f, a};
        line(s[0], s[1], s[2], s[3], 2 * px1, c, true, 0);
    }
    // Walls: a dark outline under a bright line, so they read on any floor.
    const float wallDark[4] = {0.0f, 0.02f, 0.05f, 0.85f}, wallLit[4] = {0.62f, 0.86f, 1.0f, 1.0f};
    for (size_t i = 0; i + 3 < level_.walls.size(); i += 4) {
        const float* s = &level_.walls[i];
        line(s[0], s[1], s[2], s[3], 4 * px1, wallDark, true, 0);
    }
    for (size_t i = 0; i + 3 < level_.walls.size(); i += 4) {
        const float* s = &level_.walls[i];
        line(s[0], s[1], s[2], s[3], 2 * px1, wallLit, true, 0);
    }
    glBindBuffer(GL_ARRAY_BUFFER, staticVbo_);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(v_.size() * sizeof(V)), v_.data(), GL_STATIC_DRAW);
    staticCount_ = (int)v_.size();
    v_.clear();
}

void Automap::draw(int W, int H, const MapFrame& f, Mode mode, float zoom, bool revealAll, float time) {
    if (mode == Off || !prog_) return;
    v_.clear();
    const bool mini = mode == Mini;

    // View: the minimap turns with the player (heading up); the full map keeps north up.
    float cx, cy, scale, radius = 0;
    float rot = mini ? -f.yaw : 0.0f;
    float ox = f.px, oz = f.pz;
    if (mini) {
        radius = std::round(std::max(80.0f, H * 0.16f));
        float margin = std::round(H * 0.03f) + 4;
        float u = std::max(1.0f, std::round(H / 225.0f));      // clear of the look scale at the edge
        cx = W - margin - 12 * u - radius;
        cy = H - margin - radius;                                // top right (y counts up here)
        scale = radius / (kCellSize * 7.0f) * zoom;             // about 7 cells to the rim
    } else {
        cx = W * 0.5f;
        cy = H * 0.5f;
        float fit = std::min(W * 0.9f / (maxX_ - minX_), H * 0.88f / (maxZ_ - minZ_));
        scale = fit * zoom;
        if (zoom <= 1.001f) {                                  // whole level: centre the level itself
            ox = (minX_ + maxX_) * 0.5f;
            oz = (minZ_ + maxZ_) * 0.5f;
        }
    }
    float px1 = std::max(1.0f, std::round(H / 600.0f));       // one "line pixel" at this resolution
    float ui = std::max(1.0f, H / 600.0f);                     // size of small symbols

    // Background: a dark disc for the minimap, a dimming veil for the full map.
    const float veil[4] = {0.02f, 0.035f, 0.06f, mini ? 0.8f : 0.82f};
    if (mini) {
        disc(ox, oz, radius + 2, veil, false, 48);
    } else {
        float big = 1e5f;
        quad(ox - big, oz - big, ox + big, oz + big, -1, -1, -1, -1, veil, false);
    }

    // The level's floor, ledges and walls don't change: they're built once per level (and line
    // width) into their own buffer, drawn between the background and the live markers.
    std::vector<V> pre;
    pre.swap(v_);
    const float white[4] = {1, 1, 1, 1};
    if (staticPx_ != px1) {
        staticPx_ = px1;
        buildStatic(px1);
    }

    // Doors: orange bars; open doors fade to a thin outline.
    for (size_t i = 0; i + 4 < f.doors.size(); i += 5) {
        const float* d = &f.doors[i];
        float open = d[4];
        const float dark[4] = {0.05f, 0.02f, 0.0f, 0.9f};
        const float c[4] = {1.0f, 0.55f - 0.2f * open, 0.18f, 1.0f - 0.65f * open};
        line(d[0], d[1], d[2], d[3], (6 - 3 * open) * px1, dark, true, scale);
        line(d[0], d[1], d[2], d[3], (4 - 2.5f * open) * px1, c, true, scale);
    }
    // Pickups and creatures on explored ground.
    for (size_t i = 0; i + 1 < f.items.size(); i += 2) {
        float s = 3.2f * ui;
        const float pts[8] = {0, -s, s, 0, 0, s, -s, 0};
        const float out[8] = {0, -s - px1 * 1.5f, s + px1 * 1.5f, 0, 0, s + px1 * 1.5f, -s - px1 * 1.5f, 0};
        const float dark[4] = {0, 0.05f, 0.05f, 0.9f}, c[4] = {0.35f, 1.0f, 0.85f, 1.0f};
        poly(f.items[i], f.items[i + 1], out, 4, dark, false);
        poly(f.items[i], f.items[i + 1], pts, 4, c, false);
    }
    float pulse = 0.85f + 0.15f * std::sin(time * 6.0f);
    for (size_t i = 0; i + 2 < f.enemies.size(); i += 3) {
        float r = std::clamp(f.enemies[i + 2] * scale * 0.6f, 3.0f * ui, 9.0f * ui);
        const float dark[4] = {0.1f, 0, 0, 0.9f}, c[4] = {1.0f * pulse, 0.18f, 0.12f, 1.0f};
        disc(f.enemies[i], f.enemies[i + 1], r + 1.5f * px1, dark, false);
        disc(f.enemies[i], f.enemies[i + 1], r, c, false);
    }

    // The player: a view cone and an arrow.
    {
        float fx = std::sin(f.yaw), fz = -std::cos(f.yaw);
        float len = (mini ? radius * 0.75f : std::max(40.0f, 4.5f * kCellSize * scale));
        const int n = 16;
        float cone[(n + 2) * 2];
        cone[0] = 0;
        cone[1] = 0;
        for (int i = 0; i <= n; ++i) {
            float a = f.yaw - f.fov * 0.5f + f.fov * i / n;
            cone[2 + i * 2] = std::sin(a) * len;
            cone[3 + i * 2] = -std::cos(a) * len;
        }
        const float coneC[4] = {1.0f, 0.92f, 0.45f, 0.13f};
        poly(f.px, f.pz, cone, n + 2, coneC, false);
        float s = 7.0f * ui;
        float rx = -fz, rz = fx;  // right of the heading
        const float arrow[8] = {fx * s * 1.4f, fz * s * 1.4f, -fx * s + rx * s, -fz * s + rz * s, -fx * s * 0.45f, -fz * s * 0.45f,
                                -fx * s - rx * s, -fz * s - rz * s};
        float o = 1.6f * ui;
        const float outline[8] = {fx * (s * 1.4f + o * 1.6f), fz * (s * 1.4f + o * 1.6f),
                                  -fx * (s + o) + rx * (s + o * 1.6f), -fz * (s + o) + rz * (s + o * 1.6f),
                                  -fx * (s * 0.45f + o), -fz * (s * 0.45f + o),
                                  -fx * (s + o) - rx * (s + o * 1.6f), -fz * (s + o) - rz * (s + o * 1.6f)};
        const float dark[4] = {0.08f, 0.06f, 0, 0.95f}, yellow[4] = {1.0f, 0.88f, 0.25f, 1.0f};
        // Arrow triangles: (tip, left, notch) and (tip, notch, right).
        const float t1[6] = {outline[0], outline[1], outline[2], outline[3], outline[4], outline[5]};
        const float t2[6] = {outline[0], outline[1], outline[4], outline[5], outline[6], outline[7]};
        poly(f.px, f.pz, t1, 3, dark, false);
        poly(f.px, f.pz, t2, 3, dark, false);
        const float a1[6] = {arrow[0], arrow[1], arrow[2], arrow[3], arrow[4], arrow[5]};
        const float a2[6] = {arrow[0], arrow[1], arrow[4], arrow[5], arrow[6], arrow[7]};
        poly(f.px, f.pz, a1, 3, yellow, false);
        poly(f.px, f.pz, a2, 3, yellow, false);
    }

    // Minimap rim with a north marker.
    if (mini) {
        const int n = 64;
        const float rim[4] = {0.75f, 0.85f, 0.95f, 0.55f};
        for (int i = 0; i < n; ++i) {
            float a0 = 6.2831853f * i / n, a1 = 6.2831853f * (i + 1) / n;
            float r0 = radius - 1.0f * px1, r1 = radius + 1.0f * px1;
            // Ring in screen space: offsets are rotated with the map, so undo the turn.
            float c0 = std::cos(a0 - rot), s0 = std::sin(a0 - rot), c1 = std::cos(a1 - rot), s1 = std::sin(a1 - rot);
            const float seg[8] = {c0 * r0, s0 * r0, c0 * r1, s0 * r1, c1 * r1, s1 * r1, c1 * r0, s1 * r0};
            poly(ox, oz, seg, 4, rim, false);
        }
        // "N": a small badge on the rim where north is.
        float br = 8.5f * ui;
        float nr = radius - br - 3 * ui;
        float bx = 0, bz = -nr;  // north in map-aligned pixel offsets
        const float badge[4] = {0.85f, 0.2f, 0.15f, 0.95f};
        disc(ox + bx / scale, oz + bz / scale, br, badge, false, 20);
        // The letter, from three strokes in screen-aligned offsets.
        auto strokeN = [&](float ax, float ay, float bx2, float by) {
            // screen-aligned -> map-aligned offsets (inverse of the view turn)
            auto toMap = [&](float sx, float sy, float& mx, float& my) {
                float c = std::cos(-rot), s = std::sin(-rot);
                float ux = sx, uy = -sy;  // screen y up -> map offset y down
                mx = c * ux - s * uy;
                my = s * ux + c * uy;
            };
            float m0x, m0y, m1x, m1y;
            toMap(ax, ay, m0x, m0y);
            toMap(bx2, by, m1x, m1y);
            float l = std::sqrt((m1x - m0x) * (m1x - m0x) + (m1y - m0y) * (m1y - m0y));
            if (l < 1e-4f) return;
            float dx = (m1x - m0x) / l, dy = (m1y - m0y) / l, w = 0.8f * ui;
            float nx = -dy * w, ny = dx * w;
            const float q[8] = {bx + m0x + nx, bz + m0y + ny, bx + m1x + nx, bz + m1y + ny, bx + m1x - nx, bz + m1y - ny,
                                bx + m0x - nx, bz + m0y - ny};
            poly(ox, oz, q, 4, white, false);
        };
        float s = 3.6f * ui;
        strokeN(-s, -s, -s, s);
        strokeN(-s, s, s, -s);
        strokeN(s, -s, s, s);
    }

    // Upload the explored mask and draw.
    mask_.assign(f.explored ? f.explored : nullptr, f.explored ? f.explored + kGrid * kGrid : nullptr);
    if (mask_.size() != (size_t)kGrid * kGrid) mask_.assign((size_t)kGrid * kGrid, 255);
    glBindTexture(GL_TEXTURE_2D, maskTex_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, kGrid, kGrid, 0, GL_RED, GL_UNSIGNED_BYTE, mask_.data());

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, W, H);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(prog_);
    auto U = [&](const char* n) { return glGetUniformLocation(prog_, n); };
    glUniform2f(U("uOrigin"), ox, oz);
    glUniform2f(U("uCenter"), cx, cy);
    glUniform1f(U("uScale"), scale);
    glUniform2f(U("uRot"), std::cos(rot), std::sin(rot));
    glUniform2f(U("uViewport"), (float)W, (float)H);
    glUniform1i(U("uReveal"), revealAll ? 1 : 0);
    glUniform3f(U("uClip"), cx, cy, mini ? radius + 2 : 0.0f);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, floorTex_);
    glUniform1i(U("uFloor"), 0);
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, maskTex_);
    glUniform1i(U("uMask"), 1);
    glActiveTexture(GL_TEXTURE0);
    size_t nPre = pre.size();
    pre.insert(pre.end(), v_.begin(), v_.end());
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(pre.size() * sizeof(V)), pre.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)nPre);
    glBindVertexArray(staticVao_);
    glDrawArrays(GL_TRIANGLES, 0, staticCount_);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, (GLint)nPre, (GLsizei)(pre.size() - nPre));
    glBindVertexArray(0);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}
