
#include <stdio.h>

#include "audio_api.h"
#include <pspkernel.h>
#include <pspaudiolib.h>
#include <pspaudio.h>
int chan = -1;

static bool audio_psp_init(void) {
    pspAudioInit();
    chan = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, 512, PSP_AUDIO_FORMAT_STEREO);
    sceAudioChangeChannelConfig	(chan,PSP_AUDIO_FORMAT_STEREO );	
    return true;
}

static int audio_psp_buffered(void) {
 
    return  512 / 4 - sceAudioGetChannelRestLen(chan);
}

static int audio_psp_get_desired_buffered(void) {
    return 512 / 4;
}

static void audio_psp_play(const uint8_t *buf, size_t len) {  
    if(!sceAudioOutput( chan ,PSP_VOLUME_MAX , buf)){
    printf("audio output failed ");
    }
}

struct AudioAPI audio_psp = {
    audio_psp_init,
    audio_psp_buffered,
    audio_psp_get_desired_buffered,
    audio_psp_play
};


