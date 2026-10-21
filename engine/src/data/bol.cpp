#include "bol.h"

#include <cctype>
#include <cstring>

static uint32_t rd32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

bool Bol::load(std::vector<uint8_t> data, std::string& error) {
    bytes_ = std::move(data);
    if (bytes_.size() < 16) { error = "archive too small"; return false; }
    uint32_t count = bytes_[0] | bytes_[1] << 8;
    uint32_t dirOff = rd32(&bytes_[2]);
    uint32_t dirSize = rd32(&bytes_[6]);
    if ((uint64_t)dirOff + dirSize > bytes_.size() || (uint64_t)count * 12 > dirSize) {
        error = "bad archive directory";
        return false;
    }
    lumps_.clear();
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* e = &bytes_[dirOff + i * 12];
        Lump l;
        l.offset = rd32(e);
        l.size = rd32(e + 4);
        uint32_t nameOff = dirOff + rd32(e + 8);
        if (nameOff < bytes_.size()) {
            const char* s = (const char*)&bytes_[nameOff];
            size_t n = strnlen(s, bytes_.size() - nameOff);
            l.name.assign(s, n);
        }
        if ((uint64_t)l.offset + l.size > bytes_.size()) { error = "lump out of range: " + l.name; return false; }
        lumps_.push_back(l);
    }
    return true;
}

int Bol::find(const std::string& name) const {
    for (size_t i = 0; i < lumps_.size(); ++i) {
        const std::string& n = lumps_[i].name;
        if (n.size() != name.size()) continue;
        bool eq = true;
        for (size_t k = 0; k < n.size() && eq; ++k)
            eq = std::tolower((unsigned char)n[k]) == std::tolower((unsigned char)name[k]);
        if (eq) return (int)i;
    }
    return -1;
}
