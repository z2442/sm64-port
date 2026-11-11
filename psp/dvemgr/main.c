/*
 * sm64DveMgr: MINIMAL kernel module for the PSP slim's TV output (component / composite).
 *
 * It is adapted from the "dvemgr" module of z2442's Ocarina of Time port for PSP (https://github.com/z2442/oot-psp), which in turn
 * follows the TV output path of the Daedalus emulator. Changes from the original:
 * - It does NOT change the VRAM size when loaded (module_start does nothing): only pspDveMgrSetEdramSize() does, when the game asks for it.
 * - It exports pspDveMgrGetModel() so the game does not have to link against kubridge (this way it boots on any firmware).
 *
 * The module is embedded in SM64.PRX and the game writes and loads it by itself (see psp_dve.c); without it the TV output is simply not available.
 */
#include <pspsdk.h>
#include <pspkernel.h>
#include <pspsysevent.h>

#include <stdio.h>
#include <string.h>
#include <pspge.h>

PSP_MODULE_INFO("sm64DveMgr_Module", 0x1006, 1, 0);

int sceHprmIsHeadphoneExist();
int sceImposeSetVideoOutMode(int, int, int);
int sceDve_driver_DEB2F80C(int);
int sceDve_driver_93828323(int);
int sceDve_driver_0B85524C(int);
int sceDve_driver_A265B504(int, int, int);
int sceGeEdramSetSizeDriver(int);
int sceGeEdramSetSizeUser(int);
int sceKernelGetModel(void);

#define RETURN(x) res = x; pspSdkSetK1(k1); return x

/* 0 = PSP-1000, 1 = 2000, 2 = 3000, ... (the same value as kuKernelGetModel) */
int pspDveMgrGetModel(void) {
    int k1 = pspSdkSetK1(0);
    int model = sceKernelGetModel();

    pspSdkSetK1(k1);
    return model;
}

/* Video cable: 0 = none, 1 = composite, 2 = component */
int pspDveMgrCheckVideoOut() {
    int k1 = pspSdkSetK1(0);
    int intr = sceKernelCpuSuspendIntr();

    /* Warning from the original: the NID changed between firmware 3.60 and 3.71. */
    int cable = sceHprmIsHeadphoneExist();

    sceKernelCpuResumeIntr(intr);
    pspSdkSetK1(k1);
    return cable;
}

int pspDveMgrSetVideoOut(int u, int mode, int width, int height, int x, int y, int z) {
    int k1 = pspSdkSetK1(0);
    int res = sceDve_driver_DEB2F80C(u);
    if (res < 0) {
        RETURN(-1);
    }

    /* These parameters end up in sceDisplaySetMode. */
    res = sceImposeSetVideoOutMode(mode, width, height);
    if (res < 0) {
        RETURN(-2);
    }

    res = sceDve_driver_93828323(0);
    if (res < 0) {
        RETURN(-3);
    }

    res = sceDve_driver_0B85524C(1);
    if (res < 0) {
        RETURN(-4);
    }

    res = sceDve_driver_A265B504(x, y, z);
    if (res < 0) {
        RETURN(-5);
    }

    pspSdkSetK1(k1);
    return res;
}

/* Asks the system for the slim's full VRAM (4 MB). It must be called BEFORE sceGuInit. */
int pspDveMgrSetEdramSize(unsigned int size) {
    int k1 = pspSdkSetK1(0);
    int res = sceGeEdramSetSizeUser((int) size);

    if (res < 0) {
        res = sceGeEdramSetSizeDriver((int) size);
    }
    pspSdkSetK1(k1);
    return res;
}

int module_start(SceSize args, void *argp) {
    return 0;
}

int module_stop(SceSize args, void *argp) {
    return 0;
}
