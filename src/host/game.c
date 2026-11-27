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

/* Optional scripted input for headless gameplay tests (JPOG_AUTO env var):
 * comma-separated steps: FRAME:esc | FRAME:enter | FRAME:space |
 * FRAME:move:DX:DY | FRAME:click | FRAME:shot:PATH */
typedef struct {
    int frame;
    char op[16];
    int a, b;
    char path[256];
} auto_step_t;
static auto_step_t g_auto[64];
static int g_nauto;
static int g_auto_init;

static void auto_parse(void) {
    g_auto_init = 1;
    const char *env = getenv("JPOG_AUTO");
    if (!env || !*env) return;
    char buf[2048];
    snprintf(buf, sizeof buf, "%s", env);
    for (char *tok = strtok(buf, ","); tok && g_nauto < 64; tok = strtok(NULL, ",")) {
        auto_step_t *s = &g_auto[g_nauto];
        memset(s, 0, sizeof *s);
        char *c1 = strchr(tok, ':');
        if (!c1) continue;
        *c1++ = 0;
        s->frame = atoi(tok);
        char *c2 = strchr(c1, ':');
        if (c2) *c2++ = 0;
        snprintf(s->op, sizeof s->op, "%s", c1);
        if (!strcmp(s->op, "move") && c2) {
            sscanf(c2, "%d:%d", &s->a, &s->b);
        } else if (!strcmp(s->op, "shot") && c2) {
            snprintf(s->path, sizeof s->path, "%s", c2);
        }
        g_nauto++;
    }
}

static void auto_tick(int f) {
    if (!g_auto_init) auto_parse();
    for (int i = 0; i < g_nauto; i++) {
        auto_step_t *s = &g_auto[i];
        if (!strcmp(s->op, "esc") || !strcmp(s->op, "enter") || !strcmp(s->op, "space") ||
            !strcmp(s->op, "right") || !strcmp(s->op, "left") || !strcmp(s->op, "up") ||
            !strcmp(s->op, "down") || !strcmp(s->op, "tab") || !strcmp(s->op, "b")) {
            uint32_t vk = 0, sc = 0;
            if (!strcmp(s->op, "esc"))        { vk = 0x1Bu; sc = 0x01u; }
            else if (!strcmp(s->op, "enter")) { vk = 0x0Du; sc = 0x1Cu; }
            else if (!strcmp(s->op, "space")) { vk = 0x20u; sc = 0x39u; }
            else if (!strcmp(s->op, "right")) { vk = 0x27u; sc = 0xCDu; }
            else if (!strcmp(s->op, "left"))  { vk = 0x25u; sc = 0xCBu; }
            else if (!strcmp(s->op, "up"))    { vk = 0x26u; sc = 0xC8u; }
            else if (!strcmp(s->op, "down"))  { vk = 0x28u; sc = 0xD0u; }
            else if (!strcmp(s->op, "tab"))   { vk = 0x09u; sc = 0x0Fu; }
            else if (!strcmp(s->op, "b"))     { vk = 0x42u; sc = 0x30u; }
            if (f == s->frame) hle_input_key(1, vk, sc);
            else if (f == s->frame + 2) hle_input_key(0, vk, sc);
        } else if (!strcmp(s->op, "move")) {
            if (f == s->frame) hle_input_motion(s->a, s->b);
        } else if (!strcmp(s->op, "click")) {
            int x, y;
            hle_input_position(&x, &y);
            if (f == s->frame) hle_input_mouse(0x0201u /* WM_LBUTTONDOWN */, x, y);
            else if (f == s->frame + 2) hle_input_mouse(0x0202u /* WM_LBUTTONUP */, x, y);
        } else if (!strcmp(s->op, "shot")) {
            if (f == s->frame) {
                jpog_d3d8_shot(s->path);
                fprintf(stderr, "[game] shot frame %d -> %s\n", f, s->path);
            }
        }
    }
}

void jpog_game_on_present(void) {
    g_run_frames++;
    auto_tick(g_run_frames);
    if (g_run_max_frames > 0 && g_run_frames >= g_run_max_frames) {
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
