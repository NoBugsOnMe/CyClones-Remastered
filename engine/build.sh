#!/bin/bash
# Builds the CyClones engine for Linux (testing) and Windows x64.
set -e
cd "$(dirname "$0")"
SRC="src/main.cpp src/render/gl.cpp src/render/renderer.cpp src/render/automap.cpp src/render/hud.cpp src/render/firstperson.cpp src/render/voxel.cpp src/data/disc.cpp src/data/arj.cpp src/data/bol.cpp src/data/level.cpp src/data/sprite.cpp src/data/entities.cpp src/data/smacker.cpp src/data/hdvideo.cpp src/game/world.cpp src/game/player.cpp src/game/combat.cpp src/game/ai.cpp src/game/cutscene.cpp src/game/videoplayer.cpp src/game/save.cpp src/game/keepsake.cpp src/game/script.cpp src/game/surfanim.cpp src/game/inventory.cpp src/game/scan.cpp src/audio/audio.cpp"
FLAGS="-std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers"
DEPS=/home/claude/deps
mkdir -p build/linux build/win64
JOBS=""
# dav1d (BSD-2-Clause) decodes the Enhanced cut-scenes; built static from its 1.5.1 sources.
g++ $FLAGS -I$DEPS/sdl3-linux/include -I$DEPS/dav1d-linux/include $SRC -o build/linux/cyclones-engine -L$DEPS/sdl3-linux/lib -lSDL3 -Wl,-rpath,$DEPS/sdl3-linux/lib \
    $DEPS/dav1d-linux/lib/x86_64-linux-gnu/libdav1d.a -lpthread -ldl
[ "$1" = "linux" ] && { ls -l build/linux/cyclones-engine; exit 0; }
MW=$DEPS/SDL3-3.4.18/x86_64-w64-mingw32
x86_64-w64-mingw32-windres res/engine.rc -O coff -o build/win64/engine_res.o
x86_64-w64-mingw32-g++ $FLAGS -s -static-libgcc -static-libstdc++ -I$MW/include -I$DEPS/dav1d-win64/include $SRC build/win64/engine_res.o -o "build/win64/CyClones Engine.exe" \
    -L$MW/lib -lSDL3 $DEPS/dav1d-win64/lib/libdav1d.a -mwindows -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic
cp $MW/bin/SDL3.dll build/win64/
ls -l build/linux/cyclones-engine "build/win64/CyClones Engine.exe" build/win64/SDL3.dll
