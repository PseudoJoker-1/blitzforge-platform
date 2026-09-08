#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" {
#include "../third_party/lua/lauxlib.h"
#include "../third_party/lua/lua.h"
#include "../third_party/lua/lualib.h"
}

namespace {
uint32_t g_checks = 0u;
uint32_t g_failures = 0u;
void Check(bool condition, const char* label) {
    ++g_checks;
    if (condition) return;
    ++g_failures;
    std::printf("FAIL: %s\n", label);
}
}  // namespace

int main() {
    Check(std::strcmp(LUA_VERSION_MAJOR, "5") == 0, "major version is 5");
    Check(std::strcmp(LUA_VERSION_MINOR, "4") == 0, "minor version is 4");

    lua_State* state = luaL_newstate();
    Check(state != nullptr, "a state can be created");
    if (!state) return 1;

    luaL_openlibs(state);
    const int load_status = luaL_loadstring(state, "return 6 * 7");
    Check(load_status == LUA_OK, "a chunk compiles");
    const int call_status = lua_pcall(state, 0, 1, 0);
    Check(call_status == LUA_OK, "a chunk runs");
    Check(lua_isinteger(state, -1) == 1, "5.4 returns an integer, not a float");
    Check(lua_tointeger(state, -1) == 42, "the chunk returns 42");

    // Bytecode must be refusable, because the sandbox depends on it.
    lua_settop(state, 0);
    const char kFakeBytecode[] = "\x1bLua rest is not text";
    const int text_only = luaL_loadbufferx(
        state, kFakeBytecode, sizeof(kFakeBytecode) - 1, "=probe", "t");
    Check(text_only != LUA_OK, "mode \"t\" refuses a bytecode chunk");

    lua_close(state);
    std::printf("Lua vendor: %u passed, %u failed\n",
                g_checks - g_failures, g_failures);
    return g_failures == 0u ? 0 : 1;
}
