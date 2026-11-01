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
#define PSP_ME_JOB_TIMEOUT_US 100000   /* espera bloqueante maxima por un trabajo; despues el ME se da por "ocupado" y se sigue sin esperar */
#define PSP_ME_STUCK_DEAD_US 3000000   /* si en 3 s no termina, se da por muerto y el audio pasa a la CPU para siempre */
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
    PSP_ME_SHARED_JOB_FRAMES, /* PSP_ME_JOB_MAX entradas */
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

unsigned int gPspMePolls, gPspMeUs, gPspMeBlocks, gPspMeWaits; /* diagnostico del log: sondeos, tiempo que el hilo principal espero al ME, bloques de audio mandados al ME y veces que hubo que esperar */
static bool sPspMeBootStarted;
static int sPspMeBootResult = -1;
static bool sPspMeInitialized;
static volatile bool sPspMeSuspended; /* la consola se esta suspendiendo (o ya se suspendio): no se mandan mas trabajos al ME */
static volatile bool sPspMeResumeArmed; /* el ME quedo detenido (HALTED) antes de dormir: al despertar se le puede hacer un reset de hardware y volver a arrancarlo */
static volatile bool sPspMeResumed;      /* el ME volvio a estar listo tras reanudar: el audio puede volver a usarlo (lo recoge psp_me_take_resumed) */
unsigned int gPspMeResumeOk, gPspMeResumeFail; /* diagnostico: reanudaciones del ME que salieron bien / mal */
static volatile unsigned int sPspMeWakeCalls; /* diagnostico: veces que libme-core llamo a meLibOnWake */
static unsigned int sPspMePwrFlags[6]; /* diagnostico: flags de los 6 ultimos avisos de energia (el mas reciente en [0]) */
static unsigned int sPspMePwrN0, sPspMePwrN1; /* diagnostico: llamadas a psp_me_power_event(0) / (1) */
static int sPspMeSuspState = -1;               /* diagnostico: estado del ME al terminar de pararlo antes de dormir (5 = HALTED) */
static int sPspMeResumeState = -1, sPspMeResumeProg = -1; /* diagnostico: estado/progreso del ME cuando se rindio la reanudacion */
static bool sPspMeStuck;           /* un trabajo supero PSP_ME_JOB_TIMEOUT_US y se espera sin bloquear */
static uint32_t sPspMeStuckT0;
unsigned int gPspMeTimeouts, gPspMeRecovered; /* diagnostico del log: trabajos que pasaron el timeout / terminaron tarde y se recuperaron */
static int sPspMeBootTries; /* diagnostico: bit0 = primer intento, bit1 = reintento desde la carpeta de partidas */

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

/* Ganchos de suspension de libme-core: su manejador de eventos del sistema los llama en modo kernel durante el sueno/despertar. Aqui NO se
 * toca el hardware (el reset del ME se hace mas tarde, ya con la consola despierta, en psp_me_power_event(0)): solo se cuenta la llamada. */
__attribute__((noinline, aligned(4))) void meLibOnSleep(void) {
}

__attribute__((noinline, aligned(4))) void meLibOnWake(void) {
    sPspMeWakeCalls++;
}

/* Se ejecuta en modo kernel (via kcall): vuelve a copiar el codigo de arranque del ME a su memoria y le hace el reset de hardware (meLibReset),
 * lo mismo que hizo meLibDefaultInit la primera vez. */
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
             * Un "trabajo" son 1..PSP_ME_JOB_MAX bloques seguidos: el hilo principal no toca el estado del audio mientras el ME
             * trabaja (ver audio_psp_tick_begin / audio_psp_tick_end), asi que las cachas de las dos CPU no se pisan. */
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
    /* libme-core escribe "kcall.prx" en el DIRECTORIO ACTUAL del sistema de archivos de la PSP y lo carga desde ahi (-3 = no pudo
     * escribirlo). Segun como se lance el juego (cargador de .prx, carpeta de solo lectura...) ese directorio puede no ser
     * escribible; entonces el ME no arranca y todo el audio se sintetiza en la CPU principal (~20 % de CPU). Primero se intenta
     * como siempre; si falla con -3 se repite desde la carpeta de partidas guardadas, que siempre es escribible. (sceIoChdir solo
     * cambia el directorio del nucleo; las rutas relativas de la libc del juego no cambian.) */
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

/* diagnostico para el log: 0 = resultado de meLibDefaultInit (<0: fallo), 1 = inicializado, 2 = progreso del ME, 3 = estado */
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

/* Manda un trabajo al ME: "count" bloques seguidos de audio (frames[i] muestras cada uno) escritos uno tras otro en samples.
 * No espera: el hilo principal recoge el resultado con psp_me_job_wait(). Devuelve false si el ME no esta listo. */
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

/* Espera a que el ME termine el trabajo y deja la cache del hilo principal al dia (descarta lineas viejas del estado del audio).
 * Normalmente el ME ya termino (trabaja mientras la CPU dibuja) y no se espera nada.
 * Antes, UN solo trabajo que tardara mas de 1 s (visto al entrar a un nivel) congelaba el juego 1 s y desactivaba el ME para toda la
 * sesion (todo el audio pasaba a la CPU principal: ~5 ms por frame). Ahora: tras PSP_ME_JOB_TIMEOUT_US se devuelve BUSY y los ticks
 * siguientes solo miran (sin bloquear); si el ME termina tarde se descarta ese trabajo y se sigue usando el ME. Mientras esta ocupado
 * la CPU NO sintetiza (tocaria el mismo estado que el ME); solo si pasan PSP_ME_STUCK_DEAD_US se da por muerto. */
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
        sPspMeInitialized = false; /* FAULT / HALTED: el ME se detuvo, no hay vuelta atras */
        return PSP_ME_WAIT_DEAD;
    }

    /* Discard stale Allegrex cache lines after the ME published audio state
     * and PCM. */
    sceKernelDcacheWritebackInvalidateAll();
    return PSP_ME_WAIT_DONE;
}


/* Suspension de la consola. El ME ejecuta NUESTRO codigo en un bucle sin fin (sondeando el estado compartido); si la consola se duerme con el ME asi,
 * el sistema no consigue volver a encenderla al reanudar (el ME tiene que estar parado). Por eso, al suspender: se dejan de mandar trabajos, se
 * espera a que termine el que haya en curso (el ME solo mira la orden de parar entre trabajos: si se pidiera con un trabajo en marcha, al terminar
 * lo pisaria con IDLE y la orden se perderia), se le ordena parar y se espera a que quede detenido. Mientras tanto el audio pasa a la CPU.
 * Al reanudar (RESUME_COMPLETE) el gancho meLibOnWake ya le hizo el reset de hardware: aqui se le vuelve a dar la orden de arranque, se espera a que
 * este listo y se avisa al audio (psp_me_take_resumed). Si algo falla o tarda, el audio simplemente se queda en la CPU. */
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
        /* el ME esta apagado desde que se durmio la consola: se le hace el reset de arranque (en modo kernel) y se le suelta con IDLE
         * (meLibOnProcess espera BOOTING); si no esta listo en 600 ms se repite el reset una vez */
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
            sPspMeState = PSP_ME_STATE_STOP; /* por si llegara a arrancar mas tarde, que se pare */
            meLibSync();
            sPspMeSuspended = false;
            gPspMeResumeFail++;
        }
        return;
    }
    if (sPspMeSuspended) {
        return; /* ya suspendido (aviso repetido): no se pierde el permiso de reanudar */
    }
    sPspMeSuspended = true;
    sPspMeResumeArmed = false;
    if (!sPspMeBootStarted || (sPspMeBootResult < 0) || !sPspMeInitialized) {
        return;
    }
    sceKernelDelayThread(3000); /* un psp_me_job_start que ya pasara la comprobacion termina de publicar su trabajo */
    t0 = sceKernelGetSystemTimeLow();
    for (;;) {
        st = sPspMeState;
        if (st == PSP_ME_STATE_IDLE) {
            sPspMeState = PSP_ME_STATE_STOP;
            meLibSync();
            break;
        }
        if (st != PSP_ME_STATE_GENERATE_AUDIO) {
            break; /* ya detenido / fallo / parando */
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
        /* detenido de verdad: se prepara el estado para el proximo arranque y se permite el reset al despertar */
        sPspMeProgress = 0;
        sPspMeState = PSP_ME_STATE_BOOTING;
        meLibSync();
        sPspMeResumeArmed = true;
    }
    sceKernelDcacheWritebackInvalidateAll(); /* lo ultimo que escribio el ME (estado del audio) tiene que leerse de memoria */
    sPspMeInitialized = false;
}

/* true una sola vez cuando el ME volvio a estar listo tras reanudar (lo llama el hilo productor de audio entre dos sintesis en la CPU) */
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
