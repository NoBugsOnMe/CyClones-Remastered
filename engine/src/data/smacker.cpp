#include "smacker.h"

#include <cstring>
#include <functional>

namespace {

constexpr int kNode = 0x80000000;

uint32_t rd32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

// The 6-bit to 8-bit palette scale.
const uint8_t kPal[64] = {
    0x00, 0x04, 0x08, 0x0C, 0x10, 0x14, 0x18, 0x1C, 0x20, 0x24, 0x28, 0x2C, 0x30, 0x34, 0x38, 0x3C,
    0x41, 0x45, 0x49, 0x4D, 0x51, 0x55, 0x59, 0x5D, 0x61, 0x65, 0x69, 0x6D, 0x71, 0x75, 0x79, 0x7D,
    0x82, 0x86, 0x8A, 0x8E, 0x92, 0x96, 0x9A, 0x9E, 0xA2, 0xA6, 0xAA, 0xAE, 0xB2, 0xB6, 0xBA, 0xBE,
    0xC3, 0xC7, 0xCB, 0xCF, 0xD3, 0xD7, 0xDB, 0xDF, 0xE3, 0xE7, 0xEB, 0xEF, 0xF3, 0xF7, 0xFB, 0xFF};

// Block runs by the 6-bit count in a block type.
const int kRuns[64] = {1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14,  15,  16,  17,   18,   19,   20,  21, 22,
                       23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36,  37,  38,  39,   40,   41,   42,  43, 44,
                       45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58,  59,  128, 256,  512,  1024, 2048};

// A byte tree: 1 = a node (left, then right), 0 = a leaf with an 8-bit value.
struct ByteTree {
    struct N { int l = -1, r = -1, v = 0; };
    std::vector<N> n;
    bool present = false;
    int build(std::function<int()> bit, std::function<int(int)> bits, int depth) {
        int me = (int)n.size();
        n.push_back({});
        if (depth > 32) return me;
        if (bit()) {
            int l = build(bit, bits, depth + 1);
            int r = build(bit, bits, depth + 1);
            n[me].l = l;
            n[me].r = r;
        } else {
            n[me].v = bits(8);
        }
        return me;
    }
};

}  // namespace

bool Smacker::BigTree::read(BitReader& br, int size) {
    ByteTree lo, hi;
    ByteTree* bt[2] = {&lo, &hi};
    auto bit = [&]() { return br.bit(); };
    auto bits = [&](int k) { return br.bits(k); };
    for (ByteTree* b : bt) {
        if (br.bit()) {
            b->present = true;
            b->build(bit, bits, 0);
            br.bit();   // the closing bit
        }
    }
    int esc[3];
    for (int& e : esc) e = br.bits(16);
    last[0] = last[1] = last[2] = -1;
    int length = (size + 3) >> 2;
    t.assign((size_t)length + 3, 0);
    int current = 0;
    auto byteGet = [&](ByteTree& b) {
        if (!b.present) return 0;
        int i = 0;
        while (b.n[i].l >= 0) i = br.bit() ? b.n[i].r : b.n[i].l;
        return b.n[i].v;
    };
    bool ok = true;
    std::function<int(int)> big = [&](int depth) -> int {
        if (!ok || depth > 600 || current >= length || br.pos >= br.n * 8) { ok = false; return 0; }
        if (!br.bit()) {   // a leaf
            int v = byteGet(lo) | byteGet(hi) << 8;
            if (v == esc[0]) { last[0] = current; v = 0; }
            else if (v == esc[1]) { last[1] = current; v = 0; }
            else if (v == esc[2]) { last[2] = current; v = 0; }
            t[current++] = v;
            return 1;
        }
        int at = current++;
        int r = big(depth + 1);
        t[at] = kNode | r;
        ++r;
        return r + big(depth + 1);
    };
    big(0);
    br.bit();
    for (int& l : last)
        if (l == -1) l = current++;
    if (current > (int)t.size()) t.resize(current, 0);
    return ok;
}

int Smacker::BigTree::get(BitReader& br) {
    int i = 0;
    while (t[i] & kNode) {
        if (br.bit()) i += t[i] & ~kNode;
        ++i;
        if (i >= (int)t.size()) return 0;
    }
    int v = t[i];
    if (v != t[last[0]]) {
        t[last[2]] = t[last[1]];
        t[last[1]] = t[last[0]];
        t[last[0]] = v;
    }
    return v;
}

bool Smacker::open(std::vector<uint8_t> file, std::string& error) {
    file_ = std::move(file);
    const uint8_t* d = file_.data();
    if (file_.size() < 104 || (std::memcmp(d, "SMK2", 4) != 0 && std::memcmp(d, "SMK4", 4) != 0)) {
        error = "not a Smacker file";
        return false;
    }
    w_ = (int)rd32(d + 4);
    h_ = (int)rd32(d + 8);
    frames_ = (int)rd32(d + 12);
    int rate = (int)rd32(d + 16);
    fps_ = rate > 0 ? 1000.0f / rate : rate < 0 ? 100000.0f / -rate : 10.0f;
    flags_ = rd32(d + 20);
    uint32_t treeSize = rd32(d + 52);
    int sizes[4] = {(int)rd32(d + 56), (int)rd32(d + 60), (int)rd32(d + 64), (int)rd32(d + 68)};
    if (w_ <= 0 || h_ <= 0 || w_ > 2048 || h_ > 2048 || frames_ <= 0 || frames_ > 100000) {
        error = "bad Smacker header";
        return false;
    }
    int n = frames_ + ((flags_ & 1) ? 1 : 0);
    size_t p = 104;
    if (file_.size() < p + n * 5 + treeSize) { error = "short Smacker file"; return false; }
    sizes_.resize(n);
    types_.resize(n);
    for (int i = 0; i < n; ++i) sizes_[i] = rd32(d + p + i * 4) & ~3u;
    p += n * 4;
    for (int i = 0; i < n; ++i) types_[i] = d[p + i];
    p += n;
    BitReader br{d + p, treeSize, 0};
    BigTree* tables[4] = {&mmap_, &mclr_, &full_, &type_};
    for (int k = 0; k < 4; ++k) {
        if (!br.bit()) {
            tables[k]->t.assign(2, 0);
            tables[k]->last[0] = tables[k]->last[1] = tables[k]->last[2] = 1;
        } else if (!tables[k]->read(br, sizes[k])) {
            error = "bad Smacker tree";
            return false;
        }
    }
    p += treeSize;
    dataPos_ = p;
    px_.assign((size_t)w_ * h_, 0);
    std::memset(pal_, 0, sizeof(pal_));
    cur_ = 0;
    return true;
}

bool Smacker::next() {
    if (cur_ >= frames_) return false;
    size_t size = sizes_[cur_];
    uint8_t type = types_[cur_];
    if (dataPos_ + size > file_.size()) return false;
    const uint8_t* f = file_.data() + dataPos_;
    size_t at = 0;
    dataPos_ += size;
    ++cur_;
    // Palette: a length byte (x 4 bytes, itself included), then runs: 0x80 | n keeps n + 1
    // entries, 0x40 | n copies n + 1 entries from the old palette at an offset, otherwise a new
    // colour (three 6-bit values).
    if (type & 1) {
        size_t len = (size_t)f[0] * 4;
        uint8_t old[768];
        std::memcpy(old, pal_, 768);
        size_t q = 1;
        int sz = 0;
        uint8_t* pal = pal_;
        while (sz < 256 && q < len && q < size) {
            int t = f[q++];
            if (t & 0x80) {
                int k = (t & 0x7f) + 1;
                sz += k;
                pal += k * 3;
            } else if (t & 0x40) {
                int off = f[q++];
                int j = (t & 0x3f) + 1;
                if (off + j > 256) break;
                off *= 3;
                while (j-- && sz < 256) {
                    *pal++ = old[off];
                    *pal++ = old[off + 1];
                    *pal++ = old[off + 2];
                    off += 3;
                    ++sz;
                }
            } else {
                *pal++ = kPal[t & 0x3f];
                *pal++ = kPal[f[q++] & 0x3f];
                *pal++ = kPal[f[q++] & 0x3f];
                ++sz;
            }
        }
        at = len;
    }
    // Audio chunks (none in this game's videos): skipped.
    for (int k = 0; k < 7; ++k)
        if (type & (2 << k)) {
            if (at + 4 > size) return true;
            at += rd32(f + at);
        }
    if (at >= size) return true;
    BitReader br{f + at, size - at, 0};
    mmap_.resetLast();
    mclr_.resetLast();
    full_.resetLast();
    type_.resetLast();
    int bw = w_ / 4, bh = h_ / 4, blocks = bw * bh, blk = 0;
    bool smk4 = std::memcmp(file_.data(), "SMK4", 4) == 0;
    while (blk < blocks) {
        int t = type_.get(br);
        int run = kRuns[(t >> 2) & 0x3f];
        switch (t & 3) {
        case 0:   // two colours and a 16-bit map
            while (run-- && blk < blocks) {
                int clr = mclr_.get(br), map = mmap_.get(br);
                uint8_t hiC = (uint8_t)(clr >> 8), loC = (uint8_t)clr;
                uint8_t* o = &px_[(size_t)(blk / bw) * 4 * w_ + (blk % bw) * 4];
                for (int i = 0; i < 4; ++i, o += w_, map >>= 4)
                    for (int x = 0; x < 4; ++x) o[x] = (map >> x) & 1 ? hiC : loC;
                ++blk;
            }
            break;
        case 1: {   // full block
            int mode = 0;
            if (smk4) {
                if (br.bit()) mode = 1;
                else if (br.bit()) mode = 2;
            }
            while (run-- && blk < blocks) {
                uint8_t* o = &px_[(size_t)(blk / bw) * 4 * w_ + (blk % bw) * 4];
                if (mode == 0) {
                    for (int i = 0; i < 4; ++i, o += w_) {
                        int p = full_.get(br);
                        o[2] = (uint8_t)p;
                        o[3] = (uint8_t)(p >> 8);
                        p = full_.get(br);
                        o[0] = (uint8_t)p;
                        o[1] = (uint8_t)(p >> 8);
                    }
                } else if (mode == 1) {   // double width and height
                    for (int i = 0; i < 2; ++i) {
                        int p = full_.get(br);
                        for (int r = 0; r < 2; ++r, o += w_) {
                            o[0] = o[1] = (uint8_t)p;
                            o[2] = o[3] = (uint8_t)(p >> 8);
                        }
                    }
                } else {   // double height
                    for (int i = 0; i < 2; ++i) {
                        int p = full_.get(br);
                        int q = full_.get(br);
                        for (int r = 0; r < 2; ++r, o += w_) {
                            o[2] = (uint8_t)p;
                            o[3] = (uint8_t)(p >> 8);
                            o[0] = (uint8_t)q;
                            o[1] = (uint8_t)(q >> 8);
                        }
                    }
                }
                ++blk;
            }
            break;
        }
        case 2:   // unchanged
            blk += run;
            break;
        case 3: {  // one colour
            uint8_t c = (uint8_t)(t >> 8);
            while (run-- && blk < blocks) {
                uint8_t* o = &px_[(size_t)(blk / bw) * 4 * w_ + (blk % bw) * 4];
                for (int i = 0; i < 4; ++i, o += w_) std::memset(o, c, 4);
                ++blk;
            }
            break;
        }
        }
    }
    return true;
}
