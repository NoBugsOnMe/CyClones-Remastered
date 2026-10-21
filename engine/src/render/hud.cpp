#include "hud.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

const char* kHudVS = R"(#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aCol;
uniform vec2 uViewport;
out vec2 vUV;
out vec4 vCol;
void main() {
    gl_Position = vec4(aPos.x / uViewport.x * 2.0 - 1.0, 1.0 - aPos.y / uViewport.y * 2.0, 0.0, 1.0);
    vUV = aUV;
    vCol = aCol;
}
)";

const char* kHudFS = R"(#version 330 core
in vec2 vUV;
in vec4 vCol;
uniform sampler2D uTex;
out vec4 oColor;
void main() {
    vec4 t = texture(uTex, vUV);
    oColor = t * vCol;
    if (oColor.a <= 0.003) discard;
}
)";

GLuint compileHud(std::string& error) {
    auto stage = [&](GLenum type, const char* src) -> GLuint {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[1024];
            glGetShaderInfoLog(s, sizeof(log), nullptr, log);
            error += log;
            return 0;
        }
        return s;
    };
    GLuint v = stage(GL_VERTEX_SHADER, kHudVS), f = stage(GL_FRAGMENT_SHADER, kHudFS);
    if (!v || !f) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    return ok ? p : 0;
}

const int kAtlasW = 2048, kAtlasH = 1024;

}  // namespace

bool Hud::init(GameData& data, std::string& error) {
    prog_ = compileHud(error);
    if (!prog_) return false;
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(V), (void*)offsetof(V, x));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(V), (void*)offsetof(V, u));
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(V), (void*)offsetof(V, r));
    for (int i = 0; i < 3; ++i) glEnableVertexAttribArray(i);
    glBindVertexArray(0);

    auto& pal = pal_;
    for (int i = 0; i < 256; ++i)
        for (int c = 0; c < 3; ++c) {
            int v = data.palette[i * 3 + c] & 63;
            pal[i][c] = (uint8_t)std::min(255, v * 4 + v / 16);
        }
    // The options screen's palette.
    uint8_t opal[256][3];
    std::memcpy(opal, pal, sizeof(opal));
    if (int op = data.hd.find("options_pal"); op >= 0 && data.hd.size(op) >= 768)
        for (int i = 0; i < 256; ++i)
            for (int c = 0; c < 3; ++c) {
                int v = data.hd.data(op)[i * 3 + c] & 63;
                opal[i][c] = (uint8_t)std::min(255, v * 4 + v / 16);
            }
    std::vector<uint8_t> atlas((size_t)kAtlasW * kAtlasH * 4, 0);
    int px = 2, py = 2, rowH = 0;
    // A white texel for plain rectangles.
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x) std::memset(&atlas[((size_t)y * kAtlasW + x) * 4], 255, 4);
    whiteU_ = 1.0f / kAtlasW;
    whiteV_ = 1.0f / kAtlasH;
    // Places w*h palette indices (0 = clear unless solid) in the atlas.
    auto put = [&](const uint8_t* d, int w, int h, Glyph& g, bool digit, bool solid, const uint8_t (*p)[3]) {
        if (px + w + 2 > kAtlasW) { px = 2; py += rowH + 2; rowH = 0; }
        if (py + h + 2 > kAtlasH) return;
        int lo = w, hi = -1;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                uint8_t c = d[y * w + x];
                if (!c && !solid) continue;
                lo = std::min(lo, x);
                hi = std::max(hi, x);
                uint8_t* o = &atlas[((size_t)(py + y) * kAtlasW + px + x) * 4];
                o[0] = p[c][0];
                o[1] = p[c][1];
                o[2] = p[c][2];
                o[3] = (digit && c < 120) ? 40 : 255;  // unlit LCD segments stay faint
            }
        g.u0 = (float)px / kAtlasW;
        g.v0 = (float)py / kAtlasH;
        g.u1 = (float)(px + w) / kAtlasW;
        g.v1 = (float)(py + h) / kAtlasH;
        g.w = w;
        g.h = h;
        g.left = hi < 0 ? 0 : lo;
        g.adv = hi < 0 ? 3 : hi - lo + 2;
        g.valid = true;
        px += w + 2;
        rowH = std::max(rowH, h);
    };
    // An image lump: u16 w, u16 h, 4 bytes, w*h palette indices.
    auto lump = [&](const std::string& name, int& w, int& h) -> const uint8_t* {
        int i = data.hd.find(name);
        if (i < 0 || data.hd.size(i) < 8) return nullptr;
        const uint8_t* d = data.hd.data(i);
        w = d[0] | d[1] << 8, h = d[2] | d[3] << 8;
        if (w <= 0 || h <= 0 || (size_t)(8 + w * h) > data.hd.size(i)) return nullptr;
        return d + 8;
    };
    auto place = [&](const std::string& name, Glyph& g, bool digit = false, bool solid = false, const uint8_t (*p)[3] = nullptr) {
        int w, h;
        if (const uint8_t* d = lump(name, w, h)) put(d, w, h, g, digit, solid, p ? p : pal);
    };
    const char* prefix[4] = {"c", "ag", "ar", "gmred"};
    for (int f = 0; f <= Red; ++f) {
        for (int ch = 0; ch < 64; ++ch) place(prefix[f] + std::to_string(32 + ch), glyphs_[f][ch]);
        // Text height: the tallest ink among the capitals.
        glyphH_[f] = 5;
    }
    // Stamped lettering: the green font's letters (not its shadow) in the frame's own greys
    // (options_pal 6 and 29).
    for (int f = Engraved; f <= EngravedEdge; ++f) {
        uint8_t ink[256][3] = {};
        uint8_t v = f == Engraved ? 36 : 212;
        for (int c = 1; c < 256; ++c) ink[c][0] = ink[c][1] = ink[c][2] = v;
        for (int ch = 0; ch < 64; ++ch) {
            int w, h;
            const uint8_t* d = lump("c" + std::to_string(32 + ch), w, h);
            if (!d) continue;
            std::vector<uint8_t> letter(d, d + (size_t)w * h);
            for (auto& px : letter)
                if (px == 2) px = 0;   // the font's built-in shadow
            put(letter.data(), w, h, glyphs_[f][ch], false, false, ink);
        }
        glyphH_[f] = 5;
    }
    for (int d = 0; d < 10; ++d) place("digit" + std::to_string(d), digit_[d], true);
    const char* imgs[4] = {"statsicon", "gunicon", "i_biokit", "i_armork"};
    for (int i = 0; i < 4; ++i) place(imgs[i], images_[i]);
    // The options screen: a raw 320x200 picture, then the art drawn over it.
    if (int i = data.hd.find("options"); i >= 0 && data.hd.size(i) >= 64000) {
        if (px != 2) { px = 2; py += rowH + 2; rowH = 0; }
        put(data.hd.data(i), 320, 200, images_[MenuScreen], false, true, opal);
    }
    place("option0", images_[MenuLogo], false, false, opal);
    place("autocursor", images_[MenuCursor], false, false, opal);
    place("disreturn", images_[MenuOffReturn], false, false, opal);
    place("dissave", images_[MenuOffSave], false, false, opal);
    place("optbar", images_[MenuBar], false, false, opal);      // the SETTINGS sliders (anchors 17,6 and 10,11)
    place("optslide", images_[MenuSlide], false, false, opal);
    static const char* buttons[11] = {"optbut1", "optbut2", "optbut3", "optbut4", "optbut5", "optbut6",
                                      "optbut7", "optbut8", "optbut9", "optno", "optyes"};
    for (int b = 0; b < 11; ++b) {
        place(buttons[b], images_[MenuButton + b], false, false, opal);
        place(std::string("h") + buttons[b], images_[MenuLit + b], false, false, opal);
    }
    // The first-person weapons: each gun's 15 aiming poses (FUN_0005185c picks one), the chaingun's
    // spinning-barrel set, the alien rifle's second set, the control-stick hand of the suit-mounted
    // weapons, the three gloves and the muzzle flashes.
    {
        std::vector<std::string> names;
        for (const char* g : {"gaussp", "chaing", "chainf", "alpstl"})
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 5; ++c) names.push_back(g + std::to_string(r) + std::to_string(c));
        for (int i = 1; i <= 30; ++i) names.push_back((i < 10 ? "dbgn0" : "dbgn") + std::to_string(i));
        for (int i = 1; i <= 15; ++i) names.push_back((i < 10 ? "lnch0" : "lnch") + std::to_string(i));
        for (const char* n : {"joyanim1", "joyanim2", "joyanim3", "joymidcl", "joyfwd", "joyfwdcl", "joyrgt", "joyrgtcl",
                              "joylft", "joylftcl", "glove10", "glove11", "glove12", "glove20", "glove21", "glove22",
                              "glove23", "glove24", "glove25", "glove30", "glove31", "glove32", "gflash10", "gflash11",
                              "gflash12", "cflash10", "cflash20", "cflash11", "blflash0", "blflash1", "blflash2",
                              "grflash0", "grflash1", "grflash2", "grflash3", "grflash4", "apflash0", "apflash1",
                              "apflash2", "apflast0", "apflast1", "apflast2", "dgflash0", "dgflash1", "dgflash2", "dgflash3"})
            names.push_back(n);
        // The inventory: every item's picture (i_...), the slot's background and the DROP button,
        // the Demo Pack and jump-jet icons (each with its blink), the key-ring pictures.
        for (const auto& l : data.hd.lumps())
            if (l.name.size() > 2 && l.name[0] == 'i' && l.name[1] == '_') names.push_back(l.name);
        for (const char* n : {"meshgrid", "dropicon", "demopakicon", "demopakicon2", "jumpjeticon", "jumpjeticon2",
                              "invkeyr", "invkeyb", "invkeyg", "itemsicon", "arrowb"})
            names.push_back(n);
        for (const auto& n : names) {
            int i = data.hd.find(n);
            if (i < 0 || data.hd.size(i) < 8) continue;
            const uint8_t* d = data.hd.data(i);
            int w = d[0] | d[1] << 8, h = d[2] | d[3] << 8;
            if (w <= 0 || h <= 0 || (size_t)(8 + w * h) > data.hd.size(i)) continue;
            Sprite sp;
            sp.ax = (int16_t)(d[4] | d[5] << 8);
            sp.ay = (int16_t)(d[6] | d[7] << 8);
            put(d + 8, w, h, sp.g, false, false, pal);
            if (sp.g.w) sprites_[n] = sp;
        }
        // The glove has no picture in the weapons cache (the game leaves it out); for the weapon
        // tiles (an addition), each suit's first-person glove (glove10, glove20, glove30) shrunk to
        // fit a tile, averaging the pixels it covers: wglove0-2.
        for (int k = 0; k < 3; ++k) {
            int w, h;
            const uint8_t* d = lump("glove" + std::to_string(k + 1) + "0", w, h);
            if (!d) continue;
            float f = std::min(42.0f / w, 25.0f / h);
            int ow = std::max(1, (int)(w * f)), oh = std::max(1, (int)(h * f));
            if (px + ow + 2 > kAtlasW) { px = 2; py += rowH + 2; rowH = 0; }
            if (py + oh + 2 > kAtlasH) continue;
            for (int y = 0; y < oh; ++y)
                for (int x = 0; x < ow; ++x) {
                    float acc[4] = {0, 0, 0, 0};
                    int x0 = (int)(x / f), x1 = std::max(x0 + 1, (int)((x + 1) / f)), y0 = (int)(y / f), y1 = std::max(y0 + 1, (int)((y + 1) / f));
                    for (int yy = y0; yy < y1 && yy < h; ++yy)
                        for (int xx = x0; xx < x1 && xx < w; ++xx) {
                            uint8_t c = d[yy * w + xx];
                            if (!c) { acc[3] += 0; continue; }
                            for (int ch = 0; ch < 3; ++ch) acc[ch] += pal[c][ch];
                            acc[3] += 1;
                        }
                    float n = (float)((x1 - x0) * (y1 - y0));
                    uint8_t* o = &atlas[((size_t)(py + y) * kAtlasW + px + x) * 4];
                    if (acc[3] > 0)
                        for (int ch = 0; ch < 3; ++ch) o[ch] = (uint8_t)std::lround(acc[ch] / acc[3]);
                    o[3] = (uint8_t)std::lround(255 * std::min(1.0f, acc[3] / n * 1.15f));
                }
            Sprite sp;
            sp.g.u0 = (float)px / kAtlasW;
            sp.g.v0 = (float)py / kAtlasH;
            sp.g.u1 = (float)(px + ow) / kAtlasW;
            sp.g.v1 = (float)(py + oh) / kAtlasH;
            sp.g.w = ow;
            sp.g.h = oh;
            sp.g.valid = true;
            sp.ax = -(46 - ow) / 2;   // centred in the tile
            sp.ay = -(29 - oh) / 2 - 1;
            sprites_["wglove" + std::to_string(k)] = sp;
            px += ow + 2;
            rowH = std::max(rowH, oh);
        }
    }
    // The game has no disabled LOAD button (it never needed one): stipple LOAD the way RETURN and
    // SAVE are, every other pixel near-black and the rest a shade darker, learning the colours
    // from those two.
    {
        int w1 = 0, h1 = 0, w2 = 0, h2 = 0, w3 = 0, h3 = 0, w4 = 0, h4 = 0, w5 = 0, h5 = 0;
        const uint8_t *ret = lump("optbut1", w1, h1), *offRet = lump("disreturn", w2, h2), *save = lump("optbut4", w3, h3),
                      *offSave = lump("dissave", w4, h4), *load = lump("optbut3", w5, h5);
        if (ret && offRet && save && offSave && load && w1 == w2 && h1 == h2 && w3 == w4 && h3 == h4) {
            std::vector<int> votes(256 * 256, 0);   // [from][to]
            auto learn = [&](const uint8_t* a, const uint8_t* b, int n) {
                for (int i = 0; i < n; ++i) ++votes[a[i] * 256 + b[i]];
            };
            learn(ret, offRet, w1 * h1);
            learn(save, offSave, w3 * h3);
            uint8_t dark[256], shade[256];
            for (int c = 0; c < 256; ++c) {
                int nd = 0, ns = 0;
                dark[c] = shade[c] = (uint8_t)c;
                for (int t = 0; t < 256; ++t) {
                    int v = votes[c * 256 + t];
                    if (t < 100 && v > nd) nd = v, dark[c] = (uint8_t)t;
                    if (t >= 100 && v > ns) ns = v, shade[c] = (uint8_t)t;
                }
                if (!ns) shade[c] = dark[c];
                if (!nd) dark[c] = shade[c];
            }
            std::vector<uint8_t> out((size_t)w5 * h5);
            for (int y = 0; y < h5; ++y)
                for (int x = 0; x < w5; ++x) {
                    uint8_t c = load[y * w5 + x];
                    out[(size_t)y * w5 + x] = ((x + y) & 1) ? dark[c] : shade[c];
                }
            put(out.data(), w5, h5, images_[MenuOffLoad], false, false, opal);
        }
    }
    // Room for a saved game's picture (the LOAD and SAVE screens), filled in by setThumb().
    {
        std::vector<uint8_t> black((size_t)kThumbW * kThumbH, 0);
        static const uint8_t kBlack[1][3] = {{0, 0, 0}};
        put(black.data(), kThumbW, kThumbH, images_[MenuThumb], false, true, kBlack);
    }
    glGenTextures(1, &tex_);
    glBindTexture(GL_TEXTURE_2D, tex_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kAtlasW, kAtlasH, 0, GL_RGBA, GL_UNSIGNED_BYTE, atlas.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return true;
}

void Hud::begin(int width, int height) {
    w_ = width;
    h_ = height;
    v_.clear();
}

void Hud::quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, float r, float g, float b, float a) {
    V p{x0, y0, u0, v0, r, g, b, a}, q{x1, y0, u1, v0, r, g, b, a}, s{x1, y1, u1, v1, r, g, b, a}, t{x0, y1, u0, v1, r, g, b, a};
    v_.insert(v_.end(), {p, q, s, p, s, t});
}

void Hud::rect(float x, float y, float w, float h, float r, float g, float b, float a) {
    quad(x, y, x + w, y + h, whiteU_, whiteV_, whiteU_, whiteV_, r, g, b, a);
}

float Hud::text(Font f, float x, float y, float s, const std::string& str, float alpha, bool measureOnly) {
    return drawText(f, x, y, s, str, 1, 1, 1, alpha, measureOnly);
}

float Hud::tinted(Font f, float x, float y, float s, const std::string& str, float r, float g, float b, float alpha) {
    return drawText(f, x, y, s, str, r, g, b, alpha, false);
}

void Hud::image(Image i, float x, float y, float s, float alpha) {
    const Glyph& g = images_[i];
    if (!g.valid) return;
    float gx = std::round(x), gy = std::round(y);
    quad(gx, gy, gx + g.w * s, gy + g.h * s, g.u0, g.v0, g.u1, g.v1, 1, 1, 1, alpha);
}

void Hud::sprite(const std::string& lump, float x, float y, float sx, float sy, bool mirror, float alpha) {
    auto it = sprites_.find(lump);
    if (it == sprites_.end()) return;
    const Sprite& sp = it->second;
    const Glyph& g = sp.g;
    // Normally the anchor column ax lands on x; mirrored, column 0 is at x + ax and the art runs
    // leftward (FUN_00039836), so the anchor stays on x.
    float left = mirror ? x - (g.w - 1 - sp.ax) * sx : x - sp.ax * sx;
    float x0 = std::round(left), y0 = std::round(y - sp.ay * sy);
    float x1 = std::round(left + g.w * sx), y1 = std::round(y - sp.ay * sy + g.h * sy);
    if (mirror) quad(x0, y0, x1, y1, g.u1, g.v0, g.u0, g.v1, 1, 1, 1, alpha);
    else quad(x0, y0, x1, y1, g.u0, g.v0, g.u1, g.v1, 1, 1, 1, alpha);
}

void Hud::weaponTile(const std::string& pic, float x, float y, float s, bool readied, const std::string& count, float alpha,
                     bool dim) {
    // the mesh (meshgrid), then the grid: lines every 9 pixels, x0..x0+45 by y0..y0+27 (FUN_0003920c)
    float a = alpha * (dim ? 0.45f : 1.0f);
    sprite("meshgrid", x, y, s, s, false, a);
    int c = readied ? 0x7c : 0xb5;
    for (int gx = 0; gx <= 45; gx += 9) palRect(x + gx * s, y, s, 28 * s, c, a);
    for (int gy = 0; gy <= 27; gy += 9) palRect(x, y + gy * s, 45 * s, s, c, a);
    if (!pic.empty()) sprite(pic, x, y, s, s, false, a);
    if (!count.empty()) text(Green, x + 2 * s, y + 2 * s, s, count, a);
}

void Hud::tintedSprite(const std::string& lump, float x, float y, float s, float r, float g, float b, float alpha) {
    auto it = sprites_.find(lump);
    if (it == sprites_.end()) return;
    const Sprite& sp = it->second;
    const Glyph& gl = sp.g;
    float x0 = std::round(x - sp.ax * s), y0 = std::round(y - sp.ay * s);
    quad(x0, y0, x0 + gl.w * s, y0 + gl.h * s, gl.u0, gl.v0, gl.u1, gl.v1, r, g, b, alpha);
}

void Hud::palRect(float x, float y, float w, float h, int index, float a) {
    index &= 255;
    rect(x, y, w, h, pal_[index][0] / 255.0f, pal_[index][1] / 255.0f, pal_[index][2] / 255.0f, a);
}

float Hud::drawText(Font f, float x, float y, float s, const std::string& str, float r, float gg, float b, float alpha, bool measureOnly) {
    float cx = x;
    for (char c0 : str) {
        int c = (unsigned char)c0;
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c < 32 || c > 95) c = '?';
        const Glyph& g = glyphs_[f][c - 32];
        if (!g.valid) { cx += 3 * s; continue; }
        if (c == '.' && (f == Amber || f == Grey)) {
            // These two fonts draw '.' as a degree sign (the compass's); a full stop is the
            // bottom pixel of their comma.
            const Glyph& k = glyphs_[f][',' - 32];
            if (!measureOnly && k.valid) {
                float gx = std::round(cx), gy = std::round(y + 4 * s);
                float du = 1.0f / kAtlasW, dv = 1.0f / kAtlasH, v0 = k.v0 + 4 * dv;
                if (shadow_) quad(gx + s, gy + s, gx + 2 * s, gy + 2 * s, k.u0, v0, k.u0 + du, v0 + dv, 0, 0, 0, alpha * 0.85f);
                quad(gx, gy, gx + s, gy + s, k.u0, v0, k.u0 + du, v0 + dv, r, gg, b, alpha);
            }
            cx += 2 * s;
            continue;
        }
        if (!measureOnly && c != ' ') {
            // A one-pixel shadow keeps the small font readable over the 3D view.
            float gx = std::round(cx - g.left * s), gy = std::round(y);
            if (shadow_) quad(gx + s, gy + s, gx + s + g.w * s, gy + s + g.h * s, g.u0, g.v0, g.u1, g.v1, 0, 0, 0, alpha * 0.85f);
            quad(gx, gy, gx + g.w * s, gy + g.h * s, g.u0, g.v0, g.u1, g.v1, r, gg, b, alpha);
        }
        cx += g.adv * s;
    }
    return cx - x;
}

float Hud::digitsWidth(float s, int value, int minDigits) {
    std::string str = std::to_string(std::max(0, value));
    while ((int)str.size() < minDigits) str = "0" + str;
    return str.size() * (digit_[0].w + 1) * s;
}

float Hud::digits(float x, float y, float s, int value, int minDigits, float alpha) {
    std::string str = std::to_string(std::max(0, value));
    while ((int)str.size() < minDigits) str = "0" + str;
    float cx = x;
    for (char c : str) {
        const Glyph& g = digit_[c - '0'];
        if (g.valid) {
            float gx = std::round(cx), gy = std::round(y);
            quad(gx + s, gy + s, gx + s + g.w * s, gy + s + g.h * s, g.u0, g.v0, g.u1, g.v1, 0, 0, 0, alpha * 0.8f);
            quad(gx, gy, gx + g.w * s, gy + g.h * s, g.u0, g.v0, g.u1, g.v1, 1, 1, 1, alpha);
        }
        cx += (digit_[0].w + 1) * s;
    }
    return cx - x;
}

void Hud::flush(GLuint texture, int viewW, int viewH, float logicalW, float logicalH) {
    glViewport(0, 0, viewW, viewH);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(prog_);
    glUniform2f(glGetUniformLocation(prog_, "uViewport"), logicalW, logicalH);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glUniform1i(glGetUniformLocation(prog_, "uTex"), 0);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(v_.size() * sizeof(V)), v_.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)v_.size());
    glBindVertexArray(0);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    v_.clear();
}

void Hud::end() {
    if (v_.empty() || !prog_) return;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    flush(tex_, w_, h_, (float)w_, (float)h_);
}

void Hud::setThumb(const uint8_t* rgb) {
    const Glyph& g = images_[MenuThumb];
    if (!g.valid || !tex_) return;
    std::vector<uint8_t> px((size_t)kThumbW * kThumbH * 4, 255);
    for (int i = 0; i < kThumbW * kThumbH; ++i)
        for (int c = 0; c < 3; ++c) px[(size_t)i * 4 + c] = rgb ? rgb[(size_t)i * 3 + c] : 0;
    glBindTexture(GL_TEXTURE_2D, tex_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, (int)std::lround(g.u0 * kAtlasW), (int)std::lround(g.v0 * kAtlasH), kThumbW, kThumbH,
                    GL_RGBA, GL_UNSIGNED_BYTE, px.data());
}

void Hud::beginCanvas(int cw, int ch, int k) {
    canvasK_ = std::clamp(k, 1, 8);
    int fw = cw * canvasK_, fh = ch * canvasK_;
    if (!fbo_) glGenFramebuffers(1, &fbo_);
    if (!fboTex_) glGenTextures(1, &fboTex_);
    if (fw != fboW_ || fh != fboH_) {
        fboW_ = fw;
        fboH_ = fh;
        glBindTexture(GL_TEXTURE_2D, fboTex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, fw, fh, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fboTex_, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    begin(cw, ch);
}

void Hud::picture(const uint8_t* rgba, int pw, int ph, int screenW, int screenH, float x, float y, float w, float h) {
    if (!prog_ || pw <= 0 || ph <= 0) return;
    if (!picTex_) glGenTextures(1, &picTex_);
    glBindTexture(GL_TEXTURE_2D, picTex_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (pw != picW_ || ph != picH_) {
        picW_ = pw;
        picH_ = ph;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, pw, ph, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, pw, ph, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    }
    beginCanvas(pw, ph, (int)std::ceil(std::max(w / pw, h / ph)));
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, fboW_, fboH_);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    quad(0, 0, (float)pw, (float)ph, 0, 0, 1, 1, 1, 1, 1, 1);
    flush(picTex_, fboW_, fboH_, (float)pw, (float)ph);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, screenW, screenH);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    w_ = screenW;
    h_ = screenH;
    quad(x, y, x + w, y + h, 0, 1, 1, 0, 1, 1, 1, 1);
    flush(fboTex_, screenW, screenH, (float)screenW, (float)screenH);
}

void Hud::photo(int slot, const uint8_t* rgba, int pw, int ph, int screenW, int screenH, float x, float y, float w, float h,
                float bright, bool clear) {
    if (!prog_ || slot < 0 || slot > 1) return;
    GLuint& t = photoTex_[slot];
    if (!t) glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    if (rgba && pw > 0 && ph > 0) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        if (pw != photoW_[slot] || ph != photoH_[slot]) {
            photoW_[slot] = pw;
            photoH_[slot] = ph;
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, pw, ph, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, pw, ph, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        }
        glGenerateMipmap(GL_TEXTURE_2D);
    }
    if (!photoW_[slot]) return;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, screenW, screenH);
    if (clear) {
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    w_ = screenW;
    h_ = screenH;
    float b = std::clamp(bright, 0.0f, 1.0f);
    quad(x, y, x + w, y + h, 0, 0, 1, 1, b, b, b, 1);
    flush(t, screenW, screenH, (float)screenW, (float)screenH);
}

void Hud::endCanvas(int screenW, int screenH, float x, float y, float w, float h) {
    if (!prog_ || !fbo_) return;
    // The canvas, k pixels to each of its own, sharp...
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, fboW_, fboH_);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    flush(tex_, fboW_, fboH_, (float)w_, (float)h_);
    // ...then smoothed only where its pixels meet, into the window.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    w_ = screenW;
    h_ = screenH;
    quad(x, y, x + w, y + h, 0, 1, 1, 0, 1, 1, 1, 1);
    flush(fboTex_, screenW, screenH, (float)screenW, (float)screenH);
}
