#include "renderer.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

// Shared fade-table lighting, plus dynamic point lights that lift surfaces toward brighter rows.
#define LIGHTING_GLSL R"(
uniform sampler2D uPal;     // 256x1 RGBA palette
uniform sampler2D uShade;   // 256x127 fade table
uniform int uLighting;
uniform float uAmbient;     // global ambient (5 in the original)
uniform float uDepthScale;  // 1.0 = authentic distance falloff
uniform float uBrightness;  // extra fade-table rows (negative = brighter)
uniform int uNumLights;
uniform vec4 uLightPos[8];  // xyz, radius
uniform vec4 uLightCol[8];  // rgb glow, intensity in shade units
vec4 shadeColor(int idx, float light, float depth, vec3 world, float extra) {
    float dyn = 0.0;
    vec3 glow = vec3(0.0);
    for (int i = 0; i < uNumLights; ++i) {
        float d = length(world - uLightPos[i].xyz);
        float f = clamp(1.0 - d / uLightPos[i].w, 0.0, 1.0);
        f *= f;
        dyn += uLightCol[i].w * f;
        glow += uLightCol[i].rgb * f;
    }
    int c = idx;
    if (uLighting == 1) {
        // Original: shade = ambient + 4*cellLight + depth, clamped to +-640, mapped to fade rows 0..126
        float shade = clamp(uAmbient + 4.0 * light + max(depth, 0.0) * uDepthScale + extra - dyn, -640.0, 639.0);
        float row = 63.0 + shade * 63.0 / 640.0 + uBrightness;
        int r = int(clamp(row, 0.0, 126.0));
        c = int(texelFetch(uShade, ivec2(idx, r), 0).r * 255.0 + 0.5);
    }
    vec4 col = texelFetch(uPal, ivec2(c, 0), 0);
    col.rgb = min(col.rgb + glow * col.rgb * 0.9 + glow * 0.12, vec3(1.0));
    return col;
}
)"

const char* kWorldVS = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec2 aLight;   // x: per-surface light offset (fade rows), y: masked
uniform mat4 uViewProj;
uniform vec3 uEye;
uniform vec3 uFwd;
uniform vec3 uOffset;                  // doors slide by this much
out vec2 vUV;
out float vLight;
out float vDepth;
out vec3 vWorld;
flat out float vMasked;
void main() {
    vec3 p = aPos + uOffset;
    gl_Position = uViewProj * vec4(p, 1.0);
    vUV = aUV;
    vLight = aLight.x;
    vMasked = aLight.y;
    vWorld = p;
    vDepth = dot(p - uEye, uFwd);   // camera-space depth, as the 1994 renderer used
}
)";

const char* kWorldFS = "#version 330 core\n" LIGHTING_GLSL R"(
uniform sampler2D uTex;     // R8 palette indices
uniform vec3 uClipMin;      // doors are only seen inside their doorway
uniform vec3 uClipMax;
in vec2 vUV;
in float vLight;
in float vDepth;
in vec3 vWorld;
flat in float vMasked;
out vec4 fragColor;
void main() {
    if (any(lessThan(vWorld, uClipMin)) || any(greaterThan(vWorld, uClipMax))) discard;
    int idx = int(texture(uTex, vUV).r * 255.0 + 0.5);
    if (vMasked > 0.5 && idx == 0) discard;
    fragColor = shadeColor(idx, vLight, vDepth, vWorld, 0.0);
}
)";

// Voxel models: one instance per surface voxel, gliding from its pose in frame A to frame B.
// Each instance draws the three cube faces that face the camera (18 vertices).
#define CUBE_FACES_GLSL R"(
const int kCorner[6] = int[6](0, 1, 2, 0, 2, 3);
vec3 cubeCorner(vec3 eyeRel, out vec3 n) {
    int face = gl_VertexID / 6;
    int k = kCorner[gl_VertexID % 6];
    vec2 q = vec2((k == 1 || k == 2) ? 0.5 : -0.5, (k >= 2) ? 0.5 : -0.5);
    if (face == 0) { float s = eyeRel.x >= 0.0 ? 1.0 : -1.0; n = vec3(s, 0, 0); return vec3(0.5 * s, q.x * s, q.y); }
    if (face == 1) { float s = eyeRel.y >= 0.0 ? 1.0 : -1.0; n = vec3(0, s, 0); return vec3(q.y, 0.5 * s, q.x * s); }
    float s = eyeRel.z >= 0.0 ? 1.0 : -1.0; n = vec3(0, 0, s); return vec3(q.x * s, q.y, 0.5 * s);
}
)"

const char* kVoxelVS = "#version 330 core\n" CUBE_FACES_GLSL R"(
layout(location = 0) in uvec4 aFrom;   // x+128, y, z+128, colour
layout(location = 1) in uvec4 aTo;
uniform mat4 uViewProj;
uniform vec3 uEye;
uniform vec3 uFwd;
uniform vec3 uPos;
uniform mat3 uRot;
uniform float uScale;
uniform float uT;
uniform float uCube;
uniform vec3 uEyeLocal;
uniform float uLight;
uniform float uFlash;
uniform vec3 uSun;
out float vDepth;
out vec3 vWorld;
flat out int vColor;
flat out float vExtra;
flat out float vLight;
void main() {
    vec3 a = vec3(aFrom.xyz) - vec3(128.0, 0.0, 128.0);
    vec3 b = vec3(aTo.xyz) - vec3(128.0, 0.0, 128.0);
    float t = smoothstep(0.0, 1.0, uT);
    vec3 c = mix(a, b, t) + vec3(0.5);
    vec3 n;
    vec3 off = cubeCorner(uEyeLocal - c, n);
    vec3 world = uPos + uRot * ((c + off * uCube) * uScale);
    gl_Position = uViewProj * vec4(world, 1.0);
    vWorld = world;
    vDepth = dot(world - uEye, uFwd);
    vColor = int(uT < 0.5 ? aFrom.w : aTo.w);
    vec3 wn = uRot * n;
    vExtra = -dot(wn, uSun) * 45.0 - uFlash * 420.0;
    vLight = uLight;
}
)";

const char* kDebrisVS = "#version 330 core\n" CUBE_FACES_GLSL R"(
layout(location = 0) in vec4 aPosSize;
layout(location = 1) in vec4 aMisc;    // tumble angle, palette index, light, seed
uniform mat4 uViewProj;
uniform vec3 uEye;
uniform vec3 uFwd;
uniform vec3 uSun;
out float vDepth;
out vec3 vWorld;
flat out int vColor;
flat out float vExtra;
flat out float vLight;
mat3 axisAngle(vec3 a, float ang) {
    float c = cos(ang), s = sin(ang), t = 1.0 - c;
    return mat3(t * a.x * a.x + c, t * a.x * a.y + s * a.z, t * a.x * a.z - s * a.y,
                t * a.x * a.y - s * a.z, t * a.y * a.y + c, t * a.y * a.z + s * a.x,
                t * a.x * a.z + s * a.y, t * a.y * a.z - s * a.x, t * a.z * a.z + c);
}
void main() {
    float seed = aMisc.w;
    vec3 axis = normalize(vec3(sin(seed * 12.9898), cos(seed * 78.233), sin(seed * 37.719) + 0.01));
    mat3 R = axisAngle(axis, aMisc.x);
    vec3 n;
    vec3 off = cubeCorner(transpose(R) * (uEye - aPosSize.xyz), n);
    vec3 world = aPosSize.xyz + R * (off * aPosSize.w);
    gl_Position = uViewProj * vec4(world, 1.0);
    vWorld = world;
    vDepth = dot(world - uEye, uFwd);
    vColor = int(aMisc.y + 0.5);
    vExtra = -dot(R * n, uSun) * 45.0;
    vLight = aMisc.z;
}
)";

const char* kSplatVS = R"(#version 330 core
layout(location = 0) in ivec4 aFrom;   // quarter units; colour in w
layout(location = 1) in ivec4 aTo;
uniform mat4 uViewProj;
uniform vec3 uEye;
uniform vec3 uFwd;
uniform vec3 uPos;
uniform mat3 uRot;
uniform float uScale;
uniform float uT;
uniform float uSize;
uniform float uGrowX;      // depth sprites seen off their original angle: widen the pixel squares
uniform int uFacing;
uniform vec3 uBasisR;      // billboard plane (and pixel square) right/up vectors
uniform vec3 uBasisU;
uniform vec3 uCamR;        // camera right/up: depth-sprite pixels always face the viewer
uniform vec3 uCamU;
uniform float uRoll;
uniform float uRollY;
uniform vec2 uKeep;
uniform float uLight;
uniform float uFlash;
out float vDepth;
out vec3 vWorld;
flat out int vColor;
flat out float vExtra;
flat out float vLight;
float hash(int i) {
    uint x = uint(i) * 2654435761u;
    x ^= x >> 15; x *= 2246822519u; x ^= x >> 13;
    return float(x & 0xffffu) / 65536.0;
}
void main() {
    float h = hash(gl_InstanceID);
    if (h < uKeep.x || h >= uKeep.y) { gl_Position = vec4(2.0, 2.0, 2.0, 1.0); return; }
    vec3 a = vec3(aFrom.xyz) * 0.25, b = vec3(aTo.xyz) * 0.25;
    vec3 c = mix(a, b, uT);
    int k = int[6](0, 1, 2, 0, 2, 3)[gl_VertexID];
    vec2 q = vec2((k == 1 || k == 2) ? 0.5 : -0.5, (k >= 2) ? 0.5 : -0.5);
    vec3 world;
    if (uFacing == 0) {
        vec3 centre = uPos + uRot * (c * uScale);
        world = centre + (uCamR * (q.x * (uSize + uGrowX)) + uCamU * (q.y * uSize)) * uScale;
    } else {
        float cr = cos(uRoll), sr = sin(uRoll);
        vec2 l = vec2(c.x, c.y - uRollY);
        l = vec2(l.x * cr - l.y * sr, l.x * sr + l.y * cr) + vec2(0.0, uRollY);
        vec3 r = uBasisR * cr + uBasisU * sr, u = -uBasisR * sr + uBasisU * cr;
        world = uPos + (uBasisR * l.x + uBasisU * l.y) * uScale + (r * q.x + u * q.y) * (uSize * uScale);
    }
    gl_Position = uViewProj * vec4(world, 1.0);
    vWorld = world;
    vDepth = dot(world - uEye, uFwd);
    vColor = uT < 0.5 ? aFrom.w : aTo.w;
    vExtra = -uFlash * 230.0;
    vLight = uLight;
}
)";

const char* kSplatFS = "#version 330 core\n" LIGHTING_GLSL R"(
uniform int uFullbright;
uniform int uAdditive;
uniform float uAlpha;
in float vDepth;
in vec3 vWorld;
flat in int vColor;
flat in float vExtra;
flat in float vLight;
out vec4 fragColor;
void main() {
    vec4 c = uFullbright == 1 ? texelFetch(uPal, ivec2(vColor, 0), 0) : shadeColor(vColor, vLight, vDepth, vWorld, vExtra);
    if (uAdditive == 1) fragColor = vec4(c.rgb * uAlpha, 1.0);
    else fragColor = vec4(c.rgb, uAlpha);
}
)";

const char* kVoxelFS = "#version 330 core\n" LIGHTING_GLSL R"(
in float vDepth;
in vec3 vWorld;
flat in int vColor;
flat in float vExtra;
flat in float vLight;
out vec4 fragColor;
void main() {
    fragColor = shadeColor(vColor, vLight, vDepth, vWorld, vExtra);
}
)";

// Soft contact shadows: a darkening disc laid on the ground under each model.
const char* kShadowVS = R"(#version 330 core
layout(location = 0) in vec4 aPosRadius;
uniform mat4 uViewProj;
out vec2 vQ;
void main() {
    int k = int[6](0, 1, 2, 0, 2, 3)[gl_VertexID];
    vec2 q = vec2((k == 1 || k == 2) ? 1.0 : -1.0, (k >= 2) ? 1.0 : -1.0);
    vec3 p = aPosRadius.xyz + vec3(q.x * aPosRadius.w, 0.6, q.y * aPosRadius.w);
    gl_Position = uViewProj * vec4(p, 1.0);
    vQ = q;
}
)";

const char* kShadowFS = R"(#version 330 core
in vec2 vQ;
out vec4 fragColor;
void main() {
    float r2 = dot(vQ, vQ);
    if (r2 > 1.0) discard;
    float a = (1.0 - r2);
    fragColor = vec4(0.0, 0.0, 0.0, a * a * 0.55);
}
)";

const char* kParticleVS = R"(#version 330 core
layout(location = 0) in vec4 aPosSize;
layout(location = 1) in vec4 aColor;
layout(location = 2) in vec4 aVel;     // velocity, stretch (seconds of motion blur)
uniform mat4 uView;
uniform mat4 uProj;
out vec2 vQ;
out vec4 vColor;
void main() {
    int k = int[6](0, 1, 2, 0, 2, 3)[gl_VertexID];
    vec2 q = vec2((k == 1 || k == 2) ? 1.0 : -1.0, (k >= 2) ? 1.0 : -1.0);
    vec4 cv = uView * vec4(aPosSize.xyz, 1.0);
    vec3 vv = mat3(uView) * aVel.xyz;
    float size = aPosSize.w;
    vec2 off = q * size;
    float sl = length(vv.xy);
    if (aVel.w > 0.0 && sl > 1.0) {
        vec2 ax = vv.xy / sl;
        vec2 pp = vec2(-ax.y, ax.x);
        off = ax * q.x * (size + sl * aVel.w) + pp * q.y * size;
    }
    cv.xy += off;
    gl_Position = uProj * cv;
    vQ = q;
    vColor = aColor;
}
)";

const char* kParticleFS = R"(#version 330 core
uniform int uAdditive;
in vec2 vQ;
in vec4 vColor;
out vec4 fragColor;
void main() {
    float r2 = dot(vQ, vQ);
    if (r2 > 1.0) discard;
    float a = (1.0 - r2);
    a *= a;
    if (uAdditive == 1) fragColor = vec4(vColor.rgb * vColor.a * a, 1.0);
    else fragColor = vec4(vColor.rgb, vColor.a * a);
}
)";

const char* kSkyVS = R"(#version 330 core
out vec2 vNdc;
void main() {
    vec2 p = vec2((gl_VertexID == 1) ? 3.0 : -1.0, (gl_VertexID == 2) ? 3.0 : -1.0);
    vNdc = p;
    gl_Position = vec4(p, 0.999, 1.0);
}
)";

// BRIGHTNESS (an addition): the 3D view through a gamma curve, lifting its dark parts.
const char* kBrightFS = R"(#version 330 core
uniform sampler2D uScreen;
uniform float uGamma;
out vec4 fragColor;
void main() {
    vec3 c = texelFetch(uScreen, ivec2(gl_FragCoord.xy), 0).rgb;
    fragColor = vec4(pow(c, vec3(1.0 / uGamma)), 1.0);
}
)";

// The game's death screen (FUN_00036b53): the whole picture squeezed into a shrinking rectangle
// on black, brightening toward the fade table's brightest row, with rows torn sideways.
const char* kCrtFS = R"(#version 330 core
uniform sampler2D uScreen;
uniform sampler2D uPrev;  // the picture shown last frame (rows the tear leaves alone keep it)
uniform vec2 uSize;     // window pixels
uniform vec4 uRect;     // x0, y0, x1, y1 in the game's 320x200 screen, y down
uniform float uBright;  // 0..1 toward white
uniform float uGlitch;  // row tear in 320-wide pixels
uniform float uStale;   // 1: the tear's skipped rows show the last frame
out vec4 fragColor;
void main() {
    vec2 p = vec2(gl_FragCoord.x, uSize.y - gl_FragCoord.y) / uSize * vec2(320.0, 200.0);
    vec2 c = (uRect.xy + uRect.zw) * 0.5;
    vec2 hs = max((uRect.zw - uRect.xy) * 0.5, vec2(0.5));   // never thinner than one game pixel
    if (any(lessThan(p, c - hs)) || any(greaterThan(p, c + hs))) { fragColor = vec4(0.0, 0.0, 0.0, 1.0); return; }
    vec2 uv = (p - (c - hs)) / (2.0 * hs);
    // The game copies its picture to the screen row by row (FUN_00036b53): rows 5, 7, 8 and 11 of
    // every 16 shifted by a quarter of the tear, 6 and 13 by half, and rows 0, 4, 12 and 14 not
    // copied at all, so they still show the frame before.
    float m = mod(floor(uv.y * 200.0), 16.0);
    if (uGlitch > 0.0 && uStale > 0.5 && (m == 0.0 || m == 4.0 || m == 12.0 || m == 14.0)) {
        fragColor = vec4(texture(uPrev, gl_FragCoord.xy / uSize).rgb, 1.0);
        return;
    }
    float shift = (m == 5.0 || m == 7.0 || m == 8.0 || m == 11.0) ? floor(uGlitch * 0.25) : (m == 6.0 || m == 13.0) ? floor(uGlitch * 0.5) : 0.0;
    uv.x = fract(uv.x + shift / 320.0);
    vec3 col = texture(uScreen, vec2(uv.x, 1.0 - uv.y)).rgb;
    fragColor = vec4(1.0 - (1.0 - col) * (1.0 - uBright), 1.0);
}
)";

// The game's sky (FUN_0003e6d9) is a flat backdrop, not a dome: sky1 and sky2 side by side make a
// 640x160 panorama for the whole turn (its 0-255 heading times 2.5 is the scroll), one sky pixel
// per column of its 90-degree view, so the screen shows half the panorama. The panorama's bottom
// row sits on the horizon; looking up past row 160 it stays put. Below it the sky is black.
const char* kSkyFS = R"(#version 330 core
uniform sampler2D uSky;
uniform sampler2D uPal;
uniform float uScroll;   // sky pixels
uniform float uHorizon;  // the horizon's row in the game's 200-row screen
uniform vec2 uFov;       // tan(hfov/2), tan(vfov/2)
in vec2 vNdc;
out vec4 fragColor;
void main() {
    // Columns and rows of the game's 320x200 view (focal length 160 both ways).
    float col = 160.0 + 160.0 * vNdc.x * uFov.x;
    float row = 100.0 - 160.0 * vNdc.y * uFov.y;
    float sr = row + max(0.0, 160.0 - uHorizon);
    if (sr >= 160.0) { fragColor = texelFetch(uPal, ivec2(0, 0), 0); return; }
    int x = int(mod(floor(320.0 - uScroll + col), 640.0));
    int idx = int(texelFetch(uSky, ivec2(x, int(clamp(sr, 0.0, 159.0))), 0).r * 255.0 + 0.5);
    fragColor = texelFetch(uPal, ivec2(idx, 0), 0);
}
)";

void mat4Mul(const float* a, const float* b, float* out) {  // column-major: out = a * b
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a[k * 4 + r] * b[c * 4 + k];
            out[c * 4 + r] = s;
        }
}

GLuint makeTexture(int w, int h, GLenum internal, GLenum format, const void* pixels, bool repeat) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)internal, w, h, 0, format, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    return t;
}

}  // namespace

GLuint Renderer::compile(const char* vs, const char* fs, std::string& error) {
    auto stage = [&](GLenum type, const char* src) -> GLuint {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[2048];
            glGetShaderInfoLog(s, sizeof(log), nullptr, log);
            error += log;
            return 0;
        }
        return s;
    };
    GLuint v = stage(GL_VERTEX_SHADER, vs), f = stage(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        error += log;
        return 0;
    }
    return p;
}

bool Renderer::init(GameData& data, std::string& error) {
    data_ = &data;
    prog_ = compile(kWorldVS, kWorldFS, error);
    skyProg_ = compile(kSkyVS, kSkyFS, error);
    crtProg_ = compile(kSkyVS, kCrtFS, error);
    brightProg_ = compile(kSkyVS, kBrightFS, error);
    voxProg_ = compile(kVoxelVS, kVoxelFS, error);
    debrisProg_ = compile(kDebrisVS, kVoxelFS, error);
    partProg_ = compile(kParticleVS, kParticleFS, error);
    shadowProg_ = compile(kShadowVS, kShadowFS, error);
    splatProg_ = compile(kSplatVS, kSplatFS, error);
    if (!prog_ || !skyProg_ || !crtProg_ || !brightProg_ || !voxProg_ || !debrisProg_ || !partProg_ || !shadowProg_ || !splatProg_) return false;
    uViewProj_ = glGetUniformLocation(prog_, "uViewProj");
    uEye_ = glGetUniformLocation(prog_, "uEye");
    uLighting_ = glGetUniformLocation(prog_, "uLighting");
    uShadeBase_ = glGetUniformLocation(prog_, "uAmbient");
    uShadeDist_ = glGetUniformLocation(prog_, "uDepthScale");
    uLightScale_ = glGetUniformLocation(prog_, "uBrightness");
    uSkyYaw_ = glGetUniformLocation(skyProg_, "uScroll");
    uSkyPitch_ = glGetUniformLocation(skyProg_, "uHorizon");
    uSkyFov_ = glGetUniformLocation(skyProg_, "uFov");

    uint8_t rgba[256 * 4];
    for (int i = 0; i < 256; ++i) {
        for (int c = 0; c < 3; ++c) {
            int v = data.palette[i * 3 + c] & 63;
            rgba[i * 4 + c] = (uint8_t)std::min(255, v * 4 + v / 16);
        }
        rgba[i * 4 + 3] = 255;
    }
    palTex_ = makeTexture(256, 1, GL_RGBA8, GL_RGBA, rgba, false);
    shadeTex_ = makeTexture(256, 127, GL_R8, GL_RED, data.fade.data(), false);
    if (data.sky) {
        std::vector<uint8_t> pano(640 * 160);
        for (int y = 0; y < 160; ++y) {
            std::memcpy(&pano[y * 640], data.sky + y * 320, 320);
            std::memcpy(&pano[y * 640 + 320], (data.sky2 ? data.sky2 : data.sky) + y * 320, 320);
        }
        skyTex_ = makeTexture(640, 160, GL_R8, GL_RED, pano.data(), true);
    }

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glGenVertexArrays(1, &skyVao_);

    // Voxel model instances: two uvec4 per instance (from, to).
    glGenVertexArrays(1, &modelVao_);
    glGenBuffers(1, &modelVbo_);
    glBindVertexArray(modelVao_);
    glBindBuffer(GL_ARRAY_BUFFER, modelVbo_);
    glVertexAttribIPointer(0, 4, GL_UNSIGNED_BYTE, sizeof(MorphVoxel), (void*)0);
    glVertexAttribIPointer(1, 4, GL_UNSIGNED_BYTE, sizeof(MorphVoxel), (void*)4);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribDivisor(0, 1);
    glVertexAttribDivisor(1, 1);

    glGenVertexArrays(1, &debrisVao_);
    glGenBuffers(1, &debrisVbo_);
    glBindVertexArray(debrisVao_);
    glBindBuffer(GL_ARRAY_BUFFER, debrisVbo_);
    glVertexAttribPointer(0, 4, GL_FLOAT, 0, sizeof(DebrisDraw), (void*)0);
    glVertexAttribPointer(1, 4, GL_FLOAT, 0, sizeof(DebrisDraw), (void*)(4 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribDivisor(0, 1);
    glVertexAttribDivisor(1, 1);

    glGenVertexArrays(1, &partVao_);
    glGenBuffers(1, &partVbo_);
    glBindVertexArray(partVao_);
    glBindBuffer(GL_ARRAY_BUFFER, partVbo_);
    for (int i = 0; i < 3; ++i) {
        glVertexAttribPointer(i, 4, GL_FLOAT, 0, sizeof(ParticleDraw), (void*)(i * 4 * sizeof(float)));
        glEnableVertexAttribArray(i);
        glVertexAttribDivisor(i, 1);
    }

    glGenVertexArrays(1, &splatVao_);
    glGenBuffers(1, &splatVbo_);
    glBindVertexArray(splatVao_);
    glBindBuffer(GL_ARRAY_BUFFER, splatVbo_);
    glVertexAttribIPointer(0, 4, GL_SHORT, sizeof(SplatMorph), (void*)0);
    glVertexAttribIPointer(1, 4, GL_SHORT, sizeof(SplatMorph), (void*)8);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribDivisor(0, 1);
    glVertexAttribDivisor(1, 1);

    glGenVertexArrays(1, &shadowVao_);
    glGenBuffers(1, &shadowVbo_);
    glBindVertexArray(shadowVao_);
    glBindBuffer(GL_ARRAY_BUFFER, shadowVbo_);
    glVertexAttribPointer(0, 4, GL_FLOAT, 0, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribDivisor(0, 1);

    glGenVertexArrays(1, &doorVao_);
    glGenBuffers(1, &doorVbo_);
    glGenVertexArrays(1, &movVao_);
    glGenBuffers(1, &movVbo_);
    glBindVertexArray(0);
    return true;
}

int Renderer::wallHeight(int number) {
    const Tex* t = wallTexture(number);
    return t ? t->h : 0;
}

int Renderer::wallWidth(int number) {
    const Tex* t = wallTexture(number);
    return t ? t->w : 0;
}

ModelRange Renderer::addSplats(const std::vector<SplatMorph>& splats) {
    ModelRange r{(int)splatData_.size(), (int)splats.size()};
    splatData_.insert(splatData_.end(), splats.begin(), splats.end());
    return r;
}

void Renderer::drawSplats(const std::vector<SplatDraw>& list, const float* vp, const Camera& cam, const float* fwd,
                          const float* right, const float* up, const RenderSettings& s, const Scene* scene) {
    if (list.empty() || splatData_.empty()) return;
    glUseProgram(splatProg_);
    setLighting(splatProg_, s, scene);
    auto U = [&](const char* n) { return glGetUniformLocation(splatProg_, n); };
    glUniformMatrix4fv(U("uViewProj"), 1, 0, vp);
    glUniform3f(U("uEye"), cam.x, cam.y, cam.z);
    glUniform3f(U("uFwd"), fwd[0], fwd[1], fwd[2]);
    glUniform3f(U("uCamR"), right[0], right[1], right[2]);
    glUniform3f(U("uCamU"), up[0], up[1], up[2]);
    float hr[3] = {right[0], 0, right[2]};
    float hl = std::sqrt(hr[0] * hr[0] + hr[2] * hr[2]);
    if (hl > 1e-5f) { hr[0] /= hl; hr[2] /= hl; } else { hr[0] = 1; }
    GLint uPos = U("uPos"), uRot = U("uRot"), uScale = U("uScale"), uT = U("uT"), uSize = U("uSize"), uFacing = U("uFacing");
    GLint uGrowX = U("uGrowX");
    GLint uBR = U("uBasisR"), uBU = U("uBasisU"), uRoll = U("uRoll"), uRollY = U("uRollY"), uKeep = U("uKeep");
    GLint uLight = U("uLight"), uFlash = U("uFlash"), uFull = U("uFullbright"), uAdd = U("uAdditive"), uAlpha = U("uAlpha");
    glBindVertexArray(splatVao_);
    glBindBuffer(GL_ARRAY_BUFFER, splatVbo_);
    int lastMode = -1;
    for (const auto& d : list) {
        if (d.range.count <= 0) continue;
        if (d.mode != lastMode) {
            lastMode = d.mode;
            if (d.mode == 0 || d.mode == 3) { glDisable(GL_BLEND); glDepthMask(GL_TRUE); }
            else {
                glEnable(GL_BLEND);
                glDepthMask(GL_FALSE);
                if (d.mode == 2) glBlendFunc(GL_ONE, GL_ONE);
                else glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            }
            glUniform1i(uAdd, d.mode == 2 ? 1 : 0);
        }
        glUniform3f(uPos, d.pos[0], d.pos[1], d.pos[2]);
        glUniformMatrix3fv(uRot, 1, 0, d.rot);
        glUniform1f(uScale, d.scale);
        glUniform1f(uT, d.t);
        glUniform1f(uSize, (d.t > 0.001f && d.t < 0.999f) ? 1.25f : (d.facing == 0 ? 1.22f : 1.0f));
        glUniform1i(uFacing, d.facing);
        glUniform1f(uGrowX, d.growX);
        if (d.facing == 1) { glUniform3f(uBR, hr[0], 0, hr[2]); glUniform3f(uBU, 0, 1, 0); }
        else { glUniform3f(uBR, right[0], right[1], right[2]); glUniform3f(uBU, up[0], up[1], up[2]); }
        glUniform1f(uRoll, d.roll);
        glUniform1f(uRollY, d.rollY);
        glUniform2f(uKeep, d.keepLo, d.keepHi);
        glUniform1f(uLight, d.light);
        glUniform1f(uFlash, d.flash);
        glUniform1i(uFull, d.fullbright ? 1 : 0);
        glUniform1f(uAlpha, d.alpha);
        glVertexAttribIPointer(0, 4, GL_SHORT, sizeof(SplatMorph), (void*)(size_t)(d.range.first * sizeof(SplatMorph)));
        glVertexAttribIPointer(1, 4, GL_SHORT, sizeof(SplatMorph), (void*)(size_t)(d.range.first * sizeof(SplatMorph) + 8));
        if (d.mode == 3) {
            // Translucent through the TINT_MAP, as the game draws glass and some creatures: the
            // depth of the front surface first, then that surface alone blended at 38 % (the
            // relief's own back layers would otherwise stack up).
            glDisable(GL_BLEND);
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            glDepthMask(GL_TRUE);
            glDrawArraysInstanced(GL_TRIANGLES, 0, 6, d.range.count);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glDepthMask(GL_FALSE);
            glEnable(GL_BLEND);
            glBlendColor(0, 0, 0, 0.38f);
            glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA);
            glDrawArraysInstanced(GL_TRIANGLES, 0, 6, d.range.count);
            lastMode = -1;   // the next draw sets its own state again
            continue;
        }
        glDrawArraysInstanced(GL_TRIANGLES, 0, 6, d.range.count);
    }
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glBindVertexArray(0);
}

ModelRange Renderer::addModel(const std::vector<MorphVoxel>& voxels) {
    ModelRange r{(int)modelData_.size(), (int)voxels.size()};
    modelData_.insert(modelData_.end(), voxels.begin(), voxels.end());
    return r;
}

void Renderer::buildDoors(const std::vector<DoorGeom>& doors) {
    std::vector<Vertex> all;
    doorBatches_.clear();
    for (const auto& d : doors) {
        const Tex* t = wallTexture(d.texture);
        Batch b{t ? t->id : 0, (int)all.size(), 0, wallKey(d.texture)};
        if (t) {
            float dx = d.x1 - d.x0, dz = d.z1 - d.z0;
            float len = std::sqrt(dx * dx + dz * dz);
            float nx = -dz / len * d.thick * 0.5f, nz = dx / len * d.thick * 0.5f;  // across the panel
            float bottom = d.base, top = d.base + d.height;
            float tTop = d.vStart / t->h, tBot = (d.vStart + d.height) / t->h;
            float uw = len / (float)t->w;
            // The slab stops half a unit short of its cell's edges: its thin ends lay in the plane
            // of a wall there, and where that wall faces another room (HIVE3's corridor at (8, 7)-
            // (8, 11), looking at the door in (8, 12) end on) the end showed through it as a light
            // strip, flickering (up to 0.5.2). The texture keeps its place.
            const float inset = std::min(0.5f, len * 0.25f), ix = dx / len * inset, iz = dz / len * inset, iu = inset / (float)t->w;
            DoorGeom e = d;
            e.x0 += ix; e.z0 += iz; e.x1 -= ix; e.z1 -= iz;
            auto quad = [&](float ax, float az, float bx, float bz, float u0, float u1) {
                Vertex q[4] = {{ax, bottom, az, u0, tBot, d.light, 1}, {bx, bottom, bz, u1, tBot, d.light, 1},
                               {bx, top, bz, u1, tTop, d.light, 1}, {ax, top, az, u0, tTop, d.light, 1}};
                const int idx[6] = {0, 1, 2, 0, 2, 3};
                for (int k : idx) all.push_back(q[k]);
            };
            // Two faces and the two thin ends of the slab, each facing outward (back faces are
            // culled), and each face reading left to right from its own side, as walls do.
            quad(e.x0 + nx, e.z0 + nz, e.x1 + nx, e.z1 + nz, iu, uw - iu);
            quad(e.x1 - nx, e.z1 - nz, e.x0 - nx, e.z0 - nz, iu, uw - iu);
            float te = 1.0f / t->w;
            quad(e.x0 - nx, e.z0 - nz, e.x0 + nx, e.z0 + nz, iu, iu + te);
            quad(e.x1 + nx, e.z1 + nz, e.x1 - nx, e.z1 - nz, uw - iu - te, uw - iu);
            // top and bottom caps
            Vertex c[4] = {{e.x0 - nx, top, e.z0 - nz, iu, tTop, d.light, 0}, {e.x1 - nx, top, e.z1 - nz, uw - iu, tTop, d.light, 0},
                           {e.x1 + nx, top, e.z1 + nz, uw - iu, tTop + te, d.light, 0}, {e.x0 + nx, top, e.z0 + nz, iu, tTop + te, d.light, 0}};
            const int up[6] = {0, 3, 2, 0, 2, 1};
            if (d.capTop) for (int k : up) all.push_back(c[k]);
            for (auto& v : c) v.y = bottom;
            const int dn[6] = {0, 1, 2, 0, 2, 3};
            if (d.capBottom) for (int k : dn) all.push_back(c[k]);
        }
        b.count = (int)all.size() - b.first;
        doorBatches_.push_back(b);
    }
    glBindVertexArray(doorVao_);
    glBindBuffer(GL_ARRAY_BUFFER, doorVbo_);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(all.size() * sizeof(Vertex)), all.empty() ? nullptr : all.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, 0, sizeof(Vertex), (void*)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, 0, sizeof(Vertex), (void*)(3 * sizeof(float)));
    glVertexAttribPointer(2, 2, GL_FLOAT, 0, sizeof(Vertex), (void*)(5 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glBindVertexArray(0);
}

void Renderer::setLighting(GLuint p, const RenderSettings& s, const Scene* scene) {
    glUniform1i(glGetUniformLocation(p, "uLighting"), s.lighting ? 1 : 0);
    glUniform1f(glGetUniformLocation(p, "uAmbient"), s.ambient);
    glUniform1f(glGetUniformLocation(p, "uDepthScale"), s.depthScale);
    glUniform1f(glGetUniformLocation(p, "uBrightness"), s.brightness);
    glUniform1i(glGetUniformLocation(p, "uPal"), 1);
    glUniform1i(glGetUniformLocation(p, "uShade"), 2);
    int n = scene ? (int)std::min<size_t>(8, scene->lights.size()) : 0;
    float pos[32], col[32];
    for (int i = 0; i < n; ++i) {
        const LightDraw& l = scene->lights[i];
        pos[i * 4] = l.x; pos[i * 4 + 1] = l.y; pos[i * 4 + 2] = l.z; pos[i * 4 + 3] = l.radius;
        col[i * 4] = l.r; col[i * 4 + 1] = l.g; col[i * 4 + 2] = l.b; col[i * 4 + 3] = l.intensity;
    }
    glUniform1i(glGetUniformLocation(p, "uNumLights"), n);
    if (n) {
        glUniform4fv(glGetUniformLocation(p, "uLightPos"), n, pos);
        glUniform4fv(glGetUniformLocation(p, "uLightCol"), n, col);
    }
}

// An activated data transmitter, frame k of kActiveTerminalFrames (BIOLAB's wall 59, the lump
// wall57: 64 x 128). An addition, not in the game. Over the terminal's own pixels: the big screen's
// five lines of text scroll up a line a frame and slide along; the small green screens cycle their
// rows; the blue level bars rise and fall; the green read-outs run; the red lights blink; the red
// bar under the big screen fills. Pixels are told apart by their colour in the palette.
std::vector<uint8_t> Renderer::transmitterFrame(const std::vector<uint8_t>& src, int w, int h, int k, const uint8_t* pal) {
    std::vector<uint8_t> out = src;
    if (w != 64 || h != 128 || (int)src.size() < w * h) return out;
    auto rgb = [&](uint8_t c, int ch) { return (pal[c * 3 + ch] & 63) * 4; };
    auto kind = [&](uint8_t c) {
        int r = rgb(c, 0), g = rgb(c, 1), b = rgb(c, 2);
        if (g > r + 40 && g > b + 20) return 'g';
        if (b > r + 40 && b > g + 20) return 'b';
        if (r > g + 60 && r > b + 60) return 'r';
        return '.';
    };
    auto at = [&](int x, int y) { return src[y * w + x]; };
    auto put = [&](int x, int y, uint8_t c) { out[y * w + x] = c; };
    auto dim = [&](uint8_t c) {   // the nearest colour at about a third of the brightness
        int r = rgb(c, 0) / 3, g = rgb(c, 1) / 3, b = rgb(c, 2) / 3, best = c, bestD = 1 << 30;
        for (int i = 1; i < 256; ++i) {
            int dr = rgb((uint8_t)i, 0) - r, dg = rgb((uint8_t)i, 1) - g, db = rgb((uint8_t)i, 2) - b, d = dr * dr + dg * dg + db * db;
            if (d < bestD) { bestD = d; best = i; }
        }
        return (uint8_t)best;
    };
    // The big screen: lines of blue text two rows high at 18, 21, 24, 29 and 32, x 15..49.
    {
        const int bands[5] = {18, 21, 24, 29, 32}, bg[5] = {20, 23, 26, 31, 31};
        const int x0 = 15, n = 35;
        for (int i = 0; i < 5; ++i)
            for (int dy = 0; dy < 2; ++dy)
                for (int x = x0; x < x0 + n; ++x)
                    if (kind(at(x, bands[i] + dy)) == 'b') put(x, bands[i] + dy, at(x, bg[i]));
        for (int i = 0; i < 5; ++i) {
            int from = bands[(i + k) % 5], shift = (k * 7 + i * 5) % n;
            for (int dy = 0; dy < 2; ++dy)
                for (int x = x0; x < x0 + n; ++x) {
                    uint8_t c = at(x, from + dy);
                    if (kind(c) == 'b') put(x0 + (x - x0 + shift) % n, bands[i] + dy, c);
                }
        }
    }
    // The two small screens: green rows at 60, 62, 64 and 66; level bars beside and beneath.
    for (int s = 0; s < 2; ++s) {
        int p = k + s * 3, x0 = s ? 40 : 14, n = 11;
        const int rows[4] = {60, 62, 64, 66};
        for (int j = 0; j < 4; ++j)
            for (int x = x0; x < x0 + n; ++x)
                if (kind(at(x, rows[j])) == 'g') put(x, rows[j], at(x, 61));
        for (int j = 0; j < 4; ++j) {
            int from = rows[(j + p) % 4], shift = (p * 3 + j * 2) % n;
            for (int x = x0; x < x0 + n; ++x) {
                uint8_t c = at(x, from);
                if (kind(c) == 'g') put(x0 + (x - x0 + shift) % n, rows[j], c);
            }
        }
        int bx = s ? 41 : 15, len = 2 + (p * 5) % 8;
        for (int x = bx + len; x < bx + 9; ++x)
            if (kind(at(x, 68)) == 'b') put(x, 68, at(x, 69));
        int vx = s ? 53 : 27, hgt = 1 + (p * 2) % 3;
        for (int y = 63; y <= 65 - hgt; ++y)
            if (kind(at(vx, y)) == 'b') put(vx, y, at(vx, 62));
    }
    // The green read-outs on row 79 run along.
    {
        const int seg[3][2] = {{10, 17}, {29, 35}, {47, 54}};
        for (int i = 0; i < 3; ++i) {
            int a = seg[i][0], n = seg[i][1] - a, shift = (k + i * 2) % n;
            for (int x = a; x < a + n; ++x)
                if (kind(at(x, 79)) == 'g') put(x, 79, at(x, 80));
            for (int x = a; x < a + n; ++x) {
                uint8_t c = at(x, 79);
                if (kind(c) == 'g') put(a + (x - a + shift) % n, 79, c);
            }
        }
    }
    // The red bar under the big screen fills, two pixels a frame.
    for (int x = 23; x <= 38; ++x)
        if (kind(at(x, 45)) == 'r' && x - 23 >= (k + 1) * 2) put(x, 45, dim(at(x, 45)));
    // The red lights blink (each run of red on rows 74, 93 and 102 a light).
    {
        int light = 0;
        for (int y : {74, 93, 102})
            for (int x = 0; x < w;) {
                if (kind(at(x, y)) != 'r') { ++x; continue; }
                int e = x;
                while (e < w && kind(at(e, y)) == 'r') ++e;
                bool lit = (k * 3 + light * 5) % 7 < 4;
                if (!lit)
                    for (int xx = x; xx < e; ++xx) put(xx, y, dim(at(xx, y)));
                ++light;
                x = e;
            }
    }
    return out;
}

// Computer screens in motion (an addition, not in the game; see kIdleScreen). Frame for clock t
// (seconds) of a group's texture, painted over the wall's own pixels, which are told apart by their
// colour. Calmer than an activated transmitter's: wall57's big screen is a log, a new line typed in
// at the bottom every 1.6 seconds, the others moving up, a cursor blinking after it; the small
// green screens change a line now and then, their level bars drift; the red bar under the big
// screen rises and falls; the read-outs change; the red lights blink, the lit buttons now and then
// go dark. wall359's two read-outs run along like traces; wall150's display changes its line and
// its bar. wall93, the aliens' pod computer: the red lamp on top breathes and its two pilot lights
// blink in turn; a scan line sweeps the hand-print screen, which glows as it ends; the blue display
// runs a line of text over a level bar; the green lamp blinks. Each group runs a little faster or
// slower than the others, from another point.
namespace {
uint32_t idleHash(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u ^ (c + 0x165667B1u) * 0xC2B2AE3Du;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return h;
}
}  // namespace

void Renderer::idleFrame(std::vector<uint8_t>& out, int w, int h, int kind, int g, float t, int state, float since) {
    const uint8_t* pal = data_->palette.data();
    auto rgb = [&](int c, int ch) { return (pal[c * 3 + ch] & 63) * 4; };
    if (!idleTables_) {
        idleTables_ = true;
        for (int c = 0; c < 256; ++c) {
            int r = rgb(c, 0), gr = rgb(c, 1), b = rgb(c, 2);
            idleClass_[c] = gr > r + 40 && gr > b + 20 ? 'g' : b > r + 40 && b > gr + 20 ? 'b' : r > gr + 60 && r > b + 60 ? 'r'
                          : r > gr + 40 && b > gr + 40 ? 'm' : '.';
            int dr = r / 3, dg = gr / 3, db = b / 3, best = c, bestD = 1 << 30;
            for (int i = 1; i < 256; ++i) {
                int er = rgb(i, 0) - dr, eg = rgb(i, 1) - dg, eb = rgb(i, 2) - db, d = er * er + eg * eg + eb * eb;
                if (d < bestD) { bestD = d; best = i; }
            }
            idleDim_[c] = (uint8_t)best;
        }
        for (int c = 1; c < 256; ++c) {
            int k = idleClass_[c] == 'r' ? 0 : idleClass_[c] == 'g' ? 1 : idleClass_[c] == 'b' ? 2 : -1;
            if (k >= 0) idleRamp_[k].push_back((uint8_t)c);
        }
        for (auto& ramp : idleRamp_) {
            std::stable_sort(ramp.begin(), ramp.end(), [&](uint8_t a, uint8_t b) {
                return rgb(a, 0) + rgb(a, 1) + rgb(a, 2) < rgb(b, 0) + rgb(b, 1) + rgb(b, 2);
            });
            for (size_t i = 0; i < ramp.size(); ++i) idleRampAt_[ramp[i]] = (int)i;
        }
    }
    const std::vector<uint8_t> src = out;
    auto at = [&](int x, int y) { return src[y * w + x]; };
    auto put = [&](int x, int y, uint8_t c) { out[y * w + x] = c; };
    auto cls = [&](uint8_t c) { return idleClass_[c]; };
    const float T = t * (0.9f + 0.07f * g) + g * 5.3f;
    auto dimRun = [&](int x0, int x1, int y) { for (int x = x0; x < x1; ++x) put(x, y, idleDim_[at(x, y)]); };
    // Each run of `c` pixels on row y (a light): f(light number) says whether it is lit.
    auto lights = [&](int y, char c, int& n, auto lit) {
        for (int x = 0; x < w;) {
            if (cls(at(x, y)) != c) { ++x; continue; }
            int e = x;
            while (e < w && cls(at(e, y)) == c) ++e;
            if (!lit(n)) dimRun(x, e, y);
            ++n;
            x = e;
        }
    };
    if (kind == 0) {
        if (w != 64 || h != 128) return;
        // The big screen: lines of blue text two rows high at 18, 21, 24, 29 and 32, x 15..49.
        const int bands[5] = {18, 21, 24, 29, 32}, bg[5] = {20, 23, 26, 31, 31};
        const int x0 = 15, n = 35;
        uint8_t cursor = 0;
        int best = -1;
        for (int i = 0; i < 5; ++i)
            for (int dy = 0; dy < 2; ++dy)
                for (int x = x0; x < x0 + n; ++x) {
                    uint8_t c = at(x, bands[i] + dy);
                    if (cls(c) != 'b') continue;
                    int v = rgb(c, 0) + rgb(c, 1) + rgb(c, 2);
                    if (v > best) { best = v; cursor = c; }
                    put(x, bands[i] + dy, at(x, bg[i]));
                }
        const float period = 1.6f;
        const int line = (int)std::floor(T / period);
        const float into = T - line * period;
        for (int i = 0; i < 5; ++i) {
            uint32_t r = idleHash((uint32_t)(line - 4 + i), (uint32_t)g, 11);
            int from = bands[r % 5], shift = (int)((r >> 8) % n), len = 12 + (int)((r >> 16) % 22);
            int shown = i < 4 ? len : std::min(len, (int)(into / 0.8f * len));
            for (int dy = 0; dy < 2; ++dy)
                for (int x = 0; x < shown; ++x) {
                    uint8_t c = at(x0 + (x + shift) % n, from + dy);
                    if (cls(c) == 'b') put(x0 + x, bands[i] + dy, c);
                }
            if (i == 4 && (shown < len || std::fmod(into, 0.5f) < 0.25f) && x0 + shown + 2 < x0 + n)
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 1; dx <= 2; ++dx) put(x0 + shown + dx, bands[i] + dy, cursor);
        }
        // The red bar under it rises and falls.
        {
            float level = 0.55f + 0.3f * std::sin(T * 1.3f) + 0.15f * std::sin(T * 3.7f + 1);
            int fill = (int)std::lround(std::clamp(level, 0.0f, 1.0f) * 16);
            for (int x = 23; x <= 38; ++x)
                if (cls(at(x, 45)) == 'r' && x - 23 >= fill) put(x, 45, idleDim_[at(x, 45)]);
        }
        // The two small screens: green rows at 60, 62, 64 and 66, each changing now and then; level
        // bars beneath (row 68) and beside (x 27 and 53).
        for (int s = 0; s < 2; ++s) {
            const int rows[4] = {60, 62, 64, 66};
            const int sx = s ? 40 : 14, sn = 11;
            for (int j = 0; j < 4; ++j)
                for (int x = sx; x < sx + sn; ++x)
                    if (cls(at(x, rows[j])) == 'g') put(x, rows[j], at(x, 61));
            for (int j = 0; j < 4; ++j) {
                int step = (int)std::floor((T + j * 0.43f + s * 0.71f) / 1.3f);
                uint32_t r = idleHash((uint32_t)step, (uint32_t)(j + 4 * s), (uint32_t)(g * 7 + 3));
                int from = rows[r % 4], shift = (int)((r >> 8) % sn), len = 6 + (int)((r >> 16) % 6);
                for (int x = 0; x < len; ++x) {
                    uint8_t c = at(sx + (x + shift) % sn, from);
                    if (cls(c) == 'g') put(sx + x, rows[j], c);
                }
            }
            float a = 0.5f + 0.5f * std::sin(T * (1.1f + 0.3f * s) + g + s * 2.0f);
            float b = 0.5f + 0.5f * std::sin(T * (0.7f + 0.2f * s) + g * 1.7f);
            int bx = s ? 41 : 15, len = 2 + (int)std::lround(a * 7);
            for (int x = bx + len; x < bx + 9; ++x)
                if (cls(at(x, 68)) == 'b') put(x, 68, at(x, 69));
            int vx = s ? 53 : 27, hgt = 1 + std::min(2, (int)(b * 3));
            for (int y = 63; y <= 65 - hgt; ++y)
                if (cls(at(vx, y)) == 'b') put(vx, y, at(vx, 62));
        }
        // The green read-outs on row 79 change.
        {
            const int seg[3][2] = {{10, 17}, {29, 35}, {47, 54}};
            for (int i = 0; i < 3; ++i) {
                int a = seg[i][0], n = seg[i][1] - a;
                uint32_t r = idleHash((uint32_t)std::floor((T + i * 0.5f) / 0.9f), (uint32_t)i, (uint32_t)g);
                int shift = (int)(r % n);
                for (int x = a; x < a + n; ++x)
                    if (cls(at(x, 79)) == 'g') put(x, 79, at(x, 80));
                for (int x = a; x < a + n; ++x) {
                    uint8_t c = at(x, 79);
                    if (cls(c) == 'g') put(a + (x - a + shift) % n, 79, c);
                }
            }
        }
        // The red lights blink, each at its own pace; the lit buttons (rows 93 and 102) now and then
        // go dark.
        int light = 0;
        lights(74, 'r', light, [&](int k) {
            float per = 1.1f + 0.45f * ((k + g) % 4);
            return std::fmod(T + k * 0.77f, per) < per * 0.55f;
        });
        for (int y : {93, 102})
            lights(y, 'r', light, [&](int k) {
                return idleHash((uint32_t)k, (uint32_t)std::floor((T + k * 0.31f) / 2.1f), (uint32_t)g) % 5 != 0;
            });
    } else if (kind == 1) {
        if (w != 64 || h != 128) return;
        // Two read-outs (rows 60-65 and 80-85, x 22..42 inside their frames) run along like traces.
        for (int s = 0; s < 2; ++s) {
            const int y0 = s ? 80 : 60, x0 = 22, n = 21;
            int shift = (int)std::floor(T * 5 + s * 9.0f) % n;
            for (int y = y0; y < y0 + 6; ++y) {
                uint8_t bgc = 0;
                bool have = false;
                for (int x = x0; x < x0 + n && !have; ++x)
                    if (cls(at(x, y)) == '.') { bgc = at(x, y); have = true; }
                if (!have) continue;
                for (int x = x0; x < x0 + n; ++x)
                    if (cls(at(x, y)) == 'b' || cls(at(x, y)) == 'm') put(x, y, bgc);
                for (int x = x0; x < x0 + n; ++x) {
                    uint8_t c = at(x, y);
                    if (cls(c) == 'b' || cls(c) == 'm') put(x0 + (x - x0 + n - shift) % n, y, c);
                }
            }
        }
    } else if (kind == 2) {
        if (w != 64 || h != 128) return;
        // The display: a line of green text (rows 81-82, x 20..39) that changes, a bar over it
        // (row 77) that rises and falls.
        const int x0 = 20, n = 20;
        uint8_t bgc = at(x0, 81);
        int dark = 1 << 30;
        for (int y = 81; y <= 82; ++y)
            for (int x = x0; x < x0 + n; ++x) {
                uint8_t c = at(x, y);
                int v = rgb(c, 0) + rgb(c, 1) + rgb(c, 2);
                if (cls(c) != 'g' && v < dark) { dark = v; bgc = c; }
            }
        uint32_t r = idleHash((uint32_t)std::floor(T / 1.4f), (uint32_t)g, 5);
        int shift = (int)(r % n), len = 9 + (int)((r >> 8) % 12);
        for (int y = 81; y <= 82; ++y) {
            for (int x = x0; x < x0 + n; ++x)
                if (cls(at(x, y)) == 'g') put(x, y, bgc);
            for (int x = 0; x < std::min(len, n); ++x) {
                uint8_t c = at(x0 + (x + shift) % n, y);
                if (cls(c) == 'g') put(x0 + x, y, c);
            }
        }
        float level = 0.6f + 0.3f * std::sin(T * 0.8f + g) + 0.1f * std::sin(T * 2.9f);
        int fill = (int)std::lround(std::clamp(level, 0.0f, 1.0f) * 21);
        for (int x = 22; x <= 42; ++x)
            if (cls(at(x, 77)) == 'g' && x - 22 >= fill) put(x, 77, idleDim_[at(x, 77)]);
    } else if (kind >= 4) {
        // Star screens: the stars twinkle. The sky is the screens' black; a star is a cluster of
        // grey pixels (a point, or a cross: a bright middle and dimmer arms) inside a screen, each
        // brightening and fading at its own pace, its arms fading first, so a cross shrinks to a
        // point and grows back; the faint dark-blue specks come and go. Grids, poles, globes and
        // the frames (coloured, or outside the black) are left alone.
        auto lum = [&](uint8_t c) { return rgb(c, 0) + rgb(c, 1) + rgb(c, 2); };
        auto neutral = [&](uint8_t c) {
            int r = rgb(c, 0), gg = rgb(c, 1), b = rgb(c, 2);
            return std::abs(r - gg) <= 8 && std::abs(b - r) <= 8;
        };
        uint8_t sky = 0;
        int skyL = 1 << 30;
        for (uint8_t c : src)
            if (neutral(c) && lum(c) < skyL) { skyL = lum(c); sky = c; }
        const bool hasSky = skyL <= 30;   // (the tactical map has none: its screen is green)
        auto isSky = [&](int x, int y) { return hasSky && at(x, y) == sky; };
        // The screens: each patch of sky big enough to be one, as its bounding box.
        std::vector<int> label((size_t)w * h, -1);
        struct Box { int x0, y0, x1, y1, n; };
        std::vector<Box> boxes;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                if (label[y * w + x] >= 0 || !isSky(x, y)) continue;
                Box b{x, y, x, y, 0};
                std::vector<int> stack{y * w + x};
                label[y * w + x] = (int)boxes.size();
                while (!stack.empty()) {
                    int i = stack.back();
                    stack.pop_back();
                    int px = i % w, py = i / w;
                    b.x0 = std::min(b.x0, px); b.x1 = std::max(b.x1, px); b.y0 = std::min(b.y0, py); b.y1 = std::max(b.y1, py);
                    ++b.n;
                    const int nb[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                    for (const auto& d : nb) {
                        int qx = px + d[0], qy = py + d[1];
                        if (qx < 0 || qy < 0 || qx >= w || qy >= h || label[qy * w + qx] >= 0 || !isSky(qx, qy)) continue;
                        label[qy * w + qx] = (int)boxes.size();
                        stack.push_back(qy * w + qx);
                    }
                }
                boxes.push_back(b);
            }
        auto inScreen = [&](int x, int y) {
            for (const Box& b : boxes)
                if (b.n >= 120 && x > b.x0 && x < b.x1 && y > b.y0 && y < b.y1) return true;
            return false;
        };
        // The nearest grey to a brightness, for the stars; the specks' blue (or the sky).
        std::vector<uint8_t> greys;
        for (int c = 1; c < 256; ++c)
            if (neutral((uint8_t)c)) greys.push_back((uint8_t)c);
        auto grey = [&](int target) {
            uint8_t best = sky;
            int bestD = 1 << 30;
            for (uint8_t q : greys) {
                int d = std::abs(lum(q) - target);
                if (d < bestD) { bestD = d; best = q; }
            }
            return best;
        };
        std::vector<uint8_t> done((size_t)w * h, 0);
        for (int y = 1; y < h - 1; ++y)
            for (int x = 1; x < w - 1; ++x) {
                uint8_t c = at(x, y);
                if (done[y * w + x] || c == sky || !inScreen(x, y)) continue;
                const bool speck = !neutral(c) && rgb(c, 2) > rgb(c, 0) + 20 && lum(c) < 80;
                if (!neutral(c) && !speck) continue;
                if (speck) {   // a faint dot: there most of the time, out now and then
                    done[y * w + x] = 1;
                    uint32_t r = idleHash((uint32_t)(y * w + x), (uint32_t)kind, (uint32_t)std::floor((T + (x * 7 + y * 3) % 10 * 0.37f) / 2.3f));
                    if (r % 4 == 0) put(x, y, sky);
                    continue;
                }
                // A star: its grey pixels touching each other.
                std::vector<int> px{y * w + x};
                done[y * w + x] = 1;
                for (size_t k = 0; k < px.size(); ++k) {
                    int qx = px[k] % w, qy = px[k] / w;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            int sx = qx + dx, sy = qy + dy;
                            if (sx < 0 || sy < 0 || sx >= w || sy >= h || done[sy * w + sx]) continue;
                            uint8_t sc = at(sx, sy);
                            if (sc == sky || !neutral(sc) || !inScreen(sx, sy)) continue;
                            done[sy * w + sx] = 1;
                            px.push_back(sy * w + sx);
                        }
                }
                if (px.size() > 24) continue;   // too big for a star: part of the picture
                int peak = 0;
                for (int i : px) peak = std::max(peak, lum(src[i]));
                uint32_t r = idleHash((uint32_t)px[0], (uint32_t)kind, 77);
                float speed = 1.2f + (float)(r % 100) / 100.0f * 2.6f, phase = (float)((r >> 8) % 628) / 100.0f;
                float f = 0.55f + 0.6f * (0.5f + 0.5f * std::sin(T * speed + phase));   // 0.55 to 1.15
                if (((r >> 20) % 5) == 0) f *= 0.85f + 0.3f * std::max(0.0f, std::sin(T * 7.0f + phase));   // a few sparkle
                for (int i : px) {
                    int l = lum(src[i]);
                    // The middle keeps its light longest; dimmer pixels (a cross's arms) go first.
                    float own = l >= peak ? f : f * f * f;
                    put(i % w, i / w, own < 0.18f ? sky : grey((int)(l * std::min(1.25f, own * 1.15f))));
                }
            }
        // The rest by the world's clock alone, so the two halves of a screen keep together.
        const float Ts = t;
        auto nearest = [&](int r, int gg, int b) {   // the palette's nearest colour
            uint8_t best = 0;
            int bestD = 1 << 30;
            for (int c = 1; c < 256; ++c) {
                int dr = rgb(c, 0) - r, dg = rgb(c, 1) - gg, db = rgb(c, 2) - b, d = dr * dr + dg * dg + db * db;
                if (d < bestD) { bestD = d; best = (uint8_t)c; }
            }
            return best;
        };
        std::map<int, uint8_t> scaled;   // (colour, f in hundredths) -> colour
        auto scale = [&](uint8_t c, float f) {
            int key = c * 1000 + std::clamp((int)std::lround(f * 100), 0, 999);
            auto it = scaled.find(key);
            if (it != scaled.end()) return it->second;
            uint8_t q = nearest(std::min(252, (int)(rgb(c, 0) * f)), std::min(252, (int)(rgb(c, 1) * f)), std::min(252, (int)(rgb(c, 2) * f)));
            scaled[key] = q;
            return q;
        };
        auto bluish = [&](uint8_t c) { return rgb(c, 2) > rgb(c, 0) + 20 && std::abs(rgb(c, 0) - rgb(c, 1)) <= 8 && lum(c) >= 150; };
        if (kind == 6 || kind == 7) {
            // The grid over the planet: a bright band runs down the grid from the horizon, so it
            // seems to stream toward you; the poles' beacons pulse and their red feet blink.
            int gy0 = h, gy1 = -1;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    if (cls(at(x, y)) == 'g') { gy0 = std::min(gy0, y); gy1 = std::max(gy1, y); }
            if (gy1 > gy0) {
                const float span = (float)(gy1 - gy0 + 6), yw = gy0 - 3 + std::fmod(Ts * 9.0f, span);
                for (int y = gy0; y <= gy1; ++y) {
                    float d = (y - yw) / 1.6f, f = 0.85f + 0.55f * std::exp(-d * d);
                    for (int x = 0; x < w; ++x)
                        if (cls(at(x, y)) == 'g') put(x, y, scale(at(x, y), f));
                }
            }
            for (int y = 1; y < h - 1; ++y)
                for (int x = 1; x < w - 1; ++x) {
                    uint8_t c = at(x, y);
                    if (cls(c) == 'r' && y >= gy0) {   // a pole's red foot
                        if (std::fmod(Ts + x * 0.13f, 1.2f) > 0.7f) put(x, y, idleDim_[c]);
                    } else if (bluish(c) && lum(c) >= 380 && y >= gy0 - 6) {   // a beacon: the brightest pixel of its 3 x 3
                        bool top = true;
                        for (int dy = -1; dy <= 1 && top; ++dy)
                            for (int dx = -1; dx <= 1; ++dx)
                                if ((dx || dy) && lum(at(x + dx, y + dy)) > lum(c)) { top = false; break; }
                        if (!top) continue;
                        float f = 0.75f + 0.5f * (0.5f + 0.5f * std::sin(Ts * 4.0f + x * 0.9f));
                        for (int dy = -1; dy <= 1; ++dy)
                            for (int dx = -1; dx <= 1; ++dx) {
                                uint8_t q = at(x + dx, y + dy);
                                if (rgb(q, 2) > rgb(q, 0) + 15) put(x + dx, y + dy, scale(q, dx || dy ? f * f : f));
                            }
                    }
                }
        } else if (kind == 8 || kind == 9) {
            // The console: the red globe turns (a light sweeps round its wireframe, across both
            // halves), the blue display runs a line of text, and the two green lamps blink in turn.
            const float X0 = kind == 9 ? 64.0f : 0.0f;   // the right half carries on from the left
            const float xb = 50 + std::fmod(Ts * 14.0f, 64.0f);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    uint8_t c = at(x, y);
                    if (cls(c) != 'r') continue;
                    float d = (X0 + x - xb) / 7.0f, f = 0.6f + 0.65f * std::exp(-d * d);
                    put(x, y, scale(c, f));
                }
            for (int y = 0; y < h; ++y) {   // the blue display: rows of blue in the panel
                int x0 = -1, x1 = -1;
                for (int x = 0; x < w; ++x)
                    if (cls(at(x, y)) == 'b' && lum(at(x, y)) > 60) { if (x0 < 0) x0 = x; x1 = x; }
                if (x0 < 0 || x1 - x0 < 8 || y < 96) continue;
                uint8_t lo = at(x0, y);
                for (int x = x0; x <= x1; ++x)
                    if (lum(at(x, y)) < lum(lo)) lo = at(x, y);
                const uint8_t lit = scale(lo, 1.7f);
                const int scroll = (int)std::floor(Ts * 6);
                for (int x = x0; x <= x1; ++x) {
                    int u = x - x0 + scroll, ch = u / 4;
                    uint32_t r = idleHash((uint32_t)ch, (uint32_t)kind, 23);
                    bool on = u % 4 != 3 && r % 7 != 0 && ((r >> ((u % 4) * 3 + (y & 3))) & 1);
                    put(x, y, on ? lit : lo);
                }
            }
            const bool first = std::fmod(Ts, 1.4f) < 0.7f;
            if (first == (kind == 8))
                for (int y = 96; y < h; ++y)
                    for (int x = 0; x < w; ++x)
                        if (cls(at(x, y)) == 'g') put(x, y, idleDim_[at(x, y)]);
        } else if (kind == 10 || kind == 11) {
            // The tactical map: a scan band sweeps down it (both halves together), the blips pulse,
            // each at its own pace, and the green marks flash now and then.
            uint8_t bg = 0;
            {
                int count[256] = {};
                for (uint8_t c : src)
                    if (cls(c) == 'g') ++count[c];
                for (int c = 1; c < 256; ++c)
                    if (count[c] > count[bg]) bg = (uint8_t)c;
            }
            int my0 = h, my1 = -1, mark = 0;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    if (at(x, y) == bg) { my0 = std::min(my0, y); my1 = std::max(my1, y); }
            for (uint8_t c : src)
                if (cls(c) == 'g') mark = std::max(mark, lum(c));
            if (my1 > my0) {
                const float span = (float)(my1 - my0 + 10), ys = my0 - 5 + std::fmod(Ts * 11.0f, span);
                for (int y = my0; y <= my1; ++y) {
                    float d = y - ys;
                    float f = d > 0.5f ? 1.0f : 1.0f + 0.7f * std::exp(d / 3.0f);   // a bright edge, fading behind
                    if (f < 1.02f) continue;
                    for (int x = 0; x < w; ++x)
                        if (cls(at(x, y)) == 'g') put(x, y, scale(at(x, y), f));
                }
            }
            for (int y = 1; y < h - 1; ++y)
                for (int x = 1; x < w - 1; ++x) {
                    uint8_t c = at(x, y);
                    if (y < my0 || y > my1) continue;
                    if (bluish(c) && lum(c) >= 380) {   // a blip's middle
                        float f = 0.7f + 0.6f * (0.5f + 0.5f * std::sin(Ts * 3.0f + x * 1.7f + y * 0.6f));
                        for (int dy = -1; dy <= 1; ++dy)
                            for (int dx = -1; dx <= 1; ++dx) {
                                uint8_t q = at(x + dx, y + dy);
                                if (rgb(q, 2) > rgb(q, 0) + 15) put(x + dx, y + dy, scale(q, dx || dy ? f * f : f));
                            }
                    } else if (cls(c) == 'g' && lum(c) >= mark - 2) {   // a bright mark
                        uint32_t r = idleHash((uint32_t)(x / 3 + y / 3 * 32), (uint32_t)kind, (uint32_t)std::floor(Ts / 0.9f));
                        if (r % 3 == 0) put(x, y, scale(c, 0.6f));
                    }
                }
        }
    } else if (kind == 3) {
        if (w != 64 || h != 128) return;
        // The nearest colour of the same hue at f times the brightness.
        auto scale = [&](uint8_t c, float f) {
            int r = cls(c) == 'r' ? 0 : cls(c) == 'g' ? 1 : cls(c) == 'b' ? 2 : -1;
            if (r < 0 || std::abs(f - 1) < 0.02f) return c;
            int want[3] = {std::min(252, (int)(rgb(c, 0) * f)), std::min(252, (int)(rgb(c, 1) * f)), std::min(252, (int)(rgb(c, 2) * f))};
            uint8_t best = c;
            int bestD = 1 << 30;
            for (uint8_t q : idleRamp_[r]) {
                int d = 0;
                for (int ch = 0; ch < 3; ++ch) d += (rgb(q, ch) - want[ch]) * (rgb(q, ch) - want[ch]);
                if (d < bestD) { bestD = d; best = q; }
            }
            return best;
        };
        auto area = [&](int x0, int y0, int x1, int y1, char c, auto f) {
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                    if (cls(at(x, y)) == c) put(x, y, f(x, y, at(x, y)));
        };
        const bool taking = state == 2, done = state == 3;
        // The red lamp (rows 69-73, x 28-34) breathes, and goes out once the pod is in; the pilot
        // lights beside it, at (19, 71) and (43, 71), blink in turn.
        {
            float f = done ? 0.3f : taking ? (std::fmod(since, 0.3f) < 0.15f ? 1.25f : 0.5f)
                                           : 0.55f + 0.45f * (0.5f + 0.5f * std::sin(T * 2.4f));
            area(26, 67, 36, 75, 'r', [&](int, int, uint8_t c) { return scale(c, f); });
            const bool first = std::fmod(T, 1.1f) < 0.55f;
            if (cls(at(19, 71)) == 'r' && (!first || done)) put(19, 71, idleDim_[at(19, 71)]);
            if (cls(at(43, 71)) == 'r' && (first || done)) put(43, 71, idleDim_[at(43, 71)]);
        }
        // The hand-print screen (rows 81-95, x 21-42): a bright line sweeps down it, then the print
        // glows for a moment, and it rests a little darker until the next sweep. Taking a pod it
        // flashes; done, it glows, slowly brighter and dimmer.
        {
            const float cycle = state == 1 ? 3.0f : 4.2f, c = std::fmod(T, cycle), sweep = 1.6f;
            area(20, 80, 43, 96, 'b', [&](int, int y, uint8_t p) {
                float f;
                if (taking) f = std::fmod(since, 0.24f) < 0.12f ? 1.6f : 1.0f;
                else if (done) f = 1.15f + 0.15f * std::sin(T * 1.7f);
                else if (c < sweep) {
                    float d = std::abs(y - (80.5f + c / sweep * 15.0f));
                    f = d < 2.5f ? 1.0f + (2.5f - d) * 0.35f : 0.85f;
                } else if (c < sweep + 0.6f) f = 1.3f;
                else f = 0.85f;
                return scale(p, f);
            });
        }
        // The blue display (rows 103-106, x 5-24): a line of text moving along rows 104-105, a level
        // bar under it on row 106 (full once the pod is in).
        {
            const int x0 = 5, x1 = 24;
            if (cls(at(x0, 104)) == 'b' && cls(at(x1, 104)) == 'b') {
                uint8_t lo = at(x0, 104);
                for (int x = x0; x <= x1; ++x)
                    for (int y = 103; y <= 106; ++y)
                        if (cls(at(x, y)) == 'b' && idleRampAt_[at(x, y)] < idleRampAt_[lo]) lo = at(x, y);
                const uint8_t lit = scale(lo, 1.6f), mid = scale(lo, 1.3f);
                const int scroll = (int)std::floor(taking ? since * 30 : T * 6);
                for (int x = x0; x <= x1; ++x) {
                    int u = x - x0 + scroll, ch = u / 4;
                    uint32_t r = idleHash((uint32_t)ch, (uint32_t)g, 21);
                    bool gap = u % 4 == 3 || r % 7 == 0;   // between letters, or a space
                    for (int y = 104; y <= 105; ++y) {
                        bool on = !gap && ((r >> ((u % 4) * 2 + (y - 104) + 3)) & 1);
                        put(x, y, on ? lit : lo);
                    }
                }
                float level = done ? 1.0f : taking ? std::min(1.0f, since / 2.0f)
                                         : 0.55f + 0.3f * std::sin(T * 0.9f + g) + 0.15f * std::sin(T * 3.1f);
                int fill = (int)std::lround(std::clamp(level, 0.0f, 1.0f) * (x1 - x0 + 1));
                for (int x = x0; x <= x1; ++x) put(x, 106, x - x0 < fill ? mid : lo);
            }
        }
        // The green lamp (rows 103-105, x 52-58): blinking on its own; out while it waits for a
        // pod; quick as it takes one; steady once it has.
        {
            bool lit = done ? true : taking ? std::fmod(since, 0.2f) < 0.1f : state == 1 ? false
                                            : std::fmod(T + g * 0.37f, 1.6f) < 1.15f;
            if (!lit) area(50, 102, 60, 106, 'g', [&](int, int, uint8_t c) { return idleDim_[c]; });
        }
    }
}

int Renderer::idleKind(int number) {
    static const char* const lumps[kIdleKinds] = {"wall57", "wall359", "wall150", "wall93", "wall221",
                                                  "wall222", "wall227", "wall228", "wall258", "wall259",
                                                  "wall224", "wall225"};
    for (int k = 0; k < kIdleKinds; ++k) {
        if (idleWall_[k] == -2) {
            int lump = data_->cd.find(lumps[k]);
            idleWall_[k] = lump > data_->startWalls ? lump - data_->startWalls : -1;
        }
        if (number > 0 && number == idleWall_[k]) return k;
    }
    return -1;
}

void Renderer::updateIdleScreens(float time) {
    for (int i = 0; i < kIdleKinds * kIdleGroups; ++i) {
        if (!idleUsed_[i]) continue;
        auto it = walls_.find(kIdleScreen + i);
        if (it == walls_.end() || !it->second.id) continue;
        const int kind = i / kIdleGroups, group = i % kIdleGroups;
        int key = (int)std::floor(time * 10);
        if (key == idleKey_[i]) continue;
        const Tex* base = wallTexture(idleWall_[kind]);
        if (!base || base->px.empty()) continue;
        idleKey_[i] = key;
        Tex& t = it->second;
        std::vector<uint8_t> img = base->px;
        idleFrame(img, base->w, base->h, kind, group, time);
        glBindTexture(GL_TEXTURE_2D, t.id);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, t.w, t.h, GL_RED, GL_UNSIGNED_BYTE, img.data());
        t.px = std::move(img);
    }
}

// SDILAB's security scanners (an addition, not in the game). Each scan room's monitor is a block
// whose side shows rows 25-60 of wall287; its screen is empty texels. The block isn't flagged
// see-through, so the game draws it solid and the screen shows black; the engine showed the wall
// behind it up to 0.5.2. Here the screen is black until the room's script starts, then stands by,
// scans (a figure with a line sweeping down it and back, read-outs, a progress bar), and then
// holds a still picture: SCAN COMPLETE, then APPROVED. Colours are the palette's nearest greens.
namespace {
struct Glyph { char c; const char* rows[5]; };
const Glyph kScanFont[] = {
    {'A', {".#.", "#.#", "###", "#.#", "#.#"}}, {'B', {"##.", "#.#", "##.", "#.#", "##."}},
    {'C', {"###", "#..", "#..", "#..", "###"}}, {'D', {"##.", "#.#", "#.#", "#.#", "##."}},
    {'E', {"###", "#..", "##.", "#..", "###"}}, {'G', {"###", "#..", "#.#", "#.#", "###"}},
    {'I', {"###", ".#.", ".#.", ".#.", "###"}}, {'L', {"#..", "#..", "#..", "#..", "###"}},
    {'M', {"#...#", "##.##", "#.#.#", "#...#", "#...#"}}, {'N', {"##.", "#.#", "#.#", "#.#", "#.#"}},
    {'O', {"###", "#.#", "#.#", "#.#", "###"}}, {'P', {"###", "#.#", "###", "#..", "#.."}},
    {'R', {"##.", "#.#", "##.", "#.#", "#.#"}}, {'S', {"###", "#..", "###", "..#", "###"}},
    {'T', {"###", ".#.", ".#.", ".#.", ".#."}}, {'V', {"#.#", "#.#", "#.#", "#.#", ".#."}},
    {'Y', {"#.#", "#.#", ".#.", ".#.", ".#."}}, {'H', {"#.#", "#.#", "###", "#.#", "#.#"}},
    {'U', {"#.#", "#.#", "#.#", "#.#", "###"}}, {'Q', {"###", "#.#", "#.#", "###", "..#"}},
    {' ', {"..", "..", "..", "..", ".."}},
};
// The words, in that font (3 wide, M 5): their width, and their dots.
int scanTextWidth(const char* word) {
    int w = -1;
    for (const char* p = word; *p; ++p)
        for (const auto& g : kScanFont)
            if (g.c == *p) w += (int)std::strlen(g.rows[0]) + 1;
    return std::max(0, w);
}
template <class Dot> void scanText(const char* word, int x, int y, Dot dot) {
    for (const char* p = word; *p; ++p)
        for (const auto& g : kScanFont)
            if (g.c == *p) {
                int gw = (int)std::strlen(g.rows[0]);
                for (int r = 0; r < 5; ++r)
                    for (int k = 0; k < gw; ++k)
                        if (g.rows[r][k] == '#') dot(x + k, y + r);
                x += gw + 1;
            }
}
// The one being scanned, from the front: 14 x 26.
const char* const kScanBody[26] = {
    ".....####.....", "....######....", "....######....", "....######....", ".....####.....",
    "......##......", "..##########..", ".############.", ".############.", "##.########.##",
    "##.########.##", "##.########.##", "##.########.##", "##.########.##", "##..######..##",
    "#...######...#", "....######....", "....##..##....", "....##..##....", "....##..##....",
    "....##..##....", "....##..##....", "....##..##....", "....##..##....", "...###..###...",
    "...###..###...",
};
}  // namespace

void Renderer::scanFrame(std::vector<uint8_t>& px, int w, int h, const ScanScreen& s) {
    const uint8_t* pal = data_->palette.data();
    auto nearest = [&](int r, int g, int b) {
        int best = 1, bestD = 1 << 30;
        for (int i = 1; i < 256; ++i) {
            int dr = (pal[i * 3] & 63) * 4 - r, dg = (pal[i * 3 + 1] & 63) * 4 - g, db = (pal[i * 3 + 2] & 63) * 4 - b;
            int d = 2 * dr * dr + 4 * dg * dg + db * db;
            if (d < bestD) { bestD = d; best = i; }
        }
        return (uint8_t)best;
    };
    // The screen: the texture's empty texels.
    std::vector<uint8_t> hole(px.size(), 0);
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (px[(size_t)y * w + x] == 0) {
                hole[(size_t)y * w + x] = 1;
                x0 = std::min(x0, x); x1 = std::max(x1, x);
                y0 = std::min(y0, y); y1 = std::max(y1, y);
            }
    if (x1 < 0) return;
    auto put = [&](int x, int y, uint8_t c) {
        if (x >= 0 && y >= 0 && x < w && y < h && hole[(size_t)y * w + x]) px[(size_t)y * w + x] = c;
    };
    if (s.phase <= 0) return;   // off: the empty texels, drawn as the game draws them (the block is solid)
    const uint8_t black = nearest(0, 0, 0);
    for (size_t i = 0; i < px.size(); ++i)
        if (hole[i]) px[i] = black;
    const uint8_t bg = nearest(6, 24, 10), line = nearest(12, 40, 18), dim = nearest(26, 88, 42),
                  mid = nearest(46, 160, 74), bright = nearest(120, 236, 130), white = nearest(226, 255, 226);
    const float t = s.t;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) put(x, y, ((y - y0) & 1) ? line : bg);
    if (s.phase == 1 && t < 0.18f) {   // switching on: a bright line opens out into the picture
        int cy = (y0 + y1) / 2, half = (int)(t / 0.18f * (y1 - y0) / 2);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                if (std::abs(y - cy) > half) put(x, y, black);
                else if (std::abs(y - cy) == half) put(x, y, white);
        return;
    }
    // The figure, left.
    const int bx = x0 + 2, by = y0 + 1;
    float sweep = -10;   // the scan line's row on the figure (0 the top of the head, 25 the feet)
    if (s.phase == 2) {
        float u = std::clamp(t / std::max(0.05f, s.dur), 0.0f, 1.0f);
        sweep = (0.5f - 0.5f * std::cos(u * 6.2831853f)) * 25.0f;
    }
    auto body = [&](int r, int c) { return r >= 0 && r < 26 && c >= 0 && c < 14 && kScanBody[r][c] == '#'; };
    for (int r = 0; r < 26; ++r)
        for (int c = 0; c < 14; ++c) {
            if (!body(r, c)) continue;
            bool edge = !body(r - 1, c) || !body(r + 1, c) || !body(r, c - 1) || !body(r, c + 1);
            uint8_t col;
            if (s.phase >= 3) col = edge ? bright : dim;
            else if (s.phase == 2) {
                float d = std::abs(r - sweep);
                col = d < 1.5f ? (edge ? white : bright) : d < 4.0f ? (edge ? bright : mid) : (edge ? mid : dim);
            } else col = edge ? dim : line;
            put(bx + c, by + r, col);
        }
    if (s.phase == 2) {
        int r = (int)std::lround(sweep);
        for (int c = -1; c <= 14; ++c)
            if (!body(r, c)) put(bx + c, by + r, (c & 1) ? bright : mid);
    }
    // A dotted divider, then the words, the read-outs and the progress bar on the right.
    const int dx = bx + 15, tx0 = dx + 1, tw = x1 - tx0 + 1;
    for (int y = y0 + 2; y <= y1 - 2; y += 2) put(dx, y, dim);
    const char* word = s.phase == 1 ? "STANDBY" : s.phase == 2 ? "SCANNING" : s.phase == 3 ? "COMPLETE" : "APPROVED";
    bool shown = s.phase == 1 ? std::fmod(t, 0.5f) < 0.32f : s.phase == 2 ? std::fmod(t, 0.3f) < 0.2f : true;
    if (shown) {
        int x = tx0 + std::max(0, (tw - scanTextWidth(word)) / 2);
        scanText(word, x, y0 + 2, [&](int px_, int py_) { put(px_, py_, s.phase == 4 ? white : bright); });
    }
    const int full = tw - 3;
    for (int i = 0; i < 5; ++i) {
        int n = 0;
        if (s.phase == 2) {
            uint32_t q = (uint32_t)i * 73856093u ^ (uint32_t)(t * 12) * 19349663u;
            q ^= q >> 13; q *= 0x5bd1e995u; q ^= q >> 15;
            n = 4 + (int)(q % (uint32_t)std::max(1, full - 4));
        } else if (s.phase >= 3) {
            n = full * (5 + (i * 7) % 5) / 10;
        }
        for (int x = 0; x < n; ++x) put(tx0 + 2 + x, y0 + 9 + i * 2, mid);
    }
    const int py0 = y1 - 5, py1 = y1 - 1;
    const int bx0 = tx0 + 1;   // the bar keeps clear of the divider
    for (int x = bx0; x <= x1; ++x) { put(x, py0, dim); put(x, py1, dim); }
    for (int y = py0; y <= py1; ++y) { put(bx0, y, dim); put(x1, y, dim); }
    float f = s.phase == 2 ? std::clamp(t / std::max(0.05f, s.dur), 0.0f, 1.0f) : s.phase >= 3 ? 1.0f : 0.0f;
    int fill = (int)std::lround(f * (x1 - bx0 - 1));
    for (int x = bx0 + 1; x < bx0 + 1 + fill; ++x)
        for (int y = py0 + 1; y < py1; ++y) put(x, y, s.phase == 4 ? bright : mid);
}

void Renderer::updateScanScreens(const std::vector<ScanScreen>& screens) {
    for (int kind = 0; kind < 2; ++kind) {
        const int first = kind ? kConsoleScreen : kScanScreen, n = kind ? kConsoleScreens : kScanScreens;
        int* keys = kind ? consoleKey_ : scanKey_;
        int mw = kind ? consoleWall() : scanMonitorWall();
        if (mw <= 0) continue;
        std::vector<ScanScreen> want((size_t)n);
        for (int i = 0; i < n; ++i) { want[i].monitor = i; want[i].kind = kind; }
        for (const auto& s : screens)
            if (s.kind == kind && s.monitor >= 0 && s.monitor < n) want[s.monitor] = s;
        const Tex* base = nullptr;
        for (int i = 0; i < n; ++i) {
            auto it = walls_.find(first + i);
            if (it == walls_.end() || !it->second.id) continue;   // no such screen in this level
            const ScanScreen& s = want[i];
            bool moving = kind ? s.phase == 1 : (s.phase == 1 || s.phase == 2);
            int key = s.phase * 100000 + (moving ? (int)(s.t * 30) : 0);
            if (key == keys[i]) continue;
            if (!base && !(base = wallTexture(mw))) break;
            keys[i] = key;
            Tex& t = it->second;
            std::vector<uint8_t> img = base->px;
            if (kind) missileFrame(img, base->w, base->h, s);
            else scanFrame(img, base->w, base->h, s);
            glBindTexture(GL_TEXTURE_2D, t.id);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, t.w, t.h, GL_RED, GL_UNSIGNED_BYTE, img.data());
            t.px = std::move(img);
        }
    }
    // The aliens' pod computers that take a pod: running all the time, by the world's clock.
    if (int pw = podWall(); pw > 0) {
        const Tex* base = nullptr;
        for (int i = 0; i < kPodScreens; ++i) {
            auto it = walls_.find(kPodScreen + i);
            if (it == walls_.end() || !it->second.id) continue;
            ScanScreen s;
            s.kind = 3;
            s.monitor = i;
            for (const auto& x : screens)
                if (x.kind == 3 && x.monitor == i) s = x;
            int key = s.phase * 1000000 + (int)(s.phase == 1 ? s.t * 30 : s.clock * 10) % 1000000;
            if (key == podKey_[i]) continue;
            if (!base && !(base = wallTexture(pw))) break;
            podKey_[i] = key;
            Tex& t = it->second;
            std::vector<uint8_t> img = base->px;
            idleFrame(img, base->w, base->h, 3, i % kIdleGroups, s.clock, s.phase + 1, s.t);
            glBindTexture(GL_TEXTURE_2D, t.id);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, t.w, t.h, GL_RED, GL_UNSIGNED_BYTE, img.data());
            t.px = std::move(img);
        }
    }
    // SDI2's launch computer: both halves at once.
    auto li = walls_.find(kLaunchScreen), ri = walls_.find(kLaunchScreen + 1);
    if (li != walls_.end() && ri != walls_.end() && li->second.id && ri->second.id) {
        ScanScreen s;
        s.kind = 2;
        for (const auto& x : screens)
            if (x.kind == 2) s = x;
        int key = s.phase * 100000 + (s.phase == 1 ? (int)(s.t * 30) : s.phase == 2 ? (int)(s.t * 10) : 0);
        const Tex* lb = wallTexture(launchWall(0));
        const Tex* rb = wallTexture(launchWall(1));
        if (key != launchKey_ && lb && rb && lb->w == 64 && rb->w == 64 && lb->h == 128 && rb->h == 128) {
            launchKey_ = key;
            std::vector<uint8_t> l = lb->px, r = rb->px;
            launchFrame(l, r, s);
            for (auto* half : {&li->second, &ri->second}) {
                glBindTexture(GL_TEXTURE_2D, half->id);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, half->w, half->h, GL_RED, GL_UNSIGNED_BYTE, half == &li->second ? l.data() : r.data());
                half->px = half == &li->second ? l : r;
            }
        }
    }
}

int Renderer::launchWall(int half) {
    if (half < 0 || half > 1) return -1;
    if (launchWall_[half] == -2) {
        int lump = data_->cd.find(half ? "wall330" : "wall329");
        launchWall_[half] = lump > data_->startWalls ? lump - data_->startWalls : -1;
    }
    return launchWall_[half];
}

// SDI2's launch computer (an addition, not in the game): the west wall of the launch room, a big
// monitor over three levers across two walls (wall329 and wall330: columns 0-127 of the pair, the
// glass in 18-104 x 17-44). The game leaves its grey screen and its levers as they are. Here, as
// the launch codes go into the console across the room (the script printing "Missile Launch
// Sequence INITIATED"), the monitor switches on: LAUNCH CODES typed in, ACCEPTED; ARMING while the
// three levers go up one after another, a box lighting for each; then SEQUENCE INITIATED, three
// lit boxes and ARMED, while a flight path is plotted from the silo across to the target, which
// locks on. After that (and in a saved game) it holds that picture with the path's dashes
// marching and the target blinking. Text in the scanners' green, the path in the consoles' red.
void Renderer::launchFrame(std::vector<uint8_t>& lpx, std::vector<uint8_t>& rpx, const ScanScreen& s) {
    if (s.phase <= 0 || lpx.size() < 64 * 128 || rpx.size() < 64 * 128) return;   // off: as the game draws it
    const int W = 128;
    std::vector<uint8_t> img((size_t)W * 128);
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 64; ++x) {
            img[(size_t)y * W + x] = lpx[(size_t)y * 64 + x];
            img[(size_t)y * W + 64 + x] = rpx[(size_t)y * 64 + x];
        }
    const std::vector<uint8_t> src = img;
    const uint8_t* pal = data_->palette.data();
    auto nearest = [&](int r, int g, int b) {
        int best = 1, bestD = 1 << 30;
        for (int i = 1; i < 256; ++i) {
            int dr = (pal[i * 3] & 63) * 4 - r, dg = (pal[i * 3 + 1] & 63) * 4 - g, db = (pal[i * 3 + 2] & 63) * 4 - b;
            int d = 4 * dr * dr + 2 * dg * dg + db * db;
            if (d < bestD) { bestD = d; best = i; }
        }
        return (uint8_t)best;
    };
    const uint8_t black = nearest(0, 0, 0), bg = nearest(4, 14, 6), line = nearest(8, 24, 10),
                  green = nearest(70, 230, 90), dim = nearest(24, 96, 34), red = nearest(200, 24, 20),
                  bright = nearest(255, 96, 64), white = nearest(240, 255, 230);
    const int gx0 = 18, gx1 = 104, gy0 = 17, gy1 = 44;
    auto inside = [&](int x, int y) {   // the glass, its corners rounded
        if (x < gx0 || x > gx1 || y < gy0 || y > gy1) return false;
        int dx = std::max(0, std::max(gx0 + 4 - x, x - (gx1 - 4))), dy = std::max(0, std::max(gy0 + 4 - y, y - (gy1 - 4)));
        return dx * dx + dy * dy <= 16;
    };
    auto put = [&](int x, int y, uint8_t c) { if (inside(x, y)) img[(size_t)y * W + x] = c; };
    const float t = s.phase == 1 ? s.t : kLaunchShow + s.t;   // phase 2 carries on from where 1 ended

    // The levers (handles 7 wide, 3 rows, on slots at x 40, 52 and 64) go up one after another,
    // from where the picture has them (72, 67, 72) to the top of their slots (66).
    const int lx[3] = {40, 52, 64}, ly0[3] = {72, 67, 72}, clean[3] = {68, 71, 68}, ltop = 66;
    float lever[3];
    for (int k = 0; k < 3; ++k) {
        lever[k] = std::clamp((t - (1.5f + 0.4f * k)) / 0.3f, 0.0f, 1.0f);
        int hy = (int)std::lround(ly0[k] + (ltop - ly0[k]) * lever[k]);
        if (hy == ly0[k]) continue;
        for (int y = ly0[k] - 2; y <= ly0[k]; ++y)
            for (int x = lx[k] - 3; x <= lx[k] + 3; ++x) img[(size_t)y * W + x] = src[(size_t)clean[k] * W + x];
        for (int r = 0; r < 3; ++r)
            for (int d = -3; d <= 3; ++d) img[(size_t)(hy - 2 + r) * W + lx[k] + d] = src[(size_t)(70 + r) * W + 40 + d];
    }

    for (int y = gy0; y <= gy1; ++y)
        for (int x = gx0; x <= gx1; ++x) put(x, y, ((y - gy0) & 1) ? line : bg);
    if (t < 0.15f) {   // switching on: a bright line opens out into the picture
        int cy = (gy0 + gy1) / 2, half = (int)(t / 0.15f * (gy1 - gy0) / 2);
        for (int y = gy0; y <= gy1; ++y)
            for (int x = gx0; x <= gx1; ++x)
                if (std::abs(y - cy) > half) put(x, y, black);
                else if (std::abs(y - cy) == half) put(x, y, white);
    } else {
        auto text = [&](const std::string& w, int x, int y, uint8_t c) {
            scanText(w.c_str(), x, y, [&](int px, int py) { put(px, py, c); });
        };
        auto centred = [&](const std::string& w, int y, uint8_t c) { text(w, (gx0 + gx1 + 1 - scanTextWidth(w.c_str())) / 2, y, c); };
        auto typed = [&](const std::string& w, float from, float perChar) {
            int n = (int)std::floor((t - from) / perChar);
            return w.substr(0, (size_t)std::clamp(n, 0, (int)w.size()));
        };
        auto box = [&](int x, int y, int size, bool lit) {
            for (int j = 0; j < size; ++j)
                for (int i = 0; i < size; ++i) {
                    bool edge = i == 0 || j == 0 || i == size - 1 || j == size - 1;
                    if (edge || lit) put(x + i, y + j, lit ? (edge ? white : green) : dim);
                }
        };
        if (t < 1.45f) {
            centred(typed("LAUNCH CODES", 0.15f, 0.05f), 22, green);
            if (t >= 0.85f && (t >= 1.3f || std::fmod(t - 0.85f, 0.2f) < 0.12f)) centred("ACCEPTED", 31, white);
        } else if (t < 2.75f) {
            if (std::fmod(t, 0.4f) < 0.26f) centred("ARMING", 22, green);
            for (int k = 0; k < 3; ++k) box(46 + 12 * k, 31, 7, lever[k] >= 1);
        } else {
            text(typed("SEQUENCE", 2.75f, 0.035f), 22, 20, green);
            text(typed("INITIATED", 3.05f, 0.035f), 22, 27, green);
            for (int k = 0; k < 3; ++k) box(22 + 5 * k, 36, 3, true);
            if (t >= 3.4f) text("ARMED", 38, 35, white);
            // The flight path: from the silo (63, 41) over the top (y 21) to the target (100, 40),
            // plotted as it goes, then its dashes marching.
            const float ax = 63, ay = 41, cx = 81.5f, cy = 1, bx = 100, by = 40;
            const int n = 64;
            float shown = std::clamp((t - 3.5f) / 1.2f, 0.0f, 1.0f);
            int march = (int)(t * 8);
            for (int i = 0; i <= n; ++i) {
                float u = (float)i / n;
                if (u > shown) break;
                if (shown >= 1 && (i + 3 - march % 3) % 3 == 0) continue;
                float x = (1 - u) * (1 - u) * ax + 2 * (1 - u) * u * cx + u * u * bx;
                float y = (1 - u) * (1 - u) * ay + 2 * (1 - u) * u * cy + u * u * by;
                bool head = shown < 1 && u > shown - 0.04f;
                put((int)std::lround(x), (int)std::lround(y), head ? white : red);
            }
            for (int i = -1; i <= 1; ++i) put(63 + i, 42, bright);   // the silo
            if (t >= 4.7f && std::fmod(t - 4.7f, 0.5f) < 0.34f)       // the target, locked on
                for (int d = -2; d <= 2; ++d) {
                    if (d == 0) continue;
                    put(100 + d, 40, bright);
                    put(100, 40 + d, bright);
                }
        }
    }
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 64; ++x) {
            lpx[(size_t)y * 64 + x] = img[(size_t)y * W + x];
            rpx[(size_t)y * 64 + x] = img[(size_t)y * W + 64 + x];
        }
}

int Renderer::podWall() {
    idleKind(0);   // looks the kinds' walls up
    return idleWall_[3];
}

int Renderer::consoleWall() {
    if (consoleWall_ == -2) {
        int lump = data_->cd.find("wall360");
        consoleWall_ = lump > data_->startWalls ? lump - data_->startWalls : -1;
    }
    return consoleWall_;
}

// SDI1's missile consoles (an addition, not in the game). Nine walls showing wall360 arm a missile
// when used (scripts 4-8 and 10-13: "CAUTION", "Missile Armed", counter 1 up 15, a creature brought
// in; seven of them open the silo bay). The game's monitor stays as it is, a grey screen. Here its
// glass (columns 12-49, rows 53-64) switches on, a red outline of a missile is traced from tail to
// nose and filled while ARMING blinks, and then it holds the armed missile and ARMED.
namespace {
// The missile, side on, nose to the right: 28 x 6 (fins at the tail and amidships, a tapered nose).
const char* const kMissile[6] = {
    "XXX.......XX................",
    "XXXXXXXXXXXXXXXXXXXXXXX.....",
    "XXXXXXXXXXXXXXXXXXXXXXXXXXX.",
    "XXXXXXXXXXXXXXXXXXXXXXXXXXX.",
    "XXXXXXXXXXXXXXXXXXXXXXX.....",
    "XXX.......XX................",
};
}  // namespace

void Renderer::missileFrame(std::vector<uint8_t>& px, int w, int h, const ScanScreen& s) {
    if (s.phase <= 0 || w != 64 || h != 128 || (int)px.size() < w * h) return;   // off: as the game draws it
    const uint8_t* pal = data_->palette.data();
    auto nearest = [&](int r, int g, int b) {
        int best = 1, bestD = 1 << 30;
        for (int i = 1; i < 256; ++i) {
            int dr = (pal[i * 3] & 63) * 4 - r, dg = (pal[i * 3 + 1] & 63) * 4 - g, db = (pal[i * 3 + 2] & 63) * 4 - b;
            int d = 4 * dr * dr + 2 * dg * dg + db * db;
            if (d < bestD) { bestD = d; best = i; }
        }
        return (uint8_t)best;
    };
    const int x0 = 12, x1 = 49, y0 = 53, y1 = 64;
    auto inside = [&](int x, int y) {   // the glass, less its rounded corners
        if (x < x0 || x > x1 || y < y0 || y > y1) return false;
        bool cx = x == x0 || x == x1, cy = y == y0 || y == y1;
        return !(cx && cy);
    };
    auto put = [&](int x, int y, uint8_t c) { if (inside(x, y)) px[(size_t)y * w + x] = c; };
    const uint8_t black = nearest(0, 0, 0), bg = nearest(14, 4, 4), line = nearest(26, 8, 8), dim = nearest(96, 14, 12),
                  red = nearest(200, 24, 20), bright = nearest(255, 96, 64), white = nearest(255, 220, 190);
    const float t = s.t;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) put(x, y, ((y - y0) & 1) ? line : bg);
    if (s.phase == 1 && t < 0.15f) {   // switching on: a bright line opens out into the picture
        int cy = (y0 + y1) / 2, half = (int)(t / 0.15f * (y1 - y0) / 2);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                if (std::abs(y - cy) > half) put(x, y, black);
                else if (std::abs(y - cy) == half) put(x, y, white);
        return;
    }
    auto body = [&](int r, int c) { return r >= 0 && r < 6 && c >= 0 && c < 28 && kMissile[r][c] == 'X'; };
    const int mx = x0 + 5, my = y0;
    // Arming: the outline traced tail to nose, then the body filled; armed: all of it.
    float trace = 27, fill = 27;
    if (s.phase == 1) {
        trace = std::clamp((t - 0.15f) / 0.7f, 0.0f, 1.0f) * 27;
        fill = std::clamp((t - 0.9f) / std::max(0.1f, s.dur - 1.1f), 0.0f, 1.0f) * 27 - 1;
    }
    for (int r = 0; r < 6; ++r)
        for (int c = 0; c < 28; ++c) {
            if (!body(r, c)) continue;
            bool edge = !body(r - 1, c) || !body(r + 1, c) || !body(r, c - 1) || !body(r, c + 1);
            if (edge) {
                if (c > trace) continue;
                bool head = s.phase == 1 && trace < 27 && c >= trace - 1;
                put(mx + c, my + r, head ? white : s.phase == 2 ? bright : red);
            } else if (c <= fill) {
                put(mx + c, my + r, s.phase == 2 ? red : dim);
            }
        }
    const char* word = s.phase == 1 ? "ARMING" : "ARMED";
    bool shown = s.phase == 2 || std::fmod(t, 0.4f) < 0.26f;
    if (shown)
        scanText(word, (x0 + x1 + 1 - scanTextWidth(word)) / 2, y0 + 7,
                 [&](int px_, int py_) { put(px_, py_, s.phase == 2 ? bright : red); });
}

int Renderer::scanMonitorWall() {
    if (scanWall_ == -2) {
        int lump = data_->cd.find("wall287");
        scanWall_ = lump > data_->startWalls ? lump - data_->startWalls : -1;
    }
    return scanWall_;
}

const Renderer::Tex* Renderer::wallTexture(int number) {
    auto it = walls_.find(number);
    if (it != walls_.end()) return it->second.id ? &it->second : nullptr;
    if (number >= kScanScreen && number < kScanScreen + kScanScreens) {
        Tex t;
        int mw = scanMonitorWall();
        const Tex* src = mw > 0 ? wallTexture(mw) : nullptr;
        if (src && !src->px.empty()) {
            std::vector<uint8_t> img = src->px;
            ScanScreen off;
            off.monitor = number - kScanScreen;
            scanFrame(img, src->w, src->h, off);
            t.id = makeTexture(src->w, src->h, GL_R8, GL_RED, img.data(), true);
            t.w = src->w;
            t.h = src->h;
            t.px = std::move(img);
        }
        scanKey_[number - kScanScreen] = 0;
        walls_[number] = t;
        return t.id ? &walls_[number] : nullptr;
    }
    if (number >= kConsoleScreen && number < kConsoleScreen + kConsoleScreens) {
        Tex t;
        int cw = consoleWall();
        const Tex* src = cw > 0 ? wallTexture(cw) : nullptr;
        if (src && !src->px.empty()) {
            t.id = makeTexture(src->w, src->h, GL_R8, GL_RED, src->px.data(), true);
            t.w = src->w;
            t.h = src->h;
            t.px = src->px;
        }
        consoleKey_[number - kConsoleScreen] = 0;
        walls_[number] = t;
        return t.id ? &walls_[number] : nullptr;
    }
    if (number == kLaunchScreen || number == kLaunchScreen + 1) {
        Tex t;
        int lw = launchWall(number - kLaunchScreen);
        const Tex* src = lw > 0 ? wallTexture(lw) : nullptr;
        if (src && !src->px.empty()) {
            t.id = makeTexture(src->w, src->h, GL_R8, GL_RED, src->px.data(), true);
            t.w = src->w;
            t.h = src->h;
            t.px = src->px;
        }
        launchKey_ = INT32_MIN;
        walls_[number] = t;
        return t.id ? &walls_[number] : nullptr;
    }
    if ((number >= kIdleScreen && number < kIdleScreen + kIdleKinds * kIdleGroups) ||
        (number >= kPodScreen && number < kPodScreen + kPodScreens)) {
        Tex t;
        const bool pod = number >= kPodScreen;
        const int i = pod ? 3 * kIdleGroups : number - kIdleScreen;
        idleKind(0);   // looks the kinds' walls up
        const Tex* src = idleWall_[i / kIdleGroups] > 0 ? wallTexture(idleWall_[i / kIdleGroups]) : nullptr;
        if (src && !src->px.empty()) {
            t.id = makeTexture(src->w, src->h, GL_R8, GL_RED, src->px.data(), true);
            t.w = src->w;
            t.h = src->h;
            t.px = src->px;
        }
        if (pod) podKey_[number - kPodScreen] = INT32_MIN;
        else idleKey_[i] = INT32_MIN;
        walls_[number] = t;
        return t.id ? &walls_[number] : nullptr;
    }
    if (number >= kActiveTerminal && number < kActiveTerminal + kActiveTerminalFrames) {
        Tex t;
        const Tex* src = wallTexture(59);
        if (src && !src->px.empty()) {
            std::vector<uint8_t> img = transmitterFrame(src->px, src->w, src->h, number - kActiveTerminal, data_->palette.data());
            t.id = makeTexture(src->w, src->h, GL_R8, GL_RED, img.data(), true);
            t.w = src->w;
            t.h = src->h;
            t.px = std::move(img);
        }
        walls_[number] = t;
        return t.id ? &walls_[number] : nullptr;
    }
    Tex t;
    int lump = data_->startWalls + number;
    const Bol& cd = data_->cd;
    if (number > 0 && lump < (int)cd.lumps().size() && cd.size(lump) > 130) {
        const uint8_t* d = cd.data(lump);
        int h = (d[0] | d[1] << 8) * 4;
        int cols = h > 0 ? (int)((cd.size(lump) - 130) / h) : 0;
        if (h > 0 && cols > 0) {
            std::vector<uint8_t> img((size_t)cols * h);
            for (int x = 0; x < cols; ++x)
                for (int y = 0; y < h; ++y) img[(size_t)y * cols + x] = d[130 + x * h + y];
            t.id = makeTexture(cols, h, GL_R8, GL_RED, img.data(), true);
            t.w = cols;
            t.h = h;
            t.px = std::move(img);
        }
    }
    walls_[number] = t;
    return t.id ? &walls_[number] : nullptr;
}

const Renderer::Tex* Renderer::flatTexture(int number) {
    auto it = flats_.find(number);
    if (it != flats_.end()) return it->second.id ? &it->second : nullptr;
    Tex t;
    int lump = data_->startFlats + number;
    const Bol& cd = data_->cd;
    if (number > 0 && lump < (int)cd.lumps().size() && cd.size(lump) >= 4096) {
        t.id = makeTexture(64, 64, GL_R8, GL_RED, cd.data(lump), true);
        t.w = t.h = 64;
    }
    flats_[number] = t;
    return t.id ? &flats_[number] : nullptr;
}

bool Renderer::holeAt(bool moving, size_t tri, float b1, float b2) const {
    const auto& keys = moving ? movTriKey_ : triKey_;
    const auto& uv = moving ? movCollisionUV_ : collisionUV_;
    const auto& mk = moving ? movTriMasked_ : triMasked_;
    if (tri >= keys.size() || (tri + 1) * 6 > uv.size()) return false;
    if (tri < mk.size() && !(mk[tri] & 1)) return false;   // drawn solid: its empty texels are drawn too
    int key = keys[tri];
    if (key < 0 || (key >> 16) != 0) return false;   // walls only
    auto it = walls_.find(key & 0xffff);
    if (it == walls_.end() || it->second.px.empty()) return false;
    const Tex& t = it->second;
    const float* q = &uv[tri * 6];
    float b0 = 1 - b1 - b2;
    float u = q[0] * b0 + q[2] * b1 + q[4] * b2, v = q[1] * b0 + q[3] * b1 + q[5] * b2;
    int x = (int)std::floor((u - std::floor(u)) * t.w), y = (int)std::floor((v - std::floor(v)) * t.h);
    x = std::clamp(x, 0, t.w - 1);
    y = std::clamp(y, 0, t.h - 1);
    return t.px[(size_t)y * t.w + x] == 0;
}

GLuint Renderer::textureFor(int key) {
    const Tex* t = (key >> 16) == 1 ? flatTexture(key & 0xffff) : wallTexture(key & 0xffff);
    return t ? t->id : 0;
}

// A batch's texture as the level's animations stand now (the game's translation tables: walls
// 0xc63d8, flats 0xb9640, stepped by FUN_00021de5).
GLuint Renderer::animated(const Batch& b) {
    if (b.key < 0) return b.tex;
    int n = b.key & 0xffff;
    const std::vector<uint16_t>& frames = (b.key >> 16) == 1 ? animFlats_ : animWalls_;
    if (n >= (int)frames.size() || frames[n] == n) return b.tex;
    GLuint t = textureFor((b.key & 0xffff0000) | frames[n]);
    return t ? t : b.tex;
}

void Renderer::setAnimFrames(const std::vector<uint16_t>& walls, const std::vector<uint16_t>& flats) {
    animWalls_ = walls;
    animFlats_ = flats;
}

void Renderer::setMovingParts(const std::vector<uint8_t>& walls, const std::vector<uint8_t>& surfaces) {
    movingWalls_ = walls;
    movingSurfaces_ = surfaces;
}

void Renderer::buildLevel(const Level& L) {
    TexVerts byTex, glassByTex;   // glass: walls flagged 0x10, drawn see-through
    for (bool& u : idleUsed_) u = false;
    emitLevel(L, false, byTex, glassByTex);
    upload(byTex, glassByTex, vao_, vbo_, batches_, glassBatches_, collision_, collisionUV_, triKey_, triMasked_);
    vertexCount_ = collision_.size() / 3;
    buildMoving(L);
}

// The blocks and the walls and surfaces the level's scripts move, in a buffer of their own that
// is cheap to build again as they move.
void Renderer::buildMoving(const Level& L) {
    TexVerts byTex, glassByTex;
    emitLevel(L, true, byTex, glassByTex);
    upload(byTex, glassByTex, movVao_, movVbo_, movBatches_, movGlassBatches_, movCollision_, movCollisionUV_, movTriKey_, movTriMasked_);
}

void Renderer::upload(TexVerts& byTex, TexVerts& glassByTex, GLuint vao, GLuint vbo, std::vector<Batch>& batches,
                      std::vector<Batch>& glass, std::vector<float>& collision, std::vector<float>& uv, std::vector<int>& keys,
                      std::vector<uint8_t>& masked) {
    std::vector<Vertex> all;
    batches.clear();
    for (auto& [key, verts] : byTex) {
        batches.push_back({textureFor(key), (int)all.size(), (int)verts.size(), key});
        all.insert(all.end(), verts.begin(), verts.end());
    }
    glass.clear();
    for (auto& [key, verts] : glassByTex) {
        glass.push_back({textureFor(key), (int)all.size(), (int)verts.size(), key});
        all.insert(all.end(), verts.begin(), verts.end());
    }
    collision.clear();
    collision.reserve(all.size() * 3);
    uv.clear();
    uv.reserve(all.size() * 2);
    for (const auto& v : all) {
        collision.push_back(v.x);
        collision.push_back(v.y);
        collision.push_back(v.z);
        uv.push_back(v.u);
        uv.push_back(v.v);
    }
    keys.assign(all.size() / 3, -1);
    masked.assign(all.size() / 3, 0);
    for (size_t t = 0; t < masked.size(); ++t) {
        const Vertex& v = all[t * 3];
        masked[t] = (uint8_t)((v.masked > 0.5f ? 1 : 0) | (v.masked > 1.5f ? 2 : 0) | (v.block > 0.5f ? 4 : 0));
    }
    for (const auto* list : {&batches, &glass})
        for (const Batch& b : *list)
            for (int t = b.first / 3; t < (b.first + b.count) / 3 && t < (int)keys.size(); ++t) keys[t] = b.key;
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(all.size() * sizeof(Vertex)), all.empty() ? nullptr : all.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, 0, sizeof(Vertex), (void*)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, 0, sizeof(Vertex), (void*)(3 * sizeof(float)));
    glVertexAttribPointer(2, 2, GL_FLOAT, 0, sizeof(Vertex), (void*)(5 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glBindVertexArray(0);
}

// The level's surfaces, walls and blocks: the parts that stay put (moving = false) or the blocks
// and the parts scripts move (moving = true).
void Renderer::emitLevel(const Level& L, bool moving, TexVerts& byTex, TexVerts& glassByTex) {
    auto sgn = [](uint8_t v) { return (float)(int8_t)v; };
    auto movingWall = [&](int i, bool north) {
        return (size_t)i < movingWalls_.size() && (movingWalls_[i] & (north ? 2 : 1));
    };
    auto movingSurface = [&](int i, int layer) {
        return (size_t)i < movingSurfaces_.size() && (movingSurfaces_[i] & (1 << layer));
    };
    auto H = [&](const SurfaceLayer& s, int x, int y) -> float {
        if (x >= kGrid || y >= kGrid) return 0.0f;
        return (float)(int16_t)s.height[y * kGrid + x];   // signed: scripts can sink a lift below 0
    };

    // Floors, ceilings and the two extra surface layers.
    struct SurfDef { const SurfaceLayer* s; bool up; int shapeShift; };
    const SurfDef surfs[] = {{&L.floor, true, 0}, {&L.ceiling, false, 3}, {&L.extra1, true, -1}, {&L.extra2, true, -1}};
    for (int layer = 0; layer < 4; ++layer) {
        const SurfDef& sd = surfs[layer];
        for (int y = 0; y < kGrid; ++y)
            for (int x = 0; x < kGrid; ++x) {
                int i = y * kGrid + x;
                int texNo = sd.s->tex[i] & 0x3fff;
                if (!texNo || movingSurface(i, layer) != moving) continue;
                const Tex* t = flatTexture(texNo);
                if (!t) continue;
                int shape = sd.shapeShift < 0 ? 0 : (L.shape[i] >> sd.shapeShift) & 7;
                float h0 = H(*sd.s, x, y);
                float h[4] = {h0, h0, h0, h0};
                if (shape != 0) {
                    h[1] = H(*sd.s, x + 1, y);
                    h[2] = H(*sd.s, x + 1, y + 1);
                    h[3] = H(*sd.s, x, y + 1);
                }
                float x0 = (float)x * kCellSize, x1 = x0 + kCellSize, z0 = (float)y * kCellSize, z1 = z0 + kCellSize;
                Vertex c[4] = {{x0, h[0], z0, x0 / 64.f, z0 / 64.f, 0, 0},
                               {x1, h[1], z0, x1 / 64.f, z0 / 64.f, 0, 0},
                               {x1, h[2], z1, x1 / 64.f, z1 / 64.f, 0, 0},
                               {x0, h[3], z1, x0 / 64.f, z1 / 64.f, 0, 0}};
                float light = sgn(sd.s->extra[i]);
                for (auto& v : c) v.light = light;
                int tri[6];
                if (shape == 3) { int a[6] = {0, 3, 1, 1, 3, 2}; std::memcpy(tri, a, sizeof(a)); }
                else { int a[6] = {0, 3, 2, 0, 2, 1}; std::memcpy(tri, a, sizeof(a)); }
                auto& out = byTex[flatKey(texNo)];
                bool twoSided = sd.s == &L.extra1 || sd.s == &L.extra2;
                if (sd.up || twoSided)
                    for (int k : tri) out.push_back(c[k]);
                if (!sd.up || twoSided)
                    for (int k = 5; k >= 0; --k) out.push_back(c[tri[k]]);
            }
    }

    // Walls on the north and west edge of each cell.
    auto addWall = [&](const EdgeLayer& e, int i, bool north, int x, int y) {
        int front = e.texFront[i];
        if (!front || movingWall(i, north) != moving) return;
        int back = e.texBack[i] ? e.texBack[i] : front;
        // A missile console (an addition): its own texture, so its screen can show the arming.
        if (auto sw = screenWalls_.find(i * 2 + (north ? 1 : 0)); sw != screenWalls_.end()) {
            const int cw = consoleWall(), own = kConsoleScreen + sw->second % kConsoleScreens;
            if (front == cw) front = own;
            if (back == cw) back = own;
        }
        // SDI2's launch computer (an addition): its own two textures.
        if (launchWalls_.count(i * 2 + (north ? 1 : 0)))
            for (int side = 0; side < 2; ++side) {
                int& tex = side ? back : front;
                if (tex == launchWall(0)) tex = kLaunchScreen;
                else if (tex == launchWall(1)) tex = kLaunchScreen + 1;
            }
        // An alien pod computer a pod goes into (an addition): its own texture.
        if (auto pw = podWalls_.find(i * 2 + (north ? 1 : 0)); pw != podWalls_.end() && pw->second < kPodScreens)
            for (int side = 0; side < 2; ++side) {
                int& tex = side ? back : front;
                if (tex > 0 && tex == podWall()) tex = kPodScreen + pw->second;
            }
        // A computer's screens (an addition): one of its kind's textures in motion.
        if (!stillWalls_.count(i * 2 + (north ? 1 : 0)))
            for (int side = 0; side < 2; ++side) {
                int& tex = side ? back : front;
                int k = idleKind(tex);
                if (k < 0) continue;
                int slot = k * kIdleGroups + (int)(idleHash((uint32_t)i, north, (uint32_t)side) % kIdleGroups);
                idleUsed_[slot] = true;
                tex = kIdleScreen + slot;
            }
        for (int side = 0; side < 2; ++side) {
            const Tex* t = wallTexture(side == 0 ? front : back);
            if (!t) continue;
            float vs, span;
            if (e.vEnd[i] == 0) { vs = 0; span = (float)t->h; }
            else { vs = (float)e.vStart[i]; span = (float)e.vEnd[i] - e.vStart[i] + 1; }
            float bottom = (float)(int16_t)e.base[i], top = bottom + span;
            float tTop = vs / t->h, tBot = (vs + span) / t->h;
            float x0 = (float)x * kCellSize, z0 = (float)y * kCellSize;
            float ax, az, bx, bz;  // left/right ends as seen from this side
            if (north) {
                if (side == 0) { ax = x0; az = z0; bx = x0 + kCellSize; bz = z0; }
                else { ax = x0 + kCellSize; az = z0; bx = x0; bz = z0; }
            } else {
                if (side == 0) { ax = x0; az = z0 + kCellSize; bx = x0; bz = z0; }
                else { ax = x0; az = z0; bx = x0; bz = z0 + kCellSize; }
            }
            float light = (float)(int8_t)e.light[i];
            // Masked (texel 0 clear) only with flag 0x02, which the loader also gives the walls whose
            // front shows texture 8, 12, 14, 18 or 21-24 (FUN_00014179's table); glass clears it
            // too. Any other wall is drawn solid (FUN_000242db writes every texel), so its empty
            // texels show colour 0 through the light table: black, greying in bright light. Up to
            // 0.5.2 the engine cleared them on every wall, and 84 crate sides in BASE, LAB2, MUSEUM2,
            // SDILAB and CORE showed holes (their "SSI RAVEN" stencil and the gaps between planks),
            // MOTHER3's 14 tank walls an empty top.
            const bool tableMasked = front == 8 || front == 12 || front == 14 || front == 18 || (front >= 21 && front <= 24);
            // 2: see-through for sight as well (flag 0x02: FUN_00020f42 lets a creature's sight pass);
            // glass (0x10 alone) is masked but blocks sight.
            float masked = ((e.flags[i] & 0x02) || tableMasked) ? 2.0f : (e.flags[i] & 0x10) ? 1.0f : 0.0f;
            Vertex q[4] = {{ax, bottom, az, 0, tBot, light, masked},
                           {bx, bottom, bz, 1, tBot, light, masked},
                           {bx, top, bz, 1, tTop, light, masked},
                           {ax, top, az, 0, tTop, light, masked}};
            // Flag 0x10 (without 0x02, the see-through grates): glass, which the game draws
            // through its TINT_MAP (FUN_0001cc24 -> FUN_0001746b mode 2).
            bool glass = (e.flags[i] & 0x10) && !(e.flags[i] & 0x02);
            int key = wallKey(side == 0 ? front : back);
            auto& out = glass ? glassByTex[key] : byTex[key];
            const int idx[6] = {0, 1, 2, 0, 2, 3};
            for (int k : idx) out.push_back(q[k]);
        }
    };
    for (int y = 0; y < kGrid; ++y)
        for (int x = 0; x < kGrid; ++x) {
            int i = y * kGrid + x;
            addWall(L.north, i, true, x, y);
            addWall(L.west, i, false, x, y);
        }

    // Cubes: free-standing boxes (crates, consoles, pillars, catwalk blocks).
    // Record (u16): [1]=x [2]=z [3]=base height [4]=size x [5]=size z [6]=top flat [7]=bottom flat
    //               [8..11]=side walls [12..13]=height range; layout from FUN_00014106 -> FUN_00013b14.
    // A block's base is signed; scripts move it up and down (FUN_0002284e).
    int scanSides = 0;   // the security scanners' monitors so far (World::initScans counts them alike)
    const int scanWall = scanMonitorWall();
    for (const auto& c : L.cubes) {
        if (!moving) break;
        auto u16 = [&](int k) { return (int)(c[k * 2] | c[k * 2 + 1] << 8); };
        int scanFirst = scanSides;
        for (int k = 8; k <= 11; ++k)
            if (scanWall > 0 && u16(k) == scanWall) ++scanSides;
        float cx = (float)u16(1), cz = (float)u16(2), base = (float)(int16_t)u16(3);
        float sx = (float)(c[8]), sz = (float)(c[10]);
        // Height [13] - [12], the sides showing texture rows from [12] (as a wall's vStart); when
        // that is not positive, the side texture's own height from its top (FUN_00013b14).
        int h = (int16_t)(u16(13) - u16(12));
        float vs = (float)u16(12);
        if (h < 1) {
            vs = 0;
            for (int k = 8; k <= 11; ++k)
                if (u16(k)) h = wallHeight(u16(k));
        }
        float hgt = (float)h;
        if (sx <= 0 || sz <= 0 || hgt <= 0) continue;
        float top = base + hgt;
        float x0 = cx, x1 = cx + sx, z0 = cz, z1 = cz + sz;
        // A side lying on a cell edge shares its plane with any wall there, and seen from the far
        // side of that wall the two would flicker through each other (a bookshelf against the
        // wall showing in the next room). The game's renderer simply draws the wall in front;
        // tuck such sides half a unit into the block so the wall wins from that side. A side keeps
        // its full length unless its end meets a wall there, so the faces of blocks set end to end
        // (the gates in BIOLAB and LAB2) still meet without a gap between them.
        const float ex0 = x0, ex1 = x1, ez0 = z0, ez1 = z1;
        auto onEdge = [](float v) { return std::fmod(v, (float)kCellSize) == 0.0f; };
        if (onEdge(x0)) x0 += 0.5f;
        if (onEdge(x1)) x1 -= 0.5f;
        if (onEdge(z0)) z0 += 0.5f;
        if (onEdge(z1)) z1 -= 0.5f;
        auto cellOf = [](float v) { return (int)std::floor(v / kCellSize); };
        auto inGrid = [](int a, int b) { return a >= 0 && a < kGrid && b >= 0 && b < kGrid; };
        // The end of a side running along x (at depth z) sits on the vertical line x = e: a west wall
        // there? And the end of a side running along z (at x) on the line z = e: a north wall there?
        auto endX = [&](float e, float tucked, float z) {
            int k = (int)e / kCellSize, zc = cellOf(z);
            return e != tucked && inGrid(k, zc) && L.west.texFront[zc * kGrid + k] ? tucked : e;
        };
        auto endZ = [&](float e, float tucked, float x) {
            int k = (int)e / kCellSize, xc = cellOf(x);
            return e != tucked && inGrid(xc, k) && L.north.texFront[k * kGrid + xc] ? tucked : e;
        };
        struct Side { float ax, az, bx, bz; int tex; };
        const Side sides[4] = {{endX(ex0, x0, z0), z0, endX(ex1, x1, z0), z0, u16(8)},
                               {x1, endZ(ez0, z0, x1), x1, endZ(ez1, z1, x1), u16(9)},
                               {endX(ex1, x1, z1), z1, endX(ex0, x0, z1), z1, u16(10)},
                               {x0, endZ(ez1, z1, x0), x0, endZ(ez0, z0, x0), u16(11)}};
        // [14]'s low byte holds the block's flags, read like a wall's (FUN_0001e51c at 0x1eb28):
        // 0x02 draws the sides masked (mode 0), else 0x10 draws them as glass through the TINT_MAP
        // (mode 2, the column loop at 0x27986). The energy curtains are such blocks: BIOLAB's and
        // LAB2's red gates (wall234), CLONE's blue ones, FAILSAF2's and MOTHER3's. Drawn solid
        // (the engine up to 0.5.2) they were opaque slabs of colour.
        const int flags = c[28];
        const bool glass = (flags & 0x10) && !(flags & 0x02);
        // Sides face outward: walk the outline clockwise seen from above, so reverse for CCW-from-outside.
        for (const auto& sd : sides) {
            int tex = sd.tex;
            if (scanWall > 0 && tex == scanWall) tex = kScanScreen + (scanFirst++ % kScanScreens);
            if (int k = idleKind(tex); k >= 0) {   // a computer's screens (an addition)
                int slot = k * kIdleGroups + (int)(idleHash((uint32_t)(&c - L.cubes.data()), (uint32_t)(&sd - sides), 7) % kIdleGroups);
                idleUsed_[slot] = true;
                tex = kIdleScreen + slot;
            }
            const Tex* t = wallTexture(tex);
            if (!t) continue;
            float len = std::sqrt((sd.bx - sd.ax) * (sd.bx - sd.ax) + (sd.bz - sd.az) * (sd.bz - sd.az));
            float u1 = len / (float)t->w, tt = vs / (float)t->h, tb = (vs + hgt) / (float)t->h;
            // Masked only with 0x02 (glass clears texel 0 too); otherwise solid, as the walls above.
            const float mk = (flags & 0x02) ? 2.0f : (flags & 0x10) ? 1.0f : 0.0f;
            Vertex q[4] = {{sd.bx, base, sd.bz, 0, tb, 0, mk}, {sd.ax, base, sd.az, u1, tb, 0, mk},
                           {sd.ax, top, sd.az, u1, tt, 0, mk}, {sd.bx, top, sd.bz, 0, tt, 0, mk}};
            for (auto& v : q) v.block = 1;
            auto& out = glass ? glassByTex[wallKey(tex)] : byTex[wallKey(tex)];
            const int idx[6] = {0, 1, 2, 0, 2, 3};
            for (int k : idx) out.push_back(q[k]);
        }
        for (int face = 0; face < 2; ++face) {
            const Tex* t = flatTexture(u16(face == 0 ? 6 : 7) & 0x3fff);
            if (!t) continue;
            float y = face == 0 ? top : base;
            // The flat starts at the block's own corner, not the grid's: the desks' tops (flat249,
            // flat250) have their picture in the flat's first 32 x 32 texels and black around it,
            // which showed as a black square on a desk off the grid (BIOLAB 20, 22 and 22, 47).
            auto fu = [&](float x) { return (x - cx) / 64.f; };
            auto fv = [&](float z) { return (z - cz) / 64.f; };
            Vertex q[4] = {{x0, y, z0, fu(x0), fv(z0), 0, 0}, {x1, y, z0, fu(x1), fv(z0), 0, 0},
                           {x1, y, z1, fu(x1), fv(z1), 0, 0}, {x0, y, z1, fu(x0), fv(z1), 0, 0}};
            for (auto& v : q) v.block = 1;
            auto& out = byTex[flatKey(u16(face == 0 ? 6 : 7) & 0x3fff)];
            const int up[6] = {0, 3, 2, 0, 2, 1}, down[6] = {0, 1, 2, 0, 2, 3};
            for (int k : (face == 0 ? up : down)) out.push_back(q[k]);
        }
    }
}

void Renderer::ensureTarget(int w, int h) {
    if (fbo_ && fboW_ == w && fboH_ == h) return;
    if (!fbo_) {
        glGenFramebuffers(1, &fbo_);
        glGenTextures(1, &fboColor_);
        glGenRenderbuffers(1, &fboDepth_);
    }
    glBindTexture(GL_TEXTURE_2D, fboColor_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindRenderbuffer(GL_RENDERBUFFER, fboDepth_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fboColor_, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, fboDepth_);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    fboW_ = w;
    fboH_ = h;
}

void Renderer::draw(const Camera& cam, int width, int height, const RenderSettings& s, const Scene* scene) {
    if (scene) {
        updateScanScreens(scene->scanScreens);
        updateIdleScreens(scene->time);
    }
    int rw = width, rh = height;
    if (s.renderScale == -1) {  // 320x200 at 4:3, widened for the window's aspect (Hor+)
        rh = 200;
        rw = std::max(240, (int)std::lround(240.0 * width / height));  // 320 wide at 4:3
    } else if (s.renderScale < 100) {
        rw = std::max(1, width * s.renderScale / 100);
        rh = std::max(1, height * s.renderScale / 100);
    }
    bool offscreen = rw != width || rh != height;
    if (offscreen) {
        ensureTarget(rw, rh);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    }
    glViewport(0, 0, rw, rh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    float aspect = (float)width / (float)height;
    // The game projects with one focal length for both axes (half the view's width: 90 degrees
    // across 320 columns, 64 down 200 rows) and its picture is shown at 4:3; with tall pixels the
    // view is stretched the same way. The vertical view is fixed; wider screens see more (Hor+).
    float tanH43 = std::tan(s.hFovDeg4x3 * 3.14159265f / 360.0f);
    float stretch = s.tallPixels ? 1.2f : 1.0f;
    float tanV = tanH43 * 0.75f / stretch;
    float tanH = tanV * aspect * stretch;

    if (s.showSky && skyTex_) {
        glDisable(GL_DEPTH_TEST);
        glUseProgram(skyProg_);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, skyTex_);
        glUniform1i(glGetUniformLocation(skyProg_, "uSky"), 0);
        glActiveTexture(GL_TEXTURE0 + 1);
        glBindTexture(GL_TEXTURE_2D, palTex_);
        glUniform1i(glGetUniformLocation(skyProg_, "uPal"), 1);
        // The game's heading: 0-255 from east, counter-clockwise (yaw 0 looks north, turning right).
        float heading = 64.0f - cam.yaw * 128.0f / 3.14159265f;
        heading -= 256.0f * std::floor(heading / 256.0f);
        glUniform1f(uSkyYaw_, heading * 2.5f);
        glUniform1f(uSkyPitch_, 100.0f + 160.0f * std::tan(std::clamp(cam.pitch, -1.5f, 1.5f)));
        glUniform2f(uSkyFov_, tanH, tanV);
        glBindVertexArray(skyVao_);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    // View: rotate by -yaw around Y, then -pitch around X; projection with near 2, far 8192.
    float cy = std::cos(cam.yaw), sy = std::sin(cam.yaw), cp = std::cos(cam.pitch), sp = std::sin(cam.pitch);
    float fwd[3] = {sy * cp, sp, -cy * cp};
    float right[3] = {cy, 0, sy};
    float up[3] = {right[1] * fwd[2] - right[2] * fwd[1], right[2] * fwd[0] - right[0] * fwd[2],
                   right[0] * fwd[1] - right[1] * fwd[0]};
    float eye[3] = {cam.x, cam.y, cam.z};
    auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    float view[16] = {right[0], up[0], -fwd[0], 0, right[1], up[1], -fwd[1], 0,
                      right[2], up[2], -fwd[2], 0, -dot(right, eye), -dot(up, eye), dot(fwd, eye), 1};
    float n = 2.0f, f = 8192.0f;
    float proj[16] = {1.0f / tanH, 0, 0, 0, 0, 1.0f / tanV, 0, 0, 0, 0, -(f + n) / (f - n), -1, 0, 0, -2 * f * n / (f - n), 0};
    float vp[16];
    mat4Mul(proj, view, vp);
    const float sun[3] = {0.42f, 0.82f, 0.39f};  // soft key light for voxel faces (shade offset only)

    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, palTex_);
    glActiveTexture(GL_TEXTURE0 + 2);
    glBindTexture(GL_TEXTURE_2D, shadeTex_);
    glActiveTexture(GL_TEXTURE0);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glUseProgram(prog_);
    glUniformMatrix4fv(uViewProj_, 1, 0, vp);
    glUniform3f(uEye_, cam.x, cam.y, cam.z);
    setLighting(prog_, s, scene);
    glUniform3f(glGetUniformLocation(prog_, "uFwd"), fwd[0], fwd[1], fwd[2]);
    glUniform3f(glGetUniformLocation(prog_, "uOffset"), 0, 0, 0);
    GLint uClipMin = glGetUniformLocation(prog_, "uClipMin"), uClipMax = glGetUniformLocation(prog_, "uClipMax");
    glUniform3f(uClipMin, -1e9f, -1e9f, -1e9f);
    glUniform3f(uClipMax, 1e9f, 1e9f, 1e9f);
    glUniform1i(glGetUniformLocation(prog_, "uTex"), 0);
    glBindVertexArray(vao_);
    for (const auto& b : batches_) {
        glBindTexture(GL_TEXTURE_2D, animated(b));
        glDrawArrays(GL_TRIANGLES, b.first, b.count);
    }
    glBindVertexArray(movVao_);
    for (const auto& b : movBatches_) {
        glBindTexture(GL_TEXTURE_2D, animated(b));
        glDrawArrays(GL_TRIANGLES, b.first, b.count);
    }
    // Doors: each slab drawn at its current slide offset.
    if (scene && !doorBatches_.empty()) {
        GLint uOff = glGetUniformLocation(prog_, "uOffset");
        glBindVertexArray(doorVao_);
        for (size_t i = 0; i < doorBatches_.size(); ++i) {
            const Batch& b = doorBatches_[i];
            if (!b.tex || !b.count) continue;
            float ox = 0, oy = 0, oz = 0;
            if (i * 3 + 2 < scene->doorOffsets.size()) {
                ox = scene->doorOffsets[i * 3]; oy = scene->doorOffsets[i * 3 + 1]; oz = scene->doorOffsets[i * 3 + 2];
            }
            glUniform3f(uOff, ox, oy, oz);
            if (i * 6 + 5 < scene->doorClips.size()) {
                const float* c = &scene->doorClips[i * 6];
                glUniform3f(uClipMin, c[0], c[1], c[2]);
                glUniform3f(uClipMax, c[3], c[4], c[5]);
            }
            glBindTexture(GL_TEXTURE_2D, animated(b));
            glDrawArrays(GL_TRIANGLES, b.first, b.count);
        }
        glUniform3f(uClipMin, -1e9f, -1e9f, -1e9f);
        glUniform3f(uClipMax, 1e9f, 1e9f, 1e9f);
    }
    glBindVertexArray(0);
    glDisable(GL_CULL_FACE);

    if (scene && !scene->shadows.empty()) {
        glUseProgram(shadowProg_);
        glUniformMatrix4fv(glGetUniformLocation(shadowProg_, "uViewProj"), 1, 0, vp);
        glBindVertexArray(shadowVao_);
        glBindBuffer(GL_ARRAY_BUFFER, shadowVbo_);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(scene->shadows.size() * sizeof(float)), scene->shadows.data(), GL_STREAM_DRAW);
        glDepthMask(GL_FALSE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
        glDrawArraysInstanced(GL_TRIANGLES, 0, 6, (GLsizei)(scene->shadows.size() / 4));
        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
    }

    if (scene) {
        // Voxel models
        if (modelUploaded_ != modelData_.size()) {
            glBindBuffer(GL_ARRAY_BUFFER, modelVbo_);
            glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(modelData_.size() * sizeof(MorphVoxel)), modelData_.data(), GL_STATIC_DRAW);
            modelUploaded_ = modelData_.size();
        }
        if (!scene->models.empty() && !modelData_.empty()) {
            glUseProgram(voxProg_);
            setLighting(voxProg_, s, scene);
            glUniformMatrix4fv(glGetUniformLocation(voxProg_, "uViewProj"), 1, 0, vp);
            glUniform3f(glGetUniformLocation(voxProg_, "uEye"), cam.x, cam.y, cam.z);
            glUniform3f(glGetUniformLocation(voxProg_, "uFwd"), fwd[0], fwd[1], fwd[2]);
            glUniform3f(glGetUniformLocation(voxProg_, "uSun"), sun[0], sun[1], sun[2]);
            GLint uPos = glGetUniformLocation(voxProg_, "uPos"), uRot = glGetUniformLocation(voxProg_, "uRot");
            GLint uScale = glGetUniformLocation(voxProg_, "uScale"), uT = glGetUniformLocation(voxProg_, "uT");
            GLint uCube = glGetUniformLocation(voxProg_, "uCube"), uEyeL = glGetUniformLocation(voxProg_, "uEyeLocal");
            GLint uLight = glGetUniformLocation(voxProg_, "uLight"), uFlash = glGetUniformLocation(voxProg_, "uFlash");
            glBindVertexArray(modelVao_);
            for (const auto& m : scene->models) {
                if (m.range.count <= 0) continue;
                // eye in model space (voxel units)
                float d[3] = {cam.x - m.pos[0], cam.y - m.pos[1], cam.z - m.pos[2]};
                float el[3];
                for (int a = 0; a < 3; ++a) el[a] = (d[0] * m.rot[a * 3] + d[1] * m.rot[a * 3 + 1] + d[2] * m.rot[a * 3 + 2]) / m.scale;
                glUniform3f(uPos, m.pos[0], m.pos[1], m.pos[2]);
                glUniformMatrix3fv(uRot, 1, 0, m.rot);
                glUniform1f(uScale, m.scale);
                glUniform1f(uT, m.t);
                glUniform1f(uCube, (m.t > 0.001f && m.t < 0.999f) ? 1.12f : 1.0f);
                glUniform3f(uEyeL, el[0], el[1], el[2]);
                glUniform1f(uLight, m.light);
                glUniform1f(uFlash, m.flash);
                // Instanced draw starting at range.first: offset the attribute pointers.
                glBindBuffer(GL_ARRAY_BUFFER, modelVbo_);
                glVertexAttribIPointer(0, 4, GL_UNSIGNED_BYTE, sizeof(MorphVoxel), (void*)(size_t)(m.range.first * sizeof(MorphVoxel)));
                glVertexAttribIPointer(1, 4, GL_UNSIGNED_BYTE, sizeof(MorphVoxel), (void*)(size_t)(m.range.first * sizeof(MorphVoxel) + 4));
                glDrawArraysInstanced(GL_TRIANGLES, 0, 18, m.range.count);
            }
        }
        // Sprite pixels: opaque first
        if (splatUploaded_ != splatData_.size()) {
            glBindBuffer(GL_ARRAY_BUFFER, splatVbo_);
            glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(splatData_.size() * sizeof(SplatMorph)), splatData_.data(), GL_STATIC_DRAW);
            splatUploaded_ = splatData_.size();
        }
        drawSplats(scene->splats, vp, cam, fwd, right, up, s, scene);
        // Debris voxels
        if (!scene->debris.empty()) {
            glUseProgram(debrisProg_);
            setLighting(debrisProg_, s, scene);
            glUniformMatrix4fv(glGetUniformLocation(debrisProg_, "uViewProj"), 1, 0, vp);
            glUniform3f(glGetUniformLocation(debrisProg_, "uEye"), cam.x, cam.y, cam.z);
            glUniform3f(glGetUniformLocation(debrisProg_, "uFwd"), fwd[0], fwd[1], fwd[2]);
            glUniform3f(glGetUniformLocation(debrisProg_, "uSun"), sun[0], sun[1], sun[2]);
            glBindVertexArray(debrisVao_);
            glBindBuffer(GL_ARRAY_BUFFER, debrisVbo_);
            glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(scene->debris.size() * sizeof(DebrisDraw)), scene->debris.data(), GL_STREAM_DRAW);
            glDrawArraysInstanced(GL_TRIANGLES, 0, 18, (GLsizei)scene->debris.size());
        }
        // Particles: smoke (alpha, back to front), then glow (additive). Depth-tested, no depth writes.
        if (!scene->smoke.empty() || !scene->glow.empty()) {
            glUseProgram(partProg_);
            glUniformMatrix4fv(glGetUniformLocation(partProg_, "uView"), 1, 0, view);
            glUniformMatrix4fv(glGetUniformLocation(partProg_, "uProj"), 1, 0, proj);
            GLint uAdd = glGetUniformLocation(partProg_, "uAdditive");
            glBindVertexArray(partVao_);
            glBindBuffer(GL_ARRAY_BUFFER, partVbo_);
            glDepthMask(GL_FALSE);
            glEnable(GL_BLEND);
            if (!scene->smoke.empty()) {
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                glUniform1i(uAdd, 0);
                glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(scene->smoke.size() * sizeof(ParticleDraw)), scene->smoke.data(), GL_STREAM_DRAW);
                glDrawArraysInstanced(GL_TRIANGLES, 0, 6, (GLsizei)scene->smoke.size());
            }
            if (!scene->glow.empty()) {
                glBlendFunc(GL_ONE, GL_ONE);
                glUniform1i(uAdd, 1);
                glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(scene->glow.size() * sizeof(ParticleDraw)), scene->glow.data(), GL_STREAM_DRAW);
                glDrawArraysInstanced(GL_TRIANGLES, 0, 6, (GLsizei)scene->glow.size());
            }
            glDisable(GL_BLEND);
            glDepthMask(GL_TRUE);
        }
        drawSplats(scene->effects, vp, cam, fwd, right, up, s, scene);
        glBindVertexArray(0);
    }

    // Glass walls last, over everything behind them. The game blends them through its TINT_MAP
    // (the destination pixel's row, the shaded texel's column), which is close to 62 % of what
    // is behind and 38 % of the glass; holes in the glass (texel 0) show straight through.
    if (!glassBatches_.empty() || !movGlassBatches_.empty()) {
        glUseProgram(prog_);
        glUniform3f(glGetUniformLocation(prog_, "uOffset"), 0, 0, 0);
        glUniform3f(glGetUniformLocation(prog_, "uClipMin"), -1e9f, -1e9f, -1e9f);
        glUniform3f(glGetUniformLocation(prog_, "uClipMax"), 1e9f, 1e9f, 1e9f);
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glDepthMask(GL_FALSE);
        glEnable(GL_BLEND);
        glBlendColor(0, 0, 0, 0.38f);
        glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA);
        glBindVertexArray(vao_);
        for (const auto& b : glassBatches_) {
            glBindTexture(GL_TEXTURE_2D, animated(b));
            glDrawArrays(GL_TRIANGLES, b.first, b.count);
        }
        glBindVertexArray(movVao_);
        for (const auto& b : movGlassBatches_) {
            glBindTexture(GL_TEXTURE_2D, animated(b));
            glDrawArrays(GL_TRIANGLES, b.first, b.count);
        }
        glBindVertexArray(0);
        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
        glDisable(GL_CULL_FACE);
    }

    if (offscreen) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, rw, rh, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
}

static void screenTexture(GLuint& tex, int& w, int& h, int width, int height) {
    if (tex && w == width && h == height) return;
    if (!tex) glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    w = width;
    h = height;
}

// The level's textures stay bound between frames, so these passes put back what they change.
struct TextureKeeper {
    GLint unit0 = 0, unit1 = 0;
    TextureKeeper() {
        glActiveTexture(GL_TEXTURE1);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &unit1);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &unit0);
    }
    ~TextureKeeper() {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, (GLuint)unit1);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, (GLuint)unit0);
    }
};

void Renderer::keepFrame(int width, int height) {
    TextureKeeper keep;
    screenTexture(prevTex_, prevW_, prevH_, width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, prevTex_);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, width, height);
}

void Renderer::brighten(int width, int height, float gamma) {
    if (gamma <= 1.001f || !brightProg_) return;
    TextureKeeper keep;
    screenTexture(brightTex_, brightW_, brightH_, width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, brightTex_);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, width, height);
    glViewport(0, 0, width, height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glUseProgram(brightProg_);
    glUniform1i(glGetUniformLocation(brightProg_, "uScreen"), 0);
    glUniform1f(glGetUniformLocation(brightProg_, "uGamma"), gamma);
    glBindVertexArray(skyVao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glEnable(GL_DEPTH_TEST);
}

void Renderer::crt(int width, int height, const float rect[4], float bright, float glitch) {
    // Grab what's on screen, then draw it back squeezed into the rectangle.
    TextureKeeper keep;
    screenTexture(crtTex_, crtW_, crtH_, width, height);
    bool stale = prevTex_ && prevW_ == width && prevH_ == height;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, crtTex_);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, width, height);
    if (stale) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, prevTex_);
        glActiveTexture(GL_TEXTURE0);
    }
    glViewport(0, 0, width, height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glUseProgram(crtProg_);
    glUniform1i(glGetUniformLocation(crtProg_, "uScreen"), 0);
    glUniform1i(glGetUniformLocation(crtProg_, "uPrev"), 1);
    glUniform1f(glGetUniformLocation(crtProg_, "uStale"), stale ? 1.0f : 0.0f);
    glUniform2f(glGetUniformLocation(crtProg_, "uSize"), (float)width, (float)height);
    glUniform4f(glGetUniformLocation(crtProg_, "uRect"), rect[0], rect[1], rect[2], rect[3]);
    glUniform1f(glGetUniformLocation(crtProg_, "uBright"), bright);
    glUniform1f(glGetUniformLocation(crtProg_, "uGlitch"), glitch);
    glBindVertexArray(skyVao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
}

std::vector<uint8_t> Renderer::thumbnail(int width, int height, int tw, int th) {
    std::vector<uint8_t> out((size_t)tw * th * 3, 0);
    if (width <= 0 || height <= 0) return out;
    std::vector<uint8_t> px((size_t)width * height * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    float cw = std::min((float)width, height * 1.6f), ch = cw / 1.6f;
    float x0 = (width - cw) / 2, y0 = (height - ch) / 2;
    for (int ty = 0; ty < th; ++ty)
        for (int tx = 0; tx < tw; ++tx) {
            // The average of the screen pixels under this one.
            int sx0 = (int)(x0 + cw * tx / tw), sx1 = std::max(sx0 + 1, (int)(x0 + cw * (tx + 1) / tw));
            int sy0 = (int)(y0 + ch * ty / th), sy1 = std::max(sy0 + 1, (int)(y0 + ch * (ty + 1) / th));
            int sum[3] = {0, 0, 0}, n = 0;
            for (int sy = sy0; sy < sy1 && sy < height; ++sy)
                for (int sx = sx0; sx < sx1 && sx < width; ++sx) {
                    const uint8_t* p = &px[((size_t)(height - 1 - sy) * width + sx) * 4];
                    sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2];
                    ++n;
                }
            for (int c = 0; c < 3; ++c) out[((size_t)ty * tw + tx) * 3 + c] = (uint8_t)(n ? sum[c] / n : 0);
        }
    return out;
}

bool Renderer::screenshot(int width, int height, const std::string& path) {
    std::vector<uint8_t> px((size_t)width * height * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;   // the window is opaque; overlays leave alpha behind
    SDL_Surface* s = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA32);
    if (!s) return false;
    for (int y = 0; y < height; ++y)
        std::memcpy((uint8_t*)s->pixels + (size_t)y * s->pitch, &px[(size_t)(height - 1 - y) * width * 4], (size_t)width * 4);
    bool ok = SDL_SaveBMP(s, path.c_str());
    SDL_DestroySurface(s);
    return ok;
}
