    .set noreorder

#include "pspstub.s"

    .section .lib.stub,"a",@progbits
    .global sm64PspCrashImport
sm64PspCrashImport:
    /* WEAK import of the "sm64Crash" library (psp/crashlog, kernel module, crash report): the game boots the same even if the module is not
     * loaded (it is only called once it is, see psp_watchdog.c). Same mechanism as the TV modules (psp_dve_user.s). */
    STUB_START "sm64Crash",0x40090000,0x00010005
    STUB_FUNC 0x5C3A9E11,pspCrashInstall
    STUB_END
