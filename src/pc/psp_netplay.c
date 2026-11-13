/* Ad-hoc WLAN multiplayer, phase 1 (see psp_netplay.h for the overview).
 *
 * A worker thread (low priority, wakes up every 20 ms) owns the whole network stack. The game thread only sends requests (host / join / leave)
 * and reads the state, and once per frame it swaps the save file when told to (psp_netplay_frame). Nothing here runs unless the player picks
 * Host or Join in the options menu.
 *
 * Wire format: every packet starts with a 12 byte header (magic "SM64", type, two small arguments, a 32 bit session id). One room = one
 * session id, chosen by the host and also encoded in the name of its ad-hoc group ("SM" + 6 hex digits), so the client can find it with a scan.
 *   ADV      host -> broadcast, every 500 ms while it has no client
 *   JOIN     client -> host, every 300 ms until WELCOME arrives
 *   WELCOME  host -> client (a = host's current save file, 1..4)
 *   SAVE     host -> client, 2 chunks of 256 bytes (a = chunk, b = chunk count), repeated until SAVEACK
 *   SAVEACK  client -> host: it has the whole save file
 *   PING / PONG  both ways every 500 ms, 4 byte timestamp (round trip time and liveness)
 *   PROGRESS either side -> the other: the sender's current save file (stars, keys, doors...), repeated every 250 ms until PROGACK. The receiver
 *            MERGES it into its own current file (union of stars / flags, best coin scores) so both consoles end up with the same progress; the host
 *            writes it to the Memory Stick, the client only to its in-memory copy.
 *   PROGACK  receiver -> sender (a = the PROGRESS sequence number)
 *   STATE    either side -> the other, ~30 times a second, unreliable: the sender's pose (level, area, position, angles, animation), which the
 *            receiver shows as a second Mario / Luigi (game/psp_netplay_game.c). Old or repeated ones are dropped by their sequence number.
 *   LEAVE    either side
 */
#include "psp_netplay.h"

#if defined(TARGET_PSP) && PSP_NETPLAY

#include <stdio.h>
#include <string.h>
#include <pspkernel.h>
#include <pspnet.h>
#include <pspnet_adhoc.h>
#include <pspnet_adhocctl.h>
#include <pspwlan.h>
#include <psputility.h>

extern int osEepromLongRead(void *mq, unsigned char address, unsigned char *buffer, int nbytes); /* ultra_reimplementation.c */
extern short gCurrSaveFileNum;                                                                  /* game/save_file.c */
extern void save_file_load_all(void);
extern void psp_netplay_adopt_host_save(int hostFile); /* game/save_file.c */
extern int psp_netplay_merge_progress(const void *data, int size); /* game/save_file.c */
extern void psp_netplay_refresh_hud(void);                         /* game/save_file.c */

#define NP_PORT 0x5C64
#define NP_SAVE_BYTES 512
#define NP_CHUNK 256
#define NP_CHUNKS (NP_SAVE_BYTES / NP_CHUNK)
#define NP_THREAD_PRIO 0x1D /* above the game thread (0x20) and the audio threads (0x1E/0x20): at 0x38 it only ran when the game waited for the vblank, so pings answered late (RTT of seconds); at 60 fps the game leaves even less idle time, so the thread must be able to preempt it at any moment */
#define NP_ADV_US 500000U
#define NP_JOIN_US 300000U
#define NP_PING_US 500000U
#define NP_SCAN_EVERY_US 2000000U
#define NP_SCAN_TIMEOUT_US 30000000U
#define NP_NETWORK_TIMEOUT_US 15000000U
#define NP_PEER_TIMEOUT_US 15000000U /* silence before the other console is given up on. Was 5 s: radio stalls of 2-9 s were seen and each one dropped the link and restarted the world sync. sm64coopdx waits 15 s (server) / 22 s (client) and sends a keep-alive every 5 s */
#define NP_RETRIES_MAX 6
#define NP_STABLE_US 20000000U
#define NP_RESEND_SAVE_US 250000U
#define NP_PROG_MAX 96
#define NP_PROG_RESEND_US 250000U
#define NP_LOG "ms0:/PSP/SAVEDATA/SM64PORT/sm64_net.txt"

enum { PK_ADV = 1, PK_JOIN, PK_WELCOME, PK_SAVE, PK_SAVEACK, PK_PING, PK_PONG, PK_LEAVE, PK_PROGRESS, PK_PROGACK, PK_STATE, PK_NAME, PK_EVENT, PK_EVACK, PK_OBJ, PK_FLD };
enum { REQ_NONE, REQ_HOST, REQ_JOIN, REQ_LEAVE };

typedef struct {
    uint8_t magic[4];
    uint8_t type, a, b, pad;
    uint32_t session;
} NpHeader; /* 12 bytes, followed by the payload */

static volatile int sState = PSP_NP_OFF;
static char sStatus[64] = "";
static volatile int sRtt = -1;
/* Link health (see np_pace): the radio queues what it cannot carry and the delay grows into seconds, so the streams are paced by a byte budget and stopped
 * (drained) while the delay is high. */
#define NP_OBJ_US 40000U  /* base interval of the enemy stream */
#define NP_FLD_US 60000U  /* base interval of the platform stream */
#define NP_BUDGET_BPS 14000 /* bytes per second the enemy + platform streams may use when the link is fine (halved per congestion level) */
static int sBudget; /* bytes available now */
static uint32_t sTBudget;
static int sDrain; /* 1: the delay is very high, only the small packets go until it comes down */
static uint32_t sTxFail, sTxBytes, sRxPkts, sGapMax, sTLastStep, sTStat;
static uint32_t sTPressure; /* a send failed at this time: the link counts as congested for a second */
static volatile int sRttAvg = -1; /* smoothed: decides how much is sent (a slow link must not be fed more than it carries: the radio queues it and the delay grows) */
static volatile int sReq = REQ_NONE;
static volatile int sQuit;
static SceUID sThread = -1;

/* what has been opened (to close it in reverse order) */
static int sModCommon, sModAdhoc, sNet, sAdhoc, sCtl, sHandler = -1, sSocket = -1;

static volatile unsigned sEvents;
static volatile int sEventError;
static volatile int sEventSeen = -1;
static uint8_t sMac[6];
static int sHost;
static uint32_t sSession;
static SceNetAdhocctlScanInfo sScan[16];
static SceNetAdhocctlScanInfo sTarget;
static int sHaveTarget;
static int sScanPending;

static uint8_t sPeerMac[6];
static int sPeerKnown; /* client: the host's MAC (learned from its ADV); host: a client has joined */
static int sWelcomed;  /* client: WELCOME received */
static uint32_t sTStart, sTPing, sTSent, sTScan, sTScanStart, sTRx;

/* the host's save file, captured when a client joins */
static uint8_t sHostImage[NP_SAVE_BYTES];
static int sHostFile = 1;
static int sAcked; /* host: the client confirmed the whole image */

/* the copy the client plays with; sSaveReady means "complete, waiting for the game thread to swap it in" */
static uint8_t sClientImage[NP_SAVE_BYTES];
static uint8_t sChunkMask;
static volatile int sSaveReady;
static volatile int sOverride;
static int sClientHostFile = 1;

/* progress exchange (see PROGRESS above); the buffers are shared by the game thread and the worker, copies are made with interrupts off */
static uint8_t sProgOut[NP_PROG_MAX];
static int sProgOutLen;
static uint8_t sProgOutSeq;
static volatile int sProgOutPending;
static uint32_t sTProgSent;
static uint8_t sProgIn[NP_PROG_MAX];
static int sProgInLen;
static volatile int sProgInPending;
static int sProgInSeq = -1;
static uint32_t sTRecovered; /* when the last automatic restart was issued (late error events of the teardown are not new errors) */
static int sIgnoredErr; /* radio error events ignored in a row while the link kept working (cleared when the link is stable, see sRetries) */
static int sRetries; /* automatic restarts in a row (radio error, lost link); cleared once the link has been stable for a while */
static uint32_t sTConnected; /* when the link last became CONNECTED */
static uint32_t sTSwitch;
static int sWlanOffCount;

/* pose exchange (see STATE above) */
static PspNpPlayer sStateOut;
static volatile int sStateOutNew;
static uint8_t sStateOutSeq;
static uint32_t sTStateSent;
static PspNpPlayer sStateIn;
static volatile int sStateInHave;
static int sStateInSeq;
static uint32_t sTStateRx;
static int sGameActive; /* the game side currently shows the other player */

/* world events (EVENT / EVACK: one in flight at a time, repeated until acknowledged; the receiver applies each one once, in order) */
#define NP_EV_OUT 256
#define NP_EV_IN 32
#define NP_EV_RESEND_US 60000U
static PspNpWorldEvent sEvOut[NP_EV_OUT];
static volatile int sEvOutHead, sEvOutTail; /* head: next free, tail: the one in flight */
static uint8_t sEvSeq;                       /* sequence number of the one in flight */
static uint32_t sTEvSent;
static PspNpWorldEvent sEvIn[NP_EV_IN];
static volatile int sEvInHead, sEvInTail;
static uint8_t sEvInNext;                    /* sequence number expected next */

/* enemy states (OBJ: unreliable like STATE; only the newest batch matters) */
static uint8_t sObjOut[PSP_NP_OBJ_BYTES];
static volatile int sObjOutLen;
static uint32_t sTObjSent;
static uint8_t sObjIn[PSP_NP_OBJ_BYTES];
static volatile int sObjInLen;
static uint8_t sObjInSeq;
static uint8_t sObjOutSeq;
static int sObjInHave;

/* platform states (FLD: the key values of the moving platforms, unreliable, only the newest batch matters; both consoles can send) */
static uint8_t sFldOut[PSP_NP_FLD_BYTES];
static volatile int sFldOutLen;
static uint32_t sTFldSent;
static uint8_t sFldIn[PSP_NP_FLD_BYTES];
static volatile int sFldInLen;
static uint8_t sFldInSeq, sFldOutSeq;
static int sFldInHave;

/* an event is repeated until acknowledged, but not faster than the link answers: on a slow link the copies would only add to the queue */
static uint32_t np_ev_resend_us(void) {
    const uint32_t wait = (sRttAvg > 0) ? (uint32_t) sRttAvg * 1500U : 0U;
    return (wait < NP_EV_RESEND_US) ? NP_EV_RESEND_US : ((wait > 1500000U) ? 1500000U : wait);
}

static void np_events_reset(void) {
    const int lock = sceKernelCpuSuspendIntr();
    sEvOutHead = sEvOutTail = 0;
    sEvInHead = sEvInTail = 0;
    sEvSeq = 0;
    sEvInNext = 0;
    sTEvSent = 0;
    sObjOutLen = sObjInLen = 0;
    sObjInHave = 0;
    sFldOutLen = sFldInLen = 0;
    sFldInHave = 0;
    sceKernelCpuResumeIntr(lock);
}

/* names (NAME: sent with every ping, so it is known before the link is up): this console's nickname, from the system settings, and the other's */
static char sMyName[PSP_NP_NAME_BYTES];
static char sPeerName[PSP_NP_NAME_BYTES];
static volatile int sPeerNameNew;

static uint8_t sRx[PSP_NP_FLD_BYTES + 64]; /* the biggest packet is a batch of enemy states */
static uint8_t sTx[PSP_NP_FLD_BYTES + 64];

/* ------------------------------------------------------------------ helpers */

/* Short diagnostic log (sm64_net.txt in the save folder): every status change with its time, so a failure can be understood afterwards. */
static void np_log(const char *text) {
    char line[112];
    SceUID fd;
    const int n = snprintf(line, sizeof(line), "%8u ms  %s\n", (unsigned int) (sceKernelGetSystemTimeLow() / 1000U), text);
    if (n <= 0) {
        return;
    }
    if (n >= (int) sizeof(line)) {
        line[sizeof(line) - 2] = '\n'; /* (a long text is cut, but the line still ends: the next one must not join it) */
    }
    fd = sceIoOpen(NP_LOG, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, line, (n < (int) sizeof(line)) ? n : (int) sizeof(line) - 1);
        sceIoClose(fd);
    }
}

static void np_status(int state, const char *text) {
    sState = state;
    if (state == PSP_NP_CONNECTED) {
        sTConnected = sceKernelGetSystemTimeLow();
    }
    snprintf(sStatus, sizeof(sStatus), "%s", text);
    if (text[0]) {
        np_log(text);
    }
}

/* Handler of the ad-hoc control library: it runs in the library's own thread, so it only records what happened. */
static void np_control_event(int flag, int error, void *arg) {
    (void) arg;
    if (flag == 0) {
        sEventError = error;
    }
    sEventSeen = flag; /* (diagnostic) */
    if (flag >= 0 && flag < 8) {
        sEvents |= 1U << flag;
    }
}

static unsigned np_take_events(void) {
    const int lock = sceKernelCpuSuspendIntr();
    const unsigned e = sEvents;
    sEvents = 0;
    sceKernelCpuResumeIntr(lock);
    return e;
}

static uint32_t np_nonce(void) {
    uint32_t v = sceKernelGetSystemTimeLow() * 2654435761U;
    int i;
    for (i = 0; i < 6; i++) {
        v = (v ^ sMac[i]) * 16777619U;
    }
    return v ? v : 1;
}

static int np_send(const uint8_t *mac, int type, int a, int b, const void *payload, int len) {
    NpHeader *h = (NpHeader *) sTx;
    uint8_t dest[6];
    if (sSocket < 0 || len < 0 || len > NP_CHUNK) {
        return -1;
    }
    memcpy(h->magic, "SM64", 4);
    h->type = (uint8_t) type;
    h->a = (uint8_t) a;
    h->b = (uint8_t) b;
    h->pad = 0;
    h->session = sSession;
    if (len) {
        memcpy(sTx + sizeof(NpHeader), payload, (size_t) len);
    }
    memcpy(dest, mac, 6);
    {
        const int r = sceNetAdhocPdpSend(sSocket, dest, NP_PORT, sTx, (unsigned int) (sizeof(NpHeader) + (unsigned) len), 0, 1);
        if (r < 0) {
            sTxFail++;
            sTPressure = sceKernelGetSystemTimeLow() | 1U;
        } else {
            sTxBytes += (uint32_t) (sizeof(NpHeader) + (unsigned) len);
        }
        return r;
    }
}

/* Closes everything that was opened, in reverse order. Safe to call when nothing is open. */
static void np_cleanup(void) {
    if (sSocket >= 0) {
        sceNetAdhocPdpDelete(sSocket, 0);
        sSocket = -1;
    }
    if (sHandler >= 0) {
        sceNetAdhocctlDelHandler(sHandler);
        sHandler = -1;
    }
    if (sCtl) {
        sceNetAdhocctlDisconnect();
        sceNetAdhocctlTerm();
        sCtl = 0;
    }
    if (sAdhoc) {
        sceNetAdhocTerm();
        sAdhoc = 0;
    }
    if (sNet) {
        sceNetTerm();
        sNet = 0;
    }
    if (sModAdhoc) {
        sceUtilityUnloadNetModule(PSP_NET_MODULE_ADHOC);
        sModAdhoc = 0;
    }
    if (sModCommon) {
        sceUtilityUnloadNetModule(PSP_NET_MODULE_COMMON);
        sModCommon = 0;
    }
    sPeerKnown = sWelcomed = sHaveTarget = sScanPending = 0;
    sAcked = 0;
    sChunkMask = 0;
    sRtt = -1;
    sDrain = 0;
    sTPressure = 0;
    sRttAvg = -1;
    sProgOutPending = 0;
    sProgInPending = 0;
    sProgInSeq = -1;
    sStateInHave = 0;
    sStateOutNew = 0;
    np_events_reset();
}

static void np_fail(const char *what, int code) {
    char text[64];
    if (code != 0) {
        snprintf(text, sizeof(text), "%s (0x%08X)", what, (unsigned int) code);
    } else {
        snprintf(text, sizeof(text), "%s", what);
    }
    np_cleanup();
    np_status(PSP_NP_ERROR, text);
}

/* 0x80110802 / 0x80111102: the module is already loaded (by the system or by someone else): it is used but not unloaded. */
static int np_load_module(int module, int *owned) {
    const int r = sceUtilityLoadNetModule(module);
    if (r == 0) {
        *owned = 1;
        return 0;
    }
    if ((unsigned int) r == 0x80110802U || (unsigned int) r == 0x80111102U) {
        return 0;
    }
    return r;
}

static int np_open(void) {
    struct productStruct product;
    int r;

    /* The switch is read a few times: a single reading can be wrong right after the radio was used or closed. A negative value is an error of the
     * query itself, not "off". */
    for (r = 0; r < 6 && sceWlanGetSwitchState() == 0; r++) {
        sceKernelDelayThread(150000);
    }
    if (r >= 6) {
        np_fail("Turn the WLAN switch on", 0);
        return 0;
    }
    np_status(PSP_NP_STARTING, "Loading network modules...");
    if ((r = np_load_module(PSP_NET_MODULE_COMMON, &sModCommon)) < 0) { np_fail("Net module", r); return 0; }
    if ((r = np_load_module(PSP_NET_MODULE_ADHOC, &sModAdhoc)) < 0) { np_fail("Ad-hoc module", r); return 0; }
    if ((r = sceNetInit(0x20000, 0x20, 0x1000, 0x20, 0x1000)) < 0) { np_fail("Net init", r); return 0; }
    sNet = 1;
    if ((r = sceNetAdhocInit()) < 0) { np_fail("Ad-hoc init", r); return 0; }
    sAdhoc = 1;
    memset(&product, 0, sizeof(product));
    memcpy(product.product, "SM64PSP01", 9);
    if ((r = sceNetAdhocctlInit(0x2000, 0x30, &product)) < 0) { np_fail("Control init", r); return 0; }
    sCtl = 1;
    if ((r = sceNetAdhocctlAddHandler(np_control_event, NULL)) < 0) { np_fail("Control handler", r); return 0; }
    sHandler = r;
    if ((r = sceWlanGetEtherAddr(sMac)) < 0) { np_fail("WLAN address", r); return 0; }
    sEvents = 0;
    sEventError = 0;
    sTStart = sceKernelGetSystemTimeLow();
    return 1;
}

static void np_start_host(void) {
    char group[9];
    int r;
    np_cleanup();
    sHost = 1;
    if (!np_open()) {
        return;
    }
    sSession = np_nonce() & 0xFFFFFFU;
    if (!sSession) {
        sSession = 1;
    }
    snprintf(group, sizeof(group), "SM%06X", (unsigned int) sSession);
    if ((r = sceNetAdhocctlCreate(group)) < 0) {
        np_fail("Create room", r);
        return;
    }
    np_status(PSP_NP_STARTING, "Creating the room...");
}

static void np_start_join(void) {
    int r;
    np_cleanup();
    sHost = 0;
    if (!np_open()) {
        return;
    }
    sSession = 0;
    if ((r = sceNetAdhocctlScan()) < 0) {
        np_fail("Scan", r);
        return;
    }
    sScanPending = 1;
    sTScan = sTScanStart = sceKernelGetSystemTimeLow();
    np_status(PSP_NP_SEARCHING, "Looking for a room...");
}

/* Picks the first scan result that looks like one of our rooms: name "SM" + 6 hex digits, on a normal channel. */
static int np_pick_room(void) {
    int length = (int) sizeof(sScan);
    int r, count, i, k;
    r = sceNetAdhocctlGetScanInfo(&length, sScan);
    if (r < 0 || length < 0 || length > (int) sizeof(sScan)) {
        return 0;
    }
    count = length / (int) sizeof(sScan[0]);
    for (i = 0; i < count; i++) {
        const SceNetAdhocctlScanInfo *s = &sScan[i];
        uint32_t session = 0;
        int ok = (s->name[0] == 'S' && s->name[1] == 'M');
        for (k = 2; ok && k < 8; k++) {
            int d = s->name[k];
            if (d >= '0' && d <= '9') d -= '0';
            else if (d >= 'A' && d <= 'F') d -= 'A' - 10;
            else ok = 0;
            session = (session << 4) | (uint32_t) (d & 15);
        }
        if (ok && session != 0 && (s->channel == 1 || s->channel == 6 || s->channel == 11)) {
            memset(&sTarget, 0, sizeof(sTarget));
            sTarget.channel = s->channel;
            memcpy(sTarget.name, s->name, 8);
            memcpy(sTarget.bssid, s->bssid, 6);
            sSession = session;
            sHaveTarget = 1;
            return 1;
        }
    }
    return 0;
}

/* The host captures its own save file (the one on the Memory Stick) to send it. */
static void np_capture_host_save(void) {
    memset(sHostImage, 0, sizeof(sHostImage));
    if (osEepromLongRead(NULL, 0, sHostImage, NP_SAVE_BYTES) != 0) {
        memset(sHostImage, 0, sizeof(sHostImage)); /* no save yet: the client starts with an empty one (the game wipes invalid files) */
    }
    sHostFile = (gCurrSaveFileNum >= 1 && gCurrSaveFileNum <= 4) ? gCurrSaveFileNum : 1;
}

static void np_handle_packet(const uint8_t *mac, const uint8_t *data, int len, uint32_t now) {
    const NpHeader *h = (const NpHeader *) data;
    const uint8_t *payload = data + sizeof(NpHeader);
    const int plen = len - (int) sizeof(NpHeader);

    if (len < (int) sizeof(NpHeader) || memcmp(h->magic, "SM64", 4) != 0 || h->session != sSession) {
        return;
    }
    if (sPeerKnown && memcmp(mac, sPeerMac, 6) != 0 && h->type != PK_JOIN) {
        return; /* someone else */
    }
    if (sPeerKnown && memcmp(mac, sPeerMac, 6) == 0) {
        sTRx = now;
    }

    switch (h->type) {
        case PK_ADV: /* client only: the host announces itself */
            if (!sHost && !sPeerKnown) {
                memcpy(sPeerMac, mac, 6);
                sPeerKnown = 1;
                sTRx = now;
                sTSent = now - NP_JOIN_US;
                np_status(PSP_NP_JOINING, "Room found, joining...");
            }
            break;
        case PK_JOIN: /* host only */
            if (sHost && (!sPeerKnown || memcmp(mac, sPeerMac, 6) == 0)) {
                if (!sPeerKnown) {
                    memcpy(sPeerMac, mac, 6);
                    sPeerKnown = 1;
                    sAcked = 0;
                    np_events_reset();
                    np_capture_host_save();
                    sTSent = now - NP_RESEND_SAVE_US;
                    sTPing = now;
                    np_status(PSP_NP_JOINING, "Player joining, sending the save...");
                }
                sTRx = now;
                np_send(sPeerMac, PK_WELCOME, sHostFile, 0, NULL, 0);
            }
            break;
        case PK_WELCOME: /* client only */
            if (!sHost && sPeerKnown && !sWelcomed) {
                sWelcomed = 1;
                np_events_reset();
                sClientHostFile = h->a;
                sChunkMask = 0;
                sTPing = now;
                np_status(PSP_NP_JOINING, "Receiving the host's save...");
            }
            break;
        case PK_SAVE: /* client only */
            if (!sHost && sWelcomed && h->a < NP_CHUNKS && plen == NP_CHUNK) {
                memcpy(sClientImage + (int) h->a * NP_CHUNK, payload, NP_CHUNK);
                sChunkMask |= (uint8_t) (1U << h->a);
                if (sChunkMask == (uint8_t) ((1U << NP_CHUNKS) - 1U)) {
                    np_send(sPeerMac, PK_SAVEACK, 0, 0, NULL, 0); /* repeated if the host keeps sending (it did not get it) */
                    if (!sSaveReady && sState != PSP_NP_CONNECTED) {
                        __asm__ volatile("sync" ::: "memory");
                        sSaveReady = 1;
                        np_status(PSP_NP_CONNECTED, "Connected (host's save)");
                    }
                }
            }
            break;
        case PK_SAVEACK: /* host only */
            if (sHost && sPeerKnown && !sAcked) {
                sAcked = 1;
                np_status(PSP_NP_CONNECTED, "Connected");
            }
            break;
        case PK_PING:
            if (sPeerKnown && plen == 4) {
                np_send(sPeerMac, PK_PONG, 0, 0, payload, 4);
            }
            break;
        case PK_PONG:
            if (sPeerKnown && plen == 4) {
                uint32_t t;
                memcpy(&t, payload, 4);
                sRtt = (int) ((now - t) / 1000U);
                sRttAvg = (sRttAvg < 0) ? sRtt : (sRttAvg * 3 + sRtt) / 4;
            }
            break;
        case PK_STATE:
            if (sPeerKnown && sState == PSP_NP_CONNECTED && plen == (int) sizeof(PspNpPlayer) &&
                (!sStateInHave || (int8_t) (h->a - (uint8_t) sStateInSeq) > 0)) {
                const int lock = sceKernelCpuSuspendIntr();
                memcpy(&sStateIn, payload, sizeof(PspNpPlayer));
                sStateInSeq = h->a;
                sTStateRx = now;
                sStateInHave = 1;
                sceKernelCpuResumeIntr(lock);
            }
            break;
        case PK_EVENT:
            if (sPeerKnown && sState == PSP_NP_CONNECTED && plen == (int) sizeof(PspNpWorldEvent)) {
                if (h->a == sEvInNext) {
                    if (((sEvInHead + 1) % NP_EV_IN) != sEvInTail) { /* (when the game has not taken the older ones yet it is not acknowledged: it comes again) */
                        const int lock = sceKernelCpuSuspendIntr();
                        memcpy(&sEvIn[sEvInHead], payload, sizeof(PspNpWorldEvent));
                        sEvInHead = (sEvInHead + 1) % NP_EV_IN;
                        sEvInNext++;
                        sceKernelCpuResumeIntr(lock);
                        np_send(sPeerMac, PK_EVACK, h->a, 0, NULL, 0);
                    }
                } else {
                    np_send(sPeerMac, PK_EVACK, h->a, 0, NULL, 0); /* a repeat: our acknowledgement was lost */
                }
            }
            break;
        case PK_EVACK:
            if (sPeerKnown && sEvOutTail != sEvOutHead && h->a == sEvSeq) {
                sEvOutTail = (sEvOutTail + 1) % NP_EV_OUT;
                sEvSeq++;
                sTEvSent = 0;
            }
            break;
        case PK_OBJ:
            if (sPeerKnown && sState == PSP_NP_CONNECTED && plen > 0 && plen <= PSP_NP_OBJ_BYTES &&
                (!sObjInHave || (int8_t) (h->a - sObjInSeq) > 0)) {
                const int lock = sceKernelCpuSuspendIntr();
                memcpy(sObjIn, payload, (size_t) plen);
                sObjInLen = plen;
                sObjInSeq = (uint8_t) h->a;
                sObjInHave = 1;
                sceKernelCpuResumeIntr(lock);
            }
            break;
        case PK_FLD:
            if (sPeerKnown && sState == PSP_NP_CONNECTED && plen > 0 && plen <= PSP_NP_FLD_BYTES &&
                (!sFldInHave || (int8_t) (h->a - sFldInSeq) > 0)) {
                const int lock = sceKernelCpuSuspendIntr();
                memcpy(sFldIn, payload, (size_t) plen);
                sFldInLen = plen;
                sFldInSeq = (uint8_t) h->a;
                sFldInHave = 1;
                sceKernelCpuResumeIntr(lock);
            }
            break;
        case PK_NAME:
            if (sPeerKnown && plen == PSP_NP_NAME_BYTES && memcmp(sPeerName, payload, PSP_NP_NAME_BYTES) != 0) {
                const int lock = sceKernelCpuSuspendIntr();
                memcpy(sPeerName, payload, PSP_NP_NAME_BYTES);
                sPeerName[PSP_NP_NAME_BYTES - 1] = '\0';
                sPeerNameNew = 1;
                sceKernelCpuResumeIntr(lock);
            }
            break;
        case PK_PROGRESS:
            if (sPeerKnown && sState == PSP_NP_CONNECTED && plen > 0 && plen <= NP_PROG_MAX) {
                np_send(sPeerMac, PK_PROGACK, h->a, 0, NULL, 0);
                if (h->a != sProgInSeq) { /* a repeat (our ACK was lost) is not applied twice */
                    const int lock = sceKernelCpuSuspendIntr();
                    memcpy(sProgIn, payload, (size_t) plen);
                    sProgInLen = plen;
                    sProgInSeq = h->a;
                    sProgInPending = 1;
                    sceKernelCpuResumeIntr(lock);
                }
            }
            break;
        case PK_PROGACK:
            if (sPeerKnown && h->a == sProgOutSeq) {
                sProgOutPending = 0;
            }
            break;
        case PK_LEAVE:
            if (sPeerKnown) {
                if (sHost) {
                    sPeerKnown = 0;
                    sPeerName[0] = '\0'; /* the next player is somebody else */
                    sAcked = 0;
                    np_status(PSP_NP_HOSTING, "The player left. Waiting for a player...");
                } else {
                    np_fail("The host closed the room", 0);
                }
            }
            break;
        default:
            break;
    }
}

static void np_start_host(void);
static void np_start_join(void);

/* The radio drops now and then (interference, a bad frame, the other console moving): instead of ending the room the link is rebuilt from scratch, up to
 * NP_RETRIES_MAX times in a row (the counter is cleared once a link lasted NP_STABLE_US). The wait grows each time so the radio can settle, and the
 * host and the client, which both do this, meet again. The client keeps its game (the host's save is not adopted twice). Returns 0 if it gave up. */
static int np_recover(const char *why) {
    char msg[64];
    if (sRetries >= NP_RETRIES_MAX) {
        return 0;
    }
    sRetries++;
    snprintf(msg, sizeof(msg), "%s: reconnecting (%d/%d)", why, sRetries, NP_RETRIES_MAX);
    np_status(PSP_NP_STARTING, msg);
    np_cleanup();
    for (;;) {
        sceKernelDelayThread(800000U + 700000U * (unsigned) sRetries);
        if (sQuit || sReq != REQ_NONE || sState == PSP_NP_OFF) {
            return 1; /* the player left meanwhile */
        }
        np_take_events(); /* what the teardown itself reported is not a new error */
        if (sHost) {
            np_start_host();
        } else {
            np_start_join();
        }
        if (sState != PSP_NP_ERROR || sRetries >= NP_RETRIES_MAX || sQuit || sReq != REQ_NONE) {
            break;
        }
        sRetries++; /* even starting again failed (the radio is still busy): wait longer and retry */
    }
    sTRecovered = sceKernelGetSystemTimeLow() | 1U; /* (from the moment the restart has been issued) */
    return 1;
}

/* Pacing of the big streams: a byte budget that refills with time (less when the link is slow) and a drain mode with hysteresis. */
static void np_pace(uint32_t now) {
    const int lvl = psp_np_congestion();
    const int rate = NP_BUDGET_BPS >> (lvl > 1 ? lvl - 1 : 0); /* (level 1 keeps the full rate: only the intervals stretch) */
    const uint32_t dt = now - sTBudget;
    sTBudget = now;
    sBudget += (int) ((uint64_t) rate * dt / 1000000U);
    if (sBudget > rate / 4) {
        sBudget = rate / 4; /* at most a quarter of a second saved */
    }
    if (sRttAvg > 1000 || (sRtt > 2500 && sRttAvg > 400)) {
        sDrain = 1;
    } else if (sRttAvg >= 0 && sRttAvg < 600) {
        sDrain = 0;
    }
}

extern int gPspSixtyMode; /* gfx_psp.c: 1 while the game presents 60 fps (shown in the link log to compare both modes) */
static unsigned sSixtyTicks, sStepTicks;

/* Time of every tick of the game thread (microseconds), written by pc_main.c and printed every 20 s while connected: how long a whole tick takes, how long
 * the game logic plus the drawing list takes, the multiplayer frame, and the end of the frame (the waits for the GPU and the vblank, so not work). It tells
 * whether the ticks stay inside their budget (33.3 ms) and where the time goes, so the 30 / 60 fps question online can be judged with numbers. */
static volatile unsigned sPfN, sPfSlow, sPfSum[4], sPfMax[4]; /* 0 whole tick, 1 game logic, 2 multiplayer frame, 3 end of frame */
void psp_netplay_perf_note(unsigned tickUs, unsigned logicUs, unsigned netUs, unsigned endUs) {
    const unsigned v[4] = { tickUs, logicUs, netUs, endUs };
    int i;
    sPfN++;
    if (tickUs > 40000U) {
        sPfSlow++;
    }
    for (i = 0; i < 4; i++) {
        sPfSum[i] += v[i];
        if (v[i] > sPfMax[i]) {
            sPfMax[i] = v[i];
        }
    }
}

static void np_step(void) {
    const uint32_t now = sceKernelGetSystemTimeLow();
    static const uint8_t broadcast[6] = { 255, 255, 255, 255, 255, 255 };
    const unsigned events = np_take_events();
    int group = 0, r, i;
    if (sTLastStep != 0 && now - sTLastStep > sGapMax) {
        sGapMax = now - sTLastStep; /* the longest time this thread went without running */
    }
    sTLastStep = now;
    sStepTicks++;
    sSixtyTicks += (unsigned) (gPspSixtyMode != 0);
    if (sState == PSP_NP_CONNECTED) {
        static uint32_t sTPerf;
        np_pace(now);
        if (now - sTPerf >= 20000000U) { /* the time of the game ticks of the last 20 s (see psp_netplay_perf_note) */
            const unsigned n = sPfN;
            if (sTPerf != 0 && n > 0) {
                char pmsg[112];
                snprintf(pmsg, sizeof(pmsg), "perf %u ticks %u%% 60fps: tick %u/%u ms logic %u/%u net %u/%u end %u/%u slow %u", n, sStepTicks ? (100U * sSixtyTicks) / sStepTicks : 0U,
                         sPfSum[0] / n / 1000U, sPfMax[0] / 1000U, sPfSum[1] / n / 1000U, sPfMax[1] / 1000U, sPfSum[2] / n / 1000U, sPfMax[2] / 1000U,
                         sPfSum[3] / n / 1000U, sPfMax[3] / 1000U, sPfSlow);
                np_log(pmsg);
            }
            sTPerf = now;
            sPfN = sPfSlow = 0;
            memset((void *) sPfSum, 0, sizeof(sPfSum));
            memset((void *) sPfMax, 0, sizeof(sPfMax));
        }
        if (sTStat == 0) {
            sTStat = now;
        } else if (now - sTStat >= 2000000U) { /* a line every 2 s, only when something looked wrong (the file is small) */
            if (sRtt > 400 || sTxFail != 0 || sGapMax > 200000U || sDrain) {
                char msg[128];
                snprintf(msg, sizeof(msg), "link rtt %d avg %d lvl %d drain %d txfail %u tx %uB rx %u gap %ums 60fps %u%%", sRtt, sRttAvg, psp_np_congestion(), sDrain, (unsigned) sTxFail,
                         (unsigned) sTxBytes, (unsigned) sRxPkts, (unsigned) (sGapMax / 1000U), sStepTicks ? (100U * sSixtyTicks) / sStepTicks : 0U);
                np_log(msg);
            }
            {
                static uint32_t sTDiag;
                extern int psp_np_world_diag(char *buf, int n);
                extern int psp_opt_fps_cap(void);
                if (now - sTDiag >= 10000000U) { /* the world sync counters, every 10 s and only when something did not match */
                    char dmsg[128];
                    sTDiag = now;
                    if (psp_np_world_diag(dmsg, (int) sizeof(dmsg)) > 0) {
                        np_log(dmsg);
                        snprintf(dmsg, sizeof(dmsg), "fps limit in force %d, 60fps mode %u%%", psp_opt_fps_cap(), sStepTicks ? (100U * sSixtyTicks) / sStepTicks : 0U);
                        np_log(dmsg);
                    }
                }
            }
            sTStat = now;
            sTxFail = sTxBytes = sRxPkts = sGapMax = 0;
            sSixtyTicks = sStepTicks = 0;
        }
    }
    if (!sCtl) {
        return; /* the radio is not open yet (the start request has not been taken by this thread yet): nothing to look at */
    }

    if (sState == PSP_NP_CONNECTED && (sRetries != 0 || sIgnoredErr != 0) && now - sTConnected >= NP_STABLE_US) {
        sRetries = 0;
        sIgnoredErr = 0;
    }
    if (now - sTSwitch >= 250000U) { /* not every step: the query itself is a call into the radio driver */
        sTSwitch = now;
        if (sceWlanGetSwitchState() == 0) {
            if (++sWlanOffCount >= 4) { /* ~1 s in a row */
                np_fail("WLAN switch turned off", 0);
                return;
            }
        } else {
            sWlanOffCount = 0;
        }
    }
    if (events & 1U) {
        /* radio error: while starting (no socket yet) it is retried automatically, twice, from scratch */
        char msg[80];
        snprintf(msg, sizeof(msg), "radio error 0x%08X, last rx %u ms ago, switch %d", (unsigned int) sEventError, (unsigned) ((now - sTRx) / 1000U), sceWlanGetSwitchState());
        np_log(msg);
        if (sTRecovered != 0 && now - sTRecovered < 2000000U) {
            np_log("radio error ignored: left over from the restart");
            return; /* the radio library reports the teardown's own errors late: they were seen to start the next restart before it began (6 restarts in 50 s) */
        }
        {
            /* An error event from the ad-hoc library while the group is still up and the other console is still being heard is not worth a restart (a restart
             * takes ~6 s and redoes the world sync): if the link is really gone the group state or the silence timeout below catch it anyway. */
            int grp = 0;
            if (sState == PSP_NP_CONNECTED && sPeerKnown && sSocket >= 0 && sceNetAdhocctlGetState(&grp) >= 0 && grp == 1 && now - sTRx < 1500000U && sIgnoredErr < 3) {
                sIgnoredErr++;
                np_log("radio error ignored: still connected and hearing the peer");
                return;
            }
        }
        if (np_recover("radio error")) {
            return;
        }
        np_fail("Radio error", sEventError);
        return;
    }
    r = sceNetAdhocctlGetState(&group);
    if (r < 0) {
        np_fail("Group state", r);
        return;
    }

    /* --- client: looking for a room --- */
    if (!sHost && !sHaveTarget) {
        if (sScanPending) {
            if (events & (1U << 3)) {
                sScanPending = 0;
                if (np_pick_room()) {
                    np_status(PSP_NP_SEARCHING, "Room found, connecting...");
                    r = sceNetAdhocctlJoin(&sTarget);
                    if (r < 0) {
                        np_fail("Join room", r);
                        return;
                    }
                    sTStart = now;
                }
            } else if (now - sTScanStart >= NP_SCAN_TIMEOUT_US) {
                np_fail("No room found", 0);
            }
            if (!sHaveTarget) {
                return;
            }
        } else if (now - sTScan >= NP_SCAN_EVERY_US) {
            if (now - sTScanStart >= NP_SCAN_TIMEOUT_US) {
                np_fail("No room found", 0);
                return;
            }
            np_take_events();
            r = sceNetAdhocctlScan();
            if (r < 0) {
                np_fail("Scan", r);
                return;
            }
            sScanPending = 1;
            sTScan = now;
            return;
        } else {
            return;
        }
    }

    /* --- both: wait until the radio network is up, then open the socket --- */
    if (sSocket < 0) {
        SceNetAdhocctlParams params;
        if (group != 1) {
            if (now - sTStart >= NP_NETWORK_TIMEOUT_US) {
                np_fail("Radio connection timed out", 0);
            }
            return;
        }
        memset(&params, 0, sizeof(params));
        if ((r = sceNetAdhocctlGetParameter(&params)) < 0) {
            np_fail("Room details", r);
            return;
        }
        if ((r = sceNetAdhocPdpCreate(sMac, NP_PORT, 8192, 0) /* bigger receive buffer: bursts are no longer dropped */) < 0) {
            np_fail("Socket", r);
            return;
        }
        sSocket = r;
        sTStart = sTSent = sTRx = now;
        sTPing = now;
        if (sHost) {
            np_status(PSP_NP_HOSTING, "Room open. Waiting for a player...");
        } else {
            np_status(PSP_NP_JOINING, "Waiting for the room's announcement...");
        }
        return;
    } else if (group != 1) {
        if (!np_recover("link lost")) {
            np_fail("Connection lost", 0);
        }
        return;
    }

    /* --- packets --- */
    for (i = 0; i < 8; i++) {
        uint8_t mac[8];
        unsigned short port = 0;
        int length = (int) sizeof(sRx);
        r = sceNetAdhocPdpRecv(sSocket, mac, &port, sRx, &length, 0, 1);
        if (r < 0) {
            if ((unsigned int) r != 0x80410709U) { /* 0x80410709 = nothing to read */
                if (!np_recover("receive error")) {
                    np_fail("Receive", r);
                }
                return;
            }
            break;
        }
        sRxPkts++;
        np_handle_packet(mac, sRx, length, now);
        if (sState == PSP_NP_ERROR || sState == PSP_NP_OFF) {
            return;
        }
    }

    /* --- periodic sends --- */
    if (sHost) {
        if (!sPeerKnown) {
            if (now - sTSent >= NP_ADV_US) {
                sTSent = now;
                np_send(broadcast, PK_ADV, 0, 0, NULL, 0);
            }
        } else if (!sAcked && now - sTSent >= NP_RESEND_SAVE_US) {
            sTSent = now;
            for (i = 0; i < NP_CHUNKS; i++) {
                np_send(sPeerMac, PK_SAVE, i, NP_CHUNKS, sHostImage + i * NP_CHUNK, NP_CHUNK);
            }
        }
    } else if (sPeerKnown && !sWelcomed && now - sTSent >= NP_JOIN_US) {
        sTSent = now;
        np_send(sPeerMac, PK_JOIN, 0, 0, NULL, 0);
    }
    if (sState == PSP_NP_CONNECTED && sPeerKnown && sProgOutPending && now - sTProgSent >= NP_PROG_RESEND_US) {
        uint8_t tmp[NP_PROG_MAX];
        uint8_t seq;
        int len;
        const int lock = sceKernelCpuSuspendIntr();
        len = sProgOutLen;
        seq = sProgOutSeq;
        memcpy(tmp, sProgOut, (size_t) len);
        sceKernelCpuResumeIntr(lock);
        sTProgSent = now;
        np_send(sPeerMac, PK_PROGRESS, seq, 0, tmp, len);
    }
    if (sState == PSP_NP_CONNECTED && sPeerKnown && sStateOutNew && now - sTStateSent >= (30000U << psp_np_congestion())) {
        PspNpPlayer tmp;
        const int lock = sceKernelCpuSuspendIntr();
        tmp = sStateOut;
        sStateOutNew = 0;
        sceKernelCpuResumeIntr(lock);
        sTStateSent = now;
        np_send(sPeerMac, PK_STATE, ++sStateOutSeq, 0, &tmp, (int) sizeof(tmp));
    }
    if (sState == PSP_NP_CONNECTED && sPeerKnown && sObjOutLen > 0 && !sDrain && sBudget > 0 && now - sTObjSent >= (NP_OBJ_US << psp_np_congestion())) {
        uint8_t tmp[PSP_NP_OBJ_BYTES];
        int len;
        const int lock = sceKernelCpuSuspendIntr();
        len = sObjOutLen;
        memcpy(tmp, sObjOut, (size_t) len);
        sObjOutLen = 0;
        sceKernelCpuResumeIntr(lock);
        sTObjSent = now;
        sBudget -= len;
        np_send(sPeerMac, PK_OBJ, ++sObjOutSeq, 0, tmp, len);
    }
    if (sState == PSP_NP_CONNECTED && sPeerKnown && sFldOutLen > 0 && !sDrain && sBudget > 0 && now - sTFldSent >= (NP_FLD_US << psp_np_congestion())) {
        uint8_t tmp[PSP_NP_FLD_BYTES];
        int len;
        const int lock = sceKernelCpuSuspendIntr();
        len = sFldOutLen;
        memcpy(tmp, sFldOut, (size_t) len);
        sFldOutLen = 0;
        sceKernelCpuResumeIntr(lock);
        sTFldSent = now;
        sBudget -= len;
        np_send(sPeerMac, PK_FLD, ++sFldOutSeq, 0, tmp, len);
    }
    if (sState == PSP_NP_CONNECTED && sPeerKnown && sEvOutTail != sEvOutHead && (sTEvSent == 0 || now - sTEvSent >= np_ev_resend_us())) {
        PspNpWorldEvent ev;
        const int lock = sceKernelCpuSuspendIntr();
        ev = sEvOut[sEvOutTail];
        sceKernelCpuResumeIntr(lock);
        sTEvSent = now ? now : 1;
        np_send(sPeerMac, PK_EVENT, sEvSeq, 0, &ev, (int) sizeof(ev));
    }
    if (sPeerKnown && (sHost || sWelcomed) && now - sTPing >= NP_PING_US) {
        sTPing = now;
        np_send(sPeerMac, PK_PING, 0, 0, &now, 4);
        np_send(sPeerMac, PK_NAME, 0, 0, sMyName, PSP_NP_NAME_BYTES);
    }

    /* --- timeouts --- */
    if (!sHost && !sPeerKnown && now - sTStart >= NP_NETWORK_TIMEOUT_US) {
        np_fail("The room did not answer", 0);
    } else if (!sHost && sPeerKnown && !sWelcomed && now - sTStart >= 2U * NP_NETWORK_TIMEOUT_US) {
        np_fail("The host did not accept", 0);
    } else if (sPeerKnown && now - sTRx >= NP_PEER_TIMEOUT_US) {
        if (sHost) {
            sPeerKnown = 0;
            sPeerName[0] = '\0'; /* the next player is somebody else */
            sAcked = 0;
            np_status(PSP_NP_HOSTING, "Player lost. Waiting for a player...");
        } else if (!np_recover("host silent")) {
            np_fail("Connection to the host lost", 0);
        }
    }
}

static void np_leave_now(void) {
    int i;
    if (sSocket >= 0 && sPeerKnown) {
        for (i = 0; i < 2; i++) {
            np_send(sPeerMac, PK_LEAVE, 0, 0, NULL, 0);
        }
    }
    np_cleanup();
    np_status(PSP_NP_OFF, "");
}

static int np_thread(SceSize args, void *argp) {
    (void) args;
    (void) argp;
    while (!sQuit) {
        const int req = sReq;
        if (req != REQ_NONE) {
            sReq = REQ_NONE;
            if (req == REQ_HOST) {
                np_start_host();
            } else if (req == REQ_JOIN) {
                np_start_join();
            } else {
                np_leave_now();
            }
        } else if (sState != PSP_NP_OFF && sState != PSP_NP_ERROR) {
            np_step();
        }
        sceKernelDelayThread(6000); /* short poll: lower latency for events and states (the thread has a low priority) */
    }
    np_leave_now();
    return 0;
}

static void np_request(int req) {
    if (sThread < 0) {
        sThread = sceKernelCreateThread("sm64_netplay", np_thread, NP_THREAD_PRIO, 0x4000, PSP_THREAD_ATTR_USER, NULL);
        if (sThread < 0) {
            np_status(PSP_NP_ERROR, "Could not start the network thread");
            return;
        }
        sceKernelStartThread(sThread, 0, NULL);
    }
    sReq = req;
}

/* ------------------------------------------------------------------ public API */

/* This console's nickname (System Settings), cut to what is sent without splitting a UTF-8 character. Game thread, before the worker starts. */
static void np_read_names(void) {
    char nick[128];
    int n = 0;

    memset(nick, 0, sizeof(nick));
    memset(sMyName, 0, sizeof(sMyName));
    memset(sPeerName, 0, sizeof(sPeerName));
    if (sceUtilityGetSystemParamString(PSP_SYSTEMPARAM_ID_STRING_NICKNAME, nick, sizeof(nick)) != 0) {
        return;
    }
    nick[sizeof(nick) - 1] = '\0';
    while (nick[n] != '\0') {
        const unsigned char c = (unsigned char) nick[n];
        const int len = (c < 0x80) ? 1 : ((c >= 0xF0) ? 4 : ((c >= 0xE0) ? 3 : 2));
        if (n + len > PSP_NP_NAME_BYTES - 1) {
            break;
        }
        n += len;
    }
    memcpy(sMyName, nick, (size_t) n);
}

void psp_netplay_host(void) {
    if (sState == PSP_NP_OFF || sState == PSP_NP_ERROR) {
        np_read_names();
        sRetries = 0;
        np_status(PSP_NP_STARTING, "Starting...");
        np_request(REQ_HOST);
    }
}

void psp_netplay_join(void) {
    if (sState == PSP_NP_OFF || sState == PSP_NP_ERROR) {
        np_read_names();
        sRetries = 0;
        np_status(PSP_NP_STARTING, "Starting...");
        np_request(REQ_JOIN);
    }
}

void psp_netplay_leave(void) {
    if (sState != PSP_NP_OFF) {
        np_status(PSP_NP_OFF, "");
        np_request(REQ_LEAVE);
    }
}

int psp_netplay_state(void) {
    return sState;
}

const char *psp_netplay_status(void) {
    return sStatus;
}

/* 0 = the link is fine, 1..3 = increasingly slow: the streams are sent less often and smaller */
int psp_np_congestion(void) {
    const int r = sRttAvg;
    /* The thresholds are high on purpose: in the field logs the RTT sat at 250-450 ms while we were sending only 2-3 KB/s (and was 40 ms at 6+ KB/s earlier), so
 * that latency comes from the radio, not from our load, and backing off only made the enemies and platforms of the other console update less. */
    int lvl = (r < 0 || r < 350) ? 0 : ((r < 700) ? 1 : ((r < 1100) ? 2 : 3));
    if (sTPressure != 0 && sceKernelGetSystemTimeLow() - sTPressure < 1000000U && lvl < 2) {
        lvl = 2; /* the radio refused a packet a moment ago */
    }
    return lvl;
}

int psp_netplay_rtt_ms(void) {
    return sRtt;
}

void psp_netplay_frame(void) {
    if (sSaveReady && !sOverride) {
        if (sState == PSP_NP_CONNECTED) {
            /* the client starts playing with a copy of the host's save file, in memory only */
            sOverride = 1;
            psp_netplay_adopt_host_save(sClientHostFile);
        } else {
            sSaveReady = 0; /* the room ended before the swap: nothing to undo */
        }
    }
    if (sProgInPending && sState == PSP_NP_CONNECTED && (sOverride || sHost) && gCurrSaveFileNum >= 1) {
        /* the other console's progress: merged into the current save file (here, on the game thread, between frames) */
        uint8_t tmp[NP_PROG_MAX];
        int len, lock = sceKernelCpuSuspendIntr();
        len = sProgInLen;
        memcpy(tmp, sProgIn, (size_t) len);
        sProgInPending = 0;
        sceKernelCpuResumeIntr(lock);
        psp_netplay_merge_progress(tmp, len);
    }
    if (sState == PSP_NP_CONNECTED && (sHost || sOverride)) {
        /* the other player: this player's pose goes out, the other one's comes in and is shown as a second Mario (Luigi if it is the client) */
        PspNpPlayer me, it;
        int have, lock;
        const uint32_t now = sceKernelGetSystemTimeLow();
        if (psp_np_game_capture(&me)) {
            lock = sceKernelCpuSuspendIntr();
            sStateOut = me;
            sStateOutNew = 1;
            sceKernelCpuResumeIntr(lock);
        }
        lock = sceKernelCpuSuspendIntr();
        have = sStateInHave && (now - sTStateRx) < 1000000U; /* no news for a second: it is hidden */
        if (have) {
            it = sStateIn;
        }
        sceKernelCpuResumeIntr(lock);
        psp_np_game_apply(have ? &it : NULL, sHost);
        psp_np_game_keep_local_luigi(!sHost);
        /* names and notices (drawn by psp_np_hud_draw in game/psp_options.c) */
        if (sPeerNameNew || !sGameActive) {
            char name[PSP_NP_NAME_BYTES];
            lock = sceKernelCpuSuspendIntr();
            memcpy(name, sPeerName, sizeof(name));
            sPeerNameNew = 0;
            sceKernelCpuResumeIntr(lock);
            psp_np_hud_peer_name(name, sHost); /* (no name yet: "Luigi" / "Mario", whichever it plays as) */
        }
        if (!sGameActive) {
            psp_np_hud_link(1);
        }
        psp_np_hud_peer_place(have, have ? it.level : 0, have ? it.area : 0, have ? it.act : 0);
#if PSP_NETPLAY_WORLD
        psp_np_world_frame(have, have ? it.level : 0, have ? ((it.area & 15) | ((it.act & 15) << 4)) : 0, sHost); /* (the area byte also carries the act: see np_area_key) */
#endif
        sGameActive = 1;
    } else if (sGameActive) {
        psp_np_game_remove_remote();
        psp_np_game_keep_local_luigi(0);
        psp_np_hud_link(0);
#if PSP_NETPLAY_WORLD
        psp_np_world_reset();
#endif
        sGameActive = 0;
    }
    if (sOverride && sState != PSP_NP_CONNECTED) {
        /* the room is over: back to this console's own save file (the one on the Memory Stick was never touched) */
        sOverride = 0;
        sSaveReady = 0;
        save_file_load_all();
        psp_netplay_refresh_hud();
    }
}

int psp_np_world_send(const PspNpWorldEvent *e) {
    int ok = 0, lock;
    if (sState != PSP_NP_CONNECTED) {
        return 0;
    }
    lock = sceKernelCpuSuspendIntr();
    if (((sEvOutHead + 1) % NP_EV_OUT) != sEvOutTail) {
        sEvOut[sEvOutHead] = *e;
        sEvOutHead = (sEvOutHead + 1) % NP_EV_OUT;
        ok = 1;
    }
    sceKernelCpuResumeIntr(lock);
    return ok;
}

int psp_np_world_obj_send(const void *data, int len) {
    int lock;
    if (sState != PSP_NP_CONNECTED || len <= 0 || len > PSP_NP_OBJ_BYTES) {
        return 0;
    }
    lock = sceKernelCpuSuspendIntr();
    memcpy(sObjOut, data, (size_t) len);
    sObjOutLen = len;
    sceKernelCpuResumeIntr(lock);
    return 1;
}

int psp_np_world_obj_take(void *data) {
    int len, lock = sceKernelCpuSuspendIntr();
    len = sObjInLen;
    if (len > 0) {
        memcpy(data, sObjIn, (size_t) len);
        sObjInLen = 0;
    }
    sceKernelCpuResumeIntr(lock);
    return len;
}

int psp_np_world_fld_send(const void *data, int len) {
    int lock;
    if (sState != PSP_NP_CONNECTED || len <= 0 || len > PSP_NP_FLD_BYTES) {
        return 0;
    }
    lock = sceKernelCpuSuspendIntr();
    memcpy(sFldOut, data, (size_t) len);
    sFldOutLen = len;
    sceKernelCpuResumeIntr(lock);
    return 1;
}

int psp_np_world_fld_take(void *data) {
    int len, lock = sceKernelCpuSuspendIntr();
    len = sFldInLen;
    if (len > 0) {
        memcpy(data, sFldIn, (size_t) len);
        sFldInLen = 0;
    }
    sceKernelCpuResumeIntr(lock);
    return len;
}

int psp_np_world_take(PspNpWorldEvent *e) {
    int ok = 0, lock;
    lock = sceKernelCpuSuspendIntr();
    if (sEvInTail != sEvInHead) {
        *e = sEvIn[sEvInTail];
        sEvInTail = (sEvInTail + 1) % NP_EV_IN;
        ok = 1;
    }
    sceKernelCpuResumeIntr(lock);
    return ok;
}

/* Called by the save code (game thread) whenever the current save file was written: it is sent to the other console. */
void psp_netplay_progress_changed(const void *file, int size) {
    int lock;
    if (sState != PSP_NP_CONNECTED || file == NULL || size <= 0 || size > NP_PROG_MAX) {
        return;
    }
    lock = sceKernelCpuSuspendIntr();
    memcpy(sProgOut, file, (size_t) size);
    sProgOutLen = size;
    sProgOutSeq++;
    sProgOutPending = 1;
    sTProgSent = 0; /* send it on the worker's next turn */
    sceKernelCpuResumeIntr(lock);
}

int psp_netplay_save_override(void) {
    return sOverride;
}

void psp_netplay_save_read(uint8_t *out512) {
    memcpy(out512, sClientImage, NP_SAVE_BYTES);
}

void psp_netplay_save_write(const uint8_t *in512) {
    memcpy(sClientImage, in512, NP_SAVE_BYTES);
}

/* Before the console sleeps: leaves the room and waits until the worker has closed the network, since the WLAN does not survive sleep. The worker can be
 * in the middle of a blocking call (scan, module load), so the wait is generous (up to ~3 s): sleeping with the WLAN half closed is worse than a slow sleep.
 * The result (see psp_netplay.h) goes to the power log, so a hang on wake-up can be tied to it or ruled out. */
unsigned int psp_netplay_suspend(void) {
    int i;
    unsigned int open;
    if (sThread < 0 || (sState == PSP_NP_OFF && sSocket < 0 && !sCtl && !sNet && !sModAdhoc && !sModCommon)) {
        return 0x80000000u;
    }
    psp_netplay_leave();
    if (sState == PSP_NP_OFF && sReq == REQ_NONE && (sSocket >= 0 || sCtl || sNet || sModAdhoc || sModCommon)) {
        np_request(REQ_LEAVE); /* the room was already left but something is still open (psp_netplay_leave did nothing) */
    }
    for (i = 0; i < 600 && (sReq != REQ_NONE || sSocket >= 0 || sCtl || sNet || sModAdhoc || sModCommon); i++) {
        sceKernelDelayThread(5000);
    }
    open = (sReq != REQ_NONE ? 1U : 0U) | (sSocket >= 0 ? 2U : 0U) | (sCtl ? 4U : 0U) | (sNet ? 8U : 0U) | (sModAdhoc ? 16U : 0U) | (sModCommon ? 32U : 0U);
    return 0x40000000u | (open << 16) | (unsigned int) (i * 5 > 0xFFFF ? 0xFFFF : i * 5);
}

void psp_netplay_shutdown(void) {
    int i;
    if (sThread < 0) {
        return;
    }
    sQuit = 1;
    for (i = 0; i < 100 && sThread >= 0; i++) { /* up to ~1 s for the thread to close the network */
        SceKernelThreadInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelReferThreadStatus(sThread, &info) < 0 || (info.status & PSP_THREAD_STOPPED)) {
            break;
        }
        sceKernelDelayThread(10000);
    }
}

#endif
