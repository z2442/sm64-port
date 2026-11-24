#include <ultra64.h>

#include "config.h"
#include "framebuffers.h"

// 0x70800 bytes
#ifdef AVOID_UB
u16 gFrameBuffers[3][GFRAMEBUFFER_LEN];
#else
u16 gFrameBuffer0[GFRAMEBUFFER_LEN];
u16 gFrameBuffer1[GFRAMEBUFFER_LEN];
u16 gFrameBuffer2[GFRAMEBUFFER_LEN];
#endif
