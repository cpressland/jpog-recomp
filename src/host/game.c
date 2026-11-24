/*
 * game.c - Run the actual game (SimJP.exe).
 *
 * This calls the game's main function at 0x484330 with proper setup.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32hle.h"
#include "jpog.h"
#include "recomp_funcs.h"

/* The game's real main function (from survey.json) */
#define GAME_MAIN 0x484330

/* TApplication constructor and global object address */
#define TAPPLICATION_CTOR 0x01701150
#define TAPPLICATION_OBJ 0x008B8580

int jpog_run_game(void) {
    /* Find SimJP.exe module */
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

    fprintf(stderr, "[game] Initializing global TApplication object at 0x%08X\n", TAPPLICATION_OBJ);
    
    /* Call TApplication constructor to initialize the global object */
    /* The constructor is a thiscall with 'this' in ecx */
    recomp_func_t ctor = recomp_lookup(TAPPLICATION_CTOR);
    if (!ctor) {
        fprintf(stderr, "[game] TApplication constructor not found at 0x%08X\n", TAPPLICATION_CTOR);
        return 1;
    }
    
    uint32_t saved_esp = g_esp;
    PUSH32(g_esp, RECOMP_RETADDR);
    g_ecx = TAPPLICATION_OBJ;
    ctor();
    g_esp = saved_esp;
    
    fprintf(stderr, "[game] TApplication initialized, vtable at 0x%08X\n", MEM32(TAPPLICATION_OBJ));

    fprintf(stderr, "[game] Running SimJP.exe at 0x%08X\n", GAME_MAIN);

    /* Set up argc and argv on the guest stack */
    /* The game expects: int main(int argc, char **argv) */
    uint32_t argc = 1;
    
    /* Allocate argv array on guest stack */
    uint32_t argv_str = hle_va(hle_alloc(16));  /* "C:\\SimJP.exe\0" */
    memcpy(hle_ptr(argv_str), "C:\\SimJP.exe\0", 14);
    
    uint32_t argv = hle_va(hle_alloc(8));  /* pointer to argv[0] */
    MEM32(argv) = argv_str;
    
    /* Call the game's main function */
    uint32_t args[2] = { argc, argv };
    uint32_t result = hle_call_guest(GAME_MAIN, 2, args);
    
    fprintf(stderr, "[game] SimJP.exe returned %u\n", result);
    return result;
}
