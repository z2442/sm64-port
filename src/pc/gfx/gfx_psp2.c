#include "../compat.h"

//#if defined(TARGET_SCEGU) && defined(ENABLE_OPENGL)
#if 1
#include "gfx_window_manager_api.h"
#include "gfx_screen_config.h"
#include "macros.h"

#include <stdio.h>
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspgum.h>

#define GFX_API_NAME "PSP - sceGU"

#define SCR_WIDTH (480)
#define SCR_HEIGHT (272)

void __assert_func(const char* file, int line, const char* method,
                   const char* expression) {
}

// static int vsync_enabled = 0;

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

static void gfx_psp2_init(UNUSED const char *game_name, UNUSED bool start_in_fullscreen) {

    int thid = 0;

    thid = sceKernelCreateThread("update_thread", callbackThread, 0x11, 0xFA0, 0, 0);
    if (thid >= 0) {
        sceKernelStartThread(thid, 0, 0);
    }
}

static void
gfx_psp2_set_fullscreen_changed_callback(UNUSED void (*on_fullscreen_changed)(bool is_now_fullscreen)) {
}

static void gfx_psp2_set_fullscreen(UNUSED bool enable) {
}

static void gfx_psp2_set_keyboard_callbacks(UNUSED bool (*on_key_down)(int scancode),
                                            UNUSED bool (*on_key_up)(int scancode),
                                            UNUSED void (*on_all_keys_up)(void)) {
}

static void gfx_psp2_main_loop(void (*run_one_game_iter)(void)) {
    while (1) {
        run_one_game_iter();
    }
}

static void gfx_psp2_get_dimensions(uint32_t *width, uint32_t *height) {
    *width = SCR_WIDTH;
    *height = SCR_HEIGHT;
}

static void gfx_psp2_handle_events(void) {
}

static bool gfx_psp2_start_frame(void) {
    return true;
}

#if 0
static void sync_framerate_with_timer(void) {
    // Number of milliseconds a frame should take (30 fps)
    const Uint32 FRAME_TIME = 1000 / 30;
    static Uint32 last_time;
    Uint32 elapsed = SDL_GetTicks() - last_time;

    if (elapsed < FRAME_TIME)
        SDL_Delay(FRAME_TIME - elapsed);
    last_time += FRAME_TIME;
}
#endif

static void gfx_psp2_swap_buffers_begin(void) {
#if 0
    if (!vsync_enabled) {
        sync_framerate_with_timer();
    }

    SDL_GL_SwapWindow(wnd);
#endif
}

static void gfx_psp2_swap_buffers_end(void) {
    return;
    //sceDisplayWaitVblankStart();
    //sceGuSwapBuffers();
}

static double gfx_psp2_get_time(void) {
    return 0.0;
}

struct GfxWindowManagerAPI gfx_psp = { gfx_psp2_init,
                                        gfx_psp2_set_keyboard_callbacks,
                                        gfx_psp2_set_fullscreen_changed_callback,
                                        gfx_psp2_set_fullscreen,
                                        gfx_psp2_main_loop,
                                        gfx_psp2_get_dimensions,
                                        gfx_psp2_handle_events,
                                        gfx_psp2_start_frame,
                                        gfx_psp2_swap_buffers_begin,
                                        gfx_psp2_swap_buffers_end,
                                        gfx_psp2_get_time };

#endif
