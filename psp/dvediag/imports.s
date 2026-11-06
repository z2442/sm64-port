    .set noreorder

#include "pspstub.s"

    STUB_START "sceHprm_driver",0x00010000,0x00040005
    STUB_FUNC 0x7E69EDA4,sceHprmIsHeadphoneExist
    STUB_FUNC 0x208DB1BD,sceHprmIsRemoteExist
    STUB_FUNC 0x219C58F1,sceHprmIsMicrophoneExist
    STUB_FUNC 0x7B038374,sceHprmGetInternalState
    STUB_END

    STUB_START "sceSyscon_driver",0x00010011,0x00010005
    STUB_FUNC 0xE0DDFE18,sceSysconGetHPConnect
    STUB_END

    STUB_START "sceImpose_driver",0x00010011,0x00010005
    STUB_FUNC 0x116CFF64,sceImposeCheckVideoOut
    STUB_END
