#include <ultra64.h>
#include "sm64.h"
#include "surface_terrains.h"
#include "geo_commands.h"

#include "make_const_nonconst.h"

// Note: This bin does not use bin IDs, unlike the other segmented bins.
#include "mario/model.inc.c"

#include "luigi/model.inc.c"
#if defined(TARGET_PSP) && PSP_NETPLAY /* (the other sm64coopdx characters: only in the builds with the WLAN multiplayer) */
#include "wario/model.inc.c"
#include "waluigi/model.inc.c"
#include "toad_player/model.inc.c"
/* their caps (after the player models: they use their textures) */
#include "luigi_cap/model.inc.c"
#include "toad_cap/model.inc.c"
#include "waluigi_cap/model.inc.c"
#include "wario_cap/model.inc.c"
#endif

#include "bubble/model.inc.c"

#include "walk_smoke/model.inc.c"

#include "burn_smoke/model.inc.c"

#include "stomp_smoke/model.inc.c"

#include "water_wave/model.inc.c"

#include "sparkle/model.inc.c"

#include "water_splash/model.inc.c"

#include "white_particle_small/model.inc.c"

#include "sparkle_animation/model.inc.c"
