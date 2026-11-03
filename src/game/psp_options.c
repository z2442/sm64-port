#ifdef TARGET_PSP

/* Menu de opciones de la PSP (graficos, audio y controles).
 *
 * Disenado igual que el menu de opciones de sm64rt / sm64ex: se abre con R en la pantalla de pausa (la pausa sigue de fondo, oscurecida),
 * titulo grande arriba, de a 4 opciones con la seleccionada en roja y el valor debajo, flechas a los lados y una barra de desplazamiento.
 * Pagina principal: Graphics / Audio / Controls. Texto con la misma fuente del juego (la de los dialogos).
 * Entradas (siempre los botones FISICOS de la PSP, para que remapear los controles nunca deje el menu inutilizable):
 *   moverse: stick o cruceta arriba / abajo; cambiar el valor: izquierda / derecha o X (cruz); volver: cuadrado o circulo; cerrar: START o R.
 * En Controls, X sobre un boton pide "Press a button" y asigna el siguiente boton que se pulse (si ese boton ya lo usaba otra accion, se intercambian);
 * si no se pulsa nada en unos 5 s se cancela.
 * TODOS los ajustes (graficos, audio y controles) se guardan en un solo archivo de texto legible, sm64config.txt, al cerrar el menu y al salir del juego;
 * se cargan al iniciar (configfile_load los pide a psp_options_load_file). Tambien se puede editar a mano. Los archivos antiguos (sm64config.txt solo con
 * controles numericos + sm64_options.txt) se leen y se convierten al formato nuevo la primera vez. */

#include <ultra64.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <psppower.h>
#include <pspctrl.h>

#include "sm64.h"
#include "audio/external.h"
#include "game_init.h"
#include "gfx_dimensions.h"
#include "ingame_menu.h"
#include "print.h"
#include "segment2.h"
#include "psp_options.h"
#include "save_file.h"
#include "sound_init.h"
#include "pc/fs_utils.h"
#include "pc/psp_me.h"
#include "pc/configfile.h"

void create_dl_scale_matrix(s8 pushOp, f32 x, f32 y, f32 z); /* ingame_menu.c */
void shade_screen(void);                                      /* ingame_menu.c */
extern int gPspFpsLogOff;                                    /* game_init.c: en la version final siempre 1 (el registro de rendimiento esta apagado) */
extern u8 gRenderFPS;                                        /* game_init.c: contador de FPS (y registro de rendimiento) */
extern unsigned int gPspRawButtons;                          /* controller_psp.c: botones fisicos de la PSP (PSP_CTRL_*) */
extern int gPspRawStickX, gPspRawStickY;                     /* controller_psp.c: stick fisico, -128..127, arriba y derecha positivos */

#define OPT_FILE "sm64_options.txt" /* formato antiguo (solo se lee, para migrar) */
#define CTRL_FILE "sm64config.txt"  /* el archivo unico de ajustes (el mismo que usa pc_main.c, CONFIG_FILE) */

enum {
    OPT_FPSCAP,
    OPT_PACING,
    OPT_SHOWFPS,
    OPT_SOUNDCPU,
    OPT_OBJDIST,
    OPT_FARDIST,
    OPT_CULLOBJ,
    OPT_RESET,
    OPT_COUNT
};

/* Filas que no son de video (los indices empiezan donde acaban los OPT_*, asi los arreglos de arriba no cambian). */
#define NUM_BINDS 10
enum {
    ROW_VIDEO = OPT_COUNT, /* entrada a la pagina Graphics (video) */
    ROW_AUDIO,             /* entrada a la pagina Audio */
    ROW_CONTROLS,          /* entrada a la pagina Controls */
    ROW_BIND0,             /* ROW_BIND0 .. ROW_BIND0 + NUM_BINDS - 1: un boton del N64 cada una */
    ROW_DEAD = ROW_BIND0 + NUM_BINDS,
    ROW_CTRLRESET,
    ROW_MUSIC,    /* volumen de la musica (0 a 100) */
    ROW_SFX,      /* volumen de los efectos de sonido (0 a 100) */
    ROW_SOUNDMODE, /* Stereo / Mono / Headset: el mismo ajuste del menu de seleccion de archivo */
    ROW_AUDRESET
};

enum { PG_MAIN, PG_VIDEO, PG_AUDIO, PG_CTRL };

static const u8 sPgMain[] = { ROW_VIDEO, ROW_AUDIO, ROW_CONTROLS };
static const u8 sPgAudio[] = { ROW_MUSIC, ROW_SFX, ROW_SOUNDMODE, ROW_AUDRESET };
static const u8 sPgVideo[] = { OPT_FPSCAP, OPT_PACING, OPT_OBJDIST, OPT_FARDIST, OPT_CULLOBJ, OPT_SHOWFPS, OPT_SOUNDCPU, OPT_RESET };
static const u8 sPgCtrl[] = { ROW_BIND0, ROW_BIND0 + 1, ROW_BIND0 + 2, ROW_BIND0 + 3, ROW_BIND0 + 4, ROW_BIND0 + 5, ROW_BIND0 + 6,
                              ROW_BIND0 + 7, ROW_BIND0 + 8, ROW_BIND0 + 9, ROW_DEAD, ROW_CTRLRESET };
/* Titulos con la fuente de colores del HUD: en la version US solo existen las letras A-I, K-P, R-U, W, Y (no hay J, Q, V, X ni Z) y una letra que falta
 * cuelga el dibujo; por eso la pagina de video se llama "Graphics" y no "Video". */
static const char *const sPgTitle[4] = { "OPTIONS", "GRAPHICS", "AUDIO", "CONTROLS" };

/* Botones del N64 y su asignacion por defecto en la PSP (la misma que en configfile.c / controller_psp.c). */
static const struct { const char *name; unsigned int *mask; unsigned int def; } sBind[NUM_BINDS] = {
    { "A Button", &configKeyA, PSP_CTRL_CROSS },
    { "B Button", &configKeyB, PSP_CTRL_SQUARE },
    { "Start", &configKeyStart, PSP_CTRL_START },
    { "L Trigger", &configKeyL, PSP_CTRL_TRIANGLE },
    { "R Trigger", &configKeyR, PSP_CTRL_RTRIGGER },
    { "Z Trigger", &configKeyZ, PSP_CTRL_CIRCLE | PSP_CTRL_LTRIGGER },
    { "C-Up", &configKeyCUp, PSP_CTRL_UP },
    { "C-Down", &configKeyCDown, PSP_CTRL_DOWN },
    { "C-Left", &configKeyCLeft, PSP_CTRL_LEFT },
    { "C-Right", &configKeyCRight, PSP_CTRL_RIGHT },
};
#define DEAD_DEF 32 /* zona muerta por defecto del stick (la de configfile.c) */
#define DEAD_MAX 80

/* Botones fisicos que se pueden asignar (en este orden se prefiere uno si se pulsan varios a la vez). */
static const struct { const char *name; unsigned int mask; } sPhys[] = {
    { "Cross", PSP_CTRL_CROSS },     { "Circle", PSP_CTRL_CIRCLE }, { "Square", PSP_CTRL_SQUARE },   { "Triangle", PSP_CTRL_TRIANGLE },
    { "L Trig", PSP_CTRL_LTRIGGER }, { "R Trig", PSP_CTRL_RTRIGGER }, { "Select", PSP_CTRL_SELECT }, { "Start", PSP_CTRL_START },
    { "D-Up", PSP_CTRL_UP },         { "D-Down", PSP_CTRL_DOWN },    { "D-Left", PSP_CTRL_LEFT },     { "D-Right", PSP_CTRL_RIGHT },
};
#define NUM_PHYS ((int) (sizeof(sPhys) / sizeof(sPhys[0])))
#define BINDABLE (PSP_CTRL_CROSS | PSP_CTRL_CIRCLE | PSP_CTRL_SQUARE | PSP_CTRL_TRIANGLE | PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER | PSP_CTRL_SELECT \
                  | PSP_CTRL_START | PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_LEFT | PSP_CTRL_RIGHT)
#define CAPTURE_TICKS 150 /* ~5 s a 30 ticks por segundo */

/* Regla del menu: ningun valor deja el juego por debajo de como se ve en la N64. Las filas de distancia empiezan en el 100 % (lo original) y
 * solo suben; lo demas (limite de fps, ritmo, contador) no cambia lo que se dibuja. */
int gPspOptFpsCap = 60;   /* limite de fps: 30 / 60 */
int gPspOptPipe = 1;      /* Triple Buffer: la CPU empieza el cuadro siguiente sin esperar a que el anterior llegue a pantalla (gfx_scegu.c) */
int gPspOptKeepSpeed = 0; /* 0 = "Keep FPS": siempre intenta el limite (si no cabe, el juego va mas lento); 1 = "Keep Speed": baja los fps para no perder velocidad */
float gPspObjDistScale = 1.5f; /* multiplica la distancia a la que los objetos se desactivan (>= 1.0); por defecto 150 % */
float gPspFarScale = 1.5f;     /* multiplica el plano lejano de la camara (>= 1.0); por defecto 150 % */
int gPspOptCullObj = 1;        /* 0 = culling de objetos original (solo horizontal), 1 = tambien vertical (por defecto) */
float gPspVolMusic = 1.0f;     /* volumen de la musica, 0.0 a 1.0 (1.0 = el original del juego); lo lee effects.c */
float gPspVolSfx = 1.0f;       /* volumen de los efectos de sonido, 0.0 a 1.0 */
int gPspClockWant = 333;       /* reloj de CPU fijo (ya no es opcion): lo pide gfx_psp.c al arrancar */
unsigned int gPspOptChanges;   /* cuantas veces se cambio una opcion (la lee el registro de rendimiento) */

static const char *const sLabel[OPT_COUNT] = {
    "FPS Limit", "Frame Pacing", "FPS Counter", "Show Sound CPU", "Object Distance", "Far Distance", "Object Culling", "Reset to Defaults",
};

/* Cada fila tiene sus valores de menor a mayor (derecha = mas), y sDef es el indice del valor por defecto. */
static const char *const sFpsTxt[2] = { "30", "60" };
static const int sFpsCap[2] = { 30, 60 };
static const char *const sPacingTxt[2] = { "Keep FPS", "Keep Speed" };
static const char *const sOnOffTxt[2] = { "Off", "On" };
static const char *const sFpsCtrTxt[2] = { "Off", "On" };
/* Object Distance y Far Distance: de 100 % (lo original) a 300 %. (La opcion "Unlimited" se quito: rompia cosas.) El limite de profundidad de los objetos es
 * 20000 * escala y no debe pasar de 65536 (ver obj_is_in_view): con 300 % queda en 60000. */
static const char *const sObjTxt[5] = { "100%", "150%", "200%", "250%", "300%" };
static const float sObjScale[5] = { 1.0f, 1.5f, 2.0f, 2.5f, 3.0f };
static const char *const sFarTxt[5] = { "100%", "150%", "200%", "250%", "300%" };
static const float sFarScale[5] = { 1.0f, 1.5f, 2.0f, 2.5f, 3.0f }; /* plano lejano de la camara; el nivel de detalle siempre es el maximo */
#define DIST_ROW_MAX 4 /* indice del mayor valor de las filas de distancia (300 %) */
static const char *const sCullTxt[2] = { "Original", "Vertical" };

static const int sCount[OPT_COUNT] = { 2, 2, 2, 2, 5, 5, 2, 0 };
static const int sDef[OPT_COUNT] = { 1, 0, 0, 0, 1, 1, 1, 0 }; /* 60 fps, Keep FPS, contador Off, Object / Far Distance 150 %, culling Vertical */

static int sVal[OPT_COUNT] = { 1, 0, 0, 0, 1, 1, 1, 0 }; /* indice elegido en cada fila */
#define VOL_STEP 5 /* el menu sube / baja el volumen de 5 en 5 */
static int sMusic = 100; /* 0 a 100 */
static int sSfx = 100;
static int sInit;
static int sOpen;
static int sSel;
static int sScroll;
static int sPage;           /* PG_MAIN / PG_VIDEO / PG_CTRL */
static int sCapture = -1;   /* indice (0..NUM_BINDS-1) del boton que espera un boton fisico, -1 = ninguno */
static int sCaptureTimer;
static int sWaitRelease;    /* despues de asignar: ignora todo hasta soltar los botones (el mapeo acaba de cambiar) */
static unsigned int sPrevRaw;
static int sJustOpened;
static int sPrevX, sPrevY, sTimeX, sTimeY;

static void psp_opt_apply(void) {
    gPspOptFpsCap = sFpsCap[sVal[OPT_FPSCAP]];
    gPspOptKeepSpeed = sVal[OPT_PACING];
    gPspOptPipe = 1; /* Triple Buffer: siempre activo (ya no es opcion) */
    gPspObjDistScale = sObjScale[sVal[OPT_OBJDIST]];
    gPspFarScale = sFarScale[sVal[OPT_FARDIST]];
    gPspOptCullObj = sVal[OPT_CULLOBJ];
    gPspVolMusic = (float) sMusic * 0.01f;
    gPspVolSfx = (float) sSfx * 0.01f;
    /* "FPS Counter": 0 = apagado, 1 = contador en pantalla. Sin registro en archivo (version final). */
    gRenderFPS = sVal[OPT_SHOWFPS] ? 1 : 0;
    gPspFpsLogOff = 1;
}

/* Archivo de opciones ANTIGUO: "SM64OPT 7" y despues una linea "clave valor" por opcion (las que falten o no se reconozcan quedan en su valor por
 * defecto, asi se pueden agregar opciones sin invalidar el archivo). De las versiones 2 a 5 solo se leen FPS Limit y Frame Pacing: Object Distance,
 * Far Distance y Object Culling toman los valores por defecto nuevos (150 % / 150 % / Vertical), y Triple Buffer y CPU Clock ya no son opciones. */
static const struct { const char *key; int row; } sSaveKeys[] = {
    { "fps", OPT_FPSCAP }, { "pace", OPT_PACING }, { "scpu", OPT_SOUNDCPU }, { "obj", OPT_OBJDIST }, { "farp", OPT_FARDIST }, { "cull", OPT_CULLOBJ },
};
#define SAVE_KEYS ((int) (sizeof(sSaveKeys) / sizeof(sSaveKeys[0])))

/* valor de una version anterior (hasta la 5) -> indice actual (-1 = no se lee, queda el valor por defecto) */
static int psp_opt_from_old(int row, int v) {
    switch (row) {
        case OPT_FPSCAP: /* antes 30 / 40 / 45 / 50 / 60: ahora 30 / 60 */
            return (v >= 2) ? 1 : 0;
        case OPT_PACING:
            return v;
        default:
            return -1;
    }
}

/* Lee el archivo de opciones ANTIGUO (sm64_options.txt) para migrar los ajustes la primera vez. */
static void psp_opt_load_legacy(void) {
    char path[FS_MAX_PATH];
    FILE *f;
    int ver = 0, i;

    fs_resolve_path(path, sizeof(path), OPT_FILE);
    f = fopen(path, "r");
    if (f == NULL) {
        return;
    }
    if (fscanf(f, "SM64OPT %d", &ver) == 1) {
        if (ver == 2) {
            int v[7] = { 0 };
            static const int rows[7] = { OPT_FPSCAP, OPT_PACING, -1, -1, -1, -1, -1 }; /* el resto ya no se lee */
            if (fscanf(f, "%d %d %d %d %d %d %d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]) == 7) {
                for (i = 0; i < 7; i++) {
                    const int nv = (rows[i] < 0) ? -1 : psp_opt_from_old(rows[i], v[i]);
                    if (nv >= 0 && nv < sCount[rows[i]]) {
                        sVal[rows[i]] = nv;
                    }
                }
            }
        } else if (ver >= 3 && ver <= 7) {
            char key[16];
            int val;
            while (fscanf(f, "%15s %d", key, &val) == 2) {
                for (i = 0; i < SAVE_KEYS; i++) {
                    if (strcmp(key, sSaveKeys[i].key) == 0) {
                        int nv = (ver <= 5) ? psp_opt_from_old(sSaveKeys[i].row, val) : val;
                        /* version 6: Object / Far Distance no tenian 250 % (300 % y Unlimited eran los indices 3 y 4) */
                        if (ver == 6 && (sSaveKeys[i].row == OPT_OBJDIST || sSaveKeys[i].row == OPT_FARDIST) && val >= 3) {
                            nv = val + 1;
                        }
                        /* "Unlimited" (ya no existe) pasa a 300 % */
                        if ((sSaveKeys[i].row == OPT_OBJDIST || sSaveKeys[i].row == OPT_FARDIST) && nv > DIST_ROW_MAX && nv <= 5) {
                            nv = DIST_ROW_MAX;
                        }
                        if (nv >= 0 && nv < sCount[sSaveKeys[i].row]) {
                            sVal[sSaveKeys[i].row] = nv;
                        }
                    }
                }
            }
        }
    }
    fclose(f);
}

void psp_options_poll(void) {
    if (!sInit) {
        psp_options_load_file(CTRL_FILE); /* normalmente configfile_load ya lo hizo al arrancar */
    }
}

/* Repite el movimiento mientras se mantiene el stick / la cruceta: primero espera ~8 ticks y luego avanza cada 3. */
static int psp_opt_repeat(int cur, int *prev, int *timer) {
    if (cur == 0) {
        *prev = 0;
        *timer = 0;
        return 0;
    }
    if (cur != *prev) {
        *prev = cur;
        *timer = 0;
        return cur;
    }
    (*timer)++;
    if (*timer >= 8 && ((*timer - 8) % 3) == 0) {
        return cur;
    }
    return 0;
}

/* "Cross", "Circle/L Trig"... (un boton del N64 puede tener varios botones fisicos, p. ej. Z por defecto) */
static void psp_opt_mask_text(unsigned int mask, char *out, int max) {
    int i, n = 0;
    out[0] = '\0';
    for (i = 0; i < NUM_PHYS; i++) {
        if (mask & sPhys[i].mask) {
            n += snprintf(out + n, max - n, "%s%s", n ? "/" : "", sPhys[i].name);
            if (n >= max - 1) {
                return;
            }
        }
    }
    if (n == 0) {
        snprintf(out, max, "None");
    }
}

/* Modo de sonido (0 Stereo, 1 Mono, 2 Headset): mismos indices que el menu de seleccion de archivo. Se aplica al instante; el guardado se hace al cerrar. */
static const char *const sSndModeTxt[3] = { "Stereo", "Mono", "Headset" };
static int sSndMode = 0;
static int sSndDirty = 0;
static void psp_opt_set_sound_mode(int mode) {
    if (mode != sSndMode) {
        sSndMode = mode;
        set_sound_mode((u16) mode);
        sSndDirty = 1;
    }
}

static const char *psp_opt_label(int row) {
    if (row < OPT_COUNT) return sLabel[row];
    if (row == ROW_VIDEO) return "Graphics";
    if (row == ROW_AUDIO) return "Audio";
    if (row == ROW_MUSIC) return "Music Volume";
    if (row == ROW_SFX) return "Effects Volume";
    if (row == ROW_SOUNDMODE) return "Sound Mode";
    if (row == ROW_AUDRESET) return "Reset to Defaults";
    if (row == ROW_CONTROLS) return "Controls";
    if (row == ROW_DEAD) return "Stick Deadzone";
    if (row == ROW_CTRLRESET) return "Reset to Defaults";
    return sBind[row - ROW_BIND0].name;
}

/* filas de una sola linea (entradas de pagina y "Reset"): el texto va centrado en la casilla, sin linea de valor */
static int psp_opt_is_action(int row) {
    return row == OPT_RESET || row == ROW_VIDEO || row == ROW_AUDIO || row == ROW_CONTROLS || row == ROW_CTRLRESET || row == ROW_AUDRESET;
}

static const char *psp_opt_value_text(int row) {
    static char buf[40];
    switch (row) {
        case OPT_FPSCAP: return sFpsTxt[sVal[row]];
        case OPT_PACING: return sPacingTxt[sVal[row]];
        case OPT_SHOWFPS: return sFpsCtrTxt[sVal[row]];
        case OPT_SOUNDCPU: return sOnOffTxt[sVal[row]];
        case OPT_OBJDIST: return sObjTxt[sVal[row]];
        case OPT_FARDIST: return sFarTxt[sVal[row]];
        case OPT_CULLOBJ: return sCullTxt[sVal[row]];
        case ROW_DEAD:
            snprintf(buf, sizeof(buf), "%u", configDeadzone);
            return buf;
        case ROW_MUSIC:
            snprintf(buf, sizeof(buf), "%d%%", sMusic);
            return buf;
        case ROW_SFX:
            snprintf(buf, sizeof(buf), "%d%%", sSfx);
            return buf;
        case ROW_SOUNDMODE: return sSndModeTxt[sSndMode];
        default:
            if (row >= ROW_BIND0 && row < ROW_BIND0 + NUM_BINDS) {
                if (sCapture == row - ROW_BIND0) {
                    return "Press a button";
                }
                psp_opt_mask_text(*sBind[row - ROW_BIND0].mask, buf, sizeof(buf));
                return buf;
            }
            return "";
    }
}

/* ¿hay mas valores hacia la izquierda / derecha? (para las flechas) */
static int psp_opt_can_dec(int row) {
    if (row == ROW_DEAD) return configDeadzone > 0;
    if (row == ROW_MUSIC) return sMusic > 0;
    if (row == ROW_SFX) return sSfx > 0;
    if (row == ROW_SOUNDMODE) return sSndMode > 0;
    return row < OPT_RESET && sVal[row] > 0;
}
static int psp_opt_can_inc(int row) {
    if (row == ROW_DEAD) return configDeadzone < DEAD_MAX;
    if (row == ROW_MUSIC) return sMusic < 100;
    if (row == ROW_SFX) return sSfx < 100;
    if (row == ROW_SOUNDMODE) return sSndMode < 2;
    return row < OPT_RESET && sVal[row] < sCount[row] - 1;
}

/* Sin vuelta: en el primer/ultimo valor no pasa nada (las flechas de los lados indican hacia donde se puede ir).
 * wrap = 1 solo para el boton A en filas de dos valores (Si/No), donde "siguiente" es alternar. */
static void psp_opt_change(int row, int dir, int wrap) {
    int nv;
    if (row == ROW_DEAD) {
        nv = (int) configDeadzone + dir * 2;
        if (nv < 0 || nv > DEAD_MAX) {
            return;
        }
        configDeadzone = (unsigned int) nv;
        play_sound(SOUND_MENU_CHANGE_SELECT, gDefaultSoundArgs);
        return;
    }
    if (row == ROW_SOUNDMODE) {
        nv = sSndMode + dir;
        if (nv < 0 || nv > 2) {
            return;
        }
        psp_opt_set_sound_mode(nv);
        play_sound(SOUND_MENU_CHANGE_SELECT, gDefaultSoundArgs);
        return;
    }
    if (row == ROW_MUSIC || row == ROW_SFX) {
        int *vol = (row == ROW_MUSIC) ? &sMusic : &sSfx;
        nv = *vol + dir * VOL_STEP;
        nv = (nv < 0) ? 0 : ((nv > 100) ? 100 : nv);
        if (nv == *vol) {
            return;
        }
        *vol = nv;
        psp_opt_apply();
        play_sound(SOUND_MENU_CHANGE_SELECT, gDefaultSoundArgs); /* es un efecto: se oye al volumen nuevo */
        return;
    }
    if (row < 0 || row >= OPT_RESET) {
        return;
    }
    nv = sVal[row] + dir;
    if (nv < 0 || nv >= sCount[row]) {
        if (!(wrap && sCount[row] == 2)) {
            return;
        }
        nv = (nv + sCount[row]) % sCount[row];
    }
    sVal[row] = nv;
    gPspOptChanges++;
    psp_opt_apply();
    play_sound(SOUND_MENU_CHANGE_SELECT, gDefaultSoundArgs);
}

static void psp_opt_reset(void) {
    int i;
    for (i = 0; i < OPT_RESET; i++) {
        sVal[i] = sDef[i];
    }
    gPspOptChanges++;
    psp_opt_apply();
}

static void psp_opt_reset_controls(void) {
    int i;
    for (i = 0; i < NUM_BINDS; i++) {
        *sBind[i].mask = sBind[i].def;
    }
    configDeadzone = DEAD_DEF;
}

static void psp_opt_reset_audio(void) {
    sMusic = 100;
    sSfx = 100;
    psp_opt_set_sound_mode(0);
    psp_opt_apply();
}

/* Asigna el boton fisico b al boton i del N64. Si otra accion ya lo usaba se intercambian (esa recibe lo que tenia i), asi ninguna accion se
 * queda sin boton y no hay dos acciones en el mismo boton. */
static void psp_opt_bind(int i, unsigned int b) {
    const unsigned int old = *sBind[i].mask;
    int j;
    for (j = 0; j < NUM_BINDS; j++) {
        if (j != i && (*sBind[j].mask & b)) {
            unsigned int nm = *sBind[j].mask & ~b;
            if (nm == 0) {
                nm = old & ~b;
            }
            if (nm != 0) {
                *sBind[j].mask = nm;
            }
        }
    }
    *sBind[i].mask = b;
}

/* ------------------------------------------------------------------------------------------------------------------------------------------
 * Archivo unico de ajustes (sm64config.txt): texto legible, "nombre = valor" con comentarios. Se escribe entero al cerrar el menu y al salir.
 * ------------------------------------------------------------------------------------------------------------------------------------------ */

static char sCfgName[40] = CTRL_FILE;
static int sSawNew; /* el archivo tenia al menos un ajuste del formato nuevo (si no, es uno antiguo y se migra) */

static const char *const sRowKey[OPT_COUNT] = {
    "fps_limit", "frame_pacing", "fps_counter", "show_sound_cpu", "object_distance", "far_distance", "object_culling", NULL,
};
static const char *const sBindKey[NUM_BINDS] = {
    "a_button", "b_button", "start", "l_trigger", "r_trigger", "z_trigger", "c_up", "c_down", "c_left", "c_right",
};
static const char *const sBindKeyOld[NUM_BINDS] = { /* nombres del sm64config.txt antiguo (el valor era la mascara numerica) */
    "key_a", "key_b", "key_start", "key_l", "key_r", "key_z", "key_cup", "key_cdown", "key_cleft", "key_cright",
};
/* otros nombres que se aceptan al leer botones fisicos */
static const struct { const char *name; unsigned int mask; } sPhysAlias[] = {
    { "L", PSP_CTRL_LTRIGGER }, { "R", PSP_CTRL_RTRIGGER }, { "L Trigger", PSP_CTRL_LTRIGGER }, { "R Trigger", PSP_CTRL_RTRIGGER },
    { "Up", PSP_CTRL_UP }, { "Down", PSP_CTRL_DOWN }, { "Left", PSP_CTRL_LEFT }, { "Right", PSP_CTRL_RIGHT },
};

static const char *const *psp_opt_texts(int row) {
    switch (row) {
        case OPT_FPSCAP: return sFpsTxt;
        case OPT_PACING: return sPacingTxt;
        case OPT_SHOWFPS: return sFpsCtrTxt;
        case OPT_SOUNDCPU: return sOnOffTxt;
        case OPT_OBJDIST: return sObjTxt;
        case OPT_FARDIST: return sFarTxt;
        case OPT_CULLOBJ: return sCullTxt;
        default: return NULL;
    }
}

static int psp_ci_eq(const char *a, const char *b) {
    for (; *a != '\0' && *b != '\0'; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return *a == *b;
}

static char *psp_trim(char *s) {
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = '\0';
    return s;
}

/* numero entero al principio del texto (sin nada raro: "abc" no vale); devuelve 0 si no es un numero */
static int psp_parse_int(const char *s, int *out) {
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s || (*end != '\0' && *end != ' ' && *end != '\t')) {
        return 0;
    }
    *out = (int) v;
    return 1;
}

/* "Cross", "Circle/L Trig", "None" o (archivo antiguo) un numero -> mascara de botones. Devuelve 0 si no se entiende. */
static int psp_opt_parse_mask(char *v, unsigned int *out) {
    unsigned int m = 0;
    if (v[0] >= '0' && v[0] <= '9') {
        *out = (unsigned int) strtoul(v, NULL, 10) & BINDABLE;
        return 1;
    }
    for (;;) {
        char *sep = strchr(v, '/');
        char *t;
        int i, found = 0;
        if (sep != NULL) *sep = '\0';
        t = psp_trim(v);
        if (t[0] != '\0' && !psp_ci_eq(t, "none")) {
            for (i = 0; i < NUM_PHYS && !found; i++) {
                if (psp_ci_eq(t, sPhys[i].name)) { m |= sPhys[i].mask; found = 1; }
            }
            for (i = 0; i < (int) (sizeof(sPhysAlias) / sizeof(sPhysAlias[0])) && !found; i++) {
                if (psp_ci_eq(t, sPhysAlias[i].name)) { m |= sPhysAlias[i].mask; found = 1; }
            }
            if (!found) return 0;
        }
        if (sep == NULL) break;
        v = sep + 1;
    }
    *out = m;
    return 1;
}

/* una linea del archivo: "nombre = valor  # comentario" (tambien "nombre valor", como el archivo antiguo); [secciones] y comentarios se saltan */
static void psp_cfg_line(char *line) {
    char *c = strchr(line, '#');
    char *key, *val, *eq;
    int i, v;

    if (c != NULL) *c = '\0';
    line = psp_trim(line);
    if (line[0] == '\0' || line[0] == '[') {
        return;
    }
    eq = strchr(line, '=');
    if (eq != NULL) {
        *eq = '\0';
        val = eq + 1;
    } else {
        val = line;
        while (*val != '\0' && *val != ' ' && *val != '\t') val++;
        if (*val != '\0') *val++ = '\0';
    }
    key = psp_trim(line);
    val = psp_trim(val);
    for (c = key; *c != '\0'; c++) {
        if (*c >= 'A' && *c <= 'Z') *c += 32;
    }

    for (i = 0; i < OPT_RESET; i++) {
        if (strcmp(key, sRowKey[i]) == 0) {
            const char *const *t = psp_opt_texts(i);
            for (v = 0; v < sCount[i]; v++) {
                if (psp_ci_eq(val, t[v])) {
                    sVal[i] = v;
                    sSawNew = 1;
                    break;
                }
            }
            /* "Unlimited" (ya no existe) de un sm64config.txt anterior pasa a 300 % */
            if ((i == OPT_OBJDIST || i == OPT_FARDIST) && psp_ci_eq(val, "unlimited")) {
                sVal[i] = DIST_ROW_MAX;
                sSawNew = 1;
            }
            return;
        }
    }
    if (strcmp(key, "music_volume") == 0 || strcmp(key, "effects_volume") == 0) {
        if (psp_parse_int(val, &v)) {
            v = (v < 0) ? 0 : ((v > 100) ? 100 : v);
            if (key[0] == 'm') sMusic = v; else sSfx = v;
            sSawNew = 1;
        }
        return;
    }
    if (strcmp(key, "stick_deadzone") == 0 || strcmp(key, "deadzone") == 0) {
        if (psp_parse_int(val, &v)) {
            configDeadzone = (unsigned int) ((v < 0) ? 0 : ((v > DEAD_MAX) ? DEAD_MAX : v));
            if (key[0] == 's') sSawNew = 1;
        }
        return;
    }
    for (i = 0; i < NUM_BINDS; i++) {
        if (strcmp(key, sBindKey[i]) == 0 || strcmp(key, sBindKeyOld[i]) == 0) {
            unsigned int m;
            if (psp_opt_parse_mask(val, &m)) {
                *sBind[i].mask = m;
                if (key[0] != 'k') sSawNew = 1;
            }
            return;
        }
    }
}

/* A, B, Start y R no pueden quedar sin boton (con ellos se juega, se pausa y se abre este menu): un archivo con "None" ahi se corrige. */
static void psp_opt_sanitize_controls(void) {
    int i;
    for (i = 0; i < NUM_BINDS; i++) {
        *sBind[i].mask &= BINDABLE;
        if (*sBind[i].mask == 0 && (i <= 2 || i == 4)) {
            *sBind[i].mask = sBind[i].def;
        }
    }
}

static void psp_cfg_put(FILE *f, const char *key, const char *val, const char *comment) {
    char buf[96];
    snprintf(buf, sizeof(buf), "%s = %s", key, val);
    if (comment != NULL) {
        fprintf(f, "%-34s # %s\n", buf, comment);
    } else {
        fprintf(f, "%s\n", buf);
    }
}

void psp_options_save_file(const char *filename) {
    char path[FS_MAX_PATH];
    char num[16], names[48];
    FILE *f;
    int i;
    static const char *const sComment[OPT_COUNT] = {
        "30 or 60",
        "Keep FPS (always aim for the FPS limit) or Keep Speed (drop FPS to keep the game speed)",
        "Off or On (frames per second on screen)",
        "Off or On (shows on the pause screen which processor mixes the audio)",
        "100%, 150%, 200%, 250% or 300% (how far away objects stay active)",
        "100%, 150%, 200%, 250% or 300% (how far you see)",
        "Original or Vertical (Vertical also hides objects above / below the view)",
        NULL,
    };
    static const char *const sBindComment[NUM_BINDS] = {
        NULL, NULL, NULL, "(unused in the game)", "opens the Options menu on the pause screen", "either button works", NULL, NULL, NULL, NULL,
    };

    if (filename != NULL && filename[0] != '\0') {
        snprintf(sCfgName, sizeof(sCfgName), "%s", filename);
    }
    fs_resolve_path(path, sizeof(path), sCfgName);
    fs_prepare_parent_path(sCfgName);
    f = fopen(path, "w");
    if (f == NULL) {
        return;
    }
    fprintf(f, "# Super Mario 64 for PSP - settings\n"
               "#\n"
               "# Everything here can be changed in the game: press R on the pause screen to open the Options menu.\n"
               "# The file is read when the game starts and rewritten when you close the menu or exit the game.\n"
               "# You can also edit it by hand: one \"name = value\" per line, and everything after a # is a comment.\n"
               "# Lines that are missing or have a value that isn't understood keep their default.\n"
               "\n[Graphics]\n");
    for (i = 0; i < OPT_RESET; i++) {
        psp_cfg_put(f, sRowKey[i], psp_opt_texts(i)[sVal[i]], sComment[i]);
    }
    fprintf(f, "\n[Audio]\n");
    snprintf(num, sizeof(num), "%d", sMusic);
    psp_cfg_put(f, "music_volume", num, "0 to 100 (100 = original volume)");
    snprintf(num, sizeof(num), "%d", sSfx);
    psp_cfg_put(f, "effects_volume", num, "0 to 100 (100 = original volume)");
    fprintf(f, "\n[Controls]\n"
               "# PSP buttons: Cross, Circle, Square, Triangle, L Trig, R Trig, Select, Start, D-Up, D-Down, D-Left, D-Right\n"
               "# A Nintendo 64 button can use two PSP buttons if you join them with / (for example Circle/L Trig).\n"
               "# A Button, B Button, Start and R Trigger can't be None.\n");
    for (i = 0; i < NUM_BINDS; i++) {
        psp_opt_mask_text(*sBind[i].mask, names, sizeof(names));
        psp_cfg_put(f, sBindKey[i], names, sBindComment[i]);
    }
    snprintf(num, sizeof(num), "%u", configDeadzone);
    psp_cfg_put(f, "stick_deadzone", num, "0 to 80 (how far the stick must move before it counts)");
    fclose(f);
}

void psp_options_load_file(const char *filename) {
    char path[FS_MAX_PATH];
    char line[200];
    FILE *f;

    if (filename != NULL && filename[0] != '\0') {
        snprintf(sCfgName, sizeof(sCfgName), "%s", filename);
    }
    sInit = 1;
    sSawNew = 0;
    fs_resolve_path(path, sizeof(path), sCfgName);
    f = fopen(path, "r");
    if (f == NULL && strcmp(path, sCfgName) != 0) {
        f = fopen(sCfgName, "r");
    }
    if (f != NULL) {
        while (fgets(line, sizeof(line), f) != NULL) {
            psp_cfg_line(line);
        }
        fclose(f);
    }
    if (!sSawNew) {
        psp_opt_load_legacy(); /* instalacion antigua: los ajustes graficos estaban en sm64_options.txt */
    }
    psp_opt_sanitize_controls();
    psp_opt_apply();
    if (!sSawNew) {
        psp_options_save_file(sCfgName); /* crea el archivo (o lo convierte al formato nuevo) */
    }
}

/* ASCII -> codigos de texto del juego (letras, numeros y unos pocos simbolos; lo demas es un espacio) */
static void psp_opt_str(u8 *dst, const char *src, int max) {
    int n = 0;
    for (; *src != '\0' && n < max - 1; src++, n++) {
        const char c = *src;
        u8 d;
        if (c >= '0' && c <= '9') d = (u8) (c - '0');
        else if (c >= 'A' && c <= 'Z') d = (u8) (c - 'A' + 0x0A);
        else if (c >= 'a' && c <= 'z') d = (u8) (c - 'a' + 0x24);
        else if (c == '-') d = 0x9F;
        else if (c == '.') d = 0x3F;
        else if (c == '%') d = 0xF3;
        else if (c == '/') d = 0xD0;
        else if (c == ':') d = 0xE6;
        else if (c == '>') d = 0x53;
        else if (c == '<') d = 0x52;
        else d = 0x9E; /* espacio */
        dst[n] = d;
    }
    dst[n] = 0xFF; /* terminador */
}

/* texto centrado en x con sombra negra, blanco (col 0) o rojo (col 1); y se mide desde abajo como en los dialogos */
static void psp_opt_text(s16 x, s16 y, const char *txt, int col) {
    u8 buf[48];
    s16 tx;

    psp_opt_str(buf, txt, sizeof(buf));
    tx = get_str_x_pos_from_center(x, buf, 10.0f);
    gDPSetEnvColor(gDisplayListHead++, 0, 0, 0, 255);
    print_generic_string(tx + 1, y - 1, buf);
    if (col) {
        gDPSetEnvColor(gDisplayListHead++, 255, 32, 32, 255);
    } else {
        gDPSetEnvColor(gDisplayListHead++, 255, 255, 255, 255);
    }
    print_generic_string(tx, y, buf);
}

/* rectangulo relleno con la caja de los dialogos (130 x 80 de base) */
static void psp_opt_box(s16 x, s16 ytop, s16 w, s16 h, u8 r, u8 g, u8 b, u8 a) {
    create_dl_translation_matrix(MENU_MTX_PUSH, x, ytop, 0);
    create_dl_scale_matrix(MENU_MTX_NOPUSH, w / 130.0f, h / 80.0f, 1.0f);
    gDPSetEnvColor(gDisplayListHead++, r, g, b, a);
    gSPDisplayList(gDisplayListHead++, dl_draw_text_bg_box);
    gSPPopMatrix(gDisplayListHead++, G_MTX_MODELVIEW);
}

static const u8 *psp_opt_page_rows(int *n) {
    switch (sPage) {
        case PG_VIDEO: *n = (int) sizeof(sPgVideo); return sPgVideo;
        case PG_AUDIO: *n = (int) sizeof(sPgAudio); return sPgAudio;
        case PG_CTRL: *n = (int) sizeof(sPgCtrl); return sPgCtrl;
        default: *n = (int) sizeof(sPgMain); return sPgMain;
    }
}

static void psp_opt_draw(void) {
    u8 title[16];
    const u8 *rows;
    int n, i;

    rows = psp_opt_page_rows(&n);
    shade_screen();

    /* titulo grande (letras de colores del HUD), 12 px por letra */
    psp_opt_str(title, sPgTitle[sPage], sizeof(title));
    gSPDisplayList(gDisplayListHead++, dl_rgba16_text_begin);
    gDPSetEnvColor(gDisplayListHead++, 255, 255, 255, 255);
    print_hud_lut_string(HUD_LUT_GLOBAL, 160 - (int) strlen(sPgTitle[sPage]) * 6, 40, title);
    gSPDisplayList(gDisplayListHead++, dl_rgba16_text_end);

    /* barra de desplazamiento a la derecha (pista y deslizador), de la altura de las 4 filas visibles; solo si hay mas de 4 filas */
    if (n > 4) {
        const int sh = 118 * 4 / n; /* el deslizador mide lo que las 4 filas visibles respecto del total */
        psp_opt_box(272, 150, 8, 118, 128, 128, 128, 200);
        psp_opt_box(272, 150 - (118 - sh) * sScroll / (n - 4), 8, sh, 255, 255, 255, 255);
    }

    gSPDisplayList(gDisplayListHead++, dl_ia_text_begin);
    for (i = sScroll; i < sScroll + 4 && i < n; i++) {
        const int y = 140 - 32 * (i - sScroll);
        const int col = (i == sSel) ? 1 : 0;
        if (psp_opt_is_action(rows[i])) {
            psp_opt_text(160, y - 6, psp_opt_label(rows[i]), col);
        } else {
            psp_opt_text(160, y, psp_opt_label(rows[i]), col);
            psp_opt_text(160, y - 13, psp_opt_value_text(rows[i]), col);
        }
    }
    psp_opt_text(48, 14, "B: Return", 0); /* esquina inferior izquierda */
    /* ayuda al centro, abajo, en las filas de botones */
    if (sPage == PG_CTRL && rows[sSel] >= ROW_BIND0 && rows[sSel] < ROW_BIND0 + NUM_BINDS) {
        psp_opt_text(160, 14, (sCapture >= 0) ? "Wait to cancel" : "A: Change", 0);
    }
    /* flechas a los lados del valor de la fila seleccionada: solo aparece la del lado hacia donde se puede ir
     * (sin flecha izquierda = primer valor, sin flecha derecha = ultimo valor) */
    gDPSetEnvColor(gDisplayListHead++, 255, 255, 255, 255);
    if (!psp_opt_is_action(rows[sSel])) {
        u8 l[2], r[2];
        const int ay = 140 - 32 * (sSel - sScroll) - 13;
        psp_opt_str(l, "<", sizeof(l));
        psp_opt_str(r, ">", sizeof(r));
        if (psp_opt_can_dec(rows[sSel])) {
            print_generic_string(88, ay, l);
        }
        if (psp_opt_can_inc(rows[sSel])) {
            print_generic_string(222, ay, r);
        }
    }
    /* flechas arriba/abajo junto a la barra de desplazamiento: hay mas filas en esa direccion */
    if (n > 4) {
        static const u8 up[2] = { 0x50, 0xFF };
        static const u8 dn[2] = { 0x51, 0xFF };
        /* centradas en la barra: x = 273 (el glifo mide ~7 de ancho y la barra va de 272 a 280) y a la misma distancia (~6.5) de la pista por arriba
         * (la pista empieza en y = 150) y por abajo (termina en y = 32); medido sobre una captura de la PSP. Las bases difieren porque el glifo de abajo ocupa menos de su casilla. */
        if (sScroll > 0) {
            print_generic_string(273, 151, up);
        }
        if (sScroll < n - 4) {
            print_generic_string(273, 14, dn);
        }
    }
    gSPDisplayList(gDisplayListHead++, dl_ia_text_end);
}

/* "SOUND: CPU / ME": que procesador sintetiza el audio (el CPU principal o el Media Engine). Se dibuja en las dos pausas (castillo y niveles) solo si
 * la opcion "Show Sound CPU" esta activada. */
void psp_options_sound_info(void) {
    if (sVal[OPT_SOUNDCPU]) {
        print_text(0, 0, "SOUND:");
        print_text(0 + (4 * 16), 0, psp_me_is_active() ? "ME" : "CPU");
    }
}

/* "R Options" en la esquina de la pausa, mientras el menu esta cerrado (se llama despues de dibujar la pausa normal).
 * Si el boton R del N64 se reasigno, muestra el boton fisico que lo abre ahora. */
void psp_options_pause_prompt(void) {
    char name[24], txt[40];
    s16 x;

    if (sOpen) {
        return;
    }
    if (configKeyR == PSP_CTRL_RTRIGGER) {
        snprintf(txt, sizeof(txt), "R Options");
    } else {
        psp_opt_mask_text(configKeyR, name, sizeof(name));
        snprintf(txt, sizeof(txt), "%s Options", name);
    }
    x = 304 - (s16) strlen(txt) * 5; /* alineado a la derecha, sin salirse de la pantalla */
    if (x > 264) {
        x = 264;
    }
    gSPDisplayList(gDisplayListHead++, dl_ia_text_begin);
    psp_opt_text(x, 212, txt, 0);
    gSPDisplayList(gDisplayListHead++, dl_ia_text_end);
}

static void psp_opt_close(void) {
    sOpen = 0;
    sCapture = -1;
    if (sSndDirty) {
        /* igual que al elegirlo en el menu de archivos: se guarda en el archivo de guardado (una sola escritura, al cerrar) */
        save_file_set_sound_mode(sSndMode);
        sSndDirty = 0;
    }
    psp_options_save_file(sCfgName);
    play_sound(SOUND_MENU_PAUSE_2, gDefaultSoundArgs);
}

/* Al cerrar, este mismo frame lo dibuja la pausa normal (oscurecido, monedas, opciones del curso): si no dibujara nada se veria un frame de la escena
 * sin oscurecer (parpadeo). Los botones que cerraron el menu se quitan para que la pausa no los lea (START/A la despausarian). */
static int psp_opt_closed_frame(struct Controller *c) {
    c->buttonPressed &= ~(START_BUTTON | A_BUTTON | B_BUTTON | Z_TRIG | R_TRIG);
    return 0;
}

static void psp_opt_enter_page(int page, int sel) {
    sPage = page;
    sSel = sel;
    sScroll = 0;
    if (sSel > 3) {
        sScroll = sSel - 3;
    }
    sPrevX = sPrevY = sTimeX = sTimeY = 0;
    play_sound(SOUND_MENU_CHANGE_SELECT, gDefaultSoundArgs);
}

int psp_options_pause_frame(int can_open) {
    struct Controller *c = gPlayer3Controller;
    const u8 *rows;
    unsigned int raw, pressed;
    int n, dx, dy;

    psp_options_poll();

    if (!sOpen) {
        if (!can_open || !(c->buttonPressed & R_TRIG)
            || (c->buttonDown & (L_TRIG | Z_TRIG | B_BUTTON | A_BUTTON | START_BUTTON))) {
            return 0;
        }
        sOpen = 1;
        sPage = PG_MAIN;
        sSel = 0;
        sScroll = 0;
        sJustOpened = 1;
        sCapture = -1;
        sWaitRelease = 0;
        sPrevX = sPrevY = sTimeX = sTimeY = 0;
        sPrevRaw = gPspRawButtons; /* lo que se esta pulsando al abrir (la R) no cuenta */
        sVal[OPT_SHOWFPS] = gRenderFPS ? 1 : 0;
        sSndMode = save_file_get_sound_mode();
        if (sSndMode > 2) {
            sSndMode = 0;
        }
        sSndDirty = 0;
        play_sound(SOUND_MENU_CAMERA_ZOOM_IN, gDefaultSoundArgs);
    }

    raw = gPspRawButtons;
    pressed = raw & ~sPrevRaw;
    sPrevRaw = raw;
    rows = psp_opt_page_rows(&n);

    if (sJustOpened) {
        sJustOpened = 0;
    } else if (sWaitRelease) {
        /* acaba de cambiar el mapeo con botones aun pulsados: no se hace nada hasta soltarlos todos */
        if (!(raw & BINDABLE)) {
            sWaitRelease = 0;
        }
    } else if (sCapture >= 0) {
        if (pressed & BINDABLE) {
            int k;
            for (k = 0; k < NUM_PHYS; k++) {
                if (pressed & sPhys[k].mask) {
                    psp_opt_bind(sCapture, sPhys[k].mask);
                    break;
                }
            }
            sCapture = -1;
            sWaitRelease = 1;
            play_sound(SOUND_MENU_CLICK_FILE_SELECT, gDefaultSoundArgs);
        } else if (--sCaptureTimer <= 0) {
            sCapture = -1; /* nadie pulso nada: se cancela */
            play_sound(SOUND_MENU_CAMERA_BUZZ, gDefaultSoundArgs);
        }
    } else {
        if (pressed & (PSP_CTRL_START | PSP_CTRL_RTRIGGER)) {
            psp_opt_close();
            return psp_opt_closed_frame(c);
        }
        if (pressed & (PSP_CTRL_SQUARE | PSP_CTRL_CIRCLE)) {
            if (sPage == PG_MAIN) {
                psp_opt_close();
                return psp_opt_closed_frame(c);
            }
            psp_opt_enter_page(PG_MAIN, sPage - 1); /* la entrada de la pagina de la que se sale */
            psp_opt_draw();
            return 1;
        }

        dy = 0;
        dx = 0;
        if (gPspRawStickY > 50 || (raw & PSP_CTRL_UP)) dy = -1;
        if (gPspRawStickY < -50 || (raw & PSP_CTRL_DOWN)) dy = 1;
        if (gPspRawStickX > 50 || (raw & PSP_CTRL_RIGHT)) dx = 1;
        if (gPspRawStickX < -50 || (raw & PSP_CTRL_LEFT)) dx = -1;
        dy = psp_opt_repeat(dy, &sPrevY, &sTimeY);
        dx = psp_opt_repeat(dx, &sPrevX, &sTimeX);

        if (dy != 0) {
            /* sin vuelta: arriba de la primera fila o abajo de la ultima no hace nada */
            const int ns = sSel + dy;
            if (ns >= 0 && ns < n) {
                sSel = ns;
                if (sSel < sScroll) {
                    sScroll = sSel;
                } else if (sSel > sScroll + 3) {
                    sScroll = sSel - 3;
                }
                play_sound(SOUND_MENU_CHANGE_SELECT, gDefaultSoundArgs);
            }
        }
        if (dx != 0) {
            psp_opt_change(rows[sSel], dx, 0);
        }
        if (pressed & PSP_CTRL_CROSS) {
            const int row = rows[sSel];
            if (row == ROW_VIDEO) {
                psp_opt_enter_page(PG_VIDEO, 0);
            } else if (row == ROW_AUDIO) {
                psp_opt_enter_page(PG_AUDIO, 0);
            } else if (row == ROW_CONTROLS) {
                psp_opt_enter_page(PG_CTRL, 0);
            } else if (row == ROW_AUDRESET) {
                psp_opt_reset_audio();
                play_sound(SOUND_MENU_CHANGE_SELECT, gDefaultSoundArgs);
            } else if (row == OPT_RESET) {
                psp_opt_reset();
                play_sound(SOUND_MENU_CHANGE_SELECT, gDefaultSoundArgs);
            } else if (row == ROW_CTRLRESET) {
                psp_opt_reset_controls();
                play_sound(SOUND_MENU_CHANGE_SELECT, gDefaultSoundArgs);
            } else if (row >= ROW_BIND0 && row < ROW_BIND0 + NUM_BINDS) {
                sCapture = row - ROW_BIND0;
                sCaptureTimer = CAPTURE_TICKS;
                play_sound(SOUND_MENU_CHANGE_SELECT, gDefaultSoundArgs);
            } else {
                psp_opt_change(row, 1, 1);
            }
        }
    }

    psp_opt_draw();
    return 1;
}

#endif /* TARGET_PSP */
