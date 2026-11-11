#if defined(TARGET_PSP)

#include "psp_me.h"

#include <me-core-mapper/me-core.h>
#include <me-core-mapper/kernel/kcall.h>
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <psputils.h>
#include <stdint.h>

#include "audio/audio_psp.h"
#include "gfx/gfx_screen_config.h"

#define PSP_ME_READY_TIMEOUT_US 250000
#define PSP_ME_POLL_US 100
#define PSP_ME_JOB_TIMEOUT_US 100000   /* maximum blocking wait for a job; after that the ME is considered "busy" and it carries on without waiting */
#define PSP_ME_STUCK_DEAD_US 3000000   /* if it does not finish within 3 s, it is considered dead and the audio moves to the CPU for good */
#define PSP_ME_PROGRESS_ENTERED 0x10
#define PSP_ME_PROGRESS_BOOT_RELEASED 0x20
#define PSP_ME_PROGRESS_READY 0x100

enum PspMeState {
    PSP_ME_STATE_STOPPED,
    PSP_ME_STATE_BOOTING,
    PSP_ME_STATE_IDLE,
    PSP_ME_STATE_GENERATE_AUDIO,
    PSP_ME_STATE_STOP,
    PSP_ME_STATE_HALTED,
    PSP_ME_STATE_FAULT,
};

enum PspMeSharedIndex {
    PSP_ME_SHARED_STATE,
    PSP_ME_SHARED_PROGRESS,
    PSP_ME_SHARED_AUDIO_OUTPUT,
    PSP_ME_SHARED_JOB_COUNT,
    PSP_ME_SHARED_JOB_FRAMES, /* PSP_ME_JOB_MAX entries */
    PSP_ME_SHARED_COUNT = PSP_ME_SHARED_JOB_FRAMES + PSP_ME_JOB_MAX,
};

static volatile uint32_t sPspMeSharedStorage[PSP_ME_SHARED_COUNT]
    __attribute__((aligned(64), section(".uncached")));

#define sPspMeShared                                                                  \
    ((volatile uint32_t *)(UNCACHED_USER_MASK | (uintptr_t)sPspMeSharedStorage))
#define sPspMeState sPspMeShared[PSP_ME_SHARED_STATE]
#define sPspMeProgress sPspMeShared[PSP_ME_SHARED_PROGRESS]
#define sPspMeAudioOutput sPspMeShared[PSP_ME_SHARED_AUDIO_OUTPUT]
#define sPspMeJobCount sPspMeShared[PSP_ME_SHARED_JOB_COUNT]
#define sPspMeJobFrames(i_) sPspMeShared[PSP_ME_SHARED_JOB_FRAMES + (i_)]

unsigned int gPspMePolls, gPspMeUs, gPspMeBlocks, gPspMeWaits; /* log diagnostic: polls, time the main thread waited for the ME, audio blocks sent to the ME and times it had to wait */
static bool sPspMeBootStarted;
static int sPspMeBootResult = -1;
static bool sPspMeInitialized;
static volatile bool sPspMeSuspended; /* the console is suspending (or already suspended): no more jobs are sent to the ME */
static volatile bool sPspMeResumeArmed; /* the ME was left halted (HALTED) before sleeping: on wake-up it can be hardware-reset and started again */
static volatile bool sPspMeResumed;      /* the ME is ready again after resuming: audio can use it again (picked up by psp_me_take_resumed) */
unsigned int gPspMeResumeOk, gPspMeResumeFail; /* diagnostic: ME resumes that went well / badly */
static volatile unsigned int sPspMeWakeCalls; /* diagnostic: times libme-core called meLibOnWake */
static unsigned int sPspMePwrFlags[6]; /* diagnostic: flags of the last 6 power notices (the most recent in [0]) */
static unsigned int sPspMePwrN0, sPspMePwrN1; /* diagnostic: calls to psp_me_power_event(0) / (1) */
static int sPspMeSuspState = -1;               /* diagnostic: ME state when it finished stopping before sleep (5 = HALTED) */
static int sPspMeResumeState = -1, sPspMeResumeProg = -1; /* diagnostic: ME state/progress when the resume gave up */
static bool sPspMeStuck;           /* a job exceeded PSP_ME_JOB_TIMEOUT_US and is being waited for without blocking */
static uint32_t sPspMeStuckT0;
unsigned int gPspMeTimeouts, gPspMeRecovered; /* log diagnostic: jobs that passed the timeout / finished late and recovered */
static int sPspMeBootTries; /* diagnostic: bit0 = first attempt, bit1 = retry from the savedata folder */

__attribute__((noinline, aligned(4))) void meLibOnException(void) {
    sPspMeState = PSP_ME_STATE_FAULT;
    meLibSync();
    meLibHalt();
}

__attribute__((noinline, aligned(4))) void meLibOnExternalInterrupt(void) {
    sPspMeState = PSP_ME_STATE_FAULT;
    meLibSync();
    meLibHalt();
}

/* libme-core suspend hooks: its system event handler calls them in kernel mode during sleep/wake. Here the hardware is NOT
 * touched (the ME reset is done later, with the console awake, in psp_me_power_event(0)): the call is only counted. */
__attribute__((noinline, aligned(4))) void meLibOnSleep(void) {
}

__attribute__((noinline, aligned(4))) void meLibOnWake(void) {
    sPspMeWakeCalls++;
}

/* Runs in kernel mode (via kcall): copies the ME boot code to its memory again and hardware-resets it (meLibReset),
 * the same thing meLibDefaultInit did the first time. */
static int psp_me_kreset(void) {
    meLibReset();
    return 0;
}

__attribute__((noinline, aligned(4))) void meLibOnProcess(void) {
    sPspMeProgress = PSP_ME_PROGRESS_ENTERED;
    meLibSync();

    do {
        meLibDelayPipeline();
    } while (sPspMeState == PSP_ME_STATE_BOOTING);

    sPspMeProgress = PSP_ME_PROGRESS_BOOT_RELEASED;
    meLibSync();
    sPspMeProgress = PSP_ME_PROGRESS_READY;
    meLibSync();

    while (sPspMeState != PSP_ME_STATE_STOP) {
        if (sPspMeState == PSP_ME_STATE_GENERATE_AUDIO) {
            int16_t *output = (int16_t *)(uintptr_t)sPspMeAudioOutput;
            uint32_t count = sPspMeJobCount;
            uint32_t i;

            /* The SM64 audio engine still builds and executes its ABI work as
             * one unit. Keep its mutable globals coherent until the mixer is
             * split into the command-list model used by the OOT port.
             * A "job" is 1..PSP_ME_JOB_MAX consecutive blocks: the main thread does not touch the audio state while the ME
             * works (see audio_psp_tick_begin / audio_psp_tick_end), so the caches of the two CPUs do not overwrite each other. */
            meCoreDcacheWritebackInvalidateAll();
            if (count > PSP_ME_JOB_MAX) {
                count = PSP_ME_JOB_MAX;
            }
            for (i = 0; i < count; i++) {
                const uint32_t frames = sPspMeJobFrames(i);
                audio_psp_generate_cpu(output, frames);
                output += frames * 2;
            }
            meCoreDcacheWritebackInvalidateAll();
            meLibSync();
            sPspMeState = PSP_ME_STATE_IDLE;
            meLibSync();
        } else {
            meLibDelayPipeline();
        }
    }

    sPspMeState = PSP_ME_STATE_HALTED;
    meLibSync();
    meLibHalt();
}

int psp_me_boot(void) {
    if (sPspMeBootStarted) {
        return sPspMeBootResult;
    }

    sPspMeState = PSP_ME_STATE_BOOTING;
    sPspMeProgress = 0;
    sPspMeAudioOutput = 0;
    sPspMeJobCount = 0;
    meLibSync();

    sPspMeBootStarted = true;
    /* libme-core writes "kcall.prx" in the CURRENT DIRECTORY of the PSP file system and loads it from there (-3 = it could not
     * write it). Depending on how the game is launched (.prx loader, read-only folder...) that directory may not be
     * writable; then the ME does not start and all the audio is synthesized on the main CPU (~20 % of CPU). First it is tried
     * as usual; if it fails with -3 it is repeated from the saved games folder, which is always writable. (sceIoChdir only
     * changes the kernel's directory; the game's libc relative paths do not change.) */
    sPspMeBootTries = 1;
    sPspMeBootResult = meLibDefaultInit();
    if (sPspMeBootResult == -3) {
        sceIoMkdir("ms0:/PSP", 0777);
        sceIoMkdir("ms0:/PSP/SAVEDATA", 0777);
        sceIoMkdir("ms0:/PSP/SAVEDATA/SM64PORT", 0777);
        if (sceIoChdir("ms0:/PSP/SAVEDATA/SM64PORT") >= 0) {
            sPspMeBootTries |= 2;
            sPspMeBootResult = meLibDefaultInit();
        }
    }
    return sPspMeBootResult;
}

bool psp_me_init(void) {
    uint32_t start;

    if (sPspMeInitialized) {
        return true;
    }
    if (psp_me_boot() < 0) {
        return false;
    }

    meLibSync();
    sPspMeState = PSP_ME_STATE_IDLE;
    meLibSync();

    start = sceKernelGetSystemTimeLow();
    while (sPspMeProgress != PSP_ME_PROGRESS_READY) {
        if ((sceKernelGetSystemTimeLow() - start) >= PSP_ME_READY_TIMEOUT_US) {
            sPspMeState = PSP_ME_STATE_STOP;
            meLibSync();
            return false;
        }
        sceKernelDelayThread(PSP_ME_POLL_US);
    }

    sPspMeInitialized = true;
    return true;
}

/* diagnostic for the log: 0 = meLibDefaultInit result (<0: failure), 1 = initialized, 2 = ME progress, 3 = state */
int psp_me_debug(int which) {
    switch (which) {
        case 0: return sPspMeBootStarted ? sPspMeBootResult : -999;
        case 1: return sPspMeInitialized ? 1 : 0;
        case 2: return (int) sPspMeProgress;
        case 4: return sPspMeBootTries;
        case 5: return sPspMeStuck ? 1 : 0;
        case 6: return (int) gPspMeResumeOk;
        case 7: return (int) gPspMeResumeFail;
        case 8: return (int) sPspMeWakeCalls;
        case 12: return (int) sPspMePwrN0;
        case 13: return (int) sPspMePwrN1;
        case 14: case 15: case 16: case 17: case 18: case 19: return (int) sPspMePwrFlags[which - 14];
        case 9: return sPspMeSuspState;
        case 10: return sPspMeResumeState;
        case 11: return sPspMeResumeProg;
        default: return (int) sPspMeState;
    }
}

bool psp_me_is_active(void) {
    return sPspMeInitialized;
}

/* Sends a job to the ME: "count" consecutive audio blocks (frames[i] samples each) written one after another in samples.
 * It does not wait: the main thread collects the result with psp_me_job_wait(). Returns false if the ME is not ready. */
bool psp_me_job_start(int16_t *samples, const uint32_t *frames, int count) {
    int i;

    if (sPspMeSuspended || !sPspMeInitialized || (sPspMeState != PSP_ME_STATE_IDLE) || (samples == NULL) || (frames == NULL) || (count <= 0)
        || (count > PSP_ME_JOB_MAX)) {
        return false;
    }
    for (i = 0; i < count; i++) {
        sPspMeJobFrames(i) = frames[i];
    }
    sPspMeJobCount = (uint32_t) count;
    sPspMeAudioOutput = (uint32_t) (uintptr_t) samples;
    gPspMeBlocks += (unsigned int) count;

    /* Publish every Allegrex-owned audio input before releasing the ME. */
    sceKernelDcacheWritebackInvalidateAll();
    meLibSync();
    sPspMeState = PSP_ME_STATE_GENERATE_AUDIO;
    meLibSync();
    return true;
}

/* Waits for the ME to finish the job and brings the main thread's cache up to date (discards old lines of the audio state).
 * Normally the ME has already finished (it works while the CPU draws) and nothing is waited for.
 * Before, a SINGLE job that took longer than 1 s (seen when entering a level) froze the game for 1 s and disabled the ME for the whole
 * session (all the audio moved to the main CPU: ~5 ms per frame). Now: after PSP_ME_JOB_TIMEOUT_US BUSY is returned and the following
 * ticks only look (without blocking); if the ME finishes late that job is discarded and the ME keeps being used. While it is busy
 * the CPU does NOT synthesize (it would touch the same state as the ME); only if PSP_ME_STUCK_DEAD_US passes is it considered dead. */
int psp_me_job_wait(void) {
    uint32_t polls = 0;
    uint32_t t0;

    if (!sPspMeInitialized) {
        return PSP_ME_WAIT_DEAD;
    }
    t0 = sceKernelGetSystemTimeLow();
    if (sPspMeStuck) {
        if (sPspMeState == PSP_ME_STATE_IDLE) {
            sPspMeStuck = false;
            gPspMeRecovered++;
            sceKernelDcacheWritebackInvalidateAll();
            return PSP_ME_WAIT_STALE;
        }
        if ((sPspMeState != PSP_ME_STATE_GENERATE_AUDIO) || ((t0 - sPspMeStuckT0) >= PSP_ME_STUCK_DEAD_US)) {
            sPspMeInitialized = false;
            return PSP_ME_WAIT_DEAD;
        }
        return PSP_ME_WAIT_BUSY;
    }
    while (sPspMeState == PSP_ME_STATE_GENERATE_AUDIO) {
        if ((sceKernelGetSystemTimeLow() - t0) >= PSP_ME_JOB_TIMEOUT_US) {
            break;
        }
        meLibSync();
        polls++;
        sceKernelDelayThread(PSP_ME_POLL_US);
    }
    if (polls) {
        gPspMePolls += polls;
        gPspMeUs += sceKernelGetSystemTimeLow() - t0;
        gPspMeWaits++;
    }
    if (sPspMeState == PSP_ME_STATE_GENERATE_AUDIO) {
        sPspMeStuck = true;
        sPspMeStuckT0 = t0;
        gPspMeTimeouts++;
        return PSP_ME_WAIT_BUSY;
    }
    if (sPspMeState != PSP_ME_STATE_IDLE) {
        sPspMeInitialized = false; /* FAULT / HALTED: the ME stopped, there is no way back */
        return PSP_ME_WAIT_DEAD;
    }

    /* Discard stale Allegrex cache lines after the ME published audio state
     * and PCM. */
    sceKernelDcacheWritebackInvalidateAll();
    return PSP_ME_WAIT_DONE;
}


/* Console suspend. The ME runs OUR code in an endless loop (polling the shared state); if the console goes to sleep with the ME like that,
 * the system cannot turn it back on at resume (the ME has to be stopped). That is why, on suspend: no more jobs are sent, it
 * waits for the one in progress to finish (the ME only looks at the stop order between jobs: if it were requested with a job running, when it finished
 * it would overwrite it with IDLE and the order would be lost), it is ordered to stop and it waits for it to be halted. Meanwhile the audio goes to the CPU.
 * On resume (RESUME_COMPLETE) the meLibOnWake hook has already hardware-reset it: here it is given the start order again, it waits for it to be
 * ready and the audio is notified (psp_me_take_resumed). If anything fails or takes too long, the audio simply stays on the CPU. */
void psp_me_note_flags(int flags) {
    int i;
    for (i = 5; i > 0; i--) {
        sPspMePwrFlags[i] = sPspMePwrFlags[i - 1];
    }
    sPspMePwrFlags[0] = (unsigned int) flags;
}

void psp_me_power_event(int suspending) {
    uint32_t t0;
    uint32_t st;

    if (suspending) {
        sPspMePwrN1++;
    } else {
        sPspMePwrN0++;
    }

    if (!suspending) {
        if (!sPspMeResumeArmed) {
            return;
        }
        /* the ME has been off since the console went to sleep: the boot reset is done (in kernel mode) and it is released with IDLE
         * (meLibOnProcess waits for BOOTING); if it is not ready within 600 ms the reset is repeated once */
        int attempt;
        sPspMeResumeArmed = false;
        for (attempt = 0; attempt < 2; attempt++) {
            sPspMeProgress = 0;
            sPspMeState = PSP_ME_STATE_BOOTING;
            meLibSync();
            kcall(psp_me_kreset, 0);
            sPspMeState = PSP_ME_STATE_IDLE;
            meLibSync();
            t0 = sceKernelGetSystemTimeLow();
            while ((sPspMeProgress != PSP_ME_PROGRESS_READY) && ((sceKernelGetSystemTimeLow() - t0) < 600000)) {
                sceKernelDelayThread(500);
            }
            if (sPspMeProgress == PSP_ME_PROGRESS_READY) {
                break;
            }
        }
        if (sPspMeProgress == PSP_ME_PROGRESS_READY) {
            sPspMeStuck = false;
            sceKernelDcacheWritebackInvalidateAll();
            sPspMeInitialized = true;
            sPspMeSuspended = false;
            sPspMeResumed = true;
            gPspMeResumeOk++;
        } else {
            sPspMeResumeState = (int) sPspMeState;
            sPspMeResumeProg = (int) sPspMeProgress;
            sPspMeState = PSP_ME_STATE_STOP; /* in case it ever starts later, it should stop */
            meLibSync();
            sPspMeSuspended = false;
            gPspMeResumeFail++;
        }
        return;
    }
    if (sPspMeSuspended) {
        return; /* already suspended (repeated notice): the permission to resume is not lost */
    }
    sPspMeSuspended = true;
    sPspMeResumeArmed = false;
    if (!sPspMeBootStarted || (sPspMeBootResult < 0) || !sPspMeInitialized) {
        return;
    }
    sceKernelDelayThread(3000); /* a psp_me_job_start that already passed the check finishes publishing its job */
    t0 = sceKernelGetSystemTimeLow();
    for (;;) {
        st = sPspMeState;
        if (st == PSP_ME_STATE_IDLE) {
            sPspMeState = PSP_ME_STATE_STOP;
            meLibSync();
            break;
        }
        if (st != PSP_ME_STATE_GENERATE_AUDIO) {
            break; /* already halted / failure / stopping */
        }
        if ((sceKernelGetSystemTimeLow() - t0) >= 400000) {
            break;
        }
        meLibSync();
        sceKernelDelayThread(500);
    }
    t0 = sceKernelGetSystemTimeLow();
    while ((sPspMeState != PSP_ME_STATE_HALTED) && ((sceKernelGetSystemTimeLow() - t0) < PSP_ME_READY_TIMEOUT_US)) {
        sceKernelDelayThread(PSP_ME_POLL_US);
    }
    sPspMeSuspState = (int) sPspMeState;
    if (sPspMeState == PSP_ME_STATE_HALTED) {
        /* really halted: the state is prepared for the next start and the reset on wake-up is allowed */
        sPspMeProgress = 0;
        sPspMeState = PSP_ME_STATE_BOOTING;
        meLibSync();
        sPspMeResumeArmed = true;
    }
    sceKernelDcacheWritebackInvalidateAll(); /* the last thing the ME wrote (audio state) must be read from memory */
    sPspMeInitialized = false;
}

/* true only once when the ME is ready again after resuming (called by the audio producer thread between two syntheses on the CPU) */
bool psp_me_take_resumed(void) {
    if (!sPspMeResumed) {
        return false;
    }
    sPspMeResumed = false;
    return true;
}

void psp_me_shutdown(void) {
    uint32_t start;

    if (!sPspMeBootStarted || (sPspMeBootResult < 0)) {
        return;
    }
    if (sPspMeState == PSP_ME_STATE_HALTED) {
        sPspMeInitialized = false;
        return;
    }

    sPspMeState = PSP_ME_STATE_STOP;
    meLibSync();
    start = sceKernelGetSystemTimeLow();
    while ((sPspMeState != PSP_ME_STATE_HALTED) &&
           ((sceKernelGetSystemTimeLow() - start) < PSP_ME_READY_TIMEOUT_US)) {
        sceKernelDelayThread(PSP_ME_POLL_US);
    }
    sPspMeInitialized = false;
}

#endif
