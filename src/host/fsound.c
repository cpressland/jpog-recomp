/*
 * fsound.c - fmod.dll, answered by the host: the 41 FSOUND_* calls SimJP.exe
 * makes of FMOD 3 (stdcall, by their decorated names), on a mixer of its own.
 *
 * Sounds are 16- or 8-bit PCM WAVs, read whole; music is Ogg Vorbis
 * (libvorbisfile), decoded as it plays by a thread of its own. The mixer
 * reads only host memory, never the guest's, so it needs no machine lock.
 *
 *   - A channel is a handle: its index, and above bit 12 how many times it has
 *     been played, so a handle to a sound that has gone does nothing.
 *   - 2D: volume and pan are linear, 0 to 255; a mono sound in the middle is
 *     half each side, a stereo one under FSOUND_STEREOPAN (-1) as it is.
 *   - 3D (FSOUND_HW3D): falls off as min / distance between the sample's
 *     minimum and maximum, placed by where the listener looks, with Doppler.
 *     Left-handed, as FMOD 3 is.
 *   - A stream's end is told (Stream_SetEndCallback) from FSOUND_Update: on
 *     the game's thread, where FMOD used one of its own.
 *
 * jpog_fsound_render mixes without a device, for tests and --listen; JPOG_WAV=FILE
 * keeps what was played.
 */
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <SDL.h>
#ifdef JPOG_VORBIS
#include <vorbis/vorbisfile.h>
#endif

#include "win32hle.h"
#include "jpog.h"

enum { LOOP_OFF = 1, LOOP_NORMAL = 2, LOOP_BIDI = 4, M_8BITS = 8, M_16BITS = 0x10, M_MONO = 0x20, M_STEREO = 0x40, M_UNSIGNED = 0x80,
       HW3D = 0x1000, M_2D = 0x2000, LOADMEMORY = 0x8000, LOADRAW = 0x10000 };
enum { CH_FREE = -1, CH_UNMANAGED = -2, CH_ALL = -3, STEREOPAN = -1 };
enum { MAX_CHANNELS = 1024, RING = 1 << 16, AHEAD = 44100 };

typedef struct sample {
    uint32_t guest;                          /* the FSOUND_SAMPLE the game holds */
    float   *pcm;                            /* interleaved, channels of them */
    uint32_t frames;
    int      channels, rate, mode;
    int      def_freq, def_vol, def_pan, def_pri, max_playbacks;
    float    min, max;
    struct stream *stream;                   /* a stream's: no pcm */
    struct sample *next;
    size_t   bytes;
} sample_t;

typedef struct stream {
    uint32_t guest, callback, param;
    sample_t *sample;
#ifdef JPOG_VORBIS
    OggVorbis_File vf;
#endif
    int      vorbis, ended, told;
    float   *ring;                           /* stereo */
    uint64_t rd, wr;                         /* frames, ever */
    uint64_t length;
    uint32_t src;                            /* a WAV's next frame */
    struct stream *next;
} stream_t;

typedef struct {
    int      used, paused, gen, vol, pan, freq, pri;
    sample_t *s;
    stream_t *stream;
    double   pos;
    uint64_t age;
    float    at[3], vel[3], gl, gr;          /* the gains as they were, moved to by the next mix */
    int      fresh;
} chan_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static chan_t g_ch[MAX_CHANNELS];
static int g_nch = 32, g_rate = 44100, g_open, g_master = 255, g_feeding;
static float g_sep = 1;
static uint64_t g_age;
static size_t g_bytes, g_most;
static sample_t *g_samples;
static stream_t *g_streams;
static SDL_AudioDeviceID g_dev;
static struct { float pos[3], vel[3], fwd[3], top[3]; } g_ear = { { 0 }, { 0 }, { 0, 0, 1 }, { 0, 1, 0 } };
int jpog_fsound_offline;                     /* no device: what is asked of jpog_fsound_render */

/* ---- files ---------------------------------------------------------------- */
static uint8_t *read_whole(const char *guest, size_t *n) {
    char host[4096], full[512];
    snprintf(full, sizeof full, "C:\\%s", guest);
    FILE *f = hle_host_path(guest, host, sizeof host) || hle_host_path(full, host, sizeof host) ? fopen(host, "rb") : NULL;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    uint8_t *d = len > 0 ? malloc((size_t)len) : NULL;
    if (d && fread(d, 1, (size_t)len, f) != (size_t)len) { free(d); d = NULL; }
    fclose(f);
    *n = d ? (size_t)len : 0;
    return d;
}
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }

static float *pcm_of(const uint8_t *d, size_t bytes, int bits, int is_signed, int channels, uint32_t *frames) {
    size_t n = bytes / (size_t)(bits / 8);
    n -= n % (size_t)channels;
    float *out = malloc((n ? n : 1) * sizeof *out);
    for (size_t i = 0; i < n; i++)
        out[i] = bits == 16 ? (float)(int16_t)le16(d + 2 * i) / 32768.0f : is_signed ? (float)(int8_t)d[i] / 128.0f : ((float)d[i] - 128.0f) / 128.0f;
    *frames = (uint32_t)(n / (size_t)channels);
    return out;
}
/* A RIFF WAV, PCM. */
static int wav(const uint8_t *d, size_t n, sample_t *s) {
    if (n < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) return 0;
    int bits = 0;
    for (size_t p = 12; p + 8 <= n;) {
        size_t len = le32(d + p + 4), room = n - p - 8;
        if (len > room) len = room;
        if (!memcmp(d + p, "fmt ", 4) && len >= 16) {
            if (le16(d + p + 8) != 1) return 0;
            s->channels = (int)le16(d + p + 10), s->rate = (int)le32(d + p + 12), bits = (int)le16(d + p + 22);
        } else if (!memcmp(d + p, "data", 4) && (bits == 8 || bits == 16) && (s->channels == 1 || s->channels == 2)) {
            s->pcm = pcm_of(d + p + 8, len, bits, bits == 16, s->channels, &s->frames);
            return s->rate > 0;
        }
        p += 8 + len + (len & 1);
    }
    return 0;
}

/* ---- samples and streams --------------------------------------------------- */
static void grew(size_t n) { g_bytes += n; if (g_bytes > g_most) g_most = g_bytes; }
static sample_t *new_sample(int mode) {
    sample_t *s = calloc(1, sizeof *s);
    s->guest = hle_va(hle_alloc(16));
    s->mode = mode, s->def_freq = 44100, s->def_vol = 255, s->def_pan = 128, s->def_pri = 128, s->max_playbacks = MAX_CHANNELS;
    s->min = 1, s->max = 1e9f;
    s->next = g_samples, g_samples = s;
    return s;
}
static void loaded(sample_t *s) {
    s->def_freq = s->rate;
    s->bytes = (size_t)s->frames * (size_t)s->channels * 2;
    grew(s->bytes);
}
static sample_t *sample_at(uint32_t va) {
    for (sample_t *s = g_samples; va && s; s = s->next) if (s->guest == va) return s;
    return NULL;
}
static stream_t *stream_at(uint32_t va) {
    for (stream_t *s = g_streams; va && s; s = s->next) if (s->guest == va) return s;
    return NULL;
}
static void drop_sample(sample_t *s) {
    for (int i = 0; i < g_nch; i++) if (g_ch[i].used && g_ch[i].s == s) g_ch[i].used = 0;
    for (sample_t **p = &g_samples; *p; p = &(*p)->next) if (*p == s) { *p = s->next; break; }
    g_bytes -= s->bytes;
    hle_free(hle_ptr(s->guest));
    free(s->pcm);
    free(s);
}
static void drop_stream(stream_t *t) {
    for (int i = 0; i < g_nch; i++) if (g_ch[i].used && g_ch[i].stream == t) g_ch[i].used = 0;
    for (stream_t **p = &g_streams; *p; p = &(*p)->next) if (*p == t) { *p = t->next; break; }
#ifdef JPOG_VORBIS
    if (t->vorbis) ov_clear(&t->vf);
#endif
    drop_sample(t->sample);
    hle_free(hle_ptr(t->guest));
    free(t->ring);
    free(t);
}
/* More of a stream into its ring, up to `want` frames ahead. Holding the lock. */
static void fill(stream_t *t, uint64_t want) {
    int loop = (t->sample->mode & (LOOP_NORMAL | LOOP_BIDI)) != 0, stuck = 0;
    if (want > RING - 4096) want = RING - 4096;
    while (!t->ended && t->wr - t->rd < want) {
        float block[4096][2];
        int got = 0;
        if (t->vorbis) {
#ifdef JPOG_VORBIS
            float **pcm;
            int section;
            long n = ov_read_float(&t->vf, &pcm, 4096, &section);
            if (n == OV_HOLE) continue;
            if (n > 0) {
                int c = ov_info(&t->vf, -1)->channels;
                for (long i = 0; i < n; i++) block[i][0] = pcm[0][i], block[i][1] = pcm[c > 1][i];
                got = (int)n;
            }
#endif
        } else {
            sample_t *s = t->sample;
            for (; got < 4096 && t->src < s->frames; got++, t->src++)
                block[got][0] = s->pcm[(size_t)t->src * (size_t)s->channels], block[got][1] = s->pcm[(size_t)t->src * (size_t)s->channels + (s->channels > 1)];
        }
        for (int i = 0; i < got; i++, t->wr++) memcpy(t->ring + 2 * (t->wr % RING), block[i], sizeof block[i]);
        if (got) { stuck = 0; continue; }
        if (!loop || stuck++) { t->ended = 1; break; }
#ifdef JPOG_VORBIS
        if (t->vorbis && ov_pcm_seek(&t->vf, 0) != 0) t->ended = 1;
#endif
        t->src = 0;
    }
}
static void rewind_stream(stream_t *t) {
#ifdef JPOG_VORBIS
    if (t->vorbis && (t->rd || t->wr)) (void)ov_pcm_seek(&t->vf, 0);
#endif
    t->rd = t->wr = 0, t->src = 0, t->ended = t->told = 0;
}
static stream_t *open_stream(const char *name, int mode) {
    sample_t *s = new_sample(mode);
    stream_t *t = calloc(1, sizeof *t);
    int ok = 0;
    size_t n = 0;
#ifdef JPOG_VORBIS
    char host[4096], full[512];
    snprintf(full, sizeof full, "C:\\%s", name);
    if ((hle_host_path(name, host, sizeof host) || hle_host_path(full, host, sizeof host)) && ov_fopen(host, &t->vf) == 0) {
        t->vorbis = ok = 1;
        s->channels = ov_info(&t->vf, -1)->channels, s->rate = (int)ov_info(&t->vf, -1)->rate;
        t->length = (uint64_t)ov_pcm_total(&t->vf, -1);
        s->frames = (uint32_t)t->length, s->def_freq = s->rate;
    }
#endif
    if (!ok) {
        uint8_t *d = read_whole(name, &n);
        if (d && wav(d, n, s)) ok = 1, loaded(s), t->length = s->frames;
        free(d);
    }
    t->sample = s;
    if (!ok) { drop_sample(s); free(t); return NULL; }
    s->stream = t;
    t->guest = hle_va(hle_alloc(16));
    t->ring = calloc(RING, 2 * sizeof(float));
    t->next = g_streams, g_streams = t;
    return t;
}

/* ---- the mixer -------------------------------------------------------------- */
static float dot(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static int positional(const chan_t *c) { return (c->s->mode & HW3D) && !(c->s->mode & M_2D); }
/* A channel's two gains and its rate, as they are now. */
static void place(const chan_t *c, float *l, float *r, double *rate) {
    float v = (float)c->vol / 255.0f * (float)g_master / 255.0f;
    *rate = (double)c->freq / g_rate;
    if (positional(c)) {
        float d[3], right[3];
        for (int i = 0; i < 3; i++) d[i] = c->at[i] - g_ear.pos[i];
        const float *t = g_ear.top, *f = g_ear.fwd;
        right[0] = t[1] * f[2] - t[2] * f[1], right[1] = t[2] * f[0] - t[0] * f[2], right[2] = t[0] * f[1] - t[1] * f[0];
        float dist = sqrtf(dot(d, d)), rl = sqrtf(dot(right, right));
        float near_ = c->s->min > 0.0001f ? c->s->min : 0.0001f, far_ = c->s->max > near_ ? c->s->max : near_;
        float at = dist < near_ ? near_ : dist > far_ ? far_ : dist, side = 0;
        if (dist > 1e-4f && rl > 1e-4f) side = dot(d, right) / (dist * rl);
        if (dist < near_) side *= dist / near_;          /* inside the sound, it is all round */
        v *= near_ / at;
        *l = v * sqrtf((1 - side) / 2), *r = v * sqrtf((1 + side) / 2);
        if (dist > 1e-4f) {                              /* Doppler: how fast each goes along the line between them */
            float to = -dot(c->vel, d) / dist, from = dot(g_ear.vel, d) / dist;
            if (to > 300) to = 300;
            if (to < -300) to = -300;
            if (from > 300) from = 300;
            if (from < -300) from = -300;
            *rate *= (double)((340 + from) / (340 - to));
        }
        return;
    }
    if (c->pan < 0) { *l = *r = v; return; }
    float p = ((float)c->pan - 127.5f) / 127.5f * g_sep;
    if (c->pan == 128) p = 0;
    *l = v * (1 - p) / 2, *r = v * (1 + p) / 2;
}
static void mix_channel(chan_t *c, float *out, int frames) {
    float l, r;
    double rate;
    place(c, &l, &r, &rate);
    if (c->fresh) c->gl = l, c->gr = r, c->fresh = 0;
    float dl = (l - c->gl) / (float)frames, dr = (r - c->gr) / (float)frames, gl = c->gl, gr = c->gr;
    c->gl = l, c->gr = r;
    sample_t *s = c->s;
    stream_t *t = c->stream;
    int loop = (s->mode & (LOOP_NORMAL | LOOP_BIDI)) != 0;
    if (t) fill(t, (uint64_t)(rate * frames) + 4);
    for (int i = 0; i < frames; i++, gl += dl, gr += dr) {
        float a[2], b[2];
        uint64_t k = (uint64_t)c->pos;
        float fr = (float)(c->pos - (double)k);
        if (t) {
            if (k >= t->wr) { if (t->ended) c->used = 0; break; }
            const float *p = t->ring + 2 * (k % RING), *q = t->ring + 2 * ((k + 1 < t->wr ? k + 1 : k) % RING);
            a[0] = p[0], a[1] = p[1], b[0] = q[0], b[1] = q[1];
        } else {
            if (k >= s->frames) {
                if (!loop || !s->frames) { c->used = 0; break; }
                c->pos = fmod(c->pos, s->frames), k = (uint64_t)c->pos, fr = (float)(c->pos - (double)k);
            }
            uint64_t n = k + 1 < s->frames ? k + 1 : loop ? 0 : k;
            int m = s->channels > 1;
            size_t sc = (size_t)s->channels;
            a[0] = s->pcm[k * sc], a[1] = s->pcm[k * sc + (size_t)m], b[0] = s->pcm[n * sc], b[1] = s->pcm[n * sc + (size_t)m];
        }
        out[2 * i] += (a[0] + (b[0] - a[0]) * fr) * gl;
        out[2 * i + 1] += (a[1] + (b[1] - a[1]) * fr) * gr;
        c->pos += rate;
    }
    if (t) t->rd = (uint64_t)c->pos < t->wr ? (uint64_t)c->pos : t->wr;
}
void jpog_fsound_render(float *out, int frames) {
    memset(out, 0, (size_t)frames * 2 * sizeof *out);
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nch && g_open; i++) if (g_ch[i].used && !g_ch[i].paused) mix_channel(&g_ch[i], out, frames);
    pthread_mutex_unlock(&g_lock);
    for (int i = 0; i < frames * 2; i++) out[i] = out[i] > 1 ? 1 : out[i] < -1 ? -1 : out[i];
}
/* JPOG_WAV=FILE: what was played, 16-bit. */
static FILE *g_wav;
static void keep(const float *f, int frames) {
    static int checked;
    if (!checked++ && getenv("JPOG_WAV") && (g_wav = fopen(getenv("JPOG_WAV"), "wb"))) {
        uint8_t h[44] = "RIFF\xff\xff\xff\x7fWAVEfmt \x10\0\0\0\1\0\2\0";
        uint32_t rate = (uint32_t)g_rate, bps = rate * 4, most = 0x7FFFFF00u;
        memcpy(h + 24, &rate, 4), memcpy(h + 28, &bps, 4), memcpy(h + 32, "\4\0\20\0data", 8), memcpy(h + 40, &most, 4);
        fwrite(h, 1, 44, g_wav);
    }
    if (!g_wav) return;
    for (int i = 0; i < frames * 2; i++) { int16_t v = (int16_t)(f[i] * 32767); fwrite(&v, 2, 1, g_wav); }
    fflush(g_wav);
}
static void audio(void *ud, Uint8 *stream, int len) {
    (void)ud;
    jpog_fsound_render((float *)(void *)stream, len / 8);
    keep((float *)(void *)stream, len / 8);
}
static void *feeder(void *ud) {              /* the streams' decoding, off the sound card's thread */
    (void)ud;
    for (;;) {
        for (int i = 0; i < g_nch; i++) {
            pthread_mutex_lock(&g_lock);
            int on = g_feeding;
            if (on && g_ch[i].used && g_ch[i].stream && !g_ch[i].paused) fill(g_ch[i].stream, AHEAD);
            pthread_mutex_unlock(&g_lock);
            if (!on) return NULL;
        }
        usleep(20000);
    }
}

/* ---- channels ----------------------------------------------------------------- */
static int handle_of(int i) { return i | (g_ch[i].gen & 0x7FFFF) << 12; }
static chan_t *chan(int h) {
    int i = h & 0xFFF;
    return h >= 0 && i < g_nch && g_ch[i].used && handle_of(i) == h ? &g_ch[i] : NULL;
}
/* Each channel a call is about: one, or every one (FSOUND_ALL). */
#define EACH(h, c) for (int i_ = 0, all_ = (int)(h) == CH_ALL; i_ < (all_ ? g_nch : 1); i_++) \
    for (chan_t *c = all_ ? (g_ch[i_].used ? &g_ch[i_] : NULL) : chan((int)(h)); c; c = NULL)

static int play(int want, sample_t *s, stream_t *t, int paused) {
    int pick = -1, playing = 0;
    for (int i = 0; i < g_nch; i++) playing += g_ch[i].used && g_ch[i].s == s;
    if (!t && playing >= s->max_playbacks) return -1;
    if (want >= 0 && (want & 0xFFF) < g_nch) pick = want & 0xFFF;
    for (int i = 0; i < g_nch && pick < 0; i++) if (!g_ch[i].used) pick = i;
    if (pick < 0) {                          /* none free: the least important's, the oldest of those */
        for (int i = 0; i < g_nch; i++)
            if (!g_ch[i].stream && (pick < 0 || g_ch[i].pri < g_ch[pick].pri || (g_ch[i].pri == g_ch[pick].pri && g_ch[i].age < g_ch[pick].age))) pick = i;
        if (pick < 0 || g_ch[pick].pri > s->def_pri) return -1;
    }
    chan_t *c = &g_ch[pick];
    int gen = c->gen + 1;
    memset(c, 0, sizeof *c);
    c->used = c->fresh = 1, c->gen = gen, c->paused = paused, c->s = s, c->stream = t, c->age = ++g_age;
    c->vol = s->def_vol, c->pan = s->channels > 1 && s->def_pan == 128 ? STEREOPAN : s->def_pan, c->freq = s->def_freq, c->pri = s->def_pri;
    memcpy(c->at, g_ear.pos, sizeof c->at);
    if (t) rewind_stream(t), fill(t, 4096);
    return handle_of(pick);
}

/* ---- the calls ------------------------------------------------------------------ */
#define X(name) static void f_##name(void)
#define LOCKED(...) do { pthread_mutex_lock(&g_lock); __VA_ARGS__; pthread_mutex_unlock(&g_lock); } while (0)
static float farg(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static void v3(float *out, uint32_t va) { if (va) for (uint32_t i = 0; i < 3; i++) out[i] = farg(MEM32(va + 4 * i)); }

X(SetHWND) { RET(1, 1); }
X(SetBufferSize) { RET(!g_open, 1); }                    /* before Init, as FMOD has it */
X(SetMaxHardwareChannels) { RET(1, 1); }
X(SetSpeakerMode) { RET(0, 1); }
X(Init) {                                                /* (rate, channels, flags) */
    int rate = (int)A32(0), n = (int)A32(1);
    if (g_open) RET(1, 3);
    g_rate = rate >= 4000 && rate <= 65535 ? rate : 44100;
    g_nch = n < 1 ? 1 : n > MAX_CHANNELS ? MAX_CHANNELS : n;
    memset(g_ch, 0, sizeof g_ch);
    g_open = 1, g_master = 255;
    if (!jpog_headless && !jpog_fsound_offline) {
        SDL_AudioSpec w, have;
        SDL_zero(w);
        w.freq = g_rate, w.format = AUDIO_F32SYS, w.channels = 2, w.samples = 1024, w.callback = audio;
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) g_dev = SDL_OpenAudioDevice(NULL, 0, &w, &have, 0);
        if (g_dev) {
            pthread_t t;
            g_feeding = 1;
            if (pthread_create(&t, NULL, feeder, NULL) == 0) pthread_detach(t);
            SDL_PauseAudioDevice(g_dev, 0);
        }
        fprintf(stderr, "[fmod] %s, %d Hz, %d channels\n", g_dev ? SDL_GetCurrentAudioDriver() : SDL_GetError(), g_rate, g_nch);
    }
    RET(1, 3);
}
X(Close) {
    if (g_dev) SDL_CloseAudioDevice(g_dev), g_dev = 0;
    LOCKED(g_feeding = 0; g_open = 0;
           while (g_streams) drop_stream(g_streams);
           while (g_samples) drop_sample(g_samples);
           for (int i = 0; i < MAX_CHANNELS; i++) g_ch[i].used = 0);
    RET(0, 0);
}
X(Update) {                                              /* the streams that have ended say so */
    for (;;) {
        uint32_t cb = 0, a[4] = { 0 };
        LOCKED(for (stream_t *t = g_streams; t && !cb; t = t->next) {
            int on = 0;
            for (int i = 0; i < g_nch; i++) on |= g_ch[i].used && g_ch[i].stream == t;
            if (!on && t->ended && !t->told) {
                t->told = 1;
                if (t->callback) cb = t->callback, a[0] = t->guest, a[3] = t->param;
            }
        });
        if (!cb) break;
        hle_call_guest(cb, 4, a);                        /* (stream, NULL, 0, param): it may close it, or play another */
    }
    RET(0, 0);
}
X(GetMaxChannels) { RET(g_nch, 0); }
X(GetChannelsPlaying) { int n = 0; LOCKED(for (int i = 0; i < g_nch; i++) n += g_ch[i].used); RET(n, 0); }
X(GetMemoryStats) { if (A32(0)) MEM32(A32(0)) = (uint32_t)g_bytes; if (A32(1)) MEM32(A32(1)) = (uint32_t)g_most; RET(0, 2); }
X(SetSFXMasterVolume) { int v = (int)A32(0); LOCKED(g_master = v < 0 ? 0 : v > 255 ? 255 : v); RET(0, 1); }
X(SetPanSeperation) { LOCKED(g_sep = farg(A32(0))); RET(0, 1); }

X(Sample_Load) {                                         /* (index, name or data, mode, length) */
    uint32_t p = A32(1), mode = A32(2), len = A32(3), va = 0;
    uint8_t *own = NULL;
    const uint8_t *d = NULL;
    size_t n = len;
    if (mode & LOADMEMORY) d = p ? (const uint8_t *)hle_ptr(p) : NULL;
    else d = own = p ? read_whole((const char *)hle_ptr(p), &n) : NULL;
    LOCKED({
        sample_t *s = new_sample((int)mode);
        int ok = 0;
        if (d && (mode & LOADRAW)) {
            s->channels = mode & M_STEREO ? 2 : 1, s->rate = 44100;
            s->pcm = pcm_of(d, n, mode & M_8BITS ? 8 : 16, !(mode & M_UNSIGNED), s->channels, &s->frames);
            ok = 1;
        } else if (d) ok = wav(d, n, s);
        if (ok) loaded(s), va = s->guest;
        else drop_sample(s);
    });
    if (!va) fprintf(stderr, "[fmod] cannot load %s\n", !p || mode & LOADMEMORY ? "(memory)" : (const char *)hle_ptr(p));
    free(own);
    RET(va, 4);
}
X(Sample_Free) { LOCKED({ sample_t *s = sample_at(A32(0)); if (s && !s->stream) drop_sample(s); }); RET(0, 1); }
X(Sample_SetMode) {                                      /* the loop and the 2D bits: the rest is how it was made */
    int ok = 0;
    LOCKED({ sample_t *s = sample_at(A32(0)); const int may = LOOP_OFF | LOOP_NORMAL | LOOP_BIDI | M_2D;
             if (s) s->mode = (s->mode & ~may) | ((int)A32(1) & may), ok = 1; });
    RET(ok, 2);
}
X(Sample_SetMaxPlaybacks) { int ok = 0; LOCKED({ sample_t *s = sample_at(A32(0)); if (s) s->max_playbacks = (int)A32(1), ok = 1; }); RET(ok, 2); }
X(Sample_SetMinMaxDistance) { int ok = 0; LOCKED({ sample_t *s = sample_at(A32(0)); if (s) s->min = farg(A32(1)), s->max = farg(A32(2)), ok = 1; }); RET(ok, 3); }
X(Sample_SetDefaults) {                                  /* (sample, freq, vol, pan, pri), -1 as they are */
    int ok = 0;
    LOCKED({ sample_t *s = sample_at(A32(0));
             if (s) {
                 ok = 1;
                 if ((int)A32(1) >= 0) s->def_freq = (int)A32(1);
                 if ((int)A32(2) >= 0) s->def_vol = (int)A32(2) > 255 ? 255 : (int)A32(2);
                 if ((int)A32(3) >= 0) s->def_pan = (int)A32(3) > 255 ? 255 : (int)A32(3);
                 if ((int)A32(4) >= 0) s->def_pri = (int)A32(4) > 255 ? 255 : (int)A32(4);
             } });
    RET(ok, 5);
}
X(Sample_GetDefaults) {
    int ok = 0;
    sample_t k;
    LOCKED({ sample_t *s = sample_at(A32(0)); if (s) k = *s, ok = 1; });
    if (ok) {
        const int v[4] = { k.def_freq, k.def_vol, k.def_pan, k.def_pri };
        for (int i = 0; i < 4; i++) if (A32(1 + i)) MEM32(A32(1 + i)) = (uint32_t)v[i];
    }
    RET(ok, 5);
}

X(PlaySoundEx) {                                         /* (channel, sample, dsp, start paused) */
    int h = -1;
    LOCKED({ sample_t *s = sample_at(A32(1)); if (s && !s->stream && g_open) h = play((int)A32(0), s, NULL, A32(3) & 0xFF ? 1 : 0); });
    RET((uint32_t)h, 4);
}
X(StopSound) { int ok = 0; LOCKED(EACH(A32(0), c) { c->used = 0, ok = 1; }); RET(ok, 1); }
X(SetPaused) { int ok = 0; LOCKED(EACH(A32(0), c) { c->paused = (A32(1) & 0xFF) != 0, ok = 1; }); RET(ok, 2); }
X(GetPaused) { int v = 0; LOCKED({ chan_t *c = chan((int)A32(0)); if (c) v = c->paused; }); RET(v, 1); }
X(SetVolume) { int ok = 0, v = (int)A32(1); LOCKED(EACH(A32(0), c) { c->vol = v < 0 ? 0 : v > 255 ? 255 : v, ok = 1; }); RET(ok, 2); }
X(GetVolume) { int v = 0; LOCKED({ chan_t *c = chan((int)A32(0)); if (c) v = c->vol; }); RET(v, 1); }
X(SetPan) { int ok = 0, v = (int)A32(1); LOCKED(EACH(A32(0), c) { c->pan = v < 0 ? STEREOPAN : v > 255 ? 255 : v, ok = 1; }); RET(ok, 2); }
X(GetPan) { int v = 0; LOCKED({ chan_t *c = chan((int)A32(0)); if (c) v = c->pan; }); RET((uint32_t)v, 1); }
X(SetFrequency) { int ok = 0, v = (int)A32(1); LOCKED(EACH(A32(0), c) { c->freq = v < 100 ? 100 : v > 705600 ? 705600 : v, ok = 1; }); RET(ok, 2); }
X(GetFrequency) { int v = 0; LOCKED({ chan_t *c = chan((int)A32(0)); if (c) v = c->freq; }); RET(v, 1); }
X(GetCurrentSample) { uint32_t v = 0; LOCKED({ chan_t *c = chan((int)A32(0)); if (c) v = c->s->guest; }); RET(v, 1); }

X(3D_SetAttributes) {                                    /* (channel, position, velocity), either of them NULL */
    int ok = 0;
    float p[3] = { 0 }, v[3] = { 0 };
    v3(p, A32(1)), v3(v, A32(2));
    LOCKED(EACH(A32(0), c) {
        if (A32(1)) memcpy(c->at, p, sizeof p);
        if (A32(2)) memcpy(c->vel, v, sizeof v);
        ok = 1;
    });
    RET(ok, 3);
}
X(3D_Listener_SetAttributes) {                           /* (position, velocity, forward x y z, top x y z) */
    float p[3] = { 0 }, v[3] = { 0 }, d[6];
    v3(p, A32(0)), v3(v, A32(1));
    for (int i = 0; i < 6; i++) d[i] = farg(A32(2 + i));
    LOCKED(if (A32(0)) memcpy(g_ear.pos, p, sizeof p);
           if (A32(1)) memcpy(g_ear.vel, v, sizeof v);
           memcpy(g_ear.fwd, d, 12); memcpy(g_ear.top, d + 3, 12));
    RET(0, 8);
}

X(Stream_OpenFile) {                                     /* (name, mode, length) */
    uint32_t va = 0;
    LOCKED({ stream_t *t = open_stream(ASTR(0), (int)A32(1)); if (t) va = t->guest; });
    if (!va) fprintf(stderr, "[fmod] cannot open %s\n", ASTR(0));
    RET(va, 3);
}
X(Stream_Close) { int ok = 0; LOCKED({ stream_t *t = stream_at(A32(0)); if (t) drop_stream(t), ok = 1; }); RET(ok, 1); }
X(Stream_PlayEx) {                                       /* (channel, stream, dsp, start paused) */
    int h = -1;
    LOCKED({ stream_t *t = stream_at(A32(1));
             if (t && g_open) {
                 for (int i = 0; i < g_nch; i++) if (g_ch[i].used && g_ch[i].stream == t) g_ch[i].used = 0;
                 h = play((int)A32(0), t->sample, t, A32(3) & 0xFF ? 1 : 0);
             } });
    RET((uint32_t)h, 4);
}
X(Stream_SetEndCallback) { int ok = 0; LOCKED({ stream_t *t = stream_at(A32(0)); if (t) t->callback = A32(1), t->param = A32(2), ok = 1; }); RET(ok, 3); }
X(Stream_GetOpenState) { int v = -1; LOCKED(if (stream_at(A32(0))) v = 0); RET((uint32_t)v, 1); }   /* opened before it returned */
X(Stream_GetSample) { uint32_t v = 0; LOCKED({ stream_t *t = stream_at(A32(0)); if (t) v = t->sample->guest; }); RET(v, 1); }
X(Stream_SetBufferSize) { RET(1, 1); }
X(Stream_GetLengthMs) { uint32_t v = 0; LOCKED({ stream_t *t = stream_at(A32(0)); if (t) v = (uint32_t)(t->length * 1000 / (uint64_t)t->sample->rate); }); RET(v, 1); }
X(Stream_GetTime) {
    uint32_t v = 0;
    LOCKED({ stream_t *t = stream_at(A32(0)); if (t && t->length) v = (uint32_t)((t->ended && t->rd >= t->length ? t->rd - (t->rd / t->length - 1) * t->length : t->rd % t->length) * 1000 / (uint64_t)t->sample->rate); });
    RET(v, 1);
}

#define E(name, bytes) { "_FSOUND_" #name "@" #bytes, f_##name }
const win32hle_shim jpog_fsound_shims[] = {
    E(GetPan, 4), E(SetPanSeperation, 4), E(SetPan, 8), E(3D_SetAttributes, 12), E(GetMemoryStats, 8), E(SetSpeakerMode, 4),
    E(Sample_GetDefaults, 20), E(GetCurrentSample, 4), E(3D_Listener_SetAttributes, 32), E(SetFrequency, 8), E(SetSFXMasterVolume, 4),
    E(SetHWND, 4), E(GetFrequency, 4), E(Stream_SetEndCallback, 12), E(Init, 12), E(GetVolume, 4), E(Sample_SetMinMaxDistance, 12),
    E(StopSound, 4), E(Stream_GetOpenState, 4), E(Stream_GetSample, 4), E(PlaySoundEx, 16), E(Sample_SetMode, 8),
    E(Sample_SetMaxPlaybacks, 8), E(Sample_SetDefaults, 20), E(Sample_Load, 16), E(GetMaxChannels, 0), E(GetChannelsPlaying, 0),
    E(Update, 0), E(SetVolume, 8), E(Stream_OpenFile, 12), E(Stream_GetTime, 4), E(Stream_GetLengthMs, 4), E(SetPaused, 8),
    E(Stream_Close, 4), E(Stream_PlayEx, 16), E(Sample_Free, 4), E(SetBufferSize, 4), E(GetPaused, 4), E(Stream_SetBufferSize, 4),
    E(Close, 0), E(SetMaxHardwareChannels, 4), { NULL, NULL } };

/* ---- --listen -------------------------------------------------------------------- */
static uint32_t ask(const char *name, int n, const uint32_t *a) { return hle_call_guest(hle_resolve(name), n, a); }
#define ASK(name, n, ...) ask("_FSOUND_" name, n, (uint32_t[]){ __VA_ARGS__ })
/* One sound or piece of music, by the calls the game makes; its peak and how
 * long it played. Heard unless headless. */
static int listen_to(const char *name, int loud) {
    size_t len = strlen(name);
    int music = len > 4 && !strcasecmp(name + len - 4, ".ogg");
    uint32_t gn = hle_guest_strdup(name), s = music ? ASK("Stream_OpenFile@12", 3, gn, 0x2000, 0) : ASK("Sample_Load@16", 4, 0xFFFFFFFEu, gn, 0x2000, 0);
    hle_free(hle_ptr(gn));
    if (!s) return 0;
    uint32_t ms = music ? ASK("Stream_GetLengthMs@4", 1, s) : 0;
    uint32_t h = music ? ASK("Stream_PlayEx@16", 4, (uint32_t)CH_FREE, s, 0, 0) : ASK("PlaySoundEx@16", 4, (uint32_t)CH_FREE, s, 0, 0);
    float peak = 0, buf[2 * 1024];
    uint64_t frames = 0;
    while (ask("_FSOUND_GetChannelsPlaying@0", 0, NULL)) {
        if (jpog_headless) {
            jpog_fsound_render(buf, 1024);
            keep(buf, 1024);
            for (int i = 0; i < 2048; i++) if (fabsf(buf[i]) > peak) peak = fabsf(buf[i]);
            frames += 1024;
        } else usleep(20000);
        ask("_FSOUND_Update@0", 0, NULL);
    }
    int ok = h != (uint32_t)-1 && (!jpog_headless || (frames && (!music || llabs((long long)(frames * 1000 / (uint64_t)g_rate) - (long long)ms) < 60)));
    if (loud || !ok) fprintf(stderr, "[listen] %s: %s, %.2f s, peak %.3f\n", name, ok ? "ok" : "WRONG", music ? ms / 1000.0 : (double)frames / g_rate, (double)peak);
    if (music) ASK("Stream_Close@4", 1, s);
    else ASK("Sample_Free@4", 1, s);
    return ok;
}
/* A file of the game's, or `all`: every .wav and .ogg under Data, headless. */
static int each(const char *dir, int *bad) {
    char cmd[4200], line[4096];
    int n = 0;
    snprintf(cmd, sizeof cmd, "find '%s' -type f \\( -iname '*.wav' -o -iname '*.ogg' \\) | sort", dir);
    FILE *p = popen(cmd, "r");
    while (p && fgets(line, sizeof line, p)) {
        line[strcspn(line, "\n")] = 0;
        char *at = strstr(line, "Data/");
        n++;
        if (!listen_to(at ? at : line, 0)) (*bad)++;
    }
    if (p) pclose(p);
    return n;
}
int jpog_listen(const char *what) {
    int bad = 0, n = 1, all = !strcmp(what, "all");
    mach_enter();
    if (all) jpog_headless = 1;              /* before Init: no device mixing as well */
    ASK("Init@12", 3, 44100, 32, 0);
    if (all) n = each("Data", &bad);
    else bad = !listen_to(what, 1);
    ask("_FSOUND_Close@0", 0, NULL);
    fprintf(stderr, "[listen] %d played, %d wrong\n", n, bad);
    mach_leave();
    return bad != 0;
}
