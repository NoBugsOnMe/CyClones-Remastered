#include "cutscene.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

#include "../audio/audio.h"
#include "../data/disc.h"

namespace {
std::string upper(std::string s) {
    for (auto& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}
}  // namespace

void Cutscenes::start(const std::vector<std::vector<std::string>>& playlists, const DiscImage* disc, Audio* audio) {
    disc_ = disc;
    audio_ = audio;
    // The game leaves for the video player (its front end runs SMKPLAY between missions), so its
    // CD music and sounds stop first; a video that wants music starts its own track (op 4). Most
    // between-mission videos have none (30, 31, 34, 35, 37, 38, 39), and up to 0.5.2 they played
    // over the last level's music.
    if (audio_) {
        audio_->music(0);
        audio_->stopSounds();
    }
    lists_ = playlists;
    list_ = -1;
    nextList();
}

std::string Cutscenes::current() const {
    if (!active() || video_ >= (int)lists_[list_].size()) return "";
    return lists_[list_][video_];
}

// A new playlist starts on a black screen (the front end resets the display between them).
void Cutscenes::nextList() {
    ++list_;
    video_ = 0;
    px_.assign(320 * 200, 0);
    std::memset(pal_, 0, sizeof(pal_));
    fade_ = 1;
    block_ = None;
    open_ = false;
    rgba_.assign(320 * 200 * 4, 0);
    for (size_t i = 3; i < rgba_.size(); i += 4) rgba_[i] = 255;
    hdFull_ = hdOverlay_ = false;
    ++serial_;
    subId_ = -1;
    if (active()) openVideo();
}

void Cutscenes::openVideo() {
    open_ = false;
    while (active()) {
        if (video_ >= (int)lists_[list_].size()) { nextList(); return; }
        std::string name = upper(lists_[list_][video_]);
        std::vector<uint8_t> smk, ssd;
        std::string err;
        if (disc_ && disc_->readFile("GAME/" + name + ".SMK", smk) && smk_.open(std::move(smk), err)) {
            ops_.clear();
            if (disc_->readFile("GAME/" + name + ".SSD", ssd))
                for (size_t i = 12; i + 5 <= ssd.size(); i += 5) {
                    Op o;
                    o.op = ssd[i];
                    o.arg = (int)(ssd[i + 1] | ssd[i + 2] << 8 | ssd[i + 3] << 16 | (uint32_t)ssd[i + 4] << 24);
                    ops_.push_back(o);
                    if (o.op == 10) break;
                }
            pc_ = 0;
            speech_ = 0;   // a new video keeps to its own speech, not the last one's tail
            waitFrame_ = 0;
            frame_ = 0;
            frameDur_ = frameWait_ = 0.1f;
            hdOpen(name);
            open_ = true;
            running_ = true;   // the script runs once before the first frame (FUN_00010608(0))
            return;
        }
        SDL_Log("Cut-scene %s: %s", name.c_str(), err.empty() ? "not on the disc" : err.c_str());
        ++video_;
    }
}

// The end of a video: the CD stops (FUN_00011d30), the next one in the playlist follows.
void Cutscenes::endVideo() {
    if (audio_) audio_->music(0);
    open_ = false;
    ++video_;
    openVideo();
}

void Cutscenes::skip() {
    if (!active()) return;
    if (audio_) {
        audio_->music(0);
        audio_->stopSounds();
    }
    nextList();
}

void Cutscenes::stop() {
    if (audio_ && active()) {
        audio_->music(0);
        audio_->stopSounds();
    }
    list_ = (int)lists_.size();
}

// The script (FUN_00010608): 5-byte ops after a 12-byte header, run after each frame from the
// first whose number has come up (op 6) until the next op 6 or the end (op 10).
void Cutscenes::runOps() {
    while (running_ && block_ == None && pc_ < ops_.size()) {
        Op o = ops_[pc_++];
        switch (o.op) {
        case 0: {   // speech or a sound: id, volume, pan
            int id = o.arg & 255, vol = (o.arg >> 8) & 255, pan = (int8_t)((o.arg >> 16) & 255);
            subId_ = id;
            subAt_ = vtime_ - tleft_;
            std::vector<uint8_t> raw;
            double rate = 0;
            auto clean = enhanced_ && pack_ ? pack_->speech(id, &rate) : nullptr;
            if (audio_ && (clean || (disc_ && disc_->readFile("GAME/SOUND/" + std::to_string(id) + ".RAW", raw)))) {
                uint64_t h = clean ? audio_->playPcm(clean, rate, vol, -pan) : audio_->playRaw(raw, vol, -pan);
                if (audio_->hasDevice()) {   // (captures have no device: they keep the plain timing)
                    speech_ = h;
                    speechAt_ = vtime_ - tleft_;
                }
            }
            break;
        }
        case 1:   // hold the picture for n frames
            block_ = Hold;
            blockLeft_ = o.arg * frameDur_;
            break;
        case 2:   // fade to black over n vertical retraces (70 a second); new palettes stay dark
        case 3:   // fade in from black to the palette
            block_ = Fade;
            fadeFrom_ = o.op == 2 ? fade_ : 0;
            fadeTo_ = o.op == 2 ? 0.0f : 1.0f;
            fadeDur_ = blockLeft_ = std::max(1, o.arg) / 70.0f;
            fade_ = fadeFrom_;
            break;
        case 4:   // CD audio: a track, or 200 / 201: track 2 from 1:13 for 35 s / from 1:48 for 48 s
            if (!audio_) break;
            if (o.arg == 200) audio_->cdPlay(2, 73, 35);
            else if (o.arg == 201) audio_->cdPlay(2, 108, 48);
            else audio_->cdPlay(o.arg);
            break;
        case 5:   // stop the CD
            if (audio_) audio_->music(0);
            break;
        case 6:   // wait for frame n
            waitFrame_ = o.arg;
            running_ = false;
            break;
        case 7: { // frames a second (the timer counts milliseconds: 500 / n x 2 per frame)
            int fps = std::max(1, o.arg);
            frameDur_ = (500 / fps) * 2 / 1000.0f;
            if (frame_ == 0) frameWait_ = frameDur_;
            break;
        }
        case 10:  // the end of the script
            waitFrame_ = 1 << 30;
            running_ = false;
            break;
        default: break;
        }
    }
    if (pc_ >= ops_.size()) running_ = false;
}

void Cutscenes::update(float dt) {
    float t = dt;
    // Keep to the speech: where the picture would be after this update, against where the
    // speech heard so far puts it. Small differences (the mix comes in chunks) are let be.
    drift_ = 0;
    static const bool freeRun = SDL_getenv("CYCLONES_AVFREE") != nullptr;   // testing: the old timing
    if (speech_ && audio_ && !freeRun) {
        double heard = audio_->heardTime(speech_);
        if (heard == Audio::kGone) speech_ = 0;
        else {
            double drift = (speechAt_ + heard) - (vtime_ + dt);
            if (std::abs(drift) > 0.03) {
                t = (float)std::clamp(dt + drift, 0.0, dt + 0.5);
                drift_ = (float)drift;
            }
        }
    }
    vtime_ += t;
    // Testing: CYCLONES_AVLOG=1 logs the picture against the speech once a second;
    // CYCLONES_STALL=ms stalls the loop that long every 3 s (a hitch, as a slow disc read makes).
    static const bool avlog = SDL_getenv("CYCLONES_AVLOG") != nullptr;
    static const int stallMs = SDL_getenv("CYCLONES_STALL") ? std::atoi(SDL_getenv("CYCLONES_STALL")) : 0;
    if ((avlog || stallMs) && active()) {
        static double nextLog = 0, nextStall = 3;
        if (avlog && vtime_ >= nextLog) {
            nextLog = vtime_ + 1;
            double heard = speech_ && audio_ ? audio_->heardTime(speech_) : Audio::kGone;
            SDL_Log("dbg av %s frame %d video %.3fs speech %s%.3fs (picture %+.0f ms vs speech) correction %+.0f ms", current().c_str(),
                    frame_, vtime_, speech_ ? "" : "none ", speech_ ? speechAt_ + heard : 0.0,
                    speech_ && heard != Audio::kGone ? (vtime_ - (speechAt_ + heard)) * 1000 : 0.0, drift_ * 1000.0);
        }
        if (stallMs && vtime_ >= nextStall) { nextStall = vtime_ + 3; SDL_Delay(stallMs); }
    }
    for (int guard = 0; guard < 100000 && active(); ++guard) {
        tleft_ = t;
        if (block_ != None) {
            float use = std::min(t, blockLeft_);
            blockLeft_ -= use;
            t -= use;
            if (block_ == Fade) fade_ = fadeTo_ + (fadeFrom_ - fadeTo_) * (fadeDur_ > 0 ? blockLeft_ / fadeDur_ : 0);
            if (blockLeft_ > 0) break;
            if (block_ == Fade) fade_ = fadeTo_;
            block_ = None;
            continue;
        }
        if (!open_) { openVideo(); continue; }
        if (running_) { runOps(); continue; }
        if (frame_ >= smk_.frames()) { endVideo(); continue; }
        if (frameWait_ > t) { frameWait_ -= t; break; }
        t -= frameWait_;
        frameWait_ = frameDur_;
        if (!smk_.next()) { frame_ = smk_.frames(); continue; }
        ++frame_;
        px_ = smk_.pixels();
        std::memcpy(pal_, smk_.palette(), sizeof(pal_));
        hdFrame();
        if (frame_ >= waitFrame_ && pc_ < ops_.size()) running_ = true;
    }
    compose();
}

void Cutscenes::compose() {
    rgba_.resize(320 * 200 * 4);
    uint8_t lut[256][3];
    float f = std::clamp(fade_, 0.0f, 1.0f);
    for (int i = 0; i < 256; ++i)
        for (int c = 0; c < 3; ++c) lut[i][c] = (uint8_t)std::lround(pal_[i * 3 + c] * f);
    size_t n = std::min(px_.size(), (size_t)320 * 200);
    for (size_t i = 0; i < n; ++i) {
        const uint8_t* c = lut[px_[i]];
        rgba_[i * 4] = c[0];
        rgba_[i * 4 + 1] = c[1];
        rgba_[i * 4 + 2] = c[2];
        rgba_[i * 4 + 3] = 255;
    }
}

// ---------------------------------------------------------------------------------------------
// The Enhanced cut-scenes

void Cutscenes::hdOpen(const std::string& name) {
    hd_.close();
    info_ = nullptr;
    hdFull_ = hdOverlay_ = false;
    if (!enhanced_ || !pack_) return;
    info_ = pack_->video(name);
    if (!info_) return;
    std::string err;
    if (!hd_.open(pack_->path(info_->file), err)) {
        SDL_Log("Enhanced cut-scene %s: %s", name.c_str(), err.c_str());
        info_ = nullptr;
    }
}

void Cutscenes::hdFrame() {
    ++serial_;
    hdFull_ = hdOverlay_ = false;
    if (!info_ || !hd_.isOpen()) return;
    int f = frame_ - 1;
    if (!info_->window) {
        hdFull_ = hd_.frame(std::min(f, hd_.frames() - 1), hdRgba_);
        return;
    }
    int pic = f >= 0 && f < (int)info_->picture.size() ? info_->picture[f] : -1;
    if (pic >= 0) hdOverlay_ = hd_.frame(pic, hdRgba_);
}

const std::vector<uint8_t>* Cutscenes::hdPicture(int& w, int& h) const {
    if (!hdFull_) return nullptr;
    w = hd_.width();
    h = hd_.height();
    return &hdRgba_;
}

const std::vector<uint8_t>* Cutscenes::overlay(int& x, int& y, int& w, int& h, int& pw, int& ph) const {
    if (!hdOverlay_ || !info_) return nullptr;
    x = info_->x;
    y = info_->y;
    w = info_->w;
    h = info_->h;
    pw = hd_.width();
    ph = hd_.height();
    return &hdRgba_;
}
