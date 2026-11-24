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
enum { ENGINE_W = 1024, ENGINE_H = 768 };
int  engine_start(uint32_t *kernel, uint32_t *renderer);   /* 0 if it did not */
uint32_t engine_camera(uint32_t scene, uint32_t vp, float near_, float far_);
uint32_t engine_turn(float a, float b);                    /* a quaternion (scratch): a about y, then b about x */
void engine_materials(uint32_t r, const char *file);
void engine_keys(uint32_t r, const char *file);
uint32_t engine_model(uint32_t r, const char *model, int its_keys);   /* a TGFXFileObject, or 0 */
uint32_t engine_instance(uint32_t gfx, uint32_t scene);
static inline uint32_t V(float x, float y, float z) { float v[3] = { x, y, z }; return hle_scratch_copy(v, 12); }

void engine_listen(uint32_t emitter, const char *shim, uint32_t context, int after);   /* gui.c: see there */

/* An island (terrain.c): its ground, sky and sea, seen by a camera of its own. */
typedef struct { uint32_t kernel, r, scene, vp, place, ctx, terrain, shader; } world_t;
int  world_make(world_t *w, uint32_t kernel, uint32_t r, const char *island, const char *weather);
float world_ground(const world_t *w, float x, float z);    /* up from the sea, below it too */
void world_look(const world_t *w, const float at[3], float heading, float pitch);
void world_reflect(const world_t *w, const float at[3]);   /* what the sea reflects: the context the camera's */
uint32_t world_draw(const world_t *w, const float at[3]);  /* the ground, sky and sea; how many sections */
/* A viewport that clears, on the scene, for the whole display. */
uint32_t engine_viewport(uint32_t scene, const char *name, uint8_t red, uint8_t green, uint8_t blue);
#endif
