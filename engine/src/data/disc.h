// Read files straight out of the player's CyClones CD image (raw .bin, .iso, or a .cue pointing at either).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct SDL_IOStream;

class DiscImage {
public:
    ~DiscImage();
    bool open(const std::string& path, std::string& error);
    // Path inside the ISO 9660 filesystem, case-insensitive, '/' separated, e.g. "GAME/HIVE1.MAP".
    bool readFile(const std::string& isoPath, std::vector<uint8_t>& out) const;
    std::vector<std::string> list(const std::string& isoDir) const;

private:
    struct Entry {
        std::string name;
        uint32_t lba = 0, size = 0;
        bool dir = false;
    };
    bool readSectors(uint32_t lba, uint32_t count, uint8_t* dst) const;
    std::vector<Entry> readDir(uint32_t lba, uint32_t size) const;
    bool lookup(const std::string& isoPath, Entry& out) const;

    SDL_IOStream* io_ = nullptr;
    uint32_t sectorSize_ = 2048;
    uint32_t dataOffset_ = 0;
    Entry root_;
};
