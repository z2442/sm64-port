#if defined(VERSION_SH) && !defined(TARGET_N64)
/* PSP port: the pieces of the N64's I/O that the Shindou sound engine (load_sh.c) uses to load its banks and sequences. On the N64 they are
 * DMA transfers from the cartridge; here the sound data is already in memory (gSoundDataRaw, gSoundDataADSR, gMusicData), so a "transfer" is a
 * copy, finished at once: the message the engine waits for is sent right away. */
#include <ultra64.h>
#include <string.h>
#include "macros.h"

static OSPiHandle sPspSoundRomHandle;

OSPiHandle *osCartRomInit(void) {
    return &sPspSoundRomHandle;
}

OSPiHandle *osDriveRomInit(void) {
    return &sPspSoundRomHandle;
}

s32 osEPiStartDma(UNUSED OSPiHandle *piHandle, OSIoMesg *mb, UNUSED s32 direction) {
    memcpy(mb->dramAddr, (const void *) mb->devAddr, mb->size);
    if (mb->hdr.retQueue != NULL) {
        osSendMesg(mb->hdr.retQueue, (OSMesg) mb, OS_MESG_NOBLOCK);
    }
    return 0;
}
#endif
