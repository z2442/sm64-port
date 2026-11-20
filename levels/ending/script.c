#include <ultra64.h>
#include "sm64.h"
#include "behavior_data.h"
#include "model_ids.h"
#include "seq_ids.h"
#include "segment_symbols.h"
#include "level_commands.h"

#include "game/area.h"
#include "game/level_update.h"

#include "levels/scripts.h"

#include "actors/common1.h"

#include "make_const_nonconst.h"
#include "levels/ending/header.h"
#include "levels/intro/header.h"

#ifdef TARGET_PSP
static const LevelScript level_ending_to_castle[] = {
    CLEAR_LEVEL(),
    SLEEP_BEFORE_EXIT(/*frames*/ 1),
    SET_REG(/*value*/ LEVEL_CASTLE_GROUNDS),
    EXIT_AND_EXECUTE(/*seg*/ 0x15, _scriptsSegmentRomStart, _scriptsSegmentRomEnd, level_main_scripts_entry),
};
#endif

const LevelScript level_ending_entry[] = {
    /*0*/ INIT_LEVEL(),
    /*1*/ LOAD_MIO0(/*seg*/ 0x07, _ending_segment_7SegmentRomStart, _ending_segment_7SegmentRomEnd),
    /*4*/ ALLOC_LEVEL_POOL(),

    /*5*/ AREA(/*index*/ 1, ending_geo_000050),
    /*7*/ END_AREA(),

    /*8*/ FREE_LEVEL_POOL(),
    /*9*/ SLEEP(/*frames*/ 60),
    /*10*/ BLACKOUT(/*active*/ FALSE),
    /*11*/ LOAD_AREA(/*area*/ 1),
    /*12*/ TRANSITION(/*transType*/ WARP_TRANSITION_FADE_FROM_COLOR, /*time*/ 75, /*color*/ 0x00, 0x00, 0x00),
    /*14*/ SLEEP(/*frames*/ 120),
    /*15*/ CALL(/*arg*/ 0, /*func*/ lvl_play_the_end_screen_sound),
#ifdef TARGET_PSP
    /* The N64 had its reset button; here, once the credits are over, START restarts the game (the title, as at power on). In a WLAN game it goes back
     * to the castle grounds instead, as sm64coopdx does at the end (lvl_skip_credits / lvl_exiting_credits): 2 from lvl_ending_wait_for_reset */
    CALL_LOOP(/*arg*/ 0, /*func*/ lvl_ending_wait_for_reset),
    JUMP_IF(/*op*/ OP_EQ, /*arg*/ 2, level_ending_to_castle),
    CLEAR_LEVEL(),
    SLEEP_BEFORE_EXIT(/*frames*/ 1),
    EXIT_AND_EXECUTE(/*seg*/ 0x14, _introSegmentRomStart, _introSegmentRomEnd, level_intro_entry_1),
#else
    // L1:
    /*17*/ SLEEP(/*frames*/ 1),
    /*18*/ JUMP(level_ending_entry + 17),
#endif
};
