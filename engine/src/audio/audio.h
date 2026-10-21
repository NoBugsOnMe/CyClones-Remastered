// Sound effects and CD music, played the way CYCLONE.EXE plays them.
//
// Effects are the game's own SOUND\<id>.RAW files (8-bit unsigned mono PCM; the game sets the
// HMI driver to 11,025 Hz and never changes the pitch), mixed here at 44.1 kHz. The music is the
// disc's Red Book audio, streamed straight from the player's own disc image (the tracks a .cue
// sheet lists), one track per mission, looping as the game does.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct SDL_AudioStream;
struct SDL_IOStream;
struct SDL_Mutex;

class Audio {
public:
    static constexpr int kRate = 44100;

    ~Audio();
    // Opens the default playback device (device = false: no device; render() is pulled by the
    // caller, for captures). Without a device the game runs silently.
    bool init(bool device);
    void shutdown();   // before SDL_Quit (which frees the device's streams itself)
    bool hasDevice() const { return stream_ != nullptr; }

    // Where the effects are: the folder holding <id>.RAW (the game installs them in SOUND\).
    // Returns how many were found.
    int setSoundDir(const std::string& dir);
    // Or the effects themselves, by id (the RAW files read from the installer's archive on the
    // disc): bank[id] holds <id>.RAW's bytes, empty where there is none. Returns how many.
    int setSoundBank(std::vector<std::vector<uint8_t>> bank);
    // The disc image (.cue, or a .bin with a .cue beside it): reads the audio tracks.
    // Returns how many audio tracks it found.
    int setDisc(const std::string& path);

    // Volumes, 0-100 (the game's SETTINGS sliders).
    void setVolumes(int sound, int music);

    // A sound as FUN_0004fa92 starts one in its "mode 1": volume 0-255 (the game mostly uses 200),
    // pan -127 (left) to 127 (right).
    void play(int id, int volume = 200, int pan = 0);
    // A sound at a place, "mode 0": dx, dz from the listener in world units, yaw the listener's
    // heading. Volume and pan follow the game's sums (see audio.cpp).
    void playAt(int id, float dx, float dz, float yaw, float range = 640);
    void stopSounds();

    // CD music: an audio track number from the disc (2-30 on the CyClones CD); 0 stops it.
    // restart = false keeps a track that is already playing going.
    void music(int track, bool restart = true);
    int musicTrack() const { return track_; }
    // CD audio the way the video player asks MSCDEX for it: a track (or from startSec into it, for
    // lengthSec; < 0 to its end), played once.
    void cdPlay(int track, float startSec = 0, float lengthSec = -1);
    // A sound from memory (8-bit unsigned mono at 11,025 Hz, as the speech files are), "mode 1".
    // Returns a handle for heardTime (0: not playing).
    uint64_t playRaw(const std::vector<uint8_t>& data, int volume = 200, int pan = 0);
    // How far into that sound the listener is now, in seconds: what the mixer has played less what
    // is still on its way to the speakers (negative before it is heard). kGone once it has ended or
    // been stopped. The cut-scenes keep their pictures to this (the speech is the clock).
    static constexpr double kGone = -1e9;
    double heardTime(uint64_t handle) const;
    // A sound from memory as floats at any rate (the cleaned-up speech of the Enhanced cut-scenes),
    // from startSec into it. A handle as playRaw's.
    uint64_t playPcm(std::shared_ptr<const std::vector<float>> pcm, double rate, int volume = 200, int pan = 0,
                     double startSec = 0);
    void stopVoice(uint64_t handle);

    // Mixes frames of stereo float at kRate (the device callback uses this too).
    void render(float* out, int frames);

private:
    struct Sample {
        std::vector<float> pcm;   // -1..1 at rate
        double rate = 11025;
        std::shared_ptr<const std::vector<float>> shared;   // playPcm: the caller's samples instead
        const std::vector<float>& data() const { return shared ? *shared : pcm; }
    };
    struct Voice {
        std::shared_ptr<const Sample> s;
        double pos = 0;           // in source samples
        float l = 0, r = 0;
        uint64_t serial = 0;
    };
    struct Track {
        int number = 0;
        std::string file;         // the image file holding it
        bool wave = false;        // a .wav file (else raw sectors)
        bool bigEndian = false;
        uint64_t offset = 0;      // bytes into the file
        uint64_t frames = 0;      // stereo sample frames (588 per sector)
    };
    std::shared_ptr<const Sample> sample(int id);
    void startVoice(int id, float gain, float pan01);
    uint64_t startSample(std::shared_ptr<const Sample> s, float gain, float pan01, double startPos = 0);
    void mixMusic(float* out, int frames);
    bool openTrack(const Track& t, uint64_t skipFrames = 0, uint64_t frames = 0);
    void closeTrack();

    SDL_AudioStream* stream_ = nullptr;
    SDL_Mutex* lock_ = nullptr;
    std::string soundDir_;
    std::vector<std::vector<uint8_t>> bank_;
    std::shared_ptr<const Sample> cache_[256];
    bool missing_[256] = {};
    std::vector<Voice> voices_;
    uint64_t serial_ = 0;
    float soundGain_ = 1, musicGain_ = 1;

    std::vector<Track> tracks_;
    int track_ = 0;
    // The playing track: either streamed from the image, or a whole .wav in memory.
    SDL_IOStream* io_ = nullptr;
    std::vector<int16_t> wavePcm_;
    bool waveMode_ = false;
    bool swap_ = false;
    bool loop_ = true;
    uint64_t trackOffset_ = 0, trackFrames_ = 0, trackPos_ = 0;
    std::vector<uint8_t> readBuf_;
    std::vector<float> mixBuf_;
    // When the device last took a mix, and how long that mix's end then had to wait to be heard
    // (the stream's queue plus the device's own buffer).
    uint64_t mixNs_ = 0;
    double mixLatency_ = 0, deviceLatency_ = 0;
    friend void mixedChunk(Audio* a, double queuedSec);
};
