/*
 * sm64DveDiag ("cable module"): READ-ONLY kernel module that reads the console signals related to the video cable.
 * On a PSP-3000 with a component cable: sceImposeCheckVideoOut gives 0 and value 2 (without a cable it gives an error) and syscon's HPConnect goes from 0 to 1.
 * It is kept apart from dvemgr on purpose: if any of these functions does not exist in the firmware, this module does not load and the TV output is not affected.
 */
#include <pspsdk.h>
#include <pspkernel.h>

PSP_MODULE_INFO("sm64DveDiag_Module", 0x1006, 1, 0);

int sceHprmIsHeadphoneExist(void);
int sceHprmIsRemoteExist(void);
int sceHprmIsMicrophoneExist(void);
int sceHprmGetInternalState(void);
int sceSysconGetHPConnect(void);
int sceImposeCheckVideoOut(int *value);

/* out[0..6]: headphones, remote, microphone, internal Hprm state, syscon HPConnect, return and value of sceImposeCheckVideoOut */
int pspDveDiagRead(int *out) {
    int k1 = pspSdkSetK1(0);
    int v = -1;
    int i;

    for (i = 0; i < 7; i++) {
        out[i] = 0;
    }
    out[0] = sceHprmIsHeadphoneExist();
    out[1] = sceHprmIsRemoteExist();
    out[2] = sceHprmIsMicrophoneExist();
    out[3] = sceHprmGetInternalState();
    out[4] = sceSysconGetHPConnect();
    out[5] = sceImposeCheckVideoOut(&v);
    out[6] = v;
    pspSdkSetK1(k1);
    return 0;
}

int module_start(SceSize args, void *argp) {
    return 0;
}

int module_stop(SceSize args, void *argp) {
    return 0;
}
