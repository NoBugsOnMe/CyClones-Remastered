// Builds 3D voxel models from the original 2D sprite art, at load time, from the player's own files.
//
//  * carveRotations(): monsters and projectiles were pre-rendered from 5 (mirrored) or 8 directions.
//    The model is the visual hull of those silhouettes: a voxel survives if every view sees it.
//    Each view's horizontal alignment is refined to maximise agreement, and every surface voxel
//    takes its colour from the view that looks most squarely at it.
//  * inflateSprite(): single-view art (items, most objects) becomes a rounded relief, thick in the
//    middle and thin at the edges.
//  * revolveSprite(): symmetric single-view art (barrels, canisters) is spun into a solid of revolution.
//  * morph(): pairs every voxel of one animation frame with its nearest voxel in the next, so the
//    GPU can glide between poses instead of snapping from frame to frame.
//
// Model space: x = right as seen from the front, y = up (0 = ground), z = toward the front viewer.
#pragma once
#include <cstdint>
#include <vector>

#include "../data/sprite.h"

struct Voxel {
    int16_t x, y, z;   // cell coordinates; the voxel occupies [x, x+1) etc. around the model axis
    uint8_t c;         // palette index
    uint8_t pad = 0;
};

struct VoxelModel {
    std::vector<Voxel> vox;   // surface voxels only
    int height = 0;
    float radius = 0;         // horizontal extent from the axis
    bool empty() const { return vox.empty(); }
};

// GPU instance: a voxel gliding from 'f' (frame A) to 't' (frame B). x and z are stored +128.
struct MorphVoxel {
    uint8_t fx, fy, fz, fc;
    uint8_t tx, ty, tz, tc;
};

void setVoxelPalette(const uint8_t* palette768);   // 6-bit VGA palette, enables colour blending
VoxelModel carveRotations(const std::vector<const SpriteImage*>& rotations);  // 5 (mirrored) or 8 views
VoxelModel inflateSprite(const SpriteImage& s, int maxHalfThickness = 12);
VoxelModel revolveSprite(const SpriteImage& s);
bool looksSymmetric(const SpriteImage& s);   // silhouette mirrors about its axis (revolve candidate)
VoxelModel makeModel(const SpriteImage& s);  // revolve if symmetric, else inflate

VoxelModel downsample(const VoxelModel& m);   // half resolution (draw at scale 2) for distant LOD
std::vector<MorphVoxel> morph(const VoxelModel& a, const VoxelModel& b);
std::vector<MorphVoxel> still(const VoxelModel& a);

// Depth sprites: the original sprite pixels placed in 3D. Positions are in quarter units (x right
// of the front view, y up from the ground, z toward the front viewer).
struct Splat {
    int16_t x, y, z;
    uint8_t c;
};
struct SplatSet {
    std::vector<Splat> s;
};
struct SplatMorph {       // GPU instance: a pixel gliding from 'f' to 't' (colour in the 4th short)
    int16_t fx, fy, fz, fc;
    int16_t tx, ty, tz, tc;
};
// One splat set per view (8, in 45-degree steps counter-clockwise from the front) for a 5 or 8
// view rotation set. Each view reproduces its sprite exactly when seen from its own angle; depth
// comes from the visual hull, so it turns with believable parallax in between.
// shifts (optional): each view's horizontal alignment, so view k's axis sits at column ox + shift.
std::vector<SplatSet> depthSprites(const std::vector<const SpriteImage*>& rotations, std::vector<int>* shifts = nullptr);
SplatSet flatSprite(const SpriteImage& s);   // single-view art (always faces the viewer)
std::vector<SplatMorph> morphSplats(const SplatSet& a, const SplatSet& b);
std::vector<SplatMorph> stillSplats(const SplatSet& a);
