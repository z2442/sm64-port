#define TARGET_SCEGU 1
#if defined(TARGET_SCEGU) || defined(TARGET_PSP)

#define PSP_ARENA_C18 0
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif
#include <PR/gbi.h>

#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspimpose.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspctrl.h>
#include <psputils.h>
#include <pspgum.h>
#include <intraFont.h>
#include <string.h>

#include "psp_texture_manager.h"
#include "gfx_screen_config.h"
extern unsigned int texman_get_bound(void); /* psp_texture_manager.c */
#include "../psp_home_menu.h"
#include "../psp_dve.h"
#include "../psp_watchdog.h"
#include "gfx_pc.h"
#include "../fs_utils.h"
#include "level_table.h"
extern s16 gCurrLevelNum; /* game/level_update.c */

/* Deferred flush of the vertex arena (PSP_WB_DEFER): the vertices of each batch are written to RAM (sceKernelDcacheWritebackRange) only once
 * per notice to the GE, not once per batch. The GE only reads the list up to the "stall" it was last given, so what matters is that EVERYTHING
 * pending is in RAM before any libgu call that moves that limit (sceGuGetMemory, sceGuDrawArray, sceGuFinish, sceGuCallList and
 * intraFontPrint, which draws with sceGuDrawArray). These macros do it before each of them, throughout this file. */
#ifndef PSP_WB_DEFER
#define PSP_WB_DEFER 1
#endif
static __attribute__((noinline)) void gfx_scegu_varena_publish(void);
#if PSP_WB_DEFER
#define sceGuGetMemory(s_) (gfx_scegu_varena_publish(), sceGuGetMemory(s_))
#define sceGuDrawArray(...) (gfx_scegu_varena_publish(), sceGuDrawArray(__VA_ARGS__))
#define sceGuFinish() (gfx_scegu_varena_publish(), sceGuFinish())
#define sceGuCallList(l_) (gfx_scegu_varena_publish(), sceGuCallList(l_))
#define intraFontPrint(...) (gfx_scegu_varena_publish(), intraFontPrint(__VA_ARGS__))
#endif

#define BUF_WIDTH (512)
#define SCR_WIDTH (480)
#define SCR_HEIGHT (272)
#define FRAMEBUFFER_SIZE (BUF_WIDTH * SCR_HEIGHT * sizeof(uint16_t))

float identity_matrix[4][4] __attribute__((aligned(16))) = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };

/* Shader IDs
 id        alp fog edg nse ut0 ut1 num sin0 sin1 mul0 mul1 mix0 mix1 cas
-----------------------------------------------------------------------
69        0   0   0   0   1   0   1   0    1    1    1    1    1    0
512       0   0   0   0   0   0   1   1    1    0    1    0    1    0
909       0   0   0   0   1   0   1   0    1    0    1    1    1    0
1361      0   0   0   0   1   0   2   0    1    0    1    1    1    0
2560      0   0   0   0   1   0   0   1    1    0    1    0    1    0
17059909  1   0   0   0   1   0   1   0    0    1    1    1    1    1
17062400  1   0   0   0   1   0   1   1    0    0    1    0    1    0
17305729  1   0   0   0   0   0   2   0    0    1    1    1    1    1
18092101  1   0   0   0   1   0   1   0    0    1    1    1    1    0
18874437  1   0   0   0   1   0   1   0    1    1    0    1    0    0
18874880  1   0   0   0   0   0   1   1    1    0    0    0    0    1
18875277  1   0   0   0   1   0   1   0    1    0    0    1    0    0
18876928  1   0   0   0   1   0   1   1    1    0    0    0    0    0
27263045  1   0   0   0   1   0   1   0    1    1    0    1    0    0
27265536  1   0   0   0   1   0   0   1    1    0    0    0    0    1
27265647  1   0   0   0   1   1   1   0    1    0    0    1    0    0
52428869  1   1   0   0   1   0   1   0    1    1    0    1    0    0
52429312  1   1   0   0   0   0   1   1    1    0    0    0    0    1
52431360  1   1   0   0   1   0   1   1    1    0    0    0    0    0
84168773  1   0   1   0   1   0   1   0    0    1    1    1    1    1
85983744  1   0   1   0   0   0   1   1    1    0    0    0    0    1
94374400  1   0   1   0   1   0   0   1    1    0    0    0    0    1
127928832 1   1   1   0   1   0   0   1    1    0    0    0    0    1
153092165 1   0   0   1   1   0   1   0    1    1    0    1    0    0
153092608 1   0   0   1   0   0   1   1    1    0    0    0    0    1
153093005 1   0   0   1   1   0   1   0    1    0    0    1    0    0
153094656 1   0   0   1   1   0   1   1    1    0    0    0    0    0

printf("%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n", shader_id,
    cc_features.opt_alpha,
    cc_features.opt_fog,
    cc_features.opt_texture_edge,
    cc_features.opt_noise,
    cc_features.used_textures[0],
    cc_features.used_textures[1],
    cc_features.num_inputs,
    cc_features.do_single[0],
    cc_features.do_single[1],
    cc_features.do_multiply[0],
    cc_features.do_multiply[1],
    cc_features.do_mix[0],
    cc_features.do_mix[1],
    cc_features.color_alpha_same
);
*/

/* Shader Working List:
84168773    - Menu Overlays
*/

/* Shader Broken List:
153092165   - Noise
153092608   - Noise
153093005   - Noise
153094656   - Noise
*/

// clang-format off
static uint32_t shader_ids[27] =
{
69       ,
512      ,
909      ,
1361     ,
2560     ,
17059909 ,
17062400 ,
17305729 ,
18092101 ,
18874437 ,
18874880 ,
18875277 ,
18876928 ,
27263045 ,
27265536 ,
27265647 ,
52428869 ,
52429312 ,
52431360 ,
84168773 ,
85983744 ,
94374400 ,
127928832,
153092165,
153092608,
153093005,
153094656
};

static uint32_t shader_remap[27*2] = {
69       ,69       ,
512      ,512      ,
909      ,909      ,
1361     ,1361     ,
2560     ,2560     ,
17059909 ,17059909 ,
17062400 ,17062400 ,
17305729 ,17305729 ,
18092101 ,18092101 ,
18874437 ,18874437 ,
18874880 ,18874880 ,
18875277 ,18875277 ,
18876928 ,18876928 ,
27263045 ,27263045 ,
27265536 ,27265536 ,
27265647 ,27265647 ,
52428869 ,52428869 ,
52429312 ,52429312 ,
52431360 ,52431360 ,
84168773 ,84168773 ,
85983744 ,85983744 ,
94374400 ,94374400 ,
127928832,127928832,
153092165,153092165,
153092608,153092608,
153093005,153093005,
153094656,153094656
};
/* Before, shaders with NOISE (G_AC_DITHER: Mario fading out when teleporting, the vanish cap, etc.)
 * were remapped to shader 69, which is TEXTURED. The color-only ones (Mario's body, id 153092608) ended up
 * enabling GU_TEXTURE_2D with the last bound texture, sampling a transparent texel (u=v=0) and the
 * model disappeared entirely; only the parts with their own texture were visible. Now they re-apply to themselves
 * (the GE has no alpha noise, it is drawn as normal alpha with the progressive fade). */
// clang-format on

static uint32_t shader_broken[27] = {
    153092165, // Noise
    153092608, // Noise
    153093005, // Noise
    153094656  // Noise
};

unsigned int __attribute__((aligned(64))) list[262144 * 2];

/* sceGuGetMemory stores transient vertices in the display list. Menu text can
 * emit many small sprites, so continue in a fresh list before an allocation
 * can overrun the fixed command buffer. GE state and buffers survive this. */
#define GU_LIST_COMMAND_RESERVE 4096

static size_t sVArenaUsed; /* bytes used of the vertex arena (see below) */
static size_t sVArenaWb; /* bytes at the start of the arena that are already written to RAM (only with PSP_WB_DEFER) */

/* ---- Short list for the 2nd frame (PSP_F2_REPLAY) ----
 * The big list `list` (A) stores the scene of the 1st frame of a 60 fps tick. The 2nd frame opens this other one (B), with only its
 * preamble (clear screen, etc.) and a CALL command to the scene that follows in A. That way A is not touched until the next tick.
 * sCurList / sCurListBytes say which one is open; sListGen counts the list restarts (a restart overwrites A from the beginning,
 * so it invalidates any recording). */
#define F2_LIST_WORDS 2048
static unsigned int __attribute__((aligned(64))) listB[F2_LIST_WORDS];
static unsigned int *sCurList = list;
static size_t sCurListBytes = sizeof(list);
static unsigned int sListGen;
extern int ge_list_executed[2]; /* global variable of libpspgu: id of the GE list that sceGuStart opened */
static int sFrameList = -1;     /* id of the GE list that is open (updated by start_frame and list_restart) */

/* Closes the list, waits for the GE and opens another (the GE state is kept). After waiting, the GE no longer reads the arena. */
static void gfx_scegu_list_restart(void) {
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    sceGuStart(GU_DIRECT, sCurList);
    sFrameList = ge_list_executed[0];
    sVArenaUsed = 0;
    sVArenaWb = 0;
    sListGen++;
}

static void gfx_scegu_reserve_list_memory(size_t data_size) {
    const size_t allocation_size = (data_size + 3) & ~(size_t)3;
    const size_t required_size = allocation_size + 8 + GU_LIST_COMMAND_RESERVE;

    if (((size_t)sceGuCheckList() + required_size) <= sCurListBytes) {
        return;
    }

    gfx_scegu_list_restart();
}

/* ---- Arena de vertices (cacheada) ----
 * sceGuGetMemory returns memory INSIDE the command list, and pspgu writes that list through the UNCACHED address
 * (sceGuStart adds 0x40000000): each word is a bus write. Copying ~100 KB of vertices per frame there cost
 * ~0.8 ms of CPU. Here the vertices are copied to a normal (cached) arena and the cache is written to RAM with
 * sceKernelDcacheWritebackRange right before drawing; the GE reads the arena through a pointer (the list only carries the
 * draw command). The arena is reused at the start of each frame (the previous frame already finished: end_frame syncs with the GE) and,
 * if it fills up mid-frame, the GE is waited for and the list is restarted, the same as gfx_scegu_reserve_list_memory.
 * (Idea taken from ghurone/Sonic-R-PSP, which measures the same problem.) PSP_VERTEX_ARENA 0 goes back to sceGuGetMemory. */
#ifndef PSP_VERTEX_ARENA
#define PSP_VERTEX_ARENA 1
#endif
#define VARENA_SIZE (512 * 1024)
static unsigned char sVArena[VARENA_SIZE] __attribute__((aligned(64)));
unsigned int gPspWbCalls; /* real cache flushes of the arena (printed in the log) */
static __attribute__((noinline)) void gfx_scegu_varena_publish(void) {
#if PSP_WB_DEFER
    if (sVArenaUsed > sVArenaWb) {
        sceKernelDcacheWritebackRange(sVArena + sVArenaWb, sVArenaUsed - sVArenaWb);
        sVArenaWb = sVArenaUsed;
        gPspWbCalls++;
    }
#endif
}
unsigned int gPspArenaMax; /* highest arena usage in the profile window (bytes), printed by gfx_pc.c */

/* Guarantees that `bytes` more fit in the arena (if not, it waits for the GE and restarts it). Call BEFORE changing GE state. */
static void gfx_scegu_varena_ensure(size_t bytes) {
    bytes = (bytes + 63) & ~(size_t)63;
    if (sVArenaUsed + bytes > VARENA_SIZE) {
        gfx_scegu_list_restart();
    }
}

static void *gfx_scegu_varena_alloc(size_t bytes) {
    bytes = (bytes + 63) & ~(size_t)63;
    if (sVArenaUsed + bytes > VARENA_SIZE) {
        gfx_scegu_list_restart();
    }
    void *p = sVArena + sVArenaUsed;
    sVArenaUsed += bytes;
    if (sVArenaUsed > gPspArenaMax) {
        gPspArenaMax = (unsigned int)sVArenaUsed;
    }
#if defined(TARGET_PSP) && PSP_ARENA_C18
    /* OFF (patch 108): in the real level it only moved the DRWMS cost (-0.2 ms) to VTXMS (+0.2 ms), with no net gain.
     * Measured on the real PSP (devbench3): writing to cold memory forces reading each 64 B line from RAM first (~220 ns) just to
     * overwrite it entirely. "cache 0x18" (create dirty exclusive) reserves the line in the cache without reading it: copying a batch + writeback goes from
     * 7.2 to 4.6 us (-35 %), with no corrupted data. It is safe here because the arena is aligned to 64 B and each reservation is rounded up to whole lines:
     * no line is shared with another batch, and of what is reserved the GE only reads what is written (the padding bytes at the end are not used). */
    for (size_t o = 0; o < bytes; o += 64) {
        __asm__ volatile("cache 0x18, 0(%0)" :: "r"((unsigned char *)p + o) : "memory");
    }
#endif
    return p;
}

/* ---- Sending draws without a kernel call for each one ----
 * libgu's sceGuDrawArray ALWAYS ends with sceGeListUpdateStallAddr (a kernel call that tells the GE "you can read up to here now"):
 * with ~130 draws per frame that was ~130 calls (the fixed cost of ~35 us per draw seen in the log, DRW). Here the 4 commands of the draw
 * (vertex type, base, address, PRIM) are written to the list with sceGuSendCommandi, which does NOT notify the GE, and the GE is notified every
 * PSP_KICK_DRAWS draws or PSP_KICK_TRIS triangles (sceGuGetMemory(0) moves the list limit up to here). The GE keeps working while the
 * CPU builds the rest of the frame, and the last notice is given by sceGuFinish. What is drawn is identical. PSP_GE_KICK 0 goes back to sceGuDrawArray. */
#ifndef PSP_GE_KICK
#define PSP_GE_KICK 1
#endif
/* Each notice is a kernel call (~30 us). With the GE idle in the scenes that run slowly (the bottleneck is the CPU) it is notified every
 * 12 draws / 220 triangles instead of 8 / 150: ~40% fewer notices, and the GE only has left at the end of the frame what was sent last. */
#ifndef PSP_KICK_DRAWS
#define PSP_KICK_DRAWS 12
#endif
#ifndef PSP_KICK_TRIS
#define PSP_KICK_TRIS 220
#endif
unsigned int gPspKicks, gPspRawDraws; /* notices to the GE / draws sent without notice (printed in the log) */
static unsigned int sKickDraws, sKickTris;

static void gfx_scegu_prim_tris(int vtype, int count, const void *vertices) {
#if PSP_GE_KICK
    sceGuSendCommandi(0x12, vtype);                                       /* VERTEX_TYPE */
    sceGuSendCommandi(0x10, (int)((((unsigned int)vertices) >> 8) & 0xf0000)); /* BASE */
    sceGuSendCommandi(0x01, (int)(unsigned int)vertices);                 /* VADDR */
    sceGuSendCommandi(0x04, (GU_TRIANGLES << 16) | count);                /* PRIM */
    gPspRawDraws++;
    sKickDraws++;
    sKickTris += (unsigned int)count / 3u;
    if (sKickDraws >= PSP_KICK_DRAWS || sKickTris >= PSP_KICK_TRIS) {
        sKickDraws = 0;
        sKickTris = 0;
        gPspKicks++;
        sceGuGetMemory(0); /* moves the list limit (and notifies the GE) up to here */
    }
#else
    sceGuDrawArray(GU_TRIANGLES, vtype, count, 0, vertices);
#endif
}

static unsigned int staticOffset = 0;
/* Fog color in the GE's ABGR format. Updated by gfx_dp_set_fog_color() in gfx_pc.c.
 * It starts as white, which was the fixed value used before. */
unsigned int scegu_fog_color = 0xFFFFFFFF;
static unsigned int sGuFogColor = 0xFFFFFFFF;
float scegu_fog_near = 3600.0f, scegu_fog_far = 32000.0f; /* view distances, computed by gfx_pc.c */
static float sGuFogNear = 3600.0f, sGuFogFar = 32000.0f; /* last color sent to the GE with sceGuFog */

static unsigned int getMemorySize(unsigned int width, unsigned int height, unsigned int psm) {
    switch (psm) {
        case GU_PSM_T4:
            return (width * height) >> 1;

        case GU_PSM_T8:
            return width * height;

        case GU_PSM_5650:
        case GU_PSM_5551:
        case GU_PSM_4444:
        case GU_PSM_T16:
            return 2 * width * height;

        case GU_PSM_8888:
        case GU_PSM_T32:
            return 4 * width * height;

        default:
            return 0;
    }
}

#define TEX_ALIGNMENT (16)
void *getStaticVramBuffer(unsigned int width, unsigned int height, unsigned int psm) {
    unsigned int memSize = getMemorySize(width, height, psm);
    void *result = (void *) (staticOffset | 0x40000000);
    staticOffset += memSize;

    return result;
}

void *getStaticVramBufferBytes(size_t bytes) {
    unsigned int memSize = bytes;
    void *result = (void *) (staticOffset | 0x40000000);
    staticOffset += memSize;

    return (void *) (((unsigned int) result) + ((unsigned int) sceGeEdramGetAddr()));
}

#include "gfx_cc.h"
#include "gfx_rendering_api.h"
#include "macros.h"

enum MixType {
    SH_MT_NONE,
    SH_MT_TEXTURE,
    SH_MT_COLOR,
    SH_MT_TEXTURE_TEXTURE,
    SH_MT_TEXTURE_COLOR,
    SH_MT_COLOR_COLOR,
};

struct ShaderProgram {
    bool enabled;
    uint32_t shader_id;
    struct CCFeatures cc;
    enum MixType mix;
    bool texture_used[2];
    int texfunc; /* last sceGuTexFunc mode applied (to restore after the fog pass) */
    int texture_ord[2];
    int num_inputs;
};

struct SamplerState {
    int min_filter;
    int mag_filter;
    int wrap_s;
    int wrap_t;
    uint32_t tex;
};

typedef struct Vertex {
    float u, v;
    unsigned int color;
    float x, y, z;
} Vertex;

typedef struct VertexColor {
    unsigned short a, b;
    unsigned long color;
    unsigned short x, y, z;
} VertexColor;

static struct ShaderProgram shader_program_pool[64];
static uint8_t shader_program_pool_size;
static struct ShaderProgram *cur_shader = NULL;
static struct SamplerState tmu_state[2];
static bool gl_blend = false;
static void *sDrawBuffer;
static void *sDisplayBuffer;
#define PSP_NFB 3
static void *sFb[PSP_NFB]; /* color buffers in VRAM; frame N is drawn in sFb[N % 3] */

/* ---- TV output (optional, see psp_dve.c) ----
 * The game ALWAYS draws in the 480x272 (5650) buffers; with TV active, at the end of each frame a pass is added to the same list that
 * scales that result (bilinear filter, with 16:9 black bars inside the 4:3 frame) to a 32-bit buffer the size of the TV, and that is the one
 * shown. With the console screen (gPspTvActive == 0) none of this runs. */
#define TV_VRAM_LIMIT (4u * 1024u * 1024u)
int gPspTvStateDirty;            /* 1 after the TV pass: gfx_pc.c sends all the GE state again for the next frame */
static int sTvOn;                /* the TV output is active */
static volatile int sTvCableGone; /* the cable was removed: the normal console frame is shown now (the console goes back to its own screen by itself and the TV frame would look stretched) */
static int sTvW, sTvH, sTvStride, sTvN; /* screen size, buffer width (pixels), number of TV buffers (1 or 3) */
static int sTvDx, sTvDy, sTvDw, sTvDh;  /* rectangle where the 16:9 image goes inside the TV frame */
static void *sTvFb[3];
static void *sTvCur;             /* TV buffer of the frame being built */
#define TV_POOL_BYTES (768u * 503u * 4u) /* 480i: one 768x503x4 buffer (two fields); 480p: one 768x480x4; 240p: three 512x240x4 */
#define TV_FIELD_OFFSET 262      /* 480i: the second field starts at line 262 of the 503 buffer (same as the Zelda port) */
#define TV_REQ_FRAMES 30         /* the TV change is applied when 30 frames pass without the menu changing it again */
static int sTvSlowVbl;           /* the vblank of this TV mode runs at ~30 Hz (interlaced): the 60 frame pace is kept with a clock instead of with the vblank */
static unsigned int sTvVblUs;    /* measured vblank period in this mode (us) */
static unsigned int sTvPaceT;    /* time of the last frame presented with the clock */
static int sTvAuto;              /* gfx_scegu_tv_apply is called by the cable watcher (not the menu): without the 10 s test */
static int sTvMode;             /* TV mode in use (PSP_TV_*) while sTvOn */
static void *sTvPool;            /* VRAM of the TV buffers (reserved once at boot if there are 4 MB; NULL = no TV) */
static int sTvReqMode = -1, sTvReqAge;
#define TV_PROBATION_US 10000000u /* after switching to 480p the menu waits 10 s for confirmation; if there is none, it goes back to the previous mode (the TV may not show 480p) */
extern int gPspOptTv;            /* psp_options.c: TV Output elegido (0 Off / 1 240p / 2 480i / 3 480p) */
extern int gPspOptTvArea;        /* psp_options.c: percentage of the TV frame WIDTH that the image takes (60 to 100) */
extern int gPspOptTvAreaY;       /* psp_options.c: percentage of the TV frame HEIGHT that the image takes (60 to 100) */
extern int gPspOptTvShiftY;      /* psp_options.c: lines (of 480) that the image goes down inside the TV frame (negative = up) */
extern int gPspOptTvAspect;      /* psp_options.c: 0 = 4:3 (16:9 image with bars), 1 = 16:9 (the image fills the whole frame) */
static unsigned int sTvProbT0;   /* time (us) when the 480p test started; 0 = no test in progress */
static int sTvProbBack = PSP_TV_OFF; /* mode it goes back to if not confirmed */
static int sTvRevert = -1;       /* mode it went back to by itself (the menu reads it once); -1 = nothing */
static int sTvReconnect, sTvRefresh, sTvAreaDirty;
static volatile int sTvSuspendReq; /* the power callback asks the game thread to leave the TV output before the console goes to sleep */
static volatile int sTvResumeReq;  /* the power callback tells the game thread that the console woke up (see gfx_scegu_start_frame) */
static int sTvAutoHold;            /* frames to wait before the cable watcher turns the TV on by itself (after waking up the DVE needs a moment) */
static int sTvAutoFails;           /* failed automatic attempts in a row; after TV_AUTO_TRIES it stops trying until the cable is plugged in again */
#define TV_AUTO_TRIES 10
static int sTvDispMode = -1, sTvDispW, sTvDispH; /* display mode the DVE left when the TV was turned on (0x1D1 / 0x1D2); if it changes the system took the output back */
static int sTvKeepVbl;             /* re-route to the same mode: the vblank period measured before is still valid */
static bool sHomeMenuBgActive;
static bool sHomeMenuBgCaptureRequested;
static bool sHomeMenuBgCaptured;
static uint16_t sHomeMenuBgBuffer[BUF_WIDTH * SCR_HEIGHT] __attribute__((aligned(64)));
static uint16_t sHomeMenuBgBlurScratch[BUF_WIDTH * SCR_HEIGHT] __attribute__((aligned(64)));
static intraFont *sHomeMenuFont;
static bool sHomeMenuFontInitTried;

extern void memcpy_vfpu(void *dst, const void *src, size_t size);

static void *gfx_scegu_vram_cpu_addr(const void *vram_buffer) {
    return (void *)(((uintptr_t)sceGeEdramGetAddr() | 0x40000000U) +
                    ((uintptr_t)vram_buffer & 0x00FFFFFFU));
}

static void gfx_scegu_copy_framebuffer_from_vram(uint16_t *dst, const void *src) {
    memcpy_vfpu(dst, gfx_scegu_vram_cpu_addr(src), FRAMEBUFFER_SIZE);
}

static void gfx_scegu_copy_framebuffer_to_vram(void *dst, const uint16_t *src) {
    void *dst_addr = gfx_scegu_vram_cpu_addr(dst);

    memcpy_vfpu(dst_addr, src, FRAMEBUFFER_SIZE);
    sceKernelDcacheWritebackRange(dst_addr, FRAMEBUFFER_SIZE);
}

static uint16_t gfx_scegu_make_rgb565(unsigned int r, unsigned int g, unsigned int b) {
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static void gfx_scegu_accum_rgb565(uint16_t color, unsigned int *r, unsigned int *g,
                                   unsigned int *b) {
    *r += ((color >> 11) & 0x1F) << 3;
    *g += ((color >> 5) & 0x3F) << 2;
    *b += (color & 0x1F) << 3;
}

static int gfx_scegu_clamp_int(int value, int minimum, int maximum) {
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return value;
}

static void gfx_scegu_blur_framebuffer_565(uint16_t *pixels) {
    static const int offsets[3] = { -3, 0, 3 };
    int x;
    int y;
    int ox;
    int oy;

    memcpy_vfpu(sHomeMenuBgBlurScratch, pixels, FRAMEBUFFER_SIZE);
    for (y = 0; y < SCR_HEIGHT; y++) {
        for (x = 0; x < SCR_WIDTH; x++) {
            unsigned int r = 0;
            unsigned int g = 0;
            unsigned int b = 0;

            for (oy = 0; oy < 3; oy++) {
                int sample_y = gfx_scegu_clamp_int(y + offsets[oy], 0, SCR_HEIGHT - 1);

                for (ox = 0; ox < 3; ox++) {
                    int sample_x = gfx_scegu_clamp_int(x + offsets[ox], 0, SCR_WIDTH - 1);
                    gfx_scegu_accum_rgb565(pixels[(sample_y * BUF_WIDTH) + sample_x], &r, &g, &b);
                }
            }

            sHomeMenuBgBlurScratch[(y * BUF_WIDTH) + x] =
                gfx_scegu_make_rgb565(r / 9, g / 9, b / 9);
        }
    }

    memcpy_vfpu(pixels, sHomeMenuBgBlurScratch, FRAMEBUFFER_SIZE);
    sceKernelDcacheWritebackRange(pixels, FRAMEBUFFER_SIZE);
}

static void gfx_scegu_apply_home_menu_2d_view(void) {
    sceGuOffset(2048 - (SCR_WIDTH / 2), 2048 - (SCR_HEIGHT / 2));
    sceGuViewport(2048 - (SCR_WIDTH / 2), 2048 - (SCR_HEIGHT / 2), SCR_WIDTH, SCR_HEIGHT);
    sceGuScissor(0, 0, SCR_WIDTH, SCR_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuSetMatrix(GU_VIEW, (const ScePspFMatrix4 *)identity_matrix);
    sceGuSetMatrix(GU_MODEL, (const ScePspFMatrix4 *)identity_matrix);
}

static unsigned int gfx_scegu_rgba(unsigned int r, unsigned int g, unsigned int b,
                                   unsigned int a) {
    return (a << 24) | (b << 16) | (g << 8) | r;
}

static void gfx_scegu_draw_rect(int x, int y, int width, int height, unsigned int color) {
    VertexColor *verts;

    if ((width <= 0) || (height <= 0)) {
        return;
    }

    gfx_scegu_reserve_list_memory(sizeof(VertexColor) * 2);
    verts = (VertexColor *)sceGuGetMemory(sizeof(VertexColor) * 2);
    if (verts == NULL) {
        return;
    }

    verts[0].a = 0;
    verts[0].b = 0;
    verts[0].color = color;
    verts[0].x = (unsigned short)x;
    verts[0].y = (unsigned short)y;
    verts[0].z = 0;
    verts[1].a = 0;
    verts[1].b = 0;
    verts[1].color = color;
    verts[1].x = (unsigned short)(x + width);
    verts[1].y = (unsigned short)(y + height);
    verts[1].z = 0;

    gfx_scegu_apply_home_menu_2d_view();
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT |
                                   GU_TRANSFORM_2D,
                   2, 0, verts);
}

static bool gfx_scegu_ensure_home_menu_font(void) {
    if (!sHomeMenuFontInitTried) {
        sHomeMenuFontInitTried = true;
        if (intraFontInit()) {
            sHomeMenuFont = intraFontLoad("flash0:/font/ltn0.pgf", INTRAFONT_CACHE_ASCII);
        }
    }
    return sHomeMenuFont != NULL;
}

static void gfx_scegu_draw_home_menu_text(int x, int y, const char *text, float size,
                                          unsigned int color, unsigned int options) {
    if (!gfx_scegu_ensure_home_menu_font()) {
        return;
    }

    gfx_scegu_apply_home_menu_2d_view();
    intraFontActivate(sHomeMenuFont);
    intraFontSetStyle(sHomeMenuFont, size, color, gfx_scegu_rgba(0, 0, 0, 180), 0.0f,
                      options | INTRAFONT_STRING_ASCII);
    intraFontPrint(sHomeMenuFont, (float)x, (float)y, text);
}

static void gfx_scegu_prepare_home_menu_draw(void) {
    gfx_scegu_apply_home_menu_2d_view();
    sceGuTexOffset(0.0f, 0.0f);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexEnvColor(0xffffffff);
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_FOG);
    sceGuDisable(GU_LIGHTING);
    sceGuDisable(GU_CULL_FACE);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuDepthMask(GU_FALSE);
}

static void gfx_scegu_render_home_menu_main(int selected_index, uint8_t highlight_red,
                                            uint8_t highlight_green, uint8_t highlight_blue) {
    static const char *items[] = {
        "Resume Game",
        "Controller Mapping",
        "Exit Game",
    };
    int i;

    gfx_scegu_draw_rect(0, 0, SCR_WIDTH, SCR_HEIGHT, gfx_scegu_rgba(0, 0, 0, 96));
    gfx_scegu_draw_rect(102, 44, 276, 184, gfx_scegu_rgba(0, 0, 0, 150));
    gfx_scegu_draw_home_menu_text(SCR_WIDTH / 2, 78, "Super Mario 64", 0.82f,
                                  gfx_scegu_rgba(255, 255, 245, 255), INTRAFONT_ALIGN_CENTER);

    for (i = 0; i < 3; i++) {
        int y = 122 + (i * 38);
        unsigned int color = gfx_scegu_rgba(218, 224, 218, 255);

        if (selected_index == i) {
            gfx_scegu_draw_rect(124, y - 22, 232, 28,
                                gfx_scegu_rgba(highlight_red, highlight_green,
                                               highlight_blue, 205));
            color = gfx_scegu_rgba(255, 255, 245, 255);
        }
        gfx_scegu_draw_home_menu_text(SCR_WIDTH / 2, y, items[i], 0.72f, color,
                                      INTRAFONT_ALIGN_CENTER);
    }
}

static void gfx_scegu_render_controller_mapping(int selected_index, const char *status_message,
                                                 uint8_t highlight_red, uint8_t highlight_green,
                                                 uint8_t highlight_blue) {
    char line[96];
    char value[64];
    int binding_count = psp_home_menu_get_binding_count();
    int deadzone_row = binding_count;
    int save_row = binding_count + 1;
    int reset_row = binding_count + 2;
    int back_row = binding_count + 3;
    int total_rows = back_row + 1;
    int visible_rows = 7;
    int first_row = selected_index - (visible_rows / 2);
    int row;

    if (first_row < 0) {
        first_row = 0;
    }
    if ((first_row + visible_rows) > total_rows) {
        first_row = total_rows - visible_rows;
    }

    gfx_scegu_draw_rect(0, 0, SCR_WIDTH, SCR_HEIGHT, gfx_scegu_rgba(0, 0, 0, 112));
    gfx_scegu_draw_rect(34, 20, 412, 232, gfx_scegu_rgba(0, 0, 0, 166));
    gfx_scegu_draw_home_menu_text(SCR_WIDTH / 2, 50, "Controller Mapping", 0.82f,
                                  gfx_scegu_rgba(255, 255, 245, 255), INTRAFONT_ALIGN_CENTER);

    for (row = first_row; row < first_row + visible_rows; row++) {
        int y = 82 + ((row - first_row) * 22);
        unsigned int color = gfx_scegu_rgba(218, 224, 218, 255);

        if (selected_index == row) {
            gfx_scegu_draw_rect(54, y - 17, 372, 22,
                                gfx_scegu_rgba(highlight_red, highlight_green,
                                               highlight_blue, 205));
            color = gfx_scegu_rgba(255, 255, 245, 255);
        }

        if (row < binding_count) {
            psp_home_menu_get_binding_value_text(row, value, sizeof(value));
            snprintf(line, sizeof(line), "%s: %.40s", psp_home_menu_get_binding_name(row), value);
        } else if (row == deadzone_row) {
            snprintf(line, sizeof(line), "Deadzone: %d", psp_home_menu_get_deadzone());
        } else if (row == save_row) {
            snprintf(line, sizeof(line), "Save sm64config.txt");
        } else if (row == reset_row) {
            snprintf(line, sizeof(line), "Reset defaults");
        } else {
            snprintf(line, sizeof(line), "Back");
        }

        gfx_scegu_draw_home_menu_text(70, y, line, 0.68f, color, INTRAFONT_ALIGN_LEFT);
    }

    gfx_scegu_draw_home_menu_text(
        SCR_WIDTH / 2, 242,
        ((status_message != NULL) && status_message[0])
            ? status_message
            : "Left/Right change  Cross select  Circle back",
        0.48f, gfx_scegu_rgba(185, 195, 190, 255), INTRAFONT_ALIGN_CENTER);
}

static inline uint32_t get_shader_index(uint32_t id) {
    size_t i;
    for (i = 0; i < 27; i++) {
        if (shader_ids[i] == id) {
            return i;
        }
    }
    char msg[64]; /* "ERROR! Shader not known " + up to 10 digits + newline = 36 bytes: the old 32-byte buffer could overflow */
    snprintf(msg, sizeof(msg), "ERROR! Shader not known %lu\n", (unsigned long)id);
    sceIoWrite(2, msg, strlen(msg));
    return 0;
}

static inline uint32_t get_shader_remap(uint32_t id) {
    size_t index = get_shader_index(id);
    return shader_remap[index * 2 + 1];
}

static inline bool is_shader_enabled(uint32_t id) {
    size_t i;
    for (i = 0; i < 27; i++) {
        if (shader_broken[i] == id) {
            return false;
        }
    }
    return true;
}

static struct ShaderProgram *get_shader_from_id(uint32_t id) {
    size_t i;
    for (i = 0; i < shader_program_pool_size; i++) {
        if (shader_program_pool[i].shader_id == id) {
            return &shader_program_pool[i];
        }
    }
    return NULL;
}

static bool gfx_scegu_z_is_from_0_to_1(void) {
    return true;
}

static inline int texenv_set_color(UNUSED struct ShaderProgram *prg) {
    return GU_TFX_MODULATE;
}

static inline int texenv_set_texture(UNUSED struct ShaderProgram *prg) {
    return GU_TFX_MODULATE;
}

static inline int texenv_set_texture_color(struct ShaderProgram *prg) {
    int mode;
    /* BLENDRGBFADEA (TEXEL0, SHADE, TEXEL0_ALPHA, SHADE / alpha = ENV): texture over the color according to ITS alpha = DECAL on the GE,
     * in all variants (with noise, with fog). BLENDRGBA (alpha = SHADE, explosions and smoke) stays on MODULATE.
     * If the patch was already drawn in a color pass (Boo), gfx_pc.c asks for MODULATE for that pass (scegu_enva_modulate). */
    if ((prg->shader_id & 0xFFFu) == 0x38Du && (prg->shader_id & (1u << 28))) { /* SHADER_OPT_ENVA: alpha = ENV */
        return GU_TFX_DECAL;
    }
    /*@Hack: lord forgive me for this, but this is easier */
    switch (prg->shader_id) {
        case 0x01045A00: // peach letter (castle grounds intro only: the same shader id is also used by translucent 16-bit fire sprites, see below)
            mode = (gCurrLevelNum == LEVEL_CASTLE_GROUNDS) ? GU_TFX_DECAL : GU_TFX_MODULATE;
            break;
        case 0x0000038D: // mario's eyes
        case 0x0920038D: // Mario's eyes/emblem fading out (BLENDRGBFADEA + noise): texture over the color by its alpha
        case 0x01200A00: // intro copyright fade in
            mode = GU_TFX_DECAL;
            break;
        case 0x00000551: // goddard
            mode = GU_TFX_BLEND;
            break;
        default:
            mode = GU_TFX_MODULATE;
            break;
    }

    return mode;
}

static inline int texenv_set_texture_texture(UNUSED struct ShaderProgram *prg) {
    /*@Note: hack shader 0x1A00A6F for Bowser/Peach Paintings (still broken, but just fixed on peach)*/
    return GU_TFX_DECAL;
}

static void gfx_scegu_apply_shader(struct ShaderProgram *prg) {
    /* The alpha test is ALWAYS decided, also in shaders without a texture. Before, shaders without a texture left
     * the function before touching it and inherited the state of the previous shader: with the test on
     * (GREATER 0x55) fragments with alpha <= 85 were discarded, e.g. the color fill of the cap
     * blocks (alpha 80), which was only seen depending on what had been drawn last. */
    if (prg->shader_id & SHADER_OPT_TEXTURE_EDGE) {
        sceGuEnable(GU_ALPHA_TEST);
        sceGuAlphaFunc(GU_GREATER, 0x55, 0xff); /* 0.3f  */
    } else {
        sceGuDisable(GU_ALPHA_TEST);
    }

    // If we have textures, Enable otherwise Disable
    if (prg->texture_used[0] || prg->texture_used[1]) {
        sceGuEnable(GU_TEXTURE_2D);
    } else {
        sceGuDisable(GU_TEXTURE_2D);
        return;
    }

    /* The fog (linear GE or exact 2nd pass) is decided in gfx_scegu_draw_triangles. */

    if (prg->num_inputs) {
        // have colors
        // TODO: more than one color (maybe glSecondaryColorPointer?)
        // HACK: if there's a texture and two colors, one of them is likely for speculars or some shit
        // (see mario head)
        //       if there's two colors but no texture, the real color is likely the second one
        /*
        const int hack = (prg->num_inputs > 1) * (4 - (int)prg->texture_used[0]);
        glEnableClientState(GL_COLOR_ARRAY);
        glColorPointer(4, GL_FLOAT, cur_buf_stride, ofs + hack);
        ofs += 4 * prg->num_inputs;
        */
    }

    if (prg->shader_id & SHADER_OPT_TEXTURE_EDGE) {
        // (horrible) alpha discard
        sceGuEnable(GU_ALPHA_TEST);
        sceGuAlphaFunc(GU_GREATER, 0x55, 0xff); /* 0.3f  */
    } else {
        sceGuDisable(GU_ALPHA_TEST);
    }

    if (!prg->enabled) {
        // configure formulae, we only need to do this once
        prg->enabled = true;

        int mode;
        switch (prg->mix) {
            case SH_MT_TEXTURE:
                mode = texenv_set_texture(prg);
                break;
            case SH_MT_TEXTURE_TEXTURE:
                mode = texenv_set_texture_texture(prg);
                break;
            case SH_MT_TEXTURE_COLOR:
                mode = texenv_set_texture_color(prg);
                break;
            default:
                mode = texenv_set_color(prg);
                break;
        }

        /* Transition Screens */
        if (prg->shader_id == 0x01A00045) {
            mode = GU_TFX_REPLACE;
        }
        prg->texfunc = mode;
        sceGuTexFunc(mode, GU_TCC_RGBA);
    }
}

static void gfx_scegu_unload_shader(struct ShaderProgram *old_prg) {
    if (cur_shader && (cur_shader == old_prg || !old_prg)) {
        cur_shader->enabled = false;
        cur_shader = NULL;
    }
}

static void gfx_scegu_load_shader(struct ShaderProgram *new_prg) {
    cur_shader = new_prg;
    gfx_scegu_apply_shader(cur_shader);
    if (cur_shader)
        cur_shader->enabled = false;
}

static struct ShaderProgram *gfx_scegu_create_and_load_new_shader(uint32_t shader_id) {
    struct CCFeatures ccf;
    gfx_cc_get_features(shader_id, &ccf);

    struct ShaderProgram *prg = &shader_program_pool[shader_program_pool_size++];

    prg->shader_id = shader_id;
    prg->cc = ccf;
    prg->num_inputs = ccf.num_inputs;
    prg->texture_used[0] = ccf.used_textures[0];
    prg->texture_used[1] = ccf.used_textures[1];

    if (ccf.used_textures[0] && ccf.used_textures[1]) {
        prg->mix = SH_MT_TEXTURE_TEXTURE;
        if (ccf.do_single[1]) {
            prg->texture_ord[0] = 1;
            prg->texture_ord[1] = 0;
        } else {
            prg->texture_ord[0] = 0;
            prg->texture_ord[1] = 1;
        }
    } else if (ccf.used_textures[0] && ccf.num_inputs) {
        prg->mix = SH_MT_TEXTURE_COLOR;
    } else if (ccf.used_textures[0]) {
        prg->mix = SH_MT_TEXTURE;
    } else if (ccf.num_inputs > 1) {
        prg->mix = SH_MT_COLOR_COLOR;
    } else if (ccf.num_inputs) {
        prg->mix = SH_MT_COLOR;
    }

    prg->enabled = false;

    gfx_scegu_load_shader(prg);

    return prg;
}

static struct ShaderProgram *gfx_scegu_lookup_shader(uint32_t shader_id) {
    for (size_t i = 0; i < shader_program_pool_size; i++) {
        if (shader_program_pool[i].shader_id == shader_id) {
            return &shader_program_pool[i];
        }
    }
    return NULL;
}

static void gfx_scegu_shader_get_info(struct ShaderProgram *prg, uint8_t *num_inputs, bool used_textures[2]) {
    *num_inputs = prg->num_inputs;
    used_textures[0] = prg->texture_used[0];
    used_textures[1] = prg->texture_used[1];
}

static uint32_t gfx_scegu_new_texture(void) {
    return texman_create();
}

static uint32_t gfx_cm_to_opengl(uint32_t val) {
    if (val & G_TX_MIRROR)
        return GU_REPEAT;
    if (val & G_TX_CLAMP)
        return GU_CLAMP;
    return GU_REPEAT;
}

static inline int ispow2(uint32_t x) {
    return (x & (x - 1)) == 0;
}

// compute the next highest power of 2 of 32-bit v
static inline int nextpow2(int v) {
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v++;
    return v;
}

static inline void gfx_scegu_apply_tmu_state(const int tile) {
    sceGuTexFilter(tmu_state[tile].min_filter, tmu_state[tile].mag_filter);
    sceGuTexWrap(tmu_state[tile].wrap_s, tmu_state[tile].wrap_t);
}

static void gfx_scegu_set_sampler_parameters(const int tile, const bool linear_filter, const uint32_t cms, const uint32_t cmt) {
    const int filter = linear_filter ? GU_LINEAR : GU_NEAREST;

    const int wrap_s = gfx_cm_to_opengl(cms);
    const int wrap_t = gfx_cm_to_opengl(cmt);

    tmu_state[tile].min_filter = filter;
    tmu_state[tile].mag_filter = filter;
    tmu_state[tile].wrap_s = wrap_s;
    tmu_state[tile].wrap_t = wrap_t;

    // set state for the first texture right away
    if (!tile)
        gfx_scegu_apply_tmu_state(tile);
}

static void gfx_scegu_select_texture(int tile, uint32_t texture_id) {
    /* tmu_state[].tex was only updated here, but the texture uploads (texman_upload*) and my double
     * passes bind another texture without notice. If a texture equal to the LAST one recorded was requested afterwards,
     * the bind was skipped and it was drawn with another texture (e.g. the lobby mural with the wrong part).
     * Now it is also compared with the texture actually bound. */
    if (tmu_state[tile].tex != texture_id || texman_get_bound() != texture_id) {
        tmu_state[tile].tex = texture_id;
        texman_bind_tex(texture_id);
        gfx_scegu_set_sampler_parameters(tile, false, 0, 0);
    }
}

/* Used for rescaling textures ROUGHLY into pow2 dims */
/* 256 KB (256x256 RGBA32). It used to be 1 MB (the element count was multiplied by sizeof(unsigned int) a second time). The N64 texture memory holds 4 KB, so a
 * tile has at most 8192 texels and its power-of-two size at most 32768 texels (128 KB as RGBA32): this is more than enough, and gfx_scegu_upload_texture() checks
 * the size anyway. */
static unsigned int __attribute__((aligned(16))) scaled[256 * 256];
static void gfx_scegu_resample_32bit(const unsigned int *in, int inwidth, int inheight, unsigned int *out, int outwidth, int outheight) {
    int i, j;
    const unsigned int *inrow;
    unsigned int frac, fracstep;

    fracstep = inwidth * 0x10000 / outwidth;
    for (i = 0; i < outheight; i++, out += outwidth) {
        inrow = in + inwidth * (i * inheight / outheight);
        frac = fracstep >> 1;
        for (j = 0; j < outwidth; j += 4) {
            out[j] = inrow[frac >> 16];
            frac += fracstep;
            out[j + 1] = inrow[frac >> 16];
            frac += fracstep;
            out[j + 2] = inrow[frac >> 16];
            frac += fracstep;
            out[j + 3] = inrow[frac >> 16];
            frac += fracstep;
        }
    }
}

static void gfx_scegu_resample_16bit(const unsigned short *in, int inwidth, int inheight, unsigned short *out, int outwidth, int outheight) {
    int i, j;
    const unsigned short *inrow;
    unsigned int frac, fracstep;

    fracstep = inwidth * 0x10000 / outwidth;
    for (i = 0; i < outheight; i++, out += outwidth) {
        inrow = in + inwidth * (i * inheight / outheight);
        frac = fracstep >> 1;
        for (j = 0; j < outwidth; j += 4) {
            out[j] = inrow[frac >> 16];
            frac += fracstep;
            out[j + 1] = inrow[frac >> 16];
            frac += fracstep;
            out[j + 2] = inrow[frac >> 16];
            frac += fracstep;
            out[j + 3] = inrow[frac >> 16];
            frac += fracstep;
        }
    }
}

static void gfx_scegu_resample_8bit(const unsigned char *in, int inwidth, int inheight, unsigned char *out, int outwidth, int outheight) {
    int i, j;
    const unsigned char *inrow;
    unsigned int frac, fracstep;

    fracstep = inwidth * 0x10000 / outwidth;
    for (i = 0; i < outheight; i++, out += outwidth) {
        inrow = in + inwidth * (i * inheight / outheight);
        frac = fracstep >> 1;
        for (j = 0; j < outwidth; j += 4) {
            out[j] = inrow[frac >> 16];
            frac += fracstep;
            out[j + 1] = inrow[frac >> 16];
            frac += fracstep;
            out[j + 2] = inrow[frac >> 16];
            frac += fracstep;
            out[j + 3] = inrow[frac >> 16];
            frac += fracstep;
        }
    }
}

static void gfx_scegu_upload_texture(const uint8_t *rgba32_buf, int width, int height, unsigned int type) {
    if (ispow2(width) && ispow2(height)) {
        texman_upload_swizzle(width, height, type, (void *) rgba32_buf);
    } else {
        int scaled_width = nextpow2(width);
        int scaled_height = nextpow2(height);

        if ((size_t) scaled_width * (size_t) scaled_height * 4u > sizeof(scaled)) {
            return; /* does not fit the scratch buffer (never happens with N64 textures): the texture is skipped instead of overflowing the buffer */
        }

        if (type == GU_PSM_8888) {
            gfx_scegu_resample_32bit((const unsigned int *) rgba32_buf, width, height, (void *) scaled, scaled_width, scaled_height);
            texman_upload_swizzle(scaled_width, scaled_height, type, (void *) scaled);
        } else if (type == GU_PSM_5551) {
            gfx_scegu_resample_16bit((const unsigned short *) rgba32_buf, width, height, (void *) scaled, scaled_width, scaled_height);
            texman_upload_swizzle(scaled_width, scaled_height, type, (void *) scaled);
        } else {
            gfx_scegu_resample_8bit((const unsigned char *) rgba32_buf, width, height, (void *) scaled, scaled_width, scaled_height);
            texman_upload_swizzle(scaled_width, scaled_height, type, (void *) scaled);
        }
    }
}

static void gfx_scegu_set_depth_test(bool depth_test) {
    if (depth_test) {
        sceGuEnable(GU_DEPTH_TEST);
    } else {
        sceGuDisable(GU_DEPTH_TEST);
    }
}

static bool sDepthMaskOn = false; /* true = the GE does NOT write z (GU_TRUE) */
static void gfx_scegu_set_depth_mask(bool z_upd) {
    sDepthMaskOn = !z_upd;
    sceGuDepthMask(z_upd ? GU_FALSE : GU_TRUE);
}

/* Per-triangle depth offset inside decal mode (gfx_pc.c, PSP_DECAL_ADAPTIVE). */
void gfx_scegu_set_decal_bias(int bias) {
    sceGuDepthOffset(bias);
}

static void gfx_scegu_set_zmode_decal(bool zmode_decal) {
    if (zmode_decal) {
        sceGuDepthOffset(PSP_DECAL_OFFSET); /* 16-bit z-buffer on the PSP: see gfx_screen_config.h */
    } else {
        sceGuDepthOffset(0);
    }
}

static void gfx_scegu_set_viewport(int x, int y, int width, int height) {
    sceGuViewport(2048 - (SCR_WIDTH / 2) + x + (width / 2), 2048 + (SCR_HEIGHT / 2) - y - (height / 2), width, height);
    sceGuScissor(x, SCR_HEIGHT - y - height, x + width, SCR_HEIGHT - y);
}

static void gfx_scegu_set_scissor(int x, int y, int width, int height) {
    sceGuScissor(x, SCR_HEIGHT - y - height, x + width, SCR_HEIGHT - y);
}

static void gfx_scegu_set_use_alpha(bool use_alpha) {
    gl_blend = use_alpha;
    if (use_alpha) {
        sceGuEnable(GU_BLEND);
    } else {
        sceGuDisable(GU_BLEND);
    }
}

// draws the same triangles as plain fog color + fog intensity as alpha
// on top of the normal tris and blends them to achieve sort of the same effect
// as fog would
static inline void gfx_scegu_blend_fog_tris(void) {
    /* @Todo: figure this out! */
    return;
#if 0
    // if a texture was used, replace it with fog color instead, but still keep the alpha
    if (cur_shader->texture_used[0]) {
        glActiveTexture(GL_TEXTURE0);
        TEXENV_COMBINE_ON();
        // out.rgb = input0.rgb
        TEXENV_COMBINE_SET1(RGB, GL_REPLACE, GL_PRIMARY_COLOR);
        // out.a = texel0.a * input0.a
        TEXENV_COMBINE_SET2(ALPHA, GL_MODULATE, GL_TEXTURE, GL_PRIMARY_COLOR);
    }

    glEnableClientState(GL_COLOR_ARRAY); // enable color array temporarily
    glColorPointer(4, GL_FLOAT, cur_buf_stride, cur_fog_ofs); // set fog colors as primary colors
    if (!gl_blend) glEnable(GL_BLEND); // enable blending temporarily
    glDepthFunc(GL_LEQUAL); // Z is the same as the base triangles

    glDrawArrays(GL_TRIANGLES, 0, 3 * cur_buf_num_tris);

    glDepthFunc(GL_LESS); // set back to default
    if (!gl_blend) glDisable(GL_BLEND); // disable blending if it was disabled
    glDisableClientState(GL_COLOR_ARRAY); // will get reenabled later anyway
#endif
}

/* Exact N64 fog:  final_rgb = mix(combiner_rgb, fog_color, F)  (F per vertex, 0..255), alpha untouched.
 * The GE cannot do it in 1 pass, so depending on the shader type:
 * 1 = opaque:            2nd "overlay" pass (src=fog, alpha=F, normal blend), depth GEQUAL.
 * 2 = cutout (edge):   same, but depth EQUAL, so it is only painted where the 1st pass wrote z (texels that were not discarded).
 * 3 = translucent:     the 1st pass already comes out with rgb*(1-F) (gfx_pc.c does this on the vertices) and the 2nd adds
 * fog*alpha*F with additive blending: a*(C*(1-F)+Fog*F)+(1-a)*Background, same as the N64.
 * 0 = no exact fog (special texture modes): the GE's linear fog (approximate).
 * PSP_EXACT_FOG 0 disables all of this and always uses the linear one. */
#ifndef PSP_EXACT_FOG
#define PSP_EXACT_FOG 1
#endif
extern uint32_t scegu_mt_tex[2]; /* gfx_pc.c */
extern float scegu_mtx_proj[16], scegu_mtx_view[16], scegu_mtx_model[16]; /* gfx_pc.c: ultimas matrices enviadas al GE */
extern unsigned char scegu_fog_alpha[]; /* gfx_pc.c: 0..255 factor per vertex of the buffer */

int scegu_fog_mode(uint32_t sid) {
    if (!PSP_EXACT_FOG || !(sid & SHADER_OPT_FOG)) return 0;
    if (sid & SHADER_OPT_ALPHA) {
        /* only valid if the texture is MODULATED with the vertex color (the other modes ignore the color) */
        switch (sid) {
            case 0x0000038D: case 0x01045A00: case 0x01200A00: case 0x00000551: case 0x01A00045:
                return 0;
        }
        return 3;
    }
    if (sid & SHADER_OPT_TEXTURE_EDGE) return 2;
    return 1;
}

typedef struct VertexFog {
    unsigned int color;
    float x, y, z;
} VertexFog;


/* ---- N64 noise transparency (G_AC_DITHER): Mario fading out, vanish cap ----
 * The RDP compares alpha with a random 3-bit noise per pixel (31,63,...,255) and discards the pixel if
 * alpha < noise. The pixel stays opaque and writes z (no blending). Here:
 * 1) depth pass: the geometry is drawn with a 64x64 mask texture (1-bit alpha = alpha>=noise)
 * projected in screen space (1 texel = 1 pixel), with alpha test and without writing color: only
 * z is left on the pixels that "survive";
 * 2) normal pass with depth EQUAL, no blend: it is painted only where pass 1 survived.
 * The noise is re-rolled every frame. Set PSP_NOISE_STIPPLE 0 to go back to normal blending. */
#define PSP_NOISE_STIPPLE 1
#ifndef ALIGN
#define ALIGN(VAL_, ALIGNMENT_) (((VAL_) + ((ALIGNMENT_) - 1)) & ~((ALIGNMENT_) - 1))
#endif
#define NOISE_MASK_SIZE 64
static unsigned short sNoiseMask[NOISE_MASK_SIZE * NOISE_MASK_SIZE] __attribute__((aligned(16)));
static unsigned int sNoiseFrame = 0, sNoiseMaskFrame = 0xFFFFFFFFu;
static int sNoiseMaskAlpha = -1;
static unsigned int sNoiseRng = 0x2545F491u;
static unsigned int sNoiseRegen; /* times the mask was regenerated (used by the PSP_F2_REPLAY recording) */

static void gfx_scegu_noise_update_mask(int alpha) {
    if (sNoiseMaskFrame == sNoiseFrame && sNoiseMaskAlpha == alpha) return;
    sNoiseMaskFrame = sNoiseFrame;
    sNoiseMaskAlpha = alpha;
    sNoiseRegen++;
    for (int i = 0; i < NOISE_MASK_SIZE * NOISE_MASK_SIZE; i++) {
        sNoiseRng ^= sNoiseRng << 13; sNoiseRng ^= sNoiseRng >> 17; sNoiseRng ^= sNoiseRng << 5; /* xorshift32 */
        const int noise = (int)(((sNoiseRng >> 8) & 7) << 5) | 0x1f;
        sNoiseMask[i] = (alpha >= noise) ? 0xFFFF : 0x7FFF; /* bit 15 = alpha 1 bit: sobrevive */
    }
    sceKernelDcacheWritebackRange(sNoiseMask, sizeof(sNoiseMask));
}

static void gfx_scegu_mat4_mul(float *out, const float *a, const float *b) { /* out = a * b (rows, N64 convention) */
    float t[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            t[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] +
                           a[i * 4 + 2] * b[2 * 4 + j] + a[i * 4 + 3] * b[3 * 4 + j];
    memcpy(out, t, sizeof(t));
}

/* ---- GPU profile (only with the Triangle+L shortcut in the log; gfx_pc.c) ----
 * Each draw call is surrounded by "close list + wait for the GE + open list": this way the time between both points is what
 * the GE takes on that call (minus the fixed cost of restarting the list, which is measured at the start of each frame). Frames run
 * very slowly, but the per-pass times are valid. Buckets: see the log comment in gfx_pc.c. */
#define GPUP_N 11
extern int gPspGpuProf;
#if PSP_GPU_PROF
#define PSP_GPUP() (gPspGpuProf)
#else
#define PSP_GPUP() (0)
#endif
extern float gPcAreaAcc;
unsigned int gGpuUs[GPUP_N], gGpuTris[GPUP_N], gGpuPx[GPUP_N], gGpuCalls[GPUP_N];
static unsigned int sGpupT0, sGpupBase;
static int sGpupBucket = -1, sGpupOpen = 0;
static unsigned int sGpupTris, sGpupPx;

static inline unsigned int gpup_now(void) { return sceKernelGetSystemTimeLow(); }

static __attribute__((unused)) void gpup_calibrate(void) {
    unsigned int acc = 0;
    for (int i = 0; i < 8; i++) {
        const unsigned int t = gpup_now();
        gfx_scegu_list_restart();
        acc += gpup_now() - t;
    }
    sGpupBase = acc / 8;
}

static __attribute__((unused)) void gpup_begin(void) {
    const unsigned int t = gpup_now();
    gfx_scegu_list_restart(); /* waits for everything before (clear screen, 2D, ...): recorded separately */
    const unsigned int dt = gpup_now() - t;
    gGpuUs[10] += dt > sGpupBase ? dt - sGpupBase : 0;
    sGpupT0 = gpup_now();
    sGpupOpen = 1;
}

static __attribute__((unused)) void gpup_end(void) {
    if (!sGpupOpen || sGpupBucket < 0) { sGpupOpen = 0; return; }
    gfx_scegu_list_restart();
    const unsigned int dt = gpup_now() - sGpupT0;
    gGpuUs[sGpupBucket] += dt > sGpupBase ? dt - sGpupBase : 0;
    gGpuTris[sGpupBucket] += sGpupTris;
    gGpuPx[sGpupBucket] += sGpupPx;
    gGpuCalls[sGpupBucket]++;
    sGpupOpen = 0;
}

/* opens the measurement of a triangle draw call with shader `sid` */
static __attribute__((unused)) void gpup_open_tris(uint32_t sid, bool blend, size_t ntris) {
    const int cat = blend ? 2 : ((sid & SHADER_OPT_TEXTURE_EDGE) ? 1 : 0);
    sGpupBucket = cat + ((sid & SHADER_OPT_FOG) ? 3 : 0);
    sGpupTris = (unsigned int)ntris;
    sGpupPx = (unsigned int)gPcAreaAcc;
    gPcAreaAcc = 0.0f;
    gpup_begin();
}

/* first pass finished: switches to measuring the fog pass */
static __attribute__((unused)) void gpup_split(int bucket) {
    gpup_end();
    sGpupBucket = bucket;
    gpup_begin(); /* (the cost of the restart between passes goes into bucket 10) */
}

extern int scegu_enva_modulate; /* gfx_pc.c */
extern unsigned int gPcFogAll, gPcFogKeep; /* gfx_pc.c: counters for the 2nd fog pass */
static void gfx_scegu_draw_triangles_real(float buf_vbo[], UNUSED size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    if (!is_shader_enabled(cur_shader->shader_id)) {
        gfx_scegu_apply_shader(get_shader_from_id(get_shader_remap(cur_shader->shader_id)));
    }

    const uint32_t sid = cur_shader->shader_id;
    if (PSP_GPUP()) {
        gpup_open_tris(sid, gl_blend, buf_vbo_num_tris);
    }
    if ((sid & (1u << 28)) && scegu_enva_modulate) {
        /* BLENDRGBFADEA patch already drawn in a color pass: only texels with alpha (white vertex color), see gfx_pc.c */
        sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    }

    /* Peach/Bowser frame: the N64 blends 2 textures (TRILERP mipmap: T0 + lod*(T1-T0)). The GE only
     * reads one texture per pass, so T0 is drawn opaque and T1 on top with alpha = lod (the alpha of the
     * vertex color already carries the lod). */
    if ((sid & ~(SHADER_OPT_FOG | SHADER_OPT_TEXTURE_EDGE | SHADER_OPT_NOISE)) == 0x01A00A6Fu) {
        if (PSP_GPUP()) sGpupBucket = 8;
        const size_t mv = 3 * buf_vbo_num_tris;
        gfx_scegu_reserve_list_memory(sizeof(Vertex) * mv * 2 + 64);
        Vertex *b0 = (Vertex *)sceGuGetMemory(sizeof(Vertex) * mv);
        Vertex *b1 = (Vertex *)sceGuGetMemory(sizeof(Vertex) * mv);
        memcpy_vfpu(b1, buf_vbo, sizeof(Vertex) * mv);
        memcpy_vfpu(b0, buf_vbo, sizeof(Vertex) * mv);
        for (size_t k = 0; k < mv; k++) b0[k].color = 0xFFFFFFFFu;

        sceGuDisable(GU_FOG);
        sceGuDisable(GU_ALPHA_TEST);
        sceGuEnable(GU_TEXTURE_2D);
        sceGuTexFunc(GU_TFX_DECAL, GU_TCC_RGBA);

        sceGuDisable(GU_BLEND);
        texman_bind_tex(scegu_mt_tex[0]);
        sceGuDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, mv, 0, b0);

        sceGuEnable(GU_BLEND);
        sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
        sceGuDepthMask(GU_TRUE);              /* same z as the 1st pass: GEQUAL lets it through */
        texman_bind_tex(scegu_mt_tex[1]);
        sceGuDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, mv, 0, b1);

        sceGuDepthMask(sDepthMaskOn ? GU_TRUE : GU_FALSE);
        if (!gl_blend) sceGuDisable(GU_BLEND);
        return;
    }


#if PSP_NOISE_STIPPLE
    if (sid & SHADER_OPT_NOISE) {
        if (PSP_GPUP()) sGpupBucket = 8;
        const size_t mv = 3 * buf_vbo_num_tris;
        gfx_scegu_reserve_list_memory(sizeof(Vertex) * mv * 2 + 128);
        Vertex *b0 = (Vertex *)sceGuGetMemory(sizeof(Vertex) * mv);
        Vertex *b1 = (Vertex *)sceGuGetMemory(sizeof(Vertex) * mv);
        memcpy_vfpu(b1, buf_vbo, sizeof(Vertex) * mv);
        memcpy_vfpu(b0, buf_vbo, sizeof(Vertex) * mv);
        const int alpha = (int)(b1[0].color >> 24);
        for (size_t k = 0; k < mv; k++) b0[k].color = 0xFFFFFFFFu;
        gfx_scegu_noise_update_mask(alpha);

        /* texture matrix: [pos,1] * (M*V*P) -> clip, and from clip to (s,t,q) in mask texels (1 texel = 1 pixel) */
        float F[16], X[16], T[16];
        gfx_scegu_mat4_mul(F, scegu_mtx_model, scegu_mtx_view);
        gfx_scegu_mat4_mul(F, F, scegu_mtx_proj);
        memset(X, 0, sizeof(X));
        const float ks = (float)SCR_WIDTH / NOISE_MASK_SIZE, kt = (float)SCR_HEIGHT / NOISE_MASK_SIZE;
        X[0 * 4 + 0] = 0.5f * ks; X[3 * 4 + 0] = 0.5f * ks;          /* s = ks*(cx+cw)/2 */
        X[1 * 4 + 1] = -0.5f * kt; X[3 * 4 + 1] = 0.5f * kt;         /* t = kt*(cw-cy)/2 */
        X[3 * 4 + 2] = 1.0f;                                         /* q = cw */
        X[2 * 4 + 3] = 0.0f; X[3 * 4 + 3] = 1.0f;
        gfx_scegu_mat4_mul(T, F, X);
        ScePspFMatrix4 *tm = (ScePspFMatrix4 *)ALIGN((unsigned int)sceGuGetMemory(sizeof(ScePspFMatrix4) + 15), 16);
        memcpy(tm, T, sizeof(T));

        /* 1) depth pass with the mask */
        sceGuDisable(GU_FOG);
        sceGuDisable(GU_BLEND);
        sceGuEnable(GU_TEXTURE_2D);
        sceGuTexMode(GU_PSM_5551, 0, 0, 0);
        sceGuTexImage(0, NOISE_MASK_SIZE, NOISE_MASK_SIZE, NOISE_MASK_SIZE, sNoiseMask);
        sceGuTexFilter(GU_NEAREST, GU_NEAREST);
        sceGuTexWrap(GU_REPEAT, GU_REPEAT);
        sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
        sceGuEnable(GU_ALPHA_TEST);
        sceGuAlphaFunc(GU_GREATER, 0, 0xff);
        sceGuSetMatrix(GU_TEXTURE, tm);
        sceGuTexMapMode(GU_TEXTURE_MATRIX, 0, 0);
        sceGuTexProjMapMode(GU_POSITION);
        sceGuPixelMask(0xFFFFFFFF);
        sceGuDepthMask(GU_FALSE);
        sceGuDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, mv, 0, b0);

        /* restore texture/color state */
        sceGuPixelMask(0);
        sceGuTexMapMode(GU_TEXTURE_COORDS, 0, 0);
        texman_bind_tex(texman_get_bound());
        gfx_scegu_apply_tmu_state(0);
        sceGuTexFunc(cur_shader->texfunc, GU_TCC_RGBA);
        if (cur_shader->texture_used[0] || cur_shader->texture_used[1]) sceGuEnable(GU_TEXTURE_2D); else sceGuDisable(GU_TEXTURE_2D);
        if (sid & SHADER_OPT_TEXTURE_EDGE) { sceGuEnable(GU_ALPHA_TEST); sceGuAlphaFunc(GU_GREATER, 0x55, 0xff); }
        else sceGuDisable(GU_ALPHA_TEST);

        /* 2) normal pass, only where the 1st survived: depth EQUAL, no blend, no z write */
        sceGuDepthMask(GU_TRUE);
        sceGuDepthFunc(GU_EQUAL);
        if (cur_shader->texture_used[0] || cur_shader->texture_used[1]) {
            /* cutout textures (cap wings...): the noise mask only knows the vertex alpha; the texture's
             * alpha is applied here with alpha test (no blending). Vertex alpha at 255 so that only the texture's counts. */
            for (size_t k = 0; k < mv; k++) b1[k].color |= 0xFF000000u;
            if (!(sid & SHADER_OPT_TEXTURE_EDGE)) { sceGuEnable(GU_ALPHA_TEST); sceGuAlphaFunc(GU_GREATER, 0x55, 0xff); }
        }
        sceGuDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, mv, 0, b1);
        sceGuDepthFunc(GU_GEQUAL);
        sceGuDepthMask(sDepthMaskOn ? GU_TRUE : GU_FALSE);
        if (!(sid & SHADER_OPT_TEXTURE_EDGE)) sceGuDisable(GU_ALPHA_TEST); /* the texture alpha test of the 2nd pass must not leak */
        if (gl_blend) sceGuEnable(GU_BLEND);
        return;
    }
#endif
    const bool fogged = (sid & SHADER_OPT_FOG) != 0;
    const int fogmode = scegu_fog_mode(sid);
    if (fogged && fogmode == 0) {
        /* GE linear fog (approximation with near/far computed in gfx_pc.c). */
        sceGuEnable(GU_FOG);
        if (scegu_fog_color != sGuFogColor || scegu_fog_near != sGuFogNear || scegu_fog_far != sGuFogFar) {
            sGuFogColor = scegu_fog_color;
            sGuFogNear = scegu_fog_near; sGuFogFar = scegu_fog_far;
            sceGuFog(sGuFogNear, sGuFogFar, sGuFogColor);
        }
    } else {
        sceGuDisable(GU_FOG);
    }

    const size_t nverts = 3 * buf_vbo_num_tris;
#if PSP_VERTEX_ARENA
    /* only commands go in the list; the vertices (1st pass and 2nd fog pass) go to the cached arena */
    gfx_scegu_reserve_list_memory(64);
    gfx_scegu_varena_ensure(sizeof(Vertex) * nverts * (fogmode ? 2 : 1));
    void *buf = gfx_scegu_varena_alloc(sizeof(Vertex) * nverts);
    memcpy_vfpu(buf, buf_vbo, sizeof(Vertex) * nverts);
#if !PSP_WB_DEFER
    sceKernelDcacheWritebackRange(buf, sizeof(Vertex) * nverts);
#endif
#else
    gfx_scegu_reserve_list_memory(sizeof(Vertex) * nverts * (fogmode ? 2 : 1) + 64);
    void *buf = sceGuGetMemory(sizeof(Vertex) * nverts);
    memcpy_vfpu(buf, buf_vbo, sizeof(Vertex) * nverts);
#endif
    gfx_scegu_prim_tris(GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, (int)nverts, buf);
    if (PSP_GPUP() && fogmode) {
        const unsigned int px = sGpupPx;
        gpup_split(fogmode == 3 ? 7 : 6);
        sGpupPx = px;
    }

    if (fogmode) {
        /* The 2nd pass (fog color with alpha = F per vertex) changes NOTHING on a triangle whose 3 vertices have F = 0:
         * alpha 0 -> the blend leaves the background as is (and in mode 3 fog * (alpha*F) = 0 is added). Those triangles (the close
         * geometry, before the fog starts) are skipped: less vertex copying on the CPU and less work for the GE.
         * The result on screen is identical. */
        const size_t ntris_all = buf_vbo_num_tris;
        size_t keep_tris = 0;
        for (size_t t = 0; t < ntris_all; t++) {
            if (scegu_fog_alpha[3 * t] | scegu_fog_alpha[3 * t + 1] | scegu_fog_alpha[3 * t + 2]) {
                keep_tris++;
            }
        }
        gPcFogAll += (unsigned int)ntris_all;
        gPcFogKeep += (unsigned int)keep_tris;
        if (PSP_GPUP() && ntris_all) {
            sGpupPx = (unsigned int)((unsigned long long)sGpupPx * keep_tris / ntris_all);
            sGpupTris = (unsigned int)keep_tris;
        }
        if (keep_tris == 0) {
            return;
        }
        const size_t fnverts = 3 * keep_tris;
        const Vertex *src = (const Vertex *)buf_vbo;
        const bool textured = cur_shader->texture_used[0] || cur_shader->texture_used[1];
        const bool tex_alpha_pass = (fogmode == 3) && textured; /* texture alpha: GU_TFX_BLEND with Cf=Cc=fog */
        const bool depth_equal = (fogmode == 2);
        const unsigned int rgb = scegu_fog_color & 0x00FFFFFFu;

        void *fbuf;
        if (tex_alpha_pass) {
#if PSP_VERTEX_ARENA
            Vertex *fvt = (Vertex *)gfx_scegu_varena_alloc(sizeof(Vertex) * fnverts);
#else
            Vertex *fvt = (Vertex *)sceGuGetMemory(sizeof(Vertex) * fnverts);
#endif
            size_t o = 0;
            for (size_t t = 0; t < ntris_all; t++) {
                const size_t k0 = 3 * t;
                if (!(scegu_fog_alpha[k0] | scegu_fog_alpha[k0 + 1] | scegu_fog_alpha[k0 + 2])) continue;
                for (size_t k = k0; k < k0 + 3; k++, o++) {
                    unsigned int a = (scegu_fog_alpha[k] * (src[k].color >> 24)) / 255; /* vertex_alpha * F */
                    fvt[o].u = src[k].u; fvt[o].v = src[k].v;
                    fvt[o].color = (a << 24) | rgb;
                    fvt[o].x = src[k].x; fvt[o].y = src[k].y; fvt[o].z = src[k].z;
                }
            }
#if PSP_VERTEX_ARENA && !PSP_WB_DEFER
            sceKernelDcacheWritebackRange(fvt, sizeof(Vertex) * fnverts);
#endif
            fbuf = fvt;
        } else {
#if PSP_VERTEX_ARENA
            VertexFog *fv = (VertexFog *)gfx_scegu_varena_alloc(sizeof(VertexFog) * fnverts);
#else
            VertexFog *fv = (VertexFog *)sceGuGetMemory(sizeof(VertexFog) * fnverts);
#endif
            size_t o = 0;
            for (size_t t = 0; t < ntris_all; t++) {
                const size_t k0 = 3 * t;
                if (!(scegu_fog_alpha[k0] | scegu_fog_alpha[k0 + 1] | scegu_fog_alpha[k0 + 2])) continue;
                for (size_t k = k0; k < k0 + 3; k++, o++) {
                    unsigned int a = scegu_fog_alpha[k];
                    if (fogmode == 3) a = (a * (src[k].color >> 24)) / 255;
                    fv[o].color = (a << 24) | rgb;
                    fv[o].x = src[k].x; fv[o].y = src[k].y; fv[o].z = src[k].z;
                }
            }
#if PSP_VERTEX_ARENA && !PSP_WB_DEFER
            sceKernelDcacheWritebackRange(fv, sizeof(VertexFog) * fnverts);
#endif
            fbuf = fv;
        }

        if (tex_alpha_pass) {
            sceGuTexEnvColor(scegu_fog_color);
            sceGuTexFunc(GU_TFX_BLEND, GU_TCC_RGBA);
            sceGuEnable(GU_TEXTURE_2D);
        } else {
            sceGuDisable(GU_TEXTURE_2D);
        }
        sceGuEnable(GU_BLEND);
        if (fogmode == 3) {
            sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_FIX, 0, 0xFFFFFFFF); /* dest += fog * (alpha*F) */
        } else {
            sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
        }
        sceGuAlphaFunc(GU_GREATER, 0, 0xff);
        sceGuDepthMask(GU_TRUE);
        if (depth_equal) sceGuDepthFunc(GU_EQUAL);

        gfx_scegu_prim_tris((tex_alpha_pass ? GU_TEXTURE_32BITF : 0) | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D,
                            (int)fnverts, fbuf);

        /* restore the state of the 1st pass */
        if (depth_equal) sceGuDepthFunc(GU_GEQUAL);
        sceGuDepthMask(sDepthMaskOn ? GU_TRUE : GU_FALSE);
        sceGuAlphaFunc(GU_GREATER, 0x55, 0xff);
        sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
        if (!gl_blend) sceGuDisable(GU_BLEND);
        if (tex_alpha_pass) {
            sceGuTexEnvColor(0xffffffff);
            sceGuTexFunc(cur_shader->texfunc, GU_TCC_RGBA);
        }
        if (textured) sceGuEnable(GU_TEXTURE_2D); else sceGuDisable(GU_TEXTURE_2D);
    }
}

static void gfx_scegu_draw_triangles_impl(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    if (!PSP_GPUP()) {
        gfx_scegu_draw_triangles_real(buf_vbo, buf_vbo_len, buf_vbo_num_tris);
        return;
    }
    sGpupOpen = 0;
    sGpupBucket = -1;
    gfx_scegu_draw_triangles_real(buf_vbo, buf_vbo_len, buf_vbo_num_tris);
    gpup_end(); /* close the measurement that would be left open (last pass) */
}

static void gfx_scegu_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    gfx_scegu_draw_triangles_impl(buf_vbo, buf_vbo_len, buf_vbo_num_tris);
    if (cur_shader && (cur_shader->shader_id & (1u << 28)) && scegu_enva_modulate) {
        sceGuTexFunc(cur_shader->texfunc, GU_TCC_RGBA); /* restore the shader mode */
    }
}

void gfx_scegu_draw_triangles_2d(float buf_vbo[], UNUSED size_t buf_vbo_len, UNUSED size_t buf_vbo_num_tris) {
    if (!is_shader_enabled(cur_shader->shader_id)) {
        gfx_scegu_apply_shader(get_shader_from_id(get_shader_remap(cur_shader->shader_id)));
    }

    gfx_scegu_reserve_list_memory(sizeof(VertexColor) * 2);
    void *quad_buf = sceGuGetMemory(sizeof(VertexColor) * 2);
    memcpy(quad_buf, buf_vbo, sizeof(VertexColor) * 2);
    if (PSP_GPUP()) {
        sGpupBucket = 9; sGpupTris = 1; sGpupPx = 0;
        gpup_begin();
    }
    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, quad_buf);
    if (PSP_GPUP()) {
        gpup_end();
    }
}

/* 2D quad (4-vertex strip) for G_TEXRECTFLIP; see gfx_sp_quad_flip_2d() in gfx_pc.c */
void gfx_scegu_draw_quad_2d(float buf_vbo[]) {
    if (!is_shader_enabled(cur_shader->shader_id)) {
        gfx_scegu_apply_shader(get_shader_from_id(get_shader_remap(cur_shader->shader_id)));
    }

    gfx_scegu_reserve_list_memory(sizeof(VertexColor) * 4);
    void *quad_buf = sceGuGetMemory(sizeof(VertexColor) * 4);
    memcpy(quad_buf, buf_vbo, sizeof(VertexColor) * 4);
    if (PSP_GPUP()) {
        sGpupBucket = 9; sGpupTris = 2; sGpupPx = 0;
        gpup_begin();
    }
    sceGuDrawArray(GU_TRIANGLE_STRIP, GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 4, 0, quad_buf);
    if (PSP_GPUP()) {
        gpup_end();
    }
}

#ifndef PSP_ASYNC_PRESENT
#define PSP_ASYNC_PRESENT 1
#endif
#if PSP_ASYNC_PRESENT
static void gfx_scegu_present_init(void);
#endif

/* Width (in pixels of the 480x272 buffer) of the image the game draws. On the console screen and with the TV in 16:9 it is the full width (480, widescreen view);
 * with the TV in 4:3 the game is drawn with the original 4:3 view: 362x272, stuck to the left of the buffer, and the TV pass stretches it to the TV's 4:3 frame. */
int gPspGameW = SCR_WIDTH;
#define GAME_W_43 362
static void gfx_scegu_update_game_dims(void) {
    gPspGameW = (sTvOn && !gPspOptTvAspect) ? GAME_W_43 : SCR_WIDTH;
    gfx_current_dimensions.width = (uint32_t) gPspGameW; /* gfx_pc.c and the game (GFX_DIMENSIONS_*) read this every frame */
    gfx_current_dimensions.height = SCR_HEIGHT;
    gfx_current_dimensions.aspect_ratio = (float) gPspGameW / (float) SCR_HEIGHT;
}

/* With the TV the HOME button does nothing: the firmware's exit dialog comes out garbled and leaving the game with the TV on fails.
 * To exit, turn off the TV output in the options menu (with the console screen HOME is back to normal). */
static void gfx_scegu_tv_block_home(int block) {
    (void) sceImposeSetHomePopup(block ? 0 : 1);
}

/* "TV Width" / "TV Height": the image takes that percentage of the signal frame's width and height, centered (the rest stays black: some TVs crop the edges, and not
 * always the same horizontally as vertically; with the two different, a slightly squashed image is also corrected). Even width and height multiple of 4 (480i / 240p split the lines into two fields and the first field starts on an even line). */
static int sTvBytes; /* size of each TV buffer (to clear them when the area changes without redoing everything) */
static void gfx_scegu_tv_area_calc(void) {
    int px = gPspOptTvArea, py = gPspOptTvAreaY;
    px = (px < 50) ? 50 : ((px > 100) ? 100 : px);
    py = (py < 50) ? 50 : ((py > 100) ? 100 : py);
    sTvDw = (sTvW * px / 100) & ~1;
    sTvDh = (sTvH * py / 100) & ~3;
    sTvDx = (sTvW - sTvDw) / 2;
    {   /* vertical offset (in lines of a 480 frame; in 240p it is 240 lines, half): the image cannot leave the frame, so margin is needed (lower TV Height) */
        int dy = (sTvH - sTvDh) / 2 + gPspOptTvShiftY * sTvH / 480;
        const int maxdy = sTvH - sTvDh;
        dy = (dy < 0) ? 0 : ((dy > maxdy) ? maxdy : dy);
        sTvDy = dy & ~1;
    }
}

/* Waits for the next vblank, but gives up after timeout_us (0 = a vblank came, -1 = none). sceDisplayWaitVblankStart has no timeout, so the wait is done by a
 * helper thread and this thread waits on a semaphore with a timeout. If the system never delivers a vblank (the output was just taken away) the helper stays
 * blocked (one idle thread) and the game keeps running instead of hanging. Only for rare paths (TV on / re-routing); the normal presentation does not use it. */
static SceUID sVblReq = -1, sVblDone = -1, sVblThread = -1;
static volatile int sVblBusy;

static int gfx_scegu_vbl_helper(SceSize args, void *argp) {
    (void) args;
    (void) argp;
    for (;;) {
        if (sceKernelWaitSema(sVblReq, 1, NULL) < 0) {
            break;
        }
        sceDisplayWaitVblankStart();
        sVblBusy = 0;
        sceKernelSignalSema(sVblDone, 1);
    }
    return 0;
}

static int gfx_scegu_wait_vblank_to(unsigned int timeout_us) {
    SceUInt timeout = timeout_us;
    if (sVblThread < 0) {
        sVblReq = sceKernelCreateSema("sm64_vbl_req", 0, 0, 1, NULL);
        sVblDone = sceKernelCreateSema("sm64_vbl_done", 0, 0, 8, NULL);
        if (sVblReq >= 0 && sVblDone >= 0) {
            sVblThread = sceKernelCreateThread("sm64_vbl_wait", gfx_scegu_vbl_helper, 0x1D, 0x1000, PSP_THREAD_ATTR_USER, NULL);
            if (sVblThread >= 0 && sceKernelStartThread(sVblThread, 0, NULL) < 0) {
                sceKernelDeleteThread(sVblThread);
                sVblThread = -1;
            }
        }
        if (sVblThread < 0) { /* no helper: the plain wait (as before) */
            sceDisplayWaitVblankStart();
            return 0;
        }
    }
    if (sVblBusy) {
        if (sceKernelPollSema(sVblDone, 1) < 0) {
            return -1; /* the previous wait is still blocked: the system is not delivering vblanks */
        }
        sVblBusy = 0; /* it finished late */
    }
    while (sceKernelPollSema(sVblDone, 1) >= 0) { /* stale completions */
    }
    sVblBusy = 1;
    sceKernelSignalSema(sVblReq, 1);
    if (sceKernelWaitSema(sVblDone, 1, &timeout) < 0) {
        return -1;
    }
    return 0;
}

/* Puts the TV on screen with the buffers of the "pool" reserved at boot (the video output must already be routed). 0 = ok, <0 = could not. */
static int gfx_scegu_tv_setup(int mode) {
    const int hi = 1; /* all modes use the 720x480 signal frame (240p too: see psp_dve_set_video_out) */
    const int interlaced = (mode == PSP_TV_480I || mode == PSP_TV_240P);
    const unsigned int rows = (unsigned int) (interlaced ? 503 : (hi ? 480 : 240)); /* 480i: two 240-line fields inside a 503 buffer */
    const unsigned int bytes = (unsigned int) (hi ? 768 : 512) * rows * 4u;
    const int n = hi ? 1 : 3; /* 480i / 480p: a single buffer (another does not fit in 4 MB); 240p: three, like the LCD */
    int i;

    if (sTvPool == NULL || bytes * (unsigned int) n > TV_POOL_BYTES) {
        return -1;
    }
    sTvW = hi ? 720 : 320;
    sTvH = hi ? 480 : 240;
    sTvStride = hi ? 768 : 512;
    sTvN = n;
    sTvMode = mode;
    /* 16:9: the widescreen image fills the whole frame (the TV stretches it). 4:3: original 4:3 view; at 480 it is left at 640 of the 720 pixels of width (square pixels:
     * black bars at the sides), at 240p it fills the 320x240 frame. */
    sTvBytes = bytes;
    gfx_scegu_tv_area_calc();
    for (i = 0; i < n; i++) {
        sTvFb[i] = (void *) ((unsigned int) sTvPool + bytes * (unsigned int) i);
        memset((void *) ((unsigned int) sceGeEdramGetAddr() + 0x40000000u + ((unsigned int) sTvFb[i] & 0x00FFFFFFu)), 0, bytes); /* black (and the bars stay that way) */
    }
    /* NOTE: sceDisplaySetMode is not called here: the DVE path (pspDveMgrSetVideoOut) already leaves the display mode with the TV's own code (0x1D1 / 0x1D2) and
     * sceDisplaySetMode(0, ...) changed it to mode 0 (the TV lost the signal at 480p and 240p came out doubled), same as the Zelda port does. */
    sceDisplaySetFrameBuf((void *) ((unsigned int) sceGeEdramGetAddr() + ((unsigned int) sTvFb[0] & 0x00FFFFFFu) + 0x40000000u), sTvStride,
                          GU_PSM_8888, PSP_DISPLAY_SETBUF_NEXTFRAME);
    sTvOn = 1;
    sTvCableGone = 0;
    gfx_scegu_update_game_dims();
    gfx_scegu_tv_block_home(1);
    if (sTvKeepVbl && sTvVblUs > 0) {
        sTvPaceT = sceKernelGetSystemTimeLow(); /* re-route to the same mode: the measured period is kept (it saves ~9 vblanks without a picture) */
    } else {   /* real vblank period in this mode: in the interlaced modes the console counts it per frame (~30 Hz) and the game would stay at 30 frames */
        unsigned int t0 = 0, t1 = 0;
        int ok;
        PSP_WD_PHASE("TV setup: measuring the vblank period");
        /* bounded waits: if the system never delivers vblanks (output taken away right now) the game must not hang here forever */
        ok = (gfx_scegu_wait_vblank_to(250000u) == 0);
        if (ok) {
            t0 = sceKernelGetSystemTimeLow();
            for (i = 0; i < 8 && ok; i++) {
                ok = (gfx_scegu_wait_vblank_to(250000u) == 0);
            }
            t1 = sceKernelGetSystemTimeLow();
        }
        if (ok) {
            sTvVblUs = (t1 - t0) / 8u;
            sTvSlowVbl = (sTvVblUs > 25000u);
            sTvPaceT = t1;
        } else {
            sTvVblUs = 16683u; /* no vblank seen: assume 60 Hz, the cable watcher / output watcher re-route again if the mode is wrong */
            sTvSlowVbl = 0;
            sTvPaceT = sceKernelGetSystemTimeLow();
        }
    }
    {
        int dm = 0, dw = 0, dh = 0;
        sTvDispMode = (sceDisplayGetMode(&dm, &dw, &dh) >= 0 && dm != 0) ? dm : -1;
        sTvDispW = dw;
        sTvDispH = dh;
    }
    sTvKeepVbl = 0;
    return 0;
}

/* Goes back to the console screen. */
static void gfx_scegu_tv_to_lcd(void) {
    (void) psp_dve_set_video_out(0);
    sceDisplaySetMode(0, SCR_WIDTH, SCR_HEIGHT);
    /* as the Zelda port does when changing mode: a console color buffer is put back immediately (the presentation thread puts the
     * next one as soon as a frame arrives) */
    sceDisplaySetFrameBuf((void *) ((unsigned int) sceGeEdramGetAddr() + ((unsigned int) sFb[0] & 0x00FFFFFFu) + 0x40000000u), BUF_WIDTH, GU_PSM_5650,
                          PSP_DISPLAY_SETBUF_NEXTFRAME);
    sTvOn = 0;
    sTvCableGone = 0;
    sTvSlowVbl = 0;
    gPspTvActive = 0;
    gfx_scegu_update_game_dims();
    gfx_scegu_tv_block_home(0);
}

static void gfx_scegu_init(void) {
    int tv = gPspTvActive; /* decided by psp_dve_boot_mode() at boot (0 = console screen) */
    sceGuInit();
    /* Order used by Daedalus: first the video output is routed, then the GU buffers are set up. */
    if (tv && psp_dve_set_video_out(tv) < 0) {
        tv = 0;
        gPspTvActive = 0;
    }
    if (!tv) {
        psp_dve_reset_lcd_if_tv();
    }

    void *fbp0 = getStaticVramBuffer(BUF_WIDTH, SCR_HEIGHT, GU_PSM_5650);
    void *fbp1 = getStaticVramBuffer(BUF_WIDTH, SCR_HEIGHT, GU_PSM_5650);
    void *zbp = getStaticVramBuffer(BUF_WIDTH, SCR_HEIGHT, GU_PSM_4444);
    void *fbp2 = getStaticVramBuffer(BUF_WIDTH, SCR_HEIGHT, GU_PSM_5650); /* 3rd color buffer (see "Presentation" below) */

    sFb[0] = fbp0;
    sFb[1] = fbp1;
    sFb[2] = fbp2;
    sDrawBuffer = fbp0;
    sDisplayBuffer = fbp2; /* the one shown at boot; frame 0 is drawn in fbp0, 1 in fbp1 and 2 in fbp2 (when 0 is already on screen) */

    sceGuStart(GU_DIRECT, list);
    sceGuDrawBuffer(GU_PSM_5650, fbp0, BUF_WIDTH);
    sceGuDispBuffer(SCR_WIDTH, SCR_HEIGHT, fbp2, BUF_WIDTH);
    sceGuDepthBuffer(zbp, BUF_WIDTH);
    sceGuOffset(2048 - (SCR_WIDTH / 2), 2048 - (SCR_HEIGHT / 2));
    sceGuViewport(2048 - (SCR_WIDTH / 2), 2048 - (SCR_HEIGHT / 2), SCR_WIDTH, SCR_HEIGHT);
    sceGuDepthRange(0xffff, 0);
    sceGuScissor(0, 0, SCR_WIDTH, SCR_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuDepthFunc(GU_GEQUAL);
    sceGuShadeModel(GU_SMOOTH);
    sceGuEnable(GU_CLIP_PLANES);
    sceGuEnable(GU_ALPHA_TEST);
    sceGuAlphaFunc(GU_GREATER, 0x55, 0xff); /* 0.3f  */
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuDisable(GU_LIGHTING);
    sceGuDisable(GU_BLEND);
    sceGuDisable(GU_CULL_FACE);
    sceGuFrontFace(GU_CCW);
    sceGuDepthMask(GU_FALSE);
    sceGuTexEnvColor(0xffffffff);
    sceGuTexOffset(0.0f, 0.0f);
    sceGuTexWrap(GU_REPEAT, GU_REPEAT);

    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);

    /* The texture store must fit in the VRAM that is really there (2 MB, or 4 MB when it was requested at boot): if it does not, it is shrunk to what is left
     * instead of running past the end of the VRAM. */
    unsigned int texman_size = TEXMAN_BUFFER_SIZE;
    {
        const unsigned int vram_size = (unsigned int) sceGeEdramGetSize();
        if (vram_size != 0 && staticOffset + texman_size + TEX_ALIGNMENT > vram_size) {
            texman_size = (vram_size > staticOffset + TEX_ALIGNMENT) ? (vram_size - staticOffset - TEX_ALIGNMENT) : 0;
        }
    }
    void *texman_buffer = texman_size >= (64u * 1024u) ? getStaticVramBufferBytes(texman_size) : NULL;
    void *texman_aligned = (void *) ((((unsigned int) texman_buffer + TEX_ALIGNMENT - 1) / TEX_ALIGNMENT) * TEX_ALIGNMENT);
    if (texman_buffer) {
        texman_reset(texman_aligned, texman_size);
    }
    if (!texman_buffer) {
        char msg[32];
        sprintf(msg, "OUT OF MEMORY!\n");
        sceIoWrite(1, msg, strlen(msg));

        sceKernelExitGame();
    }

    /* With 4 MB of VRAM (requested at boot, see psp_dve.c) the TV buffers are reserved NOW, after the textures: this way the TV can be turned on and
     * off from the menu without touching VRAM. With the TV off that area is not used for anything and the rest of the layout is the usual one. */
    if (psp_dve_tv_ready() && staticOffset + TV_POOL_BYTES <= TV_VRAM_LIMIT) {
        sTvPool = (void *) ((unsigned int) getStaticVramBuffer(768, 503, GU_PSM_8888));
    }
    if (tv && sTvPool != NULL) {
        /* sceGuDispBuffer (above) sets display mode 0 / 480x272 on top of the one the TV path left: the TV stayed with the encoder active but reading the
         * buffer with the console's shape ("glitchy" image when the game boots with the TV already on). It is routed again right before setting the TV buffer, same as
         * when turning it on from the menu (the sequence that has been seen to work). */
        if (psp_dve_set_video_out(tv) < 0) {
            tv = 0;
            gPspTvActive = 0;
            gfx_scegu_tv_to_lcd();
            psp_dve_mark_clear();
        }
    }
    if (tv && gfx_scegu_tv_setup(tv) < 0) {
        gfx_scegu_tv_to_lcd();
        psp_dve_mark_clear();
    }

    /* Load the firmware font before game assets consume the remaining heap. */
    gfx_scegu_ensure_home_menu_font();
#if PSP_ASYNC_PRESENT
    gfx_scegu_present_init();
#endif
}

/* ---- Record / replay the scene (PSP_F2_REPLAY) ----
 * Frame 1 (role 1, called by gfx_pc.c): after the preamble, it notes where the scene starts in list A (rec_begin) and where it
 * ends (rec_end). That part of the list is only GE commands and pointers to memory that does not change until the next tick
 * (vertex arena, textures in VRAM, data put in the list itself with sceGuGetMemory).
 * Frame 2 (role 2): replay_ready() decides whether it can be replayed and changes the FINISH/END that ended list A into a RET; then
 * start_frame opens list B and replay_scene() does a CALL to the scene. The GE executes exactly the same orders as in frame 1.
 * Counters (per window, printed by gfx_pc.c): see gPspRep*. */
unsigned int gPspRepOk, gPspRepNoRec, gPspRepRst, gPspRepTex, gPspRepBad, gPspRepNoise, gPspRepOff;
extern unsigned int gPspTexClears; /* psp_texture_manager.c */
static int sReplayArmed;           /* 1 = the frame that is starting uses list B and replays the scene */
#if PSP_F2_REPLAY
static int sRecState;              /* 0 = nothing recorded, 1 = recording, 2 = valid recording */
static unsigned int sRecBegin, sRecEnd, sRecGen, sRecTexGen, sRecNoise;
static int sRepDisabled;           /* set if the list check fails several times in a row */
static int sRepBadStreak;

#define GE_CMD_RET 0x0B000000u
#define GE_CMD_END 0x0C000000u
#define GE_CMD_FINISH 0x0F000000u
#endif

void gfx_scegu_rec_reset(void) {
#if PSP_F2_REPLAY
    sRecState = 0;
#endif
}

/* State of this backend that the recorded scene must NOT take for granted (the renderer's is in gfx_pc.c). */
static __attribute__((unused)) void gfx_scegu_state_invalidate(void) {
    sGuFogColor = ~scegu_fog_color;
    sGuFogNear = __builtin_nanf(""); /* NaN: any != comparison is true, so the 1st fog sends sceGuFog */
    sGuFogFar = __builtin_nanf("");
    tmu_state[0].tex = UINT32_MAX;
    tmu_state[1].tex = UINT32_MAX;
}

/* Frame 1, right before walking the game's list. Returns 1 if it records. */
int gfx_scegu_rec_begin(void) {
#if PSP_F2_REPLAY
    if (sReplayArmed) {
        sRecState = 0;
        return 0;
    }
    gfx_scegu_state_invalidate();
    sRecBegin = (unsigned int) sceGuCheckList();
    sRecGen = sListGen;
    sRecTexGen = gPspTexClears;
    sRecNoise = sNoiseRegen;
    sRecState = 1;
    return 1;
#else
    return 0;
#endif
}

/* Frame 1, right after the scene (before end_frame, which writes FINISH and END). */
void gfx_scegu_rec_end(void) {
#if PSP_F2_REPLAY
    if (sRecState != 1) {
        return;
    }
    sRecEnd = (unsigned int) sceGuCheckList();
    sRecState = 2;
#endif
}

/* Frame 2, before opening the list: 1 if the scene of frame 1 can be replayed as is. */
int gfx_scegu_replay_ready(void) {
#if PSP_F2_REPLAY
    volatile unsigned int *lu;
    if (sRepDisabled) {
        gPspRepOff++;
        return 0;
    }
    if (sRecState != 2) {
        gPspRepNoRec++;
        return 0;
    }
    if (sListGen != sRecGen) {
        gPspRepRst++; /* the list was restarted (full / arena full): the recorded scene is no longer complete */
        return 0;
    }
    if (gPspTexClears != sRecTexGen) {
        gPspRepTex++; /* the texture store was emptied during frame 1 */
        return 0;
    }
    if (sNoiseRegen - sRecNoise > 1U) {
        gPspRepNoise++; /* several different noise masks in one frame: the list only keeps the last one */
        return 0;
    }
    if (sRecEnd < sRecBegin || sRecEnd + 8U > sizeof(list) || ((sRecBegin | sRecEnd) & 3U) != 0U) {
        gPspRepBad++;
        return 0;
    }
    /* Check: end_frame of frame 1 closed the list with FINISH(0) and END right where the scene ended. If not, the
     * position count is not what I think it is and it is NOT replayed (three failures in a row disable the feature). */
    lu = (volatile unsigned int *) ((unsigned int) list | 0x40000000U);
    if (lu[sRecEnd >> 2] != GE_CMD_FINISH || lu[(sRecEnd >> 2) + 1] != GE_CMD_END) {
        gPspRepBad++;
        if (++sRepBadStreak >= 3) {
            sRepDisabled = 1;
        }
        return 0;
    }
    sRepBadStreak = 0;
    lu[sRecEnd >> 2] = GE_CMD_RET; /* the scene now ends in RET: it returns to the list that called it */
    return 1;
#else
    return 0;
#endif
}

/* gfx_run, before gfx_rapi->start_frame(): on = 1 -> this frame uses list B and replays the scene; 0 -> the usual list A. */
void gfx_scegu_replay_arm(int on) {
#if PSP_F2_REPLAY
    if (on) {
        sCurList = listB;
        sCurListBytes = sizeof(listB);
        sReplayArmed = 1;
    } else {
        sCurList = list;
        sCurListBytes = sizeof(list);
        sReplayArmed = 0;
    }
#else
    (void) on;
#endif
}

/* Frame 2, after start_frame: puts the recorded scene into list B. */
void gfx_scegu_replay_scene(void) {
#if PSP_F2_REPLAY
    sceGuCallList((const void *) ((unsigned int) list + sRecBegin));
    gPspRepOk++;
    sRecState = 0; /* a recording is replayed only once */
#endif
}

/* ---- Stage 2 (PSP_F2_REPLAY_MTX): rewrite model matrices inside the recorded scene ----
 * sceGuSetMatrix(GU_MODEL, m) leaves 13 words in the list: WORLD_MATRIX_NUMBER(0) and 12 of WORLD_MATRIX_DATA with m[j + i*4]
 * (i = 0..3, j = 0..2) as (cmd << 24) | (float_bits >> 8). (Checked in pspsdk: sceGuSetMatrix.c and sendCommandf.) */
#define GE_CMD_WORLD_NUMBER 0x3Au
#define GE_CMD_WORLD_DATA 0x3Bu

unsigned int gfx_scegu_list_pos(void) {
    return (unsigned int) sceGuCheckList();
}

#if PSP_F2_REPLAY
static inline unsigned int ge_float_word(unsigned int cmd, float f) {
    union { float f; unsigned int i; } t;
    t.f = f;
    return (cmd << 24) | (t.i >> 8);
}
#endif

/* During recording: checks that at position off of list A there really is a model matrix block. full != 0 also
 * compares the 12 words with the ones that would come out of m (validates the order); if not, only the start and the end of the block. */
int gfx_scegu_mtx_block_ok(unsigned int off, int full, const float *m) {
#if PSP_F2_REPLAY
    volatile unsigned int *lu;
    int i, j;
    if ((off & 3U) != 0U || off + 13U * 4U > sizeof(list)) {
        return 0;
    }
    lu = (volatile unsigned int *) ((unsigned int) list | 0x40000000U);
    lu += off >> 2;
    if (lu[0] != (GE_CMD_WORLD_NUMBER << 24)) {
        return 0;
    }
    if (full) {
        for (i = 0; i < 4; i++) {
            for (j = 0; j < 3; j++) {
                if (lu[1 + i * 3 + j] != ge_float_word(GE_CMD_WORLD_DATA, m[j + i * 4])) {
                    return 0;
                }
            }
        }
        return 1;
    }
    return (lu[12] >> 24) == GE_CMD_WORLD_DATA;
#else
    (void) off; (void) full; (void) m;
    return 0;
#endif
}

/* Frame 2, before opening the list: puts matrix m (16 floats, Mtx order) into the block of list A (offset off). */
void gfx_scegu_write_model(unsigned int off, const float *m) {
#if PSP_F2_REPLAY
    volatile unsigned int *lu = (volatile unsigned int *) ((unsigned int) list | 0x40000000U);
    int i, j;
    lu += off >> 2;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 3; j++) {
            lu[1 + i * 3 + j] = ge_float_word(GE_CMD_WORLD_DATA, m[j + i * 4]);
        }
    }
#else
    (void) off; (void) m;
#endif
}

/* Same as the above for sceGuSetMatrix(GU_VIEW, m): 13 words = VIEW_MATRIX_NUMBER(0) + 12 of VIEW_MATRIX_DATA with m[j + i*4]
 * (i = 0..3, j = 0..2). The sky carries its orthographic matrix there (the game multiplies it with G_MTX_MUL over the projection). */
#define GE_CMD_VIEW_NUMBER 0x3Cu
#define GE_CMD_VIEW_DATA 0x3Du

/* During recording: 1 if at position off of list A there is exactly the GU_VIEW block of m. */
int gfx_scegu_view_block_ok(unsigned int off, const float *m) {
#if PSP_F2_REPLAY
    volatile unsigned int *lu;
    int i, j;
    if ((off & 3U) != 0U || off + 13U * 4U > sizeof(list)) {
        return 0;
    }
    lu = (volatile unsigned int *) ((unsigned int) list | 0x40000000U);
    lu += off >> 2;
    if (lu[0] != (GE_CMD_VIEW_NUMBER << 24)) {
        return 0;
    }
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 3; j++) {
            if (lu[1 + i * 3 + j] != ge_float_word(GE_CMD_VIEW_DATA, m[j + i * 4])) {
                return 0;
            }
        }
    }
    return 1;
#else
    (void) off; (void) m;
    return 0;
#endif
}

/* Frame 2, before opening the list: puts matrix m into the GU_VIEW block of list A (offset off). */
void gfx_scegu_write_view(unsigned int off, const float *m) {
#if PSP_F2_REPLAY
    volatile unsigned int *lu = (volatile unsigned int *) ((unsigned int) list | 0x40000000U);
    int i, j;
    lu += off >> 2;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 3; j++) {
            lu[1 + i * 3 + j] = ge_float_word(GE_CMD_VIEW_DATA, m[j + i * 4]);
        }
    }
#else
    (void) off; (void) m;
#endif
}

/* ---- Presentation: ring of 3 color buffers + thread that waits for the GE and the vblank ----
 * Before, the CPU could not start the next frame until the previous one went to the screen (it waited for the vblank with the processor stopped), so
 * each frame cost a WHOLE NUMBER of vblanks: one that needs 17 ms of CPU took 33 (30 fps) and the game ran at 40 instead of 58.
 * Now there are 3 color buffers. A high-priority thread presents the frames in order (waits for the GE to finish that frame, waits for the vblank and
 * puts the buffer on screen) and the CPU can already build the next frame in another buffer meanwhile. The command list and the vertex
 * arena are still just one: the next frame does not start until the GE has finished the previous one (usually almost no wait), so no
 * memory the GE reads is overwritten. What is drawn is identical.
 * gPspOptPipe = 0 ("Triple Buffer: Off"): it waits for the previous frame to be on screen before starting the next, same as before.
 * gPspOptPipe = 1: it can start with ONE previous frame still unpresented (the image may arrive one frame later if the CPU has headroom). */
extern int gPspOptPipe;         /* game/psp_options.c */

static unsigned int sFrameN;          /* frames already closed with end_frame (the one being built now is number sFrameN) */
static int sPrevList = -1;            /* id of the previous frame's list */
static int sPrevAsync;                /* the previous frame was sent without waiting for it to reach the screen */
struct PresItem {
    void *fb;
    void *tvfb; /* TV buffer of this frame (NULL with the console screen) */
    int list;
    unsigned int t_submit;
};
static struct PresItem sPresQ[4];
static volatile unsigned int sPresHead, sPresTail; /* queue of frames to present (filled by the main thread, emptied by the presentation thread) */
static volatile unsigned int sShown;               /* frames that are already on screen */
static SceUID sPresThread = -1, sPresReq = -1, sPresDone = -1;
static volatile unsigned int sPresSyncUs, sPresVblUs;
volatile int gPspSuspending;          /* 1 between the suspend notice and the resume notice: presenting is synchronous */
static int sPipeBroken;               /* a semaphore timed out (should not happen): presenting is synchronous and without a thread */

/* Measurement (microseconds): wait for the GPU and wait for the vblank inside end_frame. Read by gfx_pc.c. */
unsigned int gPspProfSync, gPspProfVblank;
/* time (us) of the last vblank in which a frame was presented, and of the one before (pc_main.c uses them to know if a frame missed a vblank) */
unsigned int gPspLastSwapT, gPspPrevSwapT;
int gPspPresentAsync = 0;            /* set by pc_main.c: the frame being sent does not wait to reach the screen */
unsigned int gPspProfPendWait = 0;   /* us that start_frame waited (free buffer / GE of the previous frame) */
unsigned int gPspAsyncSync = 0;      /* of the last presentation: us from end_frame until the GE finished */
unsigned int gPspAsyncVbl = 0;       /* us waiting for the vblank */
int gPspPendingDone = 0;             /* 1 if the previous frame was sent without waiting (start_frame now measures how long it waited) */
unsigned int gPspGateUs, gPspGateN, gPspQMax; /* accumulated start_frame wait, frames and largest queue seen in the window (printed by gfx_pc.c) */
int gPspPipeBroken;                  /* public copy of sPipeBroken for the log */

/* Puts a frame from the queue on screen (waits for the GE to finish that list, waits for the vblank and swaps the buffer). */
static void gfx_scegu_present_item(const struct PresItem *it) {
    unsigned int t1, t2;
    if (it->list >= 0) {
        sceGeListSync(it->list, 0); /* the GE finished that frame (if the id no longer exists it returns immediately) */
    }
    t1 = sceKernelGetSystemTimeLow();
    if (sTvOn && sTvSlowVbl) {
        /* vblank at ~30 Hz: the 60 frame pace is kept with the clock (without waiting for the vblank) */
        const unsigned int step = 16683u;
        if ((int) (sTvPaceT + step - t1) > 0) {
            sceKernelDelayThread(sTvPaceT + step - t1);
            sTvPaceT += step;
        } else {
            sTvPaceT = t1;
        }
    } else {
        sceDisplayWaitVblankStart();
    }
    t2 = sceKernelGetSystemTimeLow();
    if (it->tvfb != NULL && !sTvCableGone) {
        /* with the clock (without waiting for the vblank) the buffer swap is left for the next VIDEO frame: NEXTHSYNC would do it in the middle of the image (tearing) */
        sceDisplaySetFrameBuf((void *) ((unsigned int) sceGeEdramGetAddr() + (unsigned int) it->tvfb), sTvStride, GU_PSM_8888,
                              sTvSlowVbl ? PSP_DISPLAY_SETBUF_NEXTFRAME : PSP_DISPLAY_SETBUF_NEXTHSYNC);
    } else {
        sceDisplaySetFrameBuf((void *) ((unsigned int) sceGeEdramGetAddr() + (unsigned int) it->fb), BUF_WIDTH, GU_PSM_5650,
                              PSP_DISPLAY_SETBUF_NEXTHSYNC);
    }
    sDisplayBuffer = it->fb; /* (the HOME menu capture reads this 5650 buffer: it is still the last frame drawn) */
    gPspPrevSwapT = gPspLastSwapT;
    gPspLastSwapT = t2;
    sPresSyncUs = t1 - it->t_submit;
    sPresVblUs = t2 - t1;
    sShown++;
}

/* Presents here (in the calling thread) everything in the queue. Only used if the presentation thread does not exist or stopped responding. */
static void gfx_scegu_present_drain(void) {
    while ((int) (sPresTail - sPresHead) > 0) {
        const struct PresItem it = sPresQ[sPresHead & 3];
        sPresHead++;
        gfx_scegu_present_item(&it);
    }
}

#if PSP_ASYNC_PRESENT
static int gfx_scegu_present_thread(SceSize args, void *argp) {
    (void) args;
    (void) argp;
    for (;;) {
        if (sceKernelWaitSema(sPresReq, 1, NULL) < 0) {
            break;
        }
        if ((int) (sPresTail - sPresHead) > 0 && !sPipeBroken) {
            const struct PresItem it = sPresQ[sPresHead & 3];
            sPresHead++;
            gfx_scegu_present_item(&it);
        }
        sceKernelSignalSema(sPresDone, 1);
    }
    return 0;
}

static void gfx_scegu_present_init(void) {
    sPresReq = sceKernelCreateSema("sm64_pres_req", 0, 0, 8, NULL);
    sPresDone = sceKernelCreateSema("sm64_pres_done", 0, 0, 8, NULL);
    if (sPresReq < 0 || sPresDone < 0) {
        return;
    }
    sPresThread = sceKernelCreateThread("sm64_present", gfx_scegu_present_thread, 0x1D, 0x4000, PSP_THREAD_ATTR_USER, NULL);
    if (sPresThread < 0 || sceKernelStartThread(sPresThread, 0, NULL) < 0) {
        sPresThread = -1;
    } else {
        psp_watchdog_register("presentation", sPresThread);
    }
}
#endif

/* For the freeze report (psp_watchdog.c): state of the presentation queue. It only reads variables. */
void gfx_scegu_wd_describe(char *out, int size) {
    snprintf(out, (size_t) size, "video: frame %u, shown %u, queue %u..%u, presentation thread %s, broken queue %d, TV %s (mode %d), cable %d\n", sFrameN, (unsigned int) sShown,
             (unsigned int) sPresHead, (unsigned int) sPresTail, (sPresThread >= 0) ? "yes" : "no", sPipeBroken, sTvOn ? "on" : "off", sTvMode, psp_dve_cable_state());
}

/* Waits until `need` frames (or more) are on screen. Returns the us waited. */
static unsigned int gfx_scegu_wait_shown(unsigned int need) {
    unsigned int t0;
    PSP_WD_PHASE("waiting for the frame to reach the screen (wait_shown)");
    if ((int) (sShown - need) >= 0) {
        return 0;
    }
    t0 = sceKernelGetSystemTimeLow();
    while ((int) (sShown - need) < 0) {
        SceUInt timeout = 2000000; /* 2 s: if the thread does not respond the game does not freeze */
        if (sPresThread < 0 || sPipeBroken) {
            gfx_scegu_present_drain();
            break;
        }
        if (sceKernelWaitSema(sPresDone, 1, &timeout) < 0) {
            sPipeBroken = 1;
            gPspPipeBroken = 1;
            gfx_scegu_present_drain();
            break;
        }
    }
    return sceKernelGetSystemTimeLow() - t0;
}

/* Before opening a frame's list: a color buffer is free and the GE has already finished the previous frame (the list and the vertex arena are
 * reused). With the option off it also waits for the previous one to be on screen (the usual behavior). */
static void gfx_scegu_frame_gate(void) {
    const unsigned int t0 = sceKernelGetSystemTimeLow();
    unsigned int need = sFrameN; /* all previous ones on screen */
    unsigned int qd;
    if (gPspOptPipe && !gPspSuspending && sFrameN > 0 && sPresThread >= 0 && !sPipeBroken) { /* with the TV there is also a 1-frame queue (480i / 480p have only one TV buffer: a tear may be seen, but it no longer waits every frame) */
        need = sFrameN - 1; /* the previous one may still be in the queue: this one's buffer is the one from 3 frames ago, which has already been swapped */
    }
    qd = sPresTail - sPresHead;
    if (qd > 3) qd = 3;
    if (qd > gPspQMax) gPspQMax = qd;
    gfx_scegu_wait_shown(need);
    if (sFrameN > 0 && sPrevList >= 0) {
        sceGeListSync(sPrevList, 0);
    }
    gPspProfPendWait = sceKernelGetSystemTimeLow() - t0;
    gPspAsyncSync = sPresSyncUs;
    gPspAsyncVbl = sPresVblUs;
    gPspPendingDone = sPrevAsync ? 1 : 0;
    gPspGateUs += gPspProfPendWait;
    gPspGateN++;
}

/* Waits for the GE to finish EVERYTHING sent so far and continues in a new list (the GE state is kept). Used by gfx_pc.c right before
 * emptying the texture VRAM: new textures are written where the old ones were and the GE might still be reading them. */
void gfx_scegu_sync_all(void) {
    gfx_scegu_list_restart();
}

/* Power notice (called by the pc_main.c callback, which runs in another thread). On suspend: frames stop being queued, it waits (at most ~0.3 s) for
 * the ones there are to reach the screen and for the GE to be free, so the console does not go to sleep with a half-done list or with the presentation thread in
 * the middle of a buffer swap. On resume it goes back to normal. */
void gfx_scegu_power_event(int suspending) {
    if (suspending) {
        int i;
        gPspSuspending = 1;
        for (i = 0; i < 300 && (int) (sFrameN - sShown) > 0; i++) {
            sceKernelDelayThread(1000);
        }
        sceGeDrawSync(0);
        /* The console must not go to sleep with the DVE (TV encoder) routed to the TV and the TV buffers on screen: the firmware's suspend / resume path expects
         * the console's own video mode. The game thread (the only one that may touch the video output) goes back to the console screen and waits here for it (at
         * most ~0.5 s). After waking up the cable watcher turns the TV on again by itself (the setting is kept). */
        if (sTvOn) {
            sTvSuspendReq = 1;
            for (i = 0; i < 500 && sTvSuspendReq; i++) {
                sceKernelDelayThread(1000);
            }
        }
    } else {
        gPspSuspending = 0;
        sTvResumeReq = 1;
    }
}

/* Waits until nothing is left unpresented and nothing is in the GE (must be done before touching the video output or the TV buffers). */
static void gfx_scegu_tv_drain(void) {
    gfx_scegu_wait_shown(sFrameN); /* all frames sent are already on screen */
    if (sPrevList >= 0) {
        sceGeListSync(sPrevList, 0);
    }
    sceGeDrawSync(0);
}

static int gfx_scegu_tv_current(void) {
    return sTvOn ? sTvMode : PSP_TV_OFF;
}

/* Changes the output (PSP_TV_OFF / 240P / 480I / 480P) with the game running. Called at the start of a frame: nothing is unpresented and nothing is in the GE. */
static void gfx_scegu_tv_apply(int mode) {
    const int cur = gfx_scegu_tv_current();
    if (mode == cur || (mode != PSP_TV_OFF && sTvPool == NULL)) {
        return;
    }
    if (mode != PSP_TV_OFF && psp_dve_cable_state() == PSP_CABLE_NONE) {
        return; /* the TV is only turned on with the cable plugged in: the setting is kept and the cable watcher turns it on when it is connected */
    }
    gfx_scegu_tv_drain();
    sTvProbT0 = 0;
    if (mode == PSP_TV_OFF) {
        gfx_scegu_tv_to_lcd();
        psp_dve_mark_clear();
        return;
    }
    if (cur != PSP_TV_OFF) {
        /* from one TV mode to another: it goes through the console path first, as Daedalus does, so the DVE is not left with leftovers of the previous mode */
        (void) psp_dve_set_video_out(0);
        (void) gfx_scegu_wait_vblank_to(250000u);
    }
    psp_dve_mark_attempt(); /* if the console hangs here, the next boot goes back to the console screen */
    {
        const int rv = psp_dve_set_video_out(mode);
        const int rs = (rv < 0) ? 0 : gfx_scegu_tv_setup(mode);
        if (rv < 0 || rs < 0) {
            gfx_scegu_tv_to_lcd();
            psp_dve_mark_clear();
            if (!sTvAuto) {
                sTvRevert = PSP_TV_OFF; /* the menu learns that it could not be done (an automatic attempt keeps the setting: the watcher tries again) */
            }
            return;
        }
    }
    gPspTvActive = mode;
    if (mode == PSP_TV_480P && !sTvAuto) {
        sTvProbBack = cur; /* test: if it is not confirmed in time (the TV may not show 480p), it goes back to this mode; 240p and 480i show on any TV and do not ask for confirmation */
        sTvProbT0 = sceKernelGetSystemTimeLow() | 1u;
    }
}

/* Called by the options menu when "TV Output" changes. It waits a few frames in case it keeps changing (Off > 240p > 480p) before touching the output. */
void gfx_scegu_tv_request(int mode) {
    sTvReqMode = mode;
    sTvReqAge = 0;
}

/* Format change (4:3 / 16:9) with the TV on: the TV buffers are rebuilt, the video output is not touched. */
void gfx_scegu_tv_refresh(void) {
    sTvRefresh = 1;
}

/* "TV Area" change (width / height): only the rectangle is recomputed and the buffers are cleared (without redoing the output or measuring the vblank), so it can be adjusted
 * while holding the D-pad. */
void gfx_scegu_tv_area_refresh(void) {
    sTvAreaDirty = 1;
}

/* Routes the output to the TV again (for example after unplugging and plugging the cable back in). */
void gfx_scegu_tv_reconnect(void) {
    sTvReconnect = 1;
}

/* Seconds left to confirm 480i / 480p (0 = no test in progress). */
int gfx_scegu_tv_is_on(void) {
    return sTvOn;
}

int gfx_scegu_tv_probation_left(void) {
    unsigned int el;
    if (sTvProbT0 == 0) {
        return 0;
    }
    el = sceKernelGetSystemTimeLow() - sTvProbT0;
    return (el >= TV_PROBATION_US) ? 1 : (int) ((TV_PROBATION_US - el + 999999u) / 1000000u);
}

/* The user confirmed that 480p looks fine. */
void gfx_scegu_tv_confirm(void) {
    sTvProbT0 = 0;
}

/* The user cancels the test: it goes back to the previous mode right away. */
void gfx_scegu_tv_cancel(void) {
    if (sTvProbT0 != 0) {
        sTvProbT0 = 0;
        sTvReqMode = sTvProbBack;
        sTvReqAge = TV_REQ_FRAMES;
        sTvRevert = sTvProbBack;
    }
}

/* Mode it went back to by itself (test expired, cancelled or a change that failed); -1 if nothing happened. Read by the menu to adjust its row. */
int gfx_scegu_tv_take_revert(void) {
    const int r = sTvRevert;
    sTvRevert = -1;
    return r;
}

/* Measured vblank Hz of the TV x10 (0 = no TV); only to display them in the menu. */
int gfx_scegu_tv_vbl_hz10(void) {
    return (sTvOn && sTvVblUs > 0) ? (int) (10000000u / sTvVblUs) : 0;
}

static void gfx_scegu_start_frame(void) {
    bool has_home_menu_background;
    gfx_scegu_frame_gate(); /* the previous frame may still be in the GE / unpresented: do not touch the list / arena / buffers before that */
    if (sTvProbT0 != 0 && (sceKernelGetSystemTimeLow() - sTvProbT0) >= TV_PROBATION_US) { /* nobody confirmed 480i / 480p: it goes back by itself */
        sTvReqMode = sTvProbBack;
        sTvReqAge = TV_REQ_FRAMES;
        sTvProbT0 = 0;
        sTvRevert = sTvProbBack;
    }
    if (sTvReqMode >= 0 && ++sTvReqAge >= TV_REQ_FRAMES) {
        const int m = sTvReqMode;
        sTvReqMode = -1;
        gfx_scegu_tv_apply(m);
    }
    if (sTvOn && (sFrameN % 4u) == 0 && sTvProbT0 == 0 && sTvReqMode < 0) {
        /* Fast watcher (every ~70 ms, only with the TV on): when the cable is removed the console goes back to its own screen by itself and the TV frame (720x480) would look stretched
         * until the slow watcher changes mode. With 2 consecutive reads without a cable it switches right away to the console's normal frame (the one that is drawn anyway). */
        static int sGone;
        const int cs = psp_dve_cable_state();
        if (cs == 0) {
            if (++sGone >= 2) {
                sTvCableGone = 1;
            }
        } else if (cs > 0) {
            sGone = 0;
            sTvCableGone = 0;
        }
    }
    if (sTvOn && sTvDispMode > 0 && !sTvReconnect && !sTvCableGone && sTvProbT0 == 0 && sTvReqMode < 0) {
        /* The system can take the output back to the console screen with the cable still plugged in (the SCREEN / brightness button, or right after plugging the
         * cable in): the display mode / size is no longer the TV one. Checked every frame (a cheap call) so the output is routed to the TV again right away;
         * at most once per second, so a wrong reading cannot make it loop. */
        static unsigned int sLostAt;
        static int sLostN;
        int dm = 0, dw = 0, dh = 0;
        if (sceDisplayGetMode(&dm, &dw, &dh) >= 0 && (dm != sTvDispMode || dw != sTvDispW || dh != sTvDispH)) {
            /* the system is still changing the output while it is taking it: wait until the change settles (~0.2 s) before touching the DVE */
            if (++sLostN >= 12 && (sLostAt == 0 || sFrameN - sLostAt >= 60u) && psp_dve_cable_state() > 0) {
                sLostN = 0;
                sLostAt = sFrameN | 1u;
                sTvReconnect = 1;
            }
        } else {
            sLostN = 0;
        }
    }
    if ((sFrameN % 30u) == 0 && sTvProbT0 == 0 && sTvReqMode < 0) {
        /* Cable watcher (every ~0.5 s): the TV is only on with the cable plugged in. Without a cable it goes back to the console screen (the setting is kept); when it is
         * connected (stable ~1 s) it turns on by itself with the chosen mode; if it is reconnected quickly, it is routed again. */
        static int sCablePrev = -1, sStable;
        const int cs = psp_dve_cable_state();
        if (cs >= 0) {
            sStable = (cs == sCablePrev) ? sStable + 1 : 0;
            if (cs > 0 && sCablePrev == 0) {
                sTvAutoFails = 0; /* cable plugged in again: the automatic attempts start over */
                if (sTvOn) {
                    sTvReconnect = 1;
                }
            }
            sCablePrev = cs;
            if (sStable >= 1) {
                psp_dve_cable_latch(cs);
                if (cs == 0 && sTvOn) {
                    gfx_scegu_tv_drain();
                    gfx_scegu_tv_to_lcd();
                    psp_dve_mark_clear();
                } else if (cs > 0 && !sTvOn && gPspOptTv > 0 && sTvAutoHold == 0 && sTvAutoFails < TV_AUTO_TRIES) {
                    sTvAuto = 1;
                    gfx_scegu_tv_apply(gPspOptTv);
                    sTvAuto = 0;
                    sTvAutoFails = sTvOn ? 0 : sTvAutoFails + 1;
                }
                /* with a VIDEO cable plugged in and TV Output at Off the sound comes out of the console's speakers (with headphones, cs is 0: not touched) */
                psp_dve_audio_route(cs > 0 && !sTvOn && gPspOptTv == 0);
            }
        } else {
            psp_dve_audio_route(0);
        }
    }
    if (sTvAreaDirty) {
        sTvAreaDirty = 0;
        if (sTvOn && !sTvReconnect && !sTvRefresh) {
            int k;
            gfx_scegu_tv_drain();
            gfx_scegu_tv_area_calc();
            for (k = 0; k < sTvN; k++) {
                memset((void *) ((unsigned int) sceGeEdramGetAddr() + 0x40000000u + ((unsigned int) sTvFb[k] & 0x00FFFFFFu)), 0, sTvBytes);
            }
        }
    }
    if (sTvSuspendReq) { /* the console is about to sleep (see gfx_scegu_power_event) */
        if (sTvOn) {
            gfx_scegu_tv_drain();
            gfx_scegu_tv_to_lcd();
            psp_dve_mark_clear();
        }
        sTvSuspendReq = 0;
    }
    if (sTvResumeReq) {
        /* Woke up from sleep. The DVE (TV encoder) does not come back routed to the TV: if the TV was still on (the suspend notice could not be handled
         * in time) the frame was left with the TV layout and no signal. Back to the console screen first; the cable watcher turns the TV on again (it only
         * needs the cable and the setting). The firmware may also have put the VRAM back to 2 MB: the TV buffers live above 2 MB, so if the 4 MB cannot be
         * restored the TV stays unavailable until the game is restarted (the textures and console buffers are below 2 MB and are not affected). */
        sTvResumeReq = 0;
        gfx_scegu_tv_drain();
        if (sTvOn) {
            gfx_scegu_tv_to_lcd();
            psp_dve_mark_clear();
        }
        if (!psp_dve_resume() && sTvPool != NULL) {
            sTvPool = NULL;
            sTvReqMode = -1;
        }
        sTvAutoHold = 60;   /* ~1 s before turning the TV on again */
        sTvAutoFails = 0;
    }
    if (sTvAutoHold > 0) {
        sTvAutoHold--;
    }
    if (sTvReconnect || sTvRefresh) {
        const int cur = gfx_scegu_tv_current();
        const int reroute = sTvReconnect;
        sTvReconnect = sTvRefresh = 0;
        if (cur != PSP_TV_OFF) {
            PSP_WD_PHASE("TV re-route: draining the frames");
            gfx_scegu_tv_drain();
            sTvKeepVbl = reroute; /* same mode: the vblank period does not change */
            PSP_WD_PHASE("TV re-route: pspDveMgrSetVideoOut / TV setup");
            if ((reroute && psp_dve_set_video_out(cur) < 0) || gfx_scegu_tv_setup(cur) < 0) {
                gfx_scegu_tv_to_lcd();
                psp_dve_mark_clear();
                if (!reroute) {
                    sTvRevert = PSP_TV_OFF; /* a re-route is automatic (cable / system): the setting is kept and the cable watcher tries again */
                }
            }
            sTvKeepVbl = 0;
            PSP_WD_PHASE("gfx_start_frame");
        }
    }
    sNoiseFrame++;

    /* Callback rendering (intraFont in particular) changes GU texture state
     * outside the normal Fast3D cache. Rebind the first texture used by every
     * frame so resuming from the custom HOME menu cannot inherit that state. */
    tmu_state[0].tex = UINT32_MAX;
    tmu_state[1].tex = UINT32_MAX;

    if (sHomeMenuBgCaptureRequested) {
        gfx_scegu_wait_shown(sFrameN); /* what is on screen has to be the last complete frame */
        gfx_scegu_copy_framebuffer_from_vram(sHomeMenuBgBuffer, sDisplayBuffer);
        gfx_scegu_blur_framebuffer_565(sHomeMenuBgBuffer);
        sHomeMenuBgCaptureRequested = false;
        sHomeMenuBgCaptured = true;
    }

    sDrawBuffer = sFb[sFrameN % PSP_NFB];
    has_home_menu_background = sHomeMenuBgActive && sHomeMenuBgCaptured;
    if (has_home_menu_background) {
        gfx_scegu_copy_framebuffer_to_vram(sDrawBuffer, sHomeMenuBgBuffer);
    }

    sceGuStart(GU_DIRECT, sCurList);
    sFrameList = ge_list_executed[0];
    sceGuDrawBuffer(GU_PSM_5650, sDrawBuffer, BUF_WIDTH); /* sceGuStart set the previous frame's buffer: this frame is drawn in another one */
    if (!sReplayArmed) {
        sVArenaUsed = 0; /* the previous frame has already finished (end_frame waits for the GE): the vertex arena is reused */
        sVArenaWb = 0;
    }
    /* (in a frame that replays the recorded scene the arena is NOT touched: its vertices are read by the GE when it executes the scene) */
    if (PSP_GPUP()) {
        gpup_calibrate();
    }
    sceGuDisable(GU_SCISSOR_TEST);
    sceGuDepthMask(GU_TRUE); // Must be set to clear Z-buffer
    sceGuClearDepth(0);
    if (has_home_menu_background) {
        sceGuClear(GU_DEPTH_BUFFER_BIT);
    } else {
        sceGuClearColor(0xFF000000);
        sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
    }
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDepthMask(GU_FALSE);

    // Identity every frame? unsure.
    //sceGuSetMatrix(GU_PROJECTION, (const ScePspFMatrix4 *) identity_matrix);
    sceGuSetMatrix(GU_VIEW, (const ScePspFMatrix4 *) identity_matrix);
    //sceGuSetMatrix(GU_MODEL, (const ScePspFMatrix4 *) identity_matrix);

#if 0
    const int DitherMatrix[2][16] = { { 0, 8, 0, 8,
                         8, 0, 8, 0,
                         0, 8, 0, 8,
                         8, 0, 8, 0 },
                        { 8, 8, 8, 8,
                          0, 8, 0, 8,
                          8, 8, 8, 8,
                          0, 8, 0, 8 } };

    extern int gDoDither;
    extern int gFrame;

    sceGuDisable(GU_DITHER);
    if(gDoDither){
        // every frame
        sceGuSetDither((const ScePspIMatrix4 *)DitherMatrix[(gFrame&1)]);
        sceGuEnable(GU_DITHER);
    }
#endif
}

void gfx_scegu_on_resize(void) {
}

/* Scales the frame (480x272, 5650) to the TV buffer. It goes inside the frame's list, after everything from the game. */
static void gfx_scegu_tv_pass(void) {
    struct TvVert {
        float u, v;
        float x, y, z;
    };
    const int n = (sTvDw + 63) / 64; /* strips 64 px wide: the GE reads the texture better this way */
    const float ku = (float) gPspGameW / (float) sTvDw; /* source width (480 or 362) per TV pixel */
    const int fields = (sTvMode == PSP_TV_480I || sTvMode == PSP_TV_240P) ? 2 : 1; /* 480i: the buffer carries two 240-line fields (odd on top, even from line 262) */
    const float scale = (float) SCR_HEIGHT / (float) sTvDh; /* lines of the original image per TV line */
    struct TvVert *vt;
    int i, fld;
    void *src = (void *) ((unsigned int) sceGeEdramGetAddr() + ((unsigned int) sDrawBuffer & 0x00FFFFFFu)); /* GE address (0x04...), not the CPU's */

    sTvCur = sTvFb[sFrameN % (unsigned int) sTvN];
    gfx_scegu_reserve_list_memory(sizeof(struct TvVert) * 2 * (size_t) n * (size_t) fields + 64);
    vt = (struct TvVert *) sceGuGetMemory(sizeof(struct TvVert) * 2 * (size_t) n * (size_t) fields);
    for (fld = 0; fld < fields; fld++) {
        /* Field A (fld 0): lines 2r+1 of the TV frame -> rows 0..; field B (fld 1): lines 2r -> rows from 262. Each field samples the image at the center of ITS lines
         * (v0 = +/- scale/2) so that when interlaced the same image comes out as in 480p. */
        const float y0 = (fields == 2) ? (float) ((fld ? TV_FIELD_OFFSET : 0) + sTvDy / 2) : (float) sTvDy;
        const float y1 = y0 + ((fields == 2) ? (float) (sTvDh / 2) : (float) sTvDh);
        const float v0 = (fields == 2 && sTvMode == PSP_TV_480I) ? (fld ? -0.5f * scale : 0.5f * scale) : 0.0f; /* 240p: both fields carry the same lines (240 "doubled" lines, no interlacing) */
        const float v1 = v0 + (float) SCR_HEIGHT;
        struct TvVert *f = vt + (size_t) fld * 2 * (size_t) n;
        for (i = 0; i < n; i++) {
            const int x0 = i * 64;
            int x1 = x0 + 64;
            if (x1 > sTvDw) {
                x1 = sTvDw;
            }
            f[2 * i].u = ku * (float) x0;
            f[2 * i].v = v0;
            f[2 * i].x = (float) (sTvDx + x0);
            f[2 * i].y = y0;
            f[2 * i].z = 0.0f;
            f[2 * i + 1].u = ku * (float) x1;
            f[2 * i + 1].v = v1;
            f[2 * i + 1].x = (float) (sTvDx + x1);
            f[2 * i + 1].y = y1;
            f[2 * i + 1].z = 0.0f;
        }
    }

    sceGuDrawBuffer(GU_PSM_8888, sTvCur, sTvStride);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_BLEND);
    sceGuDisable(GU_FOG);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_5650, 0, 0, 0);
    sceGuTexImage(0, 512, 512, BUF_WIDTH, src);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
    sceGuTexFlush(); /* the GE just wrote that buffer: it must not use what it had in the texture cache */
    sceGuTexSync();
    for (fld = 0; fld < fields; fld++) {
        const struct TvVert *f = vt + (size_t) fld * 2 * (size_t) n;
        sceGuScissor(sTvDx, (int) f[0].y, sTvDx + sTvDw, (int) f[1].y);
        sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2 * n, 0, f);
    }

    /* back to the frame's base state (start_frame sets the drawing buffer of the next frame again) */
    sceGuDrawBuffer(GU_PSM_5650, sDrawBuffer, BUF_WIDTH);
    sceGuScissor(0, 0, SCR_WIDTH, SCR_HEIGHT);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuEnable(GU_ALPHA_TEST);
    sceGuDisable(GU_TEXTURE_2D);
    gl_blend = false;
    gfx_scegu_state_invalidate();
    gPspTvStateDirty = 1; /* gfx_pc.c: before the next frame it sends viewport, scissor, depth, blend, shader and textures again */
}

static void gfx_scegu_end_frame(void) {
    const int async = (gPspPresentAsync != 0 && !gPspSuspending && sPresThread >= 0 && !sPipeBroken) ? 1 : 0;
    struct PresItem *it;

    if (sTvOn) {
        gfx_scegu_tv_pass();
        psp_dve_frame_tick();
    }
    sceGuFinish();
    it = &sPresQ[sPresTail & 3];
    it->fb = sDrawBuffer;
    it->tvfb = sTvOn ? sTvCur : NULL;
    it->list = sFrameList;
    it->t_submit = sceKernelGetSystemTimeLow();
    sPresTail++;
    sPrevList = sFrameList;
    sPrevAsync = async;
    sFrameN++;
    if (sPresThread >= 0 && !sPipeBroken) {
        sceKernelSignalSema(sPresReq, 1);
        if (async) {
            /* the thread waits for the GE and the vblank and swaps the buffer; the CPU carries on with its own work */
            gPspProfSync = 0;
            gPspProfVblank = 0;
            return;
        }
        gfx_scegu_wait_shown(sFrameN);
    } else {
        gfx_scegu_present_drain();
    }
    gPspPresentAsync = 0; /* this time it was presented synchronously (so gfx_pc.c / gfx_psp.c know about it) */
    gPspProfSync = sPresSyncUs;
    gPspProfVblank = sPresVblUs;
}

static void gfx_scegu_finish_render(void) {
    /* There should be something here! */
}

// clang-format off
struct GfxRenderingAPI gfx_opengl_api = {
    gfx_scegu_z_is_from_0_to_1,
    gfx_scegu_unload_shader,
    gfx_scegu_load_shader,
    gfx_scegu_create_and_load_new_shader,
    gfx_scegu_lookup_shader,
    gfx_scegu_shader_get_info,
    gfx_scegu_new_texture,
    gfx_scegu_select_texture,
    gfx_scegu_upload_texture,
    gfx_scegu_set_sampler_parameters,
    gfx_scegu_set_depth_test,
    gfx_scegu_set_depth_mask,
    gfx_scegu_set_zmode_decal,
    gfx_scegu_set_viewport,
    gfx_scegu_set_scissor,
    gfx_scegu_set_use_alpha,
    gfx_scegu_draw_triangles,
    gfx_scegu_init,
    gfx_scegu_on_resize,
    gfx_scegu_start_frame,
    gfx_scegu_end_frame,
    gfx_scegu_finish_render
};

void gfx_scegu_request_home_menu_background(void) {
    sHomeMenuBgCaptureRequested = true;
}

void gfx_scegu_set_home_menu_background_active(bool active) {
    sHomeMenuBgActive = active;
    if (!active) {
        sHomeMenuBgCaptureRequested = false;
        sHomeMenuBgCaptured = false;
    }
}

void gfx_scegu_render_home_menu(int selected_index, int screen, int control_selected_index,
                                const char *status_message, uint8_t highlight_red,
                                uint8_t highlight_green, uint8_t highlight_blue) {
    gfx_scegu_prepare_home_menu_draw();
    if (screen == 1) {
        gfx_scegu_render_controller_mapping(control_selected_index, status_message, highlight_red,
                                             highlight_green, highlight_blue);
    } else {
        gfx_scegu_render_home_menu_main(selected_index, highlight_red, highlight_green,
                                        highlight_blue);
    }
}

#endif // RAPI_GL_LEGACY
