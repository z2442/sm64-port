#ifndef PSP_LPROF_H
#define PSP_LPROF_H

/* Game logic profile (PSP). It only measures while the log is active (Triangle + R); the result goes to
 * ms0:/sm64_profile.txt together with the rest of the columns (see prof_log_window in gfx_pc.c). When the log is off
 * each macro is a comparison with zero. */
#ifdef TARGET_PSP
extern int sProfLogOn;
extern unsigned int gPspLp[32]; /* microseconds accumulated per slot (see LP_*) */
extern unsigned int gPspLc[16]; /* counters (see LC_*) */
extern unsigned int sceKernelGetSystemTimeLow(void);

enum {
    LP_AUD, LP_INP, LP_SCR, LP_UPD, LP_OBJ, LP_TERR, LP_COLL, LP_NTER, LP_CAM, LP_MAR, LP_FLR, LP_WAL, LP_CEL, LP_RND,
    LP_GEO,
    LP_FN,   /* callbacks of graph functions (switch, camera, spawners, background...) */
    LP_ANI,  /* geo_set_animation_globals */
    LP_SHD,  /* geo_process_shadow (without children) */
    LP_MLS,  /* geo_process_master_list_sub (builds the per-layer lists) */
    LP_VIEW, /* obj_is_in_view */
    LP_COUNT
};
enum { LC_TICKS, LC_FLR, LC_WAL, LC_CEL, LC_OBJ, LC_GEOOBJ, LC_GEONODE,
       LC_NMTX,  /* matrix nodes (translation, rotation, scale, billboard) */
       LC_NANIM, /* animated parts */
       LC_NDL,   /* display list nodes */
       LC_NSHD,  /* shadows */
       LC_NFN,   /* calls to callbacks */
       LC_COUNT };

#define LPROF_BEGIN(v) unsigned int v = sProfLogOn ? sceKernelGetSystemTimeLow() : 0u
#define LPROF_END(v, slot) do { if (v) { gPspLp[slot] += sceKernelGetSystemTimeLow() - (v); } } while (0)
#define LPROF_COUNT(slot) (gPspLc[slot]++)
#define LPROF_ADD(slot, n) (gPspLc[slot] += (unsigned int)(n))
#else
#define LPROF_BEGIN(v) do { } while (0)
#define LPROF_END(v, slot) do { } while (0)
#define LPROF_COUNT(slot) do { } while (0)
#define LPROF_ADD(slot, n) do { } while (0)
#endif

#endif
