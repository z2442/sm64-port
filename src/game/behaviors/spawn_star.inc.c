// spawn_default_star.c.inc

static struct ObjectHitbox sCollectStarHitbox = {
    /* interactType:      */ INTERACT_STAR_OR_KEY,
    /* downOffset:        */ 0,
    /* damageOrCoinValue: */ 0,
    /* health:            */ 0,
    /* numLootCoins:      */ 0,
    /* radius:            */ 80,
    /* height:            */ 50,
    /* hurtboxRadius:     */ 0,
    /* hurtboxHeight:     */ 0,
};

void bhv_collect_star_init(void) {
    s8 starId;
    u8 currentLevelStarFlags;

    starId = (o->oBehParams >> 24) & 0xFF;
    currentLevelStarFlags = save_file_get_star_flags(gCurrSaveFileNum - 1, gCurrCourseNum - 1);
    if (currentLevelStarFlags & (1 << starId)) {
        o->header.gfx.sharedChild = gLoadedGraphNodes[MODEL_TRANSPARENT_STAR];
    } else {
        o->header.gfx.sharedChild = gLoadedGraphNodes[MODEL_STAR];
    }

    obj_set_hitbox(o, &sCollectStarHitbox);
}

void bhv_collect_star_loop(void) {
    o->oFaceAngleYaw += 0x800;

    if (o->oInteractStatus & INT_STATUS_INTERACTED) {
        mark_obj_for_deletion(o);
        o->oInteractStatus = 0;
    }
}

void bhv_star_spawn_init(void) {
    o->oMoveAngleYaw = atan2s(o->oHomeZ - o->oPosZ, o->oHomeX - o->oPosX);
    o->oStarSpawnDisFromHome = sqrtf(sqr(o->oHomeX - o->oPosX) + sqr(o->oHomeZ - o->oPosZ));
    o->oVelY = (o->oHomeY - o->oPosY) / 30.0f;
    o->oForwardVel = o->oStarSpawnDisFromHome / 30.0f;
    o->oStarSpawnUnkFC = o->oPosY;
#if defined(TARGET_PSP) && PSP_NETPLAY && PSP_NETPLAY_WORLD
    if (o->unused2 == PSP_NP_STAR_NO_CUTSCENE) {
        cur_obj_become_intangible(); /* (WLAN multiplayer: the star of the other player's deed just comes out here) */
        return;
    }
#endif
    if (o->oBehParams2ndByte == 0 || gCurrCourseNum == COURSE_BBH)
        cutscene_object(CUTSCENE_STAR_SPAWN, o);
    else
        cutscene_object(CUTSCENE_RED_COIN_STAR_SPAWN, o);

    set_time_stop_flags(TIME_STOP_ENABLED | TIME_STOP_MARIO_AND_DOORS);
    o->activeFlags |= ACTIVE_FLAG_INITIATED_TIME_STOP;
    cur_obj_become_intangible();
}

void bhv_star_spawn_loop(void) {
    switch (o->oAction) {
        case 0:
            o->oFaceAngleYaw += 0x1000;
            if (o->oTimer > 20)
                o->oAction = 1;
            break;

        case 1:
            obj_move_xyz_using_fvel_and_yaw(o);
            o->oStarSpawnUnkFC += o->oVelY;
            o->oPosY = o->oStarSpawnUnkFC + sins((o->oTimer * 0x8000) / 30) * 400.0f;
            o->oFaceAngleYaw += 0x1000;
            spawn_object(o, MODEL_NONE, bhvSparkleSpawn);
            cur_obj_play_sound_1(SOUND_ENV_STAR);
            if (o->oTimer == 30) {
                o->oAction = 2;
                o->oForwardVel = 0;
                play_power_star_jingle(TRUE);
            }
            break;

        case 2:
            if (o->oTimer < 20)
                o->oVelY = 20 - o->oTimer;
            else
                o->oVelY = -10.0f;

            spawn_object(o, MODEL_NONE, bhvSparkleSpawn);
            obj_move_xyz_using_fvel_and_yaw(o);
            o->oFaceAngleYaw = o->oFaceAngleYaw - o->oTimer * 0x10 + 0x1000;
            cur_obj_play_sound_1(SOUND_ENV_STAR);

            if (o->oPosY < o->oHomeY) {
                cur_obj_play_sound_2(SOUND_GENERAL_STAR_APPEARS);
                cur_obj_become_tangible();
                o->oPosY = o->oHomeY;
                o->oAction = 3;
            }
            break;

        case 3:
            o->oFaceAngleYaw += 0x800;
            if (o->oTimer == 20 && (o->activeFlags & ACTIVE_FLAG_INITIATED_TIME_STOP)) { /* (only the star that started the cutscene ends it) */
                gObjCutsceneDone = TRUE;
                clear_time_stop_flags(TIME_STOP_ENABLED | TIME_STOP_MARIO_AND_DOORS);
                o->activeFlags &= ~ACTIVE_FLAG_INITIATED_TIME_STOP;
            }

            if (o->oInteractStatus & INT_STATUS_INTERACTED) {
                if (o->activeFlags & ACTIVE_FLAG_INITIATED_TIME_STOP) {
                    /* (WLAN multiplayer) the other player took it before this star finished its own landing here (its cutscene runs on each console at its
                     * own pace): the cutscene and the frozen time end now, or this player would stay stuck in them */
                    gObjCutsceneDone = TRUE;
                    clear_time_stop_flags(TIME_STOP_ENABLED | TIME_STOP_MARIO_AND_DOORS);
                    o->activeFlags &= ~ACTIVE_FLAG_INITIATED_TIME_STOP;
                }
                mark_obj_for_deletion(o);
                o->oInteractStatus = 0;
            }
            break;
    }
}

struct Object *spawn_star(struct Object *sp30, f32 sp34, f32 sp38, f32 sp3C) {
    sp30 = spawn_object_abs_with_rot(o, 0, MODEL_STAR, bhvStarSpawnCoordinates, o->oPosX, o->oPosY,
                                     o->oPosZ, 0, 0, 0);
    sp30->oBehParams = o->oBehParams;
    sp30->oHomeX = sp34;
    sp30->oHomeY = sp38;
    sp30->oHomeZ = sp3C;
    sp30->oFaceAnglePitch = 0;
    sp30->oFaceAngleRoll = 0;
    return sp30;
}

#if defined(TARGET_PSP) && PSP_NETPLAY && PSP_NETPLAY_WORLD && PSP_NETPLAY_ENEMIES
/* (WLAN multiplayer) the star for that spot is here already: it came from the other console, where it was made first (a boss beaten there, a baby penguin
 * given back there...). The same star made here a second time would be two stars for one. */
static s32 psp_np_star_already_here(f32 x, f32 y, f32 z) {
    s32 i;
    if (psp_netplay_state() != PSP_NP_CONNECTED) {
        return FALSE;
    }
    for (i = 0; i < NUM_OBJ_LISTS; i++) {
        struct ObjectNode *head = &gObjectLists[i];
        struct Object *obj;
        for (obj = (struct Object *) head->next; obj != (struct Object *) head; obj = (struct Object *) obj->header.next) {
            if (obj->activeFlags != ACTIVE_FLAG_DEACTIVATED && obj->behavior == segmented_to_virtual(bhvStarSpawnCoordinates)
                && absf(obj->oHomeX - x) < 5.0f && absf(obj->oHomeY - y) < 5.0f && absf(obj->oHomeZ - z) < 5.0f) {
                return TRUE;
            }
        }
    }
    return FALSE;
}
#define PSP_NP_STAR_DUP(x, y, z) if (psp_np_star_already_here(x, y, z)) { return; }
#else
#define PSP_NP_STAR_DUP(x, y, z)
#endif

void spawn_default_star(f32 sp20, f32 sp24, f32 sp28) {
    struct Object *sp1C;
    PSP_NP_STAR_DUP(sp20, sp24, sp28)
    sp1C = spawn_star(sp1C, sp20, sp24, sp28);
    sp1C->oBehParams2ndByte = 0;
#if defined(TARGET_PSP) && PSP_NETPLAY && PSP_NETPLAY_WORLD && PSP_NETPLAY_ENEMIES
    psp_np_world_star(o, sp1C, 0);
#endif
}

void spawn_red_coin_cutscene_star(f32 sp20, f32 sp24, f32 sp28) {
    struct Object *sp1C;
    PSP_NP_STAR_DUP(sp20, sp24, sp28)
    sp1C = spawn_star(sp1C, sp20, sp24, sp28);
    sp1C->oBehParams2ndByte = 1;
#if defined(TARGET_PSP) && PSP_NETPLAY && PSP_NETPLAY_WORLD
    if (psp_netplay_state() == PSP_NP_CONNECTED && gPspNpStarByOther) {
        sp1C->unused2 = PSP_NP_STAR_NO_CUTSCENE; /* (the other player took the last red coin: the cutscene is theirs) */
    }
#endif
#if defined(TARGET_PSP) && PSP_NETPLAY && PSP_NETPLAY_WORLD && PSP_NETPLAY_ENEMIES
    psp_np_world_star(o, sp1C, 1);
#endif
}

void spawn_no_exit_star(f32 sp20, f32 sp24, f32 sp28) {
    struct Object *sp1C;
    PSP_NP_STAR_DUP(sp20, sp24, sp28)
    sp1C = spawn_star(sp1C, sp20, sp24, sp28);
    sp1C->oBehParams2ndByte = 1;
    sp1C->oInteractionSubtype |= INT_SUBTYPE_NO_EXIT;
#if defined(TARGET_PSP) && PSP_NETPLAY && PSP_NETPLAY_WORLD
    if (psp_netplay_state() == PSP_NP_CONNECTED && gPspNpStarByOther) {
        sp1C->unused2 = PSP_NP_STAR_NO_CUTSCENE; /* (the other player found the last secret: the cutscene is theirs) */
    }
#endif
#if defined(TARGET_PSP) && PSP_NETPLAY && PSP_NETPLAY_WORLD && PSP_NETPLAY_ENEMIES
    psp_np_world_star(o, sp1C, 2);
#endif
}

void bhv_hidden_red_coin_star_init(void) {
    s16 sp36;
    struct Object *sp30;

    if (gCurrCourseNum != COURSE_JRB)
        spawn_object(o, MODEL_TRANSPARENT_STAR, bhvRedCoinStarMarker);

    sp36 = count_objects_with_behavior(bhvRedCoin);
    if (sp36 == 0) {
        sp30 =
            spawn_object_abs_with_rot(o, 0, MODEL_STAR, bhvStar, o->oPosX, o->oPosY, o->oPosZ, 0, 0, 0);
        sp30->oBehParams = o->oBehParams;
        o->activeFlags = ACTIVE_FLAG_DEACTIVATED;
    }

    o->oHiddenStarTriggerCounter = 8 - sp36;
}

void bhv_hidden_red_coin_star_loop(void) {
    gRedCoinsCollected = o->oHiddenStarTriggerCounter;
    switch (o->oAction) {
        case 0:
            if (o->oHiddenStarTriggerCounter == 8)
                o->oAction = 1;
            break;

        case 1:
            if (o->oTimer > 2) {
                spawn_red_coin_cutscene_star(o->oPosX, o->oPosY, o->oPosZ);
                spawn_mist_particles();
                o->activeFlags = ACTIVE_FLAG_DEACTIVATED;
            }
            break;
    }
}
