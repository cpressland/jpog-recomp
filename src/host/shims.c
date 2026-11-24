/*
 * shims.c - Win32 answers the game's modules need that win32hle has not got,
 * or answers differently. Registered before win32hle's, so these win.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "win32hle.h"
#include "jpog.h"

static recomp_func_t hle_own(const win32hle_shim *t, const char *name) {   /* win32hle's own, under ours */
    for (; t->name; t++) if (!strcmp(t->name, name)) return t->fn;
    hle_fatal("win32hle has no %s", name);
    return NULL;
}

/* ---- modules: the shader DLLs are lifted, loaded by loader.c --------------- */
static void k_LoadLibraryA(void) {
    uint32_t lifted = jpog_load_library(A32(0) ? ASTR(0) : NULL);
    if (lifted) RET(lifted, 1);
    hle_own(win32hle_module, "LoadLibraryA")();
}
static void k_GetProcAddress(void) {                     /* (module, name or ordinal) */
    jpog_module *m = jpog_module_at(A32(0));
    if (m) {
        uint32_t n = A32(1), va = n < 0x10000u ? jpog_export_of(m, NULL, n) : jpog_export_of(m, ASTR(1), 0);
        if (!va) hle_set_last_error(127);                /* ERROR_PROC_NOT_FOUND */
        RET(va, 2);
    }
    hle_own(win32hle_module, "GetProcAddress")();
}
static void k_FreeLibrary(void) {
    if (jpog_free_library(A32(0))) RET(1, 1);
    hle_own(win32hle_module, "FreeLibrary")();
}
static void k_DisableThreadLibraryCalls(void) { RET(1, 1); }

/* ---- W calls on the A ones: a string argument narrowed into scratch -------- */
static uint32_t narrow(uint32_t w) {
    if (w < 0x10000u) return w;                          /* NULL, or a resource's number */
    uint32_t n = 0;
    while (MEM16(w + 2 * n)) n++;
    uint32_t a = hle_scratch(n + 1);
    for (uint32_t i = 0; i < n; i++) { uint16_t c = MEM16(w + 2 * i); MEM8(a + i) = (uint8_t)(c < 256 ? c : '?'); }
    return a;
}
static void as_a(const char *name, int nargs, unsigned strings) {
    uint32_t args[10], mark = hle_scratch_mark();
    for (int i = 0; i < nargs; i++) args[i] = strings & (1u << i) ? narrow(A32(i)) : A32(i);
    uint32_t r = hle_call_guest(hle_resolve(name), nargs, args);
    hle_scratch_release(mark);
    RET(r, nargs);
}
static void k_CreateFileW(void)        { as_a("CreateFileA", 7, 1); }
static void k_FindResourceW(void)      { as_a("FindResourceA", 3, 6); }
static void k_OutputDebugStringW(void) { as_a("OutputDebugStringA", 1, 1); }
static void u_MessageBoxW(void)        { as_a("MessageBoxA", 4, 6); }
static void u_DrawTextW(void)          { as_a("DrawTextA", 5, 2); }

/* ---- the rest ---------------------------------------------------------------- */
static void k_GetConsoleTitleA(void)   { if (A32(1)) MEM8(A32(0)) = 0; RET(0, 2); }
static void k_SetConsoleTitleA(void)   { RET(1, 1); }
/* A thread is the host's, and shares the machine lock: it cannot be stopped
 * from outside. Both fail, as they can on Windows. */
static void k_SuspendThread(void)      { RET(0xFFFFFFFFu, 1); }
static void k_TerminateThread(void)    { RET(0, 2); }
static void u_CreateAcceleratorTableA(void)  { RET(0x7001, 2); }     /* TranslateAccelerator never matches */
static void u_DestroyAcceleratorTable(void)  { RET(1, 1); }
static void u_UnregisterClassA(void)   { RET(1, 2); }
static void g_SetMapMode(void)         { RET(1, 2); }                /* MM_TEXT it was, and stays */
static void g_ExtTextOutA(void) {      /* (dc, x, y, options, rect, text, n, dx): the engine's font textures */
    uint32_t args[5] = { A32(0), A32(1), A32(2), A32(5), A32(6) };
    uint32_t r = A32(6) ? hle_call_guest(hle_resolve("TextOutA"), 5, args) : 1;
    RET(r, 8);
}
const win32hle_shim jpog_shims[] = {
    { "LoadLibraryA", k_LoadLibraryA }, { "GetProcAddress", k_GetProcAddress }, { "FreeLibrary", k_FreeLibrary },
    { "DisableThreadLibraryCalls", k_DisableThreadLibraryCalls },
    { "CreateFileW", k_CreateFileW }, { "FindResourceW", k_FindResourceW },
    { "OutputDebugStringW", k_OutputDebugStringW }, { "MessageBoxW", u_MessageBoxW }, { "DrawTextW", u_DrawTextW },
    { "GetConsoleTitleA", k_GetConsoleTitleA }, { "SetConsoleTitleA", k_SetConsoleTitleA },
    { "SuspendThread", k_SuspendThread }, { "TerminateThread", k_TerminateThread },
    { "CreateAcceleratorTableA", u_CreateAcceleratorTableA }, { "DestroyAcceleratorTable", u_DestroyAcceleratorTable },
    { "UnregisterClassA", u_UnregisterClassA }, { "SetMapMode", g_SetMapMode }, { "ExtTextOutA", g_ExtTextOutA },
    { NULL, NULL }
};
