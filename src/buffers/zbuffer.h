#ifndef ZBUFFER_H
#define ZBUFFER_H

#include <PR/ultratypes.h>

#include "config.h"
#include "macros.h"

#ifdef TARGET_PSP
/* The PSP draws with its own depth buffer in VRAM: the game only uses this one's address (the N64 display list names it, and the renderer compares
 * addresses). The title screen's Mario head, which on the N64 also borrowed this memory, takes its memory from the main pool (level_script.c). */
#define GZBUFFER_LEN 8
#else
#define GZBUFFER_LEN (SCREEN_WIDTH * SCREEN_HEIGHT)
#endif
extern u16 gZBuffer[GZBUFFER_LEN];

#endif // ZBUFFER_H
