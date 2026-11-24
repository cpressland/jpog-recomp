/*
 * selftest_engine.c - Toshi's own renderer, started as the game starts it
 * but by this host: the kernel, TRenderD3DInterface, its window, its display
 * (a Direct3D 8 device) and its shader DLLs, then frames through its
 * BeginScene and EndScene.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32hle.h"
#include "jpog.h"
#include "d3d_int.h"

/* An export by its mangled name, thiscall (self in ecx; 0 for a static). */
static uint32_t call(const char *dll, const char *name, uint32_t self, int n, const uint32_t *a) {
    uint32_t va = jpog_export(dll, name), esp = g_esp;
    recomp_func_t f = va ? recomp_lookup(va) : NULL;
    if (!f) { fprintf(stderr, "[selftest] no %s\n", name); return 0; }
    for (int i = n - 1; i >= 0; i--) PUSH32(g_esp, a[i]);
    PUSH32(g_esp, RECOMP_RETADDR);
    g_ecx = self;
    f();
    g_esp = esp;
    return g_eax;
}
#define K "TKernelInterface.dll"
#define R "TRenderInterface.dll"
#define D "TRenderD3DInterface.dll"

void jpog_selftest_engine(void) {
    if (!gpu_available() || !jpog_module_named(D)) return;
    char b[64];
    /* as TApplication::Create makes it */
    uint32_t kernel = call(K, "?tmalloc@@YGPAXHPADH@Z", 0, 3, (uint32_t[]){ 0x58, 0, 0xFFFFFFFFu });
    uint32_t arg0 = hle_guest_strdup("C:\\SimJP.exe"), argv = hle_va(hle_alloc(8));
    MEM32(argv) = arg0;
    if (kernel) kernel = call(K, "??0TKernelInterface@Toshi@@QAE@HQAPAD_N@Z", kernel, 3, (uint32_t[]){ 1, argv, 0 });
    if (kernel) MEM32(kernel + 4)++;                     /* a reference of our own */
    snprintf(b, sizeof b, "0x%08X", kernel);
    jpog_check(kernel != 0, "engine: TKernelInterface", b);
    call(D, "?InitialiseStatic@TMSWindow@Toshi@@CGXXZ", 0, 0, NULL);
    call(D, "?InitialiseStatic@TRenderD3DInterface@Toshi@@CGXXZ", 0, 0, NULL);
    uint32_t r = call(D, "?CreateObject@TRenderD3DInterface@Toshi@@CGPAVTObject@2@XZ", 0, 0, NULL);
    uint32_t ok = call(D, "?Create@TRenderD3DInterface@Toshi@@UAE_NPAVTKernelInterface@2@@Z", r, 1, &kernel) & 0xFF;
    snprintf(b, sizeof b, "%u", ok);
    jpog_check(r && ok, "engine: TRenderD3DInterface::Create", b);
    if (!ok) return;
    uint32_t depth = call(R, "?MakeDepthStencilFormat@TRenderInterface@Toshi@@QAE?AW4DEPTHSTENCILFORMAT@Device@Mode@TRenderAdapter@2@II@Z",
                          r, 2, (uint32_t[]){ 24, 8 });
    uint32_t params[5] = { 800, 600, 32, depth, 0 }, mark = hle_scratch_mark(), p = hle_scratch_copy(params, sizeof params);
    ok = call(D, "?CreateDisplay@TRenderD3DInterface@Toshi@@UAE_NPBVDisplayParams@TRenderInterface@2@@Z", r, 1, &p) & 0xFF;
    snprintf(b, sizeof b, "%u (depth format %u)", ok, depth);
    jpog_check(ok, "engine: CreateDisplay", b);
    hle_scratch_release(mark);
    if (!ok) return;
    uint32_t dev = call(D, "?GetD3DDevice@TRenderD3DInterface@Toshi@@QAEPAUIDirect3DDevice8@@XZ", r, 0, NULL);
    dev_t_ *d = OBJ(dev, dev_t_);
    for (int i = 0; i < 3; i++) {
        uint32_t s = call(D, "?BeginScene@TRenderD3DInterface@Toshi@@UAE_NXZ", r, 0, NULL) & 0xFF;
        /* Clear(flags, r, g, b, a, rect as bytes..., z, stencil): its own arguments are not guessed at; the device's Clear */
        hle_call_guest(MEM32(MEM32(dev) + 4 * 36), 7, (uint32_t[]){ dev, 0, 0, 3, 0xFF204060u + (uint32_t)i, 0x3F800000, 0 });
        uint32_t e = call(D, "?EndScene@TRenderD3DInterface@Toshi@@UAE_NXZ", r, 0, NULL) & 0xFF;
        snprintf(b, sizeof b, "%u, %u", s, e);
        jpog_check(s && e, "engine: BeginScene and EndScene", b);
    }
    uint32_t w = 0, h = 0;
    static uint8_t px[800 * 600 * 4];
    int got = gpu_pixels(d, d->backbuffer, NULL, &w, &h) && w == 800 && h == 600 && gpu_pixels(d, d->backbuffer, px, &w, &h);
    snprintf(b, sizeof b, "%ux%u, %02X%02X%02X", w, h, px[2], px[1], px[0]);
    jpog_check(got && px[0] == 0x62 && px[1] == 0x40 && px[2] == 0x20, "engine: the frame", b);
    if (getenv("JPOG_TEST_SHOTS")) { char s[1024]; snprintf(s, sizeof s, "%s/engine.bmp", getenv("JPOG_TEST_SHOTS")); jpog_d3d8_shot(s); }
}
