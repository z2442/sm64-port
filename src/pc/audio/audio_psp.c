
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_api.h"
#include <pspkernel.h>
#include <pspaudio.h>

#define PSP_AUDIO_CHANNELS 2
#define PSP_AUDIO_FREQUENCY 32000
#define PSP_VOLUME_MAX 0x8000
#define PSP_AUDIO_SAMPLES_DESIRED (1600 + 528 + 544)

static int chan = -1;
static int samples = 0;
static int last_sent = 0;

uint8_t snd_buf[4352] __attribute__((aligned(64))); // 1088*4
static const char *error_msg = "ERROR OPENING AUDIO CHANNEL!\n";

static inline void psp_msleep(int ms) {
    sceKernelDelayThread(ms * 1000);
}

static bool audio_psp_init(void) {
    while (sceAudioOutput2GetRestSample() > 0)
        psp_msleep(100);
    if (sceAudioSRCChRelease())
        chan = sceAudioSRCChReserve(PSP_AUDIO_SAMPLES_DESIRED, PSP_AUDIO_FREQUENCY, PSP_AUDIO_CHANNELS);
    if (chan < 0) {
        sceIoWrite(2, error_msg, strlen(error_msg));
        return false;
    }
    samples = PSP_AUDIO_SAMPLES_DESIRED;
    last_sent = 0;
    return true;
}

static int audio_psp_buffered(void) {
    char msg[32];
    memset(msg, 0, 32);

    int ret = PSP_AUDIO_SAMPLES_DESIRED - sceAudioOutput2GetRestSample();
    ret = (ret > 0 ? ret : 0);
    sprintf(msg, "audio buffer[%d] %d\n", chan, ret);
    // sceIoWrite(1, msg, strlen(msg));
    return ret;
}

static int audio_psp_get_desired_buffered(void) {
    return 1100;
}

/* This needs to possibly be changed, 64sample alignment is uncertain */
static void audio_psp_play(const uint8_t *buf, size_t len) {
    char msg[32];
    memset(msg, 0, 32);

    int new_samples = len / (2 * PSP_AUDIO_CHANNELS);
    last_sent = new_samples;
    void *buf2 = (void *) &snd_buf[0]; /* const strip since pspsdk blows */
    sprintf(msg, "audio request[%d] %d = %d\n", chan, len, new_samples);
    // sceIoWrite(1, msg, strlen(msg));

    memcpy(buf2, buf, len);

    /* Sleep while still playing last sound, 7ms is half a frame at 60fps */
    // while (sceAudioOutput2GetRestSample() > 0) psp_msleep(7);

    /* Check if we need to reacquire channel or num samples */
    if (chan <= 0 || new_samples != samples) {
        while (sceAudioOutput2GetRestSample() > 0)
            psp_msleep(7);
        if (chan >= 0)
            sceAudioSRCChRelease();
        chan = sceAudioSRCChReserve(new_samples, PSP_AUDIO_FREQUENCY, PSP_AUDIO_CHANNELS);
    }
    sceKernelDcacheWritebackRange(buf2, len);
    sceAudioSRCOutputBlocking(PSP_VOLUME_MAX, buf2);
}

struct AudioAPI audio_psp = {
    audio_psp_init,
    audio_psp_buffered,
    audio_psp_get_desired_buffered,
    audio_psp_play
};
