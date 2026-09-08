// Minimal host for running mod_api Lua spec files outside the game.
//
// The vendored Lua tree under third_party/lua has no lua.c interpreter main,
// so specs like examples/*/tests/*_spec.lua have nothing to run them. This
// runner opens a plain Lua 5.4 state, points package.path at the mod's
// authored directory so `require("registry")` (and similar) resolves,
// forwards any extra command-line arguments into Lua's `arg` table, and
// executes the script named by argv[1].
//
// THIS STATE IS NOT THE STATE A SHIPPED MOD RUNS IN, and the difference is
// the whole of it: this one is stock `luaL_openlibs` plus a hand-set
// `package.path`, sharing no code with the host sandbox. The host opens base,
// table, string and math one at a time and deliberately not luaL_openlibs
// (loader/lua/lua_script.cpp:102-113), so `package`, `io`, `os`, `debug`,
// `utf8` and `coroutine` never exist there; it then nils `require`, `dofile`,
// `loadfile`, `collectgarbage` and `warn` (:247-254); and it takes exactly
// one entrypoint file per mod, with no path separator in its name
// (loader/lua/lua_host_mod.cpp:612-633).
//
// This comment used to claim that `require` here "resolves the same way it
// will once the real mod ships". That was false, and it invited a real
// defect: a module was written with a `require` that would have raised
// "attempt to call a nil value (global 'require')" on the first load in the
// game, and eight tasks of a green suite said nothing about it (ruling R41).
// A mod that must run in the sandbox needs a test that BUILDS the sandbox -
// see examples/blitzforge_skin_atelier/tests/assembly_spec.lua, which loads
// the assembled entrypoint into an environment holding exactly what the host
// holds and nothing else.
//
// Usage: lua_spec_runner.exe <script.lua> [arg1] [arg2] ...
// The script sees arg[1], arg[2], ... exactly as a standalone `lua` binary
// would provide them (argv[0]/script path itself is not included).
#include <cstdio>

extern "C" {
#include "../third_party/lua/lauxlib.h"
#include "../third_party/lua/lua.h"
#include "../third_party/lua/lualib.h"
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: lua_spec_runner <script.lua> [args...]\n");
        return 1;
    }

    lua_State* L = luaL_newstate();
    if (L == nullptr) {
        std::fprintf(stderr, "lua_spec_runner: could not create Lua state\n");
        return 1;
    }
    luaL_openlibs(L);

    // require("registry") must resolve against the mod's authored directory.
    // This runner is always launched with the current directory set to
    // mod_api (build_skin_atelier_tests.cmd pushes there), so the path is
    // relative to that root.
    lua_getglobal(L, "package");
    lua_pushstring(
        L,
        "examples\\blitzforge_skin_atelier\\?.lua;"
        "examples\\blitzforge_skin_atelier\\tests\\?.lua");
    lua_setfield(L, -2, "path");
    lua_pop(L, 1);

    // Forward argv[2..argc-1] as Lua arg[1], arg[2], ... the same convention
    // the standalone `lua` interpreter uses for arguments after the script.
    lua_newtable(L);
    for (int i = 2; i < argc; ++i) {
        lua_pushstring(L, argv[i]);
        lua_rawseti(L, -2, i - 1);
    }
    lua_setglobal(L, "arg");

    if (luaL_loadfile(L, argv[1]) != LUA_OK) {
        std::fprintf(stderr, "%s\n", lua_tostring(L, -1));
        lua_close(L);
        return 1;
    }
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        std::fprintf(stderr, "%s\n", lua_tostring(L, -1));
        lua_close(L);
        return 1;
    }

    lua_close(L);
    return 0;
}
