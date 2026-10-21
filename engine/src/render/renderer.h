// OpenGL 3.3 renderer for CyClones levels. Textures stay 8-bit palette indices on the GPU;
// the fragment shader applies the game's own 127-step fade table and palette, so colours and
// shading match the 1994 software renderer while geometry renders at any resolution.
//
// Preview 2 adds: instanced voxel models (monsters, items, objects) that glide between animation
// frames, tumbling debris voxels, soft additive/alpha particles, sliding doors and dynamic point
// lights that brighten surfaces through the same fade table.
#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../data/bol.h"
#include "../data/level.h"
#include "gl.h"
#include "voxel.h"

struct GameData {
    Bol hd, cd;
    std::vector<uint8_t> palette;  // 768 bytes, 6-bit VGA
    std::vector<uint8_t> fade;     // 127 x 256 shade table ('fadetob')
    int startWalls = -1, startFlats = -1;
    const uint8_t* sky = nullptr;  // 320 x 160: sky1, the left half of the sky panorama
    const uint8_t* sky2 = nullptr; // 320 x 160: sky2, the right half
};

struct Camera {
    float x = 0, y = 0, z = 0;  // world: x = east, y = up, z = south
    float yaw = 0, pitch = 0;   // radians; yaw 0 looks north (-z)
};

struct RenderSettings {
    float hFovDeg4x3 = 90.0f;   // horizontal FOV at 4:3 (the game's: 90); wider screens get more (Hor+)
    bool tallPixels = true;     // the game's 320x200 picture filled a 4:3 screen, so its pixels (and
                                // everything drawn with them) were 1.2 times as tall as wide
    bool lighting = true;       // authentic fade-table shading vs full bright
    float ambient = 5.0f;       // global ambient shade (original default 5)
    float depthScale = 1.0f;    // 1.0 = authentic distance darkening; lower = see further
    float brightness = 0.0f;    // extra fade rows (negative = brighter)
    int renderScale = 100;      // % of window resolution (25..100), or -1 for 320x200 "retro"
    bool showSky = true;
};

struct ModelRange {
    int first = 0, count = 0;
};

struct ModelDraw {
    ModelRange range;
    float pos[3] = {0, 0, 0};
    float rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};  // column-major: model x, y, z axes in world space
    float scale = 1;
    float t = 0;        // morph position 0..1 along the range
    float light = 0;    // signed cell light (same units as the level's light bytes)
    float flash = 0;    // 0..1 brightening (hit feedback)
};

// Original sprite pixels drawn as small squares in 3D ("splats"). Depth sprites rotate with the
// model; billboards face the camera like the 1994 sprites did.
struct SplatDraw {
    ModelRange range;
    float pos[3] = {0, 0, 0};
    float rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};  // depth sprites: model axes in world space
    float scale = 1;
    float t = 0;            // morph position 0..1
    float light = 0;
    float flash = 0;
    float alpha = 1;
    float roll = 0;         // billboards: rotation in the screen plane (radians)
    float rollY = 0;        // height (sprite units) the roll pivots around
    float keepLo = 0, keepHi = 1;  // dissolve: draw only pixels whose hash falls in [lo, hi)
    float growX = 0;        // depth sprites: extra width of each pixel square (closes cracks at an angle)
    uint8_t mode = 0;       // 0 opaque, 1 alpha blended, 2 additive, 3 translucent (the TINT_MAP: 38 %, front surface once)
    uint8_t facing = 0;     // 0 depth sprite, 1 upright billboard, 2 camera-facing billboard
    bool fullbright = false;
};

struct DebrisDraw {     // a free voxel: position, edge length, tumble angle, palette index, light, seed
    float x, y, z, size;
    float angle, color, light, seed;
};

struct ParticleDraw {   // camera-facing soft particle; stretch > 0 smears it along its velocity
    float x, y, z, size;
    float r, g, b, a;
    float vx, vy, vz, stretch;
};

struct LightDraw {
    float x, y, z, radius;
    float r, g, b, intensity;   // intensity in shade units (about 10 per fade row)
};

struct DoorGeom {
    float x0, z0, x1, z1;  // the panel's run
    float base, height;
    float vStart, thick, light;
    int texture;           // wall number (from 'startwalls')
    bool capTop = true, capBottom = true;   // the slab's top and bottom (off where a surface lies on them)
};

// A security scanner's monitor (an addition): what its screen shows. Phases: 0 off (black, as the
// game draws it), 1 standing by, 2 scanning, 3 scan complete, 4 approved; t seconds into the phase,
// which lasts dur.
struct ScanScreen {
    int monitor = 0, phase = 0;
    float t = 0, dur = 1;
    int kind = 0;   // 0 a security scanner (SDILAB), 1 a missile console (SDI1: 0 off, 1 arming, 2 armed),
                    // 2 SDI2's launch computer (0 off, 1 the launch codes going in, 2 the sequence running),
                    // 3 an alien pod computer a pod goes into (0 waiting, 1 taking it, 2 done)
    float clock = 0;   // kind 3: the world's clock
};

struct Scene {
    std::vector<ScanScreen> scanScreens;
    float time = 0;                  // the world's clock (seconds): the computer screens run by it
    std::vector<SplatDraw> splats;   // opaque sprites (monsters, items, objects, projectiles)
    std::vector<SplatDraw> effects;  // blended sprites, drawn in order after everything else
    std::vector<ModelDraw> models;
    std::vector<DebrisDraw> debris;
    std::vector<ParticleDraw> glow, smoke;
    std::vector<LightDraw> lights;
    std::vector<float> doorOffsets;  // xyz per door
    std::vector<float> doorClips;    // per door: the box it is seen in (x0 y0 z0 x1 y1 z1), its doorway
    std::vector<float> shadows;      // x, y, z, radius per soft contact shadow
};

class Renderer {
public:
    // An activated data transmitter (an addition, not in the game): wall numbers kActiveTerminal
    // on are frames the renderer makes from wall 59, its screens in motion.
    static constexpr int kActiveTerminal = 1000, kActiveTerminalFrames = 8;
    // SDILAB's security scanners (an addition): each monitor block side showing wall287 gets its
    // own texture number kScanScreen + i (i counting such sides block by block, side by side), whose
    // screen the renderer paints from the scene's ScanScreen for i.
    static constexpr int kScanScreen = 1008, kScanScreens = 8;
    int scanMonitorWall();   // the wall number of wall287 (-1: none)
    // SDI1's missile consoles (an addition): the walls the level's scripts arm missiles from show
    // wall360; each gets its own texture number kConsoleScreen + i, its screen painted from the
    // scene's ScanScreen of kind 1 for i. setScreenWalls names them before buildLevel: edge
    // (cell * 2 + 1 for a north wall, + 0 for a west one) -> i.
    static constexpr int kConsoleScreen = 1100, kConsoleScreens = 16;
    int consoleWall();       // the wall number of wall360 (-1: none)
    void setScreenWalls(const std::map<int, int>& walls) { screenWalls_ = walls; }
    // Computer screens in motion (an addition, not in the game). Every wall and block side showing
    // a terminal's screens gets one of kIdleGroups textures of its kind, kIdleScreen + kind *
    // kIdleGroups + group, which the renderer redraws as the world's clock runs, each group out of
    // step with the others: kind 0 wall57 (the mainframes, consoles and wall monitors of SDI1, and
    // CLONE's, FAILSAF2's ...), 1 wall359 (the twin read-outs beside SDI1's missile consoles), 2
    // wall150 (a keypad's display), 3 wall93 (the aliens' pod computer: HIVE1, HIVE3, ALIEN1,
    // MOTHER3). setStillWalls (before buildLevel) names edges (cell * 2 + 1 a
    // north wall, + 0 a west one) left as they are: those a readied item is used on, such as
    // BIOLAB's data transmitters, which come alive when a data stack goes in (kActiveTerminal).
    // Kinds 4-11 are MOTHER1's alien screens, each in two halves: 4-5 wall221/222, the big window
    // on space, whose stars twinkle; 6-7 wall227/228, stars over a grid with poles (the grid
    // streams, the beacons pulse, the poles' feet blink); 8-9 wall258/259, the console with two
    // screens (stars, the red globe turning, the blue display's text, the green lamps); 10-11
    // wall224/225, the green tactical map (a scan sweeping down it, the blips pulsing).
    static constexpr int kIdleScreen = 1200, kIdleKinds = 12, kIdleGroups = 4;
    // SDI2's launch computer (an addition): the walls showing its two halves, wall329 (left) and
    // wall330 (right), draw with kLaunchScreen + 0 and + 1, painted from the scene's ScanScreen of
    // kind 2. setLaunchWalls (before buildLevel) names the edges (as setScreenWalls).
    static constexpr int kLaunchScreen = 1300;
    static constexpr float kLaunchShow = 5.0f;   // seconds the launch codes' part takes
    int launchWall(int half);   // the wall number of wall329 (0) or wall330 (1) (-1: none)
    void setLaunchWalls(const std::set<int>& edges) { launchWalls_ = edges; }
    void setStillWalls(const std::set<int>& edges) { stillWalls_ = edges; }
    // The aliens' pod computers a pod goes into (an addition; HIVE1's two): the walls showing wall93
    // in the cells a script takes an item at draw with kPodScreen + i, painted from the scene's
    // ScanScreen of kind 3 for i: waiting for the pod, taking it, done. setPodWalls (before
    // buildLevel) names the edges (as setScreenWalls).
    static constexpr int kPodScreen = 1400, kPodScreens = 8;
    int podWall();           // the wall number of wall93 (-1: none)
    void setPodWalls(const std::map<int, int>& walls) { podWalls_ = walls; }
    bool init(GameData& data, std::string& error);
    void buildLevel(const Level& level);
    // The walls (per cell: bit 0 west edge, bit 1 north edge) and surfaces (bit k: 0 floor,
    // 1 ceiling, 2 and 3 the extra layers) the level's scripts move. They and every block are
    // built apart from the rest (buildLevel builds both); buildMoving builds just them again.
    void setMovingParts(const std::vector<uint8_t>& walls, const std::vector<uint8_t>& surfaces);
    void buildMoving(const Level& level);
    const std::vector<float>& movingTriangles() const { return movCollision_; }  // 9 floats per triangle
    // Whether a triangle (of the moving parts' or the rest) shows a hole in its texture at a point
    // (barycentric b1, b2): what "use" picks goes through such holes, as the game's picking does.
    bool holeAt(bool moving, size_t tri, float b1, float b2) const;
    // What a creature's sight (FUN_00020f42) makes of a triangle: bit 1 a wall it sees through
    // (flag 0x02: windows, grates), bit 2 part of a block, which its sight never looks at.
    int sightFlags(bool moving, size_t tri) const {
        const auto& mk = moving ? movTriMasked_ : triMasked_;
        return tri < mk.size() ? mk[tri] & 6 : 0;
    }
    void buildDoors(const std::vector<DoorGeom>& doors);
    // The level's animated walls and flats (surfanim): for each texture number, the frame shown.
    void setAnimFrames(const std::vector<uint16_t>& walls, const std::vector<uint16_t>& flats);
    ModelRange addModel(const std::vector<MorphVoxel>& voxels);
    ModelRange addSplats(const std::vector<SplatMorph>& splats);
    size_t splatCount() const { return splatData_.size(); }
    void draw(const Camera& cam, int width, int height, const RenderSettings& s, const Scene* scene = nullptr);
    size_t triangleCount() const { return vertexCount_ / 3; }
    size_t modelVoxels() const { return modelData_.size(); }
    bool screenshot(int width, int height, const std::string& path);
    // The frame on screen, cut to 16:10 about its centre and shrunk to tw x th (RGB, top row first):
    // a saved game's picture.
    std::vector<uint8_t> thumbnail(int width, int height, int tw, int th);
    // The game's death screen: what is on screen squeezed into rect (x0, y0, x1, y1 in its 320x200
    // screen), brightened toward white (0..1), rows torn sideways by glitch (320-wide pixels).
    void crt(int width, int height, const float rect[4], float bright, float glitch);
    // BRIGHTNESS (an addition): what is on screen through a gamma curve (1: nothing done).
    void brighten(int width, int height, float gamma);
    // Keeps a copy of what is on screen now (the 3D view), for the tear's untouched rows.
    void keepFrame(int width, int height);
    const std::vector<float>& collisionTriangles() const { return collision_; }  // 9 floats per triangle
    int wallHeight(int number);
    int wallWidth(int number);

private:
    struct Tex {
        GLuint id = 0;
        int w = 0, h = 0;
        std::vector<uint8_t> px;   // walls: the texels (row by row), for picking through their holes
    };
    struct Vertex {
        float x, y, z, u, v, light, masked;
        float block = 0;   // part of a block (CUBE): a creature's sight passes it (not drawn by the GPU)
    };
    struct Batch {
        GLuint tex;
        int first, count;
        int key = -1;   // the texture by number (wallKey / flatKey), for the animations; -1: fixed
    };
    static int wallKey(int number) { return number & 0xffff; }
    static int flatKey(int number) { return 1 << 16 | (number & 0xffff); }
    GLuint textureFor(int key);
    GLuint animated(const Batch& b);
    using TexVerts = std::map<int, std::vector<Vertex>>;   // by texture key
    void emitLevel(const Level& level, bool moving, TexVerts& byTex, TexVerts& glassByTex);
    void upload(TexVerts& byTex, TexVerts& glassByTex, GLuint vao, GLuint vbo, std::vector<Batch>& batches,
                std::vector<Batch>& glass, std::vector<float>& collision, std::vector<float>& uv, std::vector<int>& keys,
                std::vector<uint8_t>& masked);
    const Tex* wallTexture(int number);
    static std::vector<uint8_t> transmitterFrame(const std::vector<uint8_t>& src, int w, int h, int k, const uint8_t* pal);
    void scanFrame(std::vector<uint8_t>& px, int w, int h, const ScanScreen& s);
    void missileFrame(std::vector<uint8_t>& px, int w, int h, const ScanScreen& s);
    int consoleWall_ = -2;
    int consoleKey_[kConsoleScreens] = {};
    std::map<int, int> screenWalls_;
    void updateScanScreens(const std::vector<ScanScreen>& screens);
    int scanWall_ = -2;                   // wall287's number (-2: not looked up yet)
    std::set<int> stillWalls_;
    std::set<int> launchWalls_;
    int launchWall_[2] = {-2, -2};
    int launchKey_ = INT32_MIN;
    void launchFrame(std::vector<uint8_t>& left, std::vector<uint8_t>& right, const ScanScreen& s);
    int idleWall_[kIdleKinds] = {-2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2};          // each kind's wall number (-2: not looked up yet)
    int idleKind(int number);                          // the kind a wall number shows (-1: none)
    int idleKey_[kIdleKinds * kIdleGroups] = {};       // what each texture shows now
    bool idleUsed_[kIdleKinds * kIdleGroups] = {};     // shown in this level
    char idleClass_[256] = {};                         // palette colours: 'g' green, 'b' blue, 'r' red, 'm' magenta
    uint8_t idleDim_[256] = {};                        // the nearest colour at about a third of the brightness
    std::vector<uint8_t> idleRamp_[3];                 // the red, green and blue colours, darkest first
    int idleRampAt_[256] = {};                         // a colour's place in its ramp
    bool idleTables_ = false;
    void updateIdleScreens(float time);
    // state (kind 3): 0 running on its own, 1 waiting for a pod, 2 taking it (since: seconds), 3 done.
    void idleFrame(std::vector<uint8_t>& px, int w, int h, int kind, int group, float t, int state = 0, float since = 0);
    std::map<int, int> podWalls_;
    int podKey_[kPodScreens] = {};
    int scanKey_[kScanScreens] = {};      // what each monitor's texture shows now (phase, frame)
    const Tex* flatTexture(int number);
    GLuint compile(const char* vs, const char* fs, std::string& error);
    void ensureTarget(int w, int h);
    void setLighting(GLuint prog, const RenderSettings& s, const Scene* scene);

    GameData* data_ = nullptr;
    GLuint prog_ = 0, skyProg_ = 0, voxProg_ = 0, debrisProg_ = 0, partProg_ = 0, shadowProg_ = 0;
    GLuint shadowVao_ = 0, shadowVbo_ = 0;
    GLuint splatProg_ = 0, splatVao_ = 0, splatVbo_ = 0;
    std::vector<SplatMorph> splatData_;
    size_t splatUploaded_ = 0;
    void drawSplats(const std::vector<SplatDraw>& list, const float* vp, const Camera& cam, const float* fwd,
                    const float* right, const float* up, const RenderSettings& s, const Scene* scene);
    GLint uViewProj_ = -1, uEye_ = -1, uLighting_ = -1, uShadeBase_ = -1, uShadeDist_ = -1, uLightScale_ = -1;
    GLint uSkyYaw_ = -1, uSkyPitch_ = -1, uSkyFov_ = -1, uSkyAspect_ = -1;
    GLuint palTex_ = 0, shadeTex_ = 0, skyTex_ = 0;
    GLuint vao_ = 0, vbo_ = 0, skyVao_ = 0;
    std::map<int, Tex> walls_, flats_;
    std::vector<Batch> batches_;
    std::vector<Batch> glassBatches_;   // see-through walls (flag 0x10), drawn blended after the rest
    size_t vertexCount_ = 0;
    std::vector<float> collision_;
    std::vector<float> collisionUV_, movCollisionUV_;   // per vertex of the collision triangles: u, v
    std::vector<int> triKey_, movTriKey_;               // per triangle: its texture key
    std::vector<uint8_t> triMasked_, movTriMasked_;     // per triangle: bit 0 drawn masked (its holes see through),
                                                        // bit 1 a wall a creature sees through (flag 0x02), bit 2 a block
    // Blocks and the parts scripts move
    GLuint movVao_ = 0, movVbo_ = 0;
    std::vector<Batch> movBatches_, movGlassBatches_;
    std::vector<float> movCollision_;
    std::vector<uint8_t> movingWalls_, movingSurfaces_;
    std::vector<uint16_t> animWalls_, animFlats_;
    GLuint fbo_ = 0, fboColor_ = 0, fboDepth_ = 0;
    GLuint crtProg_ = 0, crtTex_ = 0;
    GLuint brightProg_ = 0, brightTex_ = 0;
    int brightW_ = 0, brightH_ = 0;
    int crtW_ = 0, crtH_ = 0;
    GLuint prevTex_ = 0;
    int prevW_ = 0, prevH_ = 0;
    int fboW_ = 0, fboH_ = 0;

    // Voxel models (all frames of all visuals in one instance buffer)
    std::vector<MorphVoxel> modelData_;
    size_t modelUploaded_ = 0;
    GLuint modelVao_ = 0, modelVbo_ = 0;
    // Doors
    GLuint doorVao_ = 0, doorVbo_ = 0;
    std::vector<Batch> doorBatches_;
    // Dynamic instance streams
    GLuint debrisVao_ = 0, debrisVbo_ = 0, partVao_ = 0, partVbo_ = 0;
};
