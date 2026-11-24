/*
 * engine.h - what engine.c (the engine, models, terrain) and gui.c (sprites
 * and the GUI) share: the DLLs, calls into them, and values for their
 * arguments.
 */
#ifndef JPOG_ENGINE_H
#define JPOG_ENGINE_H
#include <stdint.h>
#include <string.h>
#include "win32hle.h"
#include "jpog.h"

#define K "TKernelInterface.dll"
#define R "TRenderInterface.dll"
#define D "TRenderD3DInterface.dll"

static inline uint32_t F(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline uint32_t S(const char *s) { return hle_guest_strdup(s); }
static inline float rd(uint32_t va) { float f; uint32_t u = MEM32(va); memcpy(&f, &u, 4); return f; }

enum { SYS_SHADERS = 7, SYS_SHSYS = 8, SYS_SHSKIN = 9, SYS_GFXFILES = 16, SYS_SCENE = 17, SYS_TEXTURES = 18 };
uint32_t engine_system(uint32_t r, uint32_t which);
/* A virtual method, by its offset in the object's vtable. */
uint32_t engine_vcall(uint32_t self, uint32_t offset, int n, ...);
/* The guest's messages, as the game's loop takes them; 1 to stop. */
int  engine_pump(void);
void engine_window_shot(void);               /* JPOG_WINDOW_SHOT=FILE: the 120th frame the window shows */
/* A frame of the scene, after BeginScene and before End. */
void engine_scene(uint32_t scene, float dt);
/* A viewport that clears, on the scene, for the whole display. */
uint32_t engine_viewport(uint32_t scene, const char *name, uint8_t red, uint8_t green, uint8_t blue);
#endif
