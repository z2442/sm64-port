
struct ObjectHitbox sRecoveryHeartHitbox = {
    /* interactType:      */ 0,
    /* downOffset:        */ 0,
    /* damageOrCoinValue: */ 0,
    /* health:            */ 0,
    /* numLootCoins:      */ 0,
    /* radius:            */ 50,
    /* height:            */ 50,
    /* hurtboxRadius:     */ 50,
    /* hurtboxHeight:     */ 50,
};

void bhv_recovery_heart_loop(void) {
    s32 healsLocal;
    obj_set_hitbox(o, &sRecoveryHeartHitbox);
    /* sm64coopdx: the heart spins for any player that touches it, and heals the nearest one that does */
    const s32 localTouches = obj_check_if_collided_with_object(o, gMarioObject);
    f32 touchVel = gMarioStates[0].forwardVel;
    s32 remoteTouches = 0;
    healsLocal = localTouches;
#if defined(TARGET_PSP) && PSP_NETPLAY && PSP_NETPLAY_WORLD
    {
        struct Object *remote = psp_np_game_remote();
        struct MarioState *remoteState = psp_np_game_remote_state();
        if (remote != NULL && remoteState != NULL && obj_check_if_collided_with_object(o, remote)) {
            remoteTouches = 1;
            if (!localTouches || dist_between_objects(o, remote) < dist_between_objects(o, gMarioObject)) {
                touchVel = remoteState->forwardVel;
                healsLocal = 0;
            }
        }
    }
#endif
    if (localTouches || remoteTouches) {
        if (o->oSpinningHeartPlayedSound == 0) {
            cur_obj_play_sound_2(SOUND_GENERAL_HEART_SPIN);
            o->oSpinningHeartPlayedSound += 1;
        }

        o->oAngleVelYaw = (s32)(200.0f * touchVel) + 1000;
    } else {
        o->oSpinningHeartPlayedSound = 0;

        if ((o->oAngleVelYaw -= 50) < 400) {
            o->oAngleVelYaw = 400;
            o->oSpinningHeartTotalSpin = 0;
        }
    }

    if ((o->oSpinningHeartTotalSpin += o->oAngleVelYaw) >= 0x10000) {
        if (healsLocal) {
            gMarioStates[0].healCounter += 4;
        }
        o->oSpinningHeartTotalSpin -= 0x10000;
    }

    o->oFaceAngleYaw += o->oAngleVelYaw;
}
