/*
 * Jurassic Park: Operation Genesis - the native host, for macOS.
 *
 * The lifted C of the game's modules (run_lift.py) on pcrecomp's
 * runtime/win32hle: each is mapped and bound to the others (loader.c), and
 * every other import is win32hle's or this host's (msvcrt.c, shims.c). The
 * guest's memory is one 4 GB block of the host's, the game folder drive C:.
 *
 * So far it runs the DLLs' start-up and checks them (--selftest): the
 * window, Direct3D 8 and the game's own entry are to come (docs/PLAN.md).
 */
#define _GNU_SOURCE
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "win32hle.h"
#include "host.h"
#include "jpog.h"

int jpog_verbose;
int jpog_headless = 1;                   /* until there is a window to open */

/* A function the lift left as a stub (run_lift.py writes none, today). */
void recomp_not_lifted(uint32_t va) { hle_fatal("sub_%08X was not lifted", va); }

static void report(const char *why) {
    fprintf(stderr, "\n=== %s ===\n  in lifted sub_%08X, last import %s\n", why, g_cur_func,
            g_cur_import ? g_cur_import : "(none)");
    fprintf(stderr, "  eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X esi=%08X edi=%08X\n",
            g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi);
    fprintf(stderr, "last indirect calls (newest first):\n");
    for (int i = 1; i <= 24 && i <= (int)g_icall_trace_idx; i++) {
        uint32_t k = (g_icall_trace_idx - i) & (ICALL_TRACE_SIZE - 1);
        const char *nm = hle_name(g_icall_trace[k]);
        fprintf(stderr, "  0x%08X  from 0x%08X  %s\n", g_icall_trace[k], g_icall_from[k], nm ? nm : "");
    }
}
static void on_fault(int sig, siginfo_t *si, void *uc) {
    static volatile int once;
    (void)uc;
    if (__sync_lock_test_and_set(&once, 1)) _exit(3);
    char why[96];
    uintptr_t a = (uintptr_t)si->si_addr, base = (uintptr_t)g_mem_base;
    if (a >= base && a - base < 0x100000000ull)
        snprintf(why, sizeof why, "fault: signal %d at guest 0x%08X", sig, (uint32_t)(a - base));
    else
        snprintf(why, sizeof why, "fault: signal %d, host address %p", sig, si->si_addr);
    report(why);
    _exit(3);
}

__attribute__((visibility("default"))) int jpog_main(int argc, char **argv) {
    const char *game = "game";
    const char *view = NULL, *keys = NULL, *materials = NULL, *shot = NULL;
    int selftest = 0, anim = -1, frames = 0, windowed = 0, headless = 0;
    setvbuf(stderr, NULL, _IONBF, 0);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--game") && i + 1 < argc) game = argv[++i];
        else if (!strcmp(argv[i], "--selftest")) selftest = 1;
        else if (!strcmp(argv[i], "--view") && i + 1 < argc) view = argv[++i];
        else if (!strcmp(argv[i], "--keys") && i + 1 < argc) keys = argv[++i];
        else if (!strcmp(argv[i], "--materials") && i + 1 < argc) materials = argv[++i];
        else if (!strcmp(argv[i], "--anim") && i + 1 < argc) anim = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) {
            static char path[PATH_MAX * 2];  /* absolute: the host goes to the game folder */
            const char *s = argv[++i];
            if (s[0] != '/' && getcwd(path, sizeof path)) snprintf(path + strlen(path), sizeof path - strlen(path), "/%s", s);
            else snprintf(path, sizeof path, "%s", s);
            shot = path;
        }
        else if (!strcmp(argv[i], "--headless")) headless = 1;
        else if (!strcmp(argv[i], "--windowed")) windowed = 1;
        else if (!strcmp(argv[i], "--verbose")) jpog_verbose = 1;
        else if (!strcmp(argv[i], "--trace")) win32hle_trace = 1;
        else {
            printf("usage: jpog [--game DIR] [--selftest | --view MODEL.tmd] [--verbose] [--trace]\n"
                   "  --game DIR   the game folder (default: game)\n"
                   "  --selftest   attach every DLL and check the host and the lifted code against known answers\n"
                   "  --view MODEL.tmd [--keys LIB.tkl] [--materials LIB.tml] [--anim N]   a model, through the engine, turning\n"
                   "               [--headless] [--windowed] [--frames N] [--shot FILE.bmp (the last of N)]\n"
                   "  --verbose    every module, and every stub as it is bound\n"
                   "  --trace      every import call, as it is made\n"
                   "Without --selftest: map and bind the modules, and say what is missing.\n");
            return strcmp(argv[i], "--help") != 0;
        }
    }
    char full[PATH_MAX];
    if (!realpath(game, full)) { fprintf(stderr, "no game folder at %s\n", game); return 1; }
    {   /* faults report on their own stack: a guest stack overflow is one */
        static uint8_t alt[1 << 16];
        stack_t ss = { .ss_sp = alt, .ss_size = sizeof alt };
        sigaltstack(&ss, NULL);
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = on_fault;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        int sigs[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL };
        for (int i = 0; i < 4; i++) sigaction(sigs[i], &sa, NULL);
    }
    win32hle_register(jpog_shims);           /* first: the game's own answers win */
    win32hle_register(jpog_d3d8_shims);
    win32hle_register(jpog_msvcrt_shims);
    win32hle_register(win32hle_eh32);
    recomp_host_init();
    hle_set_drive('C', full);
    hle_set_command_line("\"C:\\SimJP.exe\"");
    /* The modules, before anything allocates: the heap is placed above them. */
    if (jpog_load(full) != 0) return 1;
    printf(JPOG_TITLE " recomp host: %u lifted functions, %d modules, %d imports not implemented yet%s\n",
           recomp_dispatch_count, g_jpog_nmod, g_jpog_stubs, g_jpog_stubs && !jpog_verbose ? " (--verbose lists them)" : "");
    if (!selftest && !view) return 0;
    fflush(stdout);
    if (chdir(full) != 0) { fprintf(stderr, "cannot enter %s\n", full); return 1; }
    int bad = jpog_attach_dlls();
    for (int i = 0; i < g_jpog_nmod; i++)    /* the shader DLLs, as the renderer loads them */
        if (g_jpog_mod[i].on_demand) bad += jpog_load_library(g_jpog_mod[i].file) == 0;
    printf("attached: %d of %d\n", g_jpog_nmod - bad, g_jpog_nmod);
    fflush(stdout);
    if (view) {
        jpog_headless = headless;
        if (hle_screen_open(JPOG_TITLE, !windowed, headless) != 0) return 1;
        int rc = jpog_view(view, keys, materials, anim, shot, frames);
        hle_exit(rc);
    }
    int failed = jpog_selftest();
    if (jpog_verbose) jpog_d3d8_summary();
    printf("selftest: %s\n", failed ? "FAILED" : "ok");
    return failed != 0;
}
