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

    // A game folder instead of a CD image (open() given a folder): the CyClones demo, whose CD
    // files sit in its ODATA folder (as its CYCLONE.CFG names it) with CYCLONE.HD and the SOUND
    // folder at its top. "GAME/x" is looked for in ODATA (or GAME), anything else at the top; any
    // case. There are no CD audio tracks.
    bool isFolder() const { return !folder_.empty(); }
    const std::string& folder() const { return folder_; }
    // The file at a path inside the folder, as it is spelled on disk ("" if there is none).
    std::string folderPath(const std::string& relPath) const;
    // Whether dir holds such a game: CYCLONE.HD and an ODATA (or GAME) folder with CYCLONE.CD.
    static bool isGameFolder(const std::string& dir);

private:
    struct Entry {
        std::string name;
        uint32_t lba = 0, size = 0;
        bool dir = false;
    };
    bool readSectors(uint32_t lba, uint32_t count, uint8_t* dst) const;
    std::vector<Entry> readDir(uint32_t lba, uint32_t size) const;
    bool lookup(const std::string& isoPath, Entry& out) const;

    std::string folder_, gameDir_;   // folder mode: the folder (ending in a separator), its ODATA
    SDL_IOStream* io_ = nullptr;
    uint32_t sectorSize_ = 2048;
    uint32_t dataOffset_ = 0;
    Entry root_;
};
