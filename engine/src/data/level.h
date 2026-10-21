// CyClones level (.MAP) data, decoded per the loader in CYCLONE.EXE (FUN_00014179).
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

constexpr int kGrid = 64;      // cells per side in every grid layer
constexpr int kCellSize = 64;  // world units per cell (1 unit = 1 texel)

struct SurfaceLayer {          // MAPF / MAPC / MAP1 / MAP2
    std::array<uint16_t, kGrid * kGrid> tex{};     // low 14 bits: flat number (from 'startflats'), top 2: flags
    std::array<uint16_t, kGrid * kGrid> height{};  // height of this cell's surface; slopes borrow neighbours'
    std::array<uint8_t, kGrid * kGrid> extra{};
};

struct EdgeLayer {             // MAPN (north edges) / MAPW (west edges)
    std::array<uint16_t, kGrid * kGrid> texFront{};  // wall number (from 'startwalls'), 0 = no wall
    std::array<uint16_t, kGrid * kGrid> texBack{};   // 0 = same as front
    std::array<uint16_t, kGrid * kGrid> base{};      // world height of the wall's bottom
    std::array<uint8_t, kGrid * kGrid> flags{};      // bit0 = not solid (no collision)
    std::array<uint8_t, kGrid * kGrid> light{};      // signed light offset
    std::array<uint16_t, kGrid * kGrid> vStart{};    // texture rows shown: vStart..vEnd (0,0 = texture height)
    std::array<uint16_t, kGrid * kGrid> vEnd{};
};

struct Level {
    std::string name;
    int width = 0, height = 0;                       // playable area (e.g. 40 x 40)
    std::array<uint8_t, kGrid * kGrid> shape{};      // MAP layer 0: bits0-2 surface shape (0 flat, 1 quad slope, 2/3 split)
    std::array<uint8_t, kGrid * kGrid> light{};      // MAP layer 1: signed static light offset per cell
    std::array<uint8_t, kGrid * kGrid> region{};     // MAP layer 2 (0x802d4, 3 bytes a cell): an area number scripts test
    std::array<uint8_t, kGrid * kGrid> lightType{};  // MAP layer 3: animated light type
    SurfaceLayer floor, ceiling, extra1, extra2;
    EdgeLayer north, west;
    std::vector<std::array<uint8_t, 56>> cubes;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> otherChunks;

    bool parse(const std::vector<uint8_t>& file, std::string& error);
};
