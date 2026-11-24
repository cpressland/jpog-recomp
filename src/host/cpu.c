/*
 * cpu.c - the CPU the modules are told they run on. Every lifted cpuid comes
 * here (run_lift.py CPUID_HOST): libmmd picks its maths by it, and the skin
 * and terrain shader DLLs their vertex code.
 *
 * A Pentium III, as stl-recomp reports: FPU, TSC, CX8, CMOV, MMX and SSE, no
 * SSE2 and no 3DNow!. JPOG_CPU=x87 says no SSE either.
 */
#include <stdlib.h>
#include <string.h>

#include "win32hle.h"
#include "jpog.h"

void jpog_cpuid(void) {
    static int sse = -1;
    if (sse < 0) { const char *c = getenv("JPOG_CPU"); sse = !(c && !strcmp(c, "x87")); }
    uint32_t leaf = g_eax;
    g_eax = g_ebx = g_ecx = g_edx = 0;
    if (leaf == 0) {                             /* the highest leaf, and the vendor */
        g_eax = 1;
        memcpy(&g_ebx, "Genu", 4);
        memcpy(&g_edx, "ineI", 4);
        memcpy(&g_ecx, "ntel", 4);
    } else if (leaf == 1) {                      /* family 6, model 7, stepping 3 */
        g_eax = 0x00000673u;
        g_edx = 1u << 0 | 1u << 4 | 1u << 8 | 1u << 15 | 1u << 23 | (sse ? 1u << 24 | 1u << 25 : 0);
    } else if (leaf == 0x80000000u) {            /* no extended leaves: no 3DNow! */
        g_eax = 0x80000000u;
    }
}
