/* Minimal loader of the port. When launching a game, the PSP loads the whole EBOOT.PBP in a slow way (with the entire game, 14.5 MB, it took
 * ~27 s until anything was seen). That is why the EBOOT.PBP is just this loader (a few KB) and the game (SM64.PRX, in the same folder) is loaded from
 * here with sceKernelLoadModule, which is much faster. It does nothing else: it starts the game and goes to sleep (using no CPU). */
#include <pspkernel.h>
#include <pspdebug.h>
#include <stdio.h>
#include <string.h>

PSP_MODULE_INFO("SM64LAUNCH", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_MAIN_THREAD_STACK_SIZE_KB(16);
PSP_HEAP_SIZE_KB(16); /* the game reserves almost all the free memory: this loader must not take anything from it */

static void fail(const char *what, int code) {
    pspDebugScreenInit();
    pspDebugScreenPrintf("Super Mario 64 (PSP)\n\n%s\nError: 0x%08X\n\n", what, (unsigned int) code);
    pspDebugScreenPrintf("Comprueba que SM64.PRX esta en la misma carpeta que EBOOT.PBP.\n");
    pspDebugScreenPrintf("Check that SM64.PRX is in the same folder as EBOOT.PBP.\n");
    sceKernelDelayThread(15 * 1000000);
    sceKernelExitGame();
}

int main(int argc, char **argv) {
    char path[300];
    char arg0[300];
    char *slash;
    SceUID mod;
    int status = 0;
    int r;

    if (argc < 1 || argv == NULL || argv[0] == NULL) {
        fail("No se conoce la ruta del EBOOT.", -1);
        return 0;
    }
    strncpy(arg0, argv[0], sizeof(arg0) - 1);
    arg0[sizeof(arg0) - 1] = 0;
    strcpy(path, arg0);
    slash = strrchr(path, '/');
    if (slash == NULL) {
        slash = strrchr(path, '\\');
    }
    if (slash == NULL) {
        fail("Ruta del EBOOT no valida.", -2);
        return 0;
    }
    strcpy(slash + 1, "SM64.PRX");

    mod = sceKernelLoadModule(path, 0, NULL);
    if (mod < 0) {
        fail(path, mod);
        return 0;
    }

    r = sceKernelStartModule(mod, strlen(arg0) + 1, arg0, &status, NULL);
    if (r < 0) {
        fail("sceKernelStartModule", r);
        return 0;
    }
    sceKernelSleepThread(); /* the game runs in its own threads and manages HOME / exit */
    return 0;
}
