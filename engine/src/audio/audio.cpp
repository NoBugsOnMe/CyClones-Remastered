#include "audio.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

constexpr int kVoices = 16;
constexpr double kSourceRate = 11025.0;   // the HMI driver's rate (FUN_0004f457: 0x2b11)
// Output levels at 100 %: the effects are full-scale 8-bit samples and several play at once, the
// music is mastered quietly; these keep the two in the balance the game's defaults give
// (KEYMAP.BIN: music 99, sound 70) without clipping.
constexpr float kSoundLevel = 0.8f, kMusicLevel = 0.9f;

std::string upper(std::string s) {
    for (auto& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

std::string dirOf(const std::string& p) {
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? std::string() : p.substr(0, s + 1);
}

bool isFile(const std::string& p) {
    SDL_PathInfo info;
    return SDL_GetPathInfo(p.c_str(), &info) && info.type == SDL_PATHTYPE_FILE;
}

}  // namespace

void mixedChunk(Audio* a, double queuedSec) {
    SDL_LockMutex(a->lock_);
    a->mixNs_ = SDL_GetTicksNS();
    a->mixLatency_ = queuedSec + a->deviceLatency_;
    SDL_UnlockMutex(a->lock_);
}

namespace {
void SDLCALL feed(void* user, SDL_AudioStream* stream, int additional, int total) {
    Audio* a = (Audio*)user;
    static thread_local std::vector<float> buf;
    int frames = additional / (int)(2 * sizeof(float));
    while (frames > 0) {
        int n = std::min(frames, 2048);
        buf.resize((size_t)n * 2);
        a->render(buf.data(), n);
        SDL_PutAudioStreamData(stream, buf.data(), n * 2 * (int)sizeof(float));
        frames -= n;
    }
    mixedChunk(a, SDL_GetAudioStreamQueued(stream) / (double)(2 * sizeof(float)) / Audio::kRate);
}
}  // namespace

Audio::~Audio() { shutdown(); }

void Audio::shutdown() {
    if (stream_) SDL_DestroyAudioStream(stream_);   // also closes its device
    stream_ = nullptr;
    closeTrack();
    voices_.clear();
    if (lock_) SDL_DestroyMutex(lock_);
    lock_ = nullptr;
}

bool Audio::init(bool device) {
    lock_ = SDL_CreateMutex();
    voices_.reserve(kVoices);
    if (!device) return true;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        SDL_Log("Audio: %s (playing without sound)", SDL_GetError());
        return false;
    }
    SDL_AudioSpec spec{SDL_AUDIO_F32, 2, kRate};
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, feed, this);
    if (!stream_) {
        SDL_Log("Audio: no playback device (%s)", SDL_GetError());
        return false;
    }
    SDL_AudioSpec dev;
    int devFrames = 0;
    if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream_), &dev, &devFrames) && dev.freq > 0)
        deviceLatency_ = devFrames / (double)dev.freq;
    SDL_ResumeAudioStreamDevice(stream_);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Effects

int Audio::setSoundBank(std::vector<std::vector<uint8_t>> bank) {
    bank_ = std::move(bank);
    bank_.resize(256);
    soundDir_.clear();
    int found = 0;
    for (int id = 0; id < 256; ++id) {
        cache_[id].reset();
        missing_[id] = false;
        if (!bank_[id].empty()) ++found;
    }
    return found;
}

int Audio::setSoundDir(const std::string& dir) {
    bank_.clear();
    soundDir_ = dir;
    if (!soundDir_.empty() && soundDir_.back() != '/' && soundDir_.back() != '\\') soundDir_ += '/';
    int found = 0;
    for (int id = 0; id < 256; ++id) {
        cache_[id].reset();
        missing_[id] = false;
        std::string n = std::to_string(id);
        if (isFile(soundDir_ + n + ".RAW") || isFile(soundDir_ + n + ".raw")) ++found;
    }
    return found;
}

std::shared_ptr<const Audio::Sample> Audio::sample(int id) {
    if (id <= 0 || id > 255 || (soundDir_.empty() && bank_.empty())) return nullptr;
    if (cache_[id] || missing_[id]) return cache_[id];
    if (!bank_.empty()) {
        const std::vector<uint8_t>& raw = bank_[id];
        if (raw.empty()) {
            missing_[id] = true;
            return nullptr;
        }
        auto s = std::make_shared<Sample>();
        s->pcm.resize(raw.size());
        for (size_t i = 0; i < raw.size(); ++i) s->pcm[i] = (raw[i] - 128) / 128.0f;
        cache_[id] = s;
        return s;
    }
    std::string n = std::to_string(id);
    size_t len = 0;
    void* bytes = SDL_LoadFile((soundDir_ + n + ".RAW").c_str(), &len);
    if (!bytes) bytes = SDL_LoadFile((soundDir_ + n + ".raw").c_str(), &len);
    if (!bytes || !len) {
        if (bytes) SDL_free(bytes);
        missing_[id] = true;
        return nullptr;
    }
    auto s = std::make_shared<Sample>();
    s->pcm.resize(len);
    const uint8_t* b = (const uint8_t*)bytes;
    for (size_t i = 0; i < len; ++i) s->pcm[i] = (b[i] - 128) / 128.0f;
    SDL_free(bytes);
    cache_[id] = s;
    return s;
}

void Audio::setVolumes(int sound, int music) {
    SDL_LockMutex(lock_);
    soundGain_ = std::clamp(sound, 0, 100) / 100.0f * kSoundLevel;
    musicGain_ = std::clamp(music, 0, 100) / 100.0f * kMusicLevel;
    SDL_UnlockMutex(lock_);
}

// The HMI driver's pan runs 0 (left) to 0xffff (right) with 0x8000 in the middle; full volume on
// both sides at the middle, the far side fading out toward either end.
void Audio::startVoice(int id, float gain, float pan01) {
    if (gain <= 0) return;
    startSample(sample(id), gain, pan01);
}

uint64_t Audio::playRaw(const std::vector<uint8_t>& data, int volume, int pan) {
    if (data.empty()) return 0;
    auto s = std::make_shared<Sample>();
    s->pcm.resize(data.size());
    for (size_t i = 0; i < data.size(); ++i) s->pcm[i] = (data[i] - 128) / 128.0f;
    float p = pan == 0 ? 0.5f : (std::clamp(pan, -127, 127) + 127) / 254.0f;
    return startSample(s, std::clamp(volume, 0, 255) / 255.0f, p);
}

double Audio::heardTime(uint64_t handle) const {
    if (!handle) return kGone;
    SDL_LockMutex(lock_);
    double t = kGone;
    for (const auto& v : voices_)
        if (v.serial == handle) {
            // the mix's position, less what was still queued when it was taken, plus the time since
            double since = mixNs_ ? std::min(0.1, (SDL_GetTicksNS() - mixNs_) / 1e9) : 0.0;
            t = v.pos / v.s->rate - mixLatency_ + since;
            break;
        }
    SDL_UnlockMutex(lock_);
    return t;
}

uint64_t Audio::playPcm(std::shared_ptr<const std::vector<float>> pcm, double rate, int volume, int pan, double startSec) {
    if (!pcm || pcm->empty() || rate <= 0) return 0;
    auto s = std::make_shared<Sample>();
    s->shared = std::move(pcm);
    s->rate = rate;
    float p = pan == 0 ? 0.5f : (std::clamp(pan, -127, 127) + 127) / 254.0f;
    return startSample(s, std::clamp(volume, 0, 255) / 255.0f, p, std::max(0.0, startSec) * rate);
}

void Audio::stopVoice(uint64_t handle) {
    if (!handle) return;
    SDL_LockMutex(lock_);
    for (size_t i = 0; i < voices_.size(); ++i)
        if (voices_[i].serial == handle) { voices_.erase(voices_.begin() + (long)i); break; }
    SDL_UnlockMutex(lock_);
}

uint64_t Audio::startSample(std::shared_ptr<const Sample> s, float gain, float pan01, double startPos) {
    if (gain <= 0 || !s || s->data().empty() || startPos >= (double)s->data().size()) return 0;
    Voice v;
    v.s = s;
    v.pos = startPos;
    v.l = gain * std::min(1.0f, 2 * (1 - pan01));
    v.r = gain * std::min(1.0f, 2 * pan01);
    SDL_LockMutex(lock_);
    v.serial = ++serial_;
    if ((int)voices_.size() < kVoices) voices_.push_back(v);
    else {
        auto oldest = std::min_element(voices_.begin(), voices_.end(), [](const Voice& a, const Voice& b) { return a.serial < b.serial; });
        *oldest = v;
    }
    SDL_UnlockMutex(lock_);
    return v.serial;
}

// Mode 1 (FUN_0004fa92): volume = the given 0-255, pan from -127..127 (0 is the middle).
void Audio::play(int id, int volume, int pan) {
    float p = pan == 0 ? 0.5f : (std::clamp(pan, -127, 127) + 127) / 254.0f;
    startVoice(id, std::clamp(volume, 0, 255) / 255.0f, p);
}

// Mode 0 (FUN_0004fa92): the offset is turned into the player's view; the distance is taken as
// |a| + |b| + min(|a|, |b|) / 2 (so diagonals carry less far than straight ahead); the volume
// falls from 255 to nothing over 640 units of that (0x72a90 = 10, shifted left 6); the pan is
// the sideways part over that distance. `range` stretches the fade for a few loud sources (an
// addition: World::creatureSound).
void Audio::playAt(int id, float dx, float dz, float yaw, float range) {
    float fwd = dx * std::sin(yaw) - dz * std::cos(yaw);
    float side = dx * std::cos(yaw) + dz * std::sin(yaw);   // + is to the right
    int a = std::abs((int)fwd), b = std::abs((int)side);
    int dist = a + b + std::min(a, b) / 2;
    if (dist == 0) { startVoice(id, 1.0f, 0.5f); return; }
    int v = std::clamp((int)(dist * 255 / std::max(1.0f, range)), 0, 255);
    float pan = ((int)side * 127 / dist + 127) / 254.0f;
    startVoice(id, (255 - v) / 255.0f, pan);
}

void Audio::stopSounds() {
    SDL_LockMutex(lock_);
    voices_.clear();
    SDL_UnlockMutex(lock_);
}

// ---------------------------------------------------------------------------------------------
// The disc's audio tracks

namespace {
struct CueTrack {
    int number = 0, file = -1;
    int sector = 2352;
    bool audio = false;
    long index0 = -1, index1 = -1;   // frames (1/75 s) into the file
};
long msf(const std::string& s) {
    int m = 0, sec = 0, f = 0;
    if (std::sscanf(s.c_str(), "%d:%d:%d", &m, &sec, &f) != 3) return -1;
    return ((long)m * 60 + sec) * 75 + f;
}
}  // namespace

// The CyClones CD's own layout (213,902 raw sectors): where each audio track's 2-second pause
// starts, found from the runs of digital silence in the image (tools/make_cue.py), for a .bin
// that has no .cue beside it.
static std::string cyclonesCue(const std::string& binName) {
    static const long kPregaps[] = {45142, 55953, 63371, 70707, 78327, 85363, 92698, 100290, 107889, 115480,
                                    122560, 129588, 136715, 143892, 151167, 158539, 165911, 173505, 181306,
                                    182268, 183293, 183895, 184479, 185384, 186751, 194155, 201491, 209128, 210849};
    auto msfText = [](long l) {
        char b[48];
        std::snprintf(b, sizeof(b), "%02ld:%02ld:%02ld", l / 4500, (l / 75) % 60, l % 75);
        return std::string(b);
    };
    std::string cue = "FILE \"" + binName + "\" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n";
    int n = 2;
    for (long p : kPregaps) {
        char t[16];
        std::snprintf(t, sizeof(t), "%02d", n++);
        cue += std::string("  TRACK ") + t + " AUDIO\n    INDEX 00 " + msfText(p) + "\n    INDEX 01 " + msfText(p + 150) + "\n";
    }
    return cue;
}

int Audio::setDisc(const std::string& pathIn) {
    tracks_.clear();
    std::string path = pathIn, cue;
    if (upper(path).size() > 4 && upper(path).substr(path.size() - 4) != ".CUE") {
        std::string base = path.substr(0, path.find_last_of('.'));
        if (isFile(base + ".cue")) path = base + ".cue";
        else if (isFile(base + ".CUE")) path = base + ".CUE";
        else {
            // No sheet: a raw image of the CyClones CD itself still has its track layout here.
            SDL_PathInfo info;
            if (!SDL_GetPathInfo(path.c_str(), &info) || info.size != (Uint64)213902 * 2352) return 0;   // an .iso: no music
            size_t slash = path.find_last_of("/\\");
            cue = cyclonesCue(slash == std::string::npos ? path : path.substr(slash + 1));
        }
    }
    if (cue.empty()) {
        size_t len = 0;
        char* txt = (char*)SDL_LoadFile(path.c_str(), &len);
        if (!txt) return 0;
        cue.assign(txt, len);
        SDL_free(txt);
    }

    struct File { std::string name, type; };
    std::vector<File> files;
    std::vector<CueTrack> cts;
    size_t p = 0;
    while (p < cue.size()) {
        size_t e = cue.find('\n', p);
        std::string line = cue.substr(p, e == std::string::npos ? std::string::npos : e - p);
        p = e == std::string::npos ? cue.size() : e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        size_t s = line.find_first_not_of(" \t");
        if (s == std::string::npos) continue;
        line = line.substr(s);
        std::string U = upper(line);
        if (U.rfind("FILE ", 0) == 0) {
            File f;
            std::string rest = line.substr(5);
            if (!rest.empty() && rest[0] == '"') {
                size_t q = rest.find('"', 1);
                f.name = rest.substr(1, q == std::string::npos ? std::string::npos : q - 1);
                f.type = q == std::string::npos ? "" : upper(rest.substr(q + 1));
            } else {
                size_t sp = rest.find(' ');
                f.name = rest.substr(0, sp);
                f.type = sp == std::string::npos ? "" : upper(rest.substr(sp + 1));
            }
            f.type.erase(0, f.type.find_first_not_of(' '));
            bool absolute = (f.name.size() > 1 && f.name[1] == ':') || (!f.name.empty() && (f.name[0] == '/' || f.name[0] == '\\'));
            if (!absolute) f.name = dirOf(path) + f.name;
            files.push_back(f);
        } else if (U.rfind("TRACK ", 0) == 0 && !files.empty()) {
            CueTrack t;
            char type[32] = {};
            std::sscanf(U.c_str() + 6, "%d %31s", &t.number, type);
            t.file = (int)files.size() - 1;
            std::string ty = type;
            t.audio = ty == "AUDIO";
            if (ty.find("/2048") != std::string::npos) t.sector = 2048;
            else if (ty.find("/2336") != std::string::npos) t.sector = 2336;
            cts.push_back(t);
        } else if (U.rfind("INDEX ", 0) == 0 && !cts.empty()) {
            int idx = -1;
            char when[32] = {};
            std::sscanf(U.c_str() + 6, "%d %31s", &idx, when);
            if (idx == 0) cts.back().index0 = msf(when);
            else if (idx == 1) cts.back().index1 = msf(when);
        }
    }
    // Byte positions: in a file, each track's sectors (from its index 00, or 01 without one)
    // follow the previous track's, each at its own sector size.
    std::vector<uint64_t> regionByte(cts.size(), 0);
    for (size_t i = 0; i < cts.size(); ++i) {
        long start = cts[i].index0 >= 0 ? cts[i].index0 : cts[i].index1;
        if (i == 0 || cts[i - 1].file != cts[i].file) regionByte[i] = (uint64_t)std::max(0L, start) * cts[i].sector;
        else {
            long prev = cts[i - 1].index0 >= 0 ? cts[i - 1].index0 : cts[i - 1].index1;
            regionByte[i] = regionByte[i - 1] + (uint64_t)std::max(0L, start - prev) * cts[i - 1].sector;
        }
    }
    for (size_t i = 0; i < cts.size(); ++i) {
        const CueTrack& t = cts[i];
        if (!t.audio || t.index1 < 0) continue;
        const File& f = files[t.file];
        Track tr;
        tr.number = t.number;
        tr.file = f.name;
        tr.wave = f.type.rfind("WAVE", 0) == 0;
        tr.bigEndian = f.type.rfind("MOTOROLA", 0) == 0;
        if (!tr.wave && f.type.rfind("BINARY", 0) != 0 && !tr.bigEndian) continue;   // MP3 and the like
        long start = t.index0 >= 0 ? t.index0 : t.index1;
        uint64_t bytes = regionByte[i] + (uint64_t)std::max(0L, t.index1 - start) * t.sector;
        if (tr.wave) bytes = (uint64_t)t.index1 * 588;   // in sample frames, for a .wav
        tr.offset = bytes;
        // Length: to the next track's index 01 in the same file, as the disc's table of contents
        // counts it (the next track's 2-second pause included), or to the end of the file.
        long next = -1;
        if (i + 1 < cts.size() && cts[i + 1].file == t.file) next = cts[i + 1].index1;
        if (next > t.index1) tr.frames = (uint64_t)(next - t.index1) * 588;
        else if (!tr.wave) {
            SDL_PathInfo info;
            if (SDL_GetPathInfo(f.name.c_str(), &info) && info.size > tr.offset) tr.frames = (info.size - tr.offset) / 4;
        } else tr.frames = 0;   // to the end of the .wav (found when it is loaded)
        tracks_.push_back(tr);
    }
    return (int)tracks_.size();
}

void Audio::closeTrack() {
    if (io_) SDL_CloseIO(io_);
    io_ = nullptr;
    wavePcm_.clear();
    waveMode_ = false;
}

bool Audio::openTrack(const Track& t, uint64_t skipFrames, uint64_t want) {
    if (t.wave) {
        SDL_AudioSpec spec;
        Uint8* buf = nullptr;
        Uint32 len = 0;
        if (!SDL_LoadWAV(t.file.c_str(), &spec, &buf, &len)) return false;
        SDL_AudioSpec dst{SDL_AUDIO_S16, 2, kRate};
        Uint8* out = nullptr;
        int outLen = 0;
        bool ok = SDL_ConvertAudioSamples(&spec, buf, (int)len, &dst, &out, &outLen);
        SDL_free(buf);
        if (!ok) return false;
        std::vector<int16_t> pcm((size_t)outLen / 2);
        std::memcpy(pcm.data(), out, pcm.size() * 2);
        SDL_free(out);
        uint64_t total = pcm.size() / 2;
        uint64_t off = std::min<uint64_t>(t.offset + skipFrames, total);
        uint64_t frames = t.frames ? std::min<uint64_t>(t.frames > skipFrames ? t.frames - skipFrames : 0, total - off) : total - off;
        if (want) frames = std::min(frames, want);
        SDL_LockMutex(lock_);
        closeTrack();
        wavePcm_.swap(pcm);
        waveMode_ = true;
        trackOffset_ = off;
        trackFrames_ = frames;
        trackPos_ = 0;
        SDL_UnlockMutex(lock_);
        return frames > 0;
    }
    SDL_IOStream* io = SDL_IOFromFile(t.file.c_str(), "rb");
    if (!io) return false;
    // Raw sectors are little-endian ("BINARY") unless the sheet says "MOTOROLA"; some rips have
    // the bytes the other way round anyway, which a look at the music tells (real audio changes
    // smoothly from one sample to the next; swapped bytes are noise).
    bool swap = t.bigEndian;
    {
        std::vector<uint8_t> probe(2352 * 32);
        uint64_t at = t.offset + std::min<uint64_t>(t.frames / 2, 75 * 20) * 4;
        at -= at % 4;
        if (SDL_SeekIO(io, (Sint64)at, SDL_IO_SEEK_SET) >= 0) {
            size_t got = SDL_ReadIO(io, probe.data(), probe.size()) / 4 * 4;
            double dl = 0, db = 0;
            for (size_t k = 4; k + 3 < got; k += 4) {
                int16_t l0 = (int16_t)(probe[k - 4] | probe[k - 3] << 8), l1 = (int16_t)(probe[k] | probe[k + 1] << 8);
                int16_t b0 = (int16_t)(probe[k - 4] << 8 | probe[k - 3]), b1 = (int16_t)(probe[k] << 8 | probe[k + 1]);
                dl += std::abs((double)l1 - l0);
                db += std::abs((double)b1 - b0);
            }
            if (dl > 0 && db > 0) swap = db < dl * 0.5 ? true : dl < db * 0.5 ? false : swap;
        }
    }
    // A stretch asked for by time may run on past the track's end into the next, as on the disc.
    uint64_t start = t.offset + skipFrames * 4;
    uint64_t frames = want ? want : (t.frames > skipFrames ? t.frames - skipFrames : 0);
    if (SDL_SeekIO(io, (Sint64)start, SDL_IO_SEEK_SET) < 0) { SDL_CloseIO(io); return false; }
    SDL_LockMutex(lock_);
    closeTrack();
    io_ = io;
    swap_ = swap;
    trackOffset_ = start;
    trackFrames_ = frames;
    trackPos_ = 0;
    SDL_UnlockMutex(lock_);
    return frames > 0;
}

void Audio::music(int track, bool restart) {
    if (track == track_ && !restart && (io_ || waveMode_)) return;
    if (SDL_getenv("CYCLONES_SOUNDLOG")) SDL_Log("dbg music track %d", track);   // testing
    if (track <= 0) {
        SDL_LockMutex(lock_);
        closeTrack();
        track_ = 0;
        SDL_UnlockMutex(lock_);
        return;
    }
    for (const auto& t : tracks_)
        if (t.number == track) {
            if (openTrack(t)) {
                SDL_LockMutex(lock_);
                track_ = track;
                loop_ = true;
                SDL_UnlockMutex(lock_);
            } else {
                SDL_Log("Audio: can't play track %d from %s", track, t.file.c_str());
                music(0);
            }
            return;
        }
    music(0);
}

void Audio::cdPlay(int track, float startSec, float lengthSec) {
    for (const auto& t : tracks_)
        if (t.number == track) {
            uint64_t skip = (uint64_t)std::max(0.0f, startSec * 75) * 588;
            uint64_t want = lengthSec >= 0 ? (uint64_t)(lengthSec * 75) * 588 : 0;
            if (openTrack(t, skip, want)) {
                SDL_LockMutex(lock_);
                track_ = track;
                loop_ = false;
                SDL_UnlockMutex(lock_);
            } else {
                music(0);
            }
            return;
        }
    music(0);
}

// Called with the lock held.
void Audio::mixMusic(float* out, int frames) {
    if (!io_ && !waveMode_) return;
    if (!trackFrames_) return;
    float g = musicGain_ / 32768.0f;
    int done = 0;
    while (done < frames) {
        if (trackPos_ >= trackFrames_) {   // the end: from the top again, as the game does
            if (!loop_) { closeTrack(); track_ = 0; return; }
            trackPos_ = 0;
            if (io_ && SDL_SeekIO(io_, (Sint64)trackOffset_, SDL_IO_SEEK_SET) < 0) { closeTrack(); return; }
        }
        int n = (int)std::min<uint64_t>((uint64_t)(frames - done), trackFrames_ - trackPos_);
        if (waveMode_) {
            const int16_t* s = wavePcm_.data() + (trackOffset_ + trackPos_) * 2;
            for (int i = 0; i < n * 2; ++i) out[done * 2 + i] += s[i] * g;
        } else {
            readBuf_.resize((size_t)n * 4);
            size_t got = SDL_ReadIO(io_, readBuf_.data(), readBuf_.size()) / 4;
            if (got == 0) { trackPos_ = trackFrames_; continue; }   // short file: loop now
            n = (int)got;
            const uint8_t* b = readBuf_.data();
            for (int i = 0; i < n * 2; ++i) {
                int16_t v = swap_ ? (int16_t)(b[2 * i] << 8 | b[2 * i + 1]) : (int16_t)(b[2 * i] | b[2 * i + 1] << 8);
                out[done * 2 + i] += v * g;
            }
        }
        trackPos_ += (uint64_t)n;
        done += n;
    }
}

// ---------------------------------------------------------------------------------------------
// The mix

void Audio::render(float* out, int frames) {
    std::memset(out, 0, sizeof(float) * 2 * (size_t)frames);
    SDL_LockMutex(lock_);
    for (size_t vi = 0; vi < voices_.size();) {
        Voice& v = voices_[vi];
        const double step = v.s->rate / kRate;
        const std::vector<float>& pcm = v.s->data();
        size_t n = pcm.size();
        float l = v.l * soundGain_, r = v.r * soundGain_;
        int i = 0;
        for (; i < frames; ++i) {
            size_t k = (size_t)v.pos;
            if (k >= n) break;
            float frac = (float)(v.pos - (double)k);
            float a = pcm[k], b = k + 1 < n ? pcm[k + 1] : 0.0f;
            float s = a + (b - a) * frac;
            out[2 * i] += s * l;
            out[2 * i + 1] += s * r;
            v.pos += step;
        }
        if ((size_t)v.pos >= n) voices_.erase(voices_.begin() + vi);
        else ++vi;
    }
    mixMusic(out, frames);
    SDL_UnlockMutex(lock_);
    // Many loud effects at once: a gentle limiter rather than hard clipping.
    for (int i = 0; i < frames * 2; ++i) {
        float x = out[i];
        float ax = std::abs(x);
        if (ax > 0.8f) out[i] = std::copysign(0.8f + 0.2f * std::tanh((ax - 0.8f) / 0.2f), x);
    }
}
