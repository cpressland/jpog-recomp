/*
 * d3d_int.h - what d3d8.c (the interfaces, the state), ff.c (fixed function)
 * and d3d_metal.m (the GPU) share: the records behind the COM objects, and
 * the GPU calls. stl-recomp's d3d9_int.h, for Direct3D 8.
 *
 * Every object is an hle_com_new block: vtable, reference count, then at +8
 * the VA of its record, in guest memory (0 once it is freed); every record
 * starts with its device. The state is kept as Direct3D 9 has it where the
 * two differ (samplers apart from the texture stages), so the GPU side stays
 * close to stl-recomp's.
 */
#ifndef D3D_INT_H
#define D3D_INT_H
#include <stdint.h>

#include "dxso.h"

#define REC(o) MEM32((o) + 8)
#define OBJ(o, T) ((T *)hle_ptr(REC(o)))

enum { R_SURFACE = 1, R_VOLUME, R_TEXTURE, R_VOLUMETEXTURE, R_CUBETEXTURE, R_VERTEXBUFFER, R_INDEXBUFFER };

typedef struct {
    uint32_t device;
    uint32_t kind;            /* D3DRESOURCETYPE */
    uint32_t width, height, depth, levels;
    uint32_t format, usage, pool;
    uint32_t size;            /* a buffer's length */
    uint32_t fvf;             /* a vertex buffer's; an index buffer's format is `format` */
    uint32_t mem[16 * 6];     /* each level's (and face's) bytes, made on first lock */
    uint32_t parent, level, face;     /* a surface of a texture */
    uint32_t surf[16 * 6];    /* a texture's: the surface of each level (and face), made once */
    uint32_t multisample;
    uint32_t dead;            /* its last reference is gone: freed once nothing is set to it */
    /* the GPU's */
    void *gpu;                /* id<MTLTexture> (a surface of a texture: its parent's) or id<MTLBuffer> */
    uint32_t dirty[6];        /* levels (bits) of each face written since they went up; a buffer's dirty[0]: written */
    uint64_t gpu_frame;       /* a dynamic buffer: the frame its copy is from */
    uint32_t gpu_offset;      /* ...and where in that frame's ring */
    void *gpu_ring;           /* ...which buffer (not retained: the frame keeps it) */
    void *gpu_map;            /* ...and where the CPU sees that copy (Vulkan) */
} res_t;

/* A vertex layout element: what FVF codes and declarations both become. */
typedef struct { uint16_t stream, offset; uint8_t type, reg; } elem_t;    /* reg: D3DVSDE_* for fixed function */
#define MAX_ELEMS 24

/* A shader: handles index a table of these (host memory, never moved). */
typedef struct {
    int used, pixel;
    uint32_t *code;  uint32_t len;        /* the function (NULL: a declaration for fixed function), bytes */
    uint32_t *decl;  uint32_t decl_len;   /* a vertex shader's declaration, as given, bytes */
    elem_t e[MAX_ELEMS]; int ne;          /* ...as a layout */
    struct { uint16_t reg; float v[4]; } k[96]; int nk;    /* ...and the constants it carries */
    dxso_shader info;
    int failed;
    void *fn[8];                          /* id<MTLFunction>, by variant */
    uint32_t variant_key[8];
} shader_t;
shader_t *shader_at(uint32_t handle);     /* d3d8.c: NULL if it is not one */

#define MAX_RS 256
#define MAX_TSS 33
#define MAX_STAGES 8

typedef struct {
    uint32_t d3d;
    uint32_t params[13];                   /* D3DPRESENT_PARAMETERS, as last set */
    uint32_t adapter, type, hwnd, flags;
    uint32_t backbuffer, depth;            /* their surfaces */
    uint32_t rt, ds;
    uint32_t rs[MAX_RS];
    uint32_t tss[MAX_STAGES][MAX_TSS];     /* D3DTSS_*, samplers' included */
    uint32_t tex[MAX_STAGES];
    uint32_t stream[16][2];                /* buffer, stride */
    uint32_t indices, base_vertex;
    uint32_t vsh, psh;                     /* an FVF or a vertex shader's handle; a pixel shader's */
    uint32_t viewport[6];
    float    transform[512][16];
    uint32_t material[17];
    float    clip[6][4];
    float    vsc[96][4], psc[8][4];
    uint32_t light[8][26];                 /* D3DLIGHT8 */
    uint32_t light_on[8];
} dev_t_;

/* A state block: pieces of dev_t_ (offset, size) and their values. */
typedef struct { uint32_t off, size, at; } sbop_t;
typedef struct {
    int used;
    uint32_t n, cap, len, data_cap;
    sbop_t *ops;
    uint8_t *data;
} sblock_t;

/* ---- d3d8.c ---- */
int      block_bytes(uint32_t format);          /* a compressed format's 4x4 block, else 0 */
int      bits_per_pixel(uint32_t format);
uint32_t level_dim(uint32_t d, uint32_t level);
uint32_t pitch_of(uint32_t format, uint32_t width);
uint32_t rows_of(uint32_t format, uint32_t height);
uint32_t level_mem(res_t *r, uint32_t level, uint32_t face, uint32_t *pitch, uint32_t *slice);
int      vertex_layout(dev_t_ *d, elem_t *e);   /* the layout now set */

/* ---- fixed function (ff.c) ---- */
#define FF_LIGHTS 8
typedef struct { float diffuse[4], specular[4], ambient[4], pos[4], dir[4], att[4], spot[4]; } ff_light;
typedef struct {                                /* buffer 0 of the vertex stage (under 4 KB) */
    float world_view[4][4][4];                  /* up to four, blended */
    float normal[3][4];                         /* world_view[0]'s inverse transpose, its rows */
    float proj[4][4];
    float tex[8][4][4];
    float viewport[4];                          /* x, y, w, h: for transformed vertices */
    float mdiffuse[4], mambient[4], mspecular[4], memissive[4];
    float ambient[4];
    float fog[4];                               /* start, end, density, - */
    float power;
    uint32_t rhw, has, lighting, specular, localviewer, normalize, sources;   /* sources: four 2-bit: d, s, a, e */
    uint32_t fog_mode, range_fog, nblend, pad;
    uint32_t stage[8][4];                       /* index, generation, count of the transform (0: none), components */
    ff_light light[FF_LIGHTS];
} ff_vs;
typedef struct { float factor[4]; float constant[8][4]; } ff_ps;   /* buffer 0 of the pixel stage */
typedef struct {                                /* what the pixel stage's text depends on */
    uint8_t n, specular;
    struct { uint8_t cop, ca0, ca1, ca2, aop, aa0, aa1, aa2, result, kind, project, pad; } s[8];
} ff_pskey;
const char *ff_vertex_text(void);               /* one vertex stage, steered by ff_vs; inputs v<D3DVSDE_*> */
char *ff_pixel_text(const ff_pskey *k);         /* malloc'd */
const char *ff_vertex_text_glsl(void);          /* GLSL 4.50 fixed-function vertex stage */
char *ff_pixel_text_glsl(const ff_pskey *k);    /* GLSL 4.50 fixed-function pixel stage (malloc'd) */
void ff_vs_fill(const dev_t_ *d, const elem_t *e, int ne, ff_vs *u);
void ff_ps_fill(const dev_t_ *d, ff_ps *u);
void ff_ps_key(const dev_t_ *d, const uint8_t kinds[8], ff_pskey *k);     /* kinds: DXSO_TEX_*, 0xFF none bound */
void ff_params(const dev_t_ *d, int programmable_vs, int rhw, float vw, float vh, dxso_params *p);
int  ff_projected(const dev_t_ *d, int stage);  /* 0, or the component D3DTTFF_PROJECTED divides by: 3 or 4 */

/* ---- the GPU (d3d_metal.m) ---- */
int  gpu_available(void);
void gpu_device(dev_t_ *d);                     /* after CreateDevice and Reset */
void gpu_clear(dev_t_ *d, uint32_t nrects, uint32_t rects, uint32_t flags, uint32_t color, float z, uint32_t stencil);
void gpu_draw(dev_t_ *d, uint32_t prim, uint32_t count, uint32_t start, int indexed, int32_t base_vertex,
              uint32_t min_index, uint32_t num_vertices, uint32_t up_va, uint32_t up_stride,
              uint32_t up_idx_va, uint32_t up_idx_format);
void gpu_present(dev_t_ *d);
int  gpu_stretch_rect(dev_t_ *d, uint32_t src, const int32_t *sr, uint32_t dst, const int32_t *dr);   /* 1 if it could */
void gpu_readback(dev_t_ *d, res_t *r, uint32_t level, uint32_t face);   /* a render target's pixels to its memory */
int  gpu_pixels(dev_t_ *d, uint32_t surface, uint8_t *bgra, uint32_t *w, uint32_t *h);   /* NULL bgra: the size */
int  gpu_is_render_target(const res_t *r);
void gpu_free(res_t *r);
int  gpu_compile(const char *text, const char *what);                   /* selftest: 1 if Metal takes it */
#endif
