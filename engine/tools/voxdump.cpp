// voxdump: build a voxel model from CYCLONE.CD/HD sprites and write "x y z c" lines (for offline previews).
// usage: voxdump <bol> <base lump> <rotations 1|5|8> [frame offset]
#include <SDL3/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "../src/data/bol.h"
#include "../src/render/voxel.h"
int main(int argc, char** argv) {
    if (argc < 4) return 1;
    size_t n; void* p = SDL_LoadFile(argv[1], &n);
    Bol b; std::string err; b.load(std::vector<uint8_t>((uint8_t*)p, (uint8_t*)p + n), err);
    { size_t hn; void* hp = SDL_LoadFile("/home/claude/cyclones/install/CYCLONE.HD", &hn);
      Bol hd; hd.load(std::vector<uint8_t>((uint8_t*)hp, (uint8_t*)hp + hn), err);
      setVoxelPalette(hd.data(hd.find("play_pal"))); }
    int base = b.find(argv[2]), rot = atoi(argv[3]), f = argc > 4 ? atoi(argv[4]) : 0;
    if (base < 0) { fprintf(stderr, "no lump\n"); return 1; }
    VoxelModel m;
    if (rot > 1) {
        std::vector<SpriteImage> imgs(rot); std::vector<const SpriteImage*> ptrs;
        for (int r = 0; r < rot; ++r) { decodeSprite(b.data(base + f + r), b.size(base + f + r), imgs[r]); ptrs.push_back(&imgs[r]); }
        m = carveRotations(ptrs);
    } else {
        SpriteImage s; decodeSprite(b.data(base + f), b.size(base + f), s); m = makeModel(s);
    }
    for (auto& v : m.vox) printf("%d %d %d %d\n", v.x, v.y, v.z, v.c);
    fprintf(stderr, "%zu voxels, height %d, radius %.1f\n", m.vox.size(), m.height, m.radius);
}
