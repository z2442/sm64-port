#ifndef PSP_OPTIONS_H
#define PSP_OPTIONS_H

/* Menu de opciones de la PSP (Video y Controles): se abre con R en la pantalla de pausa (ver psp_options.c).
 * Las variables se leen directamente donde hacen falta. Ningun valor del menu deja el juego peor que el original. */

#ifdef TARGET_PSP

/* Limite de fps (30 / 60) y modo: 0 = "Keep FPS" (siempre intenta el limite; si la escena no cabe, el juego va mas lento),
 * 1 = "Keep Speed" (baja los fps de los ticks que no caben para que el juego mantenga su velocidad) */
extern int gPspOptFpsCap;
extern int gPspOptKeepSpeed;
extern int gPspOptPipe;        /* Triple Buffer: 1 = presentacion en hilo aparte con 3 buffers de color (ver gfx_scegu.c) */
extern float gPspObjDistScale; /* multiplica la distancia a la que los objetos se desactivan (1.0 = original, nunca menos) */
extern float gPspFarScale;     /* multiplica el plano lejano de la camara (1.0 = original, nunca menos) */
extern int gPspOptCullObj;     /* 0 = culling de objetos original (solo horizontal), 1 = ademas vertical (ver obj_is_in_view) */
extern int gPspClockWant;      /* MHz de CPU pedidos (fijo en 333) */
extern unsigned int gPspOptChanges; /* cambios hechos en el menu desde que empezo el juego */

extern float gPspVolMusic;     /* volumen de la musica, 0.0 a 1.0 (1.0 = el original); lo aplica effects.c a los reproductores de musica */
extern float gPspVolSfx;       /* volumen de los efectos de sonido, 0.0 a 1.0 */

/* Archivo unico de ajustes (graficos, audio y controles): configfile_load / configfile_save lo llaman en la PSP. */
void psp_options_load_file(const char *filename);
void psp_options_save_file(const char *filename);

/* Se llama una vez por cuadro (render_fps): carga el archivo de opciones la primera vez. */
void psp_options_poll(void);
/* Se llama al principio de render_pause_courses_and_castle(). Devuelve 1 mientras el menu esta abierto (hay que saltarse la pausa normal). */
int psp_options_pause_frame(int can_open);
/* Se llama justo despues de dibujar la pausa normal: escribe "R Opciones" en una esquina. */
void psp_options_pause_prompt(void);
/* Dibuja "SOUND: CPU/ME" abajo a la izquierda si la opcion "Show Sound CPU" esta activada (se llama en las dos pausas). */
void psp_options_sound_info(void);

#endif

/* Distancias de objetos del menu: PSP_OBJ_DIST(d) = d * "Object Distance" (100 % = el valor original del juego, nunca menos). Se usa en TODOS
 * los sitios del juego que esconden un objeto por estar lejos de Mario (behavior_script, modelos de colision, set_object_visibility...). */
#ifdef TARGET_PSP
#define PSP_OBJ_DIST(d) ((d) * gPspObjDistScale)

/* Objetos que el juego solo CREA cuando Mario esta cerca (goombas, cadena del chomp, monedas en fila...): con Object Distance > 100 % se crean desde
 * mas lejos. La reserva de objetos del juego es de 240 y si se llena el juego se cuelga, asi que la zona ampliada solo se usa si despues de crear
 * n objetos quedan libres al menos 40 (lo importante) o 100 (monedas). Dentro de la distancia original se crean siempre, igual que antes. */
int psp_objects_free(void);
int psp_spawn_headroom(int n);    /* generadores importantes (chain chomp, goombas, canones...): quedan al menos 40 libres tras crear n */
int psp_spawn_headroom_lo(int n); /* generadores de poca prioridad (lineas de monedas): solo con la reserva holgada (quedan al menos 100 libres) */
void psp_pool_update(void);  /* una vez por tick (update_objects): cuenta los objetos libres */
extern float gPspSpawnScale;   /* multiplicador de distancia de los generadores importantes: Object Distance con la reserva holgada, baja hacia 1.0 al quedar pocos libres */
extern float gPspSpawnScaleLo; /* idem para lo de poca prioridad (monedas): empieza a bajar antes, asi que se descarta primero */
extern int gPspFreeMin;      /* minimo de objetos libres desde la ultima lectura (el registro lo imprime y lo reinicia) */
/* Distancia a la que un generador descarga lo que creo. Cuando la reserva se va llenando la distancia NO se corta de golpe: baja poco a poco desde la
 * ampliada hasta la original (psp_pool_update), asi lo que se descarta primero es lo MAS LEJANO y lo cercano se queda. */
#define PSP_SPAWN_DIST(d) ((d) * gPspSpawnScale)
static inline int psp_obj_near(float dist, float base, int n) {
    return dist < base || (dist < base * gPspSpawnScale && psp_spawn_headroom(n));
}
/* Igual, para lo que menos importa (lineas de monedas): su distancia empieza a bajar con mas objetos libres, asi que se recorta antes que lo importante. */
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
