#pragma once
#include <cstdint>

#include "../../include/wotbmod/session_cluster_v1.h"
#include "wotb_mod_v3_internal.h"

namespace wotbmod {
namespace v3 {

struct ClientHostDeclaredBackend;

/* The wotbmod.session.cluster API table (portable half). */
const WotbModV3SessionClusterApiV1& SessionClusterApi();

/*
 * Frame tick from the client frame pump (main thread). Drives an in-flight
 * change to CONNECTED or FAILED: once the client has left the hangar and
 * come back, the current cluster decides; 60 s without a hangar decides
 * FAILED. Cheap when nothing is in flight.
 */
void SessionClusterFrameTick(uint64_t now_ms);

/* True between STARTED and the CONNECTED/FAILED verdict. The loader keeps
 * re-arming its full UI classification while this holds: the hangar that
 * comes back after LoginManager::ChangeCluster is built into the same
 * screen object and creates no new UI resource, so nothing else asks for
 * a probe and the HANGAR context was seen 60 s late (live 2026-09-08). */
bool SessionClusterChangeInFlight();

/* client_services.cpp registers the declared-backend snapshot function here
 * (same pattern as GesSetDeclaredBackendAccessor). Unset = no backend. */
typedef bool (*SessionClusterDeclaredBackendAccessor)(ClientHostDeclaredBackend* out);
void SessionClusterSetDeclaredBackendAccessor(
    SessionClusterDeclaredBackendAccessor accessor);

/* Test hooks: replace the clock and the main-thread poster. Either may be
 * null to restore the default (GetTickCount64 / PostMainThreadEx). The
 * poster receives the job that change() wants to run on the main thread;
 * the default queues it on the runtime's MAIN ingress. */
typedef WotbModV3Result (*SessionClusterPostMainFn)(
    MainThreadCallFn callback, void* user_data);
void SessionClusterSetTestHooks(
    uint64_t (*now_ms)(), SessionClusterPostMainFn post);

void SessionClusterResetForTests();

}  // namespace v3
}  // namespace wotbmod
