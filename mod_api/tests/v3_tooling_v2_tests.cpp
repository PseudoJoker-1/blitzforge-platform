#include "../include/wotb_mod_runtime_v3.h"
#include "../src/v3/wotb_mod_v3_internal.h"

#include <cstdio>
#include <limits>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace wotbmod {
namespace v3 {

void RegisterRuntimeServices() {}
void RegisterDataServices() {}
void RegisterClientServices() {}
void SetNativeHookBackend(const WotbModV3NativeHookBackend*) {}

// This focused build links wotb_mod_v3_runtime.cpp without the services
// layer, so every services symbol it references must be stubbed here.
// wotb_mod_v3_runtime.cpp announces capability changes on the event bus
// via PublishSystemEvent; the event bus itself is not under test here.
WotbModV3Result PublishSystemEvent(
    const char*,
    const void*,
    uint32_t,
    uint32_t) {
    return WOTBMOD_V3_OK;
}

WotbModV3Result SnapshotOwnedHooksForDevtools(
    WotbModV3Handle,
    const char* selector,
    std::vector<DevtoolsHookSnapshot>* out_snapshots) {
    if (!out_snapshots) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    out_snapshots->clear();
    if (selector && std::strcmp(selector, "__error__") == 0) {
        return WOTBMOD_V3_E_PLATFORM;
    }
    DevtoolsHookSnapshot snapshot;
    snapshot.hook = 101u;
    snapshot.creation_order = 7u;
    snapshot.mode = 4u;
    snapshot.status = 0u;
    snapshot.priority = -25;
    snapshot.conflict_count = 1u;
    snapshot.target = "Camera::set\"FOV\n";
    if (!selector || selector[0] == '\0' ||
        snapshot.target == selector) {
        out_snapshots->push_back(std::move(snapshot));
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result SnapshotOwnedEventsForDevtools(
    WotbModV3Handle,
    const char* selector,
    std::vector<DevtoolsEventSnapshot>* out_snapshots) {
    if (!out_snapshots) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    out_snapshots->clear();
    if (selector && std::strcmp(selector, "__error__") == 0) {
        return WOTBMOD_V3_E_PLATFORM;
    }
    DevtoolsEventSnapshot snapshot;
    snapshot.token = 202u;
    snapshot.creation_order = 9u;
    snapshot.priority = 100;
    snapshot.receive_system_events = 1u;
    snapshot.active = 1u;
    snapshot.accepting_callbacks = 1u;
    snapshot.callbacks_in_flight = 2u;
    snapshot.topic_pattern = "mod.tests.\"quoted\n*";
    if (!selector || selector[0] == '\0' ||
        snapshot.topic_pattern == selector) {
        out_snapshots->push_back(std::move(snapshot));
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result SnapshotOwnedResourcesForDevtools(
    WotbModV3Handle,
    const char* selector,
    std::vector<DevtoolsResourceSnapshot>* out_snapshots) {
    if (!out_snapshots) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    out_snapshots->clear();
    if (selector && std::strcmp(selector, "__error__") == 0) {
        return WOTBMOD_V3_E_PLATFORM;
    }
    DevtoolsResourceSnapshot snapshot;
    snapshot.resource = 303u;
    snapshot.type = 1u;
    snapshot.state = 1u;
    snapshot.backing = 0u;
    snapshot.operation_in_progress = 1u;
    snapshot.memory_bytes = 4096u;
    snapshot.uri = "mod://assets/a\"b\n.bin";
    snapshot.group = "battle\\fx";
    snapshot.sha256 = "abc";
    snapshot.error = "waiting\tfor worker";
    if (!selector || selector[0] == '\0' ||
        snapshot.uri == selector) {
        out_snapshots->push_back(std::move(snapshot));
    }
    return WOTBMOD_V3_OK;
}

}  // namespace v3
}  // namespace wotbmod

namespace {

uint32_t g_checks = 0u;
uint32_t g_failures = 0u;

void Check(bool condition, const char* expression, int line) {
    ++g_checks;
    if (condition) return;
    ++g_failures;
    std::fprintf(
        stderr,
        "V3 TOOLING V2 FAIL line %d: %s\n",
        line,
        expression ? expression : "(null)");
}

#define CHECK(expression) Check((expression), #expression, __LINE__)

WotbModV3Result WOTBMOD_V3_CALL TestEntry(
    const WotbModV3Bootstrap*,
    WotbModV3Handle,
    WotbModV3Info* out_info) {
    if (!out_info) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    std::memset(out_info, 0, sizeof(*out_info));
    out_info->struct_size = sizeof(*out_info);
    out_info->api_version = WOTBMOD_V3_ABI_VERSION;
    out_info->requested_permission_tier =
        WOTBMOD_V3_PERMISSION_REVIEWED;
    strcpy_s(out_info->id, "tests.tooling-v2");
    strcpy_s(out_info->name, "Tooling V2 test");
    strcpy_s(out_info->version, "2.0.0");
    return WOTBMOD_V3_OK;
}

std::string CopyReport(
    const WotbModV3DiagnosticsApiV2* diagnostics,
    WotbModV3Handle mod) {
    uint32_t size = 0u;
    CHECK(
        diagnostics->copy_report_json(mod, nullptr, &size) ==
        WOTBMOD_V3_E_BUFFER_TOO_SMALL);
    CHECK(size > 1u);
    std::vector<char> bytes(size);
    CHECK(
        diagnostics->copy_report_json(
            mod,
            bytes.data(),
            &size) == WOTBMOD_V3_OK);
    return std::string(bytes.data());
}

std::string CopyInspector(
    WotbModV3DevtoolsInspectFn inspector,
    WotbModV3Handle mod,
    const char* selector = nullptr) {
    uint32_t size = 0u;
    CHECK(
        inspector(mod, selector, nullptr, &size) ==
        WOTBMOD_V3_E_BUFFER_TOO_SMALL);
    CHECK(size > 1u);
    std::vector<char> bytes(size);
    CHECK(
        inspector(
            mod,
            selector,
            bytes.data(),
            &size) == WOTBMOD_V3_OK);
    return std::string(bytes.data());
}

void TestDiagnostics(
    const WotbModV3DiagnosticsApiV2* diagnostics,
    WotbModV3Handle mod) {
    WotbModV3DiagnosticsStats stats = {};
    WOTBMOD_V3_INIT_STRUCT(
        stats,
        WOTBMOD_V3_DIAGNOSTICS_VERSION);
    CHECK(diagnostics->get_stats(mod, &stats) == WOTBMOD_V3_OK);
    CHECK(stats.loaded_mods == 1u);
    CHECK(stats.enabled_mods == 1u);

    CHECK(
        diagnostics->crash_add_context(
            mod,
            "screen",
            "hangar \"winter\"\nline") == WOTBMOD_V3_OK);
    CHECK(
        diagnostics->crash_add_context(
            mod,
            "screen",
            "battle") == WOTBMOD_V3_OK);
    CHECK(
        diagnostics->crash_set_last_action(
            mod,
            "open \"settings\"\npage") == WOTBMOD_V3_OK);
    CHECK(
        diagnostics->report_fault(
            mod,
            "test.fault",
            "fault-one",
            "{\"code\":7}") == WOTBMOD_V3_OK);

    for (uint32_t index = 1u;
         index < WOTBMOD_V3_DIAGNOSTICS_MAX_CONTEXT_ENTRIES;
         ++index) {
        char key[32] = {};
        _snprintf_s(
            key,
            sizeof(key),
            _TRUNCATE,
            "key-%u",
            index);
        CHECK(
            diagnostics->crash_add_context(
                mod,
                key,
                "value") == WOTBMOD_V3_OK);
    }
    CHECK(
        diagnostics->crash_add_context(
            mod,
            "overflow",
            "value") == WOTBMOD_V3_E_LIMIT_REACHED);
    std::string too_long_key(
        WOTBMOD_V3_DIAGNOSTICS_MAX_CONTEXT_KEY,
        'x');
    CHECK(
        diagnostics->crash_add_context(
            mod,
            too_long_key.c_str(),
            "value") == WOTBMOD_V3_E_INVALID_ARGUMENT);

    for (uint32_t index = 0u;
         index < WOTBMOD_V3_DIAGNOSTICS_MAX_BREADCRUMBS + 3u;
         ++index) {
        char message[64] = {};
        _snprintf_s(
            message,
            sizeof(message),
            _TRUNCATE,
            "bounded-crumb-%u",
            index);
        CHECK(
            diagnostics->crash_add_breadcrumb(
                mod,
                "test",
                message,
                "{\"ok\":true}") == WOTBMOD_V3_OK);
    }

    WotbModV3ModHealth health = {};
    WOTBMOD_V3_INIT_STRUCT(
        health,
        WOTBMOD_V3_DIAGNOSTICS_VERSION_2);
    CHECK(
        diagnostics->get_mod_health(mod, &health) ==
        WOTBMOD_V3_OK);
    CHECK(health.runtime_state == WOTBMOD_V3_DIAGNOSTIC_MOD_ENABLED);
    CHECK(health.enabled == 1u);
    CHECK(health.permission_tier == WOTBMOD_V3_PERMISSION_REVIEWED);
    CHECK(health.reported_faults == 1u);
    CHECK(
        health.crash_context_entries ==
        WOTBMOD_V3_DIAGNOSTICS_MAX_CONTEXT_ENTRIES);
    CHECK(
        health.retained_breadcrumbs ==
        WOTBMOD_V3_DIAGNOSTICS_MAX_BREADCRUMBS);
    CHECK(
        std::strcmp(
            health.last_action,
            "open \"settings\"\npage") == 0);
    CHECK(health.diagnostic_owned_bytes > 0u);
    CHECK(
        health.peak_diagnostic_owned_bytes >=
        health.diagnostic_owned_bytes);

    const std::string report = CopyReport(diagnostics, mod);
    CHECK(
        report.find("\"schema\":\"wotbmod.diagnostics/v2\"") !=
        std::string::npos);
    CHECK(
        report.find("\"screen\":\"battle\"") !=
        std::string::npos);
    CHECK(
        report.find("open \\\"settings\\\"\\npage") !=
        std::string::npos);
    CHECK(
        report.find("bounded-crumb-66") != std::string::npos);
    CHECK(
        report.find("bounded-crumb-0\"") == std::string::npos);
}

void TestProfiler(
    const WotbModV3DiagnosticsApiV2* diagnostics,
    const WotbModV3DevtoolsApiV2* devtools,
    WotbModV3Handle mod) {
    WotbModV3ProfilerMemory before = {};
    WOTBMOD_V3_INIT_STRUCT(
        before,
        WOTBMOD_V3_DEVTOOLS_VERSION_2);
    CHECK(
        devtools->profiler_get_mod_memory(mod, &before) ==
        WOTBMOD_V3_OK);

    WotbModV3Handle span = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        devtools->span_begin(
            mod,
            "tests",
            "cpu-work",
            &span) == WOTBMOD_V3_OK);
    CHECK(span != WOTBMOD_V3_INVALID_HANDLE);
    WotbModV3ProfilerMemory active = {};
    WOTBMOD_V3_INIT_STRUCT(
        active,
        WOTBMOD_V3_DEVTOOLS_VERSION_2);
    CHECK(
        devtools->profiler_get_mod_memory(mod, &active) ==
        WOTBMOD_V3_OK);
    CHECK(active.active_span_bytes > 0u);
    CHECK(active.total_owned_bytes > before.total_owned_bytes);

    volatile uint64_t work = 0u;
    for (uint32_t index = 0u; index < 2000000u; ++index) {
        work += index;
    }
    CHECK(work != 0u);
    WotbModV3ProfilerSpanInfo span_info = {};
    WOTBMOD_V3_INIT_STRUCT(
        span_info,
        WOTBMOD_V3_DEVTOOLS_VERSION);
    CHECK(
        devtools->span_end(mod, span, &span_info) ==
        WOTBMOD_V3_OK);
    CHECK(span_info.elapsed_ticks > 0u);
    CHECK(std::strcmp(span_info.name, "cpu-work") == 0);

    WotbModV3ProfilerAggregate aggregate = {};
    WOTBMOD_V3_INIT_STRUCT(
        aggregate,
        WOTBMOD_V3_DEVTOOLS_VERSION_2);
    CHECK(
        devtools->profiler_get_mod_cpu_time(
            mod,
            &aggregate) == WOTBMOD_V3_OK);
    CHECK(aggregate.active_spans == 0u);
    CHECK(aggregate.completed_spans == 1u);
    CHECK(aggregate.cpu_measured_spans == 1u);
    CHECK(
        aggregate.total_cpu_milliseconds ==
        static_cast<double>(aggregate.total_cpu_100ns) / 10000.0);

    WotbModV3ProfilerMemory ended = {};
    WOTBMOD_V3_INIT_STRUCT(
        ended,
        WOTBMOD_V3_DEVTOOLS_VERSION_2);
    CHECK(
        devtools->profiler_get_mod_memory(mod, &ended) ==
        WOTBMOD_V3_OK);
    CHECK(ended.active_span_bytes == 0u);
    CHECK(ended.total_owned_bytes == ended.retained_diagnostics_bytes);
    CHECK(ended.peak_owned_bytes >= active.total_owned_bytes);

    WotbModV3ModHealth health = {};
    WOTBMOD_V3_INIT_STRUCT(
        health,
        WOTBMOD_V3_DIAGNOSTICS_VERSION_2);
    CHECK(
        diagnostics->get_mod_health(mod, &health) ==
        WOTBMOD_V3_OK);
    CHECK(health.completed_profiler_spans == 1u);
    CHECK(health.cpu_measured_profiler_spans == 1u);
    CHECK(health.profiler_cpu_100ns == aggregate.total_cpu_100ns);
}

void TestCounters(
    const WotbModV3DiagnosticsApiV2* diagnostics,
    const WotbModV3DevtoolsApiV2* devtools,
    WotbModV3Handle mod) {
    CHECK(
        devtools->counter_set(
            mod,
            "battle",
            "visible-shells",
            2.0) == WOTBMOD_V3_OK);
    CHECK(
        devtools->counter_set(
            mod,
            "battle",
            "visible-shells",
            3.0) == WOTBMOD_V3_OK);
    CHECK(
        devtools->counter_set(
            mod,
            nullptr,
            "frame-budget",
            16.5) == WOTBMOD_V3_OK);
    CHECK(
        devtools->counter_set(
            mod,
            "battle",
            "",
            1.0) == WOTBMOD_V3_E_INVALID_ARGUMENT);
    CHECK(
        devtools->counter_set(
            mod,
            "battle",
            "invalid",
            std::numeric_limits<double>::infinity()) ==
        WOTBMOD_V3_E_INVALID_ARGUMENT);
    const std::string report = CopyReport(diagnostics, mod);
    CHECK(
        report.find("\"counters\":[") != std::string::npos);
    CHECK(
        report.find(
            "\"category\":\"battle\",\"name\":\"visible-shells\","
            "\"value\":3.000000000") != std::string::npos);
    CHECK(
        report.find(
            "\"category\":\"devtools\",\"name\":\"frame-budget\","
            "\"value\":16.500000000") != std::string::npos);
    CHECK(
        report.find("\"value\":2.000000000") == std::string::npos);
}

void TestInspectors(
    const WotbModV3DevtoolsApiV2* devtools,
    WotbModV3Handle mod) {
    // inspect_scene and inspect_material DO have an implementation now, on the
    // reviewed DAVA scene routes. They still refuse here, and must: this binary
    // installs no DAVA native backend, so the capability gate answers
    // E_NOT_SUPPORTED - which is the point. The row that matters is that a
    // refusal reports a required size of zero rather than leaving the caller's
    // own value in place.
    const WotbModV3DevtoolsInspectFn unsupported[] = {
        devtools->inspect_ui,
        devtools->inspect_scene,
        devtools->inspect_material
    };
    for (WotbModV3DevtoolsInspectFn inspector : unsupported) {
        CHECK(inspector != nullptr);
        uint32_t size = 99u;
        CHECK(
            inspector(mod, nullptr, nullptr, &size) ==
            WOTBMOD_V3_E_NOT_SUPPORTED);
        CHECK(size == 0u);
    }

    CHECK(devtools->inspect_resource != nullptr);
    const std::string resources =
        CopyInspector(devtools->inspect_resource, mod);
    CHECK(
        resources.find(
            "\"schema\":\"wotbmod.devtools/resources-v1\"") !=
        std::string::npos);
    CHECK(resources.find("\"scope\":\"owner\"") != std::string::npos);
    CHECK(
        resources.find("\"native_enumeration\":false") !=
        std::string::npos);
    CHECK(resources.find("\"count\":1") != std::string::npos);
    CHECK(resources.find("\"resource\":303") != std::string::npos);
    CHECK(
        resources.find("\"operation_in_progress\":true") !=
        std::string::npos);
    CHECK(
        resources.find("mod://assets/a\\\"b\\n.bin") !=
        std::string::npos);
    CHECK(resources.find("battle\\\\fx") != std::string::npos);
    CHECK(resources.find("waiting\\tfor worker") != std::string::npos);
    const std::string no_resources =
        CopyInspector(
            devtools->inspect_resource,
            mod,
            "mod://missing/resource.bin");
    CHECK(no_resources.find("\"count\":0") != std::string::npos);

    CHECK(devtools->inspect_hook_chain != nullptr);
    const std::string hooks =
        CopyInspector(devtools->inspect_hook_chain, mod);
    CHECK(
        hooks.find(
            "\"schema\":\"wotbmod.devtools/hook-chains-v1\"") !=
        std::string::npos);
    CHECK(hooks.find("\"count\":1") != std::string::npos);
    CHECK(hooks.find("\"hook\":101") != std::string::npos);
    CHECK(hooks.find("\"priority\":-25") != std::string::npos);
    CHECK(hooks.find("\"native_created\":false") != std::string::npos);
    CHECK(
        hooks.find("Camera::set\\\"FOV\\n") !=
        std::string::npos);
    const std::string no_hooks =
        CopyInspector(
            devtools->inspect_hook_chain,
            mod,
            "missing::hook");
    CHECK(no_hooks.find("\"count\":0") != std::string::npos);

    CHECK(devtools->inspect_events != nullptr);
    const std::string events =
        CopyInspector(devtools->inspect_events, mod);
    CHECK(
        events.find(
            "\"schema\":\"wotbmod.devtools/events-v1\"") !=
        std::string::npos);
    CHECK(events.find("\"count\":1") != std::string::npos);
    CHECK(events.find("\"token\":202") != std::string::npos);
    CHECK(events.find("\"receive_system_events\":true") !=
          std::string::npos);
    CHECK(events.find("\"callbacks_in_flight\":2") !=
          std::string::npos);
    CHECK(events.find("mod.tests.\\\"quoted\\n*") !=
          std::string::npos);
    const std::string no_events =
        CopyInspector(
            devtools->inspect_events,
            mod,
            "mod.missing.*");
    CHECK(no_events.find("\"count\":0") != std::string::npos);

    const WotbModV3DevtoolsInspectFn portable[] = {
        devtools->inspect_resource,
        devtools->inspect_hook_chain,
        devtools->inspect_events
    };
    for (WotbModV3DevtoolsInspectFn inspector : portable) {
        uint32_t size = 0u;
        CHECK(
            inspector(mod, nullptr, nullptr, nullptr) ==
            WOTBMOD_V3_E_INVALID_ARGUMENT);
        CHECK(
            inspector(mod, "__error__", nullptr, &size) ==
            WOTBMOD_V3_E_PLATFORM);
        CHECK(size == 0u);
        char tiny[2] = {};
        size = sizeof(tiny);
        CHECK(
            inspector(mod, nullptr, tiny, &size) ==
            WOTBMOD_V3_E_BUFFER_TOO_SMALL);
        CHECK(size > sizeof(tiny));
    }
    std::string unterminated_selector(
        WOTBMOD_V3_MAX_PATH,
        'x');
    uint32_t size = 0u;
    CHECK(
        devtools->inspect_events(
            mod,
            unterminated_selector.c_str(),
            nullptr,
            &size) == WOTBMOD_V3_E_INVALID_ARGUMENT);
}

void TestCallbackBudget(
    const WotbModV3DevtoolsApiV3* devtools,
    WotbModV3Handle mod) {
    CHECK(devtools != nullptr);
    CHECK(
        devtools->set_callback_budget(mod, 8000u, 1u) ==
        WOTBMOD_V3_OK);
    CHECK(devtools->reset_callback_profile(mod) == WOTBMOD_V3_OK);
    CHECK(
        wotbmod::v3::EnterModCallbackCoalescible(mod) ==
        WOTBMOD_V3_OK);
    wotbmod::v3::LeaveModCallback(mod);
    CHECK(
        wotbmod::v3::EnterModCallbackCoalescible(mod) ==
        WOTBMOD_V3_E_BUSY);
    WotbModV3CallbackProfile profile = {};
    WOTBMOD_V3_INIT_STRUCT(
        profile, WOTBMOD_V3_DEVTOOLS_VERSION_3);
    CHECK(
        devtools->get_callback_profile(mod, &profile) ==
        WOTBMOD_V3_OK);
    CHECK(profile.budget_microseconds == 8000u);
    CHECK(profile.max_callbacks_per_frame == 1u);
    CHECK(profile.frame_callbacks == 1u);
    CHECK(profile.total_callbacks == 1u);
    CHECK(profile.coalesced_callbacks == 1u);
    CHECK(profile.over_budget_frames == 1u);
    CHECK(devtools->reset_callback_profile(mod) == WOTBMOD_V3_OK);
    WOTBMOD_V3_INIT_STRUCT(
        profile, WOTBMOD_V3_DEVTOOLS_VERSION_3);
    CHECK(
        devtools->get_callback_profile(mod, &profile) ==
        WOTBMOD_V3_OK);
    CHECK(profile.total_callbacks == 0u);
    CHECK(profile.coalesced_callbacks == 0u);
}

void TestExport(
    const WotbModV3DiagnosticsApiV2* diagnostics,
    WotbModV3Handle mod) {
    uint32_t path_size = 0u;
    CHECK(
        diagnostics->export_bundle(
            mod,
            "focused-test.json",
            nullptr,
            &path_size) == WOTBMOD_V3_E_BUFFER_TOO_SMALL);
    CHECK(path_size > strlen("focused-test.json") + 1u);
    std::vector<char> path(path_size);
    CHECK(
        diagnostics->export_bundle(
            mod,
            "focused-test.json",
            path.data(),
            &path_size) == WOTBMOD_V3_OK);
    CHECK(
        std::string(path.data()).find(
            "mods\\data\\tooling_v2_test\\diagnostics\\focused-test.json") !=
        std::string::npos);
    std::ifstream stream(path.data(), std::ios::binary);
    CHECK(stream.good());
    const std::string contents(
        (std::istreambuf_iterator<char>(stream)),
        std::istreambuf_iterator<char>());
    CHECK(
        contents.find("\"schema\":\"wotbmod.diagnostics/v2\"") !=
        std::string::npos);
    uint32_t invalid_size = 32u;
    char invalid_path[32] = {};
    CHECK(
        diagnostics->export_bundle(
            mod,
            "..\\escape.json",
            invalid_path,
            &invalid_size) == WOTBMOD_V3_E_INVALID_ARGUMENT);
}

}  // namespace

int main() {
    static_assert(
        offsetof(WotbModV3DiagnosticsApiV2, crash_add_context) ==
            sizeof(WotbModV3DiagnosticsApiV1),
        "diagnostics V1 ABI prefix changed");
    static_assert(
        offsetof(WotbModV3DevtoolsApiV2, inspect_ui) ==
            sizeof(WotbModV3DevtoolsApiV1),
        "devtools V1 ABI prefix changed");

    WotbModV3RuntimeOptions options = {};
    options.struct_size = sizeof(options);
    options.api_version = WOTBMOD_V3_ABI_VERSION;
    options.game_directory = ".";
    options.mods_directory = "build\\v3_tooling_v2\\mods";
    options.cache_directory = "build\\v3_tooling_v2\\cache";
    options.config_directory = "build\\v3_tooling_v2\\config";
    options.client_version = "tooling-v2-test";
    options.process_architecture = 32u;
    CHECK(
        WotbModV3Runtime_Initialize(&options) ==
        WOTBMOD_V3_OK);

    WotbModV3Handle mod = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        WotbModV3Runtime_CreateMod(
            "tooling_v2_test.dll",
            WOTBMOD_V3_PERMISSION_REVIEWED,
            &mod) == WOTBMOD_V3_OK);
    WotbModV3RuntimeModuleInfo info = {};
    WOTBMOD_V3_INIT_STRUCT(info, WOTBMOD_V3_ABI_VERSION);
    CHECK(
        WotbModV3Runtime_InvokeEntry(
            mod,
            TestEntry,
            &info) == WOTBMOD_V3_OK);
    CHECK(WotbModV3Runtime_Enable(mod) == WOTBMOD_V3_OK);

    const WotbModV3Bootstrap* bootstrap =
        WotbModV3Runtime_GetBootstrap();
    CHECK(bootstrap != nullptr);
    const void* diagnostics_v1_raw = nullptr;
    const void* diagnostics_v2_raw = nullptr;
    CHECK(
        bootstrap->query_interface(
            mod,
            WOTBMOD_V3_IFACE_DIAGNOSTICS,
            WOTBMOD_V3_DIAGNOSTICS_VERSION,
            &diagnostics_v1_raw) == WOTBMOD_V3_OK);
    CHECK(
        bootstrap->query_interface(
            mod,
            WOTBMOD_V3_IFACE_DIAGNOSTICS,
            WOTBMOD_V3_DIAGNOSTICS_VERSION_2,
            &diagnostics_v2_raw) == WOTBMOD_V3_OK);
    CHECK(diagnostics_v1_raw == diagnostics_v2_raw);
    const WotbModV3DiagnosticsApiV1* diagnostics_v1 =
        static_cast<const WotbModV3DiagnosticsApiV1*>(
            diagnostics_v1_raw);
    const WotbModV3DiagnosticsApiV2* diagnostics =
        static_cast<const WotbModV3DiagnosticsApiV2*>(
            diagnostics_v2_raw);
    CHECK(
        diagnostics_v1->struct_size ==
        sizeof(WotbModV3DiagnosticsApiV2));
    WotbModV3DiagnosticsStats v1_stats = {};
    WOTBMOD_V3_INIT_STRUCT(
        v1_stats,
        WOTBMOD_V3_DIAGNOSTICS_VERSION);
    CHECK(
        diagnostics_v1->get_stats(mod, &v1_stats) ==
        WOTBMOD_V3_OK);

    const void* devtools_v1_raw = nullptr;
    const void* devtools_v2_raw = nullptr;
    const void* devtools_v3_raw = nullptr;
    CHECK(
        bootstrap->query_interface(
            mod,
            WOTBMOD_V3_IFACE_DEVTOOLS,
            WOTBMOD_V3_DEVTOOLS_VERSION,
            &devtools_v1_raw) == WOTBMOD_V3_OK);
    CHECK(
        bootstrap->query_interface(
            mod,
            WOTBMOD_V3_IFACE_DEVTOOLS,
            WOTBMOD_V3_DEVTOOLS_VERSION_2,
            &devtools_v2_raw) == WOTBMOD_V3_OK);
    CHECK(
        bootstrap->query_interface(
            mod,
            WOTBMOD_V3_IFACE_DEVTOOLS,
            WOTBMOD_V3_DEVTOOLS_VERSION_3,
            &devtools_v3_raw) == WOTBMOD_V3_OK);
    CHECK(devtools_v1_raw == devtools_v2_raw);
    CHECK(devtools_v2_raw == devtools_v3_raw);
    const WotbModV3DevtoolsApiV2* devtools =
        static_cast<const WotbModV3DevtoolsApiV2*>(
            devtools_v2_raw);
    const WotbModV3DevtoolsApiV3* devtools_v3 =
        static_cast<const WotbModV3DevtoolsApiV3*>(
            devtools_v3_raw);

    TestDiagnostics(diagnostics, mod);
    TestProfiler(diagnostics, devtools, mod);
    TestCounters(diagnostics, devtools, mod);
    TestInspectors(devtools, mod);
    TestCallbackBudget(devtools_v3, mod);
    TestExport(diagnostics, mod);

    CHECK(
        WotbModV3Runtime_DestroyMod(mod) ==
        WOTBMOD_V3_OK);
    WotbModV3Runtime_Shutdown();
    std::printf(
        "V3 TOOLING V2: %u checks, %u failures\n",
        g_checks,
        g_failures);
    return g_failures == 0u ? 0 : 1;
}
