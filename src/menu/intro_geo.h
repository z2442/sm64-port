#ifndef INTRO_GEO_H
#define INTRO_GEO_H

#include <PR/ultratypes.h>
#include <PR/gbi.h>

#include "types.h"

Gfx *geo_title_screen(s32 sp50, struct GraphNode *sp54, UNUSED void *context);
Gfx *geo_fade_transition(s32 sp40, struct GraphNode *sp44, UNUSED void *context);
Gfx *intro_backdrop_one_image(s32 index, s8 *backgroundTable);
Gfx *geo_intro_backdrop(s32 sp48, struct GraphNode *sp4c, UNUSED void *context);
Gfx *geo_game_over_tile(s32 sp40, struct GraphNode *sp44, UNUSED void *context);
#ifdef VERSION_SH
Gfx *geo_intro_face_easter_egg(s32 state, struct GraphNode *node, UNUSED void *context);
#endif

#endif // INTRO_GEO_H
