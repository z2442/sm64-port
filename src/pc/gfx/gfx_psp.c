#if 0
//#if defined(TARGET_PSP)
#include <pspsdk.h>
#include <pspdisplay.h>
#include <pspkernel.h>
#include <stdio.h>
#include <GL/glut.h>
#include <GLES/egl.h>
#include <GLES/gl.h>
#include "gfx_window_manager_api.h"
#include "gfx_screen_config.h"

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)
#define INFO_MSG(x) printf("%s %s\n", __FILE__ ":" TOSTRING(__LINE__), x)

//static int exitRequest = 0;
int exitCallback(int arg1, int arg2, void *common)
{
	(void)arg1;
	(void)arg2;
	(void)common;
	//exitRequest = 1;
	sceKernelExitGame();
	return 0;
}

int callbackThread(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
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
static EGLint width = 480;
static EGLint height = 272;
static unsigned int glut_display_mode = 0;

static EGLint attrib_list[] = {
    EGL_RED_SIZE, 8,     /* 0 */
    EGL_GREEN_SIZE, 8,   /* 2 */
    EGL_BLUE_SIZE, 8,    /* 4 */
    EGL_ALPHA_SIZE, 0,   /* 6 */
    EGL_STENCIL_SIZE, 0, /* 8 */
    EGL_DEPTH_SIZE, 0,   /* 10 */
    EGL_NONE};

void gfx_psp_init(const char *game_name, bool start_in_fullscreen) {
  (void)game_name;
  (void)start_in_fullscreen;
  EGLConfig config;
  EGLint num_configs;

  
  setupCallbacks();

  /* pass NativeDisplay=0, we only have one screen... */
  dpy = eglGetDisplay(0);
  eglInitialize(dpy, NULL, NULL);

#if 0
	psp_log("EGL vendor \"%s\"\n", eglQueryString(dpy, EGL_VENDOR));
	psp_log("EGL version \"%s\"\n", eglQueryString(dpy, EGL_VERSION));
	psp_log("EGL extensions \"%s\"\n", eglQueryString(dpy, EGL_EXTENSIONS));
#endif

  /* Select type of Display mode:   
     Double buffer 
     RGBA color
     Alpha components supported 
     Depth buffered for automatic clipping */
  glut_display_mode = (GLUT_RGBA | GLUT_DOUBLE | GLUT_ALPHA | GLUT_DEPTH);

  if (glut_display_mode & GLUT_ALPHA)
    attrib_list[7] = 8;
  if (glut_display_mode & GLUT_STENCIL)
    attrib_list[9] = 8;
  if (glut_display_mode & GLUT_DEPTH)
    attrib_list[11] = 16;

  eglChooseConfig(dpy, attrib_list, &config, 1, &num_configs);

  if (num_configs == 0) {
    fprintf(stderr, "glutCreateWindow: eglChooseConfig returned no configurations for display mode %x\n",
                glut_display_mode);
  }

#if 0
	fprintf(stderr, ("eglChooseConfig() returned config 0x%04x\n", (unsigned int)config);
#endif
  eglGetConfigAttrib(dpy, config, EGL_WIDTH, &width);
  eglGetConfigAttrib(dpy, config, EGL_HEIGHT, &height);

  ctx = eglCreateContext(dpy, config, NULL, NULL);
  surface = eglCreateWindowSurface(dpy, config, 0, NULL);
  eglMakeCurrent(dpy, surface, surface, ctx);
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
	while (1) {
		run_one_game_iter();
	}
}

void gfx_psp_get_dimensions(uint32_t *width, uint32_t *height) {
	fprintf(stderr, "%s called\n", __FUNCTION__);
	*width  = 480;
	*height = 272;
}

void gfx_psp_handle_events(void) {
#if 0
	fprintf(stderr, "%s called\n", __FUNCTION__);
#endif
}

bool gfx_psp_start_frame(void) {
#if 0
	fprintf(stderr, "%s called\n", __FUNCTION__);
#endif
}

void gfx_psp_swap_buffers_begin(void) {
#if 0
	fprintf(stderr, "%s called\n", __FUNCTION__);
#endif
}

void gfx_psp_swap_buffers_end(void) {
#if 0
	fprintf(stderr, "%s called\n", __FUNCTION__);
#endif
  sceDisplayWaitVblankStart();
	eglSwapBuffers(dpy, surface);
}

double gfx_psp_get_time(void) {
#if 0
	fprintf(stderr, "%s called\n", __FUNCTION__);
#endif
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
