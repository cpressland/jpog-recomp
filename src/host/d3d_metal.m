/*
 * d3d_metal.m - Direct3D 8 on Metal: the GPU side of d3d8.c. From
 * stl-recomp's Direct3D 9 (d3d9_metal.m), with one render target, vertex
 * layouts by register, and fixed function (ff.c) where no shader is set.
 *
 * d3d8.c keeps the device's state; at each draw this turns it into Metal's:
 * a render pass for the render target set (ended and begun again when it
 * changes; a full clear is the pass's load action), a pipeline state for the
 * shaders, the layout, the targets' formats and the blend state (cached by
 * all of them), a depth-stencil and sampler states (cached), and the raster
 * state set on the encoder.
 *
 * Memory: textures are Metal textures, filled from their guest memory when
 * a level was written (Lock marks it); render targets live on the GPU and
 * come back on a lock or a copy. Static vertex and index buffers get a Metal
 * buffer of their own, refilled when locked; dynamic ones, and the vertices
 * of the *UP draws, are copied into this frame's ring, a buffer per frame
 * in flight.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32hle.h"
#include "jpog.h"
#include <pthread.h>
#include <stdatomic.h>

#include "d3d_int.h"

/* Each frame's ring of dynamic data: 64 MB (JPOG_RING_MB for a test). */
static uint32_t ring_size(void) {
    static uint32_t n;
    if (!n) { const char *e = getenv("JPOG_RING_MB"); n = (e && atoi(e) > 0 ? (uint32_t)atoi(e) : 64u) << 20; }
    return n;
}
#define RING_SIZE ring_size()
#define FRAMES 3

@interface JPOGGpu : NSObject {
@public
    id<MTLDevice> dev;
    id<MTLCommandQueue> queue;
    id<MTLCommandBuffer> cmd, last;            /* being filled; the last committed */
    id<MTLRenderCommandEncoder> enc;
    uint32_t pass_rt, pass_ds;              /* the surfaces the open pass renders to */
    int pass_w, pass_h;
    id<MTLBuffer> ring[FRAMES];
    uint32_t ring_used;
    id<MTLBuffer> over;                        /* this frame's ring was full: where it goes on */
    uint32_t over_used;
    NSMutableArray<id<MTLBuffer>> *extras;     /* ...every such buffer, kept until the GPU is done with the frame */
    int ring_i;
    dispatch_semaphore_t inflight;
    uint64_t frame, gen;                       /* gen: the ring restarted (a frame, or full) */
    NSMutableDictionary<NSData *, id<MTLRenderPipelineState>> *pipelines;
    NSMutableDictionary<NSData *, id<MTLDepthStencilState>> *depths;
    NSMutableDictionary<NSData *, id<MTLSamplerState>> *samplers;
    NSMutableDictionary<NSNumber *, id<MTLRenderPipelineState>> *blits;
    NSMutableDictionary<NSData *, id<MTLRenderPipelineState>> *clears;
    id<MTLLibrary> blit_lib;
    id<MTLBuffer> zeros;
    /* pending clear, for the next pass */
    uint32_t clear_flags, clear_color, clear_stencil;
    float clear_z;
    uint64_t skipped, draws;
    NSMutableDictionary<NSData *, id<MTLFunction>> *ff_pixel;
    id<MTLFunction> ff_vertex;
    id<MTLTexture> black;                      /* what a shader samples at a stage with nothing bound */
}
@end
@implementation JPOGGpu
@end

static JPOGGpu *G;

/* JPOG_TRACE_FRAME=N: every draw, clear and StretchRect of frame N, said. */
static int tracing(void) {
    static long want = -2;
    if (want == -2) { const char *e = getenv("JPOG_TRACE_FRAME"); want = e ? atol(e) : -1; }
    return G && want >= 0 && (long)G->frame == want;
}
#define TRACE(...) do { if (tracing()) fprintf(stderr, "[trace] " __VA_ARGS__); } while (0)

int gpu_available(void) { return MTLCreateSystemDefaultDevice() != nil; }

/* ---- formats -------------------------------------------------------------- */
enum { CV_NONE, CV_565, CV_1555, CV_X1555, CV_4444, CV_888 };
typedef struct { MTLPixelFormat pf; int convert; MTLTextureSwizzleChannels swz; int has_swz; } fmt_t;

#define FOURCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
static MTLTextureSwizzleChannels SW(MTLTextureSwizzle r, MTLTextureSwizzle g, MTLTextureSwizzle b, MTLTextureSwizzle a) {
    return MTLTextureSwizzleChannelsMake(r, g, b, a);
}
static fmt_t fmt(uint32_t f) {
    fmt_t x = { MTLPixelFormatBGRA8Unorm, CV_NONE, { 0 }, 0 };
    switch (f) {
    case 21: break;                                                          /* A8R8G8B8 */
    case 22: x.has_swz = 1, x.swz = SW(MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleOne); break;
    case 20: x.convert = CV_888, x.has_swz = 1, x.swz = SW(MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleOne); break;
    case 23: x.convert = CV_565; break;
    case 24: x.convert = CV_X1555; break;
    case 25: x.convert = CV_1555; break;
    case 26: x.convert = CV_4444; break;
    case 28: x.pf = MTLPixelFormatA8Unorm; break;
    case 31: x.pf = MTLPixelFormatRGB10A2Unorm; break;                       /* A2B10G10R10 */
    case 32: x.pf = MTLPixelFormatRGBA8Unorm; break;                         /* A8B8G8R8 */
    case 33: x.pf = MTLPixelFormatRGBA8Unorm, x.has_swz = 1,
             x.swz = SW(MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleOne); break;
    case 34: x.pf = MTLPixelFormatRG16Unorm; break;
    case 35: x.pf = MTLPixelFormatBGR10A2Unorm; break;
    case 36: x.pf = MTLPixelFormatRGBA16Unorm; break;
    case 50: x.pf = MTLPixelFormatR8Unorm, x.has_swz = 1,                    /* L8 */
             x.swz = SW(MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleOne); break;
    case 51: x.pf = MTLPixelFormatRG8Unorm, x.has_swz = 1,                   /* A8L8 */
             x.swz = SW(MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleGreen); break;
    case 81: x.pf = MTLPixelFormatR16Unorm, x.has_swz = 1,                   /* L16 */
             x.swz = SW(MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleOne); break;
    case 60: x.pf = MTLPixelFormatRG8Snorm, x.has_swz = 1,                   /* V8U8: (u, v, 1, 1) */
             x.swz = SW(MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleOne, MTLTextureSwizzleOne); break;
    case 63: x.pf = MTLPixelFormatRGBA8Snorm; break;                         /* Q8W8V8U8 */
    case 111: x.pf = MTLPixelFormatR16Float; break;
    case 112: x.pf = MTLPixelFormatRG16Float; break;
    case 113: x.pf = MTLPixelFormatRGBA16Float; break;
    case 114: x.pf = MTLPixelFormatR32Float; break;
    case 115: x.pf = MTLPixelFormatRG32Float; break;
    case 116: x.pf = MTLPixelFormatRGBA32Float; break;
    case 75: case 73: case 79: x.pf = MTLPixelFormatDepth32Float_Stencil8; break;   /* D24S8, D15S1, D24X4S4 */
    case 71: case 77: case 80: case 82: x.pf = MTLPixelFormatDepth32Float; break;   /* D32, D24X8, D16, D24FS8 */
    case FOURCC('D', 'X', 'T', '1'): x.pf = MTLPixelFormatBC1_RGBA; break;
    case FOURCC('D', 'X', 'T', '2'): case FOURCC('D', 'X', 'T', '3'): x.pf = MTLPixelFormatBC2_RGBA; break;
    case FOURCC('D', 'X', 'T', '4'): case FOURCC('D', 'X', 'T', '5'): x.pf = MTLPixelFormatBC3_RGBA; break;
    }
    return x;
}
static int is_depth_pf(MTLPixelFormat pf) { return pf == MTLPixelFormatDepth32Float || pf == MTLPixelFormatDepth32Float_Stencil8; }

int gpu_is_render_target(const res_t *r) {
    const res_t *t = r->parent ? OBJ(r->parent, res_t) : r;
    return (t->usage & 3) && t->gpu != NULL;                 /* D3DUSAGE_RENDERTARGET, _DEPTHSTENCIL */
}

/* ---- textures -------------------------------------------------------------- */
static res_t *owner(res_t *r) { return r->parent ? OBJ(r->parent, res_t) : r; }

static id<MTLTexture> make_texture(res_t *r) {
    fmt_t f = fmt(r->format);
    MTLTextureDescriptor *td = [MTLTextureDescriptor new];
    td.pixelFormat = f.pf;
    td.width = r->width ? r->width : 1, td.height = r->height ? r->height : 1;
    td.mipmapLevelCount = r->levels ? r->levels : 1;
    td.textureType = r->kind == R_CUBETEXTURE ? MTLTextureTypeCube : r->kind == R_VOLUMETEXTURE ? MTLTextureType3D
                                                                                                : MTLTextureType2D;
    if (r->kind == R_VOLUMETEXTURE) td.depth = r->depth ? r->depth : 1;
    if (r->kind == R_CUBETEXTURE) td.height = td.width;
    td.usage = MTLTextureUsageShaderRead;
    if ((r->usage & 3) || r->kind == R_SURFACE) td.usage |= MTLTextureUsageRenderTarget;
    td.storageMode = is_depth_pf(f.pf) ? MTLStorageModePrivate : MTLStorageModeShared;
    if (f.has_swz && !(td.usage & MTLTextureUsageRenderTarget)) td.swizzle = f.swz;   /* not on a render target */
    id<MTLTexture> t = [G->dev newTextureWithDescriptor:td];
    if (!t) fprintf(stderr, "[metal] cannot make a %ux%u texture of format %u\n", r->width, r->height, r->format);
    return t;
}

static id<MTLTexture> texture_of(res_t *r) {
    res_t *o = owner(r);
    if (!o->gpu) {
        id<MTLTexture> t = make_texture(o);
        if (!t) return nil;
        o->gpu = (__bridge_retained void *)t;
        /* everything already written, and the levels never written are zero */
        for (int f = 0; f < 6; f++) o->dirty[f] = 0;
        for (int f = 0; f < 6; f++) for (int l = 0; l < 16; l++) if (o->mem[(f * 16 + l) % 96]) o->dirty[f] |= 1u << l;
    }
    return (__bridge id<MTLTexture>)o->gpu;
}

static void convert_row(int cv, const uint8_t *src, uint8_t *dst, uint32_t w) {
    for (uint32_t x = 0; x < w; x++) {
        uint32_t r, g, b, a = 255;
        if (cv == CV_888) { b = src[3 * x], g = src[3 * x + 1], r = src[3 * x + 2]; }
        else {
            uint16_t v = ((const uint16_t *)src)[x];
            if (cv == CV_565) r = (v >> 11) * 255 / 31, g = ((v >> 5) & 63) * 255 / 63, b = (v & 31) * 255 / 31;
            else if (cv == CV_4444) a = (v >> 12) * 17, r = ((v >> 8) & 15) * 17, g = ((v >> 4) & 15) * 17, b = (v & 15) * 17;
            else {
                r = ((v >> 10) & 31) * 255 / 31, g = ((v >> 5) & 31) * 255 / 31, b = (v & 31) * 255 / 31;
                a = cv == CV_1555 ? (v >> 15 ? 255 : 0) : 255;
            }
        }
        dst[4 * x] = (uint8_t)b, dst[4 * x + 1] = (uint8_t)g, dst[4 * x + 2] = (uint8_t)r, dst[4 * x + 3] = (uint8_t)a;
    }
}

/* The levels written since they went up, up. */
static void sync_texture(res_t *r) {
    res_t *o = owner(r);
    id<MTLTexture> t = texture_of(o);
    if (!t || is_depth_pf(t.pixelFormat)) { for (int f = 0; f < 6; f++) o->dirty[f] = 0; return; }
    fmt_t fm = fmt(o->format);
    int faces = o->kind == R_CUBETEXTURE ? 6 : 1;
    for (int f = 0; f < faces; f++) {
        uint32_t bits = o->dirty[f];
        o->dirty[f] = 0;
        for (uint32_t l = 0; bits && l < o->levels; l++, bits >>= 1) {
            if (!(bits & 1) || !o->mem[(f * 16 + l) % 96]) continue;
            uint32_t pitch, slice, mem = level_mem(o, l, (uint32_t)f, &pitch, &slice);
            uint32_t w = level_dim(o->width, l), h = level_dim(o->kind == R_CUBETEXTURE ? o->width : o->height, l);
            uint32_t d = o->kind == R_VOLUMETEXTURE ? level_dim(o->depth, l) : 1;
            const uint8_t *src = (const uint8_t *)hle_ptr(mem);
            MTLRegion rg = o->kind == R_VOLUMETEXTURE ? MTLRegionMake3D(0, 0, 0, w, h, d) : MTLRegionMake2D(0, 0, w, h);
            if (fm.convert) {
                uint8_t *tmp = malloc((size_t)w * 4 * h * d);
                for (uint32_t z = 0; z < d; z++)
                    for (uint32_t y = 0; y < h; y++)
                        convert_row(fm.convert, src + z * slice + y * pitch, tmp + ((size_t)z * h + y) * w * 4, w);
                [t replaceRegion:rg mipmapLevel:l slice:(NSUInteger)f withBytes:tmp bytesPerRow:w * 4 bytesPerImage:(NSUInteger)w * 4 * h];
                free(tmp);
            } else {
                [t replaceRegion:rg mipmapLevel:l slice:(NSUInteger)f withBytes:src bytesPerRow:pitch bytesPerImage:slice];
            }
        }
    }
}

/* ---- command buffers and the ring ---------------------------------------- */
static void end_encoder(void) {
    if (G->enc) { [G->enc endEncoding]; G->enc = nil; }
    G->pass_rt = G->pass_ds = 0;
}
static id<MTLCommandBuffer> command(void) {
    if (!G->cmd) G->cmd = [G->queue commandBuffer];
    return G->cmd;
}
/* Everything so far to the GPU, and wait for it (a readback, a shot). */
static void flush_wait(void) {
    end_encoder();
    if (G->cmd) { G->last = G->cmd; [G->cmd commit]; G->cmd = nil; }
    [G->last waitUntilCompleted];                       /* just after a Present: the frames still in flight */
    G->last = nil;
}

/* n bytes of this frame's (256-byte aligned): the buffer, and the offset.
 *
 * A frame that fills its ring goes on in a buffer of its own, kept until the
 * GPU has done with the frame (gpu_present). It used to submit the frame so
 * far and start the ring again, which ended the render encoder a draw was in
 * the middle of: the next call on it crashed (a heavy scene, ten minutes in). */
static uint32_t ring_alloc(uint32_t n, id<MTLBuffer> *buf) {
    n = (n + 255) & ~255u;
    uint32_t at;
    if (!G->over && G->ring_used + n <= RING_SIZE) {
        at = G->ring_used, G->ring_used += n, *buf = G->ring[G->ring_i];
        return at;
    }
    if (G->over && G->over_used + n <= G->over.length) {
        at = G->over_used, G->over_used += n, *buf = G->over;
        return at;
    }
    NSUInteger len = n > RING_SIZE ? n : RING_SIZE;
    id<MTLBuffer> b = [G->dev newBufferWithLength:len options:MTLResourceStorageModeShared];
    if (!b) { *buf = nil; return 0; }
    static int said;
    if (!said++) fprintf(stderr, "[metal] a frame needed more than its %u MB ring: it goes on in another\n", RING_SIZE >> 20);
    if (!G->extras) G->extras = [NSMutableArray new];
    [G->extras addObject:b];
    G->over = b, G->over_used = n, *buf = b;
    return 0;
}
static uint32_t ring_copy(const void *src, uint32_t n, id<MTLBuffer> *buf) {
    uint32_t at = ring_alloc(n, buf);
    if (*buf) memcpy((uint8_t *)[*buf contents] + at, src, n);
    return at;
}

/* A vertex or index buffer's bytes on the GPU: its buffer and offset. */
static id<MTLBuffer> buffer_of(res_t *r, uint32_t *offset) {
    *offset = 0;
    if (!(r->usage & 0x200)) {                              /* static: its own buffer, refilled when locked */
        if (!r->gpu) {
            id<MTLBuffer> b = [G->dev newBufferWithLength:r->size ? r->size : 16 options:MTLResourceStorageModeShared];
            r->gpu = (__bridge_retained void *)b;
            r->dirty[0] = 1;
        }
        id<MTLBuffer> b = (__bridge id<MTLBuffer>)r->gpu;
        if (r->dirty[0]) { memcpy([b contents], hle_ptr(r->mem[0]), r->size); r->dirty[0] = 0; }
        return b;
    }
    id<MTLBuffer> b;                                        /* dynamic: copied into the ring when it changed */
    if (r->dirty[0] || r->gpu_frame != G->gen + 1 || !r->gpu_ring) {
        r->gpu_offset = ring_copy(hle_ptr(r->mem[0]), r->size, &b);
        r->gpu_ring = (__bridge void *)b;                   /* this frame's: the ring, or a buffer it went on in */
        r->gpu_frame = G->gen + 1;
        r->dirty[0] = 0;
    }
    *offset = r->gpu_offset;
    return (__bridge id<MTLBuffer>)r->gpu_ring;
}

/* ---- the device -------------------------------------------------------------- */
static NSString *const BLIT_MSL =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
     "struct V { float4 pos [[position]]; float2 uv; };\n"
     "vertex V blit_v(uint id [[vertex_id]], constant float4 *r [[buffer(0)]]) {\n"
     "    float2 c = float2(id & 1, id >> 1);\n"
     "    V o; o.pos = float4(mix(r[0].xy, r[0].zw, c), 0, 1); o.uv = mix(r[1].xy, r[1].zw, c); return o; }\n"
     "fragment float4 blit_f(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
     "    return t.sample(s, in.uv); }\n"
     "struct C { float4 pos [[position]]; };\n"
     "vertex C clear_v(uint id [[vertex_id]], constant float4 *r [[buffer(0)]]) {\n"
     "    float2 c = float2(id & 1, id >> 1);\n"
     "    C o; o.pos = float4(mix(r[0].xy, r[0].zw, c), r[1].x, 1); return o; }\n"
     "fragment float4 clear_f(C in [[stage_in]], constant float4 *r [[buffer(0)]]) { return r[2]; }\n";

void gpu_device(dev_t_ *d) {
    if (!G) {
        G = [JPOGGpu new];
        G->dev = MTLCreateSystemDefaultDevice();
        if (!G->dev) { fprintf(stderr, "[metal] no Metal device\n"); return; }
        G->queue = [G->dev newCommandQueue];
        for (int i = 0; i < FRAMES; i++) G->ring[i] = [G->dev newBufferWithLength:RING_SIZE options:MTLResourceStorageModeShared];
        G->inflight = dispatch_semaphore_create(FRAMES - 1);
        G->pipelines = [NSMutableDictionary new];
        G->depths = [NSMutableDictionary new];
        G->samplers = [NSMutableDictionary new];
        G->blits = [NSMutableDictionary new];
        G->clears = [NSMutableDictionary new];
        G->ff_pixel = [NSMutableDictionary new];
        G->zeros = [G->dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
        {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:1 height:1 mipmapped:NO];
            td.storageMode = MTLStorageModeShared;
            G->black = [G->dev newTextureWithDescriptor:td];
            uint32_t px = 0xFF000000u;
            [G->black replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&px bytesPerRow:4];
        }
        NSError *e = nil;
        G->blit_lib = [G->dev newLibraryWithSource:BLIT_MSL options:nil error:&e];
        if (!G->blit_lib) fprintf(stderr, "[metal] blit shaders: %s\n", e.localizedDescription.UTF8String);
        fprintf(stderr, "[metal] %s\n", G->dev.name.UTF8String);
    }
    end_encoder();
    /* the back buffer and its depth buffer: textures now, at their size */
    res_t *bb = OBJ(d->backbuffer, res_t);
    if (bb->gpu) { CFBridgingRelease(bb->gpu); bb->gpu = NULL; }
    texture_of(bb);
    if (d->depth) {
        res_t *z = OBJ(d->depth, res_t);
        z->width = bb->width, z->height = bb->height;
        if (z->gpu) { CFBridgingRelease(z->gpu); z->gpu = NULL; }
        texture_of(z);
    }
}

/* ---- render passes ---------------------------------------------------------------- */
static MTLClearColor argb(uint32_t c) {
    return MTLClearColorMake(((c >> 16) & 255) / 255.0, ((c >> 8) & 255) / 255.0, (c & 255) / 255.0, (c >> 24) / 255.0);
}

/* The open pass, for the render target now set (begun again if it changed). */
static id<MTLRenderCommandEncoder> pass(dev_t_ *d) {
    if (G->enc && G->pass_rt == d->rt && G->pass_ds == d->ds && !G->clear_flags) return G->enc;
    end_encoder();
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    int w = 0, h = 0;
    if (d->rt) {
        res_t *s = OBJ(d->rt, res_t);
        id<MTLTexture> t = texture_of(s);
        if (t) {
            sync_texture(s);
            rp.colorAttachments[0].texture = t;
            rp.colorAttachments[0].level = s->parent ? s->level : 0;
            rp.colorAttachments[0].slice = s->parent ? s->face : 0;
            rp.colorAttachments[0].loadAction = (G->clear_flags & 1) ? MTLLoadActionClear : MTLLoadActionLoad;
            rp.colorAttachments[0].clearColor = argb(G->clear_color);
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            w = (int)level_dim((uint32_t)t.width, s->parent ? s->level : 0), h = (int)level_dim((uint32_t)t.height, s->parent ? s->level : 0);
        }
    }
    if (d->ds) {
        res_t *z = OBJ(d->ds, res_t);
        id<MTLTexture> t = texture_of(z);
        if (t && is_depth_pf(t.pixelFormat)) {
            rp.depthAttachment.texture = t;
            rp.depthAttachment.loadAction = (G->clear_flags & 2) ? MTLLoadActionClear : MTLLoadActionLoad;
            rp.depthAttachment.clearDepth = G->clear_z;
            rp.depthAttachment.storeAction = MTLStoreActionStore;
            if (t.pixelFormat == MTLPixelFormatDepth32Float_Stencil8) {
                rp.stencilAttachment.texture = t;
                rp.stencilAttachment.loadAction = (G->clear_flags & 4) ? MTLLoadActionClear : MTLLoadActionLoad;
                rp.stencilAttachment.clearStencil = G->clear_stencil;
                rp.stencilAttachment.storeAction = MTLStoreActionStore;
            }
            /* a pass is as large as its smaller target */
            if (!w || (int)t.width < w) w = (int)t.width;
            if (!h || (int)t.height < h) h = (int)t.height;
        }
    }
    G->clear_flags = 0;
    G->enc = [command() renderCommandEncoderWithDescriptor:rp];
    G->pass_rt = d->rt, G->pass_ds = d->ds;
    G->pass_w = w, G->pass_h = h;
    return G->enc;
}

/* The formats the open pass draws to. */
static MTLPixelFormat color_pf(dev_t_ *d) {
    id<MTLTexture> t = d->rt ? texture_of(OBJ(d->rt, res_t)) : nil;
    return t ? t.pixelFormat : MTLPixelFormatInvalid;
}
static MTLPixelFormat depth_pf(dev_t_ *d) {
    id<MTLTexture> t = d->ds ? texture_of(OBJ(d->ds, res_t)) : nil;
    return t && is_depth_pf(t.pixelFormat) ? t.pixelFormat : MTLPixelFormatInvalid;
}

/* A clear of part of the targets: a quad over each rectangle, writing what
 * the flags say (colour; depth by its z; stencil by replacing it). */
static void clear_quads(dev_t_ *d, const int32_t *rects, uint32_t n, uint32_t flags, uint32_t color, float z, uint32_t stencil) {
    id<MTLRenderCommandEncoder> enc = pass(d);
    MTLPixelFormat cpf = color_pf(d), dpf = depth_pf(d);
    uint32_t key[3] = { (uint32_t)cpf, (uint32_t)dpf, flags & 1 };
    NSData *k = [NSData dataWithBytes:key length:sizeof key];
    id<MTLRenderPipelineState> pl = G->clears[k];
    if (!pl) {
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [G->blit_lib newFunctionWithName:@"clear_v"];
        pd.fragmentFunction = [G->blit_lib newFunctionWithName:@"clear_f"];
        if (cpf != MTLPixelFormatInvalid) {
            pd.colorAttachments[0].pixelFormat = cpf;
            pd.colorAttachments[0].writeMask = (flags & 1) ? MTLColorWriteMaskAll : MTLColorWriteMaskNone;
        }
        if (dpf != MTLPixelFormatInvalid) pd.depthAttachmentPixelFormat = dpf;
        if (dpf == MTLPixelFormatDepth32Float_Stencil8) pd.stencilAttachmentPixelFormat = dpf;
        NSError *e = nil;
        pl = [G->dev newRenderPipelineStateWithDescriptor:pd error:&e];
        if (!pl) { fprintf(stderr, "[metal] clear pipeline: %s\n", e.localizedDescription.UTF8String); return; }
        G->clears[k] = pl;
    }
    MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
    dd.depthCompareFunction = MTLCompareFunctionAlways;
    dd.depthWriteEnabled = (flags & 2) != 0;
    if (flags & 4) {
        MTLStencilDescriptor *sd = [MTLStencilDescriptor new];
        sd.stencilCompareFunction = MTLCompareFunctionAlways;
        sd.depthStencilPassOperation = MTLStencilOperationReplace;
        dd.frontFaceStencil = sd, dd.backFaceStencil = sd;
    }
    [enc setRenderPipelineState:pl];
    [enc setDepthStencilState:[G->dev newDepthStencilStateWithDescriptor:dd]];
    [enc setStencilReferenceValue:stencil];
    [enc setCullMode:MTLCullModeNone];
    [enc setTriangleFillMode:MTLTriangleFillModeFill];
    [enc setViewport:(MTLViewport){ 0, 0, (double)G->pass_w, (double)G->pass_h, 0, 1 }];
    [enc setScissorRect:(MTLScissorRect){ 0, 0, (NSUInteger)G->pass_w, (NSUInteger)G->pass_h }];
    float w = (float)G->pass_w, h = (float)G->pass_h;
    for (uint32_t i = 0; i < n; i++) {
        const int32_t *r = rects + 4 * i;
        if (r[2] <= r[0] || r[3] <= r[1]) continue;
        float c[12] = { r[0] / w * 2 - 1, 1 - r[1] / h * 2, r[2] / w * 2 - 1, 1 - r[3] / h * 2, z, 0, 0, 0,
                        ((color >> 16) & 255) / 255.0f, ((color >> 8) & 255) / 255.0f, (color & 255) / 255.0f, (color >> 24) / 255.0f };
        [enc setVertexBytes:c length:sizeof c atIndex:0];
        [enc setFragmentBytes:c length:sizeof c atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    }
}

/* Clear(count, rects, flags, color, z, stencil): of the whole target, the
 * pass's load action; of part (the viewport, the scissor, rectangles), quads. */
void gpu_clear(dev_t_ *d, uint32_t nrects, uint32_t rects, uint32_t flags, uint32_t color, float z, uint32_t stencil) {
    if (!G) return;
    TRACE("Clear flags %X color %08X z %g rt %08X ds %08X vp %u,%u %ux%u rects %u\n", flags, color, z, d->rt, d->ds,
          d->viewport[0], d->viewport[1], d->viewport[2], d->viewport[3], nrects);
    /* the target's size */
    int tw = 0, th = 0;
    uint32_t first = d->rt ? d->rt : d->ds;
    if (first) {
        res_t *s = OBJ(first, res_t);
        id<MTLTexture> t = texture_of(s);
        if (t) tw = (int)level_dim((uint32_t)t.width, s->parent ? s->level : 0), th = (int)level_dim((uint32_t)t.height, s->parent ? s->level : 0);
    }
    /* what is cleared: the viewport, within the scissor if it is on, each rectangle within that */
    int32_t box[4] = { (int32_t)d->viewport[0], (int32_t)d->viewport[1], (int32_t)(d->viewport[0] + d->viewport[2]),
                       (int32_t)(d->viewport[1] + d->viewport[3]) };
    if (box[0] < 0) box[0] = 0;
    if (box[1] < 0) box[1] = 0;
    if (box[2] > tw) box[2] = tw;
    if (box[3] > th) box[3] = th;
    if (!nrects && box[0] == 0 && box[1] == 0 && box[2] >= tw && box[3] >= th) {
        end_encoder();
        G->clear_flags = flags & 7, G->clear_color = color, G->clear_z = z, G->clear_stencil = stencil;
        pass(d);                                            /* the pass that clears */
        return;
    }
    uint32_t n = nrects ? nrects : 1;
    int32_t *r = malloc(16 * n);
    for (uint32_t i = 0; i < n; i++) {
        int32_t *q = r + 4 * i;
        if (nrects) for (int k = 0; k < 4; k++) q[k] = (int32_t)MEM32(rects + 16 * i + 4 * k);
        else memcpy(q, box, 16);
        if (q[0] < box[0]) q[0] = box[0];
        if (q[1] < box[1]) q[1] = box[1];
        if (q[2] > box[2]) q[2] = box[2];
        if (q[3] > box[3]) q[3] = box[3];
    }
    clear_quads(d, r, n, flags & 7, color, z, stencil);
    free(r);
}

/* ---- shaders ------------------------------------------------------------------------ */
static id<MTLFunction> compile(const char *text, const char *what) {
    NSError *e = nil;
    id<MTLLibrary> lib = [G->dev newLibraryWithSource:[NSString stringWithUTF8String:text] options:nil error:&e];
    id<MTLFunction> f = [lib newFunctionWithName:@"dxso_main"];
    if (!f) fprintf(stderr, "[metal] %s does not compile: %s\n", what, e.localizedDescription.UTF8String);
    return f;
}
int gpu_compile(const char *text, const char *what) { return compile(text, what) != nil; }

/* What is bound at a stage, as a shader samples it; 0xFF: nothing. */
static uint8_t texture_kind(dev_t_ *d, int s) {
    uint32_t t = d->tex[s];
    if (!t || !REC(t)) return 0xFF;
    uint32_t k = OBJ(t, res_t)->kind;
    return k == R_CUBETEXTURE ? DXSO_TEX_CUBE : k == R_VOLUMETEXTURE ? DXSO_TEX_3D : DXSO_TEX_2D;
}

/* A shader's function. A pixel shader's variant: each stage's kind of
 * texture and projection, as ps_1_x declares neither. */
static id<MTLFunction> shader(dev_t_ *d, shader_t *b) {
    if (b->failed) return nil;
    uint32_t key = 0;
    dxso_options opt = { { 0 }, 0, { 0 } };
    if (b->pixel)
        for (int s = 0; s < 4; s++) {
            uint8_t k = texture_kind(d, s);
            opt.sampler_type[s] = k == 0xFF ? DXSO_TEX_2D : k;
            opt.project[s] = (uint8_t)ff_projected(d, s);
            key |= (uint32_t)opt.sampler_type[s] << (2 * s) | (uint32_t)(opt.project[s] ? opt.project[s] - 2 : 0) << (8 + 2 * s);
        }
    for (int i = 0; i < 8; i++) if (b->fn[i] && b->variant_key[i] == key) return (__bridge id<MTLFunction>)b->fn[i];
    dxso_shader s;
    if (dxso_translate(b->code, b->len / 4, &opt, &s) != 0) {
        fprintf(stderr, "[metal] a %s shader is not translated: %s\n", b->pixel ? "pixel" : "vertex", s.error);
        b->failed = 1;
        return nil;
    }
    id<MTLFunction> f = compile(s.text, b->pixel ? "a pixel shader" : "a vertex shader");
    if (!f) { b->failed = 1; dxso_free(&s); return nil; }
    dxso_free(&s);
    b->info = s;
    int slot = 0;
    while (slot < 8 && b->fn[slot]) slot++;
    if (slot == 8) { CFBridgingRelease(b->fn[7]); slot = 7; }
    b->fn[slot] = (__bridge_retained void *)f;
    b->variant_key[slot] = key;
    return f;
}

static id<MTLFunction> ff_pixel_fn(const ff_pskey *k) {
    NSData *key = [NSData dataWithBytes:k length:sizeof *k];
    id<MTLFunction> f = G->ff_pixel[key];
    if (f) return f;
    char *text = ff_pixel_text(k);
    f = compile(text, "a fixed-function pixel stage");
    if (f) G->ff_pixel[key] = f;
    free(text);
    return f;
}

static MTLVertexFormat vformat(int type) {
    switch (type) {
    case 0: return MTLVertexFormatFloat;
    case 1: return MTLVertexFormatFloat2;
    case 2: return MTLVertexFormatFloat3;
    case 3: return MTLVertexFormatFloat4;
    case 4: return MTLVertexFormatUChar4Normalized_BGRA;     /* D3DCOLOR */
    case 5: return MTLVertexFormatUChar4;
    case 6: return MTLVertexFormatShort2;
    case 7: return MTLVertexFormatShort4;
    case 8: return MTLVertexFormatUChar4Normalized;
    case 9: return MTLVertexFormatShort2Normalized;
    case 10: return MTLVertexFormatShort4Normalized;
    case 11: return MTLVertexFormatUShort2Normalized;
    case 12: return MTLVertexFormatUShort4Normalized;
    case 14: return MTLVertexFormatInt1010102Normalized;
    case 15: return MTLVertexFormatHalf2;
    case 16: return MTLVertexFormatHalf4;
    }
    return MTLVertexFormatFloat4;
}

/* ---- pipeline state ---------------------------------------------------------------- */
static const MTLBlendFactor BF[16] = {
    MTLBlendFactorZero, MTLBlendFactorZero, MTLBlendFactorOne, MTLBlendFactorSourceColor, MTLBlendFactorOneMinusSourceColor,
    MTLBlendFactorSourceAlpha, MTLBlendFactorOneMinusSourceAlpha, MTLBlendFactorDestinationAlpha,
    MTLBlendFactorOneMinusDestinationAlpha, MTLBlendFactorDestinationColor, MTLBlendFactorOneMinusDestinationColor,
    MTLBlendFactorSourceAlphaSaturated, MTLBlendFactorSourceAlpha, MTLBlendFactorOneMinusSourceAlpha,
    MTLBlendFactorBlendColor, MTLBlendFactorOneMinusBlendColor };
static MTLBlendFactor bf(uint32_t b) { return b < 16 ? BF[b] : MTLBlendFactorOne; }
static MTLBlendOperation bop(uint32_t o) {
    switch (o) { case 2: return MTLBlendOperationSubtract; case 3: return MTLBlendOperationReverseSubtract;
                 case 4: return MTLBlendOperationMin; case 5: return MTLBlendOperationMax; }
    return MTLBlendOperationAdd;
}

/* Fixed function's inputs: position, weights, normal, the colours, eight texture coordinates. */
#define FF_INPUTS 0x7FEBu

typedef struct {
    void *vs, *ps;
    elem_t e[MAX_ELEMS];
    uint32_t ne, strides[16], inputs;
    uint32_t color, depth;
    uint32_t blend, src, dst, op, mask;
} pkey_t;

static id<MTLRenderPipelineState> pipeline(dev_t_ *d, id<MTLFunction> vs, id<MTLFunction> ps, uint32_t inputs,
                                           const elem_t *e, int ne, const uint32_t *strides) {
    pkey_t k;
    memset(&k, 0, sizeof k);
    k.vs = (__bridge void *)vs, k.ps = (__bridge void *)ps;
    k.ne = (uint32_t)ne, k.inputs = inputs;
    memcpy(k.e, e, (size_t)ne * sizeof *e);
    memcpy(k.strides, strides, sizeof k.strides);
    k.color = (uint32_t)color_pf(d), k.depth = (uint32_t)depth_pf(d);
    k.blend = d->rs[27] != 0;
    if (k.blend) k.src = d->rs[19], k.dst = d->rs[20], k.op = d->rs[171];
    k.mask = d->rs[168] & 15;
    NSData *key = [NSData dataWithBytes:&k length:sizeof k];
    id<MTLRenderPipelineState> st = G->pipelines[key];
    if (st) return st;

    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = vs, pd.fragmentFunction = ps;
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    int need_zero = 0;
    for (int r = 0; r < DXSO_MAX_INPUTS; r++) {
        if (!((inputs >> r) & 1)) continue;
        int found = -1;
        for (int i = 0; i < ne && found < 0; i++) if (e[i].reg == r && (strides[e[i].stream])) found = i;
        if (found < 0) {                                        /* not in the layout: zeros */
            vd.attributes[r].format = MTLVertexFormatFloat4, vd.attributes[r].offset = 0, vd.attributes[r].bufferIndex = 7;
            need_zero = 1;
            continue;
        }
        vd.attributes[r].format = vformat(e[found].type);
        vd.attributes[r].offset = e[found].offset;
        vd.attributes[r].bufferIndex = 8u + e[found].stream;
        vd.layouts[8 + e[found].stream].stride = strides[e[found].stream];
    }
    if (need_zero) {
        vd.layouts[7].stride = 16;
        vd.layouts[7].stepFunction = MTLVertexStepFunctionConstant;
        vd.layouts[7].stepRate = 0;
    }
    pd.vertexDescriptor = vd;
    if (k.color) {
        MTLRenderPipelineColorAttachmentDescriptor *c = pd.colorAttachments[0];
        c.pixelFormat = (MTLPixelFormat)k.color;
        c.writeMask = (k.mask & 1 ? MTLColorWriteMaskRed : 0) | (k.mask & 2 ? MTLColorWriteMaskGreen : 0) |
                      (k.mask & 4 ? MTLColorWriteMaskBlue : 0) | (k.mask & 8 ? MTLColorWriteMaskAlpha : 0);
        if (k.blend) {
            c.blendingEnabled = YES;
            MTLBlendFactor s = bf(k.src), dd = bf(k.dst);
            if (k.src == 12) s = MTLBlendFactorSourceAlpha, dd = MTLBlendFactorOneMinusSourceAlpha;     /* BOTHSRCALPHA */
            if (k.src == 13) s = MTLBlendFactorOneMinusSourceAlpha, dd = MTLBlendFactorSourceAlpha;     /* BOTHINVSRCALPHA */
            c.sourceRGBBlendFactor = s, c.destinationRGBBlendFactor = dd, c.rgbBlendOperation = bop(k.op);
            c.sourceAlphaBlendFactor = s, c.destinationAlphaBlendFactor = dd, c.alphaBlendOperation = bop(k.op);
        }
    }
    if (k.depth) pd.depthAttachmentPixelFormat = (MTLPixelFormat)k.depth;
    if (k.depth == MTLPixelFormatDepth32Float_Stencil8) pd.stencilAttachmentPixelFormat = (MTLPixelFormat)k.depth;
    NSError *err = nil;
    st = [G->dev newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!st) {
        static int told;
        if (told++ < 10) fprintf(stderr, "[metal] pipeline: %s\n", err.localizedDescription.UTF8String);
        return nil;
    }
    G->pipelines[key] = st;
    return st;
}

static MTLCompareFunction cmpf(uint32_t c) {
    switch (c) { case 1: return MTLCompareFunctionNever; case 2: return MTLCompareFunctionLess;
                 case 3: return MTLCompareFunctionEqual; case 4: return MTLCompareFunctionLessEqual;
                 case 5: return MTLCompareFunctionGreater; case 6: return MTLCompareFunctionNotEqual;
                 case 7: return MTLCompareFunctionGreaterEqual; }
    return MTLCompareFunctionAlways;
}
static MTLStencilOperation sop(uint32_t o) {
    switch (o) { case 2: return MTLStencilOperationZero; case 3: return MTLStencilOperationReplace;
                 case 4: return MTLStencilOperationIncrementClamp; case 5: return MTLStencilOperationDecrementClamp;
                 case 6: return MTLStencilOperationInvert; case 7: return MTLStencilOperationIncrementWrap;
                 case 8: return MTLStencilOperationDecrementWrap; }
    return MTLStencilOperationKeep;
}

static id<MTLDepthStencilState> depth_state(dev_t_ *d) {
    uint32_t k[16] = { 0 };
    if (d->ds) {
        k[0] = d->rs[7] != 0, k[1] = d->rs[14] != 0, k[2] = d->rs[23];
        k[3] = d->rs[52] != 0 && depth_pf(d) == MTLPixelFormatDepth32Float_Stencil8;
        if (k[3]) {
            k[4] = d->rs[53], k[5] = d->rs[54], k[6] = d->rs[55], k[7] = d->rs[56], k[8] = d->rs[58], k[9] = d->rs[59];
        }
    }
    NSData *key = [NSData dataWithBytes:k length:sizeof k];
    id<MTLDepthStencilState> st = G->depths[key];
    if (st) return st;
    MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
    dd.depthCompareFunction = k[0] ? cmpf(k[2]) : MTLCompareFunctionAlways;
    dd.depthWriteEnabled = k[0] && k[1];
    if (k[3]) {
        MTLStencilDescriptor *f = [MTLStencilDescriptor new];
        f.stencilFailureOperation = sop(k[4]), f.depthFailureOperation = sop(k[5]), f.depthStencilPassOperation = sop(k[6]);
        f.stencilCompareFunction = cmpf(k[7]), f.readMask = k[8], f.writeMask = k[9];
        dd.frontFaceStencil = f;
        dd.backFaceStencil = f;
    }
    st = [G->dev newDepthStencilStateWithDescriptor:dd];
    G->depths[key] = st;
    return st;
}

static MTLSamplerAddressMode addr(uint32_t a) {
    switch (a) { case 2: return MTLSamplerAddressModeMirrorRepeat; case 3: return MTLSamplerAddressModeClampToEdge;
                 case 4: return MTLSamplerAddressModeClampToBorderColor; case 5: return MTLSamplerAddressModeMirrorClampToEdge; }
    return MTLSamplerAddressModeRepeat;
}
/* A stage's sampler, from its D3DTSS_*. Metal has no LOD bias on a sampler:
 * a sharpening one (the sprites' -2) is a wider anisotropy's job; left out. */
static id<MTLSamplerState> sampler_state(dev_t_ *d, int s) {
    const uint32_t *t = d->tss[s];
    uint32_t k[9] = { t[13], t[14], t[25], t[15], t[16], t[17], t[18], t[20], t[21] };
    NSData *key = [NSData dataWithBytes:k length:sizeof k];
    id<MTLSamplerState> st = G->samplers[key];
    if (st) return st;
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.sAddressMode = addr(k[0]), sd.tAddressMode = addr(k[1]), sd.rAddressMode = addr(k[2]);
    uint32_t border = k[3];
    sd.borderColor = (border >> 24) < 128 ? MTLSamplerBorderColorTransparentBlack
                   : (border & 0xFFFFFF) > 0x7F7F7F ? MTLSamplerBorderColorOpaqueWhite : MTLSamplerBorderColorOpaqueBlack;
    sd.magFilter = k[4] >= 2 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    sd.minFilter = k[5] >= 2 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    sd.mipFilter = k[6] == 0 ? MTLSamplerMipFilterNotMipmapped : k[6] == 1 ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear;
    if (k[7]) sd.lodMinClamp = (float)k[7];                  /* MAXMIPLEVEL: the largest level used */
    if (k[4] == 3 || k[5] == 3) sd.maxAnisotropy = k[8] > 1 ? (k[8] > 16 ? 16 : k[8]) : 1;
    st = [G->dev newSamplerStateWithDescriptor:sd];
    G->samplers[key] = st;
    return st;
}

/* ---- draws -------------------------------------------------------------------------- */
static uint32_t vertices_of(uint32_t prim, uint32_t count) {
    switch (prim) {
    case 1: return count;                                   /* points */
    case 2: return count * 2;                               /* lines */
    case 3: return count + 1;                               /* line strip */
    case 4: return count * 3;                               /* triangles */
    case 5: case 6: return count + 2;                       /* strip, fan */
    }
    return 0;
}
static MTLPrimitiveType mprim(uint32_t prim) {
    switch (prim) {
    case 1: return MTLPrimitiveTypePoint;
    case 2: return MTLPrimitiveTypeLine;
    case 3: return MTLPrimitiveTypeLineStrip;
    case 5: return MTLPrimitiveTypeTriangleStrip;
    }
    return MTLPrimitiveTypeTriangle;                        /* lists, and fans made lists */
}

void gpu_draw(dev_t_ *d, uint32_t prim, uint32_t count, uint32_t start, int indexed, int32_t base_vertex,
              uint32_t min_index, uint32_t num_vertices, uint32_t up_va, uint32_t up_stride,
              uint32_t up_idx_va, uint32_t up_idx_format) {
    if (!G || !count) return;
    G->draws++;
    TRACE("Draw prim %u count %u %s rt %08X vsh %08X psh %08X tex0 %08X up %u z %u/%u blend %u %u/%u cull %u vp %ux%u\n",
          prim, count, indexed ? "indexed" : "", d->rt, d->vsh, d->psh, d->tex[0], up_va != 0,
          d->rs[7], d->rs[14], d->rs[27], d->rs[19], d->rs[20], d->rs[22], d->viewport[2], d->viewport[3]);
    elem_t e[MAX_ELEMS];
    int ne = vertex_layout(d, e);
    shader_t *vsh = shader_at(d->vsh), *psh = shader_at(d->psh);
    if (vsh && !vsh->code) vsh = NULL;                      /* a declaration alone: fixed function */
    id<MTLFunction> vs, ps;
    uint32_t inputs = FF_INPUTS;
    ff_vs fv;
    int rhw = 0;
    if (vsh) {
        vs = shader(d, vsh);
        inputs = 0;
        for (int r = 0; r < DXSO_MAX_INPUTS; r++) if (vsh->info.input_usage[r] != 0xFF) inputs |= 1u << r;
    } else {
        if (!G->ff_vertex) G->ff_vertex = compile(ff_vertex_text(), "the fixed-function vertex stage");
        vs = G->ff_vertex;
        ff_vs_fill(d, e, ne, &fv);
        rhw = fv.rhw != 0;
    }
    uint8_t kinds[8];
    for (int s = 0; s < 8; s++) kinds[s] = texture_kind(d, s);
    uint32_t sampled = 0;
    if (psh) {
        ps = shader(d, psh);
        sampled = psh->info.samplers_used;
    } else {
        ff_pskey k;
        ff_ps_key(d, kinds, &k);
        ps = ff_pixel_fn(&k);
        for (int s = 0; s < k.n; s++) if (k.s[s].kind != 0xFF) sampled |= 1u << s;
    }
    if (!vs || !ps) { G->skipped++; return; }
    /* what it samples goes up before the pass is open */
    for (int s = 0; s < 8; s++) if (((sampled >> s) & 1) && kinds[s] != 0xFF) sync_texture(OBJ(d->tex[s], res_t));
    id<MTLRenderCommandEncoder> enc = pass(d);
    uint32_t strides[16] = { 0 };
    for (int i = 0; i < 16; i++) strides[i] = d->stream[i][0] && REC(d->stream[i][0]) ? d->stream[i][1] : 0;
    if (up_va) memset(strides, 0, sizeof strides), strides[0] = up_stride;
    id<MTLRenderPipelineState> pl = pipeline(d, vs, ps, inputs, e, ne, strides);
    if (!pl) { G->skipped++; return; }
    [enc setRenderPipelineState:pl];
    [enc setDepthStencilState:depth_state(d)];
    [enc setStencilReferenceValue:d->rs[57] & 255];
    [enc setFrontFacingWinding:MTLWindingClockwise];
    uint32_t cull = d->rs[22];
    [enc setCullMode:cull == 2 ? MTLCullModeFront : cull == 3 ? MTLCullModeBack : MTLCullModeNone];
    [enc setTriangleFillMode:d->rs[8] == 2 ? MTLTriangleFillModeLines : MTLTriangleFillModeFill];
    float vx = (float)d->viewport[0], vy = (float)d->viewport[1], vw = (float)d->viewport[2], vh = (float)d->viewport[3];
    float zn, zf;
    memcpy(&zn, &d->viewport[4], 4), memcpy(&zf, &d->viewport[5], 4);
    if (vw <= 0 || vh <= 0) vw = (float)G->pass_w, vh = (float)G->pass_h;
    if (rhw) zn = 0, zf = 1;                                /* a transformed vertex's z is the depth */
    [enc setViewport:(MTLViewport){ vx, vy, vw, vh, zn, zf }];
    int x0 = (int)vx, y0 = (int)vy, x1 = (int)(vx + vw), y1 = (int)(vy + vh);   /* drawn inside the viewport */
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > G->pass_w) x1 = G->pass_w;
    if (y1 > G->pass_h) y1 = G->pass_h;
    if (x1 <= x0 || y1 <= y0) return;
    [enc setScissorRect:(MTLScissorRect){ (NSUInteger)x0, (NSUInteger)y0, (NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0) }];
    /* constants and parameters */
    dxso_params P;
    ff_params(d, vsh != NULL, rhw, vw, vh, &P);
    if (vsh) [enc setVertexBytes:d->vsc length:sizeof d->vsc atIndex:0];
    else [enc setVertexBytes:&fv length:sizeof fv atIndex:0];
    [enc setVertexBytes:&P length:sizeof P atIndex:3];
    if (psh) [enc setFragmentBytes:d->psc length:sizeof d->psc atIndex:0];
    else { ff_ps fp; ff_ps_fill(d, &fp); [enc setFragmentBytes:&fp length:sizeof fp atIndex:0]; }
    [enc setFragmentBytes:&P length:sizeof P atIndex:3];
    /* shader models 2 and 3's: declared by every translated shader, read by none of Direct3D 8's */
    [enc setVertexBuffer:G->zeros offset:0 atIndex:1];
    [enc setVertexBuffer:G->zeros offset:0 atIndex:2];
    [enc setFragmentBuffer:G->zeros offset:0 atIndex:1];
    [enc setFragmentBuffer:G->zeros offset:0 atIndex:2];
    [enc setVertexBuffer:G->zeros offset:0 atIndex:7];
    for (int s = 0; s < 8; s++) {
        if (!((sampled >> s) & 1)) continue;
        id<MTLTexture> t = kinds[s] != 0xFF ? texture_of(OBJ(d->tex[s], res_t)) : G->black;
        if (!t) t = G->black;
        [enc setFragmentTexture:t atIndex:(NSUInteger)s];
        [enc setFragmentSamplerState:sampler_state(d, s) atIndex:(NSUInteger)s];
    }
    /* vertices */
    uint32_t nverts = indexed ? min_index + num_vertices : vertices_of(prim, count);
    if (up_va) {
        id<MTLBuffer> b;
        uint32_t at = ring_copy(hle_ptr(up_va), nverts * up_stride, &b);
        if (!b) return;
        [enc setVertexBuffer:b offset:at atIndex:8];
    } else {
        for (int i = 0; i < 16; i++) {
            if (!strides[i]) continue;
            uint32_t off;
            id<MTLBuffer> b = buffer_of(OBJ(d->stream[i][0], res_t), &off);
            [enc setVertexBuffer:b offset:off atIndex:8 + (NSUInteger)i];
        }
    }
    MTLPrimitiveType mp = mprim(prim);
    uint32_t n = vertices_of(prim, count);
    if (!indexed && prim != 6) {
        [enc drawPrimitives:mp vertexStart:start vertexCount:n];
        return;
    }
    /* indices: the draw's, or a fan's made a list */
    uint32_t isize = 2;
    const uint8_t *ip = NULL;
    if (indexed) {
        if (up_idx_va) { ip = (const uint8_t *)hle_ptr(up_idx_va); isize = up_idx_format == 102 ? 4 : 2; }
        else if (d->indices && REC(d->indices)) {
            res_t *ib = OBJ(d->indices, res_t);
            ip = (const uint8_t *)hle_ptr(ib->mem[0]);
            isize = ib->format == 102 ? 4 : 2;                /* D3DFMT_INDEX32 */
            ip += start * isize;
        } else return;
    }
    if (prim == 6) {                                        /* a fan: (0, i, i+1) */
        uint32_t tris = count, *list = malloc(tris * 3 * 4);
        for (uint32_t i = 0; i < tris; i++) {
            uint32_t a0 = 0, a1 = i + 1, a2 = i + 2;
            if (ip) {
                a0 = isize == 4 ? ((const uint32_t *)ip)[0] : ((const uint16_t *)ip)[0];
                a1 = isize == 4 ? ((const uint32_t *)ip)[i + 1] : ((const uint16_t *)ip)[i + 1];
                a2 = isize == 4 ? ((const uint32_t *)ip)[i + 2] : ((const uint16_t *)ip)[i + 2];
            } else a0 += start, a1 += start, a2 += start;
            list[3 * i] = a0, list[3 * i + 1] = a1, list[3 * i + 2] = a2;
        }
        id<MTLBuffer> b;
        uint32_t at = ring_copy(list, tris * 12, &b);
        free(list);
        if (!b) return;
        [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:tris * 3 indexType:MTLIndexTypeUInt32 indexBuffer:b
                 indexBufferOffset:at instanceCount:1 baseVertex:base_vertex baseInstance:0];
        return;
    }
    id<MTLBuffer> b;
    uint32_t at = ring_copy(ip, n * isize, &b);
    if (!b) return;
    [enc drawIndexedPrimitives:mp indexCount:n indexType:isize == 4 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16 indexBuffer:b
             indexBufferOffset:at instanceCount:1 baseVertex:base_vertex baseInstance:0];
}

/* ---- StretchRect, readback, present ------------------------------------------------ */
static id<MTLRenderPipelineState> blit_pipeline(MTLPixelFormat pf) {
    id<MTLRenderPipelineState> st = G->blits[@(pf)];
    if (st) return st;
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [G->blit_lib newFunctionWithName:@"blit_v"];
    pd.fragmentFunction = [G->blit_lib newFunctionWithName:@"blit_f"];
    pd.colorAttachments[0].pixelFormat = pf;
    NSError *e = nil;
    st = [G->dev newRenderPipelineStateWithDescriptor:pd error:&e];
    if (st) G->blits[@(pf)] = st;
    return st;
}

int gpu_stretch_rect(dev_t_ *d, uint32_t src, const int32_t *sr_, uint32_t dst, const int32_t *dr_) {
    if (!G || !src || !dst) return 0;
    TRACE("StretchRect %08X -> %08X\n", src, dst);
    res_t *s = OBJ(src, res_t), *t = OBJ(dst, res_t);
    id<MTLTexture> st = texture_of(s), dt = texture_of(t);
    if (!st || !dt || is_depth_pf(st.pixelFormat) || is_depth_pf(dt.pixelFormat) || !(dt.usage & MTLTextureUsageRenderTarget)) return 0;
    sync_texture(s), sync_texture(t);
    uint32_t sl = s->parent ? s->level : 0, dl = t->parent ? t->level : 0;
    float sw = (float)level_dim((uint32_t)st.width, sl), sh = (float)level_dim((uint32_t)st.height, sl);
    float dw = (float)level_dim((uint32_t)dt.width, dl), dh = (float)level_dim((uint32_t)dt.height, dl);
    float sr[4] = { 0, 0, sw, sh }, dr[4] = { 0, 0, dw, dh };
    if (sr_) for (int i = 0; i < 4; i++) sr[i] = (float)sr_[i];
    if (dr_) for (int i = 0; i < 4; i++) dr[i] = (float)dr_[i];
    end_encoder();
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = dt, rp.colorAttachments[0].level = dl, rp.colorAttachments[0].slice = t->parent ? t->face : 0;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad, rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> enc = [command() renderCommandEncoderWithDescriptor:rp];
    id<MTLRenderPipelineState> pl = blit_pipeline(dt.pixelFormat);
    if (pl) {
        float r[8] = { dr[0] / dw * 2 - 1, 1 - dr[1] / dh * 2, dr[2] / dw * 2 - 1, 1 - dr[3] / dh * 2,
                       sr[0] / sw, sr[1] / sh, sr[2] / sw, sr[3] / sh };
        [enc setRenderPipelineState:pl];
        [enc setVertexBytes:r length:sizeof r atIndex:0];
        id<MTLTexture> view = st;
        if (st.textureType != MTLTextureType2D || st.mipmapLevelCount > 1)
            view = [st newTextureViewWithPixelFormat:st.pixelFormat textureType:MTLTextureType2D
                                              levels:NSMakeRange(sl, 1) slices:NSMakeRange(s->parent ? s->face : 0, 1)];
        [enc setFragmentTexture:view atIndex:0];
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
        [enc setFragmentSamplerState:[G->dev newSamplerStateWithDescriptor:sd] atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    }
    [enc endEncoding];
    (void)d;
    return 1;
}

void gpu_readback(dev_t_ *d, res_t *r, uint32_t level, uint32_t face) {
    (void)d;
    if (!G) return;
    res_t *o = owner(r);
    if (!o->gpu) return;
    id<MTLTexture> t = (__bridge id<MTLTexture>)o->gpu;
    if (is_depth_pf(t.pixelFormat) || fmt(o->format).convert) return;   /* not read back (yet) */
    flush_wait();
    uint32_t pitch, slice, mem = level_mem(o, level, face, &pitch, &slice);
    uint32_t w = level_dim((uint32_t)t.width, level), h = level_dim((uint32_t)t.height, level);
    [t getBytes:hle_ptr(mem) bytesPerRow:pitch bytesPerImage:slice fromRegion:MTLRegionMake2D(0, 0, w, h)
        mipmapLevel:level slice:face];
}

int gpu_pixels(dev_t_ *d, uint32_t surface, uint8_t *bgra, uint32_t *pw, uint32_t *ph) {
    (void)d;
    if (!G || !surface) return 0;
    res_t *r = OBJ(surface, res_t), *o = owner(r);
    if (!o->gpu) return 0;
    id<MTLTexture> t = (__bridge id<MTLTexture>)o->gpu;
    uint32_t l = r->parent ? r->level : 0, w = level_dim((uint32_t)t.width, l), h = level_dim((uint32_t)t.height, l);
    *pw = w, *ph = h;
    if (!bgra) return 1;
    if (t.pixelFormat != MTLPixelFormatBGRA8Unorm) return 0;
    flush_wait();
    [t getBytes:bgra bytesPerRow:w * 4 bytesPerImage:(NSUInteger)w * 4 * h fromRegion:MTLRegionMake2D(0, 0, w, h)
        mipmapLevel:l slice:r->parent ? r->face : 0];
    return 1;
}

void gpu_free(res_t *r) {
    if (r->gpu) { CFBridgingRelease(r->gpu); r->gpu = NULL; }
    r->gpu_ring = NULL;
}

/* With a window: the back buffer to win32hle's window surface, which its
 * presenter (screen.c) scales into the window, F11 and F12 and the mouse's
 * mapping included, as it does a GDI game's.
 *
 * Without the CPU waiting for the GPU, as tpw-recomp's DirectDraw does it
 * (d3d_metal.m): each Present ends its frame with a copy of the back buffer
 * into one of three shared buffers, and the window shows the newest copy the
 * GPU has finished, a frame or two behind. Waiting for each frame instead
 * (the first way) left the window at 40% of the headless frame rate. */
#define SHOW 3
static id<MTLBuffer> g_show[SHOW];
static _Atomic uint64_t g_show_done[SHOW];             /* the frame each holds, once the GPU is done */
static uint64_t g_show_seq, g_shown;
static int g_show_w, g_show_h;
static uint8_t *g_win_px;                               /* what the window surface is */

/* The copy of this frame, into the buffer least recently started. */
static void window_copy(dev_t_ *d, id<MTLCommandBuffer> c) {
    res_t *bb = OBJ(d->backbuffer, res_t);
    if (!bb->gpu) return;
    id<MTLTexture> t = (__bridge id<MTLTexture>)bb->gpu;
    int w = (int)t.width, h = (int)t.height;
    if (w != g_show_w || h != g_show_h) {               /* a new size: the old copies are of no use */
        for (int i = 0; i < SHOW; i++) g_show[i] = nil, atomic_store(&g_show_done[i], 0);
        free(g_win_px);
        g_win_px = calloc((size_t)w * (size_t)h, 4);
        g_show_w = w, g_show_h = h, g_shown = 0;
        hle_gdi_window_surface(g_win_px, w, h, w * 4, 32);
    }
    int i = (int)(g_show_seq % SHOW);
    NSUInteger n = (NSUInteger)w * (NSUInteger)h * 4;
    if (!g_show[i]) g_show[i] = [G->dev newBufferWithLength:n options:MTLResourceStorageModeShared];
    atomic_store(&g_show_done[i], 0);                   /* being written */
    id<MTLBlitCommandEncoder> bl = [c blitCommandEncoder];
    [bl copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
             sourceSize:MTLSizeMake((NSUInteger)w, (NSUInteger)h, 1) toBuffer:g_show[i]
      destinationOffset:0 destinationBytesPerRow:(NSUInteger)w * 4 destinationBytesPerImage:n];
    [bl endEncoding];
    uint64_t seq = ++g_show_seq;
    _Atomic uint64_t *done = &g_show_done[i];
    [c addCompletedHandler:^(id<MTLCommandBuffer> b) { (void)b; atomic_store(done, seq); }];
}
/* The newest finished copy to the window, if it is newer than what it shows. */
static void window_show(void) {
    int best = -1;
    uint64_t newest = g_shown;
    for (int i = 0; i < SHOW; i++) {
        uint64_t s = atomic_load(&g_show_done[i]);
        if (s > newest) newest = s, best = i;
    }
    if (best < 0) return;
    memcpy(g_win_px, g_show[best].contents, (size_t)g_show_w * (size_t)g_show_h * 4);
    g_shown = newest;
    hle_gdi_window_changed();                           /* shown by the flip at the end of Present */
}

/* Autoreleased objects (command buffers, encoders, descriptors) are let go
 * once a frame: the game's thread runs no run loop to drain a pool, and
 * without one every frame's command buffer, and all it held, stayed. */
void *objc_autoreleasePoolPush(void);
void objc_autoreleasePoolPop(void *pool);
static void *g_pool;
static pthread_t g_pool_thread;
static void frame_pool(void) {
    if (g_pool && pthread_equal(g_pool_thread, pthread_self())) objc_autoreleasePoolPop(g_pool);
    g_pool = objc_autoreleasePoolPush(), g_pool_thread = pthread_self();
}

void gpu_present(dev_t_ *d) {
    if (!G) return;
    end_encoder();
    id<MTLCommandBuffer> c = command();
    if (!jpog_headless) window_copy(d, c);
    dispatch_semaphore_t sem = G->inflight;
    NSArray *keep = G->extras;                          /* the frame's overflow buffers: until the GPU is done */
    [c addCompletedHandler:^(id<MTLCommandBuffer> b) { (void)b; (void)keep; dispatch_semaphore_signal(sem); }];
    G->extras = nil, G->over = nil, G->over_used = 0;
    [c commit];
    G->last = c, G->cmd = nil;
    dispatch_semaphore_wait(G->inflight, DISPATCH_TIME_FOREVER);
    if (!jpog_headless) {
        window_show();
        /* Shown as a flip, and paced as one: held to the display's refresh,
         * near 60 a second (HLE_MAX_FPS), where the game would otherwise
         * draw as many frames as the machine allows (490 a second here). */
        hle_screen_flip();
    }
    G->frame++;
    G->ring_i = (G->ring_i + 1) % FRAMES, G->ring_used = 0;
    G->gen++;
    frame_pool();
}
