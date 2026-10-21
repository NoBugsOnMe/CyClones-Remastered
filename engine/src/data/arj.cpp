#include "arj.h"

#include <algorithm>
#include <cstring>

namespace {

uint32_t crc32(const uint8_t* p, size_t n) {
    static uint32_t table[256];
    static bool made = false;
    if (!made) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        made = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

uint16_t u16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
uint32_t u32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

std::string normal(std::string s) {
    for (char& c : s) {
        if (c == '\\') c = '/';
        else if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    }
    return s;
}

// Methods 1-3: as UNARJ's decode() (decode.c), reading from memory and writing the whole file.
class Decoder {
public:
    Decoder(const uint8_t* src, size_t n) : src_(src), left_n_(n) { fillbuf(16); }

    bool run(std::vector<uint8_t>& out, size_t size) {
        out.assign(size, 0);
        size_t r = 0;
        while (r < size) {
            int c = decodeC();
            if (bad_) return false;
            if (c <= 255) {
                out[r++] = (uint8_t)c;
            } else {
                size_t len = (size_t)(c - (256 - kThreshold));
                size_t dist = (size_t)decodeP() + 1;
                if (bad_ || dist > r) return false;
                for (size_t k = 0; k < len && r < size; ++k, ++r) out[r] = out[r - dist];
            }
        }
        return true;
    }

private:
    static constexpr int kThreshold = 3, kNC = 255 + 256 + 2 - kThreshold, kNP = 17, kNT = 19,
                         kCBit = 9, kPBit = 5, kTBit = 5, kNPT = kNT, kCTable = 4096, kPTable = 256;

    void fillbuf(int n) {
        bitbuf_ = (uint16_t)(bitbuf_ << n);
        while (n > bitcount_) {
            n -= bitcount_;
            bitbuf_ |= (uint16_t)(sub_ << n);
            if (left_n_) { sub_ = *src_++; --left_n_; }
            else sub_ = 0;
            bitcount_ = 8;
        }
        bitcount_ -= n;
        bitbuf_ |= (uint16_t)(sub_ >> bitcount_);
    }
    int getbits(int n) {
        int x = bitbuf_ >> (16 - n);
        fillbuf(n);
        return x;
    }

    void makeTable(int nchar, const uint8_t* bitlen, int tablebits, uint16_t* table, int tablesize) {
        uint16_t count[17] = {}, weight[17] = {}, start[18] = {};
        for (int i = 0; i < nchar; ++i) {
            if (bitlen[i] > 16) { bad_ = true; return; }
            count[bitlen[i]]++;
        }
        for (int i = 1; i <= 16; ++i) start[i + 1] = (uint16_t)(start[i] + (count[i] << (16 - i)));
        if (start[17] != 0) { bad_ = true; return; }   // 1 << 16 in 16 bits: the lengths must fill the code space
        int jutbits = 16 - tablebits;
        int i = 1;
        for (; i <= tablebits; ++i) {
            start[i] = (uint16_t)(start[i] >> jutbits);
            weight[i] = (uint16_t)(1 << (tablebits - i));
        }
        for (; i <= 16; ++i) weight[i] = (uint16_t)(1 << (16 - i));
        unsigned k0 = start[tablebits + 1] >> jutbits;
        if (k0 != 0) {   // (1 << 16) >> jutbits wraps to 0 when the table is full
            unsigned kmax = 1u << tablebits;
            while (k0 != kmax) table[k0++] = 0;
        }
        int avail = nchar;
        unsigned mask = 1u << (15 - tablebits);
        for (int ch = 0; ch < nchar; ++ch) {
            int len = bitlen[ch];
            if (!len) continue;
            unsigned k = start[len];
            unsigned nextcode = k + weight[len];
            if (len <= tablebits) {
                if (nextcode > (unsigned)tablesize) { bad_ = true; return; }
                for (unsigned j = start[len]; j < nextcode; ++j) table[j] = (uint16_t)ch;
            } else {
                uint16_t* p = &table[k >> jutbits];
                for (int n = len - tablebits; n; --n) {
                    if (*p == 0) {
                        if (avail >= 2 * kNC - 1) { bad_ = true; return; }
                        right_[avail] = left_[avail] = 0;
                        *p = (uint16_t)avail++;
                    }
                    p = (k & mask) ? &right_[*p] : &left_[*p];
                    k <<= 1;
                }
                *p = (uint16_t)ch;
            }
            start[len] = (uint16_t)nextcode;
        }
    }

    void readPtLen(int nn, int nbit, int special) {
        int n = getbits(nbit);
        if (n == 0) {
            int c = getbits(nbit);
            std::memset(ptLen_, 0, sizeof(ptLen_));
            for (int i = 0; i < 256; ++i) ptTable_[i] = (uint16_t)c;
            return;
        }
        if (n > nn) { bad_ = true; return; }
        int i = 0;
        while (i < n) {
            int c = bitbuf_ >> 13;
            if (c == 7) {
                unsigned mask = 1u << 12;
                while (mask & bitbuf_) { mask >>= 1; ++c; }
                if (c > 16) { bad_ = true; return; }
            }
            fillbuf(c < 7 ? 3 : c - 3);
            ptLen_[i++] = (uint8_t)c;
            if (i == special) {
                int z = getbits(2);
                while (--z >= 0 && i < nn) ptLen_[i++] = 0;
            }
        }
        while (i < nn) ptLen_[i++] = 0;
        makeTable(nn, ptLen_, 8, ptTable_, kPTable);
    }

    void readCLen() {
        int n = getbits(kCBit);
        if (n == 0) {
            int c = getbits(kCBit);
            std::memset(cLen_, 0, sizeof(cLen_));
            for (int i = 0; i < kCTable; ++i) cTable_[i] = (uint16_t)c;
            return;
        }
        if (n > kNC) { bad_ = true; return; }
        int i = 0;
        while (i < n) {
            int c = ptTable_[bitbuf_ >> 8];
            if (c >= kNT) {
                unsigned mask = 1u << 7;
                do {
                    c = (bitbuf_ & mask) ? right_[c] : left_[c];
                    mask >>= 1;
                } while (c >= kNT && mask);
                if (c >= kNT) { bad_ = true; return; }
            }
            fillbuf(ptLen_[c]);
            if (c <= 2) {
                int z = c == 0 ? 1 : c == 1 ? getbits(4) + 3 : getbits(kCBit) + 20;
                while (--z >= 0 && i < kNC) cLen_[i++] = 0;
            } else {
                cLen_[i++] = (uint8_t)(c - 2);
            }
        }
        while (i < kNC) cLen_[i++] = 0;
        makeTable(kNC, cLen_, 12, cTable_, kCTable);
    }

    int decodeC() {
        if (blocksize_ == 0) {
            blocksize_ = (unsigned)getbits(16);
            readPtLen(kNT, kTBit, 3);
            if (!bad_) readCLen();
            if (!bad_) readPtLen(kNP, kPBit, -1);
            if (bad_) return 0;
        }
        --blocksize_;
        int j = cTable_[bitbuf_ >> 4];
        if (j >= kNC) {
            unsigned mask = 1u << 3;
            do {
                j = (bitbuf_ & mask) ? right_[j] : left_[j];
                mask >>= 1;
            } while (j >= kNC && mask);
            if (j >= kNC) { bad_ = true; return 0; }
        }
        fillbuf(cLen_[j]);
        return j;
    }

    int decodeP() {
        int j = ptTable_[bitbuf_ >> 8];
        if (j >= kNP) {
            unsigned mask = 1u << 7;
            do {
                j = (bitbuf_ & mask) ? right_[j] : left_[j];
                mask >>= 1;
            } while (j >= kNP && mask);
            if (j >= kNP) { bad_ = true; return 0; }
        }
        fillbuf(ptLen_[j]);
        if (j != 0) {
            --j;
            j = (1 << j) + getbits(j);
        }
        return j;
    }

    const uint8_t* src_;
    size_t left_n_;
    uint16_t bitbuf_ = 0;
    uint8_t sub_ = 0;
    int bitcount_ = 0;
    unsigned blocksize_ = 0;
    bool bad_ = false;
    uint16_t left_[2 * kNC - 1] = {}, right_[2 * kNC - 1] = {};
    uint8_t cLen_[kNC] = {}, ptLen_[kNPT] = {};
    uint16_t cTable_[kCTable] = {}, ptTable_[kPTable] = {};
};

}  // namespace

bool ArjArchive::open(std::vector<uint8_t> bytes, std::string& error) {
    bytes_ = std::move(bytes);
    entries_.clear();
    const uint8_t* b = bytes_.data();
    size_t n = bytes_.size(), p = 0;
    bool first = true;
    while (true) {
        if (p + 4 > n || b[p] != 0x60 || b[p + 1] != 0xEA) { error = "not an ARJ archive (or damaged)"; return false; }
        size_t hs = u16(b + p + 2);
        if (hs == 0) break;   // the end of the archive
        size_t h = p + 4;
        if (hs < 30 || hs > 2600 || h + hs + 4 > n) { error = "damaged ARJ header"; return false; }
        if (crc32(b + h, hs) != u32(b + h + hs)) { error = "ARJ header CRC mismatch"; return false; }
        size_t q = h + hs + 4;
        while (true) {   // extended headers
            if (q + 2 > n) { error = "damaged ARJ header"; return false; }
            size_t es = u16(b + q);
            q += 2;
            if (es == 0) break;
            q += es + 4;
        }
        if (first) {   // the archive's own header
            first = false;
            p = q;
            continue;
        }
        Entry e;
        size_t fhs = b[h];
        e.flags = b[h + 4];
        e.method = b[h + 5];
        e.packed = u32(b + h + 12);
        e.size = u32(b + h + 16);
        e.crc = u32(b + h + 20);
        size_t nameEnd = h + fhs;
        while (nameEnd < h + hs && b[nameEnd]) ++nameEnd;
        e.name = normal(std::string((const char*)b + h + fhs, nameEnd - (h + fhs)));
        e.data = q;
        if (q + e.packed > n) { error = "ARJ archive cut short at " + e.name; return false; }
        entries_.push_back(e);
        p = q + e.packed;
    }
    return true;
}

bool ArjArchive::extract(const std::string& name, std::vector<uint8_t>& out, std::string& error) const {
    std::string want = normal(name);
    auto it = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.name == want; });
    if (it == entries_.end()) { error = name + " isn't in the archive"; return false; }
    const Entry& e = *it;
    if (e.flags & 0x0D) { error = name + ": garbled or split across volumes"; return false; }
    const uint8_t* src = bytes_.data() + e.data;
    if (e.method == 0) {
        if (e.packed != e.size) { error = name + ": stored with a different size"; return false; }
        out.assign(src, src + e.size);
    } else if (e.method >= 1 && e.method <= 3) {
        Decoder d(src, e.packed);
        if (!d.run(out, e.size)) { error = name + ": damaged packed data"; return false; }
    } else {
        error = name + ": ARJ method " + std::to_string(e.method) + " isn't supported";
        return false;
    }
    if (crc32(out.data(), out.size()) != e.crc) { error = name + ": CRC mismatch"; return false; }
    return true;
}

std::vector<std::string> ArjArchive::names() const {
    std::vector<std::string> v;
    for (const Entry& e : entries_) v.push_back(e.name);
    return v;
}
