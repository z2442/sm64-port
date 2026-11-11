#ifdef TARGET_PSP

/* PSP options menu (graphics, audio and controls).
 *
 * Designed like the sm64rt / sm64ex options menu: it opens with R on the pause screen (the pause stays in the background, darkened),
 * big title at the top, 4 options at a time with the selected one in red and its value below, arrows at the sides and a scroll bar.
 * Main page: Graphics / Audio / Controls. Text uses the same font as the game (the dialog one).
 * Input (always the PSP's PHYSICAL buttons, so that remapping the controls never leaves the menu unusable):
 * move: stick or D-pad up / down; change the value: left / right or X (cross); go back: square or circle; close: START or R.
 * In Controls, X on a button asks for "Press a button" and assigns the next button that is pressed (if another action was already using that button, they are swapped);
 * if nothing is pressed within about 5 s it is cancelled.
 * ALL settings (graphics, audio and controls) are saved in a single readable text file, sm64config.txt, when the menu is closed and when leaving the game;
 * they are loaded at startup (configfile_load asks psp_options_load_file for them). It can also be edited by hand. Old files (sm64config.txt with only
 * numeric controls + sm64_options.txt) are read and converted to the new format the first time. */

#include <ultra64.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <psppower.h>
#include <pspctrl.h>
#include <psprtc.h>

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
#include "pc/psp_dve.h"
#include "pc/configfile.h"

void create_dl_scale_matrix(s8 pushOp, f32 x, f32 y, f32 z); /* ingame_menu.c */
void shade_screen(void);                                      /* ingame_menu.c */
extern int gPspFpsLogOff;                                    /* game_init.c: in the final version always 1 (the performance log is off) */
void gfx_scegu_tv_request(int mode);
void gfx_scegu_tv_refresh(void);
void gfx_scegu_tv_area_refresh(void);
void gfx_scegu_tv_reconnect(void);
int gfx_scegu_tv_probation_left(void);
void gfx_scegu_tv_confirm(void);
void gfx_scegu_tv_cancel(void);
int gfx_scegu_tv_take_revert(void);
int gfx_scegu_tv_vbl_hz10(void);
void psp_options_save_file(const char *filename);                       /* gfx_scegu.c: applies the TV output (0 / 1 / 2) within a few frames */
extern u8 gRenderFPS;                                        /* game_init.c: FPS counter (and performance log) */
extern unsigned int gPspRawButtons;                          /* controller_psp.c: the PSP's physical buttons (PSP_CTRL_*) */
extern int gPspRawStickX, gPspRawStickY;                     /* controller_psp.c: physical stick, -128..127, up and right positive */

#define OPT_FILE "sm64_options.txt" /* old format (read only, to migrate) */
#define CTRL_FILE "sm64config.txt"  /* the single settings file (the same one pc_main.c uses, CONFIG_FILE) */

enum {
    OPT_FPSCAP,
    OPT_PACING,
    OPT_SHOWFPS,
    OPT_SOUNDCPU,
    OPT_OBJDIST,
    OPT_FARDIST,
    OPT_CULLOBJ,
    OPT_TVOUT,
    OPT_TVASPECT,
    OPT_TVAREA,  /* TV Width */
    OPT_TVAREAY, /* TV Height */
    OPT_TVSHIFT, /* TV Vertical Shift */
    OPT_RESET,
    OPT_COUNT
};

/* Non-video rows (the indices start where the OPT_* end, so the arrays above do not change). */
#define NUM_BINDS 10
enum {
    ROW_VIDEO = OPT_COUNT, /* entry to the Graphics page (video) */
    ROW_AUDIO,             /* entry to the Audio page */
    ROW_CONTROLS,          /* entry to the Controls page */
    ROW_BIND0,             /* ROW_BIND0 .. ROW_BIND0 + NUM_BINDS - 1: one N64 button each */
    ROW_DEAD = ROW_BIND0 + NUM_BINDS,
    ROW_CTRLRESET,
    ROW_MUSIC,    /* music volume (0 to 100) */
    ROW_SFX,      /* sound effects volume (0 to 100) */
    ROW_SOUNDMODE, /* Stereo / Mono / Headset: the same setting as the file select menu */
    ROW_AUDRESET,
    ROW_TVPAGE,    /* entry to the TV page (inside Graphics); can only be entered with the video cable plugged in */
    ROW_TVRESET    /* "Reset TV": leaves TV Aspect and TV Area at their defaults (does not touch TV Output or the rest of Graphics) */
};

enum { PG_MAIN, PG_VIDEO, PG_AUDIO, PG_CTRL, PG_TV };

static const u8 sPgMain[] = { ROW_VIDEO, ROW_AUDIO, ROW_CONTROLS };
static const u8 sPgAudio[] = { ROW_MUSIC, ROW_SFX, ROW_SOUNDMODE, ROW_AUDRESET };
static const u8 sPgVideo[] = { OPT_FPSCAP, OPT_PACING, OPT_OBJDIST, OPT_FARDIST, OPT_CULLOBJ, OPT_SHOWFPS, OPT_SOUNDCPU, ROW_TVPAGE, OPT_RESET };
static const u8 sPgTv[] = { OPT_TVOUT, OPT_TVASPECT, OPT_TVAREA, OPT_TVAREAY, OPT_TVSHIFT, ROW_TVRESET };
static const u8 sPgCtrl[] = { ROW_BIND0, ROW_BIND0 + 1, ROW_BIND0 + 2, ROW_BIND0 + 3, ROW_BIND0 + 4, ROW_BIND0 + 5, ROW_BIND0 + 6,
                              ROW_BIND0 + 7, ROW_BIND0 + 8, ROW_BIND0 + 9, ROW_DEAD, ROW_CTRLRESET };
/* The titles (sTrTitle, below) use the HUD's color font: in the US version only the letters A-I, K-P, R-U, W, Y exist (no J, Q, V, X or Z;
 * in the European one J, Q and X are missing) and a missing letter hangs the drawing; that is why the video page is called "Graphics" and not "Video", and the titles in
 * French and German only use letters that exist. */

/* N64 buttons and their default assignment on the PSP (the same as in configfile.c / controller_psp.c). */
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
#define DEAD_DEF 32 /* default stick dead zone (the one from configfile.c) */
#define DEAD_MAX 80

/* Physical buttons that can be assigned (in this order one is preferred if several are pressed at once). */
static const struct { const char *name; unsigned int mask; } sPhys[] = {
    { "Cross", PSP_CTRL_CROSS },     { "Circle", PSP_CTRL_CIRCLE }, { "Square", PSP_CTRL_SQUARE },   { "Triangle", PSP_CTRL_TRIANGLE },
    { "L Trig", PSP_CTRL_LTRIGGER }, { "R Trig", PSP_CTRL_RTRIGGER }, { "Select", PSP_CTRL_SELECT }, { "Start", PSP_CTRL_START },
    { "D-Up", PSP_CTRL_UP },         { "D-Down", PSP_CTRL_DOWN },    { "D-Left", PSP_CTRL_LEFT },     { "D-Right", PSP_CTRL_RIGHT },
};
#define NUM_PHYS ((int) (sizeof(sPhys) / sizeof(sPhys[0])))
#define BINDABLE (PSP_CTRL_CROSS | PSP_CTRL_CIRCLE | PSP_CTRL_SQUARE | PSP_CTRL_TRIANGLE | PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER | PSP_CTRL_SELECT \
                  | PSP_CTRL_START | PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_LEFT | PSP_CTRL_RIGHT)
#define CAPTURE_TICKS 150 /* ~5 s at 30 ticks per second */

/* Menu rule: no value leaves the game below how it looks on the N64. The distance rows start at 100 % (the original) and
 * only go up; the rest (fps limit, pacing, counter) does not change what is drawn. */
int gPspOptFpsCap = 60;   /* fps limit: 30 / 60 */
int gPspOptPipe = 1;      /* Triple Buffer: the CPU starts the next frame without waiting for the previous one to reach the screen (gfx_scegu.c) */
int gPspOptKeepSpeed = 0; /* 0 = "Keep FPS": always tries for the limit (if it does not fit, the game runs slower); 1 = "Keep Speed": lowers the fps so as not to lose speed */
float gPspObjDistScale = 1.5f; /* multiplies the distance at which objects are deactivated (>= 1.0); default 150 % */
float gPspFarScale = 1.5f;     /* multiplies the camera's far plane (>= 1.0); default 150 % */
int gPspOptTvAspect = 1;       /* TV Aspect: 0 = 4:3 (16:9 image with bars), 1 = 16:9 (fills the TV frame) */
int gPspOptTvArea = 100;       /* TV Width: percentage of the TV frame's WIDTH that the image takes (60 to 100); the rest stays black, for TVs that crop the edges */
int gPspOptTvAreaY = 100;      /* TV Height: the same for the HEIGHT (60 to 100) */
int gPspOptTvShiftY = 0;       /* TV Vertical Shift: lines (of 480) that the image goes down inside the TV frame; negative = up (-60 to 60) */
int gPspOptTv = 0;             /* TV Output (psp_dve.h): 0 Off / 1 240p / 2 480i / 3 480p; applied immediately */
int gPspOptCullObj = 1;        /* 0 = original object culling (horizontal only), 1 = vertical too (default) */
float gPspVolMusic = 1.0f;     /* music volume, 0.0 to 1.0 (1.0 = the game's original); read by effects.c */
float gPspVolSfx = 1.0f;       /* sound effects volume, 0.0 to 1.0 */
int gPspClockWant = 333;       /* fixed CPU clock (no longer an option): requested by gfx_psp.c at boot */
unsigned int gPspOptChanges;   /* how many times an option was changed (read by the performance log) */


/* Each row has its values from lowest to highest (right = more), and sDef is the index of the default value. */
static const char *const sFpsTxt[2] = { "30", "60" };
static const int sFpsCap[2] = { 30, 60 };
static const char *const sPacingTxt[2] = { "Keep FPS", "Keep Speed" };
static const char *const sOnOffTxt[2] = { "Off", "On" };
static const char *const sFpsCtrTxt[2] = { "Off", "On" };
/* Object Distance and Far Distance: from 100 % (the original) to 300 %. (The "Unlimited" option was removed: it broke things.) The objects' depth limit is
 * 20000 * scale and must not exceed 65536 (see obj_is_in_view): at 300 % it ends up at 60000. */
static const char *const sObjTxt[5] = { "100%", "150%", "200%", "250%", "300%" };
static const float sObjScale[5] = { 1.0f, 1.5f, 2.0f, 2.5f, 3.0f };
static const char *const sFarTxt[5] = { "100%", "150%", "200%", "250%", "300%" };
static const float sFarScale[5] = { 1.0f, 1.5f, 2.0f, 2.5f, 3.0f }; /* camera far plane; the level of detail is always the maximum */
#define DIST_ROW_MAX 4 /* index of the highest value of the distance rows (300 %) */
static const char *const sCullTxt[2] = { "Original", "Vertical" };

static const char *const sTvTxt[4] = { "Off", "240p", "480i", "480p" };
static const char *const sTvAspTxt[2] = { "4:3", "16:9" };
#define TV_SHIFT_MAX 60 /* value i of the row is i - TV_SHIFT_MAX lines */
static const char *const sTvShiftTxt[121] = { "-60","-59","-58","-57","-56","-55","-54","-53","-52","-51","-50","-49","-48","-47","-46","-45","-44","-43","-42","-41","-40","-39","-38","-37","-36","-35","-34","-33","-32","-31","-30","-29","-28","-27","-26","-25","-24","-23","-22","-21","-20","-19","-18","-17","-16","-15","-14","-13","-12","-11","-10","-9","-8","-7","-6","-5","-4","-3","-2","-1","0","1","2","3","4","5","6","7","8","9","10","11","12","13","14","15","16","17","18","19","20","21","22","23","24","25","26","27","28","29","30","31","32","33","34","35","36","37","38","39","40","41","42","43","44","45","46","47","48","49","50","51","52","53","54","55","56","57","58","59","60" };
#define TV_AREA_MIN 60 /* value i of the row is TV_AREA_MIN + i percent */
static const char *const sTvAreaTxt[41] = { "60%","61%","62%","63%","64%","65%","66%","67%","68%","69%","70%","71%","72%","73%","74%","75%","76%","77%","78%","79%","80%","81%","82%","83%","84%","85%","86%","87%","88%","89%","90%","91%","92%","93%","94%","95%","96%","97%","98%","99%","100%" };
static const int sCount[OPT_COUNT] = { 2, 2, 2, 2, 5, 5, 2, 4, 2, 41, 41, 121, 0 };
static const int sDef[OPT_COUNT] = { 1, 0, 0, 0, 1, 1, 1, 0, 1, 40, 40, 60, 0 }; /* 60 fps, Keep FPS, counter Off, Object / Far Distance 150 %, culling Vertical */

static int sVal[OPT_COUNT] = { 1, 0, 0, 0, 1, 1, 1, 0, 1, 40, 40, 60, 0 }; /* index chosen in each row */
#define VOL_STEP 5 /* the menu raises / lowers the volume in steps of 5 */
static int sMusic = 100; /* 0 a 100 */
static int sSfx = 100;
static int sInit;
static int sOpen;
static int sSel;
static int sScroll;
static int sPage;           /* PG_MAIN / PG_VIDEO / PG_AUDIO / PG_CTRL / PG_TV */
static int sCapture = -1;   /* index (0..NUM_BINDS-1) of the button waiting for a physical button, -1 = none */
static int sCaptureTimer;
static int sWaitRelease;    /* after assigning: ignore everything until the buttons are released (the mapping just changed) */
static unsigned int sPrevRaw;
static int sJustOpened;
static int sPrevX, sPrevY, sTimeX, sTimeY;
static int sNavBuzzed; /* the buzz already sounded on this press (the cursor cannot advance: what follows is greyed out) */

static void psp_opt_apply(void) {
    gPspOptFpsCap = sFpsCap[sVal[OPT_FPSCAP]];
    gPspOptKeepSpeed = sVal[OPT_PACING];
    gPspOptPipe = 1; /* Triple Buffer: always on (no longer an option) */
    gPspObjDistScale = sObjScale[sVal[OPT_OBJDIST]];
    gPspFarScale = sFarScale[sVal[OPT_FARDIST]];
    gPspOptCullObj = sVal[OPT_CULLOBJ];
    gPspOptTv = sVal[OPT_TVOUT];
    gPspOptTvAspect = sVal[OPT_TVASPECT];
    gPspOptTvArea = TV_AREA_MIN + sVal[OPT_TVAREA];
    gPspOptTvAreaY = TV_AREA_MIN + sVal[OPT_TVAREAY];
    gPspOptTvShiftY = sVal[OPT_TVSHIFT] - TV_SHIFT_MAX;
    gPspVolMusic = (float) sMusic * 0.01f;
    gPspVolSfx = (float) sSfx * 0.01f;
    /* "FPS Counter": 0 = off, 1 = on-screen counter. No file log (final version). */
    gRenderFPS = sVal[OPT_SHOWFPS] ? 1 : 0;
    gPspFpsLogOff = 1;
}

/* OLD options file: "SM64OPT 7" and then one "key value" line per option (missing or unrecognized ones keep their default
 * value, so options can be added without invalidating the file). From versions 2 to 5 only FPS Limit and Frame Pacing are read: Object Distance,
 * Far Distance and Object Culling take the new defaults (150 % / 150 % / Vertical), and Triple Buffer and CPU Clock are no longer options. */
static const struct { const char *key; int row; } sSaveKeys[] = {
    { "fps", OPT_FPSCAP }, { "pace", OPT_PACING }, { "scpu", OPT_SOUNDCPU }, { "obj", OPT_OBJDIST }, { "farp", OPT_FARDIST }, { "cull", OPT_CULLOBJ },
};
#define SAVE_KEYS ((int) (sizeof(sSaveKeys) / sizeof(sSaveKeys[0])))

/* value from an earlier version (up to 5) -> current index (-1 = not read, the default value stays) */
static int psp_opt_from_old(int row, int v) {
    switch (row) {
        case OPT_FPSCAP: /* before 30 / 40 / 45 / 50 / 60: now 30 / 60 */
            return (v >= 2) ? 1 : 0;
        case OPT_PACING:
            return v;
        default:
            return -1;
    }
}

/* Reads the OLD options file (sm64_options.txt) to migrate the settings the first time. */
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
            static const int rows[7] = { OPT_FPSCAP, OPT_PACING, -1, -1, -1, -1, -1 }; /* the rest is no longer read */
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
                        /* version 6: Object / Far Distance did not have 250 % (300 % and Unlimited were indices 3 and 4) */
                        if (ver == 6 && (sSaveKeys[i].row == OPT_OBJDIST || sSaveKeys[i].row == OPT_FARDIST) && val >= 3) {
                            nv = val + 1;
                        }
                        /* "Unlimited" (no longer exists) becomes 300 % */
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
        psp_options_load_file(CTRL_FILE); /* normally configfile_load already did it at boot */
    }
}

/* Repeats the movement while the stick / D-pad is held: first it waits ~8 ticks and then advances every 3. */
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

/* "Cross", "Circle/L Trig"... (an N64 button can have several physical buttons, e.g. Z by default) */
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

/* Same as psp_opt_mask_text but with the names in the menu's language (to display on screen; the file always uses the English ones) */
static void psp_opt_mask_disp(unsigned int mask, char *out, int max);
static const char *psp_opt_phys_disp(int i);

/* Sound mode (0 Stereo, 1 Mono, 2 Headset): same indices as the file select menu. Applied instantly; saving is done on close. */
static int sSndMode = 0;
static int sSndDirty = 0;
static void psp_opt_set_sound_mode(int mode) {
    if (mode != sSndMode) {
        sSndMode = mode;
        set_sound_mode((u16) mode);
        sSndDirty = 1;
    }
}

/* ------------------------------------------------------------------------------------------------------------------------------------------
 * Menu language. It is chosen automatically, according to the game version: US = English; EU = the language chosen in the game (English / French / German);
 * JP and Shindou = Japanese (kana only: the game's font has no kanji). Texts are in UTF-8 and psp_opt_str() converts them to the text codes of
 * each version. The sm64config.txt file always keeps using the English names (the tables above take care of that).
 * ------------------------------------------------------------------------------------------------------------------------------------------ */
enum { LANG_EN, LANG_FR, LANG_DE, LANG_JP, LANG_COUNT };
#define TR(en, fr, de, jp) { en, fr, de, jp }
#define LT(t) ((t)[psp_opt_lang()])

static int psp_opt_lang(void) {
#if defined(VERSION_JP) || defined(VERSION_SH)
    return LANG_JP;
#elif defined(VERSION_EU)
    const u16 l = eu_get_language();
    return (l == 1) ? LANG_FR : ((l == 2) ? LANG_DE : LANG_EN);
#else
    return LANG_EN;
#endif
}

static const char *const sTrTitle[5][LANG_COUNT] = {
    TR("OPTIONS", "OPTIONS", "OPTIONEN", "オプション"),
    TR("GRAPHICS", "GRAPHISMES", "GRAFIK", "グラフィック"),
    TR("AUDIO", "AUDIO", "AUDIO", "オーディオ"),
    TR("CONTROLS", "COMMANDES", "STEUERUNG", "コントロール"),
    TR("OUTPUT", "SORTIE", "AUSGABE", "TVシュツリョク"), /* no "TV": in the US version the letter V does not exist in the color font */
};
static const char *const sTrLabel[OPT_COUNT][LANG_COUNT] = {
    TR("FPS Limit", "Limite de FPS", "FPS-Limit", "FPSセイゲン"),
    TR("Frame Pacing", "Cadence", "Bildtempo", "フレームペース"),
    TR("FPS Counter", "Compteur FPS", "FPS-Anzeige", "FPSヒョウジ"),
    TR("Show Sound CPU", "Afficher CPU son", "Sound-CPU zeigen", "サウンドCPUヒョウジ"),
    TR("Object Distance", "Distance objets", "Objektdistanz", "オブジェクトキョリ"),
    TR("Far Distance", "Distance lointaine", "Fernsicht", "エンキョリ"),
    TR("Object Culling", "Masquage objets", "Objekt-Culling", "オブジェクトカリング"),
    TR("TV Output", "Sortie TV", "TV-Ausgabe", "TVシュツリョク"),
    TR("TV Aspect", "Format TV", "TV-Format", "TVヒリツ"),
    TR("TV Width", "Largeur TV", "TV-Breite", "TVヨコハバ"),
    TR("TV Height", "Hauteur TV", "TV-Höhe", "TVタカサ"),
    TR("TV Vertical Shift", "Décalage vertical TV", "TV vertikal", "TVタテイチ"),
    TR("Reset to Defaults", "Réinitialiser", "Zurücksetzen", "ショキチニモドス"),
};
static const char *const sTrPacing[2][LANG_COUNT] = {
    TR("Keep FPS", "Garder les FPS", "FPS halten", "FPSユウセン"),
    TR("Keep Speed", "Garder la vitesse", "Tempo halten", "スピードユウセン"),
};
static const char *const sTrOnOff[2][LANG_COUNT] = {
    TR("Off", "Non", "Aus", "オフ"),
    TR("On", "Oui", "Ein", "オン"),
};
static const char *const sTrCull[2][LANG_COUNT] = {
    TR("Original", "Original", "Original", "オリジナル"),
    TR("Vertical", "Vertical", "Vertikal", "スイチョク"),
};
static const char *const sTrUp[LANG_COUNT] = TR("Up", "Haut", "Hoch", "ウエ");
static const char *const sTrDown[LANG_COUNT] = TR("Down", "Bas", "Runter", "シタ");
static const char *const sTrTvReset[LANG_COUNT] = TR("Reset TV Settings", "Réinit. réglages TV", "TV zurücksetzen", "TVセッテイヲショキカ");
static const char *const sTrTvSafe[LANG_COUNT] = TR("Safe mode", "Mode sûr", "Sicherer Modus", "アンゼン");
static const char *const sTrTvHelp[LANG_COUNT] = TR("L: Reconnect TV", "L: Reconnecter la TV", "L: TV neu verbinden", "L: TVサイセツゾク");
static const char *const sTrCable[3][LANG_COUNT] = {
    TR("No cable", "Pas de cable", "Kein Kabel", "No cable"),
    TR("Composite cable", "Cable composite", "Composite-Kabel", "Composite cable"),
    TR("Component cable", "Cable composante", "Komponenten-Kabel", "Component cable"),
};
static const char *const sTrTvKeep[LANG_COUNT] = TR("A: keep %s (%d)", "A: garder %s (%d)", "A: %s behalten (%d)", "A: OK %s (%d)");
static const char *const sTrSndMode[3][LANG_COUNT] = {
    TR("Stereo", "Stéréo", "Stereo", "ステレオ"),
    TR("Mono", "Mono", "Mono", "モノラル"),
    TR("Headset", "Casque", "Kopfhörer", "ヘッドホン"),
};
static const char *const sTrPage[4][LANG_COUNT] = {
    TR("Graphics", "Graphismes", "Grafik", "グラフィック"),
    TR("Audio", "Audio", "Audio", "オーディオ"),
    TR("Controls", "Commandes", "Steuerung", "コントロール"),
    TR("TV Settings", "Réglages TV", "TV-Einstellungen", "TVセッテイ"),
};
static const char *const sTrAudio[3][LANG_COUNT] = {
    TR("Music Volume", "Volume musique", "Musiklautstärke", "オンガクボリューム"),
    TR("Effects Volume", "Volume effets", "Effektlautstärke", "コウカオンボリューム"),
    TR("Sound Mode", "Mode sonore", "Tonmodus", "サウンドモード"),
};
static const char *const sTrDead[LANG_COUNT] = TR("Stick Deadzone", "Zone morte du stick", "Stick-Totzone", "スティックデッドゾーン");
static const char *const sTrBind[NUM_BINDS][LANG_COUNT] = {
    TR("A Button", "Bouton A", "A-Taste", "Aボタン"),
    TR("B Button", "Bouton B", "B-Taste", "Bボタン"),
    TR("Start", "Start", "Start", "スタート"),
    TR("L Trigger", "Gâchette L", "L-Taste", "Lトリガー"),
    TR("R Trigger", "Gâchette R", "R-Taste", "Rトリガー"),
    TR("Z Trigger", "Gâchette Z", "Z-Taste", "Zトリガー"),
    TR("C-Up", "C-Haut", "C-Hoch", "Cウエ"),
    TR("C-Down", "C-Bas", "C-Runter", "Cシタ"),
    TR("C-Left", "C-Gauche", "C-Links", "Cヒダリ"),
    TR("C-Right", "C-Droite", "C-Rechts", "Cミギ"),
};
/* physical PSP buttons, in the same order as sPhys[] */
static const char *const sTrPhys[12][LANG_COUNT] = {
    TR("Cross", "Croix", "Kreuz", "バツ"),
    TR("Circle", "Rond", "Kreis", "マル"),
    TR("Square", "Carré", "Quadrat", "シカク"),
    TR("Triangle", "Triangle", "Dreieck", "サンカク"),
    TR("L Trig", "L", "L", "L"),
    TR("R Trig", "R", "R", "R"),
    TR("Select", "Select", "Select", "セレクト"),
    TR("Start", "Start", "Start", "スタート"),
    TR("D-Up", "Haut", "Hoch", "ウエ"),
    TR("D-Down", "Bas", "Runter", "シタ"),
    TR("D-Left", "Gauche", "Links", "ヒダリ"),
    TR("D-Right", "Droite", "Rechts", "ミギ"),
};
static const char *const sTrNone[LANG_COUNT] = TR("None", "Aucun", "Keine", "ナシ");
static const char *const sTrPress[LANG_COUNT] = TR("Press a button", "Appuyez sur un bouton", "Taste drücken", "ボタンをおして");
static const char *const sTrBack[LANG_COUNT] = TR("B: Return", "B: Retour", "B: Zurück", "B: モドル");
static const char *const sTrChange[LANG_COUNT] = TR("A: Change", "A: Changer", "A: Ändern", "A: ヘンコウ");
static const char *const sTrWait[LANG_COUNT] = TR("Wait to cancel", "Patientez pour annuler", "Warten zum Abbrechen", "ホウチデキャンセル");
static const char *const sTrOptions[LANG_COUNT] = TR("Options", "Options", "Optionen", "オプション");

static const char *psp_opt_label(int row) {
    if (row < OPT_COUNT) return LT(sTrLabel[row]);
    if (row == ROW_VIDEO) return LT(sTrPage[0]);
    if (row == ROW_AUDIO) return LT(sTrPage[1]);
    if (row == ROW_MUSIC) return LT(sTrAudio[0]);
    if (row == ROW_SFX) return LT(sTrAudio[1]);
    if (row == ROW_SOUNDMODE) return LT(sTrAudio[2]);
    if (row == ROW_AUDRESET) return LT(sTrLabel[OPT_RESET]);
    if (row == ROW_CONTROLS) return LT(sTrPage[2]);
    if (row == ROW_TVPAGE) return LT(sTrPage[3]);
    if (row == ROW_TVRESET) return LT(sTrTvReset);
    if (row == ROW_DEAD) return LT(sTrDead);
    if (row == ROW_CTRLRESET) return LT(sTrLabel[OPT_RESET]);
    return LT(sTrBind[row - ROW_BIND0]);
}

/* single-line rows (page entries and "Reset"): the text is centered in the box, with no value line */
static int psp_opt_is_action(int row) {
    return row == OPT_RESET || row == ROW_VIDEO || row == ROW_AUDIO || row == ROW_CONTROLS || row == ROW_TVPAGE || row == ROW_TVRESET || row == ROW_CTRLRESET || row == ROW_AUDRESET;
}

/* can the "TV Output" row be used? (module loaded + slim PSP; the cable is not required). Queried now and then, not every frame. */
static int sNoCable = 0; /* cable not detected (queried every ~0.25 s while drawing the menu): the TV row is shown grey and does not go up */
static int psp_opt_tv_ready(void) {
    static int sPoll = 0, sReady = 0;
    if (--sPoll <= 0) {
        sPoll = 20;
        sReady = psp_dve_tv_ready();
    }
    return sReady;
}

/* can the TV page be entered? It needs the TV module and the video cable plugged in (cable_state: 0 = none; -1 = unknown, let through), whatever value TV Output
 * has: without a cable the game already returned the image to the console screen (the setting is kept and the TV turns on by itself when the cable is connected). */
static int psp_opt_tv_access_now(void) {
    return psp_dve_tv_ready() && psp_dve_cable_state() != 0;
}

/* With TV Output at Off nothing else on the TV page works (format, width, height and reset): there is no TV image to adjust. They are shown grey and do not change. */
static int psp_opt_tv_row_off(int row) {
    return (row == OPT_TVASPECT || row == OPT_TVAREA || row == OPT_TVAREAY || row == OPT_TVSHIFT || row == ROW_TVRESET) && sVal[OPT_TVOUT] == 0;
}
static int sTvAcc; /* copy updated every ~0.25 s while drawing the menu */

static const char *psp_opt_value_text(int row) {
    static char buf[40];
    switch (row) {
        case OPT_FPSCAP: return sFpsTxt[sVal[row]];
        case OPT_PACING: return LT(sTrPacing[sVal[row]]);
        case OPT_SHOWFPS: return LT(sTrOnOff[sVal[row]]);
        case OPT_SOUNDCPU: return LT(sTrOnOff[sVal[row]]);
        case OPT_OBJDIST: return sObjTxt[sVal[row]];
        case OPT_FARDIST: return sFarTxt[sVal[row]];
        case OPT_CULLOBJ: return LT(sTrCull[sVal[row]]);
        case OPT_TVASPECT: return sTvAspTxt[sVal[row]];
        case OPT_TVAREA:
        case OPT_TVAREAY: return sTvAreaTxt[sVal[row]];
        case OPT_TVSHIFT: {
            const int sh = sVal[row] - TV_SHIFT_MAX;
            if (sh == 0) {
                snprintf(buf, sizeof(buf), "0");
            } else {
                snprintf(buf, sizeof(buf), "%s %d", LT(sh > 0 ? sTrDown : sTrUp), sh > 0 ? sh : -sh);
            }
            return buf;
        }
        case OPT_TVOUT:
            if (!psp_opt_tv_ready()) {
                /* cause + code (to know why it is not available: module not loaded, PSP-1000 or safe mode) */
                int det;
                switch (psp_dve_status(&det)) {
                    case PSP_DVE_LOAD_FAILED: snprintf(buf, sizeof(buf), "Plugin error %08X", (unsigned int) det); break;
                    case PSP_DVE_PSP1000: snprintf(buf, sizeof(buf), "PSP-1000 (model %d)", det); break;
                    default: snprintf(buf, sizeof(buf), "%s", LT(sTrTvSafe)); break;
                }
                return buf;
            }
            return (sVal[row] == 0) ? LT(sTrOnOff[0]) : sTvTxt[sVal[row]];
        case ROW_DEAD:
            snprintf(buf, sizeof(buf), "%u", configDeadzone);
            return buf;
        case ROW_MUSIC:
            snprintf(buf, sizeof(buf), "%d%%", sMusic);
            return buf;
        case ROW_SFX:
            snprintf(buf, sizeof(buf), "%d%%", sSfx);
            return buf;
        case ROW_SOUNDMODE: return LT(sTrSndMode[sSndMode]);
        default:
            if (row >= ROW_BIND0 && row < ROW_BIND0 + NUM_BINDS) {
                if (sCapture == row - ROW_BIND0) {
                    return LT(sTrPress);
                }
                psp_opt_mask_disp(*sBind[row - ROW_BIND0].mask, buf, sizeof(buf));
                return buf;
            }
            return "";
    }
}

/* are there more values to the left / right? (for the arrows) */
/* Greyed-out row (not usable now): shown grey and the cursor skips it. */
/* Exception: only the "TV Settings" row (in Graphics) can be selected even if it is greyed out, so that the reason (no cable, etc.) can be read below; pressing X plays the buzz. */
static int psp_opt_row_grey(int row) {
    return (row == OPT_TVOUT && (!psp_opt_tv_ready() || (sVal[OPT_TVOUT] == 0 && sNoCable))) || (row == ROW_TVPAGE && !sTvAcc) || psp_opt_tv_row_off(row);
}
static int psp_opt_row_skip(int row) {
    return psp_opt_row_grey(row) && row != ROW_TVPAGE;
}

static int psp_opt_can_dec(int row) {
    if (psp_opt_tv_row_off(row)) return 0;
    if (row == ROW_DEAD) return configDeadzone > 0;
    if (row == ROW_MUSIC) return sMusic > 0;
    if (row == ROW_SFX) return sSfx > 0;
    if (row == ROW_SOUNDMODE) return sSndMode > 0;
    if (row == OPT_TVOUT) return psp_opt_tv_ready() && sVal[row] > 0;
    return row < OPT_RESET && sVal[row] > 0;
}
static int psp_opt_can_inc(int row) {
    if (psp_opt_tv_row_off(row)) return 0;
    if (row == ROW_DEAD) return configDeadzone < DEAD_MAX;
    if (row == ROW_MUSIC) return sMusic < 100;
    if (row == ROW_SFX) return sSfx < 100;
    if (row == ROW_SOUNDMODE) return sSndMode < 2;
    if (row == OPT_TVOUT) return psp_opt_tv_ready() && psp_dve_cable_state() != 0 && sVal[row] < ((psp_dve_cable() == PSP_CABLE_COMPONENT) ? 3 : 2);
    return row < OPT_RESET && sVal[row] < sCount[row] - 1;
}

/* No wrap-around: on the first/last value nothing happens (the arrows at the sides show which way you can go).
 * wrap = 1 only for the A button on two-value rows (Yes/No), where "next" means toggle. */
static void psp_opt_change(int row, int dir, int wrap) {
    int nv;
    if (psp_opt_tv_row_off(row)) {
        return;
    }
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
        play_sound(SOUND_MENU_CHANGE_SELECT, gDefaultSoundArgs); /* it is an effect: it is heard at the new volume */
        return;
    }
    if (row == OPT_TVOUT) {
        /* without module / PSP-1000 / safe mode the row is greyed out: the "cannot move any further" buzz plays */
        const int maxv = (psp_dve_cable() == PSP_CABLE_COMPONENT) ? 3 : 2; /* 480p only with a component cable; 480i and 240p also with the composite one */
        if (!psp_opt_tv_ready()) {
            play_sound(SOUND_MENU_CAMERA_BUZZ, gDefaultSoundArgs);
            return;
        }
        nv = sVal[row] + dir;
        if (nv > sVal[row] && nv <= maxv && psp_dve_cable_state() == 0) {
            play_sound(SOUND_MENU_CAMERA_BUZZ, gDefaultSoundArgs); /* cable required: without a detected cable you cannot switch to a TV mode (going down to Off is always possible) */
            return;
        }
        if (nv < 0 || nv > maxv) {
            if (!wrap) {
                if (nv > maxv && nv < sCount[row]) {
                    play_sound(SOUND_MENU_CAMERA_BUZZ, gDefaultSoundArgs); /* 480p without a component cable */
                }
                return;
            }
            nv = 0; /* A button: back to the start */
        }
        sVal[row] = nv;
        gPspOptChanges++;
        psp_opt_apply();
        gfx_scegu_tv_request(nv); /* applied within a few frames, without restarting the game (only 480p stays "on trial": see psp_options_pause_frame) */
        play_sound(SOUND_MENU_CHANGE_SELECT, gDefaultSoundArgs);
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
    if (row == OPT_TVASPECT) {
        gfx_scegu_tv_refresh(); /* with the TV on, the buffers are rebuilt with the new format */
    } else if (row == OPT_TVAREA || row == OPT_TVAREAY || row == OPT_TVSHIFT) {
        gfx_scegu_tv_area_refresh(); /* only the image rectangle changes: fast, it can be adjusted by holding the D-pad */
    }
    play_sound(SOUND_MENU_CHANGE_SELECT, gDefaultSoundArgs);
}

/* Graphics "Reset to Defaults": does NOT touch anything TV-related (those settings have their own reset on the TV page). */
static int psp_opt_is_tv_row(int row) {
    return row == OPT_TVOUT || row == OPT_TVASPECT || row == OPT_TVAREA || row == OPT_TVAREAY || row == OPT_TVSHIFT;
}

static void psp_opt_reset(void) {
    int i;
    for (i = 0; i < OPT_RESET; i++) {
        if (!psp_opt_is_tv_row(i)) {
            sVal[i] = sDef[i];
        }
    }
    gPspOptChanges++;
    psp_opt_apply();
}

/* "Reset TV Settings": TV Aspect, TV Width and TV Height go back to their default. TV Output is left as is (it is whether the TV is on, not an image setting). */
static void psp_opt_reset_tv(void) {
    sVal[OPT_TVASPECT] = sDef[OPT_TVASPECT];
    sVal[OPT_TVAREA] = sDef[OPT_TVAREA];
    sVal[OPT_TVAREAY] = sDef[OPT_TVAREAY];
    sVal[OPT_TVSHIFT] = sDef[OPT_TVSHIFT];
    gPspOptChanges++;
    psp_opt_apply();
    gfx_scegu_tv_refresh(); /* with the TV on, the buffers are rebuilt with the default format and area */
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

/* Assigns physical button b to N64 button i. If another action was already using it they are swapped (that one receives what i had), so no action
 * is left without a button and no two actions share a button. */
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
 * Single settings file (sm64config.txt): readable text, "name = value" with comments. It is written in full when the menu is closed and when leaving.
 * ------------------------------------------------------------------------------------------------------------------------------------------ */

static char sCfgName[40] = CTRL_FILE;
static int sSawNew; /* the file had at least one setting of the new format (if not, it is an old one and gets migrated) */

static const char *const sRowKey[OPT_COUNT] = {
    "fps_limit", "frame_pacing", "fps_counter", "show_sound_cpu", "object_distance", "far_distance", "object_culling", "tv_output", "tv_aspect", "tv_width", "tv_height", "tv_shift_y", NULL,
};
static const char *const sBindKey[NUM_BINDS] = {
    "a_button", "b_button", "start", "l_trigger", "r_trigger", "z_trigger", "c_up", "c_down", "c_left", "c_right",
};
static const char *const sBindKeyOld[NUM_BINDS] = { /* names from the old sm64config.txt (the value was the numeric mask) */
    "key_a", "key_b", "key_start", "key_l", "key_r", "key_z", "key_cup", "key_cdown", "key_cleft", "key_cright",
};
/* other names accepted when reading physical buttons */
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
        case OPT_TVOUT: return sTvTxt;
        case OPT_TVASPECT: return sTvAspTxt;
        case OPT_TVAREA:
        case OPT_TVAREAY: return sTvAreaTxt;
        case OPT_TVSHIFT: return sTvShiftTxt;
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

/* integer at the start of the text (nothing odd: "abc" is not valid); returns 0 if it is not a number */
static int psp_parse_int(const char *s, int *out) {
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s || (*end != '\0' && *end != ' ' && *end != '\t')) {
        return 0;
    }
    *out = (int) v;
    return 1;
}

/* "Cross", "Circle/L Trig", "None" or (old file) a number -> button mask. Returns 0 if not understood. */
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

/* a file line: "name = value  # comment" (also "name value", like the old file); [sections] and comments are skipped */
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

    if (strcmp(key, "tv_area") == 0) { /* old name (a single value for width and height): read as both */
        v = atoi(val);
        if (v >= 50 && v <= 100) {
            sVal[OPT_TVAREA] = sVal[OPT_TVAREAY] = (v < TV_AREA_MIN) ? 0 : (v - TV_AREA_MIN);
            sSawNew = 1;
        }
        return;
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
            /* "Unlimited" (no longer exists) from an earlier sm64config.txt becomes 300 % */
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

/* A, B, Start and R cannot be left without a button (they are used to play, pause and open this menu): a file with "None" there gets corrected. */
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
        "Off, 240p, 480i or 480p (PSP slim/3000 with a video cable; applies right away; 480p asks you to confirm; hold L at start to force the PSP screen)",
        "4:3 (the original 4:3 view) or 16:9 (the widescreen view); both fill the video frame, so set the TV to its matching 4:3 / 16:9 mode",
        "60% to 100%, in steps of 1% (how much of the TV frame WIDTH the picture uses; lower it if your TV cuts off the left / right edges)",
        "60% to 100%, in steps of 1% (how much of the TV frame HEIGHT the picture uses; lower it if your TV cuts off the top / bottom edges; if the picture looks squashed, adjust it against the width)",
        "-60 to 60 (moves the picture vertically inside the TV frame, in lines of the 480-line signal; positive = down, negative = up; it needs room, so lower TV Height first)",
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
        psp_opt_load_legacy(); /* old installation: the graphics settings were in sm64_options.txt */
    }
    psp_opt_sanitize_controls();
    psp_opt_apply();
    if (!sSawNew) {
        psp_options_save_file(sCfgName); /* creates the file (or converts it to the new format) */
    }
}

/* mask -> physical button names in the menu's language */
static const char *psp_opt_phys_disp(int i) {
    return LT(sTrPhys[i]);
}

static void psp_opt_mask_disp(unsigned int mask, char *out, int max) {
    int i, n = 0;
    out[0] = '\0';
    for (i = 0; i < NUM_PHYS; i++) {
        if (mask & sPhys[i].mask) {
            n += snprintf(out + n, max - n, "%s%s", n ? "/" : "", psp_opt_phys_disp(i));
            if (n >= max - 1) {
                return;
            }
        }
    }
    if (n == 0) {
        snprintf(out, max, "%s", LT(sTrNone));
    }
}

#if defined(VERSION_JP) || defined(VERSION_SH)
/* game font codes for the kana U+3041..U+30FF (0 = does not exist); they come from charmap.txt */
static const u8 sKana[191] = {
    0xA5, 0x40, 0xA6, 0x41, 0xA7, 0x42, 0xA0, 0x43, 0xA8, 0x44, 0x45, 0x00,
    0x46, 0x00, 0x47, 0x00, 0x48, 0x00, 0x49, 0x00, 0x4A, 0x00, 0x4B, 0x00,
    0x4C, 0x00, 0x4D, 0x00, 0x4E, 0x00, 0x4F, 0x00, 0x50, 0x00, 0xA1, 0x51,
    0x00, 0x52, 0x00, 0x53, 0x00, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x00,
    0x00, 0x5A, 0x00, 0x00, 0x5B, 0x00, 0x00, 0x5C, 0x00, 0x00, 0x5D, 0x00,
    0x00, 0x5E, 0x5F, 0x60, 0x61, 0x62, 0xA2, 0x63, 0xA3, 0x64, 0xA4, 0x65,
    0x66, 0x67, 0x68, 0x69, 0x6A, 0x00, 0x6B, 0x00, 0x00, 0x6C, 0x6D, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0xF1, 0x00, 0x00, 0x00, 0x00,
    0xD5, 0x70, 0xD6, 0x71, 0xD7, 0x72, 0xD0, 0x73, 0xD8, 0x74, 0x75, 0x00,
    0x76, 0x00, 0x77, 0x00, 0x78, 0x00, 0x79, 0x00, 0x7A, 0x00, 0x7B, 0x00,
    0x7C, 0x00, 0x7D, 0x00, 0x7E, 0x00, 0x7F, 0x00, 0x80, 0x00, 0xD1, 0x81,
    0x00, 0x82, 0x00, 0x83, 0x00, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x00,
    0x00, 0x8A, 0x00, 0x00, 0x8B, 0x00, 0x00, 0x8C, 0x00, 0x00, 0x8D, 0x00,
    0x00, 0x8E, 0x8F, 0x90, 0x91, 0x92, 0xD2, 0x93, 0xD3, 0x94, 0xD4, 0x95,
    0x96, 0x97, 0x98, 0x99, 0x9A, 0x00, 0x9B, 0x00, 0x00, 0x9C, 0x9D, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFC, 0x9F, 0x00, 0x00, 0x00,
};

/* A kana with dakuten (゛) or handakuten (゜) is the mark (0xF0 / 0xF1) followed by the base kana; returns the offset to the base (1 or 2) or 0 */
static int psp_opt_kana_mark(u32 cp) {
    u32 k = cp;
    if (k >= 0x30A1) k -= 0x60; /* katakana -> equivalent hiragana */
    if ((k >= 0x304C && k <= 0x3062 && !(k & 1)) || k == 0x3065 || k == 0x3067 || k == 0x3069
        || k == 0x3070 || k == 0x3073 || k == 0x3076 || k == 0x3079 || k == 0x307C) {
        return 1;
    }
    if (k == 0x3071 || k == 0x3074 || k == 0x3077 || k == 0x307A || k == 0x307D) {
        return 2;
    }
    return 0;
}
#endif

/* UTF-8 text -> game text codes (those of the version being compiled): letters, numbers, a few symbols, and depending on the version accented letters
 * (EU) or kana (JP / Shindou). What does not exist in the font becomes a space. */
static void psp_opt_str(u8 *dst, const char *src, int max) {
    int n = 0;
    while (*src != '\0' && n < max - 3) {
        u32 cp = (u8) *src++;
        u8 d = 0x9E; /* space */
        if (cp >= 0xC0 && cp < 0xE0 && (*src & 0xC0) == 0x80) {
            cp = ((cp & 0x1F) << 6) | ((u8) *src++ & 0x3F);
        } else if (cp >= 0xE0 && cp < 0xF0 && (src[0] & 0xC0) == 0x80 && (src[1] & 0xC0) == 0x80) {
            cp = ((cp & 0x0F) << 12) | (((u8) src[0] & 0x3F) << 6) | ((u8) src[1] & 0x3F);
            src += 2;
        }
        if (cp >= '0' && cp <= '9') d = (u8) (cp - '0');
        else if (cp >= 'A' && cp <= 'Z') d = (u8) (cp - 'A' + 0x0A);
#if defined(VERSION_JP) || defined(VERSION_SH)
        /* the Japanese font only has UPPERCASE letters (no lowercase, ':' or apostrophe: a nonexistent glyph closes the game) */
        else if (cp >= 'a' && cp <= 'z') d = (u8) (cp - 'a' + 0x0A);
        else if (cp == '-') d = 0x9F;
        else if (cp == '%') d = 0xF3;
#else
        else if (cp >= 'a' && cp <= 'z') d = (u8) (cp - 'a' + 0x24);
        else if (cp == '-') d = 0x9F;
        else if (cp == '%') d = 0xF3;
        else if (cp == ':') d = 0xE6;
        else if (cp == '\'') d = 0x3E;
#endif
#if defined(VERSION_JP) || defined(VERSION_SH)
        /* JP / Shindou: the period '.' does not advance and the slash '/' is another character; kana go by table */
        else if (cp == '/') d = 0xFC;
        else if (cp == '.') d = 0x9E;
        else if (cp == 0x30FC) d = 0x9F;
        else if (cp >= 0x3041 && cp <= 0x30FF) {
            const int mk = psp_opt_kana_mark(cp);
            if (mk) {
                dst[n++] = (mk == 1) ? 0xF0 : 0xF1;
                cp -= (u32) mk;
            }
            d = sKana[cp - 0x3041];
            if (d == 0) d = 0x9E;
        }
#else
        else if (cp == '.') d = 0x3F;
        else if (cp == '/') d = 0xD0;
        else if (cp == '>') d = 0x53;
        else if (cp == '<') d = 0x52;
#ifdef VERSION_EU
        /* accented letters of the European font (codes from ingame_menu.h); in US they are written without accents */
        else if (cp == 0xE0) d = 0x60;
        else if (cp == 0xE2) d = 0x61;
        else if (cp == 0xE4) d = 0x62;
        else if (cp == 0xC4) d = 0x66;
        else if (cp == 0xE8) d = 0x70;
        else if (cp == 0xEA) d = 0x71;
        else if (cp == 0xEB) d = 0x72;
        else if (cp == 0xE9) d = 0x73;
        else if (cp == 0xC9) d = 0x77;
        else if (cp == 0xF9) d = 0x80;
        else if (cp == 0xFB) d = 0x81;
        else if (cp == 0xFC) d = 0x82;
        else if (cp == 0xDC) d = 0x86;
        else if (cp == 0xF4) d = 0x91;
        else if (cp == 0xF6) d = 0x92;
        else if (cp == 0xD6) d = 0x96;
        else if (cp == 0xEE) d = 0xA1;
        else if (cp == 0xEF) d = 0xA2;
        else if (cp == 0xDF) d = 0xEC;
        else if (cp == 0xE7) d = 0xEE;
#else
        else if (cp == 0xE0 || cp == 0xE2 || cp == 0xE4) d = 0x24;
        else if (cp == 0xC4) d = 0x0A;
        else if (cp >= 0xE8 && cp <= 0xEB) d = 0x28;
        else if (cp == 0xC9) d = 0x0E;
        else if (cp == 0xF9 || cp == 0xFB || cp == 0xFC) d = 0x38;
        else if (cp == 0xDC) d = 0x1E;
        else if (cp == 0xF4 || cp == 0xF6) d = 0x32;
        else if (cp == 0xD6) d = 0x18;
        else if (cp == 0xEE || cp == 0xEF) d = 0x2C;
        else if (cp == 0xE7) d = 0x26;
        else if (cp == 0xDF) d = 0x36;
#endif
#endif
        dst[n++] = d;
    }
    dst[n] = 0xFF; /* terminador */
}

/* width in pixels of an already converted text, same as each version's print_generic_string draws it */
extern u8 gDialogCharWidths[256];
static s16 psp_opt_width(const u8 *str) {
    int w = 0;
    for (; *str != 0xFF; str++) {
#if defined(VERSION_JP) || defined(VERSION_SH)
        if (*str == 0xF0 || *str == 0xF1) continue; /* dakuten / handakuten: zero-width marks */
        w += (*str == 0x9E) ? 5 : 10;
#else
        w += gDialogCharWidths[*str];
#endif
    }
    return (s16) w;
}

/* text centered at x with black shadow, white (col 0), red (col 1) or grey (col 2, disabled row); and measured from the bottom as in the dialogs */
static void psp_opt_text(s16 x, s16 y, const char *txt, int col) {
    u8 buf[96];
    s16 tx;

    psp_opt_str(buf, txt, sizeof(buf));
    tx = x - (psp_opt_width(buf) / 2);
    gDPSetEnvColor(gDisplayListHead++, 0, 0, 0, 255);
    print_generic_string(tx + 1, y - 1, buf);
    if (col == 2) {
        gDPSetEnvColor(gDisplayListHead++, 120, 120, 120, 255);
    } else if (col) {
        gDPSetEnvColor(gDisplayListHead++, 255, 32, 32, 255);
    } else {
        gDPSetEnvColor(gDisplayListHead++, 255, 255, 255, 255);
    }
    print_generic_string(tx, y, buf);
}

/* filled rectangle with the dialog box (130 x 80 base) */
static void psp_opt_box(s16 x, s16 ytop, s16 w, s16 h, u8 r, u8 g, u8 b, u8 a) {
    create_dl_translation_matrix(MENU_MTX_PUSH, x, ytop, 0);
    create_dl_scale_matrix(MENU_MTX_NOPUSH, w / 130.0f, h / 80.0f, 1.0f);
    gDPSetEnvColor(gDisplayListHead++, r, g, b, a);
    gSPDisplayList(gDisplayListHead++, dl_draw_text_bg_box);
    gSPPopMatrix(gDisplayListHead++, G_MTX_MODELVIEW);
}

#if defined(VERSION_JP) || defined(VERSION_SH)
/* 6 px white triangle made with stacked 2 px boxes: dir 0 = left, 1 = right, 2 = up, 3 = down; (x, ytop) = top-left corner */
static void psp_opt_tri(int dir, s16 x, s16 ytop) {
    int k;
    for (k = 0; k < 5; k++) {
        const int step = (k < 3) ? k : (4 - k); /* 0 1 2 1 0 */
        if (dir <= 1) { /* 5 rows of 2 px: widths 2 4 6 4 2 */
            const int w = 2 * (step + 1);
            psp_opt_box((dir == 0) ? (x + 6 - w) : x, ytop - 2 * k, w, 2, 255, 255, 255, 255);
        } else if (k < 3) { /* 3 rows of 2 px */
            const int w = 2 * ((dir == 2) ? (k + 1) : (3 - k));
            psp_opt_box(x + 3 - w / 2, ytop - 2 * k, w, 2, 255, 255, 255, 255);
        }
    }
}
#endif

static int psp_opt_row_index(const u8 *rows, int n, int row) {
    int i;
    for (i = 0; i < n; i++) {
        if (rows[i] == row) {
            return i;
        }
    }
    return 0;
}

static const u8 *psp_opt_page_rows(int *n) {
    switch (sPage) {
        case PG_VIDEO: *n = (int) sizeof(sPgVideo); return sPgVideo;
        case PG_AUDIO: *n = (int) sizeof(sPgAudio); return sPgAudio;
        case PG_CTRL: *n = (int) sizeof(sPgCtrl); return sPgCtrl;
        case PG_TV: *n = (int) sizeof(sPgTv); return sPgTv;
        default: *n = (int) sizeof(sPgMain); return sPgMain;
    }
}

static void psp_opt_draw(void) {
    u8 title[48];
    const u8 *rows;
    int n, i;

    rows = psp_opt_page_rows(&n);
    shade_screen();

    /* big title: HUD color letters (12 px per letter); in Japanese the HUD font has no kana, so it goes with the dialog
     * dialogs at double size */
    psp_opt_str(title, LT(sTrTitle[sPage]), sizeof(title));
    if (psp_opt_lang() == LANG_JP) {
        const f32 sc = 1.8f;
        const s16 tx = 160 - (s16) ((f32) psp_opt_width(title) * sc * 0.5f);
        int k;
        gSPDisplayList(gDisplayListHead++, dl_ia_text_begin);
        for (k = 0; k < 2; k++) {
            gDPSetEnvColor(gDisplayListHead++, k ? 255 : 0, k ? 255 : 0, k ? 255 : 0, 255);
            create_dl_translation_matrix(MENU_MTX_PUSH, tx + (k ? 0 : 1), 174 + (k ? 0 : -1), 0.0f);
            create_dl_scale_matrix(MENU_MTX_NOPUSH, sc, sc, 1.0f);
            print_generic_string(0, 0, title);
            gSPPopMatrix(gDisplayListHead++, G_MTX_MODELVIEW);
        }
        gSPDisplayList(gDisplayListHead++, dl_ia_text_end);
    } else {
        gSPDisplayList(gDisplayListHead++, dl_rgba16_text_begin);
        gDPSetEnvColor(gDisplayListHead++, 255, 255, 255, 255);
        print_hud_lut_string(HUD_LUT_GLOBAL, 160 - (int) strlen(LT(sTrTitle[sPage])) * 6, 40, title);
        gSPDisplayList(gDisplayListHead++, dl_rgba16_text_end);
    }

    /* scroll bar on the right (track and slider), the height of the 4 visible rows; only if there are more than 4 rows */
    if (n > 4) {
        const int sh = 118 * 4 / n; /* the slider measures what the 4 visible rows are relative to the total */
        psp_opt_box(272, 150, 8, 118, 128, 128, 128, 200);
        psp_opt_box(272, 150 - (118 - sh) * sScroll / (n - 4), 8, sh, 255, 255, 255, 255);
    }

    if (sPage == PG_TV && sVal[OPT_TVOUT] > 0) {
        /* white frame at the edge of the image: shows how far the image reaches on your TV (if any side is not visible, lower the width / height) */
        /* from edge to edge of the REAL image: in 16:9 it is wider than the classic screen's 320 (like the HUD, which sticks to the edges) */
        const int bl = GFX_DIMENSIONS_RECT_FROM_LEFT_EDGE(0), br = GFX_DIMENSIONS_RECT_FROM_RIGHT_EDGE(0);
        psp_opt_box(bl, 240, br - bl, 2, 255, 255, 255, 255);
        psp_opt_box(bl, 2, br - bl, 2, 255, 255, 255, 255);
        psp_opt_box(bl, 240, 2, 240, 255, 255, 255, 255);
        psp_opt_box(br - 2, 240, 2, 240, 255, 255, 255, 255);
    }
    {
        static int sNcPoll = 0;
        if (--sNcPoll <= 0) {
            sNcPoll = 15;
            sNoCable = (psp_opt_tv_ready() && psp_dve_cable_state() == 0);
            sTvAcc = psp_opt_tv_access_now();
        }
    }
    gSPDisplayList(gDisplayListHead++, dl_ia_text_begin);
    for (i = sScroll; i < sScroll + 4 && i < n; i++) {
        const int y = 140 - 32 * (i - sScroll);
        const int col = psp_opt_row_grey(rows[i]) ? 2 : ((i == sSel) ? 1 : 0);
        if (psp_opt_is_action(rows[i])) {
            psp_opt_text(160, y - 6, psp_opt_label(rows[i]), col);
        } else {
            psp_opt_text(160, y, psp_opt_label(rows[i]), col);
            psp_opt_text(160, y - 13, psp_opt_value_text(rows[i]), col);
        }
    }
    psp_opt_text(48, 14, LT(sTrBack), 0); /* bottom left corner */
    {
        const int left = gfx_scegu_tv_probation_left();
        if (left > 0) {
            char kb[40];
            snprintf(kb, sizeof(kb), LT(sTrTvKeep), sTvTxt[(sVal[OPT_TVOUT] > 0 && sVal[OPT_TVOUT] < 4) ? sVal[OPT_TVOUT] : 3], left);
            psp_opt_text(160, 14, kb, 1); /* 480p test: the box at the bottom center asks for confirmation */
        } else if (sPage == PG_TV && rows[sSel] == OPT_TVOUT && psp_opt_tv_ready()) {
            /* real cable state (queried every ~0.25 s); if unknown, with the TV on the L help (reconnect) is shown */
            static int sCabPoll = 0, sCab = -1;
            char db[64];
            if (--sCabPoll <= 0) {
                sCabPoll = 15;
                sCab = psp_dve_cable_state();
            }
            if (sCab >= 0 && sCab <= 2) {
                const int hz = gfx_scegu_tv_vbl_hz10();
                if (hz > 0) {
                    snprintf(db, sizeof(db), "%s | %d.%dHz", LT(sTrCable[sCab]), hz / 10, hz % 10); /* (measured vblank Hz: information for tuning the pacing) */
                } else {
                    snprintf(db, sizeof(db), "%s", LT(sTrCable[sCab]));
                } /* (when the cable is reconnected the TV is routed by itself; with L it can also be done by hand) */
                psp_opt_text(160, 14, db, 0);
            } else if (sVal[OPT_TVOUT] > 0) {
                psp_opt_text(160, 14, LT(sTrTvHelp), 0);
            }
        }
    }
    /* the TV row is greyed out: it says why (no cable, or the reason the TV module is not available) */
    if (sPage == PG_VIDEO && rows[sSel] == ROW_TVPAGE && !sTvAcc) {
        psp_opt_text(160, 14, psp_opt_tv_ready() ? LT(sTrCable[0]) : psp_opt_value_text(OPT_TVOUT), 0);
    }
    /* help at the bottom center, on the button rows */
    if (sPage == PG_CTRL && rows[sSel] >= ROW_BIND0 && rows[sSel] < ROW_BIND0 + NUM_BINDS) {
        psp_opt_text(160, 14, (sCapture >= 0) ? LT(sTrWait) : LT(sTrChange), 0);
    }
    /* arrows at the sides of the selected row's value: only the one on the side you can go to appears
     * (no left arrow = first value, no right arrow = last value) */
#if !defined(VERSION_JP) && !defined(VERSION_SH)
    gDPSetEnvColor(gDisplayListHead++, 255, 255, 255, 255);
    if (!psp_opt_is_action(rows[sSel])) {
        u8 l[8], r[8];
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
    /* up/down arrows next to the scroll bar: there are more rows in that direction */
    if (n > 4) {
        static const u8 up[2] = { 0x50, 0xFF };
        static const u8 dn[2] = { 0x51, 0xFF };
        /* centered on the bar: x = 273 (the glyph is ~7 wide and the bar goes from 272 to 280) and at the same distance (~6.5) from the track above
         * (the track starts at y = 150) and below (ends at y = 32); measured on a PSP screenshot. The bases differ because the bottom glyph takes up less of its box. */
        if (sScroll > 0) {
            print_generic_string(273, 151, up);
        }
        if (sScroll < n - 4) {
            print_generic_string(273, 14, dn);
        }
    }
#endif
    gSPDisplayList(gDisplayListHead++, dl_ia_text_end);
#if defined(VERSION_JP) || defined(VERSION_SH)
    /* the Japanese font has no arrows: they are drawn with stacked boxes (6 px triangles) */
    if (!psp_opt_is_action(rows[sSel])) {
        const int ay = 140 - 32 * (sSel - sScroll) - 13 + 12; /* above the value line */
        if (psp_opt_can_dec(rows[sSel])) {
            psp_opt_tri(0, 88, ay);
        }
        if (psp_opt_can_inc(rows[sSel])) {
            psp_opt_tri(1, 222, ay);
        }
    }
    if (n > 4) {
        if (sScroll > 0) {
            psp_opt_tri(2, 273, 158);
        }
        if (sScroll < n - 4) {
            psp_opt_tri(3, 273, 24);
        }
    }
#endif
}

/* "SOUND: CPU / ME": which processor synthesizes the audio (the main CPU or the Media Engine). It is drawn on both pauses (castle and levels) only if
 * the "Show Sound CPU" option is on. */
void psp_options_sound_info(void) {
    if (sVal[OPT_SOUNDCPU]) {
        print_text(0, 0, "SOUND:");
        print_text(0 + (4 * 16), 0, psp_me_is_active() ? "ME" : "CPU");
    }
}

/* "R Options" in the corner of the pause screen, while the menu is closed (called after drawing the normal pause).
 * If the N64 R button was reassigned, it shows the physical button that opens it now. */
void psp_options_pause_prompt(void) {
    char name[48], txt[96];
    u8 buf[96];
    s16 x;

    if (sOpen) {
        return;
    }
    if (configKeyR == PSP_CTRL_RTRIGGER) {
        snprintf(txt, sizeof(txt), "R %s", LT(sTrOptions));
    } else {
        psp_opt_mask_disp(configKeyR, name, sizeof(name));
        snprintf(txt, sizeof(txt), "%s %s", name, LT(sTrOptions));
    }
    if (psp_opt_lang() == LANG_EN) {
        x = 304 - (s16) strlen(txt) * 5; /* right-aligned, without going off the screen */
    } else {
        psp_opt_str(buf, txt, sizeof(buf));
        x = 296 - psp_opt_width(buf) / 2; /* same, but measuring the text (French, German, Japanese) */
    }
    if (x > 264) {
        x = 264;
    }
    gSPDisplayList(gDisplayListHead++, dl_ia_text_begin);
    psp_opt_text(x, 212, txt, 0);
    gSPDisplayList(gDisplayListHead++, dl_ia_text_end);
}

/* HOME with the TV active: the system HOME is blocked (the firmware dialog comes out deformed on the TV and leaving through it closes the console badly). So that it does not look
 * like the button is not responding: Lakitu's "the camera cannot move any further" buzz plays and for a few seconds a notice at the bottom center says the TV must be turned off to exit. */
extern int gfx_scegu_tv_is_on(void);
static const char *const sTrTvHome[LANG_COUNT] = TR("Turn TV Output Off to exit", "Désactivez la sortie TV pour quitter", "TV-Ausgabe ausschalten zum Beenden", "TVヲ オフニ シテクダサイ");
static unsigned int sHomePrev;
static unsigned long long sHomeUntil;

void psp_options_home_notice(void) {
    const unsigned int raw = gPspRawButtons;
    const unsigned int pressed = raw & ~sHomePrev;
    unsigned long long now = 0;

    sHomePrev = raw;
    if (!gfx_scegu_tv_is_on()) {
        sHomeUntil = 0;
        return;
    }
    if (sHomeUntil == 0 && !(pressed & PSP_CTRL_HOME)) {
        return; /* no active notice and no new HOME: there is no need to even read the clock */
    }
    sceRtcGetCurrentTick(&now);
    if (pressed & PSP_CTRL_HOME) {
        play_sound(SOUND_MENU_CAMERA_BUZZ, gDefaultSoundArgs);
        sHomeUntil = now + 3000000ULL; /* 3 s (the clock runs at 1 MHz) */
    }
    if (now >= sHomeUntil) {
        sHomeUntil = 0; /* already passed: the clock is no longer read until the next HOME */
    } else {
        create_dl_ortho_matrix();
        gDPSetScissor(gDisplayListHead++, G_SC_NON_INTERLACE, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
        gSPDisplayList(gDisplayListHead++, dl_ia_text_begin);
        psp_opt_text(160, 26, LT(sTrTvHome), 0);
        gSPDisplayList(gDisplayListHead++, dl_ia_text_end);
    }
}

static void psp_opt_close(void) {
    sOpen = 0;
    sCapture = -1;
    if (sSndDirty) {
        /* same as when choosing it in the file menu: it is stored in the save file (a single write, on close) */
        save_file_set_sound_mode(sSndMode);
        sSndDirty = 0;
    }
    psp_options_save_file(sCfgName);
    play_sound(SOUND_MENU_PAUSE_2, gDefaultSoundArgs);
}

/* On close, this very frame is drawn by the normal pause (darkened, coins, course options): if it drew nothing, one frame of the scene would be seen
 * undarkened (flicker). The buttons that closed the menu are removed so the pause does not read them (START/A would unpause it). */
static int psp_opt_closed_frame(struct Controller *c) {
    c->buttonPressed &= ~(START_BUTTON | A_BUTTON | B_BUTTON | Z_TRIG | R_TRIG);
    return 0;
}

static void psp_opt_enter_page(int page, int sel) {
    if (page == PG_VIDEO) {
        (void) psp_dve_probe(); /* loads the TV module (only once) to know whether the "TV Output" row can be used */
    }
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

    {
        /* a TV change that reverted by itself (the 480p test expired or was cancelled, or the output could not be routed): the row shows what really happens */
        const int rv = gfx_scegu_tv_take_revert();
        if (rv >= 0 && rv != sVal[OPT_TVOUT]) {
            sVal[OPT_TVOUT] = rv;
            psp_opt_apply();
            psp_options_save_file(NULL);
            play_sound(SOUND_MENU_CAMERA_BUZZ, gDefaultSoundArgs);
        }
    }

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
        sPrevRaw = gPspRawButtons; /* what is being pressed on opening (the R) does not count */
        sVal[OPT_SHOWFPS] = gRenderFPS ? 1 : 0;
        sSndMode = save_file_get_sound_mode();
        if (sSndMode > 2) {
            sSndMode = 0;
        }
        sSndDirty = 0;
        play_sound(SOUND_MENU_CAMERA_ZOOM_IN, gDefaultSoundArgs);
    }

    if (sPage == PG_TV && !sTvAcc) {
        /* the cable was removed with the TV off: leaves the TV page for Graphics */
        sPage = PG_VIDEO;
        sSel = psp_opt_row_index(sPgVideo, (int) sizeof(sPgVideo), ROW_TVPAGE);
        /* the cursor stays on the TV Settings row (greyed out) so "No cable" can be read */
        sScroll = (sSel > 3) ? sSel - 3 : 0;
    }

    raw = gPspRawButtons;
    pressed = raw & ~sPrevRaw;
    sPrevRaw = raw;
    rows = psp_opt_page_rows(&n);

    if (sJustOpened) {
        sJustOpened = 0;
    } else if (sWaitRelease) {
        /* the mapping just changed with buttons still held: nothing is done until they are all released */
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
            sCapture = -1; /* nobody pressed anything: cancelled */
            play_sound(SOUND_MENU_CAMERA_BUZZ, gDefaultSoundArgs);
        }
    } else if (gfx_scegu_tv_probation_left() > 0) {
        /* 480p test (the TV may not show it): A confirms, B cancels; with no answer in 10 s it goes back to the previous mode by itself. Nothing else does anything. */
        if (pressed & PSP_CTRL_CROSS) {
            gfx_scegu_tv_confirm();
            psp_options_save_file(NULL);
            play_sound(SOUND_MENU_CLICK_FILE_SELECT, gDefaultSoundArgs);
        } else if (pressed & (PSP_CTRL_SQUARE | PSP_CTRL_CIRCLE)) {
            gfx_scegu_tv_cancel();
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
            if (sPage == PG_TV) {
                psp_opt_enter_page(PG_VIDEO, psp_opt_row_index(sPgVideo, (int) sizeof(sPgVideo), ROW_TVPAGE)); /* from the TV page it goes back to Graphics, on the TV row */
            } else {
                psp_opt_enter_page(PG_MAIN, sPage - 1); /* the entry of the page being left */
            }
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
            /* no wrap-around: above the first row or below the last one does nothing. Greyed-out rows are skipped (except TV Settings); if all the ones left in that direction are greyed out
             * it does not move and Lakitu's buzz plays (once per press) */
            const int first = sSel + dy;
            int ns = first;
            while (ns >= 0 && ns < n && psp_opt_row_skip(rows[ns])) {
                ns += dy;
            }
            if (ns >= 0 && ns < n) {
                sSel = ns;
                if (sSel < sScroll) {
                    sScroll = sSel;
                } else if (sSel > sScroll + 3) {
                    sScroll = sSel - 3;
                }
                play_sound(SOUND_MENU_CHANGE_SELECT, gDefaultSoundArgs);
            } else if (first >= 0 && first < n && !sNavBuzzed) {
                sNavBuzzed = 1;
                play_sound(SOUND_MENU_CAMERA_BUZZ, gDefaultSoundArgs);
            }
        } else {
            sNavBuzzed = 0;
        }
        if (dx != 0) {
            psp_opt_change(rows[sSel], dx, 0);
        }
        if ((pressed & PSP_CTRL_LTRIGGER) && rows[sSel] == OPT_TVOUT && psp_opt_tv_ready() && sVal[OPT_TVOUT] > 0) {
            gfx_scegu_tv_reconnect(); /* routes the output again (for example after unplugging and plugging the cable back in) */
            play_sound(SOUND_MENU_CLICK_FILE_SELECT, gDefaultSoundArgs);
        }
        if (pressed & PSP_CTRL_CROSS) {
            const int row = rows[sSel];
            if (row == ROW_VIDEO) {
                psp_opt_enter_page(PG_VIDEO, 0);
            } else if (row == ROW_AUDIO) {
                psp_opt_enter_page(PG_AUDIO, 0);
            } else if (row == ROW_CONTROLS) {
                psp_opt_enter_page(PG_CTRL, 0);
            } else if (row == ROW_TVPAGE) {
                if (psp_opt_tv_access_now()) {
                    sTvAcc = 1;
                    psp_opt_enter_page(PG_TV, 0);
                } else {
                    play_sound(SOUND_MENU_CAMERA_BUZZ, gDefaultSoundArgs); /* no video cable (or no TV module): not entered */
                }
            } else if (row == ROW_AUDRESET) {
                psp_opt_reset_audio();
                play_sound(SOUND_MENU_CHANGE_SELECT, gDefaultSoundArgs);
            } else if (row == ROW_TVRESET) {
                if (psp_opt_tv_row_off(row)) {
                    play_sound(SOUND_MENU_CAMERA_BUZZ, gDefaultSoundArgs); /* with TV Output at Off it is useless */
                } else {
                    psp_opt_reset_tv();
                    play_sound(SOUND_MENU_CHANGE_SELECT, gDefaultSoundArgs);
                }
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

/* Sets "TV Output" to this value ONLY for this session (does not save the file): the game booted with the console already in its native TV mode. */
void psp_options_tv_set(int mode) {
    sVal[OPT_TVOUT] = mode;
    psp_opt_apply();
}

void psp_options_tv_off(void) {
    sVal[OPT_TVOUT] = 0;
    psp_opt_apply();
    psp_options_save_file(NULL);
}

#endif /* TARGET_PSP */
