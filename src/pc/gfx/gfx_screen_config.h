#ifndef GFX_SCREEN_CONFIG_H
#define GFX_SCREEN_CONFIG_H

#define DESIRED_SCREEN_WIDTH 640
#define DESIRED_SCREEN_HEIGHT 480

/* PSP: 1 = the last frame of each 60 fps tick is presented (GE sync + vblank + buffer swap) in a thread, and the CPU already
 * runs the next tick's logic while the GE finishes. 0 = previous behavior (all synchronous). */
/* GPU profile (Triangle+L): 0 = not compiled (the hot path hooks disappear). Set to 1 only for measuring. */
#ifndef PSP_GPU_PROF
#define PSP_GPU_PROF 0
#endif
/* Switches to isolate a failure: 0 = disables that piece.
 * PSP_F2_SKIP: skip the 2nd frame of a 60 fps tick if the 1st missed a vblank (pc_main.c).
 * PSP_ME_FASTPOLL: sleep ~3/4 of the previous block's time before polling the ME (psp_me.c). */
#ifndef PSP_F2_SKIP
#define PSP_F2_SKIP 0 /* 0 = both frames of the tick are always drawn (fixed 60 fps; if they do not fit, the game runs slower). 1 = in heavy areas it drops to 30 fps at normal speed */
#endif
/* PSP_F2_REPLAY (option 3, stage 1): in a 60 fps tick the 2nd frame does NOT walk the game's list again nor generate triangles;
 * it calls (sceGuCallList) the scene that the 1st frame already left written in the GE list. Stage 1: it repeats it WITHOUT changing anything (the 2nd frame
 * comes out equal to the 1st; it is a test of the mechanism and of how much CPU is saved). If anything does not match (list restarted, textures erased...),
 * that frame is drawn as usual. 0 = fully disabled. */
#ifndef PSP_F2_REPLAY
#define PSP_F2_REPLAY 0 /* OFF by default: the 2nd frame emits the full scene again with the real state (stable). 1 = replay the 1st one's list */
#endif
/* PSP_F2_REPLAY_MTX (option 3, stage 2): the 2nd frame, besides replaying the scene, rewrites in the GE list the matrices of
 * each object with the ones of the real state (the 1st carries them interpolated), so it really moves at 60 fps. If there is any other patch
 * pending (HUD, dialogs, transitions, snow, bubbles, frames, fov change...) or an object moved too much, that frame is
 * drawn in full as usual. 0 = stage 1 behavior (replay without changing anything). */
#ifndef PSP_F2_REPLAY_MTX
#define PSP_F2_REPLAY_MTX 1
#endif
/* PSP_F2_SKY: the sky (own orthographic matrix inside its list) is also rewritten in the 2nd frame with the real state's:
 * frame 1 carries it interpolated, so without this the sky updated at 30 fps. It is only patched if both versions draw
 * the same tiles (if not, it stays as before). Needs PSP_F2_REPLAY_MTX. 0 = disabled. */
#ifndef PSP_F2_SKY
#define PSP_F2_SKY 1
#endif
/* Triangles in "decal" mode (ZMODE_DEC: shadows, stains, shadows embedded in the map floor...). The N64 compares them with the z-buffer
 * with a tolerance that depends on the SLOPE of the floor on screen (the pixel's dz). The PSP's GE only has a fixed offset
 * and the z-buffer is 16-bit.
 * PSP_DECAL_ADAPTIVE 1: the offset is computed per triangle with the EXACT N64 formula (see gfx_pc.c, gfx_decal_bias).
 * 0 = fixed value PSP_DECAL_OFFSET. */
#ifndef PSP_DECAL_ADAPTIVE
#define PSP_DECAL_ADAPTIVE 1
#endif
/* Margin added to the N64 tolerance for the GE's z interpolation error (units of the 16-bit z-buffer).
 * If a shadow gets cut or flickers (especially far away), raise it (16, 24...); if it covers what is flush with the floor, lower it. */
#ifndef PSP_DECAL_OFFSET
#define PSP_DECAL_OFFSET 1
#endif
/* Scale of the N64 tolerance (1.0 = the formula as is). The GE does not need as much as the N64 (its z error is smaller than the
 * N64's at 320x240) and a large tolerance makes the shadow cover what is flush with the floor (feet, object bases). 0.25 = a quarter. */
#ifndef PSP_DECAL_SCALE
#define PSP_DECAL_SCALE 0.25f
#endif
#ifndef PSP_DECAL_MAX
#define PSP_DECAL_MAX 1024      /* safety cap */
#endif
#ifndef PSP_DECAL_FALLBACK
#define PSP_DECAL_FALLBACK 128  /* triangles that touch the near plane: cannot be measured */
#endif
/* PSP_SAFE_WARP: during a level/area change or a warp transition the game is drawn in normal mode (1 frame per tick, without
 * interpolating or replaying lists). 0 = disabled. */
#ifndef PSP_SAFE_WARP
#define PSP_SAFE_WARP 0 /* with fixed 60 fps it does not drop to 30 in transitions; set to 1 if something fails when changing levels */
#endif
#ifndef PSP_ME_FASTPOLL
#define PSP_ME_FASTPOLL 1
#endif
#ifndef PSP_ASYNC_PRESENT
#define PSP_ASYNC_PRESENT 1
#endif

#endif
