#pragma once
/*
 * Engine-facing half of wotbmod.ges for client 11.20.0.887.
 *
 * GES::GameEventSystem keeps `this+0x04: unordered_map<type_index,
 * ListenerList*>`. Its Subscribe<T> templates all end in the same
 * non-templated core, and every inlined publish site runs the same loop:
 *
 *   list = GetListeners(&typeid(T)); if (!list) return;
 *   for (node = list->head->next; node != list->head; ) {
 *       if (!(node->flags & 2)) { node->flags |= 1; node->impl->_Do_call(&event); node->flags &= ~1; }
 *       node = (node->flags & 4) ? ListenerList::Erase(list, &it, &node) : node->next;
 *   }
 *
 * This unit registers an MSVC std::function-compatible listener of our own
 * through the engine's GetOrCreateList + ListenerList::Add, so the engine
 * itself calls us with the event pointer, and replays the loop above for mod
 * publishes. Engine entry points are injected (GesEngine) so the unit runs
 * against a host double in tests/v3_ges_native_tests.cpp.
 *
 * Layouts (spec §2): ListenerList {+0 vtable, +4 head, +8 size, +0xC
 * insert_pos}; node 0x50 bytes {+0 next, +4 prev, +8..+0x1F reserved,
 * +0x20 std::function (0x28 bytes, _Ptr at +0x44), +0x48 flags}; impl vtable
 * {+0 _Copy, +4 _Move, +8 _Do_call, +0xC _Target_type, +0x10 _Delete_this,
 * +0x14 _Get}.
 */
#include <cstddef>
#include <cstdint>

#include "../include/wotbmod/base.h"

namespace wotbmod {
namespace loader {
namespace ges {

/* 11.20.0.887 (IDA sub_6DBC30/sub_6DBF80, 2026-09-04): both take the std::type_info
 * pointer itself and hash its name at +4; a pointer to that pointer keys a list
 * nobody publishes to. */
typedef void* (__thiscall* GesGetListenersFn)(void* bus, const void* type_info);
typedef void* (__thiscall* GesGetOrCreateListFn)(void* bus, const void* type_info, void* factory);
typedef void** (__cdecl* GesListFactoryFn)(void** out_list);
typedef void* (__thiscall* GesListAddFn)(void* list, uint32_t* out_handle, void* std_function, int position);
typedef void** (__thiscall* GesListEraseFn)(void* list, void** out_iterator, void** iterator);
/* flags: WOTBMOD_V3_GES_EVENT_MOD_PUBLISHED when the delivery is the echo of a
 * GesPublish from a mod, 0 for an engine publish. */
typedef void (*GesDeliverFn)(void* user, const char* type_name, const void* payload, uint32_t publisher_rva, uint32_t flags);
typedef void (*GesLogFn)(const char* message);

struct GesEngine {
    void* bus;                       /* GES::GameEventSystem*, captured at runtime */
    const void* image_base;          /* game module base; publisher RVAs are relative to it (0 in tests) */
    GesGetListenersFn get_listeners;
    GesGetOrCreateListFn get_or_create_list;
    GesListFactoryFn list_factory;
    GesListAddFn list_add;
    GesListEraseFn list_erase;
};

const size_t kGesNodeSize = 0x50u;
const size_t kGesNodeFunctionOffset = 0x20u;
const size_t kGesNodeImplOffset = 0x44u;
const size_t kGesNodeFlagsOffset = 0x48u;
const size_t kGesListHeadOffset = 0x04u;
const size_t kGesStdFunctionSize = 0x28u;
const size_t kGesStdFunctionPtrOffset = 0x24u;

const uint32_t kGesFlagInvoking = 1u;
const uint32_t kGesFlagDisabled = 2u;
const uint32_t kGesFlagDeferredErase = 4u;

/* Scans [data_begin, data_end) for GES std::type_info objects and records
 * the engine entry points. False when an entry point is missing or no type
 * was found. Safe to call again (re-initialises). */
bool GesInit(const GesEngine& engine, const uint8_t* data_begin, const uint8_t* data_end,
             GesDeliverFn deliver, void* deliver_user, GesLogFn log);
void GesSetBus(void* bus);
/* Erases every node this unit registered; the unit is no longer ready. Also
 * what a fault does on its own (fail-closed): the first exception in a
 * callback or an engine call retires every observation. */
void GesShutdown();
/* Initialised, bus captured and not faulted. */
bool GesReady();
uint32_t GesTypeCount();
const char* GesTypeName(uint32_t index);   /* "Owner::Name", or null */

WotbModV3Result GesObserve(const char* type_name, bool observe);
WotbModV3Result GesPublish(const char* type_name, const void* payload, uint32_t payload_size, uint32_t flags);

}  // namespace ges
}  // namespace loader
}  // namespace wotbmod
