#ifdef TARGET_PSP
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <pspkernel.h>
#include <psppower.h>
#include <pspge.h>
#include <psprtc.h>

#include "sm64.h"
#include "game/area.h"
#include "game/level_update.h"
#include "game/game_init.h"
#include "fs_utils.h"
#include "psp_watchdog.h"

extern int gFrame;                       /* pc_main.c: finished frames */
extern volatile int gPspSuspending;      /* gfx_scegu.c */
extern unsigned int gPspMePolls, gPspMeUs, gPspMeBlocks, gPspMeWaits, gPspMeTimeouts, gPspMeRecovered, gPspMeResumeOk, gPspMeResumeFail; /* psp_me.c */
void gfx_scegu_wd_describe(char *out, int size); /* gfx_scegu.c */

const char *volatile gPspWdPhase = "boot";

#define WD_MAX_THREADS 8
#define WD_FRAMES_STILL 4   /* seconds without new frames (a level load takes 1-2 at most) */
#define WD_BOOT_STILL 40    /* before the first frame (boot takes ~15-25 s) */

static struct {
    const char *label;
    SceUID id;
    unsigned int runPrev; /* CPU clock consumed (us, low 32 bits) at the previous read */
    unsigned int runDelta; /* us of CPU in the last second */
} sTh[WD_MAX_THREADS];
static int sThN;
static char sPath[256];

void psp_watchdog_register(const char *label, int thid) {
    if (sThN < WD_MAX_THREADS && thid >= 0) {
        sTh[sThN].label = label;
        sTh[sThN].id = thid;
        sTh[sThN].runPrev = 0;
        sTh[sThN].runDelta = 0;
        sThN++;
    }
}

static void wd_sample(void) {
    int i;
    for (i = 0; i < sThN; i++) {
        SceKernelThreadInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelReferThreadStatus(sTh[i].id, &info) >= 0) {
            sTh[i].runDelta = info.runClocks.low - sTh[i].runPrev;
            sTh[i].runPrev = info.runClocks.low;
        }
    }
}

/* No big fixed buffer (the game's memory is very tight): each line is built in a small stack buffer and written to the file. */
static void wd_put(SceUID fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void wd_put(SceUID fd, const char *fmt, ...) {
    char b[256];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    if (n > (int) sizeof(b) - 1) {
        n = (int) sizeof(b) - 1;
    }
    if (fd >= 0 && n > 0) {
        sceIoWrite(fd, b, (SceSize) n);
    }
}

static void wd_write(int secs, int first) {
    char b[256];
    ScePspDateTime t;
    SceUID fd = sceIoOpen(sPath, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    int i;

    if (fd < 0) {
        return;
    }
    sceRtcGetCurrentClockLocalTime(&t);
    wd_put(fd, "SM64 PSP - freeze report (%s)\n", first ? "first warning" : "still frozen");
    wd_put(fd, "date %04d-%02d-%02d %02d:%02d:%02d, built %s %s\n", t.year, t.month, t.day, t.hour, t.minute, t.second, __DATE__, __TIME__);
    wd_put(fd, "no new frames for %d s (frame %d), the main thread was in: %s\n", secs, gFrame, (const char *) gPspWdPhase);
    wd_put(fd, "level %d, area %d, game time %u\n", (int) gCurrLevelNum, (int) gCurrAreaIndex, (unsigned int) gGlobalTimer);
    if (gMarioState != NULL) {
        wd_put(fd, "Mario: action %08X at %d,%d,%d\n", (unsigned int) gMarioState->action, (int) gMarioState->pos[0], (int) gMarioState->pos[1], (int) gMarioState->pos[2]);
    }
    for (i = 0; i < sThN; i++) {
        SceKernelThreadInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelReferThreadStatus(sTh[i].id, &info) < 0) {
            wd_put(fd, "thread %s: could not read\n", sTh[i].label);
        } else {
            /* state: 1 running, 2 ready, 4 waiting, 8 suspended, 16 sleeping, 32 dead */
            wd_put(fd, "thread %s: state %d, wait type %d on %08X, priority %d, CPU last second %u us\n", sTh[i].label, info.status, info.waitType,
                   (unsigned int) info.waitId, info.currentPriority, sTh[i].runDelta);
        }
    }
    wd_put(fd, "GE: %d (0 done, 1 queued, 2 drawing, 3 stalling, 4 paused)\n", sceGeDrawSync(1));
    b[0] = 0;
    gfx_scegu_wd_describe(b, (int) sizeof(b));
    wd_put(fd, "%s", b);
    wd_put(fd, "ME: polls %u, blocks %u, waits %u, time waited %u us, timeouts %u, recovered %u, resumes %u ok / %u failed\n", gPspMePolls, gPspMeBlocks,
           gPspMeWaits, gPspMeUs, gPspMeTimeouts, gPspMeRecovered, gPspMeResumeOk, gPspMeResumeFail);
    wd_put(fd, "battery %d %% (%d mV, %d C, %s%s), CPU clock %d MHz, bus %d MHz\n", scePowerGetBatteryLifePercent(), scePowerGetBatteryVolt(), scePowerGetBatteryTemp(),
           scePowerIsPowerOnline() ? "plugged in" : "on battery", scePowerIsLowBattery() ? ", LOW" : "", scePowerGetCpuClockFrequency(), scePowerGetBusClockFrequency());
    sceIoClose(fd);
}

/* If the game recovers after a warning it is noted at the end (this way a freeze can be told apart from a long pause). */
static void wd_note_recovery(int secs) {
    char line[96];
    int n = snprintf(line, sizeof(line), "\nThe game recovered after %d s (frame %d).\n", secs, gFrame);
    SceUID fd = sceIoOpen(sPath, PSP_O_WRONLY | PSP_O_APPEND, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, line, n);
        sceIoClose(fd);
    }
}

static int wd_thread(SceSize args, void *argp) {
    int last = -1, still = 0, reported = 0;
    unsigned int lastT = sceKernelGetSystemTimeLow();
    (void) args;
    (void) argp;
    for (;;) {
        unsigned int now, dt;
        int f, limit;
        sceKernelDelayThread(1000000);
        now = sceKernelGetSystemTimeLow();
        dt = now - lastT;
        lastT = now;
        f = gFrame;
        if (dt > 3000000u || gPspSuspending) {
            /* the console slept (or the thread did not run for a long time): does not count as a freeze */
            last = f;
            still = 0;
            reported = 0;
            continue;
        }
        wd_sample();
        if (f != last) {
            if (reported) {
                wd_note_recovery(still);
            }
            last = f;
            still = 0;
            reported = 0;
            continue;
        }
        still++;
        limit = (f == 0) ? WD_BOOT_STILL : WD_FRAMES_STILL;
        if (still == limit || (still > limit && still <= limit + 8 && ((still - limit) % 4) == 0)) {
            wd_write(still, still == limit);
            reported = 1;
        }
    }
    return 0;
}

void psp_watchdog_start(void) {
    SceUID th;
    fs_resolve_path(sPath, sizeof(sPath), "freeze.txt");
    psp_watchdog_register("main", sceKernelGetThreadId());
    /* above the audio (0x1E / 0x20), the presentation (0x1D) and the main thread: it has to run no matter what they do */
    th = sceKernelCreateThread("sm64_watchdog", wd_thread, 0x11, 0x2000, PSP_THREAD_ATTR_USER, NULL);
    if (th >= 0 && sceKernelStartThread(th, 0, NULL) < 0) {
        sceKernelDeleteThread(th);
    }
}
#endif
