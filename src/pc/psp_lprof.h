#ifndef PSP_LPROF_H
#define PSP_LPROF_H

/* Perfil de la logica del juego (PSP). Solo mide mientras el registro esta activo (Triangulo + R); el resultado sale en
 * ms0:/sm64_profile.txt junto con el resto de columnas (ver prof_log_window en gfx_pc.c). Cuando el registro esta apagado
 * cada macro es una comparacion con cero. */
#ifdef TARGET_PSP
extern int sProfLogOn;
extern unsigned int gPspLp[32]; /* microsegundos acumulados por ranura (ver LP_*) */
extern unsigned int gPspLc[16]; /* contadores (ver LC_*) */
extern unsigned int sceKernelGetSystemTimeLow(void);

enum {
    LP_AUD, LP_INP, LP_SCR, LP_UPD, LP_OBJ, LP_TERR, LP_COLL, LP_NTER, LP_CAM, LP_MAR, LP_FLR, LP_WAL, LP_CEL, LP_RND,
    LP_GEO,
    LP_FN,   /* callbacks de funciones del grafo (switch, camara, generadores, fondo...) */
    LP_ANI,  /* geo_set_animation_globals */
    LP_SHD,  /* geo_process_shadow (sin hijos) */
    LP_MLS,  /* geo_process_master_list_sub (construye las listas por capa) */
    LP_VIEW, /* obj_is_in_view */
    LP_COUNT
};
enum { LC_TICKS, LC_FLR, LC_WAL, LC_CEL, LC_OBJ, LC_GEOOBJ, LC_GEONODE,
       LC_NMTX,  /* nodos de matriz (traslacion, rotacion, escala, billboard) */
       LC_NANIM, /* partes animadas */
       LC_NDL,   /* nodos de display list */
       LC_NSHD,  /* sombras */
       LC_NFN,   /* llamadas a callbacks */
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
