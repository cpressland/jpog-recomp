/*
 * terrain.c - an island, through the engine's own terrain: the world --terrain
 * shows, and --park plays in.
 *
 *   jpog --game DIR --terrain nublar|SEED [--headless] [--shot FILE.bmp] [--frames N]
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "engine.h"

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
static uint32_t sections(const world_t *w, uint32_t pos) {
    uint32_t list = hle_scratch(4), n = 0;
    jpog_call(R, "?SetCurrentRenderContext@TRenderInterface@Toshi@@QAEPAVTRenderContext@2@PAV32@@Z", w->r, 1, w->ctx);
    uint32_t planes = inward(jpog_call(R, "?GetWorldPlanes@TRenderContext@Toshi@@QAEPBVTPlane@2@XZ", w->ctx, 0));
    uint32_t guard = inward(jpog_call(R, "?GetWorldGuardPlanes@TRenderContext@Toshi@@QAEPBVTPlane@2@XZ", w->ctx, 0));
    jpog_call(T, "?CullTerrain@TTerrainInterface@Toshi@@UAEXPAVTPlane@2@IPAPAVTTerrainSection@2@I0H@Z", w->terrain, 6, planes, 6u, list, 0x3Fu, guard, 0x3Fu);
    for (uint32_t q = MEM32(list); q; q = jpog_call(T, "?NextSection@TTerrainSection@Toshi@@QBEPAV12@XZ", q, 0), n++)
        jpog_call(T, "?ActivateTerrainSection@TTerrainInterface@Toshi@@UAEPAVTTerrainSection@2@PAV32@PAVTVector3@2@@Z", w->terrain, 2, q, pos);
    jpog_call(R, "?FlushShaders@TRenderInterface@Toshi@@QAEXXZ", w->r, 0);
    return n;
}
uint32_t world_draw(const world_t *w, const float at[3]) { return sections(w, V(at[0], at[1], at[2])); }

/* What the sea reflects: the view of a world turned over about y = 0, as the
 * shader has it (it culls the other way round). The context is put back. */
void world_reflect(const world_t *w, const float at[3]) {
    uint32_t view = jpog_call(R, "?GetWorldViewMatrix@TRenderContext@Toshi@@QBEABVTMatrix44@2@XZ", w->ctx, 0);
    float m[16], o[16];
    for (uint32_t k = 0; k < 16; k++) o[k] = rd(view + 4 * k), m[k] = k / 4 == 1 ? -o[k] : o[k];
    for (int over = 1; over >= 0; over--) {
        uint32_t v = hle_scratch_copy(over ? m : o, sizeof m);
        jpog_call(R, "?SetWorldViewMatrix@TRenderContext@Toshi@@UAEXABVTMatrix44@2@@Z", w->ctx, 1, v);
        jpog_call(R, "?SetModelViewMatrix@TRenderContext@Toshi@@UAEXABVTMatrix44@2@@Z", w->ctx, 1, v);
        if (!over) break;
        jpog_call(H, "?EnableRenderEnvMap@TTerrainShaderHAL@Toshi@@UAEX_N@Z", w->shader, 1, 1u);
        jpog_call(T, "?EnableRenderEnvMap@TTerrainInterface@Toshi@@QAEX_N@Z", w->terrain, 1, 1u);
        sections(w, V(at[0], at[1], at[2]));
        jpog_call(T, "?EnableRenderEnvMap@TTerrainInterface@Toshi@@QAEX_N@Z", w->terrain, 1, 0u);
        jpog_call(H, "?EnableRenderEnvMap@TTerrainShaderHAL@Toshi@@UAEX_N@Z", w->shader, 1, 0u);
    }
}
float world_ground(const world_t *w, float x, float z) {
    jpog_call(T, "?GetHeight@TTerrainInterface@Toshi@@QBEMMM@Z", w->terrain, 2, F(x), F(z));
    return (float)fp_pop();                  /* the world's y points down; this is up from the sea */
}
void world_look(const world_t *w, const float at[3], float heading, float pitch) {
    jpog_call(R, "?SetTranslate@TTransformObject@Toshi@@QAEXABVTVector3@2@@Z", w->place, 1, V(at[0], at[1], at[2]));
    jpog_call(R, "?SetQuat@TTransformObject@Toshi@@QAEXABVTQuaternion@2@@Z", w->place, 1, engine_turn(heading, pitch));
}
int world_make(world_t *w, uint32_t kernel, uint32_t r, const char *island, const char *wn) {
    uint64_t t0 = hle_mono_ns();
    uint32_t t = jpog_call(J, "?CreateObject@ASimJPTerrain@@CGPAVTObject@Toshi@@XZ", 0, 0);
    if (!t) { fprintf(stderr, "[terrain] no terrain\n"); return 0; }
    w->kernel = kernel, w->r = r, w->terrain = t, w->scene = engine_system(r, SYS_SCENE);
    jpog_call(J, "?SetKernel@ASimJPTerrain@@QAEXPAVTKernelInterface@Toshi@@@Z", t, 1, kernel);
    jpog_call(J, "?Create@ASimJPTerrain@@UAEXIIPAVTRenderInterface@Toshi@@PBD@Z", t, 4, SECTIONS, SECTIONS, r, S("terrain"));
    uint32_t sh = w->shader = jpog_call(T, "?GetTerrainShader@TTerrainInterface@Toshi@@QAEPAVTTerrainShader@2@XZ", t, 0);
    if (!sh) return 0;
    fprintf(stderr, "[terrain] terrain %08X, shader %08X: high end ground %u, detail %u, water %u\n", t, sh,
            jpog_call(H, "?IsHighEndTerrain@TTerrainShaderHAL@Toshi@@UAE_NXZ", sh, 0) & 0xFF,
            jpog_call(H, "?IsTerrainDetailPass@TTerrainShaderHAL@Toshi@@UAE_NXZ", sh, 0) & 0xFF,
            jpog_call(H, "?IsHighEndWater@TTerrainShaderHAL@Toshi@@UAE_NXZ", sh, 0) & 0xFF);
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
    const char *fs = getenv("JPOG_TERRAIN_FAR");
    float far_ = fs ? (float)atof(fs) : 150; /* the sky is 145 across (TTerrainInterface::Create) and hides what is beyond */
    w->vp = engine_viewport(w->scene, "world", 0x8C, 0x96, 0xE6);
    uint32_t cam = engine_camera(w->scene, w->vp, 1, far_);
    w->place = jpog_call(R, "?GetTransformObject@TCameraObject@Toshi@@QAEPAVTTransformObject@2@XZ", cam, 0);
    w->ctx = jpog_call(R, "?GetRenderContext@TViewport@Toshi@@QAEPAVTRenderContext@2@XZ", w->vp, 0);
    terrain_weather(t, sh, w->ctx, wn);
    terrain_sky(r, sh, wn);
    return 1;
}

int jpog_terrain(const char *island, const char *shot, int frames) {
    world_t w;
    uint32_t kernel, r;
    const char *wn = getenv("JPOG_WEATHER"), *at = getenv("JPOG_TERRAIN_AT");
    engine_window_shot();
    mach_enter();
    if (!engine_start(&kernel, &r) || !world_make(&w, kernel, r, island, wn ? wn : "normal")) return 1;
    float px = 504, pz = 504, up = 40, head = 0.6f, dip = -0.25f;
    if (at) sscanf(at, "%f,%f,%f,%f,%f", &px, &pz, &up, &head, &dip);
    uint64_t start = hle_mono_ns();
    int i = 0;
    for (; (!frames || i < frames) && !engine_pump(); i++) {
        uint32_t mark = hle_scratch_mark();
        while (jpog_headless && hle_mono_ns() - start < (uint64_t)i * 16666667u) usleep(500);   /* a frame's time: the sea moves by it */
        jpog_call(K, "?Update@TKernelInterface@Toshi@@QAE_NXZ", kernel, 0);
        float ground = world_ground(&w, px, pz), pos[3] = { px, -(ground > 0 ? ground : 0) - up, pz };
        if (i == 0) fprintf(stderr, "[terrain] the ground at %g, %g is %g\n", (double)px, (double)pz, (double)ground);
        jpog_call(D, "?BeginScene@TRenderD3DInterface@Toshi@@UAE_NXZ", r, 0);
        jpog_call(T, "?TickLODCounter@TTerrainSection@Toshi@@SGXAAVTTerrainInterface@2@@Z", 0, 1, w.terrain);
        world_look(&w, pos, head + (float)i * 0.002f, dip);
        engine_scene(w.scene, 1 / 60.0f);
        world_reflect(&w, pos);
        jpog_call(R, "?End@TScene@Toshi@@QAE_NXZ", w.scene, 0);
        engine_scene(w.scene, 1 / 60.0f);
        uint32_t n = world_draw(&w, pos);
        jpog_call(R, "?End@TScene@Toshi@@QAE_NXZ", w.scene, 0);
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
