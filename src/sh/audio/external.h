#ifndef AUDIO_EXTERNAL_H
#define AUDIO_EXTERNAL_H

#include <PR/ultratypes.h>

#include "types.h"

// Sequence arguments, passed to seq_player_play_sequence. seqId may be bit-OR'ed with
// SEQ_VARIATION; this will load the same sequence, but set a variation
// bit which may be read by the sequence script.
#define SEQUENCE_ARGS(priority, seqId) ((priority << 8) | seqId)

#define SOUND_MODE_STEREO           0
#define SOUND_MODE_MONO             3
#define SOUND_MODE_HEADSET          1

#define SEQ_PLAYER_LEVEL            0  // Level background music
#define SEQ_PLAYER_ENV              1  // Misc music like the puzzle jingle
#define SEQ_PLAYER_SFX              2  // Sound effects

extern s32 gAudioErrorFlags;
extern f32 gGlobalSoundSource[3];

// defined in data.c, used by the game
extern u32 gAudioRandom;

extern u8 gAudioSPTaskYieldBuffer[]; // ucode yield data ptr; only used in JP

struct SPTask *create_next_audio_frame_task(void);
void play_sound(s32 soundBits, f32 *pos);
void audio_signal_game_loop_tick(void);
void seq_player_fade_out(u8 player, u16 fadeDuration);
void fade_volume_scale(u8 player, u8 targetScale, u16 fadeDuration);
void seq_player_lower_volume(u8 player, u16 fadeDuration, u8 percentage);
void seq_player_unlower_volume(u8 player, u16 fadeDuration);
void set_audio_muted(u8 muted);
void sound_init(void);
void get_currently_playing_sound(u8 bank, u8 *numPlayingSounds, u8 *numSoundsInBank, u8 *soundId);
void stop_sound(u32 soundBits, f32 *pos);
void stop_sounds_from_source(f32 *pos);
void stop_sounds_in_continuous_banks(void);
void sound_banks_disable(u8 player, u16 bankMask);
void sound_banks_enable(u8 player, u16 bankMask);
void set_sound_moving_speed(u8 bank, u8 speed);
void play_dialog_sound(u8 dialogID);
void play_music(u8 player, u16 seqArgs, u16 fadeTimer);
void stop_background_music(u16 seqId);
void fadeout_background_music(u16 arg0, u16 fadeOut);
void drop_queued_background_music(void);
u16 get_current_background_music(void);
void play_secondary_music(u8 seqId, u8 bgMusicVolume, u8 volume, u16 fadeTimer);
void func_80321080(u16 fadeTimer);
void func_803210D4(u16 fadeOutTime);
void play_course_clear(void);
void play_peachs_jingle(void);
void play_puzzle_jingle(void);
void play_star_fanfare(void);
void play_power_star_jingle(u8 arg0);
void play_race_fanfare(void);
void play_toads_jingle(void);
void sound_reset(u8 presetId);
void audio_set_sound_mode(u8 arg0);

void audio_init(void); // in load.c

/* PSP port: the rest of the game uses the older names of the port's other versions (src/audio/external.h), from before n64decomp/sm64 named
 * these functions; this engine (the Shindou one, from the current decomp) keeps the decomp's names and answers to both. */
#define gDefaultSoundArgs gGlobalSoundSource
#define sequence_player_fade_out seq_player_fade_out
#define func_8031FFB4 seq_player_lower_volume
#define sequence_player_unlower seq_player_unlower_volume
#define set_sound_disabled set_audio_muted
#define func_803205E8 stop_sound
#define func_803206F8 stop_sounds_from_source
#define func_80320890 stop_sounds_in_continuous_banks
#define func_80320A4C set_sound_moving_speed

/* PSP WLAN multiplayer: the characters' voices and the other player's voice (see external.c). The characters, in sm64coopdx's order (CT_*) */
enum { PSP_CHAR_MARIO, PSP_CHAR_LUIGI, PSP_CHAR_TOAD /* Blue Toad */, PSP_CHAR_TOAD_YELLOW, PSP_CHAR_WALUIGI, PSP_CHAR_WARIO, PSP_CHAR_COUNT };
extern u8 gPspLocalChar;
extern u8 gPspSoloChar; /* the character outside a multiplayer room: Mario, or Luigi when a file was chosen holding L + R (file_select.c) */
extern u8 gPspVoiceNoRemap;
extern u32 gPspVoiceLast;
extern u8 gPspVoiceSeq;
extern u32 gPspSfxRing[4];
extern u8 gPspSfxSeq;
void psp_play_remote_sfx(u32 soundBits, f32 *pos);
void psp_play_remote_voice(u32 soundBits, int character, f32 *pos);

#if defined(VERSION_EU) || defined(VERSION_SH)
struct SPTask *unused_80321460();
struct SPTask *unused_80321460(void);
#endif

#endif // AUDIO_EXTERNAL_H
