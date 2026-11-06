#if defined(TARGET_PSP)

#include "audio_psp.h"

#include <pspaudio.h>
#include <pspkernel.h>
#include <pspthreadman.h>
#include <psputils.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "audio_api.h"
#include "../psp_me.h"
#include "../psp_watchdog.h"

#define PSP_AUDIO_CHANNELS 2
#define PSP_AUDIO_FREQUENCY 32000
#define PSP_AUDIO_OUTPUT_FRAMES 512
#define PSP_AUDIO_RING_FRAMES 8192
#define PSP_AUDIO_RING_MASK (PSP_AUDIO_RING_FRAMES - 1)
#define PSP_AUDIO_STARTUP_FRAMES 2048
#define PSP_AUDIO_TARGET_FRAMES 3072
#define PSP_AUDIO_OUTPUT_THREAD_PRIORITY 0x1E
#define PSP_AUDIO_PRODUCER_THREAD_PRIORITY 0x20
/* With the ME, the main thread sends the audio (see audio_psp_tick_begin/end): ring level to aim for right after
 * collecting a job, and estimate of what is consumed until the next tick (30 Hz = ~1067 samples). */
#define PSP_AUDIO_ME_TARGET_FRAMES 2560
#define PSP_AUDIO_ME_DRAIN_FRAMES 1100 /* assumed minimum consumption between two ticks (30 Hz); if ticks take longer the measured value is used */
#define PSP_AUDIO_ME_DRAIN_MAX_US 250000 /* cap of the tick interval estimate (a 4 s load must not ask for 4 s of audio) */

#ifdef VERSION_EU
#define PSP_AUDIO_SAMPLES_HIGH 656
#define PSP_AUDIO_SAMPLES_LOW 640
#else
#define PSP_AUDIO_SAMPLES_HIGH 544
#define PSP_AUDIO_SAMPLES_LOW 528
#endif

#if (PSP_AUDIO_RING_FRAMES & (PSP_AUDIO_RING_FRAMES - 1)) != 0
#error PSP_AUDIO_RING_FRAMES must be a power of two
#endif

extern void create_next_audio_buffer(int16_t *samples, uint32_t num_samples);
extern void memcpy_vfpu(void *dst, const void *src, size_t size);
extern int gProcessAudio;

static int16_t sAudioRing[PSP_AUDIO_RING_FRAMES * PSP_AUDIO_CHANNELS]
    __attribute__((aligned(64)));
static int16_t sAudioGenerate[PSP_ME_JOB_MAX * PSP_AUDIO_SAMPLES_HIGH * PSP_AUDIO_CHANNELS]
    __attribute__((aligned(64)));
static int16_t sAudioOutput[2][PSP_AUDIO_OUTPUT_FRAMES * PSP_AUDIO_CHANNELS]
    __attribute__((aligned(64)));

static volatile uint32_t sAudioReadPos;
static volatile uint32_t sAudioWritePos;
static volatile bool sAudioRunning;
static volatile bool sAudioStarted;
static volatile uint32_t sAudioPlayedFrames; /* music frames actually played (music clock) */
static SceUID sAudioOutputThreadId = -1;
static SceUID sAudioProducerThreadId = -1;
static bool sAudioChannelReady;
static volatile bool sAudioMeDriven; /* true: the ME synthesizes and the main thread coordinates it; false: the producer thread does it on the CPU */
static int sMeJobBlocks;             /* blocks of the job in progress on the ME (0 = none) */
static unsigned int sTickBeginT;     /* time (us) of the last audio_psp_tick_begin */
static unsigned int sTickIntervalEst; /* estimate (us) of how long the next tick will take: goes up at once, comes down slowly */
static uint32_t sMeJobFrames[PSP_ME_JOB_MAX];
unsigned int gPspNoteMax; /* highest number of synthesizer notes in use seen at the start of a tick (reset to 0 when printed) */
extern int psp_audio_active_notes(void);
unsigned int gPspAudUnder;           /* times the ring emptied and the output had to play silence (audio dropouts) */

static inline void audio_psp_sync(void) {
    __asm__ volatile("sync" ::: "memory");
}

static uint32_t audio_psp_buffered_frames(void) {
    return (sAudioWritePos - sAudioReadPos) & PSP_AUDIO_RING_MASK;
}

static uint32_t audio_psp_free_frames(void) {
    return (PSP_AUDIO_RING_FRAMES - 1) - audio_psp_buffered_frames();
}

static void audio_psp_queue_frames(const int16_t *samples, uint32_t frames) {
    uint32_t write_pos = sAudioWritePos;
    uint32_t copied = 0;

    if (frames > audio_psp_free_frames()) {
        return;
    }

    while (copied < frames) {
        uint32_t todo = frames - copied;
        uint32_t until_wrap = PSP_AUDIO_RING_FRAMES - write_pos;
        size_t bytes;

        if (todo > until_wrap) {
            todo = until_wrap;
        }
        bytes = todo * PSP_AUDIO_CHANNELS * sizeof(int16_t);
        memcpy_vfpu(&sAudioRing[write_pos * PSP_AUDIO_CHANNELS],
                    samples + (copied * PSP_AUDIO_CHANNELS), bytes);
        copied += todo;
        write_pos = (write_pos + todo) & PSP_AUDIO_RING_MASK;
    }

    audio_psp_sync();
    sAudioWritePos = write_pos;
}

static void audio_psp_dequeue_output(int16_t *output) {
    uint32_t read_pos = sAudioReadPos;
    uint32_t copied = 0;

    while (copied < PSP_AUDIO_OUTPUT_FRAMES) {
        uint32_t todo = PSP_AUDIO_OUTPUT_FRAMES - copied;
        uint32_t until_wrap = PSP_AUDIO_RING_FRAMES - read_pos;
        size_t bytes;

        if (todo > until_wrap) {
            todo = until_wrap;
        }
        bytes = todo * PSP_AUDIO_CHANNELS * sizeof(int16_t);
        memcpy_vfpu(output + (copied * PSP_AUDIO_CHANNELS),
                    &sAudioRing[read_pos * PSP_AUDIO_CHANNELS], bytes);
        copied += todo;
        read_pos = (read_pos + todo) & PSP_AUDIO_RING_MASK;
    }

    audio_psp_sync();
    sAudioReadPos = read_pos;
    sAudioPlayedFrames += PSP_AUDIO_OUTPUT_FRAMES;
}

/* Music clock: frames (at 32 kHz) already played, and position where what is generated now will sound. */
uint32_t audio_psp_played_frames(void) {
    return sAudioPlayedFrames;
}

uint32_t audio_psp_play_position_frames(void) {
    return sAudioPlayedFrames + audio_psp_buffered_frames();
}

void audio_psp_generate_cpu(int16_t *samples, uint32_t frames) {
    create_next_audio_buffer(samples, frames);
}

static int audio_psp_output_thread(SceSize args, void *argp) {
    unsigned int output_index = 0;

    (void)args;
    (void)argp;

    while (sAudioRunning) {
        int16_t *output = sAudioOutput[output_index];

        if (!sAudioStarted && (audio_psp_buffered_frames() >= PSP_AUDIO_STARTUP_FRAMES)) {
            sAudioStarted = true;
        }

        if (sAudioStarted && (audio_psp_buffered_frames() >= PSP_AUDIO_OUTPUT_FRAMES)) {
            audio_psp_dequeue_output(output);
        } else {
            if (sAudioStarted) {
                sAudioStarted = false;
                gPspAudUnder++;
            }
            memset(output, 0, sizeof(sAudioOutput[0]));
        }

        sceKernelDcacheWritebackRange(output, sizeof(sAudioOutput[0]));
        if (sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX, output) < 0) {
            sceKernelDelayThread(PSP_AUDIO_OUTPUT_FRAMES * 1000000U / PSP_AUDIO_FREQUENCY);
        }
        output_index ^= 1;
    }

    sAudioOutputThreadId = -1;
    sceKernelExitDeleteThread(0);
    return 0;
}

unsigned int gPspAudCpuUs, gPspAudCpuN; /* audio synthesis done on the main CPU (fallback without ME): us and calls */

/* Block size pattern: the N64 makes 2 calls per game frame (60 per second), that is 32000/60 = 533.33 samples
 * de promedio: patron 528, 528, 544. */
static uint32_t audio_psp_next_block_frames(void) {
#ifdef VERSION_EU
    return PSP_AUDIO_SAMPLES_LOW; /* 640 = 32000/50 */
#else
    static const uint32_t sTickFrames[3] = { 528, 528, 544 };
    static uint32_t sTickIndex = 0;
    const uint32_t frames = sTickFrames[sTickIndex];
    sTickIndex = (sTickIndex + 1) % 3;
    return frames;
#endif
}

/* Start of each game tick (BEFORE the logic touches the audio): collects the ME's job. With the ME, synthesis runs
 * on the other core while the main CPU draws; between audio_psp_tick_end and the next call to this function the game does not
 * touch the audio state, so the two caches do not overwrite each other. Before, the ME synthesized IN PARALLEL with the logic: the two CPUs
 * wrote the same cache lines (sound request counters, pause flags...) and writes were lost
 * (sounds that do not play, music that does not stop on pause). */
void audio_psp_tick_begin(void) {
    {
        /* Measures how long the tick lasts (real time between two calls). In heavy areas the tick goes from 33 ms to 60-100 ms (slow motion):
         * the output consumes 32 samples per ms no matter what, and with a split designed for 33 ms the ring emptied (dropouts). */
        const unsigned int now = sceKernelGetSystemTimeLow();
        const unsigned int dt = sTickBeginT ? (now - sTickBeginT) : 0;
        sTickBeginT = now;
        if (dt > PSP_AUDIO_ME_DRAIN_MAX_US) {
            /* pause / load: not learned from */
        } else if (dt > sTickIntervalEst) {
            sTickIntervalEst = dt;
        } else {
            sTickIntervalEst = (sTickIntervalEst * 7U + dt) / 8U;
        }
    }
    if (!sAudioMeDriven || (sMeJobBlocks <= 0)) {
        return;
    }
    {
        const int r = psp_me_job_wait();
        if (r == PSP_ME_WAIT_BUSY) {
            return; /* slow ME: the audio is not touched and no other job is sent; it is looked at again on the next tick */
        }
        if (r == PSP_ME_WAIT_STALE) {
            sMeJobBlocks = 0; /* finished late: that audio is no longer useful, but the ME is free again */
            return;
        }
        if (r != PSP_ME_WAIT_DONE) {
            /* the ME failed: the producer thread takes synthesis back on the CPU */
            sMeJobBlocks = 0;
            sAudioMeDriven = false;
            return;
        }
    }
    {
        const int16_t *src = sAudioGenerate;
        int i;
        for (i = 0; i < sMeJobBlocks; i++) {
            audio_psp_queue_frames(src, sMeJobFrames[i]);
            src += sMeJobFrames[i] * PSP_AUDIO_CHANNELS;
        }
    }
    sMeJobBlocks = 0;
    {
        /* the ME is stopped: the notes in use can be counted (diagnostic) */
        const unsigned int n = (unsigned int)psp_audio_active_notes();
        if (n > gPspNoteMax) {
            gPspNoteMax = n;
        }
    }
}

/* End of the tick's logic: computes how many blocks are needed to keep the ring up and sends them to the ME (does not wait). */
void audio_psp_tick_end(void) {
    int need, blocks, i;

    if (!sAudioMeDriven || (sMeJobBlocks > 0) || !gProcessAudio || !sAudioRunning) {
        return;
    }
    {
        /* samples that will play out while the job arrives (until the next tick): the measured value, with a minimum of 1100 (30 Hz tick) */
        int drain = (int)((sTickIntervalEst * 32U) / 1000U);
        drain += drain / 16; /* ~6 %: the audio clock runs somewhat ahead of the CPU's */
        if (drain < (int)PSP_AUDIO_ME_DRAIN_FRAMES) {
            drain = (int)PSP_AUDIO_ME_DRAIN_FRAMES;
        }
        need = (int)PSP_AUDIO_ME_TARGET_FRAMES + drain - (int)audio_psp_buffered_frames();
    }
    if (need <= 0) {
        return;
    }
    blocks = (need + 531) / 532;
    if (blocks > PSP_ME_JOB_MAX) {
        blocks = PSP_ME_JOB_MAX;
    }
    for (i = 0; i < blocks; i++) {
        sMeJobFrames[i] = audio_psp_next_block_frames();
    }
    if (psp_me_job_start(sAudioGenerate, sMeJobFrames, blocks)) {
        sMeJobBlocks = blocks;
    } else {
        sAudioMeDriven = false; /* the ME is not ready: synthesis on the CPU */
    }
}


static int audio_psp_producer_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;

    while (sAudioRunning) {
        uint32_t buffered = audio_psp_buffered_frames();

        /* after the console resumes the ME is ready again: the audio is given back to it here, between two syntheses of this thread, so that the CPU and the ME never
         * work at the same time on the audio state (the next job is sent by the main thread in audio_psp_tick_end) */
        if (!sAudioMeDriven && sMeJobBlocks <= 0 && psp_me_take_resumed()) {
            sAudioMeDriven = true;
        }
        if (sAudioMeDriven) {
            sceKernelDelayThread(4000); /* with the ME the audio is sent by the main thread; this thread only watches */
        } else if (gProcessAudio && (buffered < PSP_AUDIO_TARGET_FRAMES) &&
            (audio_psp_free_frames() >= PSP_AUDIO_SAMPLES_HIGH)) {
            const uint32_t frames = audio_psp_next_block_frames();
            /* without ME: synthesis runs on the main CPU and steals time from the game (counted for the log) */
            const unsigned int t_cpu0 = sceKernelGetSystemTimeLow();
            audio_psp_generate_cpu(sAudioGenerate, frames);
            gPspAudCpuUs += sceKernelGetSystemTimeLow() - t_cpu0;
            gPspAudCpuN++;
            audio_psp_queue_frames(sAudioGenerate, frames);
            sceKernelRotateThreadReadyQueue(sceKernelGetThreadCurrentPriority());
        } else {
            sceKernelDelayThread(500);
        }
    }

    sAudioProducerThreadId = -1;
    sceKernelExitDeleteThread(0);
    return 0;
}

static bool audio_psp_init(void) {
    if (sAudioChannelReady) {
        return true;
    }

    sceAudioOutput2Release();
    sceAudioSRCChRelease();
    if (sceAudioSRCChReserve(PSP_AUDIO_OUTPUT_FRAMES, PSP_AUDIO_FREQUENCY,
                             PSP_AUDIO_CHANNELS) < 0) {
        return false;
    }

    memset(sAudioRing, 0, sizeof(sAudioRing));
    memset(sAudioGenerate, 0, sizeof(sAudioGenerate));
    memset(sAudioOutput, 0, sizeof(sAudioOutput));
    sAudioReadPos = 0;
    sAudioWritePos = 0;
    sAudioRunning = false;
    sAudioStarted = false;
    sAudioChannelReady = true;
    return true;
}

bool audio_psp_start(void) {
    SceUID thread_id;
    int result;

    if (!sAudioChannelReady) {
        return false;
    }
    if (sAudioRunning) {
        return true;
    }

    sMeJobBlocks = 0;
    sAudioMeDriven = psp_me_init();
    sAudioRunning = true;

    thread_id = sceKernelCreateThread("SM64 PSP AudioOut", audio_psp_output_thread,
                                     PSP_AUDIO_OUTPUT_THREAD_PRIORITY, 0x10000,
                                     PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU, NULL);
    if (thread_id < 0) {
        sAudioRunning = false;
        return false;
    }
    result = sceKernelStartThread(thread_id, 0, NULL);
    if (result < 0) {
        sceKernelDeleteThread(thread_id);
        sAudioRunning = false;
        return false;
    }
    sAudioOutputThreadId = thread_id;
    psp_watchdog_register("audio output", thread_id);

    thread_id = sceKernelCreateThread("SM64 PSP AudioGen", audio_psp_producer_thread,
                                     PSP_AUDIO_PRODUCER_THREAD_PRIORITY, 0x20000,
                                     PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU, NULL);
    if (thread_id < 0) {
        audio_psp_shutdown();
        return false;
    }
    result = sceKernelStartThread(thread_id, 0, NULL);
    if (result < 0) {
        sceKernelDeleteThread(thread_id);
        audio_psp_shutdown();
        return false;
    }
    sAudioProducerThreadId = thread_id;
    psp_watchdog_register("audio generator", thread_id);
    return true;
}

void audio_psp_shutdown(void) {
    SceUID output_thread = sAudioOutputThreadId;
    SceUID producer_thread = sAudioProducerThreadId;

    sAudioRunning = false;
    audio_psp_sync();

    /* Releasing SRC wakes a blocking output call so the thread can exit. */
    if (sAudioChannelReady) {
        sceAudioSRCChRelease();
        sAudioChannelReady = false;
    }
    if (producer_thread >= 0) {
        sceKernelWaitThreadEnd(producer_thread, NULL);
    }
    if (output_thread >= 0) {
        sceKernelWaitThreadEnd(output_thread, NULL);
    }

    sAudioOutputThreadId = -1;
    sAudioProducerThreadId = -1;
    psp_me_shutdown();
}

static int audio_psp_buffered(void) {
    return (int)audio_psp_buffered_frames();
}

static int audio_psp_get_desired_buffered(void) {
    return PSP_AUDIO_TARGET_FRAMES;
}

static void audio_psp_play(const uint8_t *buf, size_t len) {
    uint32_t frames = (uint32_t)(len / (sizeof(int16_t) * PSP_AUDIO_CHANNELS));

    if ((buf != NULL) && (frames != 0)) {
        audio_psp_queue_frames((const int16_t *)buf, frames);
    }
}

struct AudioAPI audio_psp = {
    audio_psp_init,
    audio_psp_buffered,
    audio_psp_get_desired_buffered,
    audio_psp_play,
};

#endif
