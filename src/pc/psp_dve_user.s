    .set noreorder

#include "pspstub.s"

    .section .lib.stub,"a",@progbits
    .global sm64PspDveImport
sm64PspDveImport:
    /* WEAK import of the "sm64DveMgr" library (psp/dvemgr, kernel module): the game boots the same even if the module is not loaded
     * (PSP-1000, PPSSPP, or if the module could not be written / loaded). Same mechanism as the kcall stub (psp_me_kcall.s). */
    STUB_START "sm64DveMgr",0x40090000,0x00040005
    STUB_FUNC 0x2ACFCB6D,pspDveMgrCheckVideoOut
    STUB_FUNC 0xF9C86C73,pspDveMgrSetVideoOut
    STUB_FUNC 0xC8CCE483,pspDveMgrSetEdramSize
    STUB_FUNC 0x6D0DE1A1,pspDveMgrGetModel
    STUB_END

    /* Temporary cable diagnostic: separate module (psp/dvediag), also a weak import. */
    STUB_START "sm64DveDiag",0x40090000,0x00010005
    STUB_FUNC 0x3A5C7E21,pspDveDiagRead
    STUB_END

    /* System native TV mode (read only): separate module (psp/dvenat), also a weak import. */
    STUB_START "sm64DveNat",0x40090000,0x00010005
    STUB_FUNC 0x6B1D4E52,pspDveNatState
    STUB_END
