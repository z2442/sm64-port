#include <ultra64.h>
#include "sm64.h"
#include "geo_commands.h"

#include "make_const_nonconst.h"

#include "common1.h"
#include "group0.h"

#include "bubble/geo.inc.c"
#include "walk_smoke/geo.inc.c"
#include "burn_smoke/geo.inc.c"
#include "stomp_smoke/geo.inc.c"
#include "water_wave/geo.inc.c"
#include "sparkle/geo.inc.c"
#include "water_splash/geo.inc.c"
#include "sparkle_animation/geo.inc.c"
#include "mario/geo.inc.c"
#include "luigi/geo.inc.c"
#if defined(TARGET_PSP) && PSP_NETPLAY
#include "wario/geo.inc.c"
#include "waluigi/geo.inc.c"
#include "toad_player/geo.inc.c"
#include "luigi_cap/geo.inc.c"
#include "toad_cap/geo.inc.c"
#include "waluigi_cap/geo.inc.c"
#include "wario_cap/geo.inc.c"
#endif
