// The Enhanced cut-scenes (an addition): a folder of new pictures and sound made from the game's
// own videos, shown in their place when present.
//
//   index.txt   one line per video or speech file:
//                 video NAME full FILE                  every frame anew
//                 video NAME window X Y W H FILE RUNS   the live-action window at X,Y W x H of the
//                                                       game's frames: RUNS ("B170 .208 ...") say
//                                                       which frames show it (B) and which don't;
//                                                       FILE holds one picture per B frame
//                 speech ID FILE                        GAME\SOUND\ID.RAW cleaned up (FLAC)
//                 subtitles ID FILE                     what is said in GAME\SOUND\ID.RAW (.srt, its
//                                                       times from the start of the speech)
//   *.ivf       AV1 in IVF, one picture per frame of the game's video, decoded with dav1d
//
// Everything else (timing, fades, holds, music) still comes from the game's own files.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct Dav1dContext;

// One AV1 stream: its pictures by number, as RGBA.
class HdStream {
public:
    ~HdStream();
    bool open(const std::string& path, std::string& error);
    void close();
    bool isOpen() const { return ctx_ != nullptr; }
    int frames() const { return (int)tu_.size(); }
    int width() const { return w_; }
    int height() const { return h_; }
    // Picture n (0-based) as w x h RGBA. The next picture is quick; any other is found from the
    // key frame before it.
    bool frame(int n, std::vector<uint8_t>& rgba);

private:
    struct Unit { size_t off = 0; uint32_t size = 0; bool key = false; };
    bool decodeNext(std::vector<uint8_t>* rgba);
    std::vector<uint8_t> file_;
    std::vector<Unit> tu_;
    Dav1dContext* ctx_ = nullptr;
    int w_ = 0, h_ = 0;
    int fed_ = 0;      // temporal units sent to the decoder
    int next_ = 0;     // the picture the decoder gives next
    size_t pendingOff_ = 0, pendingLeft_ = 0;
};

struct SubtitleCue {
    double t0 = 0, t1 = 0;      // seconds into the speech
    std::string text;           // in capitals, the game's font's characters, '\n' between lines
};

struct HdVideoInfo {
    std::string name, file;
    bool window = false;
    int x = 0, y = 0, w = 0, h = 0;
    std::vector<int> picture;   // window videos: per game frame (0-based), the stream's picture or -1
};

class HdPack {
public:
    bool open(const std::string& dir);
    bool loaded() const { return !videos_.empty(); }
    const HdVideoInfo* video(const std::string& name) const;   // by the game's name, any case
    std::string path(const std::string& file) const { return dir_ + file; }
    // The cleaned-up speech for GAME\SOUND\<id>.RAW: mono floats at *rate (cached).
    std::shared_ptr<const std::vector<float>> speech(int id, double* rate) const;
    // The subtitles for GAME\SOUND\<id>.RAW (nullptr: none), and the cue at t seconds into it.
    const std::vector<SubtitleCue>* subtitles(int id) const;
    const SubtitleCue* cueAt(int id, double t) const;
    bool hasSubtitles() const { return !subtitles_.empty(); }
    const std::string& dir() const { return dir_; }

private:
    static std::vector<SubtitleCue> parseSrt(const std::string& text);
    std::string dir_;
    std::map<std::string, HdVideoInfo> videos_;
    std::map<int, std::string> speechFiles_;
    std::map<int, std::vector<SubtitleCue>> subtitles_;
    mutable std::map<int, std::pair<std::shared_ptr<const std::vector<float>>, double>> speechCache_;
};
