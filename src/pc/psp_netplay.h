#ifndef PSP_NETPLAY_H
#define PSP_NETPLAY_H

/* Ad-hoc WLAN multiplayer (phase 1: lobby, link test and the host's save file).
 *
 * The feature is ON by default; compile with PSP_NETPLAY=0 (make TARGET_PSP=1 PSP_NETPLAY=0 ...) to leave it out. With PSP_NETPLAY=0 no network code is built or linked, the
 * "Multiplayer" page of the options menu disappears and every function below becomes an empty stub. Even when it is built in, nothing is loaded
 * or started until the player picks Host / Join in that page.
 *
 * Design (see psp_netplay.c): one host and one client talk over PDP (ad-hoc UDP). Every console runs its own game logic. The client plays with a
 * COPY OF THE HOST'S SAVE FILE kept only in memory: the client's own save file on the Memory Stick is never touched while it is in a room. */

#ifndef PSP_NETPLAY
#define PSP_NETPLAY 1
#endif

enum {
    PSP_NP_OFF,        /* nothing is running */
    PSP_NP_STARTING,   /* loading the network modules */
    PSP_NP_HOSTING,    /* room created, waiting for the other console */
    PSP_NP_SEARCHING,  /* looking for a room */
    PSP_NP_JOINING,    /* room found, joining / receiving the save file */
    PSP_NP_CONNECTED,  /* linked: the save file was exchanged and the link is alive */
    PSP_NP_ERROR       /* failed: psp_netplay_status() says why; Leave resets it */
};

#if defined(TARGET_PSP) && PSP_NETPLAY

#include <stdint.h>

/* One player's pose, sent ~30 times a second (see PK_STATE in psp_netplay.c). Filled / consumed by game/psp_netplay_game.c. */
typedef struct {
    int16_t level, area;
    float pos[3];
    int16_t angle[3];
    int16_t anim, frame, yTrans;
    int32_t accel;
    uint8_t visible;
    uint8_t frozen;     /* the sender is in its pause menu: its animation must not run on by itself */
    /* what the model needs besides the pose (the sender's MarioBodyState): upper body / head tilt, cap and power-up look, hands, eyes */
    uint32_t action;
    int16_t modelState;
    int16_t torso[3], head[3];
    uint8_t capState, eyeState, handState, wingFlutter, grabPos, punchState;
    /* the sender's last voice sound (SOUND_MARIO_*): voiceSeq changes for every new one */
    uint32_t voice;
    uint8_t voiceSeq;
    /* its last sounds of the action bank (steps, jumps, landings, swims, punches...), as sm64coopdx plays every player's: sfxSeq counts them, sfx[] keeps the
     * last four (sfx[seq & 3]) */
    uint8_t sfxSeq;
    uint32_t sfx[4];
    /* what the other console needs to judge a fight (see psp_np_player_attacks in game/interaction.c): the sender's MarioState */
    uint8_t invincTimer, hurtCounter;
    int8_t knockbackTimer;
    uint8_t bounceSquish;   /* the sender is being squashed by a jump / ground pound of the other player */
    uint16_t actionState;
    uint16_t flags;         /* low half of MarioState.flags: caps, punching, kicking, tripping */
    int16_t forwardVel;
    int16_t vel[3];
    uint8_t headCam;        /* the sender looks around in first person (C-Up): head[] is its camera's head rotation */
    uint8_t act;            /* the act (star) it is playing in its course */
    /* the moving platform it stands on (marioObj->platform, which sm64coopdx sends with every player): its kind and order in the platform table, 0xFF = none.
     * Finding it again from the floor under the model failed while the two copies of a tilting platform were not tilted the same way. */
    uint8_t platKind, platOrd;
    uint8_t scaleXZ, scaleY; /* the model's scale in percent (the squash of a hard landing, of an object or of a jump on it: 100 = normal) */
    uint32_t particles;      /* PARTICLE_* flags of the sender's last frame (dust, splashes, bubbles, sparkles, stars, fire...): shown around the model here too */
    int8_t lives;            /* its lives: a player that loses one brings the other's down to its count (sm64coopdx PACKET_DEATH) */
    uint8_t stompSeq;        /* +1 each time the sender lands on the other player (or ground-pounds it): that player's own console squashes its model */
    uint8_t stompFrames;     /* how long that squash lasts (bounceSquishTimer: 4 for a jump, 20 for a ground pound) */
    uint8_t pad2;
} PspNpPlayer;
/* Player vs player, like sm64coopdx: the two Marios are solid (they push each other and bounce off each other's head, a crouching one
 * makes the other twirl), and every attack of the game (punch, kick, dive, slide kick, ground pound, ...) hurts and knocks back the other
 * player ("PvP" is always on, as in the classic mode of sm64coopdx). Set this to 0 to turn all of it off; everything else of the multiplayer
 * keeps working. */
#ifndef PSP_NETPLAY_SOLID_PLAYERS
#define PSP_NETPLAY_SOLID_PLAYERS 1
#endif
struct Object;
struct MarioState;
struct MarioState *psp_np_game_remote_state(void); /* the other player as a MarioState (only what a fight needs is filled), NULL when it is not shown */
void psp_np_game_remote_squish(int frames);       /* squashes the other player's model for that many frames */
struct Object *psp_np_game_remote(void);        /* the other player's model, NULL when it is not shown */
int psp_np_game_remote_touches(const struct Object *obj); /* the other player's body (Mario's hitbox) overlaps obj's hitbox, as obj_check_if_collided_with_object */
uint32_t psp_np_game_remote_action(void);       /* its action (the sender's MarioState action) */
int psp_np_game_remote_pose(float pos[3], short angle[3], unsigned int *action); /* its latest position, angles and action, also while its model is
                                                                                   * not shown (in a cannon); 0 when it is not in this level / area / act */
int psp_np_game_remote_vanished(void);          /* it wears the vanish cap */
struct Object *psp_np_game_remote_held(void);   /* what it carries in its hands (drawn in them, as the local Mario's), NULL for nothing */
int psp_np_game_remote_age(void);               /* frames since it appeared (nobody pushes anybody while a level is loading) */
struct MarioBodyState;
struct MarioBodyState *psp_np_game_body_state(int index); /* body state the geo callbacks must use for the model being drawn */
int psp_np_game_drawing_remote(void);                       /* the model being drawn is the other player's */
int psp_np_game_remote_head_cam(void);                      /* the other player looks around in first person: its headAngle is that head rotation */
int psp_np_game_capture(PspNpPlayer *out);
void psp_np_game_apply(const PspNpPlayer *in, int remoteIsLuigi);
void psp_np_game_remove_remote(void);
void psp_np_game_local_luigi(int on);
void psp_np_game_keep_local_luigi(int want);

/* Names, notices and name tags, like sm64coopdx's (drawn by game/psp_options.c, which has the menu font and the translations):
 * "<name> connected / disconnected / entered <course> / entered this level / left this level", and the other player's name over its head.
 * The name is the console's nickname (System Settings). Set PSP_NETPLAY_NAMETAGS to 0 to keep only the notices. */
#define PSP_NP_NAME_BYTES 24
#ifndef PSP_NETPLAY_NAMETAGS
#define PSP_NETPLAY_NAMETAGS 1
#endif
void psp_np_hud_peer_name(const char *utf8, int peerIsLuigi); /* "" = unknown yet */
void psp_np_hud_link(int up);                                 /* the other player connected / disconnected */
void psp_np_hud_peer_place(int known, int level, int area, int act); /* where the other player is (every frame while linked) */
int psp_np_hud_peer_act(int course, int *peerIsLuigi);        /* the act the other player is in if it is in that course, else 0 */
void psp_np_hud_draw(void);                                   /* once per drawn frame, at the end of the display list */
void psp_np_hud_patch(void);                                  /* between the two frames of a 60 fps tick (interpolation) */

/* World events (stage 1 of the world sync, like PACKET_COLLECT_COIN / COLLECT_STAR / COLLECT_ITEM of sm64coopdx): what one player picks up
 * (coins, stars, caps, 1-ups) disappears for the other one too. The object is found by its behavior and position, as sm64coopdx does. Set
 * PSP_NETPLAY_WORLD to 0 to turn it off; the rest of the multiplayer keeps working. */
#ifndef PSP_NETPLAY_WORLD
#define PSP_NETPLAY_WORLD 1
#endif
typedef struct {
    uint8_t type, level, area, coinValue;
    int32_t bhv;      /* behavior of the object: its address relative to bhvYellowCoin (the same in both consoles) */
    int16_t pos[3];
    int16_t value;    /* coins: the sender's coin count */
    int16_t aux[3];   /* star spawn: where the star ends up */
} PspNpWorldEvent;
enum { PSP_NP_EV_COIN = 1, PSP_NP_EV_STAR, PSP_NP_EV_CAP, PSP_NP_EV_1UP, PSP_NP_EV_DEAD, PSP_NP_EV_ATTACK, PSP_NP_EV_SPAWN, PSP_NP_EV_ACTION, PSP_NP_EV_STARSPAWN, PSP_NP_EV_DIGEST, PSP_NP_EV_RESYNC,
       PSP_NP_EV_ITEM, /* like PACKET_COLLECT_ITEM of sm64coopdx: the object the player used (a secret's trigger, a tower door, a pillar detector) is used here too */
       PSP_NP_EV_LEVEL /* like PACKET_LEVEL_MACRO / PACKET_LEVEL_SPAWN_INFO of sm64coopdx: an object of the area's lists that is gone (or a coin formation / goomba
                        * triplet with some of its own gone), sent to a player that comes into the area */ };
/* Stage 3: what a player sets off (doors opening, switches pressed, the water level of Wet-Dry World) is shown to the other one too. Set
 * PSP_NETPLAY_TRIGGERS to 0 to turn it off. */
#ifndef PSP_NETPLAY_TRIGGERS
#define PSP_NETPLAY_TRIGGERS 1
#endif
/* Stage 2: enemies. The host simulates the enemies of the area both players are in and streams their state; the client shows them (its copy
 * does not run its own behavior while the stream lasts) and sends what its player does to them. Set PSP_NETPLAY_ENEMIES to 0 to turn it off. */
#ifndef PSP_NETPLAY_ENEMIES
#define PSP_NETPLAY_ENEMIES 1
#endif
#define PSP_NP_OBJ_BYTES 480 /* most a state packet of objects can carry */
#define PSP_NP_FLD_BYTES 432 /* most a packet of platform states can carry (4 records) */
int psp_np_world_fld_send(const void *data, int len);  /* the newest batch of platform states (unreliable) */
int psp_np_world_fld_take(void *data);                 /* the newest batch received (length), 0 if none */
int psp_np_congestion(void);                           /* 0 = fine .. 3 = the link is very slow (from the ping time) */
int psp_np_world_obj_send(const void *data, int len);  /* the host's latest batch of enemy states (unreliable, the newest replaces the one not sent yet) */
int psp_np_world_obj_take(void *data);                 /* the newest batch received (length), 0 if none */
int psp_np_world_is_host(void);         /* this console hosts the room */
int psp_np_world_remote_floor_type(void); /* surface type of the floor under the other player, -1 when it is not here */
void psp_np_world_claim(struct Object *o); /* the local player kicked / punched this platform-like object: this console drives it for a while */
void psp_np_world_chomp_freed(struct Object *chomp); /* the local player set the chain chomp free (its post): the other console sets its own free too */
void psp_np_world_drive(struct Object *o); /* while called every frame, this console drives o (the race of the racing penguin, the talk of Yoshi): the other console follows it */
#define PSP_NP_SIG_UKIKI_HAT 1  /* the cap ukiki took (value 1) or gave back (0) the cap of this console's player */
#define PSP_NP_SIG_LAKITU 2     /* the intro lakitu: this console's player started it (1), its text is over (2) */
#define PSP_NP_SIG_MANTA_RING 3 /* this console's player passed a ring of the manta ray: value = its count of rings, extra = the ring's index */
#define PSP_NP_SIG_NLE_STAR 4   /* a star that does not end the level came out for this console's player (Toad, MIPS): value = its number */
#define PSP_NP_SIG_BOWSER_PLAT 5 /* a platform of the last Bowser arena started to fall here: value = its index (second parameter byte) */
int psp_np_world_bowser_fx(const struct Object *bowser); /* Bowser's flames and shock waves: made by the console of the player nearest to him only */
void psp_np_world_bowser_fx_spawned(struct Object *obj, int which); /* (one was made here: the other console makes the same one) */
void psp_np_world_nle_star(const struct Object *star); /* (that star was just made: the other console shows it where it ends up) */
void psp_np_world_signal(const struct Object *o, int sig, int value); /* tells the other console what one of these objects did here (found there by its home) */
void psp_np_world_signal_ex(const struct Object *o, int sig, int value, int extra); /* the same, with one more number */
#define PSP_NP_LAKITU_OTHER 0x4C414B49u /* (Object.unused2 of the intro lakitu: it came down for the other player, see camera_lakitu.inc.c) */
#define PSP_NP_BOX_REMOTE 0x52424F58u   /* (Object.unused2 of an exclamation box the other player broke: its coins come from that console, see exclamation_box.inc.c) */
int psp_np_world_spawner_mine(const struct Object *o); /* a spawner of rolling balls: only the console of the player nearest to it makes them (the other console gets them announced) */
int psp_np_world_follow(struct Object *o);             /* the object is being driven by the other console right now: its behavior must not run */
#if PSP_NETPLAY_WORLD && PSP_NETPLAY_ENEMIES
short psp_np_attack_yaw(const struct Object *o, short def); /* the direction the other player was facing when it hit this object a moment ago, else def */
int psp_np_world_peer_here(void);                           /* linked, and the other player is in this very area (Bowser does not replay his intro for the one who comes in second) */
int psp_np_world_bowser_carried(const struct Object *o);   /* the other player carries this Bowser by the tail (this copy hangs from its model) */
void psp_np_world_bowser_carry_step(struct Object *o);       /* puts it there */
int psp_np_world_bowser_give_up(int frames);                /* a Bowser waiting for the other console's state (action 20) for that many frames: should he start the fight here? */
void psp_np_world_bowser_reward(const struct Object *reward, int isStar); /* this console made the key / grand star of a beaten Bowser: tells the other one where, and that the fight is over */
void psp_np_world_smooth_step(void);                        /* after the objects' update: eases away the jumps the other console's corrections made (only the picture) */
#else
static inline void psp_np_world_smooth_step(void) { }
static inline void psp_np_world_bowser_reward(const struct Object *reward, int isStar) { (void) reward; (void) isStar; }
static inline short psp_np_attack_yaw(const struct Object *o, short def) { (void) o; return def; }
static inline int psp_np_world_peer_here(void) { return 0; }
static inline int psp_np_world_bowser_carried(const struct Object *o) { (void) o; return 0; }
static inline void psp_np_world_bowser_carry_step(struct Object *o) { (void) o; }
static inline int psp_np_world_bowser_give_up(int frames) { (void) frames; return 1; }
#endif
int psp_np_world_rescuer(float pos[3]); /* the other player can pop a bubbled player (it is here, alive and not in a bubble itself): where it is */
int psp_np_world_remote_on(const struct Object *o);  /* the other player stands on this object (a platform, a switch): sm64coopdx's cur_obj_is_any_player_on_platform */
struct Object *psp_np_world_nearest_player(const struct Object *o); /* the player (Mario's object or the other player's model) nearest to o: sm64coopdx's nearest_player_to_object */
int psp_np_world_dialog_ok(const struct Object *o); /* this console's Mario may start a dialog with o (the retargeted enemy that only the OTHER player is near must not freeze this one) */
int psp_np_world_remote_within(float x, float y, float z, float dist); /* the other player is in this area and within dist of the point (respawners wait for BOTH players to be far) */
void psp_np_world_retarget(struct Object *o);          /* an enemy this console simulates aims at the nearest player (it could be the other one) */
void psp_np_world_star(struct Object *parent, struct Object *star, int type); /* a boss spawned a star: the other console gets it too */
int psp_np_world_follow_step(struct Object *o);        /* a followed object, before its behavior: reports what hit it; 1 = it is only placed this frame */
void psp_np_world_follow_running(struct Object *o);
int psp_np_world_quiet(void); /* 1 while the world is not synced (the ending and the credits) */    /* the followed object whose behavior is running now (NULL: none) */
void psp_np_world_spawned(struct Object *parent, struct Object *obj, int model, const void *behavior); /* every spawn_object (host: announces loot of enemies) */
int psp_np_world_send(const PspNpWorldEvent *e);   /* queues an event for the other console (reliable, in order); 0 if the queue is full */
int psp_np_world_take(PspNpWorldEvent *e);         /* next event received, 0 if none */
void psp_np_world_collected(struct MarioState *m, struct Object *o, int type); /* the local player picked o up (game/psp_netplay_game.c) */
extern int gPspNpStarByOther; /* the last red coin / secret was taken by the other player: their star comes out with no cutscene here */
#define PSP_NP_STAR_NO_CUTSCENE 0x53544152u /* (Object.unused2 of a spawning star: no camera cutscene nor frozen time on this console) */
void psp_np_world_frame(int peerKnown, int peerLevel, int peerAreaKey, int isHost);       /* once per frame while linked (peerAreaKey: area in bits 0-3, act in bits 4-7) */
void psp_np_world_reset(void);                                                 /* the link went down */
void psp_netplay_host(void);
void psp_netplay_join(void);   /* searches for a room and joins the first one found */
void psp_netplay_leave(void);  /* leaves the room and goes back to the console's own save file */
int psp_netplay_state(void);
const char *psp_netplay_status(void); /* short text for the menu (English; it is a status line) */
int psp_netplay_rtt_ms(void);         /* last round trip time, -1 if unknown */

/* Called once per frame by the game thread BEFORE the game logic (the only place where the save file may be swapped). */
void psp_netplay_frame(void);
void psp_netplay_perf_note(unsigned tickUs, unsigned logicUs, unsigned netUs, unsigned endUs); /* one tick of the game thread (pc_main.c); logged every 20 s while connected */

/* The current save file was written (called by the save code): the other console gets it so both keep the same progress. */
void psp_netplay_progress_changed(const void *file, int size);

/* Save file redirection used by osEepromLongRead / osEepromLongWrite (ultra_reimplementation.c). */
int psp_netplay_save_override(void);
void psp_netplay_save_read(uint8_t *out512);
void psp_netplay_save_write(const uint8_t *in512);

/* Closes everything (exit callback). Safe to call at any time. */
void psp_netplay_shutdown(void);
/* Before the console sleeps (the WLAN does not survive it): leaves the room and waits for the network to be closed.
 * Returns a report for the power log: bits 0-15 = ms waited, bits 16-21 = what was still open when it gave up (0 = all closed),
 * bit 30 = a room / network was in use, bit 31 = it was not (nothing to close). */
unsigned int psp_netplay_suspend(void);

#else

static inline void psp_netplay_host(void) {}
static inline void psp_netplay_join(void) {}
static inline void psp_netplay_leave(void) {}
static inline int psp_netplay_state(void) { return PSP_NP_OFF; }
static inline const char *psp_netplay_status(void) { return ""; }
static inline int psp_netplay_rtt_ms(void) { return -1; }
static inline void psp_netplay_frame(void) {}
static inline void psp_netplay_perf_note(unsigned tickUs, unsigned logicUs, unsigned netUs, unsigned endUs) { (void) tickUs; (void) logicUs; (void) netUs; (void) endUs; }
static inline void psp_netplay_progress_changed(const void *file, int size) { (void) file; (void) size; }
static inline int psp_netplay_save_override(void) { return 0; }
static inline void psp_netplay_save_read(unsigned char *out512) { (void) out512; }
static inline void psp_netplay_save_write(const unsigned char *in512) { (void) in512; }
static inline void psp_netplay_shutdown(void) {}
static inline unsigned int psp_netplay_suspend(void) { return 0x80000000u; }
static inline void psp_np_game_local_luigi(int on) { (void) on; }
static inline void psp_np_hud_draw(void) {}
static inline int psp_np_hud_peer_act(int course, int *peerIsLuigi) { (void) course; (void) peerIsLuigi; return 0; }
static inline void psp_np_hud_patch(void) {}
struct Object;
static inline struct Object *psp_np_game_remote(void) { return 0; }
static inline int psp_np_game_remote_touches(const struct Object *obj) { (void) obj; return 0; }
static inline unsigned int psp_np_game_remote_action(void) { return 0; }
static inline int psp_np_game_remote_pose(float pos[3], short angle[3], unsigned int *action) { (void) pos; (void) angle; (void) action; return 0; }
static inline int psp_np_game_remote_vanished(void) { return 0; }
static inline struct Object *psp_np_game_remote_held(void) { return 0; }
static inline int psp_np_game_remote_age(void) { return 0; }
struct MarioState;
static inline struct MarioState *psp_np_game_remote_state(void) { return 0; }
static inline void psp_np_game_remote_squish(int frames) { (void) frames; }
static inline short psp_np_attack_yaw(const struct Object *o, short def) { (void) o; return def; }
static inline int psp_np_world_dialog_ok(const struct Object *o) { (void) o; return 1; }
static inline int psp_np_world_spawner_mine(const struct Object *o) { (void) o; return 1; }
static inline int psp_np_world_remote_floor_type(void) { return -1; }
static inline int psp_np_world_is_host(void) { return 1; }
static inline void psp_np_world_claim(struct Object *o) { (void) o; }
static inline void psp_np_world_chomp_freed(struct Object *chomp) { (void) chomp; }
static inline void psp_np_world_drive(struct Object *o) { (void) o; }
static inline void psp_np_world_signal(const struct Object *o, int sig, int value) { (void) o; (void) sig; (void) value; }
#define PSP_NP_SIG_UKIKI_HAT 1
#define PSP_NP_SIG_LAKITU 2
#define PSP_NP_SIG_MANTA_RING 3
#define PSP_NP_SIG_NLE_STAR 4
#define PSP_NP_SIG_BOWSER_PLAT 5
static inline int psp_np_world_bowser_fx(const struct Object *bowser) { (void) bowser; return 1; }
static inline void psp_np_world_bowser_fx_spawned(struct Object *obj, int which) { (void) obj; (void) which; }
static inline void psp_np_world_nle_star(const struct Object *star) { (void) star; }
static inline void psp_np_world_signal_ex(const struct Object *o, int sig, int value, int extra) { (void) o; (void) sig; (void) value; (void) extra; }
#define PSP_NP_LAKITU_OTHER 0x4C414B49u
#define PSP_NP_BOX_REMOTE 0x52424F58u
static inline int psp_np_world_remote_on(const struct Object *o) { (void) o; return 0; }
static inline int psp_np_world_rescuer(float pos[3]) { (void) pos; return 0; }
static inline struct Object *psp_np_world_nearest_player(const struct Object *o) { (void) o; return 0; }
static inline int psp_np_world_peer_here(void) { return 0; }
static inline int psp_np_world_bowser_carried(const struct Object *o) { (void) o; return 0; }
static inline void psp_np_world_bowser_carry_step(struct Object *o) { (void) o; }
static inline int psp_np_world_bowser_give_up(int frames) { (void) frames; return 1; }
static inline void psp_np_world_bowser_reward(const struct Object *reward, int isStar) { (void) reward; (void) isStar; }
static inline int psp_np_world_remote_within(float x, float y, float z, float dist) { (void) x; (void) y; (void) z; (void) dist; return 0; }

#endif

#endif
