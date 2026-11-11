#ifndef AUDIO_PSP_H
#define AUDIO_PSP_H

#include <stdbool.h>
#include <stdint.h>

struct AudioAPI;

extern struct AudioAPI audio_psp;

#if defined(TARGET_PSP)
bool audio_psp_start(void);
void audio_psp_shutdown(void);
void audio_psp_generate_cpu(int16_t *samples, uint32_t frames);
void audio_psp_tick_begin(void);
void audio_psp_tick_end(void);
#endif

#endif
