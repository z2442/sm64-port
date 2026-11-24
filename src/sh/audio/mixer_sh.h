#ifndef MIXER_SH_H
#define MIXER_SH_H

/* PSP port: the Shindou audio microcode on the CPU (mixer_sh.c). Its functions get their own names, as src/pc/mixer.c (the older microcode of
 * the other versions) is also in the game. */
#include <stdbool.h>
#include <stdint.h>
#include <ultra64.h>

#define aClearBufferImpl aClearBufferShImpl
#define aLoadBufferImpl aLoadBufferShImpl
#define aSaveBufferImpl aSaveBufferShImpl
#define aLoadADPCMImpl aLoadADPCMShImpl
#define aSetBufferImpl aSetBufferShImpl
#define aInterleaveImpl aInterleaveShImpl
#define aDMEMMoveImpl aDMEMMoveShImpl
#define aSetLoopImpl aSetLoopShImpl
#define aADPCMdecImpl aADPCMdecShImpl
#define aResampleImpl aResampleShImpl
#define aEnvSetup1Impl aEnvSetup1ShImpl
#define aEnvSetup2Impl aEnvSetup2ShImpl
#define aMixImpl aMixShImpl
#define aS8DecImpl aS8DecShImpl
#define aAddMixerImpl aAddMixerShImpl
#define aResampleZohImpl aResampleZohShImpl
#define aInterlImpl aInterlShImpl
#define aFilterImpl aFilterShImpl
#define aUnkCmd19Impl aUnkCmd19ShImpl

void aClearBufferImpl(uint16_t addr, int nbytes);
void aLoadBufferImpl(const void *source_addr, uint16_t dest_addr, uint16_t nbytes);
void aSaveBufferImpl(uint16_t source_addr, int16_t *dest_addr, uint16_t nbytes);
void aLoadADPCMImpl(int num_entries_times_16, const int16_t *book_source_addr);
void aSetBufferImpl(uint8_t flags, uint16_t in, uint16_t out, uint16_t nbytes);
void aInterleaveImpl(uint16_t dest, uint16_t left, uint16_t right, uint16_t c);
void aDMEMMoveImpl(uint16_t in_addr, uint16_t out_addr, int nbytes);
void aSetLoopImpl(ADPCM_STATE *adpcm_loop_state);
void aADPCMdecImpl(uint8_t flags, ADPCM_STATE state);
void aResampleImpl(uint8_t flags, uint16_t pitch, RESAMPLE_STATE state);
void aEnvSetup1Impl(uint8_t initial_vol_wet, uint16_t rate_wet, uint16_t rate_left, uint16_t rate_right);
void aEnvSetup2Impl(uint16_t initial_vol_left, uint16_t initial_vol_right);
void aMixImpl(uint16_t count, int16_t gain, uint16_t in_addr, uint16_t out_addr);
void aS8DecImpl(uint8_t flags, ADPCM_STATE state);
void aAddMixerImpl(uint16_t count, uint16_t in_addr, uint16_t out_addr);
void aResampleZohImpl(uint16_t pitch, uint16_t start_fract);
void aInterlImpl(uint16_t in_addr, uint16_t out_addr, uint16_t n_samples);
void aFilterImpl(uint8_t flags, uint16_t count_or_buf, int16_t *state_or_filter);
void aUnkCmd19Impl(uint8_t f, uint16_t count, uint16_t out_addr, uint16_t in_addr);
void aEnvMixerShImpl(uint16_t in_addr, uint16_t n_samples, bool swap_reverb, bool neg_left, bool neg_right, uint16_t dry_left,
                     uint16_t dry_right, uint16_t wet_left, uint16_t wet_right);
void aDuplicateShImpl(uint16_t in_addr, uint16_t out_addr, uint16_t copies);
void aHiLoGainShImpl(uint8_t g, uint16_t count, uint16_t addr);

/* The commands, with the Shindou argument order (include/PR/abi.h of n64decomp/sm64); "count" arguments in bytes */
#undef aSegment
#undef aClearBuffer
#undef aSetBuffer
#undef aLoadBuffer
#undef aSaveBuffer
#undef aDMEMMove
#undef aMix
#undef aEnvMixer
#undef aResample
#undef aInterleave
#undef aSetLoop
#undef aLoadADPCM
#undef aADPCMdec
#undef aS8Dec
#undef aAddMixer
#undef aDuplicate
#undef aResampleZoh
#undef aEnvSetup1
#undef aEnvSetup2
#undef aFilter
#undef aHiLoGain
#undef aDownsampleHalf
#undef aUnknown25

#define aSegment(pkt, s, b) do { } while (0)
#define aClearBuffer(pkt, d, c) aClearBufferImpl(d, c)
#define aLoadBuffer(pkt, s, d, c) aLoadBufferImpl(s, d, c)
#define aSaveBuffer(pkt, s, d, c) aSaveBufferImpl(s, d, c)
#define aLoadADPCM(pkt, c, d) aLoadADPCMImpl(c, d)
#define aSetBuffer(pkt, f, i, o, c) aSetBufferImpl(f, i, o, c)
#define aInterleave(pkt, o, l, r, c) aInterleaveImpl(o, l, r, c)
#define aDMEMMove(pkt, i, o, c) aDMEMMoveImpl(i, o, c)
#define aSetLoop(pkt, a) aSetLoopImpl(a)
#define aADPCMdec(pkt, f, s) aADPCMdecImpl(f, s)
#define aS8Dec(pkt, f, s) aS8DecImpl(f, s)
#define aResample(pkt, f, p, s) aResampleImpl(f, p, s)
#define aResampleZoh(pkt, pitch, startFract) aResampleZohImpl(pitch, startFract)
#define aEnvSetup1(pkt, initialVolReverb, rampReverb, rampLeft, rampRight) aEnvSetup1Impl(initialVolReverb, rampReverb, rampLeft, rampRight)
#define aEnvSetup2(pkt, initialVolLeft, initialVolRight) aEnvSetup2Impl(initialVolLeft, initialVolRight)
#define aEnvMixer(pkt, inBuf, nSamples, swapReverb, negLeft, negRight, dryLeft, dryRight, wetLeft, wetRight) \
    aEnvMixerShImpl(inBuf, nSamples, swapReverb, negLeft, negRight, dryLeft, dryRight, wetLeft, wetRight)
#define aMix(pkt, g, i, o, c) aMixImpl((c) >> 4, g, i, o) /* (the Ocarina of Time version counts in 16-byte units) */
#define aAddMixer(pkt, s, d, c) aAddMixerImpl(c, s, d)
#define aDuplicate(pkt, s, d, c) aDuplicateShImpl(s, d, c)
#define aDownsampleHalf(pkt, nSamples, i, o) aInterlImpl(i, o, nSamples)
#define aFilter(pkt, f, countOrBuf, addr) aFilterImpl(f, countOrBuf, addr)
#define aHiLoGain(pkt, g, buflen, i) aHiLoGainShImpl(g, buflen, i)
#define aUnknown25(pkt, f, c, o, i) aUnkCmd19Impl(f, c, o, i)

#endif
