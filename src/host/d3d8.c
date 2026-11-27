/*
 * d3d8.c - Direct3D 8: the interfaces, as the SDK lays them out, and the
 * state behind them. d3d_metal.m draws; ff.c is the fixed function.
 *
 * Written on stl-recomp's d3d9.c. What Direct3D 8 does otherwise: vertex
 * shaders are handles, which carry their declaration (and constants), or are
 * FVF codes; pixel shaders and state blocks are handles too; samplers are
 * texture stage states; a stream has no offset and the index buffer a base
 * vertex; surfaces copy with CopyRects.
 *
 * Objects are reference counted as Direct3D 8 does (a texture's surfaces
 * count on the texture); one whose last reference goes is freed at the next
 * Present at which nothing is set to it.
 *
 * Every call is counted; at exit a summary lists them (--verbose prints each
 * call as it is made). JPOG_SHOT=DIR keeps every JPOG_SHOT_EVERY'th frame
 * (default 300) as a BMP; JPOG_TRACE_FRAME=N lists frame N's draws.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "win32hle.h"
#include "jpog.h"
#include "d3d_int.h"

#define D3D_OK                 0u
#define D3DERR_NOTAVAILABLE    0x8876086Au
#define D3DERR_INVALIDCALL     0x8876086Cu
#define D3DERR_NOTFOUND        0x88760866u

/* ---- the call count ------------------------------------------------------- */
#define MAX_METHODS 256
static struct { const char *name; uint64_t n; } g_calls[MAX_METHODS];
static int g_ncalls;
static uint64_t g_frames;
static struct { const char *name; int i; } g_count_cache[256];

static void count(const char *name) {
    unsigned h = (unsigned)(((uintptr_t)name >> 3) & 255);
    int i;
    if (g_count_cache[h].name == name) i = g_count_cache[h].i;
    else {
        for (i = 0; i < g_ncalls; i++) if (g_calls[i].name == name) break;
        if (i == g_ncalls) {
            if (g_ncalls == MAX_METHODS) return;
            g_calls[g_ncalls++].name = name;
        }
        g_count_cache[h].name = name, g_count_cache[h].i = i;
    }
    g_calls[i].n++;
    if (jpog_verbose) fprintf(stderr, "[d3d8] %s\n", name);
}
/* A method: counted by its C name. */
#define M(name) static void name(void)
#define COUNT() count(__func__)

void jpog_d3d8_summary(void) {
    if (!g_ncalls) return;
    fprintf(stderr, "\n[d3d8] %d methods called, %llu frames presented:\n", g_ncalls, (unsigned long long)g_frames);
    for (int i = 0; i < g_ncalls; i++) fprintf(stderr, "  %10llu  %s\n", (unsigned long long)g_calls[i].n, g_calls[i].name);
}

/* ---- objects -------------------------------------------------------------- */
static uint32_t g_vt_d3d, g_vt_dev, g_vt_tex, g_vt_cube, g_vt_vol, g_vt_surf, g_vt_volume, g_vt_vb, g_vt_ib;
static uint32_t g_device;                /* the one device */

static uint32_t make(uint32_t vt, uint32_t rec_size, void **rec) {
    void *r = hle_alloc(rec_size);
    uint32_t o = hle_com_new(vt, 12);
    REC(o) = hle_va(r);
    if (rec) *rec = r;
    return o;
}
#define DEV(o) OBJ(o, dev_t_)
#define RES(o) OBJ(o, res_t)

/* The resources there are, so the dead ones can be freed. */
static uint32_t *g_res;
static size_t g_nres, g_cap_res;
static int g_any_dead;
static void keep(uint32_t o) {
    if (g_nres == g_cap_res) g_res = realloc(g_res, (g_cap_res = g_cap_res ? 2 * g_cap_res : 1024) * sizeof *g_res);
    g_res[g_nres++] = o;
}

/* A texture's surface counts on the texture. */
static uint32_t counted(uint32_t o) { return REC(o) && (MEM32(o) == g_vt_surf || MEM32(o) == g_vt_volume) && RES(o)->parent ? RES(o)->parent : o; }
static void addref(uint32_t o) { if (o) MEM32(counted(o) + 4)++; }
static void out_obj(uint32_t pp, uint32_t o) { if (pp) MEM32(pp) = o; }

M(u_QueryInterface) { COUNT(); addref(A32(0)); out_obj(A32(2), A32(0)); RET(D3D_OK, 3); }
M(u_AddRef) { uint32_t o = counted(A32(0)); RET(++MEM32(o + 4), 1); }
M(u_Release) {
    uint32_t o = counted(A32(0)), n = MEM32(o + 4) ? --MEM32(o + 4) : 0;
    uint32_t vt = MEM32(o);
    if (!n && REC(o) && vt != g_vt_d3d && vt != g_vt_dev) RES(o)->dead = 1, g_any_dead = 1;
    RET(n, 1);
}
M(u_GetDevice) {                          /* every record's first word */
    COUNT();
    uint32_t dev = MEM32(REC(A32(0)));
    addref(dev);
    out_obj(A32(1), dev);
    RET(D3D_OK, 2);
}
M(u_SetPrivateData)  { COUNT(); RET(D3D_OK, 5); }
M(u_GetPrivateData)  { COUNT(); RET(D3DERR_NOTFOUND, 4); }
M(u_FreePrivateData) { COUNT(); RET(D3D_OK, 2); }
M(u_SetPriority)     { COUNT(); RET(0, 2); }
M(u_GetPriority)     { COUNT(); RET(0, 1); }
M(u_PreLoad)         { COUNT(); RET(0, 1); }
M(u_GetType)         { COUNT(); RET(RES(A32(0))->kind, 1); }

/* ---- formats -------------------------------------------------------------- */
#define FOURCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define FMT_DXT1 FOURCC('D', 'X', 'T', '1')
#define FMT_DXT2 FOURCC('D', 'X', 'T', '2')
#define FMT_DXT3 FOURCC('D', 'X', 'T', '3')
#define FMT_DXT4 FOURCC('D', 'X', 'T', '4')
#define FMT_DXT5 FOURCC('D', 'X', 'T', '5')

int block_bytes(uint32_t f) {
    return f == FMT_DXT1 ? 8 : (f == FMT_DXT2 || f == FMT_DXT3 || f == FMT_DXT4 || f == FMT_DXT5) ? 16 : 0;
}
int bits_per_pixel(uint32_t f) {
    switch (f) {
    case 20: return 24;                                         /* R8G8B8 */
    case 23: case 24: case 25: case 26: case 51: case 60: case 80: return 16;
    case 28: case 50: return 8;                                 /* A8, L8 */
    default: return 32;
    }
}
uint32_t level_dim(uint32_t d, uint32_t level) { d >>= level; return d ? d : 1; }
uint32_t pitch_of(uint32_t f, uint32_t w) {
    int b = block_bytes(f);
    return b ? ((w + 3) / 4) * (uint32_t)b : (w * (uint32_t)bits_per_pixel(f) + 7) / 8;
}
uint32_t rows_of(uint32_t f, uint32_t h) { return block_bytes(f) ? (h + 3) / 4 : h; }
static uint32_t full_levels(uint32_t w, uint32_t h, uint32_t d) {
    uint32_t m = w > h ? w : h, n = 1;
    if (d > m) m = d;
    while (m > 1) m >>= 1, n++;
    return n;
}
static int is_depth(uint32_t f) { return f == 71 || f == 75 || f == 77 || f == 79 || f == 80; }
/* Textures: the colour formats, alpha, luminance, V8U8 and Q8W8V8U8 bump
 * maps, DXTn. Not palettes, nor the mixed bump formats (no Metal kin). */
static int texture_format(uint32_t f) {
    switch (f) {
    case 20: case 21: case 22: case 23: case 24: case 25: case 26: case 28: case 50: case 51: case 60: case 63:
    case FMT_DXT1: case FMT_DXT2: case FMT_DXT3: case FMT_DXT4: case FMT_DXT5:
        return 1;
    }
    return 0;
}
/* What can be drawn to: what Metal draws to as it is, and R5G6B5, kept as
 * A8R8G8B8 (the water's ripples are made in one). The screen is 32 bits. */
static int target_format(uint32_t f) { return f == 21 || f == 22 || f == 23; }
static int display_format(uint32_t f) { return f == 21 || f == 22; }

uint32_t level_mem(res_t *r, uint32_t level, uint32_t face, uint32_t *pitch, uint32_t *slice) {
    uint32_t w = level_dim(r->width, level), h = level_dim(r->height, level),
             d = r->kind == R_VOLUMETEXTURE ? level_dim(r->depth, level) : 1;
    uint32_t p = pitch_of(r->format, w), s = p * rows_of(r->format, h);
    uint32_t *slot = &r->mem[(face * 16 + level) % (16 * 6)];
    if (!*slot) *slot = hle_va(hle_alloc(s * d ? s * d : 16));
    if (pitch) *pitch = p;
    if (slice) *slice = s;
    return *slot;
}

/* ---- IDirect3D8 ------------------------------------------------------------- */
static void put_f(uint32_t va, float f) { memcpy(hle_ptr(va), &f, 4); }

/* D3DCAPS8 (212 bytes): a GeForce 4 Ti, vs_1_1 and ps_1_3. */
static void caps(uint32_t c) {
    memset(hle_ptr(c), 0, 212);
    MEM32(c + 0) = 1;                       /* D3DDEVTYPE_HAL */
    MEM32(c + 8) = 0x00020000u;             /* Caps: READ_SCANLINE */
    MEM32(c + 12) = 0x20020000u;            /* Caps2: DYNAMICTEXTURES, FULLSCREENGAMMA */
    MEM32(c + 20) = 0x80000003u;            /* PresentationIntervals: IMMEDIATE, ONE, TWO */
    MEM32(c + 24) = 1;                      /* CursorCaps: COLOR */
    MEM32(c + 28) = 0x0009BEF0u;            /* DevCaps: HW T&L and rasterisation, textures anywhere, DrawPrimitives2Ex */
    MEM32(c + 32) = 0x00000FF2u;            /* PrimitiveMiscCaps: MASKZ, the culls, COLORWRITEENABLE, clipping, TSSARGTEMP, BLENDOP */
    MEM32(c + 36) = 0x00736191u;            /* RasterCaps: z test, vertex and table fog, w fog, z bias, range fog, anisotropy */
    MEM32(c + 40) = 0xFF; MEM32(c + 52) = 0xFF;                 /* Z and alpha compare: all */
    MEM32(c + 44) = 0x1FFF; MEM32(c + 48) = 0x1FFF;             /* blend factors: all */
    MEM32(c + 56) = 0x00084208u;            /* ShadeCaps: Gouraud colour, specular, alpha, fog */
    MEM32(c + 60) = 0x0001EC05u;            /* TextureCaps: perspective, alpha, projected, cube, volume, mipmaps of each */
    MEM32(c + 64) = MEM32(c + 68) = MEM32(c + 72) = 0x07030700u;   /* filters: point, linear, anisotropic; mips point, linear */
    MEM32(c + 76) = MEM32(c + 80) = 0x3F;   /* address modes: all */
    MEM32(c + 84) = 0x1F;                   /* LineCaps */
    MEM32(c + 88) = MEM32(c + 92) = 4096;   /* MaxTextureWidth, Height */
    MEM32(c + 96) = 512;                    /* MaxVolumeExtent */
    MEM32(c + 100) = 8192; MEM32(c + 104) = 4096; MEM32(c + 108) = 16;
    put_f(c + 112, 1e10f);                  /* MaxVertexW */
    put_f(c + 116, -32768.0f); put_f(c + 120, -32768.0f); put_f(c + 124, 32768.0f); put_f(c + 128, 32768.0f);
    MEM32(c + 136) = 0xFF;                  /* StencilCaps */
    MEM32(c + 140) = 0x00080008u;           /* FVFCaps: 8 texture coordinates, DONOTSTRIPELEMENTS */
    MEM32(c + 144) = 0x03FEFFFFu;           /* TextureOpCaps: all but PREMODULATE */
    MEM32(c + 148) = 8; MEM32(c + 152) = 4; /* MaxTextureBlendStages, MaxSimultaneousTextures */
    MEM32(c + 156) = 0x3B;                  /* VertexProcessingCaps: texgen, material sources, directional and positional lights, local viewer */
    MEM32(c + 160) = 8; MEM32(c + 164) = 6; MEM32(c + 168) = 4;   /* lights, clip planes, blend matrices */
    put_f(c + 176, 1.0f);                   /* MaxPointSize */
    MEM32(c + 180) = 0x000FFFFFu; MEM32(c + 184) = 0x000FFFFFu;   /* MaxPrimitiveCount, MaxVertexIndex */
    MEM32(c + 188) = 16; MEM32(c + 192) = 255;                    /* MaxStreams, MaxStreamStride */
    MEM32(c + 196) = 0xFFFE0101u;           /* vs_1_1 */
    MEM32(c + 200) = 96;                    /* MaxVertexShaderConst */
    MEM32(c + 204) = 0xFFFF0103u;           /* ps_1_3 */
    put_f(c + 208, 8.0f);                   /* MaxPixelShaderValue */
}

static int g_modes[HLE_DD_MAX_MODES][2], g_nmodes;
static void desktop(int *w, int *h) {
    if (hle_screen_desktop(w, h) && *w >= 640 && *h >= 480) return;
    *w = 1024, *h = 768;                                 /* headless */
}
static void modes(void) {
    int w, h;
    desktop(&w, &h);
    if (!g_nmodes) g_nmodes = hle_dd_modes(w, h, g_modes);
}

M(d_RegisterSoftwareDevice) { COUNT(); RET(D3D_OK, 2); }
M(d_GetAdapterCount) { COUNT(); RET(1, 1); }
M(d_GetAdapterIdentifier) {                  /* (adapter, flags, D3DADAPTER_IDENTIFIER8*) */
    COUNT();
    uint32_t p = A32(3);
    memset(hle_ptr(p), 0, 1068);
    strcpy((char *)hle_ptr(p), "nv4_disp.dll");
    strcpy((char *)hle_ptr(p + 512), "NVIDIA GeForce4 Ti 4600");
    MEM32(p + 1024) = 0x000A0FB4u; MEM32(p + 1028) = 0x0006000Du;   /* 6.13.10.4020: a 2002 driver */
    MEM32(p + 1032) = 0x10DE; MEM32(p + 1036) = 0x0250;             /* vendor, device */
    MEM32(p + 1044) = 0xA3;
    MEM32(p + 1048) = 0xD7B71E3Eu;                                  /* some identifier, always the same */
    MEM32(p + 1064) = 1;                                             /* WHQL */
    RET(D3D_OK, 4);
}
M(d_GetAdapterModeCount) { COUNT(); modes(); RET((uint32_t)g_nmodes, 2); }
static void put_mode(uint32_t p, uint32_t w, uint32_t h, uint32_t fmt) {
    MEM32(p) = w, MEM32(p + 4) = h, MEM32(p + 8) = 60, MEM32(p + 12) = fmt;
}
M(d_EnumAdapterModes) {                      /* (adapter, mode, D3DDISPLAYMODE*) */
    COUNT();
    modes();
    uint32_t i = A32(2);
    if (i >= (uint32_t)g_nmodes) RET(D3DERR_INVALIDCALL, 4);
    put_mode(A32(3), (uint32_t)g_modes[i][0], (uint32_t)g_modes[i][1], 22);
    RET(D3D_OK, 4);
}
M(d_GetAdapterDisplayMode) {                 /* (adapter, D3DDISPLAYMODE*) */
    COUNT();
    int w, h;
    desktop(&w, &h);
    put_mode(A32(2), (uint32_t)w, (uint32_t)h, 22);
    RET(D3D_OK, 3);
}
M(d_CheckDeviceType) {                       /* (adapter, type, displayFmt, backFmt, windowed) */
    COUNT();
    RET(A32(2) == 1 && A32(3) == 22 && display_format(A32(4)) ? D3D_OK : D3DERR_NOTAVAILABLE, 6);
}
M(d_CheckDeviceFormat) {                     /* (adapter, type, adapterFmt, usage, rtype, checkFmt) */
    COUNT();
    uint32_t usage = A32(4), f = A32(6);
    int ok = usage & 2 ? is_depth(f) : usage & 1 ? target_format(f) : texture_format(f);
    if (!ok && jpog_verbose) fprintf(stderr, "[d3d8] CheckDeviceFormat: no format %u (0x%08X), usage 0x%X\n", f, f, usage);
    RET(ok && A32(2) == 1 ? D3D_OK : D3DERR_NOTAVAILABLE, 7);
}
M(d_CheckDeviceMultiSampleType) { COUNT(); RET(A32(5) == 0 ? D3D_OK : D3DERR_NOTAVAILABLE, 6); }
M(d_CheckDepthStencilMatch) { COUNT(); RET(target_format(A32(4)) && is_depth(A32(5)) ? D3D_OK : D3DERR_NOTAVAILABLE, 6); }
M(d_GetDeviceCaps) { COUNT(); caps(A32(3)); RET(D3D_OK, 4); }
M(d_GetAdapterMonitor) { COUNT(); RET(0x00010001u, 2); }

static uint32_t new_surface(uint32_t device, uint32_t w, uint32_t h, uint32_t fmt, uint32_t usage, uint32_t pool) {
    res_t *r;
    uint32_t o = make(g_vt_surf, sizeof(res_t), (void **)&r);
    r->kind = R_SURFACE, r->device = device, r->width = w, r->height = h, r->depth = 1, r->levels = 1;
    r->format = fmt, r->usage = usage, r->pool = pool;
    keep(o);
    return o;
}

static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static void identity(float *m) { memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1; }

/* The state a device starts with, and Reset puts back. */
static void defaults(dev_t_ *d) {
    memset(d->rs, 0, sizeof d->rs);
    uint32_t *rs = d->rs;
    rs[7] = d->params[8] ? 1 : 0;                       /* ZENABLE: with an automatic depth buffer */
    rs[8] = 3, rs[9] = 2, rs[14] = 1, rs[16] = 1;       /* FILLMODE solid, SHADEMODE Gouraud, ZWRITEENABLE, LASTPIXEL */
    rs[19] = 2, rs[20] = 1, rs[22] = 3, rs[23] = 4;     /* SRCBLEND one, DESTBLEND zero, CULLMODE ccw, ZFUNC lessequal */
    rs[25] = 8;                                         /* ALPHAFUNC always */
    rs[30] = 1;                                         /* ZVISIBLE */
    rs[37] = rs[38] = f2u(1.0f);                        /* FOGEND, FOGDENSITY */
    rs[53] = rs[54] = rs[55] = 1, rs[56] = 8;           /* stencil keep, keep, keep, always */
    rs[58] = rs[59] = 0xFFFFFFFFu;                      /* STENCILMASK, STENCILWRITEMASK */
    rs[60] = 0xFFFFFFFFu;                               /* TEXTUREFACTOR */
    rs[136] = rs[137] = 1;                              /* CLIPPING, LIGHTING */
    rs[141] = rs[142] = 1;                              /* COLORVERTEX, LOCALVIEWER */
    rs[145] = 1, rs[146] = 2;                           /* DIFFUSE from color1, SPECULAR from color2; the rest the material's */
    rs[154] = rs[155] = f2u(1.0f), rs[166] = f2u(64.0f);   /* POINTSIZE, _MIN, _MAX */
    rs[161] = 1, rs[162] = 0xFFFFFFFFu;                 /* MULTISAMPLEANTIALIAS, MULTISAMPLEMASK */
    rs[164] = f2u(1.0f);                                /* PATCHSEGMENTS */
    rs[168] = 0xF;                                      /* COLORWRITEENABLE */
    rs[171] = 1;                                        /* BLENDOP add */
    memset(d->tss, 0, sizeof d->tss);
    for (int i = 0; i < MAX_STAGES; i++) {
        uint32_t *t = d->tss[i];
        t[1] = i == 0 ? 4 : 1;                          /* COLOROP: modulate on stage 0, disable after */
        t[2] = 2, t[3] = 1;                             /* COLORARG1 texture, COLORARG2 current */
        t[4] = i == 0 ? 2 : 1;                          /* ALPHAOP: selectarg1 on stage 0 */
        t[5] = 2, t[6] = 1;                             /* ALPHAARG1 texture, ALPHAARG2 current */
        t[11] = (uint32_t)i;                            /* TEXCOORDINDEX */
        t[13] = t[14] = t[25] = 1;                      /* ADDRESSU, V, W wrap */
        t[16] = t[17] = 1;                              /* MAG and MINFILTER point; MIPFILTER none */
        t[21] = 1;                                      /* MAXANISOTROPY */
        t[26] = t[27] = t[28] = 1;                      /* COLORARG0, ALPHAARG0, RESULTARG: current */
    }
    d->vsh = d->psh = d->indices = d->base_vertex = 0;
    memset(d->stream, 0, sizeof d->stream), memset(d->tex, 0, sizeof d->tex);
    memset(d->light, 0, sizeof d->light), memset(d->light_on, 0, sizeof d->light_on);
    memset(d->material, 0, sizeof d->material), memset(d->clip, 0, sizeof d->clip);
    memset(d->vsc, 0, sizeof d->vsc), memset(d->psc, 0, sizeof d->psc);
    for (int i = 0; i < 512; i++) identity(d->transform[i]);
    d->rt = d->backbuffer, d->ds = d->depth;
    d->viewport[0] = d->viewport[1] = 0, d->viewport[2] = d->params[0], d->viewport[3] = d->params[1];
    d->viewport[4] = 0, d->viewport[5] = f2u(1.0f);
}

static void display_mode(dev_t_ *d) { if (!d->params[7]) hle_set_screen_size((int)d->params[0], (int)d->params[1]); }

static int take_params(dev_t_ *d, uint32_t pp) {
    memcpy(d->params, hle_ptr(pp), sizeof d->params);
    if (!d->params[0] || !d->params[1]) {               /* a zero size is the window's */
        int w = 0, h = 0;
        uint32_t wnd = d->params[6] ? d->params[6] : d->hwnd, rc = hle_scratch(16);
        if (wnd) {
            hle_call_guest(hle_resolve("GetClientRect"), 2, (uint32_t[]){ wnd, rc });
            w = (int)MEM32(rc + 8), h = (int)MEM32(rc + 12);
        }
        if (w <= 0 || h <= 0) desktop(&w, &h);
        d->params[0] = (uint32_t)w, d->params[1] = (uint32_t)h;
    }
    if (!d->params[2]) d->params[2] = 22;
    if (!d->params[3]) d->params[3] = 1;
    memcpy(hle_ptr(pp), d->params, 16);
    return display_format(d->params[2]) && (!d->params[8] || is_depth(d->params[9]));
}

M(d_CreateDevice) {                          /* (adapter, type, hwnd, flags, D3DPRESENT_PARAMETERS*, &device) */
    COUNT();
    dev_t_ *d;
    if (A32(2) != 1) RET(D3DERR_NOTAVAILABLE, 7);       /* hardware only */
    uint32_t o = make(g_vt_dev, sizeof(dev_t_), (void **)&d), mark = hle_scratch_mark();
    d->d3d = A32(0), d->adapter = A32(1), d->type = A32(2), d->hwnd = A32(3), d->flags = A32(4);
    int ok = take_params(d, A32(5));
    hle_scratch_release(mark);
    if (!ok) { fprintf(stderr, "[d3d8] CreateDevice: format %u, depth %u not supported\n", d->params[2], d->params[9]); RET(D3DERR_NOTAVAILABLE, 7); }
    addref(d->d3d);
    fprintf(stderr, "[d3d8] CreateDevice: %ux%u, format %u, depth %u, %s, behaviour 0x%X\n", d->params[0], d->params[1],
            d->params[2], d->params[8] ? d->params[9] : 0, d->params[7] ? "windowed" : "full screen", d->flags);
    d->backbuffer = new_surface(o, d->params[0], d->params[1], d->params[2], 1, 0);
    if (d->params[8]) d->depth = new_surface(o, d->params[0], d->params[1], d->params[9], 2, 0);
    defaults(d);
    g_device = o;
    gpu_device(d);
    display_mode(d);
    out_obj(A32(6), o);
    RET(D3D_OK, 7);
}

/* ---- state blocks --------------------------------------------------------------
 * A block is a list of pieces of the device's state (an offset into dev_t_
 * and a size) with their values. Between BeginStateBlock and EndStateBlock a
 * setter records its piece instead of setting it; CreateStateBlock lists all
 * of a type's; Capture reads each piece's value from the device, Apply
 * writes it back. A block's handle is its place in the table, from 1. */
static sblock_t **g_sb;
static uint32_t g_nsb;
static sblock_t *g_rec;                  /* being recorded */

static void sb_put(sblock_t *b, uint32_t off, uint32_t size, const void *val) {
    for (uint32_t i = 0; i < b->n; i++)
        if (b->ops[i].off == off && b->ops[i].size == size) { memcpy(b->data + b->ops[i].at, val, size); return; }
    if (b->n == b->cap) { b->cap = b->cap ? 2 * b->cap : 64; b->ops = realloc(b->ops, b->cap * sizeof *b->ops); }
    if (b->len + size > b->data_cap) {
        while (b->len + size > b->data_cap) b->data_cap = b->data_cap ? 2 * b->data_cap : 4096;
        b->data = realloc(b->data, b->data_cap);
    }
    b->ops[b->n++] = (sbop_t){ off, size, b->len };
    memcpy(b->data + b->len, val, size);
    b->len += size;
}
static void set(dev_t_ *d, void *field, const void *val, uint32_t size) {
    if (g_rec) sb_put(g_rec, (uint32_t)((uint8_t *)field - (uint8_t *)d), size, val);
    else memmove(field, val, size);
}
static void set32(dev_t_ *d, uint32_t *field, uint32_t v) { set(d, field, &v, 4); }

static uint32_t sb_add(sblock_t *b) {
    uint32_t i = 0;
    while (i < g_nsb && g_sb[i]) i++;
    if (i == g_nsb) g_sb = realloc(g_sb, ++g_nsb * sizeof *g_sb);
    g_sb[i] = b;
    return i + 1;
}
static sblock_t *sb_at(uint32_t h) { return h && h <= g_nsb ? g_sb[h - 1] : NULL; }

M(v_BeginStateBlock) {
    COUNT();
    if (g_rec) RET(D3DERR_INVALIDCALL, 1);
    g_rec = calloc(1, sizeof *g_rec);
    RET(D3D_OK, 1);
}
M(v_EndStateBlock) {                         /* (&token) */
    COUNT();
    if (!g_rec) RET(D3DERR_INVALIDCALL, 2);
    MEM32(A32(1)) = sb_add(g_rec);
    g_rec = NULL;
    RET(D3D_OK, 2);
}
M(v_ApplyStateBlock) {
    COUNT();
    sblock_t *b = sb_at(A32(1));
    uint8_t *d = (uint8_t *)DEV(A32(0));
    if (!b) RET(D3DERR_INVALIDCALL, 2);
    for (uint32_t i = 0; i < b->n; i++) set(DEV(A32(0)), d + b->ops[i].off, b->data + b->ops[i].at, b->ops[i].size);
    RET(D3D_OK, 2);
}
M(v_CaptureStateBlock) {
    COUNT();
    sblock_t *b = sb_at(A32(1));
    uint8_t *d = (uint8_t *)DEV(A32(0));
    if (!b) RET(D3DERR_INVALIDCALL, 2);
    for (uint32_t i = 0; i < b->n; i++) memcpy(b->data + b->ops[i].at, d + b->ops[i].off, b->ops[i].size);
    RET(D3D_OK, 2);
}
M(v_DeleteStateBlock) {
    COUNT();
    sblock_t *b = sb_at(A32(1));
    if (!b) RET(D3DERR_INVALIDCALL, 2);
    g_sb[A32(1) - 1] = NULL;
    free(b->ops), free(b->data), free(b);
    RET(D3D_OK, 2);
}
#define PIECE(field) sb_put(b, (uint32_t)((uint8_t *)&d->field - (uint8_t *)d), sizeof d->field, &d->field)
/* (type, &token): D3DSBT_ALL 1, PIXELSTATE 2, VERTEXSTATE 3 */
M(v_CreateStateBlock) {
    COUNT();
    dev_t_ *d = DEV(A32(0));
    sblock_t *b = calloc(1, sizeof *b);
    uint32_t type = A32(1);
    PIECE(rs); PIECE(tss);                              /* both kinds': a little more than the SDK's lists */
    if (type != 3) { PIECE(psh); PIECE(psc); }
    if (type != 2) { PIECE(vsh); PIECE(vsc); PIECE(light); PIECE(light_on); }
    if (type == 1) { PIECE(tex); PIECE(stream); PIECE(indices); PIECE(base_vertex); PIECE(viewport); PIECE(transform); PIECE(material); PIECE(clip); }
    MEM32(A32(2)) = sb_add(b);
    RET(D3D_OK, 3);
}

/* ---- freeing the dead ------------------------------------------------------- */
static int is_set(uint32_t o) {              /* is the device, or a state block, still set to it? */
    dev_t_ *d = DEV(g_device);
    uint32_t in[] = { d->rt, d->ds, d->backbuffer, d->depth };
    for (int i = 0; i < 4; i++) if (in[i] && counted(in[i]) == o) return 1;
    for (int i = 0; i < MAX_STAGES; i++) if (d->tex[i] == o) return 1;
    for (int i = 0; i < 16; i++) if (d->stream[i][0] == o) return 1;
    if (d->indices == o) return 1;
    for (uint32_t k = 0; k < g_nsb; k++)
        for (uint32_t i = 0; g_sb[k] && i + 4 <= g_sb[k]->len; i += 4)
            if (*(uint32_t *)(g_sb[k]->data + i) == o) return 1;
    return 0;
}
static void free_dead(void) {
    if (!g_any_dead || g_rec) return;
    g_any_dead = 0;
    for (size_t i = 0; i < g_nres; i++) {
        uint32_t o = g_res[i];
        res_t *r = RES(o);
        if (!r->dead) continue;
        if (MEM32(o + 4)) { r->dead = 0; continue; }    /* taken up again (GetTexture) */
        if (is_set(o)) { g_any_dead = 1; continue; }
        gpu_free(r);
        for (int k = 0; k < 16 * 6; k++) {
            if (r->mem[k]) hle_free(hle_ptr(r->mem[k]));
            if (r->surf[k]) { hle_free(hle_ptr(REC(r->surf[k]))); REC(r->surf[k]) = 0; hle_com_free(r->surf[k]); }
        }
        hle_free(r);
        REC(o) = 0;
        hle_com_free(o);
        g_res[i--] = g_res[--g_nres];
    }
}

/* ---- IDirect3DDevice8 --------------------------------------------------------- */
M(v_TestCooperativeLevel) { COUNT(); RET(D3D_OK, 1); }
M(v_GetAvailableTextureMem) { COUNT(); RET(256u << 20, 1); }
M(v_ResourceManagerDiscardBytes) { COUNT(); RET(D3D_OK, 2); }
M(v_GetDirect3D) { COUNT(); uint32_t o = DEV(A32(0))->d3d; addref(o); out_obj(A32(1), o); RET(D3D_OK, 2); }
M(v_GetDeviceCaps) { COUNT(); caps(A32(1)); RET(D3D_OK, 2); }
M(v_GetDisplayMode) {
    COUNT();
    dev_t_ *d = DEV(A32(0));
    int w = (int)d->params[0], h = (int)d->params[1];
    if (d->params[7]) desktop(&w, &h);
    put_mode(A32(1), (uint32_t)w, (uint32_t)h, 22);
    RET(D3D_OK, 2);
}
M(v_GetCreationParameters) {
    COUNT();
    dev_t_ *d = DEV(A32(0));
    uint32_t p = A32(1);
    MEM32(p) = d->adapter, MEM32(p + 4) = d->type, MEM32(p + 8) = d->hwnd, MEM32(p + 12) = d->flags;
    RET(D3D_OK, 2);
}
M(v_SetCursorProperties) { COUNT(); RET(D3D_OK, 4); }
M(v_SetCursorPosition) { COUNT(); RET(0, 4); }
M(v_ShowCursor) { COUNT(); RET(0, 2); }
M(v_Reset) {
    COUNT();
    dev_t_ *d = DEV(A32(0));
    uint32_t mark = hle_scratch_mark();
    int ok = take_params(d, A32(1));
    hle_scratch_release(mark);
    if (!ok) RET(D3DERR_INVALIDCALL, 2);
    res_t *bb = RES(d->backbuffer);
    bb->width = d->params[0], bb->height = d->params[1], bb->format = d->params[2];
    for (int k = 0; k < 16 * 6; k++) if (bb->mem[k]) { hle_free(hle_ptr(bb->mem[k])); bb->mem[k] = 0; }
    if (d->params[8] && !d->depth) d->depth = new_surface(A32(0), d->params[0], d->params[1], d->params[9], 2, 0);
    if (d->depth && d->params[8]) RES(d->depth)->width = d->params[0], RES(d->depth)->height = d->params[1], RES(d->depth)->format = d->params[9];
    if (!d->params[8]) d->depth = 0;
    defaults(d);
    gpu_device(d);
    display_mode(d);
    fprintf(stderr, "[d3d8] Reset: %ux%u\n", d->params[0], d->params[1]);
    RET(D3D_OK, 2);
}

void jpog_write_bmp(const char *path, const uint8_t *px, uint32_t w, uint32_t h) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t row = w * 3 + (4 - (w * 3) % 4) % 4, size = 54 + row * h;
    uint8_t hdr[54] = { 'B', 'M' };
    memcpy(hdr + 2, &size, 4), hdr[10] = 54, hdr[14] = 40;
    memcpy(hdr + 18, &w, 4), memcpy(hdr + 22, &h, 4), hdr[26] = 1, hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    uint8_t *line = calloc(1, row);
    for (uint32_t y = h; y-- > 0;) {
        for (uint32_t x = 0; x < w; x++) memcpy(line + 3 * x, px + ((size_t)y * w + x) * 4, 3);
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
}
int jpog_d3d8_shot(const char *path) {       /* the back buffer, as a BMP */
    uint32_t w, h;
    if (!g_device) return 0;
    dev_t_ *d = DEV(g_device);
    if (!gpu_pixels(d, d->backbuffer, NULL, &w, &h)) return 0;
    uint8_t *px = malloc((size_t)w * h * 4);
    int ok = gpu_pixels(d, d->backbuffer, px, &w, &h);
    if (ok) jpog_write_bmp(path, px, w, h);
    free(px);
    return ok;
}
void jpog_d3d8_size(uint32_t *w, uint32_t *h) {
    if (!g_device) { *w = 640; *h = 480; return; }
    dev_t_ *d = DEV(g_device);
    *w = d->params[0] ? d->params[0] : 640;
    *h = d->params[1] ? d->params[1] : 480;
}
int jpog_d3d8_is_backbuffer(void) {
    if (!g_device) return 0;
    dev_t_ *d = DEV(g_device);
    return d->rt == d->backbuffer;
}
static void shots(void) {
    static const char *dir;
    static uint64_t every;
    static int checked;
    if (!checked) {
        checked = 1;
        dir = getenv("JPOG_SHOT");
        const char *e = getenv("JPOG_SHOT_EVERY");
        every = e ? strtoull(e, NULL, 0) : 300;
        if (dir) mkdir(dir, 0755);
    }
    if (!dir || !every || g_frames % every) return;
    char path[4096];
    snprintf(path, sizeof path, "%s/frame_%06llu.bmp", dir, (unsigned long long)g_frames);
    jpog_d3d8_shot(path);
}
M(v_Present) {                               /* (src, dst, window, dirty) */
    COUNT();
    dev_t_ *d = DEV(A32(0));
    ++g_frames;
    shots();
    gpu_present(d);
    jpog_game_on_present();
    free_dead();
    if (!jpog_headless) hle_screen_pump();
    RET(D3D_OK, 5);
}
M(v_GetBackBuffer) {                         /* (index, type, &surface) */
    COUNT();
    uint32_t bb = DEV(A32(0))->backbuffer;
    if (A32(1)) { out_obj(A32(3), 0); RET(D3DERR_INVALIDCALL, 4); }
    addref(bb);
    out_obj(A32(3), bb);
    RET(D3D_OK, 4);
}
M(v_GetRasterStatus) { COUNT(); memset(hle_ptr(A32(1)), 0, 8); RET(D3D_OK, 2); }
M(v_SetGammaRamp) { COUNT(); RET(0, 3); }
M(v_GetGammaRamp) {
    COUNT();
    uint16_t *r = (uint16_t *)hle_ptr(A32(1));
    for (int c = 0; c < 3; c++) for (int i = 0; i < 256; i++) r[c * 256 + i] = (uint16_t)(i * 257);
    RET(0, 2);
}

static uint32_t new_texture(uint32_t vt, uint32_t kind, uint32_t dev, uint32_t w, uint32_t h, uint32_t depth,
                            uint32_t levels, uint32_t usage, uint32_t fmt, uint32_t pool) {
    res_t *r;
    uint32_t o = make(vt, sizeof(res_t), (void **)&r);
    r->kind = kind, r->device = dev, r->width = w, r->height = h, r->depth = depth;
    r->levels = levels ? levels : full_levels(w, h, kind == R_VOLUMETEXTURE ? depth : 1);
    if (r->levels > 16) r->levels = 16;
    r->format = fmt, r->usage = usage, r->pool = pool;
    keep(o);
    return o;
}
static int can_make(uint32_t usage, uint32_t f) {
    int ok = usage & 2 ? is_depth(f) : usage & 1 ? target_format(f) : texture_format(f);
    if (!ok) fprintf(stderr, "[d3d8] no texture of format %u, usage 0x%X\n", f, usage);
    return ok;
}
M(v_CreateTexture) {                         /* (w, h, levels, usage, format, pool, &tex) */
    COUNT();
    if (!can_make(A32(4), A32(5))) { out_obj(A32(7), 0); RET(D3DERR_INVALIDCALL, 8); }
    out_obj(A32(7), new_texture(g_vt_tex, R_TEXTURE, A32(0), A32(1), A32(2), 1, A32(3), A32(4), A32(5), A32(6)));
    RET(D3D_OK, 8);
}
M(v_CreateVolumeTexture) {                   /* (w, h, d, levels, usage, format, pool, &tex) */
    COUNT();
    if (!can_make(A32(5), A32(6))) { out_obj(A32(8), 0); RET(D3DERR_INVALIDCALL, 9); }
    out_obj(A32(8), new_texture(g_vt_vol, R_VOLUMETEXTURE, A32(0), A32(1), A32(2), A32(3), A32(4), A32(5), A32(6), A32(7)));
    RET(D3D_OK, 9);
}
M(v_CreateCubeTexture) {                     /* (edge, levels, usage, format, pool, &tex) */
    COUNT();
    if (!can_make(A32(3), A32(4))) { out_obj(A32(6), 0); RET(D3DERR_INVALIDCALL, 7); }
    out_obj(A32(6), new_texture(g_vt_cube, R_CUBETEXTURE, A32(0), A32(1), A32(1), 1, A32(2), A32(3), A32(4), A32(5)));
    RET(D3D_OK, 7);
}
static uint32_t new_buffer(uint32_t vt, uint32_t kind, uint32_t dev, uint32_t len, uint32_t usage, uint32_t fmt_or_fvf,
                           uint32_t pool) {
    res_t *r;
    uint32_t o = make(vt, sizeof(res_t), (void **)&r);
    r->kind = kind, r->device = dev, r->size = len, r->usage = usage, r->pool = pool;
    r->width = len, r->height = r->depth = r->levels = 1;
    if (kind == R_VERTEXBUFFER) r->fvf = fmt_or_fvf, r->format = 100;   /* D3DFMT_VERTEXDATA */
    else r->format = fmt_or_fvf;
    r->mem[0] = hle_va(hle_alloc(len ? len : 16));
    keep(o);
    return o;
}
M(v_CreateVertexBuffer) {                    /* (len, usage, fvf, pool, &vb) */
    COUNT();
    out_obj(A32(5), new_buffer(g_vt_vb, R_VERTEXBUFFER, A32(0), A32(1), A32(2), A32(3), A32(4)));
    RET(D3D_OK, 6);
}
M(v_CreateIndexBuffer) {                     /* (len, usage, format, pool, &ib) */
    COUNT();
    out_obj(A32(5), new_buffer(g_vt_ib, R_INDEXBUFFER, A32(0), A32(1), A32(2), A32(3), A32(4)));
    RET(D3D_OK, 6);
}
M(v_CreateRenderTarget) {                    /* (w, h, fmt, ms, lockable, &surf) */
    COUNT();
    if (!target_format(A32(3))) RET(D3DERR_INVALIDCALL, 7);
    out_obj(A32(6), new_surface(A32(0), A32(1), A32(2), A32(3), 1, 0));
    RET(D3D_OK, 7);
}
M(v_CreateDepthStencilSurface) {             /* (w, h, fmt, ms, &surf) */
    COUNT();
    if (!is_depth(A32(3))) RET(D3DERR_INVALIDCALL, 6);
    out_obj(A32(5), new_surface(A32(0), A32(1), A32(2), A32(3), 2, 0));
    RET(D3D_OK, 6);
}
M(v_CreateImageSurface) {                    /* (w, h, fmt, &surf): system memory */
    COUNT();
    out_obj(A32(4), new_surface(A32(0), A32(1), A32(2), A32(3), 0, 2));
    RET(D3D_OK, 5);
}

/* A surface's owner, and which of its levels and faces it is. */
static res_t *where(uint32_t s, uint32_t *level, uint32_t *face) {
    res_t *r = RES(s);
    *level = r->parent ? r->level : 0, *face = r->parent ? r->face : 0;
    return r->parent ? RES(r->parent) : r;
}
/* CopyRects(src, rects, n, dst, points): on the GPU when the destination is
 * drawn to; else by the CPU, from a target's pixels brought back first. */
M(v_CopyRects) {
    COUNT();
    uint32_t src = A32(1), rects = A32(2), n = A32(3), dst = A32(4), points = A32(5), sl, sf, dl, df;
    res_t *s = where(src, &sl, &sf), *t = where(dst, &dl, &df);
    int32_t whole[4] = { 0, 0, (int32_t)level_dim(s->width, sl), (int32_t)level_dim(s->height, sl) };
    if (!rects || !n) n = 1, rects = 0;
    if (s->format != t->format) RET(D3DERR_INVALIDCALL, 6);
    int brought = 0;
    for (uint32_t i = 0; i < n; i++) {
        int32_t r[4], q[4];
        for (int k = 0; k < 4; k++) r[k] = rects ? (int32_t)MEM32(rects + 16 * i + 4u * (uint32_t)k) : whole[k];
        q[0] = points ? (int32_t)MEM32(points + 8 * i) : rects ? r[0] : 0;
        q[1] = points ? (int32_t)MEM32(points + 8 * i + 4) : rects ? r[1] : 0;
        q[2] = q[0] + r[2] - r[0], q[3] = q[1] + r[3] - r[1];
        if (gpu_is_render_target(t) && gpu_stretch_rect(DEV(A32(0)), src, r, dst, q)) continue;
        if (gpu_is_render_target(s) && !brought++) gpu_readback(DEV(A32(0)), s, sl, sf);
        uint32_t sp, ss, dp, ds, sm = level_mem(s, sl, sf, &sp, &ss), dm = level_mem(t, dl, df, &dp, &ds);
        int block = block_bytes(s->format), b = block ? block : bits_per_pixel(s->format) / 8, unit = block ? 4 : 1;
        int w = (r[2] - r[0] + unit - 1) / unit * b, h = (r[3] - r[1] + unit - 1) / unit;
        if (r[0] < 0 || r[1] < 0 || q[0] < 0 || q[1] < 0 || r[2] > whole[2] || r[3] > whole[3] ||
            q[2] > (int32_t)level_dim(t->width, dl) || q[3] > (int32_t)level_dim(t->height, dl)) RET(D3DERR_INVALIDCALL, 6);
        for (int y = 0; y < h; y++)
            memcpy(hle_ptr(dm + (uint32_t)(q[1] / unit + y) * dp + (uint32_t)(q[0] / unit * b)),
                   hle_ptr(sm + (uint32_t)(r[1] / unit + y) * sp + (uint32_t)(r[0] / unit * b)), (size_t)w);
        t->dirty[df] |= 1u << dl;
    }
    RET(D3D_OK, 6);
}
M(v_UpdateTexture) {                         /* (system-memory texture, texture): every level, by the CPU */
    COUNT();
    res_t *src = RES(A32(1)), *dst = RES(A32(2));
    int faces = src->kind == R_CUBETEXTURE ? 6 : 1;
    uint32_t skip = 0;                                   /* the source may have more (larger) levels */
    while (skip < src->levels && level_dim(src->width, skip) > dst->width) skip++;
    for (int f = 0; f < faces; f++)
        for (uint32_t l = 0; l < dst->levels && l + skip < src->levels; l++) {
            uint32_t sp, ss, dp, ds;
            uint32_t sm = level_mem(src, l + skip, (uint32_t)f, &sp, &ss), dm = level_mem(dst, l, (uint32_t)f, &dp, &ds);
            if (src->kind == R_VOLUMETEXTURE) ss *= level_dim(src->depth, l + skip), ds *= level_dim(dst->depth, l);
            memcpy(hle_ptr(dm), hle_ptr(sm), ss < ds ? ss : ds);
            dst->dirty[f] |= 1u << l;
        }
    RET(D3D_OK, 3);
}
M(v_GetFrontBuffer) {                        /* (A8R8G8B8 system surface): what is on the screen */
    COUNT();
    dev_t_ *d = DEV(A32(0));
    res_t *bb = RES(d->backbuffer), *t = RES(A32(1));
    uint32_t sp, ss, dp, ds;
    gpu_readback(d, bb, 0, 0);
    uint32_t sm = level_mem(bb, 0, 0, &sp, &ss), dm = level_mem(t, 0, 0, &dp, &ds);
    if (t->format != 21) RET(D3DERR_INVALIDCALL, 2);
    for (uint32_t y = 0; y < bb->height && y < t->height; y++) memcpy(hle_ptr(dm + y * dp), hle_ptr(sm + y * sp), sp < dp ? sp : dp);
    RET(D3D_OK, 2);
}
M(v_SetRenderTarget) {                       /* (target or NULL: as it is, depth or NULL: none) */
    COUNT();
    dev_t_ *d = DEV(A32(0));
    if (A32(1)) {
        res_t *r = RES(A32(1));
        d->rt = A32(1);
        uint32_t w = r->width, h = r->height;            /* the viewport becomes all of it */
        d->viewport[0] = d->viewport[1] = 0, d->viewport[2] = w, d->viewport[3] = h;
        d->viewport[4] = 0, d->viewport[5] = f2u(1.0f);
    }
    d->ds = A32(2);
    RET(D3D_OK, 3);
}
M(v_GetRenderTarget) { COUNT(); uint32_t s = DEV(A32(0))->rt; addref(s); out_obj(A32(1), s); RET(s ? D3D_OK : D3DERR_NOTFOUND, 2); }
M(v_GetDepthStencilSurface) { COUNT(); uint32_t s = DEV(A32(0))->ds; addref(s); out_obj(A32(1), s); RET(s ? D3D_OK : D3DERR_NOTFOUND, 2); }
M(v_BeginScene) { COUNT(); RET(D3D_OK, 1); }
M(v_EndScene) { COUNT(); RET(D3D_OK, 1); }
static float arg_f(int i) { uint32_t u = A32(i); float f; memcpy(&f, &u, 4); return f; }
M(v_Clear) {                                 /* (count, rects, flags, color, z, stencil) */
    COUNT();
    gpu_clear(DEV(A32(0)), A32(1), A32(2), A32(3), A32(4), arg_f(5), A32(6));
    RET(D3D_OK, 7);
}
M(v_SetTransform) {
    COUNT();
    dev_t_ *d = DEV(A32(0));
    if (A32(1) < 512) set(d, d->transform[A32(1)], hle_ptr(A32(2)), 64);
    RET(D3D_OK, 3);
}
M(v_GetTransform) { COUNT(); if (A32(1) < 512) memcpy(hle_ptr(A32(2)), DEV(A32(0))->transform[A32(1)], 64); RET(D3D_OK, 3); }
M(v_MultiplyTransform) {                     /* (state, matrix): the matrix, then what was there */
    COUNT();
    dev_t_ *d = DEV(A32(0));
    if (A32(1) < 512) {
        float a[16], t[16], *b = d->transform[A32(1)];
        memcpy(a, hle_ptr(A32(2)), 64);
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
                t[4 * r + c] = a[4 * r] * b[c] + a[4 * r + 1] * b[4 + c] + a[4 * r + 2] * b[8 + c] + a[4 * r + 3] * b[12 + c];
        set(d, b, t, 64);
    }
    RET(D3D_OK, 3);
}
M(v_SetViewport) { COUNT(); dev_t_ *d = DEV(A32(0)); set(d, d->viewport, hle_ptr(A32(1)), 24); RET(D3D_OK, 2); }
M(v_GetViewport) { COUNT(); memcpy(hle_ptr(A32(1)), DEV(A32(0))->viewport, 24); RET(D3D_OK, 2); }
M(v_SetMaterial) { COUNT(); dev_t_ *d = DEV(A32(0)); set(d, d->material, hle_ptr(A32(1)), 68); RET(D3D_OK, 2); }
M(v_GetMaterial) { COUNT(); memcpy(hle_ptr(A32(1)), DEV(A32(0))->material, 68); RET(D3D_OK, 2); }
M(v_SetLight) { COUNT(); dev_t_ *d = DEV(A32(0)); if (A32(1) < 8) set(d, d->light[A32(1)], hle_ptr(A32(2)), 104); RET(D3D_OK, 3); }
M(v_GetLight) { COUNT(); if (A32(1) < 8) memcpy(hle_ptr(A32(2)), DEV(A32(0))->light[A32(1)], 104); RET(D3D_OK, 3); }
M(v_LightEnable) { COUNT(); dev_t_ *d = DEV(A32(0)); if (A32(1) < 8) set32(d, &d->light_on[A32(1)], A32(2) != 0); RET(D3D_OK, 3); }
M(v_GetLightEnable) { COUNT(); MEM32(A32(2)) = A32(1) < 8 ? DEV(A32(0))->light_on[A32(1)] : 0; RET(D3D_OK, 3); }
M(v_SetClipPlane) { COUNT(); dev_t_ *d = DEV(A32(0)); if (A32(1) < 6) set(d, d->clip[A32(1)], hle_ptr(A32(2)), 16); RET(D3D_OK, 3); }
M(v_GetClipPlane) { COUNT(); if (A32(1) < 6) memcpy(hle_ptr(A32(2)), DEV(A32(0))->clip[A32(1)], 16); RET(D3D_OK, 3); }
M(v_SetRenderState) { COUNT(); dev_t_ *d = DEV(A32(0)); if (A32(1) < MAX_RS) set32(d, &d->rs[A32(1)], A32(2)); RET(D3D_OK, 3); }
M(v_GetRenderState) { COUNT(); MEM32(A32(2)) = A32(1) < MAX_RS ? DEV(A32(0))->rs[A32(1)] : 0; RET(D3D_OK, 3); }
M(v_SetClipStatus) { COUNT(); RET(D3D_OK, 2); }
M(v_GetClipStatus) { COUNT(); memset(hle_ptr(A32(1)), 0, 8); RET(D3D_OK, 2); }
M(v_GetTexture) { COUNT(); uint32_t t = A32(1) < MAX_STAGES ? DEV(A32(0))->tex[A32(1)] : 0; addref(t); out_obj(A32(2), t); RET(D3D_OK, 3); }
M(v_SetTexture) { COUNT(); dev_t_ *d = DEV(A32(0)); if (A32(1) < MAX_STAGES) set32(d, &d->tex[A32(1)], A32(2)); RET(D3D_OK, 3); }
M(v_GetTextureStageState) { COUNT(); MEM32(A32(3)) = A32(1) < MAX_STAGES && A32(2) < MAX_TSS ? DEV(A32(0))->tss[A32(1)][A32(2)] : 0; RET(D3D_OK, 4); }
M(v_SetTextureStageState) {
    COUNT();
    dev_t_ *d = DEV(A32(0));
    if (A32(1) < MAX_STAGES && A32(2) < MAX_TSS) set32(d, &d->tss[A32(1)][A32(2)], A32(3));
    RET(D3D_OK, 4);
}
M(v_ValidateDevice) { COUNT(); if (A32(1)) MEM32(A32(1)) = 1; RET(D3D_OK, 2); }
M(v_GetInfo) { COUNT(); RET(1, 4); }                     /* S_FALSE: not here */
M(v_SetPaletteEntries) { COUNT(); RET(D3D_OK, 3); }
M(v_GetPaletteEntries) { COUNT(); RET(D3DERR_INVALIDCALL, 3); }
M(v_SetCurrentTexturePalette) { COUNT(); RET(D3D_OK, 2); }
M(v_GetCurrentTexturePalette) { COUNT(); MEM32(A32(1)) = 0; RET(D3D_OK, 2); }
M(v_DrawPrimitive) {                         /* (type, start vertex, primitives) */
    COUNT();
    gpu_draw(DEV(A32(0)), A32(1), A32(3), A32(2), 0, 0, 0, 0, 0, 0, 0, 0);
    RET(D3D_OK, 4);
}
M(v_DrawIndexedPrimitive) {                  /* (type, min index, vertices, start index, primitives) */
    COUNT();
    dev_t_ *d = DEV(A32(0));
    gpu_draw(d, A32(1), A32(5), A32(4), 1, (int32_t)d->base_vertex, A32(2), A32(3), 0, 0, 0, 0);
    RET(D3D_OK, 6);
}
/* The *UP draws leave stream 0 (and the indices) unset, as Direct3D does. */
M(v_DrawPrimitiveUP) {                       /* (type, primitives, data, stride) */
    COUNT();
    dev_t_ *d = DEV(A32(0));
    gpu_draw(d, A32(1), A32(2), 0, 0, 0, 0, 0, A32(3), A32(4), 0, 0);
    d->stream[0][0] = d->stream[0][1] = 0;
    RET(D3D_OK, 5);
}
M(v_DrawIndexedPrimitiveUP) {                /* (type, min index, vertices, primitives, indices, format, data, stride) */
    COUNT();
    dev_t_ *d = DEV(A32(0));
    gpu_draw(d, A32(1), A32(4), 0, 1, 0, A32(2), A32(3), A32(7), A32(8), A32(5), A32(6));
    d->stream[0][0] = d->stream[0][1] = d->indices = 0;
    RET(D3D_OK, 9);
}
M(v_ProcessVertices) { COUNT(); RET(D3DERR_INVALIDCALL, 6); }

/* ---- shaders: handles, odd (an FVF code is even) ------------------------------ */
static shader_t **g_sh;
static uint32_t g_nsh;
shader_t *shader_at(uint32_t h) { return (h & 1) && h >> 1 < g_nsh && g_sh[h >> 1] ? g_sh[h >> 1] : NULL; }
static uint32_t shader_add(shader_t *s) {
    uint32_t i = 1;
    while (i < g_nsh && g_sh[i]) i++;
    if (i >= g_nsh) {
        g_sh = realloc(g_sh, (i + 1) * sizeof *g_sh);
        while (g_nsh <= i) g_sh[g_nsh++] = NULL;
    }
    g_sh[i] = s;
    return i << 1 | 1;
}
static uint32_t token_len(uint32_t code) {   /* bytes, the end token (found where an instruction would start) included */
    uint32_t n = 1;
    while (n < (1u << 16)) {
        uint32_t t = MEM32(code + 4 * n);
        if (t == 0x0000FFFFu) return 4 * (n + 1);
        if ((t & 0xFFFF) == 0xFFFE) n += 1 + ((t >> 16) & 0x7FFF);
        else if ((t & 0xFFFF) == 81 && !(t & 0x80000000u)) n += 6;      /* def: its values may hold anything */
        else n++;
    }
    return 4 * n;
}

/* A declaration's tokens (D3DVSD_*): streams, the data each register takes
 * from them, and constants. */
static void read_decl(shader_t *s) {
    uint32_t stream = 0, at = 0;
    static const uint8_t bytes[8] = { 4, 8, 12, 16, 4, 4, 4, 8 };       /* FLOAT1-4, D3DCOLOR, UBYTE4, SHORT2, SHORT4 */
    for (uint32_t i = 0; i < s->decl_len / 4; i++) {
        uint32_t t = s->decl[i];
        if (t == 0xFFFFFFFFu) break;
        switch (t >> 29) {
        case 1: if (!(t & 0x10000000u)) stream = t & 0xF, at = 0; break;
        case 2:
            if (t & 0x10000000u) at += 4 * ((t >> 16) & 0xF);           /* SKIP: dwords */
            else {
                uint32_t ty = (t >> 16) & 7;
                if (s->ne < MAX_ELEMS) s->e[s->ne++] = (elem_t){ (uint16_t)stream, (uint16_t)at, (uint8_t)ty, (uint8_t)(t & 0x1F) };
                at += bytes[ty];
            }
            break;
        case 4: {                                                        /* CONST: registers of four floats */
            uint32_t n = (t >> 25) & 0xF, reg = t & 0x7F;
            for (uint32_t k = 0; k < n && i + 4 < s->decl_len / 4; k++, i += 4)
                if (s->nk < 96) { s->k[s->nk].reg = (uint16_t)(reg + k); memcpy(s->k[s->nk++].v, s->decl + i + 1, 16); }
            break;
        }
        case 5: i += (t >> 24) & 0x1F; break;                            /* EXT */
        }
    }
}
static uint32_t decl_len(uint32_t d) {
    uint32_t n = 0;
    while (n < 1024 && MEM32(d + 4 * n) != 0xFFFFFFFFu) {
        uint32_t t = MEM32(d + 4 * n);
        n += 1 + (t >> 29 == 4 ? 4 * ((t >> 25) & 0xF) : t >> 29 == 5 ? (t >> 24) & 0x1F : 0);
    }
    return 4 * (n + 1);
}
static uint32_t *copy_in(uint32_t va, uint32_t len) { uint32_t *p = malloc(len); memcpy(p, hle_ptr(va), len); return p; }
static void shader_free(uint32_t h) {
    shader_t *s = shader_at(h);
    if (!s) return;
    g_sh[h >> 1] = NULL;
    free(s->code), free(s->decl), free(s);   /* its Metal functions stay: pipelines are keyed by them */
}

M(v_CreateVertexShader) {                    /* (declaration, function or NULL, &handle, usage) */
    COUNT();
    shader_t *s = calloc(1, sizeof *s);
    s->decl_len = decl_len(A32(1)), s->decl = copy_in(A32(1), s->decl_len);
    if (A32(2)) s->len = token_len(A32(2)), s->code = copy_in(A32(2), s->len);
    read_decl(s);
    MEM32(A32(3)) = shader_add(s);
    RET(D3D_OK, 5);
}
M(v_SetVertexShader) {
    COUNT();
    dev_t_ *d = DEV(A32(0));
    set32(d, &d->vsh, A32(1));
    shader_t *s = shader_at(A32(1));
    for (int i = 0; s && i < s->nk; i++) if (s->k[i].reg < 96) set(d, d->vsc[s->k[i].reg], s->k[i].v, 16);
    RET(D3D_OK, 2);
}
M(v_GetVertexShader) { COUNT(); MEM32(A32(1)) = DEV(A32(0))->vsh; RET(D3D_OK, 2); }
M(v_DeleteVertexShader) { COUNT(); dev_t_ *d = DEV(A32(0)); if (d->vsh == A32(1)) d->vsh = 0; shader_free(A32(1)); RET(D3D_OK, 2); }
M(v_GetVertexShaderDeclaration) {            /* (handle, data, &size) */
    COUNT();
    shader_t *s = shader_at(A32(1));
    if (!s) RET(D3DERR_INVALIDCALL, 4);
    if (A32(2) && MEM32(A32(3)) >= s->decl_len) memcpy(hle_ptr(A32(2)), s->decl, s->decl_len);
    MEM32(A32(3)) = s->decl_len;
    RET(D3D_OK, 4);
}
M(v_GetShaderFunction) {
    COUNT();
    shader_t *s = shader_at(A32(1));
    if (!s || !s->code) RET(D3DERR_INVALIDCALL, 4);
    if (A32(2) && MEM32(A32(3)) >= s->len) memcpy(hle_ptr(A32(2)), s->code, s->len);
    MEM32(A32(3)) = s->len;
    RET(D3D_OK, 4);
}
M(v_CreatePixelShader) {                     /* (function, &handle) */
    COUNT();
    shader_t *s = calloc(1, sizeof *s);
    s->pixel = 1, s->len = token_len(A32(1)), s->code = copy_in(A32(1), s->len);
    MEM32(A32(2)) = shader_add(s);
    RET(D3D_OK, 3);
}
M(v_SetPixelShader) { COUNT(); dev_t_ *d = DEV(A32(0)); set32(d, &d->psh, A32(1)); RET(D3D_OK, 2); }
M(v_GetPixelShader) { COUNT(); MEM32(A32(1)) = DEV(A32(0))->psh; RET(D3D_OK, 2); }
M(v_DeletePixelShader) { COUNT(); dev_t_ *d = DEV(A32(0)); if (d->psh == A32(1)) d->psh = 0; shader_free(A32(1)); RET(D3D_OK, 2); }
#define CONSTS(Name, arr, max)                                                                  \
    M(v_Set##Name) {                                                                            \
        COUNT();                                                                                \
        uint32_t s = A32(1), n = A32(3);                                                        \
        dev_t_ *d = DEV(A32(0));                                                                \
        if (s < max && n) set(d, d->arr[s], hle_ptr(A32(2)), (n > max - s ? max - s : n) * 16); \
        RET(D3D_OK, 4);                                                                         \
    }                                                                                           \
    M(v_Get##Name) {                                                                            \
        COUNT();                                                                                \
        uint32_t s = A32(1), n = A32(3);                                                        \
        if (s < max && n) memcpy(hle_ptr(A32(2)), DEV(A32(0))->arr[s], (n > max - s ? max - s : n) * 16); \
        RET(D3D_OK, 4);                                                                         \
    }
CONSTS(VertexShaderConstant, vsc, 96)
CONSTS(PixelShaderConstant, psc, 8)

M(v_SetStreamSource) {                       /* (stream, vb, stride) */
    COUNT();
    dev_t_ *d = DEV(A32(0));
    uint32_t v[2] = { A32(2), A32(3) };
    if (A32(1) < 16) set(d, d->stream[A32(1)], v, 8);
    RET(D3D_OK, 4);
}
M(v_GetStreamSource) {
    COUNT();
    uint32_t *x = DEV(A32(0))->stream[A32(1) & 15];
    addref(x[0]);
    out_obj(A32(2), x[0]);
    if (A32(3)) MEM32(A32(3)) = x[1];
    RET(D3D_OK, 4);
}
M(v_SetIndices) { COUNT(); dev_t_ *d = DEV(A32(0)); set32(d, &d->indices, A32(1)); set32(d, &d->base_vertex, A32(2)); RET(D3D_OK, 3); }
M(v_GetIndices) {
    COUNT();
    dev_t_ *d = DEV(A32(0));
    addref(d->indices);
    out_obj(A32(1), d->indices);
    if (A32(2)) MEM32(A32(2)) = d->base_vertex;
    RET(D3D_OK, 3);
}
M(v_DrawPatch) { COUNT(); RET(D3DERR_INVALIDCALL, 4); }
M(v_DeletePatch) { COUNT(); RET(D3D_OK, 2); }

/* The layout now set: an FVF code's, or the vertex shader's declaration. */
int vertex_layout(dev_t_ *d, elem_t *e) {
    shader_t *s = shader_at(d->vsh);
    if (s) { memcpy(e, s->e, (size_t)s->ne * sizeof *e); return s->ne; }
    uint32_t fvf = d->vsh, pos = fvf & 0xE;
    int n = 0;
    uint16_t at = 0;
#define EL(type_, reg_, bytes_) (e[n++] = (elem_t){ 0, at, type_, reg_ }, at = (uint16_t)(at + (bytes_)))
    if (pos == 0x4) EL(3, 0, 16);                                        /* XYZRHW */
    else if (pos) {
        EL(2, 0, 12);                                                    /* XYZ */
        int w = pos == 0x6 ? 1 : pos == 0x8 ? 2 : pos == 0xA ? 3 : pos == 0xC ? 4 : pos == 0xE ? 5 : 0;
        int f = fvf & 0x1000 ? w - 1 : w;                                /* LASTBETA_UBYTE4: the last is indices */
        if (f > 0) { e[n++] = (elem_t){ 0, at, (uint8_t)((f > 4 ? 4 : f) - 1), 1 }; at = (uint16_t)(at + 4 * f); }
        if (f < w) EL(5, 2, 4);
    }
    if (fvf & 0x10) EL(2, 3, 12);                                        /* NORMAL */
    if (fvf & 0x20) EL(0, 4, 4);                                         /* PSIZE */
    if (fvf & 0x40) EL(4, 5, 4);                                         /* DIFFUSE */
    if (fvf & 0x80) EL(4, 6, 4);                                         /* SPECULAR */
    for (uint32_t i = 0; i < ((fvf >> 8) & 0xF) && i < 8; i++) {
        static const uint8_t comps[4] = { 2, 3, 4, 1 };
        uint8_t c = comps[(fvf >> (16 + 2 * i)) & 3];
        EL((uint8_t)(c - 1), (uint8_t)(7 + i), 4 * c);
    }
#undef EL
    return n;
}

/* ---- resources ----------------------------------------------------------- */
M(r_GetLevelCount) { COUNT(); RET(RES(A32(0))->levels, 1); }
M(r_GetLOD) { COUNT(); RET(0, 1); }
M(r_SetLOD) { COUNT(); RET(0, 2); }
/* D3DSURFACE_DESC: format, type, usage, pool, size, multisample, width, height. */
static void put_desc(uint32_t p, res_t *r, uint32_t level) {
    uint32_t w = level_dim(r->width, level), h = level_dim(r->height, level);
    MEM32(p) = r->format, MEM32(p + 4) = R_SURFACE, MEM32(p + 8) = r->usage, MEM32(p + 12) = r->pool;
    MEM32(p + 16) = pitch_of(r->format, w) * rows_of(r->format, h), MEM32(p + 20) = r->multisample;
    MEM32(p + 24) = w, MEM32(p + 28) = h;
}
M(t_GetLevelDesc) { COUNT(); res_t *r = RES(A32(0)); if (A32(1) >= r->levels) RET(D3DERR_INVALIDCALL, 3); put_desc(A32(2), r, A32(1)); RET(D3D_OK, 3); }
M(s_GetDesc) { COUNT(); put_desc(A32(1), RES(A32(0)), 0); RET(D3D_OK, 2); }
static void put_volume_desc(uint32_t p, res_t *r, uint32_t l) {   /* format, type, usage, pool, size, width, height, depth */
    uint32_t w = level_dim(r->width, l), h = level_dim(r->height, l), d = level_dim(r->depth, l);
    MEM32(p) = r->format, MEM32(p + 4) = R_VOLUME, MEM32(p + 8) = r->usage, MEM32(p + 12) = r->pool;
    MEM32(p + 16) = pitch_of(r->format, w) * rows_of(r->format, h) * d;
    MEM32(p + 20) = w, MEM32(p + 24) = h, MEM32(p + 28) = d;
}
M(vt_GetLevelDesc) { COUNT(); res_t *r = RES(A32(0)); if (A32(1) >= r->levels) RET(D3DERR_INVALIDCALL, 3); put_volume_desc(A32(2), r, A32(1)); RET(D3D_OK, 3); }
M(vo_GetDesc) { COUNT(); put_volume_desc(A32(1), RES(A32(0)), 0); RET(D3D_OK, 2); }
static uint32_t level_surface(uint32_t tex, uint32_t level, uint32_t face) {
    res_t *t = RES(tex), *s;
    uint32_t *slot = &t->surf[(face * 16 + level) % 96];
    if (!*slot) {
        int vol = t->kind == R_VOLUMETEXTURE;
        uint32_t o = make(vol ? g_vt_volume : g_vt_surf, sizeof(res_t), (void **)&s);
        s->kind = vol ? R_VOLUME : R_SURFACE, s->device = t->device;
        s->width = level_dim(t->width, level), s->height = level_dim(t->height, level);
        s->depth = vol ? level_dim(t->depth, level) : 1, s->levels = 1;
        s->format = t->format, s->usage = t->usage, s->pool = t->pool;
        s->parent = tex, s->level = level, s->face = face;
        *slot = o;
    }
    addref(*slot);                                       /* the texture's count */
    return *slot;
}
M(t_GetSurfaceLevel) { COUNT(); if (A32(1) >= RES(A32(0))->levels) RET(D3DERR_INVALIDCALL, 3); out_obj(A32(2), level_surface(A32(0), A32(1), 0)); RET(D3D_OK, 3); }
M(c_GetCubeMapSurface) {                     /* (face, level, &surface) */
    COUNT();
    if (A32(2) >= RES(A32(0))->levels || A32(1) > 5) RET(D3DERR_INVALIDCALL, 4);
    out_obj(A32(3), level_surface(A32(0), A32(2), A32(1)));
    RET(D3D_OK, 4);
}
M(vt_GetVolumeLevel) { COUNT(); if (A32(1) >= RES(A32(0))->levels) RET(D3DERR_INVALIDCALL, 3); out_obj(A32(2), level_surface(A32(0), A32(1), 0)); RET(D3D_OK, 3); }

/* A level's bytes, at a rectangle of it, for the guest to read and write. */
static uint32_t lock(res_t *r, uint32_t level, uint32_t face, uint32_t out, uint32_t rect, uint32_t flags) {
    if (level >= r->levels) return D3DERR_INVALIDCALL;
    if (gpu_is_render_target(r)) gpu_readback(DEV(r->device), r, level, face);
    uint32_t pitch, slice, mem = level_mem(r, level, face, &pitch, &slice);
    if (rect) {
        uint32_t x = MEM32(rect), y = MEM32(rect + 4);
        int b = block_bytes(r->format);
        mem += b ? (y / 4) * pitch + (x / 4) * (uint32_t)b : y * pitch + x * (uint32_t)bits_per_pixel(r->format) / 8;
    }
    MEM32(out) = pitch, MEM32(out + 4) = mem;
    if (!(flags & 0x10)) r->dirty[face] |= 1u << level;  /* not READONLY */
    return D3D_OK;
}
M(t_LockRect) { COUNT(); RET(lock(RES(A32(0)), A32(1), 0, A32(2), A32(3), A32(4)), 5); }            /* (level, &locked, rect, flags) */
M(t_UnlockRect) { COUNT(); RET(D3D_OK, 2); }
M(t_AddDirtyRect) { COUNT(); RET(D3D_OK, 2); }
M(c_LockRect) { COUNT(); RET(lock(RES(A32(0)), A32(2), A32(1) % 6, A32(3), A32(4), A32(5)), 6); }   /* (face, level, &locked, rect, flags) */
M(c_UnlockRect) { COUNT(); RET(D3D_OK, 3); }
M(c_AddDirtyRect) { COUNT(); RET(D3D_OK, 3); }
M(s_LockRect) {                              /* (&locked, rect, flags) */
    COUNT();
    uint32_t l, f;
    res_t *r = where(A32(0), &l, &f);
    RET(lock(r, l, f, A32(1), A32(2), A32(3)), 4);
}
M(s_UnlockRect) { COUNT(); RET(D3D_OK, 1); }
M(s_GetContainer) { COUNT(); uint32_t p = RES(A32(0))->parent; p = p ? p : RES(A32(0))->device; addref(p); out_obj(A32(2), p); RET(D3D_OK, 3); }
static uint32_t lock_box(res_t *r, uint32_t l, uint32_t out, uint32_t box, uint32_t flags) {   /* row pitch, slice pitch, bits */
    uint32_t pitch, slice;
    if (l >= r->levels) return D3DERR_INVALIDCALL;
    uint32_t mem = level_mem(r, l, 0, &pitch, &slice);
    if (box) {                                           /* left, top, right, bottom, front, back */
        int b = block_bytes(r->format);
        uint32_t x = MEM32(box), y = MEM32(box + 4), z = MEM32(box + 16);
        mem += z * slice + (b ? (y / 4) * pitch + (x / 4) * (uint32_t)b : y * pitch + x * (uint32_t)bits_per_pixel(r->format) / 8);
    }
    MEM32(out) = pitch, MEM32(out + 4) = slice, MEM32(out + 8) = mem;
    if (!(flags & 0x10)) r->dirty[0] |= 1u << l;
    return D3D_OK;
}
M(vt_LockBox) { COUNT(); RET(lock_box(RES(A32(0)), A32(1), A32(2), A32(3), A32(4)), 5); }   /* (level, &locked, box, flags) */
M(vo_LockBox) {                              /* (&locked, box, flags) */
    COUNT();
    res_t *v = RES(A32(0));
    RET(lock_box(RES(v->parent), v->level, A32(1), A32(2), A32(3)), 4);
}
M(vo_UnlockBox) { COUNT(); RET(D3D_OK, 1); }
M(vt_UnlockBox) { COUNT(); RET(D3D_OK, 2); }
M(vt_AddDirtyBox) { COUNT(); RET(D3D_OK, 2); }
M(b_Lock) {                                  /* (offset, size, &data, flags) */
    COUNT();
    res_t *r = RES(A32(0));
    MEM32(A32(3)) = r->mem[0] + A32(1);
    if (!(A32(4) & 0x10)) r->dirty[0] = 1;
    RET(D3D_OK, 5);
}
M(b_Unlock) { COUNT(); RET(D3D_OK, 1); }
M(b_GetDesc) {                               /* format, type, usage, pool, size, and a vertex buffer's FVF */
    COUNT();
    res_t *r = RES(A32(0));
    uint32_t p = A32(1);
    MEM32(p) = r->format, MEM32(p + 4) = r->kind, MEM32(p + 8) = r->usage, MEM32(p + 12) = r->pool, MEM32(p + 16) = r->size;
    if (r->kind == R_VERTEXBUFFER) MEM32(p + 20) = r->fvf;
    RET(D3D_OK, 2);
}
M(v_CreateAdditionalSwapChain) { COUNT(); out_obj(A32(2), 0); RET(D3DERR_NOTAVAILABLE, 3); }

/* ---- the vtables, in the SDK's order ----------------------------------------- */
#define E(f) { #f, f }
#define IUNKNOWN E(u_QueryInterface), E(u_AddRef), E(u_Release)
#define RESOURCE E(u_GetDevice), E(u_SetPrivateData), E(u_GetPrivateData), E(u_FreePrivateData), E(u_SetPriority), E(u_GetPriority), E(u_PreLoad), E(u_GetType)
#define END { NULL, NULL }
static const win32hle_shim vt_d3d[] = { IUNKNOWN, E(d_RegisterSoftwareDevice), E(d_GetAdapterCount), E(d_GetAdapterIdentifier),
    E(d_GetAdapterModeCount), E(d_EnumAdapterModes), E(d_GetAdapterDisplayMode), E(d_CheckDeviceType), E(d_CheckDeviceFormat),
    E(d_CheckDeviceMultiSampleType), E(d_CheckDepthStencilMatch), E(d_GetDeviceCaps), E(d_GetAdapterMonitor), E(d_CreateDevice), END };
static const win32hle_shim vt_dev[] = { IUNKNOWN,
    E(v_TestCooperativeLevel), E(v_GetAvailableTextureMem), E(v_ResourceManagerDiscardBytes), E(v_GetDirect3D), E(v_GetDeviceCaps),
    E(v_GetDisplayMode), E(v_GetCreationParameters), E(v_SetCursorProperties), E(v_SetCursorPosition), E(v_ShowCursor),
    E(v_CreateAdditionalSwapChain), E(v_Reset), E(v_Present), E(v_GetBackBuffer), E(v_GetRasterStatus), E(v_SetGammaRamp),
    E(v_GetGammaRamp), E(v_CreateTexture), E(v_CreateVolumeTexture), E(v_CreateCubeTexture), E(v_CreateVertexBuffer),
    E(v_CreateIndexBuffer), E(v_CreateRenderTarget), E(v_CreateDepthStencilSurface), E(v_CreateImageSurface), E(v_CopyRects),
    E(v_UpdateTexture), E(v_GetFrontBuffer), E(v_SetRenderTarget), E(v_GetRenderTarget), E(v_GetDepthStencilSurface),
    E(v_BeginScene), E(v_EndScene), E(v_Clear), E(v_SetTransform), E(v_GetTransform), E(v_MultiplyTransform), E(v_SetViewport),
    E(v_GetViewport), E(v_SetMaterial), E(v_GetMaterial), E(v_SetLight), E(v_GetLight), E(v_LightEnable), E(v_GetLightEnable),
    E(v_SetClipPlane), E(v_GetClipPlane), E(v_SetRenderState), E(v_GetRenderState), E(v_BeginStateBlock), E(v_EndStateBlock),
    E(v_ApplyStateBlock), E(v_CaptureStateBlock), E(v_DeleteStateBlock), E(v_CreateStateBlock), E(v_SetClipStatus),
    E(v_GetClipStatus), E(v_GetTexture), E(v_SetTexture), E(v_GetTextureStageState), E(v_SetTextureStageState), E(v_ValidateDevice),
    E(v_GetInfo), E(v_SetPaletteEntries), E(v_GetPaletteEntries), E(v_SetCurrentTexturePalette), E(v_GetCurrentTexturePalette),
    E(v_DrawPrimitive), E(v_DrawIndexedPrimitive), E(v_DrawPrimitiveUP), E(v_DrawIndexedPrimitiveUP), E(v_ProcessVertices),
    E(v_CreateVertexShader), E(v_SetVertexShader), E(v_GetVertexShader), E(v_DeleteVertexShader), E(v_SetVertexShaderConstant),
    E(v_GetVertexShaderConstant), E(v_GetVertexShaderDeclaration), E(v_GetShaderFunction), E(v_SetStreamSource),
    E(v_GetStreamSource), E(v_SetIndices), E(v_GetIndices), E(v_CreatePixelShader), E(v_SetPixelShader), E(v_GetPixelShader),
    E(v_DeletePixelShader), E(v_SetPixelShaderConstant), E(v_GetPixelShaderConstant), E(v_GetShaderFunction), E(v_DrawPatch),
    E(v_DrawPatch), E(v_DeletePatch), END };
_Static_assert(sizeof vt_d3d / sizeof *vt_d3d == 16 + 1, "IDirect3D8 has 16 methods");
_Static_assert(sizeof vt_dev / sizeof *vt_dev == 97 + 1, "IDirect3DDevice8 has 97 methods");
static const win32hle_shim vt_tex[] = { IUNKNOWN, RESOURCE, E(r_SetLOD), E(r_GetLOD), E(r_GetLevelCount), E(t_GetLevelDesc),
    E(t_GetSurfaceLevel), E(t_LockRect), E(t_UnlockRect), E(t_AddDirtyRect), END };
static const win32hle_shim vt_cube[] = { IUNKNOWN, RESOURCE, E(r_SetLOD), E(r_GetLOD), E(r_GetLevelCount), E(t_GetLevelDesc),
    E(c_GetCubeMapSurface), E(c_LockRect), E(c_UnlockRect), E(c_AddDirtyRect), END };
static const win32hle_shim vt_vol[] = { IUNKNOWN, RESOURCE, E(r_SetLOD), E(r_GetLOD), E(r_GetLevelCount), E(vt_GetLevelDesc),
    E(vt_GetVolumeLevel), E(vt_LockBox), E(vt_UnlockBox), E(vt_AddDirtyBox), END };
static const win32hle_shim vt_surf[] = { IUNKNOWN, E(u_GetDevice), E(u_SetPrivateData), E(u_GetPrivateData), E(u_FreePrivateData),
    E(s_GetContainer), E(s_GetDesc), E(s_LockRect), E(s_UnlockRect), END };
static const win32hle_shim vt_volume[] = { IUNKNOWN, E(u_GetDevice), E(u_SetPrivateData), E(u_GetPrivateData), E(u_FreePrivateData),
    E(s_GetContainer), E(vo_GetDesc), E(vo_LockBox), E(vo_UnlockBox), END };
static const win32hle_shim vt_buf[] = { IUNKNOWN, RESOURCE, E(b_Lock), E(b_Unlock), E(b_GetDesc), END };
_Static_assert(sizeof vt_tex / sizeof *vt_tex == 19 + 1 && sizeof vt_surf / sizeof *vt_surf == 11 + 1 &&
               sizeof vt_volume / sizeof *vt_volume == 11 + 1 &&
               sizeof vt_buf / sizeof *vt_buf == 14 + 1, "textures have 19 methods, surfaces 11, buffers 14");

static void d_Direct3DCreate8(void) {        /* (D3D_SDK_VERSION) */
    if (!gpu_available()) { fprintf(stderr, "[d3d8] no GPU\n"); RET(0, 1); }
    if (!g_vt_d3d) {
        g_vt_d3d = hle_com_vtable(vt_d3d), g_vt_dev = hle_com_vtable(vt_dev), g_vt_tex = hle_com_vtable(vt_tex);
        g_vt_cube = hle_com_vtable(vt_cube), g_vt_vol = hle_com_vtable(vt_vol), g_vt_surf = hle_com_vtable(vt_surf), g_vt_volume = hle_com_vtable(vt_volume);
        g_vt_vb = hle_com_vtable(vt_buf), g_vt_ib = hle_com_vtable(vt_buf);
    }
    RET(make(g_vt_d3d, 16, NULL), 1);
}
const win32hle_shim jpog_d3d8_shims[] = { { "Direct3DCreate8", d_Direct3DCreate8 }, { NULL, NULL } };
