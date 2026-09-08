#include "v3_native_ges.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "../include/wotbmod/ges_v1.h"
#include "../src/v3/ges_schemas.h"

namespace wotbmod {
namespace loader {
namespace ges {
namespace {

/* MSVC std::function impl vtable order (spec §2). */
struct ImplVtable {
    void* copy;
    void* move;
    void* do_call;
    void* target_type;
    void* delete_this;
    void* get;
};

const uint32_t kMagic = 0x53454758u; /* 'GESX' */

/* Our _Func_impl. The engine only ever sees it through the vtable. */
struct Impl {
    const ImplVtable* vtable;
    uint32_t magic;
    uint32_t type_index;
};

/* __thiscall callee shape through the __fastcall idiom: ecx = this, edx
 * unused, remaining arguments on the stack, callee cleans - identical to what
 * the engine emits for a member function call. */
typedef void (__fastcall* DoCallFn)(Impl* self, void* edx, const void* event);

struct TypeEntry {
    const void* type_info;
    std::string name;   /* "Owner::Name" */
};

struct Observation {
    uint32_t type_index;
    void* list;
    uint8_t* node;      /* engine node holding our impl copy */
};

struct State {
    GesEngine engine = {};
    GesDeliverFn deliver = nullptr;
    void* deliver_user = nullptr;
    GesLogFn log = nullptr;
    std::vector<TypeEntry> types;
    std::vector<Observation> observations;
    bool initialised = false;
    bool faulted = false;
    bool layout_logged = false;
    std::recursive_mutex mutex;
};

State& S() {
    static State state;
    return state;
}

/* Set for the duration of a GesPublish replay so the echo of our own node is
 * reported as MOD_PUBLISHED; engine publishes deliver with 0. */
thread_local uint32_t t_delivery_flags = 0u;

WotbModV3Result RemoveObservation(size_t index);

void Log(const char* message) {
    if (S().log) S().log(message);
}

/* Fail-closed: the unit stops delivering and every node it registered is
 * retired through the engine's own Erase (deferred when the engine is inside
 * the callback that faulted). Retire failures are ignored here - the unit is
 * already off. */
void Fault(const char* where) {
    State& s = S();
    std::lock_guard<std::recursive_mutex> lock(s.mutex);
    s.faulted = true;
    char line[160] = {};
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "[v3] ges disabled: fault in %s; retiring %u observation(s)", where,
                static_cast<unsigned>(s.observations.size()));
    Log(line);
    while (!s.observations.empty()) RemoveObservation(s.observations.size() - 1u);
}

/* ---- our std::function impl ------------------------------------------- */

Impl* NewImpl(uint32_t type_index) {
    Impl* impl = static_cast<Impl*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Impl)));
    if (!impl) return nullptr;
    impl->magic = kMagic;
    impl->type_index = type_index;
    return impl;
}

extern const ImplVtable kImplVtable;

/* _Copy / _Move: the engine never keeps our object inline; it stores the
 * pointer we return and later calls _Delete_this(true) on it. */
static volatile LONG g_gesCopies = 0;
static volatile LONG g_gesDeliveries = 0;

void* __fastcall ImplCopy(Impl* self, void*, void*) {
    if (!self || self->magic != kMagic) return nullptr;
    Impl* copy = NewImpl(self->type_index);
    if (copy) copy->vtable = &kImplVtable;
    /* Live diagnostics (2026-09-04): the engine clones through _Copy when
     * Add stores the listener; the first clone proves the std::function
     * layout the loader hands in is the one the engine reads. */
    if (InterlockedIncrement(&g_gesCopies) == 1) {
        char line[160] = {};
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "[v3] ges: engine copied our listener impl (type %u) -> %p",
                    self->type_index, static_cast<void*>(copy));
        Log(line);
    }
    return copy;
}

void DeliverGuarded(Impl* self, const void* event, uint32_t publisher) {
    State& s = S();
    __try {
        if (s.deliver) {
            s.deliver(s.deliver_user, s.types[self->type_index].name.c_str(), event, publisher,
                      t_delivery_flags);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Fault("_Do_call");
    }
}

extern "C" {
volatile LONGLONG g_wotbProfGesTicks = 0;
volatile LONGLONG g_wotbProfGesCalls = 0;
}

struct GesDeliveryProfile {
    LARGE_INTEGER started;
    GesDeliveryProfile() { QueryPerformanceCounter(&started); }
    ~GesDeliveryProfile() {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        InterlockedExchangeAdd64(&g_wotbProfGesTicks, now.QuadPart - started.QuadPart);
        InterlockedIncrement64(&g_wotbProfGesCalls);
    }
};

void __fastcall ImplDoCall(Impl* self, void*, const void* event) {
    GesDeliveryProfile profile;
    State& s = S();
    if (s.faulted || !self || self->magic != kMagic || self->type_index >= s.types.size()) return;
    const uintptr_t site = reinterpret_cast<uintptr_t>(_ReturnAddress());
    const uintptr_t base = reinterpret_cast<uintptr_t>(s.engine.image_base);
    const uint32_t publisher = base != 0u && site > base ? static_cast<uint32_t>(site - base) : 0u;
    const LONG count = InterlockedIncrement(&g_gesDeliveries);
    if (count <= 3) {
        char line[256] = {};
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "[v3] ges: delivery #%ld type=%s publisher=0x%08X event=%p",
                    count, s.types[self->type_index].name.c_str(), publisher, event);
        Log(line);
    }
    DeliverGuarded(self, event, publisher);
}

const void* __fastcall ImplTargetType(Impl*, void*) {
    static const int kTag = 0;
    return &kTag;
}

void __fastcall ImplDeleteThis(Impl* self, void*, bool free_memory) {
    if (self && free_memory) HeapFree(GetProcessHeap(), 0u, self);
}

void* __fastcall ImplGet(Impl* self, void*) {
    return self;
}

const ImplVtable kImplVtable = {
    reinterpret_cast<void*>(&ImplCopy),
    reinterpret_cast<void*>(&ImplCopy),   /* _Move behaves as _Copy */
    reinterpret_cast<void*>(&ImplDoCall),
    reinterpret_cast<void*>(&ImplTargetType),
    reinterpret_cast<void*>(&ImplDeleteThis),
    reinterpret_cast<void*>(&ImplGet),
};

/* ---- type table ---------------------------------------------------- */

/* One std::type_info name at `text` (NUL within `limit` bytes), or 0. */
size_t GuardedNameLength(const char* text, size_t limit) {
    __try {
        size_t length = 0u;
        while (length < limit && text[length] != '\0') ++length;
        return length < limit ? length : 0u;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0u;
    }
}

bool GuardedMemEq(const uint8_t* at, const char* needle, size_t n) {
    __try {
        return std::memcmp(at, needle, n) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void ScanTypes(const uint8_t* begin, const uint8_t* end) {
    State& s = S();
    static const char kNeedle[] = ".?AU";
    if (!begin || !end || end <= begin + 8) return;
    for (const uint8_t* p = begin; p + 8 < end; ++p) {
        if (!GuardedMemEq(p, kNeedle, 4u)) continue;
        const size_t limit = static_cast<size_t>(end - p) < 256u ? static_cast<size_t>(end - p) : 256u;
        const size_t length = GuardedNameLength(reinterpret_cast<const char*>(p), limit);
        if (length == 0u) continue;
        char name[WOTBMOD_V3_GES_TYPE_NAME_SIZE] = {};
        if (!wotbmod::v3::GesTypeNameFromMangled(reinterpret_cast<const char*>(p), name, sizeof(name))) {
            continue;
        }
        TypeEntry entry;
        entry.type_info = p - 8;   /* std::type_info: vtable, spare, name[] */
        entry.name = name;
        s.types.push_back(entry);
        p += length;
    }
}

int FindType(const char* type_name) {
    const std::vector<TypeEntry>& types = S().types;
    for (size_t i = 0u; i < types.size(); ++i) {
        if (types[i].name == type_name) return static_cast<int>(i);
    }
    return -1;
}

/* ---- engine access, POD-only frames so __try is legal ----------------- */

uint8_t* GuardedGetOrCreateList(const void* type_info) {
    State& s = S();
    __try {
        return static_cast<uint8_t*>(s.engine.get_or_create_list(
            s.engine.bus, type_info, reinterpret_cast<void*>(s.engine.list_factory)));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool GuardedAdd(uint8_t* list, Impl* impl, uint32_t* handle) {
    State& s = S();
    __try {
        uint8_t function[kGesStdFunctionSize] = {};
        *reinterpret_cast<Impl**>(function + kGesStdFunctionPtrOffset) = impl;
        s.engine.list_add(list, handle, function, 1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

/* True when `node` looks like an engine node holding our impl for type_index. */
bool GuardedNodeIsOurs(const uint8_t* node, uint32_t type_index) {
    __try {
        if (!node) return false;
        Impl* impl = *reinterpret_cast<Impl* const*>(node + kGesNodeImplOffset);
        return impl && impl->vtable == &kImplVtable && impl->magic == kMagic &&
               impl->type_index == type_index;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

/* Diagnostics for a layout that did not match: the handle Add returned, the
 * list header and the first node, as raw dwords, so the next reverse pass has
 * something to compare against. */
void GuardedDumpList(const uint8_t* list, const uint32_t* handle, char* out, size_t capacity) {
    __try {
        size_t used = 0u;
        int n = _snprintf_s(out, capacity, _TRUNCATE, "handle=%08X,%08X list=", handle[0], handle[1]);
        used = n > 0 ? static_cast<size_t>(n) : 0u;
        for (size_t i = 0u; i < 12u && used < capacity; ++i) {
            n = _snprintf_s(out + used, capacity - used, _TRUNCATE, "%08X ",
                            *reinterpret_cast<const uint32_t*>(list + i * 4u));
            used += n > 0 ? static_cast<size_t>(n) : 0u;
        }
        const uint8_t* head = *reinterpret_cast<const uint8_t* const*>(list + kGesListHeadOffset);
        const uint8_t* first = head ? *reinterpret_cast<const uint8_t* const*>(head) : nullptr;
        n = _snprintf_s(out + used, capacity - used, _TRUNCATE, "head=%p first=%p node=", head, first);
        used += n > 0 ? static_cast<size_t>(n) : 0u;
        for (size_t i = 0u; first && i < 20u && used < capacity; ++i) {
            n = _snprintf_s(out + used, capacity - used, _TRUNCATE, "%08X ",
                            *reinterpret_cast<const uint32_t*>(first + i * 4u));
            used += n > 0 ? static_cast<size_t>(n) : 0u;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        _snprintf_s(out, capacity, _TRUNCATE, "list dump faulted");
    }
}

/* The node whose impl is ours for `type_index`, or null. */
uint8_t* GuardedFindNode(uint8_t* list, uint32_t type_index) {
    __try {
        uint8_t* head = *reinterpret_cast<uint8_t**>(list + kGesListHeadOffset);
        uint32_t guard = 0u;
        for (uint8_t* node = *reinterpret_cast<uint8_t**>(head); node != head && guard < 4096u; ++guard) {
            Impl* impl = *reinterpret_cast<Impl**>(node + kGesNodeImplOffset);
            if (impl && impl->vtable == &kImplVtable && impl->magic == kMagic && impl->type_index == type_index) {
                return node;
            }
            node = *reinterpret_cast<uint8_t**>(node);
        }
        return nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

/* Disable the node; erase it now unless the engine is inside its callback,
 * in which case the deferred-erase bit makes the engine erase it itself. */
bool GuardedRetire(void* list, uint8_t* node) {
    State& s = S();
    __try {
        uint32_t* flags = reinterpret_cast<uint32_t*>(node + kGesNodeFlagsOffset);
        *flags |= kGesFlagDisabled;
        if ((*flags & kGesFlagInvoking) == 0u) {
            void* iterator = node;
            void* out = nullptr;
            s.engine.list_erase(list, &out, &iterator);
        } else {
            *flags |= kGesFlagDeferredErase;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool GuardedPublish(const void* type_info, const void* payload, bool echo) {
    State& s = S();
    __try {
        uint8_t* list = static_cast<uint8_t*>(s.engine.get_listeners(s.engine.bus, type_info));
        if (!list) return true;   /* nobody listens: the engine does nothing either */
        uint8_t* head = *reinterpret_cast<uint8_t**>(list + kGesListHeadOffset);
        uint32_t guard = 0u;
        for (uint8_t* node = *reinterpret_cast<uint8_t**>(head); node != head && guard < 4096u; ++guard) {
            uint32_t* flags = reinterpret_cast<uint32_t*>(node + kGesNodeFlagsOffset);
            Impl* impl = *reinterpret_cast<Impl**>(node + kGesNodeImplOffset);
            const bool ours = impl && impl->vtable == &kImplVtable;
            if ((*flags & kGesFlagDisabled) == 0u && impl && (!ours || echo)) {
                *flags |= kGesFlagInvoking;
                const ImplVtable* vtable = *reinterpret_cast<const ImplVtable**>(impl);
                reinterpret_cast<DoCallFn>(vtable->do_call)(impl, nullptr, payload);
                *flags &= ~kGesFlagInvoking;
            }
            if ((*flags & kGesFlagDeferredErase) != 0u) {
                void* iterator = node;
                void* out = nullptr;
                s.engine.list_erase(list, &out, &iterator);
                node = static_cast<uint8_t*>(out);
            } else {
                node = *reinterpret_cast<uint8_t**>(node);
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

WotbModV3Result AddObservation(uint32_t type_index) {
    State& s = S();
    uint8_t* list = GuardedGetOrCreateList(s.types[type_index].type_info);
    if (!list) {
        Fault("observe/list");
        return WOTBMOD_V3_E_PLATFORM;
    }
    Impl* temporary = NewImpl(type_index);
    if (!temporary) return WOTBMOD_V3_E_PLATFORM;
    temporary->vtable = &kImplVtable;
    uint32_t handle[2] = {};
    const bool added = GuardedAdd(list, temporary, handle);
    /* Add copied through ImplCopy; the temporary was only the source. */
    HeapFree(GetProcessHeap(), 0u, temporary);
    if (!added) {
        Fault("observe/add");
        return WOTBMOD_V3_E_PLATFORM;
    }
    /* Add answers with an iterator; when its first word is our node, that is
     * the node - no list walk needed. Otherwise walk the list as documented in
     * the spec, and if that finds nothing either, record what the engine
     * actually left behind before going fail-closed. */
    /* Live 11.20.0.887 (2026-09-04): Add returns a small token, and the node
     * is not on the head ring right after the call - the engine parks new
     * listeners until its dispatch loop is not running. So the node is not
     * required here: the impl the engine copied is ours whenever it fires,
     * and the node is looked up lazily when the observation is retired. The
     * layout is still logged once so the next reverse pass can compare. */
    uint8_t* node = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(handle[0]));
    if (handle[0] < 0x10000u || !GuardedNodeIsOurs(node, type_index)) node = GuardedFindNode(list, type_index);
    if (!node && !s.layout_logged) {
        s.layout_logged = true;
        char dump[768] = {};
        GuardedDumpList(list, handle, dump, sizeof(dump));
        char line[896] = {};
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "[v3] ges observe/node not on the ring yet (token %u); resolving lazily: %s",
                    handle[0], dump);
        Log(line);
    }
    Observation observation;
    observation.type_index = type_index;
    observation.list = list;
    observation.node = node;
    s.observations.push_back(observation);
    return WOTBMOD_V3_OK;
}

WotbModV3Result RemoveObservation(size_t index) {
    State& s = S();
    Observation observation = s.observations[index];
    s.observations.erase(s.observations.begin() + static_cast<std::ptrdiff_t>(index));
    if (!observation.node) {
        observation.node = GuardedFindNode(static_cast<uint8_t*>(observation.list),
                                           observation.type_index);
    }
    if (!observation.node) {
        /* Never seen on the ring: nothing to retire; the impl copy the engine
         * holds answers with faulted/uninitialised checks if it ever fires. */
        Log("[v3] ges release: node not found on the ring; observation dropped");
        return WOTBMOD_V3_OK;
    }
    if (!GuardedRetire(observation.list, observation.node)) {
        if (!s.faulted) Fault("release");
        return WOTBMOD_V3_E_PLATFORM;
    }
    return WOTBMOD_V3_OK;
}

}  // namespace

bool GesInit(const GesEngine& engine, const uint8_t* data_begin, const uint8_t* data_end,
             GesDeliverFn deliver, void* deliver_user, GesLogFn log) {
    State& s = S();
    std::lock_guard<std::recursive_mutex> lock(s.mutex);
    s.engine = engine;
    s.deliver = deliver;
    s.deliver_user = deliver_user;
    s.log = log;
    s.types.clear();
    s.observations.clear();
    s.faulted = false;
    s.initialised = false;
    if (!engine.get_listeners || !engine.get_or_create_list || !engine.list_factory ||
        !engine.list_add || !engine.list_erase) {
        Log("[v3] ges: engine entry points incomplete");
        return false;
    }
    ScanTypes(data_begin, data_end);
    if (s.types.empty()) {
        Log("[v3] ges: no GES type descriptors found in .data");
        return false;
    }
    s.initialised = true;
    char line[96] = {};
    _snprintf_s(line, sizeof(line), _TRUNCATE, "[v3] ges: %u event types indexed",
                static_cast<unsigned>(s.types.size()));
    Log(line);
    return true;
}

/* Live diagnostics (2026-09-04): the client may run more than one
 * GameEventSystem (hangar vs battle); every distinct bus seen by the
 * SubscribeImpl detour is logged once so observations can be checked
 * against the bus the publishers actually use. */
static void* g_gesBusesSeen[8] = {};
static volatile LONG g_gesBusCount = 0;

void GesSetBus(void* bus) {
    State& s = S();
    std::lock_guard<std::recursive_mutex> lock(s.mutex);
    bool known = false;
    const LONG count = InterlockedCompareExchange(&g_gesBusCount, 0, 0);
    for (LONG i = 0; i < count && i < 8; ++i) {
        if (g_gesBusesSeen[i] == bus) { known = true; break; }
    }
    if (!known && count < 8) {
        g_gesBusesSeen[count] = bus;
        InterlockedIncrement(&g_gesBusCount);
        char line[160] = {};
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "[v3] ges: SubscribeImpl bus #%ld = %p (observations follow the latest one)",
                    count + 1, bus);
        Log(line);
    }
    s.engine.bus = bus;
}

bool GesReady() {
    State& s = S();
    return s.initialised && s.engine.bus != nullptr && !s.faulted;
}

uint32_t GesTypeCount() {
    return static_cast<uint32_t>(S().types.size());
}

const char* GesTypeName(uint32_t index) {
    const State& s = S();
    return index < s.types.size() ? s.types[index].name.c_str() : nullptr;
}

WotbModV3Result GesObserve(const char* type_name, bool observe) {
    State& s = S();
    std::lock_guard<std::recursive_mutex> lock(s.mutex);
    if (!GesReady()) return WOTBMOD_V3_E_NOT_SUPPORTED;
    const int index = type_name ? FindType(type_name) : -1;
    if (index < 0) return WOTBMOD_V3_E_NOT_FOUND;
    for (size_t i = 0u; i < s.observations.size(); ++i) {
        if (s.observations[i].type_index == static_cast<uint32_t>(index)) {
            return observe ? WOTBMOD_V3_OK : RemoveObservation(i);
        }
    }
    return observe ? AddObservation(static_cast<uint32_t>(index)) : WOTBMOD_V3_OK;
}

WotbModV3Result GesPublish(const char* type_name, const void* payload, uint32_t payload_size,
                           uint32_t flags) {
    State& s = S();
    std::lock_guard<std::recursive_mutex> lock(s.mutex);
    if (!GesReady()) return WOTBMOD_V3_E_NOT_SUPPORTED;
    if (!payload || payload_size == 0u) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    const int index = type_name ? FindType(type_name) : -1;
    if (index < 0) return WOTBMOD_V3_E_NOT_FOUND;
    const uint32_t previous_flags = t_delivery_flags;
    t_delivery_flags = WOTBMOD_V3_GES_EVENT_MOD_PUBLISHED;
    const bool published =
        GuardedPublish(s.types[static_cast<size_t>(index)].type_info, payload, (flags & 1u) != 0u);
    t_delivery_flags = previous_flags;
    if (!published) {
        Fault("publish");
        return WOTBMOD_V3_E_PLATFORM;
    }
    return WOTBMOD_V3_OK;
}

void GesShutdown() {
    State& s = S();
    std::lock_guard<std::recursive_mutex> lock(s.mutex);
    while (!s.observations.empty()) {
        if (RemoveObservation(s.observations.size() - 1u) != WOTBMOD_V3_OK) break;
    }
    s.observations.clear();
    s.initialised = false;
    s.engine.bus = nullptr;
}

}  // namespace ges
}  // namespace loader
}  // namespace wotbmod
