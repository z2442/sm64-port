// hoot.c.inc

/* PSP WLAN multiplayer: whether this console's player is the one that talks to the Hoot (0 no, 1 talking, 2 done): the player that woke it up.
 * The other one just sees it awake (its state comes from the other console, see sPlatKind), like sm64coopdx's one shared Hoot. */
static s32 sLocalTalkToHoot;
static s32 sHootSanity; /* it became ready to fly on the other console: made tangible here once */
static s32 sHootMaxAvail; /* it only ever wakes up: once awake on either console it never goes back to sleep in its tree (sm64coopdx: one shared Hoot) */

void bhv_hoot_init(void) {
    cur_obj_init_animation(0);
    sLocalTalkToHoot = 0;
    sHootSanity = TRUE;
    sHootMaxAvail = HOOT_AVAIL_ASLEEP_IN_TREE;

    o->oHomeX = o->oPosX + 800.0f;
    o->oHomeY = o->oPosY - 150.0f;
    o->oHomeZ = o->oPosZ + 300.0f;
    o->header.gfx.node.flags |= GRAPH_RENDER_INVISIBLE;

    cur_obj_become_intangible();
}

// sp28 = arg0
// sp2c = arg1

f32 hoot_find_next_floor(struct FloorGeometry **arg0, f32 arg1) {
    f32 sp24 = arg1 * sins(o->oMoveAngleYaw) + o->oPosX;
    UNUSED f32 sp20 = o->oPosY;
    f32 sp1c = arg1 * coss(o->oMoveAngleYaw) + o->oPosZ;
    f32 floorY = find_floor_height_and_data(sp24, 10000.0f, sp1c, arg0);

    return floorY;
}

void hoot_floor_bounce(void) {
    struct FloorGeometry *sp1c;
    f32 floorY;

    floorY = hoot_find_next_floor(&sp1c, 375.0f);
    if (floorY + 75.0f > o->oPosY)
        o->oMoveAnglePitch -= 3640.8888;

    floorY = hoot_find_next_floor(&sp1c, 200.0f);
    if (floorY + 125.0f > o->oPosY)
        o->oMoveAnglePitch -= 7281.7776;

    floorY = hoot_find_next_floor(&sp1c, 0);
    if (floorY + 125.0f > o->oPosY)
        o->oPosY = floorY + 125.0f;
    if (o->oMoveAnglePitch < -21845.3328)
        o->oMoveAnglePitch = -21845;
}

// sp30 = fastOscY
// sp34 = speed

void hoot_free_step(s16 fastOscY, s32 speed) {
    struct FloorGeometry *sp2c;
    s16 yaw = o->oMoveAngleYaw;
    s16 pitch = o->oMoveAnglePitch;
    s16 sp26 = o->header.gfx.unk38.animFrame;
    f32 xPrev = o->oPosX;
    f32 zPrev = o->oPosZ;
    f32 hSpeed;

    o->oVelY = sins(pitch) * speed;
    hSpeed = coss(pitch) * speed;
    o->oVelX = sins(yaw) * hSpeed;
    o->oVelZ = coss(yaw) * hSpeed;

    o->oPosX += o->oVelX;
    if (fastOscY == 0)
        o->oPosY -= o->oVelY + coss((s32)(sp26 * 3276.8)) * 50.0f / 4;
    else
        o->oPosY -= o->oVelY + coss((s32)(sp26 * 6553.6)) * 50.0f / 4;
    o->oPosZ += o->oVelZ;

    find_floor_height_and_data(o->oPosX, o->oPosY, o->oPosZ, &sp2c);
    if (sp2c == NULL) {
        o->oPosX = xPrev;
        o->oPosZ = zPrev;
    }

    if (sp26 == 0)
        cur_obj_play_sound_2(SOUND_GENERAL_SWISH_WATER);
}

void hoot_player_set_yaw(void) {
    s16 stickX = gPlayer3Controller->rawStickX;
    s16 stickY = gPlayer3Controller->rawStickY;
    UNUSED s16 pitch = o->oMoveAnglePitch;
    if (stickX < 10 && stickX >= -9)
        stickX = 0;
    if (stickY < 10 && stickY >= -9)
        stickY = 0;

    o->oMoveAngleYaw -= 5 * stickX;
}

// sp28 = speed
// sp2c = xPrev
// sp30 = zPrev

void hoot_carry_step(s32 speed, UNUSED f32 xPrev, UNUSED f32 zPrev) {
    s16 yaw = o->oMoveAngleYaw;
    s16 pitch = o->oMoveAnglePitch;
    s16 sp22 = o->header.gfx.unk38.animFrame;
    f32 hSpeed;

    o->oVelY = sins(pitch) * speed;
    hSpeed = coss(pitch) * speed;
    o->oVelX = sins(yaw) * hSpeed;
    o->oVelZ = coss(yaw) * hSpeed;

    o->oPosX += o->oVelX;
    o->oPosY -= o->oVelY + coss((s32)(sp22 * 6553.6)) * 50.0f / 4;
    o->oPosZ += o->oVelZ;

    if (sp22 == 0)
        cur_obj_play_sound_2(SOUND_GENERAL_SWISH_WATER);
}

// sp48 = xPrev
// sp4c = yPrev
// sp50 = zPrev

void hoot_surface_collision(f32 xPrev, UNUSED f32 yPrev, f32 zPrev) {
    struct FloorGeometry *sp44;
    struct WallCollisionData hitbox;
    f32 floorY;

    hitbox.x = o->oPosX;
    hitbox.y = o->oPosY;
    hitbox.z = o->oPosZ;
    hitbox.offsetY = 10.0;
    hitbox.radius = 50.0;

    if (find_wall_collisions(&hitbox) != 0) {
        o->oPosX = hitbox.x;
        o->oPosY = hitbox.y;
        o->oPosZ = hitbox.z;
        gMarioObject->oInteractStatus |= INT_STATUS_MARIO_UNK7; /* bit 7 */
    }

    floorY = find_floor_height_and_data(o->oPosX, o->oPosY, o->oPosZ, &sp44);
    if (sp44 == NULL) {
        o->oPosX = xPrev;
        o->oPosZ = zPrev;
        return;
    }

    if (absf_2(o->oPosX) > 8000.0f)
        o->oPosX = xPrev;
    if (absf_2(o->oPosZ) > 8000.0f)
        o->oPosZ = zPrev;
    if (floorY + 125.0f > o->oPosY)
        o->oPosY = floorY + 125.0f;
}

// sp28 = xPrev
// sp2c = zPrev

void hoot_act_ascent(f32 xPrev, f32 zPrev) {
    f32 negX = 0 - o->oPosX;
    f32 negZ = 0 - o->oPosZ;
    s16 angleToOrigin = atan2s(negZ, negX);

    o->oMoveAngleYaw = approach_s16_symmetric(o->oMoveAngleYaw, angleToOrigin, 0x500);
    o->oMoveAnglePitch = 0xCE38;

    if (o->oTimer >= 29) {
        cur_obj_play_sound_1(SOUND_ENV_WIND2);
        o->header.gfx.unk38.animFrame = 1;
    }

    if (o->oPosY > 6500.0f)
        o->oAction = HOOT_ACT_CARRY;

    hoot_carry_step(60, xPrev, zPrev);
}

void hoot_action_loop(void) {
    f32 xPrev = o->oPosX;
    f32 yPrev = o->oPosY;
    f32 zPrev = o->oPosZ;

    switch (o->oAction) {
        case HOOT_ACT_ASCENT:
            hoot_act_ascent(xPrev, zPrev);
            break;

        case HOOT_ACT_CARRY:
            hoot_player_set_yaw();

            o->oMoveAnglePitch = 0x71C;

            if (o->oPosY < 2700.0f) {
                set_time_stop_flags_if_alone(TIME_STOP_ENABLED | TIME_STOP_MARIO_AND_DOORS); /* (sm64coopdx: no time stop here with two players) */

                if (cutscene_object_with_dialog(CUTSCENE_DIALOG, o, DIALOG_045)) {
                    clear_time_stop_flags(TIME_STOP_ENABLED | TIME_STOP_MARIO_AND_DOORS);

                    o->oAction = HOOT_ACT_TIRED;
                }
            }

            hoot_carry_step(20, xPrev, zPrev);
            break;

        case HOOT_ACT_TIRED:
            hoot_player_set_yaw();

            o->oMoveAnglePitch = 0;

            hoot_carry_step(20, xPrev, zPrev);

            if (o->oTimer >= 61)
                gMarioObject->oInteractStatus |= INT_STATUS_MARIO_UNK7; /* bit 7 */
            break;
    }

    hoot_surface_collision(xPrev, yPrev, zPrev);
}

void hoot_turn_to_home(void) {
    f32 homeDistX = o->oHomeX - o->oPosX;
    f32 homeDistY = o->oHomeY - o->oPosY;
    f32 homeDistZ = o->oHomeZ - o->oPosZ;
    s16 hAngleToHome = atan2s(homeDistZ, homeDistX);
    s16 vAngleToHome = atan2s(sqrtf(homeDistX * homeDistX + homeDistZ * homeDistZ), -homeDistY);

    o->oMoveAngleYaw = approach_s16_symmetric(o->oMoveAngleYaw, hAngleToHome, 0x140);
    o->oMoveAnglePitch = approach_s16_symmetric(o->oMoveAnglePitch, vAngleToHome, 0x140);
}

/* PSP WLAN multiplayer (sm64coopdx syncs the Hoot): while the other player rides it, this console's Hoot is where that player hangs (ACT_RIDING_HOOT puts
 * Mario 92.5 below the Hoot and faces it the Hoot's way), instead of flying on its own somewhere else. Returns TRUE while that is the case. */
static s32 hoot_follow_remote_rider(void) {
#if defined(TARGET_PSP) && PSP_NETPLAY && PSP_NETPLAY_WORLD
    struct Object *rider = psp_np_game_remote();
    if (rider != NULL && psp_np_game_remote_action() == ACT_RIDING_HOOT && o->oInteractStatus != INT_STATUS_HOOT_GRABBED_BY_MARIO
        && dist_between_objects(o, rider) < 3000.0f) {
        o->oPosX = rider->oPosX;
        o->oPosY = rider->oPosY + 92.5f;
        o->oPosZ = rider->oPosZ;
        o->oMoveAngleYaw = o->oFaceAngleYaw = rider->header.gfx.angle[1];
        o->oForwardVel = 0.0f;
        o->oVelY = 0.0f;
        cur_obj_init_animation(1);
        o->header.gfx.node.flags &= ~GRAPH_RENDER_INVISIBLE;
        return TRUE;
    }
#endif
    return FALSE;
}

void hoot_awake_loop(void) {
    if (hoot_follow_remote_rider()) {
        return;
    }
    if (o->oInteractStatus == INT_STATUS_HOOT_GRABBED_BY_MARIO) {
        hoot_action_loop();
        cur_obj_init_animation(1);
    } else {
        cur_obj_init_animation(0);

        hoot_turn_to_home();
        hoot_floor_bounce();
        hoot_free_step(0, 10);

        o->oAction = 0;
        o->oTimer = 0;
    }

    set_object_visibility(o, 2000);
}

void bhv_hoot_loop(void) {
    /* never backwards: a console that still had it asleep (its state comes from the console of the nearest player) does not put it back in the tree */
    if (o->oHootAvailability < sHootMaxAvail) {
        o->oHootAvailability = sHootMaxAvail;
    } else {
        sHootMaxAvail = o->oHootAvailability;
    }

    switch (o->oHootAvailability) {
        case HOOT_AVAIL_ASLEEP_IN_TREE:
            if (hoot_follow_remote_rider()) { /* the other player is riding it, so it is awake and done talking: it stays that way here too */
                o->oHootAvailability = sHootMaxAvail = HOOT_AVAIL_READY_TO_FLY;
                break;
            }
            if (is_point_within_radius_of_mario(o->oPosX, o->oPosY, o->oPosZ, 50)) {
                o->header.gfx.node.flags &= ~GRAPH_RENDER_INVISIBLE;
                o->oHootAvailability = HOOT_AVAIL_WANTS_TO_TALK;
                sLocalTalkToHoot = 1; /* this player woke it up: this player is the one it talks to */
            }
            break;

        case HOOT_AVAIL_WANTS_TO_TALK:
            hoot_awake_loop();

            /* (only the player that woke it talks to it: the other one gets it awake from the other console and just waits for it to be ready to fly) */
            if (sLocalTalkToHoot == 1 && set_mario_npc_dialog(2) == 2 && cutscene_object_with_dialog(CUTSCENE_DIALOG, o, DIALOG_044)) {
                sLocalTalkToHoot = 2;
                set_mario_npc_dialog(0);

                cur_obj_become_tangible();
                sHootSanity = FALSE;

                o->oHootAvailability = HOOT_AVAIL_READY_TO_FLY;
            }
            break;

        case HOOT_AVAIL_READY_TO_FLY:
            if (sLocalTalkToHoot == 1) { /* the other player's console said it is ready while this player was still talking: the dialog is finished */
                if (set_mario_npc_dialog(2) == 2 && cutscene_object_with_dialog(CUTSCENE_DIALOG, o, DIALOG_044)) {
                    sLocalTalkToHoot = 2;
                    set_mario_npc_dialog(0);
                }
            }
            if (sHootSanity) { /* (it got ready on the other console) */
                cur_obj_become_tangible();
                sHootSanity = FALSE;
            }
            hoot_awake_loop();
            break;
    }
}
