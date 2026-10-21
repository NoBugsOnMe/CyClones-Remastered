// The game's installer archive (DATA.A00 on the disc, ARJ 2.x): the files the 1994 installer
// copied to the hard disk (CYCLONE.HD, the SOUND folder's effects and the rest), read straight
// from the disc image, so the engine needs no installed copy of the game.
//
// ARJ: each header starts 60 EA, then a u16 size of the basic header (0 ends the archive), the
// basic header, its CRC-32, and extended headers (u16 size, data, CRC-32) up to a zero size. The
// first header is the archive's own; each later one is a file followed by its packed data. Basic
// header: u8 first_hdr_size, u8 version, u8 min version, u8 host OS, u8 flags, u8 method, u8 file
// type, u8 reserved, u32 DOS time, u32 packed size, u32 original size, u32 CRC-32 of the file,
// u16 filespec position, u16 access mode, u16 host data, (first_hdr_size bytes in all), then the
// name and the comment, each ending in a NUL. Methods 1-3 are LZSS with Huffman-coded blocks
// (a 26,624-byte window; decoded as UNARJ, Robert K. Jung's public-domain reference, does);
// method 0 is stored. DATA.A00 uses 0 and 1; method 4 isn't supported.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

class ArjArchive {
public:
    bool open(std::vector<uint8_t> bytes, std::string& error);
    // A file by its path in the archive ("CYCLONE.HD", "SOUND/1.RAW"; any case, / or \).
    bool extract(const std::string& name, std::vector<uint8_t>& out, std::string& error) const;
    std::vector<std::string> names() const;

private:
    struct Entry {
        std::string name;   // upper case, '/' separated
        uint8_t method = 0, flags = 0;
        size_t data = 0;
        uint32_t packed = 0, size = 0, crc = 0;
    };
    std::vector<uint8_t> bytes_;
    std::vector<Entry> entries_;
};
