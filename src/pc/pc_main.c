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

#include "compat.h"

#if defined(TARGET_PSP)
#include <pspsdk.h>
#include <pspkernel.h>
#include <psppower.h>
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
/* Interpolacion de cuadros (60 fps dinamico). La lista de dibujo se construye con matrices interpoladas; estas funciones
 * la reescriben para que apunte a las matrices del estado real (ver parche 60fps de sm64ex). */
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
/* PSP_F2_REPLAY: parches que NO son solo "cambiar matrices de objetos" (vertices, escala del titulo, HUD, dialogo...).
 * Si alguno esta pendiente el 2o cuadro vuelve a emitir la escena completa (como antes de la etapa 2). */
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
}
#endif

void send_display_list(struct SPTask *spTask) {
    if (!inited) {
        return;
    }
#if defined(TARGET_PSP)
    /* Modo 30 fps: un solo cuadro con el estado real -> se parchea ya. Modo 60 fps: este primer cuadro sale
     * interpolado y el parche se aplica antes del segundo cuadro (ver produce_one_frame). */
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
/* Menu HOME nativo de la PSP: se registra un callback de salida (sin esto el boton HOME no hace nada).
 * Al confirmar "Salir del juego" el sistema llama a este callback. */
static int psp_exit_callback(UNUSED int arg1, UNUSED int arg2, UNUSED void *common) {
    save_config();
    audio_psp_shutdown();
    sceKernelExitGame();
    return 0;
}

extern void gfx_scegu_power_event(int suspending); /* gfx_scegu.c */
static int psp_power_callback(UNUSED int unknown, int flags, UNUSED void *common) {
    psp_me_note_flags(flags);
    if (flags & (PSP_POWER_CB_SUSPENDING | PSP_POWER_CB_POWER_SWITCH | PSP_POWER_CB_STANDBY)) {
        if (!(flags & (PSP_POWER_CB_RESUMING | PSP_POWER_CB_RESUME_COMPLETE))) {
            psp_me_power_event(1); /* el ME tiene que estar parado antes de que la consola se duerma */
        }
        gfx_scegu_power_event(1);
    }
    if (flags & (PSP_POWER_CB_RESUME_COMPLETE | PSP_POWER_CB_RESUMING)) {
        gfx_scegu_power_event(0);
    }
    if (flags & PSP_POWER_CB_RESUME_COMPLETE) {
        psp_me_power_event(0); /* vuelve a arrancar el ME (si se detuvo bien antes de dormir) */
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
    int thid = sceKernelCreateThread("exit_callback_thread", psp_callback_thread, 0x11, 0xFA0, 0, NULL);
    if (thid >= 0) {
        sceKernelStartThread(thid, 0, NULL);
    }
}
#endif

extern int gPspPresentAsync; /* gfx_scegu.c */
extern int gPspOptPipe, gPspOptFpsCap; /* game/psp_options.c */
extern int gPspAsyncWant;    /* gfx_pc.c: 1 si la cola del GE es lo bastante larga como para que compense presentar en asincrono */

extern unsigned int gPspLastSwapT, gPspPrevSwapT; /* gfx_scegu.c: hora de los dos ultimos vblank con cuadro presentado */
extern void gfx_psp_skip_frame_slot(void);
extern void gfx_psp_set_role(int role); /* gfx_pc.c: 0 = normal, 1 = 1er cuadro de un tick de 60 fps (se graba), 2 = 2o cuadro (se repite) */
unsigned int gPspF2Skip; /* ticks de 60 fps cuyo 2o cuadro se descarto porque el 1o se paso de un vblank */
/* Si el 1er cuadro de un tick de 60 fps tardo mas de ~1,5 vblank desde que empezo el tick (la logica y el dibujado no cupieron en
 * 16,7 ms), el 2o cuadro caeria en un 3er vblank y el tick duraria 50 ms (juego a 20 Hz = camara lenta). En ese caso se omite. */
#define PSP_F2_MAX_FRAME1_US 25000U

void produce_one_frame(void) {
#if defined(TARGET_PSP)
    unsigned int t_entry;
    gfx_psp_update_mode(); /* decide si este tick presenta 1 cuadro (30 fps) o 2 (60 fps) */
    gfx_psp_set_role(gPspSixtyMode ? 1 : 0);
    t_entry = sceKernelGetSystemTimeLow();
    /* Triple Buffer (solo a 60 fps de tope; con topes menores el ritmo lo marca el sueno de gfx_psp.c): el cuadro se presenta en el hilo, la CPU sigue */
    gfx_start_frame();
    gPspPresentAsync = (gPspOptPipe && gPspOptFpsCap >= 60 && gPspSixtyMode) ? 1 : 0;
    game_loop_one_iteration();
    gFrame++;
    gfx_end_frame();
    gPspPresentAsync = 0;
#if PSP_F2_SKIP
    if (gPspSixtyMode) {
        /* referencia: el mas tardio entre el inicio del tick y el vblank del cuadro anterior (con presentacion asincrona el tick
         * empieza antes de que el cuadro anterior llegue a pantalla) */
        const unsigned int ref = ((int) (gPspPrevSwapT - t_entry) > 0) ? gPspPrevSwapT : t_entry;
        const int dt = (int) (gPspLastSwapT - ref);
        if (dt > (int) PSP_F2_MAX_FRAME1_US) {
            gPspF2Skip++;
            /* IMPORTANTE: aunque este cuadro no se dibuje, hay que aplicar los parches de interpolacion: ademas de reescribir la lista
             * de dibujo, vacian las tablas de posiciones (gMtxTblSize = 0, etc.). Si no, las entradas viejas apuntan a una lista ya
             * reutilizada y el siguiente parche escribe sobre memoria ajena (cuelga el GE). */
            patch_interpolations();
            gfx_psp_skip_frame_slot();
            gfx_psp_set_role(0);
            return;
        }
    }
#endif
    if (gPspSixtyMode) {
        /* Segundo cuadro del tick: misma lista de dibujo, ahora con las matrices del estado real. */
        gfx_psp_set_role(2);
        gfx_psp_replay_prepare(psp_f2_nonmatrix_pending()); /* ANTES de patch_interpolations(): esta vacia la tabla de matrices */
        gfx_start_frame();
        patch_interpolations();
        gPspPresentAsync = ((gPspOptPipe && gPspOptFpsCap >= 60) || gPspAsyncWant || gfx_psp_replay_decided() || gPspReplayEma >= 600) ? 1 : 0; /* ultimo cuadro del tick: su sync + vblank se hacen en un hilo mientras corre la logica siguiente */
        send_display_list(gGfxSPTask);
        gfx_end_frame();
        gPspPresentAsync = 0;
        gfx_psp_set_role(0);
    }
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
