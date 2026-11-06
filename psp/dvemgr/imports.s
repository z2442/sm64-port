    .set noreorder

#include "pspstub.s"

    STUB_START "sceDve_driver",0x00010011,0x00040005
    STUB_FUNC 0x0B85524C,sceDve_driver_0B85524C
    STUB_FUNC 0x93828323,sceDve_driver_93828323
    STUB_FUNC 0xA265B504,sceDve_driver_A265B504
    STUB_FUNC 0xDEB2F80C,sceDve_driver_DEB2F80C
    STUB_END

    STUB_START "sceGe_driver",0x40090000,0x00010005
    STUB_FUNC 0xD8633888,sceGeEdramSetSizeDriver
    STUB_END

    /* Daedalus imports this user-library NID.  Calling it from this PRX's
     * kernel start thread handles standalone launches; the driver import
     * above remains available for PSPLINK/CFW variants. */
    STUB_START "sceGe_user",0x40010000,0x00010005
    STUB_FUNC 0x5BAA5439,sceGeEdramSetSizeUser
    STUB_END

    STUB_START "sceHprm_driver",0x00010000,0x00010005
    STUB_FUNC 0x7E69EDA4,sceHprmIsHeadphoneExist
    STUB_END

    STUB_START "sceImpose_driver",0x00010011,0x00010005
    STUB_FUNC 0x116DDED6,sceImposeSetVideoOutMode
    STUB_END

    STUB_START "SysMemForKernel",0x00010011,0x00010005
    STUB_FUNC 0x07C586A1,sceKernelGetModel
    STUB_END
