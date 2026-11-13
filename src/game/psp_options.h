#ifndef PSP_OPTIONS_H
#define PSP_OPTIONS_H

/* PSP options menu (Video and Controls): opens with R on the pause screen (see psp_options.c).
 * The variables are read directly where they are needed. No menu value leaves the game worse than the original. */

#ifdef TARGET_PSP

/* Fps limit (30 / 60) and mode: 0 = "Keep FPS" (always tries for the limit; if the scene does not fit, the game runs slower),
 * 1 = "Keep Speed" (lowers the fps of the ticks that do not fit so the game keeps its speed) */
extern int gPspOptFpsCap;
int psp_opt_fps_cap(void); /* the limit in force (30 while online) */
extern int gPspOptKeepSpeed;
extern int gPspOptPipe;        /* Triple Buffer: 1 = presentation in a separate thread with 3 color buffers (see gfx_scegu.c) */
extern float gPspObjDistScale; /* multiplies the distance at which objects are deactivated (1.0 = original, never less) */
extern float gPspFarScale;     /* multiplies the camera's far plane (1.0 = original, never less) */
extern int gPspOptCullObj;     /* 0 = original object culling (horizontal only), 1 = also vertical (see obj_is_in_view) */
extern int gPspClockWant;      /* CPU MHz requested (fixed at 333) */
extern unsigned int gPspOptChanges; /* changes made in the menu since the game started */

extern float gPspVolMusic;     /* music volume, 0.0 to 1.0 (1.0 = the original); applied by effects.c to the music players */
extern float gPspVolSfx;       /* sound effects volume, 0.0 to 1.0 */

/* Single settings file (graphics, audio and controls): configfile_load / configfile_save call it on the PSP. */
void psp_options_load_file(const char *filename);
void psp_options_save_file(const char *filename);

/* Called once per frame (render_fps): loads the options file the first time. */
void psp_options_poll(void);
/* Called at the start of render_pause_courses_and_castle(). Returns 1 while the menu is open (the normal pause must be skipped). */
int psp_options_pause_frame(int can_open);
/* Called right after drawing the normal pause: writes "R Options" in a corner. */
void psp_options_pause_prompt(void);
void psp_options_home_notice(void); /* HOME with the TV active: buzz + notice at the bottom center (called every frame) */
/* Draws "SOUND: CPU/ME" at the bottom left if the "Show Sound CPU" option is on (called on both pauses). */
void psp_options_sound_info(void);

#endif

/* Menu object distances: PSP_OBJ_DIST(d) = d * "Object Distance" (100 % = the game's original value, never less). Used in ALL
 * the places in the game that hide an object for being far from Mario (behavior_script, collision models, set_object_visibility...). */
#ifdef TARGET_PSP
#define PSP_OBJ_DIST(d) ((d) * gPspObjDistScale)

/* Objects that the game only CREATES when Mario is near (goombas, chomp chain, rows of coins...): with Object Distance > 100 % they are created from
 * farther away. The game's object pool is 240 and if it fills up the game hangs, so the enlarged zone is only used if after creating
 * n objects at least 40 remain free (the important ones) or 100 (coins). Within the original distance they are always created, as before. */
int psp_objects_free(void);
int psp_spawn_headroom(int n);    /* important spawners (chain chomp, goombas, cannons...): at least 40 left free after creating n */
int psp_spawn_headroom_lo(int n); /* low priority spawners (coin lines): only with a comfortable pool (at least 100 left free) */
void psp_pool_update(void);  /* once per tick (update_objects): counts the free objects */
extern float gPspSpawnScale;   /* distance multiplier for the important spawners: Object Distance with a comfortable pool, goes down towards 1.0 when few are free */
extern float gPspSpawnScaleLo; /* same for low priority things (coins): starts going down earlier, so it is dropped first */
extern int gPspFreeMin;      /* minimum of free objects since the last read (the log prints and resets it) */
/* Distance at which a spawner unloads what it created. As the pool fills up the distance is NOT cut all at once: it goes down gradually from the
 * enlarged one to the original (psp_pool_update), so what is dropped first is what is FARTHEST and the nearby stays. */
#define PSP_SPAWN_DIST(d) ((d) * gPspSpawnScale)
static inline int psp_obj_near(float dist, float base, int n) {
    return dist < base || (dist < base * gPspSpawnScale && psp_spawn_headroom(n));
}
/* Same, for what matters least (coin lines): its distance starts going down with more free objects, so it is trimmed before the important ones. */
#define PSP_SPAWN_DIST_LO(d) ((d) * gPspSpawnScaleLo)
static inline int psp_obj_near_lo(float dist, float base, int n) {
    return dist < base || (dist < base * gPspSpawnScaleLo && psp_spawn_headroom_lo(n));
}
#else
#define PSP_OBJ_DIST(d) (d)
#define PSP_SPAWN_DIST(d) (d)
#define psp_obj_near(dist, base, n) ((dist) < (base))
#define PSP_SPAWN_DIST_LO(d) (d)
#define psp_obj_near_lo(dist, base, n) ((dist) < (base))
#endif

#endif
