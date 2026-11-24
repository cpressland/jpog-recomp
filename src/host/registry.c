/*
 * registry.c - Windows registry emulation for JPOG.
 *
 * The game reads registry keys for settings and paths. We emulate the
 * registry API with a simple in-memory store backed by a JSON file.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "win32hle.h"
#include "jpog.h"

/* Registry keys are stored as "HKEY\path\name" -> value */
#define MAX_REG_ENTRIES 1024
#define MAX_KEY_LEN 512
#define MAX_VALUE_LEN 1024

typedef struct {
    char key[MAX_KEY_LEN];
    uint32_t type;  /* REG_SZ=1, REG_DWORD=4 */
    union {
        char sz[MAX_VALUE_LEN];
        uint32_t dw;
    } value;
} reg_entry;

static reg_entry g_registry[MAX_REG_ENTRIES];
static int g_reg_count = 0;
static uint32_t g_next_handle = 0x100;

/* Simple handle table: handle -> registry key */
typedef struct {
    uint32_t handle;
    char key[MAX_KEY_LEN];
} reg_handle;

#define MAX_HANDLES 64
static reg_handle g_handles[MAX_HANDLES];
static int g_handle_count = 0;

static const char *reg_file = "registry.json";

static void load_registry(void) {
    static int loaded = 0;
    if (loaded) return;
    loaded = 1;

    FILE *f = fopen(reg_file, "r");
    if (!f) {
        /* Create default entries */
        strcpy(g_registry[g_reg_count].key, "HKEY_CURRENT_USER\\Software\\Blue Tongue Technology\\Jurassic Park Operation Genesis\\Version");
        g_registry[g_reg_count].type = 1;
        strcpy(g_registry[g_reg_count].value.sz, "1.0");
        g_reg_count++;

        strcpy(g_registry[g_reg_count].key, "HKEY_CURRENT_USER\\Software\\Blue Tongue Technology\\Jurassic Park Operation Genesis\\InstallPath");
        g_registry[g_reg_count].type = 1;
        strcpy(g_registry[g_reg_count].value.sz, "C:\\");
        g_reg_count++;
        return;
    }

    /* Simple JSON parsing - just read key-value pairs */
    char line[2048];
    while (fgets(line, sizeof(line), f) && g_reg_count < MAX_REG_ENTRIES) {
        char *key_start = strstr(line, "\"key\": \"");
        char *type_start = strstr(line, "\"type\": ");
        char *value_start = strstr(line, "\"value\": \"");

        if (key_start && type_start && value_start) {
            key_start += 8;
            char *key_end = strchr(key_start, '"');
            if (key_end) {
                *key_end = 0;
                strncpy(g_registry[g_reg_count].key, key_start, MAX_KEY_LEN - 1);

                type_start += 8;
                g_registry[g_reg_count].type = atoi(type_start);

                value_start += 10;
                char *value_end = strchr(value_start, '"');
                if (value_end) {
                    *value_end = 0;
                    if (g_registry[g_reg_count].type == 4) {
                        g_registry[g_reg_count].value.dw = strtoul(value_start, NULL, 10);
                    } else {
                        strncpy(g_registry[g_reg_count].value.sz, value_start, MAX_VALUE_LEN - 1);
                    }
                    g_reg_count++;
                }
            }
        }
    }
    fclose(f);
}

static void save_registry(void) {
    FILE *f = fopen(reg_file, "w");
    if (!f) return;

    fprintf(f, "[\n");
    for (int i = 0; i < g_reg_count; i++) {
        fprintf(f, "  {\"key\": \"%s\", \"type\": %u, \"value\": \"%s\"}%s\n",
                g_registry[i].key, g_registry[i].type,
                g_registry[i].type == 4 ? "" : g_registry[i].value.sz,
                i < g_reg_count - 1 ? "," : "");
    }
    fprintf(f, "]\n");
    fclose(f);
}

static reg_entry *find_entry(const char *key) {
    for (int i = 0; i < g_reg_count; i++) {
        if (strcmp(g_registry[i].key, key) == 0) {
            return &g_registry[i];
        }
    }
    return NULL;
}

static uint32_t add_handle(const char *key) {
    if (g_handle_count >= MAX_HANDLES) return 0;
    g_handles[g_handle_count].handle = g_next_handle++;
    strncpy(g_handles[g_handle_count].key, key, MAX_KEY_LEN - 1);
    return g_handles[g_handle_count++].handle;
}

static const char *get_handle_key(uint32_t handle) {
    for (int i = 0; i < g_handle_count; i++) {
        if (g_handles[i].handle == handle) {
            return g_handles[i].key;
        }
    }
    return NULL;
}

/* HKEY constants */
#define HKEY_CLASSES_ROOT       0x80000000
#define HKEY_CURRENT_USER       0x80000001
#define HKEY_LOCAL_MACHINE      0x80000002
#define HKEY_USERS              0x80000003

static const char *hkey_name(uint32_t hkey) {
    switch (hkey) {
        case HKEY_CLASSES_ROOT: return "HKEY_CLASSES_ROOT";
        case HKEY_CURRENT_USER: return "HKEY_CURRENT_USER";
        case HKEY_LOCAL_MACHINE: return "HKEY_LOCAL_MACHINE";
        case HKEY_USERS: return "HKEY_USERS";
        default: return NULL;
    }
}

/* RegOpenKeyExA - open a registry key */
static void reg_open_key_ex_a(void) {
    uint32_t hkey = A32(0);
    uint32_t subkey = A32(1);
    uint32_t reserved = A32(2);
    uint32_t access = A32(3);
    uint32_t result = A32(4);

    load_registry();

    const char *root = hkey_name(hkey);
    const char *sub = subkey ? (const char *)hle_ptr(subkey) : "";

    if (!root) {
        hle_set_last_error(87);  /* ERROR_INVALID_PARAMETER */
        RET(87, 5);
    }

    char full_key[MAX_KEY_LEN];
    if (sub && *sub) {
        snprintf(full_key, sizeof(full_key), "%s\\%s", root, sub);
    } else {
        strncpy(full_key, root, MAX_KEY_LEN - 1);
    }

    uint32_t handle = add_handle(full_key);
    if (!handle) {
        hle_set_last_error(8);  /* ERROR_NOT_ENOUGH_MEMORY */
        RET(8, 5);
    }

    if (result) {
        MEM32(result) = handle;
    }

    RET(0, 5);  /* ERROR_SUCCESS */
}

/* RegQueryValueExA - query a registry value */
static void reg_query_value_ex_a(void) {
    uint32_t hkey = A32(0);
    uint32_t name = A32(1);
    uint32_t reserved = A32(2);
    uint32_t type = A32(3);
    uint32_t data = A32(4);
    uint32_t size = A32(5);

    load_registry();

    const char *key = get_handle_key(hkey);
    if (!key) {
        hle_set_last_error(6);  /* ERROR_INVALID_HANDLE */
        RET(6, 6);
    }

    const char *value_name = name ? (const char *)hle_ptr(name) : "";
    char full_key[MAX_KEY_LEN];
    if (value_name && *value_name) {
        snprintf(full_key, sizeof(full_key), "%s\\%s", key, value_name);
    } else {
        strncpy(full_key, key, MAX_KEY_LEN - 1);
    }

    reg_entry *entry = find_entry(full_key);
    if (!entry) {
        hle_set_last_error(2);  /* ERROR_FILE_NOT_FOUND */
        RET(2, 6);
    }

    if (type) {
        MEM32(type) = entry->type;
    }

    if (entry->type == 1) {  /* REG_SZ */
        uint32_t len = strlen(entry->value.sz) + 1;
        if (size) {
            uint32_t buf_size = MEM32(size);
            if (buf_size < len) {
                hle_set_last_error(234);  /* ERROR_MORE_DATA */
                RET(234, 6);
            }
            MEM32(size) = len;
        }
        if (data) {
            memcpy(hle_ptr(data), entry->value.sz, len);
        }
    } else if (entry->type == 4) {  /* REG_DWORD */
        if (size) {
            uint32_t buf_size = MEM32(size);
            if (buf_size < 4) {
                hle_set_last_error(234);
                RET(234, 6);
            }
            MEM32(size) = 4;
        }
        if (data) {
            MEM32(data) = entry->value.dw;
        }
    }

    RET(0, 6);  /* ERROR_SUCCESS */
}

/* RegCloseKey - close a registry key handle */
static void reg_close_key(void) {
    uint32_t hkey = A32(0);
    for (int i = 0; i < g_handle_count; i++) {
        if (g_handles[i].handle == hkey) {
            g_handles[i] = g_handles[--g_handle_count];
            RET(0, 1);
        }
    }
    RET(6, 1);  /* ERROR_INVALID_HANDLE */
}

/* RegSetValueExA - set a registry value */
static void reg_set_value_ex_a(void) {
    uint32_t hkey = A32(0);
    uint32_t name = A32(1);
    uint32_t reserved = A32(2);
    uint32_t type = A32(3);
    uint32_t data = A32(4);
    uint32_t size = A32(5);

    load_registry();

    const char *key = get_handle_key(hkey);
    if (!key) {
        hle_set_last_error(6);
        RET(6, 6);
    }

    const char *value_name = name ? (const char *)hle_ptr(name) : "";
    char full_key[MAX_KEY_LEN];
    if (value_name && *value_name) {
        snprintf(full_key, sizeof(full_key), "%s\\%s", key, value_name);
    } else {
        strncpy(full_key, key, MAX_KEY_LEN - 1);
    }

    reg_entry *entry = find_entry(full_key);
    if (!entry) {
        if (g_reg_count >= MAX_REG_ENTRIES) {
            hle_set_last_error(8);
            RET(8, 6);
        }
        entry = &g_registry[g_reg_count++];
        strncpy(entry->key, full_key, MAX_KEY_LEN - 1);
    }

    entry->type = type;
    if (type == 1) {  /* REG_SZ */
        const char *str = data ? (const char *)hle_ptr(data) : "";
        strncpy(entry->value.sz, str, MAX_VALUE_LEN - 1);
    } else if (type == 4) {  /* REG_DWORD */
        entry->value.dw = data ? MEM32(data) : 0;
    }

    save_registry();
    RET(0, 6);
}

const win32hle_shim jpog_registry_shims[] = {
    { "RegOpenKeyExA",        reg_open_key_ex_a },
    { "RegQueryValueExA",     reg_query_value_ex_a },
    { "RegCloseKey",          reg_close_key },
    { "RegSetValueExA",       reg_set_value_ex_a },
    { NULL, NULL }
};
