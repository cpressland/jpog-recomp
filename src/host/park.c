/*
 * park.c - --park: everything of the engine's the host can run without
 * SimJP.exe, at once and in one window: an island and its sky, sea and grass,
 * dinosaurs walking on it, trees and buildings, the HUD and its mouse, music,
 * and 3D sound heard from the camera. Not the game: nothing decides anything.
 * It is there to find what only shows when the parts run together.
 *
 *   jpog --game DIR --park [--windowed] [--headless] [--frames N] [--shot FILE.bmp]
 *
 * The camera goes round the dinosaurs until a key is pressed: W A S D, the
 * arrows to turn, Q and E down and up, Tab for the next dinosaur. A HUD button
 * opens and closes a dialog. JPOG_PARK_FOLLOW=N: the dinosaur it starts at;
 * JPOG_PARK_BEHIND=1: from behind it;
 * JPOG_PARK_KEYS, JPOG_PARK_PRESS: keys held, and a press, without a hand.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "engine.h"
#include "gui.h"

/* Who is in it: the walking speeds, with each walk. */
static const struct { const char *model, *materials, *walk, *call; float speed, size; } kind[] = {
    { "TRex",    "dinos_judithriver_b",  "Walk_lp", "Trex/TrexRoar01",      5.6f,  5 },
    { "Raptor",  "dinos_flamingcliffs",  "walk_lp", "Raptor/RapBark01",     1.3f,  2 },
    { "Tricera", "dinos_hellcreek_b",    "Walk_Lp", "Tricera/TricCall01",   2.64f, 4 },
    { "Brach",   "dinos_tendaguru",      "walk_lp", "Brach/BrachCall01",    0.8f,  9 },
    { "Para",    "dinos_hellcreek_a",    "Walk_lp", "Para/ParaCall01",      2.0f,  4 },
    { "Steg",    "dinos_morrison_a",     "Walk_Lp", "Steg/Stegcall01",      0.9f,  4 },
};
enum { KINDS = sizeof kind / sizeof *kind, HERD = 2, DINOS = KINDS * HERD, THINGS = 60 };
typedef struct { uint32_t place; int kind; float cx, cz, radius, angle, x, z, heading; uint32_t voice; double speaks; } dino_t;
static dino_t g_dino[DINOS];
static world_t g_w;
static float g_at[3], g_heading, g_pitch;
static uint32_t g_sections, g_dialog, g_call[KINDS], g_click;
static int g_open;
static const float MIDDLE_X = 470, MIDDLE_Z = 560;

/* ---- sound: fmod.dll's calls -------------------------------------------------- */
static uint32_t ask(const char *name, int n, const uint32_t *a) { return hle_call_guest(hle_resolve(name), n, a); }
#define ASK(name, n, ...) ask("_FSOUND_" name, n, (uint32_t[]){ __VA_ARGS__ })
#define FREE 0xFFFFFFFFu
static uint32_t sample(const char *file, uint32_t mode) {
    char path[128];
    snprintf(path, sizeof path, "Data/Sound/%s.wav", file);
    uint32_t s = ASK("Sample_Load@16", 4, FREE, S(path), mode, 0);
    if (!s) fprintf(stderr, "[park] no %s\n", path);
    return s;
}
/* FMOD's ear and the world: the same, but that the world's y points down. */
static void ear(void) {
    uint32_t p = V(g_at[0], -g_at[1], g_at[2]), v = V(0, 0, 0);
    float c = cosf(g_pitch), f[3] = { sinf(g_heading) * c, sinf(g_pitch), cosf(g_heading) * c }, t[3] = { -sinf(g_heading) * f[1], c, -cosf(g_heading) * f[1] };
    ASK("3D_Listener_SetAttributes@32", 8, p, v, F(f[0]), F(f[1]), F(f[2]), F(t[0]), F(t[1]), F(t[2]));
}

/* ---- the world's viewport: the ground goes in between ------------------------- */
static void park_Reflect(void) { world_reflect(&g_w, g_at); RET(1, 3); }
static void park_Ground(void)  { g_sections = world_draw(&g_w, g_at); RET(1, 3); }
static const win32hle_shim park_listeners[] = { { "park_Reflect", park_Reflect }, { "park_Ground", park_Ground }, { NULL, NULL } };

static float ground(float x, float z) { float g = world_ground(&g_w, x, z); return g > 0 ? g : 0; }
/* An instance on the ground. The models are z up, and look back the way they are turned. */
static uint32_t put(uint32_t gfx, float x, float z, float heading) {
    uint32_t inst = engine_instance(gfx, g_w.scene);
    uint32_t place = inst ? jpog_call(R, "?GetTransformObject@TGFXFileInstanceObject@Toshi@@QAEPAVTTransformObject@2@XZ", inst, 0) : 0;
    if (!place) return 0;
    jpog_call(R, "?SetTranslate@TTransformObject@Toshi@@QAEXABVTVector3@2@@Z", place, 1, V(x, -ground(x, z), z));
    jpog_call(R, "?SetQuat@TTransformObject@Toshi@@QAEXABVTQuaternion@2@@Z", place, 1, engine_turn(heading, (float)M_PI / 2));
    return place;
}
static int walk(uint32_t gfx, uint32_t place_inst, const char *name) {
    (void)gfx;
    uint32_t mi = place_inst ? jpog_call(R, "?GetModelInstance@TGFXFileInstanceObject@Toshi@@QAEPAVTModelInstance@2@XZ", place_inst, 0) : 0;
    uint32_t sk = mi ? jpog_call(R, "?GetSkeletonInstance@TModelInstance@Toshi@@QAEPAVTSkeletonInstance@2@XZ", mi, 0) : 0;
    uint32_t sn = sk ? jpog_call(R, "?GetSkeleton@TSkeletonInstance@Toshi@@QAEPAVTSkeleton@2@XZ", sk, 0) : 0;
    int id = sn ? (int)jpog_call(R, "?GetSequenceID@TSkeleton@Toshi@@QAEHPBDH@Z", sn, 2, S(name), (uint32_t)strlen(name)) : -1;
    if (id < 0) return 0;
    jpog_call(R, "?AddAnimation@TSkeletonInstance@Toshi@@QAEPAVTAnimation@2@FMM@Z", sk, 3, (uint32_t)id, F(1.0f), F(0.0f));
    return 1;
}
/* Somewhere dry near the middle, the same every time. */
static uint32_t g_seed = 12345;
static float chance(void) { g_seed = g_seed * 1103515245u + 12345u; return (float)((g_seed >> 16) & 0x7FFF) / 32768.0f; }
static void dry(float spread, float *x, float *z) {
    for (int i = 0; i < 200; i++) {
        *x = MIDDLE_X + (chance() - 0.5f) * spread, *z = MIDDLE_Z + (chance() - 0.5f) * spread;
        if (world_ground(&g_w, *x, *z) > 0.4f) return;
    }
}

static void people_the_island(void) {
    char path[96];
    int walking = 0, things = 0;
    for (int k = 0; k < KINDS; k++) {
        snprintf(path, sizeof path, "Data/matlibs/%s.tml", kind[k].materials);
        engine_materials(g_w.r, path);
        snprintf(path, sizeof path, "Data/Models/%s.tmd", kind[k].model);
        uint32_t gfx = engine_model(g_w.r, path, 1);
        for (int h = 0; h < HERD; h++) {
            dino_t *d = &g_dino[k * HERD + h];
            d->kind = k, d->radius = 6 + kind[k].size * 2, d->angle = chance() * 6.28f;
            dry(130, &d->cx, &d->cz);
            uint32_t inst = engine_instance(gfx, g_w.scene);
            d->place = inst ? jpog_call(R, "?GetTransformObject@TGFXFileInstanceObject@Toshi@@QAEPAVTTransformObject@2@XZ", inst, 0) : 0;
            walking += walk(gfx, inst, kind[k].walk);
        }
        g_call[k] = sample((snprintf(path, sizeof path, "Dino/%s", kind[k].call), path), 0x1000 | 1);
        if (g_call[k]) ASK("Sample_SetMinMaxDistance@12", 3, g_call[k], F(10), F(150));
    }
    static const char *const libs[] = { "Flora", "Structures", "Vehicles", "People" };
    for (unsigned i = 0; i < sizeof libs / sizeof *libs; i++) {
        snprintf(path, sizeof path, "Data/matlibs/%s.tml", libs[i]);
        engine_materials(g_w.r, path);
    }
    static const char *const trees[] = { "JPalmL", "TreeFrnL", "CocoPalm", "ChkFernL", "GreePalm", "TallPalm" }, *const built[] = { "WelcCntr", "Fountain", "jeep" };
    uint32_t tree[6], b[3];
    for (int i = 0; i < 6; i++) tree[i] = engine_model(g_w.r, (snprintf(path, sizeof path, "Data/Models/%s.tmd", trees[i]), path), 1);
    for (int i = 0; i < 3; i++) b[i] = engine_model(g_w.r, (snprintf(path, sizeof path, "Data/Models/%s.tmd", built[i]), path), 1);
    for (int i = 0; i < THINGS; i++) {
        float x, z;
        dry(180, &x, &z);
        things += put(tree[i % 6], x, z, chance() * 6.28f) != 0;
    }
    things += put(b[0], MIDDLE_X, MIDDLE_Z, 0) != 0;
    things += put(b[1], MIDDLE_X + 22, MIDDLE_Z + 12, 0) != 0;
    things += put(b[2], MIDDLE_X + 12, MIDDLE_Z - 18, 1) != 0;
    fprintf(stderr, "[park] %d dinosaurs, %d walking; %d trees and buildings\n", DINOS, walking, things);
}
static void pressed(const char *widget) {
    fprintf(stderr, "[park] \"%s\" pressed\n", widget);
    if (g_click) ASK("PlaySoundEx@16", 4, FREE, g_click, 0, 0);
    gui_visible(g_dialog, g_open = !g_open);
}
static int held(int vk) { return (hle_call_guest(hle_resolve("GetAsyncKeyState"), 1, (uint32_t[]){ (uint32_t)vk }) & 0x8000) != 0; }

int jpog_park(const char *shot, int frames) {
    uint32_t kernel, r;
    engine_window_shot();
    mach_enter();
    if (!engine_start(&kernel, &r)) return 1;
    uint64_t t0 = hle_mono_ns();
    ASK("Init@12", 3, 44100, 32, 0);
    if (!world_make(&g_w, kernel, r, "nublar", getenv("JPOG_WEATHER") ? getenv("JPOG_WEATHER") : "normal")) return 1;
    win32hle_register(park_listeners);
    engine_listen(jpog_call(R, "?GetBeginRenderEmitter@TViewport@Toshi@@QAEAAV?$TEmitter@VTViewport@Toshi@@VBeginRenderEvent@12@@2@XZ", g_w.vp, 0), "park_Reflect", 0, 1);
    engine_listen(jpog_call(R, "?GetRenderEmitter@TViewport@Toshi@@QAEAAV?$TEmitter@VTViewport@Toshi@@VRenderEvent@12@@2@XZ", g_w.vp, 0), "park_Ground", 0, 1);
    people_the_island();
    if (!gui_open(kernel, r, "Data/GUI/main.gui", "English", 0)) return 1;
    gui_show("MoneyHUD_WIN_TEST", 0, 0);
    gui_show("RadarHUD", 0.02f, 0.98f);
    g_dialog = gui_show("AParkFinanceDialog", 0.5f, 0.6f);
    gui_visible(g_dialog, 0);
    gui_pressed = pressed;
    g_click = sample("GUI/click01", 0x2000 | 1);
    uint32_t music = ASK("Stream_OpenFile@12", 3, S("Data/Music/Inpark.ogg"), 0x2000 | 2, 0), air = sample("World/World01/Forest_B01", 0x2000 | 2);
    uint32_t mc = music ? ASK("Stream_PlayEx@16", 4, FREE, music, 0, 0) : FREE, ac = air ? ASK("PlaySoundEx@16", 4, FREE, air, 0, 0) : FREE;
    ASK("SetVolume@8", 2, mc, 110), ASK("SetVolume@8", 2, ac, 90);
    fprintf(stderr, "[park] ready in %.1f s\n", (double)(hle_mono_ns() - t0) / 1e9);

    const char *fs = getenv("JPOG_PARK_FOLLOW"), *keys = getenv("JPOG_PARK_KEYS");
    int behind = getenv("JPOG_PARK_BEHIND") != NULL, follow = fs ? atoi(fs) % DINOS : 0, free_ = 0, tab = 0, i = 0, px = -1, py = -1, mx = GUI_W / 2, my = GUI_H / 2, pressed_at = 5, calls = 0;
    if (getenv("JPOG_PARK_PRESS")) sscanf(getenv("JPOG_PARK_PRESS"), "%d,%d", &px, &py);
    uint64_t start = hle_mono_ns(), worst = 0, last = start;
    double heard = 0;
    float peak = 0;
    static float buf[2 * 4096];
    for (; (!frames || i < frames) && !engine_pump(); i++) {
        uint32_t mark = hle_scratch_mark();
        while (jpog_headless && hle_mono_ns() - start < (uint64_t)i * 16666667u) usleep(500);
        uint64_t now = hle_mono_ns();
        if (i > 30 && now - last > worst) worst = now - last;
        float dt = (float)(now - last) / 1e9f, time = (float)(now - start) / 1e9f;
        last = now;
        if (dt > 0.1f) dt = 0.1f;
        jpog_call(K, "?Update@TKernelInterface@Toshi@@QAE_NXZ", kernel, 0);
        if (px >= 0 && i >= 5) {                     /* slowly: the engine doubles a fast move, as Windows did */
            int dx = px - mx, dy = py - my;
            dx = dx > 4 ? 4 : dx < -4 ? -4 : dx, dy = dy > 4 ? 4 : dy < -4 ? -4 : dy;
            if (dx || dy) hle_input_motion(dx, dy), mx += dx, my += dy, pressed_at = i + 5;
            if (i == pressed_at || i == pressed_at + 4) {
                int sx, sy;
                hle_input_position(&sx, &sy);
                hle_input_mouse(i == pressed_at ? 0x201 : 0x202, sx, sy);
            }
        }
        gui_frame(time, dt);

        for (int n = 0; n < DINOS; n++) {            /* round its circle, at its own pace */
            dino_t *d = &g_dino[n];
            if (!d->place) continue;
            d->angle += kind[d->kind].speed / d->radius * dt;
            d->x = d->cx + cosf(d->angle) * d->radius, d->z = d->cz + sinf(d->angle) * d->radius;
            d->heading = atan2f(-sinf(d->angle), cosf(d->angle));
            jpog_call(R, "?SetTranslate@TTransformObject@Toshi@@QAEXABVTVector3@2@@Z", d->place, 1, V(d->x, -ground(d->x, d->z), d->z));
            jpog_call(R, "?SetQuat@TTransformObject@Toshi@@QAEXABVTQuaternion@2@@Z", d->place, 1, engine_turn(d->heading + (float)M_PI, (float)M_PI / 2));
            uint32_t p = V(d->x, ground(d->x, d->z) + kind[d->kind].size, d->z);
            if (g_call[d->kind] && (double)time > d->speaks) {
                if (d->speaks > 0) {
                    d->voice = ASK("PlaySoundEx@16", 4, FREE, g_call[d->kind], 0, 1), calls++;
                    ASK("3D_SetAttributes@12", 3, d->voice, p, V(0, 0, 0));
                    ASK("SetPaused@8", 2, d->voice, 0);
                }
                d->speaks = (double)(time + 4 + chance() * 14);
            } else if (d->voice) ASK("3D_SetAttributes@12", 3, d->voice, p, V(0, 0, 0));
        }

        int any = 0;
        float go = 0, side = 0, up = 0;
#define KEY(vk, c) (held(vk) || (keys && i > 10 && strchr(keys, c)))
        if (KEY('W', 'w')) go += 1, any = 1;
        if (KEY('S', 's')) go -= 1, any = 1;
        if (KEY('A', 'a')) side -= 1, any = 1;
        if (KEY('D', 'd')) side += 1, any = 1;
        if (KEY('E', 'e')) up += 1, any = 1;
        if (KEY('Q', 'q')) up -= 1, any = 1;
        if (held(0x25)) g_heading -= dt * 1.5f, any = 1;
        if (held(0x27)) g_heading += dt * 1.5f, any = 1;
        if (held(0x26)) g_pitch += dt, any = 1;
        if (held(0x28)) g_pitch -= dt, any = 1;
        if (any) free_ = 1;
        int t = held(9);
        if (t && !tab) follow = (follow + 1) % DINOS, free_ = 0;
        tab = t;
        if (free_) {
            float fast = 14 * dt, floor_;
            g_at[0] += (sinf(g_heading) * go + cosf(g_heading) * side) * fast, g_at[2] += (cosf(g_heading) * go - sinf(g_heading) * side) * fast;
            g_at[1] -= up * fast;
            floor_ = -ground(g_at[0], g_at[2]) - 1.7f;
            if (g_at[1] > floor_) g_at[1] = floor_;
        } else {                                     /* round the one it follows, looking at it */
            const dino_t *d = &g_dino[follow];
            float a = behind ? d->heading : time * 0.12f + 0.6f, back = 2 + kind[d->kind].size * 1.8f, high = kind[d->kind].size * 0.4f + 0.8f;
            g_at[0] = d->x - sinf(a) * back, g_at[2] = d->z - cosf(a) * back;
            float under = ground(g_at[0], g_at[2]), here = ground(d->x, d->z);
            g_at[1] = -(under > here ? under : here) - high;
            g_heading = a, g_pitch = atan2f(here + kind[d->kind].size * 0.35f + g_at[1], back);
        }
        world_look(&g_w, g_at, g_heading, g_pitch);
        ear();
        ask("_FSOUND_Update@0", 0, NULL);

        jpog_call(D, "?BeginScene@TRenderD3DInterface@Toshi@@UAE_NXZ", r, 0);
        jpog_call("TTerrainInterface.dll", "?TickLODCounter@TTerrainSection@Toshi@@SGXAAVTTerrainInterface@2@@Z", 0, 1, g_w.terrain);
        engine_scene(g_w.scene, dt);
        jpog_call(R, "?End@TScene@Toshi@@QAE_NXZ", g_w.scene, 0);
        jpog_call(R, "?FlushShaders@TRenderInterface@Toshi@@QAEXXZ", r, 0);
        if (shot && frames && i == frames - 1) fprintf(stderr, "[park] shot %s: %s\n", shot, jpog_d3d8_shot(shot) ? "ok" : "no");
        jpog_call(D, "?EndScene@TRenderD3DInterface@Toshi@@UAE_NXZ", r, 0);
        if (jpog_headless) {                         /* what would have been heard */
            jpog_fsound_render(buf, 735);
            for (int k = 0; k < 1470; k++) { float f = fabsf(buf[k]); heard += (double)f; if (f > peak) peak = f; }
        }
        hle_scratch_release(mark);
    }
    double took = (double)(hle_mono_ns() - start) / 1e9;
    fprintf(stderr, "[park] %d frames in %.1f s, %.0f a second, the longest %.0f ms; %u sections, %d calls", i, took, took > 0 ? i / took : 0, (double)worst / 1e6, g_sections, calls);
    if (jpog_headless) fprintf(stderr, ", sound peak %.3f mean %.4f", (double)peak, heard / (1470.0 * (i ? i : 1)));
    fprintf(stderr, "\n");
    if (jpog_verbose) jpog_d3d8_summary();
    ask("_FSOUND_Close@0", 0, NULL);
    mach_leave();
    return 0;
}
