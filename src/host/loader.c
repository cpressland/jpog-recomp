/*
 * loader.c - the lifted modules into guest memory, and their imports bound,
 * as Windows' loader would (stl-recomp's, for this game's modules).
 *
 * Each module is mapped at the base it was lifted at (tools/gen/modules.py,
 * JPOG_MODULES in recomp_target.h) and relocated there, so the game folder
 * holds the files as shipped. All go in before the first heap allocation,
 * which win32hle places from 0x10000000 up.
 *
 * An import from a lifted module is that module's export, a variable of
 * msvcrt's is msvcrt.c's, anything else is a shim, or a stub that names
 * itself when it is called. A build need not have every module: what one
 * imports from a module of the game that was not lifted is counted, and that
 * module is mapped but never attached.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "win32hle.h"
#include "pe_format.h"
#include "pe_loader.h"
#include "jpog.h"

#define PAGE 0x1000u
#define DIR_EXPORT 0
#define DIR_RELOC  5

jpog_module g_jpog_mod[JPOG_MAX_MODULES];
int g_jpog_nmod, g_jpog_stubs;

static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = len > 0 ? malloc((size_t)len) : NULL;
    if (p && fread(p, 1, (size_t)len, f) != (size_t)len) { free(p); p = NULL; }
    fclose(f);
    *n = (size_t)len;
    return p;
}

/* Map path at base: headers, sections, and its relocations if base is not
 * the file's own. 0 on success. */
static int map_at(const char *path, uint32_t base, pe_image *img) {
    size_t flen;
    uint8_t *file = slurp(path, &flen);
    if (!file) { fprintf(stderr, "[loader] cannot read %s\n", path); return 1; }
    pe_dos_header *dos = (pe_dos_header *)file;
    pe_nt_headers32 *nt = (pe_nt_headers32 *)(file + (dos->e_lfanew > 0 ? dos->e_lfanew : 0));
    if (flen < 0x200 || dos->e_magic != 0x5A4D || (size_t)dos->e_lfanew + sizeof *nt > flen ||
        nt->Signature != 0x00004550 || nt->OptionalHeader.Magic != 0x10B) {
        free(file);
        fprintf(stderr, "[loader] %s is not a 32-bit PE\n", path);
        return 1;
    }
    pe_opt_header32 *opt = &nt->OptionalHeader;
    uint32_t span = (opt->SizeOfImage + PAGE - 1) & ~(PAGE - 1);
    if (hle_vm_reserve(base, span) != base) {
        free(file);
        fprintf(stderr, "[loader] 0x%08X-0x%08X is in use: %s cannot go there\n", base, base + span, path);
        return 1;
    }
    memcpy(hle_ptr(base), file, opt->SizeOfHeaders < flen ? opt->SizeOfHeaders : flen);
    pe_section_header *sec = (pe_section_header *)((uint8_t *)opt + nt->FileHeader.SizeOfOptionalHeader);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        uint32_t n = sec[i].SizeOfRawData < sec[i].VirtualSize ? sec[i].SizeOfRawData : sec[i].VirtualSize;
        if (!sec[i].VirtualSize) n = sec[i].SizeOfRawData;
        if (n && (size_t)sec[i].PointerToRawData + n <= flen && sec[i].VirtualAddress + n <= span)
            memcpy(hle_ptr(base + sec[i].VirtualAddress), file + sec[i].PointerToRawData, n);
    }
    if (base != opt->ImageBase) {
        pe_data_dir r = opt->DataDirectory[DIR_RELOC];
        if (!r.Size) { free(file); fprintf(stderr, "[loader] %s has no relocations\n", path); return 1; }
        recomp_pe_relocate(base, r.VirtualAddress, r.Size, (int32_t)(base - opt->ImageBase));
        pe_nt_headers32 *mnt = (pe_nt_headers32 *)hle_ptr(base + (uint32_t)dos->e_lfanew);
        mnt->OptionalHeader.ImageBase = base;            /* as Windows does */
    }
    img->base = base;
    img->image_base = opt->ImageBase;
    img->span = span;
    img->entry = opt->AddressOfEntryPoint ? base + opt->AddressOfEntryPoint : 0;
    img->import_rva = opt->DataDirectory[PE_DIR_IMPORT].VirtualAddress;
    img->resource_rva = opt->DataDirectory[2].VirtualAddress;
    img->resource_size = opt->DataDirectory[2].Size;
    img->stamp = nt->FileHeader.TimeDateStamp;
    free(file);
    return 0;
}

static const char *base_name(const char *p) {
    const char *a = strrchr(p, '\\'), *b = strrchr(p, '/');
    if (b > a) a = b;
    return a ? a + 1 : p;
}
jpog_module *jpog_module_named(const char *dll) {
    dll = base_name(dll);
    size_t n = strlen(dll);
    for (int i = 0; i < g_jpog_nmod; i++) {              /* LoadLibrary's name may lack the .dll */
        const char *f = g_jpog_mod[i].file;
        if (!strcasecmp(f, dll) || (!strchr(dll, '.') && !strncasecmp(f, dll, n) && !strcasecmp(f + n, ".dll")))
            return &g_jpog_mod[i];
    }
    return NULL;
}
jpog_module *jpog_module_at(uint32_t base) {
    for (int i = 0; i < g_jpog_nmod; i++)
        if (g_jpog_mod[i].img.base == base) return &g_jpog_mod[i];
    return NULL;
}

/* An export of a mapped module, by name or (name NULL) ordinal; 0 if none.
 * The names are sorted, and there are thousands to bind: a binary search. */
uint32_t jpog_export_of(const jpog_module *m, const char *name, uint32_t ordinal) {
    pe_dos_header *dos = (pe_dos_header *)hle_ptr(m->img.base);
    pe_nt_headers32 *nt = (pe_nt_headers32 *)hle_ptr(m->img.base + (uint32_t)dos->e_lfanew);
    pe_data_dir d = nt->OptionalHeader.DataDirectory[DIR_EXPORT];
    if (!d.Size) return 0;
    uint32_t e = m->img.base + d.VirtualAddress;
    uint32_t obase = MEM32(e + 16), nfunc = MEM32(e + 20), nname = MEM32(e + 24);
    uint32_t funcs = m->img.base + MEM32(e + 28), names = m->img.base + MEM32(e + 32),
             ords = m->img.base + MEM32(e + 36);
    uint32_t idx = UINT32_MAX;
    if (name) {
        for (uint32_t lo = 0, hi = nname; lo < hi; ) {
            uint32_t mid = (lo + hi) / 2;
            int c = strcmp((const char *)hle_ptr(m->img.base + MEM32(names + 4 * mid)), name);
            if (!c) { idx = MEM16(ords + 2 * mid); break; }
            if (c < 0) lo = mid + 1; else hi = mid;
        }
    } else if (ordinal >= obase) {
        idx = ordinal - obase;
    }
    if (idx >= nfunc || !MEM32(funcs + 4 * idx)) return 0;
    uint32_t rva = MEM32(funcs + 4 * idx);
    if (rva >= d.VirtualAddress && rva < d.VirtualAddress + d.Size) {
        fprintf(stderr, "[loader] %s!%s forwards; not followed\n", m->file, name ? name : "(ordinal)");
        return 0;
    }
    return m->img.base + rva;
}
uint32_t jpog_export(const char *dll, const char *name) {
    jpog_module *m = jpog_module_named(dll);
    return m ? jpog_export_of(m, name, 0) : 0;
}

/* What Windows brought, not the game: answered by shims. Any other DLL is
 * the game's, and lifted or missing. */
static int system_dll(const char *dll) {
    static const char *sys[] = { "kernel32", "user32", "gdi32", "advapi32", "version", "winmm", "msvcrt", "ole32",
                                 "dsound", "dinput8", "d3d8", "msacm32", "binkw32", "fmod", NULL };
    for (int i = 0; sys[i]; i++) {
        size_t n = strlen(sys[i]);
        if (!strncasecmp(dll, sys[i], n) && !strcasecmp(dll + n, ".dll")) return 1;
    }
    return 0;
}

static void bind_module(jpog_module *m) {
    if (!m->img.import_rva) return;
    uint32_t base = m->img.base;
    int stubs = 0, lifted = 0, shims = 0;
    for (pe_import_descriptor *d = (pe_import_descriptor *)hle_ptr(base + m->img.import_rva); d->Name; d++) {
        const char *dll = (const char *)hle_ptr(base + d->Name);
        jpog_module *to = jpog_module_named(dll);
        int absent = !to && !system_dll(dll), crt = !strcasecmp(dll, "msvcrt.dll");
        uint32_t *ilt = (uint32_t *)hle_ptr(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        uint32_t *iat = (uint32_t *)hle_ptr(base + d->FirstThunk);
        for (; *ilt; ilt++, iat++) {
            const char *name = *ilt & 0x80000000u ? NULL : (const char *)hle_ptr(base + *ilt + 2);
            char nm[128];
            uint32_t va = 0;
            if (to) {
                va = jpog_export_of(to, name, *ilt & 0xFFFFu);
                if (va) lifted++;
                else {
                    fprintf(stderr, "[loader] %s: %s has no export %s\n", m->file, dll, name ? name : "(ordinal)");
                    m->missing++;
                }
            } else if (absent) {
                va = hle_resolve_or_stub("(a module of the game this build has not lifted)");
                m->missing++;
            } else {
                if (!name) {
                    snprintf(nm, sizeof nm, "%s#%u", dll, *ilt & 0xFFFFu);
                    for (char *p = nm; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
                    name = nm;
                }
                if (crt) va = jpog_msvcrt_data(name);
                if (!va) va = hle_resolve(name);
                if (va) shims++;
                else {
                    va = hle_resolve_or_stub(strdup(name));
                    stubs++;
                    if (jpog_verbose) fprintf(stderr, "[loader] %s: %s!%s is a stub\n", m->file, dll, name);
                }
            }
            *iat = va;
        }
    }
    g_jpog_stubs += stubs;
    if (jpog_verbose || m->missing)
        fprintf(stderr, "[loader] %s: %d imports from lifted modules, %d answered by the host, %d stubs, %d missing\n",
                m->file, lifted, shims, stubs, m->missing);
}

/* For SimJP.exe, also patch the original IAT (at 0x739000) to match the resolved PE IAT.
 * The original IAT slots contain addresses of PE IAT slots, which we now resolve to
 * actual function addresses. We need to update the original IAT to contain those addresses. */
static void patch_original_iat(jpog_module *m) {
    if (!m->is_exe) return;  /* Only for SimJP.exe */
    
    /* The original IAT is at 0x739000 and has 1108 entries */
    uint32_t original_iat_base = 0x739000;
    uint32_t original_iat_count = 1108;
    
    /* For each entry in the original IAT, check if it points to a PE IAT slot */
    for (uint32_t i = 0; i < original_iat_count; i++) {
        uint32_t *orig_slot = (uint32_t *)hle_ptr(original_iat_base + i * 4);
        uint32_t pe_iat_slot = *orig_slot;
        
        /* Check if this looks like a PE IAT slot address (in the 0x00E0XXXX range) */
        if (pe_iat_slot >= 0x00E0E000 && pe_iat_slot < 0x00E10000) {
            /* Read the resolved function address from the PE IAT slot */
            uint32_t func_addr = *(uint32_t *)hle_ptr(pe_iat_slot);
            /* Update the original IAT slot to contain the function address */
            *orig_slot = func_addr;
        }
    }
    
    if (jpog_verbose)
        fprintf(stderr, "[loader] %s: patched original IAT at 0x%08X\n", m->file, original_iat_base);
}

int jpog_load(const char *game) {
    static const struct { const char *file; uint32_t base, entry, stamp; int on_demand; } want[] = JPOG_MODULES;
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) {
        jpog_module *m = &g_jpog_mod[g_jpog_nmod];
        char guest[300], host[4096];
        snprintf(guest, sizeof guest, "C:\\%s", want[i].file);
        
        /* For SimJP.exe, use the patched version from analysis folder if it exists */
        if (strcasecmp(want[i].file, "SimJP.exe") == 0) {
            const char *patched = "work/analysis/simjp/SimJP.exe";
            FILE *test = fopen(patched, "rb");
            if (test) {
                fclose(test);
                strncpy(host, patched, sizeof(host) - 1);
                host[sizeof(host) - 1] = '\0';
                if (jpog_verbose)
                    fprintf(stderr, "[loader] using patched SimJP.exe from %s\n", patched);
            } else {
                if (!hle_host_path(guest, host, sizeof host)) {
                    fprintf(stderr, "[loader] no %s in %s\n", want[i].file, game);
                    return 1;
                }
            }
        } else {
            if (!hle_host_path(guest, host, sizeof host)) {
                fprintf(stderr, "[loader] no %s in %s\n", want[i].file, game);
                return 1;
            }
        }
        if (map_at(host, want[i].base, &m->img) != 0) return 1;
        if (m->img.stamp != want[i].stamp) {
            fprintf(stderr, "[loader] %s is not the build this host was lifted from (PE timestamp 0x%08X)\n",
                    host, m->img.stamp);
            return 1;
        }
        m->file = base_name(want[i].file);
        m->path = want[i].file;
        size_t n = strlen(m->file);
        m->is_exe = n > 4 && !strcasecmp(m->file + n - 4, ".exe");
        m->on_demand = want[i].on_demand;
        if (!m->on_demand) hle_module_add(guest, m->img.base, m->img.resource_rva, m->img.resource_size, m->is_exe);
        g_jpog_nmod++;
        if (jpog_verbose)
            fprintf(stderr, "[loader] %-24s 0x%08X-0x%08X entry 0x%08X\n", m->file, m->img.base,
                    m->img.base + m->img.span, m->img.entry);
    }
    for (int i = 0; i < g_jpog_nmod; i++) {
        bind_module(&g_jpog_mod[i]);
        patch_original_iat(&g_jpog_mod[i]);
    }
    return 0;
}

static uint32_t dll_main(jpog_module *m, uint32_t reason) {
    uint32_t args[3] = { m->img.base, reason, 0 };
    return m->img.entry ? hle_call_guest(m->img.entry, 3, args) : 1;
}

/* DLL_PROCESS_ATTACH, after the modules it imports from, as Windows does.
 * 0 if it is attached (an import cycle's second visit counts as it is). */
int jpog_attach(jpog_module *m) {
    if (m->state) return 0;
    if (m->missing) {
        fprintf(stderr, "[loader] %s: %d imports missing; not attached\n", m->file, m->missing);
        return 1;
    }
    m->state = 1;
    if (m->img.import_rva)
        for (pe_import_descriptor *d = (pe_import_descriptor *)hle_ptr(m->img.base + m->img.import_rva); d->Name; d++) {
            jpog_module *to = jpog_module_named((const char *)hle_ptr(m->img.base + d->Name));
            if (to && !to->is_exe && jpog_attach(to) != 0) { m->state = 0; return 1; }
        }
    if (jpog_verbose) fprintf(stderr, "[loader] %s: DllMain(PROCESS_ATTACH)\n", m->file);
    if (!dll_main(m, 1)) {
        fprintf(stderr, "[loader] %s refused to attach\n", m->file);
        m->state = 0;
        return 1;
    }
    m->state = 2;
    return 0;
}

/* The DLLs loaded at the start. Returns how many could not be attached. */
int jpog_attach_dlls(void) {
    int bad = 0;
    for (int i = 0; i < g_jpog_nmod; i++)
        if (!g_jpog_mod[i].is_exe && !g_jpog_mod[i].on_demand) bad += jpog_attach(&g_jpog_mod[i]) != 0;
    return bad;
}

/* ponytail: a module freed is not detached, so a second load finds it as it
 * was. Windows would start it afresh; do that if the game frees a shader DLL
 * and loads it again. */
uint32_t jpog_load_library(const char *name) {
    jpog_module *m = name ? jpog_module_named(name) : NULL;
    if (!m || m->is_exe) return 0;
    if (m->on_demand && !m->state) {
        char guest[300];
        snprintf(guest, sizeof guest, "C:\\%s", m->path);
        hle_module_add(guest, m->img.base, m->img.resource_rva, m->img.resource_size, 0);
    }
    if (jpog_attach(m) != 0) return 0;
    m->refs++;
    return m->img.base;
}
int jpog_free_library(uint32_t base) {
    jpog_module *m = jpog_module_at(base);
    if (!m) return 0;
    if (m->refs > 0) m->refs--;
    return 1;
}
