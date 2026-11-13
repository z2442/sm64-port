/* WLAN multiplayer, game side (see pc/psp_netplay.c for the network): captures this player's pose, and shows the other player as a second
 * Mario / Luigi object. Like the castle's mirror Mario, the second model uses the same geo layout as Mario, so it shares Mario's body state
 * (eyes, hands, head turn) while its position, angles and animation come from the network.
 *
 * Everything here runs on the game thread, between two frames (called from psp_netplay_frame). */
#include <stdio.h>
#include <ultra64.h>
#include <stddef.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#include "sm64.h"
#include "area.h"
#include "camera.h"
#include "behavior_data.h"
#include "engine/graph_node.h"
#include "game_init.h"
#include "level_update.h"
#include "mario.h"
#include "memory.h"
#include "model_ids.h"
#include "object_helpers.h"
#include "object_list_processor.h"
#include "spawn_object.h"
#include "spawn_sound.h"
#include "mario_misc.h"
#include "rendering_graph_node.h"
#include "audio/external.h"
#include "behavior_actions.h"
#include "course_table.h"
#include "interaction.h"
#include "engine/math_util.h"
#include "object_fields.h"
#include "pc/psp_netplay.h"

#if defined(TARGET_PSP) && PSP_NETPLAY

#define NP_OBJ_MARK 0x4E50 /* oBehParams2ndByte-style marker: tells our object from any other that reuses the slot */
#define NP_ANIM_BUF_BYTES 0x4000
#define NP_SNAP_DIST 800.0f
#define NP_EXTRAP_MAX 5 /* frames the other player is carried on at the speed of its last packet */

static struct Object *sRemote;
static int sRemoteModel = -1;
static struct MarioState sNetMs;          /* only marioObj / animation / unkB0 are used: the animation helpers of mario.c work on it */
static struct MarioAnimation sNetAnimList; /* its own animation buffer: the local Mario keeps using the game's one */
static void *sNetAnimBuf;
static int sAnimReady;
static int sLocalLuigi;
static struct MarioBodyState sNetBody; /* the other player's body state: what the geo callbacks of its model read (see BODY_STATE in mario_misc.c) */
static u8 sVoiceSeqSeen;               /* last voice of the other player already played here */
static int sVoiceSeqKnown;
static int sRemoteAge;
static int sRemoteHeadCam;                 /* the other player is in first person: sNetBody.headAngle is its camera's head rotation */
static int sRemoteSquish;                  /* frames left of the squash of the other player's model (jumped / ground pounded on) */

/* MarioState.flags <-> the 16 bits sent: the caps (bits 0-4) and punching / kicking / tripping (bits 20-22) */
#define NP_FLAGS_PACK(f) ((uint16_t) (((f) & MARIO_CAP_FLAGS) | ((((f) >> 20) & 7) << 5)))
#define NP_FLAGS_UNPACK(p) ((uint32_t) (((p) & MARIO_CAP_FLAGS) | ((((p) >> 5) & 7U) << 20)))

#define NP_PLAY_MODE_PAUSED 2 /* sCurrPlayMode value of the pause menu (the define is private to level_update.c) */

struct MarioBodyState *psp_np_game_body_state(int index) {
    if (sRemote != NULL && gCurGraphNodeObject == &sRemote->header.gfx) {
        return &sNetBody;
    }
    return &gBodyStates[index];
}

int psp_np_game_remote_head_cam(void) {
    return sRemoteHeadCam;
}

int psp_np_game_drawing_remote(void) {
    return sRemote != NULL && gCurGraphNodeObject == &sRemote->header.gfx;
}

static int np_anim_init(void) {
    if (sAnimReady) {
        return 1;
    }
    sNetAnimBuf = malloc(NP_ANIM_BUF_BYTES);
    if (sNetAnimBuf == NULL || D_80339D10.animDmaTable == NULL) {
        free(sNetAnimBuf);
        sNetAnimBuf = NULL;
        return 0;
    }
    memset(sNetAnimBuf, 0, NP_ANIM_BUF_BYTES);
    sNetAnimList.animDmaTable = D_80339D10.animDmaTable; /* the same table of animations in the ROM data */
    sNetAnimList.currentAnimAddr = NULL;
    sNetAnimList.targetAnim = (struct Animation *) sNetAnimBuf;
    sNetMs.animation = &sNetAnimList;
    sAnimReady = 1;
    return 1;
}

static int np_remote_alive(void) {
    return sRemote != NULL && sRemote->activeFlags != ACTIVE_FLAG_DEACTIVATED && sRemote->oBehParams2ndByte == NP_OBJ_MARK
           && sRemote->behavior == segmented_to_virtual(bhvStaticObject);
}

void psp_np_game_remove_remote(void) {
    if (np_remote_alive()) {
        obj_mark_for_deletion(sRemote);
    }
    sRemote = NULL;
    sRemoteAge = 0;
    sRemoteSquish = 0;
}

struct MarioState *psp_np_game_remote_state(void) {
    return np_remote_alive() ? &sNetMs : NULL;
}

void psp_np_game_remote_squish(int frames) {
    if (frames > sRemoteSquish) {
        sRemoteSquish = frames;
    }
}

struct Object *psp_np_game_remote(void) {
    return np_remote_alive() ? sRemote : NULL;
}

uint32_t psp_np_game_remote_action(void) {
    return sNetBody.action;
}

int psp_np_game_remote_vanished(void) {
    return (sNetBody.modelState & 0x100) != 0 && (sNetBody.capState & 0x1) == 0; /* alpha bit set while the cap bit 0 is clear: the remote is drawn translucent (vanish cap) */
}

int psp_np_game_remote_age(void) {
    return sRemoteAge;
}

/* This player's pose, to send. Returns 0 when there is nothing to send (no level running). */
/* The "instance" a player is in, the way sm64coopdx has it: the level, the area AND the act (the star chosen at the star select). Two players that are in
 * different acts of the same course are in different copies of it: they do not see each other, and what one does to the world (enemies, coins, stars,
 * platforms) is not shown to the other. The area byte of the world messages carries both: bits 0-3 the area, bits 4-7 the act (only the main courses
 * have acts; anywhere else the act is 0, whatever number the game left in gCurrActNum). */
static int np_act(void) {
    return COURSE_IS_MAIN_COURSE(gCurrCourseNum) ? (gCurrActNum & 15) : 0;
}

static int np_area_key(void) {
    return (gCurrAreaIndex & 15) | (np_act() << 4);
}

int psp_np_game_capture(PspNpPlayer *out) {
    struct Object *m = gMarioObject;
    if (m == NULL || gMarioState == NULL || gCurrentArea == NULL) {
        return 0;
    }
    out->level = (int16_t) gCurrLevelNum;
    out->area = (int16_t) gCurrAreaIndex;
    out->pos[0] = m->header.gfx.pos[0];
    out->pos[1] = m->header.gfx.pos[1];
    out->pos[2] = m->header.gfx.pos[2];
    out->angle[0] = m->header.gfx.angle[0];
    out->angle[1] = m->header.gfx.angle[1];
    out->angle[2] = m->header.gfx.angle[2];
    out->anim = (int16_t) m->header.gfx.unk38.animID;
    out->frame = (int16_t) m->header.gfx.unk38.animFrame;
    out->yTrans = (int16_t) m->header.gfx.unk38.animYTrans;
    out->accel = (int32_t) m->header.gfx.unk38.animAccel;
    out->visible = (m->header.gfx.node.flags & GRAPH_RENDER_ACTIVE) ? 1 : 0;
    out->frozen = (sCurrPlayMode == NP_PLAY_MODE_PAUSED && !psp_live_pause()) ? 1 : 0;
    {
        const struct MarioBodyState *b = &gBodyStates[0];
        out->action = b->action;
        out->modelState = b->modelState;
        out->capState = (uint8_t) b->capState;
        out->eyeState = (uint8_t) b->eyeState;
        out->handState = (uint8_t) b->handState;
        out->wingFlutter = (uint8_t) b->wingFlutter;
        out->grabPos = (uint8_t) b->grabPos;
        out->punchState = b->punchState;
        for (int i = 0; i < 3; i++) {
            out->torso[i] = b->torsoAngle[i];
            out->head[i] = b->headAngle[i];
        }
    }
    out->voice = gPspVoiceLast;
    out->voiceSeq = gPspVoiceSeq;
    {
        const struct MarioState *s = gMarioState;
        out->invincTimer = (uint8_t) (s->invincTimer < 0 ? 0 : (s->invincTimer > 255 ? 255 : s->invincTimer));
        out->hurtCounter = s->hurtCounter;
        out->knockbackTimer = s->knockbackTimer;
        out->bounceSquish = s->bounceSquishTimer;
        out->actionState = s->actionState;
        /* punching / kicking / tripping are set for a single frame or two of the attack: they are held a few frames so one lost or late packet
         * does not lose the attack (the other console only looks at them together with the action) */
        static uint32_t sAttackBits;
        static int sAttackHold;
        const uint32_t now = s->flags & (MARIO_PUNCHING | MARIO_KICKING | MARIO_TRIPPING);
        if (now != 0) {
            sAttackBits = now;
            sAttackHold = 4;
        } else if (sAttackHold > 0) {
            sAttackHold--;
        } else {
            sAttackBits = 0;
        }
        out->flags = NP_FLAGS_PACK((s->flags & ~(MARIO_PUNCHING | MARIO_KICKING | MARIO_TRIPPING)) | sAttackBits);
        out->forwardVel = (int16_t) s->forwardVel;
        for (int i = 0; i < 3; i++) {
            out->vel[i] = (int16_t) s->vel[i];
        }
    }
    /* first person (C-Up): the head follows the camera, which is not in the body state (see geo_mario_head_rotation) */
    out->headCam = 0;
    if (gCurrentArea->camera != NULL && gCurrentArea->camera->mode == CAMERA_MODE_C_UP) {
        out->headCam = 1;
        out->head[0] = gPlayerCameraState->headRotation[0];
        out->head[1] = gPlayerCameraState->headRotation[1];
        out->head[2] = 0;
    }
    out->act = (uint8_t) np_act();
    return 1;
}

/* Shows the other player (in == NULL: hides it). remoteIsLuigi: the other console is the client, which plays as Luigi. */
void psp_np_game_apply(const PspNpPlayer *in, int remoteIsLuigi) {
    struct Object *o;
    const int model = remoteIsLuigi ? MODEL_LUIGI : MODEL_MARIO;
    /* this console is in a pause menu that stops the game (not the multiplayer one, see psp_live_pause): no object is updated, so the model is
     * moved by hand */
    const int paused = (sCurrPlayMode == NP_PLAY_MODE_PAUSED && !psp_live_pause());

    if (in == NULL || !in->visible || in->level != (int16_t) gCurrLevelNum || in->area != (int16_t) gCurrAreaIndex || in->act != (uint8_t) np_act() || gMarioObject == NULL
        || gCurrentArea == NULL || gLoadedGraphNodes[model] == NULL || !np_anim_init()) {
        psp_np_game_remove_remote();
        sVoiceSeqKnown = 0;
        return;
    }
    if (np_remote_alive() && sRemoteModel != model) {
        psp_np_game_remove_remote();
    }
    if (!np_remote_alive()) {
        if (paused) {
            return; /* it is created as soon as the game goes on */
        }
        sRemote = spawn_object(gMarioObject, model, bhvStaticObject);
        if (sRemote == NULL) {
            return;
        }
        sRemoteModel = model;
        o = sRemote;
        o->oBehParams2ndByte = NP_OBJ_MARK;
        o->oFlags |= OBJ_FLAG_ACTIVE_FROM_AFAR;
        o->oDrawingDistance = 20000.0f;
        o->oPosX = in->pos[0];
        o->oPosY = in->pos[1];
        o->oPosZ = in->pos[2];
        o->header.gfx.unk38.animID = -1; /* forces the first animation to be set */
    }
    o = sRemote;
    if (!paused && sRemoteAge < 1000000) {
        sRemoteAge++;
    }

    /* body state of the model (read by the geo callbacks, see BODY_STATE in mario_misc.c) */
    sNetBody.action = in->action;
    sNetBody.modelState = in->modelState;
    sNetBody.capState = (s8) in->capState;
    sNetBody.eyeState = (s8) in->eyeState;
    sNetBody.handState = (s8) in->handState;
    sNetBody.wingFlutter = (s8) in->wingFlutter;
    sNetBody.grabPos = (s8) in->grabPos;
    sNetBody.punchState = in->punchState;
    sRemoteHeadCam = in->headCam;
    for (int i = 0; i < 3; i++) {
        sNetBody.torsoAngle[i] = in->torso[i];
        sNetBody.headAngle[i] = in->head[i];
    }

    /* position: the packet is the other console's position at the moment it was sent, and the next one is 30-120 ms away (more on a slow link),
     * so the target is carried on with the speed that came in the packet for the frames since it arrived (at most NP_EXTRAP_MAX), and the model eases
     * toward it. Without this the other player moves in steps at the packet rate and always trails behind. A snap when far (warps, level entry). */
    {
        static float sLastPos[3];
        static uint32_t sLastAct;
        static int16_t sLastFrame, sLastAnim;
        static int sAge;
        float tx = in->pos[0], ty = in->pos[1], tz = in->pos[2];
        if (!paused) {
            if (in->pos[0] != sLastPos[0] || in->pos[1] != sLastPos[1] || in->pos[2] != sLastPos[2] || in->action != sLastAct || in->frame != sLastFrame
                || in->anim != sLastAnim) {
                sLastPos[0] = in->pos[0]; sLastPos[1] = in->pos[1]; sLastPos[2] = in->pos[2];
                sLastAct = in->action; sLastFrame = in->frame; sLastAnim = in->anim;
                sAge = 0; /* a new packet */
            } else if (sAge < 1000) {
                sAge++;
            }
        }
        if (sAge > 0 && !in->frozen) {
            const int n = sAge > NP_EXTRAP_MAX ? NP_EXTRAP_MAX : sAge;
            tx += (float) in->vel[0] * (float) n;
            ty += (float) in->vel[1] * (float) n;
            tz += (float) in->vel[2] * (float) n;
        }
        if (fabsf(in->pos[0] - o->oPosX) + fabsf(in->pos[2] - o->oPosZ) > NP_SNAP_DIST || fabsf(in->pos[1] - o->oPosY) > NP_SNAP_DIST) {
            o->oPosX = in->pos[0];
            o->oPosY = in->pos[1];
            o->oPosZ = in->pos[2];
        } else {
            o->oPosX += (tx - o->oPosX) * 0.5f;
            o->oPosY += (ty - o->oPosY) * 0.5f;
            o->oPosZ += (tz - o->oPosZ) * 0.5f;
        }
    }
    o->oFaceAnglePitch = in->angle[0];
    o->oFaceAngleYaw = in->angle[1];
    o->oFaceAngleRoll = in->angle[2];
    if (paused) {
        /* the object update (which copies these into the graph node) does not run during the pause: it is done here so the other player keeps moving */
        o->header.gfx.pos[0] = o->oPosX;
        o->header.gfx.pos[1] = o->oPosY;
        o->header.gfx.pos[2] = o->oPosZ;
        o->header.gfx.angle[0] = in->angle[0];
        o->header.gfx.angle[1] = in->angle[1];
        o->header.gfx.angle[2] = in->angle[2];
    }

    /* the other player as a MarioState, for the player vs player code (interaction.c): only what a fight looks at */
    sNetMs.marioObj = o;
    sNetMs.marioBodyState = &sNetBody;
    sNetMs.pos[0] = o->oPosX;
    sNetMs.pos[1] = o->oPosY;
    sNetMs.pos[2] = o->oPosZ;
    sNetMs.faceAngle[0] = in->angle[0];
    sNetMs.faceAngle[1] = in->angle[1];
    sNetMs.faceAngle[2] = in->angle[2];
    sNetMs.action = in->action;
    sNetMs.actionState = in->actionState;
    sNetMs.flags = NP_FLAGS_UNPACK(in->flags);
    sNetMs.forwardVel = in->forwardVel;
    for (int i = 0; i < 3; i++) {
        sNetMs.vel[i] = in->vel[i];
    }
    sNetMs.invincTimer = in->invincTimer;
    sNetMs.hurtCounter = in->hurtCounter;
    sNetMs.knockbackTimer = in->knockbackTimer;
    if (in->bounceSquish > sRemoteSquish) {
        sRemoteSquish = in->bounceSquish;
    }
    /* its squash (another player jumped on it): the same rubber band as the game's squash of Mario */
    if (sRemoteSquish > 0) {
        extern u8 sSquishScaleOverTime[16];
        if (sRemoteSquish <= 16) {
            const int t = sRemoteSquish - 1;
            o->header.gfx.scale[1] = 1.0f - ((sSquishScaleOverTime[15 - t] * 0.6f) / 100.0f);
            o->header.gfx.scale[0] = ((sSquishScaleOverTime[15 - t] * 0.4f) / 100.0f) + 1.0f;
        } else {
            o->header.gfx.scale[0] = 1.4f;
            o->header.gfx.scale[1] = 0.4f;
        }
        o->header.gfx.scale[2] = o->header.gfx.scale[0];
        if (!paused) {
            sRemoteSquish--;
        }
    } else {
        o->header.gfx.scale[0] = o->header.gfx.scale[1] = o->header.gfx.scale[2] = 1.0f;
    }

    /* animation: the same calls the game makes for Mario, on this object and its own buffer. It runs on by itself between packets (like Mario's);
     * a packet only corrects it when it drifted (looping animations are compared around their loop), or pins it while the sender is paused. */
    sNetMs.marioObj = o;
    sNetMs.unkB0 = in->yTrans;
    if (in->anim >= 0) {
        if (in->accel != 0) {
            set_mario_anim_with_accel(&sNetMs, in->anim, in->accel);
        } else {
            set_mario_animation(&sNetMs, in->anim);
        }
        if (o->header.gfx.unk38.curAnim != NULL) {
            const struct Animation *a = o->header.gfx.unk38.curAnim;
            int diff = (int) o->header.gfx.unk38.animFrame - (int) in->frame;
            const int loop = (int) a->unk08 - (int) a->unk06;
            if (diff < 0) {
                diff = -diff;
            }
            if (loop > 0 && !(a->flags & ANIM_FLAG_NOLOOP) && loop - diff < diff) {
                diff = loop - diff;
            }
            if (in->frozen || diff > 3) {
                set_anim_to_frame(&sNetMs, in->frame);
            }
            if (paused) {
                /* nothing advances the animation during the pause: the frame is set directly */
                o->header.gfx.unk38.animFrame = in->frame;
                o->header.gfx.unk38.animFrameAccelAssist = ((s32) in->frame) << 16;
            }
        }
    }

    /* its voice (Mario's or Luigi's, whichever it is) is played here, from its position */
    if (!sVoiceSeqKnown) {
        sVoiceSeqSeen = in->voiceSeq;
        sVoiceSeqKnown = 1;
    } else if (in->voiceSeq != sVoiceSeqSeen) {
        sVoiceSeqSeen = in->voiceSeq;
        if (in->voice != 0) {
            psp_play_remote_voice(in->voice, remoteIsLuigi, o->header.gfx.cameraToObject);
        }
    }
}

/* The client plays as Luigi: the local Mario object just gets Luigi's model (and goes back to Mario's when the room ends). */
void psp_np_game_local_luigi(int on) {
    gPspLuigiVoice = on ? 1 : 0;
    if (on == sLocalLuigi || gMarioObject == NULL) {
        return;
    }
    if (gLoadedGraphNodes[MODEL_LUIGI] != NULL && gLoadedGraphNodes[MODEL_MARIO] != NULL) {
        gMarioObject->header.gfx.sharedChild = gLoadedGraphNodes[on ? MODEL_LUIGI : MODEL_MARIO];
        sLocalLuigi = on;
    }
}

/* After a level / area change Mario is created again with his own model: Luigi's is set again for the client. */
void psp_np_game_keep_local_luigi(int want) {
    gPspLuigiVoice = want ? 1 : 0;
    if (want && gMarioObject != NULL && gLoadedGraphNodes[MODEL_LUIGI] != NULL
        && gMarioObject->header.gfx.sharedChild != gLoadedGraphNodes[MODEL_LUIGI]) {
        gMarioObject->header.gfx.sharedChild = gLoadedGraphNodes[MODEL_LUIGI];
        sLocalLuigi = 1;
    } else if (!want && sLocalLuigi) {
        psp_np_game_local_luigi(0);
    }
}


#if PSP_NETPLAY_WORLD
/* ---- World events: what one player picks up (coins, stars, caps, 1-ups) disappears for the other one too ----
 * Like sm64coopdx (PACKET_COLLECT_COIN / COLLECT_STAR / COLLECT_ITEM), the object is not identified by a number but by its behavior and
 * position: the receiver takes the nearest object of that behavior. Both consoles run the same level, so the objects are where the sender
 * saw them. Coins also share the coin count (and the 100-coin star), as in sm64coopdx.
 * The area's collected items are kept in a log: when the other player comes into the area they are replayed (flag 0x80, so the coin count
 * is not touched again), and when this player enters an area the other one is already in, the other console replays its own. */
#define NP_W_LOG 160
#define NP_W_PEND 16
#define NP_W_RETRY 90 /* frames an event waits for its object (it may not exist yet, or may have been collected here at the same time) */
#define NP_W_REPLAY 0x80
static PspNpWorldEvent sWLog[NP_W_LOG];
static int sWLogN;
static PspNpWorldEvent sWPend[NP_W_PEND];
static u8 sWPendAge[NP_W_PEND];
static int sWPendN;
static int sWLevel = -1, sWArea = -1, sWAge;
static int sWPeerHere;
static int sPeerNowKnown, sPeerNowLevel, sPeerNowArea; /* where the other player was last reported (every frame, even while an area is being loaded) */
static int sWHost;

static s32 np_w_bhv(const struct Object *o) {
    return (s32) ((uintptr_t) virtual_to_segmented(0x13, o->behavior) - (uintptr_t) bhvYellowCoin);
}

static void np_w_log(const PspNpWorldEvent *e) {
    int i;
    for (i = 0; i < sWLogN; i++) {
        if (sWLog[i].bhv == e->bhv && abs(sWLog[i].pos[0] - e->pos[0]) < 8 && abs(sWLog[i].pos[1] - e->pos[1]) < 8
            && abs(sWLog[i].pos[2] - e->pos[2]) < 8) {
            return;
        }
    }
    if (sWLogN < NP_W_LOG) {
        sWLog[sWLogN++] = *e;
    }
}

/* The nearest live object with that behavior (and coin value) within maxDist of pos. */
/* A coin that falls out of an enemy flies away at once, so where it is collected is not where the other console's copy is any more. Both copies carry the same
 * number (Object.unused2: the spawning console makes it, odd for the host and even for the client) and the collection event names it. */
static int sLootId;
static s16 np_w_loot_new_id(void) {
    sLootId = (sLootId + 1) % 16000;
    return (s16) ((sLootId + 1) * 2 + (sWHost ? 1 : 0));
}

static struct Object *np_w_find_loot(s32 bhv, s16 id) {
    int i;
    for (i = 0; i < NUM_OBJ_LISTS; i++) {
        struct ObjectNode *head = &gObjectLists[i];
        struct Object *obj;
        for (obj = (struct Object *) head->next; obj != (struct Object *) head; obj = (struct Object *) obj->header.next) {
            if (obj != sRemote && obj->activeFlags != ACTIVE_FLAG_DEACTIVATED && !(obj->oInteractStatus & INT_STATUS_INTERACTED) && obj->behavior != NULL
                && obj->unused2 == (u32) id && np_w_bhv(obj) == bhv) {
                return obj;
            }
        }
    }
    return NULL;
}

static struct Object *np_w_find(s32 bhv, const s16 *pos, f32 maxDist, int coinValue) {
    struct Object *best = NULL;
    f32 bestD = maxDist * maxDist;
    int i;
    for (i = 0; i < NUM_OBJ_LISTS; i++) {
        struct ObjectNode *head = &gObjectLists[i];
        struct Object *obj;
        for (obj = (struct Object *) head->next; obj != (struct Object *) head; obj = (struct Object *) obj->header.next) {
            f32 dx, dy, dz, d;
            if (obj == sRemote || obj->activeFlags == ACTIVE_FLAG_DEACTIVATED || (obj->oInteractStatus & INT_STATUS_INTERACTED)
                || obj->behavior == NULL || np_w_bhv(obj) != bhv || (coinValue != 0 && obj->oDamageOrCoinValue != coinValue)) {
                continue;
            }
            dx = obj->oPosX - pos[0];
            dy = obj->oPosY - pos[1];
            dz = obj->oPosZ - pos[2];
            d = dx * dx + dy * dy + dz * dz;
            if (d < bestD) {
                bestD = d;
                best = obj;
            }
        }
    }
    return best;
}

/* The local player picked o up (called right after the game's own interaction code handled it). */
void psp_np_world_collected(struct MarioState *m, struct Object *o, int type) {
    PspNpWorldEvent e;
    if (psp_netplay_state() != PSP_NP_CONNECTED || o == NULL || o->behavior == NULL || gCurrentArea == NULL) {
        return;
    }
    if (type != PSP_NP_EV_1UP && !(o->oInteractStatus & INT_STATUS_INTERACTED)) {
        return; /* the game did not take it (e.g. a star with too little health) */
    }
    memset(&e, 0, sizeof(e));
    e.type = (uint8_t) type;
    e.level = (uint8_t) gCurrLevelNum;
    e.area = (uint8_t) np_area_key();
    e.coinValue = (type == PSP_NP_EV_COIN) ? (uint8_t) o->oDamageOrCoinValue : 0;
    e.bhv = np_w_bhv(o);
    e.pos[0] = (s16) o->oPosX;
    e.pos[1] = (s16) o->oPosY;
    e.pos[2] = (s16) o->oPosZ;
    e.value = m->numCoins;
    if (type == PSP_NP_EV_COIN && o->unused2 != 0 && (o->behavior == segmented_to_virtual(bhvSingleCoinGetsSpawned) || o->behavior == segmented_to_virtual(bhvBlueCoinJumping)
                                                                    || o->behavior == segmented_to_virtual(bhvMovingYellowCoin))) {
        e.aux[1] = (s16) o->unused2; /* a coin dropped by an enemy: named by its number, since it has flown away from where it was announced */
    }
    if (type == PSP_NP_EV_COIN && o->behavior == segmented_to_virtual(bhvRedCoin) && o->parentObj != NULL) {
        e.aux[0] = (s16) (o->parentObj->oHiddenStarTriggerCounter + 1); /* the star's red coin counter once this coin counts (its loop has not run yet) */
    }
    np_w_log(&e);
    psp_np_world_send(&e);
}


#if PSP_NETPLAY_ENEMIES
/* ---- Enemies (stage 2): the host simulates, the client shows ----
 * The enemies placed by the level are the same in both consoles; one is told from the others by its kind and its home position (a level
 * object never moves from where it was placed). While both players are in the same area the host streams the state of the enemies that are
 * running (position, angles, action, health, scale, animation) and the client's copy stops running its own behavior and follows. What the
 * client's player does to a followed enemy is reported to the host (ATTACK: the interaction status the game set on it), which applies it
 * to the real one. When an enemy disappears (it died) on either side the other one removes its copy (DEAD) and the loot the host's enemy
 * drops is announced (SPAWN). If the stream stops (the host left, the enemy is far from it) the client's copy goes on by itself. */
typedef struct {
    uint32_t accel;
    uint8_t kind, action, subAction, health;
    int16_t home[3], pos[3], yaw, pitch, roll, moveYaw;
    int16_t anim, frame;
    uint16_t scale[3]; /* x1000 */
    int16_t fvel, yvel; /* forward and vertical speed x32: the other console moves the copy with them until the next update arrives */
    uint8_t opacity, pad; /* (boos fade in and out by where the player looks: the copy only shows what the owner computed) */
} PspNpObj;
_Static_assert(sizeof(PspNpObj) == 44, "PspNpObj must stay 44 bytes (10 per packet)");

#define NP_W_FOLLOW_FRAMES 45 /* a followed object that has heard nothing for this long goes back to its own behavior */
#define NP_W_BATCH ((int) (PSP_NP_OBJ_BYTES / sizeof(PspNpObj)))
#define NP_W_TRK 48
#define NP_W_HOME_TOL 3

static const BehaviorScript *const sKindBhv[] = { bhvGoomba, bhvPiranhaPlant, bhvScuttlebug, bhvSmallBully, bhvBigBully, bhvThwomp, bhvThwomp2, bhvSwoop, bhvSpindrift,
    /* added in patch 248 (appended: kind numbers must stay the same on both consoles) */
    bhvKoopa, bhvBobomb, bhvBulletBill, bhvFlyGuy, bhvSkeeter, bhvBub, bhvSnufit, bhvMrBlizzard, bhvMontyMole, bhvHomingAmp, bhvCirclingAmp, bhvMips, bhvMoneybag, bhvKlepto, bhvRacingPenguin, bhvSmallPenguin, bhvMrI, bhvGrindel, bhvHorizontalGrindel, bhvUnagi, bhvSpiny,
    /* patch 251: bosses */
    bhvKingBobomb, bhvWhompKingBoss, bhvFirePiranhaPlant,
    /* patch 260 */
    bhvHeaveHo, bhvBubba, bhvSushiShark, bhvEnemyLakitu, bhvSmallWhomp, bhvSLWalkingPenguin,
    bhvBoo, bhvMerryGoRoundBoo, bhvGhostHuntBoo, bhvGhostHuntBigBoo, bhvBalconyBigBoo, bhvMerryGoRoundBigBoo,
    /* patch 272 (single objects that sm64coopdx also syncs and whose state is all in the standard fields; the ones with parts of their own, such as the
     * chain chomp, wiggler, pokey, Bowser and Eyerok, or that grab the player, such as chuckya and hoot, are not here: their parts and grabs are not carried) */
    bhvMadPiano, bhvFlyingBookend, bhvMantaRay, bhvSmallChillBully, bhvBigChillBully, bhvTweester, bhvUkiki };
#define NP_W_KINDS ((int) (sizeof(sKindBhv) / sizeof(sKindBhv[0])))
static const void *sKindVirt[NP_W_KINDS];
static int sKindInit;
static u32 sClock = 1000; /* counts the frames of the world sync; a followed object is stamped with it (Object.unused1) */
static int sObjCursor;
static unsigned sDgIn, sDgMiss, sDgOwn, sDgAtk, sDgAtkMiss, sDgDeadWait, sDgSpawn, sDgSpawnPar, sDgCoinOut, sDgCoinIn; /* diagnostics of the world sync (see psp_np_world_diag) */
static struct Object *sAtkObj; /* the enemy the other player hit last, with the direction it was facing (psp_np_attack_yaw) */
static s16 sAtkYaw;
static u32 sAtkT;
static int sPlatN, sPlatCursor, sPlatTick; /* the moving platforms (see np_p_frame) */
static u32 sBowserHeard;    /* sClock when a record of Bowser last came in from the other console (any state: even its intro tells it is alive) */
static u32 sBowserPeerWait; /* ... and when that record said Bowser there waits for ours (action 20: the other player has just come into the arena) */
static u32 sBowserCarried;  /* sClock when the other console last said its player carries Bowser by the tail (this copy swings around the other player's model meanwhile) */
#define NP_P_BOWSER_HELD_SLOT (NP_P_STD + 7) /* the oHeldState extra of Bowser */
#define NP_P_BOWSER_CARRY_FRAMES 6
static struct {
    struct Object *o;
    u8 kind;
    s16 home[3];
} sTrk[NP_W_TRK];
static int sTrkN;

static int np_w_kind(const struct Object *o) {
    int i;
    if (!sKindInit) {
        for (i = 0; i < NP_W_KINDS; i++) {
            sKindVirt[i] = segmented_to_virtual(sKindBhv[i]);
        }
        sKindInit = 1;
    }
    for (i = 0; i < NP_W_KINDS; i++) {
        if (o->behavior == sKindVirt[i]) {
            return i + 1;
        }
    }
    return 0;
}

/* Crates and breakable walls: they do not move and have no home, so they are told apart by where they stand. When one is destroyed here the other console
 * destroys its own copy (the loot it drops is announced like the loot of an enemy). */
static const BehaviorScript *const sBrkBhv[] = { bhvBreakableBox, bhvWfBreakableWallLeft, bhvWfBreakableWallRight,
    /* patch 284: the mines of the Bowser arenas (they do not move either). When one goes off here the other console makes it go off too, the way it went (DEAD value:
     * 1 = a player ran into it, 2 = Bowser landed on it) */
    bhvBowserBomb };
#define NP_W_BRK_KINDS ((int) (sizeof(sBrkBhv) / sizeof(sBrkBhv[0])))
#define NP_W_BRK_BASE 200 /* DEAD events with e.bhv >= this are crates (e.bhv - this = the index in sBrkBhv) */
#define NP_W_BRK_MINE 4   /* (1-based index of bhvBowserBomb in sBrkBhv) */
static const void *sBrkVirt[NP_W_BRK_KINDS];
static int sBrkInit;

/* 1-based index in sBrkBhv, 0 = not one */
static int np_w_brk_kind(const struct Object *o) {
    int i;
    if (!sBrkInit) {
        for (i = 0; i < NP_W_BRK_KINDS; i++) {
            sBrkVirt[i] = segmented_to_virtual(sBrkBhv[i]);
        }
        sBrkInit = 1;
    }
    for (i = 0; i < NP_W_BRK_KINDS; i++) {
        if (o->behavior == sBrkVirt[i]) {
            return i + 1;
        }
    }
    return 0;
}

static int np_w_alive(const struct Object *o) {
    return o->activeFlags != ACTIVE_FLAG_DEACTIVATED && o != sRemote;
}

/* an enemy is told from the others by its home: one without a home (it never set it) cannot be matched */
static int np_w_homed(const struct Object *o) {
    return o->oHomeX != 0.0f || o->oHomeY != 0.0f || o->oHomeZ != 0.0f;
}

static struct Object *np_w_find_kind(int kind, const s16 *home) {
    int i;
    for (i = 0; i < NUM_OBJ_LISTS; i++) {
        struct ObjectNode *head = &gObjectLists[i];
        struct Object *obj;
        for (obj = (struct Object *) head->next; obj != (struct Object *) head; obj = (struct Object *) obj->header.next) {
            if (np_w_alive(obj) && obj->behavior != NULL && np_w_kind(obj) == kind && fabsf(obj->oHomeX - home[0]) < NP_W_HOME_TOL
                && fabsf(obj->oHomeY - home[1]) < NP_W_HOME_TOL && fabsf(obj->oHomeZ - home[2]) < NP_W_HOME_TOL) {
                return obj;
            }
        }
    }
    return NULL;
}

#define NP_W_ANIMS 16
#define NP_W_EXTRAP_FRAMES 4
#define NP_W_CLAIM_FRAMES 90 /* a console that picked an enemy up keeps driving it this long after letting go (the throw) */
static int np_w_anim_index(const struct Object *o) {
    const struct Animation *cur = o->header.gfx.unk38.curAnim;
    int i;
    if (cur == NULL || o->oAnimations == NULL) {
        return -1;
    }
    for (i = 0; i < NP_W_ANIMS; i++) { /* the table has no end marker: only its first entries are looked at */
        if (o->oAnimations[i] == cur) {
            return i;
        }
    }
    return -1;
}

/* this console picked the enemy up (or threw it a moment ago): it drives it, whoever drove it before */
static int np_w_claimed(const struct Object *o) {
    return (sClock - o->unused2) < NP_W_CLAIM_FRAMES;
}

/* ---- Who simulates an enemy (the model of sm64coopdx: the player that is nearest to it) ----
 * Both consoles apply the same rule to the same two distances, so they agree without talking: the host owns an enemy unless the client is nearer to it
 * by more than NP_W_OWN_MARGIN (the margin keeps it from changing hands back and forth when both are about as far, and absorbs the lag of the other
 * player's position). A player carrying an enemy always owns it, and says so in the stream (the high bit of the kind), whatever the distances are. If
 * the two consoles ever disagree for a moment, each one just keeps simulating its own copy and ignores the other's stream: nothing freezes. */
#define NP_W_OWN_MARGIN 200.0f
#define NP_W_HELD_FRAMES 30 /* how long a "the other console is carrying it" flag is believed after the last stream that had it */
#define NP_W_KIND_HELD 0x80
static struct { const struct Object *o; u32 t; } sRemHeld[8];
static int sRemHeldNext;

static void np_w_remote_holds_set(const struct Object *o) {
    int i;
    for (i = 0; i < 8; i++) {
        if (sRemHeld[i].o == o) {
            sRemHeld[i].t = sClock;
            return;
        }
    }
    sRemHeld[sRemHeldNext].o = o;
    sRemHeld[sRemHeldNext].t = sClock;
    sRemHeldNext = (sRemHeldNext + 1) & 7;
}

static int np_w_remote_holds(const struct Object *o) {
    int i;
    for (i = 0; i < 8; i++) {
        if (sRemHeld[i].o == o && (sClock - sRemHeld[i].t) < NP_W_HELD_FRAMES) {
            return 1;
        }
    }
    return 0;
}

static int np_w_rule_mine(const struct Object *o) {
    f32 dMe, dIt;
    if (!sWPeerHere || !np_remote_alive() || gMarioObject == NULL) {
        return 1; /* alone in this area: everything runs here */
    }
    dMe = dist_between_objects(o, gMarioObject);
    dIt = dist_between_objects(o, sRemote);
    return sWHost ? (dMe <= dIt + NP_W_OWN_MARGIN) : (dMe + NP_W_OWN_MARGIN < dIt);
}

/* the enemy is simulated here */
static int np_w_owned(const struct Object *o) {
    if (np_w_claimed(o)) {
        return 1;
    }
    if (o->activeFlags & ACTIVE_FLAG_INITIATED_TIME_STOP) {
        return 1; /* a dialog is running in THIS copy: it must stay here until it ends, or the player would stay frozen forever */
    }
    if (np_w_remote_holds(o)) {
        return 0;
    }
    return np_w_rule_mine(o);
}

int psp_np_world_follow(struct Object *o) {
    return (sClock - o->unused1) < NP_W_FOLLOW_FRAMES && np_w_kind(o) != 0 && !np_w_owned(o);
}

/* The client's copy of a followed enemy: instead of its behavior, it reports what the interactions of the game did to it. */
void psp_np_world_follow_step(struct Object *o) {
    const u32 status = (u32) o->oInteractStatus;
    if (sClock - o->unused1 >= 1 && sClock - o->unused1 <= NP_W_EXTRAP_FRAMES) { /* between two updates the copy keeps moving the way it was */
        o->oPosX += o->oForwardVel * sins(o->oMoveAngleYaw);
        o->oPosZ += o->oForwardVel * coss(o->oMoveAngleYaw);
        o->oPosY += o->oVelY;
    }
    static u32 sAtkLast;
    if ((status & (INT_STATUS_WAS_ATTACKED | INT_STATUS_ATTACKED_MARIO)) == 0) {
        o->oInteractStatus = 0; /* only a hit matters to the real enemy, either way: it was hit, or it hurt the player here (so it enters its "attacked Mario" action) */
    } else if (sClock - sAtkLast >= 3) { /* (a hit that cannot be sent yet stays set and is sent a frame later, instead of flooding the queue) */
        PspNpWorldEvent e;
        memset(&e, 0, sizeof(e));
        e.type = PSP_NP_EV_ATTACK;
        e.level = (uint8_t) gCurrLevelNum;
        e.area = (uint8_t) np_area_key();
        e.coinValue = (uint8_t) np_w_kind(o);
        e.bhv = (s32) status;
        e.pos[0] = (s16) o->oHomeX;
        e.pos[1] = (s16) o->oHomeY;
        e.pos[2] = (s16) o->oHomeZ;
        e.aux[0] = (s16) gMarioObject->header.gfx.angle[1]; /* where the attacker was facing (a kicked bob-omb flies that way) */
        if (psp_np_world_send(&e)) {
            sAtkLast = sClock;
            o->oInteractStatus = 0;
        }
    }
}

/* The loot of an enemy (coins that jump out of it) is announced so the other console has it too. The coin behaviors are bhvSingleCoinGetsSpawned
 * (yellow) and bhvBlueCoinJumping (blue). Their direction and speed are chosen at random in their first update, so the announcement waits one
 * frame and carries what this console rolled; the other console makes the coin and, after its own first update, puts that on it. */
#define NP_W_LOOT 24
#define NP_LOOT_MOVING 20 /* coinValue of a SPAWN event for a coin of bhvMovingYellowCoin: the ones a bob-omb, a money bag, a bully or a crate throws out (obj_spawn_yellow_coins) */
static struct { struct Object *o; u8 type; u8 age; s16 home[3]; } sLootOut[NP_W_LOOT];
static int sLootOutN;
static struct { struct Object *o; s16 vy, fv, yaw; } sLootFix[NP_W_LOOT];
static int sLootFixN;
#define NP_W_REDFIX 8
static struct { struct Object *star; s16 target; u8 age; } sRedFix[NP_W_REDFIX];
static int sRedFixN;

/* What an enemy that this console simulates shoots or drops, other than coins: the type of the SPAWN event is 3 + the index here. */
static const BehaviorScript *const sMirBhv[] = { bhvSnufitBalls, bhvMrIParticle, bhvKoopaShell, bhvExplosion };
static const BehaviorScript *const sMirParent[] = { bhvSnufit, bhvMrI, bhvKoopa, bhvBobomb };
static const u32 sMirModel[] = { MODEL_BOWLING_BALL, MODEL_PURPLE_MARBLE, MODEL_KOOPA_SHELL, MODEL_EXPLOSION };
#define NP_W_MIR ((int) (sizeof(sMirBhv) / sizeof(sMirBhv[0])))

void psp_np_world_spawned(struct Object *parent, struct Object *obj, int model, const void *behavior) {
    int type;
    (void) model;
    if (parent != NULL && behavior == bhvWaterBomb && parent->behavior == segmented_to_virtual(bhvWaterBombSpawner) && psp_netplay_state() == PSP_NP_CONNECTED
        && sLootOutN < NP_W_LOOT) { /* a bomb that falls on this player: the other one sees it fall too */
        sLootOut[sLootOutN].o = obj;
        sLootOut[sLootOutN].type = 2;
        sLootOut[sLootOutN].age = 0;
        sLootOutN++;
        return;
    }
    if (parent == NULL || obj == NULL || parent == obj || psp_netplay_state() != PSP_NP_CONNECTED || (np_w_kind(parent) == 0 && np_w_brk_kind(parent) == 0) || psp_np_world_follow(parent)) {
        return;
    }
    if (behavior == bhvSingleCoinGetsSpawned) {
        type = 0;
    } else if (behavior == bhvBlueCoinJumping) {
        type = 1;
    } else if (behavior == bhvMovingYellowCoin) {
        type = NP_LOOT_MOVING;
    } else {
        int m;
        for (m = 0; m < NP_W_MIR; m++) { /* what an enemy shoots or drops: the other console makes the same one (sm64coopdx sends every such spawn) */
            if (behavior == sMirBhv[m] && parent->behavior == segmented_to_virtual(sMirParent[m])) {
                break;
            }
        }
        if (m == NP_W_MIR) {
            return;
        }
        type = 3 + m;
    }
    if (sLootOutN < NP_W_LOOT) {
        sLootOut[sLootOutN].o = obj;
        sLootOut[sLootOutN].type = (u8) type;
        sLootOut[sLootOutN].age = 0;
        sLootOut[sLootOutN].home[0] = (s16) parent->oHomeX;
        sLootOut[sLootOutN].home[1] = (s16) parent->oHomeY;
        sLootOut[sLootOutN].home[2] = (s16) parent->oHomeZ;
        sLootOutN++;
    }
}

/* once per frame: sends the loot that has had its first update, and puts the announced speed on the coins made from the other console's */
static void np_w_loot_step(void) {
    int i;
    for (i = 0; i < sLootOutN;) {
        if (++sLootOut[i].age < 2) {
            i++;
            continue;
        }
        if (sLootOut[i].o->activeFlags != ACTIVE_FLAG_DEACTIVATED) {
            PspNpWorldEvent e;
            const struct Object *c = sLootOut[i].o;
            memset(&e, 0, sizeof(e));
            e.type = PSP_NP_EV_SPAWN;
            e.level = (uint8_t) gCurrLevelNum;
            e.area = (uint8_t) np_area_key();
            e.coinValue = sLootOut[i].type;
            e.pos[0] = (s16) c->oPosX; e.pos[1] = (s16) c->oPosY; e.pos[2] = (s16) c->oPosZ;
            e.aux[0] = (s16) (c->oVelY * 32.0f);
            e.aux[1] = (s16) (c->oForwardVel * 32.0f);
            e.aux[2] = (s16) c->oMoveAngleYaw;
            if (e.coinValue == 2) { /* water bomb: where it is and what it is doing */
                e.value = (s16) c->oAction;
            } else if (e.coinValue >= 3 && e.coinValue != NP_LOOT_MOVING) { /* shot or dropped by an enemy: the parent is named by its home, so the other console can find its own copy */
                e.value = sLootOut[i].home[1];
                e.bhv = (s32) (((u32) (uint16_t) sLootOut[i].home[0]) | (((u32) (uint16_t) sLootOut[i].home[2]) << 16));
            } else {
                e.value = np_w_loot_new_id();
                sLootOut[i].o->unused2 = (u32) e.value;
            }
            if (e.coinValue <= 1 || e.coinValue == NP_LOOT_MOVING) {
                sDgCoinOut++;
            }
            psp_np_world_send(&e);
        }
        sLootOut[i] = sLootOut[--sLootOutN];
    }
    for (i = 0; i < sLootFixN; i++) {
        struct Object *c = sLootFix[i].o;
        if (c->activeFlags != ACTIVE_FLAG_DEACTIVATED) {
            c->oVelY = (f32) sLootFix[i].vy / 32.0f;
            c->oForwardVel = (f32) sLootFix[i].fv / 32.0f;
            c->oMoveAngleYaw = sLootFix[i].yaw;
        }
    }
    sLootFixN = 0;
    for (i = 0; i < sRedFixN;) { /* the red coin star's counter never stays behind the one of the console that collected the coin */
        if (++sRedFix[i].age < 3) {
            i++;
            continue;
        }
        if (sRedFix[i].star->activeFlags != ACTIVE_FLAG_DEACTIVATED && sRedFix[i].star->oHiddenStarTriggerCounter < sRedFix[i].target) {
            sRedFix[i].star->oHiddenStarTriggerCounter = sRedFix[i].target;
        }
        sRedFix[i] = sRedFix[--sRedFixN];
    }
}

static void np_w_spawn_loot(const PspNpWorldEvent *e) {
    struct Object *c;
    if (e->coinValue == 2) { /* water bomb of the other player's spawner (its parent is the other player's model: the bomb writes into its parent when it explodes) */
        if (np_remote_alive()) {
            c = spawn_object(sRemote, MODEL_WATER_BOMB, bhvWaterBomb);
            if (c != NULL) {
                c->oPosX = e->pos[0];
                c->oPosY = e->pos[1];
                c->oPosZ = e->pos[2];
                c->oAction = e->value;
                c->oVelY = (f32) e->aux[0] / 32.0f;
                spawn_object(c, MODEL_WATER_BOMB_SHADOW, bhvWaterBombShadow);
            }
        }
        return;
    }
    if (e->coinValue >= 3 && e->coinValue < 3 + NP_W_MIR && gMarioObject != NULL) {
        const int m = e->coinValue - 3;
        s16 home[3];
        struct Object *par = NULL;
        int k, pk = 0;
        home[0] = (s16) (uint16_t) ((u32) e->bhv & 0xFFFF);
        home[1] = e->value;
        home[2] = (s16) (uint16_t) (((u32) e->bhv) >> 16);
        (void) np_w_kind(gMarioObject); /* (makes sure the kind table is ready) */
        for (k = 0; k < NP_W_KINDS; k++) {
            if (sKindVirt[k] == segmented_to_virtual(sMirParent[m])) {
                pk = k + 1;
            }
        }
        if (pk != 0) {
            par = np_w_find_kind(pk, home);
        }
        sDgSpawn++;
        if (par == NULL) {
            sDgSpawnPar++;
        }
        c = spawn_object(par != NULL ? par : gMarioObject, sMirModel[m], sMirBhv[m]);
        if (c != NULL) {
            if (sMirBhv[m] == bhvExplosion) {
                c->oGraphYOffset += 100.0f; /* (like bobomb_act_explode) */
            }
            c->oPosX = e->pos[0];
            c->oPosY = e->pos[1];
            c->oPosZ = e->pos[2];
            if (sLootFixN < NP_W_LOOT) { /* (its own first update sets a start; the announced one goes on after it) */
                sLootFix[sLootFixN].o = c;
                sLootFix[sLootFixN].vy = e->aux[0];
                sLootFix[sLootFixN].fv = e->aux[1];
                sLootFix[sLootFixN].yaw = e->aux[2];
                sLootFixN++;
            }
        }
        return;
    }
    if ((e->coinValue > 2 && e->coinValue != NP_LOOT_MOVING) || gMarioObject == NULL) {
        return;
    }
    sDgCoinIn++;
    c = spawn_object(gMarioObject, e->coinValue == 1 ? MODEL_BLUE_COIN : MODEL_YELLOW_COIN,
                     e->coinValue == 0 ? bhvSingleCoinGetsSpawned : (e->coinValue == 1 ? bhvBlueCoinJumping : bhvMovingYellowCoin));
    if (c != NULL) {
        c->oPosX = e->pos[0];
        c->oPosY = e->pos[1];
        c->oPosZ = e->pos[2];
        c->unused2 = (u32) (uint16_t) e->value;
        if (e->coinValue == NP_LOOT_MOVING) { /* (these coins roll nothing of their own: the sender's speed is all they have) */
            c->oVelY = (f32) e->aux[0] / 32.0f;
            c->oForwardVel = (f32) e->aux[1] / 32.0f;
            c->oMoveAngleYaw = e->aux[2];
        }
        if (sLootFixN < NP_W_LOOT) { /* (its first update picks its own random speed: the announced one goes on after it) */
            sLootFix[sLootFixN].o = c;
            sLootFix[sLootFixN].vy = e->aux[0];
            sLootFix[sLootFixN].fv = e->aux[1];
            sLootFix[sLootFixN].yaw = e->aux[2];
            sLootFixN++;
        }
    }
}

/* Client: one enemy state from the host. */
static void np_w_follow_apply(const PspNpObj *e) {
    const int kind = e->kind & ~NP_W_KIND_HELD;
    struct Object *o = (kind >= 1 && kind <= NP_W_KINDS) ? np_w_find_kind(kind, e->home) : NULL;
    sDgIn++;
    if (o == NULL) {
        sDgMiss++; /* the other console sent an enemy that is not found here: its kind or its home differ */
        return;
    }
    if (e->kind & NP_W_KIND_HELD) {
        np_w_remote_holds_set(o); /* the other console is carrying it: it drives it, wherever the two players are */
    }
    if (o->oHeldState != HELD_FREE || np_w_owned(o)) {
        sDgOwn++;
        return; /* this console drives it (or is the one that should): the other one's stream is not applied */
    }
    o->unused1 = sClock;
    o->oForwardVel = (f32) e->fvel / 32.0f; /* (the script does not run here: these fields are free for the extrapolation in psp_np_world_follow_step) */
    o->oVelY = (f32) e->yvel / 32.0f;
    o->oPosX = e->pos[0];
    o->oPosY = e->pos[1];
    o->oPosZ = e->pos[2];
    o->oFaceAngleYaw = e->yaw;
    o->oFaceAnglePitch = e->pitch;
    o->oFaceAngleRoll = e->roll;
    o->oMoveAngleYaw = e->moveYaw;
    if (o->oAction != e->action) {
        o->oAction = o->oPrevAction = e->action;
        o->oTimer = 0;
    }
    o->oSubAction = e->subAction;
    o->oHealth = e->health;
    o->oOpacity = e->opacity;
    o->oIntangibleTimer = 0;
    o->header.gfx.scale[0] = (f32) e->scale[0] / 1000.0f;
    o->header.gfx.scale[1] = (f32) e->scale[1] / 1000.0f;
    o->header.gfx.scale[2] = (f32) e->scale[2] / 1000.0f;
    if (e->anim >= 0 && e->anim < NP_W_ANIMS && o->oAnimations != NULL) {
        geo_obj_init_animation(&o->header.gfx, &o->oAnimations[e->anim]);
        o->header.gfx.unk38.animFrame = e->frame;
        o->header.gfx.unk38.animAccel = e->accel;
    }
}

/* Host: sends the state of the enemies that are running, a few per frame in turns. An enemy that has not changed since its last update is only
 * repeated now and then (often enough for the other console to keep following it), so the radio is used for what moves. */
#define NP_W_SENT 48
#define NP_W_REFRESH 20 /* frames between the repeats of an enemy that stands still (the other console follows one for NP_W_FOLLOW_FRAMES) */
static struct { struct Object *o; u32 sig; u32 t; } sSent[NP_W_SENT];
static int sSentNext;

static u32 np_w_sig(const struct Object *o) {
    u32 h = 2166136261u;
    const s32 v[] = { (s32) o->oPosX, (s32) o->oPosY, (s32) o->oPosZ, (s32) o->oFaceAngleYaw, (s32) o->oAction, (s32) o->oSubAction, (s32) o->oHealth,
                      np_w_anim_index(o), (s32) o->oOpacity, (s32) (o->header.gfx.scale[0] * 100.0f), (s32) (o->header.gfx.scale[1] * 100.0f), (s32) (o->header.gfx.scale[2] * 100.0f) };
    int i;
    for (i = 0; i < (int) (sizeof(v) / sizeof(v[0])); i++) {
        h = (h ^ (u32) v[i]) * 16777619u;
    }
    return h;
}

/* An enemy that is hidden here (far from this console's Mario) still runs its script, and with the retarget it chases the other player. If that player is
 * the nearer one the other console is looking at it, so it must keep being sent: the flag only says it is out of THIS Mario's draw distance. */
static int np_w_near_remote(const struct Object *o) {
    return np_remote_alive() && dist_between_objects(o, sRemote) < o->oDrawingDistance && dist_between_objects(o, sRemote) < dist_between_objects(o, gMarioObject);
}

static void np_w_enemies_send(void) {
    PspNpObj batch[NP_W_BATCH];
    struct Object *cand[40];
    u32 sigs[40];
    int nc = 0, i, j, n = 0;
    const int cap = (NP_W_BATCH >> psp_np_congestion()) < 3 ? 3 : (NP_W_BATCH >> psp_np_congestion()); /* smaller batches on a slow link */
    for (i = 0; i < NUM_OBJ_LISTS && nc < 40; i++) {
        struct ObjectNode *head = &gObjectLists[i];
        struct Object *obj;
        for (obj = (struct Object *) head->next; obj != (struct Object *) head && nc < 40; obj = (struct Object *) obj->header.next) {
            if (np_w_alive(obj) && obj->behavior != NULL && (((obj->activeFlags & ACTIVE_FLAG_FAR_AWAY) == 0) || np_w_near_remote(obj)) && np_w_kind(obj) != 0 && np_w_homed(obj) && np_w_owned(obj)) {
                const u32 sig = np_w_sig(obj);
                for (j = 0; j < NP_W_SENT && sSent[j].o != obj; j++) {
                }
                if (j == NP_W_SENT || sSent[j].sig != sig || sClock - sSent[j].t >= NP_W_REFRESH) { /* changed, never sent, or due for its repeat */
                    cand[nc] = obj;
                    sigs[nc++] = sig;
                }
            }
        }
    }
    if (nc == 0) {
        return;
    }
    for (i = 0; i < nc && n < cap; i++) {
        const int ci = (sObjCursor + i) % nc;
        struct Object *o = cand[ci];
        PspNpObj *e = &batch[n++];
        e->kind = (uint8_t) (np_w_kind(o) | (np_w_claimed(o) ? NP_W_KIND_HELD : 0));
        e->action = (uint8_t) o->oAction;
        e->subAction = (uint8_t) o->oSubAction;
        e->health = (uint8_t) o->oHealth;
        e->home[0] = (s16) o->oHomeX; e->home[1] = (s16) o->oHomeY; e->home[2] = (s16) o->oHomeZ;
        e->pos[0] = (s16) o->oPosX; e->pos[1] = (s16) o->oPosY; e->pos[2] = (s16) o->oPosZ;
        e->yaw = (s16) o->oFaceAngleYaw; e->pitch = (s16) o->oFaceAnglePitch; e->roll = (s16) o->oFaceAngleRoll;
        e->moveYaw = (s16) o->oMoveAngleYaw;
        e->anim = (int16_t) np_w_anim_index(o);
        e->frame = o->header.gfx.unk38.animFrame;
        e->accel = o->header.gfx.unk38.animAccel;
        e->scale[0] = (uint16_t) (o->header.gfx.scale[0] * 1000.0f); /* (the three axes: a squashed goomba is wide and flat) */
        e->scale[1] = (uint16_t) (o->header.gfx.scale[1] * 1000.0f);
        e->scale[2] = (uint16_t) (o->header.gfx.scale[2] * 1000.0f);
        e->opacity = (uint8_t) o->oOpacity;
        e->pad = 0;
        e->fvel = (int16_t) (o->oForwardVel * 32.0f);
        e->yvel = (int16_t) (o->oVelY * 32.0f);
        for (j = 0; j < NP_W_SENT && sSent[j].o != o; j++) {
        }
        if (j == NP_W_SENT) {
            j = sSentNext;
            sSentNext = (sSentNext + 1) % NP_W_SENT;
        }
        sSent[j].o = o;
        sSent[j].sig = sigs[ci];
        sSent[j].t = sClock;
    }
    sObjCursor = (sObjCursor + n) % nc;
    psp_np_world_obj_send(batch, (int) (n * sizeof(PspNpObj)));
}

/* Both: finds the enemies that are gone since last frame and tells the other console. */
#define NP_W_BRK 48
static struct { struct Object *o; u8 kind; s16 pos[3]; } sBrk[NP_W_BRK];
static int sBrkN;

static struct Object *np_w_find_brk(int kind, const s16 *pos) {
    int i;
    for (i = 0; i < NUM_OBJ_LISTS; i++) {
        struct ObjectNode *head = &gObjectLists[i];
        struct Object *obj;
        for (obj = (struct Object *) head->next; obj != (struct Object *) head; obj = (struct Object *) obj->header.next) {
            if (np_w_alive(obj) && obj->behavior != NULL && np_w_brk_kind(obj) == kind && fabsf(obj->oPosX - pos[0]) < NP_W_HOME_TOL
                && fabsf(obj->oPosY - pos[1]) < NP_W_HOME_TOL && fabsf(obj->oPosZ - pos[2]) < NP_W_HOME_TOL) {
                return obj;
            }
        }
    }
    return NULL;
}

/* once per frame: a crate that was there and is gone now was destroyed (a whole area going away is not: the list is dropped on every area change) */
static void np_w_brk_track(void) {
    struct { struct Object *o; u8 kind; s16 pos[3]; } cur[NP_W_BRK];
    struct ObjectNode *head;
    struct Object *obj;
    int nc = 0, i, j;
    for (i = 0; i < NUM_OBJ_LISTS; i++) {
        head = &gObjectLists[i];
        for (obj = (struct Object *) head->next; obj != (struct Object *) head; obj = (struct Object *) obj->header.next) {
            int k;
            if (nc < NP_W_BRK && np_w_alive(obj) && obj->behavior != NULL && (k = np_w_brk_kind(obj)) != 0) {
                cur[nc].o = obj;
                cur[nc].kind = (u8) k;
                cur[nc].pos[0] = (s16) obj->oPosX; cur[nc].pos[1] = (s16) obj->oPosY; cur[nc].pos[2] = (s16) obj->oPosZ;
                nc++;
            }
        }
    }
    for (i = 0; i < sBrkN; i++) {
        int still = 0;
        for (j = 0; j < nc && !still; j++) {
            still = (cur[j].o == sBrk[i].o);
        }
        if (!still && sBrk[i].o->activeFlags == ACTIVE_FLAG_DEACTIVATED) {
            PspNpWorldEvent e;
            memset(&e, 0, sizeof(e));
            e.type = PSP_NP_EV_DEAD;
            e.level = (uint8_t) gCurrLevelNum;
            e.area = (uint8_t) np_area_key();
            e.bhv = NP_W_BRK_BASE + sBrk[i].kind;
            if (sBrk[i].kind == NP_W_BRK_MINE) {
                e.value = (sBrk[i].o->oInteractStatus & INT_STATUS_HIT_MINE) ? 2 : 1;
            }
            e.pos[0] = sBrk[i].pos[0]; e.pos[1] = sBrk[i].pos[1]; e.pos[2] = sBrk[i].pos[2];
            np_w_log(&e);
            psp_np_world_send(&e);
        }
    }
    for (i = 0; i < nc; i++) {
        sBrk[i].o = cur[i].o;
        sBrk[i].kind = cur[i].kind;
        memcpy(sBrk[i].pos, cur[i].pos, sizeof(sBrk[i].pos));
    }
    sBrkN = nc;
}

static void np_w_enemies_track(void) {
    struct { struct Object *o; u8 kind; s16 home[3]; } cur[NP_W_TRK];
    int nc = 0, i, j, gone = 0, reload = 0;
    for (i = 0; i < NUM_OBJ_LISTS; i++) {
        struct ObjectNode *head = &gObjectLists[i];
        struct Object *obj;
        for (obj = (struct Object *) head->next; obj != (struct Object *) head; obj = (struct Object *) obj->header.next) {
            if (nc < NP_W_TRK && np_w_alive(obj) && obj->behavior != NULL && np_w_kind(obj) != 0 && np_w_homed(obj)) {
                if (obj->oHeldState != HELD_FREE) { /* Mario here is carrying it: from now on this console drives it */
                    obj->unused2 = sClock;
                    obj->unused1 = 0;
                }
                cur[nc].o = obj;
                cur[nc].kind = (u8) np_w_kind(obj);
                cur[nc].home[0] = (s16) obj->oHomeX; cur[nc].home[1] = (s16) obj->oHomeY; cur[nc].home[2] = (s16) obj->oHomeZ;
                nc++;
            }
        }
    }
    for (i = 0; i < sTrkN; i++) { /* (how many are gone: when most of them are, the area was reloaded, which is not a death) */
        int still = 0;
        if (i == 0) {
            for (j = 0, gone = 0; j < sTrkN; j++) {
                int k, found = 0;
                for (k = 0; k < nc && !found; k++) {
                    found = (cur[k].o == sTrk[j].o && cur[k].kind == sTrk[j].kind);
                }
                gone += !found;
            }
            reload = (gone >= 3 && gone * 2 >= sTrkN);
        }
        for (j = 0; j < nc && !still; j++) {
            still = (cur[j].o == sTrk[i].o && cur[j].kind == sTrk[i].kind);
        }
        if (!still && !reload && sTrk[i].o->activeFlags == ACTIVE_FLAG_DEACTIVATED) { /* it was removed: it died */
            PspNpWorldEvent e;
            memset(&e, 0, sizeof(e));
            e.type = PSP_NP_EV_DEAD;
            e.level = (uint8_t) gCurrLevelNum;
            e.area = (uint8_t) np_area_key();
            e.bhv = sTrk[i].kind;
            e.pos[0] = sTrk[i].home[0]; e.pos[1] = sTrk[i].home[1]; e.pos[2] = sTrk[i].home[2];
            np_w_log(&e);
            psp_np_world_send(&e);
        }
    }
    for (i = 0; i < nc; i++) {
        sTrk[i].o = cur[i].o;
        sTrk[i].kind = cur[i].kind;
        memcpy(sTrk[i].home, cur[i].home, sizeof(sTrk[i].home));
    }
    sTrkN = nc;
}
#endif /* PSP_NETPLAY_ENEMIES */


#if PSP_NETPLAY_TRIGGERS
/* ---- Triggers (stage 3): doors, purple switches, the water level diamonds ----
 * They are not streamed: every console runs them. When one of them leaves its idle action here (a player opened the door, stepped on the
 * switch) the change is announced, and the other console puts its own copy in that action, so the animation, the sounds and what the
 * switch controls happen there too, like the door events of sm64coopdx. Only the start of the action is sent (the rest of it is the
 * object's own business) and it is only taken by a copy that is idle. The objects are matched by behavior and position. */
static const BehaviorScript *const sTrgBhv[] = { bhvDoor, bhvDoorWarp, bhvFloorSwitchHardcodedModel, bhvFloorSwitchGrills,
                                                 bhvFloorSwitchAnimatesObject, bhvFloorSwitchHiddenObjects, bhvWaterLevelDiamond,
                                                 bhvCapSwitch, bhvPurpleSwitchHiddenBoxes, /* these two: patch 250 */
                                                 bhvBlueCoinSwitch, bhvCannonClosed, bhvExclamationBox, /* patch 260 */
                                                 bhvStarDoor, bhvThiTinyIslandTop }; /* patch 284: the star door (each half is announced; the other half follows the nearest one by itself)
                                                                                      * and the top of the tiny island of Tiny-Huge Island (the ground pound that drains the water) */
static const u8 sTrgIdle[] = { 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 2, 0, 0 }; /* the action each one rests in */
#define NP_W_TRG ((int) (sizeof(sTrgBhv) / sizeof(sTrgBhv[0])))
#define NP_W_TRG_WATER 7 /* kind (1-based) of the water level diamond: its effect lasts, so it is kept in the log for whoever comes in later */
#define NP_W_TRG_THI 14  /* (1-based) the tiny island top of THI: the water it drains stays drained, so it is kept in the log too */
#define NP_W_TRGS 48
static const void *sTrgVirt[NP_W_TRG];
static int sTrgInit;
static struct {
    struct Object *o;
    s16 last;
} sTrgSeen[NP_W_TRGS];
static int sTrgSeenN;

static int np_w_trg_kind(const struct Object *o) {
    int i;
    if (!sTrgInit) {
        for (i = 0; i < NP_W_TRG; i++) {
            sTrgVirt[i] = segmented_to_virtual(sTrgBhv[i]);
        }
        sTrgInit = 1;
    }
    for (i = 0; i < NP_W_TRG; i++) {
        if (o->behavior == sTrgVirt[i]) {
            return i + 1;
        }
    }
    return 0;
}

static struct Object *np_w_trg_find(int kind, const s16 *pos) {
    int i;
    for (i = 0; i < NUM_OBJ_LISTS; i++) {
        struct ObjectNode *head = &gObjectLists[i];
        struct Object *obj;
        for (obj = (struct Object *) head->next; obj != (struct Object *) head; obj = (struct Object *) obj->header.next) {
            if (np_w_alive(obj) && obj->behavior != NULL && np_w_trg_kind(obj) == kind && fabsf(obj->oPosX - pos[0]) < 60.0f
                && fabsf(obj->oPosZ - pos[2]) < 60.0f && fabsf(obj->oPosY - pos[1]) < 400.0f) {
                return obj;
            }
        }
    }
    return NULL;
}

/* The other console set it off: put the copy in the same action (only if it is idle). */
static int np_w_trg_apply(const PspNpWorldEvent *e) {
    struct Object *o;
    int i;
    if (e->coinValue < 1 || e->coinValue > NP_W_TRG || e->bhv < 0 || e->bhv > 31) {
        return 1; /* (not one of ours: an action number the objects never use) */
    }
    o = np_w_trg_find(e->coinValue, e->pos);
    if (o == NULL) {
        return 0;
    }
    if (o->oAction == sTrgIdle[e->coinValue - 1]) {
        o->oAction = e->bhv;
        if (e->coinValue == NP_W_TRG_WATER) {
            gWDWWaterLevelChanging = 1;
        }
        for (i = 0; i < sTrgSeenN; i++) { /* (not reported back as if it had happened here) */
            if (sTrgSeen[i].o == o) {
                sTrgSeen[i].last = (s16) o->oAction;
            }
        }
    }
    return 1;
}

/* Both: reports the triggers that just left their idle action. */
static void np_w_triggers_track(void) {
    int i;
    for (i = 0; i < NUM_OBJ_LISTS; i++) {
        struct ObjectNode *head = &gObjectLists[i];
        struct Object *obj;
        for (obj = (struct Object *) head->next; obj != (struct Object *) head; obj = (struct Object *) obj->header.next) {
            int kind, j;
            if (!np_w_alive(obj) || obj->behavior == NULL || (kind = np_w_trg_kind(obj)) == 0) {
                continue;
            }
            for (j = 0; j < sTrgSeenN && sTrgSeen[j].o != obj; j++) {
            }
            if (j == sTrgSeenN) {
                if (sTrgSeenN >= NP_W_TRGS) {
                    continue;
                }
                sTrgSeen[sTrgSeenN].o = obj;
                sTrgSeen[sTrgSeenN].last = (s16) obj->oAction;
                sTrgSeenN++;
                continue;
            }
            if (sTrgSeen[j].last != (s16) obj->oAction) {
                if (sTrgSeen[j].last == sTrgIdle[kind - 1] && obj->oAction != sTrgIdle[kind - 1]) {
                    PspNpWorldEvent e;
                    memset(&e, 0, sizeof(e));
                    e.type = PSP_NP_EV_ACTION;
                    e.level = (uint8_t) gCurrLevelNum;
                    e.area = (uint8_t) np_area_key();
                    e.coinValue = (uint8_t) kind;
                    e.bhv = obj->oAction;
                    e.pos[0] = (s16) obj->oPosX;
                    e.pos[1] = (s16) obj->oPosY;
                    e.pos[2] = (s16) obj->oPosZ;
                    if (kind == NP_W_TRG_WATER || kind == NP_W_TRG_THI) {
                        np_w_log(&e);
                    }
                    psp_np_world_send(&e);
                }
                sTrgSeen[j].last = (s16) obj->oAction;
            }
        }
    }
}
#endif /* PSP_NETPLAY_TRIGGERS */

/* Applies an event to the object it names. Returns 0 if that object is not there (yet). */
/* A boss has died on the other console and its star comes out: here the boss copy did not run its own death, so the star is made from the announcement. */
static int np_p_kind(const struct Object *o);
static int np_p_nearest_driven(const struct Object *o); /* (the table of platforms and state objects further down) */
void psp_np_world_star(struct Object *parent, struct Object *star, int type) {
    PspNpWorldEvent e;
    if (parent == NULL || star == NULL || psp_netplay_state() != PSP_NP_CONNECTED || (np_w_kind(parent) == 0 && !np_p_nearest_driven(parent)) || psp_np_world_follow(parent)) {
        return;
    }
    memset(&e, 0, sizeof(e));
    e.type = PSP_NP_EV_STARSPAWN;
    e.level = (uint8_t) gCurrLevelNum;
    e.area = (uint8_t) np_area_key();
    e.coinValue = (uint8_t) type;
    e.bhv = (s32) star->oBehParams;
    e.pos[0] = (s16) star->oPosX; e.pos[1] = (s16) star->oPosY; e.pos[2] = (s16) star->oPosZ;
    e.aux[0] = (s16) star->oHomeX; e.aux[1] = (s16) star->oHomeY; e.aux[2] = (s16) star->oHomeZ;
    psp_np_world_send(&e);
}

static void np_w_spawn_star(const PspNpWorldEvent *e) {
    struct Object *s;
    int i;
    if (gMarioObject == NULL) {
        return;
    }
    for (i = 0; i < NUM_OBJ_LISTS; i++) { /* already here (it came through another way): not made twice */
        struct ObjectNode *head = &gObjectLists[i];
        struct Object *obj;
        for (obj = (struct Object *) head->next; obj != (struct Object *) head; obj = (struct Object *) obj->header.next) {
            if (np_w_alive(obj) && obj->behavior == segmented_to_virtual(bhvStarSpawnCoordinates) && fabsf(obj->oHomeX - e->aux[0]) < 5.0f
                && fabsf(obj->oHomeY - e->aux[1]) < 5.0f && fabsf(obj->oHomeZ - e->aux[2]) < 5.0f) {
                return;
            }
        }
    }
    s = spawn_object_abs_with_rot(gMarioObject, 0, MODEL_STAR, bhvStarSpawnCoordinates, e->pos[0], e->pos[1], e->pos[2], 0, 0, 0);
    if (s == NULL) {
        return;
    }
    s->oBehParams = e->bhv;
    s->oHomeX = e->aux[0];
    s->oHomeY = e->aux[1];
    s->oHomeZ = e->aux[2];
    s->oFaceAnglePitch = 0;
    s->oFaceAngleRoll = 0;
    s->oBehParams2ndByte = (e->coinValue == 0) ? 0 : 1;
    if (e->coinValue == 2) {
        s->oInteractionSubtype |= INT_SUBTYPE_NO_EXIT;
    }
}

/* An enemy this console simulates aims at the nearest player: the other player's Mario is not gMarioObject here. */
/* Spawners that load their enemies only while a player is near (they look at the local Mario alone otherwise, so an enemy next to the other player would
 * not exist on this console, or would be unloaded again at once). */
static int np_w_is_spawner(const struct Object *o) {
    static const void *virt;
    if (virt == NULL) {
        virt = segmented_to_virtual(bhvGoombaTripletSpawner);
    }
    return o->behavior == virt;
}

short psp_np_attack_yaw(const struct Object *o, short def) {
    return (o == sAtkObj && sClock - sAtkT <= 3) ? sAtkYaw : def;
}

int psp_np_world_dialog_ok(const struct Object *o) {
    if (!sWPeerHere || !np_remote_alive() || gMarioObject == NULL || np_w_kind(o) == 0) {
        return 1;
    }
    return dist_between_objects(o, gMarioObject) <= dist_between_objects(o, sRemote) + NP_W_OWN_MARGIN;
}

int psp_np_world_remote_within(float x, float y, float z, float dist) {
    float dx, dy, dz;
    if (!sWPeerHere || !np_remote_alive()) {
        return 0;
    }
    dx = sRemote->oPosX - x; dy = sRemote->oPosY - y; dz = sRemote->oPosZ - z;
    return dx * dx + dy * dy + dz * dz < dist * dist;
}

void psp_np_world_retarget(struct Object *o) {
    if (!sWPeerHere || !np_remote_alive()) {
        return;
    }
    if (np_w_is_spawner(o)) {
        if ((o->oFlags & OBJ_FLAG_COMPUTE_DIST_TO_MARIO) && dist_between_objects(o, sRemote) < o->oDistanceToMario) {
            o->oDistanceToMario = dist_between_objects(o, sRemote);
        }
        return;
    }
    if (np_w_kind(o) == 0 || !np_w_owned(o)) {
        return;
    }
    if (dist_between_objects(o, sRemote) < dist_between_objects(o, gMarioObject)) {
        if (o->oFlags & OBJ_FLAG_COMPUTE_DIST_TO_MARIO) {
            o->oDistanceToMario = dist_between_objects(o, sRemote);
        }
        if (o->oFlags & OBJ_FLAG_COMPUTE_ANGLE_TO_MARIO) {
            o->oAngleToMario = obj_angle_to_object(o, sRemote);
        }
    }
}

static int np_w_apply(const PspNpWorldEvent *e) {
    struct Object *o;
    switch (e->type) {
        case PSP_NP_EV_COIN:
            o = (e->aux[1] != 0) ? np_w_find_loot(e->bhv, e->aux[1]) : NULL;
            if (o == NULL) {
                o = np_w_find(e->bhv, e->pos, e->aux[1] != 0 ? 600.0f : 150.0f, e->coinValue);
            }
            if (o != NULL) {
                o->oInteractStatus = INT_STATUS_INTERACTED; /* the coin's own behavior does the rest (sparkles, red coin counter, ...) */
                if (e->aux[0] > 0 && o->parentObj != NULL && sRedFixN < NP_W_REDFIX) { /* red coin: the counter is also set to the sender's (see np_w_loot_step) */
                    sRedFix[sRedFixN].star = o->parentObj;
                    sRedFix[sRedFixN].target = e->aux[0];
                    sRedFix[sRedFixN].age = 0;
                    sRedFixN++;
                }
            }
            return o != NULL;
        case PSP_NP_EV_STAR:
            o = np_w_find(e->bhv, e->pos, 500.0f, 0);
            if (o != NULL) {
                o->oInteractStatus = INT_STATUS_INTERACTED;
            }
            return o != NULL;
        case PSP_NP_EV_CAP:
        case PSP_NP_EV_1UP:
            o = np_w_find(e->bhv, e->pos, 150.0f, 0);
            if (o != NULL) {
                obj_mark_for_deletion(o);
            }
            return o != NULL;
#if PSP_NETPLAY_ENEMIES
        case PSP_NP_EV_DEAD:
            if (e->bhv >= NP_W_BRK_BASE) { /* a crate or a wall */
                o = np_w_find_brk(e->bhv - NP_W_BRK_BASE, e->pos);
                if (o != NULL) {
                    struct Object *saved = gCurrentObject;
                    int k;
                    if (e->bhv - NP_W_BRK_BASE == NP_W_BRK_MINE) { /* a mine of Bowser's arena: it goes off here too (it is not marked as gone for good: it comes back with the area) */
                        gCurrentObject = o;
                        if (e->value == 1) { /* a player ran into it */
                            spawn_object(o, MODEL_EXPLOSION, bhvExplosion);
                        } else if (e->value == 2) { /* Bowser landed on it */
                            spawn_object(o, MODEL_BOWSER_FLAMES, bhvBowserBombExplosion);
                            create_sound_spawner(SOUND_GENERAL_BOWSER_BOMB_EXPLOSION);
                            set_camera_shake_from_point(SHAKE_POS_LARGE, o->oPosX, o->oPosY, o->oPosZ);
                        } /* (0: a replay of what happened before this player came in: it just disappears) */
                        gCurrentObject = saved;
                    } else {
                        set_object_respawn_info_bits(o, RESPAWN_INFO_DONT_RESPAWN);
                        gCurrentObject = o;
                        spawn_mist_particles();
                        gCurrentObject = saved;
                    }
                    obj_mark_for_deletion(o);
                    for (k = 0; k < sBrkN; k++) { /* (not reported back as if it had been destroyed here) */
                        if (sBrk[k].o == o) {
                            sBrk[k] = sBrk[--sBrkN];
                            break;
                        }
                    }
                }
                return o != NULL;
            }
            o = np_w_find_kind(e->bhv, e->pos);
            if (o != NULL) {
                struct Object *saved = gCurrentObject;
                set_object_respawn_info_bits(o, RESPAWN_INFO_DONT_RESPAWN);
                gCurrentObject = o; /* (the effect is made around the object that is running: here nobody is) */
                spawn_mist_particles(); /* the puff the enemy leaves where it died, which its own death would have made */
                gCurrentObject = saved;
                obj_mark_for_deletion(o);
            } else {
                sDgDeadWait++;
            }
            return o != NULL;
        case PSP_NP_EV_ATTACK:
            o = np_w_find_kind(e->coinValue, e->pos);
            if (o == NULL) {
                sDgAtkMiss++;
            } else {
                sDgAtk++;
                o->oInteractStatus |= (s32) e->bhv;
                sAtkObj = o;
                sAtkYaw = e->aux[0];
                sAtkT = sClock;
            }
            return 1; /* (a hit is not kept waiting) */
        case PSP_NP_EV_SPAWN:
            np_w_spawn_loot(e);
            return 1;
        case PSP_NP_EV_STARSPAWN:
            np_w_spawn_star(e);
            return 1;
#endif
#if PSP_NETPLAY_TRIGGERS
        case PSP_NP_EV_ACTION:
            return np_w_trg_apply(e);
#endif
        default:
            return 1;
    }
}

/* For the link log (psp_netplay.c): what did not match since the last call, or 0 when nothing is worth a line. A "miss" means the other console talked about
 * an enemy that this one cannot find (kind or home differ), a hit or death that found nobody, or a shot whose parent was not found. */
int psp_np_world_diag(char *buf, int n) {
#if PSP_NETPLAY_ENEMIES
    int r = 0;
    if (sDgMiss != 0 || sDgAtkMiss != 0 || sDgDeadWait != 0 || sDgSpawnPar != 0 || sDgCoinOut != 0 || sDgCoinIn != 0 || sDgOwn > 50) {
        r = snprintf(buf, (size_t) n, "sync: obj in %u miss %u mine %u | hit ok %u miss %u | death wait %u | shots %u noparent %u | coins out %u in %u", sDgIn, sDgMiss, sDgOwn, sDgAtk, sDgAtkMiss,
                     sDgDeadWait, sDgSpawn, sDgSpawnPar, sDgCoinOut, sDgCoinIn);
    }
    sDgIn = sDgMiss = sDgOwn = sDgAtk = sDgAtkMiss = sDgDeadWait = sDgSpawn = sDgSpawnPar = sDgCoinOut = sDgCoinIn = 0;
    return r;
#else
    (void) buf; (void) n;
    return 0;
#endif
}

void psp_np_world_reset(void) {
#if PSP_NETPLAY_TRIGGERS
    sTrgSeenN = 0;
#endif
    sWLogN = sWPendN = sWAge = sWPeerHere = sPeerNowKnown = 0;
#if PSP_NETPLAY_ENEMIES
    sClock += 1000; /* every stamp is stale now: nothing is followed or claimed */
    sLootOutN = sLootFixN = sRedFixN = 0;
    memset(sSent, 0, sizeof(sSent));
    memset(sRemHeld, 0, sizeof(sRemHeld));
    sTrkN = 0;
    sPlatN = sPlatCursor = sPlatTick = 0;
    sBowserHeard = sBowserPeerWait = sBowserCarried = 0;
    sBrkN = 0;
#endif
    sWLevel = sWArea = -1;
}

#if PSP_NETPLAY_ENEMIES
/* ---- Moving platforms (the model of sm64coopdx) ----
 * Every console runs the platform's own script. On top of that the one that drives a platform (the host, or the client while Mario is standing on it) sends
 * its key values now and then (position, speed, action, timers and the fields of the kind); the other console writes them over its copy, and if Mario is
 * standing on that platform he is moved by the same step, so a correction does not slide him off. Nothing is puppeted: if the packets stop, the platforms
 * keep running on their own. A platform is identified by its kind and its order among the platforms of that kind. */
#define NP_P_MAX 192
#define NP_P_EXTRA 8
#define NP_P_STD 16
#define NP_P_RIDE_FRAMES 60 /* the client keeps sending a platform this long after Mario stepped off it */
#define NP_P_PERIOD 4       /* frames between two batches */
#define NP_P_PER_BATCH 4

#define NPF(f) ((uint16_t) offsetof(struct Object, f))
static const struct {
    const BehaviorScript *bhv;
    uint8_t ttc; /* 1 = the platform reads gTTCSpeedSetting: the host's value goes along; 2 = an enemy (not a platform): the console of the player nearest to it sends it
                  * (np_w_rule_mine) and the other one only corrects its own copy, which keeps running its script; 3 / 4 = the same, found by home position (4: sent at every batch) */
    uint16_t off[NP_P_EXTRA];
} sPlatKind[] = {
    { bhvTTC2DRotator, 1, { NPF(oTTC2DRotatorMinTimeUntilNextTurn), NPF(oTTC2DRotatorTargetYaw), NPF(oTTC2DRotatorIncrement), NPF(oTTC2DRotatorRandomDirTimer), NPF(oAngleVelYaw) } },
    { bhvTTCCog, 1, { NPF(oTTCCogSpeed), NPF(oTTCCogTargetVel), NPF(oAngleVelYaw) } },
    { bhvTTCElevator, 1, { NPF(oTTCElevatorDir), NPF(oTTCElevatorMoveTime) } },
    { bhvTTCMovingBar, 1, { NPF(oTTCMovingBarDelay), NPF(oTTCMovingBarStoppedTimer), NPF(oTTCMovingBarOffset), NPF(oTTCMovingBarSpeed), NPF(oTTCMovingBarStartOffset) } },
    { bhvTTCPendulum, 1, { NPF(oTTCPendulumAccelDir), NPF(oTTCPendulumAngle), NPF(oTTCPendulumAngleVel), NPF(oTTCPendulumAngleAccel), NPF(oTTCPendulumDelay), NPF(oTTCPendulumSoundTimer) } },
    { bhvTTCPitBlock, 1, { NPF(oTTCPitBlockDir), NPF(oTTCPitBlockWaitTime) } },
    { bhvTTCRotatingSolid, 1, { NPF(oTTCRotatingSolidNumTurns), NPF(oTTCRotatingSolidRotationDelay), NPF(oTTCRotatingSolidVelY), NPF(oTTCChangeDirTimer), NPF(oAngleVelRoll) } },
    { bhvTTCSpinner, 1, { NPF(oAngleVelPitch), NPF(oTTCSpinnerDir), NPF(oTTCChangeDirTimer) } },
    { bhvTTCTreadmill, 1, { NPF(oTTCTreadmillSpeed), NPF(oTTCTreadmillTargetSpeed), NPF(oTTCTreadmillTimeUntilSwitch) } },
    { bhvLllWoodPiece, 0, { NPF(oLllWoodPieceOscillationTimer) } },
    { bhvLllRotatingHexagonalRing, 0, { NPF(oAngleVelYaw) } },
    { bhvLllMovingOctagonalMeshPlatform, 0, { NPF(oHorizontalMovementUnkF8), NPF(oHorizontalMovementUnkF4), NPF(oHorizontalMovementUnk100), NPF(oForwardVel) } },
    { bhvLllRotatingHexFlame, 0, { NPF(oAngleVelYaw) } },
    { bhvLllSinkingRectangularPlatform, 0, { NPF(oLllWoodPieceOscillationTimer) } },
    { bhvLllSinkingRockBlock, 0, { NPF(oSinkWhenSteppedOnUnk104), NPF(oGraphYOffset) } },
    { bhvSeesawPlatform, 0, { NPF(oSeesawPlatformPitchVel) } },
    { bhvSwingPlatform, 0, { NPF(oSwingPlatformAngle), NPF(oSwingPlatformSpeed), NPF(oAngleVelRoll) } },
    { bhvSlidingPlatform2, 0, { NPF(oBackAndForthPlatformUnkF4), NPF(oBackAndForthPlatformUnkF8), NPF(oBackAndForthPlatformUnkFC), NPF(oBackAndForthPlatformUnk100) } },
    { bhvToxBox, 0, { NPF(oForwardVel), NPF(oUnkC0), NPF(oToxBoxMovementStep) } },
    { bhvActivatedBackAndForthPlatform, 0, { NPF(oActivatedBackAndForthPlatformMaxOffset), NPF(oActivatedBackAndForthPlatformOffset), NPF(oActivatedBackAndForthPlatformVel),
          NPF(oActivatedBackAndForthPlatformCountdown), NPF(oActivatedBackAndForthPlatformStartYaw), NPF(oActivatedBackAndForthPlatformVertical), NPF(oActivatedBackAndForthPlatformFlipRotation) } },
    { bhvFerrisWheelAxle, 0, { NPF(oFaceAngleRoll) } },
    /* patch 260: the other platforms sm64coopdx keeps in step (most only need the standard values: position, action, timer, angles) */
    { bhvArrowLift, 0, { 0 } },
    { bhvBbhTiltingTrapPlatform, 0, { NPF(oAngleVelPitch) } },
    { bhvHauntedBookshelf, 0, { NPF(oHauntedBookshelfShouldOpen) } },
    { bhvFallingBowserPlatform, 0, { 0 } },
    { bhvSquishablePlatform, 0, { NPF(oPlatformTimer) } },
    { bhvBitfsSinkingPlatforms, 0, { NPF(oPlatformTimer) } },
    { bhvBitfsSinkingCagePlatform, 0, { NPF(oPlatformTimer) } },
    { bhvAnotherElavator, 0, { 0 } },
    { bhvRrElevatorPlatform, 0, { 0 } },
    { bhvHmcElevatorPlatform, 0, { 0 } },
    { bhvMeshElevator, 0, { 0 } },
    { bhvWdwExpressElevator, 0, { 0 } },
    { bhvPyramidElevator, 0, { 0 } },
    { bhvWfElevatorTowerPlatform, 0, { 0 } },
    { bhvLllRotatingBlockWithFireBars, 0, { NPF(oAngleVelYaw) } },
    { bhvLllSinkingSquarePlatforms, 0, { NPF(oLllWoodPieceOscillationTimer) } },
    { bhvJrbSlidingBox, 0, { NPF(oJrbSlidingBoxUnkF8), NPF(oJrbSlidingBoxUnkFC) } },
    { bhvShipPart3, 0, { NPF(oShipPart3UnkF4) } },
    { bhvInSunkenShip3, 0, { NPF(oShipPart3UnkF4) } },
    { bhvLllVolcanoFallingTrap, 0, { NPF(oRollingLogUnkF4), NPF(oAngleVelPitch) } },
    { bhvHauntedChair, 0, { NPF(oHauntedChairUnk104), NPF(oHauntedChairUnkF4), NPF(oHauntedChairUnkF8), NPF(oHauntedChairUnkFC), NPF(oMoveAnglePitch) } },
    { bhvSpindel, 0, { NPF(oSpindelUnkF4), NPF(oSpindelUnkF8), NPF(oAngleVelPitch), NPF(oMoveAnglePitch) } },
    { bhvKickableBoard, 0, { NPF(oAngleVelPitch), NPF(oKickableBoardF4), NPF(oKickableBoardF8) } },
    { bhvDonutPlatform, 0, { NPF(oGravity), NPF(oIntangibleTimer) } },
    { bhvTumblingBridgePlatform, 0, { 0 } },
    { bhvWfTumblingBridge, 0, { NPF(oIntangibleTimer) } },
    { bhvBbhTumblingBridge, 0, { NPF(oIntangibleTimer) } },
    { bhvLllTumblingBridge, 0, { NPF(oIntangibleTimer) } },
    { bhvBookSwitch, 0, { NPF(oBookSwitchUnkF4), NPF(oIntangibleTimer) } },
    { bhvLllBowserPuzzle, 0, { NPF(oBowserPuzzleCompletionFlags) } },
    { bhvLllDrawbridge, 0, { 0 } },
    /* patch 278: enemies with parts or a long script that cannot be puppeted (their chain / segments are built by their own script): every console keeps
     * running its own copy and the one of the player nearest to it corrects the other's now and then */
    { bhvChainChomp, 2, { NPF(oChainChompDistToPivot), NPF(oChainChompUnk104), NPF(oChainChompRestrictedByChain), NPF(oChainChompTargetPitch), NPF(oChainChompNumLunges),
          NPF(oChainChompReleaseStatus), NPF(oChainChompHitGate) } },
    { bhvWigglerHead, 2, { NPF(oHealth), NPF(oWigglerFallThroughFloorsHeight), NPF(oWigglerWalkAnimSpeed), NPF(oWigglerSquishSpeed), NPF(oWigglerTimeUntilRandomTurn),
          NPF(oWigglerTargetYaw), NPF(oWigglerWalkAwayFromWallTimer) } },
    { bhvPlatformOnTrack, 0, { NPF(oPlatformOnTrackBaseBallIndex), NPF(oPlatformOnTrackDistMovedSinceLastBall), NPF(oPlatformOnTrackSkiLiftRollVel), NPF(oPlatformOnTrackPrevWaypointFlags),
          NPF(oPlatformOnTrackPitch), NPF(oPlatformOnTrackYaw), NPF(oPlatformOnTrackOffsetY) } },
    /* patch 280: more of what sm64coopdx keeps in step (it syncs these objects by their action and a few fields; same fields here). Appended: the numbers
     * of the kinds above must not change. The ones with ttc 2 are driven by the console of the nearest player, the others like the platforms. */
    { bhvTtmRollingLog, 0, { NPF(oAngleVelPitch), NPF(oMoveAnglePitch), NPF(oPitouneUnkF4), NPF(oPitouneUnkF8), NPF(oPitouneUnkFC) } },
    { bhvLllRollingLog, 0, { NPF(oAngleVelPitch), NPF(oMoveAnglePitch), NPF(oPitouneUnkF4), NPF(oPitouneUnkF8), NPF(oPitouneUnkFC) } },
    { bhvControllablePlatform, 0, { 0 } },
    { bhvWaterLevelPillar, 0, { NPF(oWaterLevelPillarUnkF8) } },
    { bhvDorrie, 2, { NPF(oDorrieOffsetY), NPF(oDorrieVelY), NPF(oDorrieYawVel), NPF(oDorrieNeckAngle), NPF(oAngleVelYaw) } }, /* (not oDorrieLiftingMario: it is about the local player) */
    { bhvClamShell, 2, { NPF(oClamUnkF4) } },
    { bhvPyramidTop, 2, { 0 } },
    { bhvPushableMetalBox, 2, { 0 } },
    { bhvSnowmansBottom, 2, { 0 } },
    { bhvBigBoulder, 2, { 0 } },
    { bhvBowlingBall, 2, { NPF(oBowlingBallTargetYaw) } },
    { bhvFreeBowlingBall, 2, { 0 } },
    { bhvPitBowlingBall, 2, { 0 } },
    /* the puzzle chests of Jolly Roger Bay: the answer counter lives in the object that makes the chests (default list: only scanned in that course) */
    { bhvTreasureChestsJrb, 2, { NPF(oTreasureChestUnkF4), NPF(oTreasureChestUnkF8), NPF(oTreasureChestUnkFC) } },
    { bhvTreasureChestsShip, 2, { NPF(oTreasureChestUnkF4), NPF(oTreasureChestUnkF8), NPF(oTreasureChestUnkFC) } },
    { bhvTreasureChestBottom, 2, { NPF(oIntangibleTimer) } },
    /* patch 282: the bosses and the other enemies with a long script of their own. Every console runs the script; what the console of the nearest player says is copied by
     * the other one, but only from the moment this one is past its own start (the camera cutscene and the text of Bowser and Eyerok, the parts of the pokey, the hands of Eyerok)
     * and never the grabs (a chuckya holding a player, a Bowser held by the tail), which belong to the console of that player (np_p_ready / np_p_state_ok).
     * ttc 3 = like 2 but the object is found by its kind and its home position (there can be several, and the far ones are unloaded, so their order is not the same in both
     * consoles); ttc 4 = the same, and it is sent at every batch (the bosses: the fight is decided by what they do now). Appended: the kind numbers must not change. */
    { bhvBowser, 4, { NPF(oHealth), NPF(oBowserUnkF8), NPF(oOpacity), NPF(oBowserUnk1AC), NPF(oBowserUnk10E), NPF(oBowserUnk110), NPF(oBowserEyesShut),
          NPF(oHeldState) } }, /* (oHeldState only tells the other console that a player carries him by the tail: it is never written over the copy's own, see np_p_apply) */
    { bhvEyerokBoss, 4, { NPF(oEyerokBossUnkFC), NPF(oEyerokBossActiveHand), NPF(oEyerokBossUnk104), NPF(oEyerokBossUnk108), NPF(oEyerokBossUnk10C), NPF(oEyerokBossUnk110),
          NPF(oEyerokBossUnk1AC) } },
    { bhvEyerokHand, 4, { NPF(oEyerokHandUnkFC), NPF(oEyerokHandUnk100), NPF(oGravity) } },
    { bhvPokey, 3, { NPF(oPokeyAliveBodyPartFlags), NPF(oPokeyNumAliveBodyParts), NPF(oPokeyHeadWasKilled), NPF(oPokeyTargetYaw), NPF(oPokeyChangeTargetTimer),
          NPF(oPokeyTurningAwayFromWall) } },
    { bhvChuckya, 3, { NPF(oChuckyaUnkF8), NPF(oChuckyaUnkFC) } }, /* (not oChuckyaUnk88 / 100: they say "the player is held" and make the anchor throw THIS console's Mario) */
    { bhvWoodenPost, 2, { NPF(oWoodenPostOffsetY), NPF(oWoodenPostSpeedY) } }, /* (the post of the chain chomp sinks when it is ground-pounded; not the "pounding" flag: it is about the local player) */
};
#define NP_P_KINDS ((int) (sizeof(sPlatKind) / sizeof(sPlatKind[0])))

/* the fields every platform sends (offsets into the object) */
static const uint16_t sPlatStd[NP_P_STD - 1] = { NPF(oPosX), NPF(oPosY), NPF(oPosZ), NPF(oVelX), NPF(oVelY), NPF(oVelZ), NPF(oForwardVel), NPF(oAction), NPF(oPrevAction),
    NPF(oSubAction), NPF(oMoveAngleYaw), NPF(oTimer), NPF(oFaceAnglePitch), NPF(oFaceAngleYaw), NPF(oFaceAngleRoll) };

typedef struct {
    uint8_t kind, n, ord, flags; /* flags bit 0: sent by the host */
    uint8_t level, area;
    int16_t ttc;
    uint32_t params;
    uint32_t w[NP_P_STD + NP_P_EXTRA];
} PspNpFld; /* 108 bytes */
#define NP_P_REC ((int) sizeof(PspNpFld))
#define NP_P_HOST 1

static const void *sPlatVirt[NP_P_KINDS];
static int sPlatInit;
static struct {
    struct Object *o;
    uint8_t kind, ord, n;
} sPlat[NP_P_MAX];

static int np_p_kind(const struct Object *o) {
    int i;
    if (!sPlatInit) {
        for (i = 0; i < NP_P_KINDS; i++) {
            sPlatVirt[i] = segmented_to_virtual(sPlatKind[i].bhv);
        }
        sPlatInit = 1;
    }
    for (i = 0; i < NP_P_KINDS; i++) {
        if (o->behavior == sPlatVirt[i]) {
            return i;
        }
    }
    return -1;
}

/* the kinds the console of the nearest player drives (2, 3 and 4 in the table) */
static int np_p_near(int kind) {
    return sPlatKind[kind].ttc >= 2;
}

/* ... and the ones found by home position instead of by order (3, 4) */
static int np_p_byhome(int kind) {
    return sPlatKind[kind].ttc >= 3;
}

#define NP_P_BYHOME 2 /* flags: ord / n of the record carry the home hash of the object, not its order */
static uint16_t np_p_hash(const struct Object *o) {
    return (uint16_t) ((int) o->oHomeX * 31 + (int) o->oHomeZ * 17 + (int) o->oHomeY * 7);
}

/* a state object the console of the nearest player drives (the puzzle chests, ...): the star it makes is announced like the one of a boss */
static int np_p_nearest_driven(const struct Object *o) {
    const int k = np_p_kind(o);
    return k >= 0 && np_p_near(k);
}

/* the platforms of this area, with their order within their kind (the surface list keeps the order they were created in) */
static void np_p_scan(void) {
    static const u8 lists[3] = { OBJ_LIST_SURFACE, OBJ_LIST_GENACTOR, OBJ_LIST_DEFAULT };
    uint8_t count[NP_P_KINDS];
    int i, l;
    memset(count, 0, sizeof(count));
    sPlatN = 0;
    for (l = 0; l < ((gCurrLevelNum == LEVEL_JRB) ? 3 : 2); l++) { /* (the default list is only looked through where something of it is kept: the puzzle chests) */
        struct ObjectNode *head = &gObjectLists[lists[l]];
        struct Object *obj;
        for (obj = (struct Object *) head->next; obj != (struct Object *) head; obj = (struct Object *) obj->header.next) {
            int k;
            if (obj->activeFlags == ACTIVE_FLAG_DEACTIVATED || obj->behavior == NULL || (k = np_p_kind(obj)) < 0) {
                continue;
            }
            if (l >= 1 && !np_p_near(k)) {
                continue; /* (in the actor and default lists only the entries driven by the nearest player) */
            }
            if (sPlatN < NP_P_MAX) {
                sPlat[sPlatN].o = obj;
                sPlat[sPlatN].kind = (uint8_t) k;
                sPlat[sPlatN].ord = count[k];
                sPlatN++;
            }
            if (count[k] < 255) {
                count[k]++;
            }
        }
    }
    for (i = 0; i < sPlatN; i++) {
        sPlat[i].n = count[sPlat[i].kind];
    }
}

static int np_p_riding(const struct Object *o) {
    return gMarioObject != NULL && gMarioObject->platform == o;
}

/* The track platform goes from waypoint to waypoint by a pointer: it travels as the index counted from the first waypoint (sm64coopdx does the same) */
#define NP_P_WP_SLOT (NP_P_STD + NP_P_EXTRA - 1)
static int np_p_is_track(int kind) {
    return sPlatKind[kind].bhv == bhvPlatformOnTrack;
}

/* Is the object ready to be corrected by the other console (or to be sent)? An object that has not finished its own start must be left alone: the chain
 * chomp builds its chain only when this player gets within 3000 of it (and frees it again when the player goes away, leaving the pointer where it was),
 * the wiggler its body in its first update. Putting the other console's action into one of them before that sends its script into code that uses what
 * was never built (a null pointer: the console freezes, which is what happened to the player entering Bob-omb Battlefield while the other one was already
 * near the chomp). An enemy of the "nearest player" kind that is hidden here (far from this player) is left alone too: its script is not running, so it
 * could not use the values anyway. */
#define NP_P_W_ACTION 7 /* index of oAction in the standard fields (sPlatStd) */
#define NP_P_BOWSER_ACTIONS 20 /* the actions of Bowser's table that are carried (sBowserActions); 5 and 6 (intro) are not, and 20 only says "waiting for the other console" */
#define NP_P_BOWSER_WAIT 20
#define NP_P_BOWSER_QUIET 150 /* frames without any word of the other console's Bowser before the waiting one starts the fight on its own */
#define NP_P_BOWSER_FOLLOW 12 /* frames the console that is not the nearest still sends Bowser after the other one said it waits */

/* Not sWPeerHere: that one is only brought up to date at the start of a frame, and an area that has just been loaded runs its objects' first frame in the very
 * frame it was loaded, so it still says what was true of the area this console came from. The place the other player was last reported at is compared with the
 * area that is loaded NOW. */
int psp_np_world_peer_here(void) {
    return sPeerNowKnown && psp_netplay_state() == PSP_NP_CONNECTED && gCurrentArea != NULL && sPeerNowLevel == gCurrLevelNum && sPeerNowArea == np_area_key();
}

/* Bowser is in the other player's hands (that console says so at every batch): this copy does not run his script, it hangs from that player's model */
int psp_np_world_bowser_carried(const struct Object *o) {
    return (sClock - sBowserCarried) < NP_P_BOWSER_CARRY_FRAMES && o->oHeldState == HELD_FREE && np_remote_alive();
}

void psp_np_world_bowser_carry_step(struct Object *o) {
    const s16 yaw = (s16) sRemote->header.gfx.angle[1]; /* the way the other player faces: the way Bowser hangs, and the way he goes when it lets go */
    o->oPosX = sRemote->oPosX + sins(yaw) * 330.0f;
    o->oPosZ = sRemote->oPosZ + coss(yaw) * 330.0f;
    o->oPosY = sRemote->oPosY + 60.0f;
    o->oMoveAngleYaw = o->oFaceAngleYaw = yaw;
    o->oForwardVel = o->oVelY = 0.0f;
    o->oAngleVelYaw = 0;
}

int psp_np_world_bowser_give_up(int frames) {
    if (!psp_np_world_peer_here()) {
        return frames >= 30; /* nobody to wait for (the other player left, or it is a single-player game that got here by a stale flag) */
    }
    if (frames < NP_P_BOWSER_QUIET) {
        return 0;
    }
    return sBowserHeard == 0 || (sClock - sBowserHeard) > NP_P_BOWSER_QUIET;
}
static int np_p_ready(const struct Object *o, int kind) {
    const BehaviorScript *b = sPlatKind[kind].bhv;
    if (b == bhvChainChomp) {
        return o->oAction == CHAIN_CHOMP_ACT_MOVE && o->oChainChompSegments != NULL && o->parentObj != NULL && o->parentObj != o;
    }
    if (b == bhvWigglerHead) {
        return o->oAction != WIGGLER_ACT_UNINITIALIZED && o->oWigglerSegments != NULL;
    }
    if (np_p_near(kind) && (o->activeFlags & ACTIVE_FLAG_FAR_AWAY)) {
        return 0;
    }
    if (np_p_near(kind) && o->oHeldState != HELD_FREE) {
        return 0; /* carried or thrown by a player: that belongs to the console of that player */
    }
    if (b == bhvBowser) { /* its own start (the camera cutscene and the text, actions 5 and 6) goes first; an action past the table would jump into whatever follows it */
        return o->oAction >= 0 && o->oAction <= NP_P_BOWSER_WAIT && o->oAction != 5 && o->oAction != 6; /* (20: waiting for the other console's state, which is what this call lets in) */
    }
    if (b == bhvEyerokBoss) { /* the intro (waking up, the text) is this console's own; the hands are made by its first frame */
        return o->oAction == EYEROK_BOSS_ACT_FIGHT && o->oEyerokBossNumHands > 0;
    }
    if (b == bhvEyerokHand) {
        return o->oAction != EYEROK_HAND_ACT_SLEEP && o->parentObj != NULL && o->parentObj != o && o->parentObj->oAction == EYEROK_BOSS_ACT_FIGHT;
    }
    if (b == bhvPokey) { /* the body parts are made when it wakes up (and taken away when the player goes far: the action goes back to 0) */
        return o->oAction == POKEY_ACT_WANDER && o->oPokeyNumAliveBodyParts > 0;
    }
    if (b == bhvChuckya) { /* (action 1 = it holds a player) */
        return o->oAction != 1 && !(o->oInteractStatus & INT_STATUS_GRABBED_MARIO);
    }
    if (b == bhvWoodenPost) { /* the chomp makes it as its child: before that there is nothing to describe */
        return o->parentObj != NULL;
    }
    return 1;
}

/* the same for the state that comes in: the other console's copy must also be past its start (a chain chomp that is not moving has no chain to describe) */
static int np_p_state_ok(int kind, const PspNpFld *r) {
    const BehaviorScript *b = sPlatKind[kind].bhv;
    if (b == bhvChainChomp) {
        return r->w[NP_P_W_ACTION] == CHAIN_CHOMP_ACT_MOVE;
    }
    if (b == bhvWigglerHead) {
        return r->w[NP_P_W_ACTION] != WIGGLER_ACT_UNINITIALIZED;
    }
    if (b == bhvBowser) {
        return r->w[NP_P_W_ACTION] < (uint32_t) NP_P_BOWSER_ACTIONS && r->w[NP_P_W_ACTION] != 5 && r->w[NP_P_W_ACTION] != 6;
    }
    if (b == bhvEyerokBoss) {
        return r->w[NP_P_W_ACTION] == EYEROK_BOSS_ACT_FIGHT;
    }
    if (b == bhvEyerokHand) {
        return r->w[NP_P_W_ACTION] != EYEROK_HAND_ACT_SLEEP && r->w[NP_P_W_ACTION] <= EYEROK_HAND_ACT_DIE;
    }
    if (b == bhvPokey) { /* (extras 0 and 1: which parts are alive, how many) */
        return r->w[NP_P_W_ACTION] == POKEY_ACT_WANDER && r->w[NP_P_STD] <= 0x1Fu && r->w[NP_P_STD + 1] <= 5u;
    }
    if (b == bhvChuckya) { /* not 1 (holding a player) */
        return r->w[NP_P_W_ACTION] == 0 || r->w[NP_P_W_ACTION] == 2 || r->w[NP_P_W_ACTION] == 3;
    }
    return 1;
}

static void np_p_pack(PspNpFld *r, int i, int host) {
    struct Object *o = sPlat[i].o;
    int j;
    memset(r, 0, sizeof(*r));
    r->kind = sPlat[i].kind;
    r->n = sPlat[i].n;
    r->ord = sPlat[i].ord;
    r->flags = host ? NP_P_HOST : 0;
    if (np_p_byhome(r->kind)) {
        const uint16_t h = np_p_hash(o);
        r->flags |= NP_P_BYHOME;
        r->ord = (uint8_t) h;
        r->n = (uint8_t) (h >> 8);
    }
    r->level = (uint8_t) sWLevel;
    r->area = (uint8_t) sWArea;
    r->ttc = gTTCSpeedSetting;
    r->params = (uint32_t) o->oBehParams;
    for (j = 0; j < NP_P_STD - 1; j++) {
        memcpy(&r->w[j], (const char *) o + sPlatStd[j], 4);
    }
    for (j = 0; j < NP_P_EXTRA && sPlatKind[r->kind].off[j] != 0; j++) {
        memcpy(&r->w[NP_P_STD + j], (const char *) o + sPlatKind[r->kind].off[j], 4);
    }
    if (np_p_is_track(r->kind)) {
        const struct Waypoint *start = o->oPlatformOnTrackStartWaypoint, *prev = o->oPlatformOnTrackPrevWaypoint;
        r->w[NP_P_WP_SLOT] = (start != NULL && prev != NULL) ? (uint32_t) (prev - start) : 0xFFFFFFFFu;
    }
}

/* A pokey after the other console's state has been written over it. Its tower grows back by itself on each console (a bigger count that comes in is not taken), and
 * once the head is dead the parts take themselves off the count one by one (taking the other console's count then would take them off twice and leave a negative
 * count that is never 0: the pokey would never go away). What does come in is the loss of parts: the parts the other console no longer has are removed here too. */
static void np_p_pokey_parts(struct Object *pokey, uint32_t flagsBefore, s32 numBefore) {
    struct ObjectNode *head = &gObjectLists[OBJ_LIST_GENACTOR];
    const void *bhvPart = segmented_to_virtual(bhvPokeyBodyPart);
    struct Object *obj, *part[8];
    int n = 0, i, excess;
    if (pokey->oPokeyHeadWasKilled || pokey->oPokeyNumAliveBodyParts > numBefore) {
        pokey->oPokeyAliveBodyPartFlags = flagsBefore;
        pokey->oPokeyNumAliveBodyParts = numBefore;
        return;
    }
    for (obj = (struct Object *) head->next; obj != (struct Object *) head && n < 8; obj = (struct Object *) obj->header.next) {
        if (obj->activeFlags != ACTIVE_FLAG_DEACTIVATED && obj->behavior == bhvPart && obj->parentObj == pokey) {
            part[n++] = obj;
        }
    }
    for (excess = n - pokey->oPokeyNumAliveBodyParts; excess > 0; excess--) {
        int best = -1;
        for (i = 0; i < n; i++) { /* the lowest one whose flag the other console has cleared */
            u32 idx;
            if (part[i] == NULL) {
                continue;
            }
            idx = (u32) part[i]->oBehParams2ndByte;
            if (idx < 32 && !(pokey->oPokeyAliveBodyPartFlags & (1u << idx)) && (best < 0 || idx > (u32) part[best]->oBehParams2ndByte)) {
                best = i;
            }
        }
        if (best < 0) {
            break;
        }
        obj_mark_for_deletion(part[best]);
        part[best] = NULL;
    }
}

static void np_p_apply(const PspNpFld *r) {
    struct Object *o;
    float old[3];
    uint32_t pokeyFlags = 0;
    s32 pokeyNum = 0, heldBefore = 0;
    int j, i;
    if (r->kind >= NP_P_KINDS || r->level != sWLevel || r->area != sWArea) {
        return;
    }
    for (i = 0; i < sPlatN; i++) {
        if (sPlat[i].kind != r->kind) {
            continue;
        }
        if (r->flags & NP_P_BYHOME) {
            if (np_p_hash(sPlat[i].o) == (uint16_t) ((r->n << 8) | r->ord)) {
                break;
            }
        } else if (sPlat[i].ord == r->ord) {
            break;
        }
    }
    if (i >= sPlatN || (!(r->flags & NP_P_BYHOME) && sPlat[i].n != r->n)) {
        return; /* not the same platform (or the two consoles have a different number of them): better leave it alone */
    }
    o = sPlat[i].o;
    if (o->activeFlags == ACTIVE_FLAG_DEACTIVATED || (uint32_t) o->oBehParams != r->params) {
        return;
    }
    if (sPlatKind[r->kind].bhv == bhvBowser) {
        sBowserHeard = sClock; /* (whatever it says: even in its intro the other console's Bowser is there) */
        if (r->w[NP_P_W_ACTION] == NP_P_BOWSER_WAIT) {
            sBowserPeerWait = sClock;
        }
    }
    if (sPlatKind[r->kind].bhv == bhvBowser && r->w[NP_P_BOWSER_HELD_SLOT] == HELD_HELD) { /* the other player carries him: nothing of the state is taken, the copy just hangs from that player */
        if (o->oHeldState == HELD_FREE && !(o->activeFlags & ACTIVE_FLAG_FAR_AWAY)) {
            sBowserCarried = sClock;
        }
        return;
    }
    if (!np_p_ready(o, r->kind) || !np_p_state_ok(r->kind, r)) {
        return;
    }
    if (np_p_near(r->kind)) {
        if (np_w_rule_mine(o) && !(sPlatKind[r->kind].bhv == bhvBowser && o->oAction == NP_P_BOWSER_WAIT)) {
            return; /* the nearest player is this one: this copy is the one that counts (but a Bowser that waits for the fight takes it from whoever has it) */
        }
    } else if (!sWHost && (sClock - o->unused2) < NP_P_RIDE_FRAMES) {
        return; /* the client drives the platforms it is standing on */
    }
    if (sPlatKind[r->kind].ttc == 1 && (r->flags & NP_P_HOST)) {
        gTTCSpeedSetting = r->ttc;
    }
    old[0] = o->oPosX;
    old[1] = o->oPosY;
    old[2] = o->oPosZ;
    if (sPlatKind[r->kind].bhv == bhvPokey) {
        pokeyFlags = o->oPokeyAliveBodyPartFlags;
        pokeyNum = o->oPokeyNumAliveBodyParts;
    }
    heldBefore = o->oHeldState;
    for (j = 0; j < NP_P_STD - 1; j++) {
        memcpy((char *) o + sPlatStd[j], &r->w[j], 4);
    }
    for (j = 0; j < NP_P_EXTRA && sPlatKind[r->kind].off[j] != 0; j++) {
        memcpy((char *) o + sPlatKind[r->kind].off[j], &r->w[NP_P_STD + j], 4);
    }
    if (sPlatKind[r->kind].bhv == bhvPokey) {
        np_p_pokey_parts(o, pokeyFlags, pokeyNum);
    }
    if (sPlatKind[r->kind].bhv == bhvBowser) {
        o->oHeldState = heldBefore; /* (the extra only says whether a player carries him: that is dealt with above) */
        o->oBowserUnk88 = 2; /* the intro of this copy is over (the camera sets this when its cutscene ends; without it the end of an action could send Bowser back to the text) */
    }
    if (np_p_is_track(r->kind) && o->oPlatformOnTrackStartWaypoint != NULL && r->w[NP_P_WP_SLOT] < 64u) {
        const struct Waypoint *wp = o->oPlatformOnTrackStartWaypoint;
        uint32_t w;
        for (w = 0; w < r->w[NP_P_WP_SLOT] && wp->flags != WAYPOINT_FLAGS_END; w++) { /* (the index has to exist in THIS console's path: a pointer past its end would be read as waypoints) */
            wp++;
        }
        if (w == r->w[NP_P_WP_SLOT] && wp->flags != WAYPOINT_FLAGS_END) {
            o->oPlatformOnTrackPrevWaypoint = (struct Waypoint *) wp;
        }
    }
    o->unused1 = sClock; /* heard from it: this console does not send it back for a while */
    if (np_p_riding(o)) { /* Mario goes along with the correction */
        const float dx = o->oPosX - old[0], dy = o->oPosY - old[1], dz = o->oPosZ - old[2];
        if (fabsf(dx) < 2000.0f && fabsf(dy) < 2000.0f && fabsf(dz) < 2000.0f) {
            gMarioStates[0].pos[0] += dx;
            gMarioStates[0].pos[1] += dy;
            gMarioStates[0].pos[2] += dz;
            gMarioObject->oPosX += dx;
            gMarioObject->oPosY += dy;
            gMarioObject->oPosZ += dz;
        }
    }
}

/* is this entry's state one this console has to send now? */
static int np_p_sendable(int i) {
    struct Object *o = sPlat[i].o;
    const int bowser = sPlatKind[sPlat[i].kind].bhv == bhvBowser;
    if (bowser && !(o->activeFlags & ACTIVE_FLAG_FAR_AWAY) && (o->oAction == 5 || o->oAction == 6 || o->oAction == NP_P_BOWSER_WAIT)) {
        return 1; /* the intro and the wait are not carried over, but they are told: the other console knows this Bowser is alive (and that it must lead if it is waited for) */
    }
    if (bowser && o->oHeldState == HELD_HELD && !(o->activeFlags & ACTIVE_FLAG_FAR_AWAY)) {
        return 1; /* a player of this console carries him by the tail: the other console only needs to know that (its copy hangs from this player's model) */
    }
    if (bowser && psp_np_world_bowser_carried(o)) {
        return 0; /* ... and the other way round */
    }
    if ((o->activeFlags & ACTIVE_FLAG_FAR_AWAY) || !np_p_ready(o, sPlat[i].kind)) {
        return 0; /* its script is not running here (or it has not finished its start: what it would send is not a state of the object) */
    }
    if (np_p_near(sPlat[i].kind)) {
        if (bowser && (sClock - sBowserPeerWait) < NP_P_BOWSER_FOLLOW && (sClock - o->unused1) >= NP_P_RIDE_FRAMES) {
            return 1; /* the other player has just come in and waits for this Bowser's state: it goes out even if that player is the nearest to him */
        }
        return np_w_rule_mine(o) && (sClock - o->unused1) >= NP_P_RIDE_FRAMES; /* only the console of the nearest player sends it (and not right after it received a correction) */
    }
    return sWHost ? (sClock - o->unused1) >= NP_P_RIDE_FRAMES : (sClock - o->unused2) < NP_P_RIDE_FRAMES; /* the host does not send what the client is driving; the client only what it has been standing on */
}

static void np_p_frame(int here) {
    PspNpFld batch[NP_P_PER_BATCH];
    uint8_t buf[PSP_NP_FLD_BYTES];
    int len, i, nb = 0, tries;
    np_p_scan();
    if (!here) {
        return;
    }
    while ((len = psp_np_world_fld_take(buf)) > 0) {
        for (i = 0; i + NP_P_REC <= len; i += NP_P_REC) {
            PspNpFld r;
            memcpy(&r, buf + i, sizeof(r)); /* (the buffer is not aligned for the struct) */
            np_p_apply(&r);
        }
    }
    if (!sWHost && gMarioObject != NULL && gMarioObject->platform != NULL) { /* the client's claim on the platform it stands on */
        for (i = 0; i < sPlatN; i++) {
            if (sPlat[i].o == gMarioObject->platform) {
                sPlat[i].o->unused2 = sClock;
            }
        }
    }
    if (++sPlatTick < NP_P_PERIOD || sPlatN == 0) {
        return;
    }
    sPlatTick = 0;
    for (i = 0; i < sPlatN && nb < NP_P_PER_BATCH - 1; i++) { /* the bosses first, at every batch (kept one place free for the rotation below) */
        if (sPlatKind[sPlat[i].kind].ttc == 4 && np_p_sendable(i)) {
            np_p_pack(&batch[nb++], i, sWHost);
        }
    }
    for (tries = 0; tries < sPlatN && nb < NP_P_PER_BATCH; tries++) {
        i = (sPlatCursor + tries) % sPlatN;
        if (sPlatKind[sPlat[i].kind].ttc == 4 || !np_p_sendable(i)) {
            continue; /* (the bosses went already) */
        }
        np_p_pack(&batch[nb++], i, sWHost);
    }
    sPlatCursor = (sPlatCursor + tries) % sPlatN;
    if (nb > 0) {
        psp_np_world_fld_send(batch, nb * NP_P_REC);
    }
}
#endif /* PSP_NETPLAY_ENEMIES (platforms) */

/* Self-healing: every few seconds each console tells how many world events it has recorded for this area. The records of the two should match; if the
 * other one has more, something was lost on the way (a full queue, an event that waited too long for its object) and it is asked to send its record again
 * (the replay is harmless for what is already done). */
static int sDigestT, sResyncN;

void psp_np_world_frame(int peerKnown, int peerLevel, int peerArea, int isHost) {
    PspNpWorldEvent e;
    struct MarioState *m = &gMarioStates[0];
    int here, i;
    sPeerNowKnown = peerKnown;
    sPeerNowLevel = peerLevel;
    sPeerNowArea = peerArea;
    if (gCurrentArea == NULL) {
        return; /* a warp is in progress: the events wait */
    }
    if (sWLevel != gCurrLevelNum || sWArea != np_area_key()) { /* a new area: its state starts from scratch (what the other player has done there comes in as a replay) */
        sWLevel = gCurrLevelNum;
        sWArea = np_area_key();
        sWLogN = sWPendN = sWAge = 0;
        sDigestT = sResyncN = 0;
#if PSP_NETPLAY_ENEMIES
        {
            uint8_t stale[PSP_NP_OBJ_BYTES]; /* (states of enemies and platforms that were sent for the area this console has just left) */
            while (psp_np_world_obj_take(stale) > 0) {
            }
            while (psp_np_world_fld_take(stale) > 0) {
            }
        }
#endif
#if PSP_NETPLAY_ENEMIES
        sTrkN = 0;
        sPlatN = sPlatCursor = sPlatTick = 0;
        sBowserHeard = sBowserPeerWait = sBowserCarried = 0;
        sBrkN = 0;
        memset(sSent, 0, sizeof(sSent));
#endif
#if PSP_NETPLAY_TRIGGERS
        sTrgSeenN = 0;
#endif
        sWPeerHere = peerKnown && peerLevel == sWLevel && peerArea == sWArea;
    }
    if (sWAge < 255) {
        sWAge++;
    }
    here = peerKnown && peerLevel == sWLevel && peerArea == sWArea;
    if (here && !sWPeerHere) {
#if PSP_NETPLAY_ENEMIES
        memset(sSent, 0, sizeof(sSent)); /* a player just came in: it needs every enemy again */
#endif
        for (i = 0; i < sWLogN; i++) { /* the other player has just come into this area: it gets what was collected here */
            e = sWLog[i];
            e.type |= NP_W_REPLAY;
            psp_np_world_send(&e);
        }
    }
    sWPeerHere = here;
    sWHost = isHost;
#if PSP_NETPLAY_TRIGGERS
    if (here && sWAge >= 2 && ++sDigestT >= 300) {
        sDigestT = 0;
        memset(&e, 0, sizeof(e));
        e.type = PSP_NP_EV_DIGEST;
        e.level = (uint8_t) sWLevel;
        e.area = (uint8_t) sWArea;
        e.value = (s16) sWLogN;
        psp_np_world_send(&e);
    }
    if (sWAge >= 2) {
        np_w_triggers_track();
    }
#endif
#if PSP_NETPLAY_ENEMIES
    if (sWAge >= 2) {
        PspNpObj batch[PSP_NP_OBJ_BYTES / sizeof(PspNpObj) + 1];
        int len;
        sClock++;
        np_w_enemies_track();
        np_w_brk_track();
        np_w_loot_step();
        if (here) {
            np_w_enemies_send(); /* what this console drives: the host's enemies, and what the client has picked up */
            while ((len = psp_np_world_obj_take(batch)) > 0) {
                for (i = 0; i < len / (int) sizeof(PspNpObj); i++) {
                    np_w_follow_apply(&batch[i]); /* (an object is followed only while its updates keep coming: see psp_np_world_follow) */
                }
            }
        }
    }
#endif
#if PSP_NETPLAY_ENEMIES
    if (sWAge >= 2) {
        np_p_frame(here);
    }
#endif
    if (sWAge < 2) {
        return; /* the area's objects exist from its first frames; the events wait until then */
    }
    while (psp_np_world_take(&e)) {
        const int replay = (e.type & NP_W_REPLAY) != 0;
        e.type &= (uint8_t) ~NP_W_REPLAY;
        if (replay && e.type == PSP_NP_EV_DEAD && e.bhv >= NP_W_BRK_BASE) {
            e.value = 0; /* what was destroyed before this player came in: no explosion to show now */
        }
        if (e.level != sWLevel || (e.area >> 4) != (sWArea >> 4)) {
            continue; /* another level, or another act of this one: a different copy of the course (see np_area_key) */
        }
        if (e.type == PSP_NP_EV_COIN && !replay) { /* the coin count is shared among the players of a level, like sm64coopdx */
            const s16 old = m->numCoins;
            m->numCoins = (s16) ((e.value > m->numCoins + e.coinValue) ? e.value : m->numCoins + e.coinValue);
            if (COURSE_IS_MAIN_COURSE(gCurrCourseNum) && old < 100 && m->numCoins >= 100 && e.area == sWArea) {
                bhv_spawn_star_no_level_exit(6);
            }
        }
        if (e.area != sWArea) {
            continue;
        }
        if (e.type == PSP_NP_EV_DIGEST) {
            if (e.value > sWLogN && sResyncN < 3) {
                sResyncN++;
                memset(&e, 0, sizeof(e));
                e.type = PSP_NP_EV_RESYNC;
                e.level = (uint8_t) sWLevel;
                e.area = (uint8_t) sWArea;
                psp_np_world_send(&e);
            }
            continue;
        }
        if (e.type == PSP_NP_EV_RESYNC) {
            for (i = 0; i < sWLogN; i++) {
                PspNpWorldEvent r = sWLog[i];
                r.type |= NP_W_REPLAY;
                psp_np_world_send(&r);
            }
            continue;
        }
        if (e.type != PSP_NP_EV_ATTACK && e.type != PSP_NP_EV_SPAWN && e.type != PSP_NP_EV_STARSPAWN && e.type != PSP_NP_EV_DIGEST
            && e.type != PSP_NP_EV_RESYNC && (e.type != PSP_NP_EV_ACTION || e.coinValue == 7)) {
            np_w_log(&e);
        }
        if (!np_w_apply(&e) && sWPendN < NP_W_PEND) {
            sWPend[sWPendN] = e;
            sWPendAge[sWPendN++] = 0;
        }
    }
    for (i = 0; i < sWPendN;) { /* the ones whose object was not there: tried again for a while */
        if (np_w_apply(&sWPend[i]) || ++sWPendAge[i] > NP_W_RETRY) {
            sWPend[i] = sWPend[--sWPendN];
            sWPendAge[i] = sWPendAge[sWPendN];
        } else {
            i++;
        }
    }
}

#endif /* PSP_NETPLAY_WORLD */

#endif
