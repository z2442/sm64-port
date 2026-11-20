
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
    obj_set_hitbox(o, &sRecoveryHeartHitbox);
    /* sm64coopdx: the heart spins for any player that touches it (as fast as the nearest one runs), and each turn heals this console's player, as in the
     * original game, also the turns completed after it went past (the heart keeps spinning a while). sm64coopdx heals the nearest player whose
     * interactObj is the heart, and its own console's player when there is none: no player ever interacts with it, so that is always the case. */
    const s32 localTouches = obj_check_if_collided_with_object(o, gMarioObject);
    f32 touchVel = gMarioStates[0].forwardVel;
    s32 remoteTouches = 0;
#if defined(TARGET_PSP) && PSP_NETPLAY && PSP_NETPLAY_WORLD
    {
        struct Object *remote = psp_np_game_remote();
        struct MarioState *remoteState = psp_np_game_remote_state();
        if (remote != NULL && remoteState != NULL && psp_np_game_remote_touches(o)) {
            remoteTouches = 1;
            if (!localTouches || dist_between_objects(o, remote) < dist_between_objects(o, gMarioObject)) {
                touchVel = remoteState->forwardVel;
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
        gMarioStates[0].healCounter += 4;
        o->oSpinningHeartTotalSpin -= 0x10000;
    }

    o->oFaceAngleYaw += o->oAngleVelYaw;
}
