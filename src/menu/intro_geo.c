#include <PR/ultratypes.h>

#include "engine/math_util.h"
#include "game/memory.h"
#include "game/segment2.h"
#include "game/segment7.h"
#include "intro_geo.h"
#include "sm64.h"
#include "textures.h"
#include "types.h"
#include "engine/math_util.h"
#include "gfx_dimensions.h"
#include "prevent_bss_reordering.h"

// frame counts for the zoom in, hold, and zoom out of title model
#define INTRO_STEPS_ZOOM_IN 20
#define INTRO_STEPS_HOLD_1 75
#define INTRO_STEPS_ZOOM_OUT 91

// background types
#define INTRO_BACKGROUND_SUPER_MARIO 0
#define INTRO_BACKGROUND_GAME_OVER 1

struct GraphNodeMore {
    /*0x00*/ struct GraphNode node;
    /*0x14*/ void *todo;
    /*0x18*/ u32 unk18;
};

extern void interpolate_vectors(Vec3f res, Vec3f a, Vec3f b);

// intro geo bss
s32 gGameOverFrameCounter;
s32 gGameOverTableIndex;
s16 gTitleZoomCounter;
s32 gTitleFadeCounter;

// intro screen background display lists for each of four 80x20 textures
const Gfx *introBackgroundDlRows[] = { title_screen_bg_dl_0A000130, title_screen_bg_dl_0A000148,
                                       title_screen_bg_dl_0A000160, title_screen_bg_dl_0A000178 };

// intro screen background texture X offsets
float introBackgroundOffsetX[] = {
    0.0, 80.0, 160.0, 240.0, 0.0, 80.0, 160.0, 240.0, 0.0, 80.0, 160.0, 240.0,
};

// intro screen background texture Y offsets
float introBackgroundOffsetY[] = {
    160.0, 160.0, 160.0, 160.0, 80.0, 80.0, 80.0, 80.0, 0.0, 0.0, 0.0, 0.0,
};

// table that points to either the "Super Mario 64" or "Game Over" tables
const u8 *const *introBackgroundTextureType[] = { mario_title_texture_table, game_over_texture_table };

s8 introBackgroundIndexTable[] = {
    INTRO_BACKGROUND_SUPER_MARIO, INTRO_BACKGROUND_SUPER_MARIO, INTRO_BACKGROUND_SUPER_MARIO,
    INTRO_BACKGROUND_SUPER_MARIO, INTRO_BACKGROUND_SUPER_MARIO, INTRO_BACKGROUND_SUPER_MARIO,
    INTRO_BACKGROUND_SUPER_MARIO, INTRO_BACKGROUND_SUPER_MARIO, INTRO_BACKGROUND_SUPER_MARIO,
    INTRO_BACKGROUND_SUPER_MARIO, INTRO_BACKGROUND_SUPER_MARIO, INTRO_BACKGROUND_SUPER_MARIO,
};

// only one table of indexes listed
s8 *introBackgroundTables[] = { introBackgroundIndexTable };

s8 gameOverBackgroundTable[] = {
    INTRO_BACKGROUND_GAME_OVER, INTRO_BACKGROUND_GAME_OVER, INTRO_BACKGROUND_GAME_OVER,
    INTRO_BACKGROUND_GAME_OVER, INTRO_BACKGROUND_GAME_OVER, INTRO_BACKGROUND_GAME_OVER,
    INTRO_BACKGROUND_GAME_OVER, INTRO_BACKGROUND_GAME_OVER, INTRO_BACKGROUND_GAME_OVER,
    INTRO_BACKGROUND_GAME_OVER, INTRO_BACKGROUND_GAME_OVER, INTRO_BACKGROUND_GAME_OVER,
};

// order of tiles that are flipped from "Game Over" to "Super Mario 64"
s8 gameOverBackgroundFlipOrder[] = { 0x00, 0x01, 0x02, 0x03, 0x07, 0x0B,
                                     0x0a, 0x09, 0x08, 0x04, 0x05, 0x06 };

/* 60 fps: scale of the title logo (the first frame uses the interpolated scale, the second the real one) */
static Gfx *sIntroScalePos;
static Vec3f sIntroScale;

#ifdef TARGET_PSP
int psp_title_patch_pending(void) { return sIntroScalePos != NULL; }
#endif

void patch_title_screen_scales(void) {
    if (sIntroScalePos != NULL) {
        Mtx *scaleMat = alloc_display_list(sizeof(*scaleMat));
        guScale(scaleMat, sIntroScale[0], sIntroScale[1], sIntroScale[2]);
        gSPMatrix(sIntroScalePos, scaleMat, G_MTX_MODELVIEW | G_MTX_MUL | G_MTX_PUSH);
        sIntroScalePos = NULL;
    }
}

Gfx *geo_title_screen(s32 sp50, struct GraphNode *sp54, UNUSED void *context) {
    struct GraphNode *graphNode; // sp4c
    Gfx *displayList;            // sp48
    Gfx *displayListIter;        // sp44
    Mtx *scaleMat;               // sp40
    f32 *scaleTable1;            // sp3c
    f32 *scaleTable2;            // sp38
    f32 scaleX;                  // sp34
    f32 scaleY;                  // sp30
    f32 scaleZ;                  // sp2c
    Vec3f scale;
    Vec3f scaleInterpolated;
    graphNode = sp54;
    displayList = NULL;
    displayListIter = NULL;
    scaleTable1 = segmented_to_virtual(intro_seg7_table_0700C790);
    scaleTable2 = segmented_to_virtual(intro_seg7_table_0700C880);
    if (sp50 != 1) {
        gTitleZoomCounter = 0;
    } else if (sp50 == 1) {
        graphNode->flags = (graphNode->flags & 0xFF) | 0x100;
        scaleMat = alloc_display_list(sizeof(*scaleMat));
        displayList = alloc_display_list(4 * sizeof(*displayList));
        displayListIter = displayList;
        if (gTitleZoomCounter >= 0 && gTitleZoomCounter < INTRO_STEPS_ZOOM_IN) {
            scaleX = scaleTable1[gTitleZoomCounter * 3];
            scaleY = scaleTable1[gTitleZoomCounter * 3 + 1];
            scaleZ = scaleTable1[gTitleZoomCounter * 3 + 2];
        } else if (gTitleZoomCounter >= INTRO_STEPS_ZOOM_IN && gTitleZoomCounter < INTRO_STEPS_HOLD_1) {
            scaleX = 1.0f;
            scaleY = 1.0f;
            scaleZ = 1.0f;
        } else if (gTitleZoomCounter >= INTRO_STEPS_HOLD_1
                   && gTitleZoomCounter < INTRO_STEPS_ZOOM_OUT) {
            scaleX = scaleTable2[(gTitleZoomCounter - INTRO_STEPS_HOLD_1) * 3];
            scaleY = scaleTable2[(gTitleZoomCounter - INTRO_STEPS_HOLD_1) * 3 + 1];
            scaleZ = scaleTable2[(gTitleZoomCounter - INTRO_STEPS_HOLD_1) * 3 + 2];
        } else {
            scaleX = 0.0f;
            scaleY = 0.0f;
            scaleZ = 0.0f;
        }
        vec3f_set(scale, scaleX, scaleY, scaleZ);
        interpolate_vectors(scaleInterpolated, sIntroScale, scale);
        vec3f_set(sIntroScale, scaleX, scaleY, scaleZ);
        guScale(scaleMat, scaleInterpolated[0], scaleInterpolated[1], scaleInterpolated[2]);
        sIntroScalePos = displayListIter;
        gSPMatrix(displayListIter++, scaleMat, G_MTX_MODELVIEW | G_MTX_MUL | G_MTX_PUSH);
        gSPDisplayList(displayListIter++, &intro_seg7_dl_0700B3A0);
        gSPPopMatrix(displayListIter++, G_MTX_MODELVIEW);
        gSPEndDisplayList(displayListIter);
        gTitleZoomCounter++;
    }
    return displayList;
}


/* PSP 16:9: the renderer shrinks the 2D projections (0..320 x 0..240) into a centered 4:3 box. Instead of
 * stretching the background to fill the 16:9 (the squares came out widened), extra columns of
 * 80x80 squares are drawn on each side of the box: the extra left column copies the state of its row's
 * column 0 and the right one that of column 3 (the neighbor). This way the order of the change from Game Over to Super Mario 64
 * (top left corner -> right -> bottom -> left -> middle row) looks the same as on the N64 and the
 * extra columns only accompany their neighbor. */
#ifdef WIDESCREEN
#define INTRO_BG_EXTRA_TILES 6
#else
#define INTRO_BG_EXTRA_TILES 0
#endif

static Gfx *intro_backdrop_tile(s32 index, s8 *backgroundTable, f32 offsetX, f32 offsetY) {
    Mtx *mtx;
    Gfx *displayList;
    Gfx *displayListIter;
    const u8 *const *vIntroBgTable;
    s32 i;
    mtx = alloc_display_list(sizeof(*mtx));
    displayList = alloc_display_list(36 * sizeof(*displayList));
    displayListIter = displayList;
    vIntroBgTable = segmented_to_virtual(introBackgroundTextureType[backgroundTable[index]]);
    guTranslate(mtx, offsetX, offsetY, 0.0f);
    gSPMatrix(displayListIter++, mtx, G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_PUSH);
    gSPDisplayList(displayListIter++, &title_screen_bg_dl_0A000118);
    for (i = 0; i < 4; ++i) {
        gDPLoadTextureBlock(displayListIter++, vIntroBgTable[i], G_IM_FMT_RGBA, G_IM_SIZ_16b, 80, 20, 0,
                            G_TX_CLAMP, G_TX_CLAMP, 7, 6, G_TX_NOLOD, G_TX_NOLOD)
        gSPDisplayList(displayListIter++, introBackgroundDlRows[i]);
    }
    gSPPopMatrix(displayListIter++, G_MTX_MODELVIEW);
    gSPEndDisplayList(displayListIter);
    return displayList;
}

/* Adds the extra background columns (PSP 16:9 only). */
static Gfx *intro_backdrop_extra(Gfx *dl, s8 *backgroundTable) {
#ifdef WIDESCREEN
    s32 row;
    for (row = 0; row < 3; row++) {
        const f32 y = introBackgroundOffsetY[row * 4];
        gSPDisplayList(dl++, intro_backdrop_tile(row * 4 + 0, backgroundTable, -80.0f, y));
        gSPDisplayList(dl++, intro_backdrop_tile(row * 4 + 3, backgroundTable, 320.0f, y));
    }
#else
    (void) backgroundTable;
#endif
    return dl;
}

Gfx *geo_fade_transition(s32 sp40, struct GraphNode *sp44, UNUSED void *context) {
    struct GraphNode *graphNode = sp44; // sp3c
    Gfx *displayList = NULL;            // sp38
    Gfx *displayListIter = NULL;        // sp34
    if (sp40 != 1) {
        gTitleFadeCounter = 0; // D_801B985C
    } else if (sp40 == 1) {
        displayList = alloc_display_list(5 * sizeof(*displayList));
        displayListIter = displayList;
        gSPDisplayList(displayListIter++, dl_proj_mtx_fullscreen);
        gDPSetEnvColor(displayListIter++, 255, 255, 255, gTitleFadeCounter);
        if (gTitleFadeCounter == 255) {
            if (0) {
            }
            graphNode->flags = (graphNode->flags & 0xFF) | 0x100;
            gDPSetRenderMode(displayListIter++, G_RM_AA_OPA_SURF, G_RM_AA_OPA_SURF2);
        } else {
            graphNode->flags = (graphNode->flags & 0xFF) | 0x500;
            gDPSetRenderMode(displayListIter++, G_RM_AA_XLU_SURF, G_RM_AA_XLU_SURF2);
            if (0) {
            }
        }
        gSPDisplayList(displayListIter++, &intro_seg7_dl_0700C6A0);
        gSPEndDisplayList(displayListIter);
        if (gTitleZoomCounter >= 0x13) {
            gTitleFadeCounter += 0x1a;
            if (gTitleFadeCounter >= 0x100) {
                gTitleFadeCounter = 0xFF;
            }
        }
    }
    return displayList;
}

Gfx *intro_backdrop_one_image(s32 index, s8 *backgroundTable) {
    return intro_backdrop_tile(index, backgroundTable, introBackgroundOffsetX[index], introBackgroundOffsetY[index]);
}

Gfx *geo_intro_backdrop(s32 sp48, struct GraphNode *sp4c, UNUSED void *context) {
    struct GraphNodeMore *graphNode; // sp44
    s32 index;                       // sp40
    s8 *backgroundTable;             // sp3c
    Gfx *displayList;                // sp38
    Gfx *displayListIter;            // sp34
    s32 i;                           // sp30
    graphNode = (struct GraphNodeMore *) sp4c;
    index = graphNode->unk18 & 0xff; // TODO: word at offset 0x18 of struct GraphNode
    backgroundTable = introBackgroundTables[index];
    displayList = NULL;
    displayListIter = NULL;
    if (sp48 == 1) {
        displayList = alloc_display_list((17 + INTRO_BG_EXTRA_TILES) * sizeof(*displayList));
        displayListIter = displayList;
        graphNode->node.flags = (graphNode->node.flags & 0xFF) | 0x100;
        gSPDisplayList(displayListIter++, &dl_proj_mtx_fullscreen);
        gSPDisplayList(displayListIter++, &title_screen_bg_dl_0A000100);
        for (i = 0; i < 12; ++i) {
            gSPDisplayList(displayListIter++, intro_backdrop_one_image(i, backgroundTable));
        }
        displayListIter = intro_backdrop_extra(displayListIter, backgroundTable);
        gSPDisplayList(displayListIter++, &title_screen_bg_dl_0A000190);
        gSPEndDisplayList(displayListIter);
    }
    return displayList;
}

Gfx *geo_game_over_tile(s32 sp40, struct GraphNode *sp44, UNUSED void *context) {
    struct GraphNode *graphNode; // sp3c
    Gfx *displayList;            // sp38
    Gfx *displayListIter;        // sp34
    s32 j;                       // sp30
    s32 i;                       // sp2c
    graphNode = sp44;
    displayList = NULL;
    displayListIter = NULL;
    if (sp40 != 1) {
        gGameOverFrameCounter = 0;
        gGameOverTableIndex = -2;
        for (i = 0; i < (s32) sizeof(gameOverBackgroundTable); ++i) {
            gameOverBackgroundTable[i] = INTRO_BACKGROUND_GAME_OVER;
        }
    } else {
        displayList = alloc_display_list((17 + INTRO_BG_EXTRA_TILES) * sizeof(*displayList));
        displayListIter = displayList;
        if (gGameOverTableIndex == -2) {
            if (gGameOverFrameCounter == 180) {
                gGameOverTableIndex++;
                gGameOverFrameCounter = 0;
            }
        } else {
            // transition tile from "Game Over" to "Super Mario 64"
            if (gGameOverTableIndex != 11 && !(gGameOverFrameCounter & 0x1)) {
                gGameOverTableIndex++;
                gameOverBackgroundTable[gameOverBackgroundFlipOrder[gGameOverTableIndex]] =
                    INTRO_BACKGROUND_SUPER_MARIO;
            }
        }
        if (gGameOverTableIndex != 11) {
            gGameOverFrameCounter++;
        }
        graphNode->flags = (graphNode->flags & 0xFF) | 0x100;
        gSPDisplayList(displayListIter++, &dl_proj_mtx_fullscreen);
        gSPDisplayList(displayListIter++, &title_screen_bg_dl_0A000100);
        for (j = 0; j < (s32) sizeof(gameOverBackgroundTable); ++j) {
            gSPDisplayList(displayListIter++, intro_backdrop_one_image(j, gameOverBackgroundTable));
        }
        displayListIter = intro_backdrop_extra(displayListIter, gameOverBackgroundTable);
        gSPDisplayList(displayListIter++, &title_screen_bg_dl_0A000190);
        gSPEndDisplayList(displayListIter);
    }
    return displayList;
}

#ifdef VERSION_SH
/*
 * Shindou easter egg: pressing Z on the Mario head screen (title or game over) turns the background tiles, one by one in a spiral,
 * into copies of the middle of the screen (Mario's face), and Z again turns them back.
 * On the N64 the picture is read from the frame buffer; on PSP it is read from the last frame on screen (gfx_psp_sample_screen).
 */
#include "engine/graph_node.h"
#include "game/game_init.h"
#include "game/geo_misc.h"
#include "audio/external.h"
#ifdef TARGET_PSP
#include "pc/gfx/gfx_pc.h"
#endif

#define FACE_IMAGE_W 40
#define FACE_IMAGE_H 40
#define FACE_ROWS 6
/* The N64 tiles 8 x 6 squares of 40 pixels over its 320 x 240 screen. With the 16:9 picture the grid gets two more columns on each side
 * (x -80 to 400, as the backdrop's extra tiles, see intro_backdrop_extra), and the spiral is worked out for the wider grid. */
#ifdef WIDESCREEN
#define FACE_COLS 12
#define FACE_COL_X0 (-80)
#else
#define FACE_COLS 8
#define FACE_COL_X0 0
#endif
#define FACE_TILES (FACE_COLS * FACE_ROWS)

static s8 sFaceVisible[FACE_TILES];
static s8 sFaceToggleOrder[FACE_TILES];
static s8 sFaceOrderCount = 0;
static s8 sFaceCounter = 0;

/* one buffer at a fixed address: the texture cache entry is dropped every frame (gfx_texture_cache_forget) so it is imported again */
static u16 sFaceImage[FACE_IMAGE_W * FACE_IMAGE_H] __attribute__((aligned(16)));

/* The middle 4 x 2 squares (where Mario's head is) never turn, as on the N64 (screen x 80-239, y 80-159) */
static s32 intro_face_is_middle(s32 col, s32 row) {
    s32 x = FACE_COL_X0 + col * 40;
    return row >= 2 && row <= 3 && x >= 80 && x < 240;
}

/* The N64's order is a clockwise spiral from the top left square inward, leaving the middle out: built here for any grid size
 * (for 8 x 6 it gives the N64's table: 0, 1, ... 7, 15, 23, ... 17). */
static void intro_build_face_order(void) {
    s32 top = 0, bottom = FACE_ROWS - 1, left = 0, right = FACE_COLS - 1;
    s32 i, n = 0;

    while (top <= bottom && left <= right) {
        for (i = left; i <= right; i++) {
            if (!intro_face_is_middle(i, top)) sFaceToggleOrder[n++] = top * FACE_COLS + i;
        }
        for (i = top + 1; i <= bottom; i++) {
            if (!intro_face_is_middle(right, i)) sFaceToggleOrder[n++] = i * FACE_COLS + right;
        }
        if (top < bottom) {
            for (i = right - 1; i >= left; i--) {
                if (!intro_face_is_middle(i, bottom)) sFaceToggleOrder[n++] = bottom * FACE_COLS + i;
            }
        }
        if (left < right) {
            for (i = bottom - 1; i > top; i--) {
                if (!intro_face_is_middle(left, i)) sFaceToggleOrder[n++] = i * FACE_COLS + left;
            }
        }
        top++;
        bottom--;
        left++;
        right--;
    }
    sFaceOrderCount = n;
}

/* Averages 2 x 2 pixels of the 80 x 80 box in the middle of the screen (x 120-199, y 80-159) into a 40 x 40 picture. */
static u16 *intro_sample_framebuffer(s32 imageW, s32 imageH, s32 sampleW, s32 sampleH) {
#ifdef TARGET_PSP
    if (!gfx_psp_sample_screen(sFaceImage, imageW, imageH, 120, 80, sampleW, sampleH)) {
        return NULL;
    }
    gfx_texture_cache_forget(sFaceImage);
    return sFaceImage;
#else
    (void) imageW;
    (void) imageH;
    (void) sampleW;
    (void) sampleH;
    return NULL;
#endif
}

/* The N64 draws each square with a copy mode texture rectangle, which cannot start left of the screen: here they are quads, drawn like the
 * backdrop (full screen orthographic view, y going up). */
static Gfx *intro_draw_face(u16 *image, s32 imageW, s32 imageH) {
    Gfx *dl = alloc_display_list((10 + FACE_TILES * 2) * sizeof(Gfx));
    Vtx *verts = alloc_display_list(FACE_TILES * 4 * sizeof(Vtx));
    Gfx *dlIter;
    s32 i, x, y, s, t;

    if (dl == NULL || verts == NULL) {
        return NULL;
    }
    dlIter = dl;
    gSPDisplayList(dlIter++, dl_proj_mtx_fullscreen);
    gSPDisplayList(dlIter++, title_screen_bg_dl_0A000100);
    gSPTexture(dlIter++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_ON);
    gDPLoadTextureBlock(dlIter++, VIRTUAL_TO_PHYSICAL(image), G_IM_FMT_RGBA, G_IM_SIZ_16b, imageW, imageH, 0,
                        G_TX_CLAMP | G_TX_NOMIRROR, G_TX_CLAMP | G_TX_NOMIRROR, 6, 6, G_TX_NOLOD, G_TX_NOLOD);
    s = (imageW - 1) << 5;
    t = (imageH - 1) << 5;
    for (i = 0; i < FACE_TILES; i++) {
        Vtx *v = verts + i * 4;
        if (sFaceVisible[i] == 0) {
            continue;
        }
        x = FACE_COL_X0 + (i % FACE_COLS) * 40;
        y = 240 - (i / FACE_COLS + 1) * 40; /* bottom edge, y going up */
        make_vertex(v, 0, x,      y,      -1, 0, t, 0xFF, 0xFF, 0xFF, 0xFF);
        make_vertex(v, 1, x + 40, y,      -1, s, t, 0xFF, 0xFF, 0xFF, 0xFF);
        make_vertex(v, 2, x + 40, y + 40, -1, s, 0, 0xFF, 0xFF, 0xFF, 0xFF);
        make_vertex(v, 3, x,      y + 40, -1, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF);
        gSPVertex(dlIter++, VIRTUAL_TO_PHYSICAL(v), 4, 0);
        gSP2Triangles(dlIter++, 0, 1, 2, 0x0, 0, 2, 3, 0x0);
    }
    gSPDisplayList(dlIter++, title_screen_bg_dl_0A000190);
    gSPEndDisplayList(dlIter++);
    return dl;
}

Gfx *geo_intro_face_easter_egg(s32 state, struct GraphNode *node, UNUSED void *context) {
    struct GraphNodeGenerated *genNode = (struct GraphNodeGenerated *) node;
    u16 *image;
    Gfx *dl = NULL;
    s32 i;

    if (state != 1) {
        for (i = 0; i < FACE_TILES; i++) {
            sFaceVisible[i] = 0;
        }
        sFaceCounter = 0;
        if (sFaceOrderCount == 0) {
            intro_build_face_order();
        }
    } else {
        if (sFaceCounter == 0) {
            if (gPlayer1Controller->buttonPressed & Z_TRIG) {
                play_sound(SOUND_MENU_STAR_SOUND, gDefaultSoundArgs);
                sFaceVisible[sFaceToggleOrder[0]] ^= 1;
                sFaceCounter++;
            }
        } else {
            sFaceVisible[sFaceToggleOrder[sFaceCounter++]] ^= 1;
            if (sFaceCounter >= sFaceOrderCount) {
                sFaceCounter = 0;
            }
        }

        // Draw while the first or last face is visible.
        if (sFaceOrderCount != 0
            && (sFaceVisible[sFaceToggleOrder[0]] == 1 || sFaceVisible[sFaceToggleOrder[sFaceOrderCount - 1]] == 1)) {
            image = intro_sample_framebuffer(FACE_IMAGE_W, FACE_IMAGE_H, 2, 2);
            if (image != NULL) {
                genNode->fnNode.node.flags = (genNode->fnNode.node.flags & 0xFF) | (LAYER_OPAQUE << 8);
                dl = intro_draw_face(image, FACE_IMAGE_W, FACE_IMAGE_H);
            }
        }
    }
    return dl;
}
#endif
