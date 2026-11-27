// cannon.c.inc

void bhv_cannon_base_unused_loop(void) {
    o->oPosY += o->oVelY;
}

/* PSP WLAN multiplayer: set while the cannon plays the other player's use of it (sm64coopdx oCannonPlayerIndex > 0). A field the cannon does not use. */
#define oCannonRemote OBJECT_FIELD_S32(0x1D)

/* PSP WLAN multiplayer: the other player is inside this cannon (sm64coopdx syncs the cannon: it is taken, and shown raised and aiming). Its Mario is not
 * shown while in a cannon, so this reads its latest pose instead of its model: where it is (up to ~700 above the cannon's home: the cannon rises ~340 out
 * of its hole and the player sits 350 above it) and where it aims (its pose's pitch / yaw, see psp_np_game_capture). */
static s32 cannon_remote_inside(f32 *y, s16 *pitch, s16 *yaw) {
#if defined(TARGET_PSP) && PSP_NETPLAY && PSP_NETPLAY_WORLD
    f32 pos[3];
    s16 angle[3];
    unsigned int action;
    if (psp_np_game_remote_pose(pos, angle, &action) && action == ACT_IN_CANNON
        && absf(pos[0] - o->oHomeX) < 150.0f && absf(pos[2] - o->oHomeZ) < 150.0f && absf(pos[1] - o->oHomeY) < 900.0f) {
        if (y != NULL) {
            *y = pos[1];
            *pitch = angle[0];
            *yaw = angle[1];
        }
        return TRUE;
    }
#else
    (void) y;
    (void) pitch;
    (void) yaw;
#endif
    return FALSE;
}

void opened_cannon_act_0(void) {
    if (o->oTimer == 0) {
        o->oInteractStatus = 0;
        o->oPosX = o->oHomeX;
        o->oPosY = o->oHomeY;
        o->oPosZ = o->oHomeZ;
        o->oMoveAnglePitch = 0;
        o->oMoveAngleYaw = (s16)(o->oBehParams2ndByte << 8);
        o->oCannonUnkF4 = 0;
        o->oCannonUnk10C = 0;
        cur_obj_enable_rendering();
        cur_obj_become_tangible();
    }
    if (dist_between_objects(o, nearest_player_to_object(o)) < 500.0f) { /* (sm64coopdx: the nearest player; only this console's player gets in, below) */
        cur_obj_become_tangible();
        cur_obj_enable_rendering();
        if (o->oInteractStatus & INT_STATUS_INTERACTED && !cannon_remote_inside(NULL, NULL, NULL)
            && (!(o->oInteractStatus
                  & INT_STATUS_TOUCHED_BOB_OMB))) // bob-omb explodes when it gets into a cannon
        {
            o->oAction = 4;
            o->oCannonUnk10C = 1;
            o->oCannonUnkF8 = 1;
        } else
            o->oInteractStatus = 0;
    } else {
        cur_obj_become_intangible();
        cur_obj_disable_rendering();
        o->oCannonUnk10C = 0;
    }
}

void opened_cannon_act_4(void) {
    if (o->oTimer == 0)
        cur_obj_play_sound_2(SOUND_OBJ_CANNON1);
    o->oPosY += 5.0f;
    o->oPosX += (f32)((o->oTimer / 2 & 1) - 0.5) * 2;
    o->oPosZ += (f32)((o->oTimer / 2 & 1) - 0.5) * 2;
    if (o->oTimer > 67) {
        o->oPosX += (f32)((o->oTimer / 2 & 1) - 0.5) * 4;
        o->oPosZ += (f32)((o->oTimer / 2 & 1) - 0.5) * 4;
        o->oAction = 6;
    }
}

void opened_cannon_act_6(void) {
    if (o->oTimer == 0)
        cur_obj_play_sound_2(SOUND_OBJ_CANNON2);
    if (o->oTimer < 4) {
        o->oPosX += (f32)((o->oTimer / 2 & 1) - 0.5) * 4.0f;
        o->oPosZ += (f32)((o->oTimer / 2 & 1) - 0.5) * 4.0f;
    } else {
        if (o->oTimer < 6) {
        } else {
            if (o->oTimer < 22) {
                o->oMoveAngleYaw =
                    sins(o->oCannonUnkF4) * 0x4000 + ((s16)(o->oBehParams2ndByte << 8));
                o->oCannonUnkF4 += 0x400;
            } else if (o->oTimer < 26) {
            } else {
                o->oCannonUnkF4 = 0;
                o->oAction = 5;
            }
        }
    }
}

void opened_cannon_act_5(void) {
    if (o->oTimer == 0)
        cur_obj_play_sound_2(SOUND_OBJ_CANNON3);
    if (o->oTimer < 4) {
    } else {
        if (o->oTimer < 20) {
            o->oCannonUnkF4 += 0x400;
            o->oMoveAnglePitch = sins(o->oCannonUnkF4) * 0x2000;
        } else if (o->oTimer < 25) {
        } else
            o->oAction = 1;
    }
}

void opened_cannon_act_1(void) {
    UNUSED s32 unused;
#if defined(TARGET_PSP) && PSP_NETPLAY && PSP_NETPLAY_WORLD
    if (o->oCannonRemote) {
        /* (sm64coopdx opened_cannon_act_1 with another player inside) the cannon stays shown, aiming where that player aims: its yaw is the
         * player's, and its pitch is the barrel's for that aim. The barrel's pitch counts from straight up (0) and the player's aim from level
         * (act_in_cannon: 0 to 0x38E3 up): both are 0x2000 when the player starts aiming (opened_cannon_act_5). sm64coopdx uses
         * 14563 - aim / 2, which matches at 0x2000 only; this is the exact one */
        f32 remoteY;
        s16 pitch, yaw;
        if (cannon_remote_inside(&remoteY, &pitch, &yaw)) {
            o->oMoveAnglePitch = 0x4000 - pitch;
            o->oMoveAngleYaw = yaw;
        } else {
            o->oCannonRemote = 0; /* (it was shot out: back into the hole, as after this player's own shot) */
            o->oAction = 2;
        }
        o->oCannonUnk10C = 0;
        return;
    }
#endif
    cur_obj_become_intangible();
    cur_obj_disable_rendering();
    o->oCannonUnk10C = 0;
    gMarioShotFromCannon = 1;
}

void opened_cannon_act_2(void) {
    o->oAction = 3;
}

void opened_cannon_act_3(void) {
    UNUSED s32 unused;
    if (o->oTimer > 3)
        o->oAction = 0;
}

void (*sOpenedCannonActions[])(void) = { opened_cannon_act_0, opened_cannon_act_1, opened_cannon_act_2,
                                         opened_cannon_act_3, opened_cannon_act_4, opened_cannon_act_5,
                                         opened_cannon_act_6 };

u8 unused0EA1FC[] = { 2,  0,   0, 0, 0,  0,   0, 0, 63, 128, 0, 0, 2,  0,   0, 0, 65, 32,  0, 0,
                      63, 128, 0, 0, 2,  0,   0, 0, 65, 160, 0, 0, 63, 128, 0, 0, 2,  0,   0, 0,
                      65, 160, 0, 0, 63, 128, 0, 0, 8,  0,   0, 0, 65, 32,  0, 0, 63, 128, 0, 0 };

void bhv_cannon_base_loop(void) {
    cur_obj_call_action_function(sOpenedCannonActions);
    if (o->oCannonUnkF8)
        o->oCannonUnkF8++;
    o->oInteractStatus = 0;
#if defined(TARGET_PSP) && PSP_NETPLAY && PSP_NETPLAY_WORLD
    /* sm64coopdx syncs the cannon: when the other player gets into this one, it plays the same as for this player (it rises out of its hole, swings
     * from side to side, tilts up), then aims where that player aims (opened_cannon_act_1). Taken meanwhile: nobody else gets in. */
    if (o->oAction == 0 && !o->oCannonRemote && cannon_remote_inside(NULL, NULL, NULL)) {
        o->oCannonRemote = 1;
        o->oPosX = o->oHomeX;
        o->oPosY = o->oHomeY;
        o->oPosZ = o->oHomeZ;
        o->oMoveAnglePitch = 0;
        o->oMoveAngleYaw = (s16)(o->oBehParams2ndByte << 8);
        o->oCannonUnkF4 = 0;
        o->oAction = 4;
    }
    if (o->oCannonRemote) {
        if (o->oAction != 1 && o->oAction != 4 && o->oAction != 5 && o->oAction != 6) {
            o->oCannonRemote = 0;
        } else if (o->oAction != 1 && !cannon_remote_inside(NULL, NULL, NULL)) {
            o->oCannonRemote = 0; /* (it left before aiming, e.g. the other console went away) */
            o->oAction = 2;
        } else {
            cur_obj_enable_rendering();
            cur_obj_become_intangible();
            cur_obj_push_mario_away_from_cylinder(220.0f, 300.0f);
        }
    }
#endif
}

void bhv_cannon_barrel_loop(void) {
    struct Object *parent = o->parentObj;
    if (parent->header.gfx.node.flags & GRAPH_RENDER_ACTIVE) {
        cur_obj_enable_rendering();
        obj_copy_pos(o, o->parentObj);
        o->oMoveAngleYaw = o->parentObj->oMoveAngleYaw;
        o->oFaceAnglePitch = o->parentObj->oMoveAnglePitch;
    } else
        cur_obj_disable_rendering();
}
