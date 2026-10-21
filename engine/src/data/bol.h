// CyClones 'BOL' lump archives (CYCLONE.HD, CYCLONE.CD, PIC.BOL).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

class Bol {
public:
    struct Lump {
        std::string name;
        uint32_t offset = 0, size = 0;
    };
    bool load(std::vector<uint8_t> data, std::string& error);
    int find(const std::string& name) const;  // -1 if missing (case-insensitive)
    const uint8_t* data(int index) const { return bytes_.data() + lumps_[index].offset; }
    uint32_t size(int index) const { return lumps_[index].size; }
    const std::vector<Lump>& lumps() const { return lumps_; }

private:
    std::vector<uint8_t> bytes_;
    std::vector<Lump> lumps_;
};
