#pragma once

#include <stdbool.h>
#include <stdint.h>

#if defined(TARGET_PSP)
int psp_me_boot(void);
bool psp_me_init(void);
bool psp_me_is_active(void);
void psp_me_shutdown(void);
/* The main thread sends the ME up to PSP_ME_JOB_MAX audio blocks per job and collects them afterwards */
#define PSP_ME_JOB_MAX 8
bool psp_me_job_start(int16_t *samples, const uint32_t *frames, int count);
/* Power notice (called by the pc_main.c callback): on suspend it stops the ME and on resume it starts it again (see psp_me.c). */
void psp_me_power_event(int suspending);
void psp_me_note_flags(int flags); /* diagnostic */
/* true once when the ME is ready again after resuming */
bool psp_me_take_resumed(void);
/* Result of psp_me_job_wait */
#define PSP_ME_WAIT_DEAD 0  /* the ME is no longer usable: synthesize on the CPU */
#define PSP_ME_WAIT_DONE 1  /* job finished, the audio is valid */
#define PSP_ME_WAIT_BUSY 2  /* the ME is still on the job (slow): do not touch the audio state, look again next tick */
#define PSP_ME_WAIT_STALE 3 /* finished late: discard its audio, the ME is still usable */
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
