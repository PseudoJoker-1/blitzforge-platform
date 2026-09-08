#include "wotb_mod_api_v3.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

struct ProbeCounters {
    uint32_t passed;
    uint32_t failed;
    uint32_t unavailable;
};

struct InterfaceProbe {
    const char* name;
    uint32_t version;
    size_t table_size;
};

void CopyText(char* destination, size_t capacity, const char* source) {
    if (!destination || capacity == 0u) return;
    destination[0] = '\0';
    if (!source) return;
#if defined(_MSC_VER)
    strncpy_s(destination, capacity, source, _TRUNCATE);
#else
    std::strncpy(destination, source, capacity - 1u);
    destination[capacity - 1u] = '\0';
#endif
}

const WotbModV3CoreApiV1* QueryCore(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod) {
    if (!bootstrap || !bootstrap->query_interface) return nullptr;
    const void* table = nullptr;
    if (bootstrap->query_interface(
            mod,
            WOTBMOD_V3_IFACE_CORE,
            WOTBMOD_V3_CORE_VERSION,
            &table) != WOTBMOD_V3_OK) {
        return nullptr;
    }
    return static_cast<const WotbModV3CoreApiV1*>(table);
}

void Log(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod,
    uint32_t level,
    const char* message) {
    const WotbModV3CoreApiV1* core = QueryCore(bootstrap, mod);
    if (core && core->log) {
        core->log(
            mod,
            level,
            "v3.example.core-runtime",
            message ? message : "");
    }
}

void Record(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod,
    ProbeCounters* counters,
    bool passed,
    const char* operation) {
    if (!counters) return;
    if (passed) {
        ++counters->passed;
        return;
    }
    ++counters->failed;
    char message[WOTBMOD_V3_MAX_MESSAGE] = {};
#if defined(_MSC_VER)
    sprintf_s(
        message,
        "FAILED: %s",
        operation ? operation : "unnamed probe");
#else
    std::snprintf(
        message,
        sizeof(message),
        "FAILED: %s",
        operation ? operation : "unnamed probe");
#endif
    Log(bootstrap, mod, WOTBMOD_V3_LOG_ERROR, message);
}

bool IsExpectedUnavailable(WotbModV3Result result) {
    return result == WOTBMOD_V3_E_NOT_SUPPORTED ||
           result == WOTBMOD_V3_E_PERMISSION_DENIED ||
           result == WOTBMOD_V3_E_CLIENT_MISMATCH ||
           result == WOTBMOD_V3_E_PLATFORM;
}

bool IsExpectedReadResult(WotbModV3Result result) {
    return result == WOTBMOD_V3_OK ||
           result == WOTBMOD_V3_E_NOT_FOUND ||
           result == WOTBMOD_V3_E_BUFFER_TOO_SMALL ||
           result == WOTBMOD_V3_E_WRONG_THREAD ||
           result == WOTBMOD_V3_E_BUSY ||
           IsExpectedUnavailable(result);
}

bool ValidateFunctionTable(
    const void* table,
    uint32_t expected_version,
    size_t expected_size) {
    if (!table || expected_size < sizeof(uint32_t) * 2u) return false;

    uint32_t reported_size = 0u;
    uint32_t reported_version = 0u;
    std::memcpy(&reported_size, table, sizeof(reported_size));
    std::memcpy(
        &reported_version,
        static_cast<const uint8_t*>(table) + sizeof(uint32_t),
        sizeof(reported_version));
    if (reported_size < expected_size ||
        reported_version != expected_version) {
        return false;
    }

    const uint8_t* bytes = static_cast<const uint8_t*>(table);
    for (size_t offset = sizeof(uint32_t) * 2u;
         offset + sizeof(uintptr_t) <= expected_size;
         offset += sizeof(uintptr_t)) {
        uintptr_t function_slot = 0u;
        std::memcpy(&function_slot, bytes + offset, sizeof(function_slot));
        if (function_slot == 0u) return false;
    }
    return true;
}

void ProbeInterface(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod,
    const InterfaceProbe& probe,
    ProbeCounters* counters) {
    WotbModV3InterfaceInfo info = {};
    WOTBMOD_V3_INIT_STRUCT(info, WOTBMOD_V3_ABI_VERSION);
    const WotbModV3Result info_result =
        bootstrap->get_interface_info(mod, probe.name, &info);
    Record(
        bootstrap,
        mod,
        counters,
        info_result == WOTBMOD_V3_OK,
        probe.name);
    if (info_result != WOTBMOD_V3_OK) return;

    const void* table = nullptr;
    const WotbModV3Result query_result =
        bootstrap->query_interface(
            mod,
            probe.name,
            probe.version,
            &table);
    if (query_result != WOTBMOD_V3_OK) {
        if (IsExpectedUnavailable(query_result)) {
            ++counters->unavailable;
            return;
        }
        Record(
            bootstrap,
            mod,
            counters,
            false,
            probe.name);
        return;
    }

    Record(
        bootstrap,
        mod,
        counters,
        ValidateFunctionTable(
            table,
            probe.version,
            probe.table_size),
        probe.name);
}

template <typename T>
const T* QueryApi(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod,
    const char* name,
    uint32_t version) {
    const void* table = nullptr;
    if (!bootstrap || !bootstrap->query_interface ||
        bootstrap->query_interface(
            mod,
            name,
            version,
            &table) != WOTBMOD_V3_OK) {
        return nullptr;
    }
    return static_cast<const T*>(table);
}

void ProbeBootstrap(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod,
    ProbeCounters* counters) {
    Record(
        bootstrap,
        mod,
        counters,
        bootstrap &&
            bootstrap->struct_size >= sizeof(WotbModV3Bootstrap) &&
            bootstrap->api_version == WOTBMOD_V3_ABI_VERSION &&
            bootstrap->bootstrap_version ==
                WOTBMOD_V3_BOOTSTRAP_VERSION &&
            bootstrap->query_interface &&
            bootstrap->get_interface_info &&
            bootstrap->get_last_error &&
            bootstrap->get_client_info,
        "bootstrap ABI");
    if (!bootstrap || !bootstrap->get_client_info) return;

    WotbModV3ClientInfo client = {};
    WOTBMOD_V3_INIT_STRUCT(client, WOTBMOD_V3_ABI_VERSION);
    Record(
        bootstrap,
        mod,
        counters,
        bootstrap->get_client_info(mod, &client) == WOTBMOD_V3_OK,
        "bootstrap.get_client_info");
}

void ProbeCoreServices(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod,
    ProbeCounters* counters) {
    const WotbModV3CoreApiV1* core =
        QueryApi<WotbModV3CoreApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_CORE,
            WOTBMOD_V3_CORE_VERSION);
    if (core) {
        uint64_t context = 0u;
        uint64_t frame = 0u;
        uint32_t role = WOTBMOD_V3_THREAD_UNKNOWN;
        char path[WOTBMOD_V3_MAX_PATH] = {};
        uint32_t path_size = sizeof(path);
        Record(
            bootstrap,
            mod,
            counters,
            core->get_context(mod, &context) == WOTBMOD_V3_OK,
            "core.get_context");
        Record(
            bootstrap,
            mod,
            counters,
            core->get_thread_role(mod, &role) == WOTBMOD_V3_OK,
            "core.get_thread_role");
        Record(
            bootstrap,
            mod,
            counters,
            core->get_frame_index(mod, &frame) == WOTBMOD_V3_OK,
            "core.get_frame_index");
        Record(
            bootstrap,
            mod,
            counters,
            core->get_game_directory(mod, path, &path_size) ==
                WOTBMOD_V3_OK,
            "core.get_game_directory");
    }

    const WotbModV3CapabilitiesApiV1* capabilities =
        QueryApi<WotbModV3CapabilitiesApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_CAPABILITIES,
            WOTBMOD_V3_CAPABILITIES_VERSION);
    if (capabilities) {
        uint32_t count = 0u;
        Record(
            bootstrap,
            mod,
            counters,
            capabilities->get_count(mod, &count) == WOTBMOD_V3_OK,
            "capabilities.get_count");
        if (count > 0u) {
            WotbModV3CapabilityInfo info = {};
            WOTBMOD_V3_INIT_STRUCT(info, WOTBMOD_V3_ABI_VERSION);
            Record(
                bootstrap,
                mod,
                counters,
                capabilities->get_at(mod, 0u, &info) ==
                    WOTBMOD_V3_OK,
                "capabilities.get_at");
        }
    }

    const WotbModV3PermissionsApiV1* permissions =
        QueryApi<WotbModV3PermissionsApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_PERMISSIONS,
            WOTBMOD_V3_PERMISSIONS_VERSION);
    if (permissions) {
        uint32_t tier = WOTBMOD_V3_PERMISSION_SAFE;
        Record(
            bootstrap,
            mod,
            counters,
            permissions->get_granted_tier(mod, &tier) ==
                WOTBMOD_V3_OK,
            "permissions.get_granted_tier");
    }

    const WotbModV3HandlesApiV1* handles =
        QueryApi<WotbModV3HandlesApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_HANDLES,
            WOTBMOD_V3_HANDLES_VERSION);
    if (handles) {
        WotbModV3HandleInfo info = {};
        WOTBMOD_V3_INIT_STRUCT(info, WOTBMOD_V3_ABI_VERSION);
        uint32_t alive = 0u;
        Record(
            bootstrap,
            mod,
            counters,
            handles->get_info(mod, mod, &info) == WOTBMOD_V3_OK,
            "handles.get_info(mod)");
        Record(
            bootstrap,
            mod,
            counters,
            handles->is_alive(mod, mod, &alive) == WOTBMOD_V3_OK &&
                alive != 0u,
            "handles.is_alive(mod)");
    }

    const WotbModV3LifecycleApiV1* lifecycle =
        QueryApi<WotbModV3LifecycleApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_LIFECYCLE,
            WOTBMOD_V3_LIFECYCLE_VERSION);
    if (lifecycle) {
        WotbModV3Handle current = WOTBMOD_V3_INVALID_HANDLE;
        WotbModV3LifecycleInfo info = {};
        WOTBMOD_V3_INIT_STRUCT(info, WOTBMOD_V3_ABI_VERSION);
        Record(
            bootstrap,
            mod,
            counters,
            lifecycle->get_current(mod, &current) == WOTBMOD_V3_OK &&
                current == mod,
            "lifecycle.get_current");
        Record(
            bootstrap,
            mod,
            counters,
            lifecycle->get_info(mod, &info) == WOTBMOD_V3_OK,
            "lifecycle.get_info");
    }
}

void ProbeRuntimeServices(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod,
    ProbeCounters* counters) {
    const WotbModV3HooksApiV1* hooks =
        QueryApi<WotbModV3HooksApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_HOOKS,
            WOTBMOD_V3_HOOKS_VERSION);
    if (hooks) {
        uint32_t count = 0u;
        Record(
            bootstrap,
            mod,
            counters,
            IsExpectedReadResult(hooks->enumerate(mod, nullptr, &count)),
            "hooks.enumerate");
    }

    const WotbModV3EventsApiV1* events =
        QueryApi<WotbModV3EventsApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_EVENTS,
            WOTBMOD_V3_EVENTS_VERSION);
    if (events) {
        static const char payload[] = "core-runtime-probe";
        Record(
            bootstrap,
            mod,
            counters,
            events->post(
                mod,
                "example.v3.core_runtime.probe",
                payload,
                static_cast<uint32_t>(sizeof(payload) - 1u),
                WOTBMOD_V3_EVENT_FLAG_NONE) == WOTBMOD_V3_OK,
            "events.post");
    }

    const WotbModV3AsyncApiV1* async =
        QueryApi<WotbModV3AsyncApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_ASYNC,
            WOTBMOD_V3_ASYNC_VERSION);
    if (async) {
        uint32_t role = WOTBMOD_V3_THREAD_UNKNOWN;
        uint32_t is_main = 0u;
        Record(
            bootstrap,
            mod,
            counters,
            async->thread_get_current_role(mod, &role) ==
                WOTBMOD_V3_OK,
            "async.thread_get_current_role");
        Record(
            bootstrap,
            mod,
            counters,
            async->thread_is_main(mod, &is_main) == WOTBMOD_V3_OK,
            "async.thread_is_main");
    }

    const WotbModV3HttpApiV1* http =
        QueryApi<WotbModV3HttpApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_HTTP,
            WOTBMOD_V3_HTTP_VERSION);
    if (http) {
        WotbModV3HttpHandle request = WOTBMOD_V3_INVALID_HANDLE;
        const WotbModV3Result result =
            http->request_create(mod, &request);
        Record(
            bootstrap,
            mod,
            counters,
            result == WOTBMOD_V3_OK ||
                result == WOTBMOD_V3_E_NOT_SUPPORTED,
            "http.request_create honest availability");
        if (result == WOTBMOD_V3_OK) {
            http->request_cancel(mod, request);
        }
    }

    const WotbModV3IntermodApiV1* intermod =
        QueryApi<WotbModV3IntermodApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_INTERMOD,
            WOTBMOD_V3_INTERMOD_VERSION);
    if (intermod) {
        WotbModV3Handle found = WOTBMOD_V3_INVALID_HANDLE;
        uint32_t loaded = 0u;
        Record(
            bootstrap,
            mod,
            counters,
            intermod->mod_find(
                mod,
                "examples.v3.core_runtime",
                &found) == WOTBMOD_V3_OK &&
                found == mod,
            "intermod.mod_find(self)");
        Record(
            bootstrap,
            mod,
            counters,
            intermod->mod_is_loaded(
                mod,
                "examples.v3.core_runtime",
                &loaded) == WOTBMOD_V3_OK &&
                loaded != 0u,
            "intermod.mod_is_loaded(self)");
    }
}

void ProbeDataServices(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod,
    ProbeCounters* counters) {
    const WotbModV3VfsApiV1* vfs =
        QueryApi<WotbModV3VfsApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_VFS,
            WOTBMOD_V3_VFS_VERSION);
    if (vfs) {
        char value[WOTBMOD_V3_MAX_PATH] = {};
        uint32_t size = sizeof(value);
        Record(
            bootstrap,
            mod,
            counters,
            vfs->get_namespace(mod, value, &size) == WOTBMOD_V3_OK,
            "vfs.get_namespace");
        size = sizeof(value);
        Record(
            bootstrap,
            mod,
            counters,
            vfs->normalize_uri(
                mod,
                "mod://examples.v3.core_runtime/",
                value,
                &size) == WOTBMOD_V3_OK,
            "vfs.normalize_uri");
    }

    const WotbModV3StorageApiV1* storage =
        QueryApi<WotbModV3StorageApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_STORAGE,
            WOTBMOD_V3_STORAGE_VERSION);
    if (storage) {
        uint32_t contains = 0u;
        char path[WOTBMOD_V3_MAX_PATH] = {};
        uint32_t path_size = sizeof(path);
        Record(
            bootstrap,
            mod,
            counters,
            storage->contains(
                mod,
                "v3_example_probe",
                &contains) == WOTBMOD_V3_OK,
            "storage.contains");
        Record(
            bootstrap,
            mod,
            counters,
            storage->get_path(
                mod,
                WOTBMOD_V3_STORAGE_PATH_DATA,
                path,
                &path_size) == WOTBMOD_V3_OK,
            "storage.get_path");
    }

    const WotbModV3SettingsApiV1* settings =
        QueryApi<WotbModV3SettingsApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_SETTINGS,
            WOTBMOD_V3_SETTINGS_VERSION);
    if (settings) {
        uint32_t schema_version = 0u;
        Record(
            bootstrap,
            mod,
            counters,
            settings->get_schema_version(mod, &schema_version) ==
                WOTBMOD_V3_OK,
            "settings.get_schema_version");
    }

    const WotbModV3InputApiV1* input =
        QueryApi<WotbModV3InputApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_INPUT,
            WOTBMOD_V3_INPUT_VERSION);
    if (input) {
        uint32_t down = 0u;
        Record(
            bootstrap,
            mod,
            counters,
            IsExpectedReadResult(
                input->is_action_down(
                    mod,
                    WOTBMOD_V3_INVALID_HANDLE,
                    &down)),
            "input invalid-handle validation");
    }

    const WotbModV3YamlApiV1* yaml =
        QueryApi<WotbModV3YamlApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_YAML,
            WOTBMOD_V3_YAML_VERSION);
    const WotbModV3HandlesApiV1* handles =
        QueryApi<WotbModV3HandlesApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_HANDLES,
            WOTBMOD_V3_HANDLES_VERSION);
    if (yaml) {
        static const char document_text[] =
            "enabled: true\ncount: 3\nname: probe\n";
        WotbModV3ConstBuffer data = {};
        WOTBMOD_V3_INIT_STRUCT(data, WOTBMOD_V3_ABI_VERSION);
        data.data = document_text;
        data.size =
            static_cast<uint32_t>(sizeof(document_text) - 1u);
        WotbModV3YamlLimits limits = {};
        WOTBMOD_V3_INIT_STRUCT(limits, WOTBMOD_V3_ABI_VERSION);
        limits.max_depth = 16u;
        limits.max_nodes = 128u;
        limits.max_bytes = sizeof(document_text);
        WotbModV3Handle document = WOTBMOD_V3_INVALID_HANDLE;
        const WotbModV3Result parse_result =
            yaml->parse(mod, &data, &limits, &document);
        Record(
            bootstrap,
            mod,
            counters,
            parse_result == WOTBMOD_V3_OK,
            "yaml.parse");
        if (parse_result == WOTBMOD_V3_OK) {
            WotbModV3YamlNodeId root =
                WOTBMOD_V3_YAML_INVALID_NODE;
            uint32_t type = WOTBMOD_V3_YAML_NULL;
            Record(
                bootstrap,
                mod,
                counters,
                yaml->get_root(mod, document, &root) ==
                        WOTBMOD_V3_OK &&
                    yaml->get_type(mod, document, root, &type) ==
                        WOTBMOD_V3_OK &&
                    type == WOTBMOD_V3_YAML_MAP,
                "yaml root inspection");
            if (handles) handles->release(mod, document);
        }
    }

    const WotbModV3LoadersApiV1* loaders =
        QueryApi<WotbModV3LoadersApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_LOADERS,
            WOTBMOD_V3_LOADERS_VERSION);
    if (loaders) {
        WotbModV3LoaderBackendInfo info = {};
        WOTBMOD_V3_INIT_STRUCT(info, WOTBMOD_V3_ABI_VERSION);
        Record(
            bootstrap,
            mod,
            counters,
            loaders->get_backend_info(
                mod,
                WOTBMOD_V3_LOADER_MINIMAL_YAML,
                &info) == WOTBMOD_V3_OK,
            "loaders.get_backend_info");
    }

    const WotbModV3ManifestApiV1* manifest =
        QueryApi<WotbModV3ManifestApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_MANIFEST,
            WOTBMOD_V3_MANIFEST_VERSION);
    if (manifest) {
        static const char manifest_json[] =
            "{\"manifest_version\":1,\"type\":\"native\","
            "\"id\":\"examples.v3.manifest_probe\","
            "\"name\":\"Manifest probe\",\"version\":\"1.0.0\","
            "\"developer\":\"WotbMod SDK\","
            "\"entrypoints\":{\"windows-x86\":\"bin/probe.dll\"},"
            "\"permissions\":[\"core\"]}";
        WotbModV3ConstBuffer data = {};
        WOTBMOD_V3_INIT_STRUCT(data, WOTBMOD_V3_ABI_VERSION);
        data.data = manifest_json;
        data.size =
            static_cast<uint32_t>(sizeof(manifest_json) - 1u);
        WotbModV3Handle parsed = WOTBMOD_V3_INVALID_HANDLE;
        const WotbModV3Result parse_result =
            manifest->parse_json(mod, &data, &parsed);
        Record(
            bootstrap,
            mod,
            counters,
            parse_result == WOTBMOD_V3_OK,
            "manifest.parse_json");
        if (parse_result == WOTBMOD_V3_OK) {
            Record(
                bootstrap,
                mod,
                counters,
                manifest->validate(mod, parsed) == WOTBMOD_V3_OK,
                "manifest.validate");
            if (handles) handles->release(mod, parsed);
        }
    }

    const WotbModV3CatalogApiV1* catalog =
        QueryApi<WotbModV3CatalogApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_CATALOG,
            WOTBMOD_V3_CATALOG_VERSION);
    if (catalog) {
        char name[64] = {};
        uint32_t size = sizeof(name);
        Record(
            bootstrap,
            mod,
            counters,
            catalog->status_name(
                mod,
                WOTBMOD_V3_CATALOG_COMMUNITY,
                name,
                &size) == WOTBMOD_V3_OK,
            "catalog.status_name");
    }

    const WotbModV3ContentApiV1* content =
        QueryApi<WotbModV3ContentApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_CONTENT,
            WOTBMOD_V3_CONTENT_VERSION);
    if (content) {
        static const char content_json[] =
            "{\"type\":\"content\",\"id\":\"examples.v3.content_probe\","
            "\"name\":\"Content probe\",\"version\":\"1.0.0\","
            "\"overrides\":{\"audio\":{\"ui/probe\":"
            "\"audio/probe.ogg\"}}}";
        WotbModV3ConstBuffer data = {};
        WOTBMOD_V3_INIT_STRUCT(data, WOTBMOD_V3_ABI_VERSION);
        data.data = content_json;
        data.size =
            static_cast<uint32_t>(sizeof(content_json) - 1u);
        WotbModV3Handle parsed = WOTBMOD_V3_INVALID_HANDLE;
        const WotbModV3Result parse_result =
            content->parse_json(mod, &data, &parsed);
        Record(
            bootstrap,
            mod,
            counters,
            parse_result == WOTBMOD_V3_OK,
            "content.parse_json");
        if (parse_result == WOTBMOD_V3_OK) {
            const WotbModV3Result apply_result =
                content->apply(mod, parsed);
            Record(
                bootstrap,
                mod,
                counters,
                apply_result == WOTBMOD_V3_OK ||
                    apply_result == WOTBMOD_V3_E_NOT_SUPPORTED,
                "content.apply honest availability");
            if (handles) handles->release(mod, parsed);
        }
    }
}

void WOTBMOD_V3_CALL OnEnable(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod) {
    ProbeCounters counters = {};
    ProbeBootstrap(bootstrap, mod, &counters);

    const InterfaceProbe probes[] = {
        {WOTBMOD_V3_IFACE_CORE,
         WOTBMOD_V3_CORE_VERSION,
         sizeof(WotbModV3CoreApiV1)},
        {WOTBMOD_V3_IFACE_CAPABILITIES,
         WOTBMOD_V3_CAPABILITIES_VERSION,
         sizeof(WotbModV3CapabilitiesApiV1)},
        {WOTBMOD_V3_IFACE_PERMISSIONS,
         WOTBMOD_V3_PERMISSIONS_VERSION,
         sizeof(WotbModV3PermissionsApiV1)},
        {WOTBMOD_V3_IFACE_HANDLES,
         WOTBMOD_V3_HANDLES_VERSION,
         sizeof(WotbModV3HandlesApiV1)},
        {WOTBMOD_V3_IFACE_LIFECYCLE,
         WOTBMOD_V3_LIFECYCLE_VERSION,
         sizeof(WotbModV3LifecycleApiV1)},
        {WOTBMOD_V3_IFACE_HOOKS,
         WOTBMOD_V3_HOOKS_VERSION,
         sizeof(WotbModV3HooksApiV1)},
        {WOTBMOD_V3_IFACE_UNSAFE_NATIVE,
         WOTBMOD_V3_UNSAFE_NATIVE_VERSION,
         sizeof(WotbModV3UnsafeNativeApiV1)},
        {WOTBMOD_V3_IFACE_EVENTS,
         WOTBMOD_V3_EVENTS_VERSION,
         sizeof(WotbModV3EventsApiV1)},
        {WOTBMOD_V3_IFACE_SETTINGS,
         WOTBMOD_V3_SETTINGS_VERSION,
         sizeof(WotbModV3SettingsApiV1)},
        {WOTBMOD_V3_IFACE_STORAGE,
         WOTBMOD_V3_STORAGE_VERSION,
         sizeof(WotbModV3StorageApiV1)},
        {WOTBMOD_V3_IFACE_INPUT,
         WOTBMOD_V3_INPUT_VERSION,
         sizeof(WotbModV3InputApiV1)},
        {WOTBMOD_V3_IFACE_VFS,
         WOTBMOD_V3_VFS_VERSION,
         sizeof(WotbModV3VfsApiV1)},
        {WOTBMOD_V3_IFACE_ASYNC,
         WOTBMOD_V3_ASYNC_VERSION,
         sizeof(WotbModV3AsyncApiV1)},
        {WOTBMOD_V3_IFACE_HTTP,
         WOTBMOD_V3_HTTP_VERSION,
         sizeof(WotbModV3HttpApiV1)},
        {WOTBMOD_V3_IFACE_INTERMOD,
         WOTBMOD_V3_INTERMOD_VERSION,
         sizeof(WotbModV3IntermodApiV1)},
        {WOTBMOD_V3_IFACE_YAML,
         WOTBMOD_V3_YAML_VERSION,
         sizeof(WotbModV3YamlApiV1)},
        {WOTBMOD_V3_IFACE_ARCHIVE,
         WOTBMOD_V3_ARCHIVE_VERSION,
         sizeof(WotbModV3ArchiveApiV1)},
        {WOTBMOD_V3_IFACE_LOADERS,
         WOTBMOD_V3_LOADERS_VERSION,
         sizeof(WotbModV3LoadersApiV1)},
        {WOTBMOD_V3_IFACE_DIAGNOSTICS,
         WOTBMOD_V3_DIAGNOSTICS_VERSION,
         sizeof(WotbModV3DiagnosticsApiV1)},
        {WOTBMOD_V3_IFACE_DEVTOOLS,
         WOTBMOD_V3_DEVTOOLS_VERSION,
         sizeof(WotbModV3DevtoolsApiV1)},
        {WOTBMOD_V3_IFACE_MANIFEST,
         WOTBMOD_V3_MANIFEST_VERSION,
         sizeof(WotbModV3ManifestApiV1)},
        {WOTBMOD_V3_IFACE_CATALOG,
         WOTBMOD_V3_CATALOG_VERSION,
         sizeof(WotbModV3CatalogApiV1)},
        {WOTBMOD_V3_IFACE_CONTENT,
         WOTBMOD_V3_CONTENT_VERSION,
         sizeof(WotbModV3ContentApiV1)}
    };
    for (const InterfaceProbe& probe : probes) {
        ProbeInterface(bootstrap, mod, probe, &counters);
    }

    ProbeCoreServices(bootstrap, mod, &counters);
    ProbeRuntimeServices(bootstrap, mod, &counters);
    ProbeDataServices(bootstrap, mod, &counters);

    const WotbModV3DiagnosticsApiV1* diagnostics =
        QueryApi<WotbModV3DiagnosticsApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_DIAGNOSTICS,
            WOTBMOD_V3_DIAGNOSTICS_VERSION);
    if (diagnostics) {
        WotbModV3DiagnosticsStats stats = {};
        WOTBMOD_V3_INIT_STRUCT(stats, WOTBMOD_V3_ABI_VERSION);
        Record(
            bootstrap,
            mod,
            &counters,
            diagnostics->get_stats(mod, &stats) == WOTBMOD_V3_OK,
            "diagnostics.get_stats");
    }

    const WotbModV3DevtoolsApiV1* devtools =
        QueryApi<WotbModV3DevtoolsApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_DEVTOOLS,
            WOTBMOD_V3_DEVTOOLS_VERSION);
    if (devtools) {
        Record(
            bootstrap,
            mod,
            &counters,
            devtools->marker(
                mod,
                "example",
                "core-runtime-finished",
                "{}") == WOTBMOD_V3_OK,
            "devtools.marker");
    }

    char summary[WOTBMOD_V3_MAX_MESSAGE] = {};
#if defined(_MSC_VER)
    sprintf_s(
        summary,
        "probe complete: passed=%u failed=%u unavailable=%u",
        counters.passed,
        counters.failed,
        counters.unavailable);
#else
    std::snprintf(
        summary,
        sizeof(summary),
        "probe complete: passed=%u failed=%u unavailable=%u",
        counters.passed,
        counters.failed,
        counters.unavailable);
#endif
    Log(
        bootstrap,
        mod,
        counters.failed == 0u
            ? WOTBMOD_V3_LOG_INFO
            : WOTBMOD_V3_LOG_ERROR,
        summary);
}

void WOTBMOD_V3_CALL OnDisable(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod) {
    Log(
        bootstrap,
        mod,
        WOTBMOD_V3_LOG_INFO,
        "core/runtime/data probe disabled");
}

}  // namespace

WOTBMOD_V3_ENTRY {
    if (!bootstrap || !out_info ||
        bootstrap->struct_size < sizeof(WotbModV3Bootstrap) ||
        bootstrap->api_version != WOTBMOD_V3_ABI_VERSION ||
        !bootstrap->query_interface ||
        !bootstrap->get_interface_info ||
        !bootstrap->get_last_error ||
        !bootstrap->get_client_info ||
        mod == WOTBMOD_V3_INVALID_HANDLE) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }

    std::memset(out_info, 0, sizeof(*out_info));
    out_info->struct_size = sizeof(*out_info);
    out_info->api_version = WOTBMOD_V3_ABI_VERSION;
    out_info->requested_permission_tier =
        WOTBMOD_V3_PERMISSION_UNSAFE;
    CopyText(
        out_info->id,
        sizeof(out_info->id),
        "examples.v3.core_runtime");
    CopyText(
        out_info->name,
        sizeof(out_info->name),
        "V3 Core Runtime Data Probe");
    CopyText(out_info->version, sizeof(out_info->version), "1.0.0");
    CopyText(
        out_info->author,
        sizeof(out_info->author),
        "WotbMod SDK");
    CopyText(
        out_info->description,
        sizeof(out_info->description),
        "Queries every core/runtime/data interface, validates every function slot, and performs safe behavioral probes.");
    out_info->on_enable = &OnEnable;
    out_info->on_disable = &OnDisable;
    return WOTBMOD_V3_OK;
}
