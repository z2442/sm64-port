#ifndef MARIO_ACTIONS_AUTOMATIC_H
#define MARIO_ACTIONS_AUTOMATIC_H

#include <PR/ultratypes.h>

#include "types.h"

s32 mario_execute_automatic_action(struct MarioState *m);
s32 mario_can_bubble(struct MarioState *m);   /* WLAN multiplayer (sm64coopdx): instead of dying, this player can float in a bubble to the other one */
void mario_set_bubbled(struct MarioState *m);

#endif // MARIO_ACTIONS_AUTOMATIC_H
