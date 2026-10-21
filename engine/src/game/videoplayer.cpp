#include "videoplayer.h"

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

bool VideoPlayer::open(const std::vector<std::string>& names, const DiscImage* disc, Audio* audio, const HdPack* pack,
                       bool enhanced) {
    close();
    disc_ = disc;
    audio_ = audio;
    pack_ = pack;
    enhanced_ = enhanced && pack && pack->loaded();
    for (const auto& n : names) {
        std::vector<uint8_t> smk;
        if (!disc || !disc->readFile("GAME/" + upper(n) + ".SMK", smk)) continue;
        names_.push_back(upper(n));
        smkFiles_.push_back(std::move(smk));
    }
    if (names_.empty()) return false;
    build();
    open_ = true;
    playing_ = false;
    if (audio_) {
        audio_->music(0);
        audio_->stopSounds();
    }
    seek(0);
    setPlaying(true);
    return true;
}

double VideoPlayer::measure(const std::vector<std::string>& names, const DiscImage* disc) {
    VideoPlayer p;
    p.disc_ = disc;
    for (const auto& n : names) {
        std::vector<uint8_t> smk;
        if (!disc || !disc->readFile("GAME/" + upper(n) + ".SMK", smk)) continue;
        p.names_.push_back(upper(n));
        p.smkFiles_.push_back(std::move(smk));
    }
    p.build();
    return p.length_;
}

void VideoPlayer::close() {
    if (open_) stopSound();
    open_ = playing_ = false;
    names_.clear();
    smkFiles_.clear();
    clips_.clear();
    frames_.clear();
    fades_.clear();
    sounds_.clear();
    smkVideo_ = shownVideo_ = shownFrame_ = hdVideo_ = -1;
    hd_.close();
    info_ = nullptr;
    hdFull_ = hdOverlay_ = false;
    t_ = length_ = 0;
}

// The playlist run through as Cutscenes plays it, without pictures or waiting.
void VideoPlayer::build() {
    double T = 0;
    float fade = 1;
    for (size_t v = 0; v < names_.size(); ++v) {
        struct Op { int op, arg; };
        std::vector<Op> ops;
        std::vector<uint8_t> ssd;
        if (disc_ && disc_->readFile("GAME/" + names_[v] + ".SSD", ssd))
            for (size_t i = 12; i + 5 <= ssd.size(); i += 5) {
                Op o{ssd[i], (int)(ssd[i + 1] | ssd[i + 2] << 8 | ssd[i + 3] << 16 | (uint32_t)ssd[i + 4] << 24)};
                ops.push_back(o);
                if (o.op == 10) break;
            }
        int count = 0;
        {
            Smacker probe;
            std::string err;
            if (probe.open(smkFiles_[v], err)) count = probe.frames();
        }
        Clip clip;
        clip.name = names_[v];
        clip.start = T;
        size_t pc = 0;
        int waitFrame = 0, frame = 0;
        float frameDur = 0.1f, frameWait = 0.1f;
        bool running = true;
        for (int guard = 0; guard < 1000000; ++guard) {
            if (running) {
                while (running && pc < ops.size()) {
                    Op o = ops[pc++];
                    switch (o.op) {
                    case 0: {
                        Sound s;
                        s.t = T;
                        s.kind = 0;
                        s.a = o.arg & 255;
                        s.b = ((o.arg >> 8) & 255) | (((o.arg >> 16) & 255) << 8);
                        sounds_.push_back(s);
                        break;
                    }
                    case 1: T += o.arg * frameDur; break;
                    case 2:
                    case 3: {
                        float from = o.op == 2 ? fade : 0.0f, to = o.op == 2 ? 0.0f : 1.0f;
                        double d = std::max(1, o.arg) / 70.0;
                        fades_.push_back({T, T + d, from, to});
                        T += d;
                        fade = to;
                        break;
                    }
                    case 4: {
                        Sound s;
                        s.t = T;
                        s.kind = 1;
                        s.a = o.arg == 200 || o.arg == 201 ? 2 : o.arg;
                        if (o.arg == 200) { s.start = 73; s.len = 35; }
                        if (o.arg == 201) { s.start = 108; s.len = 48; }
                        sounds_.push_back(s);
                        break;
                    }
                    case 5: { Sound s; s.t = T; s.kind = 2; sounds_.push_back(s); break; }
                    case 6: waitFrame = o.arg; running = false; break;
                    case 7: {
                        int fps = std::max(1, o.arg);
                        frameDur = (500 / fps) * 2 / 1000.0f;
                        if (frame == 0) frameWait = frameDur;
                        break;
                    }
                    case 10: waitFrame = 1 << 30; running = false; break;
                    default: break;
                    }
                }
                if (pc >= ops.size()) running = false;
                continue;
            }
            if (frame >= count) break;
            T += frameWait;
            frameWait = frameDur;
            frames_.push_back({T, (int)v, frame});
            ++frame;
            if (frame >= waitFrame && pc < ops.size()) running = true;
        }
        Sound stop;   // the CD stops at the end of every video (FUN_00011d30)
        stop.t = T;
        stop.kind = 2;
        sounds_.push_back(stop);
        clip.length = T - clip.start;
        clips_.push_back(clip);
    }
    length_ = T;
}

int VideoPlayer::clipAt(double t) const {
    int c = 0;
    for (size_t i = 0; i < clips_.size(); ++i)
        if (t >= clips_[i].start) c = (int)i;
    return c;
}

int VideoPlayer::videoFrames() const {
    if (shownVideo_ < 0) return 0;
    int n = 0;
    for (const auto& f : frames_)
        if (f.video == shownVideo_) ++n;
    return n;
}

bool VideoPlayer::hasEnhanced() const {
    if (!pack_ || !pack_->loaded()) return false;
    for (const auto& n : names_)
        if (pack_->video(n)) return true;
    return false;
}

void VideoPlayer::setEnhanced(bool on) {
    on = on && pack_ && pack_->loaded();
    if (on == enhanced_) return;
    enhanced_ = on;
    hd_.close();
    hdVideo_ = -1;
    int v = shownVideo_, f = shownFrame_;
    shownVideo_ = shownFrame_ = -1;
    if (v >= 0) {
        decodeTo(v, f);
        ++serial_;
    }
    show(t_);
    if (playing_) {   // the speech changes with it
        stopSound();
        sound(t_);
    }
}

void VideoPlayer::setPlaying(bool on) {
    if (!open_) return;
    if (on && t_ >= length_ - 1e-6) t_ = 0;   // play again from the top
    playing_ = on;
    stopSound();
    if (on) sound(t_);
    show(t_);
}

void VideoPlayer::seek(double t) {
    if (!open_) return;
    t_ = std::clamp(t, 0.0, length_);
    stopSound();
    if (playing_) sound(t_);
    show(t_);
}

void VideoPlayer::step(int n) {
    if (!open_ || frames_.empty()) return;
    if (playing_) setPlaying(false);
    // the frame shown now, then n on
    auto it = std::upper_bound(frames_.begin(), frames_.end(), t_, [](double t, const Frame& f) { return t < f.t; });
    int i = (int)(it - frames_.begin()) - 1 + n;
    i = std::clamp(i, 0, (int)frames_.size() - 1);
    seek(frames_[i].t);
}

void VideoPlayer::update(float dt) {
    if (!open_) return;
    if (playing_) {
        t_ += dt;
        // keep to the speech as heard, as Cutscenes does
        if (speech_ && audio_ && audio_->hasDevice() && speechSound_ >= 0) {
            double heard = audio_->heardTime(speech_);
            if (heard == Audio::kGone) speech_ = 0;
            else if (heard >= 0) {
                double want = sounds_[speechSound_].t + heard;
                if (std::abs(want - t_) > 0.03) t_ = want;
            }
        }
        if (t_ >= length_) {
            t_ = length_;
            playing_ = false;
            stopSound();
        } else {
            sound(t_);
        }
    }
    show(t_);
}

void VideoPlayer::stopSound() {
    if (!audio_) return;
    if (speech_) audio_->stopVoice(speech_);
    speech_ = 0;
    speechSound_ = -1;
    if (musicSound_ >= 0) audio_->music(0);
    musicSound_ = -1;
}

// What should be heard at t: the speech started last (if it hasn't ended) and the CD track
// started last (unless a stop came after it), each from as far into it as t is.
void VideoPlayer::sound(double t) {
    if (!audio_) return;
    int sp = -1, mu = -1;
    for (size_t i = 0; i < sounds_.size(); ++i) {
        if (sounds_[i].t > t + 1e-9) break;
        if (sounds_[i].kind == 0) sp = (int)i;
        else mu = sounds_[i].kind == 1 ? (int)i : -1;
    }
    if (sp != speechSound_) {
        if (speech_) audio_->stopVoice(speech_);
        speech_ = 0;
        speechSound_ = sp;
        if (sp >= 0) {
            const Sound& s = sounds_[sp];
            int vol = s.b & 255, pan = (int8_t)((s.b >> 8) & 255);
            double off = t - s.t;
            double rate = 0;
            auto clean = enhanced_ && pack_ ? pack_->speech(s.a, &rate) : nullptr;
            if (clean) speech_ = audio_->playPcm(clean, rate, vol, -pan, off);
            else {
                std::vector<uint8_t> raw;
                if (disc_ && disc_->readFile("GAME/SOUND/" + std::to_string(s.a) + ".RAW", raw)) {
                    auto pcm = std::make_shared<std::vector<float>>(raw.size());
                    for (size_t i = 0; i < raw.size(); ++i) (*pcm)[i] = (raw[i] - 128) / 128.0f;
                    speech_ = audio_->playPcm(pcm, 11025, vol, -pan, off);
                }
            }
        }
    }
    if (mu != musicSound_) {
        musicSound_ = mu;
        if (mu < 0) audio_->music(0);
        else {
            const Sound& s = sounds_[mu];
            double into = t - s.t;
            if (s.len >= 0 && into >= s.len) audio_->music(0);
            else audio_->cdPlay(s.a, (float)(s.start + into), s.len >= 0 ? (float)(s.len - into) : -1.0f);
        }
    }
}

bool VideoPlayer::decodeTo(int video, int frame) {
    if (video < 0 || video >= (int)smkFiles_.size()) return false;
    if (smkVideo_ != video || smk_.frame() > frame + 1) {
        std::string err;
        if (!smk_.open(smkFiles_[video], err)) { smkVideo_ = -1; return false; }
        smkVideo_ = video;
    }
    while (smk_.frame() < frame + 1)
        if (!smk_.next()) return false;
    px_ = smk_.pixels();
    std::memcpy(pal_, smk_.palette(), sizeof(pal_));
    // the Enhanced picture for it
    hdFull_ = hdOverlay_ = false;
    if (enhanced_ && pack_) {
        if (hdVideo_ != video) {
            hd_.close();
            hdVideo_ = video;
            info_ = pack_->video(names_[video]);
            std::string err;
            if (info_ && !hd_.open(pack_->path(info_->file), err)) info_ = nullptr;
        }
        if (info_ && hd_.isOpen()) {
            if (!info_->window) hdFull_ = hd_.frame(std::min(frame, hd_.frames() - 1), hdRgba_);
            else {
                int pic = frame < (int)info_->picture.size() ? info_->picture[frame] : -1;
                if (pic >= 0) hdOverlay_ = hd_.frame(pic, hdRgba_);
            }
        }
    }
    return true;
}

void VideoPlayer::show(double t) {
    // the frame on screen at t (before a playlist's first, black)
    auto it = std::upper_bound(frames_.begin(), frames_.end(), t, [](double tt, const Frame& f) { return tt < f.t; });
    int v = -1, f = -1;
    if (it != frames_.begin()) {
        --it;
        v = it->video;
        f = it->frame;
    }
    if (v != shownVideo_ || f != shownFrame_) {
        if (v < 0) {
            px_.assign(320 * 200, 0);
            std::memset(pal_, 0, sizeof(pal_));
            hdFull_ = hdOverlay_ = false;
        } else {
            decodeTo(v, f);
        }
        shownVideo_ = v;
        shownFrame_ = f;
        ++serial_;
    }
    // the palette's fade at t
    float fade = 1;
    for (const auto& s : fades_) {
        if (t >= s.t1) fade = s.to;
        else if (t >= s.t0) { fade = s.from + (s.to - s.from) * (float)((t - s.t0) / std::max(1e-6, s.t1 - s.t0)); break; }
        else break;
    }
    fade_ = std::clamp(fade, 0.0f, 1.0f);
    rgba_.resize(320 * 200 * 4);
    uint8_t lut[256][3];
    for (int i = 0; i < 256; ++i)
        for (int c = 0; c < 3; ++c) lut[i][c] = (uint8_t)std::lround(pal_[i * 3 + c] * fade_);
    size_t n = std::min(px_.size(), (size_t)320 * 200);
    for (size_t i = 0; i < n; ++i) {
        const uint8_t* c = lut[px_[i]];
        rgba_[i * 4] = c[0];
        rgba_[i * 4 + 1] = c[1];
        rgba_[i * 4 + 2] = c[2];
        rgba_[i * 4 + 3] = 255;
    }
}

bool VideoPlayer::speechAt(int& id, double& t) const {
    const Sound* last = nullptr;
    for (const auto& s : sounds_) {
        if (s.t > t_ + 1e-9) break;
        if (s.kind == 0) last = &s;
    }
    if (!last) return false;
    id = last->a;
    t = t_ - last->t;
    return true;
}

const std::vector<uint8_t>* VideoPlayer::hdPicture(int& w, int& h) const {
    if (!hdFull_) return nullptr;
    w = hd_.width();
    h = hd_.height();
    return &hdRgba_;
}

const std::vector<uint8_t>* VideoPlayer::overlay(int& x, int& y, int& w, int& h, int& pw, int& ph) const {
    if (!hdOverlay_ || !info_) return nullptr;
    x = info_->x;
    y = info_->y;
    w = info_->w;
    h = info_->h;
    pw = hd_.width();
    ph = hd_.height();
    return &hdRgba_;
}
