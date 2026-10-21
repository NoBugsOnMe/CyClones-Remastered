// 2D overlay drawing for the HUD, messages and menus, using the game's own fonts from CYCLONE.HD:
// the small text fonts (c = green, ag = amber, ar = grey-green, gmred = red; ASCII 32-95) and the
// red seven-segment digits. Everything is batched and drawn in one pass at the end of the frame.
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "gl.h"
#include "renderer.h"

class Hud {
public:
    // The game's four fonts, then two made from the green one's letters for lettering stamped
    // into the options screen's metal frame: the letters in the frame's dark grey, and the light
    // edge drawn a pixel below and right of them.
    enum Font { Green, Amber, Grey, Red, Engraved, EngravedEdge, FontCount };
    bool init(GameData& data, std::string& error);
    void begin(int width, int height);
    // Text in pixels from the top-left. scale = screen pixels per font pixel. Returns the width.
    float text(Font f, float x, float y, float scale, const std::string& s, float alpha = 1, bool measureOnly = false);
    float textWidth(Font f, float scale, const std::string& s) { return text(f, 0, 0, scale, s, 1, true); }
    // The same, with the font's colours multiplied by a tint.
    float tinted(Font f, float x, float y, float scale, const std::string& s, float r, float g, float b, float alpha = 1);
    // The game's status graphics (CYCLONE.HD image lumps), drawn with their top-left at x, y.
    // The options screen's art uses its own palette (options_pal): the screen (options, 320x200),
    // the logo (option0), the cursor (autocursor), the disabled RETURN and SAVE buttons (and a LOAD
    // made the same way), the buttons optbut1-9, optno, optyes and their lit versions (hoptbut...).
    enum Image {
        StatsIcon, GunIcon, BioKit, MechKit,
        MenuScreen, MenuLogo, MenuCursor, MenuOffReturn, MenuOffLoad, MenuOffSave,
        MenuButton, MenuNo = MenuButton + 9, MenuYes, MenuLit, MenuBar = MenuLit + 11, MenuSlide,
        MenuThumb,   // a saved game's picture (144 x 90, set with setThumb)
        ImageCount
    };
    void image(Image i, float x, float y, float scale, float alpha = 1);
    // The picture MenuThumb shows: kThumbW x kThumbH RGB (nullptr: black).
    static constexpr int kThumbW = 144, kThumbH = 90;
    void setThumb(const uint8_t* rgb);
    // The first-person weapon art (CYCLONE.HD image lumps: the guns in their 15 aiming poses, the
    // suit's control-stick hand, the gloves and the muzzle flashes), drawn with the lump's own
    // anchor at x, y, at sx by sy screen pixels per art pixel; mirrored round the anchor if asked.
    void sprite(const std::string& lump, float x, float y, float sx, float sy, bool mirror = false, float alpha = 1);
    // The same, its colours multiplied by a tint.
    void tintedSprite(const std::string& lump, float x, float y, float s, float r, float g, float b, float alpha = 1);
    bool hasSprite(const std::string& lump) const { return sprites_.count(lump) != 0; }
    // A tile of the game's weapons cache (its drawing at 0x417e0): meshgrid with a grid of lines
    // every 9 pixels, red for the readied weapon and green for the others, the picture (i_...),
    // the rounds in the green font at 2, 2. 46 x 29 of the game's pixels, s screen pixels each.
    // dim: a weapon not carried. Pictures for the glove: wglove0-2 (an addition).
    void weaponTile(const std::string& pic, float x, float y, float s, bool readied, const std::string& count, float alpha = 1,
                    bool dim = false);
    int imageW(Image i) const { return images_[i].w; }
    int imageH(Image i) const { return images_[i].h; }
    // A rectangle in one of the game's palette colours.
    void palRect(float x, float y, float w, float h, int index, float a = 1);
    float lineHeight(Font f, float scale) const { return (glyphH_[f] + 2) * scale; }
    // Red seven-segment digits (11 px tall). Returns the width.
    float digits(float x, float y, float scale, int value, int minDigits = 1, float alpha = 1);
    float digitsWidth(float scale, int value, int minDigits = 1);
    void rect(float x, float y, float w, float h, float r, float g, float b, float a);
    void end();
    // Text normally gets a one-pixel shadow for the 3D view; the menu's fonts carry their own.
    void setShadow(bool on) { shadow_ = on; }
    // A low-resolution canvas (the menu's 320x200): drawn at k screen pixels per canvas pixel into
    // a texture, then scaled smoothly into the window rectangle x, y, w, h.
    void beginCanvas(int cw, int ch, int k);
    // A picture (RGBA, pw x ph: the cut-scenes) on a black window, scaled into x, y, w, h: by whole
    // pixels first (sharp), then smoothly.
    void picture(const uint8_t* rgba, int pw, int ph, int screenW, int screenH, float x, float y, float w, float h);
    // A full-colour picture (an Enhanced cut-scene's, or its live-action window over the game's),
    // smoothly scaled into x, y, w, h and darkened by bright (the video's fade). slot 0 or 1 keeps
    // its own texture; rgba nullptr draws what the slot already holds. clear: the screen first.
    void photo(int slot, const uint8_t* rgba, int pw, int ph, int screenW, int screenH, float x, float y, float w, float h,
               float bright, bool clear);
    void endCanvas(int screenW, int screenH, float x, float y, float w, float h);
    int width() const { return w_; }
    int height() const { return h_; }

private:
    struct Glyph {
        float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
        int w = 0, h = 0;       // cell size
        int left = 0, adv = 0;  // ink starts at `left`; advance in font pixels
        bool valid = false;
    };
    struct V {
        float x, y, u, v, r, g, b, a;
    };
    void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, float r, float g, float b, float a);

    float drawText(Font f, float x, float y, float scale, const std::string& s, float r, float g, float b, float alpha, bool measureOnly);
    Glyph glyphs_[FontCount][64];
    Glyph digit_[10];
    Glyph images_[ImageCount];
    struct Sprite {
        Glyph g;
        int ax = 0, ay = 0;
    };
    std::unordered_map<std::string, Sprite> sprites_;
    uint8_t pal_[256][3] = {};
    int glyphH_[FontCount] = {};
    float whiteU_ = 0, whiteV_ = 0;
    GLuint prog_ = 0, vao_ = 0, vbo_ = 0, tex_ = 0;
    GLuint fbo_ = 0, fboTex_ = 0, picTex_ = 0;
    GLuint photoTex_[2] = {0, 0};
    int photoW_[2] = {0, 0}, photoH_[2] = {0, 0};
    int picW_ = 0, picH_ = 0;
    int fboW_ = 0, fboH_ = 0, canvasK_ = 1;
    bool shadow_ = true;
    void flush(GLuint texture, int viewW, int viewH, float logicalW, float logicalH);
    std::vector<V> v_;
    int w_ = 0, h_ = 0;
};
