#ifndef FRAMEBUFFERS_H
#define FRAMEBUFFERS_H

#include <PR/ultratypes.h>

#include "config.h"

// level_script.c assumes that the frame buffers are adjacent, while game.c's
// -g codegen implies that they are separate variables. This is impossible to
// reconcile without undefined behavior. Avoid that when possible.
#ifdef TARGET_PSP
/* (as gZBuffer, see zbuffer.h: on the PSP only their addresses are used) */
#define GFRAMEBUFFER_LEN 8
#else
#define GFRAMEBUFFER_LEN (SCREEN_WIDTH * SCREEN_HEIGHT)
#endif
#ifdef AVOID_UB
extern u16 gFrameBuffers[3][GFRAMEBUFFER_LEN];
#define gFrameBuffer0 gFrameBuffers[0]
#define gFrameBuffer1 gFrameBuffers[1]
#define gFrameBuffer2 gFrameBuffers[2]
#else
extern u16 gFrameBuffer0[GFRAMEBUFFER_LEN];
extern u16 gFrameBuffer1[GFRAMEBUFFER_LEN];
extern u16 gFrameBuffer2[GFRAMEBUFFER_LEN];
#endif

#endif // FRAMEBUFFERS_H
