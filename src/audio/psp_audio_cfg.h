#ifndef PSP_AUDIO_CFG_H
#define PSP_AUDIO_CFG_H

/* Simultaneous synthesizer notes (the original N64 uses 16). When they run out, a new note steals an old one or is lost (effects that
 * do not sound with dense music). Only ACTIVE notes count. The ME synthesizes in parallel, but measured in the log (patch 36):
 * up to ~23 notes the CPU almost never waits for the ME (MEWMS < 4 ms per 30-tick window); from 24 on the wait multiplies
 * (10-19 ms per window). That is why the cap is 22 (margin below that knee). Maximum 32 (sSampleDmas has 4 entries per note).
 * 16 = original behavior. */
#ifndef PSP_AUDIO_NOTES
#if defined(TARGET_PSP) && !defined(VERSION_EU) && !defined(VERSION_JP)
#define PSP_AUDIO_NOTES 22
#else
#define PSP_AUDIO_NOTES 16
#endif
#endif

#if PSP_AUDIO_NOTES > 32
#error PSP_AUDIO_NOTES: maximo 32 (sSampleDmas[0x60])
#endif

/* Extra audio heap memory (bytes) for the extra notes and DMA/command buffers (per note: 3 DMA buffers of 1296 bytes + 1 of 1440
 * + 20 * 4 commands of 8 bytes * 2). With 16 notes the N64's original heap is just enough. */
/* Memory the sound engine keeps for Luigi's voice banks (WLAN multiplayer, sound/sequences.json: banks 0x26 and 0x27). Their instrument
 * data is loaded with the sound player into the PERSISTENT bank pool (measured 0x1A90 bytes) and the larger sound player sequence into the
 * persistent sequence pool; without this room the pools overflow and sounds go missing or play as other ones. Set both to 0 when the
 * two Luigi banks are removed from sequences.json. */
#ifndef PSP_AUDIO_LUIGI_BANK_MEM
#if defined(PSP_NETPLAY) && PSP_NETPLAY
#define PSP_AUDIO_LUIGI_BANK_MEM 0x2000
#else
#define PSP_AUDIO_LUIGI_BANK_MEM 0 /* without the multiplayer the Luigi banks are not built (see the Makefile): their room is not needed */
#endif
#endif
#ifndef PSP_AUDIO_LUIGI_SEQ_MEM
#define PSP_AUDIO_LUIGI_SEQ_MEM 0x800
#endif

#ifndef PSP_AUDIO_HEAP_EXTRA
#if PSP_AUDIO_NOTES > 16
#define PSP_AUDIO_HEAP_EXTRA (0x18000 + PSP_AUDIO_LUIGI_BANK_MEM + PSP_AUDIO_LUIGI_SEQ_MEM)
#else
#define PSP_AUDIO_HEAP_EXTRA (PSP_AUDIO_LUIGI_BANK_MEM + PSP_AUDIO_LUIGI_SEQ_MEM)
#endif
#endif

#endif
