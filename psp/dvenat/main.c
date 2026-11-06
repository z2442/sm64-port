/*
 * sm64DveNat ("native TV mode module"): READ-ONLY kernel module that says whether the system's own TV output (the one turned on with the
 * Display button) is active. It reads the internal state of sceImpose_Driver, same as ste2425's Auto-Tv-Out plugin (https://github.com/ste2425/Auto-Tv-Out): a pointer
 * at offset 0xA1D0 of the module that must point to base + 0xA220, and in that structure (+0x324) the state: 0 off, 1 on, 2 changing.
 * Those offsets have only been verified for firmware 6.60 and 6.61; on any other (or if the pointer does not match) it returns -1 ("unknown"). It is kept apart from
 * dvemgr on purpose: if anything fails, only this detection is lost and the TV keeps working the same.
 */
#include <pspsdk.h>
#include <pspkernel.h>
#include <psploadcore.h>

PSP_MODULE_INFO("sm64DveNat_Module", 0x1006, 1, 0);

/* -1 = unknown, 0 = off, 1 = on, 2 = changing */
int pspDveNatState(void) {
    int k1 = pspSdkSetK1(0);
    int res = -1;
    const unsigned int ver = sceKernelDevkitVersion();

    if (ver == 0x06060010 || ver == 0x06060110) {
        SceModule *mod = sceKernelFindModuleByName("sceImpose_Driver");

        if (mod != NULL) {
            const unsigned int base = mod->text_addr;
            const unsigned int st = *(volatile unsigned int *) (base + 0xA1D0);

            if (st == base + 0xA220) {
                res = (int) *(volatile unsigned int *) (st + 0x324);
            }
        }
    }
    pspSdkSetK1(k1);
    return res;
}

int module_start(SceSize args, void *argp) {
    return 0;
}

int module_stop(SceSize args, void *argp) {
    return 0;
}
