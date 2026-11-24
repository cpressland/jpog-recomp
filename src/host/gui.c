/*
 * gui.c - sprites and the GUI, driven by the host as the game drives them:
 * --gui, which shows one of the game's dialogs (Data/GUI/main.gui) through
 * TGUIInterface, its widgets (PGUICore.dll) and their renderer
 * (PGUIRenderer.dll, on the sprite shader).
 *
 *   jpog --game DIR --gui FrontEndMainMenu_WIN [--headless] [--shot FILE.bmp] [--frames N]
 *
 * --verbose: what is pressed. JPOG_GUI_BACK=NAME: a full-screen picture behind it (Data/matlibs/NAME.tml),
 * through the sprite shader. JPOG_GUI_MOUSE=x,y[,press]: the pointer put there
 * (of 640 by 480), and pressed, without a mouse. JPOG_GUI_MOVE=dx,dy: a mouse moved.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "d3d_int.h"
#include "engine.h"
#include "gui.h"

#define G "TGUIInterface.dll"
#define P "PGUIRenderer.dll"
#define SP "TSpriteShaderD3D.dll"
#define IN "TInputInterface.dll"
#define DX "TInputDXInterface.dll"

/* A TCString, and a pooled one (by value: what TPCString is, one pointer). */
static uint32_t cstring(const char *s) {
    uint32_t o = hle_va(hle_alloc(64));
    return jpog_call(K, "??0TCString@Toshi@@QAE@PBD@Z", o, 1, S(s));
}
static uint32_t pooled(const char *s) {
    uint32_t pool = jpog_call(K, "?GetCStringPool@TSystem@Toshi@@SGPAVTCStringPool@2@XZ", 0, 0), o = hle_va(hle_alloc(16));
    jpog_call(K, "?Get@TCStringPool@Toshi@@QAE?AVTPCString@2@PBD@Z", pool, 2, o, S(s));
    return o;
}

/* What SimJP.exe makes of the GUI's abstract classes, as far as a screen needs:
 * thiscall, so a shim's arguments are what is on the stack. */
static void gui_FindSound(void) { RET(0xFFFFFFFFu, 1); }     /* TGUISoundFactory +C (name): none */
static void gui_NoSound(void) { RET(0xFFFFFFFFu, 1); }
static void gui_PlaySound(void) { RET(0, 1); }               /* TGUIAudioContext +C (sound) */
static void gui_InputName(void) { fprintf(stderr, "[gui] an input's name was asked for\n"); RET(0, 1); }
#define E(f) { #f, f }
static const win32hle_shim vt_sounds[] = { E(gui_FindSound), E(gui_NoSound), E(gui_NoSound), { NULL, NULL } };
static const win32hle_shim vt_audio[] = { E(gui_PlaySound), { NULL, NULL } };
static const win32hle_shim vt_input[] = { E(gui_InputName), { NULL, NULL } };
/* An object of class cls's, its methods from +first the host's. */
static uint32_t host_object(const char *cls, uint32_t size, uint32_t first, const win32hle_shim *own, uint32_t slots) {
    char name[96];
    uint32_t o = hle_va(hle_alloc(size)), vt = hle_va(hle_alloc(4 * slots)), mine = hle_com_vtable(own);
    snprintf(name, sizeof name, "??0%s@Toshi@@QAE@XZ", cls);
    jpog_call(G, name, o, 0);
    for (uint32_t i = 0; i < slots; i++) MEM32(vt + 4 * i) = 4 * i < first ? MEM32(MEM32(o) + 4 * i) : MEM32(mine + 4 * i - first);
    MEM32(o) = vt;
    return o;
}

typedef struct { uint32_t r, scene, vp, mgr, shader, ctx, iface, screen, object, input; const char *back; } gui_t;
static gui_t g_gui;
void (*gui_pressed)(const char *widget);

static const char *name_of(uint32_t widget) {
    uint32_t n = widget ? MEM32(jpog_call(G, "?GetName@TGUIWidget@Toshi@@QBEABVTPCString@2@XZ", widget, 0)) : 0;
    uint32_t s = n ? jpog_call(K, "?GetString@TCString@Toshi@@QBEPBDH@Z", n + 4, 1, 0u) : 0;
    return s ? (const char *)hle_ptr(s) : "";
}
/* A listener of the engine's: f(context, owner, event) is called, stdcall, and
 * says whether the next one is too. Those of a higher `after` come later. */
void engine_listen(uint32_t emitter, const char *shim, uint32_t context, int after) {
    uint32_t l = hle_va(hle_alloc(32));
    jpog_call(K, "??0TGenericListener@Toshi@@IAE@XZ", l, 0);
    jpog_call(K, "?Connect@TGenericListener@Toshi@@IAEXPAVTGenericEmitter@2@PAXP6G_N111@ZH@Z", l, 4, emitter, context, hle_resolve(shim), (uint32_t)after);
}
static void gui_OnInput(void) {              /* (screen, input interface, event) */
    uint32_t screen = A32(0), ev = A32(2);
    uint32_t used = jpog_call(G, "?NotifyInputEvent@TGUIScreen@Toshi@@QAE_NPAVInputEvent@TInputInterface@2@@Z", screen, 1, ev) & 0xFF;
    if (gui_pressed && used && MEM32(ev) == 0x30001 && MEM32(ev + 4) == 0)
        gui_pressed(name_of(jpog_call(G, "?GetWidgetUnderPointer@TGUIScreen@Toshi@@QAEPAVTGUIWidget@2@XZ", screen, 0)));
    if (jpog_verbose && MEM32(ev + 4) < 2)
        fprintf(stderr, "[gui] %05X %s over \"%s\"%s\n", MEM32(ev), MEM32(ev + 4) ? "up" : "down",
                name_of(jpog_call(G, "?GetWidgetUnderPointer@TGUIScreen@Toshi@@QAEPAVTGUIWidget@2@XZ", screen, 0)), used ? ", taken" : "");
    RET(1, 3);
}
/* A rectangle of a material, in the GUI's units, through the sprite shader. */
static void sprite(uint32_t shader, uint32_t material, float x, float y, float w, float h, float z) {
    uint32_t mark = hle_scratch_mark();
    float p0[2] = { x, y }, p1[2] = { x + w, y + h }, t0[2] = { 0, 0 }, t1[2] = { 1, 1 };
    uint32_t white = 0xFFFFFFFFu;
    jpog_call(SP, "?SetMaterial@TSpriteShaderHAL@Toshi@@UAEXPAVTSpriteMaterial@2@@Z", shader, 1, material);
    jpog_call(SP, "?SetColour@TSpriteShaderHAL@Toshi@@UAEXABVTGUIColour@2@@Z", shader, 1, hle_scratch_copy(&white, 4));
    jpog_call(SP, "?RenderRectangle@TSpriteShaderHAL@Toshi@@UAEXABVTVector2@2@000M@Z", shader, 5, hle_scratch_copy(p0, 8), hle_scratch_copy(t0, 8),
              hle_scratch_copy(p1, 8), hle_scratch_copy(t1, 8), F(z));
    hle_scratch_release(mark);
}
static void gui_OnPaint(void) {              /* (-, screen, event): 0 before the widgets, 1 after */
    if (MEM32(A32(2)) == 0 && g_gui.back) {
        static const struct { int x, y, w; } at[6] = { { 0, 0, 256 }, { 256, 0, 256 }, { 512, 0, 128 }, { 0, 256, 256 }, { 256, 256, 256 }, { 512, 256, 128 } };
        const float tall = GUI_H / 448.0f;   /* six, 640 by 448 of them the picture (a television's) */
        for (int i = 0; i < 6; i++) {
            char name[80];
            snprintf(name, sizeof name, "%s_%d", g_gui.back, i + 1);
            uint32_t m = jpog_call(R, "?GetMaterial@TMaterialLibraryManager@Toshi@@QAEPAVTMaterial@2@PBD@Z", g_gui.mgr, 1, hle_scratch_copy(name, (uint32_t)strlen(name) + 1));
            if (m) sprite(g_gui.shader, m, (float)at[i].x, (float)at[i].y * tall, (float)at[i].w, 256 * tall, -14);
        }
    }
    RET(1, 3);
}
static const win32hle_shim gui_listeners[] = { E(gui_OnInput), E(gui_OnPaint), { NULL, NULL } };

/* The mouse and keyboard, by the engine's DirectInput. */
static void gui_input(gui_t *g) {
    jpog_call(K, "?InitialiseStatic@TClass@Toshi@@QAEXXZ", jpog_export(DX, "?m_sClass@TInputDXInterface@Toshi@@2VTClass@2@A"), 0);
    g->input = jpog_call(DX, "?CreateObject@TInputDXInterface@Toshi@@CGPAVTObject@2@XZ", 0, 0);
    uint32_t win = jpog_call(D, "?GetMSWindow@TRenderD3DInterface@Toshi@@QAEPAVTMSWindow@2@XZ", g->r, 0);
    uint32_t hwnd = win ? jpog_call(D, "?GetHWND@TMSWindow@Toshi@@QBEPAUHWND__@@XZ", win, 0) : 0;
    jpog_call(DX, "?SetMainWindow@TInputDXInterface@Toshi@@QAEXPAUHWND__@@@Z", g->input, 1, hwnd);
    uint32_t ok = jpog_call(DX, "?Initialise@TInputDXInterface@Toshi@@UAE_NXZ", g->input, 0) & 0xFF;
    jpog_call(DX, "?SetMainWindow@TInputDXInterface@Toshi@@QAEXPAUHWND__@@@Z", g->input, 1, hwnd);
    uint32_t got = jpog_call(IN, "?AcquireAll@TInputInterface@Toshi@@UAE_NXZ", g->input, 0) & 0xFF;
    fprintf(stderr, "[gui] input %08X, window %X: %s%s\n", g->input, hwnd, ok ? "ready" : "no", got ? ", acquired" : "");
    engine_listen(g->input + 0x1C, "gui_OnInput", g->screen, 0);
}

static int gui_load(gui_t *g, uint32_t kernel, const char *file, const char *locale) {
    uint32_t r = g->r;
    g->mgr = jpog_call(R, "?GetMaterialLibraryManager@TRenderInterface@Toshi@@QAEPAVTMaterialLibraryManager@2@XZ", r, 0);
    g->scene = engine_system(r, SYS_SCENE);
    g->shader = jpog_call(R, "?CreateResource@TRenderInterface@Toshi@@QAEPAVTResource@2@PBVTClass@2@PBDPAV32@@Z", r, 3,
                          jpog_export(SP, "?m_sClass@TSpriteShaderHAL@Toshi@@2VTClass@2@A"), S("SHSPRITE"), engine_system(r, SYS_SHADERS));
    if (!g->shader || !(engine_vcall(g->shader, 0xC, 0) & 0xFF)) { fprintf(stderr, "[gui] no sprite shader\n"); return 0; }
    jpog_call(R, "?SetShader@TMaterialLibraryManager@Toshi@@QAEXHPAVTShader@2@@Z", g->mgr, 2, 4u, g->shader);
    static const char *const libs[] = { "gui_Main", "gui_InGame", "gui_FrontEnd", "gui_Dinopedia", "gui_FossilHunt", "gui_LoadSave", "gui_Debug" };
    for (unsigned i = 0; i < sizeof libs / sizeof *libs; i++) {
        char path[64];
        snprintf(path, sizeof path, "Data/matlibs/%s.tml", libs[i]);
        engine_materials(r, path);
    }
    uint32_t loc = jpog_call(K, "?GetLocale@TSystem@Toshi@@SGPAVTLocale@2@XZ", 0, 0);
    if (!(jpog_call(K, "?LoadInfo@TLocale@Toshi@@QAE_NABVTCString@2@@Z", loc, 1, cstring("Data/GUI/Locales/Locales.txt")) & 0xFF))
        fprintf(stderr, "[gui] no locales\n");
    jpog_call(K, "?ChangeLocale@TLocale@Toshi@@QAEXPBD@Z", loc, 1, S(locale));

    g->ctx = jpog_call(P, "?CreateObject@PGUITRDisplayContext@@CGPAVTObject@Toshi@@XZ", 0, 0);
    MEM32(r + 4)++;                          /* the managed pointer's reference, dropped by Create */
    jpog_call(P, "?Create@PGUITRDisplayContext@@QAEXV?$TManagedPtr@VTRenderInterface@Toshi@@@Toshi@@@Z", g->ctx, 1, r);
    uint32_t tf = jpog_call(P, "?GetTextureFactory@PGUITRDisplayContext@@QAEPAVPGUITRTextureFactory@@XZ", g->ctx, 0);
    uint32_t ff = jpog_call(P, "?GetFontFactory@PGUITRDisplayContext@@QAEPAVPGUITRFontFactory@@XZ", g->ctx, 0);
    /* The classes' own start-up (the widgets', the fonts' properties): the game
     * runs every class's as it starts. */
    jpog_call(K, "?InitialiseStatic@TClass@Toshi@@QAEXXZ", jpog_export(G, "?m_sClass@TGUIInterface@Toshi@@2VTClass@2@A"), 0);
    g->iface = jpog_call(G, "?CreateObject@TGUIInterface@Toshi@@CGPAVTObject@2@XZ", 0, 0);
    jpog_call(G, "?SetKernelInterface@TGUIInterface@Toshi@@QAEXPAVTKernelInterface@2@@Z", g->iface, 1, kernel);
    jpog_call(G, "?SetTextureFactory@TGUIInterface@Toshi@@QAEXPAVTGUITextureFactory@2@@Z", g->iface, 1, tf);
    jpog_call(G, "?SetFontFactory@TGUIInterface@Toshi@@QAEXPAVTGUIFontFactory@2@@Z", g->iface, 1, ff);
    jpog_call(G, "?SetSoundFactory@TGUIInterface@Toshi@@QAEXPAVTGUISoundFactory@2@@Z", g->iface, 1,
              host_object("TGUISoundFactory", 64, 0xC, vt_sounds, 6));
    jpog_call(G, "?Create@TGUIInterface@Toshi@@QAEXXZ", g->iface, 0);
    uint64_t t0 = hle_mono_ns();
    uint32_t ok = jpog_call(G, "?LoadDefinitionFile@TGUIInterface@Toshi@@QAE_NABVTCString@2@@Z", g->iface, 1, cstring(file)) & 0xFF;
    fprintf(stderr, "[gui] %s: %s, %.2f s\n", file, ok ? "read" : "did not load", (double)(hle_mono_ns() - t0) / 1e9);
    jpog_call(P, "?ResolveMaterials@PGUITRTextureFactory@@QAEXXZ", tf, 0);
    return ok != 0;
}

static void gui_screen(gui_t *g, int clear) {
    g->screen = jpog_call(G, "??0TGUIScreen@Toshi@@QAE@XZ", jpog_call(G, "?TGUINew@Toshi@@YGPAXI@Z", 0, 1, 0x144u), 0);
    jpog_call(G, "?SetInterface@TGUIScreen@Toshi@@UAEXPAVTGUIInterface@2@@Z", g->screen, 1, g->iface);
    jpog_call(G, "?SetDisplayContext@TGUIScreen@Toshi@@QAEXPAVTGUIDisplayContext@2@@Z", g->screen, 1, g->ctx);
    jpog_call(G, "?SetAudioContext@TGUIScreen@Toshi@@QAEXPAVTGUIAudioContext@2@@Z", g->screen, 1, host_object("TGUIAudioContext", 64, 0xC, vt_audio, 4));
    jpog_call(G, "?SetInputContext@TGUIScreen@Toshi@@QAEXPAVTGUIInputContext@2@@Z", g->screen, 1, host_object("TGUIInputContext", 256, 0x14, vt_input, 6));
    jpog_call(G, "?SetSize@TGUIWidget@Toshi@@QAEXHH@Z", g->screen, 2, (uint32_t)GUI_W, (uint32_t)GUI_H);
    jpog_call(G, "?SetDefaultScreen@TGUIInterface@Toshi@@QAEXPAVTGUIScreen@2@@Z", g->iface, 1, g->screen);
    g->vp = engine_viewport(g->scene, "gui", 0x20, 0x30, 0x28);
    if (!clear) jpog_call(R, "?AllowBackgroundClear@TViewport@Toshi@@QAE_N_N@Z", g->vp, 1, 0u);
    static const char *const set[] = { "X", "Y", "Width", "Height" };
    const float to[] = { 0, 0, ENGINE_W, ENGINE_H };  /* a viewport's own are -1, the display's: the GUI scales by these */
    for (int i = 0; i < 4; i++) {
        char name[64];
        snprintf(name, sizeof name, "?Set%s@TViewport@Toshi@@QAEMM@Z", set[i]);
        jpog_call(R, name, g->vp, 1, F(to[i])), (void)fp_pop();
    }
    g->object = jpog_call(R, "?CreateResource@TRenderInterface@Toshi@@QAEPAVTResource@2@PBVTClass@2@PBDPAV32@@Z", g->r, 3,
                          jpog_export(P, "?m_sClass@PGUITRRenderObject@@2VTClass@Toshi@@A"), S("gui"), g->scene);
    jpog_call(P, "?Create@PGUITRRenderObject@@UAE_NH@Z", g->object, 1, 0u);
    jpog_call(P, "?SetViewport@PGUITRRenderObject@@QAEPAVTViewport@Toshi@@PAV23@@Z", g->object, 1, g->vp);
    jpog_call(P, "?SetScreen@PGUITRRenderObject@@QAEPAVTGUIScreen@Toshi@@PAV23@@Z", g->object, 1, g->screen);
}

#define g g_gui
int gui_open(uint32_t kernel, uint32_t r, const char *file, const char *locale, int clear) {
    g.r = r;
    if (!gui_load(&g, kernel, file, locale)) return 0;
    gui_screen(&g, clear);
    win32hle_register(gui_listeners);
    gui_input(&g);
    engine_listen(g.screen + 0xB4, "gui_OnPaint", 0, 0);
    uint32_t arrow = jpog_call(G, "?FindTextureSection@TGUIInterface@Toshi@@QBEPAVTGUITextureSection@2@ABVTPCString@2@@Z", g.iface, 1, pooled("cursor_default"));
    jpog_call(G, "?SetPointerImage@TGUIScreen@Toshi@@QAEXPAVTGUITextureSection@2@HH@Z", g.screen, 3, arrow, 0u, 0u);
    jpog_call(G, "?SetPointerVisible@TGUIScreen@Toshi@@QAEX_N@Z", g.screen, 1, 1u);
    jpog_call(G, "?SetPointerPosition@TGUIScreen@Toshi@@QAE_NHH@Z", g.screen, 2, (uint32_t)GUI_W / 2, (uint32_t)GUI_H / 2);
    return 1;
}
uint32_t gui_show(const char *dialog, float x, float y) {
    uint32_t w = jpog_call(G, "?FindTopLevelInstance@TGUIInterface@Toshi@@QBEPAVTGUIWidget@2@ABVTPCString@2@I@Z", g.iface, 2, pooled(dialog), 0u);
    if (!w) { fprintf(stderr, "[gui] no %s\n", dialog); return 0; }
    jpog_call(G, "?AddWidget@TGUIWidget@Toshi@@QAEXPAV12@_N@Z", g.screen, 2, w, 0u);
    /* A dialog that is to be as large as it can be is the screen's size; the
     * rest are their own. */
    uint32_t free_w = jpog_call(G, "?GetSizeModeHoriz@TGUIWidget@Toshi@@QBE?AW4SizeMode@12@XZ", w, 0) == 2;
    uint32_t free_h = jpog_call(G, "?GetSizeModeVert@TGUIWidget@Toshi@@QBE?AW4SizeMode@12@XZ", w, 0) == 2;
    if (free_w && free_h) jpog_call(G, "?SetFixedSize@TGUIWidget@Toshi@@QAEXHH@Z", w, 2, (uint32_t)GUI_W, (uint32_t)GUI_H);
    engine_vcall(w, 0x88, 2, (uint32_t)GUI_W, (uint32_t)GUI_H);      /* Layout: how large its parts are; Pack: where */
    int ww = (int16_t)MEM16(w + 0x70), wh = (int16_t)MEM16(w + 0x72);
    if (free_w && !free_h) jpog_call(G, "?SetFixedSize@TGUIWidget@Toshi@@QAEXHH@Z", w, 2, (uint32_t)(ww = GUI_W), (uint32_t)wh);
    if (free_h && !free_w) jpog_call(G, "?SetFixedSize@TGUIWidget@Toshi@@QAEXHH@Z", w, 2, (uint32_t)ww, (uint32_t)(wh = GUI_H));
    engine_vcall(w, 0x84, 0);
    jpog_call(G, "?SetPosition@TGUIWidget@Toshi@@QAEXHH@Z", w, 2, (uint32_t)(int)((float)(GUI_W - ww) * x), (uint32_t)(int)((float)(GUI_H - wh) * y));
    fprintf(stderr, "[gui] %s: %d by %d\n", dialog, ww, wh);
    return w;
}
void gui_visible(uint32_t widget, int on) { if (widget) jpog_call(G, "?SetVisible@TGUIWidget@Toshi@@QAEX_N@Z", widget, 1, (uint32_t)(on != 0)); }
void gui_frame(float seconds, float dt) {
    jpog_call(IN, "?ProcessEvents@TInputInterface@Toshi@@UAEHM@Z", g.input, 1, F(dt));
    jpog_call(G, "?SetCurrentTime@TGUIDisplayContext@Toshi@@QAEXM@Z", g.ctx, 1, F(seconds));
}

int jpog_gui(const char *dialog, const char *file, const char *locale, const char *shot, int frames) {
    uint32_t kernel, r;
    engine_window_shot();
    mach_enter();
    if (!engine_start(&kernel, &r) || !gui_open(kernel, r, file, locale, 1) || !gui_show(dialog, 0.5f, 0.5f)) return 1;
    if ((g.back = getenv("JPOG_GUI_BACK"))) {
        char path[96];
        snprintf(path, sizeof path, "Data/matlibs/%s.tml", g.back);
        engine_materials(r, path);
    }
    int mx = GUI_W / 2, my = GUI_H / 2, dx = 0, dy = 0, press = 0, scripted = getenv("JPOG_GUI_MOUSE") != NULL;
    int moved = getenv("JPOG_GUI_MOVE") && sscanf(getenv("JPOG_GUI_MOVE"), "%d,%d", &dx, &dy) == 2;
    if (scripted) press = sscanf(getenv("JPOG_GUI_MOUSE"), "%d,%d,%d", &mx, &my, &press) == 3;
    jpog_call(G, "?SetPointerPosition@TGUIScreen@Toshi@@QAE_NHH@Z", g.screen, 2, (uint32_t)mx, (uint32_t)my);
    uint64_t start = hle_mono_ns();
    int i = 0;
    for (; (!frames || i < frames) && !engine_pump(); i++) {
        uint32_t mark = hle_scratch_mark();
        while (jpog_headless && hle_mono_ns() - start < (uint64_t)i * 16666667u) usleep(500);
        jpog_call(K, "?Update@TKernelInterface@Toshi@@QAE_NXZ", kernel, 0);
        if (moved && i == 3) hle_input_motion(dx, dy);       /* as the window would: DirectInput hears of it */
        if (press && (i == 10 || i == 40)) hle_input_mouse(i == 10 ? 0x201 : 0x202, 512, 384);
        gui_frame((float)((double)(hle_mono_ns() - start) / 1e9), 1 / 60.0f);
        if ((scripted || moved) && i == 5) { /* put there: what is under it is asked again */
            if (!moved) jpog_call(G, "?InvalidateWidgetUnderPointer@TGUIScreen@Toshi@@QAEXXZ", g.screen, 0);
            fprintf(stderr, "[gui] pointer %d,%d over \"%s\"\n", (int)MEM32(g.screen + 0x114), (int)MEM32(g.screen + 0x118), name_of(jpog_call(G, "?GetWidgetUnderPointer@TGUIScreen@Toshi@@QAEPAVTGUIWidget@2@XZ", g.screen, 0)));
        }
        jpog_call(D, "?BeginScene@TRenderD3DInterface@Toshi@@UAE_NXZ", g.r, 0);
        engine_scene(g.scene, 1 / 60.0f);
        jpog_call(R, "?End@TScene@Toshi@@QAE_NXZ", g.scene, 0);
        jpog_call(R, "?FlushShaders@TRenderInterface@Toshi@@QAEXXZ", g.r, 0);
        if (shot && frames && i == frames - 1) fprintf(stderr, "[gui] shot %s: %s\n", shot, jpog_d3d8_shot(shot) ? "ok" : "no");
        jpog_call(D, "?EndScene@TRenderD3DInterface@Toshi@@UAE_NXZ", g.r, 0);
        hle_scratch_release(mark);
    }
    double took = (double)(hle_mono_ns() - start) / 1e9;
    fprintf(stderr, "[gui] %d frames in %.1f s, %.0f a second\n", i, took, took > 0 ? i / took : 0);
    if (jpog_verbose) jpog_d3d8_summary();
    mach_leave();
    return 0;
}
