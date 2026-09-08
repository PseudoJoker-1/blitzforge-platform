#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../include/wotbmod/devtools_v3.h"
#include "../../include/wotbmod/diagnostics_v2.h"
#include "../../include/wotbmod/interface_ids.h"
#include "dava_native_registry.h"
#include "wotb_mod_v3_internal.h"

namespace wotbmod {
namespace v3 {
namespace {

static_assert(
    offsetof(WotbModV3DiagnosticsApiV2, crash_add_context) ==
        sizeof(WotbModV3DiagnosticsApiV1),
    "diagnostics V2 must preserve the V1 table prefix");
static_assert(
    offsetof(WotbModV3DevtoolsApiV2, inspect_ui) ==
        sizeof(WotbModV3DevtoolsApiV1),
    "devtools V2 must preserve the V1 table prefix");
static_assert(
    offsetof(WotbModV3DevtoolsApiV3, get_callback_profile) ==
        sizeof(WotbModV3DevtoolsApiV2),
    "devtools V3 must preserve the V2 table prefix");

struct CrashContextEntry {
    std::string key;
    std::string value;
};

struct Breadcrumb {
    uint64_t sequence = 0u;
    uint64_t timestamp_ticks = 0u;
    uint64_t frame_index = 0u;
    uint32_t thread_role = WOTBMOD_V3_THREAD_UNKNOWN;
    std::string category;
    std::string message;
    std::string context_json;
};

struct DevtoolsCounter {
    std::string category;
    std::string name;
    double value = 0.0;
    uint64_t updated_frame = 0u;
};

struct ModToolingState {
    std::vector<CrashContextEntry> crash_context;
    std::vector<Breadcrumb> breadcrumbs;
    std::vector<DevtoolsCounter> counters;
    std::string last_action;
    uint64_t next_breadcrumb_sequence = 1u;
    uint64_t completed_spans = 0u;
    uint64_t cpu_measured_spans = 0u;
    uint64_t total_cpu_100ns = 0u;
    uint64_t active_span_bytes = 0u;
    uint64_t peak_owned_bytes = 0u;
    uint32_t active_spans = 0u;
    uint32_t reported_faults = 0u;
};

const size_t kMaximumDevtoolsCounters = 128u;

struct ProfilerSpan {
    WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
    LARGE_INTEGER start = {};
    FILETIME start_kernel = {};
    FILETIME start_user = {};
    DWORD thread_id = 0u;
    uint64_t accounted_bytes = 0u;
    bool cpu_start_valid = false;
    bool tracked = false;
    bool completed = false;
    std::string category;
    std::string name;
};

std::mutex g_tooling_mutex;
std::unordered_map<
    WotbModV3Handle,
    std::unique_ptr<ModToolingState>> g_mod_tooling;

void CopyString(char* destination, size_t capacity, const char* source) {
    if (!destination || capacity == 0u) return;
    strncpy_s(destination, capacity, source ? source : "", _TRUNCATE);
}

bool IsBoundedText(
    const char* value,
    size_t capacity,
    bool allow_empty) {
    if (!value || capacity == 0u) return false;
    const size_t length = strnlen_s(value, capacity);
    return length < capacity && (allow_empty || length != 0u);
}

void AssignBounded(
    std::string* destination,
    const char* source,
    size_t maximum_length) {
    if (!destination) return;
    const char* value = source ? source : "";
    destination->assign(value, strnlen_s(value, maximum_length));
}

uint64_t AddSaturated(uint64_t left, uint64_t right) {
    return UINT64_MAX - left < right ? UINT64_MAX : left + right;
}

uint64_t StringAccountedBytes(const std::string& value) {
    return static_cast<uint64_t>(value.capacity()) + 1u;
}

uint64_t RetainedDiagnosticsBytes(const ModToolingState& state) {
    uint64_t bytes = sizeof(ModToolingState);
    bytes = AddSaturated(
        bytes,
        static_cast<uint64_t>(state.crash_context.capacity()) *
            sizeof(CrashContextEntry));
    bytes = AddSaturated(
        bytes,
        static_cast<uint64_t>(state.breadcrumbs.capacity()) *
            sizeof(Breadcrumb));
    bytes = AddSaturated(
        bytes,
        static_cast<uint64_t>(state.counters.capacity()) *
            sizeof(DevtoolsCounter));
    bytes = AddSaturated(bytes, StringAccountedBytes(state.last_action));
    for (const CrashContextEntry& entry : state.crash_context) {
        bytes = AddSaturated(bytes, StringAccountedBytes(entry.key));
        bytes = AddSaturated(bytes, StringAccountedBytes(entry.value));
    }
    for (const Breadcrumb& breadcrumb : state.breadcrumbs) {
        bytes = AddSaturated(
            bytes,
            StringAccountedBytes(breadcrumb.category));
        bytes = AddSaturated(
            bytes,
            StringAccountedBytes(breadcrumb.message));
        bytes = AddSaturated(
            bytes,
            StringAccountedBytes(breadcrumb.context_json));
    }
    for (const DevtoolsCounter& counter : state.counters) {
        bytes = AddSaturated(
            bytes,
            StringAccountedBytes(counter.category));
        bytes = AddSaturated(
            bytes,
            StringAccountedBytes(counter.name));
    }
    return bytes;
}

uint64_t TotalOwnedBytes(const ModToolingState& state) {
    return AddSaturated(
        RetainedDiagnosticsBytes(state),
        state.active_span_bytes);
}

void UpdatePeakLocked(ModToolingState* state) {
    if (!state) return;
    state->peak_owned_bytes =
        std::max(state->peak_owned_bytes, TotalOwnedBytes(*state));
}

ModToolingState* FindStateLocked(WotbModV3Handle mod) {
    const auto iterator = g_mod_tooling.find(mod);
    return iterator == g_mod_tooling.end()
        ? nullptr
        : iterator->second.get();
}

ModToolingState* GetOrCreateStateLocked(WotbModV3Handle mod) {
    ModToolingState* state = FindStateLocked(mod);
    if (state) return state;
    std::unique_ptr<ModToolingState> created =
        std::make_unique<ModToolingState>();
    created->crash_context.reserve(
        WOTBMOD_V3_DIAGNOSTICS_MAX_CONTEXT_ENTRIES);
    created->breadcrumbs.reserve(
        WOTBMOD_V3_DIAGNOSTICS_MAX_BREADCRUMBS);
    created->counters.reserve(kMaximumDevtoolsCounters);
    state = created.get();
    g_mod_tooling.emplace(mod, std::move(created));
    UpdatePeakLocked(state);
    return state;
}

uint64_t ProfilerSpanAccountedBytes(const ProfilerSpan& span) {
    uint64_t bytes = sizeof(ProfilerSpan);
    bytes = AddSaturated(bytes, StringAccountedBytes(span.category));
    bytes = AddSaturated(bytes, StringAccountedBytes(span.name));
    return bytes;
}

void DestroyProfilerSpan(void* object) {
    ProfilerSpan* span = static_cast<ProfilerSpan*>(object);
    if (!span) return;
    if (span->tracked) {
        std::lock_guard<std::mutex> lock(g_tooling_mutex);
        ModToolingState* state = FindStateLocked(span->owner);
        if (state) {
            if (state->active_spans != 0u) --state->active_spans;
            state->active_span_bytes =
                state->active_span_bytes >= span->accounted_bytes
                    ? state->active_span_bytes - span->accounted_bytes
                    : 0u;
        }
    }
    delete span;
}

uint64_t FileTimeValue(const FILETIME& value) {
    ULARGE_INTEGER converted = {};
    converted.LowPart = value.dwLowDateTime;
    converted.HighPart = value.dwHighDateTime;
    return converted.QuadPart;
}

bool ReadCurrentThreadCpuTimes(
    FILETIME* out_kernel,
    FILETIME* out_user) {
    if (!out_kernel || !out_user) return false;
    FILETIME creation = {};
    FILETIME exit = {};
    return GetThreadTimes(
               GetCurrentThread(),
               &creation,
               &exit,
               out_kernel,
               out_user) != FALSE;
}

void AppendBreadcrumbLocked(
    ModToolingState* state,
    const char* category,
    const char* message,
    const char* context_json) {
    if (!state) return;
    if (state->breadcrumbs.size() ==
        WOTBMOD_V3_DIAGNOSTICS_MAX_BREADCRUMBS) {
        state->breadcrumbs.erase(state->breadcrumbs.begin());
    }
    Breadcrumb breadcrumb;
    breadcrumb.sequence = state->next_breadcrumb_sequence++;
    LARGE_INTEGER timestamp = {};
    QueryPerformanceCounter(&timestamp);
    breadcrumb.timestamp_ticks =
        static_cast<uint64_t>(timestamp.QuadPart);
    breadcrumb.frame_index = CurrentFrameIndex();
    breadcrumb.thread_role = CurrentThreadRole();
    AssignBounded(
        &breadcrumb.category,
        category && category[0] ? category : "mod",
        WOTBMOD_V3_DIAGNOSTICS_MAX_BREADCRUMB_CATEGORY - 1u);
    AssignBounded(
        &breadcrumb.message,
        message,
        WOTBMOD_V3_DIAGNOSTICS_MAX_BREADCRUMB_MESSAGE - 1u);
    AssignBounded(
        &breadcrumb.context_json,
        context_json,
        WOTBMOD_V3_DIAGNOSTICS_MAX_BREADCRUMB_CONTEXT - 1u);
    state->breadcrumbs.push_back(std::move(breadcrumb));
    UpdatePeakLocked(state);
}

void TryRecordBreadcrumb(
    WotbModV3Handle mod,
    const char* category,
    const char* message,
    const char* context_json) {
    try {
        std::lock_guard<std::mutex> lock(g_tooling_mutex);
        AppendBreadcrumbLocked(
            GetOrCreateStateLocked(mod),
            category,
            message,
            context_json);
    } catch (...) {
        RuntimeLog(
            WOTBMOD_V3_LOG_WARNING,
            "diagnostics",
            "unable to retain a diagnostic breadcrumb");
    }
}

void AppendJsonString(std::string* output, const char* value) {
    if (!output) return;
    output->push_back('"');
    const unsigned char* cursor = reinterpret_cast<const unsigned char*>(
        value ? value : "");
    while (*cursor != 0u) {
        switch (*cursor) {
            case '"':
                output->append("\\\"");
                break;
            case '\\':
                output->append("\\\\");
                break;
            case '\b':
                output->append("\\b");
                break;
            case '\f':
                output->append("\\f");
                break;
            case '\n':
                output->append("\\n");
                break;
            case '\r':
                output->append("\\r");
                break;
            case '\t':
                output->append("\\t");
                break;
            default:
                if (*cursor < 0x20u) {
                    char escaped[7] = {};
                    _snprintf_s(
                        escaped,
                        sizeof(escaped),
                        _TRUNCATE,
                        "\\u%04x",
                        static_cast<unsigned int>(*cursor));
                    output->append(escaped);
                } else {
                    output->push_back(static_cast<char>(*cursor));
                }
                break;
        }
        ++cursor;
    }
    output->push_back('"');
}

void AppendUnsigned(std::string* output, uint64_t value) {
    char number[32] = {};
    _snprintf_s(
        number,
        sizeof(number),
        _TRUNCATE,
        "%llu",
        static_cast<unsigned long long>(value));
    output->append(number);
}

void AppendSigned(std::string* output, int64_t value) {
    char number[32] = {};
    _snprintf_s(
        number,
        sizeof(number),
        _TRUNCATE,
        "%lld",
        static_cast<long long>(value));
    output->append(number);
}

void AppendBoolean(std::string* output, bool value) {
    output->append(value ? "true" : "false");
}

void AppendDouble(std::string* output, double value) {
    char number[64] = {};
    _snprintf_s(
        number,
        sizeof(number),
        _TRUNCATE,
        "%.9f",
        value);
    output->append(number);
}

ModToolingState CopyState(WotbModV3Handle mod, bool* out_exists) {
    std::lock_guard<std::mutex> lock(g_tooling_mutex);
    ModToolingState* state = FindStateLocked(mod);
    if (out_exists) *out_exists = state != nullptr;
    return state ? *state : ModToolingState();
}

WotbModV3Result BuildReportJson(
    WotbModV3Handle mod,
    std::string* out_json) {
    if (!out_json) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "diagnostics report output is required");
    }
    if (CheckMod(mod) != WOTBMOD_V3_OK) {
        return WOTBMOD_V3_E_INVALID_HANDLE;
    }
    try {
        RuntimeStats stats = {};
        GetRuntimeStats(&stats);
        bool state_exists = false;
        const ModToolingState state = CopyState(mod, &state_exists);
        char version[WOTBMOD_V3_MAX_VERSION] = {};
        if (GetModVersion(mod, version, sizeof(version)) !=
            WOTBMOD_V3_OK) {
            version[0] = '\0';
        }
        const std::string mod_id = ModId(mod);
        const uint64_t retained_bytes =
            state_exists ? RetainedDiagnosticsBytes(state) : 0u;
        const uint64_t total_owned_bytes = state_exists
            ? AddSaturated(retained_bytes, state.active_span_bytes)
            : 0u;

        std::string json;
        json.reserve(4096u);
        json.append("{\"schema\":\"wotbmod.diagnostics/v2\",\"mod\":");
        AppendJsonString(&json, mod_id.c_str());
        json.append(",\"mod_version\":");
        AppendJsonString(&json, version);
        json.append(",\"runtime_state\":");
        AppendUnsigned(&json, ModState(mod));
        json.append(",\"enabled\":");
        AppendUnsigned(&json, IsModEnabled(mod) ? 1u : 0u);
        json.append(",\"permission_tier\":");
        AppendUnsigned(&json, ModPermissionTier(mod));
        json.append(",\"loaded_mods\":");
        AppendUnsigned(&json, stats.loaded_mods);
        json.append(",\"enabled_mods\":");
        AppendUnsigned(&json, stats.enabled_mods);
        json.append(",\"live_handles\":");
        AppendUnsigned(&json, stats.live_handles);
        json.append(",\"interfaces\":");
        AppendUnsigned(&json, stats.registered_interfaces);
        json.append(",\"capabilities\":");
        AppendUnsigned(&json, stats.registered_capabilities);
        json.append(",\"frame\":");
        AppendUnsigned(&json, stats.frame_index);
        json.append(",\"context_mask\":");
        AppendUnsigned(&json, stats.context_mask);
        json.append(",\"reported_faults\":");
        AppendUnsigned(&json, state.reported_faults);
        json.append(",\"last_action\":");
        AppendJsonString(&json, state.last_action.c_str());
        json.append(",\"crash_context\":{");
        for (size_t index = 0u;
             index < state.crash_context.size();
             ++index) {
            if (index != 0u) json.push_back(',');
            AppendJsonString(
                &json,
                state.crash_context[index].key.c_str());
            json.push_back(':');
            AppendJsonString(
                &json,
                state.crash_context[index].value.c_str());
        }
        json.append("},\"breadcrumbs\":[");
        for (size_t index = 0u;
             index < state.breadcrumbs.size();
             ++index) {
            if (index != 0u) json.push_back(',');
            const Breadcrumb& breadcrumb = state.breadcrumbs[index];
            json.append("{\"sequence\":");
            AppendUnsigned(&json, breadcrumb.sequence);
            json.append(",\"timestamp_ticks\":");
            AppendUnsigned(&json, breadcrumb.timestamp_ticks);
            json.append(",\"frame\":");
            AppendUnsigned(&json, breadcrumb.frame_index);
            json.append(",\"thread_role\":");
            AppendUnsigned(&json, breadcrumb.thread_role);
            json.append(",\"category\":");
            AppendJsonString(&json, breadcrumb.category.c_str());
            json.append(",\"message\":");
            AppendJsonString(&json, breadcrumb.message.c_str());
            json.append(",\"context_json\":");
            AppendJsonString(&json, breadcrumb.context_json.c_str());
            json.push_back('}');
        }
        json.append("],\"profiler\":{\"active_spans\":");
        AppendUnsigned(&json, state.active_spans);
        json.append(",\"completed_spans\":");
        AppendUnsigned(&json, state.completed_spans);
        json.append(",\"cpu_measured_spans\":");
        AppendUnsigned(&json, state.cpu_measured_spans);
        json.append(",\"cpu_100ns\":");
        AppendUnsigned(&json, state.total_cpu_100ns);
        json.append(",\"cpu_milliseconds\":");
        AppendDouble(
            &json,
            static_cast<double>(state.total_cpu_100ns) / 10000.0);
        json.append("},\"counters\":[");
        for (size_t index = 0u; index < state.counters.size(); ++index) {
            if (index != 0u) json.push_back(',');
            const DevtoolsCounter& counter = state.counters[index];
            json.append("{\"category\":");
            AppendJsonString(&json, counter.category.c_str());
            json.append(",\"name\":");
            AppendJsonString(&json, counter.name.c_str());
            json.append(",\"value\":");
            AppendDouble(&json, counter.value);
            json.append(",\"updated_frame\":");
            AppendUnsigned(&json, counter.updated_frame);
            json.push_back('}');
        }
        json.append("],\"memory\":{\"retained_bytes\":");
        AppendUnsigned(&json, retained_bytes);
        json.append(",\"active_span_bytes\":");
        AppendUnsigned(&json, state.active_span_bytes);
        json.append(",\"total_owned_bytes\":");
        AppendUnsigned(&json, total_owned_bytes);
        json.append(",\"peak_owned_bytes\":");
        AppendUnsigned(&json, state.peak_owned_bytes);
        json.append("}}");
        *out_json = std::move(json);
        return WOTBMOD_V3_OK;
    } catch (const std::bad_alloc&) {
        return SetError(
            mod,
            WOTBMOD_V3_E_PLATFORM,
            "unable to allocate diagnostics report memory");
    }
}

WotbModV3Result CopyDynamicString(
    WotbModV3Handle mod,
    const std::string& value,
    char* buffer,
    uint32_t* inout_size,
    const char* small_buffer_message) {
    if (!inout_size) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "output size pointer is required");
    }
    if (value.size() >= UINT32_MAX) {
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "output exceeds the public ABI size limit");
    }
    const uint32_t required =
        static_cast<uint32_t>(value.size() + 1u);
    const uint32_t capacity = *inout_size;
    *inout_size = required;
    if (!buffer || capacity < required) {
        return SetError(
            mod,
            WOTBMOD_V3_E_BUFFER_TOO_SMALL,
            small_buffer_message);
    }
    memcpy(buffer, value.c_str(), required);
    return WOTBMOD_V3_OK;
}

WotbModV3Result DiagnosticsGetStats(
    WotbModV3Handle mod,
    WotbModV3DiagnosticsStats* out_stats) {
    if (!out_stats ||
        out_stats->struct_size < sizeof(WotbModV3StructHeader)) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "diagnostics output is required");
    }
    if (CheckMod(mod) != WOTBMOD_V3_OK) {
        return WOTBMOD_V3_E_INVALID_HANDLE;
    }
    RuntimeStats stats = {};
    GetRuntimeStats(&stats);
    WotbModV3DiagnosticsStats value = {};
    value.struct_size = sizeof(value);
    value.api_version = WOTBMOD_V3_DIAGNOSTICS_VERSION;
    value.loaded_mods = stats.loaded_mods;
    value.enabled_mods = stats.enabled_mods;
    value.live_handles = stats.live_handles;
    value.registered_interfaces = stats.registered_interfaces;
    value.registered_capabilities = stats.registered_capabilities;
    value.frame_index = stats.frame_index;
    value.context_mask = stats.context_mask;
    const size_t copy_size =
        std::min<size_t>(out_stats->struct_size, sizeof(value));
    memcpy(out_stats, &value, copy_size);
    out_stats->struct_size = static_cast<uint32_t>(copy_size);
    return WOTBMOD_V3_OK;
}

WotbModV3Result DiagnosticsCopyReport(
    WotbModV3Handle mod,
    char* buffer,
    uint32_t* inout_size) {
    if (!inout_size) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "report size pointer is required");
    }
    std::string report;
    const WotbModV3Result result = BuildReportJson(mod, &report);
    if (result != WOTBMOD_V3_OK) return result;
    return CopyDynamicString(
        mod,
        report,
        buffer,
        inout_size,
        "diagnostics report buffer is too small");
}

WotbModV3Result DiagnosticsReportFault(
    WotbModV3Handle mod,
    const char* category,
    const char* message,
    const char* context_json) {
    if (!message || !message[0]) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "fault message is required");
    }
    if (CheckMod(mod) != WOTBMOD_V3_OK) {
        return WOTBMOD_V3_E_INVALID_HANDLE;
    }
    try {
        std::lock_guard<std::mutex> lock(g_tooling_mutex);
        ModToolingState* state = GetOrCreateStateLocked(mod);
        if (state->reported_faults != UINT32_MAX) {
            ++state->reported_faults;
        }
        AppendBreadcrumbLocked(
            state,
            category && category[0] ? category : "mod.fault",
            message,
            context_json);
    } catch (...) {
        RuntimeLog(
            WOTBMOD_V3_LOG_WARNING,
            "diagnostics",
            "unable to retain fault diagnostics");
    }
    char line[WOTBMOD_V3_MAX_MESSAGE + WOTBMOD_V3_MAX_NAME + 64u] = {};
    _snprintf_s(
        line,
        sizeof(line),
        _TRUNCATE,
        "[%s] %s%s%s",
        ModId(mod),
        message,
        context_json && context_json[0] ? " context=" : "",
        context_json && context_json[0] ? context_json : "");
    RuntimeLog(
        WOTBMOD_V3_LOG_ERROR,
        category && category[0] ? category : "mod.fault",
        line);
    return WOTBMOD_V3_OK;
}

WotbModV3Result DiagnosticsCrashAddContext(
    WotbModV3Handle mod,
    const char* key,
    const char* value) {
    if (!IsBoundedText(
            key,
            WOTBMOD_V3_DIAGNOSTICS_MAX_CONTEXT_KEY,
            false) ||
        !IsBoundedText(
            value,
            WOTBMOD_V3_DIAGNOSTICS_MAX_CONTEXT_VALUE,
            true)) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "bounded crash context key and value are required");
    }
    if (CheckMod(mod) != WOTBMOD_V3_OK) {
        return WOTBMOD_V3_E_INVALID_HANDLE;
    }
    try {
        std::lock_guard<std::mutex> lock(g_tooling_mutex);
        ModToolingState* state = GetOrCreateStateLocked(mod);
        for (CrashContextEntry& entry : state->crash_context) {
            if (entry.key == key) {
                entry.value = value;
                UpdatePeakLocked(state);
                return WOTBMOD_V3_OK;
            }
        }
        if (state->crash_context.size() ==
            WOTBMOD_V3_DIAGNOSTICS_MAX_CONTEXT_ENTRIES) {
            return SetError(
                mod,
                WOTBMOD_V3_E_LIMIT_REACHED,
                "crash context entry limit reached");
        }
        CrashContextEntry entry;
        entry.key = key;
        entry.value = value;
        state->crash_context.push_back(std::move(entry));
        UpdatePeakLocked(state);
        return WOTBMOD_V3_OK;
    } catch (const std::bad_alloc&) {
        return SetError(
            mod,
            WOTBMOD_V3_E_PLATFORM,
            "unable to allocate crash context storage");
    }
}

WotbModV3Result DiagnosticsCrashSetLastAction(
    WotbModV3Handle mod,
    const char* action) {
    if (!IsBoundedText(
            action,
            WOTBMOD_V3_DIAGNOSTICS_MAX_ACTION,
            true)) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "a bounded last action string is required");
    }
    if (CheckMod(mod) != WOTBMOD_V3_OK) {
        return WOTBMOD_V3_E_INVALID_HANDLE;
    }
    try {
        std::lock_guard<std::mutex> lock(g_tooling_mutex);
        ModToolingState* state = GetOrCreateStateLocked(mod);
        state->last_action = action;
        UpdatePeakLocked(state);
        return WOTBMOD_V3_OK;
    } catch (const std::bad_alloc&) {
        return SetError(
            mod,
            WOTBMOD_V3_E_PLATFORM,
            "unable to allocate last action storage");
    }
}

WotbModV3Result DiagnosticsCrashAddBreadcrumb(
    WotbModV3Handle mod,
    const char* category,
    const char* message,
    const char* context_json) {
    const char* effective_category =
        category && category[0] ? category : "mod";
    const char* effective_context = context_json ? context_json : "";
    if (!IsBoundedText(
            effective_category,
            WOTBMOD_V3_DIAGNOSTICS_MAX_BREADCRUMB_CATEGORY,
            false) ||
        !IsBoundedText(
            message,
            WOTBMOD_V3_DIAGNOSTICS_MAX_BREADCRUMB_MESSAGE,
            false) ||
        !IsBoundedText(
            effective_context,
            WOTBMOD_V3_DIAGNOSTICS_MAX_BREADCRUMB_CONTEXT,
            true)) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "bounded breadcrumb fields are required");
    }
    if (CheckMod(mod) != WOTBMOD_V3_OK) {
        return WOTBMOD_V3_E_INVALID_HANDLE;
    }
    try {
        std::lock_guard<std::mutex> lock(g_tooling_mutex);
        AppendBreadcrumbLocked(
            GetOrCreateStateLocked(mod),
            effective_category,
            message,
            effective_context);
        return WOTBMOD_V3_OK;
    } catch (const std::bad_alloc&) {
        return SetError(
            mod,
            WOTBMOD_V3_E_PLATFORM,
            "unable to allocate breadcrumb storage");
    }
}

bool IsPortableBundleName(const char* name) {
    if (!IsBoundedText(name, WOTBMOD_V3_MAX_NAME, false)) return false;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return false;
    for (const unsigned char* cursor =
             reinterpret_cast<const unsigned char*>(name);
         *cursor != 0u;
         ++cursor) {
        if (std::isalnum(*cursor) != 0 || *cursor == '.' ||
            *cursor == '-' || *cursor == '_') {
            continue;
        }
        return false;
    }
    const size_t length = strlen(name);
    return length >= 5u &&
        _stricmp(name + length - 5u, ".json") == 0;
}

std::string JoinPath(
    const std::string& left,
    const std::string& right) {
    if (left.empty()) return right;
    const char tail = left.back();
    return tail == '\\' || tail == '/'
        ? left + right
        : left + "\\" + right;
}

bool IsSafeDirectory(const std::string& path) {
    const DWORD attributes = GetFileAttributesA(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0u &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0u;
}

WotbModV3Result WriteBundleFile(
    WotbModV3Handle mod,
    const std::string& output_path,
    const std::string& diagnostics_directory,
    const std::string& report) {
    LARGE_INTEGER counter = {};
    QueryPerformanceCounter(&counter);
    std::string temporary_path;
    HANDLE file = INVALID_HANDLE_VALUE;
    for (uint32_t attempt = 0u; attempt < 8u; ++attempt) {
        char temporary_name[128] = {};
        _snprintf_s(
            temporary_name,
            sizeof(temporary_name),
            _TRUNCATE,
            ".wotbmod-diagnostics-%lu-%llu-%u.tmp",
            static_cast<unsigned long>(GetCurrentProcessId()),
            static_cast<unsigned long long>(counter.QuadPart),
            attempt);
        temporary_path = JoinPath(
            diagnostics_directory,
            temporary_name);
        file = CreateFileA(
            temporary_path.c_str(),
            GENERIC_WRITE,
            0u,
            nullptr,
            CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (file != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_FILE_EXISTS) {
            return SetError(
                mod,
                WOTBMOD_V3_E_IO,
                "unable to create diagnostics bundle");
        }
    }
    if (file == INVALID_HANDLE_VALUE) {
        return SetError(
            mod,
            WOTBMOD_V3_E_BUSY,
            "unable to reserve a diagnostics bundle file");
    }

    bool write_ok = true;
    size_t offset = 0u;
    while (offset < report.size()) {
        const size_t remaining = report.size() - offset;
        const DWORD chunk = static_cast<DWORD>(
            std::min<size_t>(remaining, 0x7FFFFFFFu));
        DWORD written = 0u;
        if (WriteFile(
                file,
                report.data() + offset,
                chunk,
                &written,
                nullptr) == FALSE ||
            written == 0u) {
            write_ok = false;
            break;
        }
        offset += written;
    }
    if (write_ok && FlushFileBuffers(file) == FALSE) write_ok = false;
    CloseHandle(file);
    if (!write_ok ||
        MoveFileExA(
            temporary_path.c_str(),
            output_path.c_str(),
            MOVEFILE_REPLACE_EXISTING |
                MOVEFILE_WRITE_THROUGH) == FALSE) {
        DeleteFileA(temporary_path.c_str());
        return SetError(
            mod,
            WOTBMOD_V3_E_IO,
            "unable to commit diagnostics bundle");
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result DiagnosticsExportBundle(
    WotbModV3Handle mod,
    const char* bundle_name,
    char* out_path,
    uint32_t* inout_path_size) {
    if (!inout_path_size || !IsPortableBundleName(bundle_name)) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "a portable JSON bundle filename and path size are required");
    }
    if (CheckMod(mod) != WOTBMOD_V3_OK) {
        return WOTBMOD_V3_E_INVALID_HANDLE;
    }
    char data_directory[WOTBMOD_V3_MAX_PATH] = {};
    uint32_t data_directory_size = sizeof(data_directory);
    WotbModV3Result result = GetModDataDirectory(
        mod,
        data_directory,
        &data_directory_size);
    if (result != WOTBMOD_V3_OK) return result;
    const std::string root(data_directory);
    if (!IsSafeDirectory(root)) {
        return SetError(
            mod,
            WOTBMOD_V3_E_IO,
            "mod data root is missing or is a reparse point");
    }
    const std::string diagnostics_directory =
        JoinPath(root, "diagnostics");
    if (CreateDirectoryA(
            diagnostics_directory.c_str(),
            nullptr) == FALSE &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        return SetError(
            mod,
            WOTBMOD_V3_E_IO,
            "unable to create the diagnostics directory");
    }
    if (!IsSafeDirectory(diagnostics_directory)) {
        return SetError(
            mod,
            WOTBMOD_V3_E_IO,
            "diagnostics directory is not a safe local directory");
    }
    const std::string output_path =
        JoinPath(diagnostics_directory, bundle_name);
    if (output_path.size() >= WOTBMOD_V3_MAX_PATH) {
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "diagnostics bundle path exceeds the ABI limit");
    }
    const uint32_t required =
        static_cast<uint32_t>(output_path.size() + 1u);
    const uint32_t capacity = *inout_path_size;
    *inout_path_size = required;
    if (!out_path || capacity < required) {
        return SetError(
            mod,
            WOTBMOD_V3_E_BUFFER_TOO_SMALL,
            "diagnostics bundle path buffer is too small");
    }
    std::string report;
    result = BuildReportJson(mod, &report);
    if (result != WOTBMOD_V3_OK) return result;
    result = WriteBundleFile(
        mod,
        output_path,
        diagnostics_directory,
        report);
    if (result != WOTBMOD_V3_OK) return result;
    memcpy(out_path, output_path.c_str(), required);
    return WOTBMOD_V3_OK;
}

WotbModV3Result DiagnosticsGetModHealth(
    WotbModV3Handle mod,
    WotbModV3ModHealth* out_health) {
    if (!out_health ||
        out_health->struct_size < sizeof(WotbModV3StructHeader)) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "mod health output is required");
    }
    if (CheckMod(mod) != WOTBMOD_V3_OK) {
        return WOTBMOD_V3_E_INVALID_HANDLE;
    }
    bool state_exists = false;
    ModToolingState state;
    try {
        state = CopyState(mod, &state_exists);
    } catch (const std::bad_alloc&) {
        return SetError(
            mod,
            WOTBMOD_V3_E_PLATFORM,
            "unable to snapshot mod health");
    }
    WotbModV3ModHealth value = {};
    value.struct_size = sizeof(value);
    value.api_version = WOTBMOD_V3_DIAGNOSTICS_VERSION_2;
    value.runtime_state = ModState(mod);
    value.enabled = IsModEnabled(mod) ? 1u : 0u;
    value.permission_tier = ModPermissionTier(mod);
    value.reported_faults = state.reported_faults;
    value.crash_context_entries =
        static_cast<uint32_t>(state.crash_context.size());
    value.retained_breadcrumbs =
        static_cast<uint32_t>(state.breadcrumbs.size());
    value.active_profiler_spans = state.active_spans;
    value.completed_profiler_spans = state.completed_spans;
    value.cpu_measured_profiler_spans = state.cpu_measured_spans;
    value.profiler_cpu_100ns = state.total_cpu_100ns;
    value.profiler_cpu_milliseconds =
        static_cast<double>(state.total_cpu_100ns) / 10000.0;
    if (state_exists) {
        value.diagnostic_owned_bytes = TotalOwnedBytes(state);
        value.peak_diagnostic_owned_bytes = state.peak_owned_bytes;
    }
    CopyString(
        value.last_action,
        sizeof(value.last_action),
        state.last_action.c_str());
    const size_t copy_size =
        std::min<size_t>(out_health->struct_size, sizeof(value));
    memcpy(out_health, &value, copy_size);
    out_health->struct_size = static_cast<uint32_t>(copy_size);
    return WOTBMOD_V3_OK;
}

WotbModV3Result DevtoolsMarker(
    WotbModV3Handle mod,
    const char* category,
    const char* name,
    const char* context_json) {
    if (!name || !name[0]) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "marker name is required");
    }
    if (CheckMod(mod) != WOTBMOD_V3_OK) {
        return WOTBMOD_V3_E_INVALID_HANDLE;
    }
    TryRecordBreadcrumb(
        mod,
        category && category[0] ? category : "devtools",
        name,
        context_json);
    char line[WOTBMOD_V3_MAX_MESSAGE] = {};
    _snprintf_s(
        line,
        sizeof(line),
        _TRUNCATE,
        "marker mod=%s name=%s%s%s",
        ModId(mod),
        name,
        context_json && context_json[0] ? " context=" : "",
        context_json && context_json[0] ? context_json : "");
    RuntimeLog(
        WOTBMOD_V3_LOG_DEBUG,
        category && category[0] ? category : "devtools",
        line);
    return WOTBMOD_V3_OK;
}

WotbModV3Result DevtoolsSpanBegin(
    WotbModV3Handle mod,
    const char* category,
    const char* name,
    WotbModV3Handle* out_span) {
    if (!name || !name[0] || !out_span) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "span name and output are required");
    }
    *out_span = WOTBMOD_V3_INVALID_HANDLE;
    if (CheckMod(mod) != WOTBMOD_V3_OK) {
        return WOTBMOD_V3_E_INVALID_HANDLE;
    }
    ProfilerSpan* span = new (std::nothrow) ProfilerSpan();
    if (!span) {
        return SetError(
            mod,
            WOTBMOD_V3_E_PLATFORM,
            "unable to allocate profiler span");
    }
    try {
        span->owner = mod;
        span->category =
            category && category[0] ? category : "mod";
        span->name = name;
        span->thread_id = GetCurrentThreadId();
        span->cpu_start_valid = ReadCurrentThreadCpuTimes(
            &span->start_kernel,
            &span->start_user);
        QueryPerformanceCounter(&span->start);
        span->accounted_bytes = ProfilerSpanAccountedBytes(*span);
        {
            std::lock_guard<std::mutex> lock(g_tooling_mutex);
            ModToolingState* state = GetOrCreateStateLocked(mod);
            state->active_span_bytes = AddSaturated(
                state->active_span_bytes,
                span->accounted_bytes);
            if (state->active_spans != UINT32_MAX) {
                ++state->active_spans;
            }
            span->tracked = true;
            UpdatePeakLocked(state);
        }
    } catch (const std::bad_alloc&) {
        delete span;
        return SetError(
            mod,
            WOTBMOD_V3_E_PLATFORM,
            "unable to allocate profiler span metadata");
    }
    const WotbModV3Result result = CreateOwnedHandle(
        mod,
        WOTBMOD_V3_HANDLE_PROFILER_SPAN,
        span,
        DestroyProfilerSpan,
        out_span);
    if (result != WOTBMOD_V3_OK) DestroyProfilerSpan(span);
    return result;
}

WotbModV3Result DevtoolsSpanEnd(
    WotbModV3Handle mod,
    WotbModV3Handle span_handle,
    WotbModV3ProfilerSpanInfo* out_info) {
    if (!out_info ||
        out_info->struct_size < sizeof(WotbModV3StructHeader)) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "span output is required");
    }
    void* object = nullptr;
    WotbModV3Result result = InspectOwnedHandle(
        mod,
        span_handle,
        WOTBMOD_V3_HANDLE_PROFILER_SPAN,
        &object,
        nullptr);
    if (result != WOTBMOD_V3_OK) return result;
    ProfilerSpan* span = static_cast<ProfilerSpan*>(object);
    LARGE_INTEGER now = {};
    LARGE_INTEGER frequency = {};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    FILETIME end_kernel = {};
    FILETIME end_user = {};
    const bool cpu_end_valid =
        span->thread_id == GetCurrentThreadId() &&
        ReadCurrentThreadCpuTimes(&end_kernel, &end_user);
    const uint64_t elapsed = static_cast<uint64_t>(
        now.QuadPart - span->start.QuadPart);
    uint64_t cpu_100ns = 0u;
    bool cpu_measured = false;
    if (span->cpu_start_valid && cpu_end_valid) {
        const uint64_t start_cpu =
            AddSaturated(
                FileTimeValue(span->start_kernel),
                FileTimeValue(span->start_user));
        const uint64_t end_cpu =
            AddSaturated(
                FileTimeValue(end_kernel),
                FileTimeValue(end_user));
        if (end_cpu >= start_cpu) {
            cpu_100ns = end_cpu - start_cpu;
            cpu_measured = true;
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_tooling_mutex);
        if (span->completed) {
            return SetError(
                mod,
                WOTBMOD_V3_E_CONFLICT,
                "profiler span has already completed");
        }
        span->completed = true;
        ModToolingState* state = FindStateLocked(mod);
        if (state) {
            state->completed_spans = AddSaturated(
                state->completed_spans,
                1u);
            if (cpu_measured) {
                state->cpu_measured_spans = AddSaturated(
                    state->cpu_measured_spans,
                    1u);
                state->total_cpu_100ns = AddSaturated(
                    state->total_cpu_100ns,
                    cpu_100ns);
            }
        }
    }
    WotbModV3ProfilerSpanInfo value = {};
    value.struct_size = sizeof(value);
    value.api_version = WOTBMOD_V3_DEVTOOLS_VERSION;
    value.start_ticks = static_cast<uint64_t>(span->start.QuadPart);
    value.elapsed_ticks = elapsed;
    value.elapsed_milliseconds = frequency.QuadPart > 0
        ? (static_cast<double>(elapsed) * 1000.0) /
            static_cast<double>(frequency.QuadPart)
        : 0.0;
    CopyString(value.category, sizeof(value.category), span->category.c_str());
    CopyString(value.name, sizeof(value.name), span->name.c_str());
    const size_t copy_size =
        std::min<size_t>(out_info->struct_size, sizeof(value));
    memcpy(out_info, &value, copy_size);
    out_info->struct_size = static_cast<uint32_t>(copy_size);
    return ReleaseOwnedHandle(mod, span_handle);
}

WotbModV3Result DevtoolsCounterSet(
    WotbModV3Handle mod,
    const char* category,
    const char* name,
    double value) {
    if (!IsBoundedText(
            name,
            WOTBMOD_V3_MAX_NAME,
            false) ||
        (category &&
         !IsBoundedText(
             category,
             WOTBMOD_V3_MAX_NAME,
             true)) ||
        !std::isfinite(value)) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "counter category, name, and finite value are required");
    }
    if (CheckMod(mod) != WOTBMOD_V3_OK) {
        return WOTBMOD_V3_E_INVALID_HANDLE;
    }
    const char* normalized_category =
        category && category[0] ? category : "devtools";
    try {
        std::lock_guard<std::mutex> lock(g_tooling_mutex);
        ModToolingState* state = GetOrCreateStateLocked(mod);
        const auto existing = std::find_if(
            state->counters.begin(),
            state->counters.end(),
            [normalized_category, name](const DevtoolsCounter& counter) {
                return counter.category == normalized_category &&
                    counter.name == name;
            });
        if (existing != state->counters.end()) {
            existing->value = value;
            existing->updated_frame = CurrentFrameIndex();
        } else {
            if (state->counters.size() >= kMaximumDevtoolsCounters) {
                return SetError(
                    mod,
                    WOTBMOD_V3_E_LIMIT_REACHED,
                    "per-mod devtools counter limit reached");
            }
            DevtoolsCounter counter;
            AssignBounded(
                &counter.category,
                normalized_category,
                WOTBMOD_V3_MAX_NAME - 1u);
            AssignBounded(
                &counter.name,
                name,
                WOTBMOD_V3_MAX_NAME - 1u);
            counter.value = value;
            counter.updated_frame = CurrentFrameIndex();
            state->counters.push_back(std::move(counter));
        }
        UpdatePeakLocked(state);
    } catch (const std::bad_alloc&) {
        return SetError(
            mod,
            WOTBMOD_V3_E_PLATFORM,
            "unable to retain devtools counter");
    }
    char line[WOTBMOD_V3_MAX_MESSAGE] = {};
    _snprintf_s(
        line,
        sizeof(line),
        _TRUNCATE,
        "counter mod=%s name=%s value=%.9g",
        ModId(mod),
        name,
        value);
    RuntimeLog(
        WOTBMOD_V3_LOG_DEBUG,
        normalized_category,
        line);
    return WOTBMOD_V3_OK;
}

WotbModV3Result DevtoolsInspectUnsupported(
    WotbModV3Handle mod,
    const char* selector,
    char* buffer,
    uint32_t* inout_size,
    const char* surface) {
    (void)selector;
    (void)buffer;
    if (inout_size) *inout_size = 0u;
    if (CheckMod(mod) != WOTBMOD_V3_OK) {
        return WOTBMOD_V3_E_INVALID_HANDLE;
    }
    char message[WOTBMOD_V3_MAX_MESSAGE] = {};
    _snprintf_s(
        message,
        sizeof(message),
        _TRUNCATE,
        "%s inspector has no native enumeration backend",
        surface);
    return SetError(
        mod,
        WOTBMOD_V3_E_NOT_SUPPORTED,
        message);
}

#define WOTBMOD_V3_DEFINE_INSPECTOR(name, surface)                         \
    WotbModV3Result name(                                                  \
        WotbModV3Handle mod,                                               \
        const char* selector,                                              \
        char* buffer,                                                      \
        uint32_t* inout_size) {                                            \
        return DevtoolsInspectUnsupported(                                 \
            mod, selector, buffer, inout_size, surface);                   \
    }

WOTBMOD_V3_DEFINE_INSPECTOR(DevtoolsInspectUi, "UI")

#undef WOTBMOD_V3_DEFINE_INSPECTOR

WotbModV3Result ValidatePortableInspectorRequest(
    WotbModV3Handle mod,
    const char* selector,
    uint32_t* inout_size) {
    if (!inout_size) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "inspector output size pointer is required");
    }
    const WotbModV3Result access = CheckAccess(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        nullptr);
    if (access != WOTBMOD_V3_OK) return access;
    if (selector &&
        strnlen_s(selector, WOTBMOD_V3_MAX_PATH) >=
            WOTBMOD_V3_MAX_PATH) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "inspector selector is not terminated or is too long");
    }
    return WOTBMOD_V3_OK;
}

void AppendInspectorHeader(
    std::string* json,
    const char* schema,
    WotbModV3Handle mod,
    const char* selector) {
    json->append("{\"schema\":");
    AppendJsonString(json, schema);
    json->append(",\"scope\":\"owner\",\"owner_mod\":");
    AppendJsonString(json, ModId(mod));
    json->append(",\"selector\":");
    AppendJsonString(json, selector ? selector : "");
    json->append(",\"native_enumeration\":false");
}

// Same header, but for an inspector whose data really did come from the engine.
// `native_enumeration` is the field a reader uses to tell "this is the runtime's
// own bookkeeping" from "this is the live scene", and answering false for the
// second would be a lie in the one field that exists to prevent it.
void AppendNativeInspectorHeader(
    std::string* json,
    const char* schema,
    WotbModV3Handle mod,
    const char* selector) {
    json->append("{\"schema\":");
    AppendJsonString(json, schema);
    json->append(",\"scope\":\"client\",\"owner_mod\":");
    AppendJsonString(json, ModId(mod));
    json->append(",\"selector\":");
    AppendJsonString(json, selector ? selector : "");
    json->append(",\"native_enumeration\":true");
}

/*
 * Read-only enumeration of the live scene, and of one node's material.
 *
 * Both used to be WOTBMOD_V3_DEFINE_INSPECTOR(..., DevtoolsInspectUnsupported),
 * which was accurate when it was written: nothing in the runtime could reach the
 * engine's scene graph. That stopped being true when the reviewed DAVA native
 * backend landed - `scene_nodes` already answers "every node name under the
 * active scene", and the material route already mints an NMaterial token for a
 * named node - so the refusal outlived its reason. The two rows in the live
 * validation panel had read NOT_SUPPORTED ever since, for a capability the
 * client had.
 *
 * Both are strictly read-only. The scene inspector only reads names. The
 * material inspector mints a token, asks the material a bounded list of
 * HAS_PROPERTY / HAS_TEXTURE / HAS_FLAG questions - which the DAVA ABI
 * documents as queries that mutate nothing - and releases the token on every
 * path including the failing ones.
 *
 * Both are gated on CAP_NMATERIAL, because that is the capability the registry
 * itself requires for `scene_nodes` and `scene_material`. Without it they
 * answer E_NOT_SUPPORTED with the reason, exactly as before.
 */

// The probe list is FIXED and documented rather than discovered, because the
// DAVA ABI exposes "does this material carry <name>" and not "list what it
// carries". A fixed list cannot claim completeness, so the JSON says so in
// `probe` - a reader must not read an absent name as "the material has nothing
// else". The names are the ones this SDK already writes through the material
// mutation route, so the inspector answers the question a mod author actually
// has: did my change land, and what was already there before it.
const char* const kMaterialProbeProperties[] = {
    "decalTileCoordScale",
    "decalTileColor",
    "decalNormalBlend",
    "flatColor",
    "albedoFlatColor"
};

const char* const kMaterialProbeTextures[] = {
    "albedo",
    "decaltexture",
    "decalColorMap",
    "decalNormalMap",
    "normalmap"
};

const char* const kMaterialProbeFlags[] = {
    "MATERIAL_DECAL",
    "MATERIAL_TEXTURE",
    "VERTEX_COLOR"
};

// A scene with more nodes than this is reported truncated rather than turned
// into an unbounded allocation inside a devtools call.
constexpr uint32_t kMaximumInspectedSceneNodes = 4096u;
constexpr uint32_t kMaximumSceneNodeBufferBytes = 1u << 20;

// A refusal writes NO data, so it reports a required size of zero. That is the
// contract DevtoolsInspectUnsupported held and callers were written against:
// only WOTBMOD_V3_E_BUFFER_TOO_SMALL leaves a meaningful size behind. Leaving
// the caller's own value in place instead would read as "your buffer was almost
// big enough" for a call that produced nothing.
WotbModV3Result RefuseInspector(uint32_t* inout_size, WotbModV3Result result) {
    if (inout_size) *inout_size = 0u;
    return result;
}

WotbModV3Result NativeSceneAvailable(WotbModV3Handle mod, const char* surface) {
    if ((InstalledDavaNativeCapabilities() &
         WOTBMOD_DAVA_NATIVE_CAP_NMATERIAL) != 0u) {
        return WOTBMOD_V3_OK;
    }
    char message[WOTBMOD_V3_MAX_MESSAGE] = {};
    _snprintf_s(
        message,
        sizeof(message),
        _TRUNCATE,
        "%s inspector has no native enumeration backend",
        surface);
    return SetError(mod, WOTBMOD_V3_E_NOT_SUPPORTED, message);
}

// Reads the newline-separated node dump into a string, sizing the buffer from
// the provider's own answer rather than guessing. Two calls, because that is
// the contract `scene_nodes` publishes: size first, then fill.
WotbModV3Result ReadSceneNodeDump(WotbModV3Handle mod, std::string* out_dump) {
    uint32_t required = 0u;
    const WotbModV3Result sized =
        InstalledDavaNativeSceneNodes(nullptr, &required);
    if (sized != WOTBMOD_V3_OK && sized != WOTBMOD_V3_E_BUFFER_TOO_SMALL) {
        return SetError(
            mod, sized, "native scene enumeration refused the size request");
    }
    if (required == 0u) {
        out_dump->clear();
        return WOTBMOD_V3_OK;
    }
    if (required > kMaximumSceneNodeBufferBytes) {
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "the active scene reported more node text than the inspector "
            "will materialise");
    }
    try {
        std::vector<char> storage(required, '\0');
        uint32_t size = required;
        const WotbModV3Result filled =
            InstalledDavaNativeSceneNodes(storage.data(), &size);
        if (filled != WOTBMOD_V3_OK) {
            return SetError(
                mod, filled, "native scene enumeration refused to fill");
        }
        if (size > required) size = required;
        out_dump->assign(storage.data(), size);
    } catch (const std::bad_alloc&) {
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "unable to allocate the scene node buffer");
    }
    return WOTBMOD_V3_OK;
}

// One line of the provider's dump: `depth|childCount|batches|name`, where the
// two counts are "?" when the walk could not read them. Parsing it here rather
// than passing the raw line through matters twice: the JSON gets real fields
// instead of a formatted string, and the material inspector can pick a node
// that actually HAS a render batch. Selecting the first line instead - which is
// the scene root, `0|1|0|<unnamed>` - is how the first live run asked for the
// material of the one node in the graph guaranteed not to have one.
struct InspectedSceneNode {
    uint32_t depth = 0u;
    bool counts_known = false;
    uint32_t children = 0u;
    uint32_t batches = 0u;
    std::string name;
};

bool ParseSceneNodeLine(const std::string& line, InspectedSceneNode* out_node) {
    size_t cursor = 0u;
    std::string fields[3];
    for (size_t field = 0u; field < 3u; ++field) {
        const size_t bar = line.find('|', cursor);
        if (bar == std::string::npos) return false;
        fields[field] = line.substr(cursor, bar - cursor);
        cursor = bar + 1u;
    }
    out_node->name = line.substr(cursor);
    if (out_node->name.empty()) return false;
    out_node->depth = static_cast<uint32_t>(strtoul(fields[0].c_str(), nullptr, 10));
    out_node->counts_known = fields[1] != "?" && fields[2] != "?";
    if (out_node->counts_known) {
        out_node->children =
            static_cast<uint32_t>(strtoul(fields[1].c_str(), nullptr, 10));
        out_node->batches =
            static_cast<uint32_t>(strtoul(fields[2].c_str(), nullptr, 10));
    }
    return true;
}

// A name the walk could not read is not a name a caller can pass back in.
bool SceneNodeNameIsUsable(const std::string& name) {
    return name != "<unnamed>" && name != "<unreadable>";
}

void SplitSceneNodes(
    const std::string& dump,
    const char* selector,
    std::vector<InspectedSceneNode>* out_nodes,
    bool* out_truncated) {
    *out_truncated = false;
    const bool all = !selector || selector[0] == '\0' ||
        (selector[0] == '*' && selector[1] == '\0');
    size_t begin = 0u;
    while (begin <= dump.size()) {
        size_t end = dump.find('\n', begin);
        if (end == std::string::npos) end = dump.size();
        // Tolerate CRLF as well as LF: the dump crosses an ABI boundary and
        // nothing in it promises which the provider used.
        size_t stop = end;
        while (stop > begin && dump[stop - 1u] == '\r') --stop;
        if (stop > begin) {
            InspectedSceneNode node;
            if (ParseSceneNodeLine(dump.substr(begin, stop - begin), &node)) {
                // The selector matches the NAME, not the formatted line -
                // otherwise "0" would match every node at depth 0 and also
                // every node with zero batches.
                if (all || node.name.find(selector) != std::string::npos) {
                    if (out_nodes->size() >= kMaximumInspectedSceneNodes) {
                        *out_truncated = true;
                        return;
                    }
                    out_nodes->push_back(std::move(node));
                }
            }
        }
        if (end == dump.size()) break;
        begin = end + 1u;
    }
}

WotbModV3Result DevtoolsInspectScene(
    WotbModV3Handle mod,
    const char* selector,
    char* buffer,
    uint32_t* inout_size) {
    WotbModV3Result result =
        ValidatePortableInspectorRequest(mod, selector, inout_size);
    if (result != WOTBMOD_V3_OK) return result;
    result = NativeSceneAvailable(mod, "scene");
    if (result != WOTBMOD_V3_OK) return RefuseInspector(inout_size, result);
    std::string dump;
    result = ReadSceneNodeDump(mod, &dump);
    if (result != WOTBMOD_V3_OK) return RefuseInspector(inout_size, result);
    try {
        std::vector<InspectedSceneNode> nodes;
        bool truncated = false;
        SplitSceneNodes(dump, selector, &nodes, &truncated);
        std::string json;
        json.reserve(256u + nodes.size() * 96u);
        AppendNativeInspectorHeader(
            &json, "wotbmod.devtools/scene-nodes-v1", mod, selector);
        json.append(",\"count\":");
        AppendUnsigned(&json, nodes.size());
        json.append(",\"truncated\":");
        AppendBoolean(&json, truncated);
        json.append(",\"nodes\":[");
        for (size_t index = 0u; index < nodes.size(); ++index) {
            if (index != 0u) json.push_back(',');
            const InspectedSceneNode& node = nodes[index];
            json.append("{\"name\":");
            AppendJsonString(&json, node.name.c_str());
            json.append(",\"depth\":");
            AppendUnsigned(&json, node.depth);
            if (node.counts_known) {
                json.append(",\"children\":");
                AppendUnsigned(&json, node.children);
                json.append(",\"render_batches\":");
                AppendUnsigned(&json, node.batches);
            } else {
                json.append(",\"children\":null,\"render_batches\":null");
            }
            json.push_back('}');
        }
        json.append("]}");
        return CopyDynamicString(
            mod, json, buffer, inout_size,
            "scene inspector buffer is too small");
    } catch (const std::bad_alloc&) {
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "unable to allocate scene inspector JSON");
    }
}

// Answers one HAS_* query. E_NOT_FOUND is a legitimate answer meaning "absent",
// so it is folded into the boolean; anything else is a real failure and is
// reported as unknown rather than silently as absent.
bool MaterialCarries(
    WotbModV3Handle mod,
    WotbModDavaNativeToken material,
    uint32_t query_kind,
    const char* name,
    bool* out_known) {
    WotbModDavaNativeMaterialMutation query = {};
    query.struct_size = sizeof(query);
    query.mutation_kind = query_kind;
    strncpy_s(query.name, sizeof(query.name), name, _TRUNCATE);
    const WotbModV3Result answer =
        InstalledDavaNativeMaterialMutate(mod, material, &query);
    *out_known =
        answer == WOTBMOD_V3_OK || answer == WOTBMOD_V3_E_NOT_FOUND;
    return answer == WOTBMOD_V3_OK;
}

void AppendMaterialProbeArray(
    std::string* json,
    WotbModV3Handle mod,
    WotbModDavaNativeToken material,
    uint32_t query_kind,
    const char* const* names,
    size_t name_count) {
    json->push_back('[');
    for (size_t index = 0u; index < name_count; ++index) {
        if (index != 0u) json->push_back(',');
        bool known = false;
        const bool present =
            MaterialCarries(mod, material, query_kind, names[index], &known);
        json->append("{\"name\":");
        AppendJsonString(json, names[index]);
        json->append(",\"present\":");
        if (known) {
            AppendBoolean(json, present);
        } else {
            json->append("null");
        }
        json->push_back('}');
    }
    json->push_back(']');
}

WotbModV3Result DevtoolsInspectMaterial(
    WotbModV3Handle mod,
    const char* selector,
    char* buffer,
    uint32_t* inout_size) {
    WotbModV3Result result =
        ValidatePortableInspectorRequest(mod, selector, inout_size);
    if (result != WOTBMOD_V3_OK) return result;
    result = NativeSceneAvailable(mod, "material");
    if (result != WOTBMOD_V3_OK) return RefuseInspector(inout_size, result);

    // A caller that already knows the node names passes one. A caller probing
    // the surface - the validation panel does exactly this - passes "*", and
    // gets the first node the live scene reports, so the row can be judged
    // without the operator having to know what the hangar calls the tank.
    std::string node_name = selector ? selector : "";
    if (node_name.empty() || node_name == "*") {
        std::string dump;
        result = ReadSceneNodeDump(mod, &dump);
        if (result != WOTBMOD_V3_OK) return RefuseInspector(inout_size, result);
        std::vector<InspectedSceneNode> nodes;
        bool truncated = false;
        SplitSceneNodes(dump, "*", &nodes, &truncated);
        // The FIRST node with a render batch and a readable name, not simply
        // the first node. The scene root is `0|1|0|<unnamed>` - no batches, no
        // name - so "the first one" asks the one node in the graph guaranteed
        // to have no material, and answers E_NOT_FOUND every time.
        for (const InspectedSceneNode& node : nodes) {
            if (node.counts_known && node.batches > 0u &&
                SceneNodeNameIsUsable(node.name)) {
                node_name = node.name;
                break;
            }
        }
        if (node_name.empty() || node_name == "*") {
            return RefuseInspector(
                inout_size,
                SetError(
                    mod,
                    WOTBMOD_V3_E_NOT_FOUND,
                    "the active scene reported no named node carrying a "
                    "render batch"));
        }
    }
    if (node_name.size() >= WOTBMOD_DAVA_NATIVE_MAX_CLASS_NAME) {
        return RefuseInspector(
            inout_size,
            SetError(
                mod,
                WOTBMOD_V3_E_INVALID_ARGUMENT,
                "scene node name is longer than the native ABI accepts"));
    }

    WotbModDavaNativeSceneMaterialRequest request = {};
    request.struct_size = sizeof(request);
    request.api_version = WOTBMOD_DAVA_NATIVE_BACKEND_VERSION;
    request.batch_index = 0u;
    strncpy_s(
        request.node_name,
        sizeof(request.node_name),
        node_name.c_str(),
        _TRUNCATE);
    WotbModDavaNativeToken material = 0u;
    const WotbModV3Result resolved =
        InstalledDavaNativeSceneMaterial(mod, &request, &material);
    if (resolved != WOTBMOD_V3_OK || material == 0u) {
        char message[WOTBMOD_V3_MAX_MESSAGE] = {};
        _snprintf_s(
            message,
            sizeof(message),
            _TRUNCATE,
            "no live material for scene node \"%s\"",
            node_name.c_str());
        return RefuseInspector(
            inout_size,
            SetError(
                mod,
                resolved == WOTBMOD_V3_OK ? WOTBMOD_V3_E_NOT_FOUND : resolved,
                message));
    }

    WotbModV3Result outcome = WOTBMOD_V3_OK;
    try {
        std::string json;
        json.reserve(1024u);
        AppendNativeInspectorHeader(
            &json, "wotbmod.devtools/scene-material-v1", mod, selector);
        json.append(",\"node\":");
        AppendJsonString(&json, node_name.c_str());
        json.append(",\"batch_index\":0");
        // Named so a reader cannot mistake a bounded probe for an enumeration.
        json.append(",\"probe\":\"bounded-known-names\"");
        json.append(",\"properties\":");
        AppendMaterialProbeArray(
            &json,
            mod,
            material,
            WOTBMOD_DAVA_NATIVE_MATERIAL_HAS_PROPERTY,
            kMaterialProbeProperties,
            sizeof(kMaterialProbeProperties) /
                sizeof(kMaterialProbeProperties[0]));
        json.append(",\"textures\":");
        AppendMaterialProbeArray(
            &json,
            mod,
            material,
            WOTBMOD_DAVA_NATIVE_MATERIAL_HAS_TEXTURE,
            kMaterialProbeTextures,
            sizeof(kMaterialProbeTextures) /
                sizeof(kMaterialProbeTextures[0]));
        json.append(",\"flags\":");
        AppendMaterialProbeArray(
            &json,
            mod,
            material,
            WOTBMOD_DAVA_NATIVE_MATERIAL_HAS_FLAG,
            kMaterialProbeFlags,
            sizeof(kMaterialProbeFlags) / sizeof(kMaterialProbeFlags[0]));
        json.push_back('}');
        outcome = CopyDynamicString(
            mod, json, buffer, inout_size,
            "material inspector buffer is too small");
    } catch (const std::bad_alloc&) {
        outcome = SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "unable to allocate material inspector JSON");
    }
    // The provider retained the material before the token existed, so this
    // balances what the provider added. It runs on the failing paths too.
    InstalledDavaNativeRelease(mod, material);
    return outcome;
}

WotbModV3Result DevtoolsInspectResource(
    WotbModV3Handle mod,
    const char* selector,
    char* buffer,
    uint32_t* inout_size) {
    WotbModV3Result result =
        ValidatePortableInspectorRequest(mod, selector, inout_size);
    if (result != WOTBMOD_V3_OK) return result;
    std::vector<DevtoolsResourceSnapshot> snapshots;
    result = SnapshotOwnedResourcesForDevtools(
        mod, selector, &snapshots);
    if (result != WOTBMOD_V3_OK) return result;
    try {
        std::string json;
        json.reserve(256u + snapshots.size() * 256u);
        AppendInspectorHeader(
            &json,
            "wotbmod.devtools/resources-v1",
            mod,
            selector);
        json.append(",\"count\":");
        AppendUnsigned(&json, snapshots.size());
        json.append(",\"resources\":[");
        for (size_t index = 0u; index < snapshots.size(); ++index) {
            if (index != 0u) json.push_back(',');
            const DevtoolsResourceSnapshot& snapshot =
                snapshots[index];
            json.append("{\"resource\":");
            AppendUnsigned(&json, snapshot.resource);
            json.append(",\"type\":");
            AppendUnsigned(&json, snapshot.type);
            json.append(",\"state\":");
            AppendUnsigned(&json, snapshot.state);
            json.append(",\"backing\":");
            AppendUnsigned(&json, snapshot.backing);
            json.append(",\"operation_in_progress\":");
            AppendBoolean(
                &json,
                snapshot.operation_in_progress != 0u);
            json.append(",\"memory_bytes\":");
            AppendUnsigned(&json, snapshot.memory_bytes);
            json.append(",\"uri\":");
            AppendJsonString(&json, snapshot.uri.c_str());
            json.append(",\"group\":");
            AppendJsonString(&json, snapshot.group.c_str());
            json.append(",\"sha256\":");
            AppendJsonString(&json, snapshot.sha256.c_str());
            json.append(",\"error\":");
            AppendJsonString(&json, snapshot.error.c_str());
            json.push_back('}');
        }
        json.append("]}");
        return CopyDynamicString(
            mod,
            json,
            buffer,
            inout_size,
            "resource inspector buffer is too small");
    } catch (const std::bad_alloc&) {
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "unable to allocate resource inspector JSON");
    }
}

WotbModV3Result DevtoolsInspectHookChain(
    WotbModV3Handle mod,
    const char* selector,
    char* buffer,
    uint32_t* inout_size) {
    WotbModV3Result result =
        ValidatePortableInspectorRequest(mod, selector, inout_size);
    if (result != WOTBMOD_V3_OK) return result;
    std::vector<DevtoolsHookSnapshot> snapshots;
    result = SnapshotOwnedHooksForDevtools(mod, selector, &snapshots);
    if (result != WOTBMOD_V3_OK) return result;
    try {
        std::string json;
        json.reserve(256u + snapshots.size() * 192u);
        AppendInspectorHeader(
            &json,
            "wotbmod.devtools/hook-chains-v1",
            mod,
            selector);
        json.append(",\"count\":");
        AppendUnsigned(&json, snapshots.size());
        json.append(",\"hooks\":[");
        for (size_t index = 0u; index < snapshots.size(); ++index) {
            if (index != 0u) json.push_back(',');
            const DevtoolsHookSnapshot& snapshot = snapshots[index];
            json.append("{\"hook\":");
            AppendUnsigned(&json, snapshot.hook);
            json.append(",\"creation_order\":");
            AppendUnsigned(&json, snapshot.creation_order);
            json.append(",\"mode\":");
            AppendUnsigned(&json, snapshot.mode);
            json.append(",\"status\":");
            AppendUnsigned(&json, snapshot.status);
            json.append(",\"priority\":");
            AppendSigned(&json, snapshot.priority);
            json.append(",\"conflict_count\":");
            AppendUnsigned(&json, snapshot.conflict_count);
            json.append(",\"native_created\":");
            AppendBoolean(&json, snapshot.native_created != 0u);
            json.append(",\"native_enabled\":");
            AppendBoolean(&json, snapshot.native_enabled != 0u);
            json.append(",\"target\":");
            AppendJsonString(&json, snapshot.target.c_str());
            json.push_back('}');
        }
        json.append("]}");
        return CopyDynamicString(
            mod,
            json,
            buffer,
            inout_size,
            "hook-chain inspector buffer is too small");
    } catch (const std::bad_alloc&) {
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "unable to allocate hook-chain inspector JSON");
    }
}

WotbModV3Result DevtoolsInspectEvents(
    WotbModV3Handle mod,
    const char* selector,
    char* buffer,
    uint32_t* inout_size) {
    WotbModV3Result result =
        ValidatePortableInspectorRequest(mod, selector, inout_size);
    if (result != WOTBMOD_V3_OK) return result;
    std::vector<DevtoolsEventSnapshot> snapshots;
    result = SnapshotOwnedEventsForDevtools(mod, selector, &snapshots);
    if (result != WOTBMOD_V3_OK) return result;
    try {
        std::string json;
        json.reserve(256u + snapshots.size() * 160u);
        AppendInspectorHeader(
            &json,
            "wotbmod.devtools/events-v1",
            mod,
            selector);
        json.append(",\"count\":");
        AppendUnsigned(&json, snapshots.size());
        json.append(",\"subscriptions\":[");
        for (size_t index = 0u; index < snapshots.size(); ++index) {
            if (index != 0u) json.push_back(',');
            const DevtoolsEventSnapshot& snapshot = snapshots[index];
            json.append("{\"token\":");
            AppendUnsigned(&json, snapshot.token);
            json.append(",\"creation_order\":");
            AppendUnsigned(&json, snapshot.creation_order);
            json.append(",\"priority\":");
            AppendSigned(&json, snapshot.priority);
            json.append(",\"receive_system_events\":");
            AppendBoolean(
                &json,
                snapshot.receive_system_events != 0u);
            json.append(",\"active\":");
            AppendBoolean(&json, snapshot.active != 0u);
            json.append(",\"accepting_callbacks\":");
            AppendBoolean(
                &json,
                snapshot.accepting_callbacks != 0u);
            json.append(",\"callbacks_in_flight\":");
            AppendUnsigned(&json, snapshot.callbacks_in_flight);
            json.append(",\"topic_pattern\":");
            AppendJsonString(
                &json,
                snapshot.topic_pattern.c_str());
            json.push_back('}');
        }
        json.append("]}");
        return CopyDynamicString(
            mod,
            json,
            buffer,
            inout_size,
            "event inspector buffer is too small");
    } catch (const std::bad_alloc&) {
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "unable to allocate event inspector JSON");
    }
}

WotbModV3Result DevtoolsProfilerGetModCpuTime(
    WotbModV3Handle mod,
    WotbModV3ProfilerAggregate* out_aggregate) {
    if (!out_aggregate ||
        out_aggregate->struct_size < sizeof(WotbModV3StructHeader)) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "profiler aggregate output is required");
    }
    if (CheckMod(mod) != WOTBMOD_V3_OK) {
        return WOTBMOD_V3_E_INVALID_HANDLE;
    }
    bool exists = false;
    ModToolingState state;
    try {
        state = CopyState(mod, &exists);
    } catch (const std::bad_alloc&) {
        return SetError(
            mod,
            WOTBMOD_V3_E_PLATFORM,
            "unable to snapshot profiler aggregate");
    }
    (void)exists;
    WotbModV3ProfilerAggregate value = {};
    value.struct_size = sizeof(value);
    value.api_version = WOTBMOD_V3_DEVTOOLS_VERSION_2;
    value.active_spans = state.active_spans;
    value.completed_spans = state.completed_spans;
    value.cpu_measured_spans = state.cpu_measured_spans;
    value.total_cpu_100ns = state.total_cpu_100ns;
    value.total_cpu_milliseconds =
        static_cast<double>(state.total_cpu_100ns) / 10000.0;
    const size_t copy_size =
        std::min<size_t>(out_aggregate->struct_size, sizeof(value));
    memcpy(out_aggregate, &value, copy_size);
    out_aggregate->struct_size = static_cast<uint32_t>(copy_size);
    return WOTBMOD_V3_OK;
}

WotbModV3Result DevtoolsProfilerGetModMemory(
    WotbModV3Handle mod,
    WotbModV3ProfilerMemory* out_memory) {
    if (!out_memory ||
        out_memory->struct_size < sizeof(WotbModV3StructHeader)) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "profiler memory output is required");
    }
    if (CheckMod(mod) != WOTBMOD_V3_OK) {
        return WOTBMOD_V3_E_INVALID_HANDLE;
    }
    bool state_exists = false;
    ModToolingState state;
    try {
        state = CopyState(mod, &state_exists);
    } catch (const std::bad_alloc&) {
        return SetError(
            mod,
            WOTBMOD_V3_E_PLATFORM,
            "unable to snapshot profiler memory");
    }
    WotbModV3ProfilerMemory value = {};
    value.struct_size = sizeof(value);
    value.api_version = WOTBMOD_V3_DEVTOOLS_VERSION_2;
    if (state_exists) {
        value.retained_diagnostics_bytes =
            RetainedDiagnosticsBytes(state);
        value.active_span_bytes = state.active_span_bytes;
        value.total_owned_bytes = AddSaturated(
            value.retained_diagnostics_bytes,
            value.active_span_bytes);
        value.peak_owned_bytes = state.peak_owned_bytes;
    }
    const size_t copy_size =
        std::min<size_t>(out_memory->struct_size, sizeof(value));
    memcpy(out_memory, &value, copy_size);
    out_memory->struct_size = static_cast<uint32_t>(copy_size);
    return WOTBMOD_V3_OK;
}

WotbModV3Result DevtoolsGetCallbackProfile(
    WotbModV3Handle mod,
    WotbModV3CallbackProfile* out_profile) {
    return GetModCallbackProfile(mod, out_profile);
}

WotbModV3Result DevtoolsSetCallbackBudget(
    WotbModV3Handle mod,
    uint32_t budget_microseconds,
    uint32_t max_callbacks_per_frame) {
    return SetModCallbackBudget(
        mod,
        budget_microseconds,
        max_callbacks_per_frame);
}

WotbModV3Result DevtoolsResetCallbackProfile(
    WotbModV3Handle mod) {
    return ResetModCallbackProfile(mod);
}

void ToolingLifecycleStateHook(
    WotbModV3Handle mod,
    uint32_t transition) {
    if (transition != LIFECYCLE_TRANSITION_UNLOADED) return;
    std::lock_guard<std::mutex> lock(g_tooling_mutex);
    g_mod_tooling.erase(mod);
}

void ToolingShutdownHook() {
    std::lock_guard<std::mutex> lock(g_tooling_mutex);
    g_mod_tooling.clear();
}

const WotbModV3DiagnosticsApiV2 kDiagnosticsApi = {
    sizeof(WotbModV3DiagnosticsApiV2),
    WOTBMOD_V3_DIAGNOSTICS_VERSION_2,
    DiagnosticsGetStats,
    DiagnosticsCopyReport,
    DiagnosticsReportFault,
    DiagnosticsCrashAddContext,
    DiagnosticsCrashSetLastAction,
    DiagnosticsCrashAddBreadcrumb,
    DiagnosticsExportBundle,
    DiagnosticsGetModHealth
};

const WotbModV3DevtoolsApiV3 kDevtoolsApi = {
    sizeof(WotbModV3DevtoolsApiV3),
    WOTBMOD_V3_DEVTOOLS_VERSION_3,
    DevtoolsMarker,
    DevtoolsSpanBegin,
    DevtoolsSpanEnd,
    DevtoolsCounterSet,
    DevtoolsInspectUi,
    DevtoolsInspectScene,
    DevtoolsInspectMaterial,
    DevtoolsInspectResource,
    DevtoolsInspectHookChain,
    DevtoolsInspectEvents,
    DevtoolsProfilerGetModCpuTime,
    DevtoolsProfilerGetModMemory,
    DevtoolsGetCallbackProfile,
    DevtoolsSetCallbackBudget,
    DevtoolsResetCallbackProfile
};

}  // namespace

void RegisterToolingServices() {
    RegisterInterface({
        WOTBMOD_V3_IFACE_DIAGNOSTICS,
        WOTBMOD_V3_DIAGNOSTICS_VERSION_2,
        &kDiagnosticsApi,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        nullptr});
    RegisterInterface({
        WOTBMOD_V3_IFACE_DEVTOOLS,
        WOTBMOD_V3_DEVTOOLS_VERSION_3,
        &kDevtoolsApi,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        nullptr});
    if (RegisterLifecycleStateHook(ToolingLifecycleStateHook) !=
        WOTBMOD_V3_OK) {
        RuntimeLog(
            WOTBMOD_V3_LOG_WARNING,
            "diagnostics",
            "unable to register diagnostics lifecycle cleanup");
    }
    if (RegisterShutdownHook(ToolingShutdownHook) != WOTBMOD_V3_OK) {
        RuntimeLog(
            WOTBMOD_V3_LOG_WARNING,
            "diagnostics",
            "unable to register diagnostics shutdown cleanup");
    }
}

}  // namespace v3
}  // namespace wotbmod
