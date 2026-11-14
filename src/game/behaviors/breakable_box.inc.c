// breakable_box.c.inc

void bhv_breakable_box_loop(void) {
    obj_set_hitbox(o, &sBreakableBoxHitbox);
    cur_obj_set_model(MODEL_BREAKABLE_BOX_SMALL);
    if (o->oTimer == 0)
        breakable_box_init();
#if defined(TARGET_PSP) && PSP_NETPLAY && PSP_NETPLAY_WORLD
    /* (WLAN multiplayer) only this console's own player breaks it here. cur_obj_was_attacked_or_ground_pounded() also counts the other player's
     * ground pound on it, but that player's console breaks it too and sends DEAD and its coins: breaking it here as well gave both consoles two
     * sets of coins. A punch or kick of the other player never hit it here, so that already came only from the other console. */
    s32 hit = ((o->oInteractStatus & INT_STATUS_INTERACTED) && (o->oInteractStatus & INT_STATUS_WAS_ATTACKED))
              || (gMarioObject->platform == o && gMarioStates[0].action == ACT_GROUND_POUND_LAND);
    o->oInteractStatus = 0;
    if (hit) {
#else
    if (cur_obj_was_attacked_or_ground_pounded() != 0) {
#endif
        obj_explode_and_spawn_coins(46.0f, 1);
        create_sound_spawner(SOUND_GENERAL_BREAK_BOX);
    }
}
