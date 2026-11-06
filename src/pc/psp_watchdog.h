#ifndef PSP_WATCHDOG_H
#define PSP_WATCHDOG_H

/* Freeze report (PSP only): a watchdog thread checks once per second that the game keeps finishing frames. If a few seconds pass without any it writes
 * "freeze.txt" (next to sm64config.txt) with what the main thread was doing, what each thread is waiting on, the state of the GE and the ME, the battery and
 * the level / Mario's action. It only writes when there is a freeze; the rest of the time it costs a few microseconds per second. */
#ifdef TARGET_PSP
void psp_watchdog_start(void);                       /* from the main thread, once */
void psp_watchdog_register(const char *label, int thid); /* one more thread for the report (maximum 8) */
extern const char *volatile gPspWdPhase;             /* which part of the frame the main thread is working on (constant string) */
#define PSP_WD_PHASE(s) (gPspWdPhase = (s))
#else
#define PSP_WD_PHASE(s) ((void) 0)
#endif

#endif
