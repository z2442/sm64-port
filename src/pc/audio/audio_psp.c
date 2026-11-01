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

#define PSP_AUDIO_CHANNELS 2
#define PSP_AUDIO_FREQUENCY 32000
#define PSP_AUDIO_OUTPUT_FRAMES 512
#define PSP_AUDIO_RING_FRAMES 8192
#define PSP_AUDIO_RING_MASK (PSP_AUDIO_RING_FRAMES - 1)
#define PSP_AUDIO_STARTUP_FRAMES 2048
#define PSP_AUDIO_TARGET_FRAMES 3072
#define PSP_AUDIO_OUTPUT_THREAD_PRIORITY 0x1E
#define PSP_AUDIO_PRODUCER_THREAD_PRIORITY 0x20
/* Con el ME, el hilo principal manda el audio (ver audio_psp_tick_begin/end): nivel del anillo que se busca justo despues de
 * recoger un trabajo, y estimacion de lo que se consume hasta el siguiente tick (30 Hz = ~1067 muestras). */
#define PSP_AUDIO_ME_TARGET_FRAMES 2560
#define PSP_AUDIO_ME_DRAIN_FRAMES 1100 /* consumo minimo supuesto entre dos ticks (30 Hz); si los ticks tardan mas se usa lo medido */
#define PSP_AUDIO_ME_DRAIN_MAX_US 250000 /* tope de la estimacion del intervalo entre ticks (4 s de carga no deben pedir 4 s de audio) */

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
static volatile uint32_t sAudioPlayedFrames; /* frames de musica realmente reproducidos (reloj de la musica) */
static SceUID sAudioOutputThreadId = -1;
static SceUID sAudioProducerThreadId = -1;
static bool sAudioChannelReady;
static volatile bool sAudioMeDriven; /* true: el ME sintetiza y el hilo principal lo coordina; false: lo hace el hilo productor en la CPU */
static int sMeJobBlocks;             /* bloques del trabajo en curso en el ME (0 = ninguno) */
static unsigned int sTickBeginT;     /* hora (us) del ultimo audio_psp_tick_begin */
static unsigned int sTickIntervalEst; /* estimacion (us) de lo que tardara el tick siguiente: sube de golpe, baja despacio */
static uint32_t sMeJobFrames[PSP_ME_JOB_MAX];
unsigned int gPspNoteMax; /* mayor numero de notas del sintetizador en uso visto al empezar un tick (se pone a 0 al imprimir) */
extern int psp_audio_active_notes(void);
unsigned int gPspAudUnder;           /* veces que el anillo se vacio y la salida tuvo que sonar en silencio (cortes de audio) */

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

/* Reloj de la musica: frames (a 32 kHz) ya reproducidos, y posicion donde sonara lo que se genere ahora. */
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

unsigned int gPspAudCpuUs, gPspAudCpuN; /* sintesis de audio hecha en la CPU principal (fallback sin ME): us y llamadas */

/* Patron de tamanos de bloque: la N64 genera 2 llamadas por frame de juego (60 por segundo), es decir 32000/60 = 533.33 muestras
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

/* Inicio de cada tick del juego (ANTES de que la logica toque el audio): recoge el trabajo del ME. Con el ME, la sintesis corre
 * en el otro nucleo mientras la CPU principal dibuja; entre audio_psp_tick_end y la siguiente llamada a esta funcion el juego no
 * toca el estado del audio, asi las dos cachas no se pisan. Antes el ME sintetizaba EN PARALELO con la logica: las dos CPU
 * escribian las mismas lineas de cache (contadores de peticiones de sonido, banderas de pausa...) y se perdian escrituras
 * (sonidos que no suenan, musica que no se detiene al pausar). */
void audio_psp_tick_begin(void) {
    {
        /* Mide lo que dura el tick (tiempo real entre dos llamadas). En zonas pesadas el tick pasa de 33 ms a 60-100 ms (camara lenta):
         * la salida consume 32 muestras por ms pase lo que pase, y con un reparto pensado para 33 ms el anillo se vaciaba (cortes). */
        const unsigned int now = sceKernelGetSystemTimeLow();
        const unsigned int dt = sTickBeginT ? (now - sTickBeginT) : 0;
        sTickBeginT = now;
        if (dt > PSP_AUDIO_ME_DRAIN_MAX_US) {
            /* pausa / carga: no se aprende de ella */
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
            return; /* ME lento: no se toca el audio ni se manda otro trabajo; se vuelve a mirar en el tick siguiente */
        }
        if (r == PSP_ME_WAIT_STALE) {
            sMeJobBlocks = 0; /* termino tarde: ese audio ya no sirve, pero el ME vuelve a estar libre */
            return;
        }
        if (r != PSP_ME_WAIT_DONE) {
            /* el ME fallo: el hilo productor retoma la sintesis en la CPU */
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
        /* el ME esta parado: se pueden contar las notas en uso (diagnostico) */
        const unsigned int n = (unsigned int)psp_audio_active_notes();
        if (n > gPspNoteMax) {
            gPspNoteMax = n;
        }
    }
}

/* Fin de la logica del tick: calcula cuantos bloques hacen falta para mantener el anillo y se los manda al ME (no espera). */
void audio_psp_tick_end(void) {
    int need, blocks, i;

    if (!sAudioMeDriven || (sMeJobBlocks > 0) || !gProcessAudio || !sAudioRunning) {
        return;
    }
    {
        /* muestras que saldran mientras llega el trabajo (hasta el proximo tick): lo medido, con un minimo de 1100 (tick de 30 Hz) */
        int drain = (int)((sTickIntervalEst * 32U) / 1000U);
        drain += drain / 16; /* ~6 %: el reloj de audio va algo por delante del de la CPU */
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
        sAudioMeDriven = false; /* el ME no esta listo: sintesis en la CPU */
    }
}


static int audio_psp_producer_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;

    while (sAudioRunning) {
        uint32_t buffered = audio_psp_buffered_frames();

        /* tras reanudar la consola el ME vuelve a estar listo: se le devuelve el audio aqui, entre dos sintesis de este hilo, para que nunca
         * trabajen la CPU y el ME a la vez sobre el estado del audio (el trabajo siguiente lo manda el hilo principal en audio_psp_tick_end) */
        if (!sAudioMeDriven && sMeJobBlocks <= 0 && psp_me_take_resumed()) {
            sAudioMeDriven = true;
        }
        if (sAudioMeDriven) {
            sceKernelDelayThread(4000); /* con el ME el audio lo manda el hilo principal; este hilo solo vigila */
        } else if (gProcessAudio && (buffered < PSP_AUDIO_TARGET_FRAMES) &&
            (audio_psp_free_frames() >= PSP_AUDIO_SAMPLES_HIGH)) {
            const uint32_t frames = audio_psp_next_block_frames();
            /* sin ME: la sintesis corre en la CPU principal y le roba tiempo al juego (se cuenta para el log) */
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
