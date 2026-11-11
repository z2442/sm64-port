#include <PR/ultratypes.h>

#include "audio/external.h"
#include "engine/math_util.h"
#include "game/area.h"
#include "game/game_init.h"
#include "game/level_update.h"
#include "game/main.h"
#include "game/memory.h"
#include "game/print.h"
#include "game/save_file.h"
#include "game/sound_init.h"
#include "level_table.h"
#include "seq_ids.h"
#include "sm64.h"

#define PRESS_START_DEMO_TIMER 800

#define STUB_LEVEL(textname, _1, _2, _3, _4, _5, _6, _7, _8) textname,
#define DEFINE_LEVEL(textname, _1, _2, _3, _4, _5, _6, _7, _8, _9, _10) textname,

static char gLevelSelect_StageNamesText[64][16] = {
    #include "levels/level_defines.h"
};
#undef STUB_LEVEL
#undef DEFINE_LEVEL

static u16 gDemoCountdown = 0;
#ifndef VERSION_JP
static s16 D_U_801A7C34 = 1;
static s16 gameOverNotPlayed = 1;
#endif

// run the demo timer on the PRESS START screen.
// this function will return a non-0 timer once
// the demo starts, signaling to the subsystem that
// the demo needs to be ran.

// don't shift this function from being the first function in the segment.
// the level scripts assume this function is the first, so it cant be moved.
int run_press_start_demo_timer(s32 timer) {
    gCurrDemoInput = NULL;

    if (timer == 0) {
        if (!gPlayer1Controller->buttonDown && !gPlayer1Controller->stickMag) {
            if ((++gDemoCountdown) == PRESS_START_DEMO_TIMER) {
                // start the demo. 800 frames has passed while
                // player is idle on PRESS START screen.

                // start the Mario demo animation for the demo list.
                load_patchable_table(&gDemo, gDemoInputListID);

                // if the next demo sequence ID is the count limit, reset it back to
                // the first sequence.
                if (++gDemoInputListID == gDemo.animDmaTable->count) {
                    gDemoInputListID = 0;
                }

                // add 1 (+4) to the pointer to skip the demoID.
                gCurrDemoInput = ((struct DemoInput *) gDemo.targetAnim) + 1;
                timer = (s8)((struct DemoInput *) gDemo.targetAnim)->timer;
                gCurrSaveFileNum = 1;
                gCurrActNum = 1;
            }
        } else { // activity was detected, so reset the demo countdown.
            gDemoCountdown = 0;
        }
    }
    return timer;
}

// input loop for the level select menu. updates the selected stage
// count if an input was received. signals the stage to be started
// or the level select to be exited if start or the quit combo is
// pressed.

s16 level_select_input_loop(void) {
    s32 stageChanged = FALSE;

    // perform the ID updates per each button press.
    if (gPlayer1Controller->buttonPressed & A_BUTTON) {
        ++gCurrLevelNum, stageChanged = TRUE;
    }
    if (gPlayer1Controller->buttonPressed & B_BUTTON) {
        --gCurrLevelNum, stageChanged = TRUE;
    }
    if (gPlayer1Controller->buttonPressed & U_JPAD) {
        --gCurrLevelNum, stageChanged = TRUE;
    }
    if (gPlayer1Controller->buttonPressed & D_JPAD) {
        ++gCurrLevelNum, stageChanged = TRUE;
    }
    if (gPlayer1Controller->buttonPressed & L_JPAD) {
        gCurrLevelNum -= 10, stageChanged = TRUE;
    }
    if (gPlayer1Controller->buttonPressed & R_JPAD) {
        gCurrLevelNum += 10, stageChanged = TRUE;
    }

    // if the stage was changed, play the sound for changing a stage.
    if (stageChanged) {
        play_sound(SOUND_GENERAL_LEVEL_SELECT_CHANGE, gDefaultSoundArgs);
    }

    // TODO: enum counts for the stage lists
    if (gCurrLevelNum > LEVEL_MAX) {
        gCurrLevelNum = LEVEL_MIN; // exceeded max. set to min.
    }

    if (gCurrLevelNum < LEVEL_MIN) {
        gCurrLevelNum = LEVEL_MAX; // exceeded min. set to max.
    }

    gCurrSaveFileNum = 4; // file 4 is used for level select tests
    gCurrActNum = 6;
    print_text_centered(160, 80, "SELECT STAGE");
    print_text_centered(160, 30, "PRESS START BUTTON");
    print_text_fmt_int(40, 60, "%2d", gCurrLevelNum);
    print_text(80, 60, gLevelSelect_StageNamesText[gCurrLevelNum - 1]); // print stage name

#define QUIT_LEVEL_SELECT_COMBO (Z_TRIG | START_BUTTON | L_CBUTTONS | R_CBUTTONS)

    // start being pressed signals the stage to be started. that is, unless...
    if (gPlayer1Controller->buttonPressed & START_BUTTON) {
        // ... the level select quit combo is being pressed, which uses START. If this
        // is the case, quit the menu instead.
        if (gPlayer1Controller->buttonDown == QUIT_LEVEL_SELECT_COMBO) {
            gDebugLevelSelect = 0;
            return -1;
        }
        play_sound(SOUND_MENU_STAR_SOUND, gDefaultSoundArgs);
        return gCurrLevelNum;
    }
    return 0;
}

#ifdef TARGET_PSP
extern void get_currently_playing_sound(u8 bankIndex, u8 *numPlayingSounds, u8 *arg2, u8 *soundId);
/* Title voice (Hello / Press Start to play). The original game requests the sound ONCE; if the request is lost (the audio
 * is processed on the ME / in another thread) Mario stays mute. Here it is watched: a few ticks later it checks whether the voice bank is playing and,
 * if not, the request is repeated (at most 2 times). Also, when returning to the title after the demo Mario repeats "Press Start to play"
 * (PSP_TITLE_VOICE_AFTER_DEMO 0 = original behavior). Log counters (VOX): [0] requests, [1] retries,
 * [2] confirmed playing, [3] last voice (1 = Hello, 2 = Press Start). */
#ifndef PSP_TITLE_VOICE_AFTER_DEMO
#define PSP_TITLE_VOICE_AFTER_DEMO 1
#endif
unsigned int gPspVox[4];
static u32 sVoxBits = 0;     /* requested voice being watched (0 = none) */
static s16 sVoxAge = 0;      /* ticks since the last request */
static s16 sVoxTries = 0;    /* reintentos hechos */
static u32 sVoxLastTick = 0; /* gGlobalTimer of the last call (to detect that the screen was entered again) */
static s16 sVoxEntered = 0;  /* the title has already been entered at least once */

static void psp_title_voice(u32 bits) {
    play_sound(bits, gDefaultSoundArgs);
    sVoxBits = bits;
    sVoxAge = 0;
    sVoxTries = 0;
    gPspVox[0]++;
    gPspVox[3] = (bits == SOUND_MARIO_HELLO) ? 1 : 2;
}

static void psp_title_voice_watch(void) {
    u8 n = 0, a = 0, id = 0xff;

    if (sVoxBits == 0) {
        return;
    }
    sVoxAge++;
    if (sVoxAge != 6 && sVoxAge != 12) {
        return;
    }
    get_currently_playing_sound(2, &n, &a, &id);
    if (n != 0) {
        gPspVox[2]++;
        sVoxBits = 0; /* plays: ready */
    } else if (sVoxTries < 2) {
        sVoxTries++;
        gPspVox[1]++;
        play_sound(sVoxBits, gDefaultSoundArgs);
    } else {
        sVoxBits = 0;
    }
}
#endif

int intro_default(void) {
    s32 sp1C = 0;

#ifndef VERSION_JP
#ifdef TARGET_PSP
    {
        /* gGlobalTimer jumps by more than 2 between calls = the title screen was just (re)entered */
        const int reentry = sVoxEntered && ((gGlobalTimer - sVoxLastTick) > 2);
        sVoxLastTick = gGlobalTimer;
        if (!sVoxEntered || reentry) {
            sVoxBits = 0;
        }
        sVoxEntered = 1;
        if (D_U_801A7C34 == 1) {
            psp_title_voice((gGlobalTimer < 0x81) ? SOUND_MARIO_HELLO : SOUND_MARIO_PRESS_START_TO_PLAY);
            D_U_801A7C34 = 0;
        } else if (PSP_TITLE_VOICE_AFTER_DEMO && reentry) {
            psp_title_voice(SOUND_MARIO_PRESS_START_TO_PLAY);
        }
        psp_title_voice_watch();
    }
#else
    if (D_U_801A7C34 == 1) {
        if (gGlobalTimer < 0x81) {
            play_sound(SOUND_MARIO_HELLO, gDefaultSoundArgs);
        } else {
            play_sound(SOUND_MARIO_PRESS_START_TO_PLAY, gDefaultSoundArgs);
        }
        D_U_801A7C34 = 0;
    }
#endif
#endif
    print_intro_text();

    if (gPlayer1Controller->buttonPressed & START_BUTTON) {
#ifdef VERSION_JP
        play_sound(SOUND_MENU_STAR_SOUND, gDefaultSoundArgs);
        sp1C = 100 + gDebugLevelSelect;
#else
        play_sound(SOUND_MENU_STAR_SOUND, gDefaultSoundArgs);
        sp1C = 100 + gDebugLevelSelect;
        D_U_801A7C34 = 1;
#endif
    }
    return run_press_start_demo_timer(sp1C);
}

int intro_game_over(void) {
    s32 sp1C = 0;

#ifndef VERSION_JP
    if (gameOverNotPlayed == 1) {
        play_sound(SOUND_MARIO_GAME_OVER, gDefaultSoundArgs);
        gameOverNotPlayed = 0;
    }
#endif

    print_intro_text();

    if (gPlayer1Controller->buttonPressed & START_BUTTON) {
        play_sound(SOUND_MENU_STAR_SOUND, gDefaultSoundArgs);
        sp1C = 100 + gDebugLevelSelect;
#ifndef VERSION_JP
        gameOverNotPlayed = 1;
#endif
    }
    return run_press_start_demo_timer(sp1C);
}

int intro_play_its_a_me_mario(void) {
    set_background_music(0, SEQ_SOUND_PLAYER, 0);
    play_sound(SOUND_MENU_COIN_ITS_A_ME_MARIO, gDefaultSoundArgs);
    return 1;
}

s32 lvl_intro_update(s16 arg1, UNUSED s32 arg2) {
    s32 retVar;

    switch (arg1) {
        case 0:
            retVar = intro_play_its_a_me_mario();
            break;
        case 1:
            retVar = intro_default();
            break;
        case 2:
            retVar = intro_game_over();
            break;
        case 3:
            retVar = level_select_input_loop();
            break;
    }
    return retVar;
}
