/*
 * selftest_movies.c - binkw32.dll's calls, as the game makes them, on the
 * title's looping movie (75 frames, 640 by 448, no sound).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "win32hle.h"
#include "jpog.h"

#ifdef HLE_WITH_FFMPEG
static uint32_t ask(const char *name, int n, const uint32_t *a) { return hle_call_guest(hle_resolve(name), n, a); }
#define ASK(name, n, ...) ask("_Bink" name, n, (uint32_t[]){ __VA_ARGS__ })
static void is(const char *what, uint32_t got, uint32_t want) {
    char b[48], n[96];
    snprintf(b, sizeof b, "%08X, not %08X", got, want), snprintf(n, sizeof n, "bink: %s", what);
    jpog_check(got == want, n, b);
}
static uint32_t sum(uint32_t buf, uint32_t words) {
    uint32_t s = 2166136261u;
    for (uint32_t i = 0; i < words; i++) s = (s ^ MEM32(buf + 4 * i)) * 16777619u + 1;
    return s;
}
static uint32_t picture(uint32_t b, uint32_t buf) {
    memset(hle_ptr(buf), 0, 640 * 448 * 4);
    ASK("CopyToBuffer@28", 7, b, buf, 640 * 4, 448, 0, 0, 3);
    return sum(buf, 640 * 448);
}
void jpog_selftest_movies(void) {
    uint32_t name = hle_guest_strdup("Data\\Movies\\JPGtloop.bik"), none = hle_guest_strdup("Data\\Movies\\None.bik");
    uint32_t buf = hle_va(hle_alloc(640 * 448 * 4)), part = hle_va(hle_alloc(640 * 448 * 4));
    is("SetSoundSystem", ASK("SetSoundSystem@8", 2, hle_resolve("_BinkOpenDirectSound@4"), 0), 1);
    is("a movie that is not there", ASK("Open@8", 2, none, 0), 0);
    uint32_t b = ASK("Open@8", 2, name, 0);
    jpog_check(b != 0, "bink: Open", "0");
    if (!b) return;
    is("Width", MEM32(b), 640), is("Height", MEM32(b + 4), 448), is("Frames", MEM32(b + 8), 75), is("FrameNum", MEM32(b + 12), 1);
    is("the rate", MEM32(b + 20) / MEM32(b + 24), 29);
    ASK("DoFrame@4", 1, b);
    uint32_t first = picture(b, buf);
    jpog_check((MEM32(buf) >> 24) == 0xFF, "bink: 32-bit is opaque", "not");

    /* a rectangle of it, where the whole has it */
    memset(hle_ptr(part), 0, 640 * 448 * 4);
    ASK("CopyToBufferRect@44", 11, b, part, 640 * 4, 448, 10, 20, 300, 200, 64, 32, 3);
    int same = 1, outside = 1;
    for (uint32_t y = 0; y < 448; y++)
        for (uint32_t x = 0; x < 640; x++) {
            uint32_t v = MEM32(part + 4 * (y * 640 + x));
            if (x >= 10 && x < 74 && y >= 20 && y < 52) same &= v == MEM32(buf + 4 * ((y - 20 + 200) * 640 + x - 10 + 300));
            else outside &= v == 0;
        }
    jpog_check(same, "bink: CopyToBufferRect", "another part"), jpog_check(outside, "bink: ...no more than it", "more");
    ASK("CopyToBufferRect@44", 11, b, part, 640 * 4, 448, 600, 440, 600, 440, 640, 448, 3);      /* off two edges: clipped */
    ASK("CopyToBuffer@28", 7, b, part, 640 * 2, 448, 0, 0, 10);
    uint32_t p = MEM32(buf + 4 * (300 * 640 + 320));
    is("565", MEM16(part + 2 * (300 * 640 + 320)), (p >> 19 & 31) << 11 | (p >> 10 & 63) << 5 | (p >> 3 & 31));

    /* paced, and round again */
    ASK("NextFrame@4", 1, b);
    is("too soon", ASK("Wait@4", 1, b), 1);
    usleep(60000);
    is("then not", ASK("Wait@4", 1, b), 0);
    ASK("DoFrame@4", 1, b);
    jpog_check(picture(b, buf) != first, "bink: the second frame", "the first");
    for (int i = 2; i < 75; i++) ASK("NextFrame@4", 1, b), ASK("DoFrame@4", 1, b);
    is("the last", MEM32(b + 12), 75), is("LastFrameNum", MEM32(b + 16), 75);
    ASK("NextFrame@4", 1, b);
    is("and the first again", MEM32(b + 12), 1);
    ASK("DoFrame@4", 1, b);
    is("...the same picture", picture(b, buf), first);
    is("SetSoundOnOff", ASK("SetSoundOnOff@8", 2, b, 0), 0);
    ASK("Service@4", 1, b);
    ASK("Close@4", 1, b);
    hle_free(hle_ptr(buf)), hle_free(hle_ptr(part)), hle_free(hle_ptr(name)), hle_free(hle_ptr(none));
}
#else
void jpog_selftest_movies(void) { }
#endif
