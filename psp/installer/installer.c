/* First-run installer + loader of the port (used instead of psp/launcher/launcher.c in the "no PC needed" release).
 *
 * The release contains no Nintendo data: SM64.template.prx is the game module with every byte that comes from the ROM zeroed, and
 * manifest.bin says where each of those bytes lives in the user's own ROM. On the first start this program finds the ROM (data/ or the
 * game folder, any name, .z64/.v64/.n64), identifies it by SHA-1, rebuilds SM64.PRX byte for byte, checks its SHA-1 and starts it.
 * On later starts SM64.PRX is already there, so it behaves exactly like the plain launcher.
 * If there is no manifest.bin in the folder (a normal build) it also just starts SM64.PRX. */
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspctrl.h>
#include <psppower.h>
#include <psputility.h>
#include <pspdisplay.h>
#include <stdio.h>
#include <string.h>

#include "installer_text.h"
#include "smi_core.h"

PSP_MODULE_INFO("SM64INSTALL", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_MAIN_THREAD_STACK_SIZE_KB(32);
PSP_HEAP_SIZE_KB(32); /* only for stdio; the big buffers come from partition memory and are freed before the game starts */

#define MAX_CANDIDATES 24
#define PATH_MAX_LEN 300

/* ---- Texts --------------------------------------------------------------------------------------------------------------------- */

enum {
#define X(id, en, fr, de, es, it, ja) id,
    SMI_MESSAGES(X)
#undef X
    M_COUNT
};

/* The Japanese column is not compiled in (see installer_text.h): it comes from text_ja.inc as bitmaps. */
#define X(id, en, fr, de, es, it, ja) en,
static const char *const g_text_en[M_COUNT] = { SMI_MESSAGES(X) };
#undef X
#define X(id, en, fr, de, es, it, ja) fr,
static const char *const g_text_fr[M_COUNT] = { SMI_MESSAGES(X) };
#undef X
#define X(id, en, fr, de, es, it, ja) de,
static const char *const g_text_de[M_COUNT] = { SMI_MESSAGES(X) };
#undef X
#define X(id, en, fr, de, es, it, ja) es,
static const char *const g_text_es[M_COUNT] = { SMI_MESSAGES(X) };
#undef X
#define X(id, en, fr, de, es, it, ja) it,
static const char *const g_text_it[M_COUNT] = { SMI_MESSAGES(X) };
#undef X

#include "text_ja.inc"
typedef char ja_table_matches_messages[(JA_MESSAGE_COUNT == M_COUNT) ? 1 : -1]; /* run tools/psp/gen_installer_ja.py if this fails */

static const char *const *g_text = g_text_en;
static int g_ja; /* 1 = draw the Japanese bitmaps instead of the debug font */

#define SCREEN_W 480
#define TEXT_COLS 59 /* the 8x8 debug font gives 60 columns; keep one free */

static void select_language(void) {
    int lang = PSP_SYSTEMPARAM_LANGUAGE_ENGLISH;
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE, &lang);
    g_ja = 0;
    switch (lang) {
        case PSP_SYSTEMPARAM_LANGUAGE_JAPANESE:
            g_ja = 1;
            break;
        case PSP_SYSTEMPARAM_LANGUAGE_FRENCH:
            g_text = g_text_fr;
            break;
        case PSP_SYSTEMPARAM_LANGUAGE_GERMAN:
            g_text = g_text_de;
            break;
        case PSP_SYSTEMPARAM_LANGUAGE_SPANISH:
            g_text = g_text_es;
            break;
        case PSP_SYSTEMPARAM_LANGUAGE_ITALIAN:
            g_text = g_text_it;
            break;
        default:
            g_text = g_text_en;
            break;
    }
}

/* Direct frame buffer access for the Japanese bitmaps (the debug font cannot draw them). 'fb' describes the current frame buffer. */
typedef struct {
    unsigned char *base;
    int bufw, bpp, fmt;
} smi_fb;

static int fb_get(smi_fb *fb) {
    void *top = NULL;
    fb->bufw = 0;
    fb->fmt = 0;
    if (sceDisplayGetFrameBuf(&top, &fb->bufw, &fb->fmt, PSP_DISPLAY_SETBUF_NEXTFRAME) < 0 || top == NULL || fb->bufw <= 0) {
        return 0;
    }
    fb->bpp = fb->fmt == PSP_DISPLAY_PIXEL_FORMAT_8888 ? 4 : 2;
    fb->base = (unsigned char *) ((unsigned int) top | 0x40000000u); /* uncached view */
    return 1;
}

static void fb_put(const smi_fb *fb, int x, int y, int white) {
    unsigned char *px = fb->base + (y * fb->bufw + x) * fb->bpp;
    if (fb->bpp == 4) {
        *(unsigned int *) px = white ? 0xFFFFFFFFu : 0xFF000000u;
    } else {
        *(unsigned short *) px = white ? 0xFFFF : (fb->fmt == PSP_DISPLAY_PIXEL_FORMAT_565 ? 0x0000 : (fb->fmt == PSP_DISPLAY_PIXEL_FORMAT_5551 ? 0x8000 : 0xF000));
    }
}

/* Black out whole text rows (the debug font cannot erase what a bitmap drew). */
static void clear_rows(int row, int nrows) {
    smi_fb fb;
    int x, y;
    if (g_ja && fb_get(&fb)) {
        for (y = row * 8; y < (row + nrows) * 8 && y < 272; y++) {
            for (x = 0; x < SCREEN_W; x++) {
                fb_put(&fb, x, y, 0);
            }
        }
    }
    for (y = row; y < row + nrows; y++) {
        pspDebugScreenSetXY(0, y);
        pspDebugScreenPrintf("%*s", TEXT_COLS, "");
    }
    pspDebugScreenSetXY(0, row);
}

/* Draws a Japanese bitmap at a text position (column, row of the 8x8 grid), black behind it up to the right edge. */
static int blit_ja(int id, int col, int row) {
    smi_fb fb;
    int x0 = col * 8, y0 = row * 8, x, y, rb = (ja_index[id].w + 7) / 8;
    if (!fb_get(&fb)) {
        return 0;
    }
    for (y = 0; y < (int) ja_index[id].h && y0 + y < 272; y++) {
        for (x = x0; x < SCREEN_W; x++) {
            int sx = x - x0;
            fb_put(&fb, x, y0 + y, sx < (int) ja_index[id].w && ((ja_data[ja_index[id].off + y * rb + sx / 8] >> (7 - sx % 8)) & 1));
        }
    }
    return 1;
}

/* Prints text wrapped at word boundaries, keeping the rest of the last line clean. */
static void print_wrapped(const char *s, int nl) {
    int col = pspDebugScreenGetX();
    while (*s) {
        int len = 0;
        while (s[len] && s[len] != ' ') {
            len++;
        }
        if (col > 0 && col + len > TEXT_COLS) {
            pspDebugScreenPrintf("%*s\n", TEXT_COLS - col, "");
            col = 0;
        }
        pspDebugScreenPrintf("%.*s", len, s);
        col += len;
        s += len;
        if (*s == ' ') {
            s++;
            if (*s) {
                pspDebugScreenPrintf(" ");
                col++;
            }
        }
    }
    if (nl) {
        pspDebugScreenPrintf("%*s\n", TEXT_COLS - col > 0 ? TEXT_COLS - col : 0, "");
    }
}

/* Shows message 'id' at the cursor. nl = 1: the cursor goes to the start of the next free line; 0: it stays to the right of the text. */
static void put_msg(int id, int nl) {
    int col = pspDebugScreenGetX(), row = pspDebugScreenGetY();
    if (g_ja && blit_ja(id, col, row)) {
        if (nl) {
            pspDebugScreenSetXY(0, row + (ja_index[id].h + 7) / 8);
        } else {
            pspDebugScreenSetXY(col + (ja_index[id].w + 7) / 8, row);
        }
    } else {
        print_wrapped(g_text[id], nl);
    }
}

/* ---- Home button -------------------------------------------------------------------------------------------------------------- */

static int exit_cb(int arg1, int arg2, void *common) {
    (void) arg1;
    (void) arg2;
    (void) common;
    sceKernelExitGame();
    return 0;
}

static int cb_thread(SceSize args, void *argp) {
    SceUID cb = sceKernelCreateCallback("exit", exit_cb, NULL);
    (void) args;
    (void) argp;
    sceKernelRegisterExitCallback(cb);
    sceKernelSleepThreadCB();
    return 0;
}

static SceUID g_cb_thread = -1;

static void start_exit_callback(void) {
    g_cb_thread = sceKernelCreateThread("exit_thread", cb_thread, 0x11, 0x1000, THREAD_ATTR_USER, NULL);
    if (g_cb_thread >= 0) {
        sceKernelStartThread(g_cb_thread, 0, NULL);
    }
}

/* ---- Memory: partition blocks, so that nothing stays reserved when the game starts ---------------------------------------------- */

#define MAX_BLOCKS 48
static struct {
    void *ptr;
    SceUID id;
} g_blocks[MAX_BLOCKS];

static void *mem_alloc(size_t size) {
    int i;
    for (i = 0; i < MAX_BLOCKS; i++) {
        if (g_blocks[i].ptr == NULL) {
            SceUID id = sceKernelAllocPartitionMemory(PSP_MEMORY_PARTITION_USER, "smi", PSP_SMEM_Low, (SceSize) (size ? size : 1), NULL);
            if (id < 0) {
                return NULL;
            }
            g_blocks[i].id = id;
            g_blocks[i].ptr = sceKernelGetBlockHeadAddr(id);
            return g_blocks[i].ptr;
        }
    }
    return NULL;
}

static void mem_release(void *ptr) {
    int i;
    for (i = 0; i < MAX_BLOCKS; i++) {
        if (g_blocks[i].ptr == ptr && ptr != NULL) {
            sceKernelFreePartitionMemory(g_blocks[i].id);
            g_blocks[i].ptr = NULL;
            return;
        }
    }
}

/* ---- Screen --------------------------------------------------------------------------------------------------------------------- */

static void wait_button_then_exit(int seconds) {
    SceCtrlData pad;
    int i;
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);
    pspDebugScreenPrintf("\n");
    put_msg(M_PRESS, 1);
    for (i = 0; i < seconds * 20; i++) {
        sceKernelDelayThread(50 * 1000);
        sceCtrlPeekBufferPositive(&pad, 1);
        if (i > 10 && (pad.Buttons & 0xFFFF) != 0) {
            break;
        }
    }
    sceKernelExitGame();
}

static void die(int msg, int code) {
    pspDebugScreenPrintf("\n\n");
    put_msg(msg, 1);
    if (code != 0) {
        put_msg(M_CODE, 0);
        pspDebugScreenPrintf(" 0x%08X\n", (unsigned int) code);
    }
    wait_button_then_exit(120);
}

static int rom_name_msg(const char *l) {
    if (strcmp(l, "us") == 0) {
        return R_US;
    }
    if (strcmp(l, "eu") == 0) {
        return R_EU;
    }
    if (strcmp(l, "jp") == 0) {
        return R_JP;
    }
    return R_SH;
}

static int g_last_pct = -1, g_last_stage = -1, g_bar_row;
static const int g_stage_msg[4] = { M_STAGE_HASH, M_STAGE_COPY, M_STAGE_BUILD, M_STAGE_VERIFY };

static int on_progress(void *user, int stage, uint32_t done, uint32_t total) {
    int pct = total ? (int) ((uint64_t) done * 100 / total) : 100, i, fill;
    (void) user;
    if (stage == g_last_stage && pct == g_last_pct) {
        return 0;
    }
    pspDebugScreenSetXY(0, g_bar_row);
    if (stage != g_last_stage) {
        put_msg(g_stage_msg[stage], 1);
    } else {
        pspDebugScreenSetXY(0, g_bar_row + (g_ja ? (ja_index[g_stage_msg[stage]].h + 7) / 8 : 1));
    }
    g_last_stage = stage;
    g_last_pct = pct;
    pspDebugScreenPrintf("[");
    fill = pct * 40 / 100;
    for (i = 0; i < 40; i++) {
        pspDebugScreenPrintf("%c", i < fill ? '#' : '.');
    }
    pspDebugScreenPrintf("] %3d%%  ", pct);
    return 0;
}

/* ---- Files ---------------------------------------------------------------------------------------------------------------------- */

static int file_exists(const char *path) {
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}

typedef struct {
    char path[PATH_MAX_LEN];
} candidate;

static int scan_dir(const char *dir, candidate *list, int n) {
    SceUID d = sceIoDopen(dir);
    SceIoDirent ent;
    if (d < 0) {
        return n;
    }
    memset(&ent, 0, sizeof(ent));
    while (n < MAX_CANDIDATES && sceIoDread(d, &ent) > 0) {
        if (FIO_S_ISREG(ent.d_stat.st_mode) && ent.d_stat.st_size == (SceOff) SMI_ROM_SIZE &&
            snprintf(list[n].path, PATH_MAX_LEN, "%s/%s", dir, ent.d_name) < PATH_MAX_LEN) {
            n++;
        }
        memset(&ent, 0, sizeof(ent));
    }
    sceIoDclose(d);
    return n;
}

/* Starts SM64.PRX like psp/launcher/launcher.c does (arg0 is the EBOOT path, which the game uses to find its folder). */
static void launch(const char *prx, const char *arg0) {
    int status = 0, r;
    SceUID mod;
    if (g_cb_thread >= 0) {
        sceKernelTerminateDeleteThread(g_cb_thread); /* the game registers its own exit callback */
        g_cb_thread = -1;
    }
    scePowerSetClockFrequency(222, 222, 111);
    mod = sceKernelLoadModule(prx, 0, NULL);
    if (mod < 0) {
        die(E_LOAD, mod);
    }
    r = sceKernelStartModule(mod, strlen(arg0) + 1, (void *) arg0, &status, NULL);
    if (r < 0) {
        die(E_START, r);
    }
    sceKernelSleepThread();
}

static int status_msg(int rc) {
    switch (rc) {
        case SMI_E_IO:
            return E_IO;
        case SMI_E_MANIFEST:
            return E_MANIFEST;
        case SMI_E_MEMORY:
            return E_MEMORY;
        case SMI_E_TEMPLATE:
            return E_TEMPLATE;
        case SMI_E_DECODE:
            return E_DECODE;
        case SMI_E_VERIFY:
            return E_VERIFY;
        default:
            return E_UNKNOWN;
    }
}

int main(int argc, char **argv) {
    char arg0[PATH_MAX_LEN], dir[PATH_MAX_LEN], prx[PATH_MAX_LEN], tpl[PATH_MAX_LEN], man[PATH_MAX_LEN], stamp[PATH_MAX_LEN],
        datadir[PATH_MAX_LEN];
    char *slash;
    smi_env env;
    smi_manifest m;
    candidate *cands;
    const char *roms[SMI_MAX_ROMS] = { NULL, NULL };
    int i, n, rc;
    long prx_size;

    pspDebugScreenInit();
    start_exit_callback();
    select_language();
    if (argc < 1 || argv == NULL || argv[0] == NULL) {
        die(E_NO_PATH, -1);
    }
    strncpy(arg0, argv[0], sizeof(arg0) - 1);
    arg0[sizeof(arg0) - 1] = 0;
    strcpy(dir, arg0);
    slash = strrchr(dir, '/');
    if (slash == NULL) {
        slash = strrchr(dir, '\\');
    }
    if (slash == NULL) {
        die(E_BAD_PATH, -2);
    }
    *slash = 0;
    sprintf(prx, "%s/SM64.PRX", dir);
    sprintf(tpl, "%s/SM64.template.prx", dir);
    sprintf(man, "%s/manifest.bin", dir);
    sprintf(stamp, "%s/SM64.PRX.stamp", dir);
    sprintf(datadir, "%s/data", dir);

    /* A normal build (no manifest): plain launcher. */
    if (!file_exists(man)) {
        launch(prx, arg0);
        return 0;
    }

    env.alloc = mem_alloc;
    env.release = mem_release;
    env.progress = on_progress;
    env.user = NULL;
    rc = smi_load_manifest(&env, man, &m);
    if (rc != SMI_OK) {
        die(status_msg(rc), rc);
    }

    /* Already installed with this exact package: just start the game. */
    prx_size = smi_file_size(prx);
    if (prx_size == (long) m.tsize && smi_stamp_matches(stamp, &m)) {
        smi_free_manifest(&env, &m);
        launch(prx, arg0);
        return 0;
    }

    scePowerSetClockFrequency(333, 333, 166);
    pspDebugScreenPrintf("Super Mario 64 (PSP)\n\n");
    put_msg(M_FIRST, 1);
    g_bar_row = pspDebugScreenGetY() + 1;

    cands = (candidate *) mem_alloc(sizeof(candidate) * MAX_CANDIDATES);
    if (cands == NULL) {
        die(E_MEMORY, SMI_E_MEMORY);
    }
    n = scan_dir(datadir, cands, 0);
    n = scan_dir(dir, cands, n);

    for (i = 0; i < n; i++) {
        int idx = smi_identify_rom(&env, &m, cands[i].path);
        if (idx >= 0 && roms[idx] == NULL) {
            int all = 1, j;
            roms[idx] = cands[i].path;
            for (j = 0; j < (int) m.nroms; j++) {
                all &= roms[j] != NULL;
            }
            if (all) {
                break;
            }
        }
    }
    for (i = 0; i < (int) m.nroms; i++) {
        if (roms[i] == NULL) {
            clear_rows(g_bar_row, 8); /* wipe the progress area */
            put_msg(M_MISSING, 1);
            pspDebugScreenPrintf("  ");
            put_msg(rom_name_msg(m.lang[i]), 1);
            if (n > 0) {
                put_msg(M_NONE_MATCH, 1);
            }
            pspDebugScreenPrintf("\n");
            put_msg(M_COPY_TO, 1);
            pspDebugScreenPrintf("  %s\n\n", datadir);
            put_msg(M_MUST_BE, 1);
            wait_button_then_exit(120);
        }
    }

    sceIoRemove(stamp); /* until everything is verified the module counts as not installed */
    rc = smi_install(&env, &m, tpl, roms, prx);
    if (rc != SMI_OK) {
        sceIoRemove(prx);
        pspDebugScreenSetXY(0, g_bar_row + 6);
        die(status_msg(rc), rc);
    }
    rc = smi_stamp_write(stamp, &m);
    if (rc != SMI_OK) {
        die(status_msg(rc), rc);
    }
    pspDebugScreenSetXY(0, g_bar_row + 6);
    put_msg(M_DONE, 1);
    sceKernelDelayThread(800 * 1000);
    mem_release(cands);
    smi_free_manifest(&env, &m);
    launch(prx, arg0);
    return 0;
}
