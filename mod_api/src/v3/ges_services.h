#pragma once
#include <cstdint>

#include "../../include/wotbmod/ges_v1.h"

namespace wotbmod {
namespace v3 {

/* The wotbmod.ges API table (portable half). */
const WotbModV3GesApiV1& GesApi();

/* True when `pattern` can match a "wotbmod.ges.*" topic and the GES source
 * bit is present in `source_mask`. */
bool GesPatternCanMatch(const char* pattern, uint64_t source_mask);

/* Loader -> mods. Synchronous: the payload pointer is only valid for the
 * duration of the call; every read_* slot refuses it afterwards. `flags`
 * carries WOTBMOD_V3_GES_EVENT_MOD_PUBLISHED for mod-originated events. */
WotbModV3Result GesHostPublish(const char* type_name, const void* payload,
                               uint32_t publisher_rva, uint32_t flags);

/* Lazy engine observation: called by EventsSubscribe / EventsUnsubscribe for
 * every system-event subscription so a type is registered in the engine only
 * while at least one mod pattern can match it. */
void GesOnPatternSubscribed(const char* pattern);
void GesOnPatternUnsubscribed(const char* pattern);
/* Re-offers to the backend every type whose observation it refused with
 * E_NOT_SUPPORTED (bus not captured yet). Cheap when nothing is pending; the
 * loader calls it once per frame. */
void GesRetryPendingObservations();

void GesResetForTests();

/* client_services.cpp registers its declared-backend snapshot function here
 * (from SetClientHostDeclaredBackend and RegisterClientServices), so this
 * unit links without client_services in the test binaries that compile the
 * runtime piecemeal. Unset = no backend. */
struct ClientHostDeclaredBackend;
typedef bool (*GesDeclaredBackendAccessor)(ClientHostDeclaredBackend* out);
void GesSetDeclaredBackendAccessor(GesDeclaredBackendAccessor accessor);

}  // namespace v3
}  // namespace wotbmod
