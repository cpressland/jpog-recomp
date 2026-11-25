/*
 * msvcrt.c - Windows' own C runtime, msvcrt.dll, answered by the host.
 *
 * MSVC 6 links msvcrt.dll dynamically, and it came with Windows: it is not
 * on the disc, so unlike stl-recomp's msvcr71 there is nothing to lift. The
 * 91 functions and variables this game's modules import are here, on win32hle
 * (to go upstream to it: nothing below knows the game).
 *
 * All are cdecl: RET(value, 0). A double argument is two dwords of the stack,
 * a double result is pushed on the x87 ring. Its variables (_iob,
 * _adjust_fdiv) are imports by address, so the loader asks jpog_msvcrt_data
 * before it asks for a function.
 *
 * What is MSVC's and not the host libc's, and a game can tell:
 * - printf: %s in a wide function is a wide string (%S the other), exponents
 *   have three digits, I64, %p is eight upper-case digits, infinities and
 *   NaNs are 1.#INF and 1.#QNAN, _snprintf returns -1 and writes no
 *   terminator when the text does not fit;
 * - a file opened without "b" is text: \r\n in the file, \n in memory, and
 *   Ctrl-Z ends it;
 * - rand() is the 214013 generator, 15 bits, per thread;
 * - the C locale: case and classes are ASCII's.
 *
 * __CxxFrameHandler and _except_handler3 are only ever addresses here: eh32
 * walks the guest's frames itself and calls neither.
 */
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "win32hle.h"
#include "jpog.h"

#define RETD(v) do { fp_push(v); g_esp += 4u; return; } while (0)     /* a double, on the x87 stack */

static double arg_double(int i) {
    uint64_t bits = (uint64_t)A32(i) | (uint64_t)A32(i + 1) << 32;
    double d;
    memcpy(&d, &bits, 8);
    return d;
}
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int upper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }
static uint32_t wlen(uint32_t s) { uint32_t n = 0; while (MEM16(s + 2 * n)) n++; return n; }

/* ---- variables ------------------------------------------------------------ */
#define NSTREAM 64
#define FILE_SIZE 32u                        /* VC6's FILE: _file, its index here, at +16 */
static struct { FILE *f; int text, eof; uint32_t va; } g_stream[NSTREAM];
static uint32_t g_iob, g_adjust_fdiv, g_fmode, g_commode, g_initenv;

static void data_init(void) {
    if (g_iob) return;
    g_iob = hle_va(hle_alloc(FILE_SIZE * 20));
    g_adjust_fdiv = hle_va(hle_alloc(4));    /* 0: the CPU's fdiv is right */
    g_fmode = hle_va(hle_alloc(4));          /* 0: text, as the CRT's default */
    g_commode = hle_va(hle_alloc(4));
    g_initenv = hle_va(hle_alloc(4));
    FILE *std[3] = { stdin, stdout, stderr };
    for (int i = 0; i < 3; i++) {
        g_stream[i].f = std[i];
        g_stream[i].va = g_iob + FILE_SIZE * (uint32_t)i;
        MEM32(g_stream[i].va + 16) = (uint32_t)i;
    }
}
uint32_t jpog_msvcrt_data(const char *name) {
    data_init();
    if (!strcmp(name, "_iob")) return g_iob;
    if (!strcmp(name, "_adjust_fdiv")) return g_adjust_fdiv;
    return 0;
}
static void c___p__fmode(void)    { data_init(); RET(g_fmode, 0); }
static void c___p__commode(void)  { data_init(); RET(g_commode, 0); }
static void c___p___initenv(void) { data_init(); RET(g_initenv, 0); }
static void c__errno(void) {
    static _Thread_local uint32_t t_errno;
    if (!t_errno) t_errno = hle_va(hle_alloc(4));
    RET(t_errno, 0);
}

/* ---- memory --------------------------------------------------------------- */
static void c_malloc(void) { uint32_t n = A32(0); RETP(hle_heap_alloc(n ? n : 1, 0), 0); }
static void c_calloc(void) {
    uint64_t n = (uint64_t)A32(0) * A32(1);
    if (n > 0x7FFFFFFFu) RET(0, 0);
    RETP(hle_heap_alloc(n ? (uint32_t)n : 1, 1), 0);
}
static void c_free(void) { if (A32(0)) hle_heap_free(APTR(0)); RETV(0); }

/* ---- starting and ending -------------------------------------------------- */
#define MAX_ONEXIT 4096
static uint32_t g_onexit[MAX_ONEXIT];
static int g_nonexit;
int jpog_msvcrt_onexit_count(void) { return g_nonexit; }

static void c__initterm(void) {              /* (begin, end): the constructors between, in order */
    uint32_t end = A32(1);
    for (uint32_t p = A32(0); p < end; p += 4)
        if (MEM32(p)) hle_call_guest(MEM32(p), 0, NULL);
    RETV(0);
}
static void c__onexit(void) {
    if (g_nonexit == MAX_ONEXIT) RET(0, 0);
    g_onexit[g_nonexit++] = A32(0);
    RET(A32(0), 0);
}
static void c___dllonexit(void) {            /* (fn, &begin, &end): a DLL's own table, grown by one */
    uint32_t fn = A32(0), pb = A32(1), pe = A32(2), begin = MEM32(pb), used = MEM32(pe) - begin;
    if (begin && hle_heap_size(hle_ptr(begin)) < used + 4) {
        void *n = hle_heap_realloc(hle_ptr(begin), used + used / 2 + 64, 0);
        if (!n) RET(0, 0);
        begin = hle_va(n);
        MEM32(pb) = begin;
    }
    if (!begin) RET(0, 0);
    MEM32(begin + used) = fn;
    MEM32(pe) = begin + used + 4;
    RET(fn, 0);
}
static void c_exit(void) {
    int code = (int)A32(0);
    while (g_nonexit) hle_call_guest(g_onexit[--g_nonexit], 0, NULL);
    hle_exit(code);
}
static void c__exit(void) { hle_exit((int)A32(0)); }
static void c___set_app_type(void)    { RETV(0); }
static void c___setusermatherr(void)  { RETV(0); }
static void c__XcptFilter(void)       { RET(0, 0); }      /* EXCEPTION_CONTINUE_SEARCH */
static void c_signal(void)            { RET(0, 0); }      /* SIG_DFL was there */
static void c__purecall(void)         { hle_fatal("[msvcrt] a pure virtual call, from sub_%08X", g_cur_func); }
static void c___CxxFrameHandler(void) { hle_fatal("[msvcrt] __CxxFrameHandler called, from sub_%08X", g_cur_func); }
static void c__except_handler3(void)  { hle_fatal("[msvcrt] _except_handler3 called, from sub_%08X", g_cur_func); }

static void c___getmainargs(void) {          /* (&argc, &argv, &env, wildcards, startup info) */
    const char *p = (const char *)hle_ptr(hle_call_guest(hle_resolve("GetCommandLineA"), 0, NULL));
    uint32_t argv = hle_va(hle_alloc(4 * 64)), argc = 0;
    while (p && *p && argc < 63) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char word[1024];
        size_t n = 0;
        for (int q = 0; *p && (q || (*p != ' ' && *p != '\t')); p++) {
            if (*p == '"') q = !q;
            else if (n + 1 < sizeof word) word[n++] = *p;
        }
        word[n] = 0;
        MEM32(argv + 4 * argc++) = hle_guest_strdup(word);
    }
    MEM32(A32(0)) = argc;
    MEM32(A32(1)) = argv;
    MEM32(A32(2)) = hle_va(hle_alloc(4));    /* an empty environment */
    RET(0, 0);
}

/* Threads are kernel32's: the arguments are CreateThread's, but cdecl. */
static void run_shim(const char *name) {
    uint32_t va = hle_resolve(name);
    recomp_func_t f = va ? recomp_lookup_import(va) : NULL;
    if (!f) hle_fatal("[msvcrt] win32hle has no %s", name);
    f();
}
static void c__beginthreadex(void) { run_shim("CreateThread"); g_esp -= 24; }
static void c__endthreadex(void)   { run_shim("ExitThread"); }

/* ---- numbers -------------------------------------------------------------- */
static void c__CIacos(void) { double x = fp_pop(); fp_push(acos(x)); g_esp += 4; }
static void c__CIpow(void)  { double y = fp_pop(), x = fp_pop(); fp_push(pow(x, y)); g_esp += 4; }
/* st(0) truncated to 64 bits whatever the control word says (a C cast), in
 * edx:eax; what does not fit is the x87's integer indefinite. */
static void c__ftol(void) {
    double v = g_stp[0];
    int64_t shadow = g_sti64p[0], q;
    if (v == (double)shadow) q = shadow;
    else if (v >= -9223372036854775808.0 && v < 9223372036854775808.0) q = (int64_t)v;
    else q = INT64_MIN;
    (void)fp_pop();
    g_eax = (uint32_t)q;
    g_edx = (uint32_t)((uint64_t)q >> 32);
    g_esp += 4;
}
static void c__finite(void) { RET(isfinite(arg_double(0)) ? 1 : 0, 0); }
static void c__isnan(void)  { RET(isnan(arg_double(0)) ? 1 : 0, 0); }

/* _controlfp(new, mask): the CRT's names for the control word's bits. */
static void c__controlfp(void) {
    static const struct { uint32_t crt; uint16_t hw; } ex[] = {
        { 0x10, 1 }, { 0x80000, 2 }, { 8, 4 }, { 4, 8 }, { 2, 16 }, { 1, 32 } };
    static const uint32_t pc[4] = { 0x20000, 0x20000, 0x10000, 0 };       /* 24, -, 53, 64 bits */
    uint32_t v = 0, mask = A32(1);
    for (int i = 0; i < 6; i++) if (g_fpu_cw & ex[i].hw) v |= ex[i].crt;
    v |= (uint32_t)((g_fpu_cw >> 10) & 3) << 8 | pc[(g_fpu_cw >> 8) & 3];
    v = (v & ~mask) | (A32(0) & mask);
    uint16_t cw = (uint16_t)(g_fpu_cw & ~0x0F3F);
    for (int i = 0; i < 6; i++) if (v & ex[i].crt) cw |= ex[i].hw;
    cw |= (uint16_t)(((v >> 8) & 3) << 10);
    cw |= (v & 0x30000) == 0x20000 ? 0 : (v & 0x30000) == 0x10000 ? 0x200 : 0x300;
    if (mask) g_fpu_cw = cw;
    RET(v, 0);
}
static void c_abs(void)     { int32_t v = (int32_t)A32(0); RET(v < 0 ? 0u - (uint32_t)v : (uint32_t)v, 0); }
static void c_atoi(void)    { RET((uint32_t)strtol(ASTR(0), NULL, 10), 0); }
static double parse_double(const char *s) {   /* strtod, less the host's hex, inf and nan */
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    const char *d = p + (*p == '-' || *p == '+');
    if (!(*d >= '0' && *d <= '9') && *d != '.') return 0;
    if (d[0] == '0' && (d[1] | 32) == 'x') return 0;
    return strtod(p, NULL);
}
static void c_atof(void)    { RETD(parse_double(ASTR(0))); }
static void c_strtoul(void) {
    char *end;
    unsigned long long v = strtoull(ASTR(0), &end, (int)A32(2));
    if (A32(1)) MEM32(A32(1)) = hle_va(end);
    RET(v > 0xFFFFFFFFull && v < 0xFFFFFFFF00000000ull ? 0xFFFFFFFFu : (uint32_t)v, 0);
}
static void c__ultoa(void) {
    uint32_t v = A32(0), radix = A32(2);
    char tmp[40];
    int n = 0;
    if (radix < 2 || radix > 36) RET(0, 0);
    do { uint32_t d = v % radix; tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); } while (v /= radix);
    char *o = ASTR(1);
    while (n) *o++ = tmp[--n];
    *o = 0;
    RET(A32(1), 0);
}
static _Thread_local uint32_t t_seed = 1;
static void c_rand(void)  { t_seed = t_seed * 214013u + 2531011u; RET((t_seed >> 16) & 0x7FFF, 0); }
static void c_srand(void) { t_seed = A32(0); RETV(0); }

/* A merge sort of the elements' places, so the comparisons see the array
 * where it is; the elements are put in order at the end. */
static int32_t compare(uint32_t fn, uint32_t a, uint32_t b) {
    uint32_t args[2] = { a, b };
    return (int32_t)hle_call_guest(fn, 2, args);
}
static void msort(uint32_t *v, uint32_t *t, uint32_t n, uint32_t fn) {
    if (n < 2) return;
    uint32_t h = n / 2, i = 0, j = h, k = 0;
    msort(v, t, h, fn);
    msort(v + h, t, n - h, fn);
    while (i < h && j < n) t[k++] = compare(fn, v[j], v[i]) < 0 ? v[j++] : v[i++];
    while (i < h) t[k++] = v[i++];
    while (j < n) t[k++] = v[j++];
    memcpy(v, t, n * sizeof *v);
}
static void c_qsort(void) {                  /* (base, count, size, compare) */
    uint32_t base = A32(0), n = A32(1), size = A32(2), fn = A32(3);
    if (n > 1 && size) {
        uint32_t *v = malloc(n * sizeof *v), *t = malloc(n * sizeof *t);
        uint8_t *copy = malloc((size_t)n * size);
        if (!v || !t || !copy) hle_fatal("[msvcrt] qsort: no memory for %u", n);
        for (uint32_t i = 0; i < n; i++) v[i] = base + i * size;
        msort(v, t, n, fn);
        for (uint32_t i = 0; i < n; i++) memcpy(copy + (size_t)i * size, hle_ptr(v[i]), size);
        memcpy(hle_ptr(base), copy, (size_t)n * size);
        free(v), free(t), free(copy);
    }
    RETV(0);
}

/* ---- time ----------------------------------------------------------------- */
static void c_time(void) {
    uint32_t t = (uint32_t)time(NULL);
    if (A32(0)) MEM32(A32(0)) = t;
    RET(t, 0);
}
static void c_localtime(void) {
    static _Thread_local uint32_t t_tm;
    time_t t = (time_t)(int32_t)MEM32(A32(0));
    struct tm r;
    if (t < 0 || !localtime_r(&t, &r)) RET(0, 0);
    if (!t_tm) t_tm = hle_va(hle_alloc(36));
    int f[9] = { r.tm_sec, r.tm_min, r.tm_hour, r.tm_mday, r.tm_mon, r.tm_year, r.tm_wday, r.tm_yday, r.tm_isdst > 0 };
    for (int i = 0; i < 9; i++) MEM32(t_tm + 4u * (uint32_t)i) = (uint32_t)f[i];
    RET(t_tm, 0);
}

/* ---- characters and strings ----------------------------------------------- */
static int ascii(uint32_t c) { return c < 128; }
static void c_isalpha(void) { uint32_t c = A32(0); RET(ascii(c) && isalpha((int)c) ? 0x103 : 0, 0); }
static void c_isdigit(void) { uint32_t c = A32(0); RET(c >= '0' && c <= '9' ? 4 : 0, 0); }
static void c_isalnum(void) { uint32_t c = A32(0); RET(ascii(c) && isalnum((int)c) ? 0x107 : 0, 0); }
static void c_isspace(void) { uint32_t c = A32(0); RET(c == ' ' || (c >= 9 && c <= 13) ? 8 : 0, 0); }
/* Past ASCII (Japanese text): letters, but for the general punctuation and
 * the ideographic space; the C locale's Latin-1 upper half has no letters. */
static int wide_letter(uint32_t c) { return c >= 0x100 && !(c >= 0x2000 && c <= 0x206F) && !(c >= 0x3000 && c <= 0x3003); }
static int wide_space(uint32_t c)  { return c == ' ' || (c >= 9 && c <= 13) || c == 0x3000; }
#define WC() (A32(0) & 0xFFFFu)
static void c_iswalpha(void)  { uint32_t c = WC(); RET((ascii(c) && isalpha((int)c)) || wide_letter(c), 0); }
static void c_iswdigit(void)  { uint32_t c = WC(); RET(c >= '0' && c <= '9', 0); }
static void c_iswalnum(void)  { uint32_t c = WC(); RET((ascii(c) && isalnum((int)c)) || wide_letter(c), 0); }
static void c_iswspace(void)  { RET(wide_space(WC()), 0); }
static void c_iswxdigit(void) { uint32_t c = WC(); RET(ascii(c) && isxdigit((int)c), 0); }

static int icmp(const char *a, const char *b, uint32_t n) {
    for (; n; n--, a++, b++) {
        int x = lower((uint8_t)*a), y = lower((uint8_t)*b);
        if (x != y || !x) return x - y;
    }
    return 0;
}
static int wicmp(uint32_t a, uint32_t b, uint32_t n, int fold) {
    for (; n; n--, a += 2, b += 2) {
        int x = MEM16(a), y = MEM16(b);
        if (fold) x = lower(x), y = lower(y);
        if (x != y || !x) return x - y;
    }
    return 0;
}
static void c__stricmp(void)  { RET(icmp(ASTR(0), ASTR(1), UINT32_MAX), 0); }
static void c__strnicmp(void) { RET(icmp(ASTR(0), ASTR(1), A32(2)), 0); }
static void c__wcsicmp(void)  { RET(wicmp(A32(0), A32(1), UINT32_MAX, 1), 0); }
static void c__wcsnicmp(void) { RET(wicmp(A32(0), A32(1), A32(2), 1), 0); }
static void c_wcscmp(void)    { int r = wicmp(A32(0), A32(1), UINT32_MAX, 0); RET(r < 0 ? -1 : r > 0, 0); }
static void c_wcsncmp(void)   { RET(wicmp(A32(0), A32(1), A32(2), 0), 0); }
static void c__strlwr(void)   { for (char *p = ASTR(0); *p; p++) *p = (char)lower((uint8_t)*p); RET(A32(0), 0); }
static void c__strupr(void)   { for (char *p = ASTR(0); *p; p++) *p = (char)upper((uint8_t)*p); RET(A32(0), 0); }
static void c__wcslwr(void)   { for (uint32_t p = A32(0); MEM16(p); p += 2) MEM16(p) = (uint16_t)lower(MEM16(p)); RET(A32(0), 0); }
static void c__wcsupr(void)   { for (uint32_t p = A32(0); MEM16(p); p += 2) MEM16(p) = (uint16_t)upper(MEM16(p)); RET(A32(0), 0); }
static void c_strchr(void)    { RETP(strchr(ASTR(0), (char)A32(1)), 0); }
static void c_strrchr(void)   { RETP(strrchr(ASTR(0), (char)A32(1)), 0); }
static void c_strstr(void)    { RETP(strstr(ASTR(0), ASTR(1)), 0); }
static void c_strncmp(void)   { RET(strncmp(ASTR(0), ASTR(1), A32(2)), 0); }
static void c_strncpy(void)   { strncpy(ASTR(0), ASTR(1), A32(2)); RET(A32(0), 0); }
static void c_strncat(void)   { strncat(ASTR(0), ASTR(1), A32(2)); RET(A32(0), 0); }
static void c_strtok(void) {
    static _Thread_local uint32_t t_next;
    uint32_t s = A32(0) ? A32(0) : t_next;
    const char *set = ASTR(1);
    if (!s) RET(0, 0);
    char *p = (char *)hle_ptr(s);
    p += strspn(p, set);
    if (!*p) { t_next = 0; RET(0, 0); }
    char *e = p + strcspn(p, set);
    t_next = *e ? hle_va(e + 1) : 0;
    *e = 0;
    RETP(p, 0);
}
static void c_wcslen(void) { RET(wlen(A32(0)), 0); }
static void c_wcschr(void) {
    uint16_t c = (uint16_t)A32(1);
    for (uint32_t p = A32(0);; p += 2) {
        if (MEM16(p) == c) RET(p, 0);
        if (!MEM16(p)) RET(0, 0);
    }
}
static void c_wcscpy(void) { memmove(APTR(0), APTR(1), 2 * wlen(A32(1)) + 2); RET(A32(0), 0); }
static void c_wcsncpy(void) {
    uint32_t d = A32(0), s = A32(1), n = A32(2), i = 0;
    for (; i < n && MEM16(s + 2 * i); i++) MEM16(d + 2 * i) = MEM16(s + 2 * i);
    for (; i < n; i++) MEM16(d + 2 * i) = 0;
    RET(d, 0);
}
static void c_wcsstr(void) {
    uint32_t h = A32(0), n = wlen(A32(1)), l = wlen(h);
    for (uint32_t i = 0; i + n <= l; i++)
        if (!memcmp(hle_ptr(h + 2 * i), APTR(1), 2 * n)) RET(h + 2 * i, 0);
    RET(0, 0);
}

/* ---- paths ---------------------------------------------------------------- */
static void put_part(uint32_t out, const char *s, size_t n, size_t cap) {
    if (!out) return;
    if (n >= cap) n = cap - 1;
    memcpy(hle_ptr(out), s, n);
    MEM8(out + (uint32_t)n) = 0;
}
static void c__splitpath(void) {             /* (path, drive, dir, name, ext) */
    const char *p = ASTR(0), *start = p, *sep = NULL, *dot = NULL;
    if (p[0] && p[1] == ':') start = p + 2;
    put_part(A32(1), p, (size_t)(start - p), 3);
    for (const char *q = start; *q; q++) if (*q == '\\' || *q == '/') sep = q + 1, dot = NULL; else if (*q == '.') dot = q;
    const char *name = sep ? sep : start, *end = name + strlen(name);
    if (!dot) dot = end;
    put_part(A32(2), start, (size_t)(name - start), 256);
    put_part(A32(3), name, (size_t)(dot - name), 256);
    put_part(A32(4), dot, (size_t)(end - dot), 256);
    RETV(0);
}
static void c__makepath(void) {              /* (path, drive, dir, name, ext) */
    char out[1100];
    const char *drive = ASTR(1), *dir = ASTR(2), *name = ASTR(3), *ext = ASTR(4);
    size_t dn = dir ? strlen(dir) : 0;
    snprintf(out, sizeof out, "%.1s%s%s%s%s%s%s", drive ? drive : "", drive && *drive ? ":" : "", dir ? dir : "",
             dn && dir[dn - 1] != '\\' && dir[dn - 1] != '/' ? "\\" : "", name ? name : "",
             ext && *ext && *ext != '.' ? "." : "", ext ? ext : "");
    strcpy(ASTR(0), out);
    RETV(0);
}
static void c__getcwd(void) {                /* (buffer or NULL, size) */
    uint32_t buf = A32(0), n = A32(1);
    if (!buf) buf = hle_va(hle_heap_alloc(n > 260 ? n : 260, 0)), n = n > 260 ? n : 260;
    uint32_t args[2] = { n, buf }, r = hle_call_guest(hle_resolve("GetCurrentDirectoryA"), 2, args);
    RET(r && r < n ? buf : 0, 0);
}

/* ---- printf --------------------------------------------------------------- */
typedef struct {
    uint32_t buf;        /* guest buffer (bytes or UTF-16), or 0: */
    char    *host;       /* a host buffer that grows (a stream's text) */
    size_t   cap, n;     /* units that fit (SIZE_MAX: as many as come), and wanted so far */
    int      wide;
} out_t;

static void put(out_t *o, uint32_t c) {
    if (o->buf) {
        if (o->n < o->cap) {
            if (o->wide) MEM16(o->buf + 2 * (uint32_t)o->n) = (uint16_t)c;
            else MEM8(o->buf + (uint32_t)o->n) = (uint8_t)(c < 256 ? c : '?');
        }
    } else {
        if (o->n + 1 >= o->cap) o->host = realloc(o->host, o->cap = o->cap * 2 + 256);
        o->host[o->n] = (char)(c < 256 ? c : '?');
    }
    o->n++;
}
static void pad(out_t *o, int n, char c) { while (n-- > 0) put(o, (uint8_t)c); }

/* A double as MSVC prints it, into s (no width: that is the caller's). */
static void format_double(char *s, size_t cap, const char *flags, int prec, char conv, double d) {
    char spec[24];
    if (!isfinite(d)) {                      /* the digits are 1.#INF, cut or zero-filled to the precision */
        const char *tail = isinf(d) ? "#INF" : signbit(d) ? "#IND" : "#QNAN";
        int p = prec < 0 ? 6 : prec, t = (int)strlen(tail);
        int keep = (conv | 32) == 'g' ? (p - 1 < t ? (p > 1 ? p - 1 : 0) : t) : p < t ? p : t;
        int zeros = (conv | 32) == 'g' ? 0 : p - keep;
        snprintf(s, cap, "%s1%s%.*s%.*s%s", signbit(d) ? "-" : strchr(flags, '+') ? "+" : strchr(flags, ' ') ? " " : "",
                 p ? "." : "", keep, tail, zeros > 0 ? (zeros < 40 ? zeros : 40) : 0,
                 "0000000000000000000000000000000000000000",
                 (conv | 32) == 'e' ? (conv == 'E' ? "E+000" : "e+000") : "");
        return;
    }
    snprintf(spec, sizeof spec, "%%%s%s%s.*%c", strchr(flags, '+') ? "+" : "", strchr(flags, ' ') ? " " : "",
             strchr(flags, '#') ? "#" : "", conv);
    snprintf(s, cap, spec, prec < 0 ? 6 : prec > 400 ? 400 : prec, d);
    char *e = (conv | 32) == 'f' ? NULL : strpbrk(s, "eE");
    if (e && strlen(e) == 4 && strlen(s) + 2 < cap) {    /* e+05 -> e+005 */
        memmove(e + 3, e + 2, 3);
        e[2] = '0';
    }
}

static void format(out_t *o, uint32_t fmt, uint32_t ap) {
#define FC(i) (o->wide ? (uint32_t)MEM16(fmt + 2u * (i)) : (uint32_t)MEM8(fmt + (i)))
#define STEP(k) (fmt += (o->wide ? 2u : 1u) * (k))
    for (uint32_t c; (c = FC(0)); ) {
        STEP(1);
        if (c != '%') { put(o, c); continue; }
        char flags[8];
        int nf = 0, width = 0, prec = -1, left, size = 0;    /* size: 'h', 'l' (and w), 'q' (I64), 'L' */
        while ((c = FC(0)) && c < 128 && strchr("-+ #0", (int)c)) { if (nf < 7) flags[nf++] = (char)c; STEP(1); }
        flags[nf] = 0;
        if (FC(0) == '*') { width = (int32_t)MEM32(ap), ap += 4; STEP(1); }
        else while (FC(0) >= '0' && FC(0) <= '9') { if (width < 100000) width = width * 10 + (int)(FC(0) - '0'); STEP(1); }
        left = strchr(flags, '-') != NULL;
        if (width < 0) left = 1, width = -width;
        if (FC(0) == '.') {
            STEP(1);
            prec = 0;
            if (FC(0) == '*') { prec = (int32_t)MEM32(ap), ap += 4; STEP(1); }
            else while (FC(0) >= '0' && FC(0) <= '9') { if (prec < 100000) prec = prec * 10 + (int)(FC(0) - '0'); STEP(1); }
        }
        for (;;) {
            c = FC(0);
            if (c == 'h') size = 'h', STEP(1);
            else if (c == 'l') size = size == 'l' ? 'q' : 'l', STEP(1);
            else if (c == 'w') size = 'l', STEP(1);
            else if (c == 'L') size = 'L', STEP(1);
            else if (c == 'I' && FC(1) == '6' && FC(2) == '4') { size = 'q'; STEP(3); }
            else if (c == 'I' && FC(1) == '3' && FC(2) == '2') STEP(3);
            else break;
        }
        c = FC(0);
        if (!c) break;
        STEP(1);
        char num[1100];
        switch (c) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'p': {
            uint64_t v = MEM32(ap);
            ap += 4;
            if (size == 'q') v |= (uint64_t)MEM32(ap) << 32, ap += 4;
            int neg = 0;
            if (c == 'd' || c == 'i') {
                int64_t s = size == 'q' ? (int64_t)v : size == 'h' ? (int16_t)v : (int32_t)v;
                neg = s < 0;
                v = neg ? 0 - (uint64_t)s : (uint64_t)s;
            } else if (size == 'h' && c != 'p') v &= 0xFFFF;
            if (c == 'p') prec = 8;
            char digits[40];
            int n = snprintf(digits, sizeof digits, c == 'x' ? "%llx" : c == 'X' || c == 'p' ? "%llX" : c == 'o' ? "%llo" : "%llu",
                             (unsigned long long)v);
            if (prec == 0 && !v) n = 0;
            const char *pre = neg ? "-" : (c == 'd' || c == 'i') && strchr(flags, '+') ? "+" :
                              (c == 'd' || c == 'i') && strchr(flags, ' ') ? " " :
                              strchr(flags, '#') && v ? (c == 'x' ? "0x" : c == 'X' ? "0X" : c == 'o' ? "0" : "") : "";
            int np = (int)strlen(pre), zeros = prec > n ? prec - n : 0;
            if (prec < 0 && strchr(flags, '0') && !left && width > n + np) zeros = width - n - np;
            int total = np + zeros + n;
            if (!left) pad(o, width - total, ' ');
            for (int i = 0; i < np; i++) put(o, (uint8_t)pre[i]);
            pad(o, zeros, '0');
            for (int i = 0; i < n; i++) put(o, (uint8_t)digits[i]);
            if (left) pad(o, width - total, ' ');
            break;
        }
        case 'e': case 'E': case 'f': case 'g': case 'G': {
            uint64_t bits = (uint64_t)MEM32(ap) | (uint64_t)MEM32(ap + 4) << 32;
            double d;
            memcpy(&d, &bits, 8);
            ap += 8;
            format_double(num, sizeof num, flags, prec, (char)c, d);
            int n = (int)strlen(num), sign = n && strchr("+- ", num[0]) ? 1 : 0;
            int zeros = strchr(flags, '0') && !left && width > n ? width - n : 0;
            if (!left && !zeros) pad(o, width - n, ' ');
            if (sign) put(o, (uint8_t)num[0]);
            pad(o, zeros, '0');
            for (int i = sign; i < n; i++) put(o, (uint8_t)num[i]);
            if (left) pad(o, width - n, ' ');
            break;
        }
        case 'c': case 'C': {
            int w = size == 'h' ? 0 : size == 'l' ? 1 : c == 'c' ? o->wide : !o->wide;
            uint32_t v = MEM32(ap);
            ap += 4;
            if (!left) pad(o, width - 1, ' ');
            put(o, w ? v & 0xFFFF : v & 0xFF);
            if (left) pad(o, width - 1, ' ');
            break;
        }
        case 's': case 'S': {
            int w = size == 'h' ? 0 : size == 'l' ? 1 : c == 's' ? o->wide : !o->wide;
            uint32_t s = MEM32(ap), n = 0;
            ap += 4;
            static const char null[] = "(null)";
            if (!s) { while (n < 6 && (prec < 0 || n < (uint32_t)prec)) n++; }
            else while ((prec < 0 || n < (uint32_t)prec) && (w ? MEM16(s + 2 * n) : MEM8(s + n))) n++;
            char fill = strchr(flags, '0') && !left ? '0' : ' ';
            if (!left) pad(o, width - (int)n, fill);
            for (uint32_t i = 0; i < n; i++) put(o, !s ? (uint8_t)null[i] : w ? MEM16(s + 2 * i) : MEM8(s + i));
            if (left) pad(o, width - (int)n, ' ');
            break;
        }
        case 'n':
            MEM32(MEM32(ap)) = (uint32_t)o->n;
            ap += 4;
            break;
        default:                             /* %% and what is not a conversion */
            put(o, c);
            break;
        }
    }
#undef FC
#undef STEP
}

/* Into a buffer of cap units: the length, or -1 if it did not all fit (no
 * terminator then, nor when it fits exactly, as MSVC has it). */
static int32_t to_buffer(uint32_t buf, size_t cap, int wide, uint32_t fmt, uint32_t ap) {
    out_t o = { buf, NULL, cap, 0, wide };
    format(&o, fmt, ap);
    if (o.n < cap) { if (wide) MEM16(buf + 2 * (uint32_t)o.n) = 0; else MEM8(buf + (uint32_t)o.n) = 0; }
    return o.n <= cap ? (int32_t)o.n : -1;
}
#define VA_AFTER(i) (g_esp + 4u + 4u * ((i) + 1u))       /* the variable arguments, after argument i */
static void c_sprintf(void)    { RET(to_buffer(A32(0), SIZE_MAX, 0, A32(1), VA_AFTER(1)), 0); }
static void c_vsprintf(void)   { RET(to_buffer(A32(0), SIZE_MAX, 0, A32(1), A32(2)), 0); }
static void c__snprintf(void)  { RET(to_buffer(A32(0), A32(1), 0, A32(2), VA_AFTER(2)), 0); }
static void c__vsnprintf(void) { RET(to_buffer(A32(0), A32(1), 0, A32(2), A32(3)), 0); }
static void c_swprintf(void)   { RET(to_buffer(A32(0), SIZE_MAX, 1, A32(1), VA_AFTER(1)), 0); }
static void c_vswprintf(void)  { RET(to_buffer(A32(0), SIZE_MAX, 1, A32(1), A32(2)), 0); }
static void c__vsnwprintf(void){ RET(to_buffer(A32(0), A32(1), 1, A32(2), A32(3)), 0); }

/* ---- files ---------------------------------------------------------------- */
static int stream_of(uint32_t va) {
    data_init();
    if (va >= g_iob && va < g_iob + 3 * FILE_SIZE) return (int)((va - g_iob) / FILE_SIZE);
    for (int i = 3; i < NSTREAM; i++) if (g_stream[i].f && g_stream[i].va == va) return i;
    return -1;
}
static void c_fopen(void) {
    const char *name = ASTR(0), *mode = ASTR(1);
    data_init();
    int i = 3;
    while (i < NSTREAM && g_stream[i].f) i++;
    if (i == NSTREAM || !name || !*name) RET(0, 0);
    char host[4096], m[4] = { mode[0], 'b', strchr(mode, '+') ? '+' : 0, 0 };
    if (mode[0] == 'r' && !strchr(mode, '+')) hle_host_path(name, host, sizeof host);
    else hle_host_path_for_write(name, host, sizeof host, mode[0] != 'w');
    FILE *f = fopen(host, m);
    if (hle_filetrace()) fprintf(stderr, "[file] fopen(%s, %s) -> %s%s\n", name, mode, host, f ? "" : ": no");
    if (!f) RET(0, 0);
    if (mode[0] != 'r') hle_path_changed();
    g_stream[i].f = f;
    g_stream[i].text = !strchr(mode, 'b');
    g_stream[i].eof = 0;
    g_stream[i].va = hle_va(hle_alloc(FILE_SIZE));
    MEM32(g_stream[i].va + 16) = (uint32_t)i;
    RET(g_stream[i].va, 0);
}
static void c_fclose(void) {
    int i = stream_of(A32(0));
    if (i < 3) RET(i < 0 ? UINT32_MAX : 0, 0);
    fclose(g_stream[i].f);
    hle_free(hle_ptr(g_stream[i].va));
    memset(&g_stream[i], 0, sizeof g_stream[i]);
    RET(0, 0);
}
static size_t read_bytes(int i, uint8_t *p, size_t n) {
    FILE *f = g_stream[i].f;
    if (!g_stream[i].text) return fread(p, 1, n, f);
    size_t k = 0;
    while (k < n && !g_stream[i].eof) {
        int c = getc(f);
        if (c == '\r') {
            int d = getc(f);
            if (d == '\n') c = '\n';
            else if (d != EOF) ungetc(d, f);
        }
        if (c == EOF || c == 0x1A) { g_stream[i].eof = 1; break; }
        p[k++] = (uint8_t)c;
    }
    return k;
}
static size_t write_bytes(int i, const uint8_t *p, size_t n) {
    FILE *f = g_stream[i].f;
    if (!g_stream[i].text) return fwrite(p, 1, n, f);
    for (size_t k = 0; k < n; k++) {
        if (p[k] == '\n' && putc('\r', f) == EOF) return k;
        if (putc(p[k], f) == EOF) return k;
    }
    return n;
}
static void c_fread(void) {                  /* (buffer, size, count, stream) */
    uint32_t size = A32(1), count = A32(2);
    int i = stream_of(A32(3));
    if (i < 0 || !size || !count) RET(0, 0);
    RET(read_bytes(i, APTR(0), (size_t)size * count) / size, 0);
}
static void c_fwrite(void) {
    uint32_t size = A32(1), count = A32(2);
    int i = stream_of(A32(3));
    if (i < 0 || !size || !count) RET(0, 0);
    RET(write_bytes(i, APTR(0), (size_t)size * count) / size, 0);
}
static int32_t to_stream(int i, uint32_t fmt, uint32_t ap) {
    if (i < 0) return -1;
    out_t o = { 0, NULL, 0, 0, 0 };
    format(&o, fmt, ap);
    size_t w = write_bytes(i, (const uint8_t *)o.host, o.n);
    free(o.host);
    return w == o.n ? (int32_t)w : -1;
}
static void c_fprintf(void) { RET(to_stream(stream_of(A32(0)), A32(1), VA_AFTER(1)), 0); }
static void c_printf(void)  { data_init(); RET(to_stream(1, A32(0), VA_AFTER(0)), 0); }
static void c_vprintf(void) { data_init(); RET(to_stream(1, A32(0), A32(1)), 0); }

/* ---- scanf ---------------------------------------------------------------- */
typedef struct { uint32_t p; int wide, n; } in_t;
static uint32_t peek(in_t *s) { return s->wide ? MEM16(s->p) : MEM8(s->p); }
static void next(in_t *s) { s->p += s->wide ? 2 : 1; s->n++; }
static int is_ws(uint32_t c) { return c == ' ' || (c >= 9 && c <= 13); }
static void skip_ws(in_t *s) { while (is_ws(peek(s))) next(s); }

static int scan(uint32_t str, uint32_t fmt, uint32_t ap, int wide) {
    in_t s = { str, wide, 0 };
    int done = 0;
#define FC(i) (wide ? (uint32_t)MEM16(fmt + 2u * (i)) : (uint32_t)MEM8(fmt + (i)))
#define STEP(k) (fmt += (wide ? 2u : 1u) * (k))
    for (uint32_t c; (c = FC(0)); ) {
        STEP(1);
        if (is_ws(c)) { skip_ws(&s); continue; }
        if (c != '%' || FC(0) == '%') {
            if (c == '%') { STEP(1); skip_ws(&s); }
            if (peek(&s) != c) break;
            next(&s);
            continue;
        }
        int skip = 0, width = 0, size = 0;
        if (FC(0) == '*') { skip = 1; STEP(1); }
        while (FC(0) >= '0' && FC(0) <= '9') { if (width < 100000) width = width * 10 + (int)(FC(0) - '0'); STEP(1); }
        for (;;) {
            c = FC(0);
            if (c == 'h') size = 'h', STEP(1);
            else if (c == 'l') size = size == 'l' ? 'q' : 'l', STEP(1);
            else if (c == 'w') size = 'l', STEP(1);
            else if (c == 'L') size = 'L', STEP(1);
            else if (c == 'I' && FC(1) == '6' && FC(2) == '4') { size = 'q'; STEP(3); }
            else break;
        }
        c = FC(0);
        if (!c) break;
        STEP(1);
        if (c == 'n') { if (!skip) { MEM32(MEM32(ap)) = (uint32_t)s.n; ap += 4; } continue; }
        if (c != 'c' && c != 'C' && c != '[') skip_ws(&s);
        if (!peek(&s)) { if (!done) done = -1; break; }        /* the input ended before the first conversion: EOF */
        uint32_t to = 0;
        if (!skip) to = MEM32(ap), ap += 4;
        if (c == 'c' || c == 'C' || c == 's' || c == 'S' || c == '[') {
            int w = size == 'h' ? 0 : size == 'l' ? 1 : c == 'C' || c == 'S' ? !wide : wide;
            uint32_t set = 0, set_n = 0;
            int invert = 0;
            if (c == '[') {                                    /* the set's own characters, in the format */
                if (FC(0) == '^') { invert = 1; STEP(1); }
                set = fmt;
                while (FC(0) && (FC(0) != ']' || !set_n)) { set_n++; STEP(1); }
                if (FC(0)) STEP(1);
            }
            int n = 0, max = width ? width : c == 'c' || c == 'C' ? 1 : INT32_MAX;
            for (uint32_t ch; n < max && (ch = peek(&s)); n++) {
                if (c == 's' || c == 'S') { if (is_ws(ch)) break; }
                else if (c == '[') {
                    int in = 0;
                    for (uint32_t i = 0; i < set_n && !in; i++) {
#define SC(i) (wide ? (uint32_t)MEM16(set + 2u * (i)) : (uint32_t)MEM8(set + (i)))
                        if (i + 2 < set_n && SC(i + 1) == '-') in = ch >= SC(i) && ch <= SC(i + 2), i += 2;
                        else in = ch == SC(i);
                    }
                    if (in == invert) break;
                }
                if (to) { if (w) MEM16(to + 2u * (uint32_t)n) = (uint16_t)ch; else MEM8(to + (uint32_t)n) = (uint8_t)ch; }
                next(&s);
            }
            if (!n || ((c == 'c' || c == 'C') && n < max)) break;
            if (to && c != 'c' && c != 'C') { if (w) MEM16(to + 2u * (uint32_t)n) = 0; else MEM8(to + (uint32_t)n) = 0; }
        } else if (c == 'e' || c == 'f' || c == 'g' || c == 'E' || c == 'G') {
            char t[128];
            int n = 0, digits = 0, dot = 0, max = width && width < 127 ? width : 127;
            uint32_t ch;
            if (((ch = peek(&s)) == '-' || ch == '+') && n < max) t[n++] = (char)ch, next(&s);
            while (n < max && (((ch = peek(&s)) >= '0' && ch <= '9') || (ch == '.' && !dot))) {
                if (ch == '.') dot = 1; else digits = 1;
                t[n++] = (char)ch, next(&s);
            }
            if (!digits) break;
            if (n < max && ((ch = peek(&s)) == 'e' || ch == 'E')) {
                t[n++] = 'e', next(&s);
                if (n < max && ((ch = peek(&s)) == '-' || ch == '+')) t[n++] = (char)ch, next(&s);
                while (n < max && (ch = peek(&s)) >= '0' && ch <= '9') t[n++] = (char)ch, next(&s);
            }
            t[n] = 0;
            double d = strtod(t, NULL);
            if (to) {
                if (size == 'l' || size == 'L') { uint64_t b; memcpy(&b, &d, 8); MEM32(to) = (uint32_t)b; MEM32(to + 4) = (uint32_t)(b >> 32); }
                else { float f = (float)d; uint32_t b; memcpy(&b, &f, 4); MEM32(to) = b; }
            }
        } else if (c < 128 && strchr("diuxXop", (int)c)) {
            uint32_t base = c == 'x' || c == 'X' || c == 'p' ? 16 : c == 'o' ? 8 : c == 'i' ? 0 : 10, ch;
            int max = width ? width : INT32_MAX, neg = 0, digits = 0, used = 0;
            uint64_t v = 0;
            if ((ch = peek(&s)) == '-' || ch == '+') neg = ch == '-', next(&s), used++;
            if ((base == 0 || base == 16) && peek(&s) == '0' && used < max) {
                next(&s), used++, digits++;
                if ((peek(&s) | 32) == 'x' && used < max) next(&s), used++, base = 16, digits = 0;
                else if (!base) base = 8;
            } else if (!base) base = 10;
            for (; used < max; used++, digits++, next(&s)) {
                ch = peek(&s);
                uint32_t dg = ch >= '0' && ch <= '9' ? ch - '0' : (ch | 32) >= 'a' && (ch | 32) <= 'f' ? (ch | 32) - 'a' + 10 : 99;
                if (dg >= base) break;
                v = v * base + dg;
            }
            if (!digits) break;
            if (neg) v = 0 - v;
            if (to) {
                if (size == 'h') MEM16(to) = (uint16_t)v;
                else { MEM32(to) = (uint32_t)v; if (size == 'q') MEM32(to + 4) = (uint32_t)(v >> 32); }
            }
        } else break;
        if (!skip) done++;
    }
#undef FC
#undef SC
#undef STEP
    return done;
}
static void c_sscanf(void)  { RET(scan(A32(0), A32(1), VA_AFTER(1), 0), 0); }
static void c_swscanf(void) { RET(scan(A32(0), A32(1), VA_AFTER(1), 1), 0); }

#define C(n) { #n, c_##n }
const win32hle_shim jpog_msvcrt_shims[] = {
    C(_CIacos), C(_CIpow), C(_XcptFilter), C(__CxxFrameHandler), C(__dllonexit), C(__getmainargs),
    C(__p___initenv), C(__p__commode), C(__p__fmode), C(__set_app_type), C(__setusermatherr),
    C(_beginthreadex), C(_controlfp), C(_endthreadex), C(_errno), C(_except_handler3), C(_exit), C(_finite),
    C(_ftol), C(_getcwd), C(_initterm), C(_isnan), C(_makepath), C(_onexit), C(_purecall), C(_snprintf),
    C(_splitpath), C(_stricmp), C(_strlwr), C(_strnicmp), C(_strupr), C(_ultoa), C(_vsnprintf), C(_vsnwprintf),
    C(_wcsicmp), C(_wcslwr), C(_wcsnicmp), C(_wcsupr), C(abs), C(atof), C(atoi), C(calloc), C(exit), C(fclose),
    C(fopen), C(fprintf), C(fread), C(free), C(fwrite), C(isalnum), C(isalpha), C(isdigit), C(isspace),
    C(iswalnum), C(iswalpha), C(iswdigit), C(iswspace), C(iswxdigit), C(localtime), C(malloc), C(printf),
    C(qsort), C(rand), C(signal), C(sprintf), C(srand), C(sscanf), C(strchr), C(strncat), C(strncmp), C(strncpy),
    C(strrchr), C(strstr), C(strtok), C(strtoul), C(swprintf), C(swscanf), C(time), C(vprintf), C(vsprintf),
    C(vswprintf), C(wcschr), C(wcscmp), C(wcscpy), C(wcslen), C(wcsncmp), C(wcsncpy), C(wcsstr),
    { "_setjmp3", jpog_setjmp3 }, { "longjmp", jpog_longjmp },
    { NULL, NULL }
};
