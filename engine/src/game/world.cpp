#include "world_internal.h"

#include <set>

World::World() = default;
World::~World() = default;

void World::init(GameData* data, Renderer* renderer) {
    data_ = data;
    renderer_ = renderer;
    for (int i = 0; i < 256; ++i)
        for (int c = 0; c < 3; ++c) {
            int v = data->palette[i * 3 + c] & 63;
            pal_[i][c] = (v * 4 + v / 16) / 255.0f;
        }
    setVoxelPalette(data->palette.data());
    sparkRamp_ = {1, 1, .9f, 1, .85f, .45f, 1, .55f, .15f, .7f, .2f, .05f};
}

// ---------------------------------------------------------------------------------------------
// Art

int World::lump(const std::string& name, bool cdFirst, const Bol** bol) const {
    const Bol* order[2] = {cdFirst ? &data_->cd : &data_->hd, cdFirst ? &data_->hd : &data_->cd};
    for (const Bol* b : order) {
        int i = b->find(name);
        if (i >= 0 && b->size(i) > 4) { *bol = b; return i; }
    }
    return -1;
}

bool World::sprite(const std::string& name, bool cdFirst, SpriteImage& out) const {
    const Bol* b = nullptr;
    int i = lump(name, cdFirst, &b);
    return i >= 0 && decodeSprite(b->data(i), b->size(i), out);
}


static int rotationCount(const Bol& bol, int idx) {
    std::string name = lower(bol.lumps()[idx].name);
    if (name.empty() || !std::isdigit((unsigned char)name.back())) return 1;
    std::string prefix = name.substr(0, name.size() - 1);
    int d = name.back() - '0', count = 1;
    while (count < 8 && idx + count < (int)bol.lumps().size() && d + count <= 9 &&
           lower(bol.lumps()[idx + count].name) == prefix + (char)('0' + d + count))
        ++count;
    return (count == 5 || count == 8) ? count : 1;
}


namespace {

HitMask makeHitMask(const SpriteImage& img, bool mirror) {
    HitMask m;
    m.w = img.w + 2;
    m.h = img.h + 1;
    m.ox = (mirror ? img.w - img.ox : img.ox) + 1;
    m.on.assign((size_t)m.w * m.h, 0);
    for (int y = 0; y < img.h; ++y) {
        int lo = img.w, hi = -1;
        for (int x = 0; x < img.w; ++x)
            if (img.at(mirror ? img.w - 1 - x : x, y)) { lo = std::min(lo, x); hi = std::max(hi, x); }
        if (hi < 0) continue;
        // Fill the row (plus a pixel either side) and the row above, so the silhouette grows by one.
        for (int yy = y; yy <= y + 1 && yy < m.h; ++yy)
            for (int x = lo; x <= hi + 2; ++x) m.on[(size_t)yy * m.w + x] = 1;
        if (y > 0)
            for (int x = lo; x <= hi + 2; ++x) m.on[(size_t)(y - 1) * m.w + x] = 1;
    }
    return m;
}

// Depth for every silhouette pixel of a depth-sprite view, read back from its splats (in the view's
// own frame: u to the right, d toward the viewer); filled gaps take the nearest depth in the row.
void addDepth(HitMask& m, const SplatSet& set, int k) {
    float a = k * kPi / 4, rx = std::cos(a), rz = -std::sin(a), fx = std::sin(a), fz = std::cos(a);
    m.depth.assign(m.on.size(), -1e9f);
    for (const auto& s : set.s) {
        float X = s.x * 0.25f, Y = s.y * 0.25f, Z = s.z * 0.25f;
        float u = X * rx + Z * rz, d = X * fx + Z * fz;
        int x = (int)std::floor(u + m.ox), y = (int)std::floor(Y);
        for (int yy = y - 1; yy <= y + 1; ++yy)  // the mask is grown by a pixel; so is the depth
            for (int xx = x - 1; xx <= x + 1; ++xx) {
                if (xx < 0 || yy < 0 || xx >= m.w || yy >= m.h) continue;
                float& dst = m.depth[(size_t)yy * m.w + xx];
                if (xx == x && yy == y) dst = d;
                else if (dst < -1e8f) dst = d;
            }
    }
    for (int y = 0; y < m.h; ++y) {
        for (int x = 0; x < m.w; ++x) {
            size_t i = (size_t)y * m.w + x;
            if (!m.on[i] || m.depth[i] > -1e8f) continue;
            float best = 0;
            int bestDist = 1 << 30;
            for (int xx = 0; xx < m.w; ++xx) {
                float d = m.depth[(size_t)y * m.w + xx];
                if (d > -1e8f && std::abs(xx - x) < bestDist) { bestDist = std::abs(xx - x); best = d; }
            }
            m.depth[i] = best;
        }
    }
}

void growBounds(const HitMask& m, float& radius, float& height) {
    radius = std::max(radius, (float)std::max(m.ox, m.w - m.ox) + 1);
    height = std::max(height, (float)m.h + 1);
}

Visual::Staged::Anim stageFlat(const std::vector<SpriteImage>& frames, const std::vector<float>& seconds, bool loop,
                               const std::vector<float>& light = {}, bool hold = true) {
    Visual::Staged::Anim a;
    a.loop = loop;
    a.hold = hold;
    std::vector<SplatSet> sets;
    for (const auto& f : frames) sets.push_back(flatSprite(f));
    for (size_t i = 0; i < sets.size(); ++i) {
        a.still.push_back(stillSplats(sets[i]));
        bool hasNext = i + 1 < sets.size() || (loop && sets.size() > 1);
        a.morph.push_back(hasNext ? morphSplats(sets[i], sets[(i + 1) % sets.size()]) : std::vector<SplatMorph>());
        a.seconds.push_back(i < seconds.size() ? seconds[i] : seconds.empty() ? 0.1f : seconds.back());
        a.light.push_back(i < light.size() ? light[i] : 0.0f);
        a.height.push_back(frames[i].h);
        a.width.push_back(frames[i].w);
        a.anchor.push_back(frames[i].oy);
    }
    return a;
}

SpriteAnim uploadFlat(Renderer* r, Visual::Staged::Anim& s) {
    SpriteAnim a;
    a.loop = s.loop;
    a.hold = s.hold;
    a.seconds = s.seconds;
    a.light = s.light;
    a.height = s.height;
    a.width = s.width;
    a.anchor = s.anchor;
    for (size_t i = 0; i < s.still.size(); ++i) {
        a.still.push_back(r->addSplats(s.still[i]));
        a.morph.push_back(s.morph[i].empty() ? a.still.back() : r->addSplats(s.morph[i]));
    }
    return a;
}

}  // namespace

const Visual* World::monsterVisual(const MonsterType& t) {
    // Built once and kept across levels, so the key holds everything the build reads: levels
    // define the same creature differently (BASE's gunball fires attack B at 9 ticks a frame,
    // BIOSTOR2's attack A with a light-coded flash). Keyed on the sprite and walk frames alone
    // (up to 0.5.2), a level played after another reused that one's animations: BIOSTOR2's
    // gunballs, after BASE, had no attack A to draw, and showed no flash.
    std::string key = "mon:" + lower(t.sprite);
    auto add = [&](const char* tag, const std::vector<uint8_t>& v) {
        key += tag;
        for (uint8_t b : v) key += std::to_string(b) + ",";
    };
    add("|w", t.walk.frames); add("/", t.walk.tics);
    add("|d", t.death.frames); add("/", t.death.tics);
    add("|p", t.pain.frames); add("/", t.pain.tics);
    add("|r", t.ready.frames); add("/", t.ready.tics);
    add("|a", t.attackA.frames); add("/", t.attackA.tics); add("/", t.fireA);
    add("|b", t.attackB.frames); add("/", t.attackB.tics); add("/", t.fireB);
    auto it = visuals_.find(key);
    if (it != visuals_.end()) return it->second.get();
    auto v = std::make_unique<Visual>();
    MonsterType type = t;
    v->build = [this, type](Visual& v) {
        const Bol* bol = nullptr;
        int base = lump(type.sprite, true, &bol);
        if (base < 0) return;
        auto frameImage = [&](int off, SpriteImage& img) {
            int li = base + off;
            return li < (int)bol->lumps().size() && decodeSprite(bol->data(li), bol->size(li), img);
        };
        // Frame times are 100 Hz ticks. A value of 128 and up flashes light in the monster's cell
        // (value - 128) and, in the game's death player (FUN_000473b1, state 8), lasts 5 ticks.
        // The engine gives those explosion frames 10 ticks so they read clearly.
        auto seqSeconds = [](const AnimSeq& s, int minTics, std::vector<float>* light = nullptr, int lightTics = 10) {
            std::vector<float> out;
            for (size_t i = 0; i < s.frames.size(); ++i) {
                int tics = i < s.tics.size() ? s.tics[i] : 9;
                float l = 0;
                if (tics >= 128) { l = (float)(tics - 128); tics = lightTics; }
                else tics = std::max(tics ? tics : 9, minTics);
                out.push_back(tics / kTicRate);
                if (light) light->push_back(l);
            }
            return out;
        };
        v.staged = std::make_shared<Visual::Staged>();
        Visual::Staged& st = *v.staged;
        int rot = rotationCount(*bol, base);
        std::vector<int> steps(type.walk.frames.begin(), type.walk.frames.end());
        if (steps.empty()) steps.push_back(0);
        std::vector<float> secs = seqSeconds(type.walk, 1);
        if (secs.empty()) secs.push_back(9 / kTicRate);
        float maxH = 0, maxW = 0;
        if (rot > 1) {
            std::map<int, std::vector<SplatSet>> views;  // walk frame -> 8 views
            std::map<int, std::array<HitMask, 8>> masks;
            for (int f : steps) {
                if (views.count(f)) continue;
                std::vector<SpriteImage> imgs(rot);
                std::vector<const SpriteImage*> ptrs;
                bool ok = true;
                for (int r = 0; r < rot && ok; ++r) {
                    ok = frameImage(f + r, imgs[r]);
                    ptrs.push_back(&imgs[r]);
                    if (ok) { maxH = std::max(maxH, (float)imgs[r].h); maxW = std::max(maxW, (float)imgs[r].w); }
                }
                std::vector<int> shifts;
                std::vector<SplatSet> vs = ok ? depthSprites(ptrs, &shifts) : std::vector<SplatSet>();
                if (vs.size() == 8) {
                    views[f] = std::move(vs);
                    for (int k = 0; k < 8; ++k) {  // 5-view sets: views 5-7 are 3-1 mirrored
                        masks[f][k] = rot == 8 ? makeHitMask(imgs[k], false) : k <= 4 ? makeHitMask(imgs[k], false)
                                                                                 : makeHitMask(imgs[8 - k], true);
                        masks[f][k].ox += shifts[k];  // the view's axis, as the depth sprite places it
                        addDepth(masks[f][k], views[f][k], k);
                    }
                }
            }
            std::vector<int> good;
            for (int f : steps)
                if (views.count(f)) good.push_back(f);
            for (int f : good) {
                v.rotHit.push_back(masks[f]);
                for (const auto& m : masks[f]) growBounds(m, v.hitRadius, v.hitHeight);
            }
            for (size_t i = 0; i < good.size(); ++i) {
                std::array<std::vector<SplatMorph>, 8> still, morph;
                const auto& a = views[good[i]];
                const auto& b = views[good[(i + 1) % good.size()]];
                for (int k = 0; k < 8; ++k) {
                    still[k] = stillSplats(a[k]);
                    if (good.size() > 1 && good[i] != good[(i + 1) % good.size()]) morph[k] = morphSplats(a[k], b[k]);
                }
                st.rot.still.push_back(std::move(still));
                st.rot.morph.push_back(std::move(morph));
                st.rot.seconds.push_back(secs[std::min(i, secs.size() - 1)]);
            }
        } else {
            std::vector<SpriteImage> imgs;
            for (int f : steps) {
                SpriteImage img;
                if (frameImage(f, img)) { imgs.push_back(img); maxH = std::max(maxH, (float)img.h); maxW = std::max(maxW, (float)img.w); }
            }
            for (const auto& img : imgs) {
                v.flatHit.push_back(makeHitMask(img, false));
                growBounds(v.flatHit.back(), v.hitRadius, v.hitHeight);
            }
            st.flat = stageFlat(imgs, secs, true);
        }
        auto single = [&](const AnimSeq& s, Visual::Staged::Anim& out, int minTics, std::vector<HitMask>& hit, int lightTics = 10) {
            // A lone frame 0 is the creature's own standing sprite: no animation at all.
            if (s.frames.empty() || (s.frames.size() == 1 && s.frames[0] == 0)) return;
            std::vector<SpriteImage> imgs;
            for (uint8_t f : s.frames) {
                SpriteImage img;
                if (frameImage(f, img)) { imgs.push_back(img); hit.push_back(makeHitMask(img, false)); }
            }
            std::vector<float> light;
            std::vector<float> secs = seqSeconds(s, minTics, &light, lightTics);
            // A last frame time of 0 means the final frame stays (a corpse); otherwise it ends.
            bool hold = s.tics.empty() || s.tics.back() == 0;
            if (!imgs.empty()) out = stageFlat(imgs, secs, false, light, hold);
        };
        // The hit reaction is usually one recoil frame (often the first death frame). Machines
        // list it at 1 tick, a flicker; the engine holds it at least 0.14 s so it can be seen.
        single(type.pain, st.pain, 14, v.painHit);
        for (const auto& m : v.painHit) growBounds(m, v.hitRadius, v.hitHeight);
        // Death frames: a little slower than the game's 9-tick frames, so they read clearly.
        single(type.death, st.death, 14, v.deathHit);
        // Attacks play at the game's own pace; the ready move unfolds before an attack.
        single(type.attackA, st.attackA, 3, v.attackAHit, 5);
        single(type.attackB, st.attackB, 3, v.attackBHit, 5);
        single(type.ready, st.ready, 3, v.readyHit);
        for (const auto* hs : {&v.attackAHit, &v.attackBHit, &v.readyHit})
            for (const auto& m : *hs) growBounds(m, v.hitRadius, v.hitHeight);
        v.fireA = type.fireA;
        v.fireB = type.fireB;
        // Legs keep pace with the ground: a full walk cycle covers about 0.8 of the creature's height.
        // Fast-flapping fliers (walk tics of 5 or less) animate with time instead.
        int ticSum = 0;
        for (uint8_t t : type.walk.tics) ticSum += t;
        v.rot.byDistance = type.walk.tics.empty() || ticSum > 5 * (int)type.walk.tics.size();
        v.rot.stride = std::max(24.0f, maxH * 0.8f);
        v.height = maxH > 0 ? maxH : 32;
        v.radius = std::max(8.0f, maxW * 0.33f);
    };
    const Visual* out = v.get();
    visuals_[key] = std::move(v);
    return out;
}

const Visual* World::spriteVisual(const std::string& key, std::vector<std::string> frames, bool cdFirst, bool loop,
                                  float frameSeconds, bool decal) {
    auto it = visuals_.find(key);
    if (it != visuals_.end()) return it->second.get();
    auto v = std::make_unique<Visual>();
    v->build = [this, frames, cdFirst, loop, frameSeconds, decal](Visual& v) {
        std::vector<SpriteImage> imgs;
        for (const auto& n : frames) {
            const Bol* bol = nullptr;
            int li = lump(n, cdFirst, &bol);
            SpriteImage img;
            if (li < 0) continue;
            bool ok = decal ? decodeDecal(bol->data(li), bol->size(li), img) : decodeSprite(bol->data(li), bol->size(li), img);
            if (ok) imgs.push_back(img);
        }
        if (imgs.empty()) return;
        v.staged = std::make_shared<Visual::Staged>();
        v.staged->flat = stageFlat(imgs, std::vector<float>(imgs.size(), frameSeconds), loop);
        int h = 0, w = 0;
        for (const auto& i : imgs) { h = std::max(h, i.h); w = std::max(w, i.w); }
        v.height = (float)h;
        v.radius = std::max(6.0f, w * 0.4f);
    };
    const Visual* out = v.get();
    visuals_[key] = std::move(v);
    return out;
}

const Visual* World::rotVisual(const std::string& key, const std::vector<std::string>& firsts, bool cdFirst,
                               float frameSeconds) {
    auto it = visuals_.find(key);
    if (it != visuals_.end()) return it->second.get();
    auto v = std::make_unique<Visual>();
    v->build = [this, firsts, cdFirst, frameSeconds](Visual& v) {
        std::vector<std::vector<SplatSet>> frames;
        float h = 0, w = 0;
        for (const auto& n : firsts) {
            const Bol* bol = nullptr;
            int base = lump(n, cdFirst, &bol);
            if (base < 0) continue;
            int rot = rotationCount(*bol, base);
            if (rot < 5) continue;
            std::vector<SpriteImage> imgs(rot);
            std::vector<const SpriteImage*> ptrs;
            bool ok = true;
            for (int r = 0; r < rot && ok; ++r) {
                ok = decodeSprite(bol->data(base + r), bol->size(base + r), imgs[r]);
                ptrs.push_back(&imgs[r]);
                h = std::max(h, (float)imgs[r].h);
                w = std::max(w, (float)imgs[r].w);
            }
            if (!ok) continue;
            auto vs = depthSprites(ptrs);
            if (vs.size() == 8) frames.push_back(std::move(vs));
        }
        if (frames.empty()) return;
        v.staged = std::make_shared<Visual::Staged>();
        for (size_t i = 0; i < frames.size(); ++i) {
            std::array<std::vector<SplatMorph>, 8> still, morph;
            for (int k = 0; k < 8; ++k) {
                still[k] = stillSplats(frames[i][k]);
                if (frames.size() > 1) morph[k] = morphSplats(frames[i][k], frames[(i + 1) % frames.size()][k]);
            }
            v.staged->rot.still.push_back(std::move(still));
            v.staged->rot.morph.push_back(std::move(morph));
            v.staged->rot.seconds.push_back(frameSeconds);
        }
        v.height = h;
        v.radius = std::max(4.0f, w * 0.4f);
    };
    const Visual* out = v.get();
    visuals_[key] = std::move(v);
    return out;
}

// A monster type's two weapons, from the ITEM records embedded in its BMON (see FORMATS.md).
void World::decodeWeapons(const MonsterType& t, MonsterWeapon* out) {
    static const int kViews[16] = {1, 1, 2, 4, 4, 5, 1, 8, 8, 9, 5, 3, 3, 1, 1, 16};
    for (int slot = 0; slot < 2; ++slot) {
        MonsterWeapon& w = out[slot];
        w = MonsterWeapon();
        if (!t.hasWeapon[slot]) continue;
        const uint8_t* p = t.weaponParams[slot];
        auto u16 = [&](int i) { return (int)(p[i] | p[i + 1] << 8); };
        auto s16 = [&](int i) { return (int)(int16_t)(p[i] | p[i + 1] << 8); };
        w.kind = t.weaponUse[slot];
        if (w.kind == 1) {
            for (int b = 0; b < 4; ++b) {
                w.acc[b] = s16(b * 2);
                w.dmgMin[b] = u16(8 + b * 2);
                w.dmgMax[b] = u16(16 + b * 2);
            }
            w.type = std::max(0, p[24] - 1);
        } else if (w.kind == 11) {
            for (int b = 0; b < 2; ++b) {
                w.acc[b] = s16(b * 2);
                w.dmgMin[b] = u16(8 + b * 2);
                w.dmgMax[b] = u16(16 + b * 2);
            }
            w.type = std::max(0, p[24] - 1);
        } else if (w.kind == 12) {
            w.dmgMin[0] = u16(8);
            w.dmgMax[0] = u16(16);
            w.type = 2;
        } else if (w.kind == 2) {
            int speed = u16(2);
            w.speed = std::min(speed, 80) / 8.0f * kTicRate;
            w.aimSpeed = speed / 8.0f * kTicRate;
            int views = kViews[p[4] & 15];
            w.lob = p[5] != 0;
            w.fuse = u16(6) / kTicRate;
            w.dmgMin[0] = p[8];
            w.dmgMax[0] = p[16];
            w.splash = p[18] ? p[10] : 0;
            w.type = std::max(0, p[28] - 1);
            char name[9] = {};
            std::memcpy(name, p + 30, 8);
            std::string sprite = lower(name);
            const Bol* bol = nullptr;
            int base = lump(sprite, false, &bol);
            if (base < 0) { w.kind = 0; continue; }
            int tics = std::max(1, (int)p[27]), flight = std::max(1, (int)p[39]);
            int impOff = u16(24), impCount = std::max(1, (int)p[26]);
            auto nameAt = [&](int i) { return i >= 0 && i < (int)bol->lumps().size() ? bol->lumps()[i].name : std::string(); };
            w.visual = "mproj:" + sprite;
            if (views > 1) {
                rotVisual("fx:" + w.visual, {nameAt(base)}, false, tics / kTicRate);   // the game never animates these
            } else {
                std::vector<std::string> frames;
                for (int f = 0; f < flight; ++f) frames.push_back(nameAt(base + f));
                spriteVisual("fx:" + w.visual, frames, false, true, tics / kTicRate);
            }
            w.impact = "mimp:" + sprite;
            std::vector<std::string> imp;
            for (int f = 0; f < impCount; ++f) imp.push_back(nameAt(base + impOff + f));
            spriteVisual("fx:" + w.impact, imp, false, false, tics / kTicRate);
            // Flight light by damage type (the game's pairs at 0x73c6d).
            static const float kLight[6][3] = {{0.2f, 0.18f, 0.1f}, {0.15f, 0.2f, 0.35f}, {0.35f, 0.25f, 0.08f},
                                               {0.35f, 0.2f, 0.05f}, {0.2f, 0.25f, 0.4f}, {0.2f, 0.2f, 0.2f}};
            int lt = std::clamp(w.type, 0, 5);
            w.lr = kLight[lt][0]; w.lg = kLight[lt][1]; w.lb = kLight[lt][2];
        } else {
            w.kind = 0;
        }
    }
}

const Visual* World::fx(const std::string& key) const {
    auto it = visuals_.find("fx:" + key);
    return it == visuals_.end() ? nullptr : it->second.get();
}

void World::requestEffects() {
    auto seq = [](const char* prefix, int a, int b) {
        std::vector<std::string> v;
        for (int i = a; i <= b; ++i) v.push_back(std::string(prefix) + std::to_string(i));
        return v;
    };
    // Impacts and explosions
    spriteVisual("fx:rcktexp", seq("rcktexp", 0, 3), false, false, 0.07f);
    spriteVisual("fx:grnaded", seq("grnaded", 1, 6), false, false, 0.05f);
    spriteVisual("fx:gboltd", seq("gboltd", 0, 2), false, false, 0.07f);
    spriteVisual("fx:lasprjd", seq("lasprjd", 0, 4), false, false, 0.05f);
    spriteVisual("fx:rdplsmd", seq("rdplsmd", 0, 3), false, false, 0.07f);
    spriteVisual("fx:approjd", seq("approjd", 0, 3), false, false, 0.07f);
    spriteVisual("fx:dblprjd", seq("dblprjd", 0, 2), false, false, 0.07f);
    spriteVisual("fx:puff", {"smoke1a", "smoke1b", "smoke1c", "smoke1d"}, false, false, 0.06f, true);
    spriteVisual("fx:smoke", {"smoke2c", "smoke2d"}, false, false, 0.25f, true);
    spriteVisual("fx:blood0", {"blood1a", "blood1b", "blood1c"}, false, false, 0.08f, true);
    spriteVisual("fx:blood1", {"blood2a", "blood2b", "blood2c"}, false, false, 0.08f, true);
    spriteVisual("fx:blood2", {"smoke1a", "smoke1b", "smoke1c", "smoke1d"}, false, false, 0.06f, true);
    spriteVisual("fx:expbit", {"expbit0"}, false, false, 1.0f);
    spriteVisual("fx:expchk", {"expchk0"}, false, false, 1.0f);
    spriteVisual("fx:expbitd", {"expbitd0", "expbitd1"}, false, false, 0.06f);
    spriteVisual("fx:expchkd", {"expchkd0", "expchkd1", "expchkd2"}, false, false, 0.06f);
    // Blood and machine bits thrown by hits (red, green, purple), and the gib animations.
    for (const char* c : {"rbl", "gbl", "pbl"}) {
        std::string s(c);
        spriteVisual("fx:" + s + "bit", {s + "bit0"}, false, false, 1.0f);
        spriteVisual("fx:" + s + "bitd", {s + "bitd0", s + "bitd1"}, false, false, 0.08f);
        spriteVisual("fx:" + s + "chk", {s + "chk0"}, false, false, 1.0f);
        spriteVisual("fx:" + s + "chkd", {s + "chkd0", s + "chkd1", s + "chkd2"}, false, false, 0.08f);
    }
    spriteVisual("fx:rbiodth", seq("rbiodth", 0, 3), false, false, 0.15f);
    spriteVisual("fx:gbiodth", seq("gbiodth", 0, 3), false, false, 0.15f);
    spriteVisual("fx:mechdth", seq("mechdth", 0, 2), false, false, 0.15f);
    spriteVisual("fx:s1dmoexp", {"s1dmoexp", "s2dmoexp", "s3dmoexp", "s4dmoexp"}, false, false, 0.15f);   // demo packs going off
    // Projectiles
    rotVisual("fx:gbolt", {"gbolt00", "gbolt10", "gbolt20"}, false, 0.05f);
    rotVisual("fx:rktprj", {"rktprj00", "rktprj10", "rktprj20"}, false, 0.05f);
    spriteVisual("fx:approj", {"approj00", "approj01"}, false, true, 0.06f);
    spriteVisual("fx:dblproj", seq("dblproj", 0, 2), false, true, 0.06f);
    spriteVisual("fx:grenade", seq("grenade", 0, 3), false, true, 0.06f);
    spriteVisual("fx:lasprj", {"lasprj0", "lasprj1"}, false, true, 0.05f);
    spriteVisual("fx:rdplsm", seq("rdplsm", 0, 2), false, true, 0.06f);
    spriteVisual("fx:barreld", {"barreld1", "barreld2", "barreld3"}, true, false, 0.07f);
}

void World::buildPending() {
    std::vector<Visual*> jobs;
    for (auto& [k, v] : visuals_)
        if (v->build) jobs.push_back(v.get());
    if (jobs.empty()) return;
    struct Shared {
        std::vector<Visual*>* jobs;
        SDL_AtomicInt next;
    } shared;
    shared.jobs = &jobs;
    SDL_SetAtomicInt(&shared.next, 0);
    auto worker = [](void* p) -> int {
        Shared* s = (Shared*)p;
        for (;;) {
            int i = SDL_AddAtomicInt(&s->next, 1);
            if (i >= (int)s->jobs->size()) break;
            Visual* v = (*s->jobs)[i];
            v->build(*v);
        }
        return 0;
    };
    int n = std::clamp(SDL_GetNumLogicalCPUCores(), 1, 16);
    n = std::min(n, (int)jobs.size());
    std::vector<SDL_Thread*> threads;
    for (int t = 1; t < n; ++t) threads.push_back(SDL_CreateThread(worker, "sprites", &shared));
    worker(&shared);
    for (SDL_Thread* t : threads) SDL_WaitThread(t, nullptr);
    for (Visual* v : jobs) {
        v->build = nullptr;
        if (!v->staged) continue;
        Visual::Staged& s = *v->staged;
        for (size_t i = 0; i < s.rot.still.size(); ++i) {
            std::array<ModelRange, 8> still, morph;
            for (int k = 0; k < 8; ++k) {
                still[k] = renderer_->addSplats(s.rot.still[i][k]);
                morph[k] = s.rot.morph[i][k].empty() ? still[k] : renderer_->addSplats(s.rot.morph[i][k]);
            }
            v->rot.still.push_back(still);
            v->rot.morph.push_back(morph);
        }
        v->rot.seconds = s.rot.seconds;
        v->flat = uploadFlat(renderer_, s.flat);
        v->pain = uploadFlat(renderer_, s.pain);
        v->death = uploadFlat(renderer_, s.death);
        v->attackA = uploadFlat(renderer_, s.attackA);
        v->attackB = uploadFlat(renderer_, s.attackB);
        v->ready = uploadFlat(renderer_, s.ready);
        v->staged.reset();
    }
}

// ---------------------------------------------------------------------------------------------
// Level queries

float World::surfaceY(const SurfaceLayer& s, int shift, float wx, float wz) const {
    int x = (int)std::floor(wx / kCellSize), z = (int)std::floor(wz / kCellSize);
    if (x < 0 || z < 0 || x >= kGrid || z >= kGrid) return 0;
    auto H = [&](int cx, int cz) { return (cx >= kGrid || cz >= kGrid) ? 0.0f : (float)(int16_t)s.height[cz * kGrid + cx]; };
    float h0 = H(x, z);
    int shape = shift < 0 ? 0 : (level_->shape[z * kGrid + x] >> shift) & 7;
    if (shape == 0) return h0;
    float h1 = H(x + 1, z), h2 = H(x + 1, z + 1), h3 = H(x, z + 1);
    float u = wx / kCellSize - x, v = wz / kCellSize - z;
    if (shape == 3) {
        if (u + v <= 1) return h0 + u * (h1 - h0) + v * (h3 - h0);
        return h2 + (1 - u) * (h3 - h2) + (1 - v) * (h1 - h2);
    }
    if (u >= v) return h0 + u * (h1 - h0) + v * (h2 - h1);
    return h0 + v * (h3 - h0) + u * (h2 - h3);
}

// Highest walkable surface at (wx, wz) not above y; optionally the lowest overhead surface.
float World::groundBelow(float wx, float wz, float y, float* ceiling) const {
    const Level& L = *level_;
    int x = (int)std::floor(wx / kCellSize), z = (int)std::floor(wz / kCellSize);
    float best = -1e9f, top = 1e9f;
    if (x < 0 || z < 0 || x >= kGrid || z >= kGrid) { if (ceiling) *ceiling = top; return 0; }
    int i = z * kGrid + x;
    auto consider = [&](float h, bool walkable) {
        if (h <= y + 0.5f) { if (walkable) best = std::max(best, h); }
        else top = std::min(top, h);
    };
    if (L.floor.tex[i] & 0x3fff) consider(surfaceY(L.floor, 0, wx, wz), true);
    if (L.extra1.tex[i] & 0x3fff) consider((float)(int16_t)L.extra1.height[i], true);
    if (L.extra2.tex[i] & 0x3fff) consider((float)(int16_t)L.extra2.height[i], true);
    if (L.ceiling.tex[i] & 0x3fff) top = std::min(top, surfaceY(L.ceiling, 3, wx, wz));
    for (int ci : cubesInCell_[i]) {
        const auto& c = L.cubes[ci];
        auto u16 = [&](int k) { return (int)(c[k * 2] | c[k * 2 + 1] << 8); };
        float x0 = (float)u16(1), z0 = (float)u16(2), base = (float)(int16_t)u16(3);
        float x1 = x0 + c[8], z1 = z0 + c[10], hgt = cubeHeight_[ci];
        if (wx < x0 || wx > x1 || wz < z0 || wz > z1 || hgt <= 0) continue;
        if (base + hgt <= y + 0.5f) best = std::max(best, base + hgt);
        else if (base > y) top = std::min(top, base);
        else top = std::min(top, base);  // inside the cube: treat as blocked overhead
    }
    if (ceiling) *ceiling = top;
    return best < -1e8f ? (L.floor.tex[i] ? (float)(int16_t)L.floor.height[i] : 0.0f) : best;
}


// Whether the wall between two neighbouring cells (if any) spans part of y0..y1; `bottom` gets
// its foot when it does.
bool World::edgeBlocks(int cx, int cz, int nx, int nz, float y0, float y1, float* bottom) const {
    const Level& L = *level_;
    auto check = [&](const EdgeLayer& e, int x, int z) {
        if (x < 0 || z < 0 || x >= kGrid || z >= kGrid) return true;
        int i = z * kGrid + x;
        if (!e.texFront[i] || (e.flags[i] & 8)) return false;
        float span = e.vEnd[i] ? (float)(e.vEnd[i] - e.vStart[i] + 1) : (float)renderer_->wallHeight(e.texFront[i]);
        float b = (float)(int16_t)e.base[i], t = b + span;
        if (b < y1 && t > y0) { if (bottom) *bottom = b; return true; }
        return false;
    };
    if (nx == cx + 1) return check(L.west, nx, cz);
    if (nx == cx - 1) return check(L.west, cx, cz);
    if (nz == cz + 1) return check(L.north, cx, nz);
    if (nz == cz - 1) return check(L.north, cx, cz);
    return false;
}

bool World::stepTarget(int cx, int cz, int nx, int nz, float y, float height, float& newY) const {
    const Level& L = *level_;
    if (nx < 0 || nz < 0 || nx >= L.width || nz >= L.height) return false;
    if (std::abs(nx - cx) + std::abs(nz - cz) == 2) {  // diagonal: both orthogonal routes must be open
        float a, b;
        if (!stepTarget(cx, cz, nx, cz, y, height, a) || !stepTarget(cx, cz, cx, nz, y, height, b)) return false;
        if (!stepTarget(nx, cz, nx, nz, a, height, a) || !stepTarget(cx, nz, nx, nz, b, height, b)) return false;
    } else if (edgeBlocks(cx, cz, nx, nz, y + 20, y + height)) {
        return false;
    }
    float wx = (nx + 0.5f) * kCellSize, wz = (nz + 0.5f) * kCellSize;
    float ceil;
    float g = groundBelow(wx, wz, y + 24, &ceil);
    if (g < y - 24 || g > y + 24) return false;
    if (ceil - g < height * 0.85f) return false;
    // Check the cell's corners too, so monsters don't step half off a ledge.
    for (int k = 0; k < 4; ++k) {
        float px = wx + ((k & 1) ? 20.0f : -20.0f), pz = wz + ((k & 2) ? 20.0f : -20.0f);
        float gc = groundBelow(px, pz, g + 24);
        if (std::abs(gc - g) > 30) return false;
    }
    newY = g;
    return true;
}

float World::cellLight(float wx, float wz) const {
    int x = (int)std::floor(wx / kCellSize), z = (int)std::floor(wz / kCellSize);
    if (x < 0 || z < 0 || x >= kGrid || z >= kGrid) return 0;
    return (float)(int8_t)level_->floor.extra[z * kGrid + x];
}


// ---------------------------------------------------------------------------------------------
// Collision

// The level's triangles for rays: the parts that stay put, then the blocks and the parts scripts
// move (refreshMoving replaces just those).
void World::buildCollision() {
    tris_ = renderer_->collisionTriangles();
    staticTris_ = tris_.size() / 9;
    const auto& m = renderer_->movingTriangles();
    tris_.insert(tris_.end(), m.begin(), m.end());
    triCells_.assign(kGrid * kGrid, {});
    movingTriCells_.clear();
    indexTriangles(0);
}

// Lists triangles from `from` on in the cells they touch, noting the cells of those past
// staticTris_ (the moving parts').
void World::indexTriangles(size_t from) {
    std::vector<uint8_t> seen;
    for (size_t t = from; t * 9 < tris_.size(); ++t) {
        const float* v = &tris_[t * 9];
        float x0 = std::min({v[0], v[3], v[6]}), x1 = std::max({v[0], v[3], v[6]});
        float z0 = std::min({v[2], v[5], v[8]}), z1 = std::max({v[2], v[5], v[8]});
        int cx0 = std::clamp((int)std::floor((x0 - 0.5f) / kCellSize), 0, kGrid - 1), cx1 = std::clamp((int)std::floor((x1 + 0.5f) / kCellSize), 0, kGrid - 1);
        int cz0 = std::clamp((int)std::floor((z0 - 0.5f) / kCellSize), 0, kGrid - 1), cz1 = std::clamp((int)std::floor((z1 + 0.5f) / kCellSize), 0, kGrid - 1);
        for (int z = cz0; z <= cz1; ++z)
            for (int x = cx0; x <= cx1; ++x) {
                int c = z * kGrid + x;
                triCells_[c].push_back((int)t);
                if (t >= staticTris_) {
                    if (seen.empty()) seen.assign(kGrid * kGrid, 0);
                    if (!seen[c]) { seen[c] = 1; movingTriCells_.push_back(c); }
                }
            }
    }
}

// The moving parts have moved: their geometry and their triangles, again.
void World::refreshMoving() {
    movedGeometry_ = false;
    if (!levelEdit_) return;
    renderer_->buildMoving(*levelEdit_);
    for (int c : movingTriCells_) {
        auto& list = triCells_[c];
        while (!list.empty() && list.back() >= (int)staticTris_) list.pop_back();
    }
    movingTriCells_.clear();
    tris_.resize(staticTris_ * 9);
    const auto& m = renderer_->movingTriangles();
    tris_.insert(tris_.end(), m.begin(), m.end());
    indexTriangles(staticTris_);
}

// The part of a door's panel still in its doorway, after the opening offset: the panel's line
// (x0, z0)-(x1, z1) and its height y0..y1. False once it is out of the way. Open, a panel has
// moved its height less 1 or 63 units along (FUN_000132a5), as in the game; the engine stopped
// 4 and 6 short up to 0.5.2, which left a solid post in the middle of BIOSTOR2's double doors.
bool World::doorPanel(const Door& d, float& x0, float& z0, float& x1, float& z1, float& y0, float& y1) const {
    float e = d.open * d.open * (3 - 2 * d.open);   // as drawn
    y0 = d.base;
    y1 = d.base + d.height;
    if (d.ew) { x0 = (float)d.spec.x; x1 = x0 + kCellSize; z0 = z1 = (float)(d.spec.z + d.spec.offset); }
    else { z0 = (float)d.spec.z; z1 = z0 + kCellSize; x0 = x1 = (float)(d.spec.x + d.spec.offset); }
    if (d.vertical) {
        float oy = d.dir * e * (d.height - 1);
        y0 = std::max(y0, d.base + oy);
        y1 = std::min(y1, d.base + d.height + oy);
        return y1 - y0 > 3;
    }
    float sl = d.dir * e * (kCellSize - 1);
    float& a = d.ew ? x0 : z0;
    float& b = d.ew ? x1 : z1;
    float lo = a, hi = b;
    a = std::max(lo, lo + sl);
    b = std::min(hi, hi + sl);
    return b - a > 3;
}

bool World::rayHitsDoor(const Door& d, const float o[3], const float dir[3], float maxT, float& t, float n[3]) const {
    float x0, z0, x1, z1, y0, y1;
    if (!doorPanel(d, x0, z0, x1, z1, y0, y1)) return false;
    int ax = d.ew ? 2 : 0;          // the panel's plane: z = z0 (east-west) or x = x0
    float plane = d.ew ? z0 : x0;
    if (std::abs(dir[ax]) < 1e-6f) return false;
    float tt = (plane - o[ax]) / dir[ax];
    if (tt <= 0.01f || tt >= maxT) return false;
    float hx = o[0] + dir[0] * tt, hy = o[1] + dir[1] * tt, hz = o[2] + dir[2] * tt;
    if (hy < y0 || hy > y1) return false;
    if (d.ew ? (hx < x0 || hx > x1) : (hz < z0 || hz > z1)) return false;
    t = tt;
    if (n) {
        n[0] = n[1] = n[2] = 0;
        n[ax] = dir[ax] > 0 ? -1.0f : 1.0f;
    }
    return true;
}

bool World::raycast(const float o[3], const float d[3], float maxT, float& tHit, float nrm[3], int* doorOut, bool pick, int sight) const {
    // 2D DDA over the cell grid; Moller-Trumbore against the triangles of each visited cell, and
    // the panel of a door in that cell where it still blocks the doorway.
    float best = maxT;
    bool hit = false;
    if (doorOut) *doorOut = -1;
    bool doorsKnown = doorCell_.size() == (size_t)(kGrid * kGrid);
    int cx = (int)std::floor(o[0] / kCellSize), cz = (int)std::floor(o[2] / kCellSize);
    int sx = d[0] > 0 ? 1 : -1, sz = d[2] > 0 ? 1 : -1;
    float tdx = std::abs(d[0]) > 1e-6f ? kCellSize / std::abs(d[0]) : 1e30f;
    float tdz = std::abs(d[2]) > 1e-6f ? kCellSize / std::abs(d[2]) : 1e30f;
    float nxb = (sx > 0 ? (cx + 1) * kCellSize : cx * kCellSize), nzb = (sz > 0 ? (cz + 1) * kCellSize : cz * kCellSize);
    float tmx = std::abs(d[0]) > 1e-6f ? (nxb - o[0]) / d[0] : 1e30f;
    float tmz = std::abs(d[2]) > 1e-6f ? (nzb - o[2]) / d[2] : 1e30f;
    float tCell = 0;
    for (int guard = 0; guard < 256; ++guard) {
        if (cx >= 0 && cz >= 0 && cx < kGrid && cz < kGrid) {
            for (int ti : triCells_[cz * kGrid + cx]) {
                const float* v = &tris_[(size_t)ti * 9];
                float e1[3] = {v[3] - v[0], v[4] - v[1], v[5] - v[2]}, e2[3] = {v[6] - v[0], v[7] - v[1], v[8] - v[2]};
                float p[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
                float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
                if (std::abs(det) < 1e-8f) continue;
                float inv = 1.0f / det;
                float s[3] = {o[0] - v[0], o[1] - v[1], o[2] - v[2]};
                float u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
                if (u < 0 || u > 1) continue;
                float q[3] = {s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0]};
                float w = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inv;
                if (w < 0 || u + w > 1) continue;
                float t = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
                if (t > 0.01f && t < best && pick) {   // through a hole in the texture
                    bool mv = (size_t)ti >= staticTris_;
                    if (renderer_->holeAt(mv, mv ? ti - staticTris_ : (size_t)ti, u, w)) continue;
                }
                if (t > 0.01f && t < best && sight) {   // a creature's sight: past blocks, and windows and grates
                    bool mv = (size_t)ti >= staticTris_;
                    int f = renderer_->sightFlags(mv, mv ? ti - staticTris_ : (size_t)ti);
                    if ((f & 4) || (sight == 1 && (f & 2))) continue;
                }
                if (t > 0.01f && t < best) {
                    best = t;
                    hit = true;
                    if (doorOut) *doorOut = -1;
                    if (nrm) {
                        float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
                        float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                        float sg = (n[0] * d[0] + n[1] * d[1] + n[2] * d[2]) > 0 ? -1.0f : 1.0f;
                        for (int a = 0; a < 3; ++a) nrm[a] = l > 0 ? n[a] / l * sg : 0;
                    }
                }
            }
        }
        if (doorsKnown && cx >= 0 && cz >= 0 && cx < kGrid && cz < kGrid && doorCell_[cz * kGrid + cx]) {
            int di = doorCell_[cz * kGrid + cx] - 1;
            float t, n[3];
            // A door drawn on a wall's own line (BIOLAB's hidden door at 53, 46, offset 0, over its
            // holographic wall) is in front of it: its panel is drawn 3 units either side of the
            // line, so it wins against a wall up to that far behind its middle.
            // A creature's sight passes a door unless it is fully shut (FUN_00020d47: state 0), even
            // one only starting to open or not yet quite closed.
            const Door& dr = doors_[di];
            bool shut = dr.open <= 0 && !dr.opening;
            if ((!sight || shut) && rayHitsDoor(dr, o, d, best + 3.5f, t, n)) {
                best = t;
                hit = true;
                if (doorOut) *doorOut = di;
                if (nrm) { nrm[0] = n[0]; nrm[1] = n[1]; nrm[2] = n[2]; }
            }
        }
        float tNext = std::min(tmx, tmz);
        if (hit && best <= tNext + 1.0f) break;
        if (tNext > maxT) break;
        tCell = tNext;
        if (tmx < tmz) { tmx += tdx; cx += sx; } else { tmz += tdz; cz += sz; }
        if (cx < -1 || cz < -1 || cx > kGrid || cz > kGrid) break;
    }
    (void)tCell;
    tHit = best;
    return hit;
}

bool World::lineOfSight(const float a[3], const float b[3], bool solid) const {
    uint64_t t0 = SDL_GetTicksNS();
    float d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len < 1) return true;
    for (float& v : d) v /= len;
    float t;
    static const bool solidAll = SDL_getenv("CYCLONES_SOLIDSIGHT") != nullptr;   // testing: sight as up to 0.5.2 (all in the way)
    bool clear = !raycast(a, d, len - 2, t, nullptr, nullptr, false, solidAll ? 0 : solid ? 2 : 1);
    losNs_ += SDL_GetTicksNS() - t0;
    return clear;
}


// ---------------------------------------------------------------------------------------------
// Level load

// The speed the game really reaches (units a second): it moves a creature tics x stats[0] / 32
// units in one jump when each walk frame ends (100 ticks a second), and a jump that crosses a cell
// centre stops there, losing the rest. Simulated over many cells, the walk cycle carrying on.
float World::originalWalkSpeed(const MonsterType& t) {
    int speed = t.stats[0];
    if (!speed) return 0;
    std::vector<int> tics;
    for (uint8_t v : t.walk.tics) tics.push_back(std::max(1, (int)v));
    if (tics.empty()) tics.push_back(5);
    float pos = 0;
    long ticks = 0;
    int cells = 0;
    size_t f = 0;
    while (cells < 64) {
        ticks += tics[f];
        pos += tics[f] * speed / 32.0f;
        f = (f + 1) % tics.size();
        if (pos >= 64) { pos = 0; ++cells; }
    }
    return cells * 64.0f / (ticks / 100.0f);
}

void World::load(const Level& L, const LevelEntities& E, unsigned seed) {
    level_ = &L;
    initSurfAnims();
    rng_.seed(seed);
    monsters_.clear();
    items_.clear();
    objects_.clear();
    doors_.clear();
    shots_.clear();
    effects_.clear();
    particles_.clear();
    lights_.clear();
    pending_.clear();
    sounds_.clear();
    kills_ = 0;
    explored_.assign(kGrid * kGrid, 0);
    exploreTimer_ = 0;
    occupied_.assign(kGrid * kGrid, 0);
    fixed_.assign(kGrid * kGrid, 0);
    playerCellX_ = playerCellZ_ = -1;   // the activity bubble is worked out afresh
    cubesInCell_.assign(kGrid * kGrid, {});
    cubeHeight_.assign(L.cubes.size(), 0.0f);
    for (size_t ci = 0; ci < L.cubes.size(); ++ci) {
        const auto& c = L.cubes[ci];
        auto u16 = [&](int k) { return (int)(c[k * 2] | c[k * 2 + 1] << 8); };
        // A cube's height is [13] - [12]; when that is not positive the game uses its side
        // texture's height (FUN_00013b14), which makes the floor-to-ceiling pillars.
        int h = (int16_t)(u16(13) - u16(12));
        if (h < 1)
            for (int k = 8; k <= 11; ++k)
                if (u16(k)) h = renderer_->wallHeight(u16(k));
        // One with neither height nor side pictures is still there for the game's collision: a
        // flat box at its base, which creatures and you stand on 1 unit over (FUN_00047ed3 sets
        // a creature on a block's top + 1). BIOSTOR2's lift at (1, 29) is one, invisible, level
        // with the floor; it rose to the floor at 442 without you, up to 0.5.2.
        if (h < 1 && c[8] > 0 && c[10] > 0) h = 1;
        cubeHeight_[ci] = (float)std::max(0, h);
        int x0 = u16(1) / kCellSize, z0 = u16(2) / kCellSize, x1 = (u16(1) + c[8]) / kCellSize, z1 = (u16(2) + c[10]) / kCellSize;
        for (int z = std::max(0, z0); z <= std::min(kGrid - 1, z1); ++z)
            for (int x = std::max(0, x0); x <= std::min(kGrid - 1, x1); ++x) cubesInCell_[z * kGrid + x].push_back((int)ci);
    }
    initScripts(E);
    initScans();   // SDILAB's security scan rooms (scan.cpp)
    findMovingParts();
    renderer_->buildLevel(L);
    buildCollision();
    keepsakeHere_ = lower(L.name) == "base";
    if (keepsakeHere_) buildKeepsake();
    uint64_t t0 = SDL_GetTicksNS();

    // Doors
    std::vector<DoorGeom> geoms;
    for (const auto& d : E.doors) {
        if (!(d.skill & (1 << difficulty))) continue;
        int tex = data_->cd.find(d.texture);
        if (tex < 0) continue;
        int number = tex - data_->startWalls;
        int th = renderer_->wallHeight(number);
        if (th <= 0) continue;
        Door door;
        door.spec = d;
        door.ew = d.runsEastWest();
        door.style = d.style();
        // As the game's loader sets a door up (FUN_00010577, FUN_00013199): flag 0x80 makes the
        // panel slide along its run, otherwise it moves up and down; a negative speed reverses
        // the direction (sinking, or sliding toward -x / -z); flags 0x02, 0x20 and 0x40 start it
        // open, opening or closing; bit 0x8000 of the next field keeps it open once opened.
        door.vertical = !(d.flags & 0x80);
        door.dir = d.speed < 0 ? -1.0f : 1.0f;
        door.staysOpen = (d.flags2 & 0x8000) != 0;
        if (d.flags & 0x02) { door.open = 1; door.opening = true; }
        else if (d.flags & 0x20) { door.open = 0; door.opening = true; }
        else if (d.flags & 0x40) { door.open = 1; door.opening = false; }
        door.height = d.height > 0 ? (float)d.height : (float)th;
        door.base = (float)d.y;
        door.cellX = d.x / kCellSize;
        door.cellZ = d.z / kCellSize;
        door.locked = (d.flags & 0x01) != 0;
        // A locked door that only a script the engine can't run yet opens or unlocks stays
        // openable meanwhile (scripts it can run do it themselves).
        for (size_t si = 0; si < scripts_.size(); ++si) {
            if (!scriptUnsupported_[si]) continue;
            for (const auto& a : scripts_[si].acts)
                if ((a.op == 2 || a.op == 3 || a.op == 18 || a.op == 21) && a.arg[0] == door.cellX && a.arg[1] == door.cellZ)
                    door.scripted = true;
        }
        DoorGeom g;
        if (door.ew) { g.x0 = (float)d.x; g.x1 = (float)d.x + kCellSize; g.z0 = g.z1 = (float)(d.z + d.offset); }
        else { g.z0 = (float)d.z; g.z1 = (float)d.z + kCellSize; g.x0 = g.x1 = (float)(d.x + d.offset); }
        door.cx = (g.x0 + g.x1) * 0.5f;
        door.cz = (g.z0 + g.z1) * 0.5f;
        g.base = door.base;
        g.height = door.height;
        g.vStart = (float)d.vStart;
        // The game draws a panel flat on its line, and only inside its own cell; the engine gives
        // it a 6-unit slab, but no thicker than keeps both faces half a unit inside that cell. A
        // panel on the cell's edge (BIOSTOR2's key-card hatch, flush with the wall round it) stays
        // flat instead of standing out of the wall as a little box, and one 2 units in from an
        // edge with a wall on it (MUSEUM2's sliding marble walls, behind the brick of the next
        // room) stays behind that wall: a 6-unit slab came through it, and a 4-unit one lay in its
        // plane and covered it (the engine up to 0.5.2).
        g.thick = std::clamp(2.0f * (float)std::min(d.offset, kCellSize - d.offset) - 1.0f, 0.5f, 6.0f);
        g.light = cellLight(door.cx, door.cz);
        g.texture = number;
        // No cap on the slab where a floor or ceiling of its cell lies on it: SDI2's sliding doors
        // at (4, 35) and (1, 35) reach from the lower floor (100) up to the deck of the launch room
        // over them (228), and their tops showed through that floor as two thin dark streaks.
        {
            const Level& Lv = *level_;
            int ci = door.cellZ * kGrid + door.cellX;
            auto near = [&](float y) {
                const SurfaceLayer* layers[4] = {&Lv.floor, &Lv.ceiling, &Lv.extra1, &Lv.extra2};
                for (const SurfaceLayer* s : layers)
                    if ((s->tex[ci] & 0x3fff) && std::abs((float)(int16_t)s->height[ci] - y) < 1.5f) return true;
                return false;
            };
            if (ci >= 0 && ci < kGrid * kGrid) {
                g.capTop = !near(g.base + g.height);
                g.capBottom = !near(g.base);
            }
        }
        door.rate = std::max(1, std::abs(d.speed)) * 100.0f;   // |speed| units a tick
        geoms.push_back(g);
        doors_.push_back(door);
    }
    renderer_->buildDoors(geoms);
    // Panels side by side in one doorway (the same line, neighbouring cells, the same height)
    // open together, as the level's scripts make them do in the game.
    for (size_t i = 0; i < doors_.size(); ++i) doors_[i].group = (int)i;
    auto root = [&](int i) {
        while (doors_[i].group != i) {
            doors_[i].group = doors_[doors_[i].group].group;
            i = doors_[i].group;
        }
        return i;
    };
    // Panels that a script moves are left to it: in 59 of the game's 70 such pairs each panel's
    // own script toggles its partner when it is used (BIOSTOR2's double doors, for one), and
    // opening the pair here as well made that toggle shut the partner again (the engine up to
    // 0.5.2, from before it ran the scripts).
    std::vector<bool> scriptMoved(doors_.size(), false);
    for (const auto& s : scripts_)
        for (const auto& a : s.acts)
            if (a.op == 2 || a.op == 18 || a.op == 19)
                for (size_t i = 0; i < doors_.size(); ++i)
                    if (doors_[i].cellX == a.arg[0] && doors_[i].cellZ == a.arg[1]) scriptMoved[i] = true;
    for (size_t i = 0; i < doors_.size(); ++i)
        for (size_t j = i + 1; j < doors_.size(); ++j) {
            const Door &a = doors_[i], &b = doors_[j];
            if (scriptMoved[i] || scriptMoved[j]) continue;
            if (a.ew != b.ew || std::abs(a.base - b.base) > 2 || std::abs(a.height - b.height) > 2) continue;
            bool line = a.ew ? a.spec.z + a.spec.offset == b.spec.z + b.spec.offset : a.spec.x + a.spec.offset == b.spec.x + b.spec.offset;
            bool next = a.ew ? (a.cellZ == b.cellZ && std::abs(a.cellX - b.cellX) == 1) : (a.cellX == b.cellX && std::abs(a.cellZ - b.cellZ) == 1);
            if (line && next) doors_[root((int)j)].group = root((int)i);
        }
    for (size_t i = 0; i < doors_.size(); ++i) doors_[i].group = root((int)i);

    // Request every sprite this level needs, then build them in parallel.
    std::map<const MonsterType*, std::array<MonsterWeapon, 2>> arsenal;
    for (const auto& s : E.monsters) {
        if (!(s.skill & (1 << difficulty))) continue;
        const MonsterType* t = E.type(s.type);
        if (!t) continue;
        monsterVisual(*t);
        if (!arsenal.count(t)) decodeWeapons(*t, arsenal[t].data());
        if (s.hasDrop) { Item probe; itemVisual(s.dropSprite, probe); }
    }
    for (const auto& s : E.items)
        if (s.skill & (1 << difficulty)) {
            Item probe;
            itemVisual(s.sprite, probe);
            if (s.use == 7) {   // a demo pack's "set" picture, for when it is placed (+0x48)
                int i = data_->hd.find(s.sprite);
                if (i >= 0 && i + 2 < (int)data_->hd.lumps().size()) itemVisual(data_->hd.lumps()[i + 2].name, probe);
            }
        }
    for (const auto& s : E.objects) {
        if (!(s.skill & (1 << difficulty))) continue;
        // Props the level animates (surfanim kind 3: fans, power coils, generators) get every
        // picture of their range.
        AnimRange r = spriteAnim(3, s.sprite);
        if (r.number >= 0) spriteVisual("obj:anim" + std::to_string(r.first), r.frames, true, true, 0.1f);
        else spriteVisual("obj:" + lower(s.sprite), {s.sprite}, true, false, 0.09f);
    }
    // The pictures scripts give props (action 55: 'startobjects' + u16(2)).
    if (int start = data_->cd.find("startobjects"); start >= 0)
        for (const auto& sc : E.scripts)
            for (const auto& act : sc.acts) {
                if (act.op != 55) continue;
                int pic = act.arg[2] | act.arg[3] << 8;
                if (start + pic >= (int)data_->cd.lumps().size()) continue;
                std::string nm = data_->cd.lumps()[start + pic].name;
                AnimRange r = spriteAnim(3, nm);
                if (r.number >= 0) spriteVisual("obj:anim" + std::to_string(r.first), r.frames, true, true, 0.1f);
                else spriteVisual("obj:" + lower(nm), {nm}, true, false, 0.09f);
            }
    requestEffects();
    buildPending();

    // Monsters. Each one the difficulty keeps takes the next slot of the game's table, even one
    // the engine can't show (FUN_00010d05 / FUN_00011f54).
    slotMonster_.clear();
    for (const auto& s : E.monsters) {
        if (!(s.skill & (1 << difficulty))) continue;
        slotMonster_.push_back(-1);
        const MonsterType* t = E.type(s.type);
        if (!t) continue;
        const Visual* v = monsterVisual(*t);
        if (!v || (v->rot.empty() && v->flat.empty())) continue;
        auto m = std::make_unique<Monster>();
        m->vis = v;
        m->type = t;
        m->index = (int)monsters_.size();
        m->slot = (int)slotMonster_.size() - 1;
        m->recordId = s.id;
        m->group = s.group;
        m->dropKind = s.dropKind;
        m->dropInvPic = s.dropInvPic;
        slotMonster_.back() = m->index;
        m->aiClass = s.tag & 0xff;
        m->startMode = s.startMode;
        m->sight = s.sight;
        m->trigger = s.trigger;
        m->bandA = s.bandA;
        m->bandB = s.bandB;
        for (int k = 0; k < 4; ++k) m->armour[k] = s.armour[k];
        m->species = t->stats[4];
        m->hover = t->stats[2];
        m->eye = t->eye ? (float)t->eye : std::max(8.0f, t->stats[6] * 0.5f);
        m->evasion = t->evasion;
        m->accuracy = t->accuracy;
        if (arsenal.count(t)) { m->weapon[0] = arsenal[t][0]; m->weapon[1] = arsenal[t][1]; }
        m->sndPain = s.sndPain;
        m->sndMelee = s.sndMelee;
        m->sndFire = s.sndFire;
        m->sndDeath = s.sndDeath;
        m->hasDrop = s.hasDrop;
        m->dropUse = s.dropUse;
        m->dropAmount = s.dropAmount;
        m->dropSprite = s.dropSprite;
        m->dropScript = s.dropScript;
        m->script = s.script;
        std::memcpy(m->dropParams, s.dropParams, sizeof(m->dropParams));
        std::string sp = lower(t->sprite);
        m->barrel = sp.find("bar") != std::string::npos;
        // A creature with no walk cycle that stands on the floor is a gun post or a prop. Gunballs
        // have none either, but they fly, and the game moves them like any flier: they fire, fold,
        // float on a cell and open up again (FUN_0002a539).
        m->stationary = (t->walk.frames.size() <= 1 && m->hover <= 0) || m->barrel;
        bool mech = sp.rfind("cyc", 0) == 0 || sp.rfind("jbot", 0) == 0 || sp.rfind("gpst", 0) == 0 ||
                    sp.rfind("cgn", 0) == 0 || sp.rfind("gren", 0) == 0 || sp.rfind("mine", 0) == 0 || m->barrel;
        bool green = sp.rfind("al", 0) == 0 || sp.rfind("blob", 0) == 0 || sp.rfind("fly", 0) == 0 ||
                     sp.rfind("wsp", 0) == 0 || sp.rfind("q", 0) == 0 || sp.rfind("gas", 0) == 0;
        m->blood = mech ? 2 : green ? 1 : 0;
        m->x = (float)s.x;
        m->z = (float)s.z;
        if (m->species == 10) m->z += 32;   // the Queen sits half a cell further south
        m->y = groundBelow(m->x, m->z, (float)s.y + 8) + m->hover;
        m->ground = m->ty = m->y - m->hover;
        // The game puts a creature at its record's height + its hover height (FUN_00010d05) and
        // only brings it down to the floor as a walk step ends, so a gun hung from the ceiling,
        // which never walks, stays where the record puts it even over a pit (BIOLAB's laser over
        // the sunken floor at 51, 51).
        if (m->stationary && m->hover > 0) {
            if (logScripts() || SDL_getenv("CYCLONES_LIST")) {
                if (std::abs((float)s.y + m->hover - m->y) > 1)
                    SDL_Log("dbg hung creature at %d,%d: %.0f (the floor under it %.0f)", s.x / kCellSize, s.z / kCellSize,
                            (float)s.y + m->hover, m->ground);
            }
            m->y = (float)s.y + m->hover;
        } else if (m->hover > 0) {
            // A flier starts there too, + 1 (as the running game shows: SDI1's Wasps at 251 over
            // the 100 floor), and makes for its height only as it moves (ai.cpp).
            m->y = (float)s.y + m->hover + 1;
        }
        if (m->hover > 0 && !m->stationary) {
            // A few fliers are placed with their box through a catwalk or over the ceiling (SDI1's
            // Flybot 18 and Wasps 16 and 17, over its 394 ceiling; SDI2's Flybots 18, 25, 26 and 31,
            // through the catwalks at 250), so that even a walk frame's sinking leaves it there. The
            // game leaves them so: the ones over the ceiling are never seen, and no step of the
            // others' stands (see ai.cpp), so they never move. The enhanced AI starts them under
            // it. (SDI1's Flybots at 336 under the 394 ceiling touch it, but get away sinking.)
            float px[6], pz[6], room;
            int n = boxPoints(*m, px, pz);
            const float cruise = groundBelow(m->x, m->z, m->y + 17) + 2 + m->hover, frame = walkFrameTics(*m);
            float first = m->y + std::clamp(cruise - m->y, -frame, frame);   // its first walk frame's height
            if (!flierFits(*m, px, pz, n, first, &room)) {
                float y = room - boxHeight(*m) - 1, y2 = y;
                bool settle = enhancedAI && room < 1e8f && flierFits(*m, px, pz, n, y2) && y2 <= y + 0.5f;
                if (SDL_getenv("CYCLONES_LIST"))
                    SDL_Log("dbg flier %d %s at %d,%d: y %.0f box %.0f..%.0f, room up to %.0f%s", (int)monsters_.size(),
                            t->name.c_str(), s.x / kCellSize, s.z / kCellSize, m->y, m->y, m->y + boxHeight(*m), room,
                            settle ? ", starts lower (enhanced AI)" : ", stuck as in the game");
                if (settle) m->y = y;
            }
        }
        m->yaw = frand(rng_, -kPi, kPi);
        m->cx = std::clamp((int)(m->x / kCellSize), 0, kGrid - 1);
        m->cz = std::clamp((int)(m->z / kCellSize), 0, kGrid - 1);
        m->ncx = m->cx;
        m->ncz = m->cz;
        m->wait = frand(rng_, 0, 3);
        m->hp = std::max(1, s.hp);
        // stats[0] is the walking speed: each walk frame moves tics x stats[0] / 32 units, i.e.
        // stats[0] x 3.125 units a second (scientist 20, Cyclone 60-80, berserker 110). But the
        // game moves in one jump as each walk frame ends, and a jump that would carry the creature
        // past a cell centre stops at the centre (FUN_00047abe), so the rest of that frame's move
        // is lost. Walk the cells the way the game does to get the speed it really reaches.
        m->speed = originalWalkSpeed(*t);
        m->stationary = m->stationary || t->stats[0] == 0 || m->aiClass == 4 || m->aiClass == 3;
        {   // the original's legs keep their own time (sum of the walk tics), so a cycle covers
            // what the creature really travels in that time
            int ticSum = 0;
            for (uint8_t tc : t->walk.tics) ticSum += tc;
            m->stride = ticSum > 0 && t->stats[0] ? m->speed * ticSum / 100.0f : std::max(24.0f, v->height * 0.8f);
        }
        m->thinkTimer = frand(rng_, 0, 0.75f);
        m->reaction = frand(rng_, 0.2f, 0.6f);
        occupied_[m->cz * kGrid + m->cx]++;
        if (m->stationary) fixed_[m->cz * kGrid + m->cx]++;
        monsters_.push_back(std::move(m));
    }

    buildNav();   // after the creatures: one walk map per body height

    // Items: the original upright sprites, standing where the level puts them.
    for (const auto& s : E.items) {
        if (!(s.skill & (1 << difficulty))) continue;
        Item it;
        const Visual* v = itemVisual(s.sprite, it);
        if (!v || v->flat.empty()) continue;
        it.vis = v;
        it.x = (float)s.x;
        it.z = (float)s.z;
        // The game draws an item at the height the level gives it, so those placed on a prop (a
        // pedestal, a table, a shelf) rest on top of it; only one placed a little under the floor
        // is lifted onto it.
        it.y = std::max((float)s.y, groundBelow(it.x, it.z, (float)s.y + 8));
        it.light = cellLight(it.x, it.z);
        it.use = s.use;
        it.amount = s.amount;
        std::memcpy(it.params, s.params, sizeof(it.params));
        it.sprite = lower(s.sprite);
        it.script = s.script;
        nameItem(it, s.kind, s.invPic, s.setPic);
        items_.push_back(it);
    }

    // Objects (barrels, chairs, tables, machinery). Every record takes a slot (FUN_0001045e;
    // scripts name props by it), whatever its difficulty bits.
    int objSlot = -1;
    slotObject_.assign(E.objects.size(), -1);
    for (const auto& s : E.objects) {
        ++objSlot;
        if (!(s.skill & (1 << difficulty))) continue;
        std::string n = lower(s.sprite);
        AnimRange ar = spriteAnim(3, s.sprite);
        const Visual* v = spriteVisual(ar.number >= 0 ? "obj:anim" + std::to_string(ar.first) : "obj:" + n, {}, true, false, 0.09f);
        if (!v || v->flat.empty()) continue;
        bool barrel = n.find("barrel") != std::string::npos;
        Object o{v, barrel ? fx("barreld") : nullptr, (float)s.x, (float)s.y, (float)s.z, cellLight((float)s.x, (float)s.z), 0,
                 barrel, true, true, 0};
        o.slot = objSlot;
        o.script = s.script;
        slotObject_[objSlot] = (int)objects_.size();
        o.animNumber = ar.number;
        o.animFirst = ar.first;
        o.y = groundBelow(o.x, o.z, (float)s.y + 8);
        if (std::abs(o.y - s.y) > 40) { o.y = (float)s.y; o.grounded = false; }  // hanging/wall-mounted pieces keep their height
        int cx = (int)(o.x / kCellSize), cz = (int)(o.z / kCellSize);
        if (cx >= 0 && cz >= 0 && cx < kGrid && cz < kGrid) { occupied_[cz * kGrid + cx]++; fixed_[cz * kGrid + cx]++; }
        objects_.push_back(o);
    }
    traceMonster_ = SDL_getenv("CYCLONES_TRACE") ? std::atoi(SDL_getenv("CYCLONES_TRACE")) : -1;
    if (SDL_getenv("CYCLONES_LIST")) {   // testing: list the creatures placed on this level
        for (const auto& m : monsters_)
            SDL_Log("dbg monster %3d %-10s class %d species %2d cell %2d,%2d y %5.0f hp %3.0f speed %3.0f (formula %3.0f) hover %3.0f wpn %d/%d %s shot %.0f/%.0f attack %.2f/%.2f s%s walk %d",
                    m->index, m->type ? m->type->name.c_str() : "?", m->aiClass, m->species, (int)(m->x / kCellSize),
                    (int)(m->z / kCellSize), m->y, m->hp, m->speed, m->type ? m->type->stats[0] * 3.125f : 0.0f, m->hover,
                    m->weapon[0].kind, m->weapon[1].kind,
                    (m->weapon[0].visual + (m->weapon[1].visual.empty() ? "" : " " + m->weapon[1].visual)).c_str(), m->weapon[0].speed, m->weapon[1].speed, m->vis->attackA.total(), m->vis->attackB.total(),
                    m->stationary ? " stationary" : "", m->type ? (int)m->type->walk.frames.size() : -1);
        {
            std::set<const MonsterType*> seen;
            for (const auto& m : monsters_) {
                if (!m->type || !seen.insert(m->type).second) continue;
                SDL_Log("dbg size %-18s box %dx%dx%d (stats %d %d %d %d %d %d %d %d) sprite height %.0f radius %.0f", m->type->name.c_str(),
                        m->type->stats[5], m->type->stats[6], m->type->stats[7], m->type->stats[0], m->type->stats[1], m->type->stats[2],
                        m->type->stats[3], m->type->stats[4], m->type->stats[5], m->type->stats[6], m->type->stats[7], m->vis->height, m->vis->radius);
                auto list = [](const std::vector<uint8_t>& v) { std::string o; for (uint8_t b : v) o += std::to_string(b) + " "; return o; };
                SDL_Log("dbg anim %-18s sprite %s walk %s| A frames %s tics %s fire %s| B frames %s tics %s fire %s| walk tics %s| ready frames %s tics %s| pain frames %s tics %s| death frames %s tics %s",
                        m->type->name.c_str(), m->type->sprite.c_str(), list(m->type->walk.frames).c_str(), list(m->type->attackA.frames).c_str(), list(m->type->attackA.tics).c_str(),
                        list(m->type->fireA).c_str(), list(m->type->attackB.frames).c_str(), list(m->type->attackB.tics).c_str(),
                        list(m->type->fireB).c_str(), list(m->type->walk.tics).c_str(), list(m->type->ready.frames).c_str(),
                        list(m->type->ready.tics).c_str(), list(m->type->pain.frames).c_str(), list(m->type->pain.tics).c_str(),
                        list(m->type->death.frames).c_str(), list(m->type->death.tics).c_str());
            }
        }
        for (const auto& it : items_)
            SDL_Log("dbg item %-10s use %2d amount %3d p0 %d at %.2f,%.2f y %.0f", it.sprite.c_str(), it.use, it.amount,
                    it.params[0], it.x / kCellSize, it.z / kCellSize, it.y);
        for (size_t i = 0; i < doors_.size(); ++i)
            SDL_Log("dbg door %d tag %d cell %d,%d %s style %d flags %02x flags2 %04x speed %d offset %d key %d base %.0f height %.0f tex %s%s",
                    (int)i, doors_[i].spec.tag, doors_[i].cellX, doors_[i].cellZ, doors_[i].ew ? "east-west" : "north-south", doors_[i].style,
                    doors_[i].spec.flags, doors_[i].spec.flags2, doors_[i].spec.speed, doors_[i].spec.offset, doors_[i].spec.key,
                    doors_[i].base, doors_[i].height, doors_[i].spec.texture.c_str(),
                    !doors_[i].locked ? "" : doors_[i].scripted ? " (locked; a script opens it)" : " (locked)");
        for (const auto& o : objects_)
            SDL_Log("dbg object at %.2f,%.2f y %.0f height %.0f%s slot %d script %d", o.x / kCellSize,
                    o.z / kCellSize, o.y, o.vis ? o.vis->height : 0.0f, o.barrel ? " (explodes)" : "", o.slot, o.script);
    }
    if (SDL_getenv("CYCLONES_STEPCHECK")) stepCheck();
    SDL_Log("World %s: %d monsters, %d items, %d objects, %d doors; sprites built in %.0f ms (%llu pixels on GPU)",
            L.name.c_str(), (int)monsters_.size(), (int)items_.size(), (int)objects_.size(), (int)doors_.size(),
            (SDL_GetTicksNS() - t0) / 1e6, (unsigned long long)renderer_->splatCount());
}

// ---------------------------------------------------------------------------------------------
// Simulation

void World::addParticle(const Particle& p) {
    if (particles_.size() < 4000) particles_.push_back(p);
}

void World::spawnEffect(const std::string& key, float x, float y, float z, float scale0, float scale1, uint8_t mode,
                        bool fullbright, float delay, float roll) {
    const Visual* v = fx(key);
    if (!v || v->flat.empty()) return;
    Effect e;
    e.vis = v;
    e.x = x; e.y = y; e.z = z;
    e.scale0 = scale0; e.scale1 = scale1;
    e.mode = mode;
    e.fullbright = fullbright;
    e.delay = delay;
    e.roll = roll;
    if (effects_.size() < 1500) effects_.push_back(e);
}

bool World::rayHitsMonster(const Monster& m, const float o[3], const float d[3], float maxT, float& tHit, float pad) const {
    if (pad <= 0) return rayHitsMonsterThin(m, o, d, maxT, tHit);
    // A projectile has a body (the game's are 10 x 10): the centre ray, then four rays offset by
    // the body's half-size across the flight path. The nearest hit counts. Each is tested in
    // flight mode (flat pictures stand square to the flight, see rayHitsMonsterThin).
    bool any = false;
    float best = maxT, t;
    if (rayHitsMonsterThin(m, o, d, best, t, true)) { best = t; any = true; }
    float hl = std::sqrt(d[0] * d[0] + d[2] * d[2]);
    float rx = hl > 1e-4f ? -d[2] / hl : 1.0f, rz = hl > 1e-4f ? d[0] / hl : 0.0f;
    const float offs[4][2] = {{pad, 0}, {-pad, 0}, {0, pad}, {0, -pad}};
    for (const auto& f : offs) {
        float oo[3] = {o[0] + rx * f[0], o[1] + f[1], o[2] + rz * f[0]};
        if (rayHitsMonsterThin(m, oo, d, best, t, true) && t < best) { best = t; any = true; }
    }
    if (any) tHit = best;
    return any;
}

bool World::rayHitsMonsterThin(const Monster& m, const float o[3], const float d[3], float maxT, float& tHit, bool flight) const {
    const Visual& v = *m.vis;
    // Broad phase: the cylinder around every frame of the creature.
    float ox = o[0] - m.x, oz = o[2] - m.z;
    float a = d[0] * d[0] + d[2] * d[2];
    if (a < 1e-8f) return false;
    float b = 2 * (ox * d[0] + oz * d[2]), c = ox * ox + oz * oz - v.hitRadius * v.hitRadius;
    float disc = b * b - 4 * a * c;
    if (disc < 0) return false;
    float t1 = (-b + std::sqrt(disc)) / (2 * a);
    if (t1 < 0) return false;
    float t0 = std::max(0.0f, (-b - std::sqrt(disc)) / (2 * a));
    if (t0 > maxT) return false;
    // The sprite as seen from the ray's origin: an upright plane through the creature's axis.
    // A projectile's flight is tested a short step at a time, and a plane turned toward each
    // step's start turns with it as it flies past, so the step never crosses it unless it passes
    // within about half a step of the axis: rockets, grenades and plasma went through barrels,
    // open gunballs and creatures in attack or pain frames except down their middle (up to 0.5.2;
    // worse the higher the frame rate). In flight the plane stands square to the flight instead,
    // which the projectile crosses exactly once (fired by you, it faces you as the picture does).
    float fx = m.x - o[0], fz = m.z - o[2], fl = std::sqrt(fx * fx + fz * fz);
    if (flight && a > 1e-6f) { fx = d[0]; fz = d[2]; fl = std::sqrt(a); }
    if (fl < 1e-3f) { fx = d[0]; fz = d[2]; fl = std::sqrt(a); }
    fx /= fl;
    fz /= fl;
    float denom = d[0] * fx + d[2] * fz;            // along the view direction
    // Flat silhouettes are tested where the ray crosses that plane; depth sprites are marched
    // through the whole cylinder, so a short step (a projectile's) that only reaches the
    // creature's front still hits it.
    bool planeOk = std::abs(denom) >= 1e-4f;
    float t = planeOk ? (fx * (m.x - o[0]) + fz * (m.z - o[2])) / denom : -1.0f;
    if (t < 0 || t > maxT) planeOk = false;
    float px = o[0] + d[0] * t, py = o[1] + d[1] * t, pz = o[2] + d[2] * t;
    float rx = -fz, rz = fx;                         // the viewer's right
    float u = (px - m.x) * rx + (pz - m.z) * rz, h = py - m.y;
    const HitMask* mask = nullptr;
    auto flatMask = [&](const SpriteAnim& a, const std::vector<HitMask>& hs, float time) -> const HitMask* {
        if (hs.empty()) return nullptr;
        int f;
        float ft;
        animFrame(a.seconds, time, false, f, ft);
        return &hs[std::clamp(f, 0, (int)hs.size() - 1)];
    };
    if ((m.state == Monster::Pain || m.state == Monster::Stun) && !v.painHit.empty()) {
        mask = flatMask(v.pain, v.painHit, m.stateTime);
    } else if (m.state == Monster::Attack && !(m.attackSlot ? v.attackBHit : v.attackAHit).empty()) {
        mask = m.attackSlot ? flatMask(v.attackB, v.attackBHit, m.stateTime) : flatMask(v.attackA, v.attackAHit, m.stateTime);
    } else if (m.state == Monster::Walk && m.unfolded && !v.ready.empty() &&
               !(m.readySlot ? v.attackBHit : v.attackAHit).empty()) {   // waiting open: as drawn
        const std::vector<HitMask>& hs = m.readySlot ? v.attackBHit : v.attackAHit;
        mask = &hs[std::clamp(openFrame(m), 0, (int)hs.size() - 1)];
    } else if ((m.state == Monster::Ready || m.state == Monster::Unready || (m.state == Monster::Walk && m.unfolded)) &&
               !v.readyHit.empty()) {
        mask = flatMask(v.ready, v.readyHit, m.state == Monster::Ready ? m.stateTime : m.state == Monster::Walk ? 1e6f : 0);
    } else if (!v.rotHit.empty()) {
        // Depth sprites turn with the creature: march along the ray through the drawn view's own
        // frame and hit the first silhouette pixel the ray passes at that pixel's depth.
        int s = ((int)std::floor(m.step)) % (int)v.rotHit.size();
        float X[3] = {-std::cos(m.yaw), 0, -std::sin(m.yaw)}, Z[3] = {std::sin(m.yaw), 0, -std::cos(m.yaw)};
        float phi = std::atan2(-fx * X[0] - fz * X[2], -fx * Z[0] - fz * Z[2]);
        if (phi < 0) phi += 2 * kPi;
        int k = ((int)std::lround(phi / (kPi / 4))) % 8;
        const HitMask& hm = v.rotHit[s][k];
        float va = k * kPi / 4;
        float nx = std::sin(va) * X[0] + std::cos(va) * Z[0], nz = std::sin(va) * X[2] + std::cos(va) * Z[2];   // toward the viewer
        float wx = std::cos(va) * X[0] - std::sin(va) * Z[0], wz = std::cos(va) * X[2] - std::sin(va) * Z[2];   // the view's right
        float tEnd = std::min(maxT, t1);
        const float step = 0.6f;
        for (float tt = t0; tt <= tEnd; tt += step) {
            float qx = o[0] + d[0] * tt - m.x, qy = o[1] + d[1] * tt - m.y, qz = o[2] + d[2] * tt - m.z;
            int x = (int)std::floor(qx * wx + qz * wz + hm.ox), y = (int)std::floor(qy);
            if (!hm.test(x, y)) continue;
            float dd = qx * nx + qz * nz;
            if (!hm.depth.empty() && std::abs(dd - hm.depth[(size_t)y * hm.w + x]) > 1.6f) continue;
            tHit = tt;
            return true;
        }
        return false;
    } else if (!v.flatHit.empty()) {
        mask = &v.flatHit[((int)std::floor(m.step)) % (int)v.flatHit.size()];
    }
    if (!planeOk) return false;
    if (!mask) {  // no silhouette: fall back to the cylinder
        if (h < 0 || h > v.height) return false;
    } else if (!mask->test((int)std::floor(u + mask->ox), (int)std::floor(h))) {
        return false;
    }
    tHit = t;
    return true;
}

bool World::rayHitsAnyMonster(const float o[3], const float d[3]) const {
    float best = 6000, t, n[3];
    if (raycast(o, d, best, t, n)) best = t;
    for (const auto& m : monsters_) {
        if (m->state == Monster::Dying || m->state == Monster::Dead) continue;
        float tt;
        if (rayHitsMonster(*m, o, d, best, tt)) return true;
    }
    return false;
}

void World::update(float dt, const Camera& cam, bool firing) {
    if (!level_) return;
    time_ += dt;

    // Player status effects and messages.
    player_.hurt = std::max(0.0f, player_.hurt - dt * 2.0f);
    player_.bonus = std::max(0.0f, player_.bonus - dt * 2.5f);
    player_.hurtShow = std::max(0.0f, player_.hurtShow - dt * 1.2f);
    for (auto& m : messages_) m.age += dt;
    while (!messages_.empty() && messages_.front().age > 4.0f) messages_.erase(messages_.begin());

    exploreTimer_ -= dt;
    if (exploreTimer_ <= 0) {
        exploreTimer_ = 0.1f;
        explore(cam);
    }

    if (SDL_getenv("CYCLONES_LIST")) {   // testing: where the player is, twice a second
        static float t = 0;
        static const float every = std::atoi(SDL_getenv("CYCLONES_LIST")) >= 2 ? 0.05f : 0.5f;
        if ((t += dt) >= every) {
            t = 0;
            SDL_Log("dbg player t %.2f %.2f,%.2f y %.0f health %.0f armor %.0f jets %d fuel %d (routes %.2f ms, sight %.2f ms; slowest creature update %.2f ms)",
                    time_, player_.x / kCellSize, player_.z / kCellSize, player_.y, player_.health, player_.armor, (int)player_.jets,
                    player_.jetFuel, pathNsMax_ / 1e6,
                    losNsMax_ / 1e6, aiNs_ / 1e6);
            aiNs_ = pathNsMax_ = losNsMax_ = 0;
        }
    }
    // The player's ground velocity, smoothed over about a fifth of a second (for leading shots).
    if (dt > 0) {
        float k = 1.0f - std::exp(-dt / 0.2f);
        float dx = player_.x - lastPX_, dz = player_.z - lastPZ_;
        if (dx * dx + dz * dz > 64 * 64) dx = dz = 0;   // a teleport or a restart
        playerVelX_ += (dx / dt - playerVelX_) * k;
        playerVelZ_ += (dz / dt - playerVelZ_) * k;
        lastPX_ = player_.x;
        lastPZ_ = player_.z;
    }
    updateFiring(dt, cam, firing);

    // The game's 100 Hz tick counter (some of its random rules use the low bits).
    tickAcc_ += dt * kTicRate;
    while (tickAcc_ >= 1) {
        tickAcc_ -= 1;
        ++ticks_;
        surfAnimTick();   // animated walls, flats and props (surfanim.cpp)
        scriptTick();   // the level's scripts (script.cpp)
    }
    scanTick();   // the security scan rooms follow their scripts (scan.cpp)

    {
        uint64_t a0 = SDL_GetTicksNS();
        pathNs_ = losNs_ = 0;
        updateMonsters(dt, cam);
        aiNs_ = std::max(aiNs_, SDL_GetTicksNS() - a0);
        pathNsMax_ = std::max(pathNsMax_, pathNs_);
        losNsMax_ = std::max(losNsMax_, losNs_);
    }

    // Doors, as the game runs them (FUN_000132a5): "use" (or a creature walking into one) opens a
    // door; two seconds after it is fully open it closes again, and if anything stands in the
    // doorway while it closes, it opens again.
    if (bumpDoors && bumpedDoor_ >= 0 && bumpedDoor_ < (int)doors_.size() && doorOpens(doors_[bumpedDoor_], true))
        openDoor(doors_[bumpedDoor_]);
    bumpedDoor_ = -1;
    for (auto& d : doors_) {
        float span = d.vertical ? d.height : (float)kCellSize;
        float step = d.rate / std::max(1.0f, span) * dt;
        if (d.opening) {
            d.open = std::min(1.0f, d.open + step);
            if (d.open >= 1.0f && (d.openFor += dt) >= 2.0f && !d.staysOpen) d.opening = false;
        } else if (d.open > 0) {
            if (doorBlocked(d)) { d.opening = true; d.openFor = 0; }
            else d.open = std::max(0.0f, d.open - step);
        }
    }

    // Objects: animated machinery and wrecked barrels
    for (auto& o : objects_) {
        int n = (int)o.vis->flat.still.size();
        if (n > 1 && o.animNumber < 0) {
            o.step += dt / o.vis->flat.seconds[0];
            if (o.step >= n) o.step -= n;
        }
        if (!o.intact) o.wreckTime += dt;
    }

    updateShots(dt);
    if (autoPickup) updatePickups();

    // Delayed (chain) explosions
    for (size_t i = 0; i < pending_.size();) {
        pending_[i].delay -= dt;
        if (pending_[i].delay <= 0) {
            Pending p = pending_[i];
            pending_.erase(pending_.begin() + i);
            explode(p.x, p.y, p.z, p.power, p.sprite);
            if (p.blast) splashDamage(p.x, p.y, p.z, 40 + (int)(rng_() % 41), 2, nullptr, false);
        } else {
            ++i;
        }
    }

    // Sprite effects
    std::vector<Effect> follow;
    for (size_t i = 0; i < effects_.size();) {
        Effect& e = effects_[i];
        if (e.delay > 0) { e.delay -= dt; ++i; continue; }
        e.age += dt;
        float life = e.life > 0 ? e.life : e.vis->flat.total() + 0.05f;
        if (e.gravity != 0 || e.vx != 0 || e.vy != 0 || e.vz != 0) {
            float drag = std::exp(-e.drag * dt);
            e.vx *= drag; e.vz *= drag;
            e.vy = e.vy * drag - e.gravity * dt;
            float nx = e.x + e.vx * dt, ny = e.y + e.vy * dt, nz = e.z + e.vz * dt;
            if (e.bounce) {
                int cx = (int)std::floor(e.x / kCellSize), cz = (int)std::floor(e.z / kCellSize);
                int ncx = (int)std::floor(nx / kCellSize), ncz = (int)std::floor(nz / kCellSize);
                if (ncx != cx && edgeBlocks(cx, cz, ncx, cz, ny - 2, ny + 2)) { e.vx = -e.vx * 0.4f; nx = e.x; ncx = cx; }
                if (ncz != cz && edgeBlocks(ncx, cz, ncx, ncz, ny - 2, ny + 2)) { e.vz = -e.vz * 0.4f; nz = e.z; }
                float g = groundBelow(nx, nz, e.y + 4);
                if (ny < g + 1) { ny = g + 1; e.vy = std::abs(e.vy) * 0.3f; e.vx *= 0.5f; e.vz *= 0.5f; e.spin *= 0.5f; }
            }
            e.x = nx; e.y = ny; e.z = nz;
        }
        e.roll += e.spin * dt;
        if (e.age >= life) {
            if (!e.then.empty()) {
                Effect f;
                f.vis = fx(e.then);
                if (f.vis) {
                    f.x = e.x; f.y = e.y; f.z = e.z;
                    f.roll = e.roll;
                    f.mode = e.mode;
                    follow.push_back(f);
                }
            }
            effects_[i] = effects_.back();
            effects_.pop_back();
            continue;
        }
        ++i;
    }
    effects_.insert(effects_.end(), follow.begin(), follow.end());

    // Soft particles: sparks, embers, missile exhaust
    for (size_t i = 0; i < particles_.size();) {
        Particle& p = particles_[i];
        p.age += dt;
        if (p.age >= p.life) { particles_[i] = particles_.back(); particles_.pop_back(); continue; }
        float drag = std::exp(-p.drag * dt);
        p.vx *= drag; p.vy *= drag; p.vz *= drag;
        p.vy -= p.gravity * dt;
        p.x += p.vx * dt; p.y += p.vy * dt; p.z += p.vz * dt;
        if (p.kind != Particle::Trail) {
            float g = groundBelow(p.x, p.z, p.y + 6);
            if (p.y < g + 0.5f) { p.y = g + 0.5f; p.vy = std::abs(p.vy) * 0.35f; p.vx *= 0.6f; p.vz *= 0.6f; }
        }
        ++i;
    }

    for (size_t i = 0; i < lights_.size();) {
        lights_[i].age += dt;
        if (lights_[i].age >= lights_[i].life) { lights_[i] = lights_.back(); lights_.pop_back(); continue; }
        ++i;
    }
    shake = std::max(0.0f, shake - dt * 30);
}

// ---------------------------------------------------------------------------------------------
// Scene

// Picks the frame and morph position for an animation clock.
void animFrame(const std::vector<float>& seconds, float time, bool loop, int& frame, float& t) {
    int n = (int)seconds.size();
    frame = 0;
    t = 0;
    if (n == 0) return;
    float total = 0;
    for (float s : seconds) total += s;
    if (loop && total > 0) time = std::fmod(time, total);
    for (int i = 0; i < n; ++i) {
        if (time < seconds[i] || i == n - 1) {
            frame = i;
            t = seconds[i] > 0 ? std::clamp(time / seconds[i], 0.0f, 1.0f) : 0;
            if (!loop && i == n - 1) t = 0;
            return;
        }
        time -= seconds[i];
    }
}

namespace {

SplatDraw flatDraw(const SpriteAnim& a, int frame, float t, bool smooth, float x, float y, float z, float light) {
    SplatDraw d;
    bool glide = smooth && t > 0 && (a.loop || frame + 1 < (int)a.still.size());
    d.range = glide ? a.morph[frame] : a.still[frame];
    d.t = glide ? t : 0;
    d.pos[0] = x; d.pos[1] = y; d.pos[2] = z;
    d.light = light;
    d.facing = 1;
    return d;
}

}  // namespace

// Creature frames hold their original pose and only glide into the next one at the very end,
// so each 1994 frame is seen as drawn instead of a constant blend.
static float poseGlide(float t) { return smoothstep(0.5f, 1.0f, t); }

// The frame a deployer holds while it waits open between attacks (the enhanced AI; the game fires
// again at once): the unfolding's last picture if the attack shows it, else the first frame after
// its first shot that neither shoots nor shows a shot (an Alien Gunner aiming its arm gun, not
// throwing its arm up as it starts nor flashing), else the first (a gunball open). Up to 0.5.2
// always the first: the gunners stood with an arm raised overhead, the flamers spouting flame.
int World::openFrame(const Monster& m) const {
    if (!m.type || m.weapon[m.readySlot ? 1 : 0].melee()) return 0;
    const SpriteAnim& a = m.readySlot ? m.vis->attackB : m.vis->attackA;
    const std::vector<uint8_t>& frames = m.readySlot ? m.type->attackB.frames : m.type->attackA.frames;
    const std::vector<uint8_t>& fire = m.readySlot ? m.vis->fireB : m.vis->fireA;
    const int n = (int)std::min(a.still.size(), frames.size());
    // An attack that comes back to the unfolding's last picture holds that (the Elite Flamer's
    // flamer levelled, without the flame).
    const std::vector<uint8_t>& ready = m.type->ready.frames;
    if (!ready.empty())
        for (int k = 0; k < n; ++k)
            if (frames[k] == ready.back()) return k;
    auto fires = [&](int k) { return k < (int)fire.size() && fire[k]; };
    std::set<int> shots;
    int first = -1;
    for (int k = 0; k < n; ++k)
        if (fires(k)) { shots.insert(frames[k]); if (first < 0) first = k; }
    if (first < 0) return 0;
    for (int k = first + 1; k < n; ++k)
        if (!fires(k) && !shots.count(frames[k]) && !(k < (int)a.light.size() && a.light[k] > 0)) return k;
    return 0;
}

void World::addMonsterDraw(Scene& sc, const Monster& m, const Camera& cam) const {
    // Creature types with bit 1 of stats[3] set (type +0x12 in memory) are drawn through the game's
    // TINT_MAP (FUN_00047010 sets the object's 0x02, FUN_0001c694 draws it in mode 2): the green
    // CyClones (cyc300) of SUBWAY and MAINTEN2, about 38 % of the picture over 62 % of what is
    // behind. Mode 3 blends the creature's front surface once, as glass is (up to 0.5.2 solid).
    const bool tint = m.type && (m.type->stats[3] & 2);
    auto emit = [&](SplatDraw d) {
        if (tint && d.mode == 0) { d.mode = 3; sc.effects.push_back(d); }
        else sc.splats.push_back(d);
    };
    float light = cellLight(m.x, m.z);
    bool open = m.state == Monster::Walk && m.unfolded && !m.vis->ready.empty();   // a deployer between shots
    // Waiting open between shots (the enhanced AI: the game fires again at once), it holds a frame
    // of its attack (openFrame), not the last of its unfolding: BIOSTOR2's, CORE's and CLONE's
    // gunballs have their gun ports shut in that one, so they seemed to fire closed (up to 0.5.2).
    const SpriteAnim& openAttack = m.readySlot ? m.vis->attackB : m.vis->attackA;
    bool openOnAttack = open && !openAttack.empty();
    if (m.state != Monster::Walk || open) {
        const SpriteAnim& a = m.state == Monster::Pain || m.state == Monster::Stun ? m.vis->pain
                            : m.state == Monster::Attack ? (m.attackSlot == 0 ? m.vis->attackA : m.vis->attackB)
                            : openOnAttack ? openAttack
                            : m.state == Monster::Ready || m.state == Monster::Unready || open ? m.vis->ready
                            : m.vis->death;
        if (a.empty()) return;
        int f;
        float t;
        bool flashing = (m.state == Monster::Attack || open) && m.flashUntil > time_ && m.flashFrame >= 0;
        const SpriteAnim& fa = m.flashSlot ? m.vis->attackB : m.vis->attackA;
        if (flashing && m.flashFrame < (int)fa.still.size()) {
            SplatDraw d = flatDraw(fa, m.flashFrame, 0, smooth, m.x, m.y, m.z, light);
            d.flash = m.flash;
            emit(d);
            return;
        }
        if (openOnAttack) { f = openFrame(m); t = 0; }
        else if (m.state == Monster::Dead || open) { f = (int)a.still.size() - 1; t = 0; }
        else if (m.state == Monster::Unready) {   // folds back up, reversed at double speed
            animFrame(a.seconds, m.stateTime * 2, false, f, t);
            f = (int)a.still.size() - 1 - f;
            t = 0;
        } else animFrame(a.seconds, m.stateTime, false, f, t);
        // A muzzle flash pops in rather than gliding in (held separately, above).
        if (m.state == Monster::Attack && f + 1 < (int)a.light.size() && a.light[f + 1] > 0) t = 0;
        SplatDraw d = flatDraw(a, f, poseGlide(t), smooth, m.x, m.y, m.z, light);
        d.flash = m.flash;
        if (m.state == Monster::Dead && m.gone) {
            // The game removes these at the end of their death (explosions, collapsing machines):
            // the last frame fades out quickly instead of staying.
            const float fade = 0.16f;
            if (m.stateTime >= fade) return;
            d.mode = 1;
            d.alpha = 1.0f - m.stateTime / fade;
            sc.effects.push_back(d);
            return;
        }
        emit(d);
        return;
    }
    if (m.vis->rot.empty()) {
        const SpriteAnim& a = m.vis->flat;
        int n = (int)a.still.size();
        int s = ((int)std::floor(m.step)) % std::max(1, n);
        SplatDraw d = flatDraw(a, s, poseGlide(m.step - std::floor(m.step)), smooth, m.x, m.y, m.z, light);
        d.flash = m.flash;
        emit(d);
        return;
    }
    const RotAnim& r = m.vis->rot;
    int n = (int)r.still.size();
    int s = ((int)std::floor(m.step)) % n;
    float t = poseGlide(m.step - std::floor(m.step));
    // Which original view faces the camera: phi is the camera's angle around the monster,
    // counter-clockwise from its front, exactly as the sprites were rendered.
    float yaw = m.yaw;
    float X[3] = {-std::cos(yaw), 0, -std::sin(yaw)}, Z[3] = {std::sin(yaw), 0, -std::cos(yaw)};
    float dx = cam.x - m.x, dz = cam.z - m.z;
    float phi = std::atan2(dx * X[0] + dz * X[2], dx * Z[0] + dz * Z[2]);
    if (phi < 0) phi += 2 * kPi;
    float f = phi / (kPi / 4);
    SplatDraw d;
    d.pos[0] = m.x; d.pos[1] = m.y; d.pos[2] = m.z;
    d.light = light;
    d.flash = m.flash;
    d.facing = 0;
    if (!smooth) {
        // Original behaviour: the nearest of the 8 views, square to the camera, frame by frame.
        int k = ((int)std::lround(f)) % 8;
        yawMatrix(yaw - (phi - k * kPi / 4), d.rot);
        d.range = r.still[s][k];
        emit(d);
        return;
    }
    // Smooth mode: the original view nearest the camera, turned with the creature in 3D. It
    // switches straight to the next view (with a little hysteresis), never through a blend.
    yawMatrix(yaw, d.rot);
    int k = m.view < 0 ? ((int)std::lround(f)) % 8 : m.view;
    // Seen off its original angle, neighbouring pixel columns at different depths drift apart;
    // widening the pixel squares a little keeps the sprite solid.
    float off = std::abs(angleDiff(f * kPi / 4, k * kPi / 4));
    d.growX = 2.6f * std::sin(std::min(off, kPi / 3));
    bool glide = n > 1 && t > 0.001f && r.morph[s][k].count > 0;
    d.t = glide ? t : 0;
    d.range = glide ? r.morph[s][k] : r.still[s][k];
    emit(d);
}

void World::buildScene(Scene& sc, const Camera& cam) const {
    sc = Scene();
    if (!level_) return;
    float cyw = std::cos(cam.yaw), syw = std::sin(cam.yaw), cp = std::cos(cam.pitch), sp = std::sin(cam.pitch);
    float fwd[3] = {syw * cp, sp, -cyw * cp};
    auto visible = [&](float x, float y, float z, float r) {
        float dx = x - cam.x, dy = y - cam.y, dz = z - cam.z;
        float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > 5200.0f * 5200.0f) return false;
        return dx * fwd[0] + dy * fwd[1] + dz * fwd[2] > -r - 8;
    };
    std::vector<LightDraw> deathLights;
    for (const auto& mp : monsters_) {
        const Monster& m = *mp;
        if (!visible(m.x, m.y + 30, m.z, m.vis->radius + 60)) continue;
        if (m.state == Monster::Dead && m.gone && m.stateTime > 0.2f) continue;
        addMonsterDraw(sc, m, cam);
        if (m.state != Monster::Dying && m.state != Monster::Dead) {
            float sy = m.hover > 0 ? groundBelow(m.x, m.z, m.y + 1) : m.y;   // a flier's on the surface under it
            sc.shadows.insert(sc.shadows.end(), {m.x, sy, m.z, std::max(10.0f, m.vis->radius * 0.9f)});
        }
        if (m.state == Monster::Dying && !m.vis->death.empty()) {
            // Death frames with light-coded times light up the room, as the game does.
            int f;
            float t;
            animFrame(m.vis->death.seconds, m.stateTime, false, f, t);
            float v = f < (int)m.vis->death.light.size() ? m.vis->death.light[f] : 0.0f;
            if (v > 0) deathLights.push_back({m.x, m.y + m.vis->height * 0.5f, m.z, 150 + v * 1.5f, 0.42f, 0.3f, 0.14f, 40 + v * 3.5f});
        }
        if (m.flashUntil > time_ && m.flashFrame >= 0 && m.state != Monster::Dying && m.state != Monster::Dead) {
            // So do attack frames (the animation player's states 4 and 5 read tics of 129 and up
            // the same way: 5 ticks and a flash of v - 128): a gunball's muzzle flash (balla1, 32)
            // lights it and the corridor round it, for as long as the flash is drawn. Up to 0.5.2
            // the engine lit only death frames, and the flash frame showed dim in the dark.
            const SpriteAnim& a = m.flashSlot ? m.vis->attackB : m.vis->attackA;
            float v = m.flashFrame < (int)a.light.size() ? a.light[m.flashFrame] : 0.0f;
            if (v > 0) deathLights.push_back({m.x, m.y + m.vis->height * 0.5f, m.z, 150 + v * 1.5f, 0.45f, 0.3f, 0.12f, 40 + v * 3.5f});
        }
    }
    for (const auto& it : items_) {
        if (it.taken || !visible(it.x, it.y, it.z, 40)) continue;
        int f = it.animNumber >= 0 ? std::clamp(animFrameOf(2, it.animNumber, it.animFirst), 0, (int)it.vis->flat.still.size() - 1) : 0;
        sc.splats.push_back(flatDraw(it.vis->flat, f, 0, false, it.x, it.y, it.z, it.light));
        sc.shadows.insert(sc.shadows.end(), {it.x, it.y, it.z, std::max(5.0f, it.vis->radius * 0.7f)});
    }
    addKeepsakeDraw(sc, cam);
    for (const auto& o : objects_) {
        if (o.removed || !visible(o.x, o.y, o.z, o.vis->radius + 40)) continue;
        // A barrel caught in a blast stands until its own explosion (the "barreld" frames,
        // played as an effect), then it's gone, as in the game.
        if (!o.intact && o.wreckTime >= 0) continue;
        const SpriteAnim& a = o.vis->flat;
        int n = (int)a.still.size();
        if (o.animNumber >= 0) {   // the level's own animation (surfanim): a picture at a time
            int f = std::clamp(animFrameOf(3, o.animNumber, o.animFirst), 0, std::max(0, n - 1));
            sc.splats.push_back(flatDraw(a, f, 0, false, o.x, o.y, o.z, o.light));
        } else {
            int s = ((int)std::floor(o.step)) % std::max(1, n);
            sc.splats.push_back(flatDraw(a, s, o.step - std::floor(o.step), smooth, o.x, o.y, o.z, o.light));
        }
        if (o.grounded) sc.shadows.insert(sc.shadows.end(), {o.x, o.y, o.z, std::max(6.0f, o.vis->radius * 0.8f)});
    }

    // Projectiles: the original sprites, glowing (full bright) as they did in 1994.
    std::vector<LightDraw> shotLights;
    for (const auto& s : shots_) {
        const Visual* v = fx(s.visual);
        if (!v) continue;
        float vel[3] = {s.vx, s.vy, s.vz};
        float h = v->height * 0.5f;
        if (!v->rot.empty()) {
            SplatDraw d;
            dirMatrix(vel, d.rot);
            const RotAnim& r = v->rot;
            int n = (int)r.still.size();
            float clock = s.age / std::max(0.01f, r.seconds[0]);
            int st = ((int)clock) % n;
            float dx = cam.x - s.x, dy = cam.y - s.y, dz = cam.z - s.z;
            float mx = dx * d.rot[0] + dy * d.rot[1] + dz * d.rot[2], mz = dx * d.rot[6] + dy * d.rot[7] + dz * d.rot[8];
            float phi = std::atan2(mx, mz);
            if (phi < 0) phi += 2 * kPi;
            int k = ((int)std::lround(phi / (kPi / 4))) % 8;
            d.range = smooth ? r.morph[st][k] : r.still[st][k];
            d.t = smooth ? clock - std::floor(clock) : 0;
            d.pos[0] = s.x - d.rot[3] * h; d.pos[1] = s.y - d.rot[4] * h; d.pos[2] = s.z - d.rot[5] * h;
            d.fullbright = true;
            sc.splats.push_back(d);
        } else if (!v->flat.empty()) {
            int f;
            float t;
            animFrame(v->flat.seconds, s.age, true, f, t);
            SplatDraw d = flatDraw(v->flat, f, t, smooth, s.x, s.y - h, s.z, 0);
            d.facing = 2;
            d.rollY = h;
            d.fullbright = true;
            sc.splats.push_back(d);
        }
        if (s.lr + s.lg + s.lb > 0) shotLights.push_back({s.x, s.y, s.z, 120, s.lr, s.lg, s.lb, 140});
    }

    // Lights (brightest first), capped at 8 in the shader.
    std::vector<LightDraw> ls = deathLights;
    ls.insert(ls.end(), shotLights.begin(), shotLights.end());
    addScanDraw(sc, ls);   // the security scanners' screens and light (scan.cpp)
    sc.time = time_;       // the computers' screens run by it
    for (const auto& l : lights_) {
        float k = 1.0f - l.age / l.life;
        k = k * k;
        ls.push_back({l.x, l.y, l.z, l.radius * (0.6f + 0.4f * k), l.r * k, l.g * k, l.b * k, l.intensity * k});
    }
    std::sort(ls.begin(), ls.end(), [](const LightDraw& a, const LightDraw& b) { return a.intensity > b.intensity; });
    for (const auto& l : ls)
        if (sc.lights.size() < 8) sc.lights.push_back(l);

    // Doors
    for (const auto& d : doors_) {
        float e = d.open * d.open * (3 - 2 * d.open);
        float ox = 0, oy = 0, oz = 0;
        if (d.vertical) oy = d.dir * e * (d.height - 1);
        else {
            float s = d.dir * e * (kCellSize - 1);
            if (d.ew) ox = s; else oz = s;
        }
        sc.doorOffsets.insert(sc.doorOffsets.end(), {ox, oy, oz});
        // As in the game, a door is only seen inside its doorway: the panel's cell along its run
        // and between its base and top, so a raised panel goes into the lintel, not past it.
        float cx0 = d.cellX * (float)kCellSize, cz0 = d.cellZ * (float)kCellSize;
        sc.doorClips.insert(sc.doorClips.end(), {cx0 - 4, d.base - 0.5f, cz0 - 4, cx0 + kCellSize + 4, d.base + d.height + 0.5f, cz0 + kCellSize + 4});
        float* c = &sc.doorClips[sc.doorClips.size() - 6];
        if (d.ew) { c[0] = cx0; c[3] = cx0 + kCellSize; }   // along the run: exactly the cell
        else { c[2] = cz0; c[5] = cz0 + kCellSize; }
    }

    // Sprite effects, far to near so the blending stacks correctly.
    std::vector<std::pair<float, SplatDraw>> fxs;
    for (const auto& e : effects_) {
        if (e.delay > 0 || !visible(e.x, e.y, e.z, 80)) continue;
        const SpriteAnim& a = e.vis->flat;
        float life = e.life > 0 ? e.life : a.total() + 0.05f;
        float clock = e.life > 0 ? e.age / e.life * a.total() : e.age;
        int f;
        float t;
        animFrame(a.seconds, clock, false, f, t);
        float u = std::clamp(e.age / life, 0.0f, 1.0f);
        float scale = e.scale0 + (e.scale1 - e.scale0) * std::sqrt(u);
        SplatDraw d = flatDraw(a, f, t, smooth, e.x, e.y, e.z, cellLight(e.x, e.z));
        d.facing = 2;
        d.scale = scale;
        float pivot = e.grounded ? 0.0f : a.anchor[f] > 0 ? (float)a.anchor[f] : a.height[f] * 0.5f;
        d.pos[1] = e.y - pivot * scale;
        d.rollY = pivot;
        d.roll = e.roll;
        d.mode = e.mode;
        d.fullbright = e.fullbright;
        d.alpha = e.alpha * (1.0f - smoothstep(0.65f, 1.0f, u));
        float depth = (e.x - cam.x) * fwd[0] + (e.y - cam.y) * fwd[1] + (e.z - cam.z) * fwd[2];
        fxs.push_back({depth, d});
    }
    std::sort(fxs.begin(), fxs.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (auto& f : fxs) sc.effects.push_back(f.second);

    // Soft particles
    auto rampAt = [](const std::vector<float>& r, float u, float* out) {
        int n = (int)r.size() / 3;
        float f = std::clamp(u, 0.0f, 1.0f) * (n - 1);
        int i = std::min(n - 2, (int)f);
        float k = f - i;
        for (int c = 0; c < 3; ++c) out[c] = r[i * 3 + c] * (1 - k) + r[(i + 1) * 3 + c] * k;
    };
    for (const auto& p : particles_) {
        if (!visible(p.x, p.y, p.z, 10)) continue;
        float u = p.age / p.life;
        ParticleDraw d{p.x, p.y, p.z, p.size0 + (p.size1 - p.size0) * std::sqrt(u), 1, 1, 1, 1, p.vx, p.vy, p.vz, 0};
        if (p.kind == Particle::Trail) {
            d.r = d.g = d.b = 0.45f;
            d.a = p.alpha * (1 - u);
            d.vx = d.vy = d.vz = 0;
            sc.smoke.push_back(d);
            continue;
        }
        float c[3];
        rampAt(sparkRamp_, p.kind == Particle::Spark ? u : 0.3f + u * 0.7f, c);
        d.r = c[0]; d.g = c[1]; d.b = c[2];
        d.a = (1 - u * u) * 0.9f;
        d.stretch = p.kind == Particle::Spark ? 0.02f : 0.008f;
        sc.glow.push_back(d);
    }
}

bool World::monsterInfo(int index, float out[5]) const {
    if (index < 0 || index >= (int)monsters_.size()) return false;
    const Monster& m = *monsters_[index];
    out[0] = m.x; out[1] = m.y; out[2] = m.z; out[3] = m.vis->height; out[4] = m.yaw;
    return true;
}

bool World::entityInfo(char kind, int index, float out[5]) const {
    if (kind == 'm') return monsterInfo(index, out);
    if (kind == 'o' && index >= 0 && index < (int)objects_.size()) {
        const Object& o = objects_[index];
        out[0] = o.x; out[1] = o.y; out[2] = o.z; out[3] = o.vis->height; out[4] = 0;
        return true;
    }
    if (kind == 'i' && index >= 0 && index < (int)items_.size()) {
        const Item& it = items_[index];
        out[0] = it.x; out[1] = it.y; out[2] = it.z; out[3] = it.vis->height; out[4] = 0;
        return true;
    }
    if (kind == 'd' && index >= 0 && index < (int)doors_.size()) {
        const Door& d = doors_[index];
        out[0] = d.cx; out[1] = d.base; out[2] = d.cz; out[3] = d.height; out[4] = d.ew ? 0.0f : 1.5707963f;
        return true;
    }
    return false;
}

std::string World::status() const {
    int alive = 0;
    for (const auto& m : monsters_) alive += m->state != Monster::Dying && m->state != Monster::Dead && !m->barrel;
    char buf[160];
    SDL_snprintf(buf, sizeof(buf), "%s — %d monsters, %d down", kWeapons[player_.weapon].name, alive, kills_);
    return buf;
}

// ---------------------------------------------------------------------------------------------
// Automap

float World::topSurface(int cx, int cz, int* flat) const {
    const Level& L = *level_;
    if (cx < 0 || cz < 0 || cx >= L.width || cz >= L.height) return -1e9f;
    int i = cz * kGrid + cx;
    float best = -1e9f;
    int f = 0;
    auto take = [&](const SurfaceLayer& s, float h) {
        int t = s.tex[i] & 0x3fff;
        if (t && h > best) { best = h; f = t; }
    };
    if (L.floor.tex[i] & 0x3fff) {
        // Sloped cells: the middle of the slope.
        int shape = L.shape[i] & 7;
        float h = (float)(int16_t)L.floor.height[i];
        if (shape) h = surfaceY(L.floor, 0, (cx + 0.5f) * kCellSize, (cz + 0.5f) * kCellSize);
        take(L.floor, h);
    }
    take(L.extra1, (float)(int16_t)L.extra1.height[i]);
    take(L.extra2, (float)(int16_t)L.extra2.height[i]);
    if (flat) *flat = f;
    return best;
}

void World::mapLevel(MapLevel& out) const {
    const Level& L = *level_;
    out = MapLevel();
    out.width = L.width;
    out.height = L.height;
    out.top.assign(kGrid * kGrid, -1e9f);
    out.flat.assign(kGrid * kGrid, 0);
    out.light.assign(kGrid * kGrid, 0);
    for (int z = 0; z < kGrid; ++z)
        for (int x = 0; x < kGrid; ++x) {
            int i = z * kGrid + x, f = 0;
            out.top[i] = topSurface(x, z, &f);
            out.flat[i] = f;
            out.light[i] = (float)(int8_t)L.floor.extra[i];
        }
    // Lone floor cells with no floor beside them (most levels have one in the corner) aren't
    // places you can reach: leave them off the map.
    std::vector<int> lone;
    for (int z = 0; z < kGrid; ++z)
        for (int x = 0; x < kGrid; ++x) {
            if (out.top[z * kGrid + x] < -1e8f) continue;
            bool neighbour = false;
            for (int dz = -1; dz <= 1 && !neighbour; ++dz)
                for (int dx = -1; dx <= 1 && !neighbour; ++dx) {
                    int nx = x + dx, nz = z + dz;
                    if ((dx == 0) != (dz == 0) && nx >= 0 && nz >= 0 && nx < kGrid && nz < kGrid && out.top[nz * kGrid + nx] > -1e8f) neighbour = true;
                }
            if (!neighbour) lone.push_back(z * kGrid + x);
        }
    for (int i : lone) out.top[i] = -1e9f;
    // Edges: walls that block at walking height (and the level's outer boundary), else ledges.
    // Collinear neighbours are merged into longer runs.
    auto edge = [&](int ax, int az, int bx, int bz, int& kind, float& drop) {
        float ta = out.top[std::clamp(az, 0, kGrid - 1) * kGrid + std::clamp(ax, 0, kGrid - 1)];
        float tb = out.top[std::clamp(bz, 0, kGrid - 1) * kGrid + std::clamp(bx, 0, kGrid - 1)];
        bool fa = ax >= 0 && az >= 0 && ax < kGrid && az < kGrid && ta > -1e8f;
        bool fb = bx >= 0 && bz >= 0 && bx < kGrid && bz < kGrid && tb > -1e8f;
        kind = 0;
        drop = 0;
        if (!fa && !fb) return;
        if (fa != fb) { kind = 1; return; }
        float y0 = std::max(ta, tb) + 16;
        if (edgeBlocks(ax, az, bx, bz, y0, y0 + 40)) { kind = 1; return; }
        drop = std::abs(ta - tb);
        if (drop >= 24) kind = 2;
    };
    for (int pass = 0; pass < 2; ++pass) {          // 0: north edges (horizontal), 1: west edges (vertical)
        for (int a = 0; a <= kGrid; ++a) {           // the line: z for north edges, x for west edges
            int runKind = 0, runStart = 0;
            float runDrop = 0;
            for (int b = 0; b <= kGrid; ++b) {       // position along the line
                int kind = 0;
                float drop = 0;
                if (b < kGrid && a < kGrid + 1) {
                    if (pass == 0) edge(b, a - 1, b, a, kind, drop);
                    else edge(a - 1, b, a, b, kind, drop);
                }
                if (kind != runKind || (kind == 2 && std::abs(drop - runDrop) > 8)) {
                    if (runKind) {
                        float p0 = (float)runStart * kCellSize, p1 = (float)b * kCellSize, q = (float)a * kCellSize;
                        std::vector<float>& dst = runKind == 1 ? out.walls : out.ledges;
                        if (pass == 0) dst.insert(dst.end(), {p0, q, p1, q});
                        else dst.insert(dst.end(), {q, p0, q, p1});
                        if (runKind == 2) dst.push_back(runDrop);
                    }
                    runKind = kind;
                    runStart = b;
                    runDrop = drop;
                }
            }
        }
    }
}

void World::explore(const Camera& cam) {
    if (!level_ || explored_.empty()) return;
    const Level& L = *level_;
    auto mark = [&](int x, int z) {
        if (x >= 0 && z >= 0 && x < kGrid && z < kGrid) explored_[z * kGrid + x] = 255;
    };
    auto closedDoor = [&](int x, int z) {
        for (const auto& d : doors_)
            if (d.cellX == x && d.cellZ == z && d.open < 0.5f) return true;
        return false;
    };
    int pcx = (int)std::floor(cam.x / kCellSize), pcz = (int)std::floor(cam.z / kCellSize);
    if (pcx < 0 || pcz < 0 || pcx >= L.width || pcz >= L.height) return;
    mark(pcx, pcz);
    const int rays = 360;
    const float reach = 14.0f;  // cells
    for (int r = 0; r < rays; ++r) {
        float a = 6.2831853f * r / rays, dx = std::cos(a), dz = std::sin(a);
        float x = cam.x / kCellSize, z = cam.z / kCellSize;
        int cx = pcx, cz = pcz;
        int sx = dx > 0 ? 1 : -1, sz = dz > 0 ? 1 : -1;
        float tdx = std::abs(dx) > 1e-6f ? std::abs(1.0f / dx) : 1e9f, tdz = std::abs(dz) > 1e-6f ? std::abs(1.0f / dz) : 1e9f;
        float tx = std::abs(dx) > 1e-6f ? ((sx > 0 ? (cx + 1 - x) : (x - cx)) * tdx) : 1e9f;
        float tz = std::abs(dz) > 1e-6f ? ((sz > 0 ? (cz + 1 - z) : (z - cz)) * tdz) : 1e9f;
        while (std::min(tx, tz) < reach) {
            int nx = cx, nz = cz;
            if (tx < tz) { nx += sx; tx += tdx; }
            else { nz += sz; tz += tdz; }
            if (nx < 0 || nz < 0 || nx >= L.width || nz >= L.height) break;
            if (edgeBlocks(cx, cz, nx, nz, cam.y - 24, cam.y + 8)) break;
            mark(nx, nz);
            float top = topSurface(nx, nz);
            if (top < -1e8f || top > cam.y + 4 || closedDoor(nx, nz)) break;
            cx = nx;
            cz = nz;
        }
    }
}

void World::mapFrame(MapFrame& out, const Camera& cam, bool all, int markers) const {
    out.px = cam.x;
    out.pz = cam.z;
    out.yaw = cam.yaw;
    out.explored = explored_.empty() ? nullptr : explored_.data();
    out.doors.clear();
    out.enemies.clear();
    out.items.clear();
    auto seen = [&](float wx, float wz) {
        if (all) return true;
        int x = (int)std::floor(wx / kCellSize), z = (int)std::floor(wz / kCellSize);
        return x >= 0 && z >= 0 && x < kGrid && z < kGrid && explored_[z * kGrid + x] > 0;
    };
    for (const auto& d : doors_) {
        float x0, z0, x1, z1;
        if (d.ew) { x0 = (float)d.spec.x; x1 = x0 + kCellSize; z0 = z1 = (float)(d.spec.z + d.spec.offset); }
        else { z0 = (float)d.spec.z; z1 = z0 + kCellSize; x0 = x1 = (float)(d.spec.x + d.spec.offset); }
        out.doors.insert(out.doors.end(), {x0, z0, x1, z1, d.open});
    }
    for (const auto& m : monsters_) {
        if (markers < 2) break;
        if (m->barrel || m->aiClass == 3 || m->state == Monster::Dying || m->state == Monster::Dead) continue;
        if (!seen(m->x, m->z)) continue;
        out.enemies.insert(out.enemies.end(), {m->x, m->z, std::max(10.0f, m->vis->radius)});
    }
    for (const auto& it : items_)
        if (markers >= 1 && !it.taken && seen(it.x, it.z)) out.items.insert(out.items.end(), {it.x, it.z});
}

// Walk links between neighbouring cells for a typical creature (48 tall), with doors open; the
// enhanced AI plans its routes on these, and closed doors are checked when it walks.
void World::buildNav() {
    const Level& L = *level_;
    uint64_t t0 = SDL_GetTicksNS();
    nav_.clear();
    doorCell_.assign(kGrid * kGrid, 0);
    for (size_t i = 0; i < doors_.size(); ++i) {
        const Door& d = doors_[i];
        if (d.cellX >= 0 && d.cellZ >= 0 && d.cellX < kGrid && d.cellZ < kGrid) doorCell_[d.cellZ * kGrid + d.cellX] = (int)i + 1;
    }
    // The floors of each cell: the floor, the two platform layers and the tops of solid blocks,
    // where there is room to stand.
    navY_.assign(kGrid * kGrid * kNavLevels, -1e9f);
    for (int z = 0; z < L.height; ++z)
        for (int x = 0; x < L.width; ++x) {
            int i = z * kGrid + x;
            float wx = (x + 0.5f) * kCellSize, wz = (z + 0.5f) * kCellSize;
            if (!hasGround(wx, wz)) continue;
            std::vector<float> cand;
            if (L.floor.tex[i] & 0x3fff) cand.push_back(surfaceY(L.floor, 0, wx, wz));
            if (L.extra1.tex[i] & 0x3fff) cand.push_back((float)(int16_t)L.extra1.height[i]);
            if (L.extra2.tex[i] & 0x3fff) cand.push_back((float)(int16_t)L.extra2.height[i]);
            for (int ci : cubesInCell_[i]) {
                const auto& c = L.cubes[ci];
                auto u16 = [&](int k) { return (int)(c[k * 2] | c[k * 2 + 1] << 8); };
                float x0 = (float)u16(1), z0 = (float)u16(2), hgt = cubeHeight_[ci];
                if (wx >= x0 && wx <= x0 + c[8] && wz >= z0 && wz <= z0 + c[10] && hgt > 0) cand.push_back((float)(int16_t)u16(3) + hgt);
            }
            std::sort(cand.begin(), cand.end(), std::greater<float>());
            int n = 0;
            for (float g : cand) {
                if (n == kNavLevels) break;
                if (n && navY_[i * kNavLevels + n - 1] - g < 4) continue;   // the same floor
                float ceil;
                float gb = groundBelow(wx, wz, g, &ceil);
                if (std::abs(gb - g) > 1 || ceil - g < 24) continue;     // covered, or no room to stand
                navY_[i * kNavLevels + n++] = g;
            }
        }
    static const int kDx[8] = {1, -1, 0, 0, 1, 1, -1, -1}, kDz[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    std::vector<int> heights;
    for (const auto& m : monsters_)
        if (!m->stationary) heights.push_back(navHeight(m->vis->height));
    std::sort(heights.begin(), heights.end());
    heights.erase(std::unique(heights.begin(), heights.end()), heights.end());
    for (int h : heights) {
        std::vector<uint32_t>& nav = nav_[h];
        nav.assign(kGrid * kGrid * kNavLevels, 0);
        for (int z = 0; z < L.height; ++z)
            for (int x = 0; x < L.width; ++x)
                for (int l = 0; l < kNavLevels; ++l) {
                    int node = (z * kGrid + x) * kNavLevels + l;
                    if (navY_[node] < -1e8f) break;
                    for (int k = 0; k < 8; ++k) {
                        float ny;
                        int nx = x + kDx[k], nz = z + kDz[k];
                        if (!stepTarget(x, z, nx, nz, navY_[node], (float)h, ny)) continue;
                        int nl = navLevel(nz * kGrid + nx, ny);
                        if (nl < 0) continue;
                        nav[node] |= (1u << k) | ((uint32_t)nl << (8 + 2 * k));
                    }
                }
    }
    SDL_Log("Walk maps for %d creature sizes in %.0f ms", (int)heights.size(), (SDL_GetTicksNS() - t0) / 1e6);
    if (const char* dump = SDL_getenv("CYCLONES_NAVDUMP")) {   // testing: x0,z0,x1,z1,height
        int x0, z0, x1, z1, hh;
        if (std::sscanf(dump, "%d,%d,%d,%d,%d", &x0, &z0, &x1, &z1, &hh) == 5 && nav_.count(hh))
            for (int z = z0; z <= z1; ++z) {
                std::string line;
                for (int x = x0; x <= x1; ++x) {
                    int c = z * kGrid + x;
                    char buf[48];
                    std::snprintf(buf, sizeof buf, " %4.0f:%02x", navY_[c * kNavLevels], nav_[hh][c * kNavLevels] & 0xff);
                    line += navY_[c * kNavLevels] < -1e8f ? "    -   " : buf;
                }
                SDL_Log("nav z%2d%s", z, line.c_str());
            }
    }
}

int World::navLevel(int cell, float y) const {
    if (cell < 0 || cell >= kGrid * kGrid || navY_.empty()) return -1;
    int best = -1;
    float bd = 40;
    for (int l = 0; l < kNavLevels; ++l) {
        float g = navY_[cell * kNavLevels + l];
        if (g < -1e8f) break;
        if (std::abs(g - y) < bd) { bd = std::abs(g - y); best = l; }
    }
    return best;
}
