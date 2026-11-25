/*
 * game.c - Run the actual game (SimJP.exe).
 *
 * Enters SimJP.exe through its real MSVC 6 C runtime start-up
 * (mainCRTStartup at 0x00731118), which runs both _initterm tables
 * (constructing all static globals, including AApplication at 0x008B8580),
 * parses command-line arguments via __getmainargs, and calls main(0x484330).
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32hle.h"
#include "jpog.h"

/* Global AApplication object (vtable 0x00CE6C40 once constructed). */
#define AAPPLICATION_OBJ 0x008B8580u

static int g_run_max_frames;
static int g_run_frames;

void jpog_game_on_present(void) {
    if (g_run_max_frames <= 0) return;
    g_run_frames++;
    if (g_run_frames >= g_run_max_frames) {
        fprintf(stderr, "[game] reached %d frames; exiting 0\n", g_run_frames);
        hle_exit(0);
    }
}

void jpog_game_on_update(void) {}

void jpog_game_log_initterm(const char *tag, uint32_t begin, uint32_t end) {
    int count = 0;
    uint32_t last = 0;
    for (uint32_t p = begin; p < end; p += 4) {
        uint32_t fn = MEM32(p);
        if (fn) { count++; last = fn; }
    }
    fprintf(stderr, "[game] _initterm(%s: 0x%08X..0x%08X): %d entries, last 0x%08X\n",
            tag, begin, end, count, last);
}

void jpog_game_log_main(uint32_t esp) {
    /* Called after `push ebp` at 0x00484330: [esp]=saved_ebp, [esp+4]=retaddr, [esp+8]=argc, [esp+12]=argv */
    uint32_t argc = MEM32(esp + 8);
    uint32_t argv = MEM32(esp + 12);
    uint32_t argv0 = argv ? MEM32(argv) : 0;
    uint32_t argv1 = argv ? MEM32(argv + 4) : 0;
    fprintf(stderr, "[game] entering main(0x00484330): MEM32(0x%08X)=0x%08X, _onexit=%d, argc=%u, argv[0]=\"%s\", argv[1]=0x%08X\n",
            AAPPLICATION_OBJ, MEM32(AAPPLICATION_OBJ), jpog_msvcrt_onexit_count(),
            argc, argv0 ? (const char *)hle_ptr(argv0) : "(null)", argv1);
}

void jpog_game_milestone(int n, const char *name) {
    static uint32_t seen;
    if (n >= 1 && n <= 31 && (seen & (1u << n))) return;
    if (n >= 1 && n <= 31) seen |= (1u << n);
    fprintf(stderr, "[game] milestone %d: %s\n", n, name);
}

int jpog_run_game(int frames) {
    jpog_module *exe = NULL;
    for (int i = 0; i < g_jpog_nmod; i++) {
        if (g_jpog_mod[i].is_exe) {
            exe = &g_jpog_mod[i];
            break;
        }
    }

    if (!exe) {
        fprintf(stderr, "[game] SimJP.exe not found\n");
        return 1;
    }

    if (!exe->img.entry) {
        fprintf(stderr, "[game] SimJP.exe has no entry point\n");
        return 1;
    }

    g_run_max_frames = frames;
    g_run_frames = 0;

    fprintf(stderr, "[game] Entering SimJP.exe mainCRTStartup at 0x%08X\n", exe->img.entry);
    uint32_t result = hle_call_guest(exe->img.entry, 0, NULL);
    fprintf(stderr, "[game] SimJP.exe returned %u\n", result);
    return (int)result;
}
