/*
 * Host double of GES::GameEventSystem's ListenerList for loader/v3_native_ges.
 * The double reproduces exactly the node/list layout and the inlined dispatch
 * loop documented in docs/superpowers/specs/2026-09-03-ges-event-bus-design.md
 * §2, so the unit is exercised the way the client exercises it: through
 * function pointers, on nodes the "engine" allocated.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "wotbmod/ges_v1.h"
#include "../loader/v3_native_ges.h"

#define CHECK(expression)                                          \
    do {                                                           \
        if (!(expression)) {                                       \
            std::fprintf(stderr, "check failed at line %d: %s\n", \
                         __LINE__, #expression);                   \
            return 1;                                              \
        }                                                          \
    } while (0)

using namespace wotbmod::loader::ges;

namespace {

struct Node {                 /* 0x50 bytes, spec §2 */
    Node* next;               /* +0 */
    Node* prev;               /* +4 */
    uint8_t reserved[0x18];   /* +8..+0x1F */
    uint8_t function[0x24];   /* +0x20 std::function storage */
    void* impl;               /* +0x44 _Ptr */
    uint32_t flags;           /* +0x48 */
    uint32_t pad;             /* +0x4C */
};
static_assert(sizeof(Node) == kGesNodeSize, "node layout");
static_assert(offsetof(Node, function) == kGesNodeFunctionOffset, "function offset");
static_assert(offsetof(Node, impl) == kGesNodeImplOffset, "impl offset");
static_assert(offsetof(Node, flags) == kGesNodeFlagsOffset, "flags offset");

struct List {                 /* 16 bytes */
    void* vtable;
    Node* head;
    uint32_t size;
    Node* insert_pos;
};
static_assert(offsetof(List, head) == kGesListHeadOffset, "head offset");

struct FakeTypeInfo {         /* std::type_info: vtable, spare, name[] */
    void* vtable;
    void* spare;
    char name[64];
};

struct ImplVtable {
    void* copy;
    void* move;
    void* do_call;
    void* target_type;
    void* delete_this;
    void* get;
};

typedef void* (__fastcall* CopyFn)(void* impl, void* edx, void* where);
typedef void (__fastcall* DoCallFn)(void* impl, void* edx, const void* event);
typedef void (__fastcall* DeleteThisFn)(void* impl, void* edx, bool free_memory);

std::map<const void*, List*> g_lists;   /* type_info* -> list */
int g_erased = 0;
int g_factory_calls = 0;

void* __fastcall FakeGetListeners(void*, void*, const void* type_info) {
    std::map<const void*, List*>::iterator it = g_lists.find(type_info);
    return it == g_lists.end() ? nullptr : it->second;
}

void** __cdecl FakeFactory(void** out) {
    ++g_factory_calls;
    List* list = new List();
    Node* head = new Node();
    head->next = head;
    head->prev = head;
    list->vtable = nullptr;
    list->head = head;
    list->size = 0u;
    list->insert_pos = head;
    *out = list;
    return out;
}

void* __fastcall FakeGetOrCreateList(void* bus, void* edx, const void* type_info, void* factory) {
    void* existing = FakeGetListeners(bus, edx, type_info);
    if (existing) return existing;
    void* created = nullptr;
    reinterpret_cast<GesListFactoryFn>(factory)(&created);
    g_lists[type_info] = static_cast<List*>(created);
    return created;
}

void* __fastcall FakeListAdd(void* list_pointer, void*, uint32_t* out_handle, void* std_function, int position) {
    List* list = static_cast<List*>(list_pointer);
    Node* node = new Node();
    void* impl = *reinterpret_cast<void**>(static_cast<uint8_t*>(std_function) + kGesStdFunctionPtrOffset);
    const ImplVtable* vtable = *static_cast<const ImplVtable* const*>(impl);
    /* MSVC std::function copy: impl->_Copy(where) returns the copy. */
    node->impl = reinterpret_cast<CopyFn>(vtable->copy)(impl, nullptr, node->function);
    Node* at = position == 1 ? list->insert_pos : list->head->next;
    node->next = at;
    node->prev = at->prev;
    at->prev->next = node;
    at->prev = node;
    ++list->size;
    out_handle[0] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(node));
    out_handle[1] = 0u;
    return out_handle;
}

void** __fastcall FakeListErase(void* list_pointer, void*, void** out_iterator, void** iterator) {
    List* list = static_cast<List*>(list_pointer);
    Node* node = static_cast<Node*>(*iterator);
    Node* next = node->next;
    node->prev->next = next;
    next->prev = node->prev;
    --list->size;
    const ImplVtable* vtable = *static_cast<const ImplVtable* const*>(node->impl);
    /* impl->_Delete_this(impl != &node->function) */
    reinterpret_cast<DeleteThisFn>(vtable->delete_this)(
        node->impl, nullptr, node->impl != static_cast<void*>(node->function));
    delete node;
    ++g_erased;
    *out_iterator = next;
    return out_iterator;
}

/* The inlined client loop, verbatim shape. */
void FakeEnginePublish(const FakeTypeInfo* type, const void* event) {
    const void* key = type;
    List* list = static_cast<List*>(FakeGetListeners(nullptr, nullptr, key));
    if (!list) return;
    for (Node* node = list->head->next; node != list->head;) {
        if ((node->flags & kGesFlagDisabled) == 0u) {
            node->flags |= kGesFlagInvoking;
            const ImplVtable* vtable = *static_cast<const ImplVtable* const*>(node->impl);
            reinterpret_cast<DoCallFn>(vtable->do_call)(node->impl, nullptr, event);
            node->flags &= ~kGesFlagInvoking;
        }
        if ((node->flags & kGesFlagDeferredErase) != 0u) {
            void* iterator = node;
            void* out = nullptr;
            FakeListErase(list, nullptr, &out, &iterator);
            node = static_cast<Node*>(out);
        } else {
            node = node->next;
        }
    }
}

std::vector<std::string> g_delivered;
const void* g_last_payload = nullptr;
uint32_t g_last_rva = 0u;
uint32_t g_last_flags = 0xFFFFFFFFu;

void Deliver(void*, const char* name, const void* payload, uint32_t publisher_rva, uint32_t flags) {
    g_delivered.push_back(name);
    g_last_payload = payload;
    g_last_rva = publisher_rva;
    g_last_flags = flags;
}

/* A mod callback that crashes inside the engine's dispatch loop. */
void DeliverFaulting(void*, const char*, const void*, uint32_t, uint32_t) {
    volatile int* bad = nullptr;
    *bad = 1;
}

void Log(const char*) {}

GesEngine Engine() {
    GesEngine engine = {};
    engine.get_listeners = reinterpret_cast<GesGetListenersFn>(&FakeGetListeners);
    engine.get_or_create_list = reinterpret_cast<GesGetOrCreateListFn>(&FakeGetOrCreateList);
    engine.list_factory = &FakeFactory;
    engine.list_add = reinterpret_cast<GesListAddFn>(&FakeListAdd);
    engine.list_erase = reinterpret_cast<GesListEraseFn>(&FakeListErase);
    return engine;
}

}  // namespace

int main() {
    /* .data double: two type_info objects followed by junk */
    FakeTypeInfo types[3] = {};
    strcpy_s(types[0].name, sizeof(types[0].name), ".?AUCameraModeChanged@Avatar@GES@@");
    strcpy_s(types[1].name, sizeof(types[1].name), ".?AUAccepted@Survey@Lobby@GES@@");
    strcpy_s(types[2].name, sizeof(types[2].name), ".?AVUIControl@DAVA@@");
    const uint8_t* begin = reinterpret_cast<const uint8_t*>(types);
    const uint8_t* end = begin + sizeof(types);

    GesEngine engine = Engine();
    CHECK(GesInit(engine, begin, end, &Deliver, nullptr, &Log));
    CHECK(GesTypeCount() == 2u);
    CHECK(std::strcmp(GesTypeName(0), "Avatar::CameraModeChanged") == 0);
    CHECK(std::strcmp(GesTypeName(1), "Lobby::Survey::Accepted") == 0);
    CHECK(GesTypeName(2) == nullptr);
    CHECK(!GesReady());                                  /* bus not captured yet */
    CHECK(GesObserve("Avatar::CameraModeChanged", true) == WOTBMOD_V3_E_NOT_SUPPORTED);
    GesSetBus(reinterpret_cast<void*>(0x1234));
    CHECK(GesReady());

    /* Observe a type nobody subscribed to: the list is created through the factory. */
    CHECK(GesObserve("Avatar::CameraModeChanged", true) == WOTBMOD_V3_OK);
    CHECK(g_factory_calls == 1 && g_lists.size() == 1u && g_lists.begin()->second->size == 1u);
    CHECK(GesObserve("Avatar::CameraModeChanged", true) == WOTBMOD_V3_OK);   /* idempotent */
    CHECK(g_lists.begin()->second->size == 1u);
    CHECK(GesObserve("Avatar::Nope", true) == WOTBMOD_V3_E_NOT_FOUND);

    int event = 0;
    FakeEnginePublish(&types[0], &event);
    CHECK(g_delivered.size() == 1u && g_delivered[0] == "Avatar::CameraModeChanged");
    CHECK(g_last_payload == &event);
    CHECK(g_last_flags == 0u);                           /* engine publish */
    FakeEnginePublish(&types[1], &event);
    CHECK(g_delivered.size() == 1u);                     /* not observed */

    /* Mod publish reaches engine listeners but skips our own node unless ECHO. */
    g_delivered.clear();
    struct { int32_t mode; uint8_t flag; } payload = {0, 0};
    CHECK(GesPublish("Avatar::CameraModeChanged", &payload, 8u, 0u) == WOTBMOD_V3_OK);
    CHECK(g_delivered.empty());
    CHECK(GesPublish("Avatar::CameraModeChanged", &payload, 8u, 1u /* ECHO */) == WOTBMOD_V3_OK);
    CHECK(g_delivered.size() == 1u && g_last_payload == &payload);
    CHECK(g_last_flags == WOTBMOD_V3_GES_EVENT_MOD_PUBLISHED);   /* echo of our own publish */
    CHECK(GesPublish("Lobby::Survey::Accepted", &payload, 8u, 1u) == WOTBMOD_V3_OK);  /* no list: no-op */
    CHECK(g_delivered.size() == 1u);
    CHECK(GesPublish("Avatar::Nope", &payload, 8u, 0u) == WOTBMOD_V3_E_NOT_FOUND);
    CHECK(GesPublish("Avatar::CameraModeChanged", nullptr, 0u, 0u) == WOTBMOD_V3_E_INVALID_ARGUMENT);

    /* Release: our node is erased through the engine's own Erase. */
    CHECK(GesObserve("Avatar::CameraModeChanged", false) == WOTBMOD_V3_OK);
    CHECK(g_erased == 1 && g_lists.begin()->second->size == 0u);
    g_delivered.clear();
    FakeEnginePublish(&types[0], &event);
    CHECK(g_delivered.empty());
    CHECK(GesObserve("Avatar::CameraModeChanged", false) == WOTBMOD_V3_OK);   /* idempotent */
    CHECK(g_erased == 1);

    /* Shutdown with a live observation erases it and the unit is not ready. */
    CHECK(GesObserve("Lobby::Survey::Accepted", true) == WOTBMOD_V3_OK);
    CHECK(g_lists.size() == 2u);
    GesShutdown();
    CHECK(g_erased == 2);
    CHECK(!GesReady());
    CHECK(GesObserve("Lobby::Survey::Accepted", true) == WOTBMOD_V3_E_NOT_SUPPORTED);

    /* Fail-closed: a callback that faults inside the engine loop takes the
     * unit down and retires its node through the engine's deferred erase. */
    CHECK(GesInit(engine, begin, end, &DeliverFaulting, nullptr, &Log));
    GesSetBus(reinterpret_cast<void*>(0x1234));
    CHECK(GesObserve("Avatar::CameraModeChanged", true) == WOTBMOD_V3_OK);
    CHECK(g_lists[&types[0]]->size == 1u);
    const int erased_before = g_erased;
    FakeEnginePublish(&types[0], &event);
    CHECK(!GesReady());
    CHECK(g_erased == erased_before + 1 && g_lists[&types[0]]->size == 0u);
    CHECK(GesObserve("Avatar::CameraModeChanged", true) == WOTBMOD_V3_E_NOT_SUPPORTED);

    std::printf("V3 GES native: host-double checks passed\n");
    return 0;
}
