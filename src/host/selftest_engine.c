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

#define D "TRenderD3DInterface.dll"

void jpog_selftest_engine(void) {
    if (!gpu_available() || !jpog_module_named(D)) return;
    char b[64];
    uint32_t kernel = jpog_engine_kernel();
    snprintf(b, sizeof b, "0x%08X", kernel);
    jpog_check(kernel != 0, "engine: TKernelInterface", b);
    uint32_t r = kernel ? jpog_engine_renderer(kernel) : 0;
    jpog_check(r != 0, "engine: TRenderD3DInterface::Create", "no");
    if (!r) return;
    int ok = jpog_engine_display(r, 800, 600);
    jpog_check(ok, "engine: CreateDisplay", "no");
    if (!ok) return;
    uint32_t dev = jpog_engine_device(r);
    dev_t_ *d = OBJ(dev, dev_t_);
    for (int i = 0; i < 3; i++) {
        uint32_t s = jpog_call(D, "?BeginScene@TRenderD3DInterface@Toshi@@UAE_NXZ", r, 0) & 0xFF;
        hle_call_guest(MEM32(MEM32(dev) + 4 * 36), 7, (uint32_t[]){ dev, 0, 0, 3, 0xFF204060u + (uint32_t)i, 0x3F800000, 0 });   /* Clear */
        uint32_t e = jpog_call(D, "?EndScene@TRenderD3DInterface@Toshi@@UAE_NXZ", r, 0) & 0xFF;
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
