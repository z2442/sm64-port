/* PSP slim TV output (experimental). Uses the kernel module psp/dvemgr (adapted from z2442's OoT port, which in turn follows what
 * Daedalus did): it is bundled in the game and loaded with pspSdkLoadStartModule(.., PSP_MEMORY_PARTITION_KERNEL), as that port does. If anything fails at any point, the
 * console screen stays. Safety measures:
 * - On a PSP slim the module is loaded and VRAM goes to 4 MB at BOOT (as the Zelda port does); with that the TV can be turned on and off from the
 * menu WITHOUT restarting. On the console (TV Off) the game draws exactly as before. Without module / PSP-1000: everything stays as usual.
 * - A cable is NOT required: the console does not always report the component cable (it gives 0 even though there is an image on the TV), so component is assumed unless
 * the console says it is composite.
 * - tv_boot.tmp is created before routing the output to the TV and deleted after ~5 s of play (or when the TV is turned off); if the file is still there at boot, the
 * previous attempt did not work: it boots on the console and the setting goes back to Off. */
#include <pspkernel.h>
#include <pspsdk.h>
#include <pspctrl.h>
#include <pspge.h>
#include <pspdisplay.h>
#include <pspaudiorouting.h>
#include <stdio.h>
#include <string.h>

#include "psp_dve.h"
#include "fs_utils.h"

int pspDveMgrCheckVideoOut(void);
int pspDveMgrSetVideoOut(int output, int mode, int width, int height, int x, int y, int z);
int pspDveMgrSetEdramSize(unsigned int size);
int pspDveMgrGetModel(void);

/* The module (psp/dvemgr, built by the Makefile) lives INSIDE SM64.PRX: dvemgr/dvemgr_prx.inc defines sDveModule[]. */
#include "dvemgr/dvemgr_prx.inc"
/* Cable diagnostic module (read only, temporary): dvediag/dvediag_prx.inc defines sDiagModule[]. */
int pspDveDiagRead(int *out);
#include "dvediag/dvediag_prx.inc"
/* System native TV mode module (read only): dvenat/dvenat_prx.inc defines sNatModule[]. */
int pspDveNatState(void);
#include "dvenat/dvenat_prx.inc"

#define DVE_MODULE "dvemgr.prx" /* temporary name in the savedata folder */
#define DVE_MARKER "tv_boot.tmp"
#define DVE_EDRAM (4 * 1024 * 1024)
#define DVE_OK_FRAMES 300 /* ~5 s a 60 fps */

int gPspTvActive;

static SceUID sMod = -1;
static int sLoadErr;   /* module load error (0 = ok) */
static int sModel = -1;
static int sCableRaw;  /* last raw value of the cable detection */
static int sProbed;
static int sReady;     /* module loaded, PSP slim and 4 MB VRAM requested: the TV can be turned on / off live */
static int sEdramErr;  /* result of requesting the 4 MB (0 = ok) */
static int sAvail;
static int sMarked;
static int sTicks;
static int sNativeTv;  /* the game booted with the console already in its native TV mode */

static void psp_dve_diag_load(void);

int psp_dve_probe(void) {
    char path[FS_MAX_PATH];
    if (sProbed) {
        return sAvail;
    }
    sProbed = 1;
    sLoadErr = 0;
    /* it is written to the savedata folder (always writable), loaded and deleted: no separate file needs to be copied */
    fs_resolve_path(path, sizeof(path), DVE_MODULE);
    {
        SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        if (fd < 0) {
            printf("dve: cannot write module (0x%08X)\n", (unsigned int) fd);
            sLoadErr = fd;
            sProbed = 0; /* it is retried next time */
            return 0;
        }
        sceIoWrite(fd, sDveModule, sizeof(sDveModule));
        sceIoClose(fd);
    }
    sMod = pspSdkLoadStartModule(path, PSP_MEMORY_PARTITION_KERNEL);
    sceIoRemove(path);
    if (sMod < 0) {
        printf("dve: module not loaded (0x%08X)\n", (unsigned int) sMod);
        sLoadErr = sMod;
        sProbed = 0; /* it is retried next time */
        return 0;
    }
    sModel = pspDveMgrGetModel();
    if (sModel > 0) { /* 0 = PSP-1000 (has no TV output) */
        sAvail = 1;
        psp_dve_diag_load();
    }
    return sAvail;
}

/* Cable type latched by the cable watcher once it has been read the same way on two checks in a row (0 = none latched). While the system is changing the video
 * output (SCREEN / brightness button) the raw signal can read differently for a moment; routing the TV with a wrong value leaves the output "up" but without a picture. */
static int sCableLatch;
void psp_dve_cable_latch(int state) {
    sCableLatch = (state > 0) ? state : 0;
}

int psp_dve_cable(void) {
    if (!sAvail) {
        return PSP_CABLE_NONE;
    }
    sCableRaw = (sCableLatch > 0) ? sCableLatch : psp_dve_cable_state(); /* the latched value is not disturbed by the transient readings of a SCREEN-button press */
    if (sCableRaw < 0) {
        sCableRaw = pspDveMgrCheckVideoOut(); /* without the cable module: the old signal (with the component cable it gives 0 even though there is an image on the TV) */
    }
    /* without a cable or without data component is assumed (so the row can be chosen before plugging in the cable) */
    return (sCableRaw == PSP_CABLE_COMPOSITE) ? PSP_CABLE_COMPOSITE : PSP_CABLE_COMPONENT;
}

/* Video cable detection: a separate read-only module (psp/dvediag, "cable module"). It is loaded when testing the TV module; if it does not load (or any of its
 * functions does not exist in the firmware) the TV keeps working the same, only without knowing whether there is a cable. It reads several signals (v[0..6]: headphones, remote, mic, Hprm
 * state, syscon HPConnect, return and the value of sceImposeCheckVideoOut). On a PSP-3000 with a component cable: impose gives 0 and value 2 (no cable: error). */
static SceUID sDiagMod = -1;
static int sDiagErr = 1; /* 1 = not attempted, 0 = loaded, other = error */
static SceUID sNatMod = -1;
static int sNatErr = 1;

static void psp_dve_diag_load(void) {
    char path[FS_MAX_PATH];
    SceUID fd;
    if (sDiagErr != 1) {
        return;
    }
    sDiagErr = -2;
    fs_resolve_path(path, sizeof(path), "dvediag.prx");
    fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) {
        sDiagErr = fd;
        return;
    }
    sceIoWrite(fd, sDiagModule, sizeof(sDiagModule));
    sceIoClose(fd);
    sDiagMod = pspSdkLoadStartModule(path, PSP_MEMORY_PARTITION_KERNEL);
    sceIoRemove(path);
    sDiagErr = (sDiagMod < 0) ? sDiagMod : 0;
    /* separate module for the native TV mode: if it does not load, only that detection is lost */
    sNatErr = -2;
    fs_resolve_path(path, sizeof(path), "dvenat.prx");
    fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, sNatModule, sizeof(sNatModule));
        sceIoClose(fd);
        sNatMod = pspSdkLoadStartModule(path, PSP_MEMORY_PARTITION_KERNEL);
        sceIoRemove(path);
        sNatErr = (sNatMod < 0) ? sNatMod : 0;
    }
}

/* System native TV mode (the one turned on with the Display button): -1 = unknown, 0 = off, 1 = on, 2 = changing. */
int psp_dve_native_state(void) {
    if (!sAvail || sNatErr != 0) {
        return -1;
    }
    return pspDveNatState();
}

/* Cable state: -1 = unknown (no cable module), 0 = none, 1 = composite, 2 = component. */
int psp_dve_cable_state(void) {
    int v[7];
    if (!sAvail || sDiagErr != 0) {
        return -1;
    }
    pspDveDiagRead(v);
    if (v[5] != 0 || v[6] <= 0) {
        return PSP_CABLE_NONE; /* sceImposeCheckVideoOut gives an error if there is no video cable */
    }
    return (v[6] == 1) ? PSP_CABLE_COMPOSITE : PSP_CABLE_COMPONENT;
}

int psp_dve_status(int *detail) {
    *detail = 0;
    if (sLoadErr != 0) {
        *detail = sLoadErr;
        return PSP_DVE_LOAD_FAILED;
    }
    if (!sAvail) {
        *detail = sModel;
        return sProbed ? PSP_DVE_PSP1000 : PSP_DVE_NOT_PROBED;
    }
    if (sEdramErr < 0) {
        *detail = sEdramErr;
        return PSP_DVE_LOAD_FAILED;
    }
    return sReady ? PSP_DVE_READY : PSP_DVE_SAFE;
}

int psp_dve_tv_ready(void) {
    return sReady;
}

static int dve_marker_exists(void) {
    char path[FS_MAX_PATH];
    FILE *f;
    fs_resolve_path(path, sizeof(path), DVE_MARKER);
    f = fopen(path, "rb");
    if (f == NULL) {
        return 0;
    }
    fclose(f);
    return 1;
}

static void dve_marker_write(int on) {
    char path[FS_MAX_PATH];
    fs_resolve_path(path, sizeof(path), DVE_MARKER);
    if (on) {
        FILE *f = fopen(path, "wb");
        if (f != NULL) {
            fputs("tv\n", f);
            fclose(f);
        }
    } else {
        remove(path);
    }
}

int psp_dve_boot_mode(int wanted) {
    int failed_before = 0;
    gPspTvActive = 0;
    sReady = 0;
    if (wanted > PSP_TV_OFF && dve_marker_exists()) { /* the previous attempt with TV did not reach ~5 s of play */
        dve_marker_write(0);
        psp_options_tv_off();
        wanted = PSP_TV_OFF;
        failed_before = 1;
    }
    if (!psp_dve_probe()) {
        return 0;
    }
    {   /* The console is already in its own TV mode (system native video output, Display button): if the game starts like that, it comes out stretched and off screen.
         * It is detected BEFORE touching VRAM, the output is left in the console's normal mode (the same sequence as turning the TV off from the menu, which already works) and
         * the TV is taken over with our own output (480i, which shows on any TV) only for this session (the saved setting does not change). */
        int mode = 0, w = 0, h = 0;
        int natState = psp_dve_native_state();
        const int gotMode = (sceDisplayGetMode(&mode, &w, &h) >= 0);
        /* native mode = the system's internal state says it is on (or changing); if unknown, a display mode different from the console's */
        const int native = !failed_before && (natState == 1 || natState == 2 || (natState < 0 && gotMode && mode != 0));
        if (native) {
            sNativeTv = 1;
            (void) psp_dve_set_video_out(0);
            (void) sceDisplaySetMode(0, 480, 272);
            sceKernelDelayThread(300 * 1000);
            if (wanted == PSP_TV_OFF) {
                wanted = PSP_TV_480I;
                psp_options_tv_set(wanted);
            }
        }
    }
    /* VRAM goes to 4 MB (needed for the 32-bit TV buffers); it has to be BEFORE anyone reserves VRAM */
    sEdramErr = pspDveMgrSetEdramSize(DVE_EDRAM);
    if (sEdramErr < 0) {
        printf("dve: edram 4MB failed (0x%08X)\n", (unsigned int) sEdramErr);
        return 0;
    }
    sReady = 1;
    if (wanted > PSP_TV_OFF && !sNativeTv && psp_dve_cable_state() == PSP_CABLE_NONE) {
        return 0; /* there is a detectable cable and it is not plugged in: boots on the console screen (the setting is kept: when the cable is connected the TV turns on by itself) */
    }
    if (wanted > PSP_TV_OFF) {
        if (wanted == PSP_TV_480P && psp_dve_cable() == PSP_CABLE_COMPOSITE) {
            wanted = PSP_TV_480I; /* 480p needs the component cable */
        }
        psp_dve_mark_attempt();
        gPspTvActive = wanted;
    }
    return gPspTvActive;
}

/* Before routing the output to the TV: the marker is created (deleted after ~5 s of play with TV, or when it is turned off). */
void psp_dve_mark_attempt(void) {
    dve_marker_write(1);
    sMarked = 1;
    sTicks = 0;
}

void psp_dve_mark_clear(void) {
    if (sMarked) {
        sMarked = 0;
        dve_marker_write(0);
    }
}

int psp_dve_set_video_out(int mode) {
    if (!sAvail) {
        return -1;
    }
    if (mode == PSP_TV_480I || mode == PSP_TV_240P) { /* like the Zelda port: mode 0x1D1 with 720x503 (two 240-line fields in a 503 buffer). 240p uses the same path (the only one
                                                       * that has been seen working on a real TV): asking the DVE for 0x1D1 with 320x240 did not engage and the output stayed on the console screen */
        return pspDveMgrSetVideoOut((psp_dve_cable() == PSP_CABLE_COMPOSITE) ? 2 : 0, 0x1D1, 720, 503, 1, 15, 0);
    }
    if (mode == PSP_TV_480P) {
        return pspDveMgrSetVideoOut(0, 0x1D2, 720, 480, 1, 15, 0);
    }
    return pspDveMgrSetVideoOut(0, 0, 480, 272, 1, 15, 0); /* console screen */
}

void psp_dve_frame_tick(void) {
    if (sMarked && ++sTicks >= DVE_OK_FRAMES) {
        sMarked = 0;
        dve_marker_write(0);
    }
}

/* When booting with the console screen: if the PSP came in its own TV mode (the system's video output), the display mode / the DVE stay as the
 * system left them and the game comes out stretched and off screen. Everything is put back in the console's normal mode (the same sequence as turning the TV off from the menu). */
void psp_dve_reset_lcd_if_tv(void) {
    int mode = 0, w = 0, h = 0;
    int bad;
    if (!sAvail) {
        return;
    }
    bad = (sceDisplayGetMode(&mode, &w, &h) < 0) || mode != 0 || w != 480 || h != 272;
    if (!bad && psp_dve_cable_state() > 0) {
        bad = 1; /* with a video cable plugged in, the mode the system reports is not trusted */
    }
    if (bad) {
        (void) psp_dve_set_video_out(0);
        (void) sceDisplaySetMode(0, 480, 272);
    }
}

/* Audio with the video cable plugged in and TV Output at Off: the AV cable goes in the same jack as the headphones and the system sends the sound there (the console's
 * speakers go silent). With want = 1 the "speakers only" output mode is requested (sceAudioRoutingSetMode: 0 = automatic, the normal one; 1 = speakers without switching to headphones;
 * 2 = always speakers; 3 = always headphones) and with 0 it goes back to automatic. Only called by the cable watcher when there really is a VIDEO cable (not with headphones). */
static int sRouteSpk;
void psp_dve_audio_route(int want) {
    want = (want != 0);
    if (want != sRouteSpk) {
        (void) sceAudioRoutingSetMode(want ? 2 : 0);
        sRouteSpk = want;
    }
}

/* After waking up from sleep (game thread, GE idle). Returns 1 if the TV can still be used (VRAM is at 4 MB), 0 if not. The audio routing is
 * applied again on the next cable check (the system may have reset it while sleeping). */
int psp_dve_resume(void) {
    unsigned int size;
    sRouteSpk = -1;
    if (!sReady) {
        return 0;
    }
    size = (unsigned int) sceGeEdramGetSize();
    if (size < DVE_EDRAM) {
        const int r = pspDveMgrSetEdramSize(DVE_EDRAM);
        size = (unsigned int) sceGeEdramGetSize();
        printf("dve: resume, VRAM was below 4 MB, request -> 0x%08X, now %u bytes\n", (unsigned int) r, size);
    }
    return size >= DVE_EDRAM;
}

void psp_dve_shutdown(void) {
    psp_dve_audio_route(0); /* on exit the system goes back to automatic mode (otherwise the headphones could be left without sound) */
    if (gPspTvActive && sAvail) {
        (void) psp_dve_set_video_out(0);
        (void) sceDisplaySetMode(0, 480, 272);
    }
}
