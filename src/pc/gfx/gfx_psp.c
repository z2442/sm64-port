#if defined(TARGET_PSP)
#include <pspsdk.h>
#include <pspdisplay.h>
#include <stdio.h>
#include <GLES/egl.h>
#include <GLES/gl.h>
#include "gfx_window_manager_api.h"
#include "gfx_screen_config.h"

int exitCallback(int arg1, int arg2, void *common)
{
	//exitRequest = 1;
	return 0;
}

int callbackThread(SceSize args, void *argp)
{
	int cbid;

	cbid = sceKernelCreateCallback("Exit Callback", exitCallback, NULL);
	sceKernelRegisterExitCallback(cbid);

	sceKernelSleepThreadCB();

	return 0;
}

int setupCallbacks(void)
{
	int thid = 0;

	thid = sceKernelCreateThread("update_thread", callbackThread, 0x11, 0xFA0, 0, 0);
	if(thid >= 0)
	{
		sceKernelStartThread(thid, 0, 0);
	}

	return thid;
}

static EGLDisplay dpy;
static EGLContext ctx;
static EGLSurface surface;
static const int screen_attrib[] = {
	EGL_RED_SIZE,		8,
	EGL_GREEN_SIZE,		8,
	EGL_BLUE_SIZE,		8,
	EGL_ALPHA_SIZE,		0,
	EGL_STENCIL_SIZE,	0,
	EGL_DEPTH_SIZE,		0,
	EGL_NONE
};

static const int pbuffer_attrib[] = {
	EGL_RED_SIZE,	8,
	EGL_GREEN_SIZE,	8,
	EGL_BLUE_SIZE,	8,
	EGL_ALPHA_SIZE,	8,
	EGL_DEPTH_SIZE,	16,

	EGL_NONE
};

static EGLConfig screen_config, pbuffer_config;
static EGLSurface screen;

void gfx_psp_init(const char *game_name, bool start_in_fullscreen) {
    fprintf(stderr, "%s called for %s, should %sstart in fullscreen\n", __FUNCTION__, game_name, start_in_fullscreen ? "" : "not ");
    fprintf(stderr, "setupCallbacks() returned %d\n", setupCallbacks());
    pspDebugScreenSetBackColor(0xFF0000FF);
    pspDebugScreenInitEx(NULL, PSP_DISPLAY_PIXEL_FORMAT_8888, 1);
    dpy = eglGetDisplay(0);
    eglInitialize(dpy, NULL, NULL);
    ctx = eglCreateContext(dpy, screen_config, EGL_NO_CONTEXT, NULL);
    screen = eglCreateWindowSurface(dpy, screen_config, 0, NULL);
    eglMakeCurrent(dpy, screen, screen, ctx);
}

void gfx_psp_set_keyboard_callbacks(bool (*on_key_down)(int scancode), bool (*on_key_up)(int scancode), void (*on_all_keys_up)(void)) {
    (void)on_key_down;
    (void)on_key_up;
    (void)on_all_keys_up;
}

void gfx_psp_set_fullscreen_changed_callback(void (*on_fullscreen_changed)(bool is_now_fullscreen)) {
    fprintf(stderr, "received fullscreen callback at %p\n", on_fullscreen_changed);
}

void gfx_psp_set_fullscreen(bool enable) {
    // we only support fullscreen, do nothing
    fprintf(stderr, "Ignoring fullscreen = %d\n", enable);
}

void gfx_psp_main_loop(void (*run_one_game_iter)(void)) {
    (void)run_one_game_iter;
}

void gfx_psp_get_dimensions(uint32_t *width, uint32_t *height) {
	fprintf(stderr, "%s called\n", __FUNCTION__);
	*width  = 480;
	*height = 272;
}

void gfx_psp_handle_events(void) {
	fprintf(stderr, "%s called\n", __FUNCTION__);
}

bool gfx_psp_start_frame(void) {
	fprintf(stderr, "%s called\n", __FUNCTION__);
}

void gfx_psp_swap_buffers_begin(void) {
	fprintf(stderr, "%s called\n", __FUNCTION__);
}

void gfx_psp_swap_buffers_end(void) {
	fprintf(stderr, "%s called\n", __FUNCTION__);
}

double gfx_psp_get_time(void) {
	fprintf(stderr, "%s called\n", __FUNCTION__);

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
