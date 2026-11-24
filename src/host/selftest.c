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

int jpog_selftest(void) {
    mach_enter();
    test_msvcrt();
    mach_leave();
    test_libmmd();
    test_toshi();
    mach_enter();
    jpog_selftest_d3d8();
    jpog_selftest_engine();
    mach_leave();
    printf("selftest: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed;
}
