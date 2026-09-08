#ifndef WOTBMOD_LUA_DEV_FILES_H_
#define WOTBMOD_LUA_DEV_FILES_H_

#include <string>

namespace wotbmod {
namespace lua {

// Filesystem and path operations used by the dev-folder host. Kept outside
// lua_host_mod.cpp so the lifecycle/ownership state machine is not also the
// place that owns Win32 file-reading and module-path arithmetic.
std::string Narrow(const std::wstring& text);
std::wstring Widen(const std::string& text);
std::wstring FileNameOf(const std::wstring& path);
std::string ScriptIdOf(const std::wstring& path);
bool HasLuaExtension(const std::wstring& path);
bool ReadWholeFile(const std::wstring& path, std::string* out);
bool FileExists(const std::wstring& path);

// Resolves the dev folder in precedence order: an explicit override, the
// WOTBMOD_LUA_DEV_DIR environment variable, then lua-dev beside this host's
// install folder. The caller owns synchronisation around its override value;
// this function receives a copy and has no host-global state.
std::wstring ResolveDevFolder(const std::wstring& override_folder);

// Resolves installed Lua mods in precedence order: an explicit test override,
// WOTBMOD_LUA_MOD_DIR, <game-directory>\mods\lua, then (only when the runtime
// cannot provide its game directory) the `lua` directory beside this host's
// install folder. A normal install is therefore:
//   mods\wotbmod.lua_host\wotbmod_lua_host.dll
//   mods\lua\<mod-id>\manifest.json
std::wstring ResolveLuaModsFolder(const std::wstring& override_folder,
                                  const std::wstring& game_directory);

}  // namespace lua
}  // namespace wotbmod

#endif  // WOTBMOD_LUA_DEV_FILES_H_
