// Smacker video (the game's *.SMK cut-scenes: "SMK2", 320x200, 8-bit palette, no audio
// tracks). A decoder written from the format: four Huffman-coded tables (block types, two-colour
// maps and colours, full blocks) over 4x4 blocks, and per-frame palette deltas.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

class Smacker {
public:
    bool open(std::vector<uint8_t> file, std::string& error);
    int width() const { return w_; }
    int height() const { return h_; }
    int frames() const { return frames_; }
    float headerFps() const { return fps_; }
    // Decodes the next frame into pixels() (palette indices, width x height) and palette()
    // (256 x RGB, 8 bits each). Returns false after the last frame.
    bool next();
    int frame() const { return cur_; }   // frames decoded so far
    const std::vector<uint8_t>& pixels() const { return px_; }
    const uint8_t* palette() const { return pal_; }

private:
    struct BitReader {
        const uint8_t* p = nullptr;
        size_t n = 0, pos = 0;   // in bits
        int bit() {
            if (pos >= n * 8) return 0;
            int b = (p[pos >> 3] >> (pos & 7)) & 1;
            ++pos;
            return b;
        }
        int bits(int k) {
            int v = 0;
            for (int i = 0; i < k; ++i) v |= bit() << i;
            return v;
        }
    };
    // One of the four tables: a big tree of 16-bit values, stored flat (a node holds the size
    // of its left half), with the three-entry recent-value cache the format uses.
    struct BigTree {
        std::vector<int> t;
        int last[3] = {0, 0, 0};
        bool read(BitReader& br, int size);
        int get(BitReader& br);
        void resetLast() { for (int k : last) if (k >= 0 && k < (int)t.size()) t[k] = 0; }
    };

    std::vector<uint8_t> file_;
    int w_ = 0, h_ = 0, frames_ = 0, cur_ = 0;
    float fps_ = 10;
    uint32_t flags_ = 0;
    std::vector<uint32_t> sizes_;
    std::vector<uint8_t> types_;
    size_t dataPos_ = 0;
    BigTree mmap_, mclr_, full_, type_;
    std::vector<uint8_t> px_;
    uint8_t pal_[768] = {};
};
