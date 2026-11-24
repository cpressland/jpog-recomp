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

/* The priorities, privileges and cursors SimJP.exe asks about: there are none
 * to change. A privilege is looked up, and not granted, as for a user who has
 * not got it. */
static void k_SetThreadPriorityBoost(void) { RET(1, 2); }
static void k_GetPriorityClass(void)       { RET(0x20, 1); }         /* NORMAL_PRIORITY_CLASS */
static void a_OpenProcessToken(void) {                               /* (process, access, &token): something to close */
    uint32_t h = hle_call_guest(hle_resolve("CreateEventA"), 4, (uint32_t[]){ 0, 1, 0, 0 });
    if (A32(2)) MEM32(A32(2)) = h;
    RET(h != 0, 3);
}
static void a_LookupPrivilegeValueA(void) {                          /* (system, name, &luid) */
    if (A32(2)) MEM32(A32(2)) = 19, MEM32(A32(2) + 4) = 0;
    RET(1, 3);
}
static void a_AdjustTokenPrivileges(void) { hle_set_last_error(1300); RET(1, 6); }   /* ERROR_NOT_ALL_ASSIGNED */
static void u_SetSystemCursor(void)        { RET(1, 2); }
static void u_LoadCursorFromFileA(void)    { uint32_t c = hle_call_guest(hle_resolve("LoadCursorA"), 2, (uint32_t[]){ 0, 32512 }); RET(c, 1); }   /* the arrow */
static void u_CopyImage(void)              { RET(A32(0), 5); }       /* (image, kind, w, h, flags): itself */
static void g_RectVisible(void)            { RET(1, 2); }
static void g_SetStretchBltMode(void) {                              /* (dc, mode) -> the one before */
    static uint32_t mode = 1;                                        /* BLACKONWHITE */
    uint32_t was = mode;
    mode = A32(1);
    RET(was, 2);
}

/* GetDriveTypeA - return CDROM for drive D: */
static void k_GetDriveTypeA(void) {
    const char *path = ASTR(0);
    if (jpog_verbose) {
        fprintf(stderr, "[shim] GetDriveTypeA: path=%s\n", path ? path : "(null)");
    }
    if (path && path[0] && (path[0] == 'D' || path[0] == 'd')) {
        if (jpog_verbose) {
            fprintf(stderr, "[shim] GetDriveTypeA: returning DRIVE_CDROM (5)\n");
        }
        RET(5, 1);  /* DRIVE_CDROM */
    }
    /* Fall through to default implementation */
    hle_own(win32hle_kernel32_ext, "GetDriveTypeA")();
}

/* GetVolumeInformationA - return volume label "JPOG" for drive D: */
static void k_GetVolumeInformationA(void) {
    const char *path = ASTR(0);
    uint32_t volume_name = A32(1);
    uint32_t volume_name_size = A32(2);
    
    if (jpog_verbose) {
        fprintf(stderr, "[shim] GetVolumeInformationA: path=%s\n", path ? path : "(null)");
    }
    
    if (path && path[0] && (path[0] == 'D' || path[0] == 'd')) {
        /* Return volume label "JPOG" */
        if (volume_name && volume_name_size > 0) {
            const char *label = "JPOG";
            uint32_t len = strlen(label);
            if (len >= volume_name_size) len = volume_name_size - 1;
            memcpy(hle_ptr(volume_name), label, len);
            ((char *)hle_ptr(volume_name))[len] = '\0';
            if (jpog_verbose) {
                fprintf(stderr, "[shim] GetVolumeInformationA: returning label 'JPOG'\n");
            }
        }
        RET(1, 8);  /* Success */
    }
    /* Fall through to default implementation */
    hle_own(win32hle_kernel32_ext, "GetVolumeInformationA")();
}

/* GetLogicalDriveStringsA - include D: as a CDROM drive */
static void k_GetLogicalDriveStringsA(void) {
    uint32_t buffer_size = A32(0);
    uint32_t buffer = A32(1);
    
    if (jpog_verbose) {
        fprintf(stderr, "[shim] GetLogicalDriveStringsA: buffer_size=%u\n", buffer_size);
    }
    
    /* Return C:\ and D:\ as available drives */
    const char *drives = "C:\\\0D:\\\0";
    uint32_t len = 8;  /* "C:\\\0D:\\\0" */
    
    if (buffer && buffer_size >= len) {
        memcpy(hle_ptr(buffer), drives, len);
        RET(len, 2);  /* Return length including null terminators */
    } else {
        RET(len, 2);  /* Return required size */
    }
}

/* CreateFileA - trace file opens for debugging */
static void k_CreateFileA(void) {
    const char *filename = ASTR(0);
    if (filename && filename[0] == 'D' && jpog_verbose) {
        fprintf(stderr, "[shim] CreateFileA: %s\n", filename);
    }
    hle_own(win32hle_kernel32_ext, "CreateFileA")();
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
    { "SetThreadPriorityBoost", k_SetThreadPriorityBoost }, { "GetPriorityClass", k_GetPriorityClass },
    { "OpenProcessToken", a_OpenProcessToken }, { "LookupPrivilegeValueA", a_LookupPrivilegeValueA },
    { "AdjustTokenPrivileges", a_AdjustTokenPrivileges }, { "SetSystemCursor", u_SetSystemCursor },
    { "LoadCursorFromFileA", u_LoadCursorFromFileA }, { "CopyImage", u_CopyImage },
    { "RectVisible", g_RectVisible }, { "SetStretchBltMode", g_SetStretchBltMode },
    { "GetDriveTypeA", k_GetDriveTypeA }, { "GetVolumeInformationA", k_GetVolumeInformationA },
    { "GetLogicalDriveStringsA", k_GetLogicalDriveStringsA },
    { "CreateFileA", k_CreateFileA },
    { NULL, NULL }
};
