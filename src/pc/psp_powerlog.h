#ifndef PSP_POWERLOG_H
#define PSP_POWERLOG_H

/* Measurement of what happens around a console sleep (power notices, video, ME, audio), written to ms0:/sm64_power.txt.
 * OFF by default: build with PSP_POWERLOG=1 (make TARGET_PSP=1 pbp VERSION=us PSP_POWERLOG=1) to get it; without it every call below is empty.
 *
 * The events are kept in memory and the file is only written with the console awake and settled (never inside the sleep / wake sequence),
 * and for 4 s after each wake-up the audio state is sampled every 100 ms (see psp_powerlog.c). */

#ifndef PSP_POWERLOG
#define PSP_POWERLOG 0
#endif

#if defined(TARGET_PSP) && PSP_POWERLOG
void psp_plog_start(void);                          /* boot: creates the writer thread */
void psp_plog(const char *fmt, ...) __attribute__((format(printf, 1, 2))); /* one line (any thread; not from the ME's sleep hook) */
void psp_plog_suspending(void);                      /* a sleep is starting: nothing is written to the Memory Stick until the wake-up has settled */
void psp_plog_resumed(void);                         /* the wake-up finished: sample the audio for 4 s, then write */
int audio_psp_plog_state(char *buf, int n);          /* audio_psp.c: the audio state, for the samples */
#else
static inline void psp_plog_start(void) {}
static inline void psp_plog(const char *fmt, ...) { (void) fmt; }
static inline void psp_plog_suspending(void) {}
static inline void psp_plog_resumed(void) {}
#endif

#endif
