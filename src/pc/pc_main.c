#include <stdlib.h>

#ifdef TARGET_WEB
#include <emscripten.h>
#include <emscripten/html5.h>
#endif

#include "sm64.h"

#include "game/memory.h"
#include "audio/external.h"

#include "gfx/gfx_pc.h"
#include "gfx/gfx_opengl.h"
#include "gfx/gfx_direct3d11.h"
#include "gfx/gfx_direct3d12.h"
#include "gfx/gfx_dxgi.h"
#include "gfx/gfx_glx.h"
#include "gfx/gfx_psp.h"
#include "gfx/gfx_screen_config.h"
#include "gfx/gfx_sdl.h"

#include "audio/audio_api.h"
#include "audio/audio_psp.h"
#include "audio/audio_wasapi.h"
#include "audio/audio_pulse.h"
#include "audio/audio_alsa.h"
#include "audio/audio_sdl.h"
#include "audio/audio_null.h"

#include "controller/controller_keyboard.h"

#include "configfile.h"
#include "fs_utils.h"
#ifdef TARGET_PSP
#include "psp_dve.h"
#include "psp_netplay.h"
#include "psp_watchdog.h"
#endif

#include "compat.h"

#if defined(TARGET_PSP)
#include <pspsdk.h>
#include <pspkernel.h>
#include <psppower.h>
#include <pspctrl.h>
#include <pspge.h>
#include <string.h>
#include <stdio.h>
#include <pspiofilemgr.h>
#include <time.h>
#include "psp_me.h"
#define MODULE_NAME "SM64 for PSP"
#ifndef SRC_VER
#define SRC_VER "UNKNOWN"
#endif

PSP_MODULE_INFO(MODULE_NAME, 0, 1, 1);
PSP_HEAP_SIZE_KB(-1024);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);

const char _srcver[] __attribute__((section (".version"), used)) = MODULE_NAME " - " SRC_VER ;
#endif

#define CONFIG_FILE "sm64config.txt"

OSMesg D_80339BEC;
OSMesgQueue gSIEventMesgQueue;

s8 gResetTimer;
s8 D_8032C648;
s8 gDebugLevelSelect;
s8 gShowProfiler;
s8 gShowDebugText;

static struct AudioAPI *audio_api;
static struct GfxWindowManagerAPI *wm_api;
static struct GfxRenderingAPI *rendering_api;

extern void gfx_run(Gfx *commands);
extern void thread5_game_loop(void *arg);
extern void create_next_audio_buffer(s16 *samples, u32 num_samples);
void game_loop_one_iteration(void);

void dispatch_audio_sptask(UNUSED struct SPTask *spTask) {
}

void set_vblank_handler(UNUSED s32 index, UNUSED struct VblankHandler *handler, UNUSED OSMesgQueue *queue, UNUSED OSMesg *msg) {
}

static uint8_t inited = 0;

#include "game/game_init.h" // for gGlobalTimer

#if defined(TARGET_PSP)
/* Frame interpolation (dynamic 60 fps). The draw list is built with interpolated matrices; these functions
 * rewrite it so that it points to the real state's matrices (see sm64ex's 60fps patch). */
extern int gPspSixtyMode;
extern void gfx_psp_update_mode(void);
extern void mtx_patch_interpolated(void);
extern void patch_screen_transition_interpolated(void);
extern void patch_title_screen_scales(void);
extern void patch_interpolated_dialog(void);
extern void patch_interpolated_hud(void);
extern void patch_interpolated_paintings(void);
extern void patch_interpolated_bubble_particles(void);
extern void patch_interpolated_snow_particles(void);

#if defined(TARGET_PSP)
/* PSP_F2_REPLAY: patches that are NOT just "change object matrices" (vertices, title scale, HUD, dialog...).
 * If any is pending the 2nd frame emits the full scene again (as before stage 2). */
extern int psp_hud_patch_pending(void);
extern int psp_dialog_patch_pending(void);
extern int psp_transition_patch_pending(void);
extern int psp_title_patch_pending(void);
extern int psp_painting_patch_pending(void);
extern int psp_bubble_patch_pending(void);
extern int psp_snow_patch_pending(void);
extern int psp_projection_patch_pending(void);
extern int psp_viewport_patch_pending(void);
static int psp_f2_nonmatrix_pending(void) {
    return psp_projection_patch_pending() | psp_viewport_patch_pending() | psp_hud_patch_pending() |
           psp_dialog_patch_pending() | psp_transition_patch_pending() | psp_title_patch_pending() |
           psp_painting_patch_pending() | psp_bubble_patch_pending() | psp_snow_patch_pending();
}
extern void gfx_psp_replay_prepare(int pending);
extern int gfx_psp_replay_decided(void);
extern int gPspReplayEma;
#endif

static inline void patch_interpolations(void) {
    mtx_patch_interpolated();
    patch_screen_transition_interpolated();
    patch_title_screen_scales();
    patch_interpolated_dialog();
    patch_interpolated_hud();
    patch_interpolated_paintings();
    patch_interpolated_bubble_particles();
    patch_interpolated_snow_particles();
    psp_np_hud_patch(); /* multiplayer name tag */
}
#endif

void send_display_list(struct SPTask *spTask) {
    if (!inited) {
        return;
    }
#if defined(TARGET_PSP)
    /* 30 fps mode: a single frame with the real state -> it is patched right away. 60 fps mode: this first frame comes out
     * interpolated and the patch is applied before the second frame (see produce_one_frame). */
    if (!gPspSixtyMode) {
        patch_interpolations();
    }
#endif
    gfx_run((Gfx *)spTask->task.t.data_ptr);
}

#ifdef VERSION_EU
#define SAMPLES_HIGH 656
#define SAMPLES_LOW 640
#else
#define SAMPLES_HIGH 544
#define SAMPLES_LOW 528
#endif

#if !defined(TARGET_PSP)
static s16 audio_buffer[SAMPLES_HIGH * 2 * 2];
#endif

extern int gProcessAudio;
int gFrame=0;
static void save_config(void);

#if defined(TARGET_PSP)
/* Native PSP HOME menu: an exit callback is registered (without this the HOME button does nothing).
 * When "Exit game" is confirmed the system calls this callback. */
extern void gfx_scegu_power_event(int suspending); /* gfx_scegu.c */
static int psp_exit_callback(UNUSED int arg1, UNUSED int arg2, UNUSED void *common) {
    static volatile int sExiting;
    if (sExiting) {
        return 0;
    }
    sExiting = 1;
    /* sceKernelExitGame does not run the atexit handlers, so everything is closed here, in this order:
     * 1) the game thread stops presenting, the GE is drained and, if the TV output was on, the video goes back to the console screen (the same
     *    path as before sleeping: only the game thread may touch the video output);
     * 2) the audio routing goes back to automatic;
     * 3) the audio threads and the ME are stopped (a ME left running our loop takes the console down when the game exits);
     * 4) the configuration is saved. */
    psp_netplay_shutdown(); /* closes the WLAN room (no-op if it was never used) */
    gfx_scegu_power_event(1);
    psp_dve_shutdown();
    audio_psp_shutdown();
    save_config();
    sceKernelExitGame();
    return 0;
}

static int psp_power_callback(UNUSED int unknown, int flags, UNUSED void *common) {
    if (flags & (PSP_POWER_CB_SUSPENDING | PSP_POWER_CB_POWER_SWITCH | PSP_POWER_CB_STANDBY)) {
        if (!(flags & (PSP_POWER_CB_RESUMING | PSP_POWER_CB_RESUME_COMPLETE))) {
            /* The order matters: the system does not wait for this thread, so what must be done before sleeping goes first and the slow things last.
             * 1) the ME (it must not be left running our loop); 2) the video (GE drained, TV back to the console screen); 3) the WLAN room (up to ~3 s,
             * but only if there is one). */
            psp_me_power_event(1);
        }
        gfx_scegu_power_event(1);
        if (!(flags & (PSP_POWER_CB_RESUMING | PSP_POWER_CB_RESUME_COMPLETE))) {
            (void) psp_netplay_suspend(); /* the WLAN does not survive sleep: the room is left */
        }
    }
    if (flags & (PSP_POWER_CB_RESUME_COMPLETE | PSP_POWER_CB_RESUMING)) {
        gfx_scegu_power_event(0);
    }
    if (flags & PSP_POWER_CB_RESUME_COMPLETE) {
        psp_me_power_event(0); /* the ME is started again (if it stopped properly before sleeping) */
    }
    return 0;
}

static int psp_callback_thread(UNUSED SceSize args, UNUSED void *argp) {
    int cbid = sceKernelCreateCallback("Exit Callback", psp_exit_callback, NULL);
    sceKernelRegisterExitCallback(cbid);
    scePowerRegisterCallback(0, sceKernelCreateCallback("Power Callback", psp_power_callback, NULL));
    sceKernelSleepThreadCB();
    return 0;
}

static void psp_setup_exit_callback(void) {
    int thid = sceKernelCreateThread("exit_callback_thread", psp_callback_thread, 0x11, 0x4000, 0, NULL);
    if (thid >= 0) {
        sceKernelStartThread(thid, 0, NULL);
    }
}
#endif

extern int gPspPresentAsync; /* gfx_scegu.c */
extern int gPspOptPipe, gPspOptFpsCap; /* game/psp_options.c */
extern int psp_opt_fps_cap(void);
extern int gPspAsyncWant;    /* gfx_pc.c: 1 if the GE queue is long enough for async presentation to pay off */

extern unsigned int gPspLastSwapT, gPspPrevSwapT; /* gfx_scegu.c: time of the last two vblanks with a frame presented */
extern void gfx_psp_skip_frame_slot(void);
extern void gfx_psp_set_role(int role); /* gfx_pc.c: 0 = normal, 1 = 1st frame of a 60 fps tick (recorded), 2 = 2nd frame (replayed) */
unsigned int gPspF2Skip; /* 60 fps ticks whose 2nd frame was dropped because the 1st missed a vblank */
/* If the 1st frame of a 60 fps tick took more than ~1.5 vblank since the tick started (the logic and drawing did not fit in
 * 16.7 ms), the 2nd frame would land on a 3rd vblank and the tick would last 50 ms (game at 20 Hz = slow motion). In that case it is skipped. */
#define PSP_F2_MAX_FRAME1_US 25000U

void produce_one_frame(void) {
#if defined(TARGET_PSP)
    unsigned int t_entry, t_net0, t_logic0, t_end0, t_end1;
    gfx_psp_update_mode(); /* decides whether this tick presents 1 frame (30 fps) or 2 (60 fps) */
    gfx_psp_set_role(gPspSixtyMode ? 1 : 0);
    t_entry = sceKernelGetSystemTimeLow();
    /* Triple Buffer (only at the 60 fps cap; with lower caps the pace is set by gfx_psp.c's sleep): the frame is presented in the thread, the CPU carries on */
    PSP_WD_PHASE("gfx_start_frame");
    gfx_start_frame();
    gPspPresentAsync = (gPspOptPipe && psp_opt_fps_cap() >= 60 && gPspSixtyMode) ? 1 : 0;
    t_net0 = sceKernelGetSystemTimeLow();
    psp_netplay_frame(); /* swaps the save file when a WLAN room starts / ends (before the game logic) */
    t_logic0 = sceKernelGetSystemTimeLow();
    PSP_WD_PHASE("game logic (game_loop_one_iteration)");
    game_loop_one_iteration();
    gFrame++;
    t_end0 = sceKernelGetSystemTimeLow();
    PSP_WD_PHASE("gfx_end_frame");
    gfx_end_frame();
    t_end1 = sceKernelGetSystemTimeLow();
    gPspPresentAsync = 0;
#if PSP_F2_SKIP
    if (gPspSixtyMode) {
        /* reference: the later of the tick start and the previous frame's vblank (with async presentation the tick
         * starts before the previous frame reaches the screen) */
        const unsigned int ref = ((int) (gPspPrevSwapT - t_entry) > 0) ? gPspPrevSwapT : t_entry;
        const int dt = (int) (gPspLastSwapT - ref);
        if (dt > (int) PSP_F2_MAX_FRAME1_US) {
            gPspF2Skip++;
            /* IMPORTANT: even if this frame is not drawn, the interpolation patches must be applied: besides rewriting the draw list
             * they empty the position tables (gMtxTblSize = 0, etc.). If not, the old entries point to a list that has already been
             * reused and the next patch writes over someone else's memory (hangs the GE). */
            patch_interpolations();
            gfx_psp_skip_frame_slot();
            gfx_psp_set_role(0);
            psp_netplay_perf_note(sceKernelGetSystemTimeLow() - t_entry, t_end0 - t_logic0, t_logic0 - t_net0, t_end1 - t_end0);
            return;
        }
    }
#endif
    if (gPspSixtyMode) {
        /* Second frame of the tick: same draw list, now with the real state's matrices. */
        gfx_psp_set_role(2);
        gfx_psp_replay_prepare(psp_f2_nonmatrix_pending()); /* BEFORE patch_interpolations(): it empties the matrix table */
        gfx_start_frame();
        patch_interpolations();
        gPspPresentAsync = ((gPspOptPipe && psp_opt_fps_cap() >= 60) || gPspAsyncWant || gfx_psp_replay_decided() || gPspReplayEma >= 600) ? 1 : 0; /* last frame of the tick: its sync + vblank are done in a thread while the next logic runs */
        send_display_list(gGfxSPTask);
        gfx_end_frame();
        gPspPresentAsync = 0;
        gfx_psp_set_role(0);
    }
    psp_netplay_perf_note(sceKernelGetSystemTimeLow() - t_entry, t_end0 - t_logic0, t_logic0 - t_net0, t_end1 - t_end0);
#else
    u32 num_audio_samples;
    int samples_left;

    gfx_start_frame();
    game_loop_one_iteration();
    gFrame++;

    if (gProcessAudio) {
        samples_left = audio_api->buffered();
        num_audio_samples = samples_left < audio_api->get_desired_buffered() ? SAMPLES_HIGH : SAMPLES_LOW;
        create_next_audio_buffer(audio_buffer, num_audio_samples);
        create_next_audio_buffer(audio_buffer + (num_audio_samples * 2), num_audio_samples);
        audio_api->play((u8 *)audio_buffer, 2 * num_audio_samples * sizeof(s16) * 2);
    }
    gfx_end_frame();
#endif
}

#ifdef TARGET_WEB
static void em_main_loop(void) {
}

static void request_anim_frame(void (*func)(double time)) {
    EM_ASM(requestAnimationFrame(function(time) {
        dynCall("vd", $0, [time]);
    }), func);
}

static void on_anim_frame(double time) {
    static double target_time;

    time *= 0.03; // milliseconds to frame count (33.333 ms -> 1)

    if (time >= target_time + 10.0) {
        // We are lagging 10 frames behind, probably due to coming back after inactivity,
        // so reset, with a small margin to avoid potential jitter later.
        target_time = time - 0.010;
    }

    for (int i = 0; i < 2; i++) {
        // If refresh rate is 15 Hz or something we might need to generate two frames
        if (time >= target_time) {
            produce_one_frame();
            target_time = target_time + 1.0;
        }
    }

    request_anim_frame(on_anim_frame);
}
#endif

static void save_config(void) {
    configfile_save(CONFIG_FILE);
}

static void on_fullscreen_changed(bool is_now_fullscreen) {
    configFullscreen = is_now_fullscreen;
}

void main_func(const char *argv0) {
#if defined(TARGET_PSP)
    (void)psp_me_boot();
    psp_setup_exit_callback();
#endif

    static u32 pool[0x165000/8 / 4 * sizeof(void *) * 4];
    main_pool_init(pool, pool + sizeof(pool) / sizeof(pool[0]));
    gEffectsMemoryPool = mem_pool_init(0x4000, MEMORY_POOL_LEFT);

    fs_init(argv0);
    configfile_load(CONFIG_FILE);
    atexit(save_config);
#ifdef TARGET_PSP
    psp_watchdog_start(); /* freeze report (freeze.txt): only writes if the game stops finishing frames */
    {
        extern int gPspOptTv; /* psp_options.c: TV Output (0 Off / 1 240p / 2 480p) */
        (void) psp_dve_boot_mode(gPspOptTv); /* decides BEFORE starting the video (switches VRAM to 4 MB if there is TV) */
        atexit(psp_dve_shutdown);
    }
#endif

#ifdef TARGET_WEB
    emscripten_set_main_loop(em_main_loop, 0, 0);
    request_anim_frame(on_anim_frame);
#endif

#if defined(ENABLE_DX12)
    rendering_api = &gfx_direct3d12_api;
    wm_api = &gfx_dxgi_api;
#elif defined(ENABLE_DX11)
    rendering_api = &gfx_direct3d11_api;
    wm_api = &gfx_dxgi_api;
#elif defined(ENABLE_OPENGL)
    rendering_api = &gfx_opengl_api;
    #if defined(__linux__) || defined(__BSD__)
        wm_api = &gfx_glx;
    #elif defined(TARGET_PSP)
        wm_api = &gfx_psp;
    #else
        wm_api = &gfx_sdl;
    #endif
#endif

    gfx_init(wm_api, rendering_api, "Super Mario 64 PC-Port", configFullscreen);
    
    wm_api->set_fullscreen_changed_callback(on_fullscreen_changed);
    wm_api->set_keyboard_callbacks(keyboard_on_key_down, keyboard_on_key_up, keyboard_on_all_keys_up);
    
#if HAVE_WASAPI
    if (audio_api == NULL && audio_wasapi.init()) {
        audio_api = &audio_wasapi;
    }
#endif
#if defined(TARGET_PSP)
    if (audio_api == NULL && audio_psp.init()) {
        audio_api = &audio_psp;
    }
    #endif
#if HAVE_PULSE_AUDIO
    if (audio_api == NULL && audio_pulse.init()) {
        audio_api = &audio_pulse;
    }
#endif
#if HAVE_ALSA
    if (audio_api == NULL && audio_alsa.init()) {
        audio_api = &audio_alsa;
    }
#endif
#ifdef TARGET_WEB
    if (audio_api == NULL && audio_sdl.init()) {
        audio_api = &audio_sdl;
    }
#endif
    if (audio_api == NULL) {
        audio_api = &audio_null;
    }

    audio_init();
    sound_init();

#if defined(TARGET_PSP)
    if (audio_api == &audio_psp) {
        audio_psp_start();
    }
#endif

    thread5_game_loop(NULL);
#ifdef TARGET_WEB
    /*for (int i = 0; i < atoi(argv[1]); i++) {
        game_loop_one_iteration();
    }*/
    inited = 1;
#else
    inited = 1;
    while (1) {
        wm_api->main_loop(produce_one_frame);
    }
#endif
}

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
int WINAPI WinMain(UNUSED HINSTANCE hInstance, UNUSED HINSTANCE hPrevInstance, UNUSED LPSTR pCmdLine, UNUSED int nCmdShow) {
    main_func(NULL);
    return 0;
}
#else
int main(int argc, char *argv[]) {
    main_func(argc > 0 ? argv[0] : NULL);
    return 0;
}
#endif
