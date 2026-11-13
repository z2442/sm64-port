#include <PR/ultratypes.h>

#include "audio/external.h"
#include "engine/geo_layout.h"
#include "engine/graph_node.h"
#include "engine/math_util.h"
#include "engine/surface_collision.h"
#include "level_table.h"
#include "object_constants.h"
#include "object_fields.h"
#include "object_helpers.h"
#include "object_list_processor.h"
#include "spawn_object.h"
#include "types.h"

/**
 * An unused linked list struct that seems to have been replaced by ObjectNode.
 */
struct LinkedList {
    struct LinkedList *next;
    struct LinkedList *prev;
};

/**
 * Clear the doubly linked usedList. Singly link each item in the pool into
 * a list, and return this list in pFreeList.
 * Appears to have been replaced by init_free_object_list.
 */
void unused_init_free_list(struct LinkedList *usedList, struct LinkedList **pFreeList,
                           struct LinkedList *pool, s32 itemSize, s32 poolLength) {
    s32 i;
    struct LinkedList *node = pool;

    usedList->next = usedList;
    usedList->prev = usedList;

    *pFreeList = pool;

    for (i = 0; i < poolLength - 1; i++) {
        // Add next node to free list
        node = (struct LinkedList *) ((u8 *) node + itemSize);
        pool->next = node;
        pool = node;
    }

    // End the list
    pool->next = NULL;
}

/**
 * Attempt to allocate a node from freeList (singly linked) and append it
 * to the end of destList (doubly linked). Return the object, or NULL if
 * freeList is empty.
 * Appears to have been replaced by try_allocate_object.
 */
struct LinkedList *unused_try_allocate(struct LinkedList *destList,
                                       struct LinkedList *freeList) {
    struct LinkedList *node = freeList->next;

    if (node != NULL) {
        // Remove from free list
        freeList->next = node->next;

        // Insert at the end of destination list
        node->prev = destList->prev;
        node->next = destList;
        destList->prev->next = node;
        destList->prev = node;
    }

    return node;
}

/**
 * Attempt to allocate an object from freeList (singly linked) and append it
 * to the end of destList (doubly linked). Return the object, or NULL if
 * freeList is empty.
 */
struct Object *try_allocate_object(struct ObjectNode *destList, struct ObjectNode *freeList) {
    struct ObjectNode *nextObj;

    if ((nextObj = freeList->next) != NULL) {
        // Remove from free list
        freeList->next = nextObj->next;

        // Insert at end of destination list
        nextObj->prev = destList->prev;
        nextObj->next = destList;
        destList->prev->next = nextObj;
        destList->prev = nextObj;
    } else {
        return NULL;
    }

    geo_remove_child(&nextObj->gfx.node);
    geo_add_child(&gObjParentGraphNode, &nextObj->gfx.node);

    return (struct Object *) nextObj;
}

/**
 * Remove the node from the doubly linked list it's in, and place it in the
 * singly linked freeList.
 * This function seems to have been replaced by deallocate_object.
 */
void unused_deallocate(struct LinkedList *freeList, struct LinkedList *node) {
    // Remove from doubly linked list
    node->next->prev = node->prev;
    node->prev->next = node->next;

    // Insert at beginning of singly linked list
    node->next = freeList->next;
    freeList->next = node;
}
/**
 * Remove the given object from the object list that it's currently in, and
 * insert it at the beginning of the free list (singly linked).
 */
static void deallocate_object(struct ObjectNode *freeList, struct ObjectNode *obj) {
    // Remove from object list
    obj->next->prev = obj->prev;
    obj->prev->next = obj->next;

    // Insert at beginning of free list
    obj->next = freeList->next;
    freeList->next = obj;
}

/**
 * Add every object in the pool to the free object list.
 */
void init_free_object_list(void) {
    s32 i;
    s32 poolLength = OBJECT_POOL_CAPACITY;

    // Add the first object in the pool to the free list
    struct Object *obj = &gObjectPool[0];
    gFreeObjectList.next = (struct ObjectNode *) obj;

    // Link each object in the pool to the following object
    for (i = 0; i < poolLength - 1; i++) {
        obj->header.next = &(obj + 1)->header;
        obj++;
    }

    // End the list
    obj->header.next = NULL;
}

/**
 * Clear each object list, without adding the objects back to the free list.
 */
void clear_object_lists(struct ObjectNode *objLists) {
    s32 i;

    for (i = 0; i < NUM_OBJ_LISTS; i++) {
        objLists[i].next = &objLists[i];
        objLists[i].prev = &objLists[i];
    }
}

/**
 * This function looks broken, but it appears to attempt to delete the leaf
 * graph nodes under obj and obj's siblings.
 */
static void unused_delete_leaf_nodes(struct Object *obj) {
    struct Object *children;
    struct Object *sibling;
    struct Object *obj0 = obj;

    if ((children = (struct Object *) obj->header.gfx.node.children) != NULL) {
        unused_delete_leaf_nodes(children);
    } else {
        // No children
        mark_obj_for_deletion(obj);
    }

    // Probably meant to be !=
    while ((sibling = (struct Object *) obj->header.gfx.node.next) == obj0) {
        unused_delete_leaf_nodes(sibling);
        obj = (struct Object *) sibling->header.gfx.node.next;
    }
}

/**
 * Free the given object.
 */
void unload_object(struct Object *obj) {
    obj->activeFlags = ACTIVE_FLAG_DEACTIVATED;
    obj->prevObj = NULL;

    obj->header.gfx.throwMatrix = NULL;
    func_803206F8(obj->header.gfx.cameraToObject);
    geo_remove_child(&obj->header.gfx.node);
    geo_add_child(&gObjParentGraphNode, &obj->header.gfx.node);

    obj->header.gfx.node.flags &= ~GRAPH_RENDER_BILLBOARD;
    obj->header.gfx.node.flags &= ~GRAPH_RENDER_ACTIVE;

    deallocate_object(&gFreeObjectList, &obj->header);
}

/**
 * Attempt to allocate a new object slot into the given object list, freeing
 * an unimportant object if necessary. If this is not possible, hang using an
 * infinite loop.
 */
#ifdef TARGET_PSP
/* free objects in the pool (see psp_obj_near in psp_options.h) */
int psp_objects_free(void) {
    int n = 0;
    const struct ObjectNode *p = gFreeObjectList.next;
    while (p != NULL && n < 1000) {
        n++;
        p = p->next;
    }
    return n;
}

int psp_spawn_headroom(int n) {
    return psp_objects_free() - n >= 40;
}

int psp_spawn_headroom_lo(int n) {
    return psp_objects_free() - n >= 100;
}

float gPspSpawnScale = 1.0f;
float gPspSpawnScaleLo = 1.0f;
int gPspFreeMin = 9999;

/* Spawner distance according to the free objects (once per tick). It goes continuously from the original (x1.0) to what Object Distance asks for:
 * important ones (chomp, goombas, cannons...): x1.0 with 30 free or fewer ... full Object Distance with 70 or more;
 * low priority (coins):                        x1.0 with 80 free or fewer ... full with 130 or more.
 * This way, when the pool gets tight, the first thing dropped is what is FARTHEST, and coins before the important things. It goes down fast (so as not to
 * run out of objects) and up slowly (so it does not oscillate). */
extern float gPspObjDistScale; /* psp_options.c */
static float psp_scale_follow(float cur, int f, int lo, int hi) {
    const float t = (f <= lo) ? 0.0f : (f >= hi) ? 1.0f : (float) (f - lo) / (float) (hi - lo);
    const float tgt = 1.0f + (gPspObjDistScale - 1.0f) * t;
    float d = tgt - cur;
    if (d < -0.10f) d = -0.10f;
    if (d > 0.02f) d = 0.02f;
    return cur + d;
}

void psp_pool_update(void) {
    const int f = psp_objects_free();
    gPspSpawnScale = psp_scale_follow(gPspSpawnScale, f, 30, 70);
    gPspSpawnScaleLo = psp_scale_follow(gPspSpawnScaleLo, f, 80, 130);
    if (f < gPspFreeMin) {
        gPspFreeMin = f;
    }
}
#endif

#ifdef TARGET_PSP
/* Last resort when the object pool is full and nothing is marked as unimportant: the active object that is farthest from Mario (never Mario
 * itself nor the object that is running), or NULL if there is none. */
static struct Object *psp_find_farthest_object(void) {
    struct Object *best = NULL;
    f32 bestDist = -1.0f;
    s32 i;

    for (i = 0; i < NUM_OBJ_LISTS; i++) {
        struct ObjectNode *list = &gObjectLists[i];
        struct ObjectNode *node = list->next;

        while (node != list) {
            struct Object *candidate = (struct Object *) node;

            node = node->next;
            if (candidate == gMarioObject || candidate == gCurrentObject || !(candidate->activeFlags & ACTIVE_FLAG_ACTIVE)) {
                continue;
            }
            if (candidate->oDistanceToMario > bestDist) {
                bestDist = candidate->oDistanceToMario;
                best = candidate;
            }
        }
    }
    return best;
}
#endif

struct Object *allocate_object(struct ObjectNode *objList) {
    s32 i;
    struct Object *obj = try_allocate_object(objList, &gFreeObjectList);

    // The object list is full if the newly created pointer is NULL.
    // If this happens, we first attempt to unload unimportant objects
    // in order to finish allocating the object.
    if (obj == NULL) {
        // Look for an unimportant object to kick out.
        struct Object *unimportantObj = find_unimportant_object();

        // If no unimportant object exists, then the object pool is exhausted.
        if (unimportantObj == NULL) {
#ifdef TARGET_PSP
            // The original game locks up here forever. On the PSP (where the active distance can be raised) the farthest active object
            // that is not Mario or the one running right now is unloaded instead, so the game goes on.
            unimportantObj = psp_find_farthest_object();
        }
        if (unimportantObj == NULL) {
#endif
            // We've met with a terrible fate.
            while (TRUE) {
            }
        } else {
            // If an unimportant object does exist, unload it and take its slot.
            unload_object(unimportantObj);
            obj = try_allocate_object(objList, &gFreeObjectList);
            if (gCurrentObject == obj) {
                //! Uh oh, the unimportant object was in the middle of
                //  updating! This could cause some interesting logic errors,
                //  but I don't know of any unimportant objects that spawn
                //  other objects.
            }
        }
    }

    // Initialize object fields

    obj->activeFlags = ACTIVE_FLAG_ACTIVE | ACTIVE_FLAG_UNK8;
    obj->parentObj = obj;
    obj->prevObj = NULL;
    obj->collidedObjInteractTypes = 0;
    obj->numCollidedObjs = 0;

#if IS_64_BIT
    for (i = 0; i < 0x50; i++) {
        obj->rawData.asS32[i] = 0;
        obj->ptrData.asVoidPtr[i] = NULL;
    }
#else
    // -O2 needs everything until = on the same line
    for (i = 0; i < 0x50; i++) obj->rawData.asS32[i] = 0;
#endif

    obj->unused1 = 0;
    obj->bhvStackIndex = 0;
    obj->bhvDelayTimer = 0;

    obj->hitboxRadius = 50.0f;
    obj->hitboxHeight = 100.0f;
    obj->hurtboxRadius = 0.0f;
    obj->hurtboxHeight = 0.0f;
    obj->hitboxDownOffset = 0.0f;
    obj->unused2 = 0;

    obj->platform = NULL;
    obj->collisionData = NULL;
    obj->oIntangibleTimer = -1;
    obj->oDamageOrCoinValue = 0;
    obj->oHealth = 2048;

    obj->oCollisionDistance = 1000.0f;
    if (gCurrLevelNum == LEVEL_TTC) {
        obj->oDrawingDistance = 2000.0f;
    } else {
        obj->oDrawingDistance = 4000.0f;
    }

    mtxf_identity(obj->transform);

    obj->respawnInfoType = RESPAWN_INFO_TYPE_NULL;
    obj->respawnInfo = NULL;

    obj->oDistanceToMario = 19000.0f;
    obj->oRoom = -1;

    obj->header.gfx.node.flags &= ~GRAPH_RENDER_INVISIBLE;
    obj->header.gfx.pos[0] = -10000.0f;
    obj->header.gfx.pos[1] = -10000.0f;
    obj->header.gfx.pos[2] = -10000.0f;
    obj->header.gfx.throwMatrix = NULL;

    return obj;
}

/**
 * If the object is close to being on the floor, move it to be exactly on the floor.
 */
static void snap_object_to_floor(struct Object *obj) {
    struct Surface *surface;

    obj->oFloorHeight = find_floor(obj->oPosX, obj->oPosY, obj->oPosZ, &surface);

    if (obj->oFloorHeight + 2.0f > obj->oPosY && obj->oPosY > obj->oFloorHeight - 10.0f) {
        obj->oPosY = obj->oFloorHeight;
        obj->oMoveFlags |= OBJ_MOVE_ON_GROUND;
    }
}

/**
 * Spawn an object at the origin with the behavior script at virtual address bhvScript.
 */
struct Object *create_object(const BehaviorScript *bhvScript) {
    s32 objListIndex;
    struct Object *obj;
    struct ObjectNode *objList;
    const BehaviorScript *behavior = bhvScript;

    // If the first behavior script command is "begin <object list>", then
    // extract the object list from it
    if ((bhvScript[0] >> 24) == 0) {
        objListIndex = (bhvScript[0] >> 16) & 0xFFFF;
    } else {
        objListIndex = OBJ_LIST_DEFAULT;
    }

    objList = &gObjectLists[objListIndex];
    obj = allocate_object(objList);

    obj->curBhvCommand = bhvScript;
    obj->behavior = behavior;

    if (objListIndex == OBJ_LIST_UNIMPORTANT) {
        obj->activeFlags |= ACTIVE_FLAG_UNIMPORTANT;
    }

    //! They intended to snap certain objects to the floor when they spawn.
    //  However, at this point the object's position is the origin. So this will
    //  place the object at the floor beneath the origin. Typically this
    //  doesn't matter since the caller of this function sets oPosX/Y/Z
    //  themselves.
    switch (objListIndex) {
        case OBJ_LIST_GENACTOR:
        case OBJ_LIST_PUSHABLE:
        case OBJ_LIST_POLELIKE:
            snap_object_to_floor(obj);
            break;
        default:
            break;
    }

    return obj;
}

/**
 * Mark an object to be unloaded at the end of the frame.
 */
void mark_obj_for_deletion(struct Object *obj) {
    //! Same issue as obj_mark_for_deletion
    obj->activeFlags = ACTIVE_FLAG_DEACTIVATED;
}
