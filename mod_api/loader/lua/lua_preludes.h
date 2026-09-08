#ifndef WOTBMOD_LUA_PRELUDES_H_
#define WOTBMOD_LUA_PRELUDES_H_

// The convenience layer: Lua-source modules that are installed into every
// script's state after the C bindings, and that call nothing a script could
// not call itself.
//
// Why Lua source and not C:
//
//   * Nothing here needs a pointer, a handle or a permission of its own.
//     Every resource these modules can create was created by a C binding that
//     already recorded it in this script's ownership ledger, so teardown,
//     revocation and the permission fence all keep working with no new code.
//     A permission name of their own would be actively wrong -
//     PermissionBitFor returns kPermissionNothing for anything outside the
//     frozen 51, and kPermissionNothing means permanently *denied*.
//   * A pure-Lua module cannot corrupt a lua_State, cannot longjmp past a
//     C++ destructor, and cannot outlive its state. A hot reload builds a
//     brand-new lua_State, so nothing here survives one - which is the
//     property that makes these modules free of the host-side static leak
//     surface that a C implementation of the same features would have.
//
// Why they live in their own translation unit: lua_bindings.cpp already owns
// the input extensions, the interface queries and RegisterAll. The two
// original preludes (wotb.players, wotb.context) stay there because moving
// them would rewrite history for no gain; everything added after them lands
// here so that lua_bindings.cpp does not become a file of embedded Lua.
//
// This header deliberately exposes the *sources*, not a Register function.
// There is exactly one place in this host that enters a Lua VM to load a
// built-in library - RegisterLuaLibrary in lua_bindings.cpp - and the F6 fix
// (route that entry through LuaScript::ProtectedCall so the instruction budget
// applies to prelude load) is only worth anything if it stays the only such
// place. Handing back a list of sources keeps it that way.

#include <cstddef>

namespace wotbmod {
namespace lua {

// One built-in Lua library: its chunk name and its source.
//
// name must start with '@' - that is what makes a Lua traceback read
// "wotb.json:41: ..." instead of dumping the whole chunk into the message.
// size is the byte length without the terminator, so callers pass it straight
// to luaL_loadbufferx without a strlen over a string whose length is known at
// compile time.
struct LuaPreludeSource {
    const char* name;
    const char* source;
    size_t size;
};

// The convenience modules, in load order. The order matters only in that a
// module must not *call* a later one while loading; none of them call anything
// at load time (see the hazard note below), so the order here is the order an
// author reads them in rather than a dependency order.
//
// Load-time work in every one of these is deliberately trivial - define
// functions, build one small constant table, and stop. Nothing enumerates,
// nothing subscribes, nothing loops over data. Two reasons:
//
//   * prelude load is charged against the same 100,000-instruction budget as
//     any other entry into the state (that is the F6 fix), and a module that
//     spent a meaningful slice of it would be stealing from the script;
//   * it runs once per script per reload, on the thread that is loading mods.
//
// A module too large to define even its functions for free installs a stub
// instead and builds itself on first index - wotb.panel is the first of those,
// and its own header records why and what each half costs. The rule that makes
// the pattern safe is the same one above: whatever runs at load must be
// trivial, and a module that cannot be is obliged to defer.
//
// out_count receives the number of entries. Never null-returns: the array is
// static and non-empty.
const LuaPreludeSource* ConveniencePreludes(size_t* out_count) noexcept;

}  // namespace lua
}  // namespace wotbmod

#endif  // WOTBMOD_LUA_PRELUDES_H_
