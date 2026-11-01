#if defined(TARGET_PSP)
#include "gfx_window_manager_api.h"
#include "gfx_screen_config.h"
#include "macros.h"

#include <stdio.h>
#include <string.h>
#include <pspsdk.h>
#include <pspkernel.h>
#include <pspctrl.h>
#include <psppower.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspgum.h>
#include <stdint.h>

#define GFX_API_NAME "PSP - sceGU"
#define SCR_WIDTH (480)
#define SCR_HEIGHT (272)

#define PSP_VI_RATE_HZ 60U
#define PSP_VI_FRAME_BASE_USEC 16666U
#define PSP_VI_FRAME_REMAINDER 40U
#define PSP_GAME_UPDATE_RATE 2U

/* Logica a 30 Hz FIJOS medidos con el reloj real. Si el dibujado se atrasa un tick completo, se salta el dibujado
 * (solo la emulacion grafica: la logica y el grafo de objetos siguen corriendo) hasta ponerse al dia, en vez de ir en
 * camara lenta. Maximo PSP_MAX_SKIPPED_TICKS seguidos (2 -> nunca menos de 10 fps de imagen). Con una pausa larga
 * (carga de nivel, menu HOME...) se re-sincroniza en vez de acelerar. Poner PSP_FRAME_SKIP a 0 para el comportamiento anterior. */
#define PSP_FRAME_SKIP 1
#define PSP_MAX_SKIPPED_TICKS 2U
#define PSP_RESYNC_TICKS 4
#define PSP_TICK_USEC 33333

/* 1 = este tick presenta DOS cuadros (60 fps: uno interpolado y uno real), 0 = un cuadro (30 fps). Lo decide gfx_pc.c
 * (gfx_psp_update_mode) al inicio de cada tick segun lo que cuesta dibujar. */
int gPspSixtyMode = 0;

static uint32_t sNextFrameCompletionUsec;
static uint32_t sFramePacingRemainder;
static bool sFramePacingInitialized;
static uint32_t sSkippedTicks;

static inline int32_t gfx_psp_time_diff(uint32_t a, uint32_t b) {
    return (int32_t)(a - b);
}

/* Mirror OOT's VI remainder accumulation. At updateRate 2 this produces
 * 33333, 33333, 33334 microsecond intervals, averaging exactly 30 Hz. */
static uint32_t gfx_psp_get_frame_usec(void) {
    /* En modo 60 fps cada cuadro presentado dura 1 vblank; en 30 fps, 2 vblanks. */
    const uint32_t update_rate = gPspSixtyMode ? (PSP_GAME_UPDATE_RATE / 2U) : PSP_GAME_UPDATE_RATE;
    uint32_t frame_usec = PSP_VI_FRAME_BASE_USEC * update_rate;

    sFramePacingRemainder += PSP_VI_FRAME_REMAINDER * update_rate;
    if (sFramePacingRemainder >= PSP_VI_RATE_HZ) {
        uint32_t extra_usec = sFramePacingRemainder / PSP_VI_RATE_HZ;

        frame_usec += extra_usec;
        sFramePacingRemainder -= extra_usec * PSP_VI_RATE_HZ;
    }

    return frame_usec;
}

extern int gPspPresentAsync;

static void gfx_psp_pace_presented_frame(void) {
    uint32_t now = sceKernelGetSystemTimeLow();
    uint32_t frame_usec = gfx_psp_get_frame_usec();
    int32_t wait_usec;

    if (!sFramePacingInitialized) {
        sNextFrameCompletionUsec = now;
        sFramePacingInitialized = true;
    }

    wait_usec = gfx_psp_time_diff(sNextFrameCompletionUsec, now);
    if (wait_usec > 0) {
        /* cuadro de presentacion asincrona (gfx_scegu.c): el GE aun no termino ni paso el vblank; no se duerme para poder
         * hacer ya la logica del siguiente tick. En 60 fps el vblank ya marca el ritmo. */
        if (!gPspPresentAsync) {
            sceKernelDelayThread((uint32_t)wait_usec);
        }
    } else if (wait_usec < 0) {
#if PSP_FRAME_SKIP
        /* Se conserva el retraso: gfx_psp_start_frame decide saltar el dibujado de los siguientes ticks hasta ponerse al dia. */
#else
        /* Do not run catch-up frames after a missed deadline. */
        sNextFrameCompletionUsec = now;
#endif
    }

    sNextFrameCompletionUsec += frame_usec;
}

/* Un cuadro se paso de un vblank y se descarta el 2o cuadro del tick (pc_main.c): se da por cumplido su plazo para que el tick dure
 * lo mismo (33,3 ms) y el juego no vaya en camara lenta ni tenga que saltar dibujado despues. */
void gfx_psp_skip_frame_slot(void) {
    if (sFramePacingInitialized) {
        sNextFrameCompletionUsec += gfx_psp_get_frame_usec();
    }
}

/* I forgot why we need this */
void __assert_func(UNUSED const char *file, UNUSED int line, UNUSED const char *method, UNUSED const char *expression) {
}

/* Minimalist PSP SDK 0.15.0 for Windows whines about missing this, linux built toolchain doesn't care */
char *stpcpy(char *__restrict__ dest, const char *__restrict__ src) {
    while ((*dest++ = *src++) != '\0')
        /* nothing */;
    return --dest;
}

int isspace(int _c) {
    char c = (char) _c;
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

static void gfx_psp_init(UNUSED const char *game_name, UNUSED bool start_in_fullscreen) {
    sNextFrameCompletionUsec = 0;
    sFramePacingRemainder = 0;
    sFramePacingInitialized = false;
    sSkippedTicks = 0;

    scePowerSetClockFrequency(333, 333, 166);
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
}

static void gfx_psp_set_fullscreen_changed_callback(UNUSED void (*on_fullscreen_changed)(bool is_now_fullscreen)) {
}

static void gfx_psp_set_fullscreen(UNUSED bool enable) {
}

static void gfx_psp_set_keyboard_callbacks(UNUSED bool (*on_key_down)(int scancode),
                                           UNUSED bool (*on_key_up)(int scancode),
                                           UNUSED void (*on_all_keys_up)(void)) {
}

static void gfx_psp_main_loop(void (*run_one_game_iter)(void)) {
    while (1) {
        run_one_game_iter();
    }
}

static void gfx_psp_get_dimensions(uint32_t *width, uint32_t *height) {
    *width = SCR_WIDTH;
    *height = SCR_HEIGHT;
}

/* What events should we be handling? */
static void gfx_psp_handle_events(void) {
    /* Lets us yield to other threads*/
    sceKernelDelayThread(100);
}

static bool gfx_psp_start_frame(void) {
#if PSP_FRAME_SKIP
    if (sFramePacingInitialized) {
        const uint32_t now = sceKernelGetSystemTimeLow();
        /* duracion nominal de un cuadro presentado en el modo actual */
        const uint32_t frame_nominal = gPspSixtyMode ? (PSP_TICK_USEC / 2U) : PSP_TICK_USEC;
        /* cuanto se ha retrasado el inicio de este cuadro respecto a su hora nominal */
        const int32_t late = gfx_psp_time_diff(now, sNextFrameCompletionUsec - frame_nominal);

        if (late >= (int32_t)(PSP_RESYNC_TICKS * PSP_TICK_USEC)) {
            /* pausa larga (carga, menu HOME...): no acelerar, volver a sincronizar */
            sNextFrameCompletionUsec = now + frame_nominal;
            sFramePacingRemainder = 0;
            sSkippedTicks = 0;
            return true;
        }
        /* En modo 60 fps no se salta dibujado: si el dibujado no cabe en un cuadro, gfx_psp_update_mode vuelve a 30 fps. */
        if (!gPspSixtyMode && late >= (int32_t)PSP_TICK_USEC && sSkippedTicks < PSP_MAX_SKIPPED_TICKS) {
            /* tick de solo logica: se da por cumplido su plazo sin esperar y sin dibujar */
            sSkippedTicks++;
            sNextFrameCompletionUsec += gfx_psp_get_frame_usec();
            return false;
        }
    }
    sSkippedTicks = 0;
#endif
    return true;
}

static void gfx_psp_swap_buffers_begin(void) {
    gfx_psp_pace_presented_frame();
}

static void gfx_psp_swap_buffers_end(void) {
    /* Lets us yield to other threads*/
    sceKernelDelayThread(100);
}

/* Idk what this is for? */
static double gfx_psp_get_time(void) {
    return 0.0;
}

struct GfxWindowManagerAPI gfx_psp = {
    gfx_psp_init,
    gfx_psp_set_keyboard_callbacks,
    gfx_psp_set_fullscreen_changed_callback,
    gfx_psp_set_fullscreen,
    gfx_psp_main_loop,
    gfx_psp_get_dimensions,
    gfx_psp_handle_events,
    gfx_psp_start_frame,
    gfx_psp_swap_buffers_begin,
    gfx_psp_swap_buffers_end,
    gfx_psp_get_time
};
#endif // TARGET_PSP
