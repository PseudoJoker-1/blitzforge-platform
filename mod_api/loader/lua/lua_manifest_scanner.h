#ifndef WOTBMOD_LUA_MANIFEST_SCANNER_H_
#define WOTBMOD_LUA_MANIFEST_SCANNER_H_

#include <cstddef>
#include <cstdint>

namespace wotbmod {
namespace lua {

using PermissionBits = uint64_t;
using PermissionBitResolver = PermissionBits (*)(const char*) noexcept;

// The host-owned fields in an installed Lua mod manifest. Unknown top-level
// metadata is still valid JSON and is ignored, but these three fields are
// parsed by the same strict scanner so path selection and permission selection
// cannot disagree about which object they read.
struct LuaManifest {
    static constexpr size_t kIdCapacity = 128u;
    static constexpr size_t kEntrypointCapacity = 260u;

    char id[kIdCapacity] = {};
    char entrypoint[kEntrypointCapacity] = {};
    PermissionBits requested_permissions = 0u;
    bool has_id = false;
    bool has_entrypoint = false;
};

// Scans exactly json_size bytes. Embedded NUL bytes are therefore trailing
// data, not a way to hide a second document from a strlen-based parser.
bool ScanLuaManifest(const char* json, size_t json_size,
                     PermissionBitResolver resolver, LuaManifest* out_manifest,
                     const char** out_error) noexcept;

// Reads exactly the top-level permissions array from a JSON manifest. The
// scanner knows JSON shape, bounds and refusal policy; the caller supplies the
// vocabulary mapping so this file does not own ScriptPermissions or Lua gates.
bool ScanPermissionManifest(const char* json, PermissionBitResolver resolver,
                            PermissionBits* out_bits,
                            const char** out_error) noexcept;

}  // namespace lua
}  // namespace wotbmod

#endif  // WOTBMOD_LUA_MANIFEST_SCANNER_H_
