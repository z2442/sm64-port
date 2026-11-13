#ifndef PSP_DVE_H
#define PSP_DVE_H

/* TV video output (PSP slim/3000 with video cable). Everything is optional: with a PSP-1000 or without the module the game comes out on the
 * console screen exactly as always. The setting is applied immediately (no restart). */

enum { PSP_TV_OFF = 0, PSP_TV_240P = 1, PSP_TV_480I = 2, PSP_TV_480P = 3 }; /* 480i works with a composite or component cable; 480p only with component */
enum { PSP_CABLE_NONE = 0, PSP_CABLE_COMPOSITE = 1, PSP_CABLE_COMPONENT = 2 };

extern int gPspTvActive; /* TV mode in use (PSP_TV_*); 0 = console screen. Decided by psp_dve_boot_mode() before starting the video. */

/* Loads the module (only once). Returns 1 if there is a module and the console is not a PSP-1000. */
int psp_dve_probe(void);
/* Cable type to use to route the TV (PSP_CABLE_*): composite only if detected; in any other case (component, none, no data) component. */
int psp_dve_cable(void);
/* Reason why the "TV Output" row is not available (shown in the menu). detail: error code / model / raw cable value. */
enum { PSP_DVE_READY = 0, PSP_DVE_NOT_PROBED, PSP_DVE_LOAD_FAILED, PSP_DVE_PSP1000, PSP_DVE_SAFE };
int psp_dve_status(int *detail);
/* Real state of the video cable: -1 = unknown, 0 = none, 1 = composite, 2 = component. Cheap: can be called every few frames. */
int psp_dve_cable_state(void);
/* System native TV mode (Display button): -1 = unknown, 0 = off, 1 = on, 2 = changing. */
int psp_dve_native_state(void);
/* 1 if the menu's "TV Output" row can be changed (module loaded + PSP slim + 4 MB VRAM). The cable is NOT required: the console does not always report it. */
int psp_dve_tv_ready(void);
/* Before starting the video (on a slim): loads the module and requests 4 MB of VRAM; returns the TV mode to boot with (0 = console screen). */
int psp_dve_boot_mode(int wanted);
/* Routes the output (mode = PSP_TV_240P / PSP_TV_480P; 0 = back to the console screen). Returns <0 if it fails. */
int psp_dve_set_video_out(int mode);
/* Once per frame: after a few seconds without problems the "TV boot in progress" marker is deleted. */
void psp_dve_frame_tick(void);
/* "TV attempt in progress" marker (tv_boot.tmp): created before routing the TV and deleted after ~5 s with TV (or when it is turned off). */
void psp_dve_mark_attempt(void);
void psp_dve_mark_clear(void);
void psp_dve_shutdown(void);
/* 1 = sound through the console speakers even if a video cable is plugged in (TV Output at Off); 0 = system automatic mode. Only changes if needed. */
void psp_dve_audio_route(int want);
/* Boot on the console screen: if the system left its TV mode on, it is put back to normal mode (before starting the video). */
void psp_dve_reset_lcd_if_tv(void);
/* After waking up from sleep: restores the 4 MB of VRAM if the system lowered it. 1 = the TV can still be used, 0 = not until restart. */
int psp_dve_resume(void);
/* The cable watcher latches the cable state (1 composite / 2 component) once it is stable; 0 / negative clears it. Used to route the TV. */
void psp_dve_cable_latch(int state);

/* psp_options.c */
void psp_options_tv_off(void); /* sets "TV Output" to Off and saves the file */
void psp_options_tv_set(int mode); /* sets "TV Output" to that value for this session only (without saving) */

#endif
