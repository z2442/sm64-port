#if defined(TARGET_PSP)
#include "gfx_window_manager_api.h"
#include "gfx_screen_config.h"
#include "macros.h"

#include <stdio.h>
#include <pspkernel.h>
#include <psppower.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspgum.h>
#include <psprtc.h>

#define GFX_API_NAME "PSP - sceGU"
#define SCR_WIDTH (480)
#define SCR_HEIGHT (272)

static int force_30fps = 1;

/* I forgot why we need this */
void __assert_func(const char *file, int line, const char *method, const char *expression) {
}

static unsigned int GetTicks(void) {
    uint64_t temp;
    sceRtcGetCurrentTick(&temp);
    return (unsigned int) ((temp / 1000) & 0xffffffff);
}

static inline void Delay(int ms) {
    sceKernelDelayThread(ms * 1000);
}

static int exitCallback(UNUSED int arg1, UNUSED int arg2, UNUSED void *common) {
    sceKernelExitGame();
    return 0;
}

static int callbackThread(UNUSED SceSize args, UNUSED void *argp) {
    int cbid;

    cbid = sceKernelCreateCallback("Exit Callback", exitCallback, NULL);
    sceKernelRegisterExitCallback(cbid);

    sceKernelSleepThreadCB();

    return 0;
}

static void gfx_psp_init(UNUSED const char *game_name, UNUSED bool start_in_fullscreen) {

    int thid = 0;

    thid = sceKernelCreateThread("update_thread", callbackThread, 0x11, 0xFA0, 0, 0);
    if (thid >= 0) {
        sceKernelStartThread(thid, 0, 0);
    }

    scePowerSetClockFrequency(333, 333, 166);
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
}

static bool gfx_psp_start_frame(void) {
    return true;
}

static void sync_framerate_with_timer(void) {
    // Number of milliseconds a frame should take (30 fps)
    const unsigned int FRAME_TIME = 1000 / 30;
    static unsigned int last_time;
    unsigned int elapsed = GetTicks() - last_time;

    if (elapsed < FRAME_TIME)
        Delay(FRAME_TIME - elapsed);
    last_time += FRAME_TIME;
}

static void gfx_psp_swap_buffers_begin(void) {
    if (force_30fps) {
        sync_framerate_with_timer();
    }
}

static void gfx_psp_swap_buffers_end(void) {
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
