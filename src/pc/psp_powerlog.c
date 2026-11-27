/* Measurement of a console sleep (see psp_powerlog.h). Built only with PSP_POWERLOG=1. */
#include "psp_powerlog.h"

#if defined(TARGET_PSP) && PSP_POWERLOG

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <pspkernel.h>
#include <pspiofilemgr.h>

#define PLOG_FILE "ms0:/sm64_power.txt"
#define PLOG_BYTES 24576
#define PLOG_SAMPLE_US 100000U  /* audio sample period after a wake-up */
#define PLOG_SAMPLE_FOR_US 4000000U

extern volatile int gPspSuspending; /* gfx_scegu.c */

static char sBuf[PLOG_BYTES];
static volatile int sLen;
static volatile int sLost;
static volatile int sHold;          /* a sleep started: nothing is written until the wake-up has settled */
static volatile uint32_t sSampleUntil;
static volatile int sSampling;
static SceUID sThread = -1;

void psp_plog(const char *fmt, ...) {
    char line[160];
    int n, lock;
    const uint32_t t = sceKernelGetSystemTimeLow();
    va_list ap;

    n = snprintf(line, sizeof(line), "%7u.%03u ", (unsigned) (t / 1000000U), (unsigned) ((t / 1000U) % 1000U));
    va_start(ap, fmt);
    n += vsnprintf(line + n, sizeof(line) - (size_t) n - 1, fmt, ap);
    va_end(ap);
    if (n > (int) sizeof(line) - 2) {
        n = (int) sizeof(line) - 2;
    }
    line[n++] = '\n';
    lock = sceKernelCpuSuspendIntr();
    if (sLen + n <= PLOG_BYTES) {
        memcpy(sBuf + sLen, line, (size_t) n);
        sLen += n;
    } else {
        sLost++;
    }
    sceKernelCpuResumeIntr(lock);
}

void psp_plog_suspending(void) {
    sHold = 1;
    sSampling = 0;
}

void psp_plog_resumed(void) {
    sSampleUntil = sceKernelGetSystemTimeLow() + PLOG_SAMPLE_FOR_US;
    sSampling = 1;
}

static void plog_flush(void) {
    static char out[PLOG_BYTES];
    int n, lost, lock;
    SceUID fd;

    lock = sceKernelCpuSuspendIntr();
    n = sLen;
    lost = sLost;
    memcpy(out, sBuf, (size_t) n);
    sLen = 0;
    sLost = 0;
    sceKernelCpuResumeIntr(lock);
    if (n == 0 && lost == 0) {
        return;
    }
    fd = sceIoOpen(PLOG_FILE, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (fd < 0) {
        return;
    }
    if (n > 0) {
        sceIoWrite(fd, out, (SceSize) n);
    }
    if (lost) {
        char l[48];
        const int k = snprintf(l, sizeof(l), "(%d lines lost: buffer full)\n", lost);
        sceIoWrite(fd, l, (SceSize) k);
    }
    sceIoClose(fd);
}

static int plog_thread(SceSize args, void *argp) {
    (void) args;
    (void) argp;
    for (;;) {
        sceKernelDelayThread(PLOG_SAMPLE_US);
        if (sSampling) {
            char st[128];
            if ((int32_t) (sceKernelGetSystemTimeLow() - sSampleUntil) >= 0) {
                sSampling = 0;
                sHold = 0; /* the wake-up has settled: the file can be written */
                psp_plog("sampling done");
            } else {
                audio_psp_plog_state(st, (int) sizeof(st));
                psp_plog("  %s", st);
            }
        }
        if (!sHold && !gPspSuspending && sLen > 0) {
            plog_flush();
        }
    }
    return 0;
}

void psp_plog_start(void) {
    if (sThread >= 0) {
        return;
    }
    {
        /* a separator, so the runs can be told apart in the file */
        SceUID fd = sceIoOpen(PLOG_FILE, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
        if (fd >= 0) {
            static const char sep[] = "\n===== boot =====\n";
            sceIoWrite(fd, sep, sizeof(sep) - 1);
            sceIoClose(fd);
        }
    }
    /* above the game thread (0x20) so the samples keep their pace, but it sleeps nearly all the time */
    sThread = sceKernelCreateThread("sm64_powerlog", plog_thread, 0x18, 0x2000, PSP_THREAD_ATTR_USER, NULL);
    if (sThread >= 0) {
        sceKernelStartThread(sThread, 0, NULL);
    }
}

#endif
