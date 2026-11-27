/*
 * sm64Crash ("crash report module"): small kernel module that catches a CPU exception of the game (a crash: bad address, unaligned access,
 * break, ...) and writes "crash.txt" in the memory stick root (ms0:/, next to freeze.txt) before going back to the system menu. It is the counterpart of the
 * freeze report (psp_watchdog.c): a freeze leaves the game running and the watchdog thread describes it, a crash stops the faulting thread
 * and only an exception handler can see where.
 *
 * What it writes: the exception (cause, address), the faulting thread, all the registers, and every value on the faulting thread's stack
 * that points into the game's code (a rough call stack). The code addresses are also given as offsets from the start of the game's code,
 * so they can be looked up in the ELF of the same build (psp-addr2line -e sm64.<ver>.f3dex2e 0x<offset + 0x8804000> ...).
 *
 * The module is embedded in SM64.PRX and the game writes and loads it by itself (psp_watchdog.c); without it (PPSSPP, or if it does not
 * load) nothing changes. It uses pspsdk's pspDebugInstallErrorHandler (the default exception handler: it is only called for exceptions
 * nobody else handles).
 */
#include <pspsdk.h>
#include <pspkernel.h>
#include <pspdebug.h>
#include <psploadcore.h>
#include <pspthreadman.h>

PSP_MODULE_INFO("sm64Crash_Module", 0x1006, 1, 0);

/* Pointers into the game that describe what it was doing (given by the game; any of them may be NULL). */
typedef struct {
    const char *const volatile *phase; /* gPspWdPhase: part of the frame the main thread was in */
    const short *level;                /* gCurrLevelNum */
    const short *area;                 /* gCurrAreaIndex */
    const unsigned int *marioAction;   /* &gMarioStates[0].action */
    const char *build;                 /* build date */
} CrashCtx;

static char sPath[160];
static CrashCtx sCtx;
static unsigned int sTextAddr, sTextSize;
static char sBuf[4096];
static int sLen;
static volatile int sInside;

static void put_str(const char *s) {
    while (s != 0 && *s != '\0' && sLen < (int) sizeof(sBuf) - 1) {
        sBuf[sLen++] = *s++;
    }
}

static void put_hex(unsigned int v) {
    static const char digits[] = "0123456789ABCDEF";
    int i;
    for (i = 28; i >= 0; i -= 4) {
        if (sLen < (int) sizeof(sBuf) - 1) {
            sBuf[sLen++] = digits[(v >> i) & 0xF];
        }
    }
}

static void put_dec(int v) {
    char tmp[12];
    int n = 0;
    unsigned int u = (v < 0) ? (unsigned int) -v : (unsigned int) v;
    if (v < 0) {
        put_str("-");
    }
    do {
        tmp[n++] = (char) ('0' + u % 10);
        u /= 10;
    } while (u != 0 && n < 11);
    while (n > 0 && sLen < (int) sizeof(sBuf) - 1) {
        sBuf[sLen++] = tmp[--n];
    }
}

static int in_game_code(unsigned int a) {
    return sTextSize != 0 && a >= sTextAddr && a < sTextAddr + sTextSize && (a & 3) == 0;
}

/* an address, followed by its offset in the game's code when it is in it */
static void put_addr(unsigned int a) {
    put_str("0x");
    put_hex(a);
    if (in_game_code(a)) {
        put_str(" (game+0x");
        put_hex(a - sTextAddr);
        put_str(")");
    }
}

/* a pointer the game gave: only read when it is in user RAM */
static int user_ptr(const void *p, unsigned int size) {
    const unsigned int a = (unsigned int) p & 0x3FFFFFFF;
    return p != 0 && a >= 0x08800000 && a + size <= 0x0C000000;
}

static const char *const sCodes[32] = {
    "interrupt", "TLB modification", "TLB load", "TLB store", "address error (load / fetch: bad or unaligned address)",
    "address error (store: bad or unaligned address)", "bus error (instruction fetch)", "bus error (data)", "syscall", "break",
    "reserved instruction", "coprocessor unusable", "overflow", "trap", "?", "floating point",
};

static void crash_handler(PspDebugRegBlock *r) {
    static const char *const names[32] = {
        "zr", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
        "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra",
    };
    const unsigned int code = (r->cause >> 2) & 31;
    SceKernelThreadInfo info;
    SceUID fd;
    int i, found;

    if (sInside) { /* a second exception while writing the first: just leave */
        sceKernelExitGame();
        return;
    }
    sInside = 1;
    sLen = 0;
    put_str("SM64 PSP - crash report\r\n");
    if (user_ptr(sCtx.build, 4)) {
        put_str("built ");
        put_str(sCtx.build);
        put_str("\r\n");
    }
    put_str("exception: ");
    put_str((code < 16 && sCodes[code] != 0) ? sCodes[code] : "?");
    put_str(" (cause 0x");
    put_hex(r->cause);
    put_str(")\r\n");
    put_str("at (epc) ");
    put_addr(r->epc);
    put_str("\r\nbad address 0x");
    put_hex(r->badvaddr);
    put_str("\r\nreturn address (ra) ");
    put_addr(r->r[31]);
    put_str("\r\n");

    info.size = sizeof(info);
    if (sceKernelReferThreadStatus(sceKernelGetThreadId(), &info) >= 0) {
        put_str("thread: ");
        put_str(info.name);
        put_str("\r\n");
    }
    if (user_ptr((const void *) sCtx.phase, 4) && user_ptr(*sCtx.phase, 1)) {
        put_str("main thread was in: ");
        put_str(*sCtx.phase);
        put_str("\r\n");
    }
    if (user_ptr(sCtx.level, 2) && user_ptr(sCtx.area, 2)) {
        put_str("level ");
        put_dec(*sCtx.level);
        put_str(", area ");
        put_dec(*sCtx.area);
        put_str("\r\n");
    }
    if (user_ptr(sCtx.marioAction, 4)) {
        put_str("Mario: action 0x");
        put_hex(*sCtx.marioAction);
        put_str("\r\n");
    }
    put_str("game code at 0x");
    put_hex(sTextAddr);
    put_str(", size 0x");
    put_hex(sTextSize);
    put_str("\r\n\r\nregisters:\r\n");
    for (i = 0; i < 32; i++) {
        put_str(names[i]);
        put_str(" ");
        put_hex(r->r[i]);
        put_str((i % 4 == 3) ? "\r\n" : "   ");
    }
    put_str("hi ");
    put_hex(r->hi);
    put_str("   lo ");
    put_hex(r->lo);
    put_str("   status ");
    put_hex(r->status);
    put_str("   fsr ");
    put_hex(r->fsr);
    put_str("\r\n\r\ngame code addresses on the stack (most recent first):\r\n");
    found = 0;
    if (user_ptr((const void *) r->r[29], 4)) {
        const unsigned int *sp = (const unsigned int *) (r->r[29] & ~3u);
        for (i = 0; i < 4096 && found < 48 && user_ptr(sp + i, 4); i++) {
            if (in_game_code(sp[i])) {
                put_str("  sp+0x");
                put_hex((unsigned int) i * 4);
                put_str("  ");
                put_addr(sp[i]);
                put_str("\r\n");
                found++;
            }
        }
    }
    if (found == 0) {
        put_str("  (none)\r\n");
    }

    fd = sceIoOpen(sPath, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, sBuf, sLen);
        sceIoClose(fd);
    }
    sceKernelExitGame(); /* back to the system menu (the game cannot go on) */
}

/* Called by the game once: where to write the report and what to read from it. Returns < 0 on error. */
int pspCrashInstall(const char *path, const CrashCtx *ctx) {
    const int k1 = pspSdkSetK1(0);
    SceModule *mod;
    int i, res;

    for (i = 0; i < (int) sizeof(sPath) - 1 && path != 0 && path[i] != '\0'; i++) {
        sPath[i] = path[i];
    }
    sPath[i] = '\0';
    if (ctx != 0) {
        sCtx = *ctx;
    }
    mod = sceKernelFindModuleByName("SM64 for PSP");
    if (mod != 0) {
        sTextAddr = mod->text_addr;
        sTextSize = mod->text_size;
    }
    res = pspDebugInstallErrorHandler(crash_handler);
    pspSdkSetK1(k1);
    return res;
}

/* pspDebugInstallErrorHandler comes with pspsdk's own on-screen report (used when no handler is given), which needs vsnprintf; this module has no C
 * library and never uses that report, so this stands in for it. */
#include <stdarg.h>
int vsnprintf(char *buf, unsigned int size, const char *fmt, va_list ap) {
    (void) fmt;
    (void) ap;
    if (buf != 0 && size > 0) {
        buf[0] = '\0';
    }
    return 0;
}

int module_start(SceSize args, void *argp) {
    return 0;
}

int module_stop(SceSize args, void *argp) {
    return 0;
}
