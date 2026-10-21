// The game's cut-scenes, played as its video player SMKPLAY.EXE plays them: each Smacker video
// runs to the script in its .SSD file (frame rate, palette fades, holds, CD music, speech from
// GAME\SOUND\2xx.RAW), and a key skips the rest of the playlist it belongs to. CYCLONES.EXE, the
// game's front end, starts with two playlists ("SMKPLAY I": raven, int1, int2, int3; then
// "SMKPLAY o": 1a2a, 1a2b, 1a3, the first briefing) before the first mission.
#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "../data/hdvideo.h"
#include "../data/smacker.h"

class DiscImage;
class Audio;

class Cutscenes {
public:
    void start(const std::vector<std::vector<std::string>>& playlists, const DiscImage* disc, Audio* audio);
    bool active() const { return list_ < (int)lists_.size(); }
    void update(float dt);
    void skip();     // a key: the rest of this playlist
    void stop();     // all of them
    // The picture: 320 x 200 RGBA with the palette fade applied.
    const std::vector<uint8_t>& rgba() const { return rgba_; }
    int width() const { return 320; }
    int height() const { return 200; }
    std::string current() const;   // the video playing (for logs and captures)
    // The Enhanced cut-scenes (an addition): pictures and speech from the HD pack in place of the
    // game's, on the game's own timing. Off, or without the pack, the game's videos play as ever.
    void setEnhanced(const HdPack* pack, bool on) { pack_ = pack; enhanced_ = on; }
    // The whole picture anew (the CGI upscaled), or nullptr: the game's picture, maybe with the
    // live-action window drawn over it (overlay). serial changes with every new picture.
    const std::vector<uint8_t>* hdPicture(int& w, int& h) const;
    const std::vector<uint8_t>* overlay(int& x, int& y, int& w, int& h, int& pw, int& ph) const;
    float fade() const { return std::clamp(fade_, 0.0f, 1.0f); }
    uint64_t serial() const { return serial_; }
    // The speech last started and how far into it the picture is (for its subtitles).
    bool speechAt(int& id, double& t) const {
        if (subId_ < 0) return false;
        id = subId_;
        t = vtime_ - subAt_;
        return true;
    }

private:
    struct Op { int op = 0, arg = 0; };
    void openVideo();
    void endVideo();
    void nextList();
    void runOps();
    void compose();

    const DiscImage* disc_ = nullptr;
    Audio* audio_ = nullptr;
    std::vector<std::vector<std::string>> lists_;
    int list_ = 0, video_ = 0;
    Smacker smk_;
    bool open_ = false;
    std::vector<Op> ops_;
    size_t pc_ = 0;
    bool running_ = false;      // the script is between ops (resumes after a hold or fade)
    int waitFrame_ = 0;         // ops wait for this frame (op 6)
    int frame_ = 0;             // frames shown
    float frameDur_ = 0.1f, frameWait_ = 0.1f;
    enum Block { None, Fade, Hold } block_ = None;
    float blockLeft_ = 0, fadeFrom_ = 1, fadeTo_ = 1, fadeDur_ = 1;
    float fade_ = 1;            // 0 black .. 1 the palette
    std::vector<uint8_t> px_;   // 320 x 200 palette indices
    uint8_t pal_[768] = {};
    std::vector<uint8_t> rgba_;
    // The speech is the clock: once a video's speech starts (at timeline point speechAt_), the
    // picture keeps to the part of it being heard, so a stall in the game's loop or the sound
    // device's delay can't put the lips out of step for the rest of the video.
    double vtime_ = 0, speechAt_ = 0;   // the video timeline (holds and fades included), seconds
    float tleft_ = 0;                   // what this update has still to spend (where runOps is)
    uint64_t speech_ = 0;               // the speech's handle (0: none)
    float drift_ = 0;                   // testing: the last correction
    // Enhanced
    void hdOpen(const std::string& name);
    void hdFrame();                     // the HD picture for the game's frame just shown
    const HdPack* pack_ = nullptr;
    bool enhanced_ = false;
    const HdVideoInfo* info_ = nullptr;
    HdStream hd_;
    std::vector<uint8_t> hdRgba_;
    bool hdFull_ = false, hdOverlay_ = false;
    uint64_t serial_ = 0;
    int subId_ = -1;
    double subAt_ = 0;
};
