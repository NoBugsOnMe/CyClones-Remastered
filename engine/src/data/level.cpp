#include "level.h"

#include <cstring>

namespace {

struct Reader {
    const uint8_t* p;
    size_t n, o = 0;
    template <size_t N>
    bool u16(std::array<uint16_t, N>& dst) {
        if (o + N * 2 > n) return false;
        for (size_t i = 0; i < N; ++i) dst[i] = (uint16_t)(p[o + i * 2] | p[o + i * 2 + 1] << 8);
        o += N * 2;
        return true;
    }
    template <size_t N>
    bool u8(std::array<uint8_t, N>& dst) {
        if (o + N > n) return false;
        std::memcpy(dst.data(), p + o, N);
        o += N;
        return true;
    }
    bool skip(size_t k) {
        if (o + k > n) return false;
        o += k;
        return true;
    }
};

bool readSurface(Reader& r, SurfaceLayer& s) { return r.u16(s.tex) && r.u16(s.height) && r.u8(s.extra); }

bool readEdges(Reader& r, EdgeLayer& e) {
    return r.u16(e.texFront) && r.u16(e.texBack) && r.u16(e.base) && r.u8(e.flags) && r.u8(e.light) &&
           r.u16(e.vStart) && r.u16(e.vEnd);
}

}  // namespace

bool Level::parse(const std::vector<uint8_t>& file, std::string& error) {
    size_t o = 0;
    bool haveMap = false;
    while (o + 10 <= file.size()) {
        std::string tag((const char*)&file[o], 4);
        uint32_t size = file[o + 6] | file[o + 7] << 8 | file[o + 8] << 16 | (uint32_t)file[o + 9] << 24;
        if (o + 10 + size > file.size()) { error = "truncated chunk " + tag; return false; }
        Reader r{&file[o + 10], size};
        bool ok = true;
        if (tag == "MAP ") {
            if (size < 4) { error = "bad MAP chunk"; return false; }
            width = file[o + 10] | file[o + 11] << 8;
            height = file[o + 12] | file[o + 13] << 8;
            std::array<uint8_t, kGrid * kGrid> unused{};
            ok = r.skip(4) && r.u8(shape) && r.u8(light) && r.u8(region) && r.u8(lightType) && r.u8(unused);
            haveMap = ok;
        } else if (tag == "MAPC") ok = readSurface(r, ceiling);
        else if (tag == "MAPF") ok = readSurface(r, floor);
        else if (tag == "MAP1") ok = readSurface(r, extra1);
        else if (tag == "MAP2") ok = readSurface(r, extra2);
        else if (tag == "MAPN") ok = readEdges(r, north);
        else if (tag == "MAPW") ok = readEdges(r, west);
        else if (tag == "CUBE") {
            for (size_t k = 0; k + 56 <= size; k += 56) {
                std::array<uint8_t, 56> c;
                std::memcpy(c.data(), &file[o + 10 + k], 56);
                cubes.push_back(c);
            }
        } else {
            otherChunks.emplace_back(tag, std::vector<uint8_t>(file.begin() + o + 10, file.begin() + o + 10 + size));
        }
        if (!ok) { error = "short chunk " + tag; return false; }
        o += 10 + size;
    }
    if (!haveMap) { error = "no MAP chunk"; return false; }
    if (width <= 0 || width > kGrid || height <= 0 || height > kGrid) width = height = kGrid;
    return true;
}
