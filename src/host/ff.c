/*
 * ff.c - Direct3D 8's fixed function as Metal shaders: the vertex stage
 * (transform, lighting, fog, texture coordinates) and the pixel stage (the
 * texture stage cascade). Either can be paired with a shader (dxso.c): they
 * share its varyings and its end of a pixel.
 *
 * The vertex stage is one text, steered by its constants (ff_vs): what a
 * vertex lacks reads as zero and `has` says so. The pixel stage is written
 * for each combination of operations (ff_pskey), as a driver would compile
 * it. The equations are the SDK's ("Mathematics of Lighting", "Texture
 * Blending"); a stage whose arguments name a texture with none bound ends
 * the cascade, as on hardware.
 *
 * Matrices are Direct3D's, row vectors and rows in memory: loaded into a
 * float4x4 whose columns are those rows, v * M is M * v.
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32hle.h"
#include "jpog.h"
#include "d3d_int.h"

/* D3DVSDE_*: the registers fixed function takes its vertex from. */
enum { V_POS = 0, V_WEIGHT, V_INDICES, V_NORMAL, V_PSIZE, V_DIFFUSE, V_SPECULAR, V_TEX0 };

/* D3DRS_* and D3DTSS_*, as d3d8types.h numbers them. */
enum { RS_ALPHATESTENABLE = 15, RS_ALPHAREF = 24, RS_ALPHAFUNC = 25, RS_FOGENABLE = 28, RS_SPECULARENABLE = 29,
       RS_FOGCOLOR = 34, RS_FOGTABLEMODE = 35, RS_FOGSTART = 36, RS_FOGEND = 37, RS_FOGDENSITY = 38, RS_ZBIAS = 47,
       RS_RANGEFOGENABLE = 48, RS_TEXTUREFACTOR = 60, RS_LIGHTING = 137, RS_AMBIENT = 139, RS_FOGVERTEXMODE = 140,
       RS_COLORVERTEX = 141, RS_LOCALVIEWER = 142, RS_NORMALIZENORMALS = 143, RS_DIFFUSESRC = 145, RS_SPECULARSRC = 146,
       RS_AMBIENTSRC = 147, RS_EMISSIVESRC = 148, RS_VERTEXBLEND = 151, RS_CLIPPLANEENABLE = 152 };
enum { TS_COLOROP = 1, TS_COLORARG1, TS_COLORARG2, TS_ALPHAOP, TS_ALPHAARG1, TS_ALPHAARG2, TS_BUMP00, TS_BUMP01,
       TS_BUMP10, TS_BUMP11, TS_TEXCOORDINDEX, TS_LSCALE = 22, TS_LOFFSET, TS_TTFF, TS_COLORARG0 = 26, TS_ALPHAARG0,
       TS_RESULTARG };
enum { XF_VIEW = 2, XF_PROJ = 3, XF_TEX0 = 16, XF_WORLD = 256 };

static float uf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static void rgba(float *o, uint32_t c) {
    o[0] = ((c >> 16) & 255) / 255.0f, o[1] = ((c >> 8) & 255) / 255.0f, o[2] = (c & 255) / 255.0f, o[3] = (c >> 24) / 255.0f;
}
/* a * b, row vectors: first a, then b. */
static void mul(float *o, const float *a, const float *b) {
    float t[16];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            t[4 * r + c] = a[4 * r] * b[c] + a[4 * r + 1] * b[4 + c] + a[4 * r + 2] * b[8 + c] + a[4 * r + 3] * b[12 + c];
    memcpy(o, t, 64);
}
static int invert(float *o, const float *m) {                 /* 0 if singular */
    double a[4][8];
    for (int r = 0; r < 4; r++) for (int c = 0; c < 8; c++) a[r][c] = c < 4 ? m[4 * r + c] : (c - 4 == r);
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++) if (fabs(a[r][c]) > fabs(a[p][c])) p = r;
        if (fabs(a[p][c]) < 1e-30) return 0;
        for (int k = 0; k < 8; k++) { double t = a[c][k]; a[c][k] = a[p][k]; a[p][k] = t; }
        double d = a[c][c];
        for (int k = 0; k < 8; k++) a[c][k] /= d;
        for (int r = 0; r < 4; r++) {
            double f = r == c ? 0 : a[r][c];
            if (f != 0) for (int k = 0; k < 8; k++) a[r][k] -= f * a[c][k];
        }
    }
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) o[4 * r + c] = (float)a[r][4 + c];
    return 1;
}

/* ---- the vertex stage ------------------------------------------------------- */
const char *ff_vertex_text(void) {
    static char *text;
    if (text) return text;
    static const char body[] =
"struct ff_light { float4 diffuse, specular, ambient, pos, dir, att, spot; };\n"
"struct ff_vs {\n"
"    float4x4 world_view[4]; float4 normal[3]; float4x4 proj; float4x4 tex[8];\n"
"    float4 viewport, mdiffuse, mambient, mspecular, memissive, ambient, fog;\n"
"    float power; uint rhw, has, lighting, specular, localviewer, normalize, sources;\n"
"    uint fog_mode, range_fog, nblend, pad;\n"
"    uint4 stage[8]; ff_light light[8];\n};\n"
"struct ff_in { float4 v0 [[attribute(0)]]; float4 v1 [[attribute(1)]]; float4 v3 [[attribute(3)]];\n"
"    float4 v5 [[attribute(5)]]; float4 v6 [[attribute(6)]]; float4 v7 [[attribute(7)]]; float4 v8 [[attribute(8)]];\n"
"    float4 v9 [[attribute(9)]]; float4 v10 [[attribute(10)]]; float4 v11 [[attribute(11)]]; float4 v12 [[attribute(12)]];\n"
"    float4 v13 [[attribute(13)]]; float4 v14 [[attribute(14)]]; };\n"
"static float4 ff_source(uint which, float4 mat, float4 c1, float4 c2) { return which == 1u ? c1 : which == 2u ? c2 : mat; }\n"
"vertex dxso_varyings dxso_main(ff_in in [[stage_in]], constant ff_vs &U [[buffer(0)]], constant dxso_params &P [[buffer(3)]])\n{\n"
"    dxso_varyings out;\n"
"    float4 c1 = (U.has & 2u) ? in.v5 : float4(1.0), c2 = (U.has & 4u) ? in.v6 : float4(0.0);\n"
"    float4 uv[8] = { in.v7, in.v8, in.v9, in.v10, in.v11, in.v12, in.v13, in.v14 };\n"
"    float3 ep = float3(0.0), n = float3(0.0);\n"
"    if (U.rhw) {                                       /* already on the screen, w 1 / rhw */\n"
"        float w = in.v0.w != 0.0 ? 1.0 / in.v0.w : 1.0;\n"
"        float2 p = (in.v0.xy - U.viewport.xy) / U.viewport.zw * float2(2.0, -2.0) + float2(-1.0, 1.0);\n"
"        dxso_position(out, float4(p, in.v0.z, 1.0) * w, P);\n"
"        out.d0 = c1; out.d1 = c2; out.fog = c2.a;\n"
"    } else {\n"
"        float4 pos = float4(in.v0.xyz, 1.0), e4;\n"
"        if (U.nblend == 0u) e4 = U.world_view[0] * pos;\n"
"        else {\n"
"            float last = 1.0; e4 = float4(0.0);\n"
"            for (uint i = 0; i < U.nblend; i++) { float w = in.v1[i]; last -= w; e4 += w * (U.world_view[i] * pos); }\n"
"            e4 += last * (U.world_view[U.nblend] * pos);\n"
"        }\n"
"        ep = e4.xyz;\n"
"        if (U.has & 1u) {\n"
"            n = in.v3.x * U.normal[0].xyz + in.v3.y * U.normal[1].xyz + in.v3.z * U.normal[2].xyz;\n"
"            if (U.normalize) n = length_squared(n) > 0.0 ? normalize(n) : n;\n"
"        }\n"
"        dxso_position(out, U.proj * e4, P);\n"
"        out.d0 = c1; out.d1 = c2;\n"
"        if (U.lighting) {\n"
"            float4 md = ff_source(U.sources & 3u, U.mdiffuse, c1, c2), ms = ff_source((U.sources >> 2) & 3u, U.mspecular, c1, c2);\n"
"            float4 ma = ff_source((U.sources >> 4) & 3u, U.mambient, c1, c2), me = ff_source((U.sources >> 6) & 3u, U.memissive, c1, c2);\n"
"            float3 dif = float3(0.0), amb = U.ambient.rgb, spc = float3(0.0);\n"
"            for (uint i = 0; i < 8u; i++) {\n"
"                constant ff_light &L = U.light[i];\n"
"                uint type = uint(L.pos.w);\n"
"                if (type == 0u) break;\n"
"                float3 l; float at = 1.0;\n"
"                if (type == 3u) l = -L.dir.xyz;\n"
"                else {\n"
"                    l = L.pos.xyz - ep; float d = length(l);\n"
"                    if (d > L.dir.w) continue;\n"
"                    l = d > 0.0 ? l / d : float3(0.0);\n"
"                    at = 1.0 / max(L.att.x + L.att.y * d + L.att.z * d * d, 1e-20);\n"
"                    if (type == 2u) {\n"
"                        float rho = dot(-l, L.dir.xyz);\n"
"                        if (rho <= L.spot.y) at = 0.0;\n"
"                        else if (rho < L.spot.x) at *= pow((rho - L.spot.y) / (L.spot.x - L.spot.y), L.att.w);\n"
"                    }\n"
"                }\n"
"                amb += L.ambient.rgb * at;\n"
"                float nl = dot(n, l);\n"
"                if (nl > 0.0) {\n"
"                    dif += L.diffuse.rgb * (nl * at);\n"
"                    if (U.specular) {\n"
"                        float3 v = U.localviewer ? normalize(-ep) : float3(0.0, 0.0, -1.0);\n"
"                        float nh = dot(n, normalize(l + v));\n"
"                        if (nh > 0.0) spc += L.specular.rgb * (pow(nh, U.power) * at);\n"
"                    }\n"
"                }\n"
"            }\n"
"            out.d0 = float4(me.rgb + ma.rgb * amb + md.rgb * dif, md.a);\n"
"            out.d1 = float4(ms.rgb * spc, (U.has & 4u) ? c2.a : 0.0);\n"
"        }\n"
"        out.fog = c2.a;\n"
"        if (U.fog_mode != 0u) {\n"
"            float d = U.range_fog ? length(ep) : abs(ep.z);\n"
"            out.fog = U.fog_mode == 3u ? (U.fog.y - d) / (U.fog.y - U.fog.x) : U.fog_mode == 1u ? exp(-d * U.fog.z)\n"
"                                                                              : exp(-(d * U.fog.z) * (d * U.fog.z));\n"
"        }\n"
"    }\n"
"    out.d0 = saturate(out.d0); out.d1 = saturate(out.d1);\n"
"    float4 t[8];\n"
"    for (uint i = 0; i < 8u; i++) {\n"
"        uint4 s = U.stage[i];\n"
"        float4 c = uv[s.x & 7u];\n"
"        if (s.y == 1u) c = float4(n, 1.0);\n"
"        else if (s.y == 2u) c = float4(ep, 1.0);\n"
"        else if (s.y == 3u) c = float4(reflect(normalize(ep), n), 1.0);\n"
"        else if (s.w == 0u) c = float4(0.0, 0.0, 0.0, 1.0);\n"
"        else if (s.z != 0u) c = s.w == 1u ? float4(c.x, 1.0, 0.0, 0.0) : s.w == 2u ? float4(c.xy, 1.0, 0.0) : s.w == 3u ? float4(c.xyz, 1.0) : c;\n"
"        else c = s.w == 1u ? float4(c.x, 0.0, 0.0, 1.0) : s.w == 2u ? float4(c.xy, 0.0, 1.0) : s.w == 3u ? float4(c.xyz, 1.0) : c;\n"
"        if (s.z != 0u) c = U.tex[i] * c;\n"
"        t[i] = c;\n"
"    }\n"
"    out.t0 = t[0]; out.t1 = t[1]; out.t2 = t[2]; out.t3 = t[3]; out.t4 = t[4]; out.t5 = t[5]; out.t6 = t[6]; out.t7 = t[7];\n"
"    return out;\n}\n";
    char *head;
    dxso_head(&head);
    size_t n = strlen(head);
    text = malloc(n + sizeof body);
    memcpy(text, head, n);
    memcpy(text + n, body, sizeof body);
    free(head);
    return text;
}

static const uint8_t TYPE_COMPS[8] = { 1, 2, 3, 4, 4, 4, 2, 4 };

void ff_vs_fill(const dev_t_ *d, const elem_t *e, int ne, ff_vs *u) {
    memset(u, 0, sizeof *u);
    uint8_t comps[8] = { 0 };
    for (int i = 0; i < ne; i++) {
        if (e[i].reg == V_POS && e[i].type == 3) u->rhw = 1;          /* a four-component position: XYZRHW */
        else if (e[i].reg == V_NORMAL) u->has |= 1;
        else if (e[i].reg == V_DIFFUSE) u->has |= 2;
        else if (e[i].reg == V_SPECULAR) u->has |= 4;
        else if (e[i].reg >= V_TEX0 && e[i].reg < V_TEX0 + 8) comps[e[i].reg - V_TEX0] = TYPE_COMPS[e[i].type & 7];
    }
    const uint32_t *rs = d->rs;
    const float *view = d->transform[XF_VIEW];
    u->nblend = rs[RS_VERTEXBLEND] <= 3 ? rs[RS_VERTEXBLEND] : 0;
    for (uint32_t i = 0; i <= u->nblend; i++) mul(&u->world_view[i][0][0], d->transform[XF_WORLD + i], view);
    float inv[16];
    if (invert(inv, &u->world_view[0][0][0]))                         /* a normal's row i: column i of the inverse */
        for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) u->normal[r][c] = inv[4 * c + r];
    memcpy(u->proj, d->transform[XF_PROJ], 64);
    for (int i = 0; i < 4; i++) u->viewport[i] = (float)d->viewport[i];
    memcpy(u->mdiffuse, &d->material[0], 16), memcpy(u->mambient, &d->material[4], 16);
    memcpy(u->mspecular, &d->material[8], 16), memcpy(u->memissive, &d->material[12], 16);
    u->power = uf(d->material[16]);
    rgba(u->ambient, rs[RS_AMBIENT]);
    u->lighting = rs[RS_LIGHTING] != 0, u->specular = rs[RS_SPECULARENABLE] != 0;
    u->localviewer = rs[RS_LOCALVIEWER] != 0, u->normalize = rs[RS_NORMALIZENORMALS] != 0;
    if (rs[RS_COLORVERTEX]) {
        static const int src[4] = { RS_DIFFUSESRC, RS_SPECULARSRC, RS_AMBIENTSRC, RS_EMISSIVESRC };
        for (int i = 0; i < 4; i++) {
            uint32_t w = rs[src[i]];
            if ((w == 1 && (u->has & 2)) || (w == 2 && (u->has & 4))) u->sources |= w << (2 * i);
        }
    }
    u->fog[0] = uf(rs[RS_FOGSTART]), u->fog[1] = uf(rs[RS_FOGEND]), u->fog[2] = uf(rs[RS_FOGDENSITY]);
    u->fog_mode = rs[RS_FOGENABLE] && !rs[RS_FOGTABLEMODE] ? rs[RS_FOGVERTEXMODE] : 0;
    u->range_fog = rs[RS_RANGEFOGENABLE] != 0;
    for (int i = 0; i < 8; i++) {
        uint32_t tci = d->tss[i][TS_TEXCOORDINDEX], ttff = d->tss[i][TS_TTFF] & 0xFF;
        u->stage[i][0] = tci & 7, u->stage[i][1] = u->rhw ? 0 : tci >> 16;
        u->stage[i][2] = u->rhw ? 0 : ttff, u->stage[i][3] = comps[tci & 7];
        memcpy(u->tex[i], d->transform[XF_TEX0 + i], 64);
    }
    int n = 0;
    for (int i = 0; i < 8; i++) {
        if (!d->light_on[i]) continue;
        const uint32_t *l = d->light[i];
        ff_light *o = &u->light[n++];
        memcpy(o->diffuse, l + 1, 16), memcpy(o->specular, l + 5, 16), memcpy(o->ambient, l + 9, 16);
        float p[3], q[3];
        memcpy(p, l + 13, 12), memcpy(q, l + 16, 12);
        for (int c = 0; c < 3; c++) {
            o->pos[c] = p[0] * view[c] + p[1] * view[4 + c] + p[2] * view[8 + c] + view[12 + c];
            o->dir[c] = q[0] * view[c] + q[1] * view[4 + c] + q[2] * view[8 + c];
        }
        float len = sqrtf(o->dir[0] * o->dir[0] + o->dir[1] * o->dir[1] + o->dir[2] * o->dir[2]);
        if (len > 0) for (int c = 0; c < 3; c++) o->dir[c] /= len;
        o->pos[3] = (float)l[0];
        o->dir[3] = uf(l[19]);
        o->att[0] = uf(l[21]), o->att[1] = uf(l[22]), o->att[2] = uf(l[23]), o->att[3] = uf(l[20]);
        o->spot[0] = cosf(uf(l[24]) / 2), o->spot[1] = cosf(uf(l[25]) / 2);
    }
}

/* ---- the pixel stage -------------------------------------------------------- */
typedef struct { char *p; size_t n, cap; } sb;
static void add(sb *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (s->n + (size_t)k + 1 > s->cap) s->p = realloc(s->p, s->cap = (s->n + (size_t)k + 1) * 2);
    va_start(ap, fmt);
    vsnprintf(s->p + s->n, (size_t)k + 1, fmt, ap);
    va_end(ap);
    s->n += (size_t)k;
}

/* An argument: D3DTA_*, with COMPLEMENT (0x10) and ALPHAREPLICATE (0x20). */
static void argument(uint32_t a, int stage, char *o, size_t cap) {
    static const char *const base[] = { "in.d0", "cur", "", "U.factor", "spec", "tmp" };
    char b[32];
    if ((a & 7) == 2) snprintf(b, sizeof b, "x%d", stage);
    else snprintf(b, sizeof b, "%s", (a & 7) < 6 ? base[a & 7] : "cur");
    char c[48];
    snprintf(c, sizeof c, a & 0x20 ? "float4(%s.a)" : "%s", b);
    snprintf(o, cap, a & 0x10 ? "(1.0 - %s)" : "%s", c);
}

/* Which arguments an operation reads: bits 0, 1, 2 for arg 0, 1, 2. */
static int op_args(int op) {
    switch (op) {
    case 1: case 0: return 0;
    case 2: case 17: return 2;
    case 3: return 4;
    case 25: case 26: return 7;
    default: return 6;
    }
}

/* D3DTOP_* on float4 (the colour takes .rgb of it, the alpha .a). */
static void operation(int op, const char *a0, const char *a1, const char *a2, int stage, char *o, size_t cap) {
    switch (op) {
    case 2: snprintf(o, cap, "%s", a1); break;
    case 3: snprintf(o, cap, "%s", a2); break;
    case 4: snprintf(o, cap, "%s * %s", a1, a2); break;
    case 5: snprintf(o, cap, "%s * %s * 2.0", a1, a2); break;
    case 6: snprintf(o, cap, "%s * %s * 4.0", a1, a2); break;
    case 7: snprintf(o, cap, "%s + %s", a1, a2); break;
    case 8: snprintf(o, cap, "%s + %s - 0.5", a1, a2); break;
    case 9: snprintf(o, cap, "(%s + %s - 0.5) * 2.0", a1, a2); break;
    case 10: snprintf(o, cap, "%s - %s", a1, a2); break;
    case 11: snprintf(o, cap, "%s + %s * (1.0 - %s)", a1, a2, a1); break;
    case 12: snprintf(o, cap, "mix(%s, %s, in.d0.a)", a2, a1); break;
    case 13: snprintf(o, cap, "mix(%s, %s, x%d.a)", a2, a1, stage); break;
    case 14: snprintf(o, cap, "mix(%s, %s, U.factor.a)", a2, a1); break;
    case 15: snprintf(o, cap, "%s + %s * (1.0 - x%d.a)", a1, a2, stage); break;
    case 16: snprintf(o, cap, "mix(%s, %s, cur.a)", a2, a1); break;
    case 18: snprintf(o, cap, "%s + %s.a * %s", a1, a1, a2); break;
    case 19: snprintf(o, cap, "%s * %s + %s.a", a1, a2, a1); break;
    case 20: snprintf(o, cap, "(1.0 - %s.a) * %s + %s", a1, a2, a1); break;
    case 21: snprintf(o, cap, "(1.0 - %s) * %s + %s.a", a1, a2, a1); break;
    case 24: snprintf(o, cap, "float4(saturate(4.0 * dot(%s.rgb - 0.5, %s.rgb - 0.5)))", a1, a2); break;
    case 25: snprintf(o, cap, "%s + %s * %s", a0, a1, a2); break;
    case 26: snprintf(o, cap, "mix(%s, %s, %s)", a2, a1, a0); break;
    default: snprintf(o, cap, "%s", a1); break;                        /* PREMODULATE: the next stage's, not done */
    }
}

char *ff_pixel_text(const ff_pskey *k) {
    sb o = { 0 };
    char *head;
    dxso_head(&head);
    add(&o, "%s", head);
    free(head);
    add(&o, "struct ff_ps { float4 factor; float4 constant_[8]; };\n"
            "fragment float4 dxso_main(dxso_varyings in [[stage_in]], constant ff_ps &U [[buffer(0)]], constant dxso_params &P [[buffer(3)]]");
    for (int i = 0; i < k->n; i++)
        if (k->s[i].kind != 0xFF)
            add(&o, ",\n    %s<float> s%d [[texture(%d)]], sampler z%d [[sampler(%d)]]",
                k->s[i].kind == DXSO_TEX_CUBE ? "texturecube" : k->s[i].kind == DXSO_TEX_3D ? "texture3d" : "texture2d", i, i, i, i);
    add(&o, ")\n{\n    float4 cur = in.d0, spec = in.d1, tmp = float4(0.0), r;\n    (void)spec; (void)tmp;\n");
    int bumped = -1;                                                   /* the stage whose bump map moves the next one's coordinates */
    for (int i = 0; i < k->n; i++) {
        if (k->s[i].kind != 0xFF) {
            add(&o, "    float4 u%d = in.t%d;\n", i, i);
            if (k->s[i].project) add(&o, "    u%d /= u%d.%c;\n", i, i, k->s[i].project == 3 ? 'z' : 'w');
            if (bumped == i - 1 && bumped >= 0)
                add(&o, "    u%d.xy += x%d.x * P.bem[%d].xy + x%d.y * P.bem[%d].zw;\n", i, i - 1, i - 1, i - 1, i - 1);
            add(&o, "    float4 x%d = s%d.sample(z%d, u%d.%s);\n", i, i, i, i, k->s[i].kind == DXSO_TEX_2D ? "xy" : "xyz");
            if (bumped == i - 1 && bumped >= 0 && k->s[i - 1].cop == 23)
                add(&o, "    x%d.rgb *= saturate(x%d.z * P.beml[%d].x + P.beml[%d].y);\n", i, i - 1, i - 1, i - 1);
        } else
            add(&o, "    float4 x%d = float4(0.0, 0.0, 0.0, 1.0); (void)x%d;\n", i, i);
        char a0[64], a1[64], a2[64], c[400], a[400];
        if (k->s[i].cop == 22 || k->s[i].cop == 23) { bumped = i; continue; }     /* BUMPENVMAP: the result goes on as it is */
        argument(k->s[i].ca0, i, a0, sizeof a0), argument(k->s[i].ca1, i, a1, sizeof a1), argument(k->s[i].ca2, i, a2, sizeof a2);
        operation(k->s[i].cop, a0, a1, a2, i, c, sizeof c);
        if (k->s[i].cop == 24) snprintf(a, sizeof a, "r");             /* DOTPRODUCT3 writes all four */
        else if (k->s[i].aop <= 1) snprintf(a, sizeof a, "cur");
        else {
            argument(k->s[i].aa0, i, a0, sizeof a0), argument(k->s[i].aa1, i, a1, sizeof a1), argument(k->s[i].aa2, i, a2, sizeof a2);
            operation(k->s[i].aop, a0, a1, a2, i, a, sizeof a);
        }
        add(&o, "    r = %s;\n    r = saturate(float4(r.rgb, (%s).a));\n    %s = r;\n", c, a, k->s[i].result == 5 ? "tmp" : "cur");
    }
    if (k->specular) add(&o, "    cur.rgb = saturate(cur.rgb + spec.rgb);\n");
    add(&o, "    return dxso_finish(cur, in, P);\n}\n");
    return o.p;
}

int ff_projected(const dev_t_ *d, int s) {
    uint32_t t = d->tss[s][TS_TTFF];
    return !(t & 0x100) ? 0 : (t & 0xFF) == 3 ? 3 : 4;
}

void ff_ps_key(const dev_t_ *d, const uint8_t kinds[8], ff_pskey *k) {
    memset(k, 0, sizeof *k);
    k->specular = d->rs[RS_SPECULARENABLE] != 0;
    for (int i = 0; i < 8; i++) {
        const uint32_t *t = d->tss[i];
        uint32_t cop = t[TS_COLOROP], aop = t[TS_ALPHAOP];
        if (cop <= 1) break;
        int need = op_args((int)cop);
        if (kinds[i] == 0xFF && (((need & 1) && (t[TS_COLORARG0] & 7) == 2) || ((need & 2) && (t[TS_COLORARG1] & 7) == 2) ||
                                 ((need & 4) && (t[TS_COLORARG2] & 7) == 2)))
            break;                                                     /* a texture asked for and not there */
        k->s[i].cop = (uint8_t)cop, k->s[i].aop = (uint8_t)aop;
        k->s[i].ca0 = (uint8_t)t[TS_COLORARG0], k->s[i].ca1 = (uint8_t)t[TS_COLORARG1], k->s[i].ca2 = (uint8_t)t[TS_COLORARG2];
        k->s[i].aa0 = (uint8_t)t[TS_ALPHAARG0], k->s[i].aa1 = (uint8_t)t[TS_ALPHAARG1], k->s[i].aa2 = (uint8_t)t[TS_ALPHAARG2];
        k->s[i].result = (uint8_t)t[TS_RESULTARG];
        k->s[i].kind = kinds[i];
        k->s[i].project = (uint8_t)ff_projected(d, i);
        k->n = (uint8_t)(i + 1);
    }
}
void ff_ps_fill(const dev_t_ *d, ff_ps *u) {
    memset(u, 0, sizeof *u);
    rgba(u->factor, d->rs[RS_TEXTUREFACTOR]);
}

/* ---- what both kinds of shader are given ------------------------------------- */
void ff_params(const dev_t_ *d, int programmable_vs, int rhw, float vw, float vh, dxso_params *p) {
    memset(p, 0, sizeof *p);
    const uint32_t *rs = d->rs;
    /* Half a pixel right and down, less a 128th: a quad with whole-number
     * edges has its first pixels' centres just inside it, not on the edge. */
    p->half_pixel[0] = 2 * (0.5f - 1 / 128.0f) / vw, p->half_pixel[1] = -2 * (0.5f - 1 / 128.0f) / vh;
    p->alpha_ref = (float)(rs[RS_ALPHAREF] & 255);
    p->alpha_func = rs[RS_ALPHATESTENABLE] ? rs[RS_ALPHAFUNC] : 0;
    rgba(p->fog_color, rs[RS_FOGCOLOR]);
    if (rs[RS_FOGENABLE]) {
        uint32_t table = rs[RS_FOGTABLEMODE];                          /* 1 exp, 2 exp2, 3 linear */
        p->fog_mode = table == 3 ? 2 : table == 1 ? 3 : table == 2 ? 4 : 1;
        p->fog[0] = uf(rs[RS_FOGSTART]), p->fog[1] = uf(rs[RS_FOGEND]), p->fog[2] = uf(rs[RS_FOGDENSITY]);
        const float *m = d->transform[XF_PROJ];                        /* by w when the projection makes one */
        p->fog[3] = !rhw && !(m[3] == 0 && m[7] == 0 && m[11] == 0 && m[15] == 1) ? 1.0f : 0.0f;
    }
    p->depth_bias = -(float)rs[RS_ZBIAS] * 5e-6f;
    for (int i = 0; i < 8; i++) {
        const uint32_t *t = d->tss[i];
        for (int c = 0; c < 4; c++) p->bem[i][c] = uf(t[TS_BUMP00 + c]);
        p->beml[i][0] = uf(t[TS_LSCALE]), p->beml[i][1] = uf(t[TS_LOFFSET]);
    }
    p->clip_mask = rhw ? 0 : rs[RS_CLIPPLANEENABLE] & 63;
    if (p->clip_mask) {
        /* World space for fixed function, clip space for a vertex shader. */
        float vp[16], inv[16];
        mul(vp, d->transform[XF_VIEW], d->transform[XF_PROJ]);
        int ok = !programmable_vs && invert(inv, vp);
        for (int i = 0; i < 6; i++)
            for (int c = 0; c < 4; c++)                                /* clip * inv = world, so the plane is inv * plane */
                p->clip[i][c] = programmable_vs ? d->clip[i][c]
                    : ok ? inv[4 * c] * d->clip[i][0] + inv[4 * c + 1] * d->clip[i][1] + inv[4 * c + 2] * d->clip[i][2] + inv[4 * c + 3] * d->clip[i][3]
                         : 0.0f;
    }
}
