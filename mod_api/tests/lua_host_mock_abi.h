#ifndef WOTBMOD_LUA_HOST_MOCK_ABI_H_
#define WOTBMOD_LUA_HOST_MOCK_ABI_H_

#include "../include/wotb_mod_api_v3.h"

// For GetCurrentThreadId, which the control_create thread witness below needs.
// Included here rather than relied on from the one file that includes this
// header, so that this header stands on its own.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// A synthetic host. Every interface it returns is a real struct of real
// function pointers that append a "name(arg,arg)" record to the vector for
// that interface area and return OK, so a binding can be driven end to end
// and then checked by what it asked the ABI to do.
//
// One vector per interface area (core_calls, storage_calls, events_calls,
// ui_calls) so a later task that adds an events or ui mock only has to add
// its own struct of function pointers that push into the vector already
// reserved for it here -- that is additive, not a restructuring of what
// Task 2 wrote. Task 5 wires up storage (all 14 slots) and core.log (the
// one slot print() needs); Task 6 wires up events (all 9 slots); Task 7 wires
// up the 18 hand-written UI slots. The mock also exposes the small generated
// UI path used by the distributable Lua UI example: active-screen ownership,
// set_parent, event_subscribe/unsubscribe and a V3 snapshot.
namespace MockAbi {

inline std::vector<std::string> core_calls;
inline std::vector<std::string> handles_calls;
inline std::vector<std::string> capabilities_calls;
inline std::vector<std::string> storage_calls;
inline std::vector<std::string> events_calls;
inline std::vector<std::string> ui_calls;
inline std::vector<std::string> vfs_calls;
inline std::vector<std::string> async_calls;
inline std::vector<std::string> hud_calls;
// One log for the interfaces the stage-1b facades stand on (camera, audio,
// input, vehicle_visual, projectile, resources, loaders, ui_read, the extra
// ui slots): "iface.slot(args)". Published only while
// publish_facade_interfaces is true, so no earlier test sees a table it did
// not expect.
inline std::vector<std::string> facade_calls;
inline bool publish_facade_interfaces = false;
// When set, the storage slots read and write a real map instead of the
// fixed probe answers, so a facade that round-trips values can be checked
// for what it stored. Off by default; Reset() clears both.
inline bool storage_map_enabled = false;
// The 11.20 client answers NOT_SUPPORTED from ui.toast_show; tests of the
// facade's overlay fallback flip this on.
inline bool toast_not_supported = false;
inline std::map<std::string, std::string> storage_map;
// input_subscriptions is declared after Reset(); it is drained lazily by
// the next InSubscribe when this flag is set.
inline bool input_subscriptions_reset_pending = false;
inline float gameplay_fov = 75.0f;

// The failure switch. Every mock function in every interface area checks
// this first (after recording its call, so Called() still sees it happened)
// and, when it is not OK, returns it immediately instead of doing its normal
// canned-success work. Without this, every mock slot only ever returns OK,
// so a binding's own failure path - a speculative push cut back by
// PushResultWith, a CheckArgTransaction refusal that never gets exercised
// because the ABI call it guards never fails - has nothing in the suite that
// ever takes it. One switch rather than a parameter threaded through every
// mock function, so Tasks 6 and 7 inherit it for events/ui without adding
// anything of their own.
inline WotbModV3Result forced_result = WOTBMOD_V3_OK;
inline void ForceFailure(WotbModV3Result result) { forced_result = result; }
inline void ClearFailure() { forced_result = WOTBMOD_V3_OK; }

// get_bytes's own two-call dance needs more than a single forced result to
// exercise: the heap tier, the growth-with-headroom retry, the "refused a
// buffer of the size it asked for" and "kept growing" failures, and the
// oversized-value refusal are all driven by a *sequence* of BUFFER_TOO_SMALL
// answers across several calls, which forced_result's one-shot shape cannot
// express. This is that sequence, selected per test rather than threaded
// through every call site.
enum class BytesMode {
    kNormal,           // the canned 5-byte value with an embedded zero, fits inline
    kLarge,            // a 4096-byte value, forces the malloc/heap tier
    kGrows,            // understates on the first probe, exercises the retry loop
    kNeverStabilizes,  // always claims more than it is given - exhausts all 4 attempts
    kRefusesSameSize,  // claims a size, then refuses a buffer of exactly that size
    kOversized,        // claims a size past the host's ceiling via BUFFER_TOO_SMALL
};
inline BytesMode bytes_mode = BytesMode::kNormal;
inline void SetBytesMode(BytesMode mode) { bytes_mode = mode; }

// begin_transaction's token. Fixed at 777 by default so a test can confirm
// the exact token a script received is the one that reaches every later
// transaction_* / commit / rollback call; settable to 0 so a test can prove
// PushToken does not treat a zero-valued token as WOTBMOD_V3_INVALID_HANDLE
// the way PushHandle would (see lua_convert.h's PushToken comment) - a mock
// hard-coded to a nonzero value could never see that bug.
inline WotbModV3Token next_transaction_token = 777u;
inline void SetTransactionToken(WotbModV3Token token) {
    next_transaction_token = token;
}

// The transactions the client believes are still open. Deliberately a list of
// tokens rather than a counter, and deliberately *not* a per-transaction
// unique token: next_transaction_token stays fixed so that every existing
// assertion of the form "storage.transaction_set_json(777,...)" keeps proving
// that the exact token a script received round-trips back through
// CheckArgTransaction. Two transactions open at once therefore both read 777
// here, which is enough for the only question this list is asked - how many
// are still open - and keeps the token value itself a stable, traceable
// constant.
//
// Task 8's own reason for existing: an open transaction is the third thing a
// script can leave behind, alongside a subscription and a control, and a
// begin_transaction that a script never commits is invisible to every
// assertion this suite had before.
inline std::vector<WotbModV3Token> open_transactions;

// ---------------------------------------------------------------------------
// The events area's own state. Declared before Reset() so it can clear it.
// ---------------------------------------------------------------------------

// One live subscription, as the client would hold it.
struct EventSubscriptionRecord {
    WotbModV3EventToken token = 0u;
    std::string pattern;
    int32_t priority = 0;
    uint32_t receive_system_events = 0u;
    WotbModV3EventCallback callback = nullptr;
    void* user_data = nullptr;
    bool live = false;
};

inline std::vector<EventSubscriptionRecord> event_subscriptions;
inline WotbModV3EventToken next_event_token = 9000u;

struct CapabilitySubscriptionRecord {
    WotbModV3Token token = 0u;
    WotbModV3CapabilityChangedCallback callback = nullptr;
    void* user_data = nullptr;
    bool live = false;
    uint32_t references = 0u;
};

inline std::vector<CapabilitySubscriptionRecord> capability_subscriptions;
inline WotbModV3Token next_capability_token = 9800u;

struct GenericHandleRecord {
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    uint32_t references = 0u;
};

inline std::vector<GenericHandleRecord> generic_handles;

struct MainDispatchRecord {
    WotbModV3Handle mod = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3TaskHandle task = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3DispatchCallback callback = nullptr;
    void* user_data = nullptr;
};

inline std::vector<MainDispatchRecord> main_dispatches;
inline WotbModV3Handle next_generic_handle = 9900u;

struct UiEventSubscriptionRecord {
    WotbModV3Token token = 0u;
    WotbModV3UiHandle control = WOTBMOD_V3_INVALID_HANDLE;
    uint32_t event_type = 0u;
    WotbModV3UiEventCallback callback = nullptr;
    void* user_data = nullptr;
    bool live = false;
    uint32_t references = 0u;
};

inline std::vector<UiEventSubscriptionRecord> ui_event_subscriptions;
inline WotbModV3Token next_ui_event_token = 9700u;

// The values FireEvent stamps on the event it dispatches. Each one is
// distinct and none of them is a plausible default, so a binding that read
// the timestamp where the context mask belongs answers with the wrong
// number rather than with a zero that would look like "not wired up yet".
inline uint32_t event_thread_role = WOTBMOD_V3_THREAD_MAIN;
inline uint64_t event_timestamp_ns = 1234567890123456789ull;
inline uint64_t event_context_mask = 0x00000000000000F4ull;
inline uint32_t event_flags = WOTBMOD_V3_EVENT_FLAG_STOPPABLE;
inline WotbModV3Token event_dispatch_token = 0xABCDEF01u;
inline std::string event_payload;
// wotbmod.ges state; the slots live further down beside the events mock.
inline std::vector<std::string> ges_calls;
inline const void* ges_current_payload = nullptr;
inline uint32_t ges_current_size = 0u;
inline std::vector<uint8_t> ges_last_published;
inline bool publish_core_context = false;
inline uint64_t core_context_mask = WOTBMOD_V3_CONTEXT_NONE;

// The mutual-exclusion witness, and the reason it lives out here in C++
// rather than in a Lua counter inside the state under test.
//
// "Two threads inside one lua_State" is the thing the host's lock forbids.
// A Lua-side counter can observe it - and does, in the threading test - but
// it is itself state in the very lua_State that is being corrupted, and in
// practice the process dies before the script can be asked what it saw. This
// counter is a plain C++ atomic in the test's own memory: whatever a second
// thread does to the VM, this still reads back.
//
// The span it measures is the *whole handler body*, not an instant inside
// one slot. EventsGetThread raises it and EventsGetContext lowers it, so a
// handler that calls get_thread first and get_context last is counted for as
// long as it is running. The first version raised and lowered inside
// get_thread alone; across 800 concurrent deliveries it never once observed
// an overlap, because a handler spends almost all of a delivery elsewhere.
// Measuring the body rather than one call is the difference between a
// witness and a decoration.
//
// Both ends are gated on witness_concurrency so that the counter stays
// balanced: every other test in the suite calls these slots independently
// and would otherwise leave it drifting.
//
// Its limits, so nobody credits it with more than it earns: with the host's
// exclusion removed, eight runs died on a hard fault, seven of them before
// the light phase finished. This catches a violation only when one is
// survivable. It is a backstop, not the detector - the detector is the
// crash. See the threading test's own comment.
inline std::atomic<int> deliveries_inside{0};
inline std::atomic<int> max_deliveries_inside{0};

// Set while the threading test is running, so the yield below - which exists
// to widen the window a violation would show up in - does not slow every
// other test that reads a thread role.
inline bool witness_concurrency = false;

// The gate: a way for a test to park a delivery *inside* the script's state
// and hold it there.
//
// A delivery holds the script lock from LuaScript::Entry until it returns,
// and everything the host does in between is out of a test's reach - except
// for the ABI calls the handler itself makes, which land back here. Blocking
// in one of those puts a delivery in the exact condition
// ReleaseEventSubscriptions' wait exists for: in flight, holding the script
// lock, with teardown wanting both. That is how the teardown ordering gets
// tested without a single line of instrumentation in the host.
//
// Armed for one delivery at a time. The first delivery to reach
// EventsGetThread with the gate armed disarms it, announces itself through
// gate_entered, and waits for gate_released.
inline std::mutex gate_mutex;
inline std::condition_variable gate_cv;
inline bool gate_armed = false;
inline bool gate_entered = false;
inline bool gate_released = false;

inline void ArmGate() {
    std::lock_guard<std::mutex> guard(gate_mutex);
    gate_armed = true;
    gate_entered = false;
    gate_released = false;
}

// Blocks until a delivery is parked inside the state. False means it never
// arrived and the caller must treat that as a failure rather than a wait.
//
// Bounded, and the bound is not belt and braces. gate_entered is set only from
// inside a delivery, so every reason a delivery might not happen - a refused
// subscribe, a handler that raised before the gate, a script that failed to
// compile - parks this thread forever. That is worse than the failure it hides:
// the process never exits, so it goes on holding wotbmod_lua_host.dll, and the
// *next* build fails at LNK1104 for a reason that has nothing to do with the
// change under test. The real signal is gone by the time anyone looks.
//
// That is not hypothetical here. Putting subscribe behind the permission fence
// created a whole new class of "no delivery ever happens", and the first bug in
// that fence - an upvalue ordering mistake that denied every call - hung two
// test processes exactly this way.
//
// Ten seconds for the same reason kPumpLimit is 2000: it is a ceiling on
// failure, not a delay. A delivery that is coming arrives in microseconds.
inline bool WaitForGateEntered(
    std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
    std::unique_lock<std::mutex> guard(gate_mutex);
    return gate_cv.wait_for(guard, timeout, []() { return gate_entered; });
}

inline void ReleaseGate() {
    {
        std::lock_guard<std::mutex> guard(gate_mutex);
        gate_armed = false;
        gate_released = true;
    }
    gate_cv.notify_all();
}

// ---------------------------------------------------------------------------
// The ui area's own state. Declared before Reset() so it can clear it.
//
// A control is a plain record this mock owns. UI event subscriptions below
// retain callbacks separately, just as the generated binding does.
// ---------------------------------------------------------------------------

// A live control, as the client's own registry would hold one. id,
// parent and position/size are the only fields any of the 18 bound slots
// can read back or act on - control_find_by_id searches by id and, when
// given a root, scopes that search to root's own descendants via parent;
// control_get_position/control_get_size round-trip whatever was last set -
// so nothing else about a real UiControl needs modelling here.
struct UiControlRecord {
    uint64_t handle = 0u;
    std::string id;
    std::string texture_uri;
    std::string text;
    bool live = false;
    bool game_owned = false;
    bool template_child = false;
    bool visible = true;
    uint32_t type = WOTBMOD_V3_UI_CONTROL_CONTAINER;
    // WOTBMOD_V3_INVALID_HANDLE (0) until control_add_child sets it and
    // control_remove_child clears it again - this mock's own stand-in for
    // the parent link UiSetParentCore maintains in client_services.cpp.
    uint64_t parent = 0u;
    WotbModV3Vec2 position = {0.0f, 0.0f};
    WotbModV3Vec2 size = {0.0f, 0.0f};
    WotbModV3Color color = {1.0f, 1.0f, 1.0f, 1.0f};
    float opacity = 1.0f;
    std::string font_uri;
    float font_size = 0.0f;
    uint32_t text_alignment = WOTBMOD_V3_UI_TEXT_ALIGN_LEFT;
    bool text_wrap = false;
    bool rich_text = false;
    bool enabled = true;
    bool interactable = true;
};

// 6000, not 0 or 1: distinct from next_transaction_token's 777 and
// next_event_token's 9000, so a binding that read the wrong counter answers
// with a wrong-looking number instead of a coincidentally plausible one.
inline uint64_t next_ui_control_handle = 6000u;
inline std::vector<UiControlRecord> ui_controls;

struct UiStyleOwnerRecord {
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3UiHandle control = WOTBMOD_V3_INVALID_HANDLE;
    bool live = false;
};

inline std::vector<UiStyleOwnerRecord> ui_style_owners;

// control_destroy marks a record dead rather than erasing it: id and the
// handle value both need to survive the destroy so a later call that names
// this same handle can still be answered - with a refusal, which is the
// entire point (see the handle-invalidation test in lua_host_tests.cpp).
inline UiControlRecord* FindUiControl(uint64_t handle) {
    for (UiControlRecord& record : ui_controls) {
        if (record.handle == handle && record.live) return &record;
    }
    return nullptr;
}

inline UiControlRecord* FindLatestUiControlById(const char* id) {
    if (!id) return nullptr;
    for (auto it = ui_controls.rbegin(); it != ui_controls.rend(); ++it) {
        if (it->live && it->id == id) return &*it;
    }
    return nullptr;
}

// The real UiControlFindById does not merely check that root is alive; it
// scopes the whole search to root's own descendants (UiIsDescendantLocked
// in client_services.cpp). This is this mock's own version of that walk,
// through the parent links control_add_child/control_remove_child maintain.
//
// root == WOTBMOD_V3_INVALID_HANDLE means "no scoping" - the whole-tree
// search a nil root asks for - so it is true unconditionally, matching
// UiControlFindById's own root != WOTBMOD_V3_INVALID_HANDLE guard before it
// resolves anything. Otherwise this walks candidate's own parent chain
// looking for root, bounded the same way UiSetParentCore bounds its own
// cycle check, so a corrupted chain cannot spin forever.
inline bool UiIsDescendant(uint64_t root, uint64_t candidate) {
    if (root == WOTBMOD_V3_INVALID_HANDLE) return true;
    uint64_t cursor = candidate;
    for (int depth = 0; depth < 64; ++depth) {
        if (cursor == root) return true;
        const UiControlRecord* record = FindUiControl(cursor);
        if (!record || record->parent == 0u) return false;
        cursor = record->parent;
    }
    return false;
}

// slot_find's own registry. kUiKnownSlotId is the one id this mock
// resolves - everything else fails, exactly as the real client's binding
// pack resolves none of its declared semantic slots today (see
// docs/API_V3_RU.md's own text on this: "slot_find для известного ID
// возвращает WOTBMOD_V3_E_NOT_SUPPORTED... неизвестный ID возвращает
// WOTBMOD_V3_E_NOT_FOUND"). kUiUnresolvedSlotId is a second, distinct id so
// a test can exercise that documented "declared but not resolved" path
// too, separately from a slot id nobody declared at all.
// The revocation-order witness.
//
// "Subscriptions first, so a callback cannot fire against a control that is
// already gone" is an ordering *between two interfaces*, and this mock keeps
// one record vector per interface - so the relative order of an
// events.unsubscribe and a ui.control_destroy is the one thing the recorded
// calls cannot show. Rather than merge the vectors (which would rewrite every
// assertion in the suite), UiControlDestroy asks the question directly at the
// moment it matters: how many subscriptions did the client still believe were
// live when a control was destroyed?
//
// Zero is the invariant, asserted per test rather than globally: it holds for
// a script that destroys no control of its own while it is running, which is
// what the "ownership_silent" case arranges. A RevokeAll that destroyed
// controls before it unsubscribed would leave this at the number of
// subscriptions the script held, and nothing else in the suite would notice.
//
// Atomic, with the same compare-exchange the delivery witness above uses.
// UiControlDestroy is reached from a delivery thread in the teardown-window
// test, and this is an ordering witness - a witness with a data race in it is
// not one. It cannot simply take RecordLock() instead: LiveSubscriptions()
// takes that lock itself and it is not recursive.
inline std::atomic<size_t> live_subscriptions_at_destroy{0u};

// The mod handle the last revocation of each kind carried.
//
// Every mock function ignores its WotbModV3Handle, so nothing in this suite
// could tell a revocation made with this script's own mod handle from one made
// with a zero or a stale value - and OwnershipRegistry passes a mod_ of its
// own, captured at Bind time, rather than reusing a binding's. These three
// close that gap where it matters: after a teardown that the script itself
// took no part in, each must carry the handle the host was loaded with.
//
// kNoMod rather than 0, so that "never called" is distinguishable from
// "called with a zero handle" - which is exactly the bug being looked for.
//
// Atomic, for the same reason live_subscriptions_at_destroy above is and in
// the same three functions. A WotbModV3Handle is 64 bits and this host is
// built for a 32-bit target, where a 64-bit store is two stores: a reader that
// caught one of them would see half of one handle joined to half of another -
// a value no caller ever passed. EventsUnsubscribe and UiControlDestroy are
// both reached from a delivery thread in the teardown-window test, so this is
// the ordinary case rather than a contrived one, and a witness with a data
// race in it is not a witness.
inline const WotbModV3Handle kNoMod = 0xFFFFFFFFFFFFFFFFull;
inline std::atomic<WotbModV3Handle> last_unsubscribe_mod{kNoMod};
inline std::atomic<WotbModV3Handle> last_control_destroy_mod{kNoMod};
inline std::atomic<WotbModV3Handle> last_rollback_mod{kNoMod};

// Which OS thread the last ui.control_create ran on.
//
// It exists for one question Task 9 has to answer and nothing else can: a
// reload must happen on the main thread, between frames, because destroying a
// lua_State from the watcher thread could free a state the render thread is
// currently inside. Nothing in a call record says which thread made the call,
// so a reload driven from the watcher thread would produce byte-identical
// ui_calls to one driven from on_frame. This is the difference, written down.
//
// control_create rather than some new slot, because every reload test already
// has the reloaded script create a control, so the witness costs no extra Lua.
//
// atomic<uint32_t>, for the same reason as its three neighbours above: a
// DWORD from GetCurrentThreadId, written from whichever thread ran the create
// and read from the test thread. A witness with a data race in it is not a
// witness, and this one is read specifically to decide whether the write came
// from the thread the test expects.
inline std::atomic<uint32_t> last_control_create_thread{0u};

inline const char kUiKnownSlotId[] = "wotb.mock_slot";
inline const char kUiUnresolvedSlotId[] = "wotb.mock_unresolved_slot";
inline uint64_t next_ui_slot_handle = 7000u;
inline std::vector<uint64_t> ui_slots;

// The record vectors are appended to from event callbacks, and this suite
// fires events from two threads at once on purpose. One mutex over the
// mock's own bookkeeping, so the thing under test is the host's locking and
// not the mock's absence of any.
inline std::mutex& RecordLock() {
    static std::mutex lock;
    return lock;
}

inline void Record(std::vector<std::string>& calls, std::string entry) {
    std::lock_guard<std::mutex> guard(RecordLock());
    calls.push_back(std::move(entry));
}

// Defined with the rest of the permissions mock further down; declared here
// because Reset() has to put the host's granted set back. A block that narrows
// the ceiling and forgets to restore it would otherwise leave every later test
// in the suite running against a fence it did not ask for - and the ones that
// would then fail are the ones that check a script *can* reach the ABI, which
// is a confusing way to find out.
inline void ResetHostPermissions();

inline void Reset() {
    ResetHostPermissions();
    core_calls.clear();
    handles_calls.clear();
    capabilities_calls.clear();
    storage_calls.clear();
    events_calls.clear();
    ui_calls.clear();
    vfs_calls.clear();
    async_calls.clear();
    hud_calls.clear();
    facade_calls.clear();
    storage_map_enabled = false;
    toast_not_supported = false;
    storage_map.clear();
    input_subscriptions_reset_pending = true;
    publish_facade_interfaces = false;
    gameplay_fov = 75.0f;
    forced_result = WOTBMOD_V3_OK;
    bytes_mode = BytesMode::kNormal;
    next_transaction_token = 777u;
    open_transactions.clear();
    event_subscriptions.clear();
    next_event_token = 9000u;
    capability_subscriptions.clear();
    next_capability_token = 9800u;
    generic_handles.clear();
    next_generic_handle = 9900u;
    main_dispatches.clear();
    ui_event_subscriptions.clear();
    next_ui_event_token = 9700u;
    event_thread_role = WOTBMOD_V3_THREAD_MAIN;
    event_timestamp_ns = 1234567890123456789ull;
    event_context_mask = 0x00000000000000F4ull;
    event_flags = WOTBMOD_V3_EVENT_FLAG_STOPPABLE;
    event_dispatch_token = 0xABCDEF01u;
    event_payload.clear();
    ges_calls.clear();
    ges_current_payload = nullptr;
    ges_current_size = 0u;
    ges_last_published.clear();
    publish_core_context = false;
    core_context_mask = WOTBMOD_V3_CONTEXT_NONE;
    deliveries_inside.store(0);
    max_deliveries_inside.store(0);
    witness_concurrency = false;
    {
        std::lock_guard<std::mutex> guard(gate_mutex);
        gate_armed = false;
        gate_entered = false;
        gate_released = false;
    }
    ui_controls.clear();
    ui_style_owners.clear();
    next_ui_control_handle = 6000u;
    ui_slots.clear();
    next_ui_slot_handle = 7000u;
    live_subscriptions_at_destroy.store(0u);
    last_unsubscribe_mod.store(kNoMod);
    last_control_destroy_mod.store(kNoMod);
    last_rollback_mod.store(kNoMod);
    last_control_create_thread.store(0u);
}

// How many subscriptions the client still believes it is holding. The
// assertion after a teardown: not "unsubscribe was called" - which a slot
// bound to the wrong token would also satisfy - but "the client is holding
// nothing that could still call back into a closed state".
inline size_t LiveSubscriptions() {
    std::lock_guard<std::mutex> guard(RecordLock());
    size_t count = 0u;
    for (const EventSubscriptionRecord& record : event_subscriptions) {
        if (record.live) ++count;
    }
    return count;
}

// The same question asked of the other two resource kinds a script can leave
// behind, and asked the same way: not "control_destroy was called" - which a
// slot bound to the wrong handle would also satisfy - but "the client is
// holding nothing this script created".
//
// No RecordLock here, unlike LiveSubscriptions above, and the difference is
// real rather than an oversight: every ui and storage slot in this mock
// mutates its own bookkeeping without that lock too (nothing in this suite
// drives ui or storage from more than one thread at a time - only events has
// a dispatcher that fires from two), so taking it here would be a lock no
// writer takes, which reads as protection that is not there.
inline size_t LiveControls() {
    size_t count = 0u;
    for (const UiControlRecord& record : ui_controls) {
        if (record.live && !record.game_owned && !record.template_child) {
            ++count;
        }
    }
    return count;
}

inline size_t LiveUiEventSubscriptions() {
    size_t count = 0u;
    for (const UiEventSubscriptionRecord& record : ui_event_subscriptions) {
        if (record.live) ++count;
    }
    return count;
}

inline size_t LiveTransactions() { return open_transactions.size(); }

inline bool Called(const std::vector<std::string>& calls, const char* record) {
    for (const std::string& entry : calls) {
        if (entry == record) return true;
    }
    return false;
}

// Called() asks for an exact record, and that is the right question almost
// everywhere in this file: a slot's arguments are the assertion, and a
// substring match over them would let a call with the wrong arguments stand in
// for the right one.
//
// This is for the one shape where the exact text is not the host's to promise:
// a message the host logs that carries Lua's own error string inside it
// ("hot:6: halfway"), whose wording belongs to the Lua release vendored under
// third_party. Pinning that exactly would turn an upstream bump into a failure
// in a test that is not about Lua's diagnostics. So the needle covers the part
// the host does promise - the level, the category, which script, and what the
// host decided happened to it - and stops where Lua's words begin.
// No RecordLock, deliberately, and matching Called/CountCalled above rather
// than differing from them. It took the lock when it was written, which was a
// trap rather than extra safety: RecordLock is a std::mutex and std::mutex is
// not recursive, so the first caller that reached this from inside a locked
// region - LiveSubscriptions holds it, and so does every mock slot that
// records - would deadlock the suite rather than fail it. All three readers are
// called from the test thread after the threads that write have been joined,
// which is what makes them safe; one of the three pretending otherwise made the
// rule harder to see, not the code safer.
inline bool CalledContaining(const std::vector<std::string>& calls,
                             const char* needle) {
    for (const std::string& entry : calls) {
        if (entry.find(needle) != std::string::npos) return true;
    }
    return false;
}

inline size_t CountCalled(const std::vector<std::string>& calls,
                          const char* record) {
    size_t count = 0u;
    for (const std::string& entry : calls) {
        if (entry == record) ++count;
    }
    return count;
}

// ---------------------------------------------------------------------------
// wotbmod.core - print plus representative generated scalar/string slots.
// ---------------------------------------------------------------------------

inline WotbModV3Result WOTBMOD_V3_CALL CoreLog(
    WotbModV3Handle, uint32_t level, const char* category,
    const char* message) {
    // Through Record(): the host logs a faulting event callback here, and a
    // callback can be running on two threads at once in this suite.
    Record(core_calls, std::string("core.log(") + std::to_string(level) + "," +
                           (category ? category : "") + "," +
                           (message ? message : "") + ")");
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL CoreGetFrameIndex(
    WotbModV3Handle, uint64_t* out_frame_index) {
    Record(core_calls, "core.get_frame_index()");
    if (!out_frame_index) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    *out_frame_index = 424242u;
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL CoreGetContext(
    WotbModV3Handle, uint64_t* out_context_mask) {
    Record(core_calls, "core.get_context()");
    if (!out_context_mask) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    *out_context_mask = core_context_mask;
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL CoreGetGameDirectory(
    WotbModV3Handle, char* buffer, uint32_t* inout_size) {
    Record(core_calls, "core.get_game_directory()");
    if (!inout_size) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    static const char kPath[] = "C:\\mock-game";
    const uint32_t needed = static_cast<uint32_t>(sizeof(kPath));
    if (!buffer || *inout_size < needed) {
        *inout_size = needed;
        return WOTBMOD_V3_E_BUFFER_TOO_SMALL;
    }
    std::memcpy(buffer, kPath, needed);
    *inout_size = needed;
    return WOTBMOD_V3_OK;
}

inline WotbModV3CoreApiV1& CoreApi() {
    static WotbModV3CoreApiV1 api = {};
    api.struct_size = sizeof(api);
    api.api_version = WOTBMOD_V3_CORE_VERSION;
    api.log = &CoreLog;
    api.get_context = publish_core_context ? &CoreGetContext : nullptr;
    api.get_frame_index = &CoreGetFrameIndex;
    api.get_game_directory = &CoreGetGameDirectory;
    return api;
}

// ---------------------------------------------------------------------------
// Generated callback proof: capabilities.subscribe owns a token whose generic
// handle release must make it impossible for the client to call closed Lua.
// ---------------------------------------------------------------------------

inline WotbModV3Result WOTBMOD_V3_CALL CapabilitiesSubscribe(
    WotbModV3Handle, WotbModV3CapabilityChangedCallback callback,
    void* user_data, WotbModV3Token* out_token) {
    Record(capabilities_calls, "capabilities.subscribe()");
    if (!callback || !out_token) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    CapabilitySubscriptionRecord record = {};
    record.token = next_capability_token++;
    record.callback = callback;
    record.user_data = user_data;
    record.live = true;
    record.references = 1u;
    capability_subscriptions.push_back(record);
    *out_token = record.token;
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL CapabilitiesUnsubscribe(
    WotbModV3Handle, WotbModV3Token token) {
    Record(capabilities_calls, std::string("capabilities.unsubscribe(") +
                                   std::to_string(token) + ")");
    for (CapabilitySubscriptionRecord& record : capability_subscriptions) {
        if (record.token == token && record.live) {
            record.live = false;
            record.references = 0u;
            return WOTBMOD_V3_OK;
        }
    }
    return WOTBMOD_V3_E_NOT_FOUND;
}

// Capability records the way the runtime keeps them: named after the
// runtime's own capability names ("gameplay.tweak.hud"), never after an
// interface id - on the live client an interface id is not a capability and
// query answers E_NOT_FOUND for it, which is what a facade must pass on.
struct MockCapability {
    const char* name;
    uint32_t status;
    const char* reason;
};

inline const MockCapability kMockCapabilities[] = {
    {"gameplay.tweak.hud", WOTBMOD_V3_CAPABILITY_AVAILABLE, ""},
    {"gameplay.tweak.camera", WOTBMOD_V3_CAPABILITY_DEGRADED,
     "FOV only; zoom and freecam remain unsupported"},
};

inline void FillMockCapability(const MockCapability& source,
                               WotbModV3CapabilityInfo* out_info) {
    const uint32_t struct_size = out_info->struct_size;
    const uint32_t api_version = out_info->api_version;
    std::memset(out_info, 0, sizeof(*out_info));
    out_info->struct_size = struct_size;
    out_info->api_version = api_version;
    out_info->interface_version = 1u;
    out_info->status = source.status;
    strncpy_s(out_info->name, source.name, _TRUNCATE);
    strncpy_s(out_info->unavailable_reason, source.reason, _TRUNCATE);
}

inline WotbModV3Result WOTBMOD_V3_CALL CapabilitiesGetCount(
    WotbModV3Handle, uint32_t* out_count) {
    if (!out_count) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    *out_count = static_cast<uint32_t>(sizeof(kMockCapabilities) /
                                       sizeof(kMockCapabilities[0]));
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL CapabilitiesGetAt(
    WotbModV3Handle, uint32_t index, WotbModV3CapabilityInfo* out_info) {
    if (!out_info) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    if (index >= sizeof(kMockCapabilities) / sizeof(kMockCapabilities[0])) {
        return WOTBMOD_V3_E_NOT_FOUND;
    }
    FillMockCapability(kMockCapabilities[index], out_info);
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL CapabilitiesQuery(
    WotbModV3Handle, const char* capability_name,
    WotbModV3CapabilityInfo* out_info) {
    if (!capability_name || !out_info) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    Record(capabilities_calls,
           std::string("capabilities.query(") + capability_name + ")");
    for (const MockCapability& candidate : kMockCapabilities) {
        if (_stricmp(candidate.name, capability_name) == 0) {
            FillMockCapability(candidate, out_info);
            return WOTBMOD_V3_OK;
        }
    }
    return WOTBMOD_V3_E_NOT_FOUND;
}

inline WotbModV3CapabilitiesApiV1& CapabilitiesApi() {
    static WotbModV3CapabilitiesApiV1 api = {};
    api.struct_size = sizeof(api);
    api.api_version = WOTBMOD_V3_CAPABILITIES_VERSION;
    api.get_count = &CapabilitiesGetCount;
    api.get_at = &CapabilitiesGetAt;
    api.query = &CapabilitiesQuery;
    api.subscribe = &CapabilitiesSubscribe;
    api.unsubscribe = &CapabilitiesUnsubscribe;
    return api;
}

inline WotbModV3Result WOTBMOD_V3_CALL HandlesRelease(
    WotbModV3Handle, WotbModV3Handle handle) {
    Record(handles_calls,
           std::string("handles.release(") + std::to_string(handle) + ")");
    for (CapabilitySubscriptionRecord& record : capability_subscriptions) {
        if (record.token == handle && record.references) {
            --record.references;
            record.live = record.references != 0u;
            return WOTBMOD_V3_OK;
        }
    }
    for (UiEventSubscriptionRecord& record : ui_event_subscriptions) {
        if (record.token == handle && record.references) {
            --record.references;
            record.live = record.references != 0u;
            return WOTBMOD_V3_OK;
        }
    }
    for (GenericHandleRecord& record : generic_handles) {
        if (record.handle == handle && record.references) {
            --record.references;
            return WOTBMOD_V3_OK;
        }
    }
    return WOTBMOD_V3_E_NOT_FOUND;
}

inline WotbModV3Result WOTBMOD_V3_CALL HandlesRetain(
    WotbModV3Handle, WotbModV3Handle handle) {
    Record(handles_calls,
           std::string("handles.retain(") + std::to_string(handle) + ")");
    for (CapabilitySubscriptionRecord& record : capability_subscriptions) {
        if (record.token == handle && record.references) {
            ++record.references;
            return WOTBMOD_V3_OK;
        }
    }
    for (GenericHandleRecord& record : generic_handles) {
        if (record.handle == handle && record.references) {
            ++record.references;
            return WOTBMOD_V3_OK;
        }
    }
    return WOTBMOD_V3_E_NOT_FOUND;
}

inline WotbModV3Result WOTBMOD_V3_CALL HandlesIsAlive(
    WotbModV3Handle, WotbModV3Handle handle, uint32_t* out_alive) {
    if (!out_alive) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    *out_alive = 0u;
    for (const CapabilitySubscriptionRecord& record : capability_subscriptions) {
        if (record.token == handle && record.references) *out_alive = 1u;
    }
    for (const GenericHandleRecord& record : generic_handles) {
        if (record.handle == handle && record.references) *out_alive = 1u;
    }
    return WOTBMOD_V3_OK;
}

inline WotbModV3HandlesApiV1& HandlesApi() {
    static WotbModV3HandlesApiV1 api = {};
    api.struct_size = sizeof(api);
    api.api_version = WOTBMOD_V3_HANDLES_VERSION;
    api.retain = &HandlesRetain;
    api.release = &HandlesRelease;
    api.is_alive = &HandlesIsAlive;
    return api;
}

inline WotbModV3Result WOTBMOD_V3_CALL AsyncDispatchMain(
    WotbModV3Handle mod,
    WotbModV3DispatchCallback callback,
    void* user_data,
    WotbModV3TaskHandle* out_task) {
    Record(async_calls, "async.dispatch_to_main_thread()");
    if (!callback || !out_task) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    const WotbModV3TaskHandle task = next_generic_handle++;
    generic_handles.push_back({task, 1u});
    main_dispatches.push_back({mod, task, callback, user_data});
    *out_task = task;
    return WOTBMOD_V3_OK;
}

inline WotbModV3AsyncApiV1& AsyncApi() {
    static WotbModV3AsyncApiV1 api = {};
    api.struct_size = sizeof(api);
    api.api_version = WOTBMOD_V3_ASYNC_VERSION;
    api.dispatch_to_main_thread = &AsyncDispatchMain;
    return api;
}

inline uint32_t PumpMainDispatch(uint32_t max_callbacks = 64u) {
    uint32_t executed = 0u;
    while (executed < max_callbacks && !main_dispatches.empty()) {
        const MainDispatchRecord record = main_dispatches.front();
        main_dispatches.erase(main_dispatches.begin());
        if (record.callback) {
            record.callback(record.mod, record.user_data);
            ++executed;
        }
    }
    return executed;
}

inline size_t LiveGenericHandles() {
    size_t count = 0u;
    for (const GenericHandleRecord& record : generic_handles) {
        if (record.references != 0u) ++count;
    }
    return count;
}

inline size_t LiveCapabilitySubscriptions() {
    size_t count = 0u;
    for (const CapabilitySubscriptionRecord& record : capability_subscriptions) {
        if (record.live) ++count;
    }
    return count;
}

inline void FireCapability() {
    WotbModV3CapabilityInfo capability = {};
    WOTBMOD_V3_INIT_STRUCT(capability, WOTBMOD_V3_CAPABILITIES_VERSION);
    capability.interface_version = 7u;
    capability.status = WOTBMOD_V3_CAPABILITY_AVAILABLE;
    capability.allowed_contexts = 0x52u;
    capability.permission_tier = WOTBMOD_V3_PERMISSION_REVIEWED;
    strncpy_s(capability.name, "mock.capability", _TRUNCATE);
    for (const CapabilitySubscriptionRecord& record : capability_subscriptions) {
        if (record.live && record.callback) {
            record.callback(1u, &capability, record.user_data);
        }
    }
}

inline WotbModV3Result WOTBMOD_V3_CALL LifecycleGetCurrent(
    WotbModV3Handle, WotbModV3Handle* out_current_mod) {
    if (!out_current_mod) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    GenericHandleRecord record = {};
    record.handle = next_generic_handle++;
    record.references = 1u;
    generic_handles.push_back(record);
    *out_current_mod = record.handle;
    return WOTBMOD_V3_OK;
}

inline WotbModV3LifecycleApiV1& LifecycleApi() {
    static WotbModV3LifecycleApiV1 api = {};
    api.struct_size = sizeof(api);
    api.api_version = WOTBMOD_V3_LIFECYCLE_VERSION;
    api.get_current = &LifecycleGetCurrent;
    return api;
}

inline uint32_t GenericHandleReferences(WotbModV3Handle handle) {
    for (const GenericHandleRecord& record : generic_handles) {
        if (record.handle == handle) return record.references;
    }
    return 0u;
}

// ---------------------------------------------------------------------------
// wotbmod.storage - all 14 slots of WotbModV3StorageApiV1.
// ---------------------------------------------------------------------------

inline WotbModV3Result WOTBMOD_V3_CALL StorageSetJson(
    WotbModV3Handle, const char* key, const char* json) {
    storage_calls.push_back(std::string("storage.set_json(") + (key ? key : "") +
                      "," + (json ? json : "") + ")");
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    if (storage_map_enabled && key) storage_map[key] = json ? json : "";
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL StorageGetJson(
    WotbModV3Handle, const char* key, char* buffer, uint32_t* inout_size) {
    storage_calls.push_back(std::string("storage.get_json(") + (key ? key : "") + ")");
    if (!inout_size) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    if (storage_map_enabled) {
        const auto found = key ? storage_map.find(key) : storage_map.end();
        if (found == storage_map.end()) return WOTBMOD_V3_E_NOT_FOUND;
        const uint32_t required = static_cast<uint32_t>(found->second.size() + 1u);
        if (!buffer || *inout_size < required) {
            *inout_size = required;
            return WOTBMOD_V3_E_BUFFER_TOO_SMALL;
        }
        std::memcpy(buffer, found->second.c_str(), required);
        *inout_size = required;
        return WOTBMOD_V3_OK;
    }
    static const char kValue[] = "{\"probe\":1}";
    const uint32_t needed = static_cast<uint32_t>(sizeof(kValue));
    if (!buffer || *inout_size < needed) {
        *inout_size = needed;
        return WOTBMOD_V3_E_BUFFER_TOO_SMALL;
    }
    std::memcpy(buffer, kValue, needed);
    *inout_size = needed;
    return WOTBMOD_V3_OK;
}

// The canned "normal" value carries a genuine embedded NUL byte ("AB\0CD",
// 5 bytes). That is the point of the mock: a get_bytes routed through the
// string helper's BoundedLength would silently come back as "AB" (2 bytes),
// so a test that reads all 5 bytes back is proof the byte-buffer path
// really is byte-safe rather than merely typed differently.
//
// bytes_mode selects which of six scenarios this call plays out - see the
// enum's own comment. forced_result, checked first, still overrides all of
// them: a hard ABI failure (permission denied, say) does not go through the
// buffer-too-small dance at all, on the real ABI or here.
inline WotbModV3Result WOTBMOD_V3_CALL StorageGetBytes(
    WotbModV3Handle, const char* key, WotbModV3Buffer* inout_buffer) {
    storage_calls.push_back(std::string("storage.get_bytes(") + (key ? key : "") + ")");
    if (!inout_buffer) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    if (forced_result != WOTBMOD_V3_OK) return forced_result;

    static const char kValue[] = {'A', 'B', '\0', 'C', 'D'};   // 5 bytes
    static const std::string kLargeValue(4096u, 'x');
    static const std::string kGrownValue(2500u, 'g');

    switch (bytes_mode) {
        case BytesMode::kNormal: {
            const uint32_t needed = static_cast<uint32_t>(sizeof(kValue));
            if (!inout_buffer->data || inout_buffer->capacity < needed) {
                inout_buffer->size = needed;
                return WOTBMOD_V3_E_BUFFER_TOO_SMALL;
            }
            std::memcpy(inout_buffer->data, kValue, needed);
            inout_buffer->size = needed;
            return WOTBMOD_V3_OK;
        }
        case BytesMode::kLarge: {
            const uint32_t needed = static_cast<uint32_t>(kLargeValue.size());
            if (!inout_buffer->data || inout_buffer->capacity < needed) {
                inout_buffer->size = needed;
                return WOTBMOD_V3_E_BUFFER_TOO_SMALL;
            }
            std::memcpy(inout_buffer->data, kLargeValue.data(), needed);
            inout_buffer->size = needed;
            return WOTBMOD_V3_OK;
        }
        case BytesMode::kGrows: {
            // Understates on the first probe (claims 1024 against the
            // caller's 512-byte inline capacity), then reports the true,
            // larger size once given a buffer of at least 1024 - the same
            // "grows between the probe and the read" shape
            // lua_convert_tests.cpp proves for FetchSizedString, now
            // exercised through FetchByteBuffer specifically.
            const uint32_t true_needed =
                static_cast<uint32_t>(kGrownValue.size());
            const uint32_t claim =
                inout_buffer->capacity < 1024u ? 1024u : true_needed;
            if (!inout_buffer->data || inout_buffer->capacity < claim) {
                inout_buffer->size = claim;
                return WOTBMOD_V3_E_BUFFER_TOO_SMALL;
            }
            std::memcpy(inout_buffer->data, kGrownValue.data(), true_needed);
            inout_buffer->size = true_needed;
            return WOTBMOD_V3_OK;
        }
        case BytesMode::kNeverStabilizes:
            // Always claims more than it was just given, so the retry loop
            // exhausts all four attempts and gives up.
            inout_buffer->size = inout_buffer->capacity + 1024u;
            return WOTBMOD_V3_E_BUFFER_TOO_SMALL;
        case BytesMode::kRefusesSameSize:
            // Claims exactly the capacity it was just handed - the buffer
            // it then refuses is the very size it asked for.
            inout_buffer->size = inout_buffer->capacity;
            return WOTBMOD_V3_E_BUFFER_TOO_SMALL;
        case BytesMode::kOversized:
            inout_buffer->size = 0x7FFFFFFFu;
            return WOTBMOD_V3_E_BUFFER_TOO_SMALL;
    }
    return WOTBMOD_V3_E_INVALID_ARGUMENT;   // unreachable; silences /W4
}

inline WotbModV3Result WOTBMOD_V3_CALL StorageSetBytes(
    WotbModV3Handle, const char* key, const WotbModV3ConstBuffer* value) {
    std::string payload;
    if (value && value->data && value->size) {
        payload.assign(static_cast<const char*>(value->data), value->size);
    }
    storage_calls.push_back(std::string("storage.set_bytes(") + (key ? key : "") +
                      "," + payload + ")");
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL StorageErase(
    WotbModV3Handle, const char* key) {
    storage_calls.push_back(std::string("storage.erase(") + (key ? key : "") + ")");
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    if (storage_map_enabled && key) storage_map.erase(key);
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL StorageContains(
    WotbModV3Handle, const char* key, uint32_t* out_contains) {
    storage_calls.push_back(std::string("storage.contains(") + (key ? key : "") + ")");
    if (!out_contains) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    if (storage_map_enabled) {
        *out_contains = (key && storage_map.count(key)) ? 1u : 0u;
        return WOTBMOD_V3_OK;
    }
    *out_contains = (key && std::strcmp(key, "present") == 0) ? 1u : 0u;
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL StorageFlush(WotbModV3Handle) {
    storage_calls.push_back("storage.flush()");
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    return WOTBMOD_V3_OK;
}

// next_transaction_token, not a hard-coded 777: settable to 0 so a test can
// prove PushToken boxes it anyway (a mock that could only ever return a
// nonzero token would never be able to see PushHandle's zero-is-invalid
// assumption misapplied to one). Defaults to 777 for every other test, so a
// script's received token can be traced through every later call.
inline WotbModV3Result WOTBMOD_V3_CALL StorageBeginTransaction(
    WotbModV3Handle, WotbModV3Token* out_transaction) {
    storage_calls.push_back("storage.begin_transaction()");
    if (!out_transaction) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    *out_transaction = next_transaction_token;
    open_transactions.push_back(next_transaction_token);
    return WOTBMOD_V3_OK;
}

// Closes one open transaction carrying this token, if there is one. Shared by
// commit and rollback - both are terminal for a transaction, and the client
// stops holding it either way.
//
// It answers nothing: an unknown token is left to return WOTBMOD_V3_OK
// exactly as it did before this list existed, so that no assertion written
// against the old mock changes meaning. The list is here to be counted by
// LiveTransactions(), not to grow a second liveness rule.
inline void CloseTransaction(WotbModV3Token transaction) {
    for (auto it = open_transactions.begin(); it != open_transactions.end();
         ++it) {
        if (*it == transaction) {
            open_transactions.erase(it);
            return;
        }
    }
}

inline WotbModV3Result WOTBMOD_V3_CALL StorageTransactionSetJson(
    WotbModV3Handle, WotbModV3Token transaction, const char* key,
    const char* json) {
    storage_calls.push_back(std::string("storage.transaction_set_json(") +
                      std::to_string(transaction) + "," + (key ? key : "") +
                      "," + (json ? json : "") + ")");
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL StorageTransactionSetBytes(
    WotbModV3Handle, WotbModV3Token transaction, const char* key,
    const WotbModV3ConstBuffer* value) {
    std::string payload;
    if (value && value->data && value->size) {
        payload.assign(static_cast<const char*>(value->data), value->size);
    }
    storage_calls.push_back(std::string("storage.transaction_set_bytes(") +
                      std::to_string(transaction) + "," + (key ? key : "") +
                      "," + payload + ")");
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL StorageTransactionErase(
    WotbModV3Handle, WotbModV3Token transaction, const char* key) {
    storage_calls.push_back(std::string("storage.transaction_erase(") +
                      std::to_string(transaction) + "," + (key ? key : "") + ")");
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL StorageCommit(
    WotbModV3Handle, WotbModV3Token transaction) {
    storage_calls.push_back(std::string("storage.commit(") +
                      std::to_string(transaction) + ")");
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    CloseTransaction(transaction);
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL StorageRollback(
    WotbModV3Handle mod, WotbModV3Token transaction) {
    last_rollback_mod.store(mod);
    storage_calls.push_back(std::string("storage.rollback(") +
                      std::to_string(transaction) + ")");
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    CloseTransaction(transaction);
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL StorageGetPath(
    WotbModV3Handle, uint32_t path_kind, char* buffer, uint32_t* inout_size) {
    storage_calls.push_back(std::string("storage.get_path(") +
                      std::to_string(path_kind) + ")");
    if (!inout_size) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    const char* value = "C:/mods/data";
    switch (path_kind) {
        case WOTBMOD_V3_STORAGE_PATH_CONFIG: value = "C:/mods/config"; break;
        case WOTBMOD_V3_STORAGE_PATH_CACHE: value = "C:/mods/cache"; break;
        case WOTBMOD_V3_STORAGE_PATH_TEMP: value = "C:/mods/temp"; break;
        default: break;
    }
    const uint32_t needed = static_cast<uint32_t>(std::strlen(value)) + 1u;
    if (!buffer || *inout_size < needed) {
        *inout_size = needed;
        return WOTBMOD_V3_E_BUFFER_TOO_SMALL;
    }
    std::memcpy(buffer, value, needed);
    *inout_size = needed;
    return WOTBMOD_V3_OK;
}

inline WotbModV3StorageApiV1& StorageApi() {
    static WotbModV3StorageApiV1 api = {};
    api.struct_size = sizeof(api);
    api.api_version = WOTBMOD_V3_STORAGE_VERSION;
    api.get_json = &StorageGetJson;
    api.set_json = &StorageSetJson;
    api.get_bytes = &StorageGetBytes;
    api.set_bytes = &StorageSetBytes;
    api.erase = &StorageErase;
    api.contains = &StorageContains;
    api.flush = &StorageFlush;
    api.begin_transaction = &StorageBeginTransaction;
    api.transaction_set_json = &StorageTransactionSetJson;
    api.transaction_set_bytes = &StorageTransactionSetBytes;
    api.transaction_erase = &StorageTransactionErase;
    api.commit = &StorageCommit;
    api.rollback = &StorageRollback;
    api.get_path = &StorageGetPath;
    return api;
}

// ---------------------------------------------------------------------------
// wotbmod.events - all 9 slots of WotbModV3EventsApiV1, plus the dispatcher
// a real client would have. This is the only interface area that calls back
// *into* the host, so the mock has to be a small event bus rather than a set
// of recorders: a binding that stored the callback but never let it be
// invoked would pass a recorder-only mock.
// ---------------------------------------------------------------------------

// The event currently being delivered on *this* thread. Per-thread because
// FireEvent is deliberately called from two threads at once, and the four
// dispatch readers below have to answer about the dispatch their caller is
// inside, not about whichever one started most recently.
inline thread_local WotbModV3Event* current_event = nullptr;

inline WotbModV3Result WOTBMOD_V3_CALL EventsSubscribe(
    WotbModV3Handle, const WotbModV3EventSubscriptionInfo* info,
    WotbModV3EventCallback callback, void* user_data,
    WotbModV3EventToken* out_token) {
    if (!info || !callback || !out_token) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    Record(events_calls,
           std::string("events.subscribe(") +
               (info->topic_pattern ? info->topic_pattern : "") + "," +
               std::to_string(info->priority) + "," +
               std::to_string(info->receive_system_events) + ")");
    if (forced_result != WOTBMOD_V3_OK) return forced_result;

    std::lock_guard<std::mutex> guard(RecordLock());
    EventSubscriptionRecord record;
    record.token = next_event_token++;
    record.pattern = info->topic_pattern ? info->topic_pattern : "";
    record.priority = info->priority;
    record.receive_system_events = info->receive_system_events;
    record.callback = callback;
    record.user_data = user_data;
    record.live = true;
    event_subscriptions.push_back(record);
    *out_token = record.token;
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL EventsUnsubscribe(
    WotbModV3Handle mod, WotbModV3EventToken token) {
    last_unsubscribe_mod.store(mod);
    Record(events_calls,
           std::string("events.unsubscribe(") + std::to_string(token) + ")");
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    std::lock_guard<std::mutex> guard(RecordLock());
    for (EventSubscriptionRecord& record : event_subscriptions) {
        if (record.token == token && record.live) {
            record.live = false;
            return WOTBMOD_V3_OK;
        }
    }
    return WOTBMOD_V3_E_INVALID_HANDLE;
}

inline WotbModV3Result WOTBMOD_V3_CALL EventsSetPriority(
    WotbModV3Handle, WotbModV3EventToken token, int32_t priority) {
    Record(events_calls, std::string("events.set_priority(") +
                             std::to_string(token) + "," +
                             std::to_string(priority) + ")");
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    std::lock_guard<std::mutex> guard(RecordLock());
    for (EventSubscriptionRecord& record : event_subscriptions) {
        if (record.token == token && record.live) {
            // Really applied, not merely recorded: FireEvent below dispatches
            // in priority order, so a set_priority bound to the wrong slot
            // changes the order handlers run in and the test sees it.
            record.priority = priority;
            return WOTBMOD_V3_OK;
        }
    }
    return WOTBMOD_V3_E_INVALID_HANDLE;
}

inline void FireEvent(const char* topic);

inline WotbModV3Result WOTBMOD_V3_CALL EventsPost(
    WotbModV3Handle, const char* topic, const void* payload,
    uint32_t payload_size, uint32_t flags) {
    std::string body;
    if (payload && payload_size) {
        body.assign(static_cast<const char*>(payload), payload_size);
    }
    Record(events_calls, std::string("events.post(") + (topic ? topic : "") +
                             "," + body + "," + std::to_string(flags) + ")");
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    // A real client's post() reaches that client's own subscribers. Doing
    // the same here is what lets one test prove a posted payload arrives at
    // a Lua handler byte-for-byte, rather than only that post() was called.
    const uint32_t previous_flags = event_flags;
    const std::string previous_payload = event_payload;
    event_flags = flags;
    event_payload = body;
    FireEvent(topic);
    event_flags = previous_flags;
    event_payload = previous_payload;
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL EventsStopPropagation(
    WotbModV3Handle, WotbModV3Token dispatch_token) {
    Record(events_calls, std::string("events.stop_propagation(") +
                             std::to_string(dispatch_token) + ")");
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    if (!current_event || current_event->dispatch_token != dispatch_token) {
        return WOTBMOD_V3_E_INVALID_HANDLE;
    }
    // Answered synchronously, on the event the handler is inside. This is
    // the promise that ruled out marshalling deliveries to the main thread.
    current_event->propagation_stopped = 1u;
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL EventsGetDispatchInfo(
    WotbModV3Handle, WotbModV3Token dispatch_token,
    WotbModV3EventDispatchInfo* out_info) {
    Record(events_calls, std::string("events.get_dispatch_info(") +
                             std::to_string(dispatch_token) + ")");
    if (!out_info) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    if (!current_event || current_event->dispatch_token != dispatch_token) {
        return WOTBMOD_V3_E_INVALID_HANDLE;
    }
    out_info->dispatch_token = current_event->dispatch_token;
    out_info->timestamp_ns = current_event->timestamp_ns;
    out_info->context_mask = current_event->context_mask;
    out_info->thread_role = current_event->thread_role;
    out_info->flags = current_event->flags;
    out_info->propagation_stopped = current_event->propagation_stopped;
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL EventsGetThread(
    WotbModV3Handle, WotbModV3Token dispatch_token, uint32_t* out_thread_role) {
    // The opening end of the witness span: the handler under test calls this
    // first and get_context last, so the counter is raised for the whole of
    // the handler body. Closed in EventsGetContext, never here.
    if (witness_concurrency) {
        const int now = deliveries_inside.fetch_add(1) + 1;
        int high = max_deliveries_inside.load();
        while (high < now &&
               !max_deliveries_inside.compare_exchange_weak(high, now)) {
        }
        if (now > 1) {
            // Reported here, at the moment of the violation, and not left
            // for main() to notice afterwards. Two threads inside one
            // lua_State is memory corruption: the usual next event is the
            // process dying, and an assertion at the end of the run is then
            // never reached. This line is already on an unbuffered stream by
            // then, so the run says what went wrong rather than only that it
            // stopped.
            std::fprintf(stderr,
                         "FAIL: %d event deliveries were inside one lua_State "
                         "at once\n",
                         now);
            std::fflush(stderr);
        }
        // Widens the window. Without it two threads could be inside the same
        // state for so short an overlap that neither observes the other, and
        // a witness that only sometimes witnesses is not one.
        std::this_thread::yield();
    }

    // The gate. Parks this delivery inside the state, holding the script
    // lock, until the test lets it go.
    {
        std::unique_lock<std::mutex> guard(gate_mutex);
        if (gate_armed) {
            gate_armed = false;
            gate_entered = true;
            gate_cv.notify_all();
            // Bounded for the same reason WaitForGateEntered is, and it is the
            // same hazard seen from the other side: a test that arms the gate
            // and then leaves without releasing it - because an assertion
            // above the release failed, or an early return was added later -
            // parks this delivery forever while it holds the script lock. The
            // process would then never exit and would go on holding the host
            // DLL, so the next build fails at LNK1104 about something
            // unrelated. Timing out and carrying on turns that into ordinary
            // failing assertions, which is the outcome a reader can act on.
            gate_cv.wait_for(guard, std::chrono::seconds(10),
                             []() { return gate_released; });
        }
    }

    Record(events_calls, std::string("events.get_thread(") +
                             std::to_string(dispatch_token) + ")");
    if (!out_thread_role) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    if (!current_event || current_event->dispatch_token != dispatch_token) {
        return WOTBMOD_V3_E_INVALID_HANDLE;
    }
    *out_thread_role = current_event->thread_role;
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL EventsGetTimestamp(
    WotbModV3Handle, WotbModV3Token dispatch_token, uint64_t* out_timestamp_ns) {
    Record(events_calls, std::string("events.get_timestamp(") +
                             std::to_string(dispatch_token) + ")");
    if (!out_timestamp_ns) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    if (!current_event || current_event->dispatch_token != dispatch_token) {
        return WOTBMOD_V3_E_INVALID_HANDLE;
    }
    *out_timestamp_ns = current_event->timestamp_ns;
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL EventsGetContext(
    WotbModV3Handle, WotbModV3Token dispatch_token, uint64_t* out_context_mask) {
    // The closing end of the witness span opened in EventsGetThread. A
    // handler that calls get_thread first and this last is counted for its
    // whole body.
    struct Leave {
        ~Leave() {
            if (witness_concurrency) deliveries_inside.fetch_sub(1);
        }
    } leave;

    Record(events_calls, std::string("events.get_context(") +
                             std::to_string(dispatch_token) + ")");
    if (!out_context_mask) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    if (!current_event || current_event->dispatch_token != dispatch_token) {
        return WOTBMOD_V3_E_INVALID_HANDLE;
    }
    *out_context_mask = current_event->context_mask;
    return WOTBMOD_V3_OK;
}

// Matches the ABI's own rule: an exact topic, or a prefix ending in '*'.
inline bool PatternMatches(const std::string& pattern, const char* topic) {
    if (!topic) return false;
    if (!pattern.empty() && pattern.back() == '*') {
        return std::strncmp(pattern.c_str(), topic, pattern.size() - 1u) == 0;
    }
    return pattern == topic;
}

// What the client's dispatcher does: build the event, walk the live
// subscriptions in priority order (highest first), and stop the moment one
// of them says so.
//
// Callable from any thread, and deliberately called from two at once by the
// threading test. The subscription list is copied under the lock and the
// callbacks are invoked outside it: a callback that subscribes or
// unsubscribes re-enters this mock, and holding the lock across it would
// deadlock the mock rather than exercise the host.
inline void FireEvent(const char* topic) {
    std::vector<EventSubscriptionRecord> targets;
    {
        std::lock_guard<std::mutex> guard(RecordLock());
        for (const EventSubscriptionRecord& record : event_subscriptions) {
            if (record.live && PatternMatches(record.pattern, topic)) {
                targets.push_back(record);
            }
        }
    }
    std::stable_sort(targets.begin(), targets.end(),
                     [](const EventSubscriptionRecord& left,
                        const EventSubscriptionRecord& right) {
                         return left.priority > right.priority;
                     });

    WotbModV3Event event = {};
    WOTBMOD_V3_INIT_STRUCT(event, WOTBMOD_V3_EVENTS_VERSION);
    event.dispatch_token = event_dispatch_token;
    event.publisher_mod = 4321u;
    event.timestamp_ns = event_timestamp_ns;
    event.context_mask = event_context_mask;
    event.thread_role = event_thread_role;
    event.flags = event_flags;
    strncpy_s(event.topic, topic ? topic : "", _TRUNCATE);
    if (!event_payload.empty()) {
        event.payload = const_cast<char*>(event_payload.data());
        event.payload_size = static_cast<uint32_t>(event_payload.size());
    }
    event.propagation_stopped = 0u;

    WotbModV3Event* const previous = current_event;
    current_event = &event;
    for (const EventSubscriptionRecord& record : targets) {
        record.callback(1u, &event, record.user_data);
        if (event.propagation_stopped) break;
    }
    current_event = previous;
}

inline WotbModV3EventsApiV1& EventsApi() {
    static WotbModV3EventsApiV1 api = {};
    api.struct_size = sizeof(api);
    api.api_version = WOTBMOD_V3_EVENTS_VERSION;
    api.subscribe = &EventsSubscribe;
    api.unsubscribe = &EventsUnsubscribe;
    api.set_priority = &EventsSetPriority;
    api.post = &EventsPost;
    api.stop_propagation = &EventsStopPropagation;
    api.get_dispatch_info = &EventsGetDispatchInfo;
    api.get_thread = &EventsGetThread;
    api.get_timestamp = &EventsGetTimestamp;
    api.get_context = &EventsGetContext;
    return api;
}

// ---------------------------------------------------------------------------
// wotbmod.ges - list_types / read_* / get_schema / schema_field / publish
// over a fake two-type catalogue, plus FireGesEvent, which delivers a GES
// event exactly as the runtime does: a system event on the topic
// wotbmod.ges.<Owner>.<Name> whose payload is a WotbModV3GesEvent. The read_*
// slots identify the current delivery by the payload pointer inside that
// struct, as src/v3/ges_services.cpp does, so a copy of the struct reads fine
// while the delivery is in progress and is refused once it is over.
// ---------------------------------------------------------------------------

inline WotbModV3Result GesReadable(const WotbModV3GesEvent* event,
                                   uint32_t offset, uint32_t size) {
    if (!event) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    if (!ges_current_payload || event->payload != ges_current_payload) {
        return WOTBMOD_V3_E_OBJECT_DESTROYED;
    }
    if (offset > ges_current_size || size > ges_current_size - offset) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL GesListTypes(
    WotbModV3Handle, const char** names, uint32_t capacity, uint32_t* count) {
    static const char* kNames[] = {"Avatar::CameraModeChanged",
                                   "Lobby::Survey::Accepted"};
    Record(ges_calls, "ges.list_types(" + std::to_string(capacity) + ")");
    if (!count) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    *count = 2u;
    for (uint32_t i = 0u; names && i < capacity && i < 2u; ++i) {
        names[i] = kNames[i];
    }
    return capacity != 0u && capacity < 2u ? WOTBMOD_V3_E_BUFFER_TOO_SMALL
                                           : WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL GesReadI32(
    const WotbModV3GesEvent* event, uint32_t offset, int32_t* out) {
    const WotbModV3Result result = GesReadable(event, offset, 4u);
    if (result != WOTBMOD_V3_OK || !out) return result;
    std::memcpy(out, static_cast<const uint8_t*>(event->payload) + offset, 4u);
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL GesReadU32(
    const WotbModV3GesEvent* event, uint32_t offset, uint32_t* out) {
    const WotbModV3Result result = GesReadable(event, offset, 4u);
    if (result != WOTBMOD_V3_OK || !out) return result;
    std::memcpy(out, static_cast<const uint8_t*>(event->payload) + offset, 4u);
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL GesReadF32(
    const WotbModV3GesEvent* event, uint32_t offset, float* out) {
    const WotbModV3Result result = GesReadable(event, offset, 4u);
    if (result != WOTBMOD_V3_OK || !out) return result;
    std::memcpy(out, static_cast<const uint8_t*>(event->payload) + offset, 4u);
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL GesReadBool(
    const WotbModV3GesEvent* event, uint32_t offset, uint8_t* out) {
    const WotbModV3Result result = GesReadable(event, offset, 1u);
    if (result != WOTBMOD_V3_OK || !out) return result;
    *out = static_cast<const uint8_t*>(event->payload)[offset];
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL GesReadPtr(
    const WotbModV3GesEvent* event, uint32_t offset, const void** out) {
    const WotbModV3Result result = GesReadable(event, offset, sizeof(void*));
    if (result != WOTBMOD_V3_OK || !out) return result;
    std::memcpy(out, static_cast<const uint8_t*>(event->payload) + offset,
                sizeof(void*));
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL GesReadCString(
    const WotbModV3GesEvent* event, uint32_t offset, char* buffer,
    uint32_t capacity) {
    const void* text = nullptr;
    const WotbModV3Result result = GesReadPtr(event, offset, &text);
    if (result != WOTBMOD_V3_OK) return result;
    if (!buffer || capacity == 0u) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    strncpy_s(buffer, capacity, text ? static_cast<const char*>(text) : "",
              _TRUNCATE);
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL GesGetSchema(
    const char* name, uint32_t* id, uint32_t* size, uint32_t* fields) {
    if (!name || !id || !size || !fields) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    Record(ges_calls, std::string("ges.get_schema(") + name + ")");
    if (std::strcmp(name, "Avatar::CameraModeChanged") != 0) {
        *id = 0u;
        *size = 0u;
        *fields = 0u;
        return WOTBMOD_V3_E_NOT_FOUND;
    }
    *id = 1u;
    *size = 8u;
    *fields = 2u;
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL GesSchemaField(
    uint32_t id, uint32_t index, WotbModV3GesField* out) {
    static const WotbModV3GesField kFields[] = {
        {sizeof(WotbModV3GesField), WOTBMOD_V3_GES_VERSION, "mode", 0u,
         WOTBMOD_V3_GES_FIELD_I32, 4u},
        {sizeof(WotbModV3GesField), WOTBMOD_V3_GES_VERSION, "flag", 4u,
         WOTBMOD_V3_GES_FIELD_BOOL, 1u}};
    if (!out) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    if (id != 1u || index >= 2u) return WOTBMOD_V3_E_NOT_FOUND;
    *out = kFields[index];
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL GesPublish(
    WotbModV3Handle, const char* name, const void* payload, uint32_t size,
    uint32_t flags) {
    Record(ges_calls, std::string("ges.publish(") + (name ? name : "") + "," +
                          std::to_string(size) + "," +
                          std::to_string(flags) + ")");
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    if (!name || !payload || size == 0u) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    ges_last_published.assign(static_cast<const uint8_t*>(payload),
                              static_cast<const uint8_t*>(payload) + size);
    return WOTBMOD_V3_OK;
}

inline WotbModV3GesApiV1& GesApi() {
    static WotbModV3GesApiV1 api = {};
    api.struct_size = sizeof(api);
    api.api_version = WOTBMOD_V3_GES_VERSION;
    api.list_types = &GesListTypes;
    api.read_i32 = &GesReadI32;
    api.read_u32 = &GesReadU32;
    api.read_f32 = &GesReadF32;
    api.read_bool = &GesReadBool;
    api.read_ptr = &GesReadPtr;
    api.read_cstring = &GesReadCString;
    api.get_schema = &GesGetSchema;
    api.schema_field = &GesSchemaField;
    api.publish = &GesPublish;
    return api;
}

// Delivers one GES event the way the runtime's GesHostPublish does: a system
// event on wotbmod.ges.<Owner>.<Name> carrying a WotbModV3GesEvent. payload
// stays readable through the read_* slots only for the duration of the call.
inline void FireGesEvent(const char* type_name, const void* payload,
                         uint32_t size, uint32_t schema_id,
                         uint32_t publisher_rva = 0u) {
    WotbModV3GesEvent ges = {};
    WOTBMOD_V3_INIT_STRUCT(ges, WOTBMOD_V3_GES_VERSION);
    strncpy_s(ges.type_name, type_name ? type_name : "", _TRUNCATE);
    ges.payload = payload;
    ges.payload_size = size;
    ges.schema_id = schema_id;
    ges.publisher_rva = publisher_rva;
    ges.flags = (size != 0u ? WOTBMOD_V3_GES_EVENT_SIZE_KNOWN : 0u) |
                (schema_id != 0u ? WOTBMOD_V3_GES_EVENT_SCHEMA_KNOWN : 0u);

    std::string topic = std::string(WOTBMOD_V3_GES_TOPIC_PREFIX);
    for (const char* p = ges.type_name; *p != '\0'; ++p) {
        if (p[0] == ':' && p[1] == ':') {
            topic += '.';
            ++p;
        } else {
            topic += *p;
        }
    }

    const uint32_t previous_flags = event_flags;
    const std::string previous_payload = event_payload;
    event_flags = WOTBMOD_V3_EVENT_FLAG_SYSTEM;
    event_payload.assign(reinterpret_cast<const char*>(&ges), sizeof(ges));
    ges_current_payload = payload;
    ges_current_size = size;
    FireEvent(topic.c_str());
    ges_current_payload = nullptr;
    ges_current_size = 0u;
    event_flags = previous_flags;
    event_payload = previous_payload;
}

// ---------------------------------------------------------------------------
// wotbmod.ui - the 18 of WotbModV3UiApiV2's 74 slots lua_bind_ui.cpp binds.
// Every function below records its call first (so Called() still sees a
// refused attempt happen, per the failure-switch convention set out above)
// and checks forced_result second, exactly like storage and events.
// ---------------------------------------------------------------------------

inline WotbModV3Result WOTBMOD_V3_CALL UiControlCreate(
    WotbModV3Handle, const WotbModV3UiControlDescriptor* descriptor,
    WotbModV3UiHandle* out_control) {
    if (!descriptor || !out_control) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    last_control_create_thread.store(GetCurrentThreadId());
    ui_calls.push_back(std::string("ui.control_create(") +
                        std::to_string(descriptor->type) + "," +
                        std::to_string(descriptor->visible) + ")");
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    UiControlRecord record;
    record.handle = next_ui_control_handle++;
    record.id = descriptor->id ? descriptor->id : "";
    record.texture_uri =
        descriptor->texture_uri ? descriptor->texture_uri : "";
    record.text = descriptor->text ? descriptor->text : "";
    record.live = true;
    record.visible = descriptor->visible != 0u;
    record.type = descriptor->type;
    record.position = {
        descriptor->geometry.x,
        descriptor->geometry.y};
    record.size = {
        descriptor->geometry.width,
        descriptor->geometry.height};
    ui_controls.push_back(record);
    *out_control = record.handle;

    if (record.id == "LuaUiFrameworkPanel" &&
        record.texture_uri == "mod://self/ui/lua_ui_framework.yaml") {
        struct TemplateChild {
            const char* id;
            float x;
            float y;
            float width;
            float height;
        };
        static const TemplateChild children[] = {
            {"OpenButton", 1500.0f, 360.0f, 300.0f, 76.0f},
            {"WindowBackground", 510.0f, 150.0f, 900.0f, 720.0f},
            {"TitleText", 560.0f, 185.0f, 760.0f, 60.0f},
            {"DescriptionText", 560.0f, 245.0f, 760.0f, 48.0f},
            {"CloseButton", 1310.0f, 175.0f, 64.0f, 56.0f},
            {"VolumeLabel", 590.0f, 325.0f, 220.0f, 42.0f},
            {"VolumeTrack", 590.0f, 380.0f, 520.0f, 36.0f},
            {"VolumeFill", 590.0f, 380.0f, 182.0f, 36.0f},
            {"VolumeKnob", 754.0f, 370.0f, 36.0f, 56.0f},
            {"ScaleLabel", 590.0f, 455.0f, 220.0f, 42.0f},
            {"ScaleTrack", 590.0f, 510.0f, 520.0f, 36.0f},
            {"ScaleFill", 590.0f, 510.0f, 364.0f, 36.0f},
            {"ScaleKnob", 936.0f, 500.0f, 36.0f, 56.0f},
            {"ToggleButton", 590.0f, 605.0f, 250.0f, 70.0f},
            {"ToggleOn", 860.0f, 615.0f, 170.0f, 50.0f},
            {"ToggleOff", 860.0f, 615.0f, 170.0f, 50.0f},
            {"ActionButton", 1050.0f, 605.0f, 250.0f, 70.0f},
            {"ResetButton", 590.0f, 725.0f, 250.0f, 70.0f},
            {"ActionStatus", 875.0f, 725.0f, 425.0f, 70.0f},
            {"FooterText", 590.0f, 815.0f, 710.0f, 34.0f},
        };
        for (const TemplateChild& child : children) {
            UiControlRecord fixture;
            fixture.handle = next_ui_control_handle++;
            fixture.id = child.id;
            fixture.live = true;
            fixture.template_child = true;
            fixture.parent = record.handle;
            fixture.position = {child.x, child.y};
            fixture.size = {child.width, child.height};
            ui_controls.push_back(fixture);
        }
    }
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL UiControlClone(
    WotbModV3Handle, WotbModV3UiHandle control,
    WotbModV3UiHandle* out_clone) {
    ui_calls.push_back(std::string("ui.control_clone(") +
                       std::to_string(control) + ")");
    if (!out_clone) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    UiControlRecord* source = FindUiControl(control);
    if (!source) return WOTBMOD_V3_E_INVALID_HANDLE;
    UiControlRecord clone = *source;
    clone.handle = next_ui_control_handle++;
    clone.parent = WOTBMOD_V3_INVALID_HANDLE;
    clone.game_owned = false;
    clone.template_child = false;
    clone.live = true;
    ui_controls.push_back(clone);
    *out_clone = clone.handle;
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL UiControlDestroy(
    WotbModV3Handle mod, WotbModV3UiHandle control) {
    // Before anything else, including the forced_result check: a destroy the
    // client is about to refuse still happened at the wrong moment if a
    // subscription was live when it was attempted. See the witness's own
    // comment above.
    last_control_destroy_mod.store(mod);
    const size_t live_now = LiveSubscriptions();
    size_t high = live_subscriptions_at_destroy.load();
    while (high < live_now &&
           !live_subscriptions_at_destroy.compare_exchange_weak(high,
                                                                live_now)) {
    }
    ui_calls.push_back(std::string("ui.control_destroy(") +
                        std::to_string(control) + ")");
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    UiControlRecord* record = FindUiControl(control);
    if (!record) return WOTBMOD_V3_E_INVALID_HANDLE;
    // Marked dead, not erased - see FindUiControl's own comment on why the
    // record has to survive its own destroy.
    record->live = false;
    for (UiControlRecord& child : ui_controls) {
        if (child.parent == control) child.live = false;
    }
    for (UiStyleOwnerRecord& style : ui_style_owners) {
        if (!style.live || style.control != control) continue;
        for (GenericHandleRecord& handle : generic_handles) {
            if (handle.handle == style.handle && handle.references) {
                --handle.references;
                break;
            }
        }
        style.live = false;
    }
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL UiControlAddChild(
    WotbModV3Handle, WotbModV3UiHandle parent, WotbModV3UiHandle child) {
    ui_calls.push_back(std::string("ui.control_add_child(") +
                        std::to_string(parent) + "," + std::to_string(child) +
                        ")");
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    UiControlRecord* parent_record = FindUiControl(parent);
    UiControlRecord* child_record = FindUiControl(child);
    if (!parent_record || !child_record) return WOTBMOD_V3_E_INVALID_HANDLE;
    // The parent link UiIsDescendant walks - see its own comment. Without
    // this, control_find_by_id could never actually scope a search, only
    // check that root happens to be alive.
    child_record->parent = parent;
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL UiControlRemoveChild(
    WotbModV3Handle, WotbModV3UiHandle parent, WotbModV3UiHandle child) {
    ui_calls.push_back(std::string("ui.control_remove_child(") +
                        std::to_string(parent) + "," + std::to_string(child) +
                        ")");
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    UiControlRecord* parent_record = FindUiControl(parent);
    UiControlRecord* child_record = FindUiControl(child);
    if (!parent_record || !child_record) return WOTBMOD_V3_E_INVALID_HANDLE;
    if (child_record->parent == parent) child_record->parent = 0u;
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL UiControlSetParent(
    WotbModV3Handle, WotbModV3UiHandle control,
    WotbModV3UiHandle parent) {
    ui_calls.push_back(std::string("ui.control_set_parent(") +
                       std::to_string(control) + "," +
                       std::to_string(parent) + ")");
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    UiControlRecord* control_record = FindUiControl(control);
    UiControlRecord* parent_record = FindUiControl(parent);
    if (!control_record || !parent_record) {
        return WOTBMOD_V3_E_INVALID_HANDLE;
    }
    control_record->parent = parent;
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL UiControlSetId(
    WotbModV3Handle, WotbModV3UiHandle control, const char* id) {
    ui_calls.push_back(std::string("ui.control_set_id(") +
                        std::to_string(control) + "," + (id ? id : "") + ")");
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    UiControlRecord* record = FindUiControl(control);
    if (!record) return WOTBMOD_V3_E_INVALID_HANDLE;
    record->id = id ? id : "";
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL UiControlFindById(
    WotbModV3Handle, WotbModV3UiHandle root, const char* id,
    WotbModV3UiHandle* out_control) {
    ui_calls.push_back(std::string("ui.control_find_by_id(") +
                        std::to_string(root) + "," + (id ? id : "") + ")");
    if (!id || !out_control) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    // Mirrors client_services.cpp's own UiControlFindById: a non-invalid
    // root is resolved through the same owned-handle lookup every other
    // control-taking slot uses, before anything else happens. Without this
    // a destroyed or forged-looking root would search as if it had been
    // omitted instead of being refused - the one control-taking slot this
    // mock used to get wrong.
    if (root != WOTBMOD_V3_INVALID_HANDLE && !FindUiControl(root)) {
        return WOTBMOD_V3_E_INVALID_HANDLE;
    }
    for (const UiControlRecord& record : ui_controls) {
        if (record.live && record.id == id &&
            UiIsDescendant(root, record.handle)) {
            *out_control = record.handle;
            if (record.template_child) {
                GenericHandleRecord* reference = nullptr;
                for (GenericHandleRecord& candidate : generic_handles) {
                    if (candidate.handle == record.handle) {
                        reference = &candidate;
                        break;
                    }
                }
                if (reference) {
                    ++reference->references;
                } else {
                    generic_handles.push_back({record.handle, 1u});
                }
            }
            return WOTBMOD_V3_OK;
        }
    }
    // Mirrors client_services.cpp's own UiControlFindById: "no such id" is
    // WOTBMOD_V3_E_NOT_FOUND, never WOTBMOD_V3_OK with an invalid handle -
    // see lua_bind_ui.cpp's own comment on why that is the shape this mock
    // has to reproduce for the binding's choice to be provable.
    return WOTBMOD_V3_E_NOT_FOUND;
}

inline WotbModV3Result WOTBMOD_V3_CALL UiControlIsAlive(
    WotbModV3Handle, WotbModV3UiHandle control, uint32_t* out_alive) {
    ui_calls.push_back(std::string("ui.control_is_alive(") +
                        std::to_string(control) + ")");
    if (!out_alive) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    // Always OK - is_alive answering "no" is a successful read, not a
    // failure, exactly as the real UiControlIsAlive never fails merely
    // because the handle is dead or was never created here.
    *out_alive = FindUiControl(control) ? 1u : 0u;
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL UiControlSetText(
    WotbModV3Handle, WotbModV3UiHandle control, const char* text) {
    ui_calls.push_back(std::string("ui.control_set_text(") +
                        std::to_string(control) + "," + (text ? text : "") +
                        ")");
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    UiControlRecord* record = FindUiControl(control);
    if (!record) return WOTBMOD_V3_E_INVALID_HANDLE;
    record->text = text ? text : "";
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL UiControlSetTexture(
    WotbModV3Handle, WotbModV3UiHandle control, const char* texture_uri) {
    ui_calls.push_back(std::string("ui.control_set_texture(") +
                       std::to_string(control) + "," +
                       (texture_uri ? texture_uri : "") + ")");
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    UiControlRecord* record = FindUiControl(control);
    if (!record) return WOTBMOD_V3_E_INVALID_HANDLE;
    record->texture_uri = texture_uri ? texture_uri : "";
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL UiControlSetColor(
    WotbModV3Handle, WotbModV3UiHandle control, WotbModV3Color color) {
    ui_calls.push_back(std::string("ui.control_set_color(") +
                       std::to_string(control) + ")");
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    UiControlRecord* record = FindUiControl(control);
    if (!record) return WOTBMOD_V3_E_INVALID_HANDLE;
    record->color = color;
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL UiControlSetOpacity(
    WotbModV3Handle, WotbModV3UiHandle control, float opacity) {
    ui_calls.push_back(std::string("ui.control_set_opacity(") +
                       std::to_string(control) + "," +
                       std::to_string(opacity) + ")");
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    UiControlRecord* record = FindUiControl(control);
    if (!record) return WOTBMOD_V3_E_INVALID_HANDLE;
    record->opacity = opacity;
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL UiControlSetFont(
    WotbModV3Handle, WotbModV3UiHandle control, const char* font_uri) {
    ui_calls.push_back(std::string("ui.control_set_font(") +
                       std::to_string(control) + "," +
                       (font_uri ? font_uri : "") + ")");
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    UiControlRecord* record = FindUiControl(control);
    if (!record) return WOTBMOD_V3_E_INVALID_HANDLE;
    record->font_uri = font_uri ? font_uri : "";
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL UiControlSetFontSize(
    WotbModV3Handle, WotbModV3UiHandle control, float size) {
    ui_calls.push_back(std::string("ui.control_set_font_size(") +
                       std::to_string(control) + "," +
                       std::to_string(size) + ")");
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    UiControlRecord* record = FindUiControl(control);
    if (!record) return WOTBMOD_V3_E_INVALID_HANDLE;
    record->font_size = size;
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL UiControlSetTextAlignment(
    WotbModV3Handle, WotbModV3UiHandle control, uint32_t alignment) {
    ui_calls.push_back(std::string("ui.control_set_text_alignment(") +
                       std::to_string(control) + "," +
                       std::to_string(alignment) + ")");
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    UiControlRecord* record = FindUiControl(control);
    if (!record) return WOTBMOD_V3_E_INVALID_HANDLE;
    record->text_alignment = alignment;
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL UiControlSetTextWrap(
    WotbModV3Handle, WotbModV3UiHandle control, uint32_t enabled) {
    ui_calls.push_back(std::string("ui.control_set_text_wrap(") +
                       std::to_string(control) + "," +
                       std::to_string(enabled) + ")");
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    UiControlRecord* record = FindUiControl(control);
    if (!record) return WOTBMOD_V3_E_INVALID_HANDLE;
    record->text_wrap = enabled != 0u;
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL UiControlSetRichText(
    WotbModV3Handle, WotbModV3UiHandle control, uint32_t enabled) {
    ui_calls.push_back(std::string("ui.control_set_rich_text(") +
                       std::to_string(control) + "," +
                       std::to_string(enabled) + ")");
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    UiControlRecord* record = FindUiControl(control);
    if (!record) return WOTBMOD_V3_E_INVALID_HANDLE;
    record->rich_text = enabled != 0u;
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL UiControlSetEnabled(
    WotbModV3Handle, WotbModV3UiHandle control, uint32_t enabled) {
    ui_calls.push_back(std::string("ui.control_set_enabled(") +
                       std::to_string(control) + "," +
                       std::to_string(enabled) + ")");
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    UiControlRecord* record = FindUiControl(control);
    if (!record) return WOTBMOD_V3_E_INVALID_HANDLE;
    record->enabled = enabled != 0u;
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL UiControlSetInteractable(
    WotbModV3Handle, WotbModV3UiHandle control, uint32_t interactable) {
    ui_calls.push_back(std::string("ui.control_set_interactable(") +
                       std::to_string(control) + "," +
                       std::to_string(interactable) + ")");
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    UiControlRecord* record = FindUiControl(control);
    if (!record) return WOTBMOD_V3_E_INVALID_HANDLE;
    record->interactable = interactable != 0u;
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL UiControlSetVisible(
    WotbModV3Handle, WotbModV3UiHandle control, uint32_t visible) {
    ui_calls.push_back(std::string("ui.control_set_visible(") +
                        std::to_string(control) + "," +
                        std::to_string(visible) + ")");
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    UiControlRecord* record = FindUiControl(control);
    if (!record) return WOTBMOD_V3_E_INVALID_HANDLE;
    record->visible = visible != 0u;
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL UiControlSetPosition(
    WotbModV3Handle, WotbModV3UiHandle control, WotbModV3Vec2 position) {
    ui_calls.push_back(std::string("ui.control_set_position(") +
                        std::to_string(control) + "," +
                        std::to_string(position.x) + "," +
                        std::to_string(position.y) + ")");
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    UiControlRecord* record = FindUiControl(control);
    if (!record) return WOTBMOD_V3_E_INVALID_HANDLE;
    record->position = position;
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL UiControlGetPosition(
    WotbModV3Handle, WotbModV3UiHandle control, WotbModV3Vec2* out_position) {
    ui_calls.push_back(std::string("ui.control_get_position(") +
                        std::to_string(control) + ")");
    if (!out_position) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    UiControlRecord* record = FindUiControl(control);
    if (!record) return WOTBMOD_V3_E_INVALID_HANDLE;
    *out_position = record->position;
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL UiControlSetSize(
    WotbModV3Handle, WotbModV3UiHandle control, WotbModV3Vec2 size) {
    ui_calls.push_back(std::string("ui.control_set_size(") +
                        std::to_string(control) + "," +
                        std::to_string(size.x) + "," +
                        std::to_string(size.y) + ")");
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    UiControlRecord* record = FindUiControl(control);
    if (!record) return WOTBMOD_V3_E_INVALID_HANDLE;
    record->size = size;
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL UiControlGetSize(
    WotbModV3Handle, WotbModV3UiHandle control, WotbModV3Vec2* out_size) {
    ui_calls.push_back(std::string("ui.control_get_size(") +
                        std::to_string(control) + ")");
    if (!out_size) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    UiControlRecord* record = FindUiControl(control);
    if (!record) return WOTBMOD_V3_E_INVALID_HANDLE;
    *out_size = record->size;
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL UiControlSetAnchor(
    WotbModV3Handle, WotbModV3UiHandle control, WotbModV3Vec2 anchor) {
    ui_calls.push_back(std::string("ui.control_set_anchor(") +
                        std::to_string(control) + "," +
                        std::to_string(anchor.x) + "," +
                        std::to_string(anchor.y) + ")");
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    if (!FindUiControl(control)) return WOTBMOD_V3_E_INVALID_HANDLE;
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL UiControlSetPivot(
    WotbModV3Handle, WotbModV3UiHandle control, WotbModV3Vec2 pivot) {
    ui_calls.push_back(std::string("ui.control_set_pivot(") +
                        std::to_string(control) + "," +
                        std::to_string(pivot.x) + "," +
                        std::to_string(pivot.y) + ")");
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    if (!FindUiControl(control)) return WOTBMOD_V3_E_INVALID_HANDLE;
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL UiSlotFind(
    WotbModV3Handle, const char* slot_id, WotbModV3UiHandle* out_slot) {
    if (out_slot) *out_slot = WOTBMOD_V3_INVALID_HANDLE;
    ui_calls.push_back(std::string("ui.slot_find(") + (slot_id ? slot_id : "") +
                        ")");
    if (!slot_id || !out_slot) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    if (std::strcmp(slot_id, kUiKnownSlotId) == 0) {
        const uint64_t handle = next_ui_slot_handle++;
        ui_slots.push_back(handle);
        *out_slot = handle;
        return WOTBMOD_V3_OK;
    }
    if (std::strcmp(slot_id, kUiUnresolvedSlotId) == 0) {
        // ui_v2.h's own documented case: declared, but the native extension
        // point is not resolved by this (mock) binding pack.
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    return WOTBMOD_V3_E_NOT_FOUND;
}

inline WotbModV3Result WOTBMOD_V3_CALL UiSlotAttach(
    WotbModV3Handle, WotbModV3UiHandle slot, WotbModV3UiHandle control,
    int32_t priority) {
    ui_calls.push_back(std::string("ui.slot_attach(") + std::to_string(slot) +
                        "," + std::to_string(control) + "," +
                        std::to_string(priority) + ")");
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    const bool slot_live =
        std::find(ui_slots.begin(), ui_slots.end(), slot) != ui_slots.end();
    if (!slot_live || !FindUiControl(control)) {
        return WOTBMOD_V3_E_INVALID_HANDLE;
    }
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL UiSlotDetach(
    WotbModV3Handle, WotbModV3UiHandle slot, WotbModV3UiHandle control) {
    ui_calls.push_back(std::string("ui.slot_detach(") + std::to_string(slot) +
                        "," + std::to_string(control) + ")");
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    const bool slot_live =
        std::find(ui_slots.begin(), ui_slots.end(), slot) != ui_slots.end();
    if (!slot_live || !FindUiControl(control)) {
        return WOTBMOD_V3_E_INVALID_HANDLE;
    }
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL UiGetActiveScreen(
    WotbModV3Handle, WotbModV3UiHandle* out_screen) {
    ui_calls.push_back("ui.get_active_screen()");
    if (!out_screen) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    if (forced_result != WOTBMOD_V3_OK) return forced_result;

    UiControlRecord* active = nullptr;
    for (UiControlRecord& record : ui_controls) {
        if (record.live && record.game_owned) {
            active = &record;
            break;
        }
    }
    if (!active) {
        UiControlRecord record;
        record.handle = next_ui_control_handle++;
        record.id = "mock.active_screen";
        record.live = true;
        record.game_owned = true;
        record.type = WOTBMOD_V3_UI_CONTROL_CONTAINER;
        record.size = {1920.0f, 1080.0f};
        ui_controls.push_back(record);
        active = &ui_controls.back();
    }

    GenericHandleRecord* reference = nullptr;
    for (GenericHandleRecord& record : generic_handles) {
        if (record.handle == active->handle) {
            reference = &record;
            break;
        }
    }
    if (reference) {
        ++reference->references;
    } else {
        generic_handles.push_back({active->handle, 1u});
    }
    *out_screen = active->handle;
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL UiGetViewportSize(
    WotbModV3Handle, WotbModV3Vec2* out_size) {
    ui_calls.push_back("ui.get_viewport_size()");
    if (!out_size) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    *out_size = {1536.0f, 864.0f};
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL UiControlGetSnapshot(
    WotbModV3Handle, WotbModV3UiHandle control,
    WotbModV3UiControlSnapshot* out_snapshot) {
    ui_calls.push_back(std::string("ui.control_get_snapshot(") +
                       std::to_string(control) + ")");
    if (!out_snapshot ||
        out_snapshot->struct_size < sizeof(*out_snapshot) ||
        out_snapshot->api_version != WOTBMOD_V3_UI_VERSION_3) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    UiControlRecord* record = FindUiControl(control);
    if (!record) return WOTBMOD_V3_E_INVALID_HANDLE;

    WotbModV3UiControlSnapshot snapshot = {};
    WOTBMOD_V3_INIT_STRUCT(snapshot, WOTBMOD_V3_UI_VERSION_3);
    snapshot.control = record->handle;
    snapshot.parent = record->parent;
    snapshot.type = record->type;
    snapshot.flags = (record->visible
                          ? WOTBMOD_V3_UI_SNAPSHOT_VISIBLE
                          : 0u) |
                     (record->game_owned
                          ? WOTBMOD_V3_UI_SNAPSHOT_GAME_OWNED
                          : 0u);
    snapshot.geometry = {record->position.x, record->position.y,
                         record->size.x, record->size.y};
    strncpy_s(snapshot.id, record->id.c_str(), _TRUNCATE);
    *out_snapshot = snapshot;
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL UiEventSubscribe(
    WotbModV3Handle, WotbModV3UiHandle control, uint32_t event_type,
    WotbModV3UiEventCallback callback, void* user_data,
    WotbModV3Token* out_token) {
    ui_calls.push_back(std::string("ui.event_subscribe(") +
                       std::to_string(control) + "," +
                       std::to_string(event_type) + ")");
    if (!callback || !out_token) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    if (!FindUiControl(control)) return WOTBMOD_V3_E_INVALID_HANDLE;

    UiEventSubscriptionRecord record;
    record.token = next_ui_event_token++;
    record.control = control;
    record.event_type = event_type;
    record.callback = callback;
    record.user_data = user_data;
    record.live = true;
    record.references = 1u;
    ui_event_subscriptions.push_back(record);
    *out_token = record.token;
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL UiEventUnsubscribe(
    WotbModV3Handle, WotbModV3Token token) {
    ui_calls.push_back(std::string("ui.event_unsubscribe(") +
                       std::to_string(token) + ")");
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    for (UiEventSubscriptionRecord& record : ui_event_subscriptions) {
        if (record.token == token && record.live) {
            record.live = false;
            record.references = 0u;
            return WOTBMOD_V3_OK;
        }
    }
    return WOTBMOD_V3_E_NOT_FOUND;
}

inline WotbModV3Result WOTBMOD_V3_CALL UiStylePush(
    WotbModV3Handle,
    WotbModV3UiHandle control,
    const WotbModV3UiStylePatch* patch,
    WotbModV3Handle* out_handle) {
    if (!patch || patch->struct_size < sizeof(*patch) ||
        patch->api_version != WOTBMOD_V3_UI_VERSION || !out_handle) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    ui_calls.push_back(
        std::string("ui.style_push(") + std::to_string(control) + "," +
        std::to_string(patch->fields) + "," +
        std::to_string(patch->background_color.r) + "," +
        std::to_string(patch->background_color.a) + ")");
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    if (!FindUiControl(control)) return WOTBMOD_V3_E_INVALID_HANDLE;
    const WotbModV3Handle handle = next_generic_handle++;
    generic_handles.push_back({handle, 1u});
    ui_style_owners.push_back({handle, control, true});
    *out_handle = handle;
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL UiStyleUpdate(
    WotbModV3Handle,
    WotbModV3Handle handle,
    const WotbModV3UiStylePatch* patch) {
    ui_calls.push_back(
        std::string("ui.style_update(") + std::to_string(handle) + ")");
    if (!patch || patch->struct_size < sizeof(*patch) ||
        patch->api_version != WOTBMOD_V3_UI_VERSION) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    for (const UiStyleOwnerRecord& style : ui_style_owners) {
        if (style.handle != handle || !style.live) continue;
        for (const GenericHandleRecord& record : generic_handles) {
            if (record.handle == handle && record.references) {
                return WOTBMOD_V3_OK;
            }
        }
        break;
    }
    return WOTBMOD_V3_E_INVALID_HANDLE;
}

inline WotbModV3Result WOTBMOD_V3_CALL UiStylePop(
    WotbModV3Handle,
    WotbModV3Handle handle) {
    ui_calls.push_back(
        std::string("ui.style_pop(") + std::to_string(handle) + ")");
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    UiStyleOwnerRecord* owner = nullptr;
    for (UiStyleOwnerRecord& style : ui_style_owners) {
        if (style.handle == handle && style.live) {
            owner = &style;
            break;
        }
    }
    if (!owner) return WOTBMOD_V3_E_INVALID_HANDLE;
    for (GenericHandleRecord& record : generic_handles) {
        if (record.handle == handle && record.references) {
            --record.references;
            owner->live = false;
            return WOTBMOD_V3_OK;
        }
    }
    return WOTBMOD_V3_E_INVALID_HANDLE;
}

inline void FireUiEventForControl(
    WotbModV3UiHandle control,
    uint32_t event_type = WOTBMOD_V3_UI_EVENT_CLICK,
    WotbModV3Vec2 pointer = {123.0f, 456.0f}) {
    std::vector<UiEventSubscriptionRecord> targets;
    for (const UiEventSubscriptionRecord& record : ui_event_subscriptions) {
        if (!record.live || record.control != control ||
            record.event_type != event_type ||
            !record.callback) {
            continue;
        }
        targets.push_back(record);
    }
    for (const UiEventSubscriptionRecord& record : targets) {
        WotbModV3UiEvent event = {};
        WOTBMOD_V3_INIT_STRUCT(event, WOTBMOD_V3_UI_VERSION);
        event.type = event_type;
        event.control = record.control;
        event.pointer = pointer;
        record.callback(1u, &event, record.user_data);
    }
}

inline void FireUiEvent(uint32_t event_type = WOTBMOD_V3_UI_EVENT_CLICK) {
    std::vector<UiEventSubscriptionRecord> targets;
    for (const UiEventSubscriptionRecord& record : ui_event_subscriptions) {
        if (!record.live || record.event_type != event_type ||
            !record.callback) {
            continue;
        }
        targets.push_back(record);
    }
    for (const UiEventSubscriptionRecord& record : targets) {
        WotbModV3UiEvent event = {};
        WOTBMOD_V3_INIT_STRUCT(event, WOTBMOD_V3_UI_VERSION);
        event.type = event_type;
        event.control = record.control;
        event.pointer = {123.0f, 456.0f};
        record.callback(1u, &event, record.user_data);
    }
}

// The 18 hand-written slots plus the generated path needed by the shipped UI
// example are wired. Other slots remain null so optional-slot guards continue
// to be exercised by this suite.
inline WotbModV3Result FacadeRecord(const char* slot, const std::string& args) {
    Record(facade_calls, std::string(slot) + "(" + args + ")");
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    return WOTBMOD_V3_OK;
}

// Defined with the gameplay_hud mock further down; declared here because the
// ui slots above it format numbers the same way.
inline std::string HudNumber(float value);

inline uint64_t MintFacadeHandle() {
    const uint64_t handle = next_generic_handle++;
    generic_handles.push_back({handle, 1u});
    return handle;
}

inline WotbModV3Result CopyOutString(const std::string& text, char* buffer,
                                     uint32_t* inout_size) {
    if (!inout_size) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    const uint32_t required = static_cast<uint32_t>(text.size() + 1u);
    if (!buffer || *inout_size < required) {
        *inout_size = required;
        return WOTBMOD_V3_E_BUFFER_TOO_SMALL;
    }
    std::memcpy(buffer, text.c_str(), required);
    *inout_size = required;
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL UiControlGetChildCount(
    WotbModV3Handle, WotbModV3UiHandle control, uint32_t* out_count) {
    FacadeRecord("ui.control_get_child_count", std::to_string(control));
    if (!out_count) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    if (!FindUiControl(control)) return WOTBMOD_V3_E_INVALID_HANDLE;
    uint32_t count = 0u;
    for (const UiControlRecord& record : ui_controls) {
        if (record.live && record.parent == control) ++count;
    }
    *out_count = count;
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL UiControlGetChildAt(
    WotbModV3Handle, WotbModV3UiHandle control, uint32_t index,
    WotbModV3UiHandle* out_child) {
    FacadeRecord("ui.control_get_child_at",
                 std::to_string(control) + "," + std::to_string(index));
    if (!out_child) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    if (!FindUiControl(control)) return WOTBMOD_V3_E_INVALID_HANDLE;
    uint32_t seen = 0u;
    for (const UiControlRecord& record : ui_controls) {
        if (!record.live || record.parent != control) continue;
        if (seen++ == index) {
            *out_child = record.handle;
            generic_handles.push_back({record.handle, 1u});
            return WOTBMOD_V3_OK;
        }
    }
    return WOTBMOD_V3_E_NOT_FOUND;
}

inline WotbModV3Result WOTBMOD_V3_CALL UiControlGetOwnerMod(
    WotbModV3Handle mod, WotbModV3UiHandle control, WotbModV3Handle* out_owner) {
    FacadeRecord("ui.control_get_owner_mod", std::to_string(control));
    if (!out_owner) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    const UiControlRecord* record = FindUiControl(control);
    if (!record) return WOTBMOD_V3_E_INVALID_HANDLE;
    *out_owner = record->game_owned ? MintFacadeHandle() : mod;
    if (!record->game_owned) generic_handles.push_back({mod, 1u});
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL UiControlFindByPath(
    WotbModV3Handle mod, WotbModV3UiHandle root, const char* path,
    WotbModV3UiHandle* out_control) {
    FacadeRecord("ui.control_find_by_path",
                 std::to_string(root) + "," + (path ? path : ""));
    if (!path || !out_control) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    std::string leaf = path;
    const size_t slash = leaf.rfind('/');
    if (slash != std::string::npos) leaf = leaf.substr(slash + 1u);
    return UiControlFindById(mod, root, leaf.c_str(), out_control);
}

inline WotbModV3Result WOTBMOD_V3_CALL UiToastShow(
    WotbModV3Handle, const char* message, float duration_seconds) {
    const WotbModV3Result recorded =
        FacadeRecord("ui.toast_show", std::string(message ? message : "") +
                                          "," + HudNumber(duration_seconds));
    if (toast_not_supported) return WOTBMOD_V3_E_NOT_SUPPORTED;
    return recorded;
}

inline WotbModV3Result UiMockDialog(const char* kind,
                                    const WotbModV3UiDialogDescriptor* descriptor,
                                    WotbModV3UiHandle* out_dialog) {
    if (!descriptor || !out_dialog) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    const WotbModV3Result result = FacadeRecord(
        kind, std::string(descriptor->title ? descriptor->title : "") + "," +
                  (descriptor->message ? descriptor->message : "") + "," +
                  (descriptor->accept_label ? descriptor->accept_label : "") +
                  "," + (descriptor->cancel_label ? descriptor->cancel_label : "") +
                  "," + std::to_string(descriptor->modal));
    if (result != WOTBMOD_V3_OK) return result;
    UiControlRecord record;
    record.handle = next_ui_control_handle++;
    record.id = kind;
    record.text = descriptor->message ? descriptor->message : "";
    record.live = true;
    record.type = WOTBMOD_V3_UI_CONTROL_CONTAINER;
    ui_controls.push_back(record);
    generic_handles.push_back({record.handle, 1u});
    *out_dialog = record.handle;
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL UiDialogShow(
    WotbModV3Handle, const WotbModV3UiDialogDescriptor* descriptor,
    WotbModV3UiHandle* out_dialog) {
    return UiMockDialog("ui.dialog_show", descriptor, out_dialog);
}

inline WotbModV3Result WOTBMOD_V3_CALL UiConfirmShow(
    WotbModV3Handle, const WotbModV3UiDialogDescriptor* descriptor,
    WotbModV3UiHandle* out_dialog) {
    return UiMockDialog("ui.confirm_show", descriptor, out_dialog);
}

// wotbmod.ui.read: the mirror text, and the "live" text the engine draws -
// distinguishable so a facade that reads the wrong one is caught.
inline WotbModV3Result WOTBMOD_V3_CALL UiReadGetText(
    WotbModV3Handle, WotbModV3UiHandle control, char* buffer, uint32_t* inout_size) {
    const UiControlRecord* record = FindUiControl(control);
    if (!record) return WOTBMOD_V3_E_INVALID_HANDLE;
    return CopyOutString(record->text, buffer, inout_size);
}

inline WotbModV3Result WOTBMOD_V3_CALL UiReadGetLiveText(
    WotbModV3Handle, WotbModV3UiHandle control, char* buffer, uint32_t* inout_size) {
    const UiControlRecord* record = FindUiControl(control);
    if (!record) return WOTBMOD_V3_E_INVALID_HANDLE;
    return CopyOutString(record->text + " (live)", buffer, inout_size);
}

inline WotbModV3UiApiV4& UiReadApi() {
    static WotbModV3UiApiV4 api = {};
    api.struct_size = sizeof(api);
    api.api_version = WOTBMOD_V3_UI_VERSION_4;
    api.control_get_text = &UiReadGetText;
    api.control_get_live_text = &UiReadGetLiveText;
    return api;
}

inline WotbModV3UiApiV3& UiApi() {
    static WotbModV3UiApiV3 api = {};
    api.v2.struct_size = sizeof(api.v2);
    api.v2.api_version = WOTBMOD_V3_UI_VERSION;
    api.v2.control_create = &UiControlCreate;
    api.v2.control_clone = &UiControlClone;
    api.v2.control_destroy = &UiControlDestroy;
    api.v2.control_add_child = &UiControlAddChild;
    api.v2.control_remove_child = &UiControlRemoveChild;
    api.v2.control_set_parent = &UiControlSetParent;
    api.v2.control_set_id = &UiControlSetId;
    api.v2.control_find_by_id = &UiControlFindById;
    api.v2.control_is_alive = &UiControlIsAlive;
    api.v2.control_set_text = &UiControlSetText;
    api.v2.control_set_texture = &UiControlSetTexture;
    api.v2.control_set_color = &UiControlSetColor;
    api.v2.control_set_opacity = &UiControlSetOpacity;
    api.v2.control_set_visible = &UiControlSetVisible;
    api.v2.control_set_font = &UiControlSetFont;
    api.v2.control_set_font_size = &UiControlSetFontSize;
    api.v2.control_set_text_alignment = &UiControlSetTextAlignment;
    api.v2.control_set_text_wrap = &UiControlSetTextWrap;
    api.v2.control_set_rich_text = &UiControlSetRichText;
    api.v2.control_set_enabled = &UiControlSetEnabled;
    api.v2.control_set_interactable = &UiControlSetInteractable;
    api.v2.control_set_position = &UiControlSetPosition;
    api.v2.control_get_position = &UiControlGetPosition;
    api.v2.control_set_size = &UiControlSetSize;
    api.v2.control_get_size = &UiControlGetSize;
    api.v2.control_set_anchor = &UiControlSetAnchor;
    api.v2.control_set_pivot = &UiControlSetPivot;
    api.v2.slot_find = &UiSlotFind;
    api.v2.slot_attach = &UiSlotAttach;
    api.v2.slot_detach = &UiSlotDetach;
    api.v2.event_subscribe = &UiEventSubscribe;
    api.v2.event_unsubscribe = &UiEventUnsubscribe;
    api.v2.style_push = &UiStylePush;
    api.v2.style_update = &UiStyleUpdate;
    api.v2.style_pop = &UiStylePop;
    api.v2.get_viewport_size = &UiGetViewportSize;
    api.get_active_screen = &UiGetActiveScreen;
    api.control_get_snapshot = &UiControlGetSnapshot;
    api.v2.control_get_child_count = &UiControlGetChildCount;
    api.v2.control_get_child_at = &UiControlGetChildAt;
    api.v2.control_get_owner_mod = &UiControlGetOwnerMod;
    api.v2.control_find_by_path = &UiControlFindByPath;
    api.v2.toast_show = &UiToastShow;
    api.v2.dialog_show = &UiDialogShow;
    api.v2.confirm_show = &UiConfirmShow;
    return api;
}

// ---------------------------------------------------------------------------
// wotbmod.permissions - what this synthetic client says the host was granted.
//
// The host derives its ceiling from this rather than from a list written into
// its own source, so this is what a test drives to ask "and what happens when
// the host holds less than the spec's four?" or "...when this client is an
// older one that has no permissions interface at all?". Without it the ceiling
// would only ever be testable at the one value the mock happened to pick.
// ---------------------------------------------------------------------------

// What the host is granted, by name. Defaults to exactly the slice the spec
// fixes for it. A test narrows this, calls the entry point or on_enable again
// so the host re-measures, and restores it afterwards.
//
// Reset() puts this list back, and that is *all* it does - it cannot reach the
// host's cached ceiling, which is inside the DLL and stands until something
// calls the entry point or on_enable again. So restoring the list is only half
// of restoring the fence: a block that narrows the ceiling must re-measure
// before it leaves, and Reset() will not do that for it. Every block below
// does; the sentence is here because the next one is the one that will not.
inline std::vector<std::string> host_permissions;

// Whether this client offers the interface at all. False is an older client.
// The host is expected to load, measure nothing, and *refuse to run any
// script*, saying so at error level from on_enable - not to fall back on a
// default ceiling, which is what this comment used to describe and what the
// same commit that wrote the refusal deleted.
inline bool permissions_interface_available = true;

// A permission the client knows about but has not granted. It must not reach
// the ceiling: the ABI carries a state per entry precisely so that "asked for"
// and "holds" are different questions, and a host that read the list without
// the state would grant a script something the runtime refused this host.
inline std::vector<std::string> host_permissions_pending;

// A client that offers the interface and then cannot answer it. Two separate
// switches because the host has two separate branches for them and they must
// not be able to stand in for each other: a get_count that fails means nothing
// was measured at all, so the host holds no ceiling and runs no script; a
// get_at that fails on one entry means the rest of the answer is still true, so
// the ceiling *is* measured and merely narrower, and scripts run under it.
// Narrowing is the safe direction and the one that has to actually happen.
//
// (This said "so the ceiling is a fallback" until the final review. There is no
// fallback ceiling any more - kFallbackCeiling was deleted in the same commit
// that added the refusal - and a switch whose comment describes a branch that
// no longer exists is worse than an undocumented one, because it tells the next
// reader the wrong thing about which of the two cases they are looking at.)
inline bool permissions_get_count_fails = false;
inline int permissions_get_at_fails_at = -1;   // -1 = never

inline void SetHostPermissions(std::vector<std::string> granted) {
    host_permissions = std::move(granted);
}

inline void ResetHostPermissions() {
    host_permissions = {"core",           "events.public", "storage",
                        "ui.modify.game", "ui.create",     "ui.modify.own",
                        "battle.ui",      "input"};
    host_permissions_pending.clear();
    permissions_interface_available = true;
    permissions_get_count_fails = false;
    permissions_get_at_fails_at = -1;
}

inline WotbModV3Result WOTBMOD_V3_CALL PermissionsGetCount(
    WotbModV3Handle, uint32_t* out_count) {
    if (!out_count) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    if (permissions_get_count_fails) return WOTBMOD_V3_E_NOT_SUPPORTED;
    *out_count = static_cast<uint32_t>(host_permissions.size() +
                                       host_permissions_pending.size());
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL PermissionsGetAt(
    WotbModV3Handle, uint32_t index, WotbModV3PermissionInfo* out_info) {
    if (!out_info) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    if (permissions_get_at_fails_at >= 0 &&
        index == static_cast<uint32_t>(permissions_get_at_fails_at)) {
        return WOTBMOD_V3_E_BUSY;
    }
    const size_t total = host_permissions.size() +
                         host_permissions_pending.size();
    if (static_cast<size_t>(index) >= total) {
        return WOTBMOD_V3_E_NOT_FOUND;
    }
    std::memset(out_info, 0, sizeof(*out_info));
    WOTBMOD_V3_INIT_STRUCT(*out_info, WOTBMOD_V3_PERMISSIONS_VERSION);
    const bool granted = static_cast<size_t>(index) < host_permissions.size();
    const std::string& name =
        granted ? host_permissions[index]
                : host_permissions_pending[index - host_permissions.size()];
    out_info->state = granted ? WOTBMOD_V3_PERMISSION_STATE_GRANTED
                              : WOTBMOD_V3_PERMISSION_STATE_REVIEW_REQUIRED;
    out_info->tier = WOTBMOD_V3_PERMISSION_REVIEWED;
    strncpy_s(out_info->name, name.c_str(), _TRUNCATE);
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL PermissionsQuery(
    WotbModV3Handle mod, const char* permission_name,
    WotbModV3PermissionInfo* out_info) {
    if (!permission_name || !out_info) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    const uint32_t total = static_cast<uint32_t>(
        host_permissions.size() + host_permissions_pending.size());
    for (uint32_t i = 0u; i < total; ++i) {
        if (PermissionsGetAt(mod, i, out_info) != WOTBMOD_V3_OK) continue;
        if (std::strcmp(out_info->name, permission_name) == 0) {
            return WOTBMOD_V3_OK;
        }
    }
    std::memset(out_info, 0, sizeof(*out_info));
    WOTBMOD_V3_INIT_STRUCT(*out_info, WOTBMOD_V3_PERMISSIONS_VERSION);
    out_info->state = WOTBMOD_V3_PERMISSION_STATE_UNAVAILABLE;
    strncpy_s(out_info->name, permission_name, _TRUNCATE);
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL PermissionsGetGrantedTier(
    WotbModV3Handle, uint32_t* out_tier) {
    if (!out_tier) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    *out_tier = WOTBMOD_V3_PERMISSION_REVIEWED;
    return WOTBMOD_V3_OK;
}

inline WotbModV3PermissionsApiV1& PermissionsApi() {
    static WotbModV3PermissionsApiV1 api = {};
    api.struct_size = sizeof(api);
    api.api_version = WOTBMOD_V3_PERMISSIONS_VERSION;
    api.get_granted_tier = &PermissionsGetGrantedTier;
    api.query = &PermissionsQuery;
    api.get_count = &PermissionsGetCount;
    api.get_at = &PermissionsGetAt;
    return api;
}

inline WotbModV3PublicEntitySnapshot PublicEntitySnapshot(
    WotbModV3EntityHandle handle,
    uint32_t public_id,
    uint32_t team,
    int32_t health,
    int32_t max_health,
    bool local,
    const char* name) {
    WotbModV3PublicEntitySnapshot value = {};
    WOTBMOD_V3_INIT_STRUCT(value, WOTBMOD_V3_ENTITY_PUBLIC_VERSION);
    value.handle = handle;
    value.public_id = public_id;
    value.type = WOTBMOD_V3_PUBLIC_ENTITY_VEHICLE;
    value.visible_to_player = 1u;
    value.local_player = local ? 1u : 0u;
    value.team = team;
    value.health = health;
    value.max_health = max_health;
    strncpy_s(value.public_type, "vehicle", _TRUNCATE);
    strncpy_s(value.display_name, name ? name : "", _TRUNCATE);
    return value;
}

inline std::array<WotbModV3PublicEntitySnapshot, 3u>&
PublicEntities() {
    static std::array<WotbModV3PublicEntitySnapshot, 3u> values = {{
        PublicEntitySnapshot(10101u, 101u, 1u, 1200, 1200, true, "Local"),
        PublicEntitySnapshot(10102u, 102u, 1u, 730, 1000, false, "Ally"),
        PublicEntitySnapshot(10201u, 201u, 2u, 450, 900, false, "Enemy"),
    }};
    return values;
}

inline const WotbModV3PublicEntitySnapshot* FindPublicEntity(
    WotbModV3EntityHandle handle) {
    for (const WotbModV3PublicEntitySnapshot& value : PublicEntities()) {
        if (value.handle == handle) return &value;
    }
    return nullptr;
}

inline WotbModV3Result WOTBMOD_V3_CALL EntityPublicGetId(
    WotbModV3Handle,
    WotbModV3EntityHandle entity,
    uint32_t* out_id) {
    const WotbModV3PublicEntitySnapshot* value = FindPublicEntity(entity);
    if (!out_id) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    if (!value) return WOTBMOD_V3_E_INVALID_HANDLE;
    *out_id = value->public_id;
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL EntityPublicGetType(
    WotbModV3Handle,
    WotbModV3EntityHandle entity,
    char* buffer,
    uint32_t* inout_size) {
    const WotbModV3PublicEntitySnapshot* value = FindPublicEntity(entity);
    if (!inout_size) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    if (!value) return WOTBMOD_V3_E_INVALID_HANDLE;
    const uint32_t required =
        static_cast<uint32_t>(std::strlen(value->public_type) + 1u);
    if (!buffer || *inout_size < required) {
        *inout_size = required;
        return WOTBMOD_V3_E_BUFFER_TOO_SMALL;
    }
    strcpy_s(buffer, *inout_size, value->public_type);
    *inout_size = required;
    return WOTBMOD_V3_OK;
}

// The five roster fields the loader publishes by name (docs/API_V3_RU.md
// section 39), answered for the ally alone. Every other entity and every
// other name is refused the way the client refuses a field with no source,
// so a facade that zero-filled a refusal would be caught here.
inline WotbModV3Result WOTBMOD_V3_CALL EntityPublicGetProperty(
    WotbModV3Handle,
    WotbModV3EntityHandle entity,
    const char* property,
    WotbModV3PublicValue* out_value) {
    if (!property || !out_value) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    if (!FindPublicEntity(entity)) return WOTBMOD_V3_E_INVALID_HANDLE;
    if (entity != 10102u) return WOTBMOD_V3_E_NOT_SUPPORTED;
    const auto string_value = [&](const char* text) {
        out_value->type = WOTBMOD_V3_PUBLIC_VALUE_STRING;
        strncpy_s(out_value->value.string_value, text, _TRUNCATE);
        return WOTBMOD_V3_OK;
    };
    const auto integer_value = [&](int64_t number) {
        out_value->type = WOTBMOD_V3_PUBLIC_VALUE_INT64;
        out_value->value.integer = number;
        return WOTBMOD_V3_OK;
    };
    if (std::strcmp(property, "clan_tag") == 0) return string_value("ABC");
    if (std::strcmp(property, "account_id") == 0) return integer_value(5000123);
    if (std::strcmp(property, "kills") == 0 ||
        std::strcmp(property, "frags") == 0) {
        return integer_value(3);
    }
    if (std::strcmp(property, "vehicle_name") == 0) {
        return string_value("usa:A100_T49");
    }
    if (std::strcmp(property, "vehicle_display_name") == 0) {
        return string_value("T49");
    }
    return WOTBMOD_V3_E_NOT_SUPPORTED;
}

inline WotbModV3Result WOTBMOD_V3_CALL EntityPublicSubscribeProperty(
    WotbModV3Handle,
    WotbModV3EntityHandle,
    const char*,
    WotbModV3PublicPropertyCallback,
    void*,
    WotbModV3Token*) {
    return WOTBMOD_V3_E_NOT_SUPPORTED;
}

inline WotbModV3Result WOTBMOD_V3_CALL EntityPublicUnsubscribeProperty(
    WotbModV3Handle,
    WotbModV3Token) {
    return WOTBMOD_V3_E_NOT_SUPPORTED;
}

inline WotbModV3Result WOTBMOD_V3_CALL EntityPublicIsVisible(
    WotbModV3Handle,
    WotbModV3EntityHandle entity,
    uint32_t* out_visible) {
    if (!out_visible) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    const WotbModV3PublicEntitySnapshot* value = FindPublicEntity(entity);
    if (!value) return WOTBMOD_V3_E_INVALID_HANDLE;
    *out_visible = value->visible_to_player;
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL EntityPublicGetSnapshot(
    WotbModV3Handle,
    WotbModV3EntityHandle entity,
    WotbModV3PublicEntitySnapshot* out_snapshot) {
    if (!out_snapshot) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    const WotbModV3PublicEntitySnapshot* value = FindPublicEntity(entity);
    if (!value) return WOTBMOD_V3_E_INVALID_HANDLE;
    *out_snapshot = *value;
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL EntityPublicEnumerateVisible(
    WotbModV3Handle mod,
    WotbModV3PublicEntityVisitor visitor,
    void* user_data) {
    if (!visitor) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    for (const WotbModV3PublicEntitySnapshot& value : PublicEntities()) {
        const WotbModV3Result result = visitor(mod, &value, user_data);
        if (result != WOTBMOD_V3_OK) return result;
    }
    return WOTBMOD_V3_OK;
}

inline WotbModV3EntityPublicApiV1& EntityPublicApi() {
    static WotbModV3EntityPublicApiV1 api = {};
    api.struct_size = sizeof(api);
    api.api_version = WOTBMOD_V3_ENTITY_PUBLIC_VERSION;
    api.get_public_id = &EntityPublicGetId;
    api.get_public_type = &EntityPublicGetType;
    api.get_public_property = &EntityPublicGetProperty;
    api.subscribe_public_property = &EntityPublicSubscribeProperty;
    api.unsubscribe_public_property = &EntityPublicUnsubscribeProperty;
    api.is_visible_to_player = &EntityPublicIsVisible;
    api.get_snapshot = &EntityPublicGetSnapshot;
    api.enumerate_visible = &EntityPublicEnumerateVisible;
    return api;
}

inline WotbModV3Result WOTBMOD_V3_CALL VfsResolve(
    WotbModV3Handle,
    const char* uri,
    char* physical_path,
    uint32_t* inout_size) {
    if (!uri || !inout_size) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    Record(vfs_calls, std::string("vfs.resolve(") + uri + ")");
    if (std::strncmp(uri, "mod://", 6u) != 0 &&
        std::strncmp(uri, "game://", 7u) != 0) {
        return WOTBMOD_V3_E_PERMISSION_DENIED;
    }
    const char resolved[] = "C:\\wotbmod-test\\resolved.asset";
    const uint32_t required = static_cast<uint32_t>(sizeof(resolved));
    if (!physical_path || *inout_size < required) {
        *inout_size = required;
        return WOTBMOD_V3_E_BUFFER_TOO_SMALL;
    }
    std::memcpy(physical_path, resolved, required);
    *inout_size = required;
    return WOTBMOD_V3_OK;
}

inline WotbModV3VfsApiV1& VfsApi() {
    static WotbModV3VfsApiV1 api = {};
    api.struct_size = sizeof(api);
    api.api_version = WOTBMOD_V3_VFS_VERSION;
    api.resolve = &VfsResolve;
    return api;
}

// ---------------------------------------------------------------------------
// wotbmod.gameplay.hud - all 34 slots. Each records its call with the
// argument as the slot received it (a colour as eight hex digits, a flag as
// 0/1, an enum as its number) and answers OK, except minimap_set_show_last_
// known, which answers E_NOT_SUPPORTED the way the 11.20.0.887 client does -
// so a facade's pass-through of a real refusal is tested against one.
// ---------------------------------------------------------------------------

inline std::string HudHex(uint32_t value) {
    static const char kDigits[] = "0123456789ABCDEF";
    std::string text(8u, '0');
    for (int index = 7; index >= 0; --index) {
        text[static_cast<size_t>(index)] = kDigits[value & 0xFu];
        value >>= 4u;
    }
    return text;
}

inline std::string HudNumber(float value) {
    char text[32] = {};
    std::snprintf(text, sizeof(text), "%g", static_cast<double>(value));
    return text;
}

inline WotbModV3Result HudRecord(const char* slot, const std::string& args) {
    Record(hud_calls, std::string("gameplay_hud.") + slot + "(" + args + ")");
    if (forced_result != WOTBMOD_V3_OK) return forced_result;
    return WOTBMOD_V3_OK;
}

#define WOTB_MOCK_HUD_U32(name)                                       \
    inline WotbModV3Result WOTBMOD_V3_CALL Hud_##name(WotbModV3Handle, \
                                                      uint32_t value) { \
        return HudRecord(#name, std::to_string(value));                 \
    }
#define WOTB_MOCK_HUD_RGBA(name)                                      \
    inline WotbModV3Result WOTBMOD_V3_CALL Hud_##name(WotbModV3Handle, \
                                                      uint32_t value) { \
        return HudRecord(#name, HudHex(value));                         \
    }
#define WOTB_MOCK_HUD_F32(name)                                       \
    inline WotbModV3Result WOTBMOD_V3_CALL Hud_##name(WotbModV3Handle, \
                                                      float value) {    \
        return HudRecord(#name, HudNumber(value));                      \
    }
#define WOTB_MOCK_HUD_STR(name)                                       \
    inline WotbModV3Result WOTBMOD_V3_CALL Hud_##name(WotbModV3Handle, \
                                                      const char* value) { \
        return HudRecord(#name, value ? value : "");                    \
    }

WOTB_MOCK_HUD_STR(reticle_set_texture)
WOTB_MOCK_HUD_RGBA(reticle_set_color)
WOTB_MOCK_HUD_F32(reticle_set_size)
WOTB_MOCK_HUD_STR(reticle_set_sniper_texture)
WOTB_MOCK_HUD_U32(reticle_set_reloading_indicator)
WOTB_MOCK_HUD_U32(reticle_set_dispersion_circle)
WOTB_MOCK_HUD_U32(damagelog_set_enabled)
WOTB_MOCK_HUD_U32(damagelog_set_position)
WOTB_MOCK_HUD_U32(damagelog_set_max_entries)
WOTB_MOCK_HUD_U32(damagelog_set_show_blocked)
WOTB_MOCK_HUD_U32(damagelog_set_show_ricochet)
WOTB_MOCK_HUD_U32(damagelog_set_show_module_damage)
WOTB_MOCK_HUD_STR(damagelog_set_format)
WOTB_MOCK_HUD_U32(damagelog_set_filter_own)
WOTB_MOCK_HUD_U32(session_stats_set_enabled)
WOTB_MOCK_HUD_U32(session_stats_set_fields)
WOTB_MOCK_HUD_F32(minimap_set_size)
WOTB_MOCK_HUD_F32(minimap_set_opacity)
WOTB_MOCK_HUD_U32(minimap_set_show_artillery_range)
WOTB_MOCK_HUD_U32(minimap_set_show_drawing)
WOTB_MOCK_HUD_U32(minimap_remove_marker)
WOTB_MOCK_HUD_STR(sixth_sense_set_texture)
WOTB_MOCK_HUD_STR(sixth_sense_set_sound)
WOTB_MOCK_HUD_F32(sixth_sense_set_scale)
WOTB_MOCK_HUD_F32(sixth_sense_set_delay_ms)
WOTB_MOCK_HUD_U32(hit_indicator_set_style)
WOTB_MOCK_HUD_RGBA(hit_indicator_set_color_hit)
WOTB_MOCK_HUD_RGBA(hit_indicator_set_color_pen)
WOTB_MOCK_HUD_RGBA(hit_indicator_set_color_ricochet)
WOTB_MOCK_HUD_RGBA(hit_indicator_set_color_crit)

#undef WOTB_MOCK_HUD_U32
#undef WOTB_MOCK_HUD_RGBA
#undef WOTB_MOCK_HUD_F32
#undef WOTB_MOCK_HUD_STR

inline WotbModV3Result WOTBMOD_V3_CALL Hud_minimap_set_show_last_known(
    WotbModV3Handle, uint32_t value) {
    HudRecord("minimap_set_show_last_known", std::to_string(value));
    return WOTBMOD_V3_E_NOT_SUPPORTED;
}

inline WotbModV3Result WOTBMOD_V3_CALL Hud_minimap_add_marker(
    WotbModV3Handle, float world_x, float world_z, const char* label,
    uint32_t color, uint32_t* out_marker_id) {
    const WotbModV3Result result = HudRecord(
        "minimap_add_marker", HudNumber(world_x) + "," + HudNumber(world_z) +
                                  "," + (label ? label : "") + "," +
                                  HudHex(color));
    if (result != WOTBMOD_V3_OK) return result;
    if (!out_marker_id) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    *out_marker_id = 7u;
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL Hud_sixth_sense_set_position(
    WotbModV3Handle, WotbModV3Vec2 position) {
    return HudRecord("sixth_sense_set_position",
                     HudNumber(position.x) + "," + HudNumber(position.y));
}

inline WotbModV3Result WOTBMOD_V3_CALL Hud_reset(WotbModV3Handle) {
    return HudRecord("reset", "");
}

inline WotbModV3GameplayHudApiV1& GameplayHudApi() {
    static WotbModV3GameplayHudApiV1 api = {};
    api.struct_size = sizeof(api);
    api.api_version = WOTBMOD_V3_GAMEPLAY_HUD_VERSION;
    api.reticle_set_texture = &Hud_reticle_set_texture;
    api.reticle_set_color = &Hud_reticle_set_color;
    api.reticle_set_size = &Hud_reticle_set_size;
    api.reticle_set_sniper_texture = &Hud_reticle_set_sniper_texture;
    api.reticle_set_reloading_indicator = &Hud_reticle_set_reloading_indicator;
    api.reticle_set_dispersion_circle = &Hud_reticle_set_dispersion_circle;
    api.damagelog_set_enabled = &Hud_damagelog_set_enabled;
    api.damagelog_set_position = &Hud_damagelog_set_position;
    api.damagelog_set_max_entries = &Hud_damagelog_set_max_entries;
    api.damagelog_set_show_blocked = &Hud_damagelog_set_show_blocked;
    api.damagelog_set_show_ricochet = &Hud_damagelog_set_show_ricochet;
    api.damagelog_set_show_module_damage =
        &Hud_damagelog_set_show_module_damage;
    api.damagelog_set_format = &Hud_damagelog_set_format;
    api.damagelog_set_filter_own = &Hud_damagelog_set_filter_own;
    api.session_stats_set_enabled = &Hud_session_stats_set_enabled;
    api.session_stats_set_fields = &Hud_session_stats_set_fields;
    api.minimap_set_size = &Hud_minimap_set_size;
    api.minimap_set_opacity = &Hud_minimap_set_opacity;
    api.minimap_set_show_last_known = &Hud_minimap_set_show_last_known;
    api.minimap_set_show_artillery_range =
        &Hud_minimap_set_show_artillery_range;
    api.minimap_set_show_drawing = &Hud_minimap_set_show_drawing;
    api.minimap_add_marker = &Hud_minimap_add_marker;
    api.minimap_remove_marker = &Hud_minimap_remove_marker;
    api.sixth_sense_set_texture = &Hud_sixth_sense_set_texture;
    api.sixth_sense_set_sound = &Hud_sixth_sense_set_sound;
    api.sixth_sense_set_position = &Hud_sixth_sense_set_position;
    api.sixth_sense_set_scale = &Hud_sixth_sense_set_scale;
    api.sixth_sense_set_delay_ms = &Hud_sixth_sense_set_delay_ms;
    api.hit_indicator_set_style = &Hud_hit_indicator_set_style;
    api.hit_indicator_set_color_hit = &Hud_hit_indicator_set_color_hit;
    api.hit_indicator_set_color_pen = &Hud_hit_indicator_set_color_pen;
    api.hit_indicator_set_color_ricochet = &Hud_hit_indicator_set_color_ricochet;
    api.hit_indicator_set_color_crit = &Hud_hit_indicator_set_color_crit;
    api.reset = &Hud_reset;
    return api;
}

// ---------------------------------------------------------------------------
// The interfaces the stage-1b facades stand on. Each slot records its call
// and answers a fixed, checkable value; a slot the facade must treat as
// absent is simply left null.
// ---------------------------------------------------------------------------

// wotbmod.vehicle.visual
inline WotbModV3Result WOTBMOD_V3_CALL VvGetLocalVehicle(
    WotbModV3Handle, WotbModV3EntityHandle* out_vehicle) {
    FacadeRecord("vehicle_visual.get_local_player_vehicle", "");
    if (!out_vehicle) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    *out_vehicle = 10101u;
    generic_handles.push_back({10101u, 1u});
    return WOTBMOD_V3_OK;
}
inline WotbModV3Result WOTBMOD_V3_CALL VvIsLocal(
    WotbModV3Handle, WotbModV3EntityHandle vehicle, uint32_t* out_is_local) {
    FacadeRecord("vehicle_visual.is_local_player", std::to_string(vehicle));
    if (!out_is_local) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    if (!FindPublicEntity(vehicle)) return WOTBMOD_V3_E_INVALID_HANDLE;
    *out_is_local = vehicle == 10101u ? 1u : 0u;
    return WOTBMOD_V3_OK;
}
inline WotbModV3Result WOTBMOD_V3_CALL VvGetPosition(
    WotbModV3Handle, WotbModV3EntityHandle vehicle, WotbModV3Vec3* out_position) {
    FacadeRecord("vehicle_visual.get_position", std::to_string(vehicle));
    if (!out_position) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    const WotbModV3PublicEntitySnapshot* value = FindPublicEntity(vehicle);
    if (!value) return WOTBMOD_V3_E_INVALID_HANDLE;
    *out_position = value->position;
    return WOTBMOD_V3_OK;
}
inline WotbModV3Result WOTBMOD_V3_CALL VvGetAppearanceState(
    WotbModV3Handle, WotbModV3EntityHandle vehicle, uint32_t* out_state) {
    FacadeRecord("vehicle_visual.get_appearance_state", std::to_string(vehicle));
    if (!out_state) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    *out_state = WOTBMOD_V3_VEHICLE_APPEARANCE_READY;
    return WOTBMOD_V3_OK;
}
inline WotbModV3Result WOTBMOD_V3_CALL VvRestoreAppearance(
    WotbModV3Handle, WotbModV3EntityHandle vehicle) {
    return FacadeRecord("vehicle_visual.restore_appearance", std::to_string(vehicle));
}
inline WotbModV3Result WOTBMOD_V3_CALL VvSetCustomSkin(
    WotbModV3Handle, WotbModV3EntityHandle vehicle, const char* uri) {
    return FacadeRecord("vehicle_visual.set_custom_skin",
                        std::to_string(vehicle) + "," + (uri ? uri : ""));
}
inline WotbModV3Result WOTBMOD_V3_CALL VvSetCustomCamouflage(
    WotbModV3Handle, WotbModV3EntityHandle vehicle, const char* uri) {
    return FacadeRecord("vehicle_visual.set_custom_camouflage",
                        std::to_string(vehicle) + "," + (uri ? uri : ""));
}
inline WotbModV3Result WOTBMOD_V3_CALL VvSkinPackRegister(
    WotbModV3Handle, const WotbModV3VehicleSkinPack* pack, WotbModV3Handle* out_pack) {
    if (!pack || !out_pack) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    const WotbModV3Result result = FacadeRecord(
        "vehicle_visual.skin_pack_register",
        std::string(pack->id ? pack->id : "") + "," +
            (pack->vehicle_name ? pack->vehicle_name : "") + "," +
            std::to_string(pack->asset_count) + "," + std::to_string(pack->priority) +
            "," + std::to_string(pack->hangar_only));
    if (result != WOTBMOD_V3_OK) return result;
    if (!pack->id || !pack->id[0]) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    *out_pack = MintFacadeHandle();
    return WOTBMOD_V3_OK;
}
inline WotbModV3Result WOTBMOD_V3_CALL VvSkinPackApply(
    WotbModV3Handle, WotbModV3Handle pack, WotbModV3EntityHandle vehicle) {
    return FacadeRecord("vehicle_visual.skin_pack_apply",
                        std::to_string(pack) + "," + std::to_string(vehicle));
}
inline WotbModV3Result WOTBMOD_V3_CALL VvSkinPackRollback(
    WotbModV3Handle, WotbModV3Handle pack) {
    return FacadeRecord("vehicle_visual.skin_pack_rollback", std::to_string(pack));
}
inline WotbModV3Result WOTBMOD_V3_CALL VvSkinPackGetState(
    WotbModV3Handle, WotbModV3Handle pack, WotbModV3VehicleSkinState* out_state) {
    FacadeRecord("vehicle_visual.skin_pack_get_state", std::to_string(pack));
    if (!out_state) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    out_state->applied = 1u;
    out_state->asset_count = 2u;
    out_state->mounted_asset_count = 2u;
    out_state->requires_model_reload = 1u;
    out_state->vehicle = 10101u;
    return WOTBMOD_V3_OK;
}
inline WotbModV3Result WOTBMOD_V3_CALL VvSkinPackRelease(
    WotbModV3Handle, WotbModV3Handle pack) {
    return FacadeRecord("vehicle_visual.skin_pack_release", std::to_string(pack));
}
inline WotbModV3VehicleVisualApiV2& VehicleVisualApi() {
    static WotbModV3VehicleVisualApiV2 api = {};
    api.v1.struct_size = sizeof(api.v1);
    api.v1.api_version = WOTBMOD_V3_VEHICLE_VISUAL_VERSION;
    api.v1.get_local_player_vehicle = &VvGetLocalVehicle;
    api.v1.is_local_player = &VvIsLocal;
    api.v1.get_position = &VvGetPosition;
    api.v1.get_appearance_state = &VvGetAppearanceState;
    api.v1.restore_appearance = &VvRestoreAppearance;
    api.v1.set_custom_skin = &VvSetCustomSkin;
    api.v1.set_custom_camouflage = &VvSetCustomCamouflage;
    api.skin_pack_register = &VvSkinPackRegister;
    api.skin_pack_apply = &VvSkinPackApply;
    api.skin_pack_rollback = &VvSkinPackRollback;
    api.skin_pack_get_state = &VvSkinPackGetState;
    api.skin_pack_release = &VvSkinPackRelease;
    return api;
}

// wotbmod.projectile
inline WotbModV3Result WOTBMOD_V3_CALL PjTracerRegister(
    WotbModV3Handle, const WotbModV3TracerStyleDescriptor* descriptor,
    WotbModV3Handle* out_style) {
    if (!descriptor || !out_style) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    const WotbModV3Result result = FacadeRecord(
        "projectile.tracer_style_register",
        std::string(descriptor->id ? descriptor->id : "") + "," +
            HudNumber(descriptor->width));
    if (result != WOTBMOD_V3_OK) return result;
    *out_style = MintFacadeHandle();
    return WOTBMOD_V3_OK;
}
inline WotbModV3Result WOTBMOD_V3_CALL PjTracerUnregister(
    WotbModV3Handle, WotbModV3Handle style) {
    return FacadeRecord("projectile.tracer_style_unregister", std::to_string(style));
}
inline WotbModV3Result WOTBMOD_V3_CALL PjGetSnapshot(
    WotbModV3Handle, WotbModV3ProjectileHandle projectile,
    WotbModV3ProjectileSnapshot* out_snapshot) {
    FacadeRecord("projectile.projectile_get_snapshot", std::to_string(projectile));
    if (!out_snapshot) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    if (projectile != 7001u) return WOTBMOD_V3_E_INVALID_HANDLE;
    out_snapshot->projectile = projectile;
    out_snapshot->sequence_id = 5u;
    out_snapshot->valid_fields = WOTBMOD_V3_PROJECTILE_FIELD_ORIGIN |
                                 WOTBMOD_V3_PROJECTILE_FIELD_DIRECTION;
    out_snapshot->lifecycle_state = WOTBMOD_V3_PROJECTILE_STATE_IN_FLIGHT;
    out_snapshot->source = WOTBMOD_V3_PROJECTILE_SOURCE_STOCK_SHOT;
    out_snapshot->owner_scope = WOTBMOD_V3_PROJECTILE_OWNER_LOCAL_PLAYER;
    out_snapshot->origin = {1.0f, 2.0f, 3.0f};
    out_snapshot->visible_direction = {0.0f, 0.0f, 1.0f};
    return WOTBMOD_V3_OK;
}
inline WotbModV3Result WOTBMOD_V3_CALL PjImpactRegister(
    WotbModV3Handle, const WotbModV3ImpactVisualDescriptor* descriptor,
    WotbModV3Handle* out_visual) {
    if (!descriptor || !out_visual) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    const WotbModV3Result result = FacadeRecord(
        "projectile.impact_visual_register",
        std::string(descriptor->id ? descriptor->id : "") + "," +
            (descriptor->scene_uri ? descriptor->scene_uri : ""));
    if (result != WOTBMOD_V3_OK) return result;
    *out_visual = MintFacadeHandle();
    return WOTBMOD_V3_OK;
}
inline WotbModV3Result WOTBMOD_V3_CALL PjImpactUpdate(
    WotbModV3Handle, WotbModV3Handle visual,
    const WotbModV3ImpactVisualDescriptor* descriptor) {
    if (!descriptor) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    return FacadeRecord("projectile.impact_visual_update",
                        std::to_string(visual) + "," + HudNumber(descriptor->uniform_scale));
}
inline WotbModV3Result WOTBMOD_V3_CALL PjImpactUnregister(
    WotbModV3Handle, WotbModV3Handle visual) {
    return FacadeRecord("projectile.impact_visual_unregister", std::to_string(visual));
}
inline WotbModV3ProjectileApiV2& ProjectileApi() {
    static WotbModV3ProjectileApiV2 api = {};
    api.struct_size = sizeof(api);
    api.api_version = WOTBMOD_V3_PROJECTILE_VERSION_2;
    api.tracer_style_register = &PjTracerRegister;
    api.tracer_style_unregister = &PjTracerUnregister;
    api.projectile_get_snapshot = &PjGetSnapshot;
    api.impact_visual_register = &PjImpactRegister;
    api.impact_visual_update = &PjImpactUpdate;
    api.impact_visual_unregister = &PjImpactUnregister;
    return api;
}

// wotbmod.camera, wotbmod.gameplay.camera, wotbmod.camera.state
inline WotbModV3Result WOTBMOD_V3_CALL CamGetActive(
    WotbModV3Handle, WotbModV3CameraHandle* out_camera) {
    FacadeRecord("camera.get_active", "");
    if (!out_camera) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    *out_camera = MintFacadeHandle();
    return WOTBMOD_V3_OK;
}
inline WotbModV3Result WOTBMOD_V3_CALL CamGetMode(
    WotbModV3Handle, WotbModV3CameraHandle, uint32_t* out_mode) {
    FacadeRecord("camera.get_mode", "");
    if (!out_mode) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    *out_mode = WOTBMOD_V3_CAMERA_MODE_ARCADE;
    return WOTBMOD_V3_OK;
}
inline WotbModV3Result WOTBMOD_V3_CALL CamGetTransform(
    WotbModV3Handle, WotbModV3CameraHandle, WotbModV3Transform* out_transform) {
    FacadeRecord("camera.get_transform", "");
    if (!out_transform) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    out_transform->position = {10.0f, 20.0f, 30.0f};
    out_transform->scale = {1.0f, 1.0f, 1.0f};
    return WOTBMOD_V3_OK;
}
inline WotbModV3Result WOTBMOD_V3_CALL CamGetFov(
    WotbModV3Handle, WotbModV3CameraHandle, float* out_degrees) {
    FacadeRecord("camera.get_fov", "");
    if (!out_degrees) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    *out_degrees = gameplay_fov;
    return WOTBMOD_V3_OK;
}
inline WotbModV3Result WOTBMOD_V3_CALL CamWorldToScreen(
    WotbModV3Handle, WotbModV3CameraHandle, const WotbModV3Vec3* world,
    WotbModV3Vec3* out_screen) {
    FacadeRecord("camera.world_to_screen", "");
    if (!world || !out_screen) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    *out_screen = {world->x * 2.0f, world->y * 2.0f, 0.0f};
    return WOTBMOD_V3_OK;
}
inline WotbModV3Result WOTBMOD_V3_CALL CamScreenToWorld(
    WotbModV3Handle, WotbModV3CameraHandle, const WotbModV3Vec3* screen,
    WotbModV3Vec3* out_world) {
    FacadeRecord("camera.screen_to_world", "");
    if (!screen || !out_world) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    *out_world = {screen->x / 2.0f, screen->y / 2.0f, 0.0f};
    return WOTBMOD_V3_OK;
}
inline WotbModV3Result WOTBMOD_V3_CALL CamTransitionTo(
    WotbModV3Handle, WotbModV3CameraHandle,
    const WotbModV3CameraTransition* transition) {
    if (!transition) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    return FacadeRecord("camera.transition_to",
                        HudNumber(transition->duration_seconds) + "," +
                            std::to_string(transition->preserve_game_control));
}
inline WotbModV3Result WOTBMOD_V3_CALL CamAddShake(
    WotbModV3Handle, WotbModV3CameraHandle, const WotbModV3CameraShake* shake) {
    if (!shake) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    return FacadeRecord("camera.add_shake", HudNumber(shake->amplitude) + "," +
                                                HudNumber(shake->duration_seconds));
}
inline WotbModV3CameraApiV1& CameraApi() {
    static WotbModV3CameraApiV1 api = {};
    api.struct_size = sizeof(api);
    api.api_version = WOTBMOD_V3_CAMERA_VERSION;
    api.get_active = &CamGetActive;
    api.get_mode = &CamGetMode;
    api.get_transform = &CamGetTransform;
    api.get_fov = &CamGetFov;
    api.world_to_screen = &CamWorldToScreen;
    api.screen_to_world = &CamScreenToWorld;
    api.transition_to = &CamTransitionTo;
    api.add_shake = &CamAddShake;
    return api;
}

inline WotbModV3Result WOTBMOD_V3_CALL GcSetFov(WotbModV3Handle, float degrees) {
    const WotbModV3Result result = FacadeRecord("gameplay_camera.set_fov", HudNumber(degrees));
    if (result == WOTBMOD_V3_OK) gameplay_fov = degrees;
    return result;
}
inline WotbModV3Result WOTBMOD_V3_CALL GcGetFov(WotbModV3Handle, float* out_degrees) {
    FacadeRecord("gameplay_camera.get_fov", "");
    if (!out_degrees) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    *out_degrees = gameplay_fov;
    return WOTBMOD_V3_OK;
}
inline WotbModV3Result WOTBMOD_V3_CALL GcSetFovHangar(WotbModV3Handle, float degrees) {
    return FacadeRecord("gameplay_camera.set_fov_hangar", HudNumber(degrees));
}
inline WotbModV3Result WOTBMOD_V3_CALL GcSetFovBattle(WotbModV3Handle, float degrees) {
    return FacadeRecord("gameplay_camera.set_fov_battle", HudNumber(degrees));
}
inline WotbModV3Result WOTBMOD_V3_CALL GcSetFovSniper(WotbModV3Handle, float degrees) {
    return FacadeRecord("gameplay_camera.set_fov_sniper", HudNumber(degrees));
}
inline WotbModV3Result WOTBMOD_V3_CALL GcResetFov(WotbModV3Handle) {
    const WotbModV3Result result = FacadeRecord("gameplay_camera.reset_fov", "");
    if (result == WOTBMOD_V3_OK) gameplay_fov = 75.0f;
    return result;
}
inline WotbModV3GameplayCameraApiV1& GameplayCameraApi() {
    static WotbModV3GameplayCameraApiV1 api = {};
    api.struct_size = sizeof(api);
    api.api_version = WOTBMOD_V3_GAMEPLAY_CAMERA_VERSION;
    api.set_fov = &GcSetFov;
    api.get_fov = &GcGetFov;
    api.set_fov_hangar = &GcSetFovHangar;
    api.set_fov_battle = &GcSetFovBattle;
    api.set_fov_sniper = &GcSetFovSniper;
    api.reset_fov = &GcResetFov;
    return api;
}

inline WotbModV3Result WOTBMOD_V3_CALL CsGetObservedState(
    WotbModV3Handle, WotbModV3CameraObservedState* out_state) {
    FacadeRecord("camera_state.get_observed_state", "");
    if (!out_state) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    out_state->animation_state = 5u;
    out_state->animation_state_valid = 1u;
    out_state->view_mode = 1u;
    out_state->view_mode_valid = 0u;
    return WOTBMOD_V3_OK;
}
inline WotbModV3CameraApiV2& CameraStateApi() {
    static WotbModV3CameraApiV2 api = {};
    api.struct_size = sizeof(api);
    api.api_version = WOTBMOD_V3_CAMERA_VERSION_2;
    api.get_observed_state = &CsGetObservedState;
    return api;
}

// wotbmod.audio
inline WotbModV3Result WOTBMOD_V3_CALL AuCreate(
    WotbModV3Handle, const WotbModV3AudioDescriptor* descriptor,
    WotbModV3AudioHandle* out_audio) {
    if (!descriptor || !out_audio) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    const WotbModV3Result result = FacadeRecord(
        "audio.create", std::string(descriptor->uri ? descriptor->uri : "") + "," +
                            std::to_string(descriptor->flags) + "," +
                            HudNumber(descriptor->volume) + "," +
                            (descriptor->bus ? descriptor->bus : ""));
    if (result != WOTBMOD_V3_OK) return result;
    if (!descriptor->uri || !descriptor->uri[0]) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    *out_audio = MintFacadeHandle();
    return WOTBMOD_V3_OK;
}
inline WotbModV3Result WOTBMOD_V3_CALL AuPlay(WotbModV3Handle, WotbModV3AudioHandle audio) {
    return FacadeRecord("audio.play", std::to_string(audio));
}
inline WotbModV3Result WOTBMOD_V3_CALL AuStop(
    WotbModV3Handle, WotbModV3AudioHandle audio, float fade_seconds) {
    return FacadeRecord("audio.stop", std::to_string(audio) + "," + HudNumber(fade_seconds));
}
inline WotbModV3Result WOTBMOD_V3_CALL AuDestroy(WotbModV3Handle, WotbModV3AudioHandle audio) {
    return FacadeRecord("audio.destroy", std::to_string(audio));
}
inline WotbModV3Result WOTBMOD_V3_CALL AuSetVolume(
    WotbModV3Handle, WotbModV3AudioHandle audio, float volume) {
    return FacadeRecord("audio.set_volume", std::to_string(audio) + "," + HudNumber(volume));
}
inline WotbModV3Result WOTBMOD_V3_CALL AuIsPlaying(
    WotbModV3Handle, WotbModV3AudioHandle audio, uint32_t* out_playing) {
    FacadeRecord("audio.is_playing", std::to_string(audio));
    if (!out_playing) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    *out_playing = 1u;
    return WOTBMOD_V3_OK;
}
inline WotbModV3Result WOTBMOD_V3_CALL AuOnFinished(
    WotbModV3Handle, WotbModV3AudioHandle audio, WotbModV3AudioLifecycleCallback callback,
    void*, WotbModV3Token* out_token) {
    FacadeRecord("audio.on_finished", std::to_string(audio));
    if (!callback || !out_token) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    *out_token = MintFacadeHandle();
    return WOTBMOD_V3_OK;
}
inline WotbModV3Result WOTBMOD_V3_CALL AuOverrideRegister(
    WotbModV3Handle, const char* event_name, const char* replacement_uri,
    int32_t priority, WotbModV3Token* out_token) {
    const WotbModV3Result result = FacadeRecord(
        "audio.sound_override_register",
        std::string(event_name ? event_name : "") + "," +
            (replacement_uri ? replacement_uri : "") + "," + std::to_string(priority));
    if (result != WOTBMOD_V3_OK) return result;
    if (!out_token) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    *out_token = MintFacadeHandle();
    return WOTBMOD_V3_OK;
}
inline WotbModV3Result WOTBMOD_V3_CALL AuOverrideUnregister(
    WotbModV3Handle, WotbModV3Token token) {
    return FacadeRecord("audio.sound_override_unregister", std::to_string(token));
}
inline WotbModV3AudioApiV2& AudioApi() {
    static WotbModV3AudioApiV2 api = {};
    api.struct_size = sizeof(api);
    api.api_version = WOTBMOD_V3_AUDIO_VERSION;
    api.create = &AuCreate;
    api.play = &AuPlay;
    api.stop = &AuStop;
    api.destroy = &AuDestroy;
    api.set_volume = &AuSetVolume;
    api.is_playing = &AuIsPlaying;
    api.on_finished = &AuOnFinished;
    api.sound_override_register = &AuOverrideRegister;
    api.sound_override_unregister = &AuOverrideUnregister;
    return api;
}

// wotbmod.input
struct InputSubscriptionRecord {
    WotbModV3Token token = 0u;
    WotbModV3Handle action = 0u;
    WotbModV3InputActionCallback callback = nullptr;
    void* user_data = nullptr;
};
inline std::vector<InputSubscriptionRecord> input_subscriptions;

inline WotbModV3Result WOTBMOD_V3_CALL InRegisterAction(
    WotbModV3Handle, const WotbModV3InputActionDesc* desc, WotbModV3Handle* out_action) {
    if (!desc || !out_action) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    const uint32_t code = desc->default_bindings && desc->default_binding_count
                              ? desc->default_bindings[0].code
                              : 0u;
    const uint32_t modifiers = desc->default_bindings && desc->default_binding_count
                                   ? desc->default_bindings[0].modifiers
                                   : 0u;
    const WotbModV3Result result = FacadeRecord(
        "input.register_action", std::string(desc->id) + "," +
                                     std::to_string(desc->value_type) + "," +
                                     std::to_string(code) + "," + std::to_string(modifiers));
    if (result != WOTBMOD_V3_OK) return result;
    if (!desc->id[0]) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    *out_action = MintFacadeHandle();
    return WOTBMOD_V3_OK;
}
inline WotbModV3Result WOTBMOD_V3_CALL InUnregisterAction(
    WotbModV3Handle, WotbModV3Handle action) {
    return FacadeRecord("input.unregister_action", std::to_string(action));
}
inline WotbModV3Result WOTBMOD_V3_CALL InIsPressed(
    WotbModV3Handle, WotbModV3Handle action, uint32_t* out_pressed) {
    FacadeRecord("input.is_action_pressed", std::to_string(action));
    if (!out_pressed) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    *out_pressed = 1u;
    return WOTBMOD_V3_OK;
}
inline WotbModV3Result WOTBMOD_V3_CALL InIsDown(
    WotbModV3Handle, WotbModV3Handle action, uint32_t* out_down) {
    FacadeRecord("input.is_action_down", std::to_string(action));
    if (!out_down) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    *out_down = 0u;
    return WOTBMOD_V3_OK;
}
inline WotbModV3Result WOTBMOD_V3_CALL InSubscribe(
    WotbModV3Handle, WotbModV3Handle action, WotbModV3InputActionCallback callback,
    void* user_data, WotbModV3Token* out_token) {
    FacadeRecord("input.subscribe", std::to_string(action));
    if (!callback || !out_token) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    if (input_subscriptions_reset_pending) {
        input_subscriptions.clear();
        input_subscriptions_reset_pending = false;
    }
    InputSubscriptionRecord record;
    record.token = MintFacadeHandle();
    record.action = action;
    record.callback = callback;
    record.user_data = user_data;
    input_subscriptions.push_back(record);
    *out_token = record.token;
    return WOTBMOD_V3_OK;
}
// Delivers one action edge to every subscription on `action`, the way the
// client's input dispatch would.
inline void FireInputAction(WotbModV3Handle action, float value, uint32_t pressed) {
    const std::vector<InputSubscriptionRecord> copy = input_subscriptions;
    for (const InputSubscriptionRecord& record : copy) {
        if ((action == 0u || record.action == action) && record.callback) {
            record.callback(1u, record.action, value, pressed, record.user_data);
        }
    }
}
inline WotbModV3Result WOTBMOD_V3_CALL InCaptureBegin(WotbModV3Handle, uint64_t contexts) {
    return FacadeRecord("input.capture_begin", std::to_string(contexts));
}
inline WotbModV3Result WOTBMOD_V3_CALL InCaptureEnd(
    WotbModV3Handle, WotbModV3InputBinding* out_binding) {
    FacadeRecord("input.capture_end", "");
    if (!out_binding) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    out_binding->device = WOTBMOD_V3_INPUT_DEVICE_KEYBOARD;
    out_binding->code = 0x76u;
    out_binding->modifiers = 0u;
    out_binding->scale = 1.0f;
    return WOTBMOD_V3_OK;
}
inline WotbModV3Result WOTBMOD_V3_CALL InFindConflicts(
    WotbModV3Handle mod, WotbModV3Handle action, WotbModV3InputConflict* conflicts,
    uint32_t* inout_count) {
    FacadeRecord("input.find_conflicts", std::to_string(action));
    if (!inout_count) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    if (!conflicts || *inout_count < 1u) {
        *inout_count = 1u;
        return conflicts ? WOTBMOD_V3_E_BUFFER_TOO_SMALL : WOTBMOD_V3_OK;
    }
    conflicts[0].owner_mod = mod;
    conflicts[0].action = action;
    strncpy_s(conflicts[0].action_id, "other.mod.toggle", _TRUNCATE);
    conflicts[0].binding.device = WOTBMOD_V3_INPUT_DEVICE_KEYBOARD;
    conflicts[0].binding.code = 0x76u;
    conflicts[0].binding.scale = 1.0f;
    conflicts[0].overlapping_contexts = 4u;
    *inout_count = 1u;
    return WOTBMOD_V3_OK;
}
inline WotbModV3Result WOTBMOD_V3_CALL InGetBindings(
    WotbModV3Handle, WotbModV3Handle action, WotbModV3InputBinding* bindings,
    uint32_t* inout_count) {
    FacadeRecord("input.get_bindings", std::to_string(action));
    if (!inout_count) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    if (!bindings || *inout_count < 1u) {
        *inout_count = 1u;
        return bindings ? WOTBMOD_V3_E_BUFFER_TOO_SMALL : WOTBMOD_V3_OK;
    }
    bindings[0].device = WOTBMOD_V3_INPUT_DEVICE_KEYBOARD;
    bindings[0].code = 0x76u;
    bindings[0].scale = 1.0f;
    *inout_count = 1u;
    return WOTBMOD_V3_OK;
}
inline WotbModV3InputApiV1& InputApi() {
    static WotbModV3InputApiV1 api = {};
    api.struct_size = sizeof(api);
    api.api_version = WOTBMOD_V3_INPUT_VERSION;
    api.register_action = &InRegisterAction;
    api.unregister_action = &InUnregisterAction;
    api.is_action_pressed = &InIsPressed;
    api.is_action_down = &InIsDown;
    api.subscribe = &InSubscribe;
    api.capture_begin = &InCaptureBegin;
    api.capture_end = &InCaptureEnd;
    api.find_conflicts = &InFindConflicts;
    api.get_bindings = &InGetBindings;
    return api;
}

// wotbmod.resources, wotbmod.loaders
inline WotbModV3Result WOTBMOD_V3_CALL RsLoad(
    WotbModV3Handle, const WotbModV3ResourceLoadDesc* desc, WotbModV3ResourceHandle* out_resource) {
    if (!desc || !out_resource) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    const WotbModV3Result result = FacadeRecord(
        "resources.load", std::to_string(desc->expected_type) + "," +
                              (desc->uri ? desc->uri : ""));
    if (result != WOTBMOD_V3_OK) return result;
    *out_resource = MintFacadeHandle();
    return WOTBMOD_V3_OK;
}
inline WotbModV3Result WOTBMOD_V3_CALL RsGetInfo(
    WotbModV3Handle, WotbModV3ResourceHandle resource, WotbModV3ResourceInfo* out_info) {
    FacadeRecord("resources.get_info", std::to_string(resource));
    if (!out_info) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    out_info->type = WOTBMOD_V3_RESOURCE_IMAGE;
    out_info->state = WOTBMOD_V3_RESOURCE_READY;
    out_info->progress = 1.0f;
    out_info->memory_bytes = 4096u;
    strncpy_s(out_info->uri, "mod://self/hud-overlay.png", _TRUNCATE);
    return WOTBMOD_V3_OK;
}
inline WotbModV3Result WOTBMOD_V3_CALL RsRelease(WotbModV3Handle, WotbModV3ResourceHandle resource) {
    return FacadeRecord("resources.release", std::to_string(resource));
}
inline WotbModV3ResourcesApiV1& ResourcesApi() {
    static WotbModV3ResourcesApiV1 api = {};
    api.struct_size = sizeof(api);
    api.api_version = WOTBMOD_V3_RESOURCES_VERSION;
    api.load = &RsLoad;
    api.get_info = &RsGetInfo;
    api.release = &RsRelease;
    return api;
}

inline WotbModV3Result LoaderText(const char* uri, WotbModV3Buffer* inout_buffer) {
    if (!uri || !inout_buffer) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    std::string text;
    if (std::strcmp(uri, "mod://self/hello.txt") == 0) {
        text = "hello, tour";
    } else if (std::strcmp(uri, "mod://self/hello.json") == 0) {
        text = "{\"answer\":42,\"list\":[1,2]}";
    } else {
        return WOTBMOD_V3_E_NOT_FOUND;
    }
    inout_buffer->size = static_cast<uint32_t>(text.size());
    if (!inout_buffer->data || inout_buffer->capacity < text.size()) {
        return WOTBMOD_V3_E_BUFFER_TOO_SMALL;
    }
    std::memcpy(inout_buffer->data, text.data(), text.size());
    return WOTBMOD_V3_OK;
}
inline WotbModV3Result WOTBMOD_V3_CALL LdLoadText(
    WotbModV3Handle, const char* uri, uint64_t max_bytes, WotbModV3Buffer* inout_buffer) {
    FacadeRecord("loaders.load_text_utf8",
                 std::string(uri ? uri : "") + "," + std::to_string(max_bytes));
    return LoaderText(uri, inout_buffer);
}
inline WotbModV3Result WOTBMOD_V3_CALL LdLoadBinary(
    WotbModV3Handle, const char* uri, uint64_t max_bytes, WotbModV3Buffer* inout_buffer) {
    FacadeRecord("loaders.load_binary",
                 std::string(uri ? uri : "") + "," + std::to_string(max_bytes));
    return LoaderText(uri, inout_buffer);
}
// wotbmod.session.cluster - four EU clusters, EU_C3 current, EU_C0 not alive.
// change() is recorded in facade_calls and honours forced_result;
// FireClusterChanged delivers the runtime's typed changed event.
struct MockClusterRecord {
    int32_t id;
    const char* name;
    uint32_t alive;
};
inline const MockClusterRecord kMockClusters[] = {
    {0, "EU_C0", 0u}, {2, "EU_C2", 1u}, {3, "EU_C3", 1u}, {4, "EU_C4", 1u}};
inline int32_t mock_current_cluster = 3;

inline void FillCluster(WotbModV3ClusterInfo* out, const MockClusterRecord& record) {
    out->cluster_id = record.id;
    out->current = record.id == mock_current_cluster ? 1u : 0u;
    out->alive = record.alive;
    out->allowed = 1u;
    out->ccu = -1;
    strncpy_s(out->name, sizeof(out->name), record.name, _TRUNCATE);
}

inline WotbModV3Result WOTBMOD_V3_CALL SessionClusterEnumerate(
    WotbModV3Handle, WotbModV3ClusterInfo* items, uint32_t* inout_count) {
    if (!inout_count) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    const uint32_t total = 4u;
    const WotbModV3Result result = FacadeRecord(
        "session_cluster.enumerate", items ? std::to_string(*inout_count) : "count");
    if (result != WOTBMOD_V3_OK) return result;
    if (!items) {
        *inout_count = total;
        return WOTBMOD_V3_OK;
    }
    const uint32_t capacity = *inout_count;
    *inout_count = total;
    if (capacity < total) return WOTBMOD_V3_E_BUFFER_TOO_SMALL;
    for (uint32_t i = 0u; i < total; ++i) FillCluster(&items[i], kMockClusters[i]);
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL SessionClusterGetCurrent(
    WotbModV3Handle, WotbModV3ClusterInfo* out) {
    if (!out) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    const WotbModV3Result result = FacadeRecord("session_cluster.get_current", "");
    if (result != WOTBMOD_V3_OK) return result;
    for (const MockClusterRecord& record : kMockClusters) {
        if (record.id == mock_current_cluster) {
            FillCluster(out, record);
            return WOTBMOD_V3_OK;
        }
    }
    return WOTBMOD_V3_E_NOT_FOUND;
}

inline WotbModV3Result WOTBMOD_V3_CALL SessionClusterChange(
    WotbModV3Handle, int32_t cluster_id) {
    return FacadeRecord("session_cluster.change", std::to_string(cluster_id));
}

inline WotbModV3SessionClusterApiV1& SessionClusterApi() {
    static WotbModV3SessionClusterApiV1 api = {};
    api.struct_size = sizeof(api);
    api.api_version = WOTBMOD_V3_SESSION_CLUSTER_VERSION;
    api.enumerate = &SessionClusterEnumerate;
    api.get_current = &SessionClusterGetCurrent;
    api.change = &SessionClusterChange;
    return api;
}

// The runtime's wotbmod.session.cluster.changed system event with its
// typed payload, so a script sees e.data.kind == "cluster_changed".
inline void FireClusterChanged(int32_t from, int32_t to, uint32_t status) {
    WotbModV3ClusterChangedEvent value = {};
    WOTBMOD_V3_INIT_STRUCT(value, WOTBMOD_V3_SESSION_CLUSTER_VERSION);
    value.from_cluster_id = from;
    value.to_cluster_id = to;
    value.status = status;
    const uint32_t previous_flags = event_flags;
    const std::string previous_payload = event_payload;
    event_flags = WOTBMOD_V3_EVENT_FLAG_SYSTEM;
    event_payload.assign(reinterpret_cast<const char*>(&value), sizeof(value));
    FireEvent(WOTBMOD_V3_EVENT_SESSION_CLUSTER_CHANGED);
    event_flags = previous_flags;
    event_payload = previous_payload;
}

inline WotbModV3LoadersApiV1& LoadersApi() {
    static WotbModV3LoadersApiV1 api = {};
    api.struct_size = sizeof(api);
    api.api_version = WOTBMOD_V3_LOADERS_VERSION;
    api.load_text_utf8 = &LdLoadText;
    api.load_binary = &LdLoadBinary;
    return api;
}

inline WotbModV3Result WOTBMOD_V3_CALL QueryInterface(
    WotbModV3Handle, const char* name, uint32_t, const void** out_interface) {
    if (!name || !out_interface) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    if (publish_facade_interfaces) {
        struct FacadeInterface {
            const char* name;
            const void* api;
        };
        const FacadeInterface published[] = {
            {WOTBMOD_V3_IFACE_UI_READ, &UiReadApi()},
            {WOTBMOD_V3_IFACE_VEHICLE_VISUAL, &VehicleVisualApi()},
            {WOTBMOD_V3_IFACE_PROJECTILE, &ProjectileApi()},
            {WOTBMOD_V3_IFACE_CAMERA, &CameraApi()},
            {WOTBMOD_V3_IFACE_GAMEPLAY_CAMERA, &GameplayCameraApi()},
            {WOTBMOD_V3_IFACE_CAMERA_STATE, &CameraStateApi()},
            {WOTBMOD_V3_IFACE_AUDIO, &AudioApi()},
            {WOTBMOD_V3_IFACE_INPUT, &InputApi()},
            {WOTBMOD_V3_IFACE_RESOURCES, &ResourcesApi()},
            {WOTBMOD_V3_IFACE_LOADERS, &LoadersApi()},
            {WOTBMOD_V3_IFACE_SESSION_CLUSTER, &SessionClusterApi()},
        };
        for (const FacadeInterface& entry : published) {
            if (std::strcmp(name, entry.name) == 0) {
                *out_interface = entry.api;
                return WOTBMOD_V3_OK;
            }
        }
    }
    if (std::strcmp(name, WOTBMOD_V3_IFACE_PERMISSIONS) == 0) {
        if (!permissions_interface_available) {
            *out_interface = nullptr;
            return WOTBMOD_V3_E_NOT_SUPPORTED;
        }
        *out_interface = &PermissionsApi();
        return WOTBMOD_V3_OK;
    }
    if (std::strcmp(name, WOTBMOD_V3_IFACE_STORAGE) == 0) {
        *out_interface = &StorageApi();
        return WOTBMOD_V3_OK;
    }
    if (std::strcmp(name, WOTBMOD_V3_IFACE_CORE) == 0) {
        *out_interface = &CoreApi();
        return WOTBMOD_V3_OK;
    }
    if (std::strcmp(name, WOTBMOD_V3_IFACE_CAPABILITIES) == 0) {
        *out_interface = &CapabilitiesApi();
        return WOTBMOD_V3_OK;
    }
    if (std::strcmp(name, WOTBMOD_V3_IFACE_HANDLES) == 0) {
        *out_interface = &HandlesApi();
        return WOTBMOD_V3_OK;
    }
    if (std::strcmp(name, WOTBMOD_V3_IFACE_ASYNC) == 0) {
        *out_interface = &AsyncApi();
        return WOTBMOD_V3_OK;
    }
    if (std::strcmp(name, WOTBMOD_V3_IFACE_LIFECYCLE) == 0) {
        *out_interface = &LifecycleApi();
        return WOTBMOD_V3_OK;
    }
    if (std::strcmp(name, WOTBMOD_V3_IFACE_EVENTS) == 0) {
        *out_interface = &EventsApi();
        return WOTBMOD_V3_OK;
    }
    if (std::strcmp(name, WOTBMOD_V3_IFACE_GES) == 0) {
        *out_interface = &GesApi();
        return WOTBMOD_V3_OK;
    }
    if (std::strcmp(name, WOTBMOD_V3_IFACE_UI) == 0) {
        *out_interface = &UiApi();
        return WOTBMOD_V3_OK;
    }
    if (std::strcmp(name, WOTBMOD_V3_IFACE_ENTITY_PUBLIC) == 0) {
        *out_interface = &EntityPublicApi();
        return WOTBMOD_V3_OK;
    }
    if (std::strcmp(name, WOTBMOD_V3_IFACE_VFS) == 0) {
        *out_interface = &VfsApi();
        return WOTBMOD_V3_OK;
    }
    if (std::strcmp(name, WOTBMOD_V3_IFACE_GAMEPLAY_HUD) == 0) {
        *out_interface = &GameplayHudApi();
        return WOTBMOD_V3_OK;
    }
    *out_interface = nullptr;
    return WOTBMOD_V3_E_NOT_SUPPORTED;
}

inline WotbModV3Result WOTBMOD_V3_CALL GetLastError(
    WotbModV3Handle, WotbModV3ErrorInfo* out_error) {
    if (!out_error) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    std::memset(out_error, 0, sizeof(*out_error));
    WOTBMOD_V3_INIT_STRUCT(*out_error, WOTBMOD_V3_ABI_VERSION);
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL GetInterfaceInfo(
    WotbModV3Handle, const char* name, WotbModV3InterfaceInfo* out_info) {
    if (!name || !out_info) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    std::memset(out_info, 0, sizeof(*out_info));
    WOTBMOD_V3_INIT_STRUCT(*out_info, WOTBMOD_V3_ABI_VERSION);
    out_info->status = WOTBMOD_V3_CAPABILITY_AVAILABLE;
    strncpy_s(out_info->name, name, _TRUNCATE);
    return WOTBMOD_V3_OK;
}

inline WotbModV3Result WOTBMOD_V3_CALL GetClientInfo(
    WotbModV3Handle, WotbModV3ClientInfo* out_info) {
    if (!out_info) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    std::memset(out_info, 0, sizeof(*out_info));
    WOTBMOD_V3_INIT_STRUCT(*out_info, WOTBMOD_V3_ABI_VERSION);
    out_info->supported = 1u;
    out_info->compatibility_state = WOTBMOD_V3_CLIENT_COMPATIBILITY_SUPPORTED;
    strncpy_s(out_info->client_version, "11.19.0.834", _TRUNCATE);
    return WOTBMOD_V3_OK;
}

inline void Install(WotbModV3Bootstrap* bootstrap) {
    Reset();
    std::memset(bootstrap, 0, sizeof(*bootstrap));
    bootstrap->struct_size = sizeof(*bootstrap);
    bootstrap->api_version = WOTBMOD_V3_ABI_VERSION;
    bootstrap->sdk_version = WOTBMOD_V3_ABI_VERSION;
    bootstrap->bootstrap_version = 1u;
    bootstrap->query_interface = &QueryInterface;
    bootstrap->get_interface_info = &GetInterfaceInfo;
    bootstrap->get_last_error = &GetLastError;
    bootstrap->get_client_info = &GetClientInfo;
}

}  // namespace MockAbi

#endif  // WOTBMOD_LUA_HOST_MOCK_ABI_H_
