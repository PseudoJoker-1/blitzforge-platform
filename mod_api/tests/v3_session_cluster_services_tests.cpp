/*
 * Portable half of wotbmod.session.cluster against a fake backend: the
 * guards of change(), the QUEUED/STARTED/CONNECTED/FAILED state machine, the
 * events it publishes and the manual-flag safety net. No client involved.
 */
#include "../include/wotb_mod_runtime_v3.h"
#include "../include/wotbmod/events_v1.h"
#include "../include/wotbmod/session_cluster_v1.h"
#include "../src/v3/client_services_backend.h"
#include "../src/v3/session_cluster_services.h"
#include "../src/v3/wotb_mod_v3_internal.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define CHECK(expression)                                          \
    do {                                                           \
        if (!(expression)) {                                       \
            std::fprintf(stderr, "check failed at line %d: %s\n", \
                         __LINE__, #expression);                   \
            return 1;                                              \
        }                                                          \
    } while (0)

using namespace wotbmod::v3;

namespace {

void Copy(char* destination, size_t capacity, const char* text) {
    strncpy_s(destination, capacity, text, _TRUNCATE);
}

WotbModV3Result WOTBMOD_V3_CALL ReviewedEntry(
    const WotbModV3Bootstrap*, WotbModV3Handle, WotbModV3Info* info) {
    if (!info) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    info->struct_size = sizeof(*info);
    info->api_version = WOTBMOD_V3_ABI_VERSION;
    info->requested_permission_tier = WOTBMOD_V3_PERMISSION_REVIEWED;
    Copy(info->id, sizeof(info->id), "tests.session-cluster");
    Copy(info->name, sizeof(info->name), "Session cluster test");
    Copy(info->version, sizeof(info->version), "1.0.0");
    Copy(info->author, sizeof(info->author), "tests");
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL SafeEntry(
    const WotbModV3Bootstrap*, WotbModV3Handle, WotbModV3Info* info) {
    if (!info) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    info->struct_size = sizeof(*info);
    info->api_version = WOTBMOD_V3_ABI_VERSION;
    info->requested_permission_tier = WOTBMOD_V3_PERMISSION_SAFE;
    Copy(info->id, sizeof(info->id), "tests.session-cluster-safe");
    Copy(info->name, sizeof(info->name), "Session cluster safe test");
    Copy(info->version, sizeof(info->version), "1.0.0");
    Copy(info->author, sizeof(info->author), "tests");
    return WOTBMOD_V3_OK;
}

WotbModV3Handle g_mod = WOTBMOD_V3_INVALID_HANDLE;
WotbModV3Handle g_safe_mod = WOTBMOD_V3_INVALID_HANDLE;
const WotbModV3Bootstrap* g_bootstrap = nullptr;
const WotbModV3EventsApiV1* g_events = nullptr;
const WotbModV3SessionClusterApiV1* g_api = nullptr;

/* ---- fake backend ------------------------------------------------------ */
struct FakeCluster {
    int32_t id;
    const char* name;
    uint32_t alive;
    uint32_t allowed;
};
std::vector<FakeCluster> g_clusters = {
    {0, "EU_C0", 1u, 1u}, {2, "EU_C2", 1u, 1u}, {3, "EU_C3", 1u, 1u}, {4, "EU_C4", 1u, 1u}};
int32_t g_current = 3;
bool g_captured = true;
std::vector<std::string> g_calls;
WotbModV3Result g_change_result = WOTBMOD_V3_OK;

void Fill(WotbModV3ClusterInfo* out, const FakeCluster& cluster) {
    out->cluster_id = cluster.id;
    out->current = cluster.id == g_current ? 1u : 0u;
    out->alive = cluster.alive;
    out->allowed = cluster.allowed;
    out->ccu = -1;
    Copy(out->name, sizeof(out->name), cluster.name);
}

WotbModV3Result FakeEnumerate(void*, WotbModV3ClusterInfo* items, uint32_t* inout_count) {
    if (!g_captured) return WOTBMOD_V3_E_NOT_SUPPORTED;
    const uint32_t total = static_cast<uint32_t>(g_clusters.size());
    if (!items) {
        *inout_count = total;
        return WOTBMOD_V3_OK;
    }
    const uint32_t capacity = *inout_count;
    *inout_count = total;
    if (capacity < total) return WOTBMOD_V3_E_BUFFER_TOO_SMALL;
    for (uint32_t i = 0u; i < total; ++i) Fill(&items[i], g_clusters[i]);
    return WOTBMOD_V3_OK;
}

WotbModV3Result FakeGetCurrent(void*, WotbModV3ClusterInfo* out) {
    if (!g_captured) return WOTBMOD_V3_E_NOT_SUPPORTED;
    for (const FakeCluster& cluster : g_clusters) {
        if (cluster.id == g_current) {
            Fill(out, cluster);
            return WOTBMOD_V3_OK;
        }
    }
    return WOTBMOD_V3_E_NOT_FOUND;
}

WotbModV3Result FakeChange(void*, int32_t id) {
    g_calls.push_back("change(" + std::to_string(id) + ")");
    return g_change_result;
}

WotbModV3Result FakeSetManual(void*, uint32_t manual) {
    g_calls.push_back("set_manual(" + std::to_string(manual) + ")");
    return WOTBMOD_V3_OK;
}

ClientHostDeclaredBackend FakeBackend() {
    ClientHostDeclaredBackend backend = {};
    backend.struct_size = sizeof(backend);
    backend.api_version = WOTBMOD_V3_CLIENT_DECLARED_BACKEND_VERSION;
    backend.compatibility_state = WOTBMOD_V3_CLIENT_COMPATIBILITY_SUPPORTED;
    backend.session_cluster_enumerate = &FakeEnumerate;
    backend.session_cluster_get_current = &FakeGetCurrent;
    backend.session_cluster_change = &FakeChange;
    backend.session_cluster_set_manual = &FakeSetManual;
    return backend;
}

/* ---- fake clock and main-thread poster --------------------------------- */
uint64_t g_now = 100000u;
uint64_t FakeNow() { return g_now; }
MainThreadCallFn g_posted = nullptr;
void* g_posted_user = nullptr;
WotbModV3Result g_post_result = WOTBMOD_V3_OK;
WotbModV3Result FakePost(MainThreadCallFn callback, void* user) {
    if (g_post_result != WOTBMOD_V3_OK) return g_post_result;
    g_posted = callback;
    g_posted_user = user;
    return WOTBMOD_V3_OK;
}
bool RunPosted() {
    if (!g_posted) return false;
    MainThreadCallFn callback = g_posted;
    g_posted = nullptr;
    callback(g_posted_user);
    return true;
}

/* ---- event capture ------------------------------------------------------ */
std::vector<WotbModV3ClusterChangedEvent> g_received;
void WOTBMOD_V3_CALL OnChanged(WotbModV3Handle, WotbModV3Event* event, void*) {
    if (!event->payload || event->payload_size < sizeof(WotbModV3ClusterChangedEvent)) return;
    WotbModV3ClusterChangedEvent value = {};
    std::memcpy(&value, event->payload, sizeof(value));
    g_received.push_back(value);
}

bool LastEventIs(int32_t from, int32_t to, uint32_t status) {
    if (g_received.empty()) return false;
    const WotbModV3ClusterChangedEvent& e = g_received.back();
    return e.from_cluster_id == from && e.to_cluster_id == to && e.status == status;
}

bool Called(const char* text) {
    for (const std::string& call : g_calls) {
        if (call == text) return true;
    }
    return false;
}

std::string LastError() {
    WotbModV3ErrorInfo error = {};
    WOTBMOD_V3_INIT_STRUCT(error, WOTBMOD_V3_ABI_VERSION);
    if (g_bootstrap->get_last_error(g_mod, &error) != WOTBMOD_V3_OK) return "";
    return error.message;
}

WotbModV3ClusterInfo Info() {
    WotbModV3ClusterInfo info = {};
    info.struct_size = sizeof(info);
    info.api_version = WOTBMOD_V3_SESSION_CLUSTER_VERSION;
    return info;
}

}  // namespace

static int TestAvailabilityAndReads() {
    const void* table = nullptr;
    CHECK(g_bootstrap->query_interface(
              g_mod, WOTBMOD_V3_IFACE_SESSION_CLUSTER,
              WOTBMOD_V3_SESSION_CLUSTER_VERSION, &table) == WOTBMOD_V3_E_NOT_SUPPORTED);
    ClientHostDeclaredBackend backend = FakeBackend();
    SetClientHostDeclaredBackend(&backend);
    CHECK(g_bootstrap->query_interface(
              g_mod, WOTBMOD_V3_IFACE_SESSION_CLUSTER,
              WOTBMOD_V3_SESSION_CLUSTER_VERSION, &table) == WOTBMOD_V3_OK);
    g_api = static_cast<const WotbModV3SessionClusterApiV1*>(table);
    CHECK(g_api && g_api->struct_size == sizeof(WotbModV3SessionClusterApiV1));

    uint32_t count = 0u;
    CHECK(g_api->enumerate(g_mod, nullptr, &count) == WOTBMOD_V3_OK && count == 4u);
    std::vector<WotbModV3ClusterInfo> items(4u, Info());
    CHECK(g_api->enumerate(g_mod, items.data(), &count) == WOTBMOD_V3_OK && count == 4u);
    CHECK(items[2].cluster_id == 3 && items[2].current == 1u && items[0].current == 0u);
    CHECK(std::strcmp(items[3].name, "EU_C4") == 0 && items[3].ccu == -1);
    WotbModV3ClusterInfo current = Info();
    CHECK(g_api->get_current(g_mod, &current) == WOTBMOD_V3_OK);
    CHECK(current.cluster_id == 3 && std::strcmp(current.name, "EU_C3") == 0);

    /* An item without struct_size is refused before the backend is asked. */
    WotbModV3ClusterInfo bare = {};
    count = 1u;
    CHECK(g_api->enumerate(g_mod, &bare, &count) == WOTBMOD_V3_E_INVALID_ARGUMENT);

    /* Not captured yet: the backend's NOT_SUPPORTED is explained. */
    g_captured = false;
    CHECK(g_api->get_current(g_mod, &current) == WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(LastError().find("not been captured") != std::string::npos);
    g_captured = true;

    /* Removing the backend removes the interface. */
    SetClientHostDeclaredBackend(nullptr);
    CHECK(g_api->enumerate(g_mod, nullptr, &count) == WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(g_bootstrap->query_interface(
              g_mod, WOTBMOD_V3_IFACE_SESSION_CLUSTER,
              WOTBMOD_V3_SESSION_CLUSTER_VERSION, &table) == WOTBMOD_V3_E_NOT_SUPPORTED);
    return 0;
}

static int TestChangeGuards() {
    SessionClusterResetForTests();
    ClientHostDeclaredBackend backend = FakeBackend();
    SetClientHostDeclaredBackend(&backend);
    g_calls.clear();

    /* A SAFE mod may read but not change. */
    uint32_t count = 0u;
    CHECK(g_api->enumerate(g_safe_mod, nullptr, &count) == WOTBMOD_V3_OK && count == 4u);
    CHECK(g_api->change(g_safe_mod, 4) == WOTBMOD_V3_E_PERMISSION_DENIED);

    CHECK(WotbModV3Runtime_SetContext(WOTBMOD_V3_CONTEXT_BATTLE) == WOTBMOD_V3_OK);
    CHECK(g_api->change(g_mod, 4) == WOTBMOD_V3_E_CONFLICT);
    CHECK(LastError().find("hangar") != std::string::npos);

    CHECK(WotbModV3Runtime_SetContext(WOTBMOD_V3_CONTEXT_HANGAR) == WOTBMOD_V3_OK);
    CHECK(g_api->change(g_mod, 7) == WOTBMOD_V3_E_NOT_FOUND);
    g_clusters[0].alive = 0u;
    CHECK(g_api->change(g_mod, 0) == WOTBMOD_V3_E_CONFLICT);
    CHECK(LastError().find("EU_C0") != std::string::npos);
    g_clusters[0].alive = 1u;
    CHECK(g_calls.empty());
    CHECK(g_received.empty());
    SetClientHostDeclaredBackend(nullptr);
    return 0;
}

static int TestHappyPath() {
    SessionClusterResetForTests();
    ClientHostDeclaredBackend backend = FakeBackend();
    SetClientHostDeclaredBackend(&backend);
    g_calls.clear();
    g_received.clear();
    g_current = 3;
    CHECK(WotbModV3Runtime_SetContext(WOTBMOD_V3_CONTEXT_HANGAR) == WOTBMOD_V3_OK);

    CHECK(g_api->change(g_mod, 4) == WOTBMOD_V3_OK);
    CHECK(g_received.size() == 1u && LastEventIs(3, 4, WOTBMOD_V3_CLUSTER_CHANGE_QUEUED));
    CHECK(g_calls.empty());                              /* nothing before the main thread */
    CHECK(g_api->change(g_mod, 0) == WOTBMOD_V3_E_BUSY); /* in flight */

    CHECK(RunPosted());
    CHECK(Called("change(4)") && !Called("set_manual(0)"));
    CHECK(g_received.size() == 2u && LastEventIs(3, 4, WOTBMOD_V3_CLUSTER_CHANGE_STARTED));
    CHECK(SessionClusterChangeInFlight());
    CHECK(g_api->change(g_mod, 0) == WOTBMOD_V3_E_BUSY); /* still in flight */

    /* Still in the hangar: nothing judged. */
    g_now += 1000u;
    SessionClusterFrameTick(g_now);
    CHECK(g_received.size() == 2u);
    /* The client left the hangar and came back on the target: CONNECTED. */
    CHECK(WotbModV3Runtime_SetContext(WOTBMOD_V3_CONTEXT_LOADING) == WOTBMOD_V3_OK);
    g_now += 2000u;
    SessionClusterFrameTick(g_now);
    CHECK(g_received.size() == 2u);
    g_current = 4;
    CHECK(WotbModV3Runtime_SetContext(WOTBMOD_V3_CONTEXT_HANGAR) == WOTBMOD_V3_OK);
    g_now += 3000u;
    SessionClusterFrameTick(g_now);
    /* The frame the hangar comes back is not judged: the screen must settle. */
    CHECK(g_received.size() == 2u);
    g_now += 499u;
    SessionClusterFrameTick(g_now);
    CHECK(g_received.size() == 2u);
    g_now += 1u;
    SessionClusterFrameTick(g_now);
    CHECK(g_received.size() == 3u && LastEventIs(3, 4, WOTBMOD_V3_CLUSTER_CHANGE_CONNECTED));
    CHECK(!Called("set_manual(0)"));

    /* Cooldown: refused inside 10 s, accepted after. */
    CHECK(g_api->change(g_mod, 2) == WOTBMOD_V3_E_BUSY);
    g_now += 11000u;
    g_calls.clear();
    CHECK(g_api->change(g_mod, 2) == WOTBMOD_V3_OK);
    CHECK(RunPosted() && Called("change(2)"));

    /* The hangar came back on a different cluster: FAILED + manual flag cleared. */
    CHECK(WotbModV3Runtime_SetContext(WOTBMOD_V3_CONTEXT_LOADING) == WOTBMOD_V3_OK);
    g_now += 100u;
    SessionClusterFrameTick(g_now);
    g_current = 3;
    CHECK(WotbModV3Runtime_SetContext(WOTBMOD_V3_CONTEXT_HANGAR) == WOTBMOD_V3_OK);
    g_now += 200u;
    SessionClusterFrameTick(g_now);
    g_now += 500u;
    SessionClusterFrameTick(g_now);
    CHECK(LastEventIs(4, 2, WOTBMOD_V3_CLUSTER_CHANGE_FAILED));
    CHECK(Called("set_manual(0)"));
    SetClientHostDeclaredBackend(nullptr);
    return 0;
}

static int TestTimeoutAutoAndPostFailure() {
    SessionClusterResetForTests();
    ClientHostDeclaredBackend backend = FakeBackend();
    SetClientHostDeclaredBackend(&backend);
    g_calls.clear();
    g_received.clear();
    g_current = 3;
    CHECK(WotbModV3Runtime_SetContext(WOTBMOD_V3_CONTEXT_HANGAR) == WOTBMOD_V3_OK);

    /* Timeout: 61 s without a hangar -> FAILED, flag cleared. */
    CHECK(g_api->change(g_mod, 4) == WOTBMOD_V3_OK && RunPosted());
    CHECK(WotbModV3Runtime_SetContext(WOTBMOD_V3_CONTEXT_LOADING) == WOTBMOD_V3_OK);
    g_now += 61000u;
    SessionClusterFrameTick(g_now);
    CHECK(LastEventIs(3, 4, WOTBMOD_V3_CLUSTER_CHANGE_FAILED) && Called("set_manual(0)"));
    CHECK(WotbModV3Runtime_SetContext(WOTBMOD_V3_CONTEXT_HANGAR) == WOTBMOD_V3_OK);
    g_now += 61000u + 11000u;

    /* AUTO clears the flag before ChangeCluster(-1); CONNECTED needs no id match. */
    g_calls.clear();
    CHECK(g_api->change(g_mod, WOTBMOD_V3_SESSION_CLUSTER_AUTO) == WOTBMOD_V3_OK);
    CHECK(RunPosted());
    CHECK(g_calls.size() == 2u && g_calls[0] == "set_manual(0)" && g_calls[1] == "change(-1)");
    CHECK(WotbModV3Runtime_SetContext(WOTBMOD_V3_CONTEXT_LOADING) == WOTBMOD_V3_OK);
    g_now += 100u;
    SessionClusterFrameTick(g_now);
    g_current = 0;
    CHECK(WotbModV3Runtime_SetContext(WOTBMOD_V3_CONTEXT_HANGAR) == WOTBMOD_V3_OK);
    g_now += 200u;
    SessionClusterFrameTick(g_now);
    g_now += 500u;
    SessionClusterFrameTick(g_now);
    CHECK(LastEventIs(3, -1, WOTBMOD_V3_CLUSTER_CHANGE_CONNECTED));
    g_now += 11000u;

    /* ChangeCluster itself refusing -> FAILED, flag cleared, back to Idle. */
    g_calls.clear();
    g_change_result = WOTBMOD_V3_E_PLATFORM;
    CHECK(g_api->change(g_mod, 2) == WOTBMOD_V3_OK && RunPosted());
    CHECK(LastEventIs(0, 2, WOTBMOD_V3_CLUSTER_CHANGE_FAILED) && Called("set_manual(0)"));
    g_change_result = WOTBMOD_V3_OK;
    g_now += 11000u;

    /* The main thread cannot be reached: refused, no job left behind. */
    g_post_result = WOTBMOD_V3_E_NOT_SUPPORTED;
    CHECK(g_api->change(g_mod, 2) == WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(LastEventIs(0, 2, WOTBMOD_V3_CLUSTER_CHANGE_FAILED));
    g_post_result = WOTBMOD_V3_OK;
    CHECK(g_api->change(g_mod, 2) == WOTBMOD_V3_OK);   /* Idle again, no cooldown after a refusal */
    CHECK(RunPosted());
    SetClientHostDeclaredBackend(nullptr);
    return 0;
}

int main() {
    WotbModV3RuntimeOptions options = {};
    options.struct_size = sizeof(options);
    options.api_version = WOTBMOD_V3_ABI_VERSION;
    options.game_directory = ".";
    options.mods_directory = "build\\v3_session_cluster_tests\\mods";
    options.cache_directory = "build\\v3_session_cluster_tests\\cache";
    options.config_directory = "build\\v3_session_cluster_tests\\config";
    options.client_version = "session-cluster-test";
    CHECK(WotbModV3Runtime_Initialize(&options) == WOTBMOD_V3_OK);
    CHECK(WotbModV3Runtime_CreateMod(
              "session_cluster_test.dll", WOTBMOD_V3_PERMISSION_REVIEWED, &g_mod) == WOTBMOD_V3_OK);
    WotbModV3RuntimeModuleInfo module = {};
    module.struct_size = sizeof(module);
    module.api_version = WOTBMOD_V3_ABI_VERSION;
    CHECK(WotbModV3Runtime_InvokeEntry(g_mod, &ReviewedEntry, &module) == WOTBMOD_V3_OK);
    CHECK(WotbModV3Runtime_Enable(g_mod) == WOTBMOD_V3_OK);
    CHECK(WotbModV3Runtime_CreateMod(
              "session_cluster_safe.dll", WOTBMOD_V3_PERMISSION_SAFE, &g_safe_mod) == WOTBMOD_V3_OK);
    WotbModV3RuntimeModuleInfo safe = {};
    safe.struct_size = sizeof(safe);
    safe.api_version = WOTBMOD_V3_ABI_VERSION;
    CHECK(WotbModV3Runtime_InvokeEntry(g_safe_mod, &SafeEntry, &safe) == WOTBMOD_V3_OK);
    CHECK(WotbModV3Runtime_Enable(g_safe_mod) == WOTBMOD_V3_OK);
    g_bootstrap = WotbModV3Runtime_GetBootstrap();
    CHECK(g_bootstrap != nullptr);
    const void* table = nullptr;
    CHECK(g_bootstrap->query_interface(
              g_mod, WOTBMOD_V3_IFACE_EVENTS, WOTBMOD_V3_EVENTS_VERSION, &table) == WOTBMOD_V3_OK);
    g_events = static_cast<const WotbModV3EventsApiV1*>(table);

    const uint64_t restore = GetEventSourceMask();
    SetEventSourceMask(WOTBMOD_V3_EVENT_SOURCE_SESSION_CLUSTER);
    SessionClusterSetTestHooks(&FakeNow, &FakePost);
    WotbModV3EventSubscriptionInfo info = {};
    WOTBMOD_V3_INIT_STRUCT(info, WOTBMOD_V3_EVENTS_VERSION);
    info.topic_pattern = "wotbmod.session.cluster.*";
    info.priority = WOTBMOD_V3_EVENT_PRIORITY_NORMAL;
    info.receive_system_events = 1u;
    WotbModV3EventToken token = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(g_events->subscribe(g_mod, &info, &OnChanged, nullptr, &token) == WOTBMOD_V3_OK);

    int failed = TestAvailabilityAndReads();
    if (!failed) failed = TestChangeGuards();
    if (!failed) failed = TestHappyPath();
    if (!failed) failed = TestTimeoutAutoAndPostFailure();

    g_events->unsubscribe(g_mod, token);
    SessionClusterSetTestHooks(nullptr, nullptr);
    SetEventSourceMask(restore);
    WotbModV3Runtime_Shutdown();
    if (failed) return 1;
    std::printf("V3 session.cluster services: availability, reads, guards, state machine and events passed\n");
    return 0;
}
