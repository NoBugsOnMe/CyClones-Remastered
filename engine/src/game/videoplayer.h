// The EXTRAS cut-scene player (an addition): any of the game's playlists, with play / pause,
// seeking, frame stepping and the Enhanced pictures on or off.
//
// The game's video player only ever runs forward in real time. To seek, each playlist is first
// run through without pictures, the way Cutscenes plays it (the same .SSD scripts: frame rates,
// holds, palette fades, speech and CD music), giving a timeline: when each frame appears, how dark
// the palette is, when speech and music start and stop. Playing is then a clock along that line;
// any point of it can be shown at once, and the sound is started part-way to match.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../data/hdvideo.h"
#include "../data/smacker.h"

class DiscImage;
class Audio;

class VideoPlayer {
public:
    struct Clip { std::string name; double start = 0, length = 0; };

    bool open(const std::vector<std::string>& names, const DiscImage* disc, Audio* audio, const HdPack* pack, bool enhanced);
    void close();
    bool isOpen() const { return open_; }
    // How long a playlist runs (its timeline, nothing played).
    static double measure(const std::vector<std::string>& names, const DiscImage* disc);
    void update(float dt);
    void setPlaying(bool on);
    bool playing() const { return playing_; }
    void seek(double t);               // the playlist's time, seconds
    void step(int frames);             // pauses; to the next or an earlier frame
    void setEnhanced(bool on);
    bool enhanced() const { return enhanced_; }
    bool hasEnhanced() const;          // the pack has pictures for any of these
    double time() const { return t_; }
    double length() const { return length_; }
    const std::vector<Clip>& clips() const { return clips_; }
    int clipAt(double t) const;
    int frameNumber() const { return shownFrame_ + 1; }   // in its own video, 1-based
    int videoFrames() const;

    // The picture now, as Cutscenes gives it.
    const std::vector<uint8_t>& rgba() const { return rgba_; }
    const std::vector<uint8_t>* hdPicture(int& w, int& h) const;
    const std::vector<uint8_t>* overlay(int& x, int& y, int& w, int& h, int& pw, int& ph) const;
    float fade() const { return fade_; }
    bool speechAt(int& id, double& t) const;   // the speech last started and how far into it
    uint64_t serial() const { return serial_; }

private:
    struct Fade { double t0, t1; float from, to; };
    struct Sound { double t = 0; int kind = 0, a = 0, b = 0; double start = 0, len = -1; };   // 0 speech (id, vol | pan << 8), 1 CD (track), 2 CD stop
    struct Frame { double t; int video, frame; };
    void build();
    void show(double t);                     // picture and fade at t
    void sound(double t);                    // speech and music as they should be heard at t
    void stopSound();
    bool decodeTo(int video, int frame);     // the game's frame (0-based) of that video

    const DiscImage* disc_ = nullptr;
    Audio* audio_ = nullptr;
    const HdPack* pack_ = nullptr;
    bool open_ = false, playing_ = false, enhanced_ = false;
    std::vector<std::string> names_;
    std::vector<std::vector<uint8_t>> smkFiles_;
    std::vector<Clip> clips_;
    std::vector<Frame> frames_;
    std::vector<Fade> fades_;
    std::vector<Sound> sounds_;
    double t_ = 0, length_ = 0;

    // what is decoded
    Smacker smk_;
    int smkVideo_ = -1;
    int shownVideo_ = -1, shownFrame_ = -1;
    std::vector<uint8_t> px_;
    uint8_t pal_[768] = {};
    float fade_ = 1;
    std::vector<uint8_t> rgba_;
    const HdVideoInfo* info_ = nullptr;
    HdStream hd_;
    int hdVideo_ = -1;
    std::vector<uint8_t> hdRgba_;
    bool hdFull_ = false, hdOverlay_ = false;
    uint64_t serial_ = 0;

    // what is heard
    uint64_t speech_ = 0;
    int speechSound_ = -1, musicSound_ = -1;
};
