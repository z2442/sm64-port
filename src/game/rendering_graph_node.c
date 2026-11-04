#include <PR/ultratypes.h>
#include <string.h>
#include "pc/psp_lprof.h"

#include "area.h"
#include "engine/math_util.h"
#include "game_init.h"
#include "gfx_dimensions.h"
#include "main.h"
#include "memory.h"
#include "print.h"
#include "rendering_graph_node.h"
#include "shadow.h"
#include "sm64.h"
extern struct Object *gMarioObject; /* object_list_processor.c */

#ifdef TARGET_PSP
/* 1 si la posicion del mundo (x, y, z) esta a menos de r unidades de Mario (o si todavia no hay Mario). Object Culling Vertical respeta
 * esta zona: lo que esta cerca de Mario se descarta igual que en el juego original. */
static int psp_near_mario(const f32 *p, f32 r) {
    const f32 *m;
    f32 dx, dy, dz;
    if (gMarioObject == NULL) {
        return 1;
    }
    m = gMarioObject->header.gfx.pos;
    dx = p[0] - m[0];
    dy = p[1] - m[1];
    dz = p[2] - m[2];
    return dx * dx + dy * dy + dz * dz < r * r;
}
#endif
#ifdef TARGET_PSP
#include "psp_options.h"
extern s16 gMenuMode; /* ingame_menu.c: -1 = sin menu de pausa */
#include "model_ids.h"
/* Hongo 1up (ver psp_billboard_pull): se acerca a la camara entre PSP_BB_PULL_MIN y PSP_BB_PULL_MAX unidades (K * distancia) sin cambiar su
 * tamano ni su posicion en pantalla. K = 0 lo apaga. */
#ifndef PSP_BB_PULL_K
#define PSP_BB_PULL_K 0.03f
#define PSP_BB_PULL_MIN 20.0f
#define PSP_BB_PULL_MAX 60.0f
#endif
/* El 1up es un cuadro plano que mira a la camara con su borde de abajo al ras del piso: de frente a una ladera queda casi paralelo al
 * terreno y a la sombra, y el z-buffer de 16 bits de la PSP los mezcla (parpadeo que cambia con el angulo de la camara). Multiplicar toda la
 * matriz (vista) por s < 1 acerca el objeto a la camara a lo largo de su rayo: se ve exactamente igual (misma posicion y tamano en pantalla)
 * pero con menos profundidad, asi que queda por delante del piso y de la sombra. */
static void psp_billboard_pull(Mat4 m) {
    const f32 d = -m[3][2];
    f32 dl, s;
    s32 i, j;
    if (d <= 120.0f) {
        return;
    }
    dl = d * PSP_BB_PULL_K;
    if (dl < PSP_BB_PULL_MIN) dl = PSP_BB_PULL_MIN;
    if (dl > PSP_BB_PULL_MAX) dl = PSP_BB_PULL_MAX;
    if (dl > d * 0.5f) dl = d * 0.5f;
    s = (d - dl) / d;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 3; j++) {
            m[i][j] *= s;
        }
    }
}
#endif

/**
 * This file contains the code that processes the scene graph for rendering.
 * The scene graph is responsible for drawing everything except the HUD / text boxes.
 * First the root of the scene graph is processed when geo_process_root
 * is called from level_script.c. The rest of the tree is traversed recursively
 * using the function geo_process_node_and_siblings, which switches over all
 * geo node types and calls a specialized function accordingly.
 * The types are defined in engine/graph_node.h
 *
 * The scene graph typically looks like:
 * - Root (viewport)
 *  - Master list
 *   - Ortho projection
 *    - Background (skybox)
 *  - Master list
 *   - Perspective
 *    - Camera
 *     - <area-specific display lists>
 *     - Object parent
 *      - <group with 240 object nodes>
 *  - Master list
 *   - Script node (Cannon overlay)
 *
 */

s16 gMatStackIndex;
Mat4 gMatStack[32];
Mtx *gMatStackFixed[32];
Mat4 gMatStackInterpolated[32];
Mtx *gMatStackInterpolatedFixed[32];

/**
 * Animation nodes have state in global variables, so this struct captures
 * the animation state so a 'context switch' can be made when rendering the
 * held object.
 */
struct GeoAnimState {
    /*0x00*/ u8 type;
    /*0x01*/ u8 enabled;
    /*0x02*/ s16 frame;
    /*0x04*/ f32 translationMultiplier;
    /*0x08*/ u16 *attribute;
    /*0x0C*/ s16 *data;
    s16 prevFrame;
};

// For some reason, this is a GeoAnimState struct, but the current state consists
// of separate global variables. It won't match EU otherwise.
struct GeoAnimState gGeoTempState;

u8 gCurAnimType;
u8 gCurAnimEnabled;
s16 gCurrAnimFrame;
s16 gPrevAnimFrame;
f32 gCurAnimTranslationMultiplier;
u16 *gCurrAnimAttribute;
s16 *gCurAnimData;

struct AllocOnlyPool *gDisplayListHeap;

struct RenderModeContainer {
    u32 modes[8];
};

/* Rendermode settings for cycle 1 for all 8 layers. */
struct RenderModeContainer renderModeTable_1Cycle[2] = { { {
    G_RM_OPA_SURF,
    G_RM_AA_OPA_SURF,
    G_RM_AA_OPA_SURF,
    G_RM_AA_OPA_SURF,
    G_RM_AA_TEX_EDGE,
    G_RM_AA_XLU_SURF,
    G_RM_AA_XLU_SURF,
    G_RM_AA_XLU_SURF,
    } },
    { {
    /* z-buffered */
    G_RM_ZB_OPA_SURF,
    G_RM_AA_ZB_OPA_SURF,
    G_RM_AA_ZB_OPA_DECAL,
    G_RM_AA_ZB_OPA_INTER,
    G_RM_AA_ZB_TEX_EDGE,
    G_RM_AA_ZB_XLU_SURF,
    G_RM_AA_ZB_XLU_DECAL,
    G_RM_AA_ZB_XLU_INTER,
    } } };

/* Rendermode settings for cycle 2 for all 8 layers. */
struct RenderModeContainer renderModeTable_2Cycle[2] = { { {
    G_RM_OPA_SURF2,
    G_RM_AA_OPA_SURF2,
    G_RM_AA_OPA_SURF2,
    G_RM_AA_OPA_SURF2,
    G_RM_AA_TEX_EDGE2,
    G_RM_AA_XLU_SURF2,
    G_RM_AA_XLU_SURF2,
    G_RM_AA_XLU_SURF2,
    } },
    { {
    /* z-buffered */
    G_RM_ZB_OPA_SURF2,
    G_RM_AA_ZB_OPA_SURF2,
    G_RM_AA_ZB_OPA_DECAL2,
    G_RM_AA_ZB_OPA_INTER2,
    G_RM_AA_ZB_TEX_EDGE2,
    G_RM_AA_ZB_XLU_SURF2,
    G_RM_AA_ZB_XLU_DECAL2,
    G_RM_AA_ZB_XLU_INTER2,
    } } };

struct GraphNodeRoot *gCurGraphNodeRoot = NULL;
struct GraphNodeMasterList *gCurGraphNodeMasterList = NULL;
struct GraphNodePerspective *gCurGraphNodeCamFrustum = NULL;
struct GraphNodeCamera *gCurGraphNodeCamera = NULL;
struct GraphNodeObject *gCurGraphNodeObject = NULL;
struct GraphNodeHeldObject *gCurGraphNodeHeldObject = NULL;
u16 gAreaUpdateCounter = 0;

#ifdef F3DEX_GBI_2
LookAt lookAt;
#endif

static Gfx *sPerspectivePos;
static Mtx *sPerspectiveMtx;
static Mtx *sPerspectiveMtxInterpolated; /* la que lleva la lista en el 1er cuadro (la real es sPerspectiveMtx) */
static f32 sPspPerspNear = 100.0f;       /* near de la ultima proyeccion (PSP_F2_REPLAY) */
/* Escala del plano lejano con la que se construyo la ultima proyeccion de la camara (1.0 = la original del juego). La lee gfx_pc.c para que la
 * niebla se calcule siempre con la proyeccion ORIGINAL y no cambie con "Far Distance" (ver gfx_pc.c, PSP_FOG_FAR_INDEPENDENT). */
float gPspPerspFarScaleUsed = 1.0f;

struct {
    Gfx *pos;
    void *mtx;
    void *displayList;

#ifdef TARGET_PSP
    void *replay; /* matriz que debe llevar este objeto en el 2o cuadro si la escena se repite (NULL = dejar la del 1o) */
#endif
} gMtxTbl[6400];
s32 gMtxTblSize;

#ifdef TARGET_PSP
/* Sombras (PSP_F2_REPLAY): su lista de dibujo interpolada tiene las alturas del suelo respecto a la posicion INTERPOLADA, asi que en el
 * 2o cuadro (que repite esa lista) la matriz debe llevar x/z reales pero la altura interpolada. Se guarda aqui por nodo de lista. */
static struct { void *node; void *mtx; } sReplayMtx[128];
static int sReplayMtxCount;
static void *sNextReplayMtx;
#endif

#ifdef TARGET_PSP
/* PSP_F2_SKY: el cielo se crea dos veces por tick (estado real e interpolado). Si las dos versiones dibujan los mismos tiles, la
 * unica diferencia es su matriz ortografica: el 2o cuadro puede cambiar una por otra (gfx_pc.c). */
extern const void *gPspSkyOrtho;
extern s32 gPspSkyBase;
static const void *sSkyRealOrtho;
static const void *sSkyInterpOrtho;
static s32 sSkyRealBase;
static s32 sSkyMatch;
const float *psp_sky_interp_ortho(void) { return sSkyMatch ? (const float *) sSkyInterpOrtho : NULL; }
const float *psp_sky_real_ortho(void) { return sSkyMatch ? (const float *) sSkyRealOrtho : NULL; }
#endif

static Gfx *sViewportPos;
static Vp sPrevViewport;
static u8 sViewportDiffers; /* 1 si el viewport (scissor) interpolado no es el real */

#ifdef TARGET_PSP
/* ---- PSP_F2_REPLAY (etapa 2): lo que el 2o cuadro de un tick de 60 fps necesita saber de los parches de matrices ----
 * El 2o cuadro repite la escena del 1o y solo reescribe las matrices de cada objeto. gfx_pc.c lee la tabla gMtxTbl (ANTES de que
 * patch_interpolations() la vacie) y pregunta si el parche de proyeccion / viewport hace algo mas que cambiar matrices de objetos. */
int psp_mtx_tbl_count(void) { return (int) gMtxTblSize; }
Gfx *psp_mtx_tbl_pos(int i) { return gMtxTbl[i].pos; }
/* Matriz (Mtx con GBI_FLOATS = 16 floats) para el 2o cuadro, o NULL si ese objeto debe quedarse como salio en el 1o (cielo...). */
const float *psp_mtx_tbl_mtx(int i) { return (const float *) gMtxTbl[i].replay; }
float psp_persp_near(void) { return (float) sPspPerspNear; }
/* 1 si la proyeccion del 2o cuadro no es la misma que la del 1o (cambio de fov): no se puede repetir la escena tal cual */
int psp_projection_patch_pending(void) {
    return sPerspectivePos != NULL && sPerspectiveMtxInterpolated != NULL &&
           memcmp(sPerspectiveMtx, sPerspectiveMtxInterpolated, sizeof(Mtx)) != 0;
}
int psp_viewport_patch_pending(void) {
    return sViewportPos != NULL && sViewportDiffers;
}
#endif

void mtx_patch_interpolated(void) {
    s32 i;

    if (sPerspectivePos != NULL) {
        gSPMatrix(sPerspectivePos, VIRTUAL_TO_PHYSICAL(sPerspectiveMtx), G_MTX_PROJECTION | G_MTX_LOAD | G_MTX_NOPUSH);
    }

    for (i = 0; i < gMtxTblSize; i++) {
        Gfx *pos = gMtxTbl[i].pos;
        gSPMatrix(pos++, VIRTUAL_TO_PHYSICAL(gMtxTbl[i].mtx),
                  G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
        gSPDisplayList(pos++, gMtxTbl[i].displayList);
    }

    if (sViewportPos != NULL) {
        Gfx *saved = gDisplayListHead;
        gDisplayListHead = sViewportPos;
        make_viewport_clip_rect(&sPrevViewport);
        gSPViewport(gDisplayListHead, VIRTUAL_TO_PHYSICAL(&sPrevViewport));
        gDisplayListHead = saved;
    }

    gMtxTblSize = 0;
#ifdef TARGET_PSP
    sSkyMatch = 0;
    sSkyRealOrtho = NULL;
    sSkyInterpOrtho = NULL;
    sReplayMtxCount = 0;
    sNextReplayMtx = NULL;
#endif
    sPerspectivePos = NULL;
    sPerspectiveMtxInterpolated = NULL;
    sViewportPos = NULL;
    sViewportDiffers = 0;
}

/**
 * Process a master list node.
 */
#ifdef TARGET_PSP
static void *psp_replay_mtx_for(struct DisplayListNode *n) {
    int k;
    if (n->displayList == n->displayListInterpolated) {
        return n->transform; /* objeto normal: en el 2o cuadro lleva su matriz real */
    }
    for (k = 0; k < sReplayMtxCount; k++) {
        if (sReplayMtx[k].node == n) {
            return sReplayMtx[k].mtx;
        }
    }
    return NULL; /* lista distinta (cielo...) sin matriz propia: se queda como en el 1er cuadro */
}
#endif

static void geo_process_master_list_sub(struct GraphNodeMasterList *node) {
    struct DisplayListNode *currList;
    s32 i;
    s32 enableZBuffer = (node->node.flags & GRAPH_RENDER_Z_BUFFER) != 0;
    struct RenderModeContainer *modeList = &renderModeTable_1Cycle[enableZBuffer];
    struct RenderModeContainer *mode2List = &renderModeTable_2Cycle[enableZBuffer];

    // @bug This is where the LookAt values should be calculated but aren't.
    // As a result, environment mapping is broken on Fast3DEX2 without the
    // changes below.
#ifdef F3DEX_GBI_2
    Mtx lMtx;
    guLookAtReflect(&lMtx, &lookAt, 0, 0, 0, /* eye */ 0, 0, 1, /* at */ 1, 0, 0 /* up */);
#endif

    if (enableZBuffer != 0) {
        gDPPipeSync(gDisplayListHead++);
        gSPSetGeometryMode(gDisplayListHead++, G_ZBUFFER);
    }

    for (i = 0; i < GFX_NUM_MASTER_LISTS; i++) {
        if ((currList = node->listHeads[i]) != NULL) {
            gDPSetRenderMode(gDisplayListHead++, modeList->modes[i], mode2List->modes[i]);
            while (currList != NULL) {
                if ((u32) gMtxTblSize < sizeof(gMtxTbl) / sizeof(gMtxTbl[0])) {
                    gMtxTbl[gMtxTblSize].pos = gDisplayListHead;
                    gMtxTbl[gMtxTblSize].mtx = currList->transform;
#ifdef TARGET_PSP
                    gMtxTbl[gMtxTblSize].replay = psp_replay_mtx_for(currList);
#endif
                    gMtxTbl[gMtxTblSize++].displayList = currList->displayList;
                }
                gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(currList->transformInterpolated),
                          G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
                gSPDisplayList(gDisplayListHead++, currList->displayListInterpolated);
                currList = currList->next;
            }
        }
    }
    if (enableZBuffer != 0) {
        gDPPipeSync(gDisplayListHead++);
        gSPClearGeometryMode(gDisplayListHead++, G_ZBUFFER);
    }
}

/**
 * Appends the display list to one of the master lists based on the layer
 * parameter. Look at the RenderModeContainer struct to see the corresponding
 * render modes of layers.
 */
static void geo_append_display_list2(void *displayList, void *displayListInterpolated, s16 layer) {

#ifdef F3DEX_GBI_2
    gSPLookAt(gDisplayListHead++, &lookAt);
#endif
    if (gCurGraphNodeMasterList != 0) {
        struct DisplayListNode *listNode =
            alloc_only_pool_alloc(gDisplayListHeap, sizeof(struct DisplayListNode));

        listNode->transform = gMatStackFixed[gMatStackIndex];
        listNode->transformInterpolated = gMatStackInterpolatedFixed[gMatStackIndex];
        listNode->displayList = displayList;
        listNode->displayListInterpolated = displayListInterpolated;
#ifdef TARGET_PSP
        if (sNextReplayMtx != NULL) {
            if (sReplayMtxCount < 128) {
                sReplayMtx[sReplayMtxCount].node = listNode;
                sReplayMtx[sReplayMtxCount].mtx = sNextReplayMtx;
                sReplayMtxCount++;
            }
            sNextReplayMtx = NULL;
        }
#endif
        listNode->next = 0;
        if (gCurGraphNodeMasterList->listHeads[layer] == 0) {
            gCurGraphNodeMasterList->listHeads[layer] = listNode;
        } else {
            gCurGraphNodeMasterList->listTails[layer]->next = listNode;
        }
        gCurGraphNodeMasterList->listTails[layer] = listNode;
    }
}

static void geo_append_display_list(void *displayList, s16 layer) {
    geo_append_display_list2(displayList, displayList, layer);
}

/**
 * Process the master list node.
 */
static void geo_process_master_list(struct GraphNodeMasterList *node) {
    s32 i;
    UNUSED s32 sp1C;

    if (gCurGraphNodeMasterList == NULL && node->node.children != NULL) {
        gCurGraphNodeMasterList = node;
        for (i = 0; i < GFX_NUM_MASTER_LISTS; i++) {
            node->listHeads[i] = NULL;
        }
        geo_process_node_and_siblings(node->node.children);
        {
            LPROF_BEGIN(lp_m);
            geo_process_master_list_sub(node);
            LPROF_END(lp_m, LP_MLS);
        }
        gCurGraphNodeMasterList = NULL;
    }
}

/**
 * Process an orthographic projection node.
 */
static void geo_process_ortho_projection(struct GraphNodeOrthoProjection *node) {
    if (node->node.children != NULL) {
        Mtx *mtx = alloc_display_list(sizeof(*mtx));
        f32 left = (gCurGraphNodeRoot->x - gCurGraphNodeRoot->width) / 2.0f * node->scale;
        f32 right = (gCurGraphNodeRoot->x + gCurGraphNodeRoot->width) / 2.0f * node->scale;
        f32 top = (gCurGraphNodeRoot->y - gCurGraphNodeRoot->height) / 2.0f * node->scale;
        f32 bottom = (gCurGraphNodeRoot->y + gCurGraphNodeRoot->height) / 2.0f * node->scale;

        guOrtho(mtx, left, right, bottom, top, -2.0f, 2.0f, 1.0f);
        gSPPerspNormalize(gDisplayListHead++, 0xFFFF);
        gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(mtx), G_MTX_PROJECTION | G_MTX_LOAD | G_MTX_NOPUSH);

        geo_process_node_and_siblings(node->node.children);
    }
}

/**
 * Process a perspective projection node.
 */
static void geo_process_perspective(struct GraphNodePerspective *node) {
    if (node->fnNode.func != NULL) {
        {
            LPROF_BEGIN(lp_f);
            node->fnNode.func(GEO_CONTEXT_RENDER, &node->fnNode.node, gMatStack[gMatStackIndex]);
            LPROF_END(lp_f, LP_FN);
            LPROF_COUNT(LC_NFN);
        }
    }
    if (node->fnNode.node.children != NULL) {
        u16 perspNorm;
        Mtx *mtxInterpolated = alloc_display_list(sizeof(*mtxInterpolated));
        Mtx *mtx = alloc_display_list(sizeof(*mtx));
        f32 fovInterpolated;

#if defined(VERSION_EU) && !defined(TARGET_PSP) /* PSP: el 1.1 compensa el estirado vertical del modo PAL de la TV; la pantalla de la PSP no lo hace y todo salia estirado en vertical */
        f32 aspect = ((f32) gCurGraphNodeRoot->width / (f32) gCurGraphNodeRoot->height) * 1.1f;
#else
        f32 aspect = (f32) gCurGraphNodeRoot->width / (f32) gCurGraphNodeRoot->height;
#endif

#ifdef TARGET_PSP
        /* menu de opciones (Far Distance): plano lejano de la camara alargado (nunca acortado). En pausa se deja el original. */
        const f32 farUse = node->far * (gMenuMode == -1 ? gPspFarScale : 1.0f);
        gPspPerspFarScaleUsed = (gMenuMode == -1 && gPspFarScale > 1.0f) ? (float) gPspFarScale : 1.0f;
#else
        const f32 farUse = node->far;
#endif
        guPerspective(mtx, &perspNorm, node->fov, aspect, node->near, farUse, 1.0f);
        sPspPerspNear = (f32) node->near;

        if (gGlobalTimer == node->prevTimestamp + 1 && gGlobalTimer != gLakituState.skipCameraInterpolationTimestamp) {

            fovInterpolated = (node->prevFov + node->fov) / 2.0f;
            guPerspective(mtxInterpolated, &perspNorm, fovInterpolated, aspect, node->near, farUse, 1.0f);
            gSPPerspNormalize(gDisplayListHead++, perspNorm);

            sPerspectivePos = gDisplayListHead;
            sPerspectiveMtx = mtx;
            sPerspectiveMtxInterpolated = mtxInterpolated;
            gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(mtxInterpolated),
                      G_MTX_PROJECTION | G_MTX_LOAD | G_MTX_NOPUSH);
        } else {
            gSPPerspNormalize(gDisplayListHead++, perspNorm);
            gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(mtx), G_MTX_PROJECTION | G_MTX_LOAD | G_MTX_NOPUSH);
        }
        node->prevFov = node->fov;
        node->prevTimestamp = gGlobalTimer;

        gCurGraphNodeCamFrustum = node;
        geo_process_node_and_siblings(node->fnNode.node.children);
        gCurGraphNodeCamFrustum = NULL;
    }
}

/**
 * Process a level of detail node. From the current transformation matrix,
 * the perpendicular distance to the camera is extracted and the children
 * of this node are only processed if that distance is within the render
 * range of this node.
 */
static void geo_process_level_of_detail(struct GraphNodeLevelOfDetail *node) {
#ifdef GBI_FLOATS
    Mtx *mtx = gMatStackFixed[gMatStackIndex];
    s16 distanceFromCam = (s32) -mtx->m[3][2]; // z-component of the translation column
#else
    // The fixed point Mtx type is defined as 16 longs, but it's actually 16
    // shorts for the integer parts followed by 16 shorts for the fraction parts
    Mtx *mtx = gMatStackFixed[gMatStackIndex];
    s16 distanceFromCam = -GET_HIGH_S16_OF_32(mtx->m[1][3]); // z-component of the translation column
#endif

#ifndef TARGET_N64
    // We assume modern hardware is powerful enough to draw the most detailed variant
    distanceFromCam = 0;
#endif

    if (node->minDistance <= distanceFromCam && distanceFromCam < node->maxDistance) {
        if (node->node.children != 0) {
            geo_process_node_and_siblings(node->node.children);
        }
    }
}

/**
 * Process a switch case node. The node's selection function is called
 * if it is 0, and among the node's children, only the selected child is
 * processed next.
 */
static void geo_process_switch(struct GraphNodeSwitchCase *node) {
    struct GraphNode *selectedChild = node->fnNode.node.children;
    s32 i;

    if (node->fnNode.func != NULL) {
        {
            LPROF_BEGIN(lp_f);
            node->fnNode.func(GEO_CONTEXT_RENDER, &node->fnNode.node, gMatStack[gMatStackIndex]);
            LPROF_END(lp_f, LP_FN);
            LPROF_COUNT(LC_NFN);
        }
    }
    for (i = 0; selectedChild != NULL && node->selectedCase > i; i++) {
        selectedChild = selectedChild->next;
    }
    if (selectedChild != NULL) {
        geo_process_node_and_siblings(selectedChild);
    }
}

void interpolate_vectors(Vec3f res, Vec3f a, Vec3f b) {
    res[0] = (a[0] + b[0]) / 2.0f;
    res[1] = (a[1] + b[1]) / 2.0f;
    res[2] = (a[2] + b[2]) / 2.0f;
}

void interpolate_vectors_s16(Vec3s res, Vec3s a, Vec3s b) {
    res[0] = (a[0] + b[0]) / 2;
    res[1] = (a[1] + b[1]) / 2;
    res[2] = (a[2] + b[2]) / 2;
}

static s16 interpolate_angle(s16 a, s16 b) {
    s32 absDiff = b - a;
    if (absDiff < 0) {
        absDiff = -absDiff;
    }
    if (absDiff >= 0x4000 && absDiff <= 0xC000) {
        return b;
    }
    if (absDiff <= 0x8000) {
        return (a + b) / 2;
    } else {
        return (a + b) / 2 + 0x8000;
    }
}

static void interpolate_angles(Vec3s res, Vec3s a, Vec3s b) {
    res[0] = interpolate_angle(a[0], b[0]);
    res[1] = interpolate_angle(a[1], b[1]);
    res[2] = interpolate_angle(a[2], b[2]);
}

/**
 * Process a camera node.
 */
static void geo_process_camera(struct GraphNodeCamera *node) {
    Mat4 cameraTransform;
    Mtx *rollMtx = alloc_display_list(sizeof(*rollMtx));
    Mtx *mtx = alloc_display_list(sizeof(*mtx));
    Mtx *mtxInterpolated = alloc_display_list(sizeof(*mtxInterpolated));
    Vec3f posInterpolated;
    Vec3f focusInterpolated;

    if (node->fnNode.func != NULL) {
        {
            LPROF_BEGIN(lp_f);
            node->fnNode.func(GEO_CONTEXT_RENDER, &node->fnNode.node, gMatStack[gMatStackIndex]);
            LPROF_END(lp_f, LP_FN);
            LPROF_COUNT(LC_NFN);
        }
    }
    mtxf_rotate_xy(rollMtx, node->rollScreen);

    gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(rollMtx), G_MTX_PROJECTION | G_MTX_MUL | G_MTX_NOPUSH);

    mtxf_lookat(cameraTransform, node->pos, node->focus, node->roll);
    mtxf_mul(gMatStack[gMatStackIndex + 1], cameraTransform, gMatStack[gMatStackIndex]);

    if (gGlobalTimer == node->prevTimestamp + 1 && gGlobalTimer != gLakituState.skipCameraInterpolationTimestamp) {
        interpolate_vectors(posInterpolated, node->prevPos, node->pos);
        interpolate_vectors(focusInterpolated, node->prevFocus, node->focus);
        float magnitude = 0;
        for (int i = 0; i < 3; i++) {
            float diff = node->pos[i] - node->prevPos[i];
            magnitude += diff * diff;
        }
        if (magnitude > 500000) {
            // Observed ~479000 in BBH when toggling R camera
            // Can get over 3 million in VCUTM though...
            vec3f_copy(posInterpolated, node->pos);
            vec3f_copy(focusInterpolated, node->focus);
        }
    } else {
        vec3f_copy(posInterpolated, node->pos);
        vec3f_copy(focusInterpolated, node->focus);
    }
    vec3f_copy(node->prevPos, node->pos);
    vec3f_copy(node->prevFocus, node->focus);
    node->prevTimestamp = gGlobalTimer;
    mtxf_lookat(cameraTransform, posInterpolated, focusInterpolated, node->roll);
    mtxf_mul(gMatStackInterpolated[gMatStackIndex + 1], cameraTransform, gMatStackInterpolated[gMatStackIndex]);

    gMatStackIndex++;
    mtxf_to_mtx(mtx, gMatStack[gMatStackIndex]);
    gMatStackFixed[gMatStackIndex] = mtx;
    mtxf_to_mtx(mtxInterpolated, gMatStackInterpolated[gMatStackIndex]);
    gMatStackInterpolatedFixed[gMatStackIndex] = mtxInterpolated;
    if (node->fnNode.node.children != 0) {
        gCurGraphNodeCamera = node;
        node->matrixPtr = &gMatStack[gMatStackIndex];
        node->matrixPtrInterpolated = &gMatStackInterpolated[gMatStackIndex];
        geo_process_node_and_siblings(node->fnNode.node.children);
        gCurGraphNodeCamera = NULL;
    }
    gMatStackIndex--;
}

/**
 * Process a translation / rotation node. A transformation matrix based
 * on the node's translation and rotation is created and pushed on both
 * the float and fixed point matrix stacks.
 * For the rest it acts as a normal display list node.
 */
static void geo_process_translation_rotation(struct GraphNodeTranslationRotation *node) {
    Mat4 mtxf;
    Vec3f translation;
    Mtx *mtx = alloc_display_list(sizeof(*mtx));
    Mtx *mtxInterpolated = alloc_display_list(sizeof(*mtxInterpolated));

    vec3s_to_vec3f(translation, node->translation);
    mtxf_rotate_zxy_and_translate(mtxf, translation, node->rotation);
    mtxf_mul(gMatStack[gMatStackIndex + 1], mtxf, gMatStack[gMatStackIndex]);
    mtxf_mul(gMatStackInterpolated[gMatStackIndex + 1], mtxf, gMatStackInterpolated[gMatStackIndex]);
    gMatStackIndex++;
    mtxf_to_mtx(mtx, gMatStack[gMatStackIndex]);
    gMatStackFixed[gMatStackIndex] = mtx;
    mtxf_to_mtx(mtxInterpolated, gMatStackInterpolated[gMatStackIndex]);
    gMatStackInterpolatedFixed[gMatStackIndex] = mtxInterpolated;
    if (node->displayList != NULL) {
        geo_append_display_list(node->displayList, node->node.flags >> 8);
    }
    if (node->node.children != NULL) {
        geo_process_node_and_siblings(node->node.children);
    }
    gMatStackIndex--;
}

/**
 * Process a translation node. A transformation matrix based on the node's
 * translation is created and pushed on both the float and fixed point matrix stacks.
 * For the rest it acts as a normal display list node.
 */
static void geo_process_translation(struct GraphNodeTranslation *node) {
    Mat4 mtxf;
    Vec3f translation;
    Mtx *mtx = alloc_display_list(sizeof(*mtx));
    Mtx *mtxInterpolated = alloc_display_list(sizeof(*mtxInterpolated));

    vec3s_to_vec3f(translation, node->translation);
    mtxf_rotate_zxy_and_translate(mtxf, translation, gVec3sZero);
    mtxf_mul(gMatStack[gMatStackIndex + 1], mtxf, gMatStack[gMatStackIndex]);
    mtxf_mul(gMatStackInterpolated[gMatStackIndex + 1], mtxf, gMatStackInterpolated[gMatStackIndex]);
    gMatStackIndex++;
    mtxf_to_mtx(mtx, gMatStack[gMatStackIndex]);
    gMatStackFixed[gMatStackIndex] = mtx;
    mtxf_to_mtx(mtxInterpolated, gMatStackInterpolated[gMatStackIndex]);
    gMatStackInterpolatedFixed[gMatStackIndex] = mtxInterpolated;
    if (node->displayList != NULL) {
        geo_append_display_list(node->displayList, node->node.flags >> 8);
    }
    if (node->node.children != NULL) {
        geo_process_node_and_siblings(node->node.children);
    }
    gMatStackIndex--;
}

/**
 * Process a rotation node. A transformation matrix based on the node's
 * rotation is created and pushed on both the float and fixed point matrix stacks.
 * For the rest it acts as a normal display list node.
 */
static void geo_process_rotation(struct GraphNodeRotation *node) {
    Mat4 mtxf;
    Mtx *mtx = alloc_display_list(sizeof(*mtx));
    Mtx *mtxInterpolated = alloc_display_list(sizeof(*mtxInterpolated));
    Vec3s rotationInterpolated;

    mtxf_rotate_zxy_and_translate(mtxf, gVec3fZero, node->rotation);
    mtxf_mul(gMatStack[gMatStackIndex + 1], mtxf, gMatStack[gMatStackIndex]);
    if (gGlobalTimer == node->prevTimestamp + 1) {
        interpolate_angles(rotationInterpolated, node->prevRotation, node->rotation);
        mtxf_rotate_zxy_and_translate(mtxf, gVec3fZero, rotationInterpolated);
    }
    vec3s_copy(node->prevRotation, node->rotation);
    node->prevTimestamp = gGlobalTimer;
    mtxf_mul(gMatStackInterpolated[gMatStackIndex + 1], mtxf, gMatStackInterpolated[gMatStackIndex]);
    gMatStackIndex++;
    mtxf_to_mtx(mtx, gMatStack[gMatStackIndex]);
    gMatStackFixed[gMatStackIndex] = mtx;
    mtxf_to_mtx(mtxInterpolated, gMatStackInterpolated[gMatStackIndex]);
    gMatStackInterpolatedFixed[gMatStackIndex] = mtxInterpolated;
    if (node->displayList != NULL) {
        geo_append_display_list(node->displayList, node->node.flags >> 8);
    }
    if (node->node.children != NULL) {
        geo_process_node_and_siblings(node->node.children);
    }
    gMatStackIndex--;
}

/**
 * Process a scaling node. A transformation matrix based on the node's
 * scale is created and pushed on both the float and fixed point matrix stacks.
 * For the rest it acts as a normal display list node.
 */
static void geo_process_scale(struct GraphNodeScale *node) {
    UNUSED Mat4 transform;
    Vec3f scaleVec;
    Mtx *mtx = alloc_display_list(sizeof(*mtx));
    Mtx *mtxInterpolated = alloc_display_list(sizeof(*mtxInterpolated));

    vec3f_set(scaleVec, node->scale, node->scale, node->scale);
    mtxf_scale_vec3f(gMatStack[gMatStackIndex + 1], gMatStack[gMatStackIndex], scaleVec);
    mtxf_scale_vec3f(gMatStackInterpolated[gMatStackIndex + 1], gMatStackInterpolated[gMatStackIndex], scaleVec);
    gMatStackIndex++;
    mtxf_to_mtx(mtx, gMatStack[gMatStackIndex]);
    gMatStackFixed[gMatStackIndex] = mtx;
    mtxf_to_mtx(mtxInterpolated, gMatStackInterpolated[gMatStackIndex]);
    gMatStackInterpolatedFixed[gMatStackIndex] = mtxInterpolated;
    if (node->displayList != NULL) {
        geo_append_display_list(node->displayList, node->node.flags >> 8);
    }
    if (node->node.children != NULL) {
        geo_process_node_and_siblings(node->node.children);
    }
    gMatStackIndex--;
}

/**
 * Process a billboard node. A transformation matrix is created that makes its
 * children face the camera, and it is pushed on the floating point and fixed
 * point matrix stacks.
 * For the rest it acts as a normal display list node.
 */
static void geo_process_billboard(struct GraphNodeBillboard *node) {
    Vec3f translation;
    Mtx *mtx = alloc_display_list(sizeof(*mtx));
    Mtx *mtxInterpolated = alloc_display_list(sizeof(*mtxInterpolated));

    gMatStackIndex++;
    vec3s_to_vec3f(translation, node->translation);
    mtxf_billboard(gMatStack[gMatStackIndex], gMatStack[gMatStackIndex - 1], translation,
                   gCurGraphNodeCamera->roll);
    mtxf_billboard(gMatStackInterpolated[gMatStackIndex], gMatStackInterpolated[gMatStackIndex - 1], translation,
                   gCurGraphNodeCamera->roll);
    if (gCurGraphNodeHeldObject != NULL) {
        mtxf_scale_vec3f(gMatStack[gMatStackIndex], gMatStack[gMatStackIndex],
                         gCurGraphNodeHeldObject->objNode->header.gfx.scale);
        mtxf_scale_vec3f(gMatStackInterpolated[gMatStackIndex], gMatStackInterpolated[gMatStackIndex],
                         gCurGraphNodeHeldObject->objNode->header.gfx.scale);
    } else if (gCurGraphNodeObject != NULL) {
        mtxf_scale_vec3f(gMatStack[gMatStackIndex], gMatStack[gMatStackIndex],
                         gCurGraphNodeObject->scale);
        mtxf_scale_vec3f(gMatStackInterpolated[gMatStackIndex], gMatStackInterpolated[gMatStackIndex],
                         gCurGraphNodeObject->scale);
    }

    mtxf_to_mtx(mtx, gMatStack[gMatStackIndex]);
    gMatStackFixed[gMatStackIndex] = mtx;
    mtxf_to_mtx(mtxInterpolated, gMatStackInterpolated[gMatStackIndex]);
    gMatStackInterpolatedFixed[gMatStackIndex] = mtxInterpolated;
    if (node->displayList != NULL) {
        geo_append_display_list(node->displayList, node->node.flags >> 8);
    }
    if (node->node.children != NULL) {
        geo_process_node_and_siblings(node->node.children);
    }
    gMatStackIndex--;
}

/**
 * Process a display list node. It draws a display list without first pushing
 * a transformation on the stack, so all transformations are inherited from the
 * parent node. It processes its children if it has them.
 */
static void geo_process_display_list(struct GraphNodeDisplayList *node) {
    if (node->displayList != NULL) {
        geo_append_display_list(node->displayList, node->node.flags >> 8);
    }
    if (node->node.children != NULL) {
        geo_process_node_and_siblings(node->node.children);
    }
}

/**
 * Process a generated list. Instead of storing a pointer to a display list,
 * the list is generated on the fly by a function.
 */
static void geo_process_generated_list(struct GraphNodeGenerated *node) {
    if (node->fnNode.func != NULL) {
        LPROF_BEGIN(lp_f);
        Gfx *list = node->fnNode.func(GEO_CONTEXT_RENDER, &node->fnNode.node,
                                     (struct AllocOnlyPool *) gMatStack[gMatStackIndex]);
        LPROF_END(lp_f, LP_FN);
        LPROF_COUNT(LC_NFN);

        if (list != 0) {
            geo_append_display_list((void *) VIRTUAL_TO_PHYSICAL(list), node->fnNode.node.flags >> 8);
        }
    }
    if (node->fnNode.node.children != NULL) {
        geo_process_node_and_siblings(node->fnNode.node.children);
    }
}

/**
 * Process a background node. Tries to retrieve a background display list from
 * the function of the node. If that function is null or returns null, a black
 * rectangle is drawn instead.
 */
static void geo_process_background(struct GraphNodeBackground *node) {
    Gfx *list = NULL;
    Gfx *listInterpolated = NULL;

    if (node->fnNode.func != NULL) {
        Vec3f posCopy;
        Vec3f focusCopy;
        Vec3f posInterpolated;
        Vec3f focusInterpolated;

        if (gGlobalTimer == node->prevCameraTimestamp + 1 &&
            gGlobalTimer != gLakituState.skipCameraInterpolationTimestamp) {
            interpolate_vectors(posInterpolated, node->prevCameraPos, gLakituState.pos);
            interpolate_vectors(focusInterpolated, node->prevCameraFocus, gLakituState.focus);
        } else {
            vec3f_copy(posInterpolated, gLakituState.pos);
            vec3f_copy(focusInterpolated, gLakituState.focus);
        }
        vec3f_copy(node->prevCameraPos, gLakituState.pos);
        vec3f_copy(node->prevCameraFocus, gLakituState.focus);
        node->prevCameraTimestamp = gGlobalTimer;

#ifdef TARGET_PSP
        gPspSkyOrtho = NULL;
#endif
        {
            LPROF_BEGIN(lp_f);
            list = node->fnNode.func(GEO_CONTEXT_RENDER, &node->fnNode.node,
                                     (struct AllocOnlyPool *) gMatStack[gMatStackIndex]);
            LPROF_END(lp_f, LP_FN);
            LPROF_COUNT(LC_NFN);
        }
#ifdef TARGET_PSP
        sSkyRealOrtho = gPspSkyOrtho;
        sSkyRealBase = gPspSkyBase;
        gPspSkyOrtho = NULL;
#endif
        vec3f_copy(posCopy, gLakituState.pos);
        vec3f_copy(focusCopy, gLakituState.focus);
        vec3f_copy(gLakituState.pos, posInterpolated);
        vec3f_copy(gLakituState.focus, focusInterpolated);
        {
            LPROF_BEGIN(lp_f);
            listInterpolated = node->fnNode.func(GEO_CONTEXT_RENDER, &node->fnNode.node, NULL);
            LPROF_END(lp_f, LP_FN);
            LPROF_COUNT(LC_NFN);
        }
#ifdef TARGET_PSP
        sSkyInterpOrtho = gPspSkyOrtho;
        sSkyMatch = (sSkyRealOrtho != NULL && sSkyInterpOrtho != NULL && sSkyRealBase == gPspSkyBase);
#endif
        vec3f_copy(gLakituState.pos, posCopy);
        vec3f_copy(gLakituState.focus, focusCopy);
    }
    if (list != 0) {
        geo_append_display_list2((void *) VIRTUAL_TO_PHYSICAL(list),
                                 (void *) VIRTUAL_TO_PHYSICAL(listInterpolated), node->fnNode.node.flags >> 8);
    } else if (gCurGraphNodeMasterList != NULL) {
#ifndef F3DEX_GBI_2E
        Gfx *gfxStart = alloc_display_list(sizeof(Gfx) * 7);
#else
        Gfx *gfxStart = alloc_display_list(sizeof(Gfx) * 8);
#endif
        Gfx *gfx = gfxStart;

        gDPPipeSync(gfx++);
        gDPSetCycleType(gfx++, G_CYC_FILL);
        gDPSetFillColor(gfx++, node->background);
        gDPFillRectangle(gfx++, GFX_DIMENSIONS_RECT_FROM_LEFT_EDGE(0), BORDER_HEIGHT,
        GFX_DIMENSIONS_RECT_FROM_RIGHT_EDGE(0) - 1, SCREEN_HEIGHT - BORDER_HEIGHT - 1);
        gDPPipeSync(gfx++);
        gDPSetCycleType(gfx++, G_CYC_1CYCLE);
        gSPEndDisplayList(gfx++);

        geo_append_display_list((void *) VIRTUAL_TO_PHYSICAL(gfxStart), 0);
    }
    if (node->fnNode.node.children != NULL) {
        geo_process_node_and_siblings(node->fnNode.node.children);
    }
}

static void anim_process(Vec3f translation, Vec3s rotation, u8 *animType, s16 animFrame, u16 **animAttribute) {
    if (*animType == ANIM_TYPE_TRANSLATION) {
        translation[0] += gCurAnimData[retrieve_animation_index(animFrame, animAttribute)]
                          * gCurAnimTranslationMultiplier;
        translation[1] += gCurAnimData[retrieve_animation_index(animFrame, animAttribute)]
                          * gCurAnimTranslationMultiplier;
        translation[2] += gCurAnimData[retrieve_animation_index(animFrame, animAttribute)]
                          * gCurAnimTranslationMultiplier;
        *animType = ANIM_TYPE_ROTATION;
    } else {
        if (*animType == ANIM_TYPE_LATERAL_TRANSLATION) {
            translation[0] +=
                gCurAnimData[retrieve_animation_index(animFrame, animAttribute)]
                * gCurAnimTranslationMultiplier;
            *animAttribute += 2;
            translation[2] +=
                gCurAnimData[retrieve_animation_index(animFrame, animAttribute)]
                * gCurAnimTranslationMultiplier;
            *animType = ANIM_TYPE_ROTATION;
        } else {
            if (*animType == ANIM_TYPE_VERTICAL_TRANSLATION) {
                *animAttribute += 2;
                translation[1] +=
                    gCurAnimData[retrieve_animation_index(animFrame, animAttribute)]
                    * gCurAnimTranslationMultiplier;
                *animAttribute += 2;
                *animType = ANIM_TYPE_ROTATION;
            } else if (*animType == ANIM_TYPE_NO_TRANSLATION) {
                *animAttribute += 6;
                *animType = ANIM_TYPE_ROTATION;
            }
        }
    }

    if (*animType == ANIM_TYPE_ROTATION) {
        rotation[0] = gCurAnimData[retrieve_animation_index(animFrame, animAttribute)];
        rotation[1] = gCurAnimData[retrieve_animation_index(animFrame, animAttribute)];
        rotation[2] = gCurAnimData[retrieve_animation_index(animFrame, animAttribute)];
    }
}

/**
 * Render an animated part. The current animation state is not part of the node
 * but set in global variables. If an animated part is skipped, everything afterwards desyncs.
 */
static void geo_process_animated_part(struct GraphNodeAnimatedPart *node) {
    Mat4 matrix;
    Vec3s rotation;
    Vec3f translation;
    Vec3s rotationInterpolated;
    Vec3f translationInterpolated;
    Mtx *matrixPtr = alloc_display_list(sizeof(*matrixPtr));
    Mtx *mtxInterpolated = alloc_display_list(sizeof(*mtxInterpolated));
    u16 *animAttribute = gCurrAnimAttribute;
    u8 animType = gCurAnimType;

    vec3s_copy(rotation, gVec3sZero);
    vec3f_set(translation, node->translation[0], node->translation[1], node->translation[2]);
    vec3s_copy(rotationInterpolated, rotation);
    vec3f_copy(translationInterpolated, translation);

    anim_process(translationInterpolated, rotationInterpolated, &animType, gPrevAnimFrame, &animAttribute);
    anim_process(translation, rotation, &gCurAnimType, gCurrAnimFrame, &gCurrAnimAttribute);
    interpolate_vectors(translationInterpolated, translationInterpolated, translation);
    interpolate_angles(rotationInterpolated, rotationInterpolated, rotation);

    mtxf_rotate_xyz_and_translate(matrix, translation, rotation);
    mtxf_mul(gMatStack[gMatStackIndex + 1], matrix, gMatStack[gMatStackIndex]);
    mtxf_rotate_xyz_and_translate(matrix, translationInterpolated, rotationInterpolated);
    mtxf_mul(gMatStackInterpolated[gMatStackIndex + 1], matrix, gMatStackInterpolated[gMatStackIndex]);
    gMatStackIndex++;
    mtxf_to_mtx(matrixPtr, gMatStack[gMatStackIndex]);
    gMatStackFixed[gMatStackIndex] = matrixPtr;
    mtxf_to_mtx(mtxInterpolated, gMatStackInterpolated[gMatStackIndex]);
    gMatStackInterpolatedFixed[gMatStackIndex] = mtxInterpolated;
    if (node->displayList != NULL) {
        geo_append_display_list(node->displayList, node->node.flags >> 8);
    }
    if (node->node.children != NULL) {
        geo_process_node_and_siblings(node->node.children);
    }
    gMatStackIndex--;
}

/**
 * Initialize the animation-related global variables for the currently drawn
 * object's animation.
 */
void geo_set_animation_globals(struct GraphNodeObject_sub *node, s32 hasAnimation) {
    struct Animation *anim = node->curAnim;

    if (hasAnimation != 0) {
        node->animFrame = geo_update_animation_frame(node, &node->animFrameAccelAssist);
    }
    node->animTimer = gAreaUpdateCounter;
    if (anim->flags & ANIM_FLAG_HOR_TRANS) {
        gCurAnimType = ANIM_TYPE_VERTICAL_TRANSLATION;
    } else if (anim->flags & ANIM_FLAG_VERT_TRANS) {
        gCurAnimType = ANIM_TYPE_LATERAL_TRANSLATION;
    } else if (anim->flags & ANIM_FLAG_6) {
        gCurAnimType = ANIM_TYPE_NO_TRANSLATION;
    } else {
        gCurAnimType = ANIM_TYPE_TRANSLATION;
    }

    gCurrAnimFrame = node->animFrame;
    if (node->prevAnimPtr == anim && node->prevAnimID == node->animID &&
        gGlobalTimer == node->prevAnimFrameTimestamp + 1) {
        gPrevAnimFrame = node->prevAnimFrame;
    } else {
        gPrevAnimFrame = node->animFrame;
    }
    node->prevAnimPtr = anim;
    node->prevAnimID = node->animID;
    node->prevAnimFrame = node->animFrame;
    node->prevAnimFrameTimestamp = gGlobalTimer;

    gCurAnimEnabled = (anim->flags & ANIM_FLAG_5) == 0;
    gCurrAnimAttribute = segmented_to_virtual((void *) anim->index);
    gCurAnimData = segmented_to_virtual((void *) anim->values);

    if (anim->unk02 == 0) {
        gCurAnimTranslationMultiplier = 1.0f;
    } else {
        gCurAnimTranslationMultiplier = (f32) node->animYTrans / (f32) anim->unk02;
    }
}

/**
 * Process a shadow node. Renders a shadow under an object offset by the
 * translation of the first animated component and rotated according to
 * the floor below it.
 */
static void geo_process_shadow(struct GraphNodeShadow *node) {
    LPROF_BEGIN(lp_sh);
    LPROF_COUNT(LC_NSHD);
    Gfx *shadowList;
    Gfx *shadowListInterpolated;
    Mat4 mtxf;
    Vec3f shadowPos;
    Vec3f shadowPosInterpolated;
    Vec3f animOffset;
    f32 objScale;
    f32 shadowScale;
    f32 sinAng;
    f32 cosAng;
    struct GraphNode *geo;
    Mtx *mtx;
    Mtx *mtxInterpolated;

    if (gCurGraphNodeCamera != NULL && gCurGraphNodeObject != NULL) {
        if (gCurGraphNodeHeldObject != NULL) {
            get_pos_from_transform_mtx(shadowPos, gMatStack[gMatStackIndex],
                                       *gCurGraphNodeCamera->matrixPtr);
            shadowScale = node->shadowScale;
        } else {
            vec3f_copy(shadowPos, gCurGraphNodeObject->pos);
            shadowScale = node->shadowScale * gCurGraphNodeObject->scale[0];
        }

        objScale = 1.0f;
        if (gCurAnimEnabled != 0) {
            if (gCurAnimType == ANIM_TYPE_TRANSLATION
                || gCurAnimType == ANIM_TYPE_LATERAL_TRANSLATION) {
                geo = node->node.children;
                if (geo != NULL && geo->type == GRAPH_NODE_TYPE_SCALE) {
                    objScale = ((struct GraphNodeScale *) geo)->scale;
                }
                animOffset[0] =
                    gCurAnimData[retrieve_animation_index(gCurrAnimFrame, &gCurrAnimAttribute)]
                    * gCurAnimTranslationMultiplier * objScale;
                animOffset[1] = 0.0f;
                gCurrAnimAttribute += 2;
                animOffset[2] =
                    gCurAnimData[retrieve_animation_index(gCurrAnimFrame, &gCurrAnimAttribute)]
                    * gCurAnimTranslationMultiplier * objScale;
                gCurrAnimAttribute -= 6;

                // simple matrix rotation so the shadow offset rotates along with the object
                sinAng = sins(gCurGraphNodeObject->angle[1]);
                cosAng = coss(gCurGraphNodeObject->angle[1]);

                shadowPos[0] += animOffset[0] * cosAng + animOffset[2] * sinAng;
                shadowPos[2] += -animOffset[0] * sinAng + animOffset[2] * cosAng;
            }
        }

        if (gCurGraphNodeHeldObject != NULL) {
            if (gGlobalTimer == gCurGraphNodeHeldObject->prevShadowPosTimestamp + 1) {
                interpolate_vectors(shadowPosInterpolated, gCurGraphNodeHeldObject->prevShadowPos, shadowPos);
            } else {
                vec3f_copy(shadowPosInterpolated, shadowPos);
            }
            vec3f_copy(gCurGraphNodeHeldObject->prevShadowPos, shadowPos);
            gCurGraphNodeHeldObject->prevShadowPosTimestamp = gGlobalTimer;
        } else {
            if (gGlobalTimer == gCurGraphNodeObject->prevShadowPosTimestamp + 1 &&
                gGlobalTimer != gCurGraphNodeObject->skipInterpolationTimestamp) {
                interpolate_vectors(shadowPosInterpolated, gCurGraphNodeObject->prevShadowPos, shadowPos);
            } else {
                vec3f_copy(shadowPosInterpolated, shadowPos);
            }
            vec3f_copy(gCurGraphNodeObject->prevShadowPos, shadowPos);
            gCurGraphNodeObject->prevShadowPosTimestamp = gGlobalTimer;
        }

        extern u8 gInterpolatingSurfaces;
#ifdef TARGET_PSP
        extern int gPspSixtyMode;
        /* Objeto quieto (la posicion interpolada es IGUAL a la real) sobre un suelo que no se mueve (no es una plataforma de objeto): la sombra
         * del cuadro interpolado es la misma que la del real, no hace falta calcularla otra vez. Es exacto, no una aproximacion. */
        int psp_same = 0;
        if (gCurGraphNodeHeldObject == NULL && shadowPosInterpolated[0] == shadowPos[0] && shadowPosInterpolated[1] == shadowPos[1]
            && shadowPosInterpolated[2] == shadowPos[2]) {
            const struct Surface *fl = ((struct Object *) gCurGraphNodeObject)->oFloor;
            psp_same = (fl != NULL && fl->object == NULL);
        }
        /* Con Object Distance > 100 % hay sombras de objetos MUY lejanos (que en la N64 ni existian: pasan de 4000 unidades). Cada sombra cuesta ~60 us
         * (busca suelo y arma vertices) y a mas de 5500 unidades mide unos pocos pixeles: esas no se calculan. Con 100 % no cambia nada. */
        const int psp_far = (gPspObjDistScale > 1.0f && !psp_near_mario(shadowPos, 5500.0f)) ? 1 : 0;
        if (gPspSixtyMode && !psp_same && !psp_far) {
#endif
        gInterpolatingSurfaces = TRUE;
        shadowListInterpolated = create_shadow_below_xyz(shadowPosInterpolated[0], shadowPosInterpolated[1],
                                                         shadowPosInterpolated[2], shadowScale,
                                                         node->shadowSolidity, node->shadowType);
        gInterpolatingSurfaces = FALSE;
#ifdef TARGET_PSP
        }
#endif
#ifdef TARGET_PSP
        shadowList = psp_far ? NULL : create_shadow_below_xyz(shadowPos[0], shadowPos[1], shadowPos[2], shadowScale,
                                                              node->shadowSolidity, node->shadowType);
#else
        shadowList = create_shadow_below_xyz(shadowPos[0], shadowPos[1], shadowPos[2], shadowScale,
                                             node->shadowSolidity, node->shadowType);
#endif
#ifdef TARGET_PSP
        if (psp_far) {
            shadowListInterpolated = NULL;
        } else if (!gPspSixtyMode || psp_same) {
            /* tick de 30 fps: no se dibuja el cuadro interpolado (patch_interpolations() deja las listas reales), asi que no se
             * calcula la sombra interpolada (ahorra buscar suelo y construir los vertices otra vez por cada sombra) */
            shadowListInterpolated = shadowList;
        }
#endif
        if (shadowListInterpolated != NULL && shadowList != NULL) {
            mtx = alloc_display_list(sizeof(*mtx));
            mtxInterpolated = alloc_display_list(sizeof(*mtxInterpolated));
            gMatStackIndex++;

            mtxf_translate(mtxf, shadowPos);
            mtxf_mul(gMatStack[gMatStackIndex], mtxf, *gCurGraphNodeCamera->matrixPtr);
            mtxf_to_mtx(mtx, gMatStack[gMatStackIndex]);
            gMatStackFixed[gMatStackIndex] = mtx;

            mtxf_translate(mtxf, shadowPosInterpolated);
            mtxf_mul(gMatStackInterpolated[gMatStackIndex], mtxf, *gCurGraphNodeCamera->matrixPtrInterpolated);
            mtxf_to_mtx(mtxInterpolated, gMatStackInterpolated[gMatStackIndex]);
            gMatStackInterpolatedFixed[gMatStackIndex] = mtxInterpolated;

            if (gShadowAboveWaterOrLava == 1) {
                geo_append_display_list2((void *) VIRTUAL_TO_PHYSICAL(shadowList),
                                         (void *) VIRTUAL_TO_PHYSICAL(shadowListInterpolated), 4);
            } else if (gMarioOnIceOrCarpet == 1) {
                geo_append_display_list2((void *) VIRTUAL_TO_PHYSICAL(shadowList),
                                         (void *) VIRTUAL_TO_PHYSICAL(shadowListInterpolated), 5);
            } else {
                geo_append_display_list2((void *) VIRTUAL_TO_PHYSICAL(shadowList),
                                         (void *) VIRTUAL_TO_PHYSICAL(shadowListInterpolated), 6);
            }
#ifdef TARGET_PSP
            sNextReplayMtx = NULL;
#endif
            gMatStackIndex--;
        }
    }
    LPROF_END(lp_sh, LP_SHD);
    if (node->node.children != NULL) {
        geo_process_node_and_siblings(node->node.children);
    }
}

/**
 * Check whether an object is in view to determine whether it should be drawn.
 * This is known as frustum culling.
 * It checks whether the object is far away, very close / behind the camera,
 * or horizontally out of view. It does not check whether it is vertically
 * out of view. It assumes a sphere of 300 units around the object's position
 * unless the object has a culling radius node that specifies otherwise.
 *
 * The matrix parameter should be the top of the matrix stack, which is the
 * object's transformation matrix times the camera 'look-at' matrix. The math
 * is counter-intuitive, but it checks column 3 (translation vector) of this
 * matrix to determine where the origin (0,0,0) in object space will be once
 * transformed to camera space (x+ = right, y+ = up, z = 'coming out the screen').
 * In 3D graphics, you typically model the world as being moved in front of a
 * static camera instead of a moving camera through a static world, which in
 * this case simplifies calculations. Note that the perspective matrix is not
 * on the matrix stack, so there are still calculations with the fov to compute
 * the slope of the lines of the frustum.
 *
 *        z-
 *
 *  \     |     /
 *   \    |    /
 *    \   |   /
 *     \  |  /
 *      \ | /
 *       \|/
 *        C       x+
 *
 * Since (0,0,0) is unaffected by rotation, columns 0, 1 and 2 are ignored.
 */
static int obj_is_in_view(struct GraphNodeObject *node, Mat4 matrix) {
    s16 cullingRadius;
    s16 halfFov; // half of the fov in in-game angle units instead of degrees
    struct GraphNode *geo;
    f32 hScreenEdge;

    if (node->node.flags & GRAPH_RENDER_INVISIBLE) {
        return FALSE;
    }

    geo = node->sharedChild;

    // ! @bug The aspect ratio is not accounted for. When the fov value is 45,
    // the horizontal effective fov is actually 60 degrees, so you can see objects
    // visibly pop in or out at the edge of the screen.
    halfFov = (gCurGraphNodeCamFrustum->fov / 2.0f + 1.0f) * 32768.0f / 180.0f + 0.5f;

    hScreenEdge = -matrix[3][2] * sins(halfFov) / coss(halfFov);
    // -matrix[3][2] is the depth, which gets multiplied by tan(halfFov) to get
    // the amount of units between the center of the screen and the horizontal edge
    // given the distance from the object to the camera.

#ifdef TARGET_PSP
    const f32 vScreenEdge = hScreenEdge; // mitad de la altura visible a esta profundidad (sin el aspecto)
#endif
#ifdef WIDESCREEN
    // This multiplication should really be performed on 4:3 as well,
    // but the issue will be more apparent on widescreen.
    hScreenEdge *= GFX_DIMENSIONS_ASPECT_RATIO;
#endif

    if (geo != NULL && geo->type == GRAPH_NODE_TYPE_CULLING_RADIUS) {
        cullingRadius =
            (f32)((struct GraphNodeCullingRadius *) geo)->cullingRadius; //! Why is there a f32 cast?
    } else {
        cullingRadius = 300;
    }

#ifdef TARGET_PSP
    const int psp_near = psp_near_mario(node->pos, 3000.0f); /* cerca de Mario: culling original */
#endif
    // Don't render if the object is close to or behind the camera
    if (matrix[3][2] > -100.0f + cullingRadius) {
        return FALSE;
    }

    //! This makes the HOLP not update when the camera is far away, and it
    //  makes PU travel safe when the camera is locked on the main map.
    //  If Mario were rendered with a depth over 65536 it would cause overflow
    //  when converting the transformation matrix to a fixed point matrix.
#ifdef TARGET_PSP
    /* con "Far Distance" > 100 % el limite se alarga igual (150 % = 30000, 300 % = 60000: sigue bajo 65536) */
    if (matrix[3][2] < -20000.0f * (gMenuMode == -1 ? gPspFarScale : 1.0f) - cullingRadius) {
        return FALSE;
    }
#else
    if (matrix[3][2] < -20000.0f - cullingRadius) {
        return FALSE;
    }
#endif

#ifdef TARGET_PSP
    {
        /* Mas alla del plano lejano de la camara no se dibuja nada (el GE lo recorta): ni se procesa el objeto. Margen de sobra de
         * 1000 unidades por si el radio de culling del objeto se quedo corto. */
        const f32 farLimit = gCurGraphNodeCamFrustum->far * (gMenuMode == -1 ? gPspFarScale : 1.0f) + cullingRadius + 1000.0f;
        if (-matrix[3][2] > farLimit && !psp_near) {
            return FALSE;
        }
    }
#endif

    // Check whether the object is horizontally in view
    if (matrix[3][0] > hScreenEdge + cullingRadius) {
        return FALSE;
    }
    if (matrix[3][0] < -hScreenEdge - cullingRadius) {
        return FALSE;
    }

#ifdef TARGET_PSP
    if (gPspOptCullObj && !psp_near) {
        /* Menu "Object Culling: Vertical". El juego original solo descarta por los lados (por eso los niveles verticales, como Tall Tall
         * Mountain, dibujan todo lo que esta arriba y abajo de la pantalla). vScreenEdge es justo la mitad de la altura visible a esa
         * profundidad (el fov del juego es vertical). Los objetos pueden ser mas altos que su radio
         * (el origen suele estar en los pies), asi que el margen es generoso: 2 * radio + 400 unidades. */
        const f32 vEdge = vScreenEdge + 2.0f * cullingRadius + 400.0f;
        if (matrix[3][1] > vEdge || matrix[3][1] < -vEdge) {
            return FALSE;
        }
    }
#endif
    return TRUE;
}


static void interpolate_matrix(Mat4 result, Mat4 a, Mat4 b) {
    s32 i, j;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            result[i][j] = (a[i][j] + b[i][j]) / 2.0f;
        }
    }
}

/**
 * Process an object node.
 */
static void geo_process_object(struct Object *node) {
    LPROF_COUNT(LC_GEOOBJ);
    Mat4 mtxf;
    s32 hasAnimation = (node->header.gfx.node.flags & GRAPH_RENDER_HAS_ANIMATION) != 0;
    Vec3f scaleInterpolated;

    if (node->header.gfx.unk18 == gCurGraphNodeRoot->areaIndex) {
        if (node->header.gfx.throwMatrix != NULL) {
            mtxf_mul(gMatStack[gMatStackIndex + 1], *node->header.gfx.throwMatrix,
                     gMatStack[gMatStackIndex]);
            if (gGlobalTimer == node->header.gfx.prevThrowMatrixTimestamp + 1 &&
                gGlobalTimer != node->header.gfx.skipInterpolationTimestamp) {
                interpolate_matrix(mtxf, *node->header.gfx.throwMatrix, node->header.gfx.prevThrowMatrix);
                mtxf_mul(gMatStackInterpolated[gMatStackIndex + 1], mtxf,
                     gMatStackInterpolated[gMatStackIndex]);
            } else {
                mtxf_mul(gMatStackInterpolated[gMatStackIndex + 1], (void *) node->header.gfx.throwMatrix,
                         gMatStackInterpolated[gMatStackIndex]);
            }
            {
                /* copia escalar: no depende de la alineacion de 16 bytes que exigen las rutinas VFPU */
                s32 mi, mj;
                for (mi = 0; mi < 4; mi++) {
                    for (mj = 0; mj < 4; mj++) {
                        node->header.gfx.prevThrowMatrix[mi][mj] = (*node->header.gfx.throwMatrix)[mi][mj];
                    }
                }
            }
            node->header.gfx.prevThrowMatrixTimestamp = gGlobalTimer;
        } else if (node->header.gfx.node.flags & GRAPH_RENDER_BILLBOARD) {
            Vec3f posInterpolated;
            if (gGlobalTimer == node->header.gfx.prevTimestamp + 1 &&
                gGlobalTimer != node->header.gfx.skipInterpolationTimestamp) {
                interpolate_vectors(posInterpolated, node->header.gfx.prevPos, node->header.gfx.pos);
            } else {
                vec3f_copy(posInterpolated, node->header.gfx.pos);
            }
            vec3f_copy(node->header.gfx.prevPos, node->header.gfx.pos);
            node->header.gfx.prevTimestamp = gGlobalTimer;
            mtxf_billboard(gMatStack[gMatStackIndex + 1], gMatStack[gMatStackIndex],
                           node->header.gfx.pos, gCurGraphNodeCamera->roll);
            mtxf_billboard(gMatStackInterpolated[gMatStackIndex + 1], gMatStackInterpolated[gMatStackIndex],
                           posInterpolated, gCurGraphNodeCamera->roll);
#ifdef TARGET_PSP
            if (PSP_BB_PULL_K > 0.0f && gLoadedGraphNodes != NULL && node->header.gfx.sharedChild != NULL
                && node->header.gfx.sharedChild == gLoadedGraphNodes[MODEL_1UP]) {
                psp_billboard_pull(gMatStack[gMatStackIndex + 1]);
                psp_billboard_pull(gMatStackInterpolated[gMatStackIndex + 1]);
            }
#endif
        } else {
            Vec3f posInterpolated;
            Vec3s angleInterpolated;
            if (gGlobalTimer == node->header.gfx.prevTimestamp + 1 &&
                gGlobalTimer != node->header.gfx.skipInterpolationTimestamp) {
                interpolate_vectors(posInterpolated, node->header.gfx.prevPos, node->header.gfx.pos);
                interpolate_angles(angleInterpolated, node->header.gfx.prevAngle, node->header.gfx.angle);
            } else {
                vec3f_copy(posInterpolated, node->header.gfx.pos);
                vec3s_copy(angleInterpolated, node->header.gfx.angle);
            }
            vec3f_copy(node->header.gfx.prevPos, node->header.gfx.pos);
            vec3s_copy(node->header.gfx.prevAngle, node->header.gfx.angle);
            node->header.gfx.prevTimestamp = gGlobalTimer;
            mtxf_rotate_zxy_and_translate(mtxf, node->header.gfx.pos, node->header.gfx.angle);
            mtxf_mul(gMatStack[gMatStackIndex + 1], mtxf, gMatStack[gMatStackIndex]);
            mtxf_rotate_zxy_and_translate(mtxf, posInterpolated, angleInterpolated);
            mtxf_mul(gMatStackInterpolated[gMatStackIndex + 1], mtxf, gMatStackInterpolated[gMatStackIndex]);
        }

        if (gGlobalTimer == node->header.gfx.prevScaleTimestamp + 1 &&
            gGlobalTimer != node->header.gfx.skipInterpolationTimestamp) {
            interpolate_vectors(scaleInterpolated, node->header.gfx.prevScale, node->header.gfx.scale);
        } else {
            vec3f_copy(scaleInterpolated, node->header.gfx.scale);
        }
        vec3f_copy(node->header.gfx.prevScale, node->header.gfx.scale);
        node->header.gfx.prevScaleTimestamp = gGlobalTimer;

        mtxf_scale_vec3f(gMatStack[gMatStackIndex + 1], gMatStack[gMatStackIndex + 1],
                         node->header.gfx.scale);
        mtxf_scale_vec3f(gMatStackInterpolated[gMatStackIndex + 1], gMatStackInterpolated[gMatStackIndex + 1],
                         scaleInterpolated);
        node->header.gfx.throwMatrix = &gMatStack[++gMatStackIndex];
        node->header.gfx.throwMatrixInterpolated = &gMatStackInterpolated[gMatStackIndex];
        node->header.gfx.cameraToObject[0] = gMatStack[gMatStackIndex][3][0];
        node->header.gfx.cameraToObject[1] = gMatStack[gMatStackIndex][3][1];
        node->header.gfx.cameraToObject[2] = gMatStack[gMatStackIndex][3][2];

        // FIXME: correct types
        if (node->header.gfx.unk38.curAnim != NULL) {
            LPROF_BEGIN(lp_an);
            geo_set_animation_globals(&node->header.gfx.unk38, hasAnimation);
            LPROF_END(lp_an, LP_ANI);
        }
        LPROF_BEGIN(lp_vw);
        const int lp_in_view = obj_is_in_view(&node->header.gfx, gMatStack[gMatStackIndex]);
        LPROF_END(lp_vw, LP_VIEW);
        if (lp_in_view) {
            Mtx *mtx = alloc_display_list(sizeof(*mtx));
            Mtx *mtxInterpolated = alloc_display_list(sizeof(*mtxInterpolated));

            mtxf_to_mtx(mtx, gMatStack[gMatStackIndex]);
            gMatStackFixed[gMatStackIndex] = mtx;
            mtxf_to_mtx(mtxInterpolated, gMatStackInterpolated[gMatStackIndex]);
            gMatStackInterpolatedFixed[gMatStackIndex] = mtxInterpolated;
            if (node->header.gfx.sharedChild != NULL) {
                gCurGraphNodeObject = (struct GraphNodeObject *) node;
                node->header.gfx.sharedChild->parent = &node->header.gfx.node;
                geo_process_node_and_siblings(node->header.gfx.sharedChild);
                node->header.gfx.sharedChild->parent = NULL;
                gCurGraphNodeObject = NULL;
            }
            if (node->header.gfx.node.children != NULL) {
                geo_process_node_and_siblings(node->header.gfx.node.children);
            }
        } else {
            node->header.gfx.prevThrowMatrixTimestamp = 0;
            node->header.gfx.prevTimestamp = 0;
            node->header.gfx.prevScaleTimestamp = 0;
        }

        gMatStackIndex--;
        gCurAnimType = ANIM_TYPE_NONE;
        node->header.gfx.throwMatrix = NULL;
        node->header.gfx.throwMatrixInterpolated = NULL;
    }
}

/**
 * Process an object parent node. Temporarily assigns itself as the parent of
 * the subtree rooted at 'sharedChild' and processes the subtree, after which the
 * actual children are be processed. (in practice they are null though)
 */
static void geo_process_object_parent(struct GraphNodeObjectParent *node) {
    if (node->sharedChild != NULL) {
        node->sharedChild->parent = (struct GraphNode *) node;
        geo_process_node_and_siblings(node->sharedChild);
        node->sharedChild->parent = NULL;
    }
    if (node->node.children != NULL) {
        geo_process_node_and_siblings(node->node.children);
    }
}

/**
 * Process a held object node.
 */
void geo_process_held_object(struct GraphNodeHeldObject *node) {
    Mat4 mat;
    Vec3f translation;
    Mtx *mtx = alloc_display_list(sizeof(*mtx));
    Mtx *mtxInterpolated = alloc_display_list(sizeof(*mtxInterpolated));
    Vec3f scaleInterpolated;

#ifdef F3DEX_GBI_2
    gSPLookAt(gDisplayListHead++, &lookAt);
#endif

    if (node->fnNode.func != NULL) {
        {
            LPROF_BEGIN(lp_f);
            node->fnNode.func(GEO_CONTEXT_RENDER, &node->fnNode.node, gMatStack[gMatStackIndex]);
            LPROF_END(lp_f, LP_FN);
            LPROF_COUNT(LC_NFN);
        }
    }
    if (node->objNode != NULL && node->objNode->header.gfx.sharedChild != NULL) {
        s32 hasAnimation = (node->objNode->header.gfx.node.flags & GRAPH_RENDER_HAS_ANIMATION) != 0;

        translation[0] = node->translation[0] / 4.0f;
        translation[1] = node->translation[1] / 4.0f;
        translation[2] = node->translation[2] / 4.0f;

        if (gGlobalTimer == node->objNode->header.gfx.prevScaleTimestamp + 1) {
            interpolate_vectors(scaleInterpolated, node->objNode->header.gfx.prevScale, node->objNode->header.gfx.scale);
        } else {
            vec3f_copy(scaleInterpolated, node->objNode->header.gfx.scale);
        }
        vec3f_copy(node->objNode->header.gfx.prevScale, node->objNode->header.gfx.scale);
        node->objNode->header.gfx.prevScaleTimestamp = gGlobalTimer;

        mtxf_translate(mat, translation);
        mtxf_copy(gMatStack[gMatStackIndex + 1], *gCurGraphNodeObject->throwMatrix);
        gMatStack[gMatStackIndex + 1][3][0] = gMatStack[gMatStackIndex][3][0];
        gMatStack[gMatStackIndex + 1][3][1] = gMatStack[gMatStackIndex][3][1];
        gMatStack[gMatStackIndex + 1][3][2] = gMatStack[gMatStackIndex][3][2];
        mtxf_mul(gMatStack[gMatStackIndex + 1], mat, gMatStack[gMatStackIndex + 1]);
        mtxf_scale_vec3f(gMatStack[gMatStackIndex + 1], gMatStack[gMatStackIndex + 1],
                         node->objNode->header.gfx.scale);
        mtxf_copy(gMatStackInterpolated[gMatStackIndex + 1], (void *) gCurGraphNodeObject->throwMatrixInterpolated);
        gMatStackInterpolated[gMatStackIndex + 1][3][0] = gMatStackInterpolated[gMatStackIndex][3][0];
        gMatStackInterpolated[gMatStackIndex + 1][3][1] = gMatStackInterpolated[gMatStackIndex][3][1];
        gMatStackInterpolated[gMatStackIndex + 1][3][2] = gMatStackInterpolated[gMatStackIndex][3][2];
        mtxf_mul(gMatStackInterpolated[gMatStackIndex + 1], mat, gMatStackInterpolated[gMatStackIndex + 1]);
        mtxf_scale_vec3f(gMatStackInterpolated[gMatStackIndex + 1], gMatStackInterpolated[gMatStackIndex + 1],
                         scaleInterpolated);
        if (node->fnNode.func != NULL) {
            node->fnNode.func(GEO_CONTEXT_HELD_OBJ, &node->fnNode.node,
                              (struct AllocOnlyPool *) gMatStack[gMatStackIndex + 1]);
        }
        gMatStackIndex++;
        mtxf_to_mtx(mtx, gMatStack[gMatStackIndex]);
        gMatStackFixed[gMatStackIndex] = mtx;
        mtxf_to_mtx(mtxInterpolated, gMatStackInterpolated[gMatStackIndex]);
        gMatStackInterpolatedFixed[gMatStackIndex] = mtxInterpolated;
        gGeoTempState.type = gCurAnimType;
        gGeoTempState.enabled = gCurAnimEnabled;
        gGeoTempState.frame = gCurrAnimFrame;
        gGeoTempState.translationMultiplier = gCurAnimTranslationMultiplier;
        gGeoTempState.attribute = gCurrAnimAttribute;
        gGeoTempState.data = gCurAnimData;
        gGeoTempState.prevFrame = gPrevAnimFrame;
        gCurAnimType = 0;
        gCurGraphNodeHeldObject = (void *) node;
        if (node->objNode->header.gfx.unk38.curAnim != NULL) {
            LPROF_BEGIN(lp_an);
            geo_set_animation_globals(&node->objNode->header.gfx.unk38, hasAnimation);
            LPROF_END(lp_an, LP_ANI);
        }

        geo_process_node_and_siblings(node->objNode->header.gfx.sharedChild);
        gCurGraphNodeHeldObject = NULL;
        gCurAnimType = gGeoTempState.type;
        gCurAnimEnabled = gGeoTempState.enabled;
        gCurrAnimFrame = gGeoTempState.frame;
        gCurAnimTranslationMultiplier = gGeoTempState.translationMultiplier;
        gCurrAnimAttribute = gGeoTempState.attribute;
        gCurAnimData = gGeoTempState.data;
        gPrevAnimFrame = gGeoTempState.prevFrame;
        gMatStackIndex--;
    }

    if (node->fnNode.node.children != NULL) {
        geo_process_node_and_siblings(node->fnNode.node.children);
    }
}

/**
 * Processes the children of the given GraphNode if it has any
 */
void geo_try_process_children(struct GraphNode *node) {
    if (node->children != NULL) {
        geo_process_node_and_siblings(node->children);
    }
}

/**
 * Process a generic geo node and its siblings.
 * The first argument is the start node, and all its siblings will
 * be iterated over.
 */
void geo_process_node_and_siblings(struct GraphNode *firstNode) {
    LPROF_COUNT(LC_GEONODE);
    s16 iterateChildren = TRUE;
    struct GraphNode *curGraphNode = firstNode;
    struct GraphNode *parent = curGraphNode->parent;

    // In the case of a switch node, exactly one of the children of the node is
    // processed instead of all children like usual
    if (parent != NULL) {
        iterateChildren = (parent->type != GRAPH_NODE_TYPE_SWITCH_CASE);
    }

    do {
        if (curGraphNode->flags & GRAPH_RENDER_ACTIVE) {
            if (curGraphNode->flags & GRAPH_RENDER_CHILDREN_FIRST) {
                geo_try_process_children(curGraphNode);
            } else {
                switch (curGraphNode->type) {
                    case GRAPH_NODE_TYPE_ORTHO_PROJECTION:
                        geo_process_ortho_projection((struct GraphNodeOrthoProjection *) curGraphNode);
                        break;
                    case GRAPH_NODE_TYPE_PERSPECTIVE:
                        geo_process_perspective((struct GraphNodePerspective *) curGraphNode);
                        break;
                    case GRAPH_NODE_TYPE_MASTER_LIST:
                        geo_process_master_list((struct GraphNodeMasterList *) curGraphNode);
                        break;
                    case GRAPH_NODE_TYPE_LEVEL_OF_DETAIL:
                        geo_process_level_of_detail((struct GraphNodeLevelOfDetail *) curGraphNode);
                        break;
                    case GRAPH_NODE_TYPE_SWITCH_CASE:
                        geo_process_switch((struct GraphNodeSwitchCase *) curGraphNode);
                        break;
                    case GRAPH_NODE_TYPE_CAMERA:
                        geo_process_camera((struct GraphNodeCamera *) curGraphNode);
                        break;
                    case GRAPH_NODE_TYPE_TRANSLATION_ROTATION:
                        LPROF_COUNT(LC_NMTX);
                        geo_process_translation_rotation(
                            (struct GraphNodeTranslationRotation *) curGraphNode);
                        break;
                    case GRAPH_NODE_TYPE_TRANSLATION:
                        LPROF_COUNT(LC_NMTX);
                        geo_process_translation((struct GraphNodeTranslation *) curGraphNode);
                        break;
                    case GRAPH_NODE_TYPE_ROTATION:
                        LPROF_COUNT(LC_NMTX);
                        geo_process_rotation((struct GraphNodeRotation *) curGraphNode);
                        break;
                    case GRAPH_NODE_TYPE_OBJECT:
                        geo_process_object((struct Object *) curGraphNode);
                        break;
                    case GRAPH_NODE_TYPE_ANIMATED_PART:
                        LPROF_COUNT(LC_NANIM);
                        geo_process_animated_part((struct GraphNodeAnimatedPart *) curGraphNode);
                        break;
                    case GRAPH_NODE_TYPE_BILLBOARD:
                        LPROF_COUNT(LC_NMTX);
                        geo_process_billboard((struct GraphNodeBillboard *) curGraphNode);
                        break;
                    case GRAPH_NODE_TYPE_DISPLAY_LIST:
                        LPROF_COUNT(LC_NDL);
                        geo_process_display_list((struct GraphNodeDisplayList *) curGraphNode);
                        break;
                    case GRAPH_NODE_TYPE_SCALE:
                        LPROF_COUNT(LC_NMTX);
                        geo_process_scale((struct GraphNodeScale *) curGraphNode);
                        break;
                    case GRAPH_NODE_TYPE_SHADOW:
                        geo_process_shadow((struct GraphNodeShadow *) curGraphNode);
                        break;
                    case GRAPH_NODE_TYPE_OBJECT_PARENT:
                        geo_process_object_parent((struct GraphNodeObjectParent *) curGraphNode);
                        break;
                    case GRAPH_NODE_TYPE_GENERATED_LIST:
                        geo_process_generated_list((struct GraphNodeGenerated *) curGraphNode);
                        break;
                    case GRAPH_NODE_TYPE_BACKGROUND:
                        geo_process_background((struct GraphNodeBackground *) curGraphNode);
                        break;
                    case GRAPH_NODE_TYPE_HELD_OBJ:
                        geo_process_held_object((struct GraphNodeHeldObject *) curGraphNode);
                        break;
                    default:
                        geo_try_process_children((struct GraphNode *) curGraphNode);
                        break;
                }
            }
        } else {
            if (curGraphNode->type == GRAPH_NODE_TYPE_OBJECT) {
                ((struct GraphNodeObject *) curGraphNode)->throwMatrix = NULL;
            }
        }
    } while (iterateChildren && (curGraphNode = curGraphNode->next) != firstNode);
}

/**
 * Process a root node. This is the entry point for processing the scene graph.
 * The root node itself sets up the viewport, then all its children are processed
 * to set up the projection and draw display lists.
 */
void geo_process_root(struct GraphNodeRoot *node, Vp *b, Vp *c, s32 clearColor) {
    UNUSED s32 unused;

    if (node->node.flags & GRAPH_RENDER_ACTIVE) {
        Mtx *initialMatrix;
        Vp *viewport = alloc_display_list(sizeof(*viewport));
        Vp *viewportInterpolated = viewport;

        gDisplayListHeap = alloc_only_pool_init(main_pool_available() - sizeof(struct AllocOnlyPool),
                                                MEMORY_POOL_LEFT);
        initialMatrix = alloc_display_list(sizeof(*initialMatrix));
        gMatStackIndex = 0;
        gCurAnimType = 0;
        vec3s_set(viewport->vp.vtrans, node->x * 4, node->y * 4, 511);
        vec3s_set(viewport->vp.vscale, node->width * 4, node->height * 4, 511);
        if (b != NULL) {
            clear_frame_buffer(clearColor);
            viewportInterpolated = alloc_display_list(sizeof(*viewportInterpolated));
            interpolate_vectors_s16(viewportInterpolated->vp.vtrans, sPrevViewport.vp.vtrans, b->vp.vtrans);
            interpolate_vectors_s16(viewportInterpolated->vp.vscale, sPrevViewport.vp.vscale, b->vp.vscale);

            sViewportPos = gDisplayListHead;
            sViewportDiffers = (memcmp(viewportInterpolated, b, sizeof(Vp)) != 0);
            make_viewport_clip_rect(viewportInterpolated);
            *viewport = *b;
        }

        else if (c != NULL) {
            clear_frame_buffer(clearColor);
            make_viewport_clip_rect(c);
        }
        sPrevViewport = *viewport;

        mtxf_identity(gMatStack[gMatStackIndex]);
        mtxf_to_mtx(initialMatrix, gMatStack[gMatStackIndex]);
        gMatStackFixed[gMatStackIndex] = initialMatrix;

        mtxf_identity(gMatStackInterpolated[gMatStackIndex]);
        gMatStackInterpolatedFixed[gMatStackIndex] = initialMatrix;

        gSPViewport(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(viewportInterpolated));
        gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(gMatStackFixed[gMatStackIndex]),
                  G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
        gCurGraphNodeRoot = node;
        if (node->node.children != NULL) {
            geo_process_node_and_siblings(node->node.children);
        }
        gCurGraphNodeRoot = NULL;
        if (gShowDebugText) {
            print_text_fmt_int(180, 36, "MEM %d",
                               gDisplayListHeap->totalSpace - gDisplayListHeap->usedSpace);
        }
        main_pool_free(gDisplayListHeap);
    }
}
