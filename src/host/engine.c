/*
 * engine.c - Toshi, driven by the host as the game drives it: the kernel and
 * the renderer (for selftest_engine.c), and --view, which shows one of the
 * game's models through the engine's own scene, camera and shaders.
 *
 *   jpog --game DIR --terrain nublar|SEED [--headless] [--shot FILE.bmp] [--frames N]     an island, its sky and sea
 *   jpog --game DIR --view Data/Models/Trex_hi.tmd --keys Data/Models/Djb.tkl \
 *        --materials Data/matlibs/dinos_judithriver_b.tml [--anim N] [--headless] [--shot FILE.bmp] [--frames N]
 */
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "d3d_int.h"
#include "engine.h"

/* Lifted code at va: thiscall with self in ecx, or stdcall. */
static uint32_t call_va(uint32_t va, const char *what, uint32_t self, int n, va_list ap) {
    uint32_t esp = g_esp, a[16];
    recomp_func_t f = va ? recomp_lookup(va) : NULL;
    if (!f) { fprintf(stderr, "[engine] no %s (%08X)\n", what, va); return 0; }
    for (int i = 0; i < n; i++) a[i] = va_arg(ap, uint32_t);
    for (int i = n - 1; i >= 0; i--) PUSH32(g_esp, a[i]);
    PUSH32(g_esp, RECOMP_RETADDR);
    g_ecx = self;
    f();
    g_esp = esp;
    return g_eax;
}
/* An export by its mangled name. */
uint32_t jpog_call(const char *dll, const char *name, uint32_t self, int n, ...) {
    va_list ap;
    va_start(ap, n);
    uint32_t r = call_va(jpog_export(dll, name), name, self, n, ap);
    va_end(ap);
    return r;
}
uint32_t engine_vcall(uint32_t self, uint32_t offset, int n, ...) {
    char what[32];
    va_list ap;
    snprintf(what, sizeof what, "virtual +%X", offset);
    va_start(ap, n);
    uint32_t r = call_va(self ? MEM32(MEM32(self) + offset) : 0, what, self, n, ap);
    va_end(ap);
    return r;
}

uint32_t jpog_engine_kernel(void) {          /* as TApplication::Create makes it */
    uint32_t k = jpog_call(K, "?tmalloc@@YGPAXHPADH@Z", 0, 3, 0x58u, 0u, 0xFFFFFFFFu);
    uint32_t argv = hle_va(hle_alloc(8));
    MEM32(argv) = S("C:\\SimJP.exe");
    if (k) k = jpog_call(K, "??0TKernelInterface@Toshi@@QAE@HQAPAD_N@Z", k, 3, 1u, argv, 0u);
    if (k) MEM32(k + 4)++;                   /* a reference of our own */
    return k;
}
uint32_t jpog_engine_renderer(uint32_t kernel) {
    jpog_call(D, "?InitialiseStatic@TMSWindow@Toshi@@CGXXZ", 0, 0);
    jpog_call(D, "?InitialiseStatic@TRenderD3DInterface@Toshi@@CGXXZ", 0, 0);
    uint32_t r = jpog_call(D, "?CreateObject@TRenderD3DInterface@Toshi@@CGPAVTObject@2@XZ", 0, 0);
    return r && (jpog_call(D, "?Create@TRenderD3DInterface@Toshi@@UAE_NPAVTKernelInterface@2@@Z", r, 1, kernel) & 0xFF) ? r : 0;
}
int jpog_engine_display(uint32_t r, uint32_t w, uint32_t h) {
    uint32_t depth = jpog_call(R, "?MakeDepthStencilFormat@TRenderInterface@Toshi@@QAE?AW4DEPTHSTENCILFORMAT@Device@Mode@TRenderAdapter@2@II@Z",
                               r, 2, 24u, 8u);
    uint32_t params[5] = { w, h, 32, depth, 0 }, mark = hle_scratch_mark(), p = hle_scratch_copy(params, sizeof params);
    uint32_t ok = jpog_call(D, "?CreateDisplay@TRenderD3DInterface@Toshi@@UAE_NPBVDisplayParams@TRenderInterface@2@@Z", r, 1, p) & 0xFF;
    hle_scratch_release(mark);
    return ok != 0;
}
uint32_t jpog_engine_device(uint32_t r) {
    return jpog_call(D, "?GetD3DDevice@TRenderD3DInterface@Toshi@@QAEPAUIDirect3DDevice8@@XZ", r, 0);
}
uint32_t engine_system(uint32_t r, uint32_t which) {
    return jpog_call(R, "?GetSystemResource@TRenderInterface@Toshi@@QAEPAVTResource@2@W4SYSRESOURCES@12@@Z", r, 1, which);
}

/* A quaternion (x, y, z, w) turning by a about y, then by b about x. */
static uint32_t turn(float a, float b) {
    float sa = sinf(a / 2), ca = cosf(a / 2), sb = sinf(b / 2), cb = cosf(b / 2);
    float q[4] = { ca * sb, sa * cb, -sa * sb, ca * cb };
    return hle_scratch_copy(q, 16);
}

/* The guest's messages, as the game's loop takes them; 1 to stop: the window
 * was closed, or Escape. */
int engine_pump(void) {
    uint32_t mark = hle_scratch_mark(), m = hle_scratch(28);
    int stop = 0;
    while (hle_call_guest(hle_resolve("PeekMessageA"), 5, (uint32_t[]){ m, 0, 0, 0, 1 })) {
        uint32_t msg = MEM32(m + 4);
        stop |= msg == 0x10 || msg == 0x12 || (msg == 0x100 && MEM32(m + 8) == 0x1B);   /* WM_CLOSE, WM_QUIT, Escape */
        if (msg == 0x10 || msg == 0x12) continue;
        hle_call_guest(hle_resolve("TranslateMessage"), 1, &m);
        hle_call_guest(hle_resolve("DispatchMessageA"), 1, &m);
    }
    hle_scratch_release(mark);
    return stop;
}

/* JPOG_WINDOW_SHOT=FILE: the 120th frame as the window had it (not the back
 * buffer: --shot). */
static void shown(const uint8_t *px, int w, int h, int pitch, int bpp) {
    static int n;
    if (++n != 120 || !px || bpp != 32 || pitch != w * 4) return;
    jpog_write_bmp(getenv("JPOG_WINDOW_SHOT"), px, (uint32_t)w, (uint32_t)h);
    fprintf(stderr, "[view] the window's frame %d: %dx%d\n", n, w, h);
}

void engine_window_shot(void) { if (getenv("JPOG_WINDOW_SHOT")) hle_screen_frame_hook = shown; }

void engine_scene(uint32_t scene, float dt) {
    jpog_call(R, "?Begin@TScene@Toshi@@QAE_NXZ", scene, 0);
    jpog_call(R, "?Update@TScene@Toshi@@QAE_NM@Z", scene, 1, F(dt));
    jpog_call(R, "?Render@TScene@Toshi@@QAE_NXZ", scene, 0);
}
uint32_t engine_viewport(uint32_t scene, const char *name, uint8_t red, uint8_t green, uint8_t blue) {
    uint32_t vp = jpog_call(R, "?CreateViewport@TScene@Toshi@@QAEPAVTViewport@2@PBDPAVTResource@2@_N@Z", scene, 3, S(name), 0u, 1u);
    jpog_call(R, "?SetBackgroundColour@TViewport@Toshi@@QAEXEEEE@Z", vp, 4, red, green, blue, 0xFFu);
    jpog_call(R, "?AllowBackgroundClear@TViewport@Toshi@@QAE_N_N@Z", vp, 1, 1u);
    jpog_call(R, "?AllowDepthClear@TViewport@Toshi@@QAE_N_N@Z", vp, 1, 1u);
    jpog_call(R, "?Enable@TViewport@Toshi@@QAEX_N@Z", vp, 1, 1u);
    return vp;
}

int jpog_view(const char *model, const char *keys, const char *materials, int anim, const char *shot, int frames) {
    engine_window_shot();
    mach_enter();
    uint32_t kernel = jpog_engine_kernel(), r = kernel ? jpog_engine_renderer(kernel) : 0;
    if (!r || !jpog_engine_display(r, 1024, 768)) { fprintf(stderr, "[view] the engine did not start\n"); return 1; }
    jpog_call(R, "?CreateMaterialLibraryManager@TRenderInterface@Toshi@@QAEXXZ", r, 0);
    uint32_t mgr = jpog_call(R, "?GetMaterialLibraryManager@TRenderInterface@Toshi@@QAEPAVTMaterialLibraryManager@2@XZ", r, 0);
    uint32_t skin = engine_system(r, SYS_SHSKIN), scene = engine_system(r, SYS_SCENE);
    fprintf(stderr, "[view] skin shader %08X, scene %08X, materials %08X\n", skin, scene, mgr);
    jpog_call(R, "?SetShader@TMaterialLibraryManager@Toshi@@QAEXHPAVTShader@2@@Z", mgr, 2, 0u, skin);
    if (materials) {
        uint32_t lib = jpog_call(R, "?LoadMaterialLibrary@TMaterialLibraryManager@Toshi@@QAEIPBD@Z", mgr, 1, S(materials));
        fprintf(stderr, "[view] %s: %08X\n", materials, lib);
    }
    char named[64];
    if (!keys) {                             /* the model's header names its keyframe library */
        char host[4096], guest[300], name[17] = { 0 };
        snprintf(guest, sizeof guest, "C:\\%s", model);
        FILE *f = hle_host_path(guest, host, sizeof host) ? fopen(host, "rb") : NULL;
        if (f && fseek(f, 12, SEEK_SET) == 0 && fread(name, 1, 12, f) == 12 && name[0]) {
            snprintf(named, sizeof named, "Data/Models/%s.tkl", name);
            snprintf(guest, sizeof guest, "C:\\%s", named);
            if (hle_host_path(guest, host, sizeof host)) keys = named;
        }
        if (f) fclose(f);
    }
    if (keys) {
        uint32_t km = jpog_call(R, "?GetKeyframeLibraryManager@TRenderInterface@Toshi@@QAEAAVTKeyframeLibraryManager@2@XZ", r, 0);
        uint32_t lib = jpog_call(R, "?LoadLibrary@TKeyframeLibraryManager@Toshi@@QAEPAVTKeyframeLibrary@2@PBD@Z", km, 1, S(keys));
        fprintf(stderr, "[view] %s: %08X\n", keys, lib);
    }
    uint32_t cls = jpog_export(R, "?m_sClass@TGFXFileObject@Toshi@@2VTClass@2@A");
    uint32_t gfx = jpog_call(R, "?CreateResource@TRenderInterface@Toshi@@QAEPAVTResource@2@PBVTClass@2@PBDPAV32@@Z", r, 3, cls, S("model"),
                             engine_system(r, SYS_GFXFILES));
    uint32_t ok = gfx ? jpog_call(R, "?Create@TGFXFileObject@Toshi@@UAE_NPAD@Z", gfx, 1, S(model)) & 0xFF : 0;
    uint32_t tm = ok ? jpog_call(R, "?GetModel@TGFXFileObject@Toshi@@QAEPAVTModel@2@XZ", gfx, 0) : 0;
    if (!tm) { fprintf(stderr, "[view] %s did not load\n", model); return 1; }
    uint32_t sphere = jpog_call(R, "?GetVisSphere@TModel@Toshi@@QAEAAVTSphere@2@XZ", tm, 0);
    fprintf(stderr, "[view] %s: %u levels of detail, sphere %g %g %g radius %g\n", model,
            jpog_call(R, "?GetLODCount@TModel@Toshi@@QAEHXZ", tm, 0), (double)rd(sphere), (double)rd(sphere + 4), (double)rd(sphere + 8),
            (double)rd(sphere + 12));

    uint32_t vp = engine_viewport(scene, "view", 0x50, 0x70, 0x90);
    uint32_t cam = jpog_call(R, "?CreateCamera@TScene@Toshi@@QAEPAVTCameraObject@2@PBDPAVTResource@2@@Z", scene, 2, S("camera"), 0u);
    jpog_call(R, "?SetViewport@TCameraObject@Toshi@@QAEPAVTViewport@2@PAV32@@Z", cam, 1, vp);
    jpog_call(R, "?SetCurrentCamera@TScene@Toshi@@QAEXPAVTCameraObject@2@@Z", scene, 1, cam);
    jpog_call(R, "?SetNear@TCameraObject@Toshi@@QAEMM@Z", cam, 1, F(0.1f)), (void)fp_pop();
    jpog_call(R, "?SetFar@TCameraObject@Toshi@@QAEMM@Z", cam, 1, F(500.0f)), (void)fp_pop();
    jpog_call(R, "?Enable@TCameraObject@Toshi@@QAE_N_N@Z", cam, 1, 1u);
    uint32_t inst = jpog_call(R, "?CreateInstance@TGFXFileObject@Toshi@@QAEPAVTGFXFileInstanceObject@2@PBDPAVTResource@2@@Z", gfx, 2, S("instance"), scene);
    uint32_t mi = inst ? jpog_call(R, "?GetModelInstance@TGFXFileInstanceObject@Toshi@@QAEPAVTModelInstance@2@XZ", inst, 0) : 0;
    fprintf(stderr, "[view] viewport %08X, camera %08X, instance %08X, model instance %08X\n", vp, cam, inst, mi);
    if (anim >= 0 && mi) {
        uint32_t sk = jpog_call(R, "?GetSkeletonInstance@TModelInstance@Toshi@@QAEPAVTSkeletonInstance@2@XZ", mi, 0);
        if (sk) jpog_call(R, "?AddAnimation@TSkeletonInstance@Toshi@@QAEPAVTAnimation@2@FMM@Z", sk, 3, (uint32_t)anim, F(1.0f), F(0.0f));
    }
    const char *zs = getenv("JPOG_VIEW_ZOOM");               /* how far back, in radii */
    float zoom = zs ? (float)atof(zs) : 0.6f;
    /* The models are z up (3ds Max's way), the world y down: the game stands
     * each instance up, and so does this. */
    uint32_t it = inst ? jpog_call(R, "?GetTransformObject@TGFXFileInstanceObject@Toshi@@QAEPAVTTransformObject@2@XZ", inst, 0) : 0;
    if (it) {
        float q[4] = { sqrtf(0.5f), 0, 0, sqrtf(0.5f) };
        jpog_call(R, "?SetQuat@TTransformObject@Toshi@@QAEXABVTQuaternion@2@@Z", it, 1, hle_scratch_copy(q, 16));
    }
    float centre[3] = { rd(sphere), -rd(sphere + 8), rd(sphere + 4) };
    uint32_t ct = jpog_call(R, "?GetTransformObject@TCameraObject@Toshi@@QAEPAVTTransformObject@2@XZ", cam, 0);
    float rad = rd(sphere + 12);
    if (!(rad > 0.01f && rad < 1000)) {      /* none kept for it: 9.87e7 everywhere */
        rad = 8;
        if (!(fabsf(centre[0]) < 1000)) memset(centre, 0, sizeof centre);
    }
    uint64_t start = hle_mono_ns();
    int i = 0;
    for (; (!frames || i < frames) && !engine_pump(); i++) {
        uint32_t mark = hle_scratch_mark();
        float a = 0.7f + (float)i * 0.005f, dist = rad * zoom;    /* round it, looking at its middle; y points down */
        float t[3] = { centre[0] - sinf(a) * dist, centre[1] - dist * 0.13f, centre[2] - cosf(a) * dist };
        jpog_call(R, "?SetTranslate@TTransformObject@Toshi@@QAEXABVTVector3@2@@Z", ct, 1, hle_scratch_copy(t, 12));
        jpog_call(R, "?SetQuat@TTransformObject@Toshi@@QAEXABVTQuaternion@2@@Z", ct, 1, turn(a, -0.13f));
        jpog_call(D, "?BeginScene@TRenderD3DInterface@Toshi@@UAE_NXZ", r, 0);
        engine_scene(scene, 1 / 60.0f);
        jpog_call(R, "?End@TScene@Toshi@@QAE_NXZ", scene, 0);
        jpog_call(R, "?FlushShaders@TRenderInterface@Toshi@@QAEXXZ", r, 0);
        if (shot && frames && i == frames - 1) fprintf(stderr, "[view] shot %s: %s\n", shot, jpog_d3d8_shot(shot) ? "ok" : "no");
        jpog_call(D, "?EndScene@TRenderD3DInterface@Toshi@@UAE_NXZ", r, 0);
        if (i == 0 && getenv("JPOG_VIEW_MATRICES")) {
            uint32_t m = hle_scratch(64), q = jpog_export(K, "?IDENTITY@TQuaternion@Toshi@@2V12@B");
            if (q) fprintf(stderr, "identity %g %g %g %g\n", (double)rd(q), (double)rd(q + 4), (double)rd(q + 8), (double)rd(q + 12));
            jpog_call(R, "?GetLocalMatrix@TTransformObject@Toshi@@UAEXAAVTMatrix44@2@@Z", ct, 1, m);
            for (int k = 0; k < 4; k++) fprintf(stderr, "camera %g %g %g %g\n", (double)rd(m + 16u * (uint32_t)k), (double)rd(m + 16u * (uint32_t)k + 4), (double)rd(m + 16u * (uint32_t)k + 8), (double)rd(m + 16u * (uint32_t)k + 12));
            dev_t_ *d = OBJ(jpog_engine_device(r), dev_t_);
            for (int x = 2; x < 4; x++) for (int k = 0; k < 4; k++) fprintf(stderr, "%s %g %g %g %g\n", x == 2 ? "view" : "proj", (double)d->transform[x][4 * k], (double)d->transform[x][4 * k + 1], (double)d->transform[x][4 * k + 2], (double)d->transform[x][4 * k + 3]);
            for (int k = 0; k < 16; k++) fprintf(stderr, "c%d %g %g %g %g\n", k, (double)d->vsc[k][0], (double)d->vsc[k][1], (double)d->vsc[k][2], (double)d->vsc[k][3]);
        }
        hle_scratch_release(mark);
    }
    double took = (double)(hle_mono_ns() - start) / 1e9;
    fprintf(stderr, "[view] %d frames in %.1f s, %.0f a second\n", i, took, took > 0 ? i / took : 0);
    if (jpog_verbose) jpog_d3d8_summary();
    mach_leave();
    return 0;
}

/* ---- terrain ---------------------------------------------------------------
 * An island, its sky, sea and grass, as the game has them: ASimJPTerrain (the
 * DLL's) makes the ground, TTerrainInterface its shaders and sections. What
 * SimJP.exe does is done here: cull the sections and activate what is seen,
 * and say what the weather is (Data/Weather.ini). */
#define T "TTerrainInterface.dll"
#define J "SimJPTerrain.dll"
#define H "TTerrainShaderD3D.dll"
enum { SECTIONS = 63 };                      /* a side, of 16 cells: islanub.hm is 1008 by 1008 */

/* A Weather.ini list: section { ... key = {a, b, ...}; }. How many it read. */
static int weather(const char *ini, const char *section, const char *key, float *out, int n) {
    const char *s = ini, *end;
    size_t sl = strlen(section), kl = strlen(key);
    do {                                     /* the section's name, on a line of its own */
        s = strstr(s, section);
        if (!s) return 0;
        s += sl;
    } while ((s - sl != ini && s[-(ptrdiff_t)sl - 1] != '\n') || (*s != '\r' && *s != '\n'));
    end = strstr(s, "\n}");
    for (const char *k = s; (k = strstr(k, key)) && (!end || k < end); k += kl) {
        const char *v = k + kl;
        if (k[-1] != '\t' && k[-1] != ' ' && k[-1] != '\n') continue;
        while (*v == ' ' || *v == '\t') v++;
        if (*v++ != '=') continue;
        int i = 0;
        while (*v == ' ' || *v == '\t' || *v == '{') v++;
        for (; i < n; i++) {
            char *e;
            out[i] = strtof(v, &e);
            if (e == v) break;
            for (v = e; *v == ' ' || *v == ',' || *v == '\t'; v++) ;
        }
        return i;
    }
    return 0;
}
static char *read_text(const char *guest) {
    char host[4096];
    FILE *f = hle_host_path(guest, host, sizeof host) ? fopen(host, "rb") : NULL;
    char *s = f ? calloc(1, 1 << 16) : NULL;
    if (s) (void)!fread(s, 1, (1 << 16) - 1, f);
    if (f) fclose(f);
    return s;
}
/* The context's planes face out of the view; the terrain's cull wants them in. */
static uint32_t inward(uint32_t planes) {
    float p[24];
    for (uint32_t k = 0; k < 24; k++) p[k] = -rd(planes + 4 * k);
    return hle_scratch_copy(p, sizeof p);
}
static uint32_t V(float x, float y, float z) { float v[3] = { x, y, z }; return hle_scratch_copy(v, 12); }

static void terrain_weather(uint32_t terrain, uint32_t shader, uint32_t ctx, const char *name) {
    char *ini = read_text("C:\\Data\\Weather.ini");
    float a[3] = { .8f, .8f, .8f }, b[3] = { 1, 1, 1 }, c[4][3], s[20];
    if (!ini) return;
    weather(ini, name, "pc_ambient", a, 3);
    weather(ini, name, "pc_sun", b, 3);
    jpog_call(T, "?SetLightColours@TTerrainInterface@Toshi@@QAEXABVTVector3@2@0@Z", terrain, 2, V(a[0], a[1], a[2]), V(b[0], b[1], b[2]));
    if (weather(ini, name, "pc_fronttop", c[0], 3) == 3 && weather(ini, name, "pc_frontbot", c[1], 3) == 3 &&
        weather(ini, name, "pc_backtop", c[2], 3) == 3 && weather(ini, name, "pc_backbot", c[3], 3) == 3)
        jpog_call(H, "?SetLowEndSkyCols@TTerrainShaderHAL@Toshi@@UAEXABVTVector3@2@000@Z", shader, 4, V(c[0][0], c[0][1], c[0][2]),
                  V(c[1][0], c[1][1], c[1][2]), V(c[2][0], c[2][1], c[2][2]), V(c[3][0], c[3][1], c[3][2]));
    if (ctx && weather(ini, name, "pc_scatlight", s, 20) == 20) {
        /* The atmosphere (Rayleigh, Mie, inscattering, Henyey-Greenstein's g),
         * the sun (heading, degrees from overhead, intensity), extinction, how
         * near and far (of the far plane), two colours, the sun's bias: the
         * order is read from TAtmosphere::SetAll and TSun::Set. */
        uint32_t air = jpog_call(R, "?GetAtmosphere@TRenderContext@Toshi@@QAEAAVTAtmosphere@2@XZ", ctx, 0);
        uint32_t sun = jpog_call(R, "?GetSun@TRenderContext@Toshi@@QAEAAVTSun@2@XZ", ctx, 0);
        jpog_call(R, "?SetAll@TAtmosphere@Toshi@@QAEXMMABVTVector3@2@0MMM@Z", air, 7, F(s[0]), F(s[1]), V(s[10], s[11], s[12]),
                  V(s[13], s[14], s[15]), F(s[2]), F(s[7]), F(s[3]));
        jpog_call(R, "?SetNear@TAtmosphere@Toshi@@QAEXM@Z", air, 1, F(s[8]));
        jpog_call(R, "?SetFar@TAtmosphere@Toshi@@QAEXM@Z", air, 1, F(s[9]));
        jpog_call(R, "?Set@TSun@Toshi@@QAEXMMABVTVector3@2@M@Z", sun, 4, F(s[5] * (float)M_PI / 180), F(s[6]), V(s[16], s[17], s[18]), F(s[19]));
    }
    float col[3], nf[2];
    if (ctx && weather(ini, name, "pc_fogcolour", col, 3) == 3 && weather(ini, name, "pc_fognear", nf, 1) && weather(ini, name, "pc_fogfar", nf + 1, 1)) {
        jpog_call(R, "?SetFogColour@TRenderContext@Toshi@@QAEXMMM@Z", ctx, 3, F(col[0] / 255), F(col[1] / 255), F(col[2] / 255));
        fprintf(stderr, "[terrain] weather %s: ambient %g, sun %g, fog %g to %g\n", name, (double)a[0], (double)b[0], (double)nf[0], (double)nf[1]);
    }
    free(ini);
}
/* The sky's pictures, by weather, in its five slots: it blends two. */
static void terrain_sky(uint32_t r, uint32_t shader, const char *name) {
    static const struct { const char *weather, *file; } sky[] = { { "normal", "Data/skynorm.dds" }, { "heatwave", "Data/skyheat.dds" },
        { "lightrain", "Data/skyrainL.dds" }, { "heavyrain", "Data/skyrain.dds" }, { "tornado", "Data/skytorn.dds" } };
    uint32_t factory = engine_system(r, SYS_TEXTURES), which = 0;
    for (uint32_t i = 0; i < 5; i++) {
        uint32_t t = jpog_call(D, "?CreateTextureFromFile@TTextureFactoryHAL@Toshi@@UAEPAVTTextureResource@2@PBDI@Z", factory, 2, S(sky[i].file), 0u);
        uint32_t tex = t ? jpog_call(D, "?GetD3DTexture@TTextureResourceHAL@Toshi@@QAEPAUIDirect3DTexture8@@XZ", t, 0) : 0;
        if (!tex) fprintf(stderr, "[terrain] %s did not load\n", sky[i].file);
        jpog_call(H, "?SetSkyTex@TTerrainShaderHAL@Toshi@@UAEXHPAUIDirect3DTexture8@@@Z", shader, 2, i, tex);
        if (!strcmp(sky[i].weather, name)) which = i;
    }
    jpog_call(H, "?SetSkyBlend@TTerrainShaderHAL@Toshi@@UAEXHHM@Z", shader, 3, which, which, F(0));
}

/* The sections in the context's view, drawn: each is meshed as it is activated. */
static uint32_t terrain_draw(uint32_t r, uint32_t t, uint32_t ctx, uint32_t pos) {
    uint32_t list = hle_scratch(4), n = 0;
    jpog_call(R, "?SetCurrentRenderContext@TRenderInterface@Toshi@@QAEPAVTRenderContext@2@PAV32@@Z", r, 1, ctx);
    uint32_t planes = inward(jpog_call(R, "?GetWorldPlanes@TRenderContext@Toshi@@QAEPBVTPlane@2@XZ", ctx, 0));
    uint32_t guard = inward(jpog_call(R, "?GetWorldGuardPlanes@TRenderContext@Toshi@@QAEPBVTPlane@2@XZ", ctx, 0));
    jpog_call(T, "?CullTerrain@TTerrainInterface@Toshi@@UAEXPAVTPlane@2@IPAPAVTTerrainSection@2@I0H@Z", t, 6, planes, 6u, list, 0x3Fu, guard, 0x3Fu);
    for (uint32_t q = MEM32(list); q; q = jpog_call(T, "?NextSection@TTerrainSection@Toshi@@QBEPAV12@XZ", q, 0), n++)
        jpog_call(T, "?ActivateTerrainSection@TTerrainInterface@Toshi@@UAEPAVTTerrainSection@2@PAV32@PAVTVector3@2@@Z", t, 2, q, pos);
    jpog_call(R, "?FlushShaders@TRenderInterface@Toshi@@QAEXXZ", r, 0);
    return n;
}

int jpog_terrain(const char *island, const char *shot, int frames) {
    engine_window_shot();
    mach_enter();
    uint32_t kernel = jpog_engine_kernel(), r = kernel ? jpog_engine_renderer(kernel) : 0;
    if (!r || !jpog_engine_display(r, 1024, 768)) { fprintf(stderr, "[terrain] the engine did not start\n"); return 1; }
    jpog_call(R, "?CreateMaterialLibraryManager@TRenderInterface@Toshi@@QAEXXZ", r, 0);
    uint32_t scene = engine_system(r, SYS_SCENE);
    uint64_t t0 = hle_mono_ns();
    uint32_t t = jpog_call(J, "?CreateObject@ASimJPTerrain@@CGPAVTObject@Toshi@@XZ", 0, 0);
    if (!t) { fprintf(stderr, "[terrain] no terrain\n"); return 1; }
    jpog_call(J, "?SetKernel@ASimJPTerrain@@QAEXPAVTKernelInterface@Toshi@@@Z", t, 1, kernel);
    jpog_call(J, "?Create@ASimJPTerrain@@UAEXIIPAVTRenderInterface@Toshi@@PBD@Z", t, 4, SECTIONS, SECTIONS, r, S("terrain"));
    uint32_t sh = jpog_call(T, "?GetTerrainShader@TTerrainInterface@Toshi@@QAEPAVTTerrainShader@2@XZ", t, 0);
    fprintf(stderr, "[terrain] terrain %08X, shader %08X: high end ground %u, detail %u, water %u\n", t, sh,
            sh ? jpog_call(H, "?IsHighEndTerrain@TTerrainShaderHAL@Toshi@@UAE_NXZ", sh, 0) & 0xFF : 0,
            sh ? jpog_call(H, "?IsTerrainDetailPass@TTerrainShaderHAL@Toshi@@UAE_NXZ", sh, 0) & 0xFF : 0,
            sh ? jpog_call(H, "?IsHighEndWater@TTerrainShaderHAL@Toshi@@UAE_NXZ", sh, 0) & 0xFF : 0);
    if (!sh) return 1;
    if (!strcmp(island, "nublar")) jpog_call(J, "?GenerateIslaNublar@ASimJPTerrain@@QAEXXZ", t, 0);
    else {                                   /* a made one: this seed, and the generator's own defaults */
        static const char *const def[] = { "fDefaultBeachHeight@ASimJPHeightField@@2MA", "fDefaultFalloff@ASimJPHeightField@@2MA",
            "fDefaultIslandShapePower@ASimJPHeightField@@2MA", "fDefaultHillHeight@ASimJPHeightField@@2MA",
            "iDefaultNoOfMountains@ASimJPHeightField@@2HA", "iDefaultMaxMountainRadius@ASimJPHeightField@@2HA",
            "iDefaultMaxMountainHeight@ASimJPHeightField@@2HA", "iDefaultNoOfRivers@ASimJPHeightField@@2HA",
            "iDefaultNoOfRiverJoins@ASimJPHeightField@@2HA", "fDefaultMinPercentBuildableLand@ASimJPHeightField@@2MA",
            "fDefaultMaxPercentBuildableLand@ASimJPHeightField@@2MA", "fDefaultWaterHeight@ASimJPHeightField@@2MA" };
        uint32_t v[12];
        for (int k = 0; k < 12; k++) {
            char n[96];
            snprintf(n, sizeof n, "?sm_%s", def[k]);
            uint32_t va = jpog_export(J, n);
            v[k] = va ? MEM32(va) : 0;
        }
        jpog_call(J, "?GenerateCustom@ASimJPTerrain@@QAEXIMMMMHHHHHMMM@Z", t, 13, (uint32_t)strtoul(island, NULL, 0), v[0], v[1], v[2], v[3], v[4],
                  v[5], v[6], v[7], v[8], v[9], v[10], v[11]);
    }
    fprintf(stderr, "[terrain] island %s in %.1f s\n", island, (double)(hle_mono_ns() - t0) / 1e9);

    uint32_t vp = engine_viewport(scene, "view", 0x8C, 0x96, 0xE6);
    uint32_t cam = jpog_call(R, "?CreateCamera@TScene@Toshi@@QAEPAVTCameraObject@2@PBDPAVTResource@2@@Z", scene, 2, S("camera"), 0u);
    jpog_call(R, "?SetViewport@TCameraObject@Toshi@@QAEPAVTViewport@2@PAV32@@Z", cam, 1, vp);
    jpog_call(R, "?SetCurrentCamera@TScene@Toshi@@QAEXPAVTCameraObject@2@@Z", scene, 1, cam);
    const char *fs = getenv("JPOG_TERRAIN_FAR");
    float far_ = fs ? (float)atof(fs) : 150;     /* the sky is 145 across (TTerrainInterface::Create) and hides what is beyond */
    jpog_call(R, "?SetNear@TCameraObject@Toshi@@QAEMM@Z", cam, 1, F(1.0f)), (void)fp_pop();
    jpog_call(R, "?SetFar@TCameraObject@Toshi@@QAEMM@Z", cam, 1, F(far_)), (void)fp_pop();
    jpog_call(R, "?Enable@TCameraObject@Toshi@@QAE_N_N@Z", cam, 1, 1u);
    uint32_t ct = jpog_call(R, "?GetTransformObject@TCameraObject@Toshi@@QAEPAVTTransformObject@2@XZ", cam, 0);
    uint32_t ctx = jpog_call(R, "?GetRenderContext@TViewport@Toshi@@QAEPAVTRenderContext@2@XZ", vp, 0);
    const char *wn = getenv("JPOG_WEATHER"), *at = getenv("JPOG_TERRAIN_AT");
    if (!wn) wn = "normal";
    terrain_weather(t, sh, ctx, wn);
    terrain_sky(r, sh, wn);
    float px = 504, pz = 504, up = 40, head = 0.6f, dip = -0.25f;
    if (at) sscanf(at, "%f,%f,%f,%f,%f", &px, &pz, &up, &head, &dip);
    uint64_t start = hle_mono_ns();
    int i = 0;
    for (; (!frames || i < frames) && !engine_pump(); i++) {
        uint32_t mark = hle_scratch_mark(), pos;
        while (jpog_headless && hle_mono_ns() - start < (uint64_t)i * 16666667u) usleep(500);   /* a frame's time: the sea moves by it */
        jpog_call(K, "?Update@TKernelInterface@Toshi@@QAE_NXZ", kernel, 0);
        float a = head + (float)i * 0.002f;
        jpog_call(T, "?GetHeight@TTerrainInterface@Toshi@@QBEMMM@Z", t, 2, F(px), F(pz));
        float ground = (float)fp_pop();      /* up from the sea; the world's y points down */
        if (i == 0) fprintf(stderr, "[terrain] the ground at %g, %g is %g\n", (double)px, (double)pz, (double)ground);
        pos = V(px, -(ground > 0 ? ground : 0) - up, pz);
        jpog_call(D, "?BeginScene@TRenderD3DInterface@Toshi@@UAE_NXZ", r, 0);
        jpog_call(T, "?TickLODCounter@TTerrainSection@Toshi@@SGXAAVTTerrainInterface@2@@Z", 0, 1, t);
        /* What the sea reflects: the view of a world turned over about y = 0,
         * as the shader has it (it culls the other way round). */
        jpog_call(R, "?SetTranslate@TTransformObject@Toshi@@QAEXABVTVector3@2@@Z", ct, 1, pos);
        jpog_call(R, "?SetQuat@TTransformObject@Toshi@@QAEXABVTQuaternion@2@@Z", ct, 1, turn(a, dip));
        engine_scene(scene, 1 / 60.0f);
        uint32_t view = jpog_call(R, "?GetWorldViewMatrix@TRenderContext@Toshi@@QBEABVTMatrix44@2@XZ", ctx, 0);
        float m[16];
        for (uint32_t k = 0; k < 16; k++) m[k] = k / 4 == 1 ? -rd(view + 4 * k) : rd(view + 4 * k);
        uint32_t over = hle_scratch_copy(m, sizeof m);
        jpog_call(R, "?SetWorldViewMatrix@TRenderContext@Toshi@@UAEXABVTMatrix44@2@@Z", ctx, 1, over);
        jpog_call(R, "?SetModelViewMatrix@TRenderContext@Toshi@@UAEXABVTMatrix44@2@@Z", ctx, 1, over);
        jpog_call(H, "?EnableRenderEnvMap@TTerrainShaderHAL@Toshi@@UAEX_N@Z", sh, 1, 1u);
        jpog_call(T, "?EnableRenderEnvMap@TTerrainInterface@Toshi@@QAEX_N@Z", t, 1, 1u);
        terrain_draw(r, t, ctx, pos);
        jpog_call(T, "?EnableRenderEnvMap@TTerrainInterface@Toshi@@QAEX_N@Z", t, 1, 0u);
        jpog_call(H, "?EnableRenderEnvMap@TTerrainShaderHAL@Toshi@@UAEX_N@Z", sh, 1, 0u);
        jpog_call(R, "?End@TScene@Toshi@@QAE_NXZ", scene, 0);
        engine_scene(scene, 1 / 60.0f);
        uint32_t n = terrain_draw(r, t, ctx, pos);
        jpog_call(R, "?End@TScene@Toshi@@QAE_NXZ", scene, 0);
        if (i == 0) fprintf(stderr, "[terrain] %u sections in view\n", n);
        if (shot && frames && i == frames - 1) fprintf(stderr, "[terrain] shot %s: %s\n", shot, jpog_d3d8_shot(shot) ? "ok" : "no");
        jpog_call(D, "?EndScene@TRenderD3DInterface@Toshi@@UAE_NXZ", r, 0);
        hle_scratch_release(mark);
    }
    double took = (double)(hle_mono_ns() - start) / 1e9;
    fprintf(stderr, "[terrain] %d frames in %.1f s, %.0f a second\n", i, took, took > 0 ? i / took : 0);
    if (jpog_verbose) jpog_d3d8_summary();
    mach_leave();
    return 0;
}
