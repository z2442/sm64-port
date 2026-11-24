#ifndef SH_AUDIO_COMPAT_H
#define SH_AUDIO_COMPAT_H

/* PSP port: this sound engine (the Shindou one) comes from the current n64decomp/sm64, which renamed some of the sound defines that the rest
 * of the port still uses with their older names (include/audio_defines.h, include/seq_ids.h). Its newer names, with the same values: */
#include "audio_defines.h"
#include "seq_ids.h"

#define SOUND_STATUS_WAITING SOUND_STATUS_STARTING

#define SOUND_BANK_ACTION     0
#define SOUND_BANK_MOVING     1
#define SOUND_BANK_VOICE      2
#define SOUND_BANK_GENERAL    3
#define SOUND_BANK_ENV        4
#define SOUND_BANK_OBJ        5
#define SOUND_BANK_AIR        6
#define SOUND_BANK_MENU       7
#define SOUND_BANK_GENERAL2   8
#define SOUND_BANK_OBJ2       9
#define SOUND_BANK_COUNT     10

#define SOUND_BANKS_ALL_BITS 0xffff
#define SOUND_BANKS_ALL ((1 << SOUND_BANK_COUNT) - 1)

#define SOUND_CONSTANT_FREQUENCY      SOUND_NO_FREQUENCY_LOSS
#define SOUND_LOWER_BACKGROUND_MUSIC  SOUND_LO_BITFLAG_UNK1
#define SOUND_DISCRETE                SOUND_LO_BITFLAG_UNK8

#ifndef SEQ_BASE_ID
#define SEQ_BASE_ID 0x7f
#endif

/* (sh_pc.c: the cartridge I/O that load_sh.c uses, done as copies) */
#include <ultra64.h>
OSPiHandle *osCartRomInit(void);
OSPiHandle *osDriveRomInit(void);
s32 osEPiStartDma(OSPiHandle *piHandle, OSIoMesg *mb, s32 direction);

#endif
