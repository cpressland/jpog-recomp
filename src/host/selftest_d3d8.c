/*
 * selftest_d3d8.c - Direct3D 8 as the game would call it (through the
 * objects' vtables), drawn by the GPU, and the pixels read back and compared
 * with what Direct3D 8 gives. JPOG_TEST_SHOTS=DIR keeps each picture.
 */
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32hle.h"
#include "jpog.h"
#include "d3d_int.h"

enum { SIZE = 64 };
static uint32_t g_dev;

/* A method by its place in the vtable: this, then the arguments. */
static uint32_t com(uint32_t o, int method, int n, ...) {
    uint32_t a[10] = { o };
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) a[i + 1] = va_arg(ap, uint32_t);
    va_end(ap);
    return hle_call_guest(MEM32(MEM32(o) + 4u * (uint32_t)method), n + 1, a);
}
enum { RESET = 14, PRESENT = 15, GETBACKBUFFER = 16, CREATETEXTURE = 20, CREATEVOLUME = 21, CREATECUBE = 22, CREATEVB = 23, CREATEIB = 24,
       CREATEIMAGE = 27, COPYRECTS = 28, FRONTBUFFER = 30, SETRT = 31, GETDS = 33, CLEAR = 36, SETXF = 37, SETVP = 40, SETMAT = 42,
       SETLIGHT = 44, LIGHTENABLE = 46, SETCLIP = 48, SETRS = 50, BEGINSB = 52, ENDSB = 53, APPLYSB = 54, SETTEX = 61, SETTSS = 63,
       DRAWPRIM = 70, DRAWINDEXED = 71, DRAWUP = 72, CREATEVS = 75, SETVS = 76, SETVSC = 79, SETSTREAM = 83, SETINDICES = 85,
       CREATEPS = 87, SETPS = 88, SETPSC = 91 };
#define RS(s, v) com(g_dev, SETRS, 2, (uint32_t)(s), (uint32_t)(v))
#define TSS(st, s, v) com(g_dev, SETTSS, 3, (uint32_t)(st), (uint32_t)(s), (uint32_t)(v))
static uint32_t F(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static uint32_t G(const void *p, size_t n) { return hle_scratch_copy(p, (uint32_t)n); }
static uint32_t out32(void) { return hle_scratch(4); }

static uint8_t g_px[SIZE * SIZE * 4];
static const char *g_name;
static void grab(void) {
    uint32_t w, h;
    dev_t_ *d = OBJ(g_dev, dev_t_);
    if (!gpu_pixels(d, d->backbuffer, g_px, &w, &h)) memset(g_px, 0xEE, sizeof g_px);
    const char *dir = getenv("JPOG_TEST_SHOTS");
    if (dir) { char p[1024]; snprintf(p, sizeof p, "%s/%s.bmp", dir, g_name); jpog_d3d8_shot(p); }
}
static uint32_t px(int x, int y) { const uint8_t *p = g_px + 4 * (y * SIZE + x); return (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }
static void expect(const char *what, int x, int y, uint32_t want, int slack) {
    uint32_t got = px(x, y);
    int ok = 1;
    for (int s = 0; s < 24; s += 8) ok &= abs((int)((got >> s) & 255) - (int)((want >> s) & 255)) <= slack;
    char b[64], n[128];
    snprintf(b, sizeof b, "%06X at %d,%d, wanted %06X", got, x, y, want);
    snprintf(n, sizeof n, "d3d8 %s: %s", g_name, what);
    jpog_check(ok, n, b);
}
static void result(const char *what, uint32_t hr) {
    char b[32], n[128];
    snprintf(b, sizeof b, "0x%08X", hr);
    snprintf(n, sizeof n, "d3d8 %s", what);
    jpog_check(hr == 0, n, b);
}

/* A test starts from a reset device, a cleared screen. */
static void begin(const char *name, uint32_t color) {
    g_name = name;
    uint32_t pp[13] = { SIZE, SIZE, 22, 1, 0, 1, 0, 1, 1, 75 };
    com(g_dev, RESET, 1, G(pp, sizeof pp));
    com(g_dev, CLEAR, 6, 0u, 0u, 7u, color, F(1.0f), 0u);
    RS(137, 0);                                         /* no lighting */
    RS(22, 1);                                          /* no culling */
}
typedef struct { float x, y, z, rhw; uint32_t c; float u, v; } tlv;      /* XYZRHW | DIFFUSE | TEX1 */
#define TLV 0x144u
static void quad_at(float x0, float y0, float x1, float y1, float z, uint32_t c) {
    tlv v[4] = { { x0, y0, z, 1, c, 0, 0 }, { x1, y0, z, 1, c, 1, 0 }, { x0, y1, z, 1, c, 0, 1 }, { x1, y1, z, 1, c, 1, 1 } };
    com(g_dev, SETVS, 1, TLV);
    com(g_dev, DRAWUP, 4, 5u, 2u, G(v, sizeof v), (uint32_t)sizeof v[0]);
}
static uint32_t texture(uint32_t w, uint32_t h, uint32_t fmt, const void *bits, uint32_t pitch, uint32_t rows) {
    uint32_t out = out32(), lk = hle_scratch(8);
    uint32_t hr = com(g_dev, CREATETEXTURE, 7, w, h, 1u, 0u, fmt, 1u, out);
    if (hr) return 0;
    uint32_t t = MEM32(out);
    com(t, 16, 4, 0u, lk, 0u, 0u);
    for (uint32_t y = 0; y < rows; y++) memcpy(hle_ptr(MEM32(lk + 4) + y * MEM32(lk)), (const uint8_t *)bits + y * pitch, pitch);
    com(t, 17, 1, 0u);
    return t;
}
static const uint32_t QUAD4[4] = { 0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFFFF };     /* red green / blue white */
static void flat_stage(uint32_t cop, uint32_t a1, uint32_t a2) { TSS(0, 1, cop); TSS(0, 2, a1); TSS(0, 3, a2); }
static void matrix(uint32_t which, const float *m) { com(g_dev, SETXF, 2, which, G(m, 64)); }
static const float IDENT[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

typedef struct { float x, y, z, nx, ny, nz; uint32_t c; float u, v; } vtx;                 /* XYZ | NORMAL | DIFFUSE | TEX1 */
#define VTX 0x152u
static void quad3d(float z, uint32_t c) {
    vtx v[4] = { { -1, 1, z, 0, 0, -1, c, 0, 0 }, { 1, 1, z, 0, 0, -1, c, 1, 0 }, { -1, -1, z, 0, 0, -1, c, 0, 1 }, { 1, -1, z, 0, 0, -1, c, 1, 1 } };
    com(g_dev, SETVS, 1, VTX);
    com(g_dev, DRAWUP, 4, 5u, 2u, G(v, sizeof v), (uint32_t)sizeof v[0]);
}
static void perspective(float n, float f) {              /* 90 degrees: x and y are -z..z */
    float m[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, f / (f - n), 1, 0, 0, -n * f / (f - n), 0 };
    matrix(3, m);
}

static void tests(void) {
    /* clears, and what of them */
    begin("clear", 0xFF102030);
    { int32_t r[4] = { 8, 8, 16, 16 }; com(g_dev, CLEAR, 6, 1u, G(r, 16), 1u, 0xFFFF8040u, F(1.0f), 0u); }
    grab();
    expect("all", 0, 0, 0x102030, 0);
    expect("inside a rectangle", 8, 8, 0xFF8040, 0), expect("its last", 15, 15, 0xFF8040, 0), expect("past it", 16, 16, 0x102030, 0);

    /* pixel centres: a transformed quad of whole-number edges covers those pixels and no more */
    begin("pixels", 0xFF000000);
    quad_at(10, 20, 14, 23, 0.5f, 0xFFFFFFFF);
    grab();
    expect("first", 10, 20, 0xFFFFFF, 0), expect("last", 13, 22, 0xFFFFFF, 0);
    expect("left of it", 9, 20, 0, 0), expect("above", 10, 19, 0, 0), expect("right", 14, 20, 0, 0), expect("below", 10, 23, 0, 0);

    /* texels land on pixels: half a pixel back, as every Direct3D 8 game does it */
    begin("texels", 0xFF000000);
    {
        uint32_t t = texture(2, 2, 21, QUAD4, 8, 2);
        com(g_dev, SETTEX, 2, 0u, t);
        quad_at(-0.5f, -0.5f, 1.5f, 1.5f, 0.5f, 0xFFFFFFFF);
        TSS(0, 16, 2), TSS(0, 17, 2);                    /* linear: exact only if centres meet centres */
        quad_at(3.5f, 3.5f, 5.5f, 5.5f, 0.5f, 0xFFFFFFFF);
        grab();
        expect("0,0", 0, 0, 0xFF0000, 0), expect("1,0", 1, 0, 0x00FF00, 0), expect("0,1", 0, 1, 0x0000FF, 0), expect("1,1", 1, 1, 0xFFFFFF, 0);
        expect("linear 0,0", 4, 4, 0xFF0000, 6), expect("linear 1,0", 5, 4, 0x00FF00, 6), expect("linear 0,1", 4, 5, 0x0000FF, 6);
        com(t, 2, 0);
    }

    /* formats */
    begin("formats", 0xFF000000);
    {
        static const uint16_t c565[4] = { 0xF800, 0xF800, 0xF800, 0xF800 }, c4444[4] = { 0x80F0, 0x80F0, 0x80F0, 0x80F0 }, c1555[4] = { 0x801F, 0x801F, 0x801F, 0x801F };
        static const uint8_t l8[4] = { 128, 128, 128, 128 }, a8l8[8] = { 64, 255, 64, 255, 64, 255, 64, 255 };
        static const uint16_t dxt1[4] = { 0xF800, 0x001F, 0x0000, 0x0000 };      /* all the first colour: red */
        static const uint8_t dxt5[16] = { 255, 0, 0, 0, 0, 0, 0, 0, 0xE0, 0x07, 0, 0, 0, 0, 0, 0 };   /* green */
        struct { uint32_t fmt; const void *p; uint32_t w, pitch, rows, want; const char *n; } f[] = {
            { 23, c565, 2, 4, 2, 0xFF0000, "R5G6B5" }, { 26, c4444, 2, 4, 2, 0x00FF00, "A4R4G4B4" }, { 25, c1555, 2, 4, 2, 0x0000FF, "A1R5G5B5" },
            { 50, l8, 2, 2, 2, 0x808080, "L8" }, { 51, a8l8, 2, 4, 2, 0x404040, "A8L8" }, { 0x31545844, dxt1, 4, 8, 1, 0xFF0000, "DXT1" },
            { 0x35545844, dxt5, 4, 16, 1, 0x00FF00, "DXT5" } };
        for (size_t i = 0; i < sizeof f / sizeof *f; i++) {
            uint32_t t = texture(f[i].w, f[i].w, f[i].fmt, f[i].p, f[i].pitch, f[i].rows);
            com(g_dev, SETTEX, 2, 0u, t);
            quad_at(8.0f * (float)i, 0, 8.0f * (float)i + 8, 8, 0.5f, 0xFFFFFFFF);
        }
        grab();
        for (size_t i = 0; i < sizeof f / sizeof *f; i++) expect(f[i].n, 8 * (int)i + 4, 4, f[i].want, 1);
    }

    /* the stage cascade */
    begin("stages", 0xFF000000);
    {
        static const uint32_t one[4] = { 0x80804020, 0x80804020, 0x80804020, 0x80804020 };
        uint32_t t = texture(2, 2, 21, one, 8, 2);
        com(g_dev, SETTEX, 2, 0u, t);
        struct { uint32_t op, want; const char *n; } o[] = {
            { 2, 0x804020, "SELECTARG1" }, { 3, 0x204080, "SELECTARG2" }, { 4, 0x101010, "MODULATE" }, { 5, 0x202020, "MODULATE2X" },
            { 6, 0x404040, "MODULATE4X" }, { 7, 0xA080A0, "ADD" }, { 8, 0x200020, "ADDSIGNED" }, { 10, 0x600000, "SUBTRACT" },
            { 11, 0x907090, "ADDSMOOTH" }, { 13, 0x504050, "BLENDTEXTUREALPHA" }, { 12, 0x284078, "BLENDDIFFUSEALPHA" } };
        int n = (int)(sizeof o / sizeof *o);
        for (int i = 0; i < n; i++) { flat_stage(o[i].op, 2, 0); quad_at((float)(i % 8 * 8), (float)(i / 8 * 8), (float)(i % 8 * 8 + 8), (float)(i / 8 * 8 + 8), 0.5f, 0x15204080); }
        /* a constant, complemented; two stages, the second with no texture bound */
        RS(60, 0x00FF8000u);
        flat_stage(2, 3 | 0x10, 0); quad_at(0, 16, 8, 24, 0.5f, 0);
        flat_stage(2, 0, 0); TSS(1, 1, 4); TSS(1, 2, 2); TSS(1, 3, 1); quad_at(8, 16, 16, 24, 0.5f, 0xFF00FF80);
        TSS(1, 1, 7); TSS(1, 2, 3); TSS(1, 3, 1); quad_at(16, 16, 24, 24, 0.5f, 0xFF000040);
        grab();
        for (int i = 0; i < n; i++) expect(o[i].n, i % 8 * 8 + 4, i / 8 * 8 + 4, o[i].want, 2);
        expect("TFACTOR complemented", 4, 20, 0x007FFF, 1), expect("no texture ends it", 12, 20, 0x00FF80, 1), expect("second stage", 20, 20, 0xFF8040, 1);
    }

    /* blending, the alpha test, the depth test, colour writes */
    begin("blend", 0xFF404040);
    flat_stage(2, 0, 0); TSS(0, 4, 2); TSS(0, 5, 0);
    RS(27, 1); RS(19, 5); RS(20, 6); quad_at(0, 0, 8, 8, 0.5f, 0x80FF0000);
    RS(19, 2); RS(20, 2); quad_at(8, 0, 16, 8, 0.5f, 0x00204060);
    RS(171, 3); quad_at(16, 0, 24, 8, 0.5f, 0x00102030); RS(171, 1);
    RS(27, 0); RS(15, 1); RS(24, 128); RS(25, 7);
    quad_at(24, 0, 32, 8, 0.5f, 0x80FFFFFF); quad_at(32, 0, 40, 8, 0.5f, 0x7FFFFFFF); RS(15, 0);
    quad_at(0, 16, 16, 32, 0.4f, 0xFF00FF00); quad_at(8, 16, 24, 32, 0.6f, 0xFFFF0000);
    RS(14, 0); quad_at(32, 16, 40, 32, 0.2f, 0xFF0000FF); RS(14, 1); quad_at(32, 16, 40, 32, 0.6f, 0xFFFFFF00);
    RS(168, 2); quad_at(48, 0, 56, 8, 0.1f, 0xFFFFFFFF); RS(168, 15);
    grab();
    expect("alpha", 4, 4, 0xA02020, 1), expect("add", 12, 4, 0x6080A0, 1), expect("reverse subtract", 20, 4, 0x302010, 1);
    expect("test passes at the reference", 28, 4, 0xFFFFFF, 0), expect("fails below", 36, 4, 0x404040, 0);
    expect("nearer stays", 12, 24, 0x00FF00, 0), expect("farther elsewhere", 20, 24, 0xFF0000, 0), expect("no depth write", 36, 24, 0xFFFF00, 0);
    expect("green only", 52, 4, 0x40FF40, 0);

    /* the depth bias: level surfaces, the second one nearer by a bias */
    begin("zbias", 0xFF000000);
    flat_stage(2, 0, 0); RS(23, 2);                      /* strictly less */
    quad_at(0, 0, 32, 8, 0.5f, 0xFFFF0000); quad_at(0, 0, 16, 8, 0.5f, 0xFF00FF00);
    RS(47, 8); quad_at(16, 0, 32, 8, 0.5f, 0xFF0000FF);
    grab();
    expect("level", 4, 4, 0xFF0000, 0), expect("biased", 24, 4, 0x0000FF, 0);

    /* transforms: the product of the three, row vectors; culling by winding on the screen */
    begin("transform", 0xFF000000);
    {
        flat_stage(2, 0, 0);
        float w[16], v[16];
        memcpy(w, IDENT, 64), memcpy(v, IDENT, 64);
        w[0] = w[5] = 0.25f, v[12] = 1.0f, v[13] = 1.0f, v[14] = 2.0f;        /* a quarter size, moved right, up and away */
        matrix(256, w), matrix(2, v), perspective(0.5f, 10);
        quad3d(0, 0xFFFFFFFF);                           /* x 0.75..1.25, z 2: 0.375..0.625 of half the width */
        RS(22, 2);                                       /* cull clockwise: a strip's first triangle goes this way */
        matrix(256, IDENT); v[12] = -1.0f, v[13] = -1.0f, v[0] = v[5] = 0.25f; matrix(2, v);
        quad3d(0, 0xFFFF0000);
        RS(22, 3);
        v[13] = 1.0f; matrix(2, v);
        quad3d(0, 0xFF00FF00);
        grab();
        expect("middle", 48, 16, 0xFFFFFF, 0), expect("left edge out", 43, 16, 0, 0), expect("left edge in", 45, 16, 0xFFFFFF, 0);
        expect("right edge in", 51, 16, 0xFFFFFF, 0), expect("right edge out", 53, 16, 0, 0);
        expect("clockwise culled", 16, 48, 0, 0), expect("clockwise kept", 16, 16, 0x00FF00, 0);
    }

    /* lighting */
    begin("lighting", 0xFF000000);
    {
        flat_stage(2, 0, 0);
        float m[17] = { 1, 0.5f, 0.25f, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0.125f, 0, 0 };     /* diffuse, ambient, specular, emissive, power */
        com(g_dev, SETMAT, 1, G(m, sizeof m));
        uint32_t l[26] = { 3, F(0.5f), F(0.5f), F(0.5f), F(1), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, F(0), F(0), F(1), F(1000) };
        com(g_dev, SETLIGHT, 2, 0u, G(l, sizeof l)); com(g_dev, LIGHTENABLE, 2, 0u, 1u);
        RS(137, 1); RS(139, 0x00202020); RS(141, 0);
        quad3d(0.5f, 0xFF00FF00);                        /* facing the light */
        grab();
        expect("directional, ambient, emissive", 32, 32, 0xA06060, 2);    /* .5+.125, .25+.125, .125+.125+.125 */
        l[18] = F(-1);
        com(g_dev, SETLIGHT, 2, 0u, G(l, sizeof l));
        com(g_dev, CLEAR, 6, 0u, 0u, 3u, 0u, F(1.0f), 0u);
        quad3d(0.5f, 0);
        grab();
        expect("from behind", 32, 32, 0x202040, 2);
        RS(141, 1);                                      /* the vertex's colour is the diffuse now */
        l[18] = F(1);
        com(g_dev, SETLIGHT, 2, 0u, G(l, sizeof l));
        com(g_dev, CLEAR, 6, 0u, 0u, 3u, 0u, F(1.0f), 0u);
        quad3d(0.5f, 0xFF00FF00);
        grab();
        expect("vertex colour", 32, 32, 0x20A040, 2);
        uint32_t p[26] = { 1, F(1), F(1), F(1), F(1), 0, 0, 0, 0, 0, 0, 0, 0, F(0), F(0), F(-1), 0, 0, 0, F(1000), 0, F(0), F(0), F(1) };
        com(g_dev, SETLIGHT, 2, 0u, G(p, sizeof p));
        RS(141, 0); RS(139, 0);
        com(g_dev, CLEAR, 6, 0u, 0u, 3u, 0u, F(1.0f), 0u);
        quad3d(0.0f, 0);                                 /* each corner sqrt(3) away: cos 1/sqrt 3 over 3 */
        grab();
        expect("point light at a corner", 0, 0, 0x31192C, 2);
        RS(29, 1);
        float s[17] = { 0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 0, 1, 0, 0, 0, 0, 4 };
        com(g_dev, SETMAT, 1, G(s, sizeof s));
        uint32_t q[26] = { 3, 0, 0, 0, 0, F(1), F(1), F(1), F(1), 0, 0, 0, 0, 0, 0, 0, F(0), F(0), F(1), F(1000) };
        com(g_dev, SETLIGHT, 2, 0u, G(q, sizeof q));
        RS(142, 0);
        com(g_dev, CLEAR, 6, 0u, 0u, 3u, 0u, F(1.0f), 0u);
        quad3d(0.5f, 0);
        grab();
        expect("specular, head on", 32, 32, 0xFFFF00, 1);
    }

    /* fog */
    begin("fog", 0xFF000000);
    flat_stage(2, 0, 0); RS(28, 1); RS(34, 0x000000FF); RS(36, F(1.0f)); RS(37, F(3.0f));
    perspective(0.5f, 10);
    {
        float v[16];
        memcpy(v, IDENT, 64), v[14] = 2, v[0] = v[5] = 0.5f, v[12] = -1;
        matrix(2, v); RS(35, 3); quad3d(0, 0xFFFF0000);  /* table, linear, by w: 2 of 1..3 */
        v[12] = 1; matrix(2, v); RS(35, 0); RS(140, 3); quad3d(0, 0xFF00FF00);   /* vertex, linear */
        grab();
        expect("table", 16, 32, 0x800080, 3), expect("vertex", 48, 32, 0x008080, 3);
        RS(140, 0); RS(35, 3);
        tlv t[3] = { { 0, 0, 2.0f / 4, 1, 0xFFFFFFFF, 0, 0 }, { 20, 0, 2.0f / 4, 1, 0xFFFFFFFF, 0, 0 }, { 0, 20, 2.0f / 4, 1, 0xFFFFFFFF, 0, 0 } };
        RS(36, F(0.0f)); RS(37, F(1.0f));
        com(g_dev, SETVS, 1, TLV); com(g_dev, DRAWUP, 4, 4u, 1u, G(t, sizeof t), (uint32_t)sizeof t[0]);
        grab();
        expect("table by z, transformed vertices", 4, 4, 0x8080FF, 3);
    }

    /* texture coordinates: transformed, and generated from the eye position */
    begin("coordinates", 0xFF000000);
    {
        uint32_t t = texture(2, 2, 21, QUAD4, 8, 2);
        com(g_dev, SETTEX, 2, 0u, t);
        float m[16];
        memcpy(m, IDENT, 64), m[8] = 0.5f;               /* row 3 moves two-component coordinates */
        matrix(16, m); TSS(0, 24, 2);
        quad3d(0.5f, 0xFFFFFFFF);
        grab();
        expect("moved half across", 16, 16, 0x00FF00, 0), expect("wrapped", 48, 16, 0xFF0000, 0);
        TSS(0, 11, 0x20000); memcpy(m, IDENT, 64), m[0] = 0.5f, m[5] = -0.5f, m[12] = 0.5f, m[13] = 0.5f; matrix(16, m);
        com(g_dev, CLEAR, 6, 0u, 0u, 3u, 0u, F(1.0f), 0u);
        quad3d(0.5f, 0xFFFFFFFF);
        grab();
        expect("from the position 0,0", 16, 16, 0xFF0000, 0), expect("1,1", 48, 48, 0xFFFFFF, 0);
        float pj[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 1.5f, 0.5f, 2, 0, 0, 0, 0, 1 };          /* (u + 1.5, v + .5, 2): projected, .75 to 1.25 */
        matrix(16, pj); TSS(0, 11, 0); TSS(0, 24, 3 | 0x100);
        com(g_dev, CLEAR, 6, 0u, 0u, 3u, 0u, F(1.0f), 0u);
        quad3d(0.5f, 0xFFFFFFFF);
        grab();
        expect("projected", 8, 8, 0x00FF00, 0), expect("projected, wrapped", 56, 8, 0xFF0000, 0);
    }

    /* cube and volume textures */
    begin("cube", 0xFF000000);
    {
        uint32_t out = out32(), lk = hle_scratch(12);
        result("CreateCubeTexture", com(g_dev, CREATECUBE, 6, 1u, 1u, 0u, 21u, 1u, out));
        uint32_t c = MEM32(out);
        for (uint32_t f = 0; f < 6; f++) { com(c, 16, 5, f, 0u, lk, 0u, 0u); MEM32(MEM32(lk + 4)) = 0xFF000000u | (f + 1) * 0x200000u | (f == 5) * 0xFFu; com(c, 17, 2, f, 0u); }
        com(g_dev, SETTEX, 2, 0u, c);
        TSS(0, 11, 0x10000);                             /* the camera-space normal, -z: face 5 */
        quad3d(0.5f, 0xFFFFFFFF);
        grab();
        expect("negative z face", 32, 32, 0xC000FF, 0);
        result("CreateVolumeTexture", com(g_dev, CREATEVOLUME, 8, 1u, 1u, 2u, 1u, 0u, 21u, 1u, out));
        uint32_t v = MEM32(out);
        com(v, 16, 4, 0u, lk, 0u, 0u);
        MEM32(MEM32(lk + 8)) = 0xFFFF0000, MEM32(MEM32(lk + 8) + MEM32(lk + 4)) = 0xFF00FF00;
        com(v, 17, 1, 0u);
        com(g_dev, SETTEX, 2, 0u, v);
        float m[16];
        memcpy(m, IDENT, 64);
        TSS(0, 11, 0x20000); TSS(0, 24, 3); matrix(16, m);
        com(g_dev, CLEAR, 6, 0u, 0u, 3u, 0u, F(1.0f), 0u);
        quad3d(0.75f, 0xFFFFFFFF);
        grab();
        expect("second slice", 32, 32, 0x00FF00, 0);
    }

    /* bump environment mapping: stage 1's coordinates moved by stage 0's texel */
    begin("bumpenv", 0xFF000000);
    {
        static const int8_t uv[8] = { 127, 0, 127, 0, 127, 0, 127, 0 };
        uint32_t b = texture(2, 2, 60, uv, 4, 2), t = texture(2, 2, 21, QUAD4, 8, 2);
        result("a V8U8 texture", b == 0);
        com(g_dev, SETTEX, 2, 0u, b); com(g_dev, SETTEX, 2, 1u, t);
        TSS(0, 1, 22); TSS(0, 2, 2); TSS(0, 3, 1); TSS(0, 7, F(0.5f)); TSS(0, 8, F(0.5f));      /* du moves u and v by half */
        TSS(1, 1, 2); TSS(1, 2, 2); TSS(1, 11, 0);
        quad_at(0, 0, 64, 64, 0.5f, 0xFFFFFFFF);
        grab();
        expect("moved half both ways", 16, 16, 0xFFFFFF, 0), expect("and wrapped", 48, 48, 0xFF0000, 0);
    }

    /* a render target, then drawn with; CopyRects both ways */
    begin("target", 0xFF000000);
    {
        uint32_t out = out32(), bb, rt, lv, ds = out32();
        result("CreateTexture, render target", com(g_dev, CREATETEXTURE, 7, 32u, 32u, 1u, 1u, 22u, 0u, out));
        rt = MEM32(out);
        com(rt, 15, 2, 0u, out); lv = MEM32(out);
        com(g_dev, GETBACKBUFFER, 3, 0u, 0u, out); bb = MEM32(out);
        com(g_dev, GETDS, 1, ds);
        com(g_dev, SETRT, 2, lv, 0u);
        com(g_dev, CLEAR, 6, 0u, 0u, 1u, 0xFF112233u, F(1.0f), 0u);
        flat_stage(2, 0, 0); RS(7, 0); quad_at(0, 0, 16, 16, 0.5f, 0xFFFF0000); RS(7, 1);
        com(g_dev, SETRT, 2, bb, MEM32(ds));
        flat_stage(2, 2, 0); com(g_dev, SETTEX, 2, 0u, rt);
        quad_at(-0.5f, -0.5f, 31.5f, 31.5f, 0.5f, 0xFFFFFFFF);
        int32_t r[4] = { 0, 0, 16, 16 }, p[2] = { 40, 40 };
        result("CopyRects to the back buffer", com(g_dev, COPYRECTS, 5, lv, G(r, 16), 1u, bb, G(p, 8)));
        uint32_t im, lk = hle_scratch(8);
        com(g_dev, CREATEIMAGE, 4, 64u, 64u, 22u, out); im = MEM32(out);
        result("CopyRects from it", com(g_dev, COPYRECTS, 5, bb, 0u, 0u, im, 0u));
        com(im, 9, 3, lk, 0u, 0x10u);
        uint32_t seen = MEM32(MEM32(lk + 4) + 45 * MEM32(lk) + 45 * 4) & 0xFFFFFF;
        com(im, 10, 0);
        char b[16];
        snprintf(b, sizeof b, "%06X", seen);
        jpog_check(seen == 0xFF0000, "d3d8 target: read back", b);
        grab();
        expect("the target's quad", 8, 8, 0xFF0000, 0), expect("its clear", 24, 24, 0x112233, 0), expect("copied", 45, 45, 0xFF0000, 0), expect("not past it", 57, 57, 0, 0);
    }

    /* a user clip plane, fixed function: what is right of x 0 goes */
    begin("clip", 0xFF000000);
    flat_stage(2, 0, 0);
    { float pl[4] = { -1, 0, 0, 0 }; com(g_dev, SETCLIP, 2, 0u, G(pl, 16)); RS(152, 1); }
    quad3d(0.5f, 0xFFFFFFFF);
    grab();
    expect("kept", 16, 32, 0xFFFFFF, 0), expect("clipped", 48, 32, 0, 0);

    /* vertex and index buffers, with a base vertex; a fan; the viewport's depth range */
    begin("buffers", 0xFF000000);
    {
        flat_stage(2, 0, 0);
        uint32_t out = out32(), vb, ib;
        tlv v[8];
        memset(v, 0, sizeof v);
        for (int i = 0; i < 4; i++) v[4 + i] = (tlv){ i & 1 ? 32.0f : 16.0f, i & 2 ? 32.0f : 16.0f, 0.5f, 1, 0xFF00FFFF, 0, 0 };
        com(g_dev, CREATEVB, 5, (uint32_t)sizeof v, 0x208u, TLV, 0u, out); vb = MEM32(out);
        com(vb, 11, 4, 0u, 0u, out, 0x2000u); memcpy(hle_ptr(MEM32(out)), v, sizeof v); com(vb, 12, 0);
        uint16_t ix[6] = { 0, 1, 2, 1, 3, 2 };
        com(g_dev, CREATEIB, 5, (uint32_t)sizeof ix, 8u, 101u, 1u, out); ib = MEM32(out);
        com(ib, 11, 4, 0u, 0u, out, 0u); memcpy(hle_ptr(MEM32(out)), ix, sizeof ix); com(ib, 12, 0);
        com(g_dev, SETSTREAM, 3, 0u, vb, (uint32_t)sizeof v[0]); com(g_dev, SETINDICES, 2, ib, 4u); com(g_dev, SETVS, 1, TLV);
        com(g_dev, DRAWINDEXED, 5, 4u, 0u, 4u, 0u, 2u);
        tlv f[4] = { { 40, 0, 0.5f, 1, 0xFFFF00FF, 0, 0 }, { 56, 0, 0.5f, 1, 0xFFFF00FF, 0, 0 }, { 56, 16, 0.5f, 1, 0xFFFF00FF, 0, 0 }, { 40, 16, 0.5f, 1, 0xFFFF00FF, 0, 0 } };
        com(g_dev, DRAWUP, 4, 6u, 2u, G(f, sizeof f), (uint32_t)sizeof f[0]);
        grab();
        expect("indexed from the base vertex", 24, 24, 0x00FFFF, 0), expect("a fan", 42, 14, 0xFF00FF, 0), expect("nothing else", 4, 4, 0, 0);
    }

    /* a state block records, and is not set until applied */
    begin("stateblock", 0xFF000000);
    {
        uint32_t tok = out32();
        flat_stage(2, 0, 0);
        com(g_dev, BEGINSB, 0); flat_stage(2, 3, 0); RS(60, 0x0000FF00); com(g_dev, ENDSB, 1, tok);
        quad_at(0, 0, 8, 8, 0.5f, 0xFFFF0000);
        result("ApplyStateBlock", com(g_dev, APPLYSB, 1, MEM32(tok)));
        quad_at(8, 0, 16, 8, 0.5f, 0xFFFF0000);
        grab();
        expect("before", 4, 4, 0xFF0000, 0), expect("after", 12, 4, 0x00FF00, 0);
    }

    /* vs_1_1 and ps_1_1, the declaration naming registers */
    begin("shaders", 0xFF000000);
    {
        static const uint32_t decl[] = { 0x20000000, 0x40020000, 0x40040005, 0x40010007,   /* stream 0: v0 float3, v5 colour, v7 float2 */
                                         0x40000000u * 2 | 1u << 25 | 10, 0x3F800000, 0, 0x3F800000, 0x3F800000,   /* c10 = 1 0 1 1 */
                                         0xFFFFFFFF };
        static const uint32_t vs[] = { 0xFFFE0101, 0x00000014, 0xC00F0000, 0x90E40000, 0xA0E40000,    /* m4x4 oPos, v0, c0 */
                                       0x00000005, 0xD00F0000, 0x90E40005, 0xA0E4000A,                /* mul oD0, v5, c10 */
                                       0x00000001, 0xE0030000, 0x90E40007, 0x0000FFFF };              /* mov oT0.xy, v7 */
        static const uint32_t ps[] = { 0xFFFF0101, 0x00000051, 0xA00F0001, 0, 0, 0x3F000000, 0,          /* def c1, 0, 0, .5, 0 */
                                       0x00000042, 0xB00F0000,                                        /* tex t0 */
                                       0x00000004, 0x800F0000, 0xB0E40000, 0x90E40000, 0xA0E40001,    /* mad r0, t0, v0, c1 */
                                       0x0000FFFF };
        uint32_t hv = out32(), hp = out32();
        result("CreateVertexShader", com(g_dev, CREATEVS, 4, G(decl, sizeof decl), G(vs, sizeof vs), hv, 0u));
        result("CreatePixelShader", com(g_dev, CREATEPS, 2, G(ps, sizeof ps), hp));
        com(g_dev, SETVS, 1, MEM32(hv)); com(g_dev, SETPS, 1, MEM32(hp));
        com(g_dev, SETVSC, 3, 0u, G(IDENT, 64), 4u);
        uint32_t t = texture(2, 2, 21, QUAD4, 8, 2);
        com(g_dev, SETTEX, 2, 0u, t);
        struct { float x, y, z; uint32_t c; float u, v; } v[4] = { { -1, 1, 0.5f, 0xFFFFFFFF, 0, 0 }, { 1, 1, 0.5f, 0xFFFFFFFF, 1, 0 }, { -1, -1, 0.5f, 0xFFFFFFFF, 0, 1 }, { 1, -1, 0.5f, 0xFFFFFFFF, 1, 1 } };
        com(g_dev, DRAWUP, 4, 5u, 2u, G(v, sizeof v), (uint32_t)sizeof v[0]);
        grab();
        expect("red", 16, 16, 0xFF0080, 1), expect("green, its green masked by c10", 48, 16, 0x000080, 1), expect("white", 48, 48, 0xFF00FF, 1);
        /* the pixel shader with fixed-function vertices, and the other way round */
        com(g_dev, CLEAR, 6, 0u, 0u, 3u, 0u, F(1.0f), 0u);
        quad3d(0.5f, 0xFF808080);
        grab();
        expect("ps on fixed-function vertices", 16, 16, 0x800080, 1);
        com(g_dev, SETPS, 1, 0u); com(g_dev, SETVS, 1, MEM32(hv));
        com(g_dev, CLEAR, 6, 0u, 0u, 3u, 0u, F(1.0f), 0u);
        com(g_dev, DRAWUP, 4, 5u, 2u, G(v, sizeof v), (uint32_t)sizeof v[0]);
        grab();
        expect("vs on the fixed-function stages", 48, 48, 0xFF00FF, 1);
    }
}

/* The game's own shaders: every vs_1_x and ps_1_x in the renderer's DLLs,
 * translated and compiled. */
static void game_shaders(void) {
    static const uint8_t params[90] = { [1] = 2, [2] = 3, [3] = 3, [4] = 4, [5] = 3, [6] = 2, [7] = 2, [8] = 3, [9] = 3, [10] = 3, [11] = 3, [12] = 3,
        [13] = 3, [14] = 2, [15] = 2, [16] = 2, [17] = 3, [18] = 4, [19] = 2, [20] = 3, [21] = 3, [22] = 3, [23] = 3, [24] = 3, [64] = 1, [65] = 1, [66] = 1,
        [67] = 2, [68] = 2, [69] = 2, [70] = 2, [71] = 2, [72] = 2, [73] = 2, [74] = 2, [76] = 3, [77] = 2, [78] = 2, [79] = 2, [80] = 4, [81] = 5 };
    int n = 0, bad = 0;
    for (int m = 0; m < g_jpog_nmod; m++) {
        uint32_t lo = g_jpog_mod[m].img.base, hi = lo + g_jpog_mod[m].img.span;
        if (!g_jpog_mod[m].on_demand) continue;
        for (uint32_t at = lo; at + 8 < hi; at += 4) {
            uint32_t v = MEM32(at), p = at + 4;
            if ((v >> 16 != 0xFFFE && v >> 16 != 0xFFFF) || (v & 0xFFFF) < 0x100 || (v & 0xFFFF) > 0x104) continue;
            int ops = 0;
            while (p + 24 < hi && ops < 400) {
                uint32_t t = MEM32(p), op = t & 0xFFFF;
                if (op == 0xFFFE && !(t >> 31)) { p += 4 + 4 * ((t >> 16) & 0x7FFF); continue; }   /* the assembler's comments */
                if (t == 0xFFFF || t & 0xB0000000u || op >= 90 || !params[op]) break;
                p += 4 + 4u * params[op], ops++;
            }
            if (MEM32(p) != 0xFFFF || !ops) continue;
            n++;
            dxso_options opt = { { 0 }, 0, { 0 } };
            dxso_shader s;
            if (dxso_translate((const uint32_t *)hle_ptr(at), (p + 4 - at) / 4, &opt, &s) != 0) {
                fprintf(stderr, "[selftest] %s 0x%08X: %s\n", g_jpog_mod[m].file, at, s.error);
                bad++;
                continue;
            }
            char what[96];
            snprintf(what, sizeof what, "%s's shader at 0x%08X", g_jpog_mod[m].file, at);
            bad += !gpu_compile(s.text, what);
            dxso_free(&s);
        }
    }
    char b[48];
    snprintf(b, sizeof b, "%d of %d do not", bad, n);
    jpog_check(n >= 20 && !bad, "d3d8: the game's shaders compile", b);
}

void jpog_selftest_d3d8(void) {
    if (!gpu_available()) { printf("selftest: no GPU; Direct3D 8 not tested\n"); return; }
    uint32_t mark = hle_scratch_mark(), out = out32();
    uint32_t d3d = hle_call_guest(hle_resolve("Direct3DCreate8"), 1, (uint32_t[]){ 220 });
    uint32_t pp[13] = { SIZE, SIZE, 22, 1, 0, 1, 0, 1, 1, 75 };
    uint32_t hr = com(d3d, 15, 6, 0u, 1u, 0u, 0x40u, G(pp, sizeof pp), out);
    result("CreateDevice", hr);
    if (hr) return;
    g_dev = MEM32(out);
    tests();
    game_shaders();
    /* what is let go is freed, once nothing is set to it */
    begin("free", 0xFF000000);
    uint32_t t = texture(2, 2, 21, QUAD4, 8, 2);
    com(g_dev, SETTEX, 2, 0u, t); com(t, 2, 0);
    com(g_dev, PRESENT, 4, 0u, 0u, 0u, 0u);
    jpog_check(REC(t) != 0, "d3d8 free: a set texture stays", "freed");
    com(g_dev, SETTEX, 2, 0u, 0u);
    com(g_dev, PRESENT, 4, 0u, 0u, 0u, 0u);
    jpog_check(REC(t) == 0, "d3d8 free: then goes", "kept");
    hle_scratch_release(mark);
}
