/* Cargador minimo del port. Al lanzar un juego, la PSP carga el EBOOT.PBP completo de un modo lento (con el juego entero, 14,5 MB, pasaban
 * ~27 s hasta ver algo). Por eso el EBOOT.PBP es solo este cargador (unos KB) y el juego (SM64.PRX, en la misma carpeta) se carga desde
 * aqui con sceKernelLoadModule, que es mucho mas rapido. No hace nada mas: arranca el juego y se queda dormido (sin usar CPU). */
#include <pspkernel.h>
#include <pspdebug.h>
#include <stdio.h>
#include <string.h>

PSP_MODULE_INFO("SM64LAUNCH", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_MAIN_THREAD_STACK_SIZE_KB(16);
PSP_HEAP_SIZE_KB(16); /* el juego reserva casi toda la memoria libre: este cargador no debe quitarle nada */

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
    sceKernelSleepThread(); /* el juego corre en sus propios hilos y gestiona HOME / salida */
    return 0;
}
