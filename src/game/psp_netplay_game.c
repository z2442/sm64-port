/* WLAN multiplayer, game side (see pc/psp_netplay.c for the network): captures this player's pose, and shows the other player as a second
 * Mario (Luigi, Toad, Waluigi, Wario) object. Like the castle's mirror Mario, the second model uses the same geo layout as Mario, so it shares Mario's body state
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
#include "ingame_menu.h"
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
#include "engine/surface_collision.h"
#include "object_fields.h"
#include "save_file.h"
#include "pc/psp_netplay.h"
#include "level_table.h"
#include "sound_init.h"

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
static int sLocalChar;  /* the character whose model the local Mario object has (PSP_CHAR_*) */
/* each character's model (sm64coopdx's characters, PSP_CHAR_* order): they all use Mario's geo callbacks and animations */
static const int sCharModel[PSP_CHAR_COUNT] = { MODEL_MARIO, MODEL_LUIGI, MODEL_TOAD_PLAYER, MODEL_WALUIGI, MODEL_WARIO };
static int np_char_model(int character) {
    return sCharModel[(character >= 0 && character < PSP_CHAR_COUNT) ? character : PSP_CHAR_MARIO];
}
static struct MarioBodyState sNetBody; /* the other player's body state: what the geo callbacks of its model read (see BODY_STATE in mario_misc.c) */
static u8 sVoiceSeqSeen;               /* last voice of the other player already played here */
static int sVoiceSeqKnown;
static u8 sSfxSeqSeen;                 /* last of the other player's action-bank sounds already played here */
static int sSfxSeqKnown;
static int sRemoteAge;
static int sRemoteHeadCam;                 /* the other player is in first person: sNetBody.headAngle is its camera's head rotation */
static int sRemoteSquish;                  /* frames left of the squash of the other player's model (jumped / ground pounded on) */

/* MarioState.flags <-> the 16 bits sent: the caps (bits 0-4) and punching / kicking / tripping (bits 20-22) */
#define NP_FLAGS_PACK(f) ((uint16_t) (((f) & MARIO_CAP_FLAGS) | ((((f) >> 20) & 7) << 5)))
#define NP_FLAGS_UNPACK(p) ((uint32_t) (((p) & MARIO_CAP_FLAGS) | ((((p) >> 5) & 7U) << 20)))

#define NP_PLAY_MODE_PAUSED 2 /* sCurrPlayMode value of the pause menu (the define is private to level_update.c) */

/* The other player's model as it left this area (sm64coopdx fades a player out where it was when it goes to another level or area, through a door, a
 * star or a death, instead of taking it away at once; and fades in the one that comes in). Only a picture: nothing of the game looks at it. */
#define NP_FADE_FULL 32
static struct Object *sGhost;
static struct MarioBodyState sGhostBody;
static int sGhostFade, sGhostBaseState;
static int sRemoteFade; /* the fade in of the model shown now, 0 .. NP_FADE_FULL */

static int np_fade_state(int modelState, int fade) {
    int a = fade * 8;
    if (fade >= NP_FADE_FULL) {
        return modelState;
    }
    if ((modelState & 0x100) && (modelState & 0xFF) < a) {
        a = modelState & 0xFF; /* (already see-through: the vanish cap) */
    }
    return (modelState & ~0xFF) | 0x100 | a;
}

static int np_ghost_ok(void) {
    return sGhost != NULL && sGhost != sRemote && sGhost->activeFlags != ACTIVE_FLAG_DEACTIVATED && sGhost->oBehParams2ndByte == NP_OBJ_MARK
           && sGhost->behavior == segmented_to_virtual(bhvStaticObject);
}

static void np_ghost_drop(void) {
    if (np_ghost_ok()) {
        obj_mark_for_deletion(sGhost);
    }
    sGhost = NULL;
}

static void np_ghost_step(int paused) {
    if (sGhost == NULL) {
        return;
    }
    if (!np_ghost_ok()) {
        sGhost = NULL; /* (gone with the area) */
        return;
    }
    if (!paused) {
        sGhostFade -= 2;
    }
    if (sGhostFade <= 0) {
        np_ghost_drop();
        return;
    }
    sGhostBody.modelState = (s16) np_fade_state(sGhostBaseState, sGhostFade);
}

struct MarioBodyState *psp_np_game_body_state(int index) {
    if (sGhost != NULL && gCurGraphNodeObject == &sGhost->header.gfx) {
        return &sGhostBody;
    }
    if (sRemote != NULL && (gCurGraphNodeObject == &sRemote->header.gfx || gCurGraphNodeObject == &gMirrorRemote)) { /* (its reflection in the castle mirror too) */
        return &sNetBody;
    }
    return &gBodyStates[index];
}

int psp_np_game_remote_head_cam(void) {
    return sRemoteHeadCam;
}

static int np_drawing_ghost(void) {
    return sGhost != NULL && gCurGraphNodeObject == &sGhost->header.gfx;
}

int psp_np_game_drawing_remote(void) {
    return (sRemote != NULL && (gCurGraphNodeObject == &sRemote->header.gfx || gCurGraphNodeObject == &gMirrorRemote)) || np_drawing_ghost();
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

extern struct Object *gPspBowserRewardOf; /* (bowser.inc.c) */
static int np_remote_alive(void) {
    return sRemote != NULL && sRemote->activeFlags != ACTIVE_FLAG_DEACTIVATED && sRemote->oBehParams2ndByte == NP_OBJ_MARK
           && sRemote->behavior == segmented_to_virtual(bhvStaticObject);
}

static void np_ghost_drop(void);
void psp_np_game_remove_remote(void) {
    if (np_remote_alive()) {
        obj_mark_for_deletion(sRemote);
    }
    if (psp_netplay_state() != PSP_NP_CONNECTED) {
        np_ghost_drop(); /* (the room is over) */
    }
    sRemote = NULL;
    sRemoteAge = 0;
    sRemoteSquish = 0;
}

struct MarioState *psp_np_game_remote_state(void) {
    return np_remote_alive() ? &sNetMs : NULL;
}

/* this player came down on the other one (interaction.c): its model is squashed here at once, and the other console is told to squash it there too.
 * That console's own test (the other player coming down on it) uses where this player was a few frames before, so it saw the landing only sometimes */
static uint8_t sStompSeq, sStompFrames;
void psp_np_game_remote_squish(int frames) {
    if (frames > sRemoteSquish) {
        sRemoteSquish = frames;
    }
    sStompSeq++;
    sStompFrames = (uint8_t) frames;
}

/* The other player's model is a picture (bhvStaticObject, in the default list): the collision pass of the game does not look at it. This is the test of
 * detect_object_hitbox_overlap with Mario's hitbox (bhvMario: radius 37, height 160) where the model stands, for the objects that react to any player
 * touching them (sm64coopdx loops over every MarioState there): the recovery heart, the pushable metal box. */
int psp_np_game_remote_touches(const struct Object *obj) {
    const struct Object *r = psp_np_game_remote();
    f32 dx, dz, bottom, top;
    if (r == NULL || obj == NULL) {
        return 0;
    }
    dx = r->oPosX - obj->oPosX;
    dz = r->oPosZ - obj->oPosZ;
    if (sqrtf(dx * dx + dz * dz) >= 37.0f + obj->hitboxRadius) {
        return 0;
    }
    bottom = obj->oPosY - obj->hitboxDownOffset;
    top = bottom + obj->hitboxHeight;
    return !(r->oPosY > top || r->oPosY + 160.0f < bottom);
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
#define NP_ACT_CREDITS 99 /* sm64coopdx: gCurrActStarNum = 99 from the start of the ending to the end of the credits */
static int np_act(void) {
    if (gCurrCreditsEntry != NULL) {
        return NP_ACT_CREDITS; /* (the same scene of the credits is the same place for both: each console plays them on its own) */
    }
    if (!COURSE_IS_MAIN_COURSE(gCurrCourseNum)) {
        return 0;
    }
    {
        /* sm64coopdx keys it on the star the player picked (gCurrActStarNum, set from gDialogCourseActNum at the act selector), not on the act whose
         * objects the game loaded (gCurrActNum). Those differ when a player picks a star it already has after one it lacks: the game then loads the
         * act of the first missing star (lvl_update_obj_and_load_act_button_actions). Keyed on gCurrActNum, two different stars of Tick Tock Clock
         * put both players in the same place. (Both consoles play the host's save file, as in sm64coopdx, so the same star loads the same act on both.) */
        return (gDialogCourseActNum >= 1 && gDialogCourseActNum <= 7) ? gDialogCourseActNum : (gCurrActNum & 7);
    }
}

/* sm64coopdx (packet_object.c, network.c): nothing of the world is synced while a player watches the ending and the credits, which are the same every time;
 * each console plays them by itself and only the players are seen (when both are in the same scene). */
int psp_np_world_quiet(void) {
    return gCurrCreditsEntry != NULL;
}

static int np_area_key(void) {
    return (gCurrAreaIndex & 15) | (np_act() << 4);
}

static void np_p_ident(const struct Object *o, uint8_t *kind, uint8_t *ord);
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
    if (gMarioState->action == ACT_IN_CANNON) { /* (the model is hidden in a cannon: the angles carry where the cannon aims, see bhv_cannon_base_loop) */
        out->angle[0] = gMarioState->faceAngle[0];
        out->angle[1] = gMarioState->faceAngle[1];
    }
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
    out->sfxSeq = gPspSfxSeq;
    memcpy(out->sfx, gPspSfxRing, sizeof(out->sfx));
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
        /* carried by something (the Hoot, a moving platform): its speed is not in vel[]. The other console carries the model on between packets with what it
         * really moved in the last frame, or it moved in steps at the packet rate (and so did the Hoot drawn above it) */
        static float sCapLast[3];
        static int sCapHave;
        if (sCapHave && (s->action == ACT_RIDING_HOOT || (s->marioObj != NULL && s->marioObj->platform != NULL))) {
            for (int i = 0; i < 3; i++) {
                const float d = out->pos[i] - sCapLast[i];
                if (fabsf(d) < 150.0f) {
                    out->vel[i] = (int16_t) d;
                }
            }
        }
        sCapLast[0] = out->pos[0];
        sCapLast[1] = out->pos[1];
        sCapLast[2] = out->pos[2];
        sCapHave = 1;
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
    out->stompSeq = sStompSeq;
    out->stompFrames = sStompFrames;
    out->character = gPspLocalChar;
    out->lives = (int8_t) (gMarioStates[0].numLives < -1 ? -1 : (gMarioStates[0].numLives > 127 ? 127 : gMarioStates[0].numLives));
    np_p_ident(gMarioObject != NULL ? gMarioObject->platform : NULL, &out->platKind, &out->platOrd);
    out->scaleXZ = (uint8_t) (m->header.gfx.scale[0] * 100.0f + 0.5f);
    out->scaleY = (uint8_t) (m->header.gfx.scale[1] * 100.0f + 0.5f);
    out->particles = (uint32_t) m->oMarioParticleFlags;
    return 1;
}

/* The other player's latest pose, kept even while its model is not shown (inside a cannon its Mario is invisible): see psp_np_game_remote_pose */
static PspNpPlayer sLastIn;
static int sLastInValid;

int psp_np_game_remote_pose(float pos[3], short angle[3], unsigned int *action) {
    if (!sLastInValid || sLastIn.level != (int16_t) gCurrLevelNum || sLastIn.area != (int16_t) gCurrAreaIndex || sLastIn.act != (uint8_t) np_act()) {
        return 0;
    }
    pos[0] = sLastIn.pos[0];
    pos[1] = sLastIn.pos[1];
    pos[2] = sLastIn.pos[2];
    angle[0] = sLastIn.angle[0];
    angle[1] = sLastIn.angle[1];
    angle[2] = sLastIn.angle[2];
    *action = sLastIn.action;
    return 1;
}

/* Shows the other player (in == NULL: hides it), with the model of the character it plays as (in->character). */
/* The particles of the other player's model (sm64coopdx runs every player's particles): the flags of its last frame make the same spawners as the local Mario's
 * (spawn_mario_particles, object_list_processor.c) around its model. A burst (a ground pound's stars, a splash) is made when its flag comes up, not again for
 * every frame the same packet stands. The idle water wave and the leaves follow the local Mario's state, so they are left out. */
#define NP_PARTICLES_BURST (PARTICLE_VERTICAL_STAR | PARTICLE_HORIZONTAL_STAR | PARTICLE_WATER_SPLASH | PARTICLE_PLUNGE_BUBBLE | PARTICLE_SHALLOW_WATER_SPLASH \
                            | PARTICLE_MIST_CIRCLE | PARTICLE_TRIANGLE)
#define NP_PARTICLES_SKIP (PARTICLE_IDLE_WATER_WAVE | PARTICLE_LEAF)
static void np_remote_particles(struct Object *o, u32 flags, int paused) {
    static u32 sPrev;
    struct Object *prevObj = gCurrentObject;
    if (paused || o == NULL) {
        return;
    }
    o->oMoveAngleYaw = o->oFaceAngleYaw;
    o->oMarioParticleFlags = (s32) flags; /* (the particles that last while it is set look at it, as they do at Mario's) */
    gCurrentObject = o;
    spawn_mario_particles(flags & ~NP_PARTICLES_SKIP & ~(sPrev & NP_PARTICLES_BURST));
    gCurrentObject = prevObj;
    sPrev = flags;
}

static struct Object *sRemBubble; /* the bubble around the other player's model (ACT_BUBBLED) */
static s16 sYawRate; /* the other player's turning speed while it swings Bowser (psp_np_game_apply) */
static int sYawAge;
void psp_np_game_apply(const PspNpPlayer *in) {
    struct Object *o;
    const int model = np_char_model(in != NULL ? in->character : PSP_CHAR_MARIO);
    /* this console is in a pause menu that stops the game (not the multiplayer one, see psp_live_pause): no object is updated, so the model is
     * moved by hand */
    const int paused = (sCurrPlayMode == NP_PLAY_MODE_PAUSED && !psp_live_pause());

    sLastInValid = (in != NULL);
    if (in != NULL) {
        sLastIn = *in;
    }
    {
        static int sStompSeen = -1;
        if (in != NULL) {
            if (sStompSeen >= 0 && in->stompSeq != (uint8_t) sStompSeen && gMarioState->bounceSquishTimer < in->stompFrames) {
                gMarioState->bounceSquishTimer = in->stompFrames; /* (the other player landed on this one: see psp_np_game_remote_squish) */
            }
            sStompSeen = in->stompSeq;
        }
    }
    /* (Each player keeps its own lives: they used to come down to the other player's count whenever it lost one, which could take them
     * below 0.) */
    /* The grand star (sm64coopdx, network_receive_player): every player flies off with it. Each console plays the cutscene of its own player, and draws
     * the other one 100 to the side (and, flying, 100 lower: jumbo_star_cutscene_flying with playerIndex 1), as each sm64coopdx client does with the
     * other players. A player whose console missed the taking goes too as soon as the other one is in the cutscene. The ending and the credits are not
     * changed: every player stands where its own console puts it, as there. */
    PspNpPlayer adj;
    static int sJumboFly;
    if (in != NULL && in->action == ACT_JUMBO_STAR_CUTSCENE && in->level == LEVEL_BOWSER_3) {
        adj = *in;
        adj.pos[0] += 100.0f;
        if (in->anim == MARIO_ANIM_WING_CAP_FLY) {
            if (!paused && sJumboFly < 30) {
                sJumboFly++;
            }
            adj.pos[1] -= 100.0f * (f32) sJumboFly / 30.0f;
        } else {
            sJumboFly = 0;
        }
        in = &adj;
        if (gCurrLevelNum == LEVEL_BOWSER_3 && gMarioObject != NULL && gMarioState->action != ACT_JUMBO_STAR_CUTSCENE) {
            mario_stop_riding_and_holding(gMarioState);
            fadeout_level_music(126);
            set_mario_action(gMarioState, ACT_JUMBO_STAR_CUTSCENE, 0);
        }
    }

    np_ghost_step(paused);
    if (in == NULL || !in->visible || in->level != (int16_t) gCurrLevelNum || in->area != (int16_t) gCurrAreaIndex || in->act != (uint8_t) np_act() || gMarioObject == NULL
        || gCurrentArea == NULL || gLoadedGraphNodes[model] == NULL || !np_anim_init()) {
        if (in != NULL && in->visible && np_remote_alive() && sRemoteAge > 2 && gMarioObject != NULL
            && (in->level != (int16_t) gCurrLevelNum || in->area != (int16_t) gCurrAreaIndex || in->act != (uint8_t) np_act())) {
            /* it went somewhere else: its model stays where it was and fades out */
            np_ghost_drop();
            sGhost = sRemote;
            sGhostBody = sNetBody;
            sGhostBaseState = sNetBody.modelState;
            sGhostFade = sRemoteFade;
            sGhost->oIntangibleTimer = -1;
            sRemote = NULL;
        }
        psp_np_game_remove_remote();
        sVoiceSeqKnown = 0;
        sSfxSeqKnown = 0;
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
        sRemoteFade = 0; /* it fades in */
        o = sRemote;
        o->oBehParams2ndByte = NP_OBJ_MARK;
        o->oFlags |= OBJ_FLAG_ACTIVE_FROM_AFAR;
        o->oDrawingDistance = 20000.0f;
        o->oPosX = in->pos[0];
        o->oPosY = in->pos[1];
        o->oPosZ = in->pos[2];
        o->oFaceAngleYaw = in->angle[1];
        o->header.gfx.unk38.animID = -1; /* forces the first animation to be set */
    }
    o = sRemote;
    if (!paused && sRemoteAge < 1000000) {
        sRemoteAge++;
    }

    /* body state of the model (read by the geo callbacks, see BODY_STATE in mario_misc.c) */
    sNetBody.action = in->action;
    if (!paused && sRemoteFade < NP_FADE_FULL) {
        sRemoteFade += 2;
    }
    sNetBody.modelState = (s16) np_fade_state(in->modelState, sRemoteFade);
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
    int snapped = 0;
    {
        static float sLastPos[3];
        static uint32_t sLastAct;
        static int16_t sLastFrame, sLastAnim;
        static int sAge;
        static s16 sYawLast;
        static int sStill; /* the last packet put it where the one before did: it stands still there, whatever its speed says */
        float tx = in->pos[0], ty = in->pos[1], tz = in->pos[2];
        if (!paused) {
            if (in->pos[0] != sLastPos[0] || in->pos[1] != sLastPos[1] || in->pos[2] != sLastPos[2] || in->action != sLastAct || in->frame != sLastFrame
                || in->anim != sLastAnim) {
                /* (the speed of a player that does not move is not always 0: the spin of a ground pound keeps it in the air with a fall speed of -50, a
                 * player that swings Bowser keeps the speed it had. Carried on, the model went down / up and came back with the next packet) */
                sStill = in->pos[0] == sLastPos[0] && in->pos[1] == sLastPos[1] && in->pos[2] == sLastPos[2];
                sLastPos[0] = in->pos[0]; sLastPos[1] = in->pos[1]; sLastPos[2] = in->pos[2];
                sLastAct = in->action; sLastFrame = in->frame; sLastAnim = in->anim;
                /* how fast it turns, from the last two packets (frames between them: the age the last one reached + 1) */
                if (in->action != ACT_HOLDING_BOWSER) {
                    sYawRate = 0;
                } else if (sAge < 6) { /* (further apart, a whole turn could hide between them: the last speed stands) */
                    sYawRate = (s16) ((s16) (in->angle[1] - sYawLast) / (sAge + 1));
                }
                sYawLast = in->angle[1];
                sAge = 0; /* a new packet */
            } else if (sAge < 1000) {
                sAge++;
            }
        }
        if (sAge > 0 && !in->frozen && !sStill) {
            const int n = sAge > NP_EXTRAP_MAX ? NP_EXTRAP_MAX : sAge;
            tx += (float) in->vel[0] * (float) n;
            ty += (float) in->vel[1] * (float) n;
            tz += (float) in->vel[2] * (float) n;
        }
        if (fabsf(in->pos[0] - o->oPosX) + fabsf(in->pos[2] - o->oPosZ) > NP_SNAP_DIST || fabsf(in->pos[1] - o->oPosY) > NP_SNAP_DIST) {
            o->oPosX = in->pos[0];
            o->oPosY = in->pos[1];
            o->oPosZ = in->pos[2];
            snapped = 1;
        } else {
            o->oPosX += (tx - o->oPosX) * 0.5f;
            o->oPosY += (ty - o->oPosY) * 0.5f;
            o->oPosZ += (tz - o->oPosZ) * 0.5f;
        }
        sYawAge = (sAge > NP_EXTRAP_MAX) ? NP_EXTRAP_MAX : sAge;
    }
    o->oFaceAnglePitch = in->angle[0];
    /* the facing eases toward the packet's too (half the way each frame): taken as it comes, the model turned in steps of 20-40 degrees at the packet
     * rate, and so did whatever it carries (np_w_in_remote_hands). A turn around still ends within a few frames. A snap with the position. */
    if (snapped) {
        o->oFaceAngleYaw = in->angle[1];
    } else if (in->action == ACT_HOLDING_BOWSER) {
        /* swinging Bowser it turns up to 22 degrees a frame: eased, the model (and Bowser in its hands) trailed a quarter turn or more behind the other console.
         * It turns on at the speed of the last packets instead, from the last one's facing */
        o->oFaceAngleYaw = (s16) (in->angle[1] + sYawRate * sYawAge);
    } else {
        o->oFaceAngleYaw = (s16) (o->oFaceAngleYaw + (s16) (in->angle[1] - (s16) o->oFaceAngleYaw) / 2);
    }
    o->oFaceAngleRoll = in->angle[2];
    if (paused) {
        /* the object update (which copies these into the graph node) does not run during the pause: it is done here so the other player keeps moving */
        o->header.gfx.pos[0] = o->oPosX;
        o->header.gfx.pos[1] = o->oPosY;
        o->header.gfx.pos[2] = o->oPosZ;
        o->header.gfx.angle[0] = in->angle[0];
        o->header.gfx.angle[1] = (s16) o->oFaceAngleYaw;
        o->header.gfx.angle[2] = in->angle[2];
    }

    /* its star (or key) dance: the star spins up around its model with its sparkles, as the game does for Mario (sm64coopdx shows it for every player) */
    {
        static uint32_t sPrevAct;
        const int danceNow = in->action == ACT_STAR_DANCE_EXIT || in->action == ACT_STAR_DANCE_NO_EXIT || in->action == ACT_STAR_DANCE_WATER;
        const int dancePrev = sPrevAct == ACT_STAR_DANCE_EXIT || sPrevAct == ACT_STAR_DANCE_NO_EXIT || sPrevAct == ACT_STAR_DANCE_WATER;
        if (danceNow && !dancePrev && !paused && sRemoteAge > 1) {
            spawn_object(o, MODEL_STAR, bhvCelebrationStar);
        }
        sPrevAct = in->action;
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
    /* its squash: what the other console's model is scaled to (a hard landing, an object that squashes it, a jump on it), as it came in the packet. The rubber band of
     * a squash lasts 16 frames and the packets come every 2-3: between them the scale of the last one stands. A squash this player makes on it (a jump on its head)
     * shows at once, before the packet that tells of it. */
    if (in->scaleXZ != 100 || in->scaleY != 100) {
        o->header.gfx.scale[0] = o->header.gfx.scale[2] = (float) in->scaleXZ / 100.0f;
        o->header.gfx.scale[1] = (float) in->scaleY / 100.0f;
        if (sRemoteSquish > 0 && !paused) {
            sRemoteSquish--;
        }
    } else if (sRemoteSquish > 0) {
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
    /* the blink after a hit: the model of a player with invincibility frames is drawn every other frame (mario.c), the other player's too */
    if (in->invincTimer >= 3 && (gGlobalTimer & 1)) {
        o->header.gfx.node.flags |= GRAPH_RENDER_INVISIBLE;
    } else {
        o->header.gfx.node.flags &= ~GRAPH_RENDER_INVISIBLE;
    }

    np_remote_particles(o, in->particles, paused);
    /* its bubble (sm64coopdx): it pops by itself when the other player is out of it (bhv_bubble_player_loop) */
    if (in->action == ACT_BUBBLED && !paused && (sRemBubble == NULL || sRemBubble->activeFlags == ACTIVE_FLAG_DEACTIVATED
                                                || sRemBubble->behavior != segmented_to_virtual(bhvBubblePlayer) || sRemBubble->parentObj != o)) {
        sRemBubble = spawn_object(o, MODEL_BUBBLE_PLAYER, bhvBubblePlayer);
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

    /* its snoring goes on until it is stopped, and nothing here stops it when the other player wakes up: it would stay in the voice channel and block the voices
     * of this player (what its own console does in act_sleeping) */
    {
        static u8 sRemoteSleeping;
        if (in->action == ACT_SLEEPING) {
            sRemoteSleeping = 1;
        } else if (sRemoteSleeping) {
            sRemoteSleeping = 0;
            func_803205E8(SOUND_MARIO_SNORING1, o->header.gfx.cameraToObject);
            func_803205E8(SOUND_MARIO_SNORING2, o->header.gfx.cameraToObject);
            func_803205E8(SOUND_MARIO_SNORING3, o->header.gfx.cameraToObject);
        }
    }

    /* its voice (its character's) is played here, from its position */
    if (!sVoiceSeqKnown) {
        sVoiceSeqSeen = in->voiceSeq;
        sVoiceSeqKnown = 1;
    } else if (in->voiceSeq != sVoiceSeqSeen) {
        sVoiceSeqSeen = in->voiceSeq;
        if (in->voice != 0) {
            psp_play_remote_voice(in->voice, in->character, o->header.gfx.cameraToObject);
        }
    }

    /* its steps, jumps, landings... (patch 312: sm64coopdx plays every player's sounds), from its position. The ones since the last state that came,
     * up to the four the state keeps (a lost state loses the older ones) */
    if (!sSfxSeqKnown) {
        sSfxSeqSeen = in->sfxSeq;
        sSfxSeqKnown = 1;
    } else if (in->sfxSeq != sSfxSeqSeen) {
        u8 n = (u8) (in->sfxSeq - sSfxSeqSeen);
        if (n > 4) {
            n = 4;
        }
        while (n > 0) {
            const u32 bits = in->sfx[(u8) (in->sfxSeq - n) & 3];
            if (bits != 0 && o->header.gfx.node.flags & GRAPH_RENDER_ACTIVE) {
                psp_play_remote_sfx(bits, o->header.gfx.cameraToObject);
            }
            n--;
        }
        sSfxSeqSeen = in->sfxSeq;
    }
}

/* In a room this player plays as the character chosen on the Multiplayer page: the local Mario object gets that character's model and voice (and
 * goes back to Mario's when the room ends). After a level / area change Mario is created again with his own model, so it is checked every frame. */
void psp_np_game_keep_local_char(int character) {
    const int model = np_char_model(character);
    if (character < 0 || character >= PSP_CHAR_COUNT) {
        character = PSP_CHAR_MARIO;
    }
    gPspLocalChar = (u8) character;
    if (gMarioObject == NULL || gLoadedGraphNodes[model] == NULL) {
        return;
    }
    if (gMarioObject->header.gfx.sharedChild != gLoadedGraphNodes[model]
        && (character != PSP_CHAR_MARIO || sLocalChar != PSP_CHAR_MARIO)) {
        gMarioObject->header.gfx.sharedChild = gLoadedGraphNodes[model];
    }
    sLocalChar = character;
}


/* ---- The characters' caps (sm64coopdx Character.capModelId...): the cap object of a player (blown off, in a box, dropped by an enemy) and the one an enemy
 * holds are the cap of that player's character ---- */
static const int sCharCapModel[PSP_CHAR_COUNT][4] = { /* normal, metal, wing, winged metal */
    { MODEL_MARIOS_CAP, MODEL_MARIOS_METAL_CAP, MODEL_MARIOS_WING_CAP, MODEL_MARIOS_WINGED_METAL_CAP },
    { MODEL_LUIGIS_CAP, MODEL_LUIGIS_METAL_CAP, MODEL_LUIGIS_WING_CAP, MODEL_LUIGIS_WINGED_METAL_CAP },
    { MODEL_TOADS_CAP, MODEL_TOADS_METAL_CAP, MODEL_TOADS_WING_CAP, MODEL_TOADS_WINGED_METAL_CAP },
    { MODEL_WALUIGIS_CAP, MODEL_WALUIGIS_METAL_CAP, MODEL_WALUIGIS_WING_CAP, MODEL_WALUIGIS_WINGED_METAL_CAP },
    { MODEL_WARIOS_CAP, MODEL_WARIOS_METAL_CAP, MODEL_WARIOS_WING_CAP, MODEL_WARIOS_WINGED_METAL_CAP },
};

/* One of Mario's cap models as the cap of a character (PSP_CHAR_*); any other model is returned as it is */
int psp_np_cap_model(int character, int model) {
    int k, m;
    if (character <= PSP_CHAR_MARIO || character >= PSP_CHAR_COUNT) {
        return model;
    }
    for (k = 0; k < 4; k++) {
        if (model == sCharCapModel[PSP_CHAR_MARIO][k]) {
            m = sCharCapModel[character][k];
            return gLoadedGraphNodes[m] != NULL ? m : model;
        }
    }
    return model;
}

/* The other player's character (PSP_CHAR_*), as its last state said (Mario if none) */
int psp_np_remote_char(void) {
    return (sLastInValid && sLastIn.character < PSP_CHAR_COUNT) ? sLastIn.character : PSP_CHAR_MARIO;
}

/* The cap a Klepto, an Ukiki or a Mr. Blizzard holds (sm64coopdx geo_mario_cap_display_list): the cap of the player it took it from. Who that is was
 * written in the object's unused1 (character + 1) when it took it here; else, the local player's if it is the one without its cap (the other player's
 * otherwise). */
extern const Gfx mario_cap_seg3_dl_03022F48[], luigi_cap_seg3_dl_03022F48[], waluigi_cap_seg3_dl_03022F48[], wario_cap_seg3_dl_03022F48[];
extern const Gfx toad_cap_mesh_layer_1[], toad_cap_mesh_layer_5[], toad_cap_material_revert_render_settings[];
static const Gfx sToadCapEnemyDl[] = {
    gsSPDisplayList(toad_cap_mesh_layer_1),
    gsSPDisplayList(toad_cap_mesh_layer_5),
    gsSPDisplayList(toad_cap_material_revert_render_settings),
    gsSPEndDisplayList(),
};
int psp_np_enemy_cap_char(struct Object *o) {
    int c;
    if (o != NULL && o->unused1 > 0 && o->unused1 <= PSP_CHAR_COUNT) {
        c = (int) o->unused1 - 1;
    } else if (psp_netplay_state() == PSP_NP_CONNECTED && gMarioState != NULL && (gMarioState->flags & MARIO_NORMAL_CAP)) {
        c = psp_np_remote_char();
    } else {
        c = gPspLocalChar;
    }
    return (c >= 0 && c < PSP_CHAR_COUNT) ? c : PSP_CHAR_MARIO;
}

Gfx *geo_psp_enemy_cap(s32 callContext, struct GraphNode *node, UNUSED Mat4 *mtx) {
    static const Gfx *const sDl[PSP_CHAR_COUNT] = { mario_cap_seg3_dl_03022F48, luigi_cap_seg3_dl_03022F48, sToadCapEnemyDl, waluigi_cap_seg3_dl_03022F48,
                                                    wario_cap_seg3_dl_03022F48 };
    struct GraphNodeGenerated *gen = (struct GraphNodeGenerated *) node;
    int c;
    if (callContext != GEO_CONTEXT_RENDER) {
        return NULL;
    }
    c = psp_np_enemy_cap_char((struct Object *) gCurGraphNodeObject);
    gen->fnNode.node.flags = (gen->fnNode.node.flags & 0xFF) | ((c == PSP_CHAR_TOAD ? LAYER_ALPHA : LAYER_OPAQUE) << 8);
    return (Gfx *) sDl[c];
}

#if PSP_NETPLAY_WORLD
/* ---- World events: what one player picks up (coins, stars, caps, 1-ups) disappears for the other one too ----
 * Like sm64coopdx (PACKET_COLLECT_COIN / COLLECT_STAR / COLLECT_ITEM), the object is not identified by a number but by its behavior and
 * position: the receiver takes the nearest object of that behavior. Both consoles run the same level, so the objects are where the sender
 * saw them. Coins also share the coin count (and the 100-coin star), as in sm64coopdx.
 * The area's collected items are kept in a log: when the other player comes into the area they are replayed (flag 0x80, so the coin count
 * is not touched again), and when this player enters an area the other one is already in, the other console replays its own. */
/* patch 314: room for everything a thorough player does in one area (every coin of a level, its enemies, items...). sm64coopdx hands a player that
 * comes in the whole state of the area; with 160 entries the rest was lost and coins already taken came back for the newcomer. The replay is sent a
 * piece per frame (NP_W_REPLAY_STEP) so it fits the queue of events (NP_EV_OUT in psp_netplay.c) */
#define NP_W_LOG 512
#define NP_W_REPLAY_STEP 24
#define NP_W_PEND 64
#define NP_W_RETRY_REPLAY 24 /* (what was collected before this player came in: if its object is not there by now it is not coming, and it must not take a new one) */
#define NP_W_RETRY 90 /* frames an event waits for its object (it may not exist yet, or may have been collected here at the same time) */
#define NP_W_REPLAY 0x80
static PspNpWorldEvent sWLog[NP_W_LOG];
static int sWLogN;
static int sWReplayAt = -1; /* next entry of the log to replay to the other player (-1: no replay going on) */
static PspNpWorldEvent sWPend[NP_W_PEND];
static u8 sWPendAge[NP_W_PEND];
static u8 sWPendReplay[NP_W_PEND];
static int sWPendN;
static int sWLevel = -1, sWArea = -1, sWAge;
static u32 sWAreaFrames; /* frames since this console came into the area (who was here first: np_w_level_vars_apply) */
static int sWPeerHere;
static int sPeerNowKnown, sPeerNowLevel, sPeerNowArea; /* where the other player was last reported (every frame, even while an area is being loaded) */
static int sWHost;

static s32 np_w_bhv(const struct Object *o) {
    return (s32) ((uintptr_t) virtual_to_segmented(0x13, o->behavior) - (uintptr_t) bhvYellowCoin);
}

static s32 np_w_bhv_of(const BehaviorScript *script) {
    return (s32) ((uintptr_t) script - (uintptr_t) bhvYellowCoin);
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

/* patch 319: the state of the area for a player that comes in, as sm64coopdx sends it (packet_level_macro.c, packet_level_spawn_info.c): every object of the
 * area's macro list and spawn list that was removed here (collected, killed, broken: the game marks it "do not respawn") goes by its index in that list, which
 * is the same on both consoles; a coin formation or a goomba triplet with some of its coins / goombas gone goes with its respawn bits. The other console
 * removes its copy and marks it the same way. Sent first, before the replay of the record. */
#define NP_LVL_MAX 384
#define NP_LVL_MACRO_DEL 0
#define NP_LVL_MACRO_SPECIAL 1
#define NP_LVL_SPAWN_DEL 2
#define NP_LVL_VARS 3 /* the level's own state (sm64coopdx network_send_level / network_send_area): TTC's speed, the water of WDW, the slide timer */
static struct { u8 kind; u16 index; s16 info; } sLvl[NP_LVL_MAX];
static void np_w_goomba_dead(struct Object *g);
static int sLvlN, sLvlAt, sLvlBuilt;

extern u8 sPssSlideStarted;       /* (interaction.c) */
extern s32 gWdwWaterLevelSet;     /* (moving_texture.c) */
extern float gPaintingMarioYEntry;
static void np_w_level_vars_pack(PspNpWorldEvent *l) {
    const u32 frames = sWAreaFrames;
    l->value = gTTCSpeedSetting;
    l->aux[0] = (s16) gEnvironmentLevels[0];
    l->aux[1] = (s16) (frames > 32767 ? 32767 : frames);
    l->aux[2] = (s16) gHudDisplay.timer;
    l->pos[0] = (s16) (((gHudDisplay.flags & HUD_DISPLAY_FLAG_TIMER) ? 1 : 0) | (sTimerRunning ? 2 : 0) | (sPssSlideStarted ? 4 : 0) | (sWHost ? 8 : 0));
    l->pos[1] = (s16) gPaintingMarioYEntry;
}

/* The level's own state from the other console: taken when that player was in the area first (sm64coopdx sends it to the one who comes in), or, when both
 * came in at about the same time, from the host. */
static void np_w_level_vars_apply(const PspNpWorldEvent *e) {
    const u32 theirs = (u32) (u16) e->aux[1];
    const int fromHost = (e->pos[0] & 8) != 0;
    if (!(theirs > sWAreaFrames + 30 || (theirs + 30 >= sWAreaFrames && sWAreaFrames + 30 >= theirs && fromHost))) {
        return; /* this console was here first: the other one takes this one's */
    }
    if (gCurrLevelNum == LEVEL_TTC && gTTCSpeedSetting != e->value && e->value >= 0 && e->value <= 3) {
        const s16 old = gTTCSpeedSetting;
        gTTCSpeedSetting = e->value;
        psp_np_ttc_speed_changed(old); /* (sm64coopdx network_receive_level: the objects started with the old speed) */
    }
    if (gCurrLevelNum == LEVEL_WDW && gEnvironmentRegions != NULL) {
        /* (sm64coopdx network_receive_area: the water of WDW, which the height its player jumped into the painting set) */
        s32 i;
        gPaintingMarioYEntry = (float) e->pos[1];
        gEnvironmentLevels[0] = e->aux[0];
        for (i = 0; i < *gEnvironmentRegions; i++) {
            gEnvironmentRegions[i * 6 + 6] = e->aux[0];
        }
        gWdwWaterLevelSet = 1; /* (its first picture does not set it again from this player's own jump) */
    }
    if ((e->pos[0] & 1) && !(gHudDisplay.flags & HUD_DISPLAY_FLAG_TIMER)) { /* the timer of the slide / the race (sm64coopdx: level control timer) */
        level_control_timer(TIMER_CONTROL_SHOW);
        gHudDisplay.timer = (u16) e->aux[2];
        if (e->pos[0] & 2) {
            level_control_timer(TIMER_CONTROL_START);
        }
        if (e->pos[0] & 4) {
            sPssSlideStarted = TRUE;
        }
    }
}

static void np_w_level_state_build(void) {
    struct SpawnInfo *si;
    s16 *list;
    u16 index;
    sLvlN = sLvlAt = 0;
    if (gCurrentArea == NULL) {
        return;
    }
    sLvl[sLvlN].kind = NP_LVL_VARS; /* (first: whatever follows may depend on it) */
    sLvlN++;
    for (list = gCurrentArea->macroObjects; list != NULL && *list != -1 && sLvlN < NP_LVL_MAX; list += 5) {
        s16 *respawnInfo = list + 4;
        if ((*list & 0x1FF) < 31) {
            break; /* (macro_special_objects.c: the end of the list) */
        }
        index = (u16) (respawnInfo - gCurrentArea->macroObjects);
        if (((*respawnInfo >> 8) & RESPAWN_INFO_DONT_RESPAWN) == RESPAWN_INFO_DONT_RESPAWN) {
            sLvl[sLvlN].kind = NP_LVL_MACRO_DEL;
        } else if ((*respawnInfo >> 8) & 0xFF) {
            sLvl[sLvlN].kind = NP_LVL_MACRO_SPECIAL; /* (some of its coins / goombas gone: their bits) */
        } else {
            continue;
        }
        sLvl[sLvlN].index = index;
        sLvl[sLvlN].info = *respawnInfo;
        sLvlN++;
    }
    for (si = gCurrentArea->objectSpawnInfos, index = 0; si != NULL && sLvlN < NP_LVL_MAX; si = si->next, index++) {
        if (((si->behaviorArg >> 8) & RESPAWN_INFO_DONT_RESPAWN) == RESPAWN_INFO_DONT_RESPAWN) {
            sLvl[sLvlN].kind = NP_LVL_SPAWN_DEL;
            sLvl[sLvlN].index = index;
            sLvl[sLvlN].info = 0;
            sLvlN++;
        }
    }
}

static struct Object *np_w_object_of_respawn_info(const void *respawnInfo) {
    int i;
    for (i = 0; i < NUM_OBJ_LISTS; i++) {
        struct ObjectNode *head = &gObjectLists[i];
        struct Object *obj;
        for (obj = (struct Object *) head->next; obj != (struct Object *) head; obj = (struct Object *) obj->header.next) {
            if (obj->activeFlags != ACTIVE_FLAG_DEACTIVATED && obj->respawnInfo == respawnInfo) {
                return obj;
            }
        }
    }
    return NULL;
}

/* (red coins and secrets count themselves in their star when they go: those are left to the replay of the record, which counts them) */
static int np_w_level_counted(const struct Object *o) {
    return o->behavior == segmented_to_virtual(bhvRedCoin) || o->behavior == segmented_to_virtual(bhvHiddenStarTrigger);
}

static void np_w_level_state_apply(const PspNpWorldEvent *e) {
    struct Object *o = NULL;
    if (gCurrentArea == NULL) {
        return;
    }
    if (e->coinValue == NP_LVL_MACRO_DEL || e->coinValue == NP_LVL_MACRO_SPECIAL) {
        s16 *respawnInfo;
        if (gCurrentArea->macroObjects == NULL) {
            return;
        }
        respawnInfo = gCurrentArea->macroObjects + (u16) e->value;
        o = np_w_object_of_respawn_info(respawnInfo);
        if (e->coinValue == NP_LVL_MACRO_DEL) {
            if (o != NULL && np_w_level_counted(o)) {
                return;
            }
            *respawnInfo |= RESPAWN_INFO_DONT_RESPAWN << 8;
            if (o != NULL) {
                obj_mark_for_deletion(o);
            }
            return;
        }
        *respawnInfo = e->aux[0];
        if (o == NULL) {
            return;
        }
        if (o->behavior == segmented_to_virtual(bhvCoinFormation)) {
            /* (sm64coopdx: the coins of the formation that are gone, by their bit) */
            int i;
            o->oBehParams = *respawnInfo;
            o->oCoinUnkF4 = (o->oBehParams >> 8) & 0xFF;
            for (i = 0; i < NUM_OBJ_LISTS; i++) {
                struct ObjectNode *head = &gObjectLists[i];
                struct Object *c;
                for (c = (struct Object *) head->next; c != (struct Object *) head; c = (struct Object *) c->header.next) {
                    if (c != o && c->parentObj == o && c->activeFlags != ACTIVE_FLAG_DEACTIVATED
                        && (c->behavior == segmented_to_virtual(bhvCoinFormationSpawn) || c->behavior == segmented_to_virtual(bhvYellowCoin))
                        && (o->oCoinUnkF4 & (1 << c->oBehParams2ndByte))) {
                        obj_mark_for_deletion(c);
                    }
                }
            }
        } else if (o->behavior == segmented_to_virtual(bhvGoombaTripletSpawner)) {
            /* (sm64coopdx: the goombas of the triplet that are dead, by their bit) */
            int i;
            o->oBehParams |= (((u32) (u16) *respawnInfo >> 8) & 0x3F) << 8; /* (goomba k: bit k of the respawn bits, bit 8 + k of the spawner, as mark_goomba_as_dead) */
            for (i = 0; i < NUM_OBJ_LISTS; i++) {
                struct ObjectNode *head = &gObjectLists[i];
                struct Object *g;
                for (g = (struct Object *) head->next; g != (struct Object *) head; g = (struct Object *) g->header.next) {
                    if (g != o && g->parentObj == o && g->activeFlags != ACTIVE_FLAG_DEACTIVATED && g->behavior == segmented_to_virtual(bhvGoomba)
                        && (((u16) *respawnInfo >> 8) & ((g->oBehParams2ndByte & GOOMBA_BP_TRIPLET_FLAG_MASK) >> 2))) {
                        np_w_goomba_dead(g);
                        obj_mark_for_deletion(g);
                    }
                }
            }
        }
        return;
    }
    if (e->coinValue == NP_LVL_VARS) {
        np_w_level_vars_apply(e);
        return;
    }
    if (e->coinValue == NP_LVL_SPAWN_DEL) {
        struct SpawnInfo *si;
        u16 index = 0;
        for (si = gCurrentArea->objectSpawnInfos; si != NULL; si = si->next, index++) {
            if (index == (u16) e->value) {
                o = np_w_object_of_respawn_info(&si->behaviorArg);
                if (o != NULL && np_w_level_counted(o)) {
                    return;
                }
                si->behaviorArg |= RESPAWN_INFO_DONT_RESPAWN << 8;
                if (o != NULL) {
                    obj_mark_for_deletion(o);
                }
                return;
            }
        }
    }
}

/* Sends the next piece of the log to the other player (while a replay is going on and it is still in this area): as many entries as the queue of events
 * takes, NP_W_REPLAY_STEP at most. A full queue is tried again on the next frame. */
static void np_w_replay_step(void) {
    int n;
    if (sWReplayAt < 0) {
        return;
    }
    if (!sWPeerHere) {
        sWReplayAt = -1;
        return;
    }
    if (!sLvlBuilt) {
        np_w_level_state_build();
        sLvlBuilt = 1;
    }
    for (n = 0; n < NP_W_REPLAY_STEP && sLvlAt < sLvlN; n++) { /* (the state of the area's lists goes first) */
        PspNpWorldEvent l;
        memset(&l, 0, sizeof(l));
        l.type = PSP_NP_EV_LEVEL;
        l.level = (uint8_t) sWLevel;
        l.area = (uint8_t) sWArea;
        l.coinValue = sLvl[sLvlAt].kind;
        l.value = (s16) sLvl[sLvlAt].index;
        l.aux[0] = sLvl[sLvlAt].info;
        if (l.coinValue == NP_LVL_VARS) {
            np_w_level_vars_pack(&l);
        }
        if (!psp_np_world_send(&l)) {
            return;
        }
        sLvlAt++;
    }
    if (sLvlAt < sLvlN) {
        return;
    }
    for (n = 0; n < NP_W_REPLAY_STEP && sWReplayAt < sWLogN; n++) {
        PspNpWorldEvent r = sWLog[sWReplayAt];
        if (r.type == PSP_NP_EV_COIN) { /* every coin of the replay carries the level's final count: the counter jumps to it at once, it does not climb */
            int k;
            for (k = 0; k < sWLogN; k++) {
                if (sWLog[k].type == PSP_NP_EV_COIN && sWLog[k].value > r.value) {
                    r.value = sWLog[k].value;
                }
            }
        }
        r.type |= NP_W_REPLAY;
        if (!psp_np_world_send(&r)) {
            return;
        }
        sWReplayAt++;
    }
    if (sWReplayAt >= sWLogN) {
        sWReplayAt = -1;
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
/* patch 308: the last red coin or secret was taken by the other player (sm64coopdx, oHiddenStarLastInteractedObject: only the player that completed them
 * gets the camera cutscene of their star and the frozen time; the other one just sees the star come out) */
int gPspNpStarByOther;

void psp_np_world_collected(struct MarioState *m, struct Object *o, int type) {
    PspNpWorldEvent e;
    if (psp_netplay_state() != PSP_NP_CONNECTED || o == NULL || o->behavior == NULL || gCurrentArea == NULL) {
        return;
    }
    if (type == PSP_NP_EV_COIN && o->behavior == segmented_to_virtual(bhvRedCoin)) {
        gPspNpStarByOther = 0;
    }
    if (type != PSP_NP_EV_1UP && type != PSP_NP_EV_ITEM && !(o->oInteractStatus & INT_STATUS_INTERACTED)) {
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
    bhvMadPiano, bhvFlyingBookend, bhvMantaRay, bhvSmallChillBully, bhvBigChillBully, bhvTweester, bhvUkiki,
    /* patch 293: the other two boos sm64coopdx syncs like the rest (the one in a cage and the ones of the castle) */
    bhvBooWithCage, bhvBooInCastle,
    /* patch 304: the small box a player can pick up and throw (sm64coopdx syncs it): the other player sees it carried, thrown and broken */
    bhvBreakableBoxSmall,
    /* patch 318: the big bully of Lethal Lava Land that comes down when its three small ones are knocked into the lava (sm64coopdx syncs every bully) */
    bhvBigBullyWithMinions };
#define NP_W_KINDS ((int) (sizeof(sKindBhv) / sizeof(sKindBhv[0])))
static const void *sKindVirt[NP_W_KINDS];
static int sKindInit;
static u32 sClock = 1000; /* counts the frames of the world sync; a followed object is stamped with it (Object.unused1) */
static unsigned sDgIn, sDgMiss, sDgOwn, sDgAtk, sDgAtkMiss, sDgDeadWait, sDgSpawn, sDgSpawnPar, sDgCoinOut, sDgCoinIn; /* diagnostics of the world sync (see psp_np_world_diag) */
static struct Object *sAtkObj; /* the enemy the other player hit last, with the direction it was facing (psp_np_attack_yaw) */
static s16 sAtkYaw;
static u32 sAtkT;
static int sPlatN, sPlatCursor, sPlatTick; /* the moving platforms (see np_p_frame) */
static u32 sBowserHeard;    /* sClock when a record of Bowser last came in from the other console (any state: even its intro tells it is alive) */
static u32 sBowserPeerWait; /* ... and when that record said Bowser there waits for ours (action 20: the other player has just come into the arena) */
static u32 sBowserCarried;  /* sClock when the other console last said its player carries Bowser by the tail (this copy swings around the other player's model meanwhile) */
static struct Object *sBowserCarriedObj; /* (that copy: drawn in the other player's hands, see psp_np_game_remote_held) */
#define NP_P_BOWSER_HELD_SLOT (NP_P_STD + 7) /* the oHeldState extra of Bowser */
#define NP_P_BOWSER_CARRY_FRAMES 6
static struct {
    struct Object *o;
    u8 kind;
    s16 home[3];
    struct Object *parent; /* (its parent when it was last seen: a spawner that unloads it, see np_w_unloaded) */
} sTrk[NP_W_TRK];

/* patch 319: an enemy that went away because its spawner unloaded it (every player went far from it: a goomba of a triplet, a cheep cheep) did not die.
 * Taken as a death, the other console killed its copy and marked the goomba dead in its spawner, and the record removed the goomba every time the triplet
 * came back on both consoles: the goombas of Bowser in the Sky stopped appearing. (sm64coopdx: a death is the object's own, an unload is not sent.) */
static int np_w_unloaded(const struct Object *o, const struct Object *parent) {
    if (parent == NULL || parent == o || parent->activeFlags == ACTIVE_FLAG_DEACTIVATED) {
        return 0;
    }
    if (o->behavior == segmented_to_virtual(bhvGoomba) && parent->behavior == segmented_to_virtual(bhvGoombaTripletSpawner)) {
        /* (its own death marks it in the spawner, mark_goomba_as_dead) */
        return parent->oAction == GOOMBA_TRIPLET_SPAWNER_ACT_UNLOADED && !(parent->oBehParams & ((o->oBehParams2ndByte & GOOMBA_BP_TRIPLET_FLAG_MASK) << 6));
    }
    if (o->behavior == segmented_to_virtual(bhvBub)) {
        return parent->oAction == 2; /* (bhv_bub_loop: its spawner went away) */
    }
    return 0;
}
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
#define NP_W_PAD_HANDS 0x01 /* (in the pad byte of a stream entry: the one that drives it has it in its hands right now, not just thrown a moment ago) */
#define NP_W_PAD_ANIM_SHIFT 1 /* patch 309: bits 1-4 of the pad byte = its oAnimState + 1 (0 = none): the copy does not run its script, so what that picks (the cap
                               * the ukiki or Klepto wears, eyes that blink) comes from the console that runs it */
static struct { const struct Object *o; u32 t; u8 hands; } sRemHeld[8];
static int sRemHeldNext;

static void np_w_remote_holds_set(const struct Object *o, int hands) {
    int i;
    for (i = 0; i < 8; i++) {
        if (sRemHeld[i].o == o) {
            sRemHeld[i].t = sClock;
            sRemHeld[i].hands = (u8) hands;
            return;
        }
    }
    sRemHeld[sRemHeldNext].o = o;
    sRemHeld[sRemHeldNext].t = sClock;
    sRemHeld[sRemHeldNext].hands = (u8) hands;
    sRemHeldNext = (sRemHeldNext + 1) & 7;
}

static void np_w_in_remote_hands(struct Object *o); /* (further down) */

/* (thrown, not in the hands: believed only a few frames past the last such stream. The thrower stops claiming it after NP_W_CLAIM_FRAMES and then simply
 * stops sending it if the nearest player is this one: waiting NP_W_HELD_FRAMES more here left it hanging in the air that long, both consoles driving
 * nothing, and then jumping when this console took over) */
#define NP_W_THROWN_FRAMES 8
static int np_w_remote_holds(const struct Object *o) {
    int i;
    for (i = 0; i < 8; i++) {
        if (sRemHeld[i].o == o && (sClock - sRemHeld[i].t) < (sRemHeld[i].hands ? NP_W_HELD_FRAMES : NP_W_THROWN_FRAMES)) {
            return 1;
        }
    }
    return 0;
}

/* the same, and it is in that player's hands (not flying from a throw: that one follows the stream like any other) */
static int np_w_remote_in_hands(const struct Object *o) {
    int i;
    for (i = 0; i < 8; i++) {
        if (sRemHeld[i].o == o && (sClock - sRemHeld[i].t) < NP_W_HELD_FRAMES) {
            return sRemHeld[i].hands;
        }
    }
    return 0;
}

/* what the other player has in its hands now: its model draws it there (geo_switch_mario_hand_grab_pos), as the local Mario draws his */
struct Object *psp_np_game_remote_held(void) {
    int i;
    if (!np_remote_alive() || np_drawing_ghost()) {
        return NULL;
    }
    for (i = 0; i < 8; i++) {
        if (sRemHeld[i].o != NULL && sRemHeld[i].hands && (sClock - sRemHeld[i].t) < NP_W_HELD_FRAMES && sRemHeld[i].o->activeFlags != ACTIVE_FLAG_DEACTIVATED) {
            return (struct Object *) sRemHeld[i].o;
        }
    }
    if (sBowserCarriedObj != NULL && sBowserCarriedObj->activeFlags != ACTIVE_FLAG_DEACTIVATED && sBowserCarriedObj->behavior == segmented_to_virtual(bhvBowser)
        && psp_np_world_bowser_carried(sBowserCarriedObj)) {
        /* patch 315: Bowser swung by the tail, as the local Mario draws him (GRAB_POS_BOWSER, at the end of the arms): he leans and rises with the
         * swing (act_holding_bowser tilts the model back with the speed). Hung at a fixed spot next to the model he did neither */
        return sBowserCarriedObj;
    }
    return NULL;
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

/* the same rule for the client at another position of the object (the one the host's copy is at) */
static int np_w_client_rule_at(const s16 *pos) {
    f32 dx, dy, dz, dMe, dIt;
    if (!sWPeerHere || !np_remote_alive() || gMarioObject == NULL) {
        return 1;
    }
    dx = pos[0] - gMarioObject->oPosX; dy = pos[1] - gMarioObject->oPosY; dz = pos[2] - gMarioObject->oPosZ;
    dMe = sqrtf(dx * dx + dy * dy + dz * dz);
    dx = pos[0] - sRemote->oPosX; dy = pos[1] - sRemote->oPosY; dz = pos[2] - sRemote->oPosZ;
    dIt = sqrtf(dx * dx + dy * dy + dz * dz);
    return dMe + NP_W_OWN_MARGIN < dIt;
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

/* The copy of an enemy the other console simulates (sm64coopdx: every client runs every object and the owner's updates overwrite it). Before its behavior
 * runs here, it reports what the interactions of the game did to it: the owner decides a hit (sm64coopdx sends the hit object right away), so the
 * behavior here does not see it. Returns 1 when the copy is only placed this frame: carried by the other player, or gone here. */
static struct Object *sFollowRun; /* the followed copy whose behavior is running now (psp_np_world_follow_running) */
void psp_np_world_follow_running(struct Object *o) {
    if (o == NULL && sFollowRun != NULL && sFollowRun->activeFlags == ACTIVE_FLAG_DEACTIVATED) {
        /* the copy ended its own death, where the owner's state took it (a squished goomba): what the owner did when the hit landed there is done here
         * too. A goomba of a triplet is marked as dead by its attack handler, which only ran on the owner's console (the rest of the deaths count themselves
         * in their own death code: boo, bully, fire piranha plant) */
        np_w_goomba_dead(sFollowRun);
    }
    sFollowRun = o;
}

int psp_np_world_follow_step(struct Object *o) {
    static u32 sAtkLast;
    static struct Object *sAtkPendObj; /* a hit that could not be sent yet: it is sent a frame later (and the behavior here still does not see it) */
    static u32 sAtkPendSt;
    u32 status = (u32) o->oInteractStatus;
    if (sAtkPendObj == o) {
        status |= sAtkPendSt;
        sAtkPendObj = NULL;
    }
    o->oInteractStatus = 0; /* (a grab or a talk makes it this console's own first: see np_w_owned) */
    if (np_w_unloaded(o, o->parentObj)) {
        obj_mark_for_deletion(o); /* (what its own script does: bhv_goomba_update, bhv_bub_loop) */
        return 1;
    }
    if ((status & (INT_STATUS_WAS_ATTACKED | INT_STATUS_ATTACKED_MARIO)) != 0) { /* it was hit, or it hurt the player here (it enters its "attacked Mario" action) */
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
        if (sClock - sAtkLast >= 3 && psp_np_world_send(&e)) { /* (not flooding the queue) */
            sAtkLast = sClock;
        } else {
            sAtkPendObj = o;
            sAtkPendSt = status;
        }
    }
    if (np_w_remote_in_hands(o)) {
        np_w_in_remote_hands(o); /* (carried by the other player: it goes along with that player's model every frame) */
        return 1;
    }
    return 0;
}

/* Bowser's flames (the breath, the blue ones of the sky) and his shock wave: sm64coopdx makes them on the console of the player nearest to him only and
 * sends them (network_send_spawn_objects), so both players see the same ones */
#define NP_LOOT_BFX 41 /* coinValue of a SPAWN event for one of them: bhv = which (see bowser.inc.c), value = its pitch */
static const BehaviorScript *const sBfxBhv[] = { bhvFlameMovingForwardGrowing, bhvBlueBowserFlame, bhvBowserShockWave };
static const u32 sBfxModel[] = { MODEL_RED_FLAME, MODEL_RED_FLAME, MODEL_BOWSER_WAVE };
#define NP_W_BFX ((int) (sizeof(sBfxBhv) / sizeof(sBfxBhv[0])))

/* The loot of an enemy (coins that jump out of it) is announced so the other console has it too. The coin behaviors are bhvSingleCoinGetsSpawned
 * (yellow) and bhvBlueCoinJumping (blue). Their direction and speed are chosen at random in their first update, so the announcement waits one
 * frame and carries what this console rolled; the other console makes the coin and, after its own first update, puts that on it. */
#define NP_W_LOOT 24
#define NP_LOOT_BALL 30   /* coinValue of a SPAWN event for what a spawner of sChildMir makes (bhv = its index there, value = its second parameter byte) */
/* Spawners that roll a random object now and then: only the console of the player nearest to the spawner runs them (psp_np_world_spawner_mine) and what
 * they make is announced, so the other console makes the same one (sm64coopdx: network_send_spawn_objects). */
static const BehaviorScript *const sChildMirParent[] = { bhvBobBowlingBallSpawner, bhvTtmBowlingBallSpawner, bhvThiBowlingBallSpawner, bhvBigBoulderGenerator,
    /* patch 308 (from here on the copy is made by this console's own spawner, found near the announced spot: see np_w_spawn_loot): the boos of the
     * merry-go-round of Big Boo's Haunt and its Big Boo, the flying books of the library */
    bhvMerryGoRoundBooManager, bhvMerryGoRoundBooManager, bhvBookendSpawn };
static const BehaviorScript *const sChildMirBhv[] = { bhvBowlingBall, bhvBowlingBall, bhvBowlingBall, bhvBigBoulder, bhvMerryGoRoundBoo, bhvMerryGoRoundBigBoo, bhvFlyingBookend };
static const u32 sChildMirModel[] = { MODEL_BOWLING_BALL, MODEL_BOWLING_BALL, MODEL_BOWLING_BALL, MODEL_HMC_ROLLING_ROCK, MODEL_BOO, MODEL_BOO, MODEL_BOOKEND };
#define NP_W_CHILDMIR_OWN 4 /* the first index whose copy comes from this console's own spawner */
static int sMirQuiet; /* (making the copy of an announced one: it is not announced back) */
#define NP_W_CHILDMIR ((int) (sizeof(sChildMirBhv) / sizeof(sChildMirBhv[0])))
#define NP_LOOT_MOVING 20 /* coinValue of a SPAWN event for a coin of bhvMovingYellowCoin: the ones a bob-omb, a money bag, a bully or a crate throws out (obj_spawn_yellow_coins) */
static struct { struct Object *o; u8 type; u8 age; s16 home[3]; } sLootOut[NP_W_LOOT];
static int sLootOutN;
static struct { struct Object *o; s16 vy, fv, yaw; } sLootFix[NP_W_LOOT];
static int sLootFixN;
#define NP_W_REDFIX 8
static struct { struct Object *star; s16 target; u8 age; } sRedFix[NP_W_REDFIX];
static int sRedFixN;

/* What an enemy that this console simulates shoots or drops, other than coins: the type of the SPAWN event is 3 + the index here. */
static const BehaviorScript *const sMirBhv[] = { bhvSnufitBalls, bhvMrIParticle, bhvKoopaShell, bhvExplosion,
    /* patch 310 (sm64coopdx sends these spawns too): the rock a monty mole throws, the 1-up of eight moles in a row, the rings of the manta ray */
    bhvMontyMoleRock, bhv1upWalking, bhvMantaRayWaterRing };
static const BehaviorScript *const sMirParent[] = { bhvSnufit, bhvMrI, bhvKoopa, bhvBobomb, bhvMontyMole, bhvMontyMole, bhvMantaRay };
static const u32 sMirModel[] = { MODEL_BOWLING_BALL, MODEL_PURPLE_MARBLE, MODEL_KOOPA_SHELL, MODEL_EXPLOSION, MODEL_PEBBLE, MODEL_1UP, MODEL_WATER_RING };
#define NP_W_MIR ((int) (sizeof(sMirBhv) / sizeof(sMirBhv[0])))

/* what the copy of a followed enemy must not make itself (the owner's console announces it: np_w_loot_step, psp_np_world_star) */
static int np_w_follow_dup(const struct Object *parent, const void *behavior) {
    int m;
    if (behavior == segmented_to_virtual(bhvSingleCoinGetsSpawned) || behavior == segmented_to_virtual(bhvBlueCoinJumping)
        || behavior == segmented_to_virtual(bhvMovingYellowCoin) || behavior == segmented_to_virtual(bhvStarSpawnCoordinates)
        || behavior == segmented_to_virtual(bhvSpawnedStar) || behavior == segmented_to_virtual(bhvSpawnedStarNoLevelExit)) {
        return 1;
    }
    for (m = 0; m < NP_W_MIR; m++) {
        if (behavior == segmented_to_virtual(sMirBhv[m]) && parent->behavior == segmented_to_virtual(sMirParent[m])) {
            return 1;
        }
    }
    return 0;
}

void psp_np_world_spawned(struct Object *parent, struct Object *obj, int model, const void *behavior) {
    int type;
    (void) model;
    if (sMirQuiet) {
        return;
    }
    if (sFollowRun != NULL && parent == sFollowRun && obj != NULL && psp_netplay_state() == PSP_NP_CONNECTED && np_w_follow_dup(parent, behavior)) {
        /* the copy of an enemy the other console simulates makes what that console announces (its coins, its shots, its star): made only once */
        obj->activeFlags = ACTIVE_FLAG_DEACTIVATED;
        obj->header.gfx.node.flags |= GRAPH_RENDER_INVISIBLE;
        obj->oIntangibleTimer = -1;
        obj->oInteractType = 0;
        return;
    }
    if (parent != NULL && behavior == bhvWaterBomb && parent->behavior == segmented_to_virtual(bhvWaterBombSpawner) && psp_netplay_state() == PSP_NP_CONNECTED
        && sLootOutN < NP_W_LOOT) { /* a bomb that falls on this player: the other one sees it fall too */
        sLootOut[sLootOutN].o = obj;
        sLootOut[sLootOutN].type = 2;
        sLootOut[sLootOutN].age = 0;
        sLootOutN++;
        return;
    }
    if (parent != NULL && obj != NULL && behavior == bhvSingleCoinGetsSpawned && psp_netplay_state() == PSP_NP_CONNECTED && sLootOutN < NP_W_LOOT
        && (parent->behavior == segmented_to_virtual(bhvExclamationBox) || parent->behavior == segmented_to_virtual(bhvThreeCoinsSpawn)
            || parent->behavior == segmented_to_virtual(bhvTenCoinsSpawn))) {
        /* patch 311: the coins of an exclamation box this player broke: the other console makes the same ones (its box makes none, see
         * exclamation_box_spawn_contents), so the coins are the same on both and taking one takes it on both */
        sLootOut[sLootOutN].o = obj;
        sLootOut[sLootOutN].type = 0;
        sLootOut[sLootOutN].age = 0;
        sLootOutN++;
        return;
    }
    if (parent != NULL && obj != NULL && psp_netplay_state() == PSP_NP_CONNECTED && sLootOutN < NP_W_LOOT) { /* what a spawner of sChildMir makes */
        int k;
        for (k = 0; k < NP_W_CHILDMIR; k++) {
            if (behavior == segmented_to_virtual(sChildMirBhv[k]) && parent->behavior == segmented_to_virtual(sChildMirParent[k])) {
                sLootOut[sLootOutN].o = obj;
                sLootOut[sLootOutN].type = NP_LOOT_BALL;
                sLootOut[sLootOutN].age = 0;
                sLootOut[sLootOutN].home[0] = (s16) k;
                sLootOutN++;
                return;
            }
        }
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

int psp_np_world_bowser_fx(const struct Object *bowser) {
    if (bowser == NULL || psp_netplay_state() != PSP_NP_CONNECTED || !sWPeerHere || !np_remote_alive() || gMarioObject == NULL) {
        return 1;
    }
    {
        const f32 dMe = dist_between_objects(bowser, gMarioObject), dIt = dist_between_objects(bowser, sRemote);
        return dMe < dIt || (dMe == dIt && sWHost); /* (sm64coopdx: nearest_mario_state_to_object(o)->playerIndex == 0) */
    }
}

void psp_np_world_bowser_fx_spawned(struct Object *obj, int which) {
    if (obj == NULL || which < 0 || which >= NP_W_BFX || psp_netplay_state() != PSP_NP_CONNECTED || !sWPeerHere || sLootOutN >= NP_W_LOOT) {
        return;
    }
    sLootOut[sLootOutN].o = obj;
    sLootOut[sLootOutN].type = NP_LOOT_BFX;
    sLootOut[sLootOutN].age = 0; /* (sent after its first update, which rolls where it starts) */
    sLootOut[sLootOutN].home[0] = (s16) which;
    sLootOutN++;
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
            if (e.coinValue == NP_LOOT_BFX) {
                e.bhv = sLootOut[i].home[0] | ((s32) c->oBehParams2ndByte << 8);
                e.value = (s16) c->oMoveAnglePitch;
            } else if (e.coinValue == NP_LOOT_BALL) { /* (a ball's path is its second parameter byte: the spawner sets it right after making it) */
                e.value = (s16) c->oBehParams2ndByte;
                e.bhv = sLootOut[i].home[0];
            } else if (e.coinValue == 2) { /* water bomb: where it is and what it is doing */
                e.value = (s16) c->oAction;
            } else if (e.coinValue >= 3 && e.coinValue != NP_LOOT_MOVING) { /* shot or dropped by an enemy: the parent is named by its home, so the other console can find its own copy */
                e.value = sLootOut[i].home[1];
                e.bhv = (s32) (((u32) (uint16_t) sLootOut[i].home[0]) | (((u32) (uint16_t) sLootOut[i].home[2]) << 16));
                if (c->behavior == segmented_to_virtual(bhvMantaRayWaterRing)) { /* (a ring does not move: its angles and its number instead) */
                    e.aux[0] = (s16) c->oFaceAnglePitch;
                    e.aux[1] = (s16) c->oWaterRingIndex;
                    e.aux[2] = (s16) c->oFaceAngleYaw;
                }
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

#define NP_LOOT_BWRD 40 /* coinValue of a SPAWN event for the key / grand star of a beaten Bowser (value 1 = grand star, aux = its yaw speed) */

void psp_np_world_bowser_reward(const struct Object *reward, int isStar) {
    PspNpWorldEvent e;
    if (reward == NULL || !psp_np_world_peer_here()) {
        return;
    }
    memset(&e, 0, sizeof(e));
    e.type = PSP_NP_EV_SPAWN;
    e.level = (uint8_t) gCurrLevelNum;
    e.area = (uint8_t) np_area_key();
    e.coinValue = NP_LOOT_BWRD;
    e.value = (s16) (isStar ? 1 : 0);
    e.pos[0] = (s16) reward->oPosX; e.pos[1] = (s16) reward->oPosY; e.pos[2] = (s16) reward->oPosZ;
    e.aux[0] = (s16) reward->oAngleVelYaw;
    psp_np_world_send(&e);
}

/* The other console beat Bowser: what its copy of the key / grand star does starts from where that one is (this console's copy of Bowser is not where the
 * other one's was), and the battle music ends here too (the fade-out belongs to the console that plays his last words). */
static void np_w_bowser_reward(const PspNpWorldEvent *e) {
    const BehaviorScript *bhv = e->value ? bhvGrandStar : bhvBowserKey;
    struct Object *r = np_w_find(np_w_bhv_of(bhv), e->pos, 100000.0f, 0), *bw;
    if (r == NULL) {
        bw = np_w_find(np_w_bhv_of(bhvBowser), e->pos, 100000.0f, 0);
        r = spawn_object(bw != NULL ? bw : gMarioObject, e->value ? MODEL_STAR : MODEL_BOWSER_KEY, bhv);
        if (r == NULL) {
            return;
        }
    }
    if (e->value && r->oAction != 0) {
        /* (a grand star of this console already on its way to the middle, with its camera cutscene: it is not started again) */
    } else {
        r->oPosX = e->pos[0];
        r->oPosY = e->pos[1];
        r->oPosZ = e->pos[2];
        r->oAngleVelYaw = e->aux[0];
        r->oAction = 0; /* (its fall / its wait starts again from there) */
        r->oTimer = 0;
        r->oVelY = 0.0f;
        r->oForwardVel = 0.0f;
        if (!e->value) {
            r->oVelY = 70.0f;
        }
    }
    gPspBowserRewardOf = np_w_find(np_w_bhv_of(bhvBowser), e->pos, 100000.0f, 0); /* (so np_p_apply does not leave another one) */
    gSecondCameraFocus = r;
    sequence_player_unlower(SEQ_PLAYER_LEVEL, 60);
    sequence_player_fade_out(0, 1);
}

static void np_w_spawn_loot(const PspNpWorldEvent *e) {
    struct Object *c;
    if (e->coinValue == NP_LOOT_BWRD) {
        np_w_bowser_reward(e);
        return;
    }
    if (e->coinValue == 2) { /* water bomb of the other player's spawner */
        if (np_remote_alive()) {
            /* its parent is this console's copy of that spawner (the one whose circle it fell in): while the bomb is alive the spawner has one out,
             * as on the other console (sm64coopdx syncs oWaterBombSpawnerBombActive), so if it becomes this console's to run it does not drop a second
             * one at once; the bomb clears it when it explodes. With no spawner found, the other player's model (the bomb writes into its parent) */
            struct Object *parent = sRemote, *ob;
            f32 best = 1.0e9f;
            int l;
            for (l = 0; l < NUM_OBJ_LISTS; l++) {
                struct ObjectNode *hd = &gObjectLists[l];
                if (hd->next == NULL) {
                    continue;
                }
                for (ob = (struct Object *) hd->next; ob != (struct Object *) hd; ob = (struct Object *) ob->header.next) {
                    if (ob->activeFlags != ACTIVE_FLAG_DEACTIVATED && ob->behavior == segmented_to_virtual(bhvWaterBombSpawner)) {
                        const f32 dx = ob->oPosX - e->pos[0], dz = ob->oPosZ - e->pos[2];
                        const f32 d = sqrtf(dx * dx + dz * dz);
                        if (d < 50 * (u16) (ob->oBehParams >> 16) + 200.0f + 1000.0f && d < best) {
                            best = d;
                            parent = ob;
                        }
                    }
                }
            }
            if (parent != sRemote) {
                parent->oWaterBombSpawnerBombActive = TRUE;
            }
            sMirQuiet = 1; /* (made from the other console's announcement: not announced back) */
            c = spawn_object(parent, MODEL_WATER_BOMB, bhvWaterBomb);
            if (c != NULL) {
                c->oPosX = e->pos[0];
                c->oPosY = e->pos[1];
                c->oPosZ = e->pos[2];
                c->oAction = e->value;
                c->oVelY = (f32) e->aux[0] / 32.0f;
                spawn_object(c, MODEL_WATER_BOMB_SHADOW, bhvWaterBombShadow);
            }
            sMirQuiet = 0;
        }
        return;
    }
    if (e->coinValue == NP_LOOT_BFX) {
        const int which = e->bhv & 0xFF;
        if (gMarioObject == NULL || which >= NP_W_BFX) {
            return;
        }
        sMirQuiet = 1;
        c = spawn_object(gMarioObject, sBfxModel[which], sBfxBhv[which]);
        sMirQuiet = 0;
        if (c != NULL) {
            c->oBehParams2ndByte = (e->bhv >> 8) & 0xFF;
            c->oPosX = e->pos[0];
            c->oPosY = e->pos[1];
            c->oPosZ = e->pos[2];
            c->oMoveAnglePitch = e->value;
            c->oMoveAngleYaw = c->oFaceAngleYaw = e->aux[2];
            c->oFaceAnglePitch = c->oFaceAngleRoll = c->oMoveAngleRoll = 0;
            if (sLootFixN < NP_W_LOOT) { /* (its own first update rolls a start; the announced one goes on after it) */
                sLootFix[sLootFixN].o = c;
                sLootFix[sLootFixN].vy = e->aux[0];
                sLootFix[sLootFixN].fv = e->aux[1];
                sLootFix[sLootFixN].yaw = e->aux[2];
                sLootFixN++;
            }
        }
        return;
    }
    if (e->coinValue == NP_LOOT_BALL && e->bhv >= NP_W_CHILDMIR_OWN && e->bhv < NP_W_CHILDMIR) {
        /* made by this console's own copy of the spawner (the boos count themselves in it, a book flies from it) */
        const void *pv = segmented_to_virtual(sChildMirParent[e->bhv]);
        struct Object *par = NULL, *obj;
        f32 best = 3000.0f;
        int i;
        for (i = 0; i < NUM_OBJ_LISTS; i++) {
            struct ObjectNode *head = &gObjectLists[i];
            for (obj = (struct Object *) head->next; obj != (struct Object *) head; obj = (struct Object *) obj->header.next) {
                if (np_w_alive(obj) && obj->behavior == pv) {
                    const f32 dx = obj->oPosX - e->pos[0], dz = obj->oPosZ - e->pos[2];
                    const f32 d = sqrtf(dx * dx + dz * dz);
                    if (d < best) {
                        best = d;
                        par = obj;
                    }
                }
            }
        }
        if (par == NULL) {
            return;
        }
        sMirQuiet = 1;
        c = spawn_object(par, sChildMirModel[e->bhv], sChildMirBhv[e->bhv]);
        sMirQuiet = 0;
        if (c == NULL) {
            return;
        }
        if (sChildMirBhv[e->bhv] == bhvMerryGoRoundBoo) {
            par->oMerryGoRoundBooManagerNumBoosSpawned++; /* (counted like a boo it made itself) */
        } else if (sChildMirBhv[e->bhv] == bhvMerryGoRoundBigBoo) {
            obj_copy_behavior_params(c, par);
            if (par->oAction != 2) {
                par->oAction = 2;
                play_puzzle_jingle();
            }
        } else { /* a flying book: from where the other console's came out, toward its player */
            c->oPosX = e->pos[0];
            c->oPosY = e->pos[1];
            c->oPosZ = e->pos[2];
            c->oAction = 3;
            c->oMoveAngleYaw = c->oFaceAngleYaw = e->aux[2];
        }
        return;
    }
    if (e->coinValue == NP_LOOT_BALL) {
        if (gMarioObject != NULL && e->bhv >= 0 && e->bhv < NP_W_CHILDMIR) {
            c = spawn_object(gMarioObject, sChildMirModel[e->bhv], sChildMirBhv[e->bhv]);
            if (c != NULL) {
                c->oPosX = e->pos[0];
                c->oPosY = e->pos[1];
                c->oPosZ = e->pos[2];
                c->oBehParams2ndByte = e->value;
                c->oMoveAngleYaw = c->oFaceAngleYaw = e->aux[2];
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
        sMirQuiet = 1; /* (a copy is not announced back: with both consoles driving the parent for a moment, each copy came back as a new one, over and over) */
        c = spawn_object(par != NULL ? par : gMarioObject, sMirModel[m], sMirBhv[m]);
        sMirQuiet = 0;
        if (c != NULL) {
            if (sMirBhv[m] == bhvExplosion) {
                c->oGraphYOffset += 100.0f; /* (like bobomb_act_explode) */
            }
            c->oPosX = e->pos[0];
            c->oPosY = e->pos[1];
            c->oPosZ = e->pos[2];
            if (sMirBhv[m] == bhvMantaRayWaterRing) {
                /* a ring of the manta ray: where and how the other console's stands, with its number (passed ones are told by it, see np_w_sig_apply). Its
                 * own init has not run yet: it works its normal out of these angles */
                c->oFaceAnglePitch = e->aux[0];
                c->oFaceAngleYaw = e->aux[2];
                c->oWaterRingIndex = e->aux[1];
                if (par != NULL && par->parentObj != NULL && par->parentObj != par && par->parentObj->oWaterRingMgrNextRingIndex <= e->aux[1]) {
                    par->parentObj->oWaterRingMgrNextRingIndex = e->aux[1] + 1; /* (if this console makes the next ones, they go on from here) */
                }
                return;
            }
            if (sMirBhv[m] == bhvMontyMoleRock && par != NULL) {
                par->prevObj = c; /* the mole holds it until it throws it (see np_w_follow_apply) */
            }
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

/* The other player carries it: on that console it is drawn in that player's hands and stays where it was picked up, so here it is put in front of the other
 * player's model (where a carried Bob-omb goes: cur_obj_set_pos_relative(Mario, 0, 60, 100)). Every frame (see psp_np_world_follow_step): from the stream alone
 * it would only move when a state comes in, and jump. */
/* the objects that np_w_in_remote_hands keeps from being drawn by themselves: they are shown again as soon as they leave the other player's hands */
static struct { struct Object *o; const BehaviorScript *bhv; } sHandHid[4];

static void np_w_hand_hide(struct Object *o) {
    int i, freeSlot = -1;
    o->header.gfx.node.flags |= GRAPH_RENDER_ACTIVE | GRAPH_RENDER_INVISIBLE; /* (invisible, not inactive: cur_obj_update makes a free object within its drawing distance active again every frame) */
    for (i = 0; i < 4; i++) {
        if (sHandHid[i].o == o) {
            sHandHid[i].bhv = o->behavior;
            return;
        }
        if (sHandHid[i].o == NULL && freeSlot < 0) {
            freeSlot = i;
        }
    }
    if (freeSlot < 0) {
        freeSlot = 0;
        sHandHid[0].o->header.gfx.node.flags &= ~GRAPH_RENDER_INVISIBLE;
    }
    sHandHid[freeSlot].o = o;
    sHandHid[freeSlot].bhv = o->behavior;
}

static void np_w_hand_unhide_step(void) {
    int i;
    for (i = 0; i < 4; i++) {
        struct Object *o = sHandHid[i].o;
        if (o == NULL) {
            continue;
        }
        if (o->activeFlags == ACTIVE_FLAG_DEACTIVATED || o->behavior != sHandHid[i].bhv) {
            sHandHid[i].o = NULL; /* (gone: its slot may already hold something else) */
        } else if (!np_w_remote_in_hands(o) && !psp_np_world_bowser_carried(o)) {
            o->header.gfx.node.flags &= ~GRAPH_RENDER_INVISIBLE; /* thrown or put down: drawn by itself again */
            sHandHid[i].o = NULL;
        }
    }
}

static void np_w_hand_unhide_all(void) {
    int i;
    for (i = 0; i < 4; i++) {
        if (sHandHid[i].o != NULL && sHandHid[i].o->activeFlags != ACTIVE_FLAG_DEACTIVATED && sHandHid[i].o->behavior == sHandHid[i].bhv) {
            sHandHid[i].o->header.gfx.node.flags &= ~GRAPH_RENDER_INVISIBLE;
        }
        sHandHid[i].o = NULL;
    }
}

static void np_w_in_remote_hands(struct Object *o) {
    s16 yaw;
    if (!np_remote_alive() || !(sRemote->header.gfx.node.flags & GRAPH_RENDER_ACTIVE)) {
        return;
    }
    yaw = sRemote->header.gfx.angle[1];
    o->oPosX = sRemote->header.gfx.pos[0] + 100.0f * sins(yaw);
    o->oPosY = sRemote->header.gfx.pos[1] + 60.0f;
    o->oPosZ = sRemote->header.gfx.pos[2] + 100.0f * coss(yaw);
    o->oFaceAngleYaw = o->oMoveAngleYaw = yaw;
    o->oForwardVel = 0.0f;
    o->oVelY = 0.0f;
    /* patch 311: drawn by the other player's model, in its hands (geo_switch_mario_hand_grab_pos, the same offsets as the local Mario), not here: this
     * position (a guess in front of the model) only serves the game logic. Drawn here it looked held farther from the hands than on the console that
     * carries it. As a local held object, its own node is not drawn meanwhile */
    np_w_hand_hide(o);
}

static void np_sm_add(struct Object *o, const float old[3], s16 oldYaw);
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
        np_w_remote_holds_set(o, (e->pad & NP_W_PAD_HANDS) != 0); /* the other console is carrying it (or has just thrown it): it drives it, wherever the two players are */
        if (!(e->pad & NP_W_PAD_HANDS) && o->behavior == segmented_to_virtual(bhvBreakableBoxSmall) && !o->oBreakableBoxSmallReleased) {
            o->oBreakableBoxSmallReleased = 1; /* thrown by the other player: if this console ends up driving it, it goes away after the same while */
            o->oBreakableBoxSmallFramesSinceReleased = 0;
        }
    }
    if (o->oHeldState != HELD_FREE || np_w_owned(o)) {
        /* patch 310: the two copies can go apart (a monty mole that pops out of a different hole on each console) and then each console counts itself as
         * the one nearest to ITS copy: both drive it and never meet again. The host's copy wins: if at the host's copy the host is the nearer one, the
         * client takes it (from then on both look at the same spot and agree). Not one this console holds, claimed or talks to */
        if (sWHost || o->oHeldState != HELD_FREE || np_w_claimed(o) || (o->activeFlags & ACTIVE_FLAG_INITIATED_TIME_STOP) || np_w_client_rule_at(e->pos)) {
            sDgOwn++;
            return; /* this console drives it (or is the one that should): the other one's stream is not applied */
        }
    }
    {
        const float old[3] = { o->oPosX, o->oPosY, o->oPosZ };
        const s16 oldYaw = (s16) o->oFaceAngleYaw;
        const int wasFollowed = psp_np_world_follow(o);
        o->oPosX = e->pos[0];
        o->oPosY = e->pos[1];
        o->oPosZ = e->pos[2];
        o->oFaceAngleYaw = e->yaw;
        if (wasFollowed && !((e->kind & NP_W_KIND_HELD) && (e->pad & NP_W_PAD_HANDS))) {
            np_sm_add(o, old, oldYaw); /* (the picture eases from where it was) */
        }
    }
    o->unused1 = sClock;
    o->oForwardVel = (f32) e->fvel / 32.0f; /* (sm64coopdx's standard fields: the behavior here goes on from them) */
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
    if ((e->pad >> NP_W_PAD_ANIM_SHIFT) & 0xF) {
        o->oAnimState = ((e->pad >> NP_W_PAD_ANIM_SHIFT) & 0xF) - 1;
    }
    o->oIntangibleTimer = 0;
    if (o->behavior == segmented_to_virtual(bhvMontyMole)) {
        /* (patch 310) a mole in its hole is not seen nor hit; one that has thrown its rock lets go of it here too (the rock flies from then on) */
        if (o->oAction == MONTY_MOLE_ACT_SELECT_HOLE) {
            o->header.gfx.node.flags |= GRAPH_RENDER_INVISIBLE;
            o->oIntangibleTimer = -1;
        } else {
            o->header.gfx.node.flags &= ~GRAPH_RENDER_INVISIBLE;
        }
        if (o->prevObj != NULL && (o->oAction != MONTY_MOLE_ACT_THROW_ROCK || o->header.gfx.unk38.animFrame >= 10)) {
            o->prevObj = NULL;
        }
        /* (its gravity is set by the action it starts: a copy that goes on by itself in the middle of a jump needs the one of that jump, or it flies off) */
        o->oGravity = (o->oAction == MONTY_MOLE_ACT_JUMP_INTO_HOLE) ? -6.0f : ((o->oAction == MONTY_MOLE_ACT_JUMP_OUT_OF_HOLE) ? -4.0f : 0.0f);
        if (o->oMontyMoleCurrentHole != NULL) { /* a copy that follows holds no hole of this console (one it took while it ran here would stay taken for good) */
            if (o->oMontyMoleCurrentHole->oMontyMoleHoleCooldown < 0) {
                o->oMontyMoleCurrentHole->oMontyMoleHoleCooldown = 0;
            }
            o->oMontyMoleCurrentHole = NULL;
        }
    }
    if (o->behavior == segmented_to_virtual(bhvRacingPenguin)) {
        /* the race is run by the console of the player that took it (psp_np_world_drive). If this copy ever goes on by itself (that player went away), it must
         * not give this player the text of a race it did not run (nor start one with no path: see racing_penguin.inc.c) */
        if (o->oAction == RACING_PENGUIN_ACT_SHOW_FINAL_TEXT) {
            o->oRacingPenguinFinalTextbox = -1;
            o->oRacingPenguinMarioWon = FALSE;
        }
    }
    o->header.gfx.scale[0] = (f32) e->scale[0] / 1000.0f;
    o->header.gfx.scale[1] = (f32) e->scale[1] / 1000.0f;
    o->header.gfx.scale[2] = (f32) e->scale[2] / 1000.0f;
    if (o->behavior == segmented_to_virtual(bhvFirePiranhaPlant)) {
        /* patch 318 (BitS, Tiny-Huge Island): its size, whether it is out and the count of the ones out live in its own fields, which the stream does not
         * carry. A copy hidden here when the stream started stayed hidden while it grew there; and one that went on by itself again (the stream stopped a
         * while) grew from a stale size, or never came out because the count of this console said two were out already */
        extern s32 sNumActiveFirePiranhaPlants;
        const s32 out = (o->oAction == FIRE_PIRANHA_PLANT_ACT_GROW) || o->header.gfx.scale[0] > 0.0f;
        o->oFirePiranhaPlantScale = o->header.gfx.scale[0];
        if (out != o->oFirePiranhaPlantActive) {
            sNumActiveFirePiranhaPlants += out ? 1 : -1;
            o->oFirePiranhaPlantActive = out;
        }
        if (o->header.gfx.scale[0] > 0.0f) {
            o->header.gfx.node.flags &= ~GRAPH_RENDER_INVISIBLE;
        } else {
            o->header.gfx.node.flags |= GRAPH_RENDER_INVISIBLE;
        }
        if (o->oFirePiranhaPlantScale <= o->oFirePiranhaPlantNeutralScale / 2) {
            o->oIntangibleTimer = -1; /* (as its own script: not touchable while it is small or hidden) */
        }
    } else if (o->behavior == segmented_to_virtual(bhvPiranhaPlant)) {
        o->oPiranhaPlantScale = o->header.gfx.scale[0]; /* (its shrink and its respawn grow from this) */
    }
    if (e->anim >= 0 && e->anim < NP_W_ANIMS && o->oAnimations != NULL) {
        geo_obj_init_animation(&o->header.gfx, &o->oAnimations[e->anim]);
        o->header.gfx.unk38.animFrame = e->frame;
        o->header.gfx.unk38.animAccel = e->accel;
    }
    if ((e->kind & NP_W_KIND_HELD) && (e->pad & NP_W_PAD_HANDS)) {
        np_w_in_remote_hands(o);
    }
}

/* Host: sends the state of the enemies that are running, a few per frame in turns. An enemy that has not changed since its last update is only
 * repeated now and then (often enough for the other console to keep following it), so the radio is used for what moves. */
#define NP_W_SENT 48
#define NP_W_REFRESH 20 /* frames between the repeats of an enemy that stands still (the other console follows one for NP_W_FOLLOW_FRAMES) */
#define NP_W_CLAIM_REFRESH 4 /* ... and of one this console has claimed (less than NP_W_THROWN_FRAMES) */
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

/* patch 310: one that neither console drives. Each one decides by where ITS copy is, and two copies that went apart (a monty mole that popped out of a
 * different hole on each console) can each be where the other player is the nearer one: then nobody sends it and both run it apart for good. The host
 * sends such a one anyway (it runs it here: nothing is being followed); the client, that does not drive its copy, takes it, and from then on both look at
 * the same spot. */
static int np_w_orphan(const struct Object *o) {
    return sWHost && sWPeerHere && (sClock - o->unused1) >= NP_W_FOLLOW_FRAMES && o->oHeldState == HELD_FREE;
}

static void np_w_enemies_send(void) {
    PspNpObj batch[NP_W_BATCH];
    struct Object *cand[64];
    u32 sigs[64], urg[64];
    int nc = 0, i, j, n = 0;
    const int cap = (NP_W_BATCH >> psp_np_congestion()) < 3 ? 3 : (NP_W_BATCH >> psp_np_congestion()); /* smaller batches on a slow link */
    for (i = 0; i < NUM_OBJ_LISTS && nc < 64; i++) {
        struct ObjectNode *head = &gObjectLists[i];
        struct Object *obj;
        for (obj = (struct Object *) head->next; obj != (struct Object *) head && nc < 64; obj = (struct Object *) obj->header.next) {
            if (np_w_alive(obj) && obj->behavior != NULL && (((obj->activeFlags & ACTIVE_FLAG_FAR_AWAY) == 0) || np_w_near_remote(obj)) && np_w_kind(obj) != 0 && np_w_homed(obj)
                && (np_w_owned(obj) || np_w_orphan(obj))) {
                const u32 sig = np_w_sig(obj);
                for (j = 0; j < NP_W_SENT && sSent[j].o != obj; j++) {
                }
                /* changed, never sent, or due for its repeat (one this console drives no matter where the players are, often: the other console keeps it as
                 * the other one's only while it hears of it, see np_w_remote_holds) */
                if (j == NP_W_SENT || sSent[j].sig != sig || sClock - sSent[j].t >= (np_w_claimed(obj) ? NP_W_CLAIM_REFRESH : NP_W_REFRESH)) {
                    /* urgency: the frames since it was last sent, more when the other player is near it (what it is looking at) */
                    u32 age = (j == NP_W_SENT) ? 64U : (sClock - sSent[j].t);
                    if (age > 64U) {
                        age = 64U;
                    }
                    if (np_remote_alive() && dist_between_objects(obj, sRemote) < 2500.0f) {
                        age *= 3U;
                    }
                    cand[nc] = obj;
                    urg[nc] = age + 1U;
                    sigs[nc++] = sig;
                }
            }
        }
    }
    if (nc == 0) {
        return;
    }
    for (i = 0; i < nc && n < cap; i++) {
        /* the most urgent first (it used to be a plain rotation: with many enemies one right in front of the other player could wait its turn) */
        int ci = -1, k;
        struct Object *o;
        PspNpObj *e;
        for (k = 0; k < nc; k++) {
            if (urg[k] != 0 && (ci < 0 || urg[k] > urg[ci])) {
                ci = k;
            }
        }
        if (ci < 0) {
            break;
        }
        urg[ci] = 0;
        o = cand[ci];
        e = &batch[n++];
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
        e->pad = (np_w_claimed(o) && o->oHeldState == HELD_HELD) ? NP_W_PAD_HANDS : 0; /* (a thrown one is still claimed, but flies: the other console must follow where it goes, not put it in the hands) */
        if (o->oAnimState >= 0 && o->oAnimState < 15) {
            e->pad |= (uint8_t) ((o->oAnimState + 1) << NP_W_PAD_ANIM_SHIFT);
        }
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
        if (!still && !reload && sTrk[i].o->activeFlags == ACTIVE_FLAG_DEACTIVATED && !np_w_unloaded(sTrk[i].o, sTrk[i].parent)) { /* it was removed: it died */
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
        sTrk[i].parent = cur[i].o->parentObj;
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
/* The chain chomp was set free (its post was pounded in): an ACTION event of its own, so each console sets its own chomp free. Its state alone did not carry
 * this: it is sent by the console of the player nearest to it, and when the other player came near the chomp during the release, that console (whose chomp
 * was still chained) became the one that counts, and its chomp never went free (no release, no broken gate). sm64coopdx: the post is synced and forces it. */
#define NP_W_ACT_CHOMP 100 /* coinValue of the ACTION event (the triggers use 1..NP_W_TRG) */
void psp_np_world_chomp_freed(struct Object *chomp) {
    PspNpWorldEvent e;
    if (chomp == NULL || psp_netplay_state() != PSP_NP_CONNECTED) {
        return;
    }
    memset(&e, 0, sizeof(e));
    e.type = PSP_NP_EV_ACTION;
    e.level = (uint8_t) gCurrLevelNum;
    e.area = (uint8_t) np_area_key();
    e.coinValue = NP_W_ACT_CHOMP;
    e.value = 1;
    e.pos[0] = (s16) chomp->oHomeX;
    e.pos[1] = (s16) chomp->oHomeY;
    e.pos[2] = (s16) chomp->oHomeZ;
    np_w_log(&e); /* (told again to a player that comes in later: its chomp is gone and the gate broken) */
    psp_np_world_send(&e);
}

static int np_w_chomp_freed(const PspNpWorldEvent *e) {
    struct Object *o = NULL, *obj;
    f32 best = 3000.0f;
    int i;
    for (i = 0; i < NUM_OBJ_LISTS; i++) {
        struct ObjectNode *head = &gObjectLists[i];
        for (obj = (struct Object *) head->next; obj != (struct Object *) head; obj = (struct Object *) obj->header.next) {
            if (np_w_alive(obj) && obj->behavior == segmented_to_virtual(bhvChainChomp)) {
                const f32 dx = obj->oHomeX - e->pos[0], dz = obj->oHomeZ - e->pos[2];
                const f32 d = sqrtf(dx * dx + dz * dz);
                if (d < best) {
                    best = d;
                    o = obj;
                }
            }
        }
    }
    if (o == NULL) {
        return 0; /* (not there yet: tried again for a while) */
    }
    if (o->oChainChompReleaseStatus != CHAIN_CHOMP_NOT_RELEASED) {
        return 1; /* already free here (set free here too, or by its state) */
    }
    for (i = 0; i < NUM_OBJ_LISTS; i++) { /* its post is down and no longer holds it (it would set it free a second time) */
        struct ObjectNode *head = &gObjectLists[i];
        for (obj = (struct Object *) head->next; obj != (struct Object *) head; obj = (struct Object *) obj->header.next) {
            if (np_w_alive(obj) && obj->behavior == segmented_to_virtual(bhvWoodenPost) && obj->parentObj == o) {
                obj->parentObj = obj;
                obj->oWoodenPostOffsetY = -190.0f;
            }
        }
    }
    if (e->value != 0 && o->oAction == CHAIN_CHOMP_ACT_MOVE && o->oChainChompSegments != NULL) {
        o->oChainChompReleaseStatus = CHAIN_CHOMP_RELEASED_TRIGGER_CUTSCENE; /* it plays its release here too (the cutscene only for the player nearest the post) */
        o->unused1 = sClock;
    } else {
        /* a player that comes in later, or a chomp with no chain here (its player was far): it is over already, like sm64coopdx's area sync. The gate breaks
         * now and the chomp goes away. */
        o->oChainChompHitGate = TRUE;
        o->oChainChompReleaseStatus = CHAIN_CHOMP_RELEASED_END_CUTSCENE;
        if (o->oAction == CHAIN_CHOMP_ACT_MOVE && o->oChainChompSegments != NULL) {
            o->oAction = CHAIN_CHOMP_ACT_UNLOAD_CHAIN;
        }
    }
    return 1;
}

/* Patch 309: what a few objects did on one console that their state alone does not carry (sm64coopdx syncs their fields): an ACTION event of its own, with the
 * signal in bhv and its value in value. The object is found by its home. */
#define NP_W_ACT_SIG 101
void psp_np_world_signal(const struct Object *o, int sig, int value) {
    psp_np_world_signal_ex(o, sig, value, 0);
}

void psp_np_world_signal_ex(const struct Object *o, int sig, int value, int extra) {
    PspNpWorldEvent e;
    if (o == NULL || psp_netplay_state() != PSP_NP_CONNECTED) {
        return;
    }
    memset(&e, 0, sizeof(e));
    e.type = PSP_NP_EV_ACTION;
    e.level = (uint8_t) gCurrLevelNum;
    e.area = (uint8_t) np_area_key();
    e.coinValue = NP_W_ACT_SIG;
    e.bhv = sig;
    e.value = (int16_t) value;
    e.pos[0] = (s16) o->oHomeX;
    e.pos[1] = (s16) o->oHomeY;
    e.pos[2] = (s16) o->oHomeZ;
    e.aux[0] = (int16_t) extra;
    if (sig == PSP_NP_SIG_UKIKI_HAT || sig == PSP_NP_SIG_BOWSER_PLAT) {
        np_w_log(&e); /* (a player that comes in later sees the cap on the ukiki too) */
    }
    psp_np_world_send(&e);
}

/* The puzzle chests (Jolly Roger Bay, its sunken ship, Dire, Dire Docks), as sm64coopdx: the console whose player opens a chest decides whether it is the
 * right one and at once sends the whole puzzle (the maker's answer count and "last one was wrong", and the action of each of its four chests), with the sound
 * to play; the other console takes it as it is. Before, they went with the platforms, each chest and the maker driven by the console of the player nearest to
 * it: what the player who opened a chest did came late to the other console, and a correction from that other console could put the answer count back here,
 * so the next right chest counted as wrong and shocked the player. value: answer count (bits 0-3), wrong (bit 4), the action of chest n in bits 5 + 2 (n - 1) */
static int np_w_chests_pack(const struct Object *maker) {
    int v = (maker->oTreasureChestUnkF4 & 15) | ((maker->oTreasureChestUnkF8 & 1) << 4), i;
    for (i = 0; i < NUM_OBJ_LISTS; i++) {
        struct ObjectNode *head = &gObjectLists[i];
        struct Object *obj;
        for (obj = (struct Object *) head->next; obj != (struct Object *) head; obj = (struct Object *) obj->header.next) {
            if (np_w_alive(obj) && obj->parentObj == maker && obj->behavior == segmented_to_virtual(bhvTreasureChestBottom)
                && obj->oBehParams2ndByte >= 1 && obj->oBehParams2ndByte <= 4) {
                v |= (obj->oAction & 3) << (5 + 2 * (obj->oBehParams2ndByte - 1));
            }
        }
    }
    return v;
}

void psp_np_world_chests(const struct Object *maker, int sound) {
    PspNpWorldEvent e;
    int i;
    if (maker == NULL || psp_netplay_state() != PSP_NP_CONNECTED) {
        return;
    }
    memset(&e, 0, sizeof(e));
    e.type = PSP_NP_EV_ACTION;
    e.level = (uint8_t) gCurrLevelNum;
    e.area = (uint8_t) np_area_key();
    e.coinValue = NP_W_ACT_SIG;
    e.bhv = PSP_NP_SIG_CHESTS;
    e.value = (int16_t) np_w_chests_pack(maker);
    e.pos[0] = (s16) maker->oPosX;
    e.pos[1] = (s16) maker->oPosY;
    e.pos[2] = (s16) maker->oPosZ;
    e.aux[0] = (int16_t) sound;
    psp_np_world_send(&e);
    /* (a player that comes into the area later gets the puzzle as it is now, with no sound: the record keeps only the latest one of this maker) */
    e.aux[0] = 0;
    for (i = 0; i < sWLogN; i++) {
        if (sWLog[i].type == PSP_NP_EV_ACTION && sWLog[i].coinValue == NP_W_ACT_SIG && sWLog[i].bhv == PSP_NP_SIG_CHESTS
            && sWLog[i].pos[0] == e.pos[0] && sWLog[i].pos[1] == e.pos[1] && sWLog[i].pos[2] == e.pos[2]) {
            sWLog[i] = e;
            return;
        }
    }
    np_w_log(&e);
}

/* patch 313: a star that does not end the level, given to this console's player by Toad or MIPS (bhv_spawn_star_no_level_exit). sm64coopdx syncs that star
 * (bhv_spawned_star_loop): the other players see it too and any of them can take it. Here the other console makes it where it ends up, over this player
 * (set_home_to_mario), with no cutscene there; taking it on either console takes it on both (PSP_NP_EV_STAR). */
void psp_np_world_nle_star(const struct Object *star) {
    PspNpWorldEvent e;
    if (star == NULL || gMarioObject == NULL || psp_netplay_state() != PSP_NP_CONNECTED) {
        return;
    }
    memset(&e, 0, sizeof(e));
    e.type = PSP_NP_EV_ACTION;
    e.level = (uint8_t) gCurrLevelNum;
    e.area = (uint8_t) np_area_key();
    e.coinValue = NP_W_ACT_SIG;
    e.bhv = PSP_NP_SIG_NLE_STAR;
    e.value = (int16_t) ((star->oBehParams >> 24) & 0xFF);
    e.pos[0] = (s16) gMarioObject->oPosX;
    e.pos[1] = (s16) (gMarioObject->oPosY + 250.0f);
    e.pos[2] = (s16) gMarioObject->oPosZ;
    psp_np_world_send(&e);
}

/* the object of that behavior whose home is nearest to pos (within dist) */
static struct Object *np_w_find_home(const BehaviorScript *bhv, const s16 *pos, f32 dist) {
    const void *b = segmented_to_virtual(bhv);
    struct Object *best = NULL, *obj;
    int i;
    for (i = 0; i < NUM_OBJ_LISTS; i++) {
        struct ObjectNode *head = &gObjectLists[i];
        for (obj = (struct Object *) head->next; obj != (struct Object *) head; obj = (struct Object *) obj->header.next) {
            if (np_w_alive(obj) && obj->behavior == b) {
                const f32 dx = obj->oHomeX - pos[0], dy = obj->oHomeY - pos[1], dz = obj->oHomeZ - pos[2];
                const f32 d = sqrtf(dx * dx + dy * dy + dz * dz);
                if (d < dist) {
                    dist = d;
                    best = obj;
                }
            }
        }
    }
    return best;
}

static int np_w_sig_apply(const PspNpWorldEvent *e) {
    struct Object *o;
    if (e->bhv == PSP_NP_SIG_CHESTS) { /* (see psp_np_world_chests) */
        const void *makers[3] = { segmented_to_virtual(bhvTreasureChestsJrb), segmented_to_virtual(bhvTreasureChestsShip), segmented_to_virtual(bhvTreasureChests) };
        struct Object *maker = NULL, *obj;
        f32 best = 300.0f;
        int i, k;
        for (i = 0; i < NUM_OBJ_LISTS; i++) {
            struct ObjectNode *head = &gObjectLists[i];
            for (obj = (struct Object *) head->next; obj != (struct Object *) head; obj = (struct Object *) obj->header.next) {
                for (k = 0; k < 3; k++) {
                    if (np_w_alive(obj) && obj->behavior == makers[k]) {
                        const f32 dx = obj->oPosX - e->pos[0], dy = obj->oPosY - e->pos[1], dz = obj->oPosZ - e->pos[2];
                        const f32 d = sqrtf(dx * dx + dy * dy + dz * dz);
                        if (d < best) {
                            best = d;
                            maker = obj;
                        }
                    }
                }
            }
        }
        if (maker == NULL) {
            return 0;
        }
        maker->oTreasureChestUnkF4 = e->value & 15;
        maker->oTreasureChestUnkF8 = (e->value >> 4) & 1;
        for (i = 0; i < NUM_OBJ_LISTS; i++) {
            struct ObjectNode *head = &gObjectLists[i];
            for (obj = (struct Object *) head->next; obj != (struct Object *) head; obj = (struct Object *) obj->header.next) {
                if (np_w_alive(obj) && obj->parentObj == maker && obj->behavior == segmented_to_virtual(bhvTreasureChestBottom)
                    && obj->oBehParams2ndByte >= 1 && obj->oBehParams2ndByte <= 4) {
                    /* (a wrong chest is not made tangible here: it only shocks the player that opened it, on that player's console) */
                    obj->oAction = (e->value >> (5 + 2 * (obj->oBehParams2ndByte - 1))) & 3;
                }
            }
        }
        if (e->aux[0] == 1) {
            play_sound(SOUND_GENERAL2_RIGHT_ANSWER, gDefaultSoundArgs);
        } else if (e->aux[0] == 2) {
            play_sound(SOUND_MENU_CAMERA_BUZZ, gDefaultSoundArgs);
        }
        return 1;
    }
    if (e->bhv == PSP_NP_SIG_BOWSER_PLAT) {
        /* sm64coopdx (bhv_falling_bowser_platform_loop): the console where a platform starts to fall sends it, and a platform that falls never goes back
         * (ignore_if_true: action > 1). It falls here too, whichever console's Bowser made it go */
        struct Object *obj;
        int i;
        for (i = 0; i < NUM_OBJ_LISTS; i++) {
            struct ObjectNode *head = &gObjectLists[i];
            for (obj = (struct Object *) head->next; obj != (struct Object *) head; obj = (struct Object *) obj->header.next) {
                if (np_w_alive(obj) && obj->behavior == segmented_to_virtual(bhvFallingBowserPlatform) && obj->oBehParams2ndByte == e->value) {
                    if (obj->oAction < 2) {
                        obj->oAction = 2;
                    }
                    return 1;
                }
            }
        }
        return 0; /* (the arena is not loaded here yet: tried again for a while) */
    }
    if (e->bhv == PSP_NP_SIG_UKIKI_HAT) {
        /* the cap ukiki took the other player's cap, or gave it back. Here it wears it too (a player of this console that catches it lets it go at once:
         * that cap is not this player's, see hat_ukiki_held_loop) */
        o = np_w_find_home(bhvUkiki, e->pos, 300.0f);
        if (o == NULL) {
            return 0;
        }
        if (o->oBehParams2ndByte != UKIKI_HAT) {
            return 1;
        }
        if (e->value) {
            o->oUkikiHasHat |= UKIKI_HAT_ON;
            o->unused1 = (u32) psp_np_remote_char() + 1; /* (it wears the other player's cap: see psp_np_enemy_cap_char) */
            if (o->oUkikiTextState == UKIKI_TEXT_DEFAULT || o->oUkikiTextState == UKIKI_TEXT_STEAL_HAT || o->oUkikiTextState == UKIKI_TEXT_STOLE_HAT) {
                o->oUkikiTextState = UKIKI_TEXT_HAS_HAT;
            }
        } else if (!(save_file_get_flags() & SAVE_FLAG_CAP_ON_UKIKI)) { /* (the one it wears here is not this player's) */
            o->oUkikiHasHat &= ~UKIKI_HAT_ON;
            if (o->oUkikiTextState == UKIKI_TEXT_HAS_HAT || o->oUkikiTextState == UKIKI_TEXT_STEAL_HAT || o->oUkikiTextState == UKIKI_TEXT_STOLE_HAT) {
                o->oUkikiTextState = UKIKI_TEXT_DEFAULT;
            }
        }
        o->oAnimState = (o->oUkikiHasHat & UKIKI_HAT_ON) ? UKIKI_ANIM_STATE_HAT_ON : UKIKI_ANIM_STATE_DEFAULT;
        return 1;
    }
    if (e->bhv == PSP_NP_SIG_NLE_STAR) { /* (see psp_np_world_nle_star) */
        struct Object *obj, *s;
        const void *b = segmented_to_virtual(bhvSpawnedStarNoLevelExit);
        int i;
        if (gMarioObject == NULL) {
            return 0;
        }
        for (i = 0; i < NUM_OBJ_LISTS; i++) { /* (already here: not made twice) */
            struct ObjectNode *head = &gObjectLists[i];
            for (obj = (struct Object *) head->next; obj != (struct Object *) head; obj = (struct Object *) obj->header.next) {
                if (np_w_alive(obj) && obj->behavior == b && ((obj->oBehParams >> 24) & 0xFF) == (u32) e->value) {
                    return 1;
                }
            }
        }
        /* (sm64coopdx syncs oMipsStarStatus: MIPS has given its star, the copy here does not give it again when this player catches it) */
        o = np_w_find(np_w_bhv_of(bhvMips), e->pos, 100000.0f, 0);
        if (o != NULL && o->oMipsStarStatus == MIPS_STAR_STATUS_HAVENT_SPAWNED_STAR) {
            o->oMipsStarStatus = MIPS_STAR_STATUS_ALREADY_SPAWNED_STAR;
        }
        s = spawn_object_abs_with_rot(gMarioObject, 0, MODEL_STAR, bhvSpawnedStarNoLevelExit, e->pos[0], e->pos[1], e->pos[2], 0, 0, 0);
        if (s != NULL) {
            s->oBehParams = (u32) e->value << 24;
            s->oInteractionSubtype = INT_SUBTYPE_NO_EXIT;
            s->oAction = 3; /* (already in place: it does not fly here, so there is no cutscene and time does not stop) */
            s->oAngleVelYaw = 0x400;
        }
        return 1;
    }
    if (e->bhv == PSP_NP_SIG_MANTA_RING) {
        /* the other player passed a ring of the manta ray (sm64coopdx: its count is one for everybody). The copy of the manta here counts it too (if this
         * console runs it, it makes the star at the fifth), and the copy of that ring goes away */
        struct Object *mgr, *obj;
        int i;
        o = np_w_find_home(bhvMantaRay, e->pos, 300.0f);
        if (o == NULL) {
            return 0;
        }
        mgr = o->parentObj;
        o->oWaterRingSpawnerRingsCollected = e->value;
        if (mgr != NULL && mgr != o && mgr->behavior == segmented_to_virtual(bhvMantaRayRingManager)) {
            mgr->oWaterRingMgrLastRingCollected = e->aux[0];
        }
        for (i = 0; i < NUM_OBJ_LISTS; i++) {
            struct ObjectNode *head = &gObjectLists[i];
            for (obj = (struct Object *) head->next; obj != (struct Object *) head; obj = (struct Object *) obj->header.next) {
                if (np_w_alive(obj) && obj->behavior == segmented_to_virtual(bhvMantaRayWaterRing) && obj->oWaterRingIndex == e->aux[0]
                    && obj->oAction == WATER_RING_ACT_NOT_COLLECTED) {
                    obj->oAction = WATER_RING_ACT_COLLECTED;
                }
            }
        }
        return 1;
    }
    if (e->bhv == PSP_NP_SIG_LAKITU) {
        /* the intro lakitu came down for the other player (sm64coopdx: one lakitu, its text for the player that stepped on the bridge). Here it flies to that
         * player's model with no text, and this player is not stopped */
        o = np_w_find_home(bhvCameraLakitu, e->pos, 30000.0f);
        if (o == NULL || o->oBehParams2ndByte == CAMERA_LAKITU_BP_FOLLOW_CAMERA) {
            return e->value == 1 ? 0 : 1;
        }
        if (e->value == 1 && o->oAction == CAMERA_LAKITU_INTRO_ACT_TRIGGER_CUTSCENE) {
            o->unused2 = PSP_NP_LAKITU_OTHER;
            o->oAction = CAMERA_LAKITU_INTRO_ACT_SPAWN_CLOUD;
        } else if (e->value == 2 && o->unused2 == PSP_NP_LAKITU_OTHER) {
            o->oCameraLakituFinishedDialog = TRUE;
        }
        return 1;
    }
    return 1;
}

static int np_w_trg_apply(const PspNpWorldEvent *e) {
    struct Object *o;
    int i;
    if (e->coinValue == NP_W_ACT_CHOMP) {
        return np_w_chomp_freed(e);
    }
    if (e->coinValue == NP_W_ACT_SIG) {
        return np_w_sig_apply(e);
    }
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
        if (o->behavior == segmented_to_virtual(bhvExclamationBox) && e->bhv == 3) {
            /* patch 311: what its own hit does when it starts that action (exclamation_box_act_2). Without it the box went on tangible: hidden while it
             * waits to come back, but still there to be hit, and broken again by a touch. Its coins are the other console's (sm64coopdx: only the
             * console that broke it makes its contents, the rest get them sent) */
            o->oIntangibleTimer = -1; /* (cur_obj_become_intangible on this object) */
            o->oExclamationBoxUnkFC = 0x4000;
            o->oVelY = 30.0f;
            o->oGravity = -8.0f;
            o->oFloorHeight = o->oPosY;
            o->unused2 = PSP_NP_BOX_REMOTE;
        }
        if (o->behavior == segmented_to_virtual(bhvCannonClosed)) {
            /* patch 313: the other player had the bob-omb buddy open this cannon. The cannon is open here too (sm64coopdx syncs the buddy and forces
             * the cannon open): without the flag the trap door reached the end of its opening (bhv_cannon_closed_init) and found the cannon still
             * locked, so it stayed half open with no cannon. The buddy here is done too (its "the cannon is open" text) */
            struct Object *buddy = np_w_find_home(bhvBobombBuddyOpensCannon, e->pos, 20000.0f);
            save_file_set_cannon_unlocked();
            if (buddy != NULL) {
                buddy->oBobombBuddyCannonStatus = BOBOMB_BUDDY_CANNON_OPENED;
                buddy->oBobombBuddyHasTalkedToMario = BOBOMB_BUDDY_HAS_TALKED;
            }
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
    if (parent == NULL || star == NULL || psp_netplay_state() != PSP_NP_CONNECTED || psp_np_world_follow(parent)) {
        return;
    }
    if (np_w_kind(parent) == 0 && !np_p_nearest_driven(parent)
        /* patch 308: the stars only one console makes (what was done there is not carried over): the mother penguin given her baby, the cage of the
         * ukiki, the slide of Cool Cool Mountain, the race of the princess's slide (made from Mario). sm64coopdx syncs those objects; here the star is told */
        && parent->behavior != segmented_to_virtual(bhvTuxiesMother) && parent->behavior != segmented_to_virtual(bhvUkikiCageStar)
        && parent->behavior != segmented_to_virtual(bhvCcmTouchedStarSpawn) && parent != gMarioObject) {
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
    s->unused2 = PSP_NP_STAR_NO_CUTSCENE; /* (patch 308: made for the other player's deed: no cutscene and no frozen time here, like sm64coopdx) */
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
    static const void *virt[4];
    int i;
    if (virt[0] == NULL) {
        virt[0] = segmented_to_virtual(bhvGoombaTripletSpawner);
        virt[1] = segmented_to_virtual(bhvCoinFormation);   /* its coins exist only while a player is near: the other player's coins must exist here too (sm64coopdx creates them at once) */
        virt[2] = segmented_to_virtual(bhvScuttlebugSpawn);
        virt[3] = segmented_to_virtual(bhvMerryGoRoundBooManager); /* (patch 308: sm64coopdx, the nearest player) */
    }
    for (i = 0; i < 4; i++) {
        if (o->behavior == virt[i]) {
            return 1;
        }
    }
    return 0;
}

short psp_np_attack_yaw(const struct Object *o, short def) {
    return (o == sAtkObj && sClock - sAtkT <= 3) ? sAtkYaw : def;
}

static u32 sRemDlgT; /* sClock when the other player was last in a text (psp_np_world_frame) */
#define NP_W_DLG_AFTER 90 /* frames after its text the boss is still its own: its next state (the text is over) is on the way */
int psp_np_world_dialog_ok(const struct Object *o) {
    /* (sm64coopdx, should_start_or_continue_dialog: the text of a synced object such as a boss goes to the player nearest to it, and only on that player's console,
     * which drives the object meanwhile; the other one just sees the boss wait and then go on.) Bowser too, for his text when he is beaten: both players could
     * get it, and the console that was not driving him lost its text halfway when the other console's state came in (its player stayed stuck). Not the boss
     * intros of Eyerok and Wiggler: they are not carried, each console plays its own. */
    if (!sWPeerHere || !np_remote_alive() || gMarioObject == NULL || (np_w_kind(o) == 0 && o->behavior != segmented_to_virtual(bhvBowser))) {
        return 1;
    }
    if ((sClock - sRemDlgT) < NP_W_DLG_AFTER && dist_between_objects(o, sRemote) < 2000.0f) {
        /* patch 318: the other player is talking to it (or has just finished): the text is that player's. Decided by who was nearer at every frame, the second
         * player walking up during the text got it too, and again when the first one's text ended, before the state that said so came in (two texts) */
        return 0;
    }
    if (np_w_kind(o) == 0) {
        return np_w_rule_mine(o); /* (exactly the console that drives him: not both of them when the two players are about as near) */
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

/* The object the other player stands on (a platform, a switch), found once per frame from the floor under its model: what sm64coopdx gets from
 * marioObj->platform of every player. Not while it is in the air. */
static const struct Object *sRemoteFloorObj;
static s16 sRemoteFloorType; /* the type of the floor under it (also while it jumps, like the floor the game keeps for Mario) */
static void np_w_remote_floor(void) {
    struct Surface *fl = NULL;
    f32 h;
    sRemoteFloorObj = NULL;
    sRemoteFloorType = -1;
    if (!sWPeerHere || !np_remote_alive()) {
        return;
    }
    h = find_floor(sRemote->oPosX, sRemote->oPosY + 150.0f, sRemote->oPosZ, &fl); /* (from above: the copy of a tilting platform here may be tilted a bit more than there) */
    if (fl != NULL) {
        sRemoteFloorType = fl->type;
    }
    if (psp_np_game_remote_action() & ACT_FLAG_AIR) {
        return;
    }
    if (fl != NULL && fl->object != NULL && sRemote->oPosY - h < 60.0f && sRemote->oPosY - h > -120.0f) {
        sRemoteFloorObj = fl->object;
    }
}

int psp_np_world_spawner_mine(const struct Object *o) {
    return psp_netplay_state() != PSP_NP_CONNECTED || np_w_rule_mine(o);
}

int psp_np_world_is_host(void) {
    return sWHost;
}

int psp_np_world_remote_floor_type(void) {
    return sRemoteFloorType;
}

/* The local player did something to a platform-like object other than standing on it (kicked a board, punched a book): like standing on it, this console
 * drives it for a while, or the host's copy (which did not see it) would put it back. */
void psp_np_world_claim(struct Object *o) {
    if (o != NULL && psp_netplay_state() == PSP_NP_CONNECTED && !sWHost) {
        o->unused2 = sClock;
    }
}

/* Patch 309: this console drives o while this is called (every frame): the racing penguin during the race of this console's player, Yoshi while it talks to it
 * and gives its present (sm64coopdx: the one that started it owns it). An enemy of the stream is marked as held by this console (the other one follows it
 * wherever the players are), a state object is sent by this console only (see np_p_sendable / np_p_apply). */
void psp_np_world_drive(struct Object *o) {
    if (o != NULL && psp_netplay_state() == PSP_NP_CONNECTED) {
        o->unused2 = sClock;
    }
}

/* The shell the other player rides (patch 308). On that console the shell runs under its player; here its copy was taken away when the other player got
 * on it (see the ITEM event), so a shell is drawn under the other player's model while it rides one (sm64coopdx moves the shell itself with that player).
 * It is only a picture: nothing touches it. */
static struct Object *sRemShell;
static int np_w_rem_shell_ok(void) {
    return sRemShell != NULL && sRemShell->activeFlags != ACTIVE_FLAG_DEACTIVATED && sRemShell->behavior == segmented_to_virtual(bhvStaticObject);
}
static void np_w_remote_shell(void) {
    const int riding = sWPeerHere && np_remote_alive() && gMarioObject != NULL && (psp_np_game_remote_action() & ACT_FLAG_RIDING_SHELL)
                       && (sRemote->header.gfx.node.flags & GRAPH_RENDER_ACTIVE);
    if (!np_w_rem_shell_ok()) {
        sRemShell = NULL; /* (the area was unloaded with it) */
    }
    if (!riding) {
        if (sRemShell != NULL) {
            obj_mark_for_deletion(sRemShell);
            sRemShell = NULL;
        }
        return;
    }
    if (sRemShell == NULL) {
        sMirQuiet = 1;
        sRemShell = spawn_object(gMarioObject, MODEL_KOOPA_SHELL, bhvStaticObject);
        sMirQuiet = 0;
        if (sRemShell == NULL) {
            return;
        }
    }
    sRemShell->oPosX = sRemote->header.gfx.pos[0];
    sRemShell->oPosY = sRemote->header.gfx.pos[1];
    sRemShell->oPosZ = sRemote->header.gfx.pos[2];
    sRemShell->oFaceAnglePitch = 0;
    sRemShell->oFaceAngleYaw = sRemote->header.gfx.angle[1];
    sRemShell->oFaceAngleRoll = 0;
    sRemShell->oInteractType = 0;
    vec3f_copy(sRemShell->header.gfx.pos, sRemote->header.gfx.pos);
    sRemShell->header.gfx.angle[0] = 0;
    sRemShell->header.gfx.angle[1] = sRemote->header.gfx.angle[1];
    sRemShell->header.gfx.angle[2] = 0;
}

static struct Object *np_p_find(uint8_t kind, uint8_t ord);
int psp_np_world_remote_on(const struct Object *o) {
    if (o == NULL || !sWPeerHere || !np_remote_alive()) {
        return 0;
    }
    if (sLastInValid && sLastIn.platKind != 0xFF) { /* what its console says it stands on */
        return np_p_find(sLastIn.platKind, sLastIn.platOrd) == o;
    }
    return sRemoteFloorObj != NULL && sRemoteFloorObj == o;
}

/* the other player is in a bubble or in its death: not a player the objects go after (sm64coopdx: is_player_active / the bubbled players are skipped) */
static int np_remote_out(void) {
    switch (sNetBody.action) {
        case ACT_BUBBLED:
        case ACT_STANDING_DEATH:
        case ACT_DEATH_ON_BACK:
        case ACT_DEATH_ON_STOMACH:
        case ACT_QUICKSAND_DEATH:
        case ACT_WATER_DEATH:
        case ACT_DROWNING:
        case ACT_ELECTROCUTION:
        case ACT_SUFFOCATION:
        case ACT_EATEN_BY_BUBBA:
            return 1;
    }
    return 0;
}

int psp_np_world_rescuer(float pos[3]) {
    if (!sWPeerHere || !np_remote_alive() || np_remote_out() || psp_np_game_remote_age() < 30) {
        return 0;
    }
    pos[0] = sRemote->oPosX;
    pos[1] = sRemote->oPosY;
    pos[2] = sRemote->oPosZ;
    return 1;
}

struct Object *psp_np_world_nearest_player(const struct Object *o) {
    if (!sWPeerHere || !np_remote_alive() || gMarioObject == NULL) {
        return NULL;
    }
    if (np_remote_out()) {
        return gMarioObject;
    }
    if (gMarioState->action == ACT_BUBBLED) {
        return sRemote;
    }
    return (dist_between_objects(o, sRemote) < dist_between_objects(o, gMarioObject)) ? sRemote : gMarioObject;
}

void psp_np_world_retarget(struct Object *o) {
    if (!sWPeerHere || !np_remote_alive() || np_remote_out()) {
        return;
    }
    if (np_w_is_spawner(o)) {
        if ((o->oFlags & OBJ_FLAG_COMPUTE_DIST_TO_MARIO) && dist_between_objects(o, sRemote) < o->oDistanceToMario) {
            o->oDistanceToMario = dist_between_objects(o, sRemote);
        }
        return;
    }
    if (np_w_kind(o) == 0) {
        /* the bosses whose state the console of the nearest player drives (sm64coopdx: Bowser, Wiggler and Eyerok aim at nearest_player_to_object). Each console
         * runs their script between two states: aimed at its own player, the copy that is not driving went off toward that one, and jumped back at every state */
        if (!(o->behavior == segmented_to_virtual(bhvBowser) || o->behavior == segmented_to_virtual(bhvWigglerHead)
              || o->behavior == segmented_to_virtual(bhvEyerokBoss) || o->behavior == segmented_to_virtual(bhvEyerokHand)
              /* patch 308: the other ones of that table that sm64coopdx aims at the nearest player (they chase, shoot at, or open for a player) */
              || o->behavior == segmented_to_virtual(bhvChuckya) || o->behavior == segmented_to_virtual(bhvPokey)
              || o->behavior == segmented_to_virtual(bhvFireSpitter) || o->behavior == segmented_to_virtual(bhvClamShell))) {
            return;
        }
    } else if (!np_w_owned(o)) {
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

/* Coins that were collected before they existed here. A coin formation only creates its coins while a player is near, so a coin that the other player took
 * far from this one (or before it came into the area) is not there when its announcement arrives, and the announcement is dropped after a while. The area's
 * record keeps it, though: every few frames the coins that have appeared since are looked through and the ones in the record are taken, like the coin that was
 * there all the time (its own behavior then marks it in the formation, so it does not come back). Red coins are left out: they count for their star. */
#define NP_W_SWEEP_PERIOD 8
void mark_goomba_as_dead(void); /* (goomba.inc.c: a goomba of a triplet is marked in its spawner, so it is not made again) */
static void np_w_goomba_dead(struct Object *g) {
    struct Object *prevObj = gCurrentObject;
    if (g->behavior != segmented_to_virtual(bhvGoomba)) {
        return;
    }
    gCurrentObject = g;
    mark_goomba_as_dead();
    gCurrentObject = prevObj;
}

/* What the record says is gone and still shows here: coins (and goombas of a triplet) that only come out when a player gets near (coin formations,
 * goomba triplets), so a replay found nothing when it came. The ones that came out in the last frames are checked every frame and go away silently,
 * before they are seen; all the coins every NP_W_SWEEP_PERIOD frames. */
static void np_w_coin_sweep(int full) {
    struct { s32 bhv; s16 pos[3]; u8 val; } g[NP_W_LOG];
    int n = 0, i, l, fresh = 0;
    s32 redBhv = np_w_bhv_of(bhvRedCoin);
    const void *goombaBhv = segmented_to_virtual(bhvGoomba);
    for (l = 0; l < NUM_OBJ_LISTS && !full && !fresh; l++) { /* (most frames nothing came out: nothing else to do) */
        struct ObjectNode *head = &gObjectLists[l];
        struct Object *obj;
        for (obj = (struct Object *) head->next; obj != (struct Object *) head; obj = (struct Object *) obj->header.next) {
            if (obj->oTimer <= 3 && obj->activeFlags != ACTIVE_FLAG_DEACTIVATED && ((obj->oInteractType & INTERACT_COIN) || obj->behavior == goombaBhv)) {
                fresh = 1;
                break;
            }
        }
    }
    if (!full && !fresh) {
        return;
    }
    for (i = 0; i < sWLogN; i++) { /* the goombas of a triplet */
        const PspNpWorldEvent *e = &sWLog[i];
        if (e->type == PSP_NP_EV_DEAD && e->bhv > 0 && e->bhv < NP_W_BRK_BASE && sKindBhv[e->bhv - 1] == bhvGoomba) {
            struct Object *o = np_w_find_kind(e->bhv, e->pos);
            if (o != NULL && o->oTimer <= 3 && o->parentObj != NULL && o->parentObj != o && o->parentObj->behavior == segmented_to_virtual(bhvGoombaTripletSpawner)) {
                np_w_goomba_dead(o);
                obj_mark_for_deletion(o);
            }
        }
    }
    for (i = 0; i < sWLogN; i++) {
        const PspNpWorldEvent *e = &sWLog[i];
        if (e->type == PSP_NP_EV_COIN && e->aux[0] == 0 && e->aux[1] == 0 && e->bhv != redBhv) {
            g[n].bhv = e->bhv;
            g[n].pos[0] = e->pos[0]; g[n].pos[1] = e->pos[1]; g[n].pos[2] = e->pos[2];
            g[n].val = e->coinValue;
            n++;
        }
    }
    if (n == 0) {
        return;
    }
    for (l = 0; l < NUM_OBJ_LISTS; l++) {
        struct ObjectNode *head = &gObjectLists[l];
        struct Object *obj;
        for (obj = (struct Object *) head->next; obj != (struct Object *) head; obj = (struct Object *) obj->header.next) {
            s32 b;
            if (!(obj->oInteractType & INTERACT_COIN) || obj->activeFlags == ACTIVE_FLAG_DEACTIVATED || (obj->oInteractStatus & INT_STATUS_INTERACTED) || obj->behavior == NULL || obj == sRemote
                || (!full && obj->oTimer > 3)) {
                continue;
            }
            b = np_w_bhv(obj);
            for (i = 0; i < n; i++) {
                f32 dx, dy, dz;
                if (g[i].bhv != b || g[i].val != (u8) obj->oDamageOrCoinValue) {
                    continue;
                }
                dx = obj->oPosX - g[i].pos[0]; dy = obj->oPosY - g[i].pos[1]; dz = obj->oPosZ - g[i].pos[2];
                if (dx * dx + dy * dy + dz * dz < 3600.0f) {
                    if (obj->oTimer <= 3 && obj->parentObj != NULL && obj->parentObj != obj && obj->parentObj->behavior == segmented_to_virtual(bhvCoinFormation)) {
                        obj->parentObj->oCoinUnkF4 |= bit_shift_left(obj->oBehParams2ndByte); /* (as its own collection does: it is not made again) */
                        obj_mark_for_deletion(obj);
                    } else {
                        obj->oInteractStatus = INT_STATUS_INTERACTED;
                    }
                    break;
                }
            }
        }
    }
}

static int sWApplyQuiet; /* the event being applied is a replay (it happened before this player came in) */
static int np_w_apply(const PspNpWorldEvent *e) {
    struct Object *o;
    switch (e->type) {
        case PSP_NP_EV_COIN:
            o = (e->aux[1] != 0) ? np_w_find_loot(e->bhv, e->aux[1]) : NULL;
            if (o == NULL) {
                o = np_w_find(e->bhv, e->pos, e->aux[1] != 0 ? 600.0f : 150.0f, e->coinValue);
            }
            if (o != NULL && sWApplyQuiet && o->behavior == segmented_to_virtual(bhvRedCoin)) {
                /* a red coin the other player took before this one came in: it just counts and goes away, with no jingle, number nor sparkles
                 * (sm64coopdx removes those with the level's state; a jingle per coin on arrival does not happen there) */
                if (o->parentObj != NULL && o->parentObj != o) {
                    o->parentObj->oHiddenStarTriggerCounter++;
                }
                o->activeFlags = ACTIVE_FLAG_DEACTIVATED;
                gPspNpStarByOther = 1;
                return 1;
            }
            if (o != NULL) {
                o->oInteractStatus = INT_STATUS_INTERACTED; /* the coin's own behavior does the rest (sparkles, red coin counter, ...) */
                if (o->behavior == segmented_to_virtual(bhvRedCoin)) {
                    gPspNpStarByOther = 1;
                }
                if (e->aux[0] > 0 && o->parentObj != NULL && sRedFixN < NP_W_REDFIX) { /* red coin: the counter is also set to the sender's (see np_w_loot_step) */
                    sRedFix[sRedFixN].star = o->parentObj;
                    sRedFix[sRedFixN].target = e->aux[0];
                    sRedFix[sRedFixN].age = 0;
                    sRedFixN++;
                }
            }
            return o != NULL;
        case PSP_NP_EV_STAR:
            /* (Bowser's key and the grand star come out where each console's Bowser ended up, which is not quite the same place: there is one of them) */
            o = np_w_find(e->bhv, e->pos, (e->bhv == np_w_bhv_of(bhvGrandStar) || e->bhv == np_w_bhv_of(bhvBowserKey)) ? 8000.0f : 500.0f, 0);
            if (o != NULL) {
                if (!sWApplyQuiet && !(o->header.gfx.node.flags & GRAPH_RENDER_INVISIBLE)) {
                    spawn_object(o, MODEL_NONE, bhvStarKeyCollectionPuffSpawner); /* (the puff the game makes where Mario takes a star or key) */
                }
                o->oInteractStatus = INT_STATUS_INTERACTED;
            }
            return o != NULL;
        case PSP_NP_EV_ITEM: /* (sm64coopdx: find_nearest_item within 1000, then oInteractStatus = INTERACTED: the object's own loop does the rest) */
            if (e->bhv == np_w_bhv_of(bhvKoopaShell) || e->bhv == np_w_bhv_of(bhvKoopaShellUnderwater)) {
                /* the other player got on a shell (or took the one under water): sm64coopdx has a single shell, so this copy goes away (the other player's
                 * shell is shown under its model instead, see np_w_remote_shell) */
                o = np_w_find(e->bhv, e->pos, 400.0f, 0);
                if (o != NULL && o->oAction == 0 && o->oHeldState == HELD_FREE) {
                    struct Object *saved = gCurrentObject;
                    gCurrentObject = o;
                    spawn_mist_particles();
                    gCurrentObject = saved;
                    obj_mark_for_deletion(o);
                }
                return 1;
            }
            o = np_w_find(e->bhv, e->pos, 1000.0f, 0);
            if (o != NULL) {
                o->oInteractStatus = INT_STATUS_INTERACTED;
            }
            return o != NULL;
        case PSP_NP_EV_CAP:
        case PSP_NP_EV_1UP:
            /* a 1-up that walks, runs away, slides or flies out of a butterfly is somewhere else on each console: the nearest one of its kind
             * (sm64coopdx: find_nearest_item, within 1000). One placed in the level does not move: it is matched closely. */
            o = np_w_find(e->bhv, e->pos, (e->type == PSP_NP_EV_1UP && e->bhv != np_w_bhv_of(bhv1Up)) ? 1000.0f : 150.0f, 0);
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
                if (o->parentObj != NULL && o->parentObj != o && o->parentObj->behavior == segmented_to_virtual(bhvMerryGoRoundBooManager)) {
                    o->parentObj->oMerryGoRoundBooManagerNumBoosKilled++; /* (a merry-go-round boo counts itself when it dies; this copy does not run its death) */
                }
                set_object_respawn_info_bits(o, RESPAWN_INFO_DONT_RESPAWN);
                np_w_goomba_dead(o);
                if ((o->behavior == segmented_to_virtual(bhvSmallBully) || o->behavior == segmented_to_virtual(bhvSmallChillBully))
                    && o->oBullySubtype == BULLY_STYPE_MINION && o->parentObj != NULL && o->parentObj != o) {
                    /* (its own death counts it in the big bully that waits for the three: sm64coopdx syncs that count. Killed on the other console, the big
                     * bully of Lethal Lava Land / Snowman's Land came out only there) */
                    o->parentObj->oBullyKBTimerAndMinionKOCounter++;
                }
                if (o->behavior == segmented_to_virtual(bhvFirePiranhaPlant)) {
                    /* (sm64coopdx syncs the count of the big ones killed: the star of Tiny-Huge Island comes out at the fifth, whichever player killed each) */
                    extern s32 sNumActiveFirePiranhaPlants, sNumKilledFirePiranhaPlants;
                    if (o->oFirePiranhaPlantActive) {
                        sNumActiveFirePiranhaPlants--;
                        o->oFirePiranhaPlantActive = FALSE;
                    }
                    if ((u16) (o->oBehParams >> 16) != 0) {
                        sNumKilledFirePiranhaPlants++;
                    }
                }
                gCurrentObject = o; /* (the effect is made around the object that is running: here nobody is) */
                spawn_mist_particles(); /* the puff the enemy leaves where it died, which its own death would have made */
                gCurrentObject = saved;
                obj_mark_for_deletion(o);
                {
                    int k;
                    for (k = 0; k < sTrkN; k++) { /* (patch 308: not reported back as a death of this console. When several share a home, such as the boos of
                                                     * the merry-go-round, the death sent back took a second one with it) */
                        if (sTrk[k].o == o) {
                            sTrk[k] = sTrk[--sTrkN];
                            break;
                        }
                    }
                }
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
        case PSP_NP_EV_LEVEL:
            np_w_level_state_apply(e);
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
    np_w_hand_unhide_all();
    if (np_w_rem_shell_ok()) {
        obj_mark_for_deletion(sRemShell);
    }
    sRemShell = NULL;
#if PSP_NETPLAY_TRIGGERS
    sTrgSeenN = 0;
#endif
    sWLogN = sWPendN = sWAge = sWPeerHere = sPeerNowKnown = 0;
    sWReplayAt = -1;
#if PSP_NETPLAY_ENEMIES
    sClock += 1000; /* every stamp is stale now: nothing is followed or claimed */
    sLootOutN = sLootFixN = sRedFixN = 0;
    memset(sSent, 0, sizeof(sSent));
    memset(sRemHeld, 0, sizeof(sRemHeld));
    sTrkN = 0;
    sPlatN = sPlatCursor = sPlatTick = 0;
    sBowserHeard = sBowserPeerWait = sBowserCarried = 0;
    sBowserCarriedObj = NULL;
    gPspBowserRewardOf = NULL;
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
    { bhvCheckerboardPlatformSub, 0, { NPF(oMoveAnglePitch), NPF(oAngleVelPitch), NPF(oCheckerBoardPlatformUnkF8) } }, /* (BitFS / BitS: they flip on a timer that starts when the area loads, a different moment on each console) */
    { bhvActivatedBackAndForthPlatform, 0, { NPF(oActivatedBackAndForthPlatformMaxOffset), NPF(oActivatedBackAndForthPlatformOffset), NPF(oActivatedBackAndForthPlatformVel),
          NPF(oActivatedBackAndForthPlatformCountdown), NPF(oActivatedBackAndForthPlatformStartYaw), NPF(oActivatedBackAndForthPlatformVertical), NPF(oActivatedBackAndForthPlatformFlipRotation) } },
    { bhvFerrisWheelAxle, 0, { NPF(oFaceAngleRoll) } },
    { bhvWaterBombCannon, 2, { NPF(oWaterCannonUnkF4), NPF(oWaterCannonUnkF8), NPF(oWaterCannonUnkFC), NPF(oWaterCannonUnk100) } }, /* (BoB: where it aims and when it shoots is
          * random; sm64coopdx leaves it to each console, here the one of the nearest player goes along so both see the same shots) */
    /* patch 260: the other platforms sm64coopdx keeps in step (most only need the standard values: position, action, timer, angles) */
    { bhvArrowLift, 0, { 0 } },
    { bhvBbhTiltingTrapPlatform, 0, { NPF(oAngleVelPitch) } },
    { bhvHauntedBookshelf, 2, { NPF(oHauntedBookshelfShouldOpen) } }, /* (patch 308: driven by the console of the nearest player, like its manager: the one
                                                                         * that solved the book puzzle opens it, host or not) */
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
    { bhvJrbSlidingBox, 0, { NPF(oJrbSlidingBoxUnkF8), NPF(oJrbSlidingBoxUnkFC), NPF(oParentRelativePosZ) } }, /* (where it is on the ship: it adds up its slide
          * every frame, so with only its swing corrected a console that entered at another moment kept it off its place, half into the ship) */
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
    /* patch 293: what else sm64coopdx syncs by its fields (same fields; the position, angles, action and timer are in the standard ones). The Bowser lava platform
     * is tilted by Bowser's own script on each console (his state is carried), the rest of its values go like those of a platform. */
    { bhvTiltingBowserLavaPlatform, 0, { NPF(oAngleVelPitch), NPF(oAngleVelRoll), NPF(oMoveAnglePitch), NPF(oMoveAngleRoll) } },
    { bhvJumpingBox, 3, { NPF(oJumpingBoxUnkF4) } },
    { bhvFireSpitter, 3, { NPF(oFireSpitterScaleVel) } },
    { bhvBooCage, 3, { 0 } },
    { bhvSnowmansHead, 3, { 0 } },
    { bhvHoot, 2, { NPF(oHootAvailability), NPF(oMoveAnglePitch) } }, /* (sm64coopdx syncs whether it is awake, where it is and its pitch; pole-like list, see np_p_scan) */    /* (default object list: it is looked through only in Snowman's Land, see np_p_scan) */
    { bhvSLSnowmanWind, 3, { 0 } },
    /* patch 308. The two platforms of Bowser in the Dark World that go round a square: they move by their own timer, which starts when each console loads
     * the level (sm64coopdx syncs their whole state). The manager of the book puzzle of Big Boo's Haunt: which books are out and how many were pushed in the
     * right order (oBookSwitchManagerUnkF4 / F8); the console of the player nearest to it drives it (sm64coopdx), so the puzzle can be solved by either
     * player and the bookshelf opens on both consoles. */
    { bhvSquarishPathMoving, 0, { 0 } },
    { bhvHauntedBookshelfManager, 2, { NPF(oBookSwitchManagerUnkF4), NPF(oBookSwitchManagerUnkF8) } },
    /* patch 309: Yoshi on the castle roof (sm64coopdx syncs where it walks to; the player it talks to drives it, see yoshi.inc.c) */
    { bhvYoshi, 2, { NPF(oYoshiChosenHome), NPF(oYoshiTargetYaw), NPF(oHomeX), NPF(oHomeZ), NPF(oYoshiBlinkTimer) } },
    /* patch 312: the rest of what sm64coopdx keeps in step by its fields. The flamethrowers (they start blowing when a player comes near, from their own
     * action and timer: in step, their flames come out together on both consoles),
     * the ring maker of the jet stream there (its timer decides when each ring comes out, and the count of the rings passed is shared) and the maker of
     * the sliding snow mounds of Snowman's Land (its timer decides when each mound comes out). The console of the nearest player drives them. */
    { bhvFlamethrower, 2, { NPF(oFlameThowerUnk110) } },
    { bhvJetStreamRingSpawner, 2, { NPF(oWaterRingSpawnerRingsCollected), NPF(oWaterRingMgrNextRingIndex), NPF(oWaterRingMgrLastRingCollected) } },
    { bhvSnowMoundSpawn, 2, { 0 } },
    /* patch 318: platforms that turn or flap on a timer started when each console loaded the area (the octagonal platform of Rainbow Ride and Bowser in the Sky,
     * the wings of the cruiser, the bridge with flames), and the tilting pyramid of Bowser in the Fire Sea (it leans where the player stands: the nearest console drives it) */
    { bhvOctagonalPlatformRotating, 0, { NPF(oAngleVelYaw) } },
    { bhvRrCruiserWing, 0, { NPF(oRRCruiserWingUnkF4), NPF(oRRCruiserWingUnkF8) } },
    { bhvRrRotatingBridgePlatform, 2, { NPF(oFlameThowerUnk110), NPF(oAngleVelYaw) } },
    { bhvBitfsTiltingInvertedPyramid, 2, { NPF(oTiltingPyramidNormalX), NPF(oTiltingPyramidNormalY), NPF(oTiltingPyramidNormalZ) } },
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
    uint32_t sent; /* sClock when this console last sent it (see np_p_frame: what waited longest, moves and is near the other player goes first) */
} sPlat[NP_P_MAX];

static void np_p_ident(const struct Object *o, uint8_t *kind, uint8_t *ord) {
    int i;
    *kind = *ord = 0xFF;
    for (i = 0; o != NULL && i < sPlatN; i++) {
        if (sPlat[i].o == o) {
            *kind = sPlat[i].kind;
            *ord = sPlat[i].ord;
            return;
        }
    }
}
static struct Object *np_p_find(uint8_t kind, uint8_t ord) {
    int i;
    for (i = 0; i < sPlatN; i++) {
        if (sPlat[i].kind == kind && sPlat[i].ord == ord) {
            return sPlat[i].o;
        }
    }
    return NULL;
}
static struct Object *sPlatPrevO[NP_P_MAX]; /* the previous scan, to carry 'sent' over (the lists keep their order: a short look ahead finds them) */
static uint32_t sPlatPrevSent[NP_P_MAX];

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
#define NP_P_DRIVEN 4 /* flags: the sender drives it now (its player talks to it, or psp_np_world_drive): taken even where the other player is the nearer one */
static uint16_t np_p_hash(const struct Object *o) {
    return (uint16_t) ((int) o->oHomeX * 31 + (int) o->oHomeZ * 17 + (int) o->oHomeY * 7);
}

/* a state object the console of the nearest player drives (the puzzle chests, ...): the star it makes is announced like the one of a boss */
static int np_p_nearest_driven(const struct Object *o) {
    const int k = np_p_kind(o);
    return k >= 0 && np_p_near(k);
}

/* the platforms of this area, with their order within their kind (the surface list keeps the order they were created in) */
static int np_p_default_list_level(void) {
    switch (gCurrLevelNum) {
        case LEVEL_JRB: case LEVEL_SL: case LEVEL_DDD:
        case LEVEL_BBH: case LEVEL_HMC: case LEVEL_LLL: case LEVEL_RR: case LEVEL_BITDW: case LEVEL_BITFS: case LEVEL_BITS: /* (flamethrowers) */
        case LEVEL_BOB: /* (the water bomb cannon) */
            return 1;
    }
    return 0;
}

static void np_p_scan(void) {
    static const u8 lists[4] = { OBJ_LIST_SURFACE, OBJ_LIST_GENACTOR, OBJ_LIST_DEFAULT, OBJ_LIST_POLELIKE };
    uint8_t count[NP_P_KINDS];
    int i, l, prevN, j;
    memset(count, 0, sizeof(count));
    prevN = sPlatN;
    for (i = 0; i < prevN; i++) {
        sPlatPrevO[i] = sPlat[i].o;
        sPlatPrevSent[i] = sPlat[i].sent;
    }
    sPlatN = 0;
    for (l = 0; l < 4; l++) {
        struct ObjectNode *head;
        struct Object *obj;
        /* (the default list is only looked through where something of it is kept: the puzzle chests, the snowman, the snow mounds, the jet stream
         * rings, the flamethrowers; the pole-like list only in Whomp's Fortress, for the Hoot) */
        if ((l == 2 && !np_p_default_list_level()) || (l == 3 && gCurrLevelNum != LEVEL_WF)) {
            continue;
        }
        head = &gObjectLists[lists[l]];
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
    for (i = 0, j = 0; i < sPlatN; i++) {
        int k;
        sPlat[i].n = count[sPlat[i].kind];
        sPlat[i].sent = sClock - 255U; /* (new: as if it had waited long) */
        for (k = j; k < prevN && k < j + 8; k++) {
            if (sPlatPrevO[k] == sPlat[i].o) {
                sPlat[i].sent = sPlatPrevSent[k];
                j = k + 1;
                break;
            }
        }
    }
}

/* How urgent a correction of this platform is: the frames since this console last sent it, more for one that moves (a Hoot, a ball, a turning
 * platform drift apart fast; a still one does not) and for one near the other player (what it is looking at). */
static uint32_t np_p_urgency(int i) {
    const struct Object *o = sPlat[i].o;
    uint32_t age = sClock - sPlat[i].sent;
    uint32_t w = 1;
    if (age > 255U) {
        age = 255U;
    }
    if (absf(o->oForwardVel) > 0.5f || absf(o->oVelY) > 0.5f || absf(o->oVelX) > 0.5f || absf(o->oVelZ) > 0.5f || o->oAngleVelYaw != 0 || o->oAngleVelPitch != 0
        || o->oAngleVelRoll != 0) {
        w += 2;
    }
    if (np_remote_alive() && dist_between_objects(o, sRemote) < 3000.0f) {
        w += 2;
    }
    return age * w;
}

/* A platform that is a child of a synced one (the platforms of the ferris wheel hang from its axle: they have no state of their own) stands for its parent. */
static const struct Object *np_p_root(const struct Object *o) {
    int i;
    if (o == NULL) {
        return NULL;
    }
    for (i = 0; i < sPlatN; i++) {
        if (sPlat[i].o == o) {
            return o;
        }
    }
    for (i = 0; i < sPlatN; i++) {
        if (sPlat[i].o == o->parentObj && o->parentObj != o && sPlatKind[sPlat[i].kind].ttc == 0) {
            return o->parentObj;
        }
    }
    return o;
}

static int np_p_riding(const struct Object *o) {
    return gMarioObject != NULL && np_p_root(gMarioObject->platform) == o;
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
    u32 act;
    if (o->oHeldState != HELD_FREE || !np_remote_alive()) {
        return 0;
    }
    if ((sClock - sBowserCarried) < NP_P_BOWSER_CARRY_FRAMES) {
        return 1;
    }
    /* patch 318: the platform stream that says so is unreliable and comes every 4+ frames (more on a slow radio): one packet lost or late and this copy
     * dropped out of the other player's hands for a few frames, shown by itself where it hung (60 above the floor) and falling, while that player went on
     * swinging him: "Luigi and Bowser rise while spinning" on the other console. The other player's own action, which comes with every pose, decides it */
    act = psp_np_game_remote_action();
    return o == sBowserCarriedObj && (act == ACT_HOLDING_BOWSER || act == ACT_PICKING_UP_BOWSER);
}

void psp_np_world_bowser_carry_step(struct Object *o) {
    const s16 yaw = (s16) sRemote->header.gfx.angle[1]; /* the way the other player faces: the way Bowser hangs, and the way he goes when it lets go */
    o->oPosX = sRemote->oPosX + sins(yaw) * 330.0f;
    o->oPosZ = sRemote->oPosZ + coss(yaw) * 330.0f;
    o->oPosY = sRemote->oPosY + 60.0f;
    o->oMoveAngleYaw = o->oFaceAngleYaw = yaw;
    o->oForwardVel = o->oVelY = 0.0f;
    o->oAngleVelYaw = 0;
    np_w_hand_hide(o); /* (drawn in that player's hands, see psp_np_game_remote_held; this position is only for the game logic) */
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
    if (b == bhvChainChomp) { /* (only while it is chained: once it is set free each console plays the release itself, see np_p_apply) */
        return o->oAction == CHAIN_CHOMP_ACT_MOVE && o->oChainChompSegments != NULL && o->parentObj != NULL && o->parentObj != o
            && o->oChainChompReleaseStatus == CHAIN_CHOMP_NOT_RELEASED;
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
    if ((o->activeFlags & ACTIVE_FLAG_INITIATED_TIME_STOP) || (sClock - o->unused2) < NP_P_RIDE_FRAMES) {
        r->flags |= NP_P_DRIVEN; /* (sm64coopdx sends the object of a running text from the console of the player that reads it, network_send_object) */
    }
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

/* A correction from the other console moves the copy at once (its next update starts from there), but the picture eases over (sm64coopdx's
 * interpolation of the synced objects): what is left of the jump is kept as an offset of the drawn position and angle that fades in a few frames.
 * Without this a flying Hoot (or any enemy this console follows) jumped a little at every packet. Not for solid objects: Mario stands on what is drawn. */
#define NP_SM_MAX 16
static struct { struct Object *o; const BehaviorScript *bhv; float e[3]; s16 ey; } sSm[NP_SM_MAX];
static void np_sm_add(struct Object *o, const float old[3], s16 oldYaw) {
    int i, free = -1;
    float dx, dy, dz;
    if (o->collisionData != NULL || !(o->oFlags & OBJ_FLAG_UPDATE_GFX_POS_AND_ANGLE)) {
        return;
    }
    dx = old[0] - o->oPosX;
    dy = old[1] - o->oPosY;
    dz = old[2] - o->oPosZ;
    for (i = 0; i < NP_SM_MAX; i++) {
        if (sSm[i].o == o) {
            break;
        }
        if (sSm[i].o == NULL && free < 0) {
            free = i;
        }
    }
    if (i == NP_SM_MAX) {
        if (free < 0) {
            return;
        }
        i = free;
        sSm[i].o = o;
        sSm[i].bhv = o->behavior;
        sSm[i].e[0] = sSm[i].e[1] = sSm[i].e[2] = 0.0f;
        sSm[i].ey = 0;
    }
    sSm[i].e[0] += dx;
    sSm[i].e[1] += dy;
    sSm[i].e[2] += dz;
    sSm[i].ey = (s16) (sSm[i].ey + (s16) (oldYaw - (s16) o->oFaceAngleYaw));
    if (fabsf(sSm[i].e[0]) + fabsf(sSm[i].e[1]) + fabsf(sSm[i].e[2]) > 600.0f) {
        sSm[i].o = NULL; /* a jump that big is a teleport (a warp, a respawn): shown as it is */
    }
}

void psp_np_world_smooth_step(void) {
    int i, k;
    for (i = 0; i < NP_SM_MAX; i++) {
        struct Object *o = sSm[i].o;
        if (o == NULL) {
            continue;
        }
        if (o->activeFlags == ACTIVE_FLAG_DEACTIVATED || o->behavior != sSm[i].bhv || psp_netplay_state() != PSP_NP_CONNECTED) {
            sSm[i].o = NULL;
            continue;
        }
        for (k = 0; k < 3; k++) {
            sSm[i].e[k] *= 0.7f;
        }
        sSm[i].ey = (s16) (sSm[i].ey * 7 / 10);
        if (fabsf(sSm[i].e[0]) + fabsf(sSm[i].e[1]) + fabsf(sSm[i].e[2]) < 1.0f && sSm[i].ey > -0x80 && sSm[i].ey < 0x80) {
            sSm[i].o = NULL;
            continue;
        }
        /* (the update just copied the object's own position into the picture) */
        o->header.gfx.pos[0] = o->oPosX + sSm[i].e[0];
        o->header.gfx.pos[1] = o->oPosY + o->oGraphYOffset + sSm[i].e[1]; /* (as obj_update_gfx_pos_and_angle: without the offset of its model the chain chomp, 240 up, sank into the ground) */
        o->header.gfx.pos[2] = o->oPosZ + sSm[i].e[2];
        o->header.gfx.angle[1] = (s16) (o->oFaceAngleYaw + sSm[i].ey);
    }
}

static void np_p_apply(const PspNpFld *r) {
    struct Object *o;
    float old[3];
    s16 oldYaw;
    uint32_t pokeyFlags = 0;
    s32 pokeyNum = 0, heldBefore = 0, deadBefore = 0;
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
            sBowserCarriedObj = o;
        }
        return;
    }
    if (sPlatKind[r->kind].bhv == bhvChainChomp && r->w[NP_P_STD + 5] != CHAIN_CHOMP_NOT_RELEASED) {
        /* the other console's chomp has been set free. Its release (the camera cutscene, the lunges, the gate, the jump away) is played by each console on
         * its own: the player in the cutscene must see it to the end, whatever the other player does meanwhile (taking the other console's later states
         * left the chomp of the player in the cutscene halfway, and that player stuck in it). Here it is only set free too, if it was not yet. */
        if (o->oAction == CHAIN_CHOMP_ACT_MOVE && o->oChainChompReleaseStatus == CHAIN_CHOMP_NOT_RELEASED && o->oChainChompSegments != NULL) {
            o->oChainChompReleaseStatus = CHAIN_CHOMP_RELEASED_TRIGGER_CUTSCENE;
            o->unused1 = sClock;
        }
        return;
    }
    if (sPlatKind[r->kind].bhv == bhvFallingBowserPlatform) {
        return; /* only its fall is told (PSP_NP_SIG_BOWSER_PLAT), as sm64coopdx's SYNC_DISTANCE_ONLY_EVENTS: a state from a console where it still stands
                 * put a falling one back up */
    }
    if (!np_p_ready(o, r->kind) || !np_p_state_ok(r->kind, r)) {
        return;
    }
    if (sPlatKind[r->kind].bhv == bhvBowser && o->oAction == 4 && r->w[NP_P_W_ACTION] == 4) {
        /* his death (fly back, bounce, last words, twirl into the trophy) goes forward only: this copy ran a step ahead of the packet (its own script goes on between
         * packets), and taking the older state of the packet put it back and made it jump forward again: Bowser stuttered where he starts to rise and spin */
        const s32 theirSub = (s32) r->w[9], theirF8 = (s32) r->w[NP_P_STD + 1];
        if (o->oSubAction > theirSub || (o->oSubAction == theirSub && o->oBowserUnkF8 > theirF8)) {
            return;
        }
    }
    if (sPlatKind[r->kind].bhv == bhvHoot) {
        /* the Hoot only ever wakes up (sm64coopdx: one shared Hoot): a copy that is less awake than this one is not taken, one that is more awake always is
         * (the console of the nearest player may be the one that still has it asleep in its tree) */
        const s32 theirs = (s32) r->w[NP_P_STD + 0];
        if (theirs < o->oHootAvailability) {
            return;
        }
        if (theirs == HOOT_AVAIL_READY_TO_FLY && psp_np_game_remote_action() == ACT_RIDING_HOOT) {
            o->oHootAvailability = theirs;
            return; /* the other player flies it: this copy hangs above that player's model (hoot_follow_remote_rider), which moves smoothly, not at the packet rate */
        }
        if (theirs > o->oHootAvailability) {
            goto take;
        }
    }
    if (np_p_near(r->kind)) {
        if (o->activeFlags & ACTIVE_FLAG_INITIATED_TIME_STOP) {
            return; /* a text of this copy is running here (its player talks to it): this copy counts until it ends, whoever comes nearer meanwhile */
        }
        if ((sClock - o->unused2) < NP_P_RIDE_FRAMES) {
            return; /* this console drives it (psp_np_world_drive) */
        }
        if ((r->flags & NP_P_DRIVEN) && np_p_near(r->kind)) {
            goto take; /* the other console drives it (its player reads its text, e.g. Bowser's last words, and his twirl into the trophy right after) */
        }
        if (np_w_rule_mine(o) && !(sPlatKind[r->kind].bhv == bhvBowser && o->oAction == NP_P_BOWSER_WAIT)) {
            return; /* the nearest player is this one: this copy is the one that counts (but a Bowser that waits for the fight takes it from whoever has it) */
        }
    } else if (!sWHost && (sClock - o->unused2) < NP_P_RIDE_FRAMES) {
        return; /* the client drives the platforms it is standing on */
    }
take:
    if (sPlatKind[r->kind].ttc == 1 && r->ttc != (uint8_t) gTTCSpeedSetting) {
        return; /* (TTC: the speed is the one of the player who was there first, handed over with the level state; until then the other speed is not mixed in) */
    }
    old[0] = o->oPosX;
    old[1] = o->oPosY;
    old[2] = o->oPosZ;
    if (sPlatKind[r->kind].bhv == bhvPokey) {
        pokeyFlags = o->oPokeyAliveBodyPartFlags;
        pokeyNum = o->oPokeyNumAliveBodyParts;
    }
    heldBefore = o->oHeldState;
    deadBefore = o->oAction == 4 && o->oSubAction >= 3;
    oldYaw = (s16) o->oFaceAngleYaw;
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
        if (o->oAction == 4 && (o->oSubAction == 4 || o->oSubAction == 11)) {
            /* patch 311: on the other console he is beaten and gone (bowser_dead_hide: his last state, the scale is not carried). A player that comes back into
             * the arena saw him standing there, see-through, instead of nothing (sm64coopdx's late joiner gets him hidden) */
            o->header.gfx.scale[0] = o->header.gfx.scale[1] = o->header.gfx.scale[2] = 0.0f;
            o->oForwardVel = 0.0f;
            o->oVelY = 0.0f;
            o->oGravity = 0.0f;
            o->oIntangibleTimer = -1;
            if (!deadBefore || gPspBowserRewardOf != o) {
                /* this copy never went through the end of the fight, so it never left the key (or the grand star): the late joiner gets one, as in
                 * sm64coopdx (its reward is a synced spawn that the level state hands to whoever comes in) */
                struct Object *prevObj = gCurrentObject, *reward;
                gCurrentObject = o;
                reward = cur_obj_nearest_object_with_behavior(o->oBehParams2ndByte == 2 ? bhvGrandStar : bhvBowserKey);
                if (reward == NULL) {
                    reward = spawn_object(o, o->oBehParams2ndByte == 2 ? MODEL_STAR : MODEL_BOWSER_KEY, o->oBehParams2ndByte == 2 ? bhvGrandStar : bhvBowserKey);
                    gPspBowserRewardOf = o; /* (once: a key this player then takes is not given again by the next packet) */
                    if (reward != NULL) {
                        reward->oAngleVelYaw = o->oAngleVelYaw;
                        gSecondCameraFocus = reward; /* (as bowser_spawn_grand_star_key does) */
                    }
                }
                gCurrentObject = prevObj;
            }
        }
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
    if (!np_p_riding(o)) {
        np_sm_add(o, old, oldYaw);
    }
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
    if (sPlatKind[sPlat[i].kind].bhv == bhvHoot && o->oHootAvailability != HOOT_AVAIL_ASLEEP_IN_TREE && (sClock - o->unused1) >= 8) {
        return 1; /* awake here: told even if the other player is the nearer one, its console may still have it asleep (see np_p_apply) */
    }
    if (np_p_near(sPlat[i].kind)) {
        if (bowser && (sClock - sBowserPeerWait) < NP_P_BOWSER_FOLLOW && (sClock - o->unused1) >= NP_P_RIDE_FRAMES) {
            return 1; /* the other player has just come in and waits for this Bowser's state: it goes out even if that player is the nearest to him */
        }
        if (o->activeFlags & ACTIVE_FLAG_INITIATED_TIME_STOP) {
            return 1; /* its text runs here: this console drives it until the text ends (see np_p_apply) */
        }
        if ((sClock - o->unused2) < NP_P_RIDE_FRAMES) {
            return 1; /* this console drives it (psp_np_world_drive), wherever the players are */
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
        const struct Object *ridden = np_p_root(gMarioObject->platform);
        for (i = 0; i < sPlatN; i++) {
            if (sPlat[i].o == ridden && !np_p_near(sPlat[i].kind)) { /* (one of the nearest player's console is not claimed by standing on it) */
                sPlat[i].o->unused2 = sClock;
            }
        }
    }
    if (++sPlatTick < NP_P_PERIOD || sPlatN == 0) {
        return;
    }
    sPlatTick = 0;
    for (i = 0; i < sPlatN && nb < NP_P_PER_BATCH - 1; i++) { /* the bosses first, at every batch (kept one place free for the others) */
        if (sPlatKind[sPlat[i].kind].ttc == 4 && np_p_sendable(i)) {
            sPlat[i].sent = sClock;
            np_p_pack(&batch[nb++], i, sWHost);
        }
    }
    /* then the most urgent ones (see np_p_urgency). It used to be a plain rotation: in an area with many platforms each one was corrected only every second
     * or so, and a fast one (the Hoot, a rolling ball) drifted apart in between. */
    {
        uint8_t ok[NP_P_MAX];
        uint32_t urg[NP_P_MAX];
        for (i = 0; i < sPlatN; i++) {
            ok[i] = sPlatKind[sPlat[i].kind].ttc != 4 && np_p_sendable(i); /* (the bosses went already) */
            urg[i] = ok[i] ? np_p_urgency(i) : 0;
        }
        for (tries = nb; tries < NP_P_PER_BATCH; tries++) {
            int best = -1;
            uint32_t bestU = 0;
            for (i = 0; i < sPlatN; i++) {
                if (ok[i] && (best < 0 || urg[i] > bestU)) {
                    best = i;
                    bestU = urg[i];
                }
            }
            if (best < 0) {
                break;
            }
            ok[best] = 0;
            sPlat[best].sent = sClock;
            np_p_pack(&batch[nb++], best, sWHost);
        }
    }
    if (nb > 0) {
        psp_np_world_fld_send(batch, nb * NP_P_REC);
    }
}
#else
static void np_p_ident(const struct Object *o, uint8_t *kind, uint8_t *ord) {
    (void) o;
    *kind = *ord = 0xFF;
}
static struct Object *np_p_find(uint8_t kind, uint8_t ord) {
    (void) kind;
    (void) ord;
    return NULL;
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
    {
        const u32 ra = psp_np_game_remote_action();
        if (np_remote_alive() && (ra == ACT_READING_NPC_DIALOG || ra == ACT_WAITING_FOR_DIALOG || ra == ACT_READING_AUTOMATIC_DIALOG)) {
            sRemDlgT = sClock;
        }
    }
    if (gCurrentArea == NULL) {
        return; /* a warp is in progress: the events wait */
    }
    if (psp_np_world_quiet()) {
        static u8 sDrop[PSP_NP_OBJ_BYTES > PSP_NP_FLD_BYTES ? PSP_NP_OBJ_BYTES : PSP_NP_FLD_BYTES];
        while (psp_np_world_take(&e)) {
        }
        psp_np_world_obj_take(sDrop);
        psp_np_world_fld_take(sDrop); /* (what the other console still sent is dropped) */
        sWLevel = sWArea = -1; /* (the area after the credits starts from scratch) */
        sWPeerHere = 0;
        sWReplayAt = -1;
        return;
    }
    if (sWLevel != gCurrLevelNum || sWArea != np_area_key()) { /* a new area: its state starts from scratch (what the other player has done there comes in as a replay) */
        sWLevel = gCurrLevelNum;
        sWArea = np_area_key();
        sWLogN = sWPendN = sWAge = 0;
        sWAreaFrames = 0;
        sWReplayAt = -1;
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
        sBowserCarriedObj = NULL;
        gPspBowserRewardOf = NULL;
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
    if (sWAreaFrames < 0x7FFFFFFF) {
        sWAreaFrames++;
    }
    here = peerKnown && peerLevel == sWLevel && peerArea == sWArea;
    if (here && !sWPeerHere) {
#if PSP_NETPLAY_ENEMIES
        memset(sSent, 0, sizeof(sSent)); /* a player just came in: it needs every enemy again */
#endif
        sWReplayAt = 0; /* the other player has just come into this area: it gets what was collected here (np_w_replay_step) */
        sLvlBuilt = 0;
    }
    sWPeerHere = here;
    np_w_replay_step();
    sWHost = isHost;
    np_w_remote_floor();
    np_w_remote_shell();
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
        np_w_hand_unhide_step();
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
        if (replay && e.type == PSP_NP_EV_ACTION && e.coinValue == NP_W_ACT_CHOMP) {
            e.value = 0; /* (a chomp set free before this player came in: no release to play now) */
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
        } else if (e.type == PSP_NP_EV_COIN && e.value > m->numCoins) {
            /* patch 311: a player that comes into a level where the other one has been collecting gets the level's count (sm64coopdx sends it with the
             * level, PACKET_LEVEL): the count the other console had at its last coin. Shown at once, not counted up coin by coin with its sound */
            m->numCoins = e.value;
            gHudDisplay.coins = e.value;
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
            sWReplayAt = 0; /* (np_w_replay_step) */
            sLvlBuilt = 0;
            continue;
        }
        if (e.type != PSP_NP_EV_ATTACK && e.type != PSP_NP_EV_SPAWN && e.type != PSP_NP_EV_STARSPAWN && e.type != PSP_NP_EV_DIGEST && e.type != PSP_NP_EV_LEVEL
            && e.type != PSP_NP_EV_RESYNC && (e.type != PSP_NP_EV_ACTION || e.coinValue == 7 || e.coinValue == NP_W_ACT_CHOMP
                || (e.coinValue == NP_W_ACT_SIG && e.bhv == PSP_NP_SIG_UKIKI_HAT))) {
            np_w_log(&e);
        }
        sWApplyQuiet = replay; /* (what was done before this player came in happens without a sound) */
        if (!np_w_apply(&e) && sWPendN < NP_W_PEND
            && !(replay && (e.type == PSP_NP_EV_CAP || e.type == PSP_NP_EV_1UP || e.type == PSP_NP_EV_STAR))) {
            /* (a cap, 1-up or star taken before this player came in that is not here now is not coming: its box was broken on the other console only.
             * Kept waiting, it took the cap this player's own hit on that box made a moment later: the metal cap "did not come out") */
            sWPend[sWPendN] = e;
            sWPendReplay[sWPendN] = (u8) replay;
            sWPendAge[sWPendN++] = 0;
        }
    }
    for (i = 0; i < sWPendN;) { /* the ones whose object was not there: tried again for a while */
        sWApplyQuiet = sWPendReplay[i];
        if (np_w_apply(&sWPend[i]) || ++sWPendAge[i] > (sWPendReplay[i] ? NP_W_RETRY_REPLAY : NP_W_RETRY)) {
            sWPend[i] = sWPend[--sWPendN];
            sWPendAge[i] = sWPendAge[sWPendN];
            sWPendReplay[i] = sWPendReplay[sWPendN];
        } else {
            i++;
        }
    }
    sWApplyQuiet = 0;
    if (sWLogN > 0) {
        /* (also once the other player left: what it took in this copy of the area stays taken here) */
        np_w_coin_sweep((sWAge % NP_W_SWEEP_PERIOD) == 0);
    }
}

#endif /* PSP_NETPLAY_WORLD */

#endif
