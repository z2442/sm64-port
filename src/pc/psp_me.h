#pragma once

#include <stdbool.h>
#include <stdint.h>

#if defined(TARGET_PSP)
int psp_me_boot(void);
bool psp_me_init(void);
bool psp_me_is_active(void);
void psp_me_shutdown(void);
/* El hilo principal manda al ME hasta PSP_ME_JOB_MAX bloques de audio por trabajo y los recoge despues */
#define PSP_ME_JOB_MAX 8
bool psp_me_job_start(int16_t *samples, const uint32_t *frames, int count);
/* Aviso de energia (lo llama el callback de pc_main.c): al suspender detiene el ME y al reanudar lo vuelve a arrancar (ver psp_me.c). */
void psp_me_power_event(int suspending);
void psp_me_note_flags(int flags); /* diagnostico */
/* true una vez cuando el ME volvio a estar listo tras reanudar */
bool psp_me_take_resumed(void);
/* Resultado de psp_me_job_wait */
#define PSP_ME_WAIT_DEAD 0  /* el ME ya no sirve: sintetizar en la CPU */
#define PSP_ME_WAIT_DONE 1  /* trabajo terminado, el audio es valido */
#define PSP_ME_WAIT_BUSY 2  /* el ME sigue con el trabajo (lento): no tocar el estado del audio, volver a mirar el tick siguiente */
#define PSP_ME_WAIT_STALE 3 /* termino tarde: descartar su audio, el ME sigue usable */
int psp_me_job_wait(void);
#else
static inline int psp_me_boot(void) {
    return -1;
}

static inline bool psp_me_init(void) {
    return false;
}

static inline bool psp_me_is_active(void) {
    return false;
}

static inline void psp_me_shutdown(void) {
}

static inline bool psp_me_job_start(int16_t *samples, const uint32_t *frames, int count) {
    (void)samples;
    (void)frames;
    (void)count;
    return false;
}

static inline int psp_me_job_wait(void) {
    return 0;
}

static inline void psp_me_power_event(int suspending) {
    (void)suspending;
}

static inline void psp_me_note_flags(int flags) {
    (void)flags;
}

static inline bool psp_me_take_resumed(void) {
    return false;
}
#endif
