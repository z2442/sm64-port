// tower_door.c.inc

void bhv_tower_door_loop(void) {
    if (o->oTimer == 0)
        o->oMoveAngleYaw -= 0x4000;
#if defined(TARGET_PSP) && PSP_NETPLAY && PSP_NETPLAY_WORLD
    /* sm64coopdx: the door the other player broke is broken here too */
    const s32 byOther = (o->oInteractStatus & INT_STATUS_INTERACTED) != 0;
    if (byOther || check_mario_attacking(0)) {
        if (!byOther) {
            psp_np_world_collected(gMarioState, o, PSP_NP_EV_ITEM);
        }
#else
    if (check_mario_attacking(0)) {
#endif
        obj_explode_and_spawn_coins(80.0f, 0);
        create_sound_spawner(SOUND_GENERAL_WALL_EXPLOSION);
    }
}
