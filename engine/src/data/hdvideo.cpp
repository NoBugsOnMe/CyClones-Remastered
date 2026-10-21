#include "hdvideo.h"

#include <SDL3/SDL.h>
#include <dav1d/dav1d.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>

#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#include "../../third_party/dr_flac.h"

namespace {

uint32_t u32le(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

bool readAll(const std::string& path, std::vector<uint8_t>& out) {
    size_t n = 0;
    void* d = SDL_LoadFile(path.c_str(), &n);
    if (!d) return false;
    out.assign((const uint8_t*)d, (const uint8_t*)d + n);
    SDL_free(d);
    return true;
}

// LEB128 (the OBUs' sizes).
bool leb128(const uint8_t*& p, const uint8_t* end, uint64_t& v) {
    v = 0;
    for (int i = 0; i < 8; ++i) {
        if (p >= end) return false;
        uint8_t b = *p++;
        v |= (uint64_t)(b & 0x7f) << (7 * i);
        if (!(b & 0x80)) return true;
    }
    return false;
}

// What a temporal unit holds: a sequence header, and whether its first frame is a key frame.
void scanUnit(const uint8_t* p, size_t n, bool& seq, bool& key) {
    seq = key = false;
    const uint8_t* end = p + n;
    bool firstFrame = true;
    while (p < end) {
        uint8_t h = *p++;
        int type = (h >> 3) & 15;
        bool ext = h & 4, sized = h & 2;
        if (ext) ++p;
        uint64_t size = (uint64_t)(end - p);
        if (sized && !leb128(p, end, size)) return;
        if (p + size > end) return;
        if (type == 1) seq = true;
        if ((type == 3 || type == 6) && firstFrame && size > 0) {
            firstFrame = false;
            // uncompressed_header: show_existing_frame (1 bit), then frame_type (2 bits; 0 = key)
            uint8_t b = p[0];
            key = !(b & 0x80) && ((b >> 5) & 3) == 0;
        }
        p += size;
    }
}

void noFree(const uint8_t*, void*) {}

}  // namespace

HdStream::~HdStream() { close(); }

void HdStream::close() {
    if (ctx_) dav1d_close(&ctx_);
    ctx_ = nullptr;
    file_.clear();
    tu_.clear();
    fed_ = next_ = 0;
    pendingLeft_ = 0;
}

bool HdStream::open(const std::string& path, std::string& error) {
    close();
    if (!readAll(path, file_)) { error = "can't read " + path; return false; }
    if (file_.size() < 32 || std::memcmp(file_.data(), "DKIF", 4) != 0 || std::memcmp(file_.data() + 8, "AV01", 4) != 0) {
        error = path + " isn't AV1 in IVF";
        file_.clear();
        return false;
    }
    size_t hdr = file_[6] | file_[7] << 8;
    w_ = file_[12] | file_[13] << 8;
    h_ = file_[14] | file_[15] << 8;
    for (size_t p = hdr; p + 12 <= file_.size();) {
        uint32_t n = u32le(file_.data() + p);
        if (p + 12 + n > file_.size()) break;
        Unit u;
        u.off = p + 12;
        u.size = n;
        bool seq = false;
        scanUnit(file_.data() + u.off, n, seq, u.key);
        u.key = u.key && seq;   // a place to start decoding from: the sequence header comes along
        tu_.push_back(u);
        p += 12 + n;
    }
    if (tu_.empty() || !tu_[0].key) { error = path + ": no key frame at the start"; file_.clear(); tu_.clear(); return false; }
    Dav1dSettings s;
    dav1d_default_settings(&s);
    s.n_threads = std::clamp(SDL_GetNumLogicalCPUCores(), 1, 8);
    s.max_frame_delay = 1;   // a picture for every unit sent
    s.logger.callback = nullptr;
    if (dav1d_open(&ctx_, &s) < 0) { error = "dav1d won't start"; ctx_ = nullptr; return false; }
    return true;
}

// The decoder's next picture (into rgba if given), sending it units as it asks for them.
bool HdStream::decodeNext(std::vector<uint8_t>* rgba) {
    Dav1dPicture pic;
    std::memset(&pic, 0, sizeof(pic));
    for (int guard = 0; guard < 64; ++guard) {
        int r = dav1d_get_picture(ctx_, &pic);
        if (r == 0) break;
        if (r != DAV1D_ERR(EAGAIN)) return false;
        if (!pendingLeft_) {
            if (fed_ >= (int)tu_.size()) return false;
            pendingOff_ = tu_[fed_].off;
            pendingLeft_ = tu_[fed_].size;
            ++fed_;
        }
        Dav1dData d;
        std::memset(&d, 0, sizeof(d));
        if (dav1d_data_wrap(&d, file_.data() + pendingOff_, pendingLeft_, noFree, nullptr) < 0) return false;
        r = dav1d_send_data(ctx_, &d);
        if (r < 0 && r != DAV1D_ERR(EAGAIN)) { dav1d_data_unref(&d); return false; }
        size_t used = pendingLeft_ - d.sz;
        pendingOff_ += used;
        pendingLeft_ = d.sz;
        dav1d_data_unref(&d);
        if (guard == 63) return false;
    }
    if (!pic.data[0]) return false;
    ++next_;
    if (rgba) {
        // BT.709, studio range (as the pack is made), 4:2:0, 8 or 10 bits; 10 bits come down to
        // 8 with a 2x2 ordered dither so the smooth skies don't band.
        int w = pic.p.w, h = pic.p.h;
        rgba->resize((size_t)w * h * 4);
        uint8_t* o = rgba->data();
        const bool hi = pic.p.bpc > 8;
        static const int bayer[4] = {8192, 40960, 57344, 24576};   // (0, 2, 3, 1) / 4 + 1/8 of a step, x 65536
        for (int y = 0; y < h; ++y) {
            const uint8_t* yr = (const uint8_t*)pic.data[0] + (ptrdiff_t)y * pic.stride[0];
            const uint8_t* ur = (const uint8_t*)pic.data[1] + (ptrdiff_t)(y >> 1) * pic.stride[1];
            const uint8_t* vr = (const uint8_t*)pic.data[2] + (ptrdiff_t)(y >> 1) * pic.stride[1];
            for (int x = 0; x < w; ++x) {
                int Y, U, V;
                if (hi) {
                    Y = ((const uint16_t*)yr)[x];
                    U = ((const uint16_t*)ur)[x >> 1];
                    V = ((const uint16_t*)vr)[x >> 1];
                } else {
                    Y = yr[x] << 2;
                    U = ur[x >> 1] << 2;
                    V = vr[x >> 1] << 2;
                }
                // 10-bit studio range to 8-bit full, x 65536: Y 64..940, Cb / Cr centred on 512
                int c = (Y - 64) * 19077, d = U - 512, e = V - 512;
                int dt = hi ? bayer[((y & 1) << 1) | (x & 1)] : 32768;
                auto px = [&](int v) { return (uint8_t)std::clamp((v + dt) >> 16, 0, 255); };
                o[0] = px(c + 29372 * e);
                o[1] = px(c - 3493 * d - 8731 * e);
                o[2] = px(c + 34609 * d);
                o[3] = 255;
                o += 4;
            }
        }
    }
    dav1d_picture_unref(&pic);
    return true;
}

bool HdStream::frame(int n, std::vector<uint8_t>& rgba) {
    if (!ctx_ || n < 0 || n >= (int)tu_.size()) return false;
    int k = n;
    while (k > 0 && !tu_[k].key) --k;
    if (n < next_ || k > next_) {   // back, or past a key frame: start from the key frame
        dav1d_flush(ctx_);
        fed_ = next_ = k;
        pendingLeft_ = 0;
    }
    while (next_ < n)
        if (!decodeNext(nullptr)) return false;
    return decodeNext(&rgba);
}

// ---------------------------------------------------------------------------------------------

bool HdPack::open(const std::string& dir) {
    videos_.clear();
    speechFiles_.clear();
    subtitles_.clear();
    speechCache_.clear();
    dir_ = dir;
    if (!dir_.empty() && dir_.back() != '/' && dir_.back() != '\\') dir_ += '/';
    std::vector<uint8_t> text;
    if (!readAll(dir_ + "index.txt", text)) return false;
    std::istringstream in(std::string(text.begin(), text.end()));
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string kind;
        ls >> kind;
        if (kind == "video") {
            HdVideoInfo v;
            std::string mode;
            ls >> v.name >> mode;
            for (auto& c : v.name) c = (char)std::toupper((unsigned char)c);
            if (mode == "full") ls >> v.file;
            else if (mode == "window") {
                v.window = true;
                ls >> v.x >> v.y >> v.w >> v.h >> v.file;
                std::string run;
                int pic = 0;
                while (ls >> run) {
                    if (run.size() < 2) continue;
                    int n = std::atoi(run.c_str() + 1);
                    for (int i = 0; i < n; ++i) v.picture.push_back(run[0] == 'B' ? pic++ : -1);
                }
            } else continue;
            if (!v.file.empty()) videos_[v.name] = v;
        } else if (kind == "speech") {
            int id = -1;
            std::string file;
            ls >> id >> file;
            if (id >= 0 && !file.empty()) speechFiles_[id] = file;
        } else if (kind == "subtitles") {
            int id = -1;
            std::string file;
            ls >> id >> file;
            std::vector<uint8_t> srt;
            if (id >= 0 && !file.empty() && readAll(dir_ + file, srt)) {
                auto cues = parseSrt(std::string(srt.begin(), srt.end()));
                if (!cues.empty()) subtitles_[id] = std::move(cues);
            }
        }
    }
    return loaded() || !subtitles_.empty();
}

// SubRip: a number, "00:00:01,500 --> 00:00:04,000", the lines, a blank line. Lowercase becomes
// capitals and characters the game's font lacks are dropped or replaced.
std::vector<SubtitleCue> HdPack::parseSrt(const std::string& text) {
    std::vector<SubtitleCue> cues;
    std::istringstream in(text);
    std::string line;
    auto stamp = [](const std::string& s, size_t at) {
        int h = 0, m = 0, sec = 0, ms = 0;
        if (std::sscanf(s.c_str() + at, "%d:%d:%d%*[,.]%d", &h, &m, &sec, &ms) < 3) return -1.0;
        return h * 3600.0 + m * 60.0 + sec + ms / 1000.0;
    };
    SubtitleCue cur;
    bool inCue = false;
    auto flush = [&]() {
        if (inCue && !cur.text.empty()) cues.push_back(cur);
        cur = SubtitleCue();
        inCue = false;
    };
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.size() >= 3 && (uint8_t)line[0] == 0xEF && (uint8_t)line[1] == 0xBB && (uint8_t)line[2] == 0xBF) line.erase(0, 3);
        size_t arrow = line.find("-->");
        if (arrow != std::string::npos) {
            flush();
            cur.t0 = stamp(line, 0);
            cur.t1 = stamp(line, arrow + 3 + (line.size() > arrow + 3 && line[arrow + 3] == ' ' ? 1 : 0));
            inCue = cur.t0 >= 0 && cur.t1 > cur.t0;
            continue;
        }
        if (line.empty()) { flush(); continue; }
        if (!inCue) continue;   // the cue's number
        std::string t;
        for (size_t i = 0; i < line.size(); ++i) {
            unsigned char c = (unsigned char)line[i];
            if (c >= 0x80) {   // UTF-8: curly quotes and dashes to plain ones, the rest dropped
                if (c == 0xE2 && i + 2 < line.size()) {
                    unsigned char c2 = (unsigned char)line[i + 2];
                    if (c2 == 0x98 || c2 == 0x99) t += '\'';
                    else if (c2 == 0x9C || c2 == 0x9D) t += '"';
                    else if (c2 == 0x93 || c2 == 0x94) t += '-';
                    else if (c2 == 0xA6) t += "...";
                    i += 2;
                }
                continue;
            }
            if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 32);
            if (c < 32 || c > 95) {
                if (c == '{' || c == '[') c = '(';
                else if (c == '}' || c == ']') c = ')';
                else continue;
            }
            t += (char)c;
        }
        if (!cur.text.empty()) cur.text += '\n';
        cur.text += t;
    }
    flush();
    return cues;
}

const std::vector<SubtitleCue>* HdPack::subtitles(int id) const {
    auto it = subtitles_.find(id);
    return it == subtitles_.end() ? nullptr : &it->second;
}

const SubtitleCue* HdPack::cueAt(int id, double t) const {
    const auto* cues = subtitles(id);
    if (!cues) return nullptr;
    for (const auto& c : *cues)
        if (t >= c.t0 && t < c.t1) return &c;
    return nullptr;
}

const HdVideoInfo* HdPack::video(const std::string& name) const {
    std::string n = name;
    for (auto& c : n) c = (char)std::toupper((unsigned char)c);
    auto it = videos_.find(n);
    return it == videos_.end() ? nullptr : &it->second;
}

std::shared_ptr<const std::vector<float>> HdPack::speech(int id, double* rate) const {
    auto c = speechCache_.find(id);
    if (c != speechCache_.end()) {
        if (rate) *rate = c->second.second;
        return c->second.first;
    }
    auto f = speechFiles_.find(id);
    if (f == speechFiles_.end()) return nullptr;
    std::vector<uint8_t> data;
    if (!readAll(dir_ + f->second, data)) return nullptr;
    unsigned int channels = 0, sampleRate = 0;
    drflac_uint64 frames = 0;
    float* pcm = drflac_open_memory_and_read_pcm_frames_f32(data.data(), data.size(), &channels, &sampleRate, &frames, nullptr);
    if (!pcm || channels == 0) return nullptr;
    auto out = std::make_shared<std::vector<float>>((size_t)frames);
    for (size_t i = 0; i < (size_t)frames; ++i) (*out)[i] = pcm[i * channels];
    drflac_free(pcm, nullptr);
    speechCache_[id] = {out, (double)sampleRate};
    if (rate) *rate = sampleRate;
    return out;
}
