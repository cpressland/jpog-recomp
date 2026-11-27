/*
 * dxso.h - Direct3D 8 shader bytecode (and Direct3D 9's) to Metal Shading
 * Language (dxso.c, from stl-recomp).
 *
 * The interface between the stages is fixed, so any vertex stage (a shader,
 * or fixed function, ff.c) pairs with any pixel stage:
 *
 *   vertex stage_in      v<n> [[attribute(n)]]; dxso_shader.input[] says
 *                        each one's usage (Direct3D 8: register n)
 *   vertex -> fragment   struct dxso_varyings (dxso_head)
 *   buffers              0 float constants (float4[256] VS, [32] PS),
 *                        1 int constants, 2 bool, 3 struct dxso_params;
 *                        streams from buffer 8
 *   textures, samplers   s<n> [[texture(n)]], [[sampler(n)]] (stage n)
 *
 * Done in the shader because Metal has no fixed function for them: the
 * pixel centres, the depth bias, user clip planes, the alpha test and fog.
 */
#ifndef DXSO_H
#define DXSO_H
#include <stddef.h>
#include <stdint.h>

#define DXSO_MAX_INPUTS 16

enum { DXSO_TEX_2D = 0, DXSO_TEX_CUBE = 1, DXSO_TEX_3D = 2 };
enum { DXSO_MSL = 0, DXSO_GLSL = 1 };
#define DXSO_USAGE_REG 0xE0              /* a Direct3D 8 input: index is the register */

typedef struct {
    /* ps_1_x declares no sampler types: each stage's bound texture's. */
    uint8_t sampler_type[16];
    /* A depth texture bound at a stage: sampled as a comparison
     * (hardware shadow mapping, tex2Dproj on a D24S8 texture). */
    uint16_t sampler_depth;
    uint8_t project[8];        /* ps_1_x: divide the stage's coordinates by z (3) or w (4) */
    uint8_t lang;              /* DXSO_MSL (default), DXSO_GLSL */
} dxso_options;

typedef struct {
    int pixel;                 /* 0 vertex, 1 pixel */
    int major, minor;
    /* Vertex inputs: for v<n>, its D3DDECLUSAGE (0xFF: not declared), index. */
    uint8_t input_usage[DXSO_MAX_INPUTS], input_index[DXSO_MAX_INPUTS];
    uint16_t samplers_used;    /* bit n: s<n> (ps_1_x: stage n) */
    uint8_t sampler_type[16];
    char *text;                /* malloc'd: MSL (entry point dxso_main) */
    char error[256];           /* why it failed, when text is NULL */
} dxso_shader;

/* The parameters every shader gets in buffer 3. Kept in step with the MSL
 * declaration (dxso_head). */
typedef struct {
    float half_pixel[2];
    float alpha_ref;           /* 0..255 */
    uint32_t alpha_func;       /* D3DCMP_*; 8 (ALWAYS) or 0: no test */
    float fog_color[4];
    uint32_t fog_mode;         /* 0 none, 1 the vertex's factor, 2 linear, 3 exp, 4 exp2 by the pixel */
    float depth_bias;
    uint32_t clip_mask, pad;
    float fog[4];              /* start, end, density; nonzero: by w, else by z */
    float bem[8][4];           /* each stage's D3DTSS_BUMPENVMAT00, 01, 10, 11 */
    float beml[8][4];          /* ...LSCALE, LOFFSET */
    float clip[6][4];          /* user clip planes, in clip space */
} dxso_params;

/* The text every shader opens with (malloc'd). */
void dxso_head(char **text);
void dxso_head_glsl(int pixel, char **text);
/* 0, with out filled; else -1, and out->error says why. */
int dxso_translate(const uint32_t *code, size_t words, const dxso_options *opt, dxso_shader *out);
void dxso_free(dxso_shader *s);

#endif
