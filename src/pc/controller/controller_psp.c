#if defined(TARGET_PSP)

#include <stdbool.h>
#include <pspctrl.h>
#include <ultra64.h>

#include "controller_api.h"

static void controller_psp_init(void) {
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    // consider using sceCtrlSetIdleCancelThreshold
}

// n64 controls https://strategywiki.org/wiki/Super_Mario_64/Controls
// PSP -> N64 control mapping
//  SQUARE   -> B
//  CROSS    -> A
//  TRIANGLE -> Z Trigger
//  CIRCLE   -> Z Trigger
//  L Trig   -> L Trigger
//  R Trig   -> R Trigger
//  Analog   -> Move
//  Start    -> Start
//  DPad     -> Camera buttons

static void controller_psp_read(OSContPad *pad) {
    static SceCtrlData data;

    if (!sceCtrlPeekBufferPositive(&data, 1))
        return;

    /* flip, scale and deadzone */
    #define DEADZONE (12) /* 12/80 = 15% */
    char stick_x = (char)((((float)data.Lx)*0.625f)-80);
    char stick_y = (char)(((((float)data.Ly)*0.625f)-80)*-1);
    pad->stick_x = stick_x * !(stick_x < DEADZONE && stick_x > -DEADZONE);
    pad->stick_y = stick_y * !(stick_y < DEADZONE && stick_y > -DEADZONE);

    if (data.Buttons & PSP_CTRL_START)
        pad->button |= START_BUTTON;
    if (data.Buttons & PSP_CTRL_SQUARE)
        pad->button |= B_BUTTON;
    if (data.Buttons & PSP_CTRL_CROSS)
        pad->button |= A_BUTTON;
    if (data.Buttons & PSP_CTRL_TRIANGLE)
        pad->button |= Z_TRIG;
    if (data.Buttons & PSP_CTRL_CIRCLE)
        pad->button |= Z_TRIG;
    if (data.Buttons & PSP_CTRL_LTRIGGER)
        pad->button |= L_TRIG;
    if (data.Buttons & PSP_CTRL_RTRIGGER)
        pad->button |= R_TRIG;
    if (data.Buttons & PSP_CTRL_UP)
        pad->button |= U_CBUTTONS;
    if (data.Buttons & PSP_CTRL_DOWN)
        pad->button |= D_CBUTTONS;
    if (data.Buttons & PSP_CTRL_LEFT)
        pad->button |= L_CBUTTONS;
    if (data.Buttons & PSP_CTRL_RIGHT)
        pad->button |= R_CBUTTONS;

}

struct ControllerAPI controller_psp = {
    controller_psp_init,
    controller_psp_read
};

#endif
