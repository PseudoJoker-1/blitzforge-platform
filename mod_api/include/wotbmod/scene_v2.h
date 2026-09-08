#pragma once

#include "base.h"

/*
 * wotbmod.scene.enumerate -- READ ONLY enumeration of the game-owned scene
 * graph. Declared 2026-08-16 as part of the release contract that follows RC1.
 * Backed by re_anchors.md, section "Entity / Scene layout -- read-only
 * enumeration", cross-checked against 11.19.0.834 / 41960DBD...E0AD.
 *
 * The proven layout
 * -----------------
 * Children are a contiguous Entity*[begin, end) at Entity+0x08 / +0x0C, with
 * capacity end at +0x10. Parent is +0x18. The name at +0x1C is a FastName --
 * one dword holding an interned const char*, NOT refcounted. TransformComponent*
 * is +0x3C and the world Matrix4 lives at TC+0x60. Scene reuses the same
 * container: Scene::Scene calls the real Entity(const FastName&) ctor before
 * swapping the vtable, and slots 5/6 of both vtables point at the same
 * AddNode/RemoveNode.
 *
 * Why the thread rule is in the ABI and not only in the prose
 * ----------------------------------------------------------
 * Entity::AddNode (0x00CD6C10) and Entity::RemoveNode (0x00D10C90) memmove the
 * child vector with NO lock, and RemoveNode calls Release on the child
 * immediately after compacting. A concurrent reader can hold a pointer that
 * the next instruction frees. There is no lock and no version counter to
 * detect it, so thread discipline is the only defence -- which means the ABI
 * has to make the unsafe shape non-existent rather than merely discouraged:
 *
 *   - There is no handle, cursor, iterator or node object. Nothing survives
 *     the call, so nothing can be carried to another thread or another frame.
 *   - There is no visitor callback. A callback would run with the engine's
 *     vector mid-walk and would hand a mod a re-entry point into it.
 *   - The one entry point is a single synchronous call that copies everything
 *     it will ever return before it returns, and answers
 *     WOTBMOD_V3_E_WRONG_THREAD anywhere but the main thread.
 *
 * No pointer, and no handle derived from an address, reaches a mod. `index`
 * and `parent_index` are positions in the caller's OWN buffer and mean nothing
 * outside the single call that produced them.
 *
 * Bounded by construction
 * -----------------------
 * Depth, children per node and total nodes are all capped, and the caller
 * supplies a fixed-capacity buffer. The walk never allocates on the mod's
 * behalf. A buffer that cannot hold the result returns
 * WOTBMOD_V3_E_BUFFER_TOO_SMALL with the required count, rather than
 * allocating or silently returning a prefix that looks complete.
 *
 * Every slot returns WOTBMOD_V3_E_NOT_SUPPORTED until the backend lands.
 * Permission: reuses the existing REVIEWED `game.entity.public`. A battle
 * scene walk discloses the identity and world placement of game-owned objects,
 * which is the same disclosure class that grant already governs; it is not a
 * SAFE read.
 */

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_SCENE_VERSION_2 2u

/* Contract ceilings. A host may enforce lower values; ask get_limits. */
#define WOTBMOD_V3_SCENE_WALK_MAX_DEPTH 16u
#define WOTBMOD_V3_SCENE_WALK_MAX_CHILDREN 4096u
#define WOTBMOD_V3_SCENE_WALK_MAX_NODES 8192u
#define WOTBMOD_V3_SCENE_NODE_NAME_SIZE 64u
/* parent_index of the root record. Not an index and never dereferenceable. */
#define WOTBMOD_V3_SCENE_NODE_NO_PARENT 0xFFFFFFFFu

typedef enum WotbModV3SceneNodeFlag {
    /* The record is a Scene (vtable 0x03612764), not a plain Entity. */
    WOTBMOD_V3_SCENE_NODE_IS_SCENE = 1u << 0,
    /*
     * TransformComponent* at +0x3C was non-null and world_matrix holds the
     * Matrix4 read from TC+0x60. When clear, world_matrix is identity and
     * means nothing -- it is a filler, not a measurement.
     */
    WOTBMOD_V3_SCENE_NODE_HAS_WORLD_MATRIX = 1u << 1,
    /* The FastName was longer than the bounded copy and was cut. */
    WOTBMOD_V3_SCENE_NODE_NAME_TRUNCATED = 1u << 2,
    /* child_count exceeded the per-node cap; the extra children are absent. */
    WOTBMOD_V3_SCENE_NODE_CHILDREN_TRUNCATED = 1u << 3,
    /* The depth cap stopped the walk here; this node's children are absent. */
    WOTBMOD_V3_SCENE_NODE_DEPTH_LIMITED = 1u << 4
} WotbModV3SceneNodeFlag;

/*
 * A clone, not a view. Every field was copied out of engine memory while the
 * main thread held it, and nothing in it points anywhere.
 */
typedef struct WotbModV3SceneNodeRecord {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t index;
    uint32_t parent_index;
    uint32_t depth;
    /* Children the engine reported, which may exceed the number emitted. */
    uint32_t child_count;
    uint32_t flags;
    uint32_t reserved;
    WotbModV3Matrix4 world_matrix;
    char name[WOTBMOD_V3_SCENE_NODE_NAME_SIZE];
} WotbModV3SceneNodeRecord;

/*
 * The caps a walk runs under. Also what get_limits reports, so a caller can
 * ask for the effective bounds and hand the same struct straight back.
 *
 * There is deliberately NO buffer pointer in here. An earlier draft embedded
 * `WotbModV3SceneNodeRecord* nodes` plus a `node_capacity` in the request
 * struct, which is an ordinary and perfectly good C shape - and it is not
 * marshallable from a scripting host. A generic struct reader allocates one
 * element for a pointer field and takes the neighbouring count from whatever
 * the script supplied, so the client would be handed a one-record buffer and
 * told it had `node_capacity` records to WRITE into. That is a heap overflow
 * rather than the over-read the same shape causes on an input array, and it
 * is reachable from a sandbox.
 *
 * The buffer is therefore a real parameter of the call, in the (array, count)
 * form every other enumerating slot in this ABI already uses (vfs.list,
 * hooks.enumerate, input.get_bindings). That shape is understood by the
 * binding generator, allocates exactly what it reports, and needs no new
 * marshalling feature to be safe.
 */
typedef struct WotbModV3SceneWalkRequest {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t max_depth;
    uint32_t max_children_per_node;
    uint32_t max_nodes;
    uint32_t reserved;
} WotbModV3SceneWalkRequest;

/*
 * There is no separate result struct. The (out_nodes, inout_count) pair is
 * the whole protocol, and it is the one every other enumerating slot in this
 * ABI already uses: call with out_nodes NULL to learn how many nodes the walk
 * would reach, then call again with a buffer that big. If the buffer is too
 * small the provider writes what fits, sets *inout_count to the number the
 * walk actually reached, and answers WOTBMOD_V3_E_BUFFER_TOO_SMALL - so
 * truncation is a return code the caller cannot ignore rather than a flag in
 * a struct the caller might not read.
 *
 * Per-node truncation is a different question and stays per-node:
 * WOTBMOD_V3_SCENE_NODE_CHILDREN_TRUNCATED and _DEPTH_LIMITED in the record's
 * flags say which node lost detail, which a single global flag could not.
 */

typedef struct WotbModV3SceneWalkLimits {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t max_depth;
    uint32_t max_children_per_node;
    uint32_t max_nodes;
    /* The provider's sizeof(WotbModV3SceneNodeRecord). */
    uint32_t node_record_size;
} WotbModV3SceneWalkLimits;

typedef struct WotbModV3SceneApiV2 {
    uint32_t struct_size;
    uint32_t api_version;

    /* Effective caps. Callable from any thread; it touches no engine memory. */
    WotbModV3Result(WOTBMOD_V3_CALL* get_limits)(
        WotbModV3Handle mod,
        WotbModV3SceneWalkLimits* out_limits);
    /*
     * Main thread only: WOTBMOD_V3_E_WRONG_THREAD anywhere else, checked
     * before a single engine pointer is read. Breadth-first from the active
     * scene root, which is written to index 0 with parent_index
     * WOTBMOD_V3_SCENE_NODE_NO_PARENT. Returns WOTBMOD_V3_E_NOT_FOUND when no
     * scene is active.
     */
    WotbModV3Result(WOTBMOD_V3_CALL* walk_active_scene)(
        WotbModV3Handle mod,
        const WotbModV3SceneWalkRequest* request,
        WotbModV3SceneNodeRecord* out_nodes,
        uint32_t* inout_count);
} WotbModV3SceneApiV2;

#ifdef __cplusplus
}
#endif
