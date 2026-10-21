// Animated walls, flats, items and props, as the game animates them (the 'surfanim' lump of
// CYCLONE.HD, loaded by FUN_00021db2 into 0xcdb54 and stepped every tick by FUN_00021de5).
//
// Each entry names a range of textures or sprites of one kind (0 walls from 'startwalls', 1 flats,
// 2 items from 'startitems', 3 props from 'startobjects') and a period in ticks. Every period each
// number in the range moves on to the next picture of the range, wrapping round, so a wall
// showing the first frame shows the second, and so on: a screen flickers, a fan turns, a power
// coil glows. Level scripts switch entries off and on (actions 23 and 22): ALIEN1's power coils
// stand still until the ship's power is back.
#include "world_internal.h"

namespace {

const uint16_t kAnimSize = 1024;   // texture and sprite numbers per kind

}  // namespace

void World::initSurfAnims() {
    surfAnims_.clear();
    for (auto& f : animFrame_) {
        f.resize(kAnimSize);
        for (uint16_t k = 0; k < kAnimSize; ++k) f[k] = k;
    }
    int lump = data_->hd.find("surfanim");
    if (lump >= 0 && data_->hd.size(lump) >= 2) {
        const uint8_t* d = data_->hd.data(lump);
        size_t size = data_->hd.size(lump);
        int last = (int16_t)(d[0] | d[1] << 8);
        for (int k = 0; k <= last && 2 + (size_t)(k + 1) * 12 <= size; ++k) {
            const uint8_t* e = d + 2 + k * 12;
            auto s16 = [&](int o) { return (int16_t)(e[o] | e[o + 1] << 8); };
            SurfAnim a;
            a.on = s16(0) != 0;
            a.kind = s16(2);
            a.period = s16(4);
            a.count = s16(6);
            a.first = s16(8);
            a.last = s16(10);
            if (a.kind < 0 || a.kind > 4 || a.first < 0 || a.last >= kAnimSize || a.first > a.last) a.on = false;
            surfAnims_.push_back(a);
        }
    }
    renderer_->setAnimFrames(animFrame_[0], animFrame_[1]);
}

// One tick (FUN_00021de5): each entry that is on counts down; when it runs out, every number in its
// range shows the next picture.
void World::surfAnimTick() {
    bool textures = false;
    for (auto& a : surfAnims_) {
        if (!a.on) continue;
        if (--a.count >= 1) continue;
        a.count += a.period;
        std::vector<uint16_t>& f = animFrame_[a.kind];
        for (int n = a.first; n <= a.last; ++n) {
            f[n] = f[n] + 1;
            if (f[n] > a.last) f[n] = (uint16_t)a.first;
        }
        if (a.kind <= 1) textures = true;
    }
    // An activated data transmitter's screens (an addition, see World::useWall): its own frames,
    // a new one every 12 ticks.
    if (ticks_ % 12 == 0) {
        std::vector<uint16_t>& f = animFrame_[0];
        for (int n = Renderer::kActiveTerminal; n < Renderer::kActiveTerminal + Renderer::kActiveTerminalFrames && n < (int)f.size(); ++n)
            f[n] = (uint16_t)(Renderer::kActiveTerminal + (f[n] - Renderer::kActiveTerminal + 1) % Renderer::kActiveTerminalFrames);
        textures = true;
    }
    if (textures) renderer_->setAnimFrames(animFrame_[0], animFrame_[1]);
}

// Actions 22 and 23 (FUN_0002208b, FUN_000220ca): an entry on or off; off, its pictures stay as they are.
void World::setSurfAnim(int entry, bool on) {
    if (entry < 0 || entry >= (int)surfAnims_.size()) return;
    surfAnims_[entry].on = on;
}

// A prop's or an item's picture in its animated range (-1: it has none). frameFor gives the
// frame of the range the sprite number shows now.
World::AnimRange World::spriteAnim(int kind, const std::string& lump) const {
    AnimRange r;
    const Bol& b = kind == 3 ? data_->cd : data_->hd;
    int start = b.find(kind == 3 ? "startobjects" : "startitems");
    int i = b.find(lump);
    if (start < 0 || i < 0) return r;
    int n = i - start;
    for (const auto& a : surfAnims_)
        if (a.kind == kind && n >= a.first && n <= a.last) {
            r.number = n;
            r.first = a.first;
            r.last = a.last;
            for (int k = a.first; k <= a.last; ++k) r.frames.push_back(b.lumps()[start + k].name);
            break;
        }
    return r;
}

// An item's picture: its sprite, or every picture of its animated range (a placed demo pack
// blinks). Sets the item's animation fields.
const Visual* World::itemVisual(const std::string& sprite, Item& it) {
    AnimRange r = spriteAnim(2, sprite);
    it.animNumber = r.number;
    it.animFirst = r.first;
    if (r.number >= 0) return spriteVisual("item:anim" + std::to_string(r.first), r.frames, false, true, 0.1f);
    return spriteVisual("item:" + lower(sprite), {sprite}, false, false, 1.0f);
}
