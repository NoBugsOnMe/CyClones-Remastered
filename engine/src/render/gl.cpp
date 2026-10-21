#include "gl.h"

#define GL_DEFINE(ret, name, args) PFN_##name name = nullptr;
GL_FUNCS(GL_DEFINE)
#undef GL_DEFINE

const char* glLoadFunctions() {
#define GL_LOAD(ret, name, args)                                              \
    name = reinterpret_cast<PFN_##name>(SDL_GL_GetProcAddress(#name));        \
    if (!name) return #name;
    GL_FUNCS(GL_LOAD)
#undef GL_LOAD
    return nullptr;
}
