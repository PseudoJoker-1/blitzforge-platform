#pragma once
/*
 * Engine-facing half of wotbmod.session.cluster for client 11.20.0.887.
 *
 * The client already implements the whole cluster switch
 * (LoginManager::ChangeCluster -> disconnect(5) -> HandleDisconnect ->
 * HandleDisconnectOnChangingCluster -> Region::GetClusterById -> re-login).
 * This unit only needs three client objects and one function, and it gets
 * the objects from one capture: LoginManager::OnHostChosen runs on every
 * login with `this` = LoginManager, and LoginManager+32 is `services`, the
 * interface every game system hangs off. From `services`:
 *
 *   region      = *(*(services->vtbl[appctx_slot]() + owner) + region)   vector<ClusterHost>
 *   connection  = services->vtbl[connection_slot]()                      ConnectionManager
 *   login       = services->vtbl[login_slot]()                           LoginManager
 *
 * ClusterHost entries are 208 bytes; ClusterHost+188 points at a descriptor
 * with the std::string name at +0, the url at +24 and the int cluster id at
 * +120. "alive" is the client's own notion (Region::HasAliveCluster): one of
 * the host's login addresses (vector at +24/+28, 208-byte entries) has a
 * positive ping; "allowed" is the host not being marked dead (state at +184
 * != 1). ConnectionManager+12 is the ClusterHost* the client is connected to.
 * A descriptor whose name does not look like a cluster name is named from
 * its url and id instead (login4.wotblitz.eu -> EU_C4). Every offset comes in through Layout (from
 * loader/anchor_rvas.h) and every read is guarded, so the unit runs against
 * synthetic memory in tests/v3_session_cluster_native_tests.cpp.
 *
 * Threads: OnHostChosen/Change run where the client calls them (main);
 * Enumerate/GetCurrent read cached pointers and fields from any thread.
 * Fail-closed: an exception in Change retires the unit.
 */
#include <cstddef>
#include <cstdint>

#include "../include/wotbmod/base.h"
#include "../include/wotbmod/session_cluster_v1.h"

namespace wotbmod {
namespace loader {
namespace session_cluster {

/* __thiscall(int) as a free pointer: ecx = login manager, edx unused. */
typedef int (__fastcall* ChangeClusterFn)(void* login_manager, void* edx, int cluster_id);
/* ConnectionManager::Disconnect(int reason, float delay, int arg) as a free
 * pointer; reason 12 re-logins through the client's automatic cluster pick. */
typedef int (__fastcall* DisconnectFn)(void* connection_manager, void* edx, int reason, float delay, int arg);
const int kAutoDisconnectReason = 12;
typedef void (*LogFn)(const char* message);

struct Layout {
    uint32_t services_offset;              /* LoginManager+32 -> services */
    uint32_t cluster_to_login_offset;      /* LoginManager+488 (diagnostic only) */
    uint32_t manual_flag_offset;           /* LoginManager+796 byte */
    uint32_t services_appctx_slot;         /* vtbl byte offset 316 */
    uint32_t services_connection_slot;     /* vtbl byte offset 100 */
    uint32_t services_login_slot;          /* vtbl byte offset 104 */
    uint32_t appctx_owner_offset;          /* +40 */
    uint32_t appctx_region_offset;         /* +1592 -> Region* {begin, end} */
    uint32_t connection_current_host_offset; /* +12 */
    uint32_t host_stride;                  /* 208 */
    uint32_t host_descriptor_offset;       /* +188 */
    uint32_t host_addresses_begin_offset;  /* +24: vector<Address> begin */
    uint32_t host_addresses_end_offset;    /* +28: vector<Address> end */
    uint32_t host_state_offset;            /* +184 int; 1 = marked dead */
    uint32_t address_stride;               /* 208 */
    uint32_t address_ping_offset;          /* +48 float; > 0 = answered */
    uint32_t descriptor_name_offset;       /* +0 std::string "EU_C4" */
    uint32_t descriptor_url_offset;        /* +24 std::string "login4..." */
    uint32_t descriptor_id_offset;         /* +120 int */
};

const uint32_t kMaxClusters = 32u;

/* Records the layout and the ChangeCluster entry point. Safe to call again. */
void Init(const Layout& layout, ChangeClusterFn change_cluster, DisconnectFn disconnect, LogFn log);

/* From the OnHostChosen detour. Captures login manager, services, connection
 * manager and region; false (and a log line) when a pointer fails its sanity
 * check, in which case the unit is not ready. `chosen_host` is only logged. */
bool OnHostChosen(void* login_manager, void* chosen_host);

/* Captured and not retired by a fault. */
bool Ready();

/* Any thread. NOT_SUPPORTED until ready. Two-pass: items null asks for the
 * count; BUFFER_TOO_SMALL keeps the count in *inout_count. */
WotbModV3Result Enumerate(WotbModV3ClusterInfo* items, uint32_t* inout_count);
WotbModV3Result GetCurrent(WotbModV3ClusterInfo* out_info);

/* Main thread. cluster_id >= 0: ChangeCluster(cluster_id) and the manual flag
 * set. WOTBMOD_V3_SESSION_CLUSTER_AUTO: manual flag cleared, then
 * ConnectionManager::Disconnect(12) so the client re-logins with its own
 * automatic pick (HandleDisconnect case 12 -> TryNextHost). */
WotbModV3Result Change(int32_t cluster_id);
WotbModV3Result SetManual(bool manual);

/* Forgets every captured pointer; the unit is no longer ready. */
void Shutdown();

}  // namespace session_cluster
}  // namespace loader
}  // namespace wotbmod
