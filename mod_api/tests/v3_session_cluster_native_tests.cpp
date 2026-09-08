/*
 * Host double of the client structures loader/v3_native_session_cluster
 * reads: a LoginManager with `services` at +32, a services vtable whose
 * slots return the app context holder, the ConnectionManager and the
 * LoginManager, a Region vector of 208-byte ClusterHost blocks whose +188
 * descriptor carries the MSVC std::string name (+24), the int id (+120) and
 * the alive/allowed bytes (+188/+189). Same offsets as loader/anchor_rvas.h.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "wotbmod/session_cluster_v1.h"
#include "../loader/v3_native_session_cluster.h"

#define CHECK(expression)                                          \
    do {                                                           \
        if (!(expression)) {                                       \
            std::fprintf(stderr, "check failed at line %d: %s\n", \
                         __LINE__, #expression);                   \
            return 1;                                              \
        }                                                          \
    } while (0)

using namespace wotbmod::loader::session_cluster;

namespace {

const uint32_t kStride = 208u;

struct Descriptor {
    uint8_t bytes[256];
};

struct StdString {          /* MSVC x86 std::string */
    union {
        char sso[16];
        char* heap;
    } data;
    uint32_t size;
    uint32_t capacity;
};

uint8_t g_login_manager[1024];
uint8_t g_connection[64];
uint8_t g_appctx_holder[64];
uint8_t g_appctx[2048];
void* g_region[2];             /* begin, end */
uint8_t g_hosts[4][kStride];
uint8_t g_addresses[4][2][kStride];   /* two login addresses per host */
Descriptor g_descriptors[4];
void* g_services_vtable[90];
void* g_services[4];           /* +0 vtable */

std::vector<std::string> g_calls;

/* __fastcall(self, edx, ...) is the free-function spelling of __thiscall. */
void* __fastcall AppCtxGetter(void*, void*) { return g_appctx_holder; }
void* __fastcall ConnectionGetter(void*, void*) { return g_connection; }
void* __fastcall LoginGetter(void*, void*) { return g_login_manager; }

int __fastcall FakeDisconnect(void* connection, void*, int reason, float delay, int arg) {
    g_calls.push_back(std::string(connection == g_connection ? "cm" : "other") +
                      ":disconnect(" + std::to_string(reason) + "," +
                      std::to_string(static_cast<int>(delay)) + "," + std::to_string(arg) + ")");
    return 0;
}

int __fastcall FakeChangeCluster(void* login_manager, void*, int cluster_id) {
    g_calls.push_back(std::string(login_manager == g_login_manager ? "lm" : "other") +
                      ":change(" + std::to_string(cluster_id) + ")");
    return 0;
}

std::vector<std::string> g_log;
void FakeLog(const char* message) { g_log.push_back(message); }

Layout TestLayout() {
    Layout layout = {};
    layout.services_offset = 0x20u;
    layout.cluster_to_login_offset = 0x1E8u;
    layout.manual_flag_offset = 0x31Cu;
    layout.services_appctx_slot = 0x13Cu;
    layout.services_connection_slot = 0x64u;
    layout.services_login_slot = 0x68u;
    layout.appctx_owner_offset = 0x28u;
    layout.appctx_region_offset = 0x638u;
    layout.connection_current_host_offset = 0xCu;
    layout.host_stride = kStride;
    layout.host_descriptor_offset = 0xBCu;
    layout.host_addresses_begin_offset = 0x18u;
    layout.host_addresses_end_offset = 0x1Cu;
    layout.host_state_offset = 0xB8u;
    layout.address_stride = kStride;
    layout.address_ping_offset = 0x30u;
    layout.descriptor_name_offset = 0x0u;
    layout.descriptor_url_offset = 0x18u;
    layout.descriptor_id_offset = 0x78u;
    return layout;
}

char g_heap_pool[16][512];
int g_heap_next = 0;

void SetString(uint8_t* where, const char* name, bool heap) {
    StdString* text = reinterpret_cast<StdString*>(where);
    std::memset(text, 0, sizeof(*text));
    const size_t length = std::strlen(name);
    if (heap || length > 15u) {
        char* storage = g_heap_pool[g_heap_next++ % 16];
        strcpy_s(storage, 512u, name);
        text->data.heap = storage;
        text->size = static_cast<uint32_t>(length);
        text->capacity = 511u;
    } else {
        strcpy_s(text->data.sso, sizeof(text->data.sso), name);
        text->size = static_cast<uint32_t>(length);
        text->capacity = 15u;
    }
}

void SetName(Descriptor* descriptor, const char* name, bool heap) {
    SetString(descriptor->bytes + 0x0u, name, heap);
}

void SetPing(int host, float first, float second) {
    *reinterpret_cast<float*>(g_addresses[host][0] + 0x30u) = first;
    *reinterpret_cast<float*>(g_addresses[host][1] + 0x30u) = second;
}

void BuildWorld() {
    std::memset(g_login_manager, 0, sizeof(g_login_manager));
    std::memset(g_connection, 0, sizeof(g_connection));
    std::memset(g_appctx_holder, 0, sizeof(g_appctx_holder));
    std::memset(g_appctx, 0, sizeof(g_appctx));
    std::memset(g_hosts, 0, sizeof(g_hosts));
    std::memset(g_addresses, 0, sizeof(g_addresses));
    std::memset(g_descriptors, 0, sizeof(g_descriptors));
    std::memset(g_services_vtable, 0, sizeof(g_services_vtable));
    g_services_vtable[0x13Cu / 4u] = reinterpret_cast<void*>(&AppCtxGetter);
    g_services_vtable[0x64u / 4u] = reinterpret_cast<void*>(&ConnectionGetter);
    g_services_vtable[0x68u / 4u] = reinterpret_cast<void*>(&LoginGetter);
    g_services[0] = g_services_vtable;
    *reinterpret_cast<void**>(g_login_manager + 0x20u) = g_services;
    *reinterpret_cast<void**>(g_appctx_holder + 0x28u) = g_appctx;
    *reinterpret_cast<void**>(g_appctx + 0x638u) = g_region;
    g_region[0] = g_hosts[0];
    g_region[1] = g_hosts[0] + 4u * kStride;
    const int ids[4] = {0, 2, 3, 4};
    const char* names[4] = {"EU_C0", "EU_C2", "EU_C3", "EU_C4"};
    for (int i = 0; i < 4; ++i) {
        *reinterpret_cast<void**>(g_hosts[i] + 0xBCu) = &g_descriptors[i];
        *reinterpret_cast<int32_t*>(g_descriptors[i].bytes + 0x78u) = ids[i];
        *reinterpret_cast<void**>(g_hosts[i] + 0x18u) = g_addresses[i][0];
        *reinterpret_cast<void**>(g_hosts[i] + 0x1Cu) = g_addresses[i][0] + 2u * kStride;
        SetPing(i, i == 0 ? -1.0f : 0.0f, i == 0 ? 0.0f : 42.5f);   /* EU_C0 never answered */
        SetName(&g_descriptors[i], names[i], false);
        SetString(g_descriptors[i].bytes + 0x18u, "loginX.wotblitz.eu:20016", false);
    }
    *reinterpret_cast<void**>(g_connection + 0xCu) = g_hosts[2];   /* EU_C3 */
}

WotbModV3ClusterInfo Info() {
    WotbModV3ClusterInfo info = {};
    info.struct_size = sizeof(info);
    info.api_version = WOTBMOD_V3_SESSION_CLUSTER_VERSION;
    return info;
}

}  // namespace

int main() {
    BuildWorld();
    Init(TestLayout(), reinterpret_cast<ChangeClusterFn>(&FakeChangeCluster),
         reinterpret_cast<DisconnectFn>(&FakeDisconnect), &FakeLog);
    CHECK(!Ready());
    uint32_t count = 7u;
    CHECK(Enumerate(nullptr, &count) == WOTBMOD_V3_E_NOT_SUPPORTED && count == 0u);
    WotbModV3ClusterInfo current = Info();
    CHECK(GetCurrent(&current) == WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(Change(4) == WOTBMOD_V3_E_NOT_SUPPORTED && g_calls.empty());

    /* Refusals before capture: null, a login manager without services, a
     * services whose login getter points elsewhere. */
    CHECK(!OnHostChosen(nullptr, g_hosts[2]) && !Ready());
    uint8_t orphan[1024] = {};
    CHECK(!OnHostChosen(orphan, g_hosts[2]) && !Ready());
    *reinterpret_cast<void**>(orphan + 0x20u) = g_services;
    CHECK(!OnHostChosen(orphan, g_hosts[2]) && !Ready());
    CHECK(!g_log.empty() && g_log.back().find("point back") != std::string::npos);

    /* Capture. */
    CHECK(OnHostChosen(g_login_manager, g_hosts[2]));
    CHECK(Ready());
    CHECK(g_log.back().find("region of 4 hosts") != std::string::npos);

    /* Enumerate: count, then items. */
    count = 0u;
    CHECK(Enumerate(nullptr, &count) == WOTBMOD_V3_OK && count == 4u);
    std::vector<WotbModV3ClusterInfo> items(2u, Info());
    count = 2u;
    CHECK(Enumerate(items.data(), &count) == WOTBMOD_V3_E_BUFFER_TOO_SMALL && count == 4u);
    items.assign(4u, Info());
    CHECK(Enumerate(items.data(), &count) == WOTBMOD_V3_OK && count == 4u);
    CHECK(items[0].cluster_id == 0 && items[0].alive == 0u && items[0].allowed == 1u);
    CHECK(std::strcmp(items[0].name, "EU_C0") == 0 && items[0].current == 0u);
    CHECK(items[2].cluster_id == 3 && items[2].current == 1u && std::strcmp(items[2].name, "EU_C3") == 0);
    CHECK(items[3].cluster_id == 4 && items[3].ccu == -1);
    CHECK(items[3].struct_size == sizeof(WotbModV3ClusterInfo) &&
          items[3].api_version == WOTBMOD_V3_SESSION_CLUSTER_VERSION);

    /* A host the client marked dead (state 1) is not allowed. */
    *reinterpret_cast<int32_t*>(g_hosts[1] + 0xB8u) = 1;
    items.assign(4u, Info());
    count = 4u;
    CHECK(Enumerate(items.data(), &count) == WOTBMOD_V3_OK);
    CHECK(items[1].allowed == 0u && items[1].alive == 1u && items[2].allowed == 1u);
    *reinterpret_cast<int32_t*>(g_hosts[1] + 0xB8u) = 0;

    /* A descriptor without a readable name is named from its url and id. */
    SetString(g_descriptors[3].bytes + 0x0u, "", false);
    SetString(g_descriptors[3].bytes + 0x18u, "login4.wotblitz.eu:20016", false);
    items.assign(4u, Info());
    count = 4u;
    CHECK(Enumerate(items.data(), &count) == WOTBMOD_V3_OK);
    CHECK(std::strcmp(items[3].name, "EU_C4") == 0);
    SetName(&g_descriptors[3], "EU_C4", false);

    /* A heap-form name longer than the ABI field is truncated with a terminator. */
    std::string long_name = "EU_C4_WITH";
    while (long_name.size() < 30u) long_name += "_x";
    SetName(&g_descriptors[3], long_name.c_str(), true);
    items.assign(4u, Info());
    count = 4u;
    CHECK(Enumerate(items.data(), &count) == WOTBMOD_V3_OK);
    CHECK(std::strlen(items[3].name) == 30u);
    CHECK(std::strncmp(items[3].name, "EU_C4_WITH", 10u) == 0);
    SetName(&g_descriptors[3], "EU_C4", false);

    /* Current. */
    current = Info();
    CHECK(GetCurrent(&current) == WOTBMOD_V3_OK);
    CHECK(current.cluster_id == 3 && current.current == 1u && std::strcmp(current.name, "EU_C3") == 0);
    *reinterpret_cast<void**>(g_connection + 0xCu) = nullptr;
    CHECK(GetCurrent(&current) == WOTBMOD_V3_E_NOT_FOUND);
    *reinterpret_cast<void**>(g_connection + 0xCu) = g_hosts[2];

    /* Change: the call reaches the fake with the captured login manager and
     * sets the manual flag; AUTO leaves it clear. */
    g_calls.clear();
    CHECK(Change(4) == WOTBMOD_V3_OK);
    CHECK(g_calls.size() == 1u && g_calls[0] == "lm:change(4)");
    CHECK(g_login_manager[0x31Cu] == 1u);
    /* AUTO: the manual flag is cleared first, then the client re-logins
     * through Disconnect(12); ChangeCluster(-1) is never called. */
    CHECK(Change(WOTBMOD_V3_SESSION_CLUSTER_AUTO) == WOTBMOD_V3_OK);
    CHECK(g_calls.size() == 2u && g_calls[1] == "cm:disconnect(12,0,0)");
    CHECK(g_login_manager[0x31Cu] == 0u);
    CHECK(SetManual(true) == WOTBMOD_V3_OK && g_login_manager[0x31Cu] == 1u);
    CHECK(SetManual(false) == WOTBMOD_V3_OK && g_login_manager[0x31Cu] == 0u);

    /* A region of an unexpected shape is refused at capture time. */
    g_region[1] = g_hosts[0] + 3u * kStride + 7u;
    CHECK(!OnHostChosen(g_login_manager, g_hosts[2]) && !Ready());
    g_region[1] = g_hosts[0] + 4u * kStride;
    CHECK(OnHostChosen(g_login_manager, g_hosts[2]) && Ready());

    Shutdown();
    CHECK(!Ready());
    CHECK(Enumerate(nullptr, &count) == WOTBMOD_V3_E_NOT_SUPPORTED);
    std::printf("V3 session.cluster native: capture, enumerate, current, change and manual flag passed\n");
    return 0;
}
