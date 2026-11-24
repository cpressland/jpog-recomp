/*
 * movies.c - binkw32.dll, answered by the host: the 13 calls SimJP.exe makes
 * of Bink (stdcall, by their decorated names), on ffmpeg's Bink decoder, their
 * sound in fsound.c's mixer. pcrecomp's bink.c answers Bink too, but not five
 * of these, and its seek finds no frames, so nothing loops (docs/PLAN.md).
 *
 *   - The handle is a BINK whose leading fields are Bink's own (Width, Height,
 *     Frames, FrameNum, ...): games read them. What is the host's is here.
 *   - A movie is paced by the clock from its first frame. Sound is mixed as
 *     it is decoded, a little ahead of the picture as the file has it.
 *   - Past the last frame it begins again (FrameNum 1), as Bink does: the
 *     game stops it, or does not. It has no call to seek with.
 *
 * --movie FILE plays one by those calls through Direct3D 8: a locked texture,
 * stretched over the display.
 */
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "d3d_int.h"
#include "engine.h"

#ifdef HLE_WITH_FFMPEG
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>

#define BINKFILEHANDLE 0x00800000u
enum { WIDTH = 0, HEIGHT = 4, FRAMES = 8, FRAMENUM = 12, LASTFRAMENUM = 16, FRAMERATE = 20, FRAMERATEDIV = 24, OPENFLAGS = 32 };

typedef struct movie {
    uint32_t guest;                          /* the BINK */
    AVFormatContext *fmt;
    AVCodecContext *vdec, *adec;
    SwrContext *swr;
    AVFrame *frame, *aframe;
    AVPacket *pkt;
    int vs, as, sound, have, drained;
    double due, period;                      /* seconds: when FrameNum is to be shown, and how long each is */
    uint64_t start;
    struct movie *next;
} movie_t;
static movie_t *g_movies;
static int g_tracks = 1, g_unpaced;
static const char *g_error = "";

static movie_t *movie_at(uint32_t va) {
    for (movie_t *m = g_movies; va && m; m = m->next) if (m->guest == va) return m;
    return NULL;
}
static void drop(movie_t *m) {
    for (movie_t **p = &g_movies; *p; p = &(*p)->next) if (*p == m) { *p = m->next; break; }
    if (m->sound >= 0) jpog_movie_sound_close(m->sound);
    if (m->swr) swr_free(&m->swr);
    if (m->vdec) avcodec_free_context(&m->vdec);
    if (m->adec) avcodec_free_context(&m->adec);
    if (m->fmt) avformat_close_input(&m->fmt);
    av_frame_free(&m->frame);
    av_frame_free(&m->aframe);
    av_packet_free(&m->pkt);
    if (m->guest) hle_free(hle_ptr(m->guest));
    free(m);
}
static AVCodecContext *decoder(AVStream *s) {
    const AVCodec *c = avcodec_find_decoder(s->codecpar->codec_id);
    AVCodecContext *x = c ? avcodec_alloc_context3(c) : NULL;
    if (x && (avcodec_parameters_to_context(x, s->codecpar) < 0 || avcodec_open2(x, c, NULL) < 0)) avcodec_free_context(&x);
    return x;
}
static void take_sound(movie_t *m) {
    float *out[2];
    int n = m->swr && m->sound >= 0 ? swr_get_out_samples(m->swr, m->aframe->nb_samples) : 0;
    if (n <= 0 || av_samples_alloc((uint8_t **)out, NULL, 2, n, AV_SAMPLE_FMT_FLTP, 0) < 0) return;
    n = swr_convert(m->swr, (uint8_t **)out, n, (const uint8_t **)m->aframe->extended_data, m->aframe->nb_samples);
    if (n > 0) jpog_movie_sound_push(m->sound, out[0], out[1], n);
    av_freep(&out[0]);
}
/* Packets until the next picture; the sound met on the way to the mixer. */
static int picture(movie_t *m) {
    for (;;) {
        int r = avcodec_receive_frame(m->vdec, m->frame);
        if (r == 0) return 1;
        if (r != AVERROR(EAGAIN) || m->drained) return 0;
        if (av_read_frame(m->fmt, m->pkt) < 0) { avcodec_send_packet(m->vdec, NULL); m->drained = 1; continue; }
        if (m->pkt->stream_index == m->vs) avcodec_send_packet(m->vdec, m->pkt);
        else if (m->adec && m->pkt->stream_index == m->as && avcodec_send_packet(m->adec, m->pkt) == 0)
            while (avcodec_receive_frame(m->adec, m->aframe) == 0) take_sound(m);
        av_packet_unref(m->pkt);
    }
}
static double now(const movie_t *m) { return m->start ? (double)(hle_mono_ns() - m->start) / 1e9 : 0; }

static void b_Open(void) {                               /* (name, flags) */
    char host[4096], full[512];
    uint32_t flags = A32(1);
    const char *name = flags & BINKFILEHANDLE ? "" : ASTR(0);
    snprintf(full, sizeof full, "C:\\%s", name);
    if (!*name || !(hle_host_path(name, host, sizeof host) || hle_host_path(full, host, sizeof host))) {
        fprintf(stderr, "[bink] no %s\n", *name ? name : "(movies by file handle)");
        g_error = "cannot open the movie";
        RET(0, 2);
    }
    movie_t *m = calloc(1, sizeof *m);
    m->sound = m->as = -1;
    m->next = g_movies, g_movies = m;
    if (avformat_open_input(&m->fmt, host, av_find_input_format("bink"), NULL) < 0 || avformat_find_stream_info(m->fmt, NULL) < 0 ||
        (m->vs = av_find_best_stream(m->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0)) < 0 || !(m->vdec = decoder(m->fmt->streams[m->vs]))) {
        fprintf(stderr, "[bink] %s: not a Bink movie\n", name);
        g_error = "not a Bink movie";
        drop(m);
        RET(0, 2);
    }
    m->as = g_tracks ? av_find_best_stream(m->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0) : -1;
    if (m->as >= 0 && (m->adec = decoder(m->fmt->streams[m->as]))) {
        AVChannelLayout two = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(&m->swr, &two, AV_SAMPLE_FMT_FLTP, m->adec->sample_rate, &m->adec->ch_layout, m->adec->sample_fmt,
                                m->adec->sample_rate, 0, NULL) == 0 && swr_init(m->swr) == 0) m->sound = jpog_movie_sound(m->adec->sample_rate);
    }
    m->frame = av_frame_alloc(), m->aframe = av_frame_alloc(), m->pkt = av_packet_alloc();
    AVStream *v = m->fmt->streams[m->vs];
    uint32_t g = m->guest = hle_va(hle_alloc(512));
    AVRational fps = v->avg_frame_rate.num ? v->avg_frame_rate : (AVRational){ 15, 1 };
    MEM32(g + WIDTH) = (uint32_t)v->codecpar->width, MEM32(g + HEIGHT) = (uint32_t)v->codecpar->height;
    MEM32(g + FRAMES) = v->nb_frames > 0 ? (uint32_t)v->nb_frames : (uint32_t)v->duration;
    MEM32(g + FRAMENUM) = 1, MEM32(g + FRAMERATE) = (uint32_t)fps.num, MEM32(g + FRAMERATEDIV) = (uint32_t)fps.den, MEM32(g + OPENFLAGS) = flags;
    m->period = (double)fps.den / fps.num;
    if (jpog_verbose) fprintf(stderr, "[bink] %s: %dx%d, %u frames, %.2f a second%s\n", name, v->codecpar->width, v->codecpar->height,
                              MEM32(g + FRAMES), 1 / m->period, m->sound >= 0 ? ", sound" : "");
    RET(g, 2);
}
static void b_Close(void) { movie_t *m = movie_at(A32(0)); if (m) drop(m); RETV(1); }
static void b_DoFrame(void) {                            /* the picture FrameNum says */
    movie_t *m = movie_at(A32(0));
    if (!m) RET(0, 1);
    if (!m->start) m->start = hle_mono_ns();
    m->have = picture(m) || m->have;                     /* none: the one before stays */
    MEM32(m->guest + LASTFRAMENUM) = MEM32(m->guest + FRAMENUM);
    RET(0, 1);
}
static void b_NextFrame(void) {
    movie_t *m = movie_at(A32(0));
    if (!m) RETV(1);
    uint32_t g = m->guest;
    m->due += m->period;
    if (now(m) - m->due > 2) m->due = now(m);            /* a long stop (a load, the window away): on from here, not all at once */
    if (MEM32(g + FRAMENUM) < MEM32(g + FRAMES)) { MEM32(g + FRAMENUM)++; RETV(1); }
    MEM32(g + FRAMENUM) = 1;                             /* round again */
    if (av_seek_frame(m->fmt, m->vs, 0, AVSEEK_FLAG_BACKWARD) < 0) fprintf(stderr, "[bink] cannot go back to the start\n");
    avcodec_flush_buffers(m->vdec);
    if (m->adec) avcodec_flush_buffers(m->adec);
    m->drained = 0;
    RETV(1);
}
static void b_Wait(void) {                               /* nonzero: it is not yet time */
    movie_t *m = movie_at(A32(0));
    RET(m && m->start && !g_unpaced && now(m) < m->due, 1);
}
static void b_Service(void) { RETV(1); }
static void b_SetSoundOnOff(void) {                      /* (bink, on) -> as it is */
    movie_t *m = movie_at(A32(0));
    if (m && m->sound >= 0) jpog_movie_sound_on(m->sound, A32(1) != 0);
    RET(A32(1) != 0, 2);
}
static void b_SetSoundTrack(void) { g_tracks = A32(0) != 0; RETV(2); }   /* (how many, which): for the next opened */
static void b_SetSoundSystem(void) { RET(1, 2); }        /* (open, device): the mixer, whatever it is */
static void b_OpenDirectSound(void) { RET(0, 1); }
static void b_RADSetMemory(void) { RETV(2); }            /* (allocate, free): the host's own */

/* A rectangle of the picture into a buffer of the kind the flags' low bits name. */
static void copy(const movie_t *m, uint8_t *dst, int pitch, int high, int dx, int dy, int sx, int sy, int w, int h, uint32_t flags) {
    const AVFrame *f = m->frame;
    int type = (int)(flags & 15), full = f->color_range == AVCOL_RANGE_JPEG;
    if (!m->have || !dst || (f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVA420P && f->format != AV_PIX_FMT_YUVJ420P)) return;
    if (sx < 0) w += sx, dx -= sx, sx = 0;
    if (sy < 0) h += sy, dy -= sy, sy = 0;
    if (w > f->width - sx) w = f->width - sx;
    if (h > f->height - sy) h = f->height - sy;
    for (int y = 0; y < h && dy + y < high; y++) {
        const uint8_t *Y = f->data[0] + (sy + y) * f->linesize[0] + sx, *U = f->data[1] + ((sy + y) / 2) * f->linesize[1],
                      *V = f->data[2] + ((sy + y) / 2) * f->linesize[2], *A = f->format == AV_PIX_FMT_YUVA420P ? f->data[3] + (sy + y) * f->linesize[3] + sx : NULL;
        uint8_t *row = dst + (ptrdiff_t)(dy + y) * pitch;
        if (dy + y < 0) continue;
        for (int x = 0; x < w; x++) {
            int d = U[(sx + x) / 2] - 128, e = V[(sx + x) / 2] - 128, r, g, b;
            if (full) { int c = Y[x] << 8; r = (c + 359 * e + 128) >> 8, g = (c - 88 * d - 183 * e + 128) >> 8, b = (c + 454 * d + 128) >> 8; }
            else { int c = 298 * (Y[x] - 16); r = (c + 409 * e + 128) >> 8, g = (c - 100 * d - 208 * e + 128) >> 8, b = (c + 516 * d + 128) >> 8; }
            r = r < 0 ? 0 : r > 255 ? 255 : r, g = g < 0 ? 0 : g > 255 ? 255 : g, b = b < 0 ? 0 : b > 255 ? 255 : b;
            uint32_t a = A ? A[x] : 255;
            int X = dx + x;
            if (X < 0) continue;
            switch (type) {
            case 1: row[3 * X] = (uint8_t)b, row[3 * X + 1] = (uint8_t)g, row[3 * X + 2] = (uint8_t)r; break;         /* 24 */
            case 2: row[3 * X] = (uint8_t)r, row[3 * X + 1] = (uint8_t)g, row[3 * X + 2] = (uint8_t)b; break;         /* 24R */
            case 3: case 5: ((uint32_t *)(void *)row)[X] = a << 24 | (uint32_t)(r << 16 | g << 8 | b); break;         /* 32, 32A */
            case 4: case 6: ((uint32_t *)(void *)row)[X] = a << 24 | (uint32_t)(b << 16 | g << 8 | r); break;         /* 32R, 32RA */
            case 7: ((uint16_t *)(void *)row)[X] = (uint16_t)(a >> 4 << 12 | (uint32_t)(r >> 4 << 8 | g >> 4 << 4 | b >> 4)); break;   /* 4444 */
            case 8: ((uint16_t *)(void *)row)[X] = (uint16_t)(a >> 7 << 15 | (uint32_t)(r >> 3 << 10 | g >> 3 << 5 | b >> 3)); break;  /* 5551 */
            case 9: ((uint16_t *)(void *)row)[X] = (uint16_t)(r >> 3 << 10 | g >> 3 << 5 | b >> 3); break;            /* 555 */
            case 10: ((uint16_t *)(void *)row)[X] = (uint16_t)(r >> 3 << 11 | g >> 2 << 5 | b >> 3); break;           /* 565 */
            }
        }
    }
    if (type == 0 || type > 10) {
        static int said;
        if (!said++) fprintf(stderr, "[bink] a surface of kind %d is not answered\n", type);
    }
}
static void b_CopyToBuffer(void) {                       /* (bink, buffer, pitch, height, x, y, flags) */
    movie_t *m = movie_at(A32(0));
    if (m && A32(1)) copy(m, APTR(1), (int)A32(2), (int)A32(3), (int)A32(4), (int)A32(5), 0, 0, m->frame->width, m->frame->height, A32(6));
    RET(0, 7);
}
static void b_CopyToBufferRect(void) {                   /* (bink, buffer, pitch, height, x, y, from x, y, width, height, flags) */
    movie_t *m = movie_at(A32(0));
    if (m && A32(1)) copy(m, APTR(1), (int)A32(2), (int)A32(3), (int)A32(4), (int)A32(5), (int)A32(6), (int)A32(7), (int)A32(8), (int)A32(9), A32(10));
    RET(0, 11);
}
const win32hle_shim jpog_bink_shims[] = {
    { "_BinkOpen@8", b_Open }, { "_BinkClose@4", b_Close }, { "_BinkDoFrame@4", b_DoFrame }, { "_BinkNextFrame@4", b_NextFrame },
    { "_BinkWait@4", b_Wait }, { "_BinkService@4", b_Service }, { "_BinkCopyToBuffer@28", b_CopyToBuffer },
    { "_BinkCopyToBufferRect@44", b_CopyToBufferRect }, { "_BinkSetSoundOnOff@8", b_SetSoundOnOff },
    { "_BinkSetSoundTrack@8", b_SetSoundTrack }, { "_BinkSetSoundSystem@8", b_SetSoundSystem },
    { "_BinkOpenDirectSound@4", b_OpenDirectSound }, { "_RADSetMemory@8", b_RADSetMemory }, { NULL, NULL } };
#else
/* No decoder: pcrecomp's bink.c plays a movie without a picture, two frames
 * of it; what it lacks of the game's calls does nothing. */
static int g_unpaced;
static void b_Nothing1(void) { RET(0, 1); }
static void b_Nothing2(void) { RET(0, 2); }
static void b_Nothing11(void) { RET(0, 11); }
const win32hle_shim jpog_bink_shims[] = { { "_BinkService@4", b_Nothing1 }, { "_BinkSetSoundOnOff@8", b_Nothing2 }, { "_BinkSetSoundTrack@8", b_Nothing2 },
    { "_RADSetMemory@8", b_Nothing2 }, { "_BinkCopyToBufferRect@44", b_Nothing11 }, { NULL, NULL } };
#endif

/* ---- --movie ------------------------------------------------------------------ */
static uint32_t ask(const char *name, int n, const uint32_t *a) { return hle_call_guest(hle_resolve(name), n, a); }
#define ASK(name, n, ...) ask("_Bink" name, n, (uint32_t[]){ __VA_ARGS__ })
static uint32_t com(uint32_t o, int method, int n, ...) {
    uint32_t a[12] = { o };
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) a[i + 1] = va_arg(ap, uint32_t);
    va_end(ap);
    return hle_call_guest(MEM32(MEM32(o) + 4u * (uint32_t)method), n + 1, a);
}
int jpog_movie(const char *file, const char *shot, int frames) {
    engine_window_shot();
    mach_enter();
    uint32_t kernel = jpog_engine_kernel(), r = kernel ? jpog_engine_renderer(kernel) : 0;
    if (!r || !jpog_engine_display(r, 1024, 768)) { fprintf(stderr, "[movie] the engine did not start\n"); return 1; }
    uint32_t dev = jpog_engine_device(r), name = S(file), b;
    ASK("SetSoundSystem@8", 2, hle_resolve("_BinkOpenDirectSound@4"), 0);
    if (!(b = ASK("Open@8", 2, name, 0))) return 1;
    uint32_t w = MEM32(b), h = MEM32(b + 4), n = MEM32(b + 8), out = hle_scratch(4), lk = hle_scratch(8), tw = 1, th = 1;
    while (tw < w) tw *= 2;
    while (th < h) th *= 2;
    com(dev, 20, 7, tw, th, 1u, 0u, 22u, 1u, out);       /* CreateTexture: X8R8G8B8, managed */
    uint32_t tex = MEM32(out);
    float u = (float)w / (float)tw, v = (float)h / (float)th;
    const struct { float x, y, z, rhw; uint32_t c; float u, v; } quad[4] = { { -0.5f, -0.5f, 0.5f, 1, ~0u, 0, 0 }, { 1023.5f, -0.5f, 0.5f, 1, ~0u, u, 0 },
                                                                            { -0.5f, 767.5f, 0.5f, 1, ~0u, 0, v }, { 1023.5f, 767.5f, 0.5f, 1, ~0u, u, v } };
    uint32_t vb = hle_scratch_copy(quad, sizeof quad);
    g_unpaced = jpog_headless;
    fprintf(stderr, "[movie] %s: %ux%u, %u frames, %.2f a second\n", file, w, h, n, (double)MEM32(b + 20) / MEM32(b + 24));
    static float heard[2 * 4096];
    double owed = 0, sum = 0, behind = 0;
    float peak = 0;
    uint64_t start = hle_mono_ns(), mixed = 0;
    int shown_ = 0, last = frames ? frames : (int)n, lit = 0;
    while (shown_ < last && !engine_pump()) {
        ASK("DoFrame@4", 1, b);
        if (com(tex, 16, 4, 0u, lk, 0u, 0u) == 0) {      /* LockRect */
            ASK("CopyToBuffer@28", 7, b, MEM32(lk + 4), MEM32(lk), th, 0, 0, 3);
            for (uint32_t i = 0; i < w * 4 && !(lit & 1); i += 64) lit |= (MEM32(MEM32(lk + 4) + h / 2 * MEM32(lk) + i) & 0xFFFFFF) > 0x202020;
            com(tex, 17, 1, 0u);
        }
        com(dev, 34, 0);
        com(dev, 50, 2, 7u, 0u), com(dev, 50, 2, 22u, 1u), com(dev, 50, 2, 137u, 0u), com(dev, 50, 2, 27u, 0u);   /* no depth, cull, lights, blend */
        com(dev, 61, 2, 0u, tex);
        com(dev, 63, 3, 0u, 1u, 2u), com(dev, 63, 3, 0u, 2u, 2u);      /* the texture's colour */
        com(dev, 63, 3, 0u, 16u, 2u), com(dev, 63, 3, 0u, 17u, 2u);    /* linear */
        com(dev, 88, 1, 0u), com(dev, 76, 1, 0x144u);                  /* no pixel shader; XYZRHW, diffuse, one texture */
        com(dev, 72, 4, 5u, 2u, vb, 28u);
        com(dev, 35, 0);
        if (shot && shown_ == last - 1) fprintf(stderr, "[movie] shot %s: %s\n", shot, jpog_d3d8_shot(shot) ? "ok" : "no");
        com(dev, 15, 4, 0u, 0u, 0u, 0u);
        shown_++;
        uint32_t was = MEM32(b + 12);
        ASK("NextFrame@4", 1, b);
        if (was == n && MEM32(b + 12) != 1) fprintf(stderr, "[movie] frame %u follows the last\n", MEM32(b + 12));
        if (jpog_headless) {                             /* a frame's worth of its sound */
            for (owed += 44100.0 * MEM32(b + 24) / MEM32(b + 20); owed >= 1;) {
                int k = owed > 4096 ? 4096 : (int)owed;
                jpog_fsound_render(heard, k);
                for (int i = 0; i < 2 * k; i++) { float f = fabsf(heard[i]); sum += (double)f; if (f > peak) peak = f; }
                owed -= k, mixed += (uint64_t)k;
            }
        } else {
            while (ASK("Wait@4", 1, b)) { hle_block_begin(); usleep(1000); hle_block_end(); }
            double late = (double)(hle_mono_ns() - start) / 1e9 - shown_ * (double)MEM32(b + 24) / MEM32(b + 20);
            if (late > behind) behind = late;
        }
    }
    double took = (double)(hle_mono_ns() - start) / 1e9;
    fprintf(stderr, "[movie] %d frames in %.2f s%s, sound peak %.3f mean %.4f\n", shown_, took, lit & 1 ? "" : ", all dark", (double)peak,
            mixed ? sum / (2 * (double)mixed) : 0);
    if (!jpog_headless) fprintf(stderr, "[movie] at most %.0f ms late\n", behind * 1000);
    ASK("Close@4", 1, b);
    com(tex, 2, 0);
    mach_leave();
    return 0;
}
