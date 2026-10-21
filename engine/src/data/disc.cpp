#include "disc.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cctype>
#include <cstring>

static std::string upper(std::string s) {
    for (auto& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

static std::string dirOf(const std::string& p) {
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? std::string() : p.substr(0, s + 1);
}

DiscImage::~DiscImage() {
    if (io_) SDL_CloseIO(io_);
}

bool DiscImage::open(const std::string& pathIn, std::string& error) {
    std::string path = pathIn;
    // .cue: use the first FILE entry (relative to the cue's folder if not absolute)
    if (path.size() > 4 && upper(path.substr(path.size() - 4)) == ".CUE") {
        size_t len = 0;
        char* txt = (char*)SDL_LoadFile(path.c_str(), &len);
        if (!txt) { error = "Cannot read " + path; return false; }
        std::string cue(txt, len);
        SDL_free(txt);
        size_t f = upper(cue).find("FILE \"");
        if (f == std::string::npos) { error = "No FILE line in " + path; return false; }
        size_t e = cue.find('"', f + 6);
        std::string bin = cue.substr(f + 6, e - (f + 6));
        bool absolute = (bin.size() > 1 && bin[1] == ':') || (!bin.empty() && (bin[0] == '/' || bin[0] == '\\'));
        path = absolute ? bin : dirOf(path) + bin;
    }
    io_ = SDL_IOFromFile(path.c_str(), "rb");
    if (!io_) { error = "Cannot open disc image " + path + ": " + SDL_GetError(); return false; }

    // Detect raw 2352-byte sectors (MODE1) vs plain 2048-byte ISO by looking for the volume descriptor.
    const struct { uint32_t size, offset; } layouts[] = {{2352, 16}, {2048, 0}, {2336, 8}};
    uint8_t buf[2048];
    bool found = false;
    for (auto& l : layouts) {
        sectorSize_ = l.size;
        dataOffset_ = l.offset;
        if (readSectors(16, 1, buf) && buf[0] == 1 && std::memcmp(buf + 1, "CD001", 5) == 0) {
            found = true;
            break;
        }
    }
    if (!found) { error = "Not a CD image (no ISO 9660 volume found): " + path; return false; }
    const uint8_t* rr = buf + 156;  // root directory record
    root_.lba = rr[2] | rr[3] << 8 | rr[4] << 16 | (uint32_t)rr[5] << 24;
    root_.size = rr[10] | rr[11] << 8 | rr[12] << 16 | (uint32_t)rr[13] << 24;
    root_.dir = true;
    return true;
}

bool DiscImage::readSectors(uint32_t lba, uint32_t count, uint8_t* dst) const {
    std::vector<uint8_t> raw(sectorSize_);
    for (uint32_t i = 0; i < count; ++i) {
        if (SDL_SeekIO(io_, (Sint64)(lba + i) * sectorSize_, SDL_IO_SEEK_SET) < 0) return false;
        if (SDL_ReadIO(io_, raw.data(), sectorSize_) != sectorSize_) return false;
        std::memcpy(dst + (size_t)i * 2048, raw.data() + dataOffset_, 2048);
    }
    return true;
}

std::vector<DiscImage::Entry> DiscImage::readDir(uint32_t lba, uint32_t size) const {
    std::vector<Entry> out;
    uint32_t sectors = (size + 2047) / 2048;
    std::vector<uint8_t> d((size_t)sectors * 2048);
    if (!readSectors(lba, sectors, d.data())) return out;
    size_t o = 0;
    while (o < size) {
        uint8_t len = d[o];
        if (len == 0) {  // records don't cross sector boundaries
            o = (o / 2048 + 1) * 2048;
            continue;
        }
        Entry e;
        e.lba = d[o + 2] | d[o + 3] << 8 | d[o + 4] << 16 | (uint32_t)d[o + 5] << 24;
        e.size = d[o + 10] | d[o + 11] << 8 | d[o + 12] << 16 | (uint32_t)d[o + 13] << 24;
        e.dir = (d[o + 25] & 2) != 0;
        uint8_t nl = d[o + 32];
        e.name.assign((const char*)&d[o + 33], nl);
        size_t semi = e.name.find(';');
        if (semi != std::string::npos) e.name.resize(semi);
        if (!e.name.empty() && e.name.back() == '.') e.name.pop_back();
        if (!(nl == 1 && (d[o + 33] == 0 || d[o + 33] == 1))) out.push_back(e);
        o += len;
    }
    return out;
}

bool DiscImage::lookup(const std::string& isoPath, Entry& out) const {
    Entry cur = root_;
    std::string p = upper(isoPath);
    size_t start = 0;
    while (start < p.size()) {
        size_t slash = p.find('/', start);
        std::string part = p.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        start = slash == std::string::npos ? p.size() : slash + 1;
        if (part.empty()) continue;
        bool hit = false;
        for (auto& e : readDir(cur.lba, cur.size)) {
            if (upper(e.name) == part) { cur = e; hit = true; break; }
        }
        if (!hit) return false;
    }
    out = cur;
    return true;
}

bool DiscImage::readFile(const std::string& isoPath, std::vector<uint8_t>& out) const {
    Entry e;
    if (!lookup(isoPath, e) || e.dir) return false;
    uint32_t sectors = (e.size + 2047) / 2048;
    out.resize((size_t)sectors * 2048);
    if (!readSectors(e.lba, sectors, out.data())) return false;
    out.resize(e.size);
    return true;
}

std::vector<std::string> DiscImage::list(const std::string& isoDir) const {
    std::vector<std::string> names;
    Entry e;
    if (!lookup(isoDir, e) || !e.dir) return names;
    for (auto& c : readDir(e.lba, e.size)) names.push_back(c.name);
    std::sort(names.begin(), names.end());
    return names;
}
