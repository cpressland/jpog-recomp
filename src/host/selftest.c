/*
 * selftest.c - the host and the lifted code against known answers
 * (jpog --selftest): msvcrt.c's MSVC rules, then libmmd's functions against
 * the host's libm, then Toshi's own maths.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "win32hle.h"
#include "jpog.h"

static int g_failed, g_checks;
void jpog_check(int ok, const char *what, const char *got) {
    g_checks++;
    if (ok) return;
    g_failed++;
    fprintf(stderr, "[selftest] FAILED: %s: got %s\n", what, got);
}

/* ---- msvcrt ---------------------------------------------------------------- */
#define check jpog_check
static uint32_t S(const char *s) { return hle_scratch_copy(s, (uint32_t)strlen(s) + 1); }
static uint32_t W(const char *s) {
    uint32_t n = (uint32_t)strlen(s), w = hle_scratch(2 * n + 2);
    for (uint32_t i = 0; i < n; i++) MEM16(w + 2 * i) = (uint8_t)s[i];
    return w;
}
static const char *from_wide(uint32_t w) {
    static char b[256];
    uint32_t i = 0;
    for (; i < 255 && MEM16(w + 2 * i); i++) b[i] = (char)MEM16(w + 2 * i);
    b[i] = 0;
    return b;
}
static uint32_t crt(const char *name, int n, const uint32_t *a) { return hle_call_guest(hle_resolve(name), n, a); }
typedef union { double d; uint32_t u[2]; } dbl;

static void fmt1(const char *fmt, int n, const uint32_t *v, const char *want) {
    uint32_t mark = hle_scratch_mark(), buf = hle_scratch(256), a[8] = { buf, S(fmt) };
    memcpy(a + 2, v, (size_t)n * 4);
    crt("sprintf", n + 2, a);
    check(!strcmp((char *)hle_ptr(buf), want), fmt, (char *)hle_ptr(buf));
    hle_scratch_release(mark);
}
static void fmtd(const char *fmt, double d, const char *want) { dbl x = { .d = d }; fmt1(fmt, 2, x.u, want); }
static void fmti(const char *fmt, uint32_t v, const char *want) { fmt1(fmt, 1, &v, want); }

static int32_t g_cmp_calls;
static void t_cmp(void) { g_cmp_calls++; int32_t x = (int32_t)MEM32(A32(0)), y = (int32_t)MEM32(A32(1)); RET(x < y ? -1 : x > y, 0); }
static const win32hle_shim t_shims[] = { { "jpog_selftest_cmp", t_cmp }, { NULL, NULL } };

static void test_msvcrt(void) {
    fmti("%d", (uint32_t)-5, "-5");
    fmti("%05d|", (uint32_t)-42, "-0042|");
    fmti("%-6x|", 0xBEEF, "beef  |");
    fmti("%#X", 255, "0XFF");
    fmti("%.3d", 7, "007");
    fmti("%hd", 0x18000, "-32768");
    fmti("%p", 0xABC, "00000ABC");
    fmti("%c%%", 'x', "x%");
    fmt1("%I64d", 2, (uint32_t[]){ 0xFFFFFFFFu, 0x7FFFFFFFu }, "9223372036854775807");
    fmtd("%f", 1.5, "1.500000");
    fmtd("%08.2f", -3.14159, "-0003.14");
    fmtd("%e", 12345.678, "1.234568e+004");
    fmtd("%g", 1e20, "1e+020");
    fmtd("%g", 0.0001, "0.0001");
    fmtd("%f", INFINITY, "1.#INF00");
    fmtd("%.2f", -0.004, "-0.00");

    uint32_t mark = hle_scratch_mark(), buf = hle_scratch(256), a[8];
    char num[32];
    fmt1("%-5s|%.2s", 2, (uint32_t[]){ S("ab"), S("xyz") }, "ab   |xy");
    fmt1("%S", 1, (uint32_t[]){ W("wide") }, "wide");
    memset(hle_ptr(buf), '#', 16);
    int32_t r = (int32_t)crt("_snprintf", 4, (uint32_t[]){ buf, 3, S("%s"), S("abcd") });
    check(r == -1 && !memcmp(hle_ptr(buf), "abc#", 4), "_snprintf overflow", (char *)hle_ptr(buf));
    r = (int32_t)crt("_snprintf", 4, (uint32_t[]){ buf, 4, S("%s"), S("abcd") });
    check(r == 4 && !memcmp(hle_ptr(buf), "abcd#", 5), "_snprintf exact fit", (char *)hle_ptr(buf));
    crt("swprintf", 5, (uint32_t[]){ buf, W("%s-%S-%d"), W("a"), S("b"), 3 });
    check(!strcmp(from_wide(buf), "a-b-3"), "swprintf", from_wide(buf));

    uint32_t v = hle_scratch(64);
    r = (int32_t)crt("sscanf", 6, (uint32_t[]){ S(" 12, 0x1F -2.5e1 word"), S("%d, %x %f %s"), v, v + 4, v + 8, v + 16 });
    float f;
    memcpy(&f, hle_ptr(v + 8), 4);
    snprintf(num, sizeof num, "%d", r);
    check(r == 4 && MEM32(v) == 12 && MEM32(v + 4) == 0x1F && f == -25.0f && !strcmp((char *)hle_ptr(v + 16), "word"), "sscanf", num);
    r = (int32_t)crt("sscanf", 4, (uint32_t[]){ S("key=val;rest"), S("%[^=]=%[a-z]"), v, v + 16 });
    check(r == 2 && !strcmp((char *)hle_ptr(v), "key") && !strcmp((char *)hle_ptr(v + 16), "val"), "sscanf sets", (char *)hle_ptr(v));
    r = (int32_t)crt("sscanf", 3, (uint32_t[]){ S(""), S("%d"), v });
    check(r == -1, "sscanf at the end", "not -1");
    r = (int32_t)crt("swscanf", 4, (uint32_t[]){ W("7 name"), W("%d %s"), v, v + 16 });
    check(r == 2 && MEM32(v) == 7 && !strcmp(from_wide(v + 16), "name"), "swscanf", from_wide(v + 16));

    uint32_t s = S("a,b;;c");
    char out[16] = "";
    for (uint32_t t = crt("strtok", 2, (uint32_t[]){ s, S(",;") }); t; t = crt("strtok", 2, (uint32_t[]){ 0, S(",;") }))
        strncat(out, (char *)hle_ptr(t), 4);
    check(!strcmp(out, "abc"), "strtok", out);
    crt("_splitpath", 5, (uint32_t[]){ S("C:\\dir.x\\sub\\name.ext"), v, v + 4, v + 24, v + 40 });
    snprintf(out, sizeof out, "%s", (char *)hle_ptr(v + 24));
    check(!strcmp((char *)hle_ptr(v), "C:") && !strcmp((char *)hle_ptr(v + 4), "\\dir.x\\sub\\") && !strcmp(out, "name") &&
          !strcmp((char *)hle_ptr(v + 40), ".ext"), "_splitpath", out);
    crt("srand", 1, (uint32_t[]){ 1 });
    uint32_t r1 = crt("rand", 0, a), r2 = crt("rand", 0, a), r3 = crt("rand", 0, a);
    snprintf(num, sizeof num, "%u %u %u", r1, r2, r3);
    check(r1 == 41 && r2 == 18467 && r3 == 6334, "rand from 1", num);
    win32hle_register(t_shims);
    uint32_t arr = hle_scratch(4 * 100), h = 1;
    for (uint32_t i = 0; i < 100; i++) MEM32(arr + 4 * i) = (h = h * 1103515245u + 12345u) >> 20;
    crt("qsort", 4, (uint32_t[]){ arr, 100, 4, hle_resolve("jpog_selftest_cmp") });
    int sorted = 1;
    for (uint32_t i = 1; i < 100; i++) sorted &= MEM32(arr + 4 * i - 4) <= MEM32(arr + 4 * i);
    check(sorted && g_cmp_calls > 100, "qsort", "unsorted");
    MEM32(v) = MEM32(v + 4) = 0;
    for (int i = 0; i < 40; i++) crt("__dllonexit", 3, (uint32_t[]){ 0x1000u + (uint32_t)i, v, v + 4 });
    check(MEM32(v) == 0, "__dllonexit with no table", "made one");
    MEM32(v) = MEM32(v + 4) = crt("malloc", 1, (uint32_t[]){ 16 });
    for (int i = 0; i < 100; i++) crt("__dllonexit", 3, (uint32_t[]){ 0x1000u + (uint32_t)i, v, v + 4 });
    check(MEM32(v + 4) - MEM32(v) == 400 && MEM32(MEM32(v) + 396) == 0x1063, "__dllonexit grows", "wrong table");
    hle_scratch_release(mark);
}

/* ---- lifted code ------------------------------------------------------------- */
static double call_double(uint32_t va, int n, const double *x) {
    mach_enter();
    recomp_func_t f = recomp_lookup(va);
    if (!f) hle_fatal("[selftest] no lifted function at 0x%08X", va);
    uint32_t esp = g_esp;
    for (int i = n - 1; i >= 0; i--) { dbl d = { .d = x[i] }; PUSH32(g_esp, d.u[1]); PUSH32(g_esp, d.u[0]); }
    PUSH32(g_esp, RECOMP_RETADDR);
    f();
    double r = fp_pop();
    g_esp = esp;
    mach_leave();
    return r;
}
static double h_fmod(double a, double b) { return fmod(a, b); }
static void test_libmmd(void) {
    static const struct { const char *name; double (*f1)(double), (*f2)(double, double); double lo, hi; } fn[] = {
        { "sin", sin, NULL, -50, 50 }, { "cos", cos, NULL, -50, 50 }, { "tan", tan, NULL, -1.5, 1.5 },
        { "exp", exp, NULL, -20, 20 }, { "log", log, NULL, 1e-6, 1000 }, { "sqrt", sqrt, NULL, 0, 1e6 },
        { "asin", asin, NULL, -1, 1 }, { "acos", acos, NULL, -1, 1 }, { "atan", atan, NULL, -100, 100 },
        { "floor", floor, NULL, -100, 100 }, { "fabs", fabs, NULL, -100, 100 },
        { "atan2", NULL, atan2, -10, 10 }, { "pow", NULL, pow, 0.01, 8 }, { "fmod", NULL, h_fmod, 0.5, 100 } };
    if (!jpog_module_named("libmmd.dll")) return;
    for (size_t k = 0; k < sizeof fn / sizeof fn[0]; k++) {
        uint32_t va = jpog_export("libmmd.dll", fn[k].name);
        if (!va) { check(0, fn[k].name, "no export"); continue; }
        double worst = 0, at = 0;
        for (int i = 0; i <= 500; i++) {
            double x[2] = { fn[k].lo + (fn[k].hi - fn[k].lo) * i / 500, fn[k].hi - (fn[k].hi - fn[k].lo) * ((i * 37) % 501) / 500 };
            double want = fn[k].f1 ? fn[k].f1(x[0]) : fn[k].f2(x[0], x[1]);
            double got = call_double(va, fn[k].f1 ? 1 : 2, x);
            double err = isnan(got) != isnan(want) ? INFINITY : fabs(got - want) / fmax(1.0, fabs(want));
            if (err > worst) worst = err, at = x[0];
        }
        char b[64];
        snprintf(b, sizeof b, "off by %g at %g", worst, at);
        check(worst < 1e-12, fn[k].name, b);
    }
}

static uint32_t thiscall(uint32_t va, uint32_t self, int n, const uint32_t *a) {
    mach_enter();
    recomp_func_t f = recomp_lookup(va);
    if (!f) hle_fatal("[selftest] no lifted function at 0x%08X", va);
    uint32_t esp = g_esp, ecx = g_ecx;
    for (int i = n - 1; i >= 0; i--) PUSH32(g_esp, a[i]);
    PUSH32(g_esp, RECOMP_RETADDR);
    g_ecx = self;
    f();
    uint32_t r = g_eax;
    g_esp = esp, g_ecx = ecx;
    mach_leave();
    return r;
}
static void test_toshi(void) {
    uint32_t invert = jpog_export("TKernelInterface.dll", "?Invert@TMatrix44@Toshi@@QAE_NXZ");
    if (!jpog_module_named("TKernelInterface.dll")) return;
    if (!invert) { check(0, "TMatrix44::Invert", "no export"); return; }
    static const float src[16] = { 2, 1, 0, 0, -1, 3, 0.5f, 0, 0, 1, 4, 0, 7, -2, 3, 1 };
    uint32_t mark = hle_scratch_mark(), m = hle_scratch(64);
    memcpy(hle_ptr(m), src, 64);
    uint32_t ok = thiscall(invert, m, 0, NULL) & 0xFF;
    float inv[16], worst = 0;
    memcpy(inv, hle_ptr(m), 64);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            float t = 0;
            for (int k = 0; k < 4; k++) t += src[4 * r + k] * inv[4 * k + c];
            worst = fmaxf(worst, fabsf(t - (r == c)));
        }
    char b[64];
    snprintf(b, sizeof b, "%u, off by %g", ok, (double)worst);
    check(worst < 1e-5f, "TMatrix44::Invert", b);
    hle_scratch_release(mark);
}

/* ---- a program's start and end: what MSVC's start-up code calls -------------- */
static char g_order[32];
static int g_pipe = -1;
static void said(char c) { size_t n = strlen(g_order); if (n < sizeof g_order - 1) g_order[n] = c; if (g_pipe >= 0) (void)!write(g_pipe, &c, 1); }
static void t_a(void) { said('a'); RET(0, 0); }
static void t_b(void) { said('b'); RET(0, 0); }
static void t_c(void) {                      /* a constructor that registers its destructor, as they do */
    said('c');
    hle_call_guest(hle_resolve("_onexit"), 1, (uint32_t[]){ hle_resolve("jpog_selftest_b") });
    RET(0, 0);
}
static const win32hle_shim t_start[] = { { "jpog_selftest_a", t_a }, { "jpog_selftest_b", t_b }, { "jpog_selftest_c", t_c }, { NULL, NULL } };

static void test_start(void) {
    uint32_t mark = hle_scratch_mark(), t = hle_scratch(24), argc = hle_scratch(4), argv = hle_scratch(4), env = hle_scratch(4), si = hle_scratch(8);
    char b[64];
    win32hle_register(t_start);
    /* the constructors, in order, the gaps passed over */
    const uint32_t table[6] = { 0, hle_resolve("jpog_selftest_a"), 0, hle_resolve("jpog_selftest_b"), hle_resolve("jpog_selftest_a"), 0 };
    for (uint32_t i = 0; i < 6; i++) MEM32(t + 4 * i) = table[i];
    crt("_initterm", 2, (uint32_t[]){ t, t + 24 });
    check(!strcmp(g_order, "aba"), "_initterm", g_order);
    crt("_initterm", 2, (uint32_t[]){ t, t });
    check(!strcmp(g_order, "aba"), "_initterm of none", g_order);
    memset(g_order, 0, sizeof g_order);
    /* main's arguments */
    MEM32(si) = 0;
    check(crt("__getmainargs", 5, (uint32_t[]){ argc, argv, env, 0, si }) == 0 && MEM32(argc) == 1, "__getmainargs", "failed");
    const char *a0 = MEM32(argc) ? (const char *)hle_ptr(MEM32(MEM32(argv))) : "";
    check(!strcmp(a0, "C:\\SimJP.exe"), "argv[0]", a0);
    check(MEM32(argc) && MEM32(MEM32(argv) + 4 * MEM32(argc)) == 0, "argv ends", "it does not");
    check(MEM32(env) && MEM32(MEM32(env)) == 0, "an environment", "none");
    crt("__set_app_type", 1, (uint32_t[]){ 2 });
    crt("__setusermatherr", 1, (uint32_t[]){ hle_resolve("jpog_selftest_a") });
    uint32_t v[3] = { crt("__p__fmode", 0, NULL), crt("__p__commode", 0, NULL), crt("__p___initenv", 0, NULL) };
    check(v[0] && v[1] && v[2] && v[0] != v[1] && v[1] != v[2], "the start-up's variables", "the same, or none");
    MEM32(v[0]) = 0x4000;
    check(MEM32(crt("__p__fmode", 0, NULL)) == 0x4000, "_fmode is kept", "not");
    MEM32(v[0]) = 0;
    check(jpog_msvcrt_data("_adjust_fdiv") && MEM32(jpog_msvcrt_data("_adjust_fdiv")) == 0, "_adjust_fdiv", "a bad divider");
    /* the FPU: the start-up sets 53 bits; what was is said, and put back */
    uint32_t was = crt("_controlfp", 2, (uint32_t[]){ 0, 0 });
    uint32_t now = crt("_controlfp", 2, (uint32_t[]){ 0x10000, 0x30000 });
    snprintf(b, sizeof b, "%X", now);
    check((now & 0x30000) == 0x10000, "_controlfp, 53 bits", b);
    crt("_controlfp", 2, (uint32_t[]){ was, 0xFFFFF });
    snprintf(b, sizeof b, "%X, not %X", crt("_controlfp", 2, (uint32_t[]){ 0, 0 }), was);
    check(crt("_controlfp", 2, (uint32_t[]){ 0, 0 }) == was, "_controlfp put back", b);
    check(crt("_XcptFilter", 2, (uint32_t[]){ 0xC0000005u, 0 }) == 0, "_XcptFilter passes a fault on", "it did not");
    /* exit: what _onexit was given, last first, then the code; in a process of its own */
    int fd[2], status = 0;
    char got[16] = { 0 };
    if (pipe(fd) == 0) {
        pid_t pid = fork();
        if (pid == 0) {
            g_pipe = fd[1];
            crt("_onexit", 1, (uint32_t[]){ hle_resolve("jpog_selftest_a") });
            MEM32(t) = hle_resolve("jpog_selftest_c");
            crt("_initterm", 2, (uint32_t[]){ t, t + 4 });
            crt("exit", 1, (uint32_t[]){ 7 });
            _exit(99);
        }
        close(fd[1]);
        waitpid(pid, &status, 0);
        (void)!read(fd[0], got, sizeof got - 1);
        close(fd[0]);
        check(!strcmp(got, "cba"), "exit runs what _onexit was given", got);
        snprintf(b, sizeof b, "%d", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        check(WIFEXITED(status) && WEXITSTATUS(status) == 7, "exit's code", b);
    }
    /* what SimJP.exe asks of Windows as it starts, and little else does */
    check(crt("GetPriorityClass", 1, (uint32_t[]){ 0xFFFFFFFFu }) == 0x20, "GetPriorityClass", "not normal");
    check(crt("SetThreadPriorityBoost", 2, (uint32_t[]){ 0xFFFFFFFEu, 1 }) == 1, "SetThreadPriorityBoost", "refused");
    uint32_t tok = hle_scratch(4), luid = hle_scratch(8);
    check(crt("OpenProcessToken", 3, (uint32_t[]){ 0xFFFFFFFFu, 0x28, tok }) == 1 && MEM32(tok), "OpenProcessToken", "none");
    check(crt("LookupPrivilegeValueA", 3, (uint32_t[]){ 0, S("SeShutdownPrivilege"), luid }) == 1, "LookupPrivilegeValueA", "none");
    check(crt("AdjustTokenPrivileges", 6, (uint32_t[]){ MEM32(tok), 0, t, 0, 0, 0 }) == 1 && crt("GetLastError", 0, NULL) == 1300, "AdjustTokenPrivileges", "granted");
    check(crt("CloseHandle", 1, (uint32_t[]){ MEM32(tok) }) == 1, "a token closes", "it does not");
    check(crt("LoadCursorFromFileA", 1, (uint32_t[]){ S("C:\\None.ani") }) != 0 && crt("CopyImage", 5, (uint32_t[]){ 77, 2, 0, 0, 0 }) == 77 &&
          crt("SetSystemCursor", 2, (uint32_t[]){ 77, 32512 }) == 1, "cursors", "not");
    check(crt("SetStretchBltMode", 2, (uint32_t[]){ 0, 3 }) == 1 && crt("SetStretchBltMode", 2, (uint32_t[]){ 0, 1 }) == 3 &&
          crt("RectVisible", 2, (uint32_t[]){ 0, t }) == 1, "GDI's modes", "not");
    hle_scratch_release(mark);
}

/* ---- the registry: pcrecomp's, seeded and kept as the game has it ------------ */
static uint32_t query(uint32_t key, const char *name, uint32_t type, uint32_t data, uint32_t size) {
    return crt("RegQueryValueExA", 6, (uint32_t[]){ key, S(name), 0, type, data, size });
}
static void test_registry(void) {
    uint32_t mark = hle_scratch_mark(), out = hle_scratch(4), type = hle_scratch(4), size = hle_scratch(4), buf = hle_scratch(64), jp, jp2;
    const uint32_t HKLM = 0x80000002u, HKCU = 0x80000001u;
    char num[32], path[] = "/tmp/jpog-registry-XXXXXX";
    /* each way the game opens a key, a key's own subkey, whatever the case */
    check(crt("RegOpenKeyExA", 5, (uint32_t[]){ HKLM, S("Software\\BTS\\JP"), 0, 0x20019, out }) == 0, "registry: RegOpenKeyExA", "failed");
    jp = MEM32(out);
    check(crt("RegOpenKeyA", 3, (uint32_t[]){ HKLM, S("SOFTWARE\\BTS"), out }) == 0, "registry: RegOpenKeyA", "failed");
    uint32_t bts = MEM32(out);
    check(crt("RegOpenKeyExA", 5, (uint32_t[]){ bts, S("jp"), 0, 0x20019, out }) == 0, "registry: a key of a key's", "failed");
    jp2 = MEM32(out);
    MEM32(size) = 64;
    check(crt("RegEnumKeyExA", 8, (uint32_t[]){ bts, 0, buf, size, 0, 0, 0, 0 }) == 0 && !strcmp((char *)hle_ptr(buf), "JP"), "registry: RegEnumKeyExA", (char *)hle_ptr(buf));
    check(crt("RegOpenKeyExA", 5, (uint32_t[]){ HKCU, S("Software\\Nothing"), 0, 0x20019, out }) == 2, "registry: a key that is not there", "opened");
    /* the installer's value: how large, too large, and it */
    MEM32(size) = 0;
    check(query(jp, "InstallDir", type, 0, size) == 0 && MEM32(size) == 4 && MEM32(type) == 1, "registry: how large a value is", "wrong");
    MEM32(size) = 2;
    check(query(jp, "InstallDir", type, buf, size) == 234 && MEM32(size) == 4, "registry: too small a buffer", "taken");
    MEM32(size) = 64;
    check(query(jp2, "installdir", type, buf, size) == 0 && !strcmp((char *)hle_ptr(buf), "C:\\"), "registry: the installer's value", (char *)hle_ptr(buf));
    check(query(jp, "Nothing", type, buf, size) == 2, "registry: a value that is not there", "found");
    /* what is written is kept, whatever its kind, and read back by another run */
    int fd = mkstemp(path);
    if (fd >= 0) close(fd);
    hle_reg_load(path);
    uint32_t number = hle_scratch(4), bytes = hle_scratch_copy("\1\0\377\"", 4);
    MEM32(number) = 305419896;
    crt("RegSetValueExA", 6, (uint32_t[]){ jp, S("Number"), 0, 4, number, 4 });
    crt("RegSetValueExA", 6, (uint32_t[]){ jp, S("Text"), 0, 1, S("a \"b\"\\c=d:e"), 12 });
    crt("RegSetValueExA", 6, (uint32_t[]){ jp, S("Bytes"), 0, 3, bytes, 4 });
    for (int i = 0; i < 3; i++) {            /* gone from memory; the file before that is put back */
        static const char *const n[] = { "Number", "Text", "Bytes" };
        char keep[4096];
        FILE *f = fopen(path, "rb");
        size_t len = f ? fread(keep, 1, sizeof keep, f) : 0;
        if (f) fclose(f);
        crt("RegDeleteValueA", 2, (uint32_t[]){ jp, S(n[i]) });
        if ((f = fopen(path, "wb"))) { fwrite(keep, 1, len, f); fclose(f); }
    }
    check(query(jp, "Number", type, buf, size) == 2, "registry: a value deleted", "found");
    hle_reg_load(path);
    MEM32(size) = 64;
    snprintf(num, sizeof num, "%u", query(jp, "Number", type, buf, size) == 0 ? MEM32(buf) : 0);
    check(MEM32(buf) == 305419896 && MEM32(type) == 4, "registry: a number kept", num);
    MEM32(size) = 64;
    check(query(jp, "Text", type, buf, size) == 0 && !strcmp((char *)hle_ptr(buf), "a \"b\"\\c=d:e"), "registry: text kept", (char *)hle_ptr(buf));
    MEM32(size) = 64;
    check(query(jp, "Bytes", type, buf, size) == 0 && MEM32(size) == 4 && !memcmp(hle_ptr(buf), "\1\0\377\"", 4) && MEM32(type) == 3, "registry: bytes kept", "not");
    check(crt("RegCloseKey", 1, &jp) == 0 && crt("RegCloseKey", 1, &jp2) == 0 && crt("RegCloseKey", 1, &bts) == 0, "registry: RegCloseKey", "failed");
    check(query(jp, "InstallDir", type, 0, size) != 0, "registry: a closed key", "read");
    unlink(path);
    hle_scratch_release(mark);
}

int jpog_selftest(void) {
    mach_enter();
    test_msvcrt();
    test_start();
    test_registry();
    mach_leave();
    test_libmmd();
    test_toshi();
    mach_enter();
    jpog_selftest_d3d8();
    jpog_selftest_engine();
    jpog_selftest_fsound();
    jpog_selftest_movies();
    mach_leave();
    printf("selftest: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed;
}
