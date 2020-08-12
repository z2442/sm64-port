
#include <stdio.h>

#include "audio_api.h"
#include <pspkernel.h>
#include <pspaudiolib.h>
#include <pspaudio.h>
int chan = -1;

static bool audio_psp_init(void) {
    sceAudioSRCChReserve(1024, 32000, 2);
    return true;
}

static int audio_psp_buffered(void) {
 
    return  1024 / 4 - sceAudioGetChannelRestLen(chan);
}

static int audio_psp_get_desired_buffered(void) {
    return 512;
}

static void audio_psp_play(const uint8_t *buf, size_t len) {  
    sceAudioSRCOutputBlocking(PSP_VOLUME_MAX,buf);
}

struct AudioAPI audio_psp = {
    audio_psp_init,
    audio_psp_buffered,
    audio_psp_get_desired_buffered,
    audio_psp_play
};


