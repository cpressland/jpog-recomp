/*
 * selftest_fsound.c - fmod.dll's calls, as the game makes them (by their
 * import names), against a mixer with no sound device: what is mixed is asked
 * for and measured.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32hle.h"
#include "jpog.h"

static uint32_t ask(const char *name, int n, const uint32_t *a) { return hle_call_guest(hle_resolve(name), n, a); }
#define ASK(name, n, ...) ask("_FSOUND_" name, n, (uint32_t[]){ __VA_ARGS__ })
#define ASK0(name) ask("_FSOUND_" name "@0", 0, NULL)
#define FREE 0xFFFFFFFFu
#define ALL 0xFFFFFFFDu
static uint32_t F(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

static void is(const char *what, double got, double want, double within) {
    char b[64], n[96];
    snprintf(b, sizeof b, "%g, not %g", got, want), snprintf(n, sizeof n, "fmod: %s", what);
    jpog_check(fabs(got - want) <= within, n, b);
}
/* The loudest of each side, and how often the left rises through zero, over some frames. */
static float g_l, g_r;
static int g_rises;
static void hear(int frames) {
    static float buf[2 * 44100];
    float last = 0;
    g_l = g_r = 0, g_rises = 0;
    jpog_fsound_render(buf, frames);
    for (int i = 0; i < frames; i++) {
        float l = buf[2 * i], r = buf[2 * i + 1];
        if (fabsf(l) > g_l) g_l = fabsf(l);
        if (fabsf(r) > g_r) g_r = fabsf(r);
        if (last < 0 && l >= 0) g_rises++;
        last = l;
    }
}
/* A WAV in the guest's memory: a sine, `hz` at 22050, frames long. */
static uint32_t sine(int hz, int frames, int channels, uint32_t *bytes) {
    uint32_t n = 44 + (uint32_t)(frames * channels * 2), va = hle_va(hle_alloc(n));
    uint8_t *p = hle_ptr(va);
    memcpy(p, "RIFF\0\0\0\0WAVEfmt \20\0\0\0\1\0\0\0\x22\x56\0\0\0\0\0\0\0\0\20\0data", 40);
    p[22] = (uint8_t)channels, p[32] = (uint8_t)(2 * channels);
    uint32_t d = n - 44;
    memcpy(p + 40, &d, 4);
    for (int i = 0; i < frames; i++)
        for (int c = 0; c < channels; c++) {
            int16_t v = (int16_t)(sin(2 * M_PI * hz * i / 22050.0) * 32000);
            memcpy(p + 44 + 2 * (i * channels + c), &v, 2);
        }
    *bytes = n;
    return va;
}
static uint32_t g_ended, g_ended_param;
static void t_end(void) { g_ended = A32(0), g_ended_param = A32(3); RET(1, 4); }
static const win32hle_shim t_shims[] = { { "jpog_selftest_stream_end", t_end }, { NULL, NULL } };

void jpog_selftest_fsound(void) {
    uint32_t mark = hle_scratch_mark(), len, p = hle_scratch(32);
    win32hle_register(t_shims);
    jpog_fsound_offline = 1;
    is("Init", ASK("Init@12", 3, 44100, 16, 0), 1, 0);
    is("GetMaxChannels", ASK0("GetMaxChannels"), 16, 0);

    /* a mono sample, 2D */
    uint32_t w = sine(441, 2205, 1, &len), s = ASK("Sample_Load@16", 4, FREE, w, 0x8000 | 0x2000 | 2, len);
    jpog_check(s != 0, "fmod: Sample_Load of memory", "0");
    ASK("Sample_GetDefaults@20", 5, s, p, p + 4, p + 8, p + 12);
    is("its frequency", MEM32(p), 22050, 0), is("its volume", MEM32(p + 4), 255, 0), is("its priority", MEM32(p + 12), 128, 0);
    uint32_t h = ASK("PlaySoundEx@16", 4, FREE, s, 0, 1);
    is("started paused", ASK("GetPaused@4", 1, h), 1, 0);
    hear(4410);
    is("silent while paused", g_l, 0, 0);
    ASK("SetPaused@8", 2, h, 0);
    hear(44100);
    is("in the middle, left", g_l, 0.4883, 0.01), is("in the middle, right", g_r, 0.4883, 0.01), is("a looped 441 Hz", g_rises, 441, 2);
    ASK("SetPan@8", 2, h, 0); hear(64); hear(4410);
    is("panned left, left", g_l, 0.977, 0.01), is("panned left, right", g_r, 0, 0.001), is("GetPan", ASK("GetPan@4", 1, h), 0, 0);
    ASK("SetPan@8", 2, h, 255); ASK("SetVolume@8", 2, h, 128); hear(64); hear(4410);
    is("panned right at half, left", g_l, 0, 0.001), is("panned right at half, right", g_r, 0.49, 0.01);
    ASK("SetSFXMasterVolume@4", 1, 128); hear(64); hear(4410);
    is("with the master at half", g_r, 0.246, 0.01), is("GetVolume is the channel's own", ASK("GetVolume@4", 1, h), 128, 0);
    ASK("SetSFXMasterVolume@4", 1, 255);
    ASK("SetFrequency@8", 2, h, 44100); ASK("SetPan@8", 2, h, 0); hear(64); hear(44100);
    is("twice the frequency", g_rises, 882, 2), is("GetFrequency", ASK("GetFrequency@4", 1, h), 44100, 0);
    is("GetCurrentSample", ASK("GetCurrentSample@4", 1, h), s, 0);
    is("GetChannelsPlaying", ASK0("GetChannelsPlaying"), 1, 0);
    is("StopSound", ASK("StopSound@4", 1, h), 1, 0), is("...and again", ASK("StopSound@4", 1, h), 0, 0);
    is("none playing", ASK0("GetChannelsPlaying"), 0, 0);

    /* once, its handle gone with it */
    ASK("Sample_SetMode@8", 2, s, 1);
    h = ASK("PlaySoundEx@16", 4, FREE, s, 0, 0);
    hear(5000);
    is("a sound ends", ASK0("GetChannelsPlaying"), 0, 0);
    uint32_t h2 = ASK("PlaySoundEx@16", 4, FREE, s, 0, 0);
    jpog_check(h2 != h && ASK("SetVolume@8", 2, h, 0) == 0, "fmod: an old handle does nothing", "it did");
    ASK("Sample_SetMaxPlaybacks@8", 2, s, 2);
    jpog_check(ASK("PlaySoundEx@16", 4, FREE, s, 0, 0) != FREE && ASK("PlaySoundEx@16", 4, FREE, s, 0, 0) == FREE, "fmod: no more than its playbacks", "more");
    ASK("Sample_SetMaxPlaybacks@8", 2, s, 100);
    for (int i = 0; i < 20; i++) h = ASK("PlaySoundEx@16", 4, FREE, s, 0, 0);
    is("a full house", ASK0("GetChannelsPlaying"), 16, 0);
    jpog_check(h != FREE, "fmod: the oldest makes way", "none did");
    ASK("Sample_SetDefaults@20", 5, s, FREE, FREE, FREE, 50);
    jpog_check(ASK("PlaySoundEx@16", 4, FREE, s, 0, 0) == FREE, "fmod: one of less importance does not", "it did");
    ASK("StopSound@4", 1, ALL);
    is("all stopped", ASK0("GetChannelsPlaying"), 0, 0);
    ASK("Sample_Free@4", 1, s);
    is("a freed sample does not play", ASK("PlaySoundEx@16", 4, FREE, s, 0, 0), FREE, 0);

    /* stereo */
    w = sine(441, 2205, 2, &len), s = ASK("Sample_Load@16", 4, FREE, w, 0x8000 | 0x2000 | 2, len);
    ASK("PlaySoundEx@16", 4, FREE, s, 0, 0); hear(4410);
    is("stereo as it is", g_l, 0.977, 0.01);
    ASK("StopSound@4", 1, ALL);

    /* 3D: at ten units, five to the right, behind; from an ear that looks along z */
    w = sine(441, 22050, 1, &len), s = ASK("Sample_Load@16", 4, FREE, w, 0x8000 | 0x1000 | 2, len);
    ASK("Sample_SetMinMaxDistance@12", 3, s, F(10), F(100));
    uint32_t zero = hle_scratch(12), at = hle_scratch(12), vel = hle_scratch(12);
    for (uint32_t i = 0; i < 3; i++) MEM32(zero + 4 * i) = MEM32(at + 4 * i) = MEM32(vel + 4 * i) = 0;
    ASK("3D_Listener_SetAttributes@32", 8, zero, zero, F(0), F(0), F(1), F(0), F(1), F(0));
    h = ASK("PlaySoundEx@16", 4, FREE, s, 0, 0);
    MEM32(at + 8) = F(10); ASK("3D_SetAttributes@12", 3, h, at, zero); hear(64); hear(4410);
    float near_ = g_l;
    is("ahead: both sides", g_l, 0.69, 0.01), is("ahead: the same", g_r, g_l, 0.001);
    MEM32(at + 8) = F(20); ASK("3D_SetAttributes@12", 3, h, at, zero); hear(64); hear(4410);
    is("twice as far, half as loud", g_l, near_ / 2, 0.01);
    MEM32(at + 8) = F(1000); ASK("3D_SetAttributes@12", 3, h, at, zero); hear(64); hear(4410);
    is("no quieter than its maximum", g_l, near_ / 10, 0.01);
    MEM32(at + 8) = 0, MEM32(at) = F(10); ASK("3D_SetAttributes@12", 3, h, at, zero); hear(64); hear(4410);
    is("to the right: right", g_r, 0.977, 0.01), is("to the right: left", g_l, 0, 0.01);
    ASK("3D_Listener_SetAttributes@32", 8, zero, zero, F(0), F(0), F(-1), F(0), F(1), F(0)); hear(64); hear(4410);
    is("turned about, to the left", g_l, 0.977, 0.01);
    MEM32(at) = F(0), MEM32(at + 8) = F(-10), MEM32(vel + 8) = F(34);      /* coming: the ear looks along -z */
    ASK("3D_SetAttributes@12", 3, h, at, vel); hear(64); hear(44100);
    is("coming nearer, higher", g_rises, 441 * 340 / 306.0, 4);
    ASK("StopSound@4", 1, ALL);

#ifdef JPOG_VORBIS
    /* a stream */
    uint32_t name = hle_guest_strdup("Data\\Music\\Victory.ogg"), st = ASK("Stream_OpenFile@12", 3, name, 0x2000, 0);
    jpog_check(st != 0, "fmod: Stream_OpenFile", "0");
    if (st) {
        uint32_t ms = ASK("Stream_GetLengthMs@4", 1, st);
        jpog_check(ms > 1000, "fmod: Stream_GetLengthMs", "short");
        is("Stream_GetOpenState", ASK("Stream_GetOpenState@4", 1, st), 0, 0);
        ASK("Stream_SetEndCallback@12", 3, st, hle_resolve("jpog_selftest_stream_end"), 77);
        h = ASK("Stream_PlayEx@16", 4, FREE, st, 0, 0);
        for (int i = 0; i < 4; i++) hear(44100);
        is("four seconds in", ASK("Stream_GetTime@4", 1, st), 4000, 3);
        jpog_check(g_l > 0.01f, "fmod: music", "silent");
        ASK0("Update");
        is("not ended yet", g_ended, 0, 0);
        for (uint32_t t = 4000; t < ms + 2000; t += 1000) hear(44100);
        is("it stops", ASK0("GetChannelsPlaying"), 0, 0);
        ASK0("Update");
        is("its end is told", g_ended, st, 0), is("...with what it was given", g_ended_param, 77, 0);
        g_ended = 0;
        ASK0("Update");
        is("...once", g_ended, 0, 0);
        h = ASK("Stream_PlayEx@16", 4, FREE, st, 0, 0);
        hear(44100);
        is("from the start again", ASK("Stream_GetTime@4", 1, st), 1000, 3);
        is("Stream_Close", ASK("Stream_Close@4", 1, st), 1, 0), is("...stops it", ASK0("GetChannelsPlaying"), 0, 0);
    }
    hle_free(hle_ptr(name));
#endif
    ASK0("Close");
    ASK("GetMemoryStats@8", 2, p, p + 4);
    is("nothing kept", MEM32(p), 0, 0);
    hle_scratch_release(mark);
}
