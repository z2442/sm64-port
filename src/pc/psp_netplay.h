#ifndef PSP_NETPLAY_H
#define PSP_NETPLAY_H

/* Ad-hoc WLAN multiplayer (phase 1: lobby, link test and the host's save file).
 *
 * The feature is OFF by default for now; compile with PSP_NETPLAY=1 (make TARGET_PSP=1 PSP_NETPLAY=1 ...) to build it. With PSP_NETPLAY=0 no network code is built or linked, the
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
uint32_t psp_np_game_remote_action(void);       /* its action (the sender's MarioState action) */
int psp_np_game_remote_vanished(void);          /* it wears the vanish cap */
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
enum { PSP_NP_EV_COIN = 1, PSP_NP_EV_STAR, PSP_NP_EV_CAP, PSP_NP_EV_1UP, PSP_NP_EV_DEAD, PSP_NP_EV_ATTACK, PSP_NP_EV_SPAWN, PSP_NP_EV_ACTION, PSP_NP_EV_STARSPAWN, PSP_NP_EV_DIGEST, PSP_NP_EV_RESYNC };
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
int psp_np_world_follow(struct Object *o);             /* the object is being driven by the other console right now: its behavior must not run */
#if PSP_NETPLAY_WORLD && PSP_NETPLAY_ENEMIES
short psp_np_attack_yaw(const struct Object *o, short def); /* the direction the other player was facing when it hit this object a moment ago, else def */
int psp_np_world_peer_here(void);                           /* linked, and the other player is in this very area (Bowser does not replay his intro for the one who comes in second) */
int psp_np_world_bowser_carried(const struct Object *o);   /* the other player carries this Bowser by the tail (this copy hangs from its model) */
void psp_np_world_bowser_carry_step(struct Object *o);       /* puts it there */
int psp_np_world_bowser_give_up(int frames);                /* a Bowser waiting for the other console's state (action 20) for that many frames: should he start the fight here? */
#else
static inline short psp_np_attack_yaw(const struct Object *o, short def) { (void) o; return def; }
static inline int psp_np_world_peer_here(void) { return 0; }
static inline int psp_np_world_bowser_carried(const struct Object *o) { (void) o; return 0; }
static inline void psp_np_world_bowser_carry_step(struct Object *o) { (void) o; }
static inline int psp_np_world_bowser_give_up(int frames) { (void) frames; return 1; }
#endif
int psp_np_world_dialog_ok(const struct Object *o); /* this console's Mario may start a dialog with o (the retargeted enemy that only the OTHER player is near must not freeze this one) */
int psp_np_world_remote_within(float x, float y, float z, float dist); /* the other player is in this area and within dist of the point (respawners wait for BOTH players to be far) */
void psp_np_world_retarget(struct Object *o);          /* an enemy this console simulates aims at the nearest player (it could be the other one) */
void psp_np_world_star(struct Object *parent, struct Object *star, int type); /* a boss spawned a star: the other console gets it too */
void psp_np_world_follow_step(struct Object *o);       /* what a followed object does instead of its behavior (reports what hit it) */
void psp_np_world_spawned(struct Object *parent, struct Object *obj, int model, const void *behavior); /* every spawn_object (host: announces loot of enemies) */
int psp_np_world_send(const PspNpWorldEvent *e);   /* queues an event for the other console (reliable, in order); 0 if the queue is full */
int psp_np_world_take(PspNpWorldEvent *e);         /* next event received, 0 if none */
void psp_np_world_collected(struct MarioState *m, struct Object *o, int type); /* the local player picked o up (game/psp_netplay_game.c) */
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
static inline unsigned int psp_np_game_remote_action(void) { return 0; }
static inline int psp_np_game_remote_vanished(void) { return 0; }
static inline int psp_np_game_remote_age(void) { return 0; }
struct MarioState;
static inline struct MarioState *psp_np_game_remote_state(void) { return 0; }
static inline void psp_np_game_remote_squish(int frames) { (void) frames; }
static inline short psp_np_attack_yaw(const struct Object *o, short def) { (void) o; return def; }
static inline int psp_np_world_dialog_ok(const struct Object *o) { (void) o; return 1; }
static inline int psp_np_world_peer_here(void) { return 0; }
static inline int psp_np_world_bowser_carried(const struct Object *o) { (void) o; return 0; }
static inline void psp_np_world_bowser_carry_step(struct Object *o) { (void) o; }
static inline int psp_np_world_bowser_give_up(int frames) { (void) frames; return 1; }
static inline int psp_np_world_remote_within(float x, float y, float z, float dist) { (void) x; (void) y; (void) z; (void) dist; return 0; }

#endif

#endif
