/* jpog.h - what the host's files share. */
#pragma once
#include <stdint.h>
#include "win32hle.h"
#include "pe_loader.h"
#include "recomp_target.h"

#define JPOG_MAX_MODULES 32

typedef struct {
    const char *file;        /* its name: TKernelInterface.dll */
    const char *path;        /* where in the game folder: Shaders\D3D\TSkinShaderD3D.dll */
    pe_image    img;
    int         is_exe;
    int         on_demand;   /* the game loads it (LoadLibrary): a shader DLL */
    int         refs;        /* ...how many loads it has not freed */
    int         state;       /* 0 not attached, 1 attaching, 2 attached */
    int         missing;     /* imports from a module of the game this build has not lifted */
} jpog_module;

extern jpog_module g_jpog_mod[JPOG_MAX_MODULES];
extern int g_jpog_nmod, g_jpog_stubs, jpog_verbose;
extern int jpog_headless;                /* no window, no sound device */

int  jpog_load(const char *game);                                   /* loader.c: map and bind every module */
int  jpog_attach(jpog_module *m);                                   /* DllMain, after its imports' */
int  jpog_attach_dlls(void);
jpog_module *jpog_module_named(const char *dll);
jpog_module *jpog_module_at(uint32_t base);
uint32_t jpog_export(const char *dll, const char *name);
uint32_t jpog_export_of(const jpog_module *m, const char *name, uint32_t ordinal);
uint32_t jpog_load_library(const char *name);                       /* a lifted module the game loads: its base, or 0 */
int  jpog_free_library(uint32_t base);                              /* 1 if base was one */

uint32_t jpog_msvcrt_data(const char *name);                        /* msvcrt.c: a variable msvcrt exports, or 0 */
void jpog_setjmp3(void);                                            /* setjmp.c */
void jpog_longjmp(void);
void jpog_cpuid(void);                                              /* cpu.c */
int  jpog_selftest(void);                                           /* selftest.c: how many checks failed */
void jpog_check(int ok, const char *what, const char *got);
void jpog_selftest_d3d8(void);                                      /* selftest_d3d8.c */
uint32_t jpog_call(const char *dll, const char *name, uint32_t self, int n, ...);   /* engine.c: a lifted export */
uint32_t jpog_engine_kernel(void);
uint32_t jpog_engine_renderer(uint32_t kernel);
int  jpog_engine_display(uint32_t renderer, uint32_t w, uint32_t h);
uint32_t jpog_engine_device(uint32_t renderer);                     /* its IDirect3DDevice8 */
extern const win32hle_shim jpog_fsound_shims[];                     /* fsound.c: fmod.dll */
extern int jpog_fsound_offline;                                     /* no sound device: the mixer is asked, below */
void jpog_fsound_render(float *stereo, int frames);
int  jpog_listen(const char *what);                                 /* a sound or piece of music, or "all" */
void jpog_selftest_fsound(void);                                    /* selftest_fsound.c */
void jpog_selftest_movies(void);                                    /* selftest_movies.c */
int  jpog_movie_sound(int rate);                                    /* a movie's sound in the mixer: an id, or -1 */
void jpog_movie_sound_push(int id, const float *l, const float *r, int frames);
void jpog_movie_sound_on(int id, int on);
void jpog_movie_sound_close(int id);
extern const win32hle_shim jpog_bink_shims[];                       /* movies.c: binkw32.dll */
int  jpog_movie(const char *file, const char *shot, int frames);
int  jpog_gui(const char *dialog, const char *file, const char *locale, const char *shot, int frames);
int  jpog_terrain(const char *island, const char *shot, int frames);
int  jpog_view(const char *model, const char *keys, const char *materials, int anim, const char *shot, int frames);
void jpog_selftest_engine(void);                                    /* selftest_engine.c */

void jpog_d3d8_summary(void);                                       /* d3d8.c: the Direct3D 8 calls made, at exit */
int  jpog_d3d8_shot(const char *path);                              /* the back buffer as a BMP */

void jpog_write_bmp(const char *path, const uint8_t *bgra, uint32_t w, uint32_t h);

extern const win32hle_shim jpog_d3d8_shims[];                       /* d3d8.c */
extern const win32hle_shim jpog_msvcrt_shims[];                     /* msvcrt.c */
extern const win32hle_shim jpog_shims[];                            /* shims.c */
extern const win32hle_shim jpog_registry_shims[];                   /* registry.c */

int jpog_run_game(void);                                            /* game.c: run SimJP.exe */
