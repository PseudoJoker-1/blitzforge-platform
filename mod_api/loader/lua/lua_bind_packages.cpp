// wotb.packages: the loader-private bridge from a Lua script to the player's
// own wotbmod.exe.
//
// The in-game catalogue needs four things a frozen C ABI does not offer and
// should not: the list of installed packages, an install through the
// wotbmod:// launcher, an uninstall, and the [mods] switch. All four already
// exist as commands of <game>\wotbmod\wotbmod.exe, with the signatures, the
// ledger and the portal sync behind them. So the bridge runs that program,
// hidden, and hands back its exit code and output. It never interprets the
// output itself; the script does, with wotb.json.
//
// Shape of the fence. The verb is one of a closed list, each verb takes a
// fixed number of arguments, every argument is drawn from a small alphabet
// (no quotes, no spaces, no newlines), and the command line is assembled
// here rather than accepted. A script can therefore ask for `uninstall
// my.mod` and nothing else - not `install <anything>`, not a path, not a
// second command. What it asks is carried out by the CLI with its own checks
// (revocations, signatures, dependents), which the bridge does not repeat.
//
// One running job per script at a time. A job's output is capped so a chatty
// child cannot grow the host without bound, and a job that was never polled
// is dropped with the script: the state userdata's finaliser terminates what
// is still running and joins its reader before the memory goes away.
#include "lua_bindings.h"
#include "lua_convert.h"
#include "lua_dev_files.h"
#include "lua_permissions.h"
#include "lua_script.h"
#include "lua_watcher.h"

extern "C" {
#include "../../third_party/lua/lauxlib.h"
}

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace wotbmod {
namespace lua {
namespace {

const char kPermissionPackagesManage[] = "packages.manage";
const char* const kPackagesPermissions[] = {kPermissionPackagesManage};
const char kStateToken[] = "wotb.packages.state";
const wchar_t kExecutableOverrideVariable[] = L"WOTBMOD_PACKAGES_EXE";
const size_t kMaxArgumentBytes = 512u;
const size_t kMaxOutputBytes = 256u * 1024u;
const size_t kMaxJobs = 16u;
const DWORD kMaxPollWaitMs = 5000u;

// verb -> (argument count, needs --yes, needs --json, detached)
struct Verb {
    const char* name;
    int argument_count;
    bool yes;
    bool json;
    bool detached;
};

const Verb kVerbs[] = {
    {"list", 0, false, true, false},
    {"info", 1, false, true, false},
    {"uninstall", 1, true, false, false},
    {"enable", 1, false, false, false},
    {"disable", 1, false, false, false},
    {"launcher-open", 1, false, false, false},
    {"restart-client", 0, false, false, true},
    {"sync", 0, true, false, false},
};

const Verb* FindVerb(const char* name) noexcept {
    for (const Verb& verb : kVerbs) {
        if (std::strcmp(verb.name, name) == 0) return &verb;
    }
    return nullptr;
}

bool ArgumentAllowed(const std::string& value) noexcept {
    if (value.empty() || value.size() > kMaxArgumentBytes) return false;
    for (const char c : value) {
        const unsigned char byte = static_cast<unsigned char>(c);
        const bool alnum = (byte >= '0' && byte <= '9') ||
                           (byte >= 'a' && byte <= 'z') ||
                           (byte >= 'A' && byte <= 'Z');
        if (!alnum && std::strchr("._@:/%?=-", c) == nullptr) return false;
    }
    return true;
}

// launcher-open takes one wotbmod:// link and nothing else; the launcher
// validates the link strictly, this only refuses what is plainly not one.
bool LinkAllowed(const std::string& value) noexcept {
    return value.rfind("wotbmod://install/", 0u) == 0u ||
           value.rfind("wotbmod://uninstall/", 0u) == 0u;
}

struct Job {
    HANDLE process = nullptr;
    HANDLE done = nullptr;
    std::thread supervisor;
    std::mutex lock;
    std::string out;
    std::string err;
    bool finished = false;
    DWORD exit_code = 0u;
    bool polled = false;

    ~Job() {
        if (process) {
            if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
                TerminateProcess(process, 1u);
            }
        }
        if (supervisor.joinable()) supervisor.join();
        if (process) CloseHandle(process);
        if (done) CloseHandle(done);
    }
};

struct PackagesState {
    std::map<int, std::unique_ptr<Job>> jobs;
    int next_id = 1;
};

struct PackagesContext {
    const WotbModV3CoreApiV1* core;
    WotbModV3Handle mod;
    LuaScript* script;
    PackagesState* state;
};

PackagesContext* Context(lua_State* state) noexcept {
    return static_cast<PackagesContext*>(
        lua_touserdata(state, GuardedUpvalueIndex(1)));
}

int PushFailure(lua_State* state, const char* message) {
    lua_pushnil(state);
    lua_pushstring(state, message);
    return 2;
}

std::wstring GameDirectory(const PackagesContext* ctx) {
    if (!ctx || !ctx->core || !ctx->core->get_game_directory) return L"";
    uint32_t size = 0u;
    const WotbModV3Result measured =
        ctx->core->get_game_directory(ctx->mod, nullptr, &size);
    if ((measured != WOTBMOD_V3_E_BUFFER_TOO_SMALL &&
         measured != WOTBMOD_V3_OK) ||
        size <= 1u || size > 32768u) {
        return L"";
    }
    std::vector<char> utf8(size, '\0');
    uint32_t written = size;
    if (ctx->core->get_game_directory(ctx->mod, utf8.data(), &written) !=
            WOTBMOD_V3_OK ||
        written == 0u || written > size ||
        std::memchr(utf8.data(), '\0', written) == nullptr) {
        return L"";
    }
    std::wstring directory = Widen(std::string(utf8.data()));
    while (!directory.empty() &&
           (directory.back() == L'\\' || directory.back() == L'/')) {
        directory.pop_back();
    }
    return directory;
}

bool FileExists(const std::wstring& path) noexcept {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0u;
}

// WOTBMOD_PACKAGES_EXE exists for the host tests, which point it at a stub
// that echoes its arguments; a player's client never sets it.
std::wstring Executable(const PackagesContext* ctx, std::string* out_error) {
    wchar_t override_path[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(
        kExecutableOverrideVariable, override_path, MAX_PATH);
    if (length > 0u && length < MAX_PATH) {
        if (FileExists(override_path)) return override_path;
        if (out_error) *out_error = "WOTBMOD_PACKAGES_EXE does not exist";
        return L"";
    }
    const std::wstring game = GameDirectory(ctx);
    if (game.empty()) {
        if (out_error) *out_error = "game directory unknown";
        return L"";
    }
    const std::wstring path = JoinPath(JoinPath(game, L"wotbmod"), L"wotbmod.exe");
    if (!FileExists(path)) {
        if (out_error) *out_error = "wotbmod.exe not installed";
        return L"";
    }
    return path;
}

std::vector<wchar_t> EnvironmentWithLauncherFlags() {
    std::vector<wchar_t> block;
    wchar_t* strings = GetEnvironmentStringsW();
    if (strings) {
        const wchar_t* cursor = strings;
        while (*cursor) {
            const size_t length = std::wcslen(cursor);
            block.insert(block.end(), cursor, cursor + length + 1u);
            cursor += length + 1u;
        }
        FreeEnvironmentStringsW(strings);
    }
    const wchar_t* extra[] = {L"WOTBMOD_LAUNCHER_YES=1",
                              L"WOTBMOD_LAUNCHER_NO_PAUSE=1"};
    for (const wchar_t* item : extra) {
        block.insert(block.end(), item, item + std::wcslen(item) + 1u);
    }
    block.push_back(L'\0');
    return block;
}

void ReadPipe(HANDLE pipe, std::string* out, std::mutex* lock) {
    char buffer[4096];
    for (;;) {
        DWORD read = 0u;
        if (!ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr) || read == 0u) {
            break;
        }
        std::lock_guard<std::mutex> guard(*lock);
        if (out->size() < kMaxOutputBytes) {
            out->append(buffer, std::min<size_t>(read, kMaxOutputBytes - out->size()));
        }
    }
    CloseHandle(pipe);
}

// Starts the child; on success the job owns the process handle and a
// supervisor thread that drains both pipes and records the exit code.
bool Spawn(Job* job, const std::wstring& executable, std::wstring command_line,
           const std::wstring& working_directory, bool detached,
           std::string* out_error) {
    SECURITY_ATTRIBUTES inheritable = {};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;
    HANDLE out_read = nullptr, out_write = nullptr;
    HANDLE err_read = nullptr, err_write = nullptr;
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    if (!detached) {
        if (!CreatePipe(&out_read, &out_write, &inheritable, 0u) ||
            !CreatePipe(&err_read, &err_write, &inheritable, 0u)) {
            if (out_read) CloseHandle(out_read);
            if (out_write) CloseHandle(out_write);
            if (out_error) *out_error = "pipe creation failed";
            return false;
        }
        SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0u);
        SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0u);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = out_write;
        startup.hStdError = err_write;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }
    std::vector<wchar_t> environment = EnvironmentWithLauncherFlags();
    PROCESS_INFORMATION process = {};
    DWORD flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;
    if (detached) flags |= DETACHED_PROCESS | CREATE_BREAKAWAY_FROM_JOB;
    const BOOL created = CreateProcessW(
        executable.c_str(), &command_line[0], nullptr, nullptr,
        detached ? FALSE : TRUE, flags, environment.data(),
        working_directory.empty() ? nullptr : working_directory.c_str(),
        &startup, &process);
    if (!detached) {
        CloseHandle(out_write);
        CloseHandle(err_write);
    }
    if (!created) {
        if (!detached) {
            CloseHandle(out_read);
            CloseHandle(err_read);
        }
        if (out_error) {
            *out_error = "wotbmod.exe could not be started (error " +
                         std::to_string(GetLastError()) + ")";
        }
        return false;
    }
    CloseHandle(process.hThread);
    job->done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (detached) {
        // Nothing to read and nothing to wait for: the helper outlives the
        // client on purpose. The job is complete the moment it exists.
        CloseHandle(process.hProcess);
        job->process = nullptr;
        job->finished = true;
        job->exit_code = 0u;
        if (job->done) SetEvent(job->done);
        return true;
    }
    job->process = process.hProcess;
    job->supervisor = std::thread([job, out_read, err_read]() {
        std::thread err_reader(ReadPipe, err_read, &job->err, &job->lock);
        ReadPipe(out_read, &job->out, &job->lock);
        err_reader.join();
        WaitForSingleObject(job->process, INFINITE);
        DWORD code = 0u;
        GetExitCodeProcess(job->process, &code);
        {
            std::lock_guard<std::mutex> guard(job->lock);
            job->exit_code = code;
            job->finished = true;
        }
        if (job->done) SetEvent(job->done);
    });
    return true;
}

int PackagesRun(lua_State* state) {
    PackagesContext* ctx = Context(state);
    const char* verb_name = luaL_checkstring(state, 1);
    const Verb* verb = FindVerb(verb_name);
    if (!verb) return PushFailure(state, "verb refused");
    std::vector<std::string> arguments;
    if (!lua_isnoneornil(state, 2)) {
        if (!lua_istable(state, 2)) return PushArgumentError(state, 2, "table");
        const lua_Integer count = luaL_len(state, 2);
        for (lua_Integer index = 1; index <= count; ++index) {
            lua_geti(state, 2, index);
            const char* text = lua_isstring(state, -1) ? lua_tostring(state, -1) : nullptr;
            const std::string value = text ? text : "";
            lua_pop(state, 1);
            if (!ArgumentAllowed(value)) return PushFailure(state, "argument refused");
            arguments.push_back(value);
        }
    }
    if (static_cast<int>(arguments.size()) != verb->argument_count) {
        return PushFailure(state, "argument count refused");
    }
    if (std::strcmp(verb->name, "launcher-open") == 0 && !LinkAllowed(arguments[0])) {
        return PushFailure(state, "argument refused");
    }
    if (!ctx || !ctx->state) return PushFailure(state, "packages state gone");
    for (const auto& entry : ctx->state->jobs) {
        std::lock_guard<std::mutex> guard(entry.second->lock);
        if (!entry.second->finished) return PushFailure(state, "busy");
    }
    // Finished-and-polled jobs are gone already; finished-unpolled ones are
    // kept for one poll but never beyond kMaxJobs of them.
    while (ctx->state->jobs.size() >= kMaxJobs) {
        ctx->state->jobs.erase(ctx->state->jobs.begin());
    }
    std::string error;
    const std::wstring executable = Executable(ctx, &error);
    if (executable.empty()) return PushFailure(state, error.c_str());
    const std::wstring game = GameDirectory(ctx);

    std::wstring command_line = L"\"" + executable + L"\"";
    if (std::strcmp(verb->name, "launcher-open") == 0) {
        command_line += L" launcher open " + Widen(arguments[0]);
    } else {
        command_line += L" " + Widen(verb->name);
        for (const std::string& argument : arguments) {
            command_line += L" " + Widen(argument);
        }
        if (verb->json) command_line += L" --json";
        if (verb->yes) command_line += L" --yes";
        if (!game.empty()) command_line += L" --game-root \"" + game + L"\"";
    }
    std::unique_ptr<Job> job(new Job());
    // The child starts in the game folder when there is one; under the host
    // tests the mock client names a folder that does not exist, and a missing
    // working directory would refuse the whole spawn.
    const DWORD game_attributes =
        game.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(game.c_str());
    const std::wstring working_directory =
        (game_attributes != INVALID_FILE_ATTRIBUTES &&
         (game_attributes & FILE_ATTRIBUTE_DIRECTORY) != 0u)
            ? game
            : std::wstring();
    if (!Spawn(job.get(), executable, command_line, working_directory,
               verb->detached, &error)) {
        return PushFailure(state, error.c_str());
    }
    const int id = ctx->state->next_id++;
    ctx->state->jobs[id] = std::move(job);
    lua_pushinteger(state, id);
    return 1;
}

int PackagesPoll(lua_State* state) {
    PackagesContext* ctx = Context(state);
    const lua_Integer id = luaL_checkinteger(state, 1);
    lua_Integer wait_ms = 0;
    if (!lua_isnoneornil(state, 2)) wait_ms = luaL_checkinteger(state, 2);
    if (!ctx || !ctx->state) return PushFailure(state, "packages state gone");
    const auto found = ctx->state->jobs.find(static_cast<int>(id));
    if (found == ctx->state->jobs.end()) return PushFailure(state, "unknown job");
    Job* job = found->second.get();
    if (wait_ms > 0 && job->done) {
        WaitForSingleObject(job->done, static_cast<DWORD>(std::min<lua_Integer>(
                                           wait_ms, kMaxPollWaitMs)));
    }
    bool finished;
    DWORD exit_code;
    std::string out, err;
    {
        std::lock_guard<std::mutex> guard(job->lock);
        finished = job->finished;
        exit_code = job->exit_code;
        if (finished) {
            out = job->out;
            err = job->err;
        }
    }
    lua_createtable(state, 0, 3);
    if (!finished) {
        lua_pushboolean(state, 1);
        lua_setfield(state, -2, "running");
        return 1;
    }
    lua_pushinteger(state, static_cast<lua_Integer>(exit_code));
    lua_setfield(state, -2, "exit_code");
    lua_pushlstring(state, out.data(), out.size());
    lua_setfield(state, -2, "stdout");
    lua_pushlstring(state, err.data(), err.size());
    lua_setfield(state, -2, "stderr");
    // Handed over once: the job's memory goes with this answer.
    ctx->state->jobs.erase(found);
    return 1;
}

int PackagesCancel(lua_State* state) {
    PackagesContext* ctx = Context(state);
    const lua_Integer id = luaL_checkinteger(state, 1);
    if (!ctx || !ctx->state) {
        lua_pushboolean(state, 0);
        return 1;
    }
    const auto found = ctx->state->jobs.find(static_cast<int>(id));
    if (found == ctx->state->jobs.end()) {
        lua_pushboolean(state, 0);
        return 1;
    }
    ctx->state->jobs.erase(found);  // ~Job terminates and joins
    lua_pushboolean(state, 1);
    return 1;
}

int PackagesExecutable(lua_State* state) {
    PackagesContext* ctx = Context(state);
    std::string error;
    const std::wstring path = Executable(ctx, &error);
    if (path.empty()) return PushFailure(state, error.c_str());
    const std::string utf8 = Narrow(path);
    lua_pushlstring(state, utf8.data(), utf8.size());
    return 1;
}

int PackagesStateGc(lua_State* state) {
    PackagesState* packages = static_cast<PackagesState*>(
        luaL_testudata(state, 1, kStateToken));
    if (packages) packages->~PackagesState();
    return 0;
}

const luaL_Reg kPackagesFuncs[] = {
    {"run", PackagesRun},
    {"poll", PackagesPoll},
    {"cancel", PackagesCancel},
    {"executable", PackagesExecutable},
    {nullptr, nullptr},
};

}  // namespace

void RegisterPackages(lua_State* state, const WotbModV3CoreApiV1* core,
                      WotbModV3Handle mod, LuaScript* script) {
    if (!state || !script) return;

    PushWotbTable(state);                                   // [wotb]
    lua_newtable(state);                                    // [wotb, packages]

    // The job table lives in its own userdata with a finaliser, so a script
    // that goes away mid-install takes the child and the reader with it.
    void* memory = lua_newuserdatauv(state, sizeof(PackagesState), 0);
    PackagesState* packages = new (memory) PackagesState();  // [wotb, packages, state]
    if (luaL_newmetatable(state, kStateToken)) {
        lua_pushcfunction(state, PackagesStateGc);
        lua_setfield(state, -2, "__gc");
    }
    lua_setmetatable(state, -2);
    lua_setfield(state, -2, "__state");                     // [wotb, packages]

    PackagesContext* ctx = static_cast<PackagesContext*>(
        lua_newuserdatauv(state, sizeof(PackagesContext), 0));
    ctx->core = core;
    ctx->mod = mod;
    ctx->script = script;
    ctx->state = packages;                                   // [wotb, packages, ctx]

    SetFuncsGuardedAll(state, kPackagesFuncs, 1, script, kPackagesPermissions,
                       sizeof(kPackagesPermissions) / sizeof(kPackagesPermissions[0]),
                       kPermissionPackagesManage);           // [wotb, packages]

    lua_pushinteger(state, static_cast<lua_Integer>(kMaxOutputBytes));
    lua_setfield(state, -2, "MAX_OUTPUT");
    lua_pushinteger(state, static_cast<lua_Integer>(kMaxPollWaitMs));
    lua_setfield(state, -2, "MAX_POLL_WAIT_MS");

    lua_setfield(state, -2, "packages");                    // [wotb]
    lua_pop(state, 1);                                      // []
}

}  // namespace lua
}  // namespace wotbmod
