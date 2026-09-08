#include "session_cluster_services.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "../../include/wotb_mod_runtime_v3.h"
#include "client_services_backend.h"
#include "wotb_mod_v3_internal.h"

/*
 * Portable half of wotbmod.session.cluster
 * (docs/superpowers/specs/2026-09-07-cluster-picker-design.md).
 *
 * The loader's native unit only reads three client structures and calls one
 * client function; everything that can be reasoned about without the client
 * lives here: permissions, the refusals of change() (context, in-flight,
 * cooldown, unknown or dead cluster), the hand-off to the main thread, the
 * QUEUED/STARTED/CONNECTED/FAILED state machine and the system event that
 * reports it. The state machine is judged from the frame pump by watching
 * the runtime context: a switch that never brings the hangar back within 60 s
 * is FAILED, and FAILED always clears the client's "manually selected" flag
 * so the next login auto-selects again (the safety net of the design).
 */
namespace wotbmod {
namespace v3 {
namespace {

const uint64_t kCooldownMs = 10000u;
const uint64_t kTimeoutMs = 60000u;
/* The hangar has to stay for this long before CONNECTED is judged. On the
 * very frame the context flips the client is still tearing down the old
 * scene (two scene releases in the same millisecond, live 2026-09-08), and a
 * mod acting in its CONNECTED handler right then - a toast panel - met a
 * null control inside the engine's next update. Half a second later the
 * screen is settled and the same handler is harmless. */
const uint64_t kHangarSettleMs = 500u;
const uint32_t kMaxClusters = 32u;

enum class Phase { Idle, Queued, Started };

std::mutex g_mutex;
SessionClusterDeclaredBackendAccessor g_accessor = nullptr;
Phase g_phase = Phase::Idle;
int32_t g_from = WOTBMOD_V3_SESSION_CLUSTER_AUTO;
int32_t g_to = WOTBMOD_V3_SESSION_CLUSTER_AUTO;
uint64_t g_started_ms = 0u;
uint64_t g_last_change_ms = 0u;
bool g_has_last_change = false;
bool g_seen_non_hangar = false;
uint64_t g_hangar_since_ms = 0u;  /* 0 = the hangar is not back yet */

uint64_t DefaultNowMs() { return static_cast<uint64_t>(GetTickCount64()); }

WotbModV3Result DefaultPost(MainThreadCallFn callback, void* user_data);

uint64_t (*g_now)() = &DefaultNowMs;
SessionClusterPostMainFn g_post = &DefaultPost;

bool Backend(ClientHostDeclaredBackend* out) {
    *out = ClientHostDeclaredBackend{};
    if (!g_accessor || !g_accessor(out)) return false;
    return out->session_cluster_enumerate && out->session_cluster_get_current &&
           out->session_cluster_change && out->session_cluster_set_manual;
}

void Publish(int32_t from, int32_t to, uint32_t status) {
    WotbModV3ClusterChangedEvent event = {};
    event.struct_size = sizeof(event);
    event.api_version = WOTBMOD_V3_SESSION_CLUSTER_VERSION;
    event.from_cluster_id = from;
    event.to_cluster_id = to;
    event.status = status;
    PublishSystemEvent(
        WOTBMOD_V3_EVENT_SESSION_CLUSTER_CHANGED, &event, sizeof(event), 0u);
}

void LogLine(uint32_t level, const char* text) {
    RuntimeLog(level, "v3.session-cluster", text);
}

bool InfoStructValid(const WotbModV3ClusterInfo* info) {
    return info && info->struct_size >= sizeof(WotbModV3ClusterInfo) &&
           info->api_version == WOTBMOD_V3_SESSION_CLUSTER_VERSION;
}

WotbModV3Result WOTBMOD_V3_CALL Enumerate(
    WotbModV3Handle mod, WotbModV3ClusterInfo* items, uint32_t* inout_count) {
    if (!inout_count) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    const WotbModV3Result permission = CheckNamedPermission(
        mod, "session.cluster.read", WOTBMOD_V3_PERMISSION_SAFE);
    if (permission != WOTBMOD_V3_OK) return permission;
    if (items) {
        for (uint32_t i = 0u; i < *inout_count; ++i) {
            if (!InfoStructValid(&items[i])) {
                return SetError(
                    mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "session.cluster.enumerate: every item needs struct_size and api_version");
            }
        }
    }
    ClientHostDeclaredBackend backend = {};
    if (!Backend(&backend)) {
        *inout_count = 0u;
        return SetError(
            mod, WOTBMOD_V3_E_NOT_SUPPORTED,
            "session.cluster: no native backend on this client");
    }
    const WotbModV3Result result =
        backend.session_cluster_enumerate(backend.user_data, items, inout_count);
    if (result == WOTBMOD_V3_E_NOT_SUPPORTED) {
        return SetError(
            mod, result,
            "session.cluster: the client's login manager has not been captured yet (no login seen)");
    }
    return result;
}

WotbModV3Result WOTBMOD_V3_CALL GetCurrent(
    WotbModV3Handle mod, WotbModV3ClusterInfo* out_info) {
    if (!InfoStructValid(out_info)) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    const WotbModV3Result permission = CheckNamedPermission(
        mod, "session.cluster.read", WOTBMOD_V3_PERMISSION_SAFE);
    if (permission != WOTBMOD_V3_OK) return permission;
    ClientHostDeclaredBackend backend = {};
    if (!Backend(&backend)) {
        return SetError(
            mod, WOTBMOD_V3_E_NOT_SUPPORTED,
            "session.cluster: no native backend on this client");
    }
    const WotbModV3Result result =
        backend.session_cluster_get_current(backend.user_data, out_info);
    if (result == WOTBMOD_V3_E_NOT_SUPPORTED) {
        return SetError(
            mod, result,
            "session.cluster: the client's login manager has not been captured yet (no login seen)");
    }
    return result;
}

/* Reads every cluster through the backend. Empty on any failure. */
std::vector<WotbModV3ClusterInfo> ReadClusters(const ClientHostDeclaredBackend& backend) {
    std::vector<WotbModV3ClusterInfo> clusters;
    uint32_t count = 0u;
    const WotbModV3Result probe =
        backend.session_cluster_enumerate(backend.user_data, nullptr, &count);
    if ((probe != WOTBMOD_V3_OK && probe != WOTBMOD_V3_E_BUFFER_TOO_SMALL) ||
        count == 0u || count > kMaxClusters) {
        return clusters;
    }
    clusters.resize(count);
    for (WotbModV3ClusterInfo& item : clusters) {
        item.struct_size = sizeof(item);
        item.api_version = WOTBMOD_V3_SESSION_CLUSTER_VERSION;
    }
    if (backend.session_cluster_enumerate(backend.user_data, clusters.data(), &count) !=
        WOTBMOD_V3_OK) {
        clusters.clear();
        return clusters;
    }
    if (count < clusters.size()) clusters.resize(count);
    return clusters;
}

void FinishFailed(const ClientHostDeclaredBackend& backend, const char* why) {
    backend.session_cluster_set_manual(backend.user_data, 0u);
    char line[192] = {};
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "cluster change to %d FAILED: %s; manual flag cleared", g_to, why);
    LogLine(WOTBMOD_V3_LOG_WARNING, line);
}

/* Main-thread job posted by change(). */
WotbModV3Result WOTBMOD_V3_CALL RunChangeOnMain(void*) {
    int32_t from = 0, to = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_phase != Phase::Queued) return WOTBMOD_V3_OK;
        from = g_from;
        to = g_to;
    }
    ClientHostDeclaredBackend backend = {};
    WotbModV3Result result = WOTBMOD_V3_E_NOT_SUPPORTED;
    if (Backend(&backend)) {
        if (to == WOTBMOD_V3_SESSION_CLUSTER_AUTO) {
            backend.session_cluster_set_manual(backend.user_data, 0u);
        }
        result = backend.session_cluster_change(backend.user_data, to);
    }
    bool started = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_phase == Phase::Queued) {
            if (result == WOTBMOD_V3_OK) {
                g_phase = Phase::Started;
                g_started_ms = g_now();
                g_seen_non_hangar = false;
                g_hangar_since_ms = 0u;
                started = true;
            } else {
                g_phase = Phase::Idle;
                g_last_change_ms = g_now();
                g_has_last_change = true;
            }
        }
    }
    if (started) {
        char line[128] = {};
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "cluster change %d -> %d STARTED (ChangeCluster called)", from, to);
        LogLine(WOTBMOD_V3_LOG_INFO, line);
        Publish(from, to, WOTBMOD_V3_CLUSTER_CHANGE_STARTED);
    } else {
        if (backend.session_cluster_set_manual) {
            char why[64] = {};
            _snprintf_s(why, sizeof(why), _TRUNCATE, "ChangeCluster answered %d",
                        static_cast<int>(result));
            FinishFailed(backend, why);
        }
        Publish(from, to, WOTBMOD_V3_CLUSTER_CHANGE_FAILED);
    }
    return WOTBMOD_V3_OK;
}

void WOTBMOD_V3_CALL CancelChangeOnMain(void*) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_phase == Phase::Queued) g_phase = Phase::Idle;
}

WotbModV3Result DefaultPost(MainThreadCallFn callback, void* user_data) {
    return PostMainThreadEx(callback, user_data, &CancelChangeOnMain);
}

WotbModV3Result WOTBMOD_V3_CALL Change(WotbModV3Handle mod, int32_t cluster_id) {
    const WotbModV3Result permission = CheckNamedPermission(
        mod, "session.cluster.change", WOTBMOD_V3_PERMISSION_REVIEWED);
    if (permission != WOTBMOD_V3_OK) return permission;
    ClientHostDeclaredBackend backend = {};
    if (!Backend(&backend)) {
        return SetError(
            mod, WOTBMOD_V3_E_NOT_SUPPORTED,
            "session.cluster: no native backend on this client");
    }
    if ((CurrentContext() & WOTBMOD_V3_CONTEXT_HANGAR) == 0u) {
        return SetError(
            mod, WOTBMOD_V3_E_CONFLICT,
            "session.cluster.change: a cluster change is allowed from the hangar only");
    }
    const uint64_t now = g_now();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_phase != Phase::Idle) {
            return SetError(
                mod, WOTBMOD_V3_E_BUSY,
                "session.cluster.change: a cluster change is already in flight");
        }
        if (g_has_last_change && now - g_last_change_ms < kCooldownMs) {
            return SetError(
                mod, WOTBMOD_V3_E_BUSY,
                "session.cluster.change: wait 10 s after the previous change");
        }
    }
    const std::vector<WotbModV3ClusterInfo> clusters = ReadClusters(backend);
    if (clusters.empty()) {
        return SetError(
            mod, WOTBMOD_V3_E_NOT_SUPPORTED,
            "session.cluster.change: the client's cluster list is not readable yet");
    }
    int32_t from = WOTBMOD_V3_SESSION_CLUSTER_AUTO;
    for (const WotbModV3ClusterInfo& item : clusters) {
        if (item.current) from = item.cluster_id;
    }
    if (cluster_id != WOTBMOD_V3_SESSION_CLUSTER_AUTO) {
        const WotbModV3ClusterInfo* target = nullptr;
        for (const WotbModV3ClusterInfo& item : clusters) {
            if (item.cluster_id == cluster_id) target = &item;
        }
        if (!target) {
            char message[128] = {};
            _snprintf_s(message, sizeof(message), _TRUNCATE,
                        "session.cluster.change: no cluster with id %d in this region",
                        cluster_id);
            return SetError(mod, WOTBMOD_V3_E_NOT_FOUND, message);
        }
        if (!target->alive || !target->allowed) {
            char message[160] = {};
            _snprintf_s(message, sizeof(message), _TRUNCATE,
                        "session.cluster.change: cluster %s is %s",
                        target->name,
                        !target->alive ? "not alive" : "not allowed for login");
            return SetError(mod, WOTBMOD_V3_E_CONFLICT, message);
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_phase != Phase::Idle) {
            return SetError(
                mod, WOTBMOD_V3_E_BUSY,
                "session.cluster.change: a cluster change is already in flight");
        }
        g_phase = Phase::Queued;
        g_from = from;
        g_to = cluster_id;
        g_started_ms = now;
        g_seen_non_hangar = false;
        g_hangar_since_ms = 0u;
    }
    Publish(from, cluster_id, WOTBMOD_V3_CLUSTER_CHANGE_QUEUED);
    const WotbModV3Result posted = g_post(&RunChangeOnMain, nullptr);
    if (posted != WOTBMOD_V3_OK) {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_phase == Phase::Queued) g_phase = Phase::Idle;
        }
        Publish(from, cluster_id, WOTBMOD_V3_CLUSTER_CHANGE_FAILED);
        return SetError(
            mod, posted,
            "session.cluster.change: could not reach the client's main thread");
    }
    char line[128] = {};
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "cluster change %d -> %d QUEUED by mod %s", from, cluster_id,
                ModId(mod) ? ModId(mod) : "?");
    LogLine(WOTBMOD_V3_LOG_INFO, line);
    return WOTBMOD_V3_OK;
}

WotbModV3SessionClusterApiV1 BuildApi() {
    WotbModV3SessionClusterApiV1 api = {};
    api.struct_size = sizeof(api);
    api.api_version = WOTBMOD_V3_SESSION_CLUSTER_VERSION;
    api.enumerate = &Enumerate;
    api.get_current = &GetCurrent;
    api.change = &Change;
    return api;
}

const WotbModV3SessionClusterApiV1 kApi = BuildApi();

}  // namespace

const WotbModV3SessionClusterApiV1& SessionClusterApi() { return kApi; }

bool SessionClusterChangeInFlight() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_phase == Phase::Started;
}

void SessionClusterFrameTick(uint64_t now_ms) {
    int32_t from = 0, to = 0;
    bool judge = false;
    bool timed_out = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_phase != Phase::Started) return;
        from = g_from;
        to = g_to;
        const uint64_t context = CurrentContext();
        if ((context & WOTBMOD_V3_CONTEXT_HANGAR) == 0u) {
            g_seen_non_hangar = true;
            g_hangar_since_ms = 0u;
        } else if (g_seen_non_hangar) {
            if (g_hangar_since_ms == 0u) g_hangar_since_ms = now_ms;
            if (now_ms - g_hangar_since_ms >= kHangarSettleMs) judge = true;
        }
        if (!judge && now_ms - g_started_ms > kTimeoutMs) {
            judge = true;
            timed_out = true;
        }
        if (!judge) return;
        g_phase = Phase::Idle;
        g_last_change_ms = now_ms;
        g_has_last_change = true;
    }
    ClientHostDeclaredBackend backend = {};
    const bool have_backend = Backend(&backend);
    bool connected = false;
    char why[160] = {};
    if (timed_out) {
        _snprintf_s(why, sizeof(why), _TRUNCATE, "no hangar within 60 s");
    } else if (!have_backend) {
        _snprintf_s(why, sizeof(why), _TRUNCATE, "backend vanished");
    } else {
        WotbModV3ClusterInfo current = {};
        current.struct_size = sizeof(current);
        current.api_version = WOTBMOD_V3_SESSION_CLUSTER_VERSION;
        const WotbModV3Result read =
            backend.session_cluster_get_current(backend.user_data, &current);
        if (read != WOTBMOD_V3_OK) {
            _snprintf_s(why, sizeof(why), _TRUNCATE,
                        "current cluster unreadable (%d)", static_cast<int>(read));
        } else if (to == WOTBMOD_V3_SESSION_CLUSTER_AUTO || current.cluster_id == to) {
            connected = true;
            char line[128] = {};
            _snprintf_s(line, sizeof(line), _TRUNCATE,
                        "cluster change %d -> %d CONNECTED (now %s, id %d)", from, to,
                        current.name, current.cluster_id);
            LogLine(WOTBMOD_V3_LOG_INFO, line);
        } else {
            _snprintf_s(why, sizeof(why), _TRUNCATE,
                        "hangar came back on %s (id %d)", current.name,
                        current.cluster_id);
        }
    }
    if (!connected && have_backend) FinishFailed(backend, why);
    Publish(from, to,
            connected ? WOTBMOD_V3_CLUSTER_CHANGE_CONNECTED
                      : WOTBMOD_V3_CLUSTER_CHANGE_FAILED);
}

void SessionClusterSetDeclaredBackendAccessor(
    SessionClusterDeclaredBackendAccessor accessor) {
    g_accessor = accessor;
}

void SessionClusterSetTestHooks(
    uint64_t (*now_ms)(), SessionClusterPostMainFn post) {
    g_now = now_ms ? now_ms : &DefaultNowMs;
    g_post = post ? post : &DefaultPost;
}

void SessionClusterResetForTests() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_phase = Phase::Idle;
    g_from = WOTBMOD_V3_SESSION_CLUSTER_AUTO;
    g_to = WOTBMOD_V3_SESSION_CLUSTER_AUTO;
    g_started_ms = 0u;
    g_hangar_since_ms = 0u;
    g_last_change_ms = 0u;
    g_has_last_change = false;
    g_seen_non_hangar = false;
}

}  // namespace v3
}  // namespace wotbmod
