#include "../include/wotb_mod_runtime_v3.h"
#include "../include/wotbmod/events_v1.h"
#include "../include/wotbmod/ges_v1.h"
#include "../src/v3/client_services_backend.h"
#include "../src/v3/ges_schemas.h"
#include "../src/v3/ges_services.h"
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

WotbModV3Result WOTBMOD_V3_CALL TestEntry(
    const WotbModV3Bootstrap*, WotbModV3Handle, WotbModV3Info* info) {
    if (!info) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    info->struct_size = sizeof(*info);
    info->api_version = WOTBMOD_V3_ABI_VERSION;
    info->requested_permission_tier = WOTBMOD_V3_PERMISSION_REVIEWED;
    Copy(info->id, sizeof(info->id), "tests.ges-services");
    Copy(info->name, sizeof(info->name), "GES services test");
    Copy(info->version, sizeof(info->version), "1.0.0");
    Copy(info->author, sizeof(info->author), "tests");
    return WOTBMOD_V3_OK;
}

WotbModV3Handle g_mod = WOTBMOD_V3_INVALID_HANDLE;
const WotbModV3EventsApiV1* g_events = nullptr;

std::vector<std::string> g_observed;    /* "add:Name" / "release:Name" */
std::vector<std::string> g_published;   /* type names the fake backend received */
int g_delivered = 0;
int32_t g_delivered_mode = -1;
uint32_t g_last_flags = 0u;
WotbModV3Result g_read_result = WOTBMOD_V3_E_NOT_SUPPORTED;
const WotbModV3GesEvent* g_stale = nullptr;

WotbModV3Result FakeListTypes(void*, ClientHostGesTypeVisitFn visit, void* data) {
    visit(data, "Avatar::CameraModeChanged");
    visit(data, "Lobby::Survey::Accepted");
    return WOTBMOD_V3_OK;
}

WotbModV3Result g_observe_result = WOTBMOD_V3_OK;

WotbModV3Result FakeObserve(void*, const char* name, uint32_t observe) {
    g_observed.push_back(std::string(observe ? "add:" : "release:") + name);
    return g_observe_result;
}

WotbModV3Result FakePublish(
    void*, const char* name, const void*, uint32_t, uint32_t) {
    g_published.push_back(name);
    return WOTBMOD_V3_OK;
}

void WOTBMOD_V3_CALL OnGesEvent(WotbModV3Handle, WotbModV3Event* event, void*) {
    ++g_delivered;
    const auto* ges = static_cast<const WotbModV3GesEvent*>(event->payload);
    g_last_flags = ges->flags;
    g_read_result = GesApi().read_i32(ges, 0u, &g_delivered_mode);
    g_stale = ges;
}

ClientHostDeclaredBackend FakeBackend() {
    ClientHostDeclaredBackend backend = {};
    backend.struct_size = sizeof(backend);
    backend.api_version = WOTBMOD_V3_CLIENT_DECLARED_BACKEND_VERSION;
    backend.compatibility_state = WOTBMOD_V3_CLIENT_COMPATIBILITY_SUPPORTED;
    backend.ges_list_types = &FakeListTypes;
    backend.ges_observe = &FakeObserve;
    backend.ges_publish = &FakePublish;
    return backend;
}

}  // namespace

static int TestNames() {
    char out[128] = {};
    CHECK(GesTypeNameFromMangled(".?AUCameraModeChanged@Avatar@GES@@", out, sizeof(out)));
    CHECK(std::strcmp(out, "Avatar::CameraModeChanged") == 0);
    CHECK(GesTypeNameFromMangled(".?AUAccepted@Survey@Lobby@GES@@", out, sizeof(out)));
    CHECK(std::strcmp(out, "Lobby::Survey::Accepted") == 0);
    CHECK(!GesTypeNameFromMangled(".?AVUIControl@DAVA@@", out, sizeof(out)));
    CHECK(!GesTypeNameFromMangled(".?AUX@GES@@", out, 4u));

    CHECK(GesTopicFromTypeName("Lobby::Survey::Accepted", out, sizeof(out)));
    CHECK(std::strcmp(out, "wotbmod.ges.Lobby.Survey.Accepted") == 0);
    CHECK(GesTypeNameFromTopic("wotbmod.ges.Avatar.CameraModeChanged", out, sizeof(out)));
    CHECK(std::strcmp(out, "Avatar::CameraModeChanged") == 0);
    CHECK(!GesTypeNameFromTopic("wotbmod.gameplay.shot_fired", out, sizeof(out)));
    return 0;
}

static int TestSchemas() {
    uint32_t id = 0u;
    const GesSchema* schema = GesFindSchema("Avatar::CameraModeChanged", &id);
    CHECK(schema != nullptr && id != 0u);
    CHECK(schema->payload_size == 8u);
    CHECK(schema->field_count == 2u);
    CHECK(std::strcmp(schema->fields[0].name, "mode") == 0);
    CHECK(schema->fields[0].offset == 0u);
    CHECK(schema->fields[0].kind == WOTBMOD_V3_GES_FIELD_I32);
    CHECK(GesSchemaById(id) == schema);
    CHECK(GesSchemaById(0u) == nullptr);
    CHECK(GesSchemaById(9999u) == nullptr);
    CHECK(GesFindSchema("Avatar::NoSuchEvent", &id) == nullptr && id == 0u);
    /* 2026-09-04 batch: sizes from the decompiled publishers. */
    schema = GesFindSchema("Avatar::PlayerDied", &id);
    CHECK(schema && schema->payload_size == 1u && schema->field_count == 0u);
    schema = GesFindSchema("Session::RoundFinished", &id);
    CHECK(schema && schema->payload_size == 8u && schema->field_count == 2u &&
          schema->fields[1].offset == 4u);
    schema = GesFindSchema("InputMode::InputModeChanged", &id);
    CHECK(schema && schema->fields[0].kind == WOTBMOD_V3_GES_FIELD_U8);
    return 0;
}

static int TestPatterns() {
    const uint64_t on = WOTBMOD_V3_EVENT_SOURCE_GES;
    CHECK(GesPatternCanMatch("wotbmod.ges.*", on));
    CHECK(GesPatternCanMatch("wotbmod.ges.Avatar.*", on));
    CHECK(GesPatternCanMatch("wotbmod.ges.Avatar.CameraModeChanged", on));
    CHECK(GesPatternCanMatch("wotbmod.*", on));
    CHECK(!GesPatternCanMatch("wotbmod.ges.*", 0u));
    CHECK(!GesPatternCanMatch("wotbmod.gameplay.*", on));
    CHECK(!GesPatternCanMatch("wotbmod.ges.", on));
    return 0;
}

static int TestLazyObservation() {
    GesResetForTests();
    g_observed.clear();
    ClientHostDeclaredBackend backend = FakeBackend();
    SetClientHostDeclaredBackend(&backend);

    GesOnPatternSubscribed("wotbmod.ges.Avatar.CameraModeChanged");
    CHECK(g_observed.size() == 1u && g_observed[0] == "add:Avatar::CameraModeChanged");
    GesOnPatternSubscribed("wotbmod.ges.Avatar.*");   /* same type: refcount, no second add */
    CHECK(g_observed.size() == 1u);
    GesOnPatternUnsubscribed("wotbmod.ges.Avatar.CameraModeChanged");
    CHECK(g_observed.size() == 1u);                   /* still held by the wildcard */
    GesOnPatternUnsubscribed("wotbmod.ges.Avatar.*");
    CHECK(g_observed.size() == 2u && g_observed[1] == "release:Avatar::CameraModeChanged");

    g_observed.clear();
    GesOnPatternSubscribed("wotbmod.ges.*");          /* every known type */
    CHECK(g_observed.size() == 2u);
    GesOnPatternUnsubscribed("wotbmod.ges.*");
    CHECK(g_observed.size() == 4u);

    g_observed.clear();
    GesOnPatternSubscribed("wotbmod.gameplay.*");     /* not a GES pattern */
    CHECK(g_observed.empty());
    SetClientHostDeclaredBackend(nullptr);
    return 0;
}

/* The bus is captured some time after the loader declares the backend; an
 * observation asked for in that window is kept and retried from the frame
 * boundary until the backend takes it. */
static int TestPendingObservation() {
    GesResetForTests();
    g_observed.clear();
    g_observe_result = WOTBMOD_V3_E_NOT_SUPPORTED;
    ClientHostDeclaredBackend backend = FakeBackend();
    SetClientHostDeclaredBackend(&backend);

    GesOnPatternSubscribed("wotbmod.ges.Avatar.CameraModeChanged");
    CHECK(g_observed.size() == 1u && g_observed[0] == "add:Avatar::CameraModeChanged");
    GesRetryPendingObservations();                    /* still no bus: asked again */
    CHECK(g_observed.size() == 2u && g_observed[1] == "add:Avatar::CameraModeChanged");
    g_observe_result = WOTBMOD_V3_OK;
    GesRetryPendingObservations();                    /* bus arrived: taken */
    CHECK(g_observed.size() == 3u);
    GesRetryPendingObservations();                    /* nothing pending any more */
    CHECK(g_observed.size() == 3u);
    GesOnPatternUnsubscribed("wotbmod.ges.Avatar.CameraModeChanged");
    CHECK(g_observed.size() == 4u && g_observed[3] == "release:Avatar::CameraModeChanged");

    /* Unsubscribed before the bus ever came: nothing to release natively. */
    g_observed.clear();
    g_observe_result = WOTBMOD_V3_E_NOT_SUPPORTED;
    GesOnPatternSubscribed("wotbmod.ges.Avatar.CameraModeChanged");
    GesOnPatternUnsubscribed("wotbmod.ges.Avatar.CameraModeChanged");
    CHECK(g_observed.size() == 1u);
    g_observe_result = WOTBMOD_V3_OK;
    GesRetryPendingObservations();
    CHECK(g_observed.size() == 1u);

    /* A final failure (unknown type) is not retried forever. */
    g_observed.clear();
    g_observe_result = WOTBMOD_V3_E_NOT_FOUND;
    GesOnPatternSubscribed("wotbmod.ges.Avatar.CameraModeChanged");
    g_observe_result = WOTBMOD_V3_OK;
    GesRetryPendingObservations();
    CHECK(g_observed.size() == 1u);
    GesOnPatternUnsubscribed("wotbmod.ges.Avatar.CameraModeChanged");
    g_observe_result = WOTBMOD_V3_OK;
    SetClientHostDeclaredBackend(nullptr);
    return 0;
}

static int TestHostPublishAndReads() {
    GesResetForTests();
    ClientHostDeclaredBackend backend = FakeBackend();
    SetClientHostDeclaredBackend(&backend);
    const uint64_t restore = GetEventSourceMask();
    SetEventSourceMask(WOTBMOD_V3_EVENT_SOURCE_GES);

    WotbModV3EventSubscriptionInfo info = {};
    WOTBMOD_V3_INIT_STRUCT(info, WOTBMOD_V3_EVENTS_VERSION);
    info.topic_pattern = "wotbmod.ges.Avatar.*";
    info.priority = WOTBMOD_V3_EVENT_PRIORITY_NORMAL;
    info.receive_system_events = 1u;
    WotbModV3EventToken token = WOTBMOD_V3_INVALID_HANDLE;
    g_observed.clear();
    CHECK(g_events->subscribe(g_mod, &info, &OnGesEvent, nullptr, &token) == WOTBMOD_V3_OK);
    CHECK(g_observed.size() == 1u && g_observed[0] == "add:Avatar::CameraModeChanged");

    struct { int32_t mode; uint8_t flag; } native = {0, 1};
    g_delivered = 0;
    g_delivered_mode = -1;
    CHECK(GesHostPublish("Avatar::CameraModeChanged", &native, 0x15FFA10u, 0u) == WOTBMOD_V3_OK);
    CHECK(g_delivered == 1);                          /* synchronous, inside the publish call */
    CHECK(g_read_result == WOTBMOD_V3_OK && g_delivered_mode == 0);
    CHECK((g_last_flags & WOTBMOD_V3_GES_EVENT_SIZE_KNOWN) != 0u);
    CHECK((g_last_flags & WOTBMOD_V3_GES_EVENT_SCHEMA_KNOWN) != 0u);
    int32_t after = 0;
    CHECK(GesApi().read_i32(g_stale, 0u, &after) == WOTBMOD_V3_E_OBJECT_DESTROYED);

    /* Unknown type: still delivered, but with no size and no schema. */
    g_delivered = 0;
    g_last_flags = 0xFFFFFFFFu;
    CHECK(GesHostPublish("Avatar::Unknown", &native, 0u, 0u) == WOTBMOD_V3_OK);
    CHECK(g_delivered == 1);
    CHECK((g_last_flags & WOTBMOD_V3_GES_EVENT_SIZE_KNOWN) == 0u);
    CHECK((g_last_flags & WOTBMOD_V3_GES_EVENT_SCHEMA_KNOWN) == 0u);

    /* A type outside the subscribed pattern is not delivered. */
    g_delivered = 0;
    CHECK(GesHostPublish("Lobby::Survey::Accepted", &native, 0u, 0u) == WOTBMOD_V3_OK);
    CHECK(g_delivered == 0);

    /* publish() gating: schema required, size must match, then the backend. */
    CHECK(GesApi().publish(g_mod, "Avatar::Unknown", &native, sizeof(native), 0u) == WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(GesApi().publish(g_mod, "Avatar::CameraModeChanged", &native, 4u, 0u) == WOTBMOD_V3_E_INVALID_ARGUMENT);
    g_published.clear();
    CHECK(GesApi().publish(g_mod, "Avatar::CameraModeChanged", &native, 8u, 0u) == WOTBMOD_V3_OK);
    CHECK(g_published.size() == 1u && g_published[0] == "Avatar::CameraModeChanged");

    /* Schema slots. */
    uint32_t schema_id = 0u, size = 0u, fields = 0u;
    CHECK(GesApi().get_schema("Avatar::CameraModeChanged", &schema_id, &size, &fields) == WOTBMOD_V3_OK);
    CHECK(schema_id == 1u && size == 8u && fields == 2u);
    WotbModV3GesField field = {};
    WOTBMOD_V3_INIT_STRUCT(field, WOTBMOD_V3_GES_VERSION);
    CHECK(GesApi().schema_field(schema_id, 1u, &field) == WOTBMOD_V3_OK);
    CHECK(std::strcmp(field.name, "flag") == 0 && field.offset == 4u && field.kind == WOTBMOD_V3_GES_FIELD_BOOL);
    CHECK(GesApi().schema_field(schema_id, 2u, &field) == WOTBMOD_V3_E_NOT_FOUND);

    /* list_types goes through the backend. */
    const char* names[4] = {};
    uint32_t count = 0u;
    CHECK(GesApi().list_types(g_mod, names, 4u, &count) == WOTBMOD_V3_OK);
    CHECK(count == 2u && std::strcmp(names[1], "Lobby::Survey::Accepted") == 0);
    CHECK(GesApi().list_types(g_mod, nullptr, 0u, &count) == WOTBMOD_V3_OK && count == 2u);

    g_observed.clear();
    CHECK(g_events->unsubscribe(g_mod, token) == WOTBMOD_V3_OK);
    CHECK(g_observed.size() == 1u && g_observed[0] == "release:Avatar::CameraModeChanged");

    SetClientHostDeclaredBackend(nullptr);
    CHECK(GesApi().list_types(g_mod, nullptr, 0u, &count) == WOTBMOD_V3_E_NOT_SUPPORTED);
    SetEventSourceMask(restore);
    return 0;
}

int main() {
    if (TestNames()) return 1;
    if (TestSchemas()) return 1;
    if (TestPatterns()) return 1;
    if (TestPendingObservation()) return 1;

    WotbModV3RuntimeOptions options = {};
    options.struct_size = sizeof(options);
    options.api_version = WOTBMOD_V3_ABI_VERSION;
    options.game_directory = ".";
    options.mods_directory = "build\\v3_ges_tests\\mods";
    options.cache_directory = "build\\v3_ges_tests\\cache";
    options.config_directory = "build\\v3_ges_tests\\config";
    options.client_version = "ges-services-test";
    CHECK(WotbModV3Runtime_Initialize(&options) == WOTBMOD_V3_OK);
    CHECK(WotbModV3Runtime_CreateMod(
              "ges_services_test.dll", WOTBMOD_V3_PERMISSION_REVIEWED, &g_mod) == WOTBMOD_V3_OK);
    WotbModV3RuntimeModuleInfo module = {};
    module.struct_size = sizeof(module);
    module.api_version = WOTBMOD_V3_ABI_VERSION;
    CHECK(WotbModV3Runtime_InvokeEntry(g_mod, &TestEntry, &module) == WOTBMOD_V3_OK);
    CHECK(WotbModV3Runtime_Enable(g_mod) == WOTBMOD_V3_OK);
    const WotbModV3Bootstrap* bootstrap = WotbModV3Runtime_GetBootstrap();
    CHECK(bootstrap != nullptr);
    const void* table = nullptr;
    CHECK(bootstrap->query_interface(g_mod, WOTBMOD_V3_IFACE_EVENTS, WOTBMOD_V3_EVENTS_VERSION, &table) == WOTBMOD_V3_OK);
    g_events = static_cast<const WotbModV3EventsApiV1*>(table);
    CHECK(g_events != nullptr);

    const int lazy = TestLazyObservation();
    const int publish = lazy ? 1 : TestHostPublishAndReads();
    WotbModV3Runtime_Shutdown();
    if (lazy || publish) return 1;
    std::printf("V3 GES services: names, schemas, patterns, observation and publish passed\n");
    return 0;
}
