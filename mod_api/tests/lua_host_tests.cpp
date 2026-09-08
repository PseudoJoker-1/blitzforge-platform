#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// For GetProcessMemoryInfo, which the print()/__tostring leak check below uses
// to measure a leak rather than argue about one. Linked here rather than in
// tests\build_lua_host_tests.cmd so the build script stays a list of sources.
#include <psapi.h>
#pragma comment(lib, "psapi.lib")

#include "../include/wotb_mod_api_v3.h"
#include "lua_host_mock_abi.h"

// Compiled straight into this binary rather than reached through the host
// DLL's exports. lua_watcher.h deliberately includes nothing else from this
// host - no Lua, no ABI, no LuaScript - so there is nothing to marshal across
// an export boundary and no reason to invent one. The precedent is
// lua_convert_tests.cpp, which links lua_convert.cpp the same way and for the
// same reason: a leaf component with no client-facing surface is tested
// directly.
//
// The host's *own* watcher is a different object in the DLL, driven through
// on_enable/on_frame further down. The two never meet, and neither is a stand
// in for the other: this one proves the debounce rule, that one proves the
// reload path is wired to it.
#include "../loader/lua/lua_watcher.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace {
// How many times a test may pump on_frame (or drain a watcher) while waiting
// for the filesystem to say something, at 5-10 ms a turn.
//
// It is a *ceiling on failure*, not a delay: every loop below exits the moment
// its condition is met, so on an idle machine a reload lands in ten or twenty
// turns and the number here costs nothing. It only decides how long a genuine
// failure takes to report.
//
// Set high on purpose, and the value is the result of watching this suite fail
// rather than a guess. At 300 (1.5 s) the reload tests went red on a machine
// that was simultaneously running a build and an antivirus scan of the very
// folder under test - ReadDirectoryChangesW notification latency is not bounded
// by anything, and a test that assumes it is under a second is a test that
// fails for a reason that has nothing to do with the code. Ten seconds is
// beyond any plausible scheduling delay while still finishing a broken build
// this decade.
constexpr int kPumpLimit = 2000;

std::string ReadSimpleManifestString(const char* path, const char* field) {
    if (!path || !field) return {};
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    const std::string bytes((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
    const std::string needle = std::string("\"") + field + "\"";
    const size_t name = bytes.find(needle);
    if (name == std::string::npos) return {};
    const size_t colon = bytes.find(':', name + needle.size());
    const size_t quote = colon == std::string::npos
                             ? std::string::npos
                             : bytes.find('"', colon + 1u);
    const size_t end = quote == std::string::npos
                           ? std::string::npos
                           : bytes.find('"', quote + 1u);
    if (quote == std::string::npos || end == std::string::npos) return {};
    return bytes.substr(quote + 1u, end - quote - 1u);
}

uint32_t g_checks = 0u;
uint32_t g_failures = 0u;
void Check(bool condition, const char* label) {
    ++g_checks;
    if (condition) return;
    ++g_failures;
    std::printf("FAIL: %s\n", label);
}

// ---------------------------------------------------------------------------
// Temp-folder helpers for the watcher and hot-reload tests
// ---------------------------------------------------------------------------

// A fresh directory under %TEMP%, named after the process so two runs of this
// suite cannot collide. Empty string on failure, which every caller treats as
// "skip and fail the guard assertion" rather than pressing on against a path
// that is not there.
std::wstring MakeTempFolder(const wchar_t* tag) {
    wchar_t temp[MAX_PATH] = {};
    const DWORD length = GetTempPathW(MAX_PATH, temp);
    if (length == 0u || length >= MAX_PATH) return std::wstring();
    static unsigned int counter = 0u;
    wchar_t path[MAX_PATH] = {};
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%swotb_lua_%s_%lu_%u", temp, tag,
                 GetCurrentProcessId(), ++counter);
    if (!CreateDirectoryW(path, nullptr)) return std::wstring();
    return std::wstring(path);
}

// Recursive, and it has to be.
//
// This used to skip every subdirectory and then call RemoveDirectoryW, which
// fails on a folder that is not empty. Only one test makes a subdirectory - the
// overflow-path test, whose whole point is that a subfolder is not a script -
// so exactly one folder per run survived, forever, in %TEMP%.
//
// That is not merely untidy. MakeTempFolder names a folder after the process id
// and a per-tag counter, and Windows reuses process ids; when a run drew a pid
// whose leftover folder was still there, CreateDirectoryW failed,
// MakeTempFolder returned empty, and "a temp folder for the overflow path"
// failed for reasons having nothing to do with any change under test. Fifty-four
// of those folders had accumulated by the time it was chased down, and it had
// already appeared once inside a neutering experiment's evidence - which is the
// real cost, because an unexplained failure sitting in a results table is what
// makes the rest of the table arguable.
void RemoveTempFolder(const std::wstring& folder) {
    if (folder.empty()) return;
    const std::wstring pattern = folder + L"\\*";
    WIN32_FIND_DATAW found = {};
    HANDLE search = FindFirstFileW(pattern.c_str(), &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = found.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring child = folder + L"\\" + name;
            if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                RemoveTempFolder(child);
            } else {
                DeleteFileW(child.c_str());
            }
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }
    RemoveDirectoryW(folder.c_str());
}

// CREATE_ALWAYS, so a rewrite is a truncate-and-write - the shape an editor
// produces, and the shape that makes a debounce necessary in the first place.
bool WriteTextFile(const std::wstring& path, const std::string& text) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0u;
    const BOOL ok = WriteFile(file, text.data(),
                              static_cast<DWORD>(text.size()), &written,
                              nullptr);
    CloseHandle(file);
    return ok != FALSE && written == text.size();
}

// Reads a checked-in example relative to mod_api, independent of the caller's
// current directory. The test executable lives in mod_api\build, so removing
// the executable name and that one directory reaches the source root.
std::string ReadModApiFile(const wchar_t* relative_path) {
    wchar_t module_path[MAX_PATH] = {};
    const DWORD length =
        GetModuleFileNameW(nullptr, module_path, MAX_PATH);
    if (length == 0u || length >= MAX_PATH || !relative_path) {
        return std::string();
    }

    std::wstring root(module_path, length);
    for (int component = 0; component < 2; ++component) {
        const size_t separator = root.find_last_of(L"\\/");
        if (separator == std::wstring::npos) return std::string();
        root.resize(separator);
    }
    const std::wstring path = root + L"\\" + relative_path;

    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::string();

    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        size.QuadPart > 1024 * 1024) {
        CloseHandle(file);
        return std::string();
    }
    std::string contents(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0u;
    const BOOL ok = ReadFile(file, &contents[0],
                             static_cast<DWORD>(contents.size()), &read,
                             nullptr);
    CloseHandle(file);
    if (!ok || read != contents.size()) return std::string();
    return contents;
}

// A script that leaves one of each tracked resource kind behind and tidies up
// after none of them - the shape the ownership tests already use, now written
// to a file so the reload path is what creates it. `generation` makes every
// rewrite genuinely different content and gives on_enable something to say, so
// "the new script really did run" is an assertion rather than an assumption.
std::string HotScriptSource(int generation) {
    char source[768] = {};
    _snprintf_s(source, sizeof(source), _TRUNCATE,
                "-- generation %d\n"
                "sub = wotb.events.subscribe('wotbmod.frame.update', "
                "function() end)\n"
                "panel = wotb.ui.control_create()\n"
                "wotb.ui.control_set_id(panel, 'panel')\n"
                "txn = wotb.storage.begin_transaction()\n"
                "function on_enable() print('enabled %d') end\n"
                "function on_frame(frame, delta)\n"
                "  if frame == 777 and delta == 0.25 then\n"
                "    wotb.storage.set_json('frame_probe', 'ok')\n"
                "  end\n"
                "end\n",
                generation, generation);
    return std::string(source);
}

// Infinite-loop probes run in a child process. That is part of the assertion,
// not test plumbing: before the instruction limiter exists the expected
// failure is a process that never returns, and running that process in-line
// would leave the DLL mapped and make the next build fail at link time. The
// parent owns a hard deadline and terminates only its own child, turning a hang
// into an ordinary red Check.
int RunInstructionLimitChild(const char* mode, const char* dll_path) {
    HMODULE module = LoadLibraryA(dll_path);
    if (!module) return 10;

    auto entry = reinterpret_cast<WotbModLoadV3Fn>(
        GetProcAddress(module, WOTBMOD_V3_ENTRY_NAME));
    if (!entry) return 11;
    WotbModV3Bootstrap bootstrap = {};
    MockAbi::Install(&bootstrap);
    WotbModV3Info info = {};
    if (entry(&bootstrap, 1u, &info) != WOTBMOD_V3_OK) return 12;

    using RunFn = uint32_t(WOTBMOD_V3_CALL*)(const char*, char*, uint32_t);
    using CallGlobalFn = uint32_t(WOTBMOD_V3_CALL*)(
        const char*, const char*, char*, uint32_t);
    const auto run = reinterpret_cast<RunFn>(
        GetProcAddress(module, "WotbLuaHost_RunScriptForTests"));
    const auto call_global = reinterpret_cast<CallGlobalFn>(
        GetProcAddress(module, "WotbLuaHost_RunScriptCallGlobalForTests"));
    if (!run || !call_global) return 13;

    char error[512] = {};
    if (std::strcmp(mode, "chunk") == 0) {
        const uint32_t status = run(
            "txn = wotb.storage.begin_transaction(); while true do end",
            error, sizeof(error));
        return status == 2u && std::strstr(error, "instruction budget") &&
                       MockAbi::LiveTransactions() == 0u
                   ? 0
                   : 20;
    }
    if (std::strcmp(mode, "pcall") == 0) {
        const uint32_t status = run(
            "local ok = pcall(function() while true do end end); return ok",
            error, sizeof(error));
        return status == 2u && std::strstr(error, "instruction budget")
                   ? 0
                   : 21;
    }
    if (std::strcmp(mode, "on_enable") == 0 ||
        std::strcmp(mode, "on_disable") == 0 ||
        std::strcmp(mode, "on_frame") == 0) {
        std::string source = "function ";
        source += mode;
        source += "() while true do end end";
        const uint32_t status = call_global(
            source.c_str(), mode, error, sizeof(error));
        return status == 2u && std::strstr(error, "instruction budget")
                   ? 0
                   : 22;
    }
    if (std::strcmp(mode, "event") == 0) {
        using CreateScriptFn = void*(WOTBMOD_V3_CALL*)(
            const char*, const char*, char*, uint32_t);
        using EvalFn = uint32_t(WOTBMOD_V3_CALL*)(
            void*, const char*, char*, uint32_t);
        using DestroyScriptFn = void(WOTBMOD_V3_CALL*)(void*);
        const auto create_script = reinterpret_cast<CreateScriptFn>(
            GetProcAddress(module, "WotbLuaHost_CreateScriptForTests"));
        const auto eval = reinterpret_cast<EvalFn>(
            GetProcAddress(module, "WotbLuaHost_EvalInScriptForTests"));
        const auto destroy_script = reinterpret_cast<DestroyScriptFn>(
            GetProcAddress(module, "WotbLuaHost_DestroyScriptForTests"));
        if (!create_script || !eval || !destroy_script) return 23;

        MockAbi::Reset();
        MockAbi::event_flags = WOTBMOD_V3_EVENT_FLAG_STOPPABLE;
        void* script = create_script(
            "limit-event",
            "sub = wotb.events.subscribe('limit.event', function(e)\n"
            "  txn = wotb.storage.begin_transaction()\n"
            "  wotb.events.stop_propagation(e.dispatch)\n"
            "  while true do end\n"
            "end)",
            error, sizeof(error));
        if (!script) return 24;

        MockAbi::FireEvent("limit.event");
        std::memset(error, 0, sizeof(error));
        const uint32_t eval_status =
            eval(script, "error('faulted script ran again')",
                 error, sizeof(error));
        const bool stopped_synchronously = MockAbi::CalledContaining(
            MockAbi::events_calls, "events.stop_propagation(");
        destroy_script(script);
        return eval_status == 3u &&
                       std::strstr(error, "instruction budget") &&
                       stopped_synchronously &&
                       MockAbi::LiveSubscriptions() == 0u &&
                       MockAbi::LiveTransactions() == 0u
                   ? 0
                   : 25;
    }
    if (std::strcmp(mode, "loaded_event") == 0) {
        using SetDevFolderFn = void(WOTBMOD_V3_CALL*)(const wchar_t*, uint32_t);
        using LoadedCountFn = uint32_t(WOTBMOD_V3_CALL*)();
        const auto set_dev_folder = reinterpret_cast<SetDevFolderFn>(
            GetProcAddress(module, "WotbLuaHost_SetDevFolderForTests"));
        const auto loaded_count = reinterpret_cast<LoadedCountFn>(
            GetProcAddress(module, "WotbLuaHost_LoadedScriptCountForTests"));
        if (!set_dev_folder || !loaded_count || !info.on_enable ||
            !info.on_disable || !info.on_frame) {
            return 26;
        }

        const std::wstring folder = MakeTempFolder(L"instruction_limit");
        if (folder.empty()) return 27;
        const std::wstring path = folder + L"\\limited.lua";
        const bool written = WriteTextFile(
            path,
            "sub = wotb.events.subscribe('limit.loaded', function(e)\n"
            "  txn = wotb.storage.begin_transaction()\n"
            "  wotb.events.stop_propagation(e.dispatch)\n"
            "  while true do end\n"
            "end)\n");
        if (!written) {
            RemoveTempFolder(folder);
            return 28;
        }

        MockAbi::Reset();
        MockAbi::event_flags = WOTBMOD_V3_EVENT_FLAG_STOPPABLE;
        set_dev_folder(folder.c_str(), 5u);
        info.on_enable(&bootstrap, 1u);
        const bool loaded_before = loaded_count() == 1u &&
                                   MockAbi::LiveSubscriptions() == 1u;
        MockAbi::FireEvent("limit.loaded");

        // The hook runs under LuaScript::Entry. The object and its ownership
        // ledger must still exist at this point: deleting it from inside the
        // callback would run RevokeAll under the lock whose absence it
        // requires. The next frame is the safe detach-and-destroy boundary.
        const bool deferred_until_frame =
            loaded_count() == 1u && MockAbi::LiveSubscriptions() == 1u &&
            MockAbi::LiveTransactions() == 1u;
        info.on_frame(&bootstrap, 1u, 1u, 0.016);
        const bool reaped_on_frame =
            loaded_count() == 0u && MockAbi::LiveSubscriptions() == 0u &&
            MockAbi::LiveTransactions() == 0u;

        info.on_disable(&bootstrap, 1u);
        set_dev_folder(nullptr, 0u);
        RemoveTempFolder(folder);
        return loaded_before && deferred_until_frame && reaped_on_frame ? 0
                                                                        : 29;
    }
    return 14;
}

bool RunInstructionLimitProbe(const char* executable, const char* dll_path,
                              const char* mode) {
    std::string command = "\"";
    command += executable;
    command += "\" --instruction-limit-probe ";
    command += mode;
    command += " \"";
    command += dll_path;
    command += "\"";
    std::vector<char> mutable_command(command.begin(), command.end());
    mutable_command.push_back('\0');

    STARTUPINFOA startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    if (!CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr,
                        FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                        &startup, &process)) {
        std::fprintf(stderr, "instruction-limit probe %s could not start\n",
                     mode);
        return false;
    }
    CloseHandle(process.hThread);

    const DWORD probe_deadline_ms =
        std::strcmp(mode, "loaded_event") == 0 ? 10000u : 2000u;
    const DWORD wait = WaitForSingleObject(process.hProcess, probe_deadline_ms);
    if (wait != WAIT_OBJECT_0) {
        const BOOL terminated = TerminateProcess(process.hProcess, 124u);
        const DWORD terminated_wait =
            terminated ? WaitForSingleObject(process.hProcess, 5000u)
                       : WAIT_FAILED;
        CloseHandle(process.hProcess);
        std::fprintf(stderr,
                     "instruction-limit probe %s timed out%s\n", mode,
                     terminated && terminated_wait == WAIT_OBJECT_0
                         ? ""
                         : " and could not be confirmed terminated");
        return false;
    }
    DWORD exit_code = 1u;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hProcess);
    if (exit_code != 0u) {
        std::fprintf(stderr, "instruction-limit probe %s exited %lu\n", mode,
                     static_cast<unsigned long>(exit_code));
    }
    return exit_code == 0u;
}

// ---------------------------------------------------------------------------
// A settings client, for the one struct field shape that used to hand the
// client a length it never allocated
// ---------------------------------------------------------------------------
//
// WotbModV3SettingPreset carries `const WotbModV3SettingPresetValue* values`
// next to `uint32_t value_count`, and the generated reader used to allocate one
// arena element for the pointer and then believe whatever number the script put
// in value_count. SettingsRegisterPreset walks value_count of them
// (src/v3/data_services.cpp:10754, reading value.struct_size off each), so
// `{values = {v}, value_count = 512}` was a script choosing how far past a
// one-element heap block the game read. The same shape sits in
// WotbModV3InputActionDesc and WotbModV3VehicleSkinPack.
//
// The synthetic client in lua_host_mock_abi.h publishes no settings interface -
// two tests further up assert exactly that - so this file lends the host one
// for the length of a single section, by wrapping query_interface rather than
// by changing the mock. The host holds the bootstrap by pointer
// (loader/lua/lua_host_mod.cpp:1202), so swapping the field and calling the
// entry again is all it takes, and swapping it back restores the world the rest
// of the suite was written against.
//
// It records what it was handed and reads exactly ONE element, whatever the
// count claims. That is deliberate: this test has to *diagnose* the fault, and
// walking value_count elements of a one-element buffer would be the fault.
namespace PresetProbe {
uint32_t calls = 0u;
uint32_t last_value_count = 0u;
bool values_were_null = true;
char first_key[WOTBMOD_V3_SETTING_KEY_MAX] = {};

void Reset() {
    calls = 0u;
    last_value_count = 0u;
    values_were_null = true;
    std::memset(first_key, 0, sizeof(first_key));
}

WotbModV3Result WOTBMOD_V3_CALL RegisterPreset(
    WotbModV3Handle, const WotbModV3SettingPreset* preset) {
    ++calls;
    if (!preset) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    last_value_count = preset->value_count;
    values_were_null = preset->values == nullptr;
    if (preset->values && preset->value_count) {
        strncpy_s(first_key, preset->values[0].key, _TRUNCATE);
    }
    return WOTBMOD_V3_OK;
}

const WotbModV3SettingsApiV1& Api() {
    static WotbModV3SettingsApiV1 api = {};
    api.struct_size = sizeof(api);
    api.api_version = WOTBMOD_V3_SETTINGS_VERSION;
    api.register_preset = &RegisterPreset;
    return api;
}

WotbModV3Result WOTBMOD_V3_CALL QueryInterface(
    WotbModV3Handle mod, const char* name, uint32_t version,
    const void** out_interface) {
    if (name && out_interface &&
        std::strcmp(name, WOTBMOD_V3_IFACE_SETTINGS) == 0) {
        *out_interface = &Api();
        return WOTBMOD_V3_OK;
    }
    return MockAbi::QueryInterface(mod, name, version, out_interface);
}
}  // namespace PresetProbe
}  // namespace

int main(int argc, char** argv) {
    if (argc == 4 &&
        std::strcmp(argv[1], "--instruction-limit-probe") == 0) {
        return RunInstructionLimitChild(argv[2], argv[3]);
    }
    if (argc != 4) {
        std::fprintf(stderr,
                     "usage: lua_host_tests <dll> <manifest> <dava-stub>\n");
        return 2;
    }
    HMODULE dava_stub = LoadLibraryA(argv[3]);
    Check(dava_stub != nullptr, "the owner-checking DAVA bridge stub loads");
    if (!dava_stub) return 1;
    HMODULE module = LoadLibraryA(argv[1]);
    Check(module != nullptr, "the host DLL loads");
    if (!module) return 1;

    auto entry = reinterpret_cast<WotbModLoadV3Fn>(
        GetProcAddress(module, WOTBMOD_V3_ENTRY_NAME));
    Check(entry != nullptr, "it exports WotbModLoadV3");
    if (!entry) return 1;

    WotbModV3Bootstrap bootstrap = {};
    MockAbi::Install(&bootstrap);

    WotbModV3Info info = {};
    const WotbModV3Result result = entry(&bootstrap, 1u, &info);
    Check(result == WOTBMOD_V3_OK, "the entry accepts a valid bootstrap");
    Check(std::strcmp(info.id, "wotbmod.lua_host") == 0, "it names itself");
    const std::string manifest_id =
        ReadSimpleManifestString(argv[2], "id");
    const std::string manifest_version =
        ReadSimpleManifestString(argv[2], "version");
    Check(!manifest_id.empty() && manifest_id == info.id,
          "the DLL id exactly matches the package manifest");
    Check(!manifest_version.empty() && manifest_version == info.version,
          "the DLL version exactly matches the package manifest");
    Check(info.requested_permission_tier == WOTBMOD_V3_PERMISSION_REVIEWED,
          "it asks for REVIEWED and no more");
    Check(info.on_enable != nullptr, "it has on_enable");
    Check(info.on_disable != nullptr, "it has on_disable");
    Check(info.on_unload != nullptr, "it has on_unload");
    // The reload pump. Without it the watcher would have nowhere to be
    // drained on the main thread, and a reload would have to happen on the
    // watcher's own - which is the one thing this task must not do.
    Check(info.on_frame != nullptr, "it has on_frame, which is where reloads "
                                    "happen and the only thread they may");

    // A null out_info must be refused, not dereferenced.
    Check(entry(&bootstrap, 1u, nullptr) == WOTBMOD_V3_E_INVALID_ARGUMENT,
          "a null info pointer is refused");
    Check(entry(nullptr, 1u, &info) == WOTBMOD_V3_E_INVALID_ARGUMENT,
          "a null bootstrap is refused");

    using RunFn = uint32_t(WOTBMOD_V3_CALL*)(const char*, char*, uint32_t);
    const RunFn run = reinterpret_cast<RunFn>(
        GetProcAddress(module, "WotbLuaHost_RunScriptForTests"));
    Check(run != nullptr, "the test entry point is exported");
    if (run) {
        char error[512] = {};
        Check(run("return 1 + 1", error, sizeof(error)) == 0u,
              "a plain script runs");

        // Everything that could reach the filesystem, the process or new code
        // must be absent - not merely discouraged.
        for (const char* forbidden : {"io", "os", "package", "debug",
                                      "require", "dofile", "loadfile"}) {
            char probe[128] = {};
            std::snprintf(probe, sizeof(probe),
                          "if %s ~= nil then error('reachable') end",
                          forbidden);
            char message[512] = {};
            const uint32_t status = run(probe, message, sizeof(message));
            Check(status == 0u, forbidden);   // label names the escape route
        }

        char message[512] = {};
        Check(run("return load('\\27Lua fake', 'c', 'b')", message,
                  sizeof(message)) == 0u,
              "load exists");
        Check(run("local f = load('\\27Lua fake'); if f ~= nil then "
                  "error('bytecode accepted') end", message,
                  sizeof(message)) == 0u,
              "load refuses bytecode");

        Check(run("this is not lua", message, sizeof(message)) == 1u,
              "a syntax error is reported as a compile error");
        Check(std::strstr(message, "line") != nullptr ||
              std::strlen(message) > 0u,
              "the compile error carries a message");

        Check(run("error('boom')", message, sizeof(message)) == 2u,
              "a runtime error is reported as a runtime error, not a crash");

        // error() with a non-string, non-number object: lua_tostring on the
        // error value returns NULL, and that must not become a NULL written
        // into a std::string (undefined behaviour, an access violation in
        // practice) or a crash of any other kind - just a reported failure
        // with a placeholder message.
        std::memset(message, 0, sizeof(message));
        Check(run("error({})", message, sizeof(message)) == 2u,
              "error() with a table is a runtime error, not a crash");
        Check(std::strcmp(message, "<non-string error>") == 0,
              "a non-string error value gets a placeholder message");

        std::memset(message, 0, sizeof(message));
        Check(run("error(nil)", message, sizeof(message)) == 2u,
              "error(nil) is a runtime error, not a crash");
        Check(std::strcmp(message, "<non-string error>") == 0,
              "error(nil) gets a placeholder message");

        // Representative generated bindings must execute through their real
        // ABI slots. Exact, distinctive values catch a wrapper that merely
        // returns a plausible default or accidentally points at a neighbour.
        MockAbi::Reset();
        std::memset(message, 0, sizeof(message));
        Check(run("local frame = wotb.core.get_frame_index()\n"
                  "if frame ~= 424242 then "
                  "error('frame ' .. tostring(frame)) end\n"
                  "local path = wotb.core.get_game_directory()\n"
                  "if path ~= 'C:\\\\mock-game' then "
                  "error('path ' .. tostring(path)) end",
                  message, sizeof(message)) == 0u,
              "generated scalar and copy-out string bindings return the "
              "client's exact values");
        Check(MockAbi::Called(MockAbi::core_calls, "core.get_frame_index()") &&
                  MockAbi::CountCalled(MockAbi::core_calls,
                                       "core.get_game_directory()") == 1u,
              "both generated bindings reached their own ABI slots, and the "
              "small string used the bounded inline read path");

        // The synthetic core intentionally leaves get_context unpublished.
        // Calling it is a normal Lua-level unsupported result, never a jump
        // through a null function pointer.
        std::memset(message, 0, sizeof(message));
        Check(run("local value, err = wotb.core.get_context()\n"
                  "if value ~= nil then error('null slot returned a value') end\n"
                  "if type(err) ~= 'string' or "
                  "not string.find(err, 'did not publish this slot') then\n"
                  "  error('wrong error ' .. tostring(err))\n"
                  "end",
                  message, sizeof(message)) == 0u,
              "a generated binding safely reports a missing client slot");

        MockAbi::publish_core_context = true;
        MockAbi::core_context_mask = WOTBMOD_V3_CONTEXT_BATTLE;
        std::memset(message, 0, sizeof(message));
        Check(run(
                  "local current = wotb.context.current()\n"
                  "if current ~= wotb.context.BATTLE then error('current') end\n"
                  "if not wotb.context.contains(current, "
                  "wotb.context.BATTLE) then error('contains') end\n"
                  "local battle = wotb.context.should_show(\n"
                  "  wotb.context.BATTLE, wotb.context.MOD_SCREEN)\n"
                  "if battle ~= true then error('battle hidden') end\n"
                  "local catalog = wotb.context.should_show(\n"
                  "  wotb.context.BATTLE, wotb.context.MOD_SCREEN,\n"
                  "  wotb.context.BATTLE + wotb.context.MOD_SCREEN)\n"
                  "if catalog ~= false then error('catalog visible') end",
                  message, sizeof(message)) == 0u,
              "wotb.context exposes named masks and fail-closed visibility "
              "policy over the live core context");
        MockAbi::publish_core_context = false;
        MockAbi::core_context_mask = WOTBMOD_V3_CONTEXT_NONE;

        // Stock load() also accepts a reader function instead of a string.
        // This sandbox does not support that form, but rejecting it must
        // follow load()'s own contract - (nil, message) - not raise.
        std::memset(message, 0, sizeof(message));
        Check(run("local ok, err = load(function() end); "
                  "if ok ~= nil then error('reader accepted') end; "
                  "if type(err) ~= 'string' then error('no message') end",
                  message, sizeof(message)) == 0u,
              "load rejects a reader function without raising");
    }

    // ---- loader-private DAVA bridge: typed Lua handles end to end ---------
    //
    // The separately loaded stub has the same base name and exports as the
    // production loader. This drives the exact GetModuleHandle/GetProcAddress
    // route used in the client, while keeping engine pointers out of the test
    // and making owner/kind mistakes ordinary red assertions.
    using DavaResetFn = void(WOTBMOD_V3_CALL*)();
    using DavaCountFn = uint32_t(WOTBMOD_V3_CALL*)();
    using DavaCapabilitiesFn = uint64_t(WOTBMOD_V3_CALL*)();
    using DavaCreateScriptFn = void*(WOTBMOD_V3_CALL*)(
        const char*, const char*, char*, uint32_t);
    using DavaEvalFn = uint32_t(WOTBMOD_V3_CALL*)(
        void*, const char*, char*, uint32_t);
    using DavaDestroyScriptFn = void(WOTBMOD_V3_CALL*)(void*);
    const DavaCapabilitiesFn dava_capabilities =
        reinterpret_cast<DavaCapabilitiesFn>(GetProcAddress(
            dava_stub, "WotbModLoader_DavaNativeCapabilities"));
    const DavaResetFn dava_reset = reinterpret_cast<DavaResetFn>(
        GetProcAddress(dava_stub, "WotbModLoader_DavaTestReset"));
    const DavaCountFn dava_live = reinterpret_cast<DavaCountFn>(
        GetProcAddress(dava_stub, "WotbModLoader_DavaTestLiveCount"));
    const DavaCountFn dava_created = reinterpret_cast<DavaCountFn>(
        GetProcAddress(dava_stub, "WotbModLoader_DavaTestCreateCount"));
    const DavaCountFn dava_mutations = reinterpret_cast<DavaCountFn>(
        GetProcAddress(dava_stub, "WotbModLoader_DavaTestMutationCount"));
    const DavaCountFn dava_swaps = reinterpret_cast<DavaCountFn>(
        GetProcAddress(dava_stub, "WotbModLoader_DavaTestSwapCount"));
    const DavaCountFn dava_tracers = reinterpret_cast<DavaCountFn>(
        GetProcAddress(dava_stub, "WotbModLoader_DavaTestTracerCount"));
    const DavaCountFn dava_releases = reinterpret_cast<DavaCountFn>(
        GetProcAddress(dava_stub, "WotbModLoader_DavaTestReleaseCount"));
    const DavaCreateScriptFn dava_create_script =
        reinterpret_cast<DavaCreateScriptFn>(GetProcAddress(
            module, "WotbLuaHost_CreateScriptForTests"));
    const DavaEvalFn dava_eval = reinterpret_cast<DavaEvalFn>(GetProcAddress(
        module, "WotbLuaHost_EvalInScriptForTests"));
    const DavaDestroyScriptFn dava_destroy_script =
        reinterpret_cast<DavaDestroyScriptFn>(GetProcAddress(
            module, "WotbLuaHost_DestroyScriptForTests"));
    Check(dava_capabilities && dava_capabilities() == 63u,
          "the DAVA bridge stub advertises all six reviewed groups");
    Check(dava_reset && dava_live && dava_created && dava_mutations &&
              dava_swaps && dava_tracers && dava_releases &&
              dava_create_script && dava_eval && dava_destroy_script,
          "the DAVA bridge stub publishes its witnesses");
    if (dava_reset && dava_live && dava_created && dava_mutations &&
        dava_swaps && dava_tracers && dava_releases &&
        dava_create_script && dava_eval && dava_destroy_script) {
        MockAbi::Reset();
        MockAbi::SetHostPermissions(
            {"core", "events.public", "storage", "ui.modify.game",
             "ui.create", "ui.modify.own", "battle.ui", "input",
             "gameplay.tweak.cosmetic", "resources.mod"});
        Check(entry(&bootstrap, 1u, &info) == WOTBMOD_V3_OK,
              "the DAVA Lua test measures its reviewed native permissions");
        dava_reset();
        char message[1024] = {};
        void* dava_script = dava_create_script(
            "dava-main-dispatch",
            "if type(wotb.dava) ~= 'table' then error('no dava table') end\n"
            "local caps = wotb.dava.capabilities()\n"
            "if caps ~= 63 then error('caps=' .. tostring(caps)) end\n"
            "for _, name in ipairs({'yaml','archive','material','mesh',"
            "'stock_tracer','class_factory'}) do\n"
            "  if wotb.dava.is_supported(name) ~= true then error(name) end\n"
            "end\n"
            "local direct, direct_err = wotb.dava.create_material('unsafe')\n"
            "if direct ~= nil or not tostring(direct_err):find('run_on_main', "
            "1, true) then error('unsafe direct call was not fenced') end\n"
            "dava_done = false\n"
            "assert(wotb.dava.run_on_main(function()\n"
            "if not wotb.dava.class_is_registered('DAVA::UIControl') then "
            "error('reviewed class missing') end\n"
            "if wotb.dava.class_is_registered('DAVA::Unreviewed') then "
            "error('arbitrary class exposed') end\n"
            "local rejected, reject_err = wotb.dava.class_create("
            "'DAVA::Unreviewed', wotb.dava.OBJECT_CLASS_INSTANCE, 'x')\n"
            "if rejected ~= nil or type(reject_err) ~= 'string' then "
            "error('unreviewed class accepted') end\n"
            "local instance = assert(wotb.dava.class_create("
            "'DAVA::UIControl', wotb.dava.OBJECT_CLASS_INSTANCE, 'probe'))\n"
            "local material = assert(wotb.dava.create_material('lua.probe'))\n"
            "local texture = assert(wotb.dava.create_texture("
            "'mod://fixtures/probe.tex'))\n"
            "local mesh = assert(wotb.dava.create_mesh("
            "'mod://fixtures/replacement.sc2'))\n"
            "local consumer = assert(wotb.dava.create_mesh_consumer("
            "'mod://fixtures/base.sc2'))\n"
            "assert(wotb.dava.material_set_property(material, 'roughness', 0.5))\n"
            "assert(wotb.dava.material_set_property(material, 'tint', "
            "{1, 0.5, 0.25, 1}, 'float4'))\n"
            "assert(wotb.dava.material_set_flag(material, 'BLENDING', 1))\n"
            "assert(wotb.dava.material_set_texture(material, 'albedo', texture))\n"
            "assert(wotb.dava.material_set_fx(material, 'NormalizedBlinnPhong'))\n"
            "assert(wotb.dava.material_set_quality(material, 'High'))\n"
            "assert(wotb.dava.material_apply(material, consumer))\n"
            "assert(wotb.dava.mesh_hot_swap(consumer, mesh))\n"
            "local wrong, wrong_err = wotb.dava.mesh_hot_swap(mesh, consumer)\n"
            "if wrong ~= nil or type(wrong_err) ~= 'string' then "
            "error('typed mesh fence failed') end\n"
            "local tracer = assert(wotb.dava.create_stock_tracer({"
            "origin={0,1,2}, destination={10,3,4}, shell_type=2}))\n"
            "assert(wotb.dava.material_remove_texture(material, 'albedo'))\n"
            "assert(wotb.dava.material_remove_flag(material, 'BLENDING'))\n"
            "assert(wotb.dava.material_remove_property(material, 'roughness'))\n"
            "for _, object in ipairs({tracer, mesh, consumer, texture, "
            "material, instance}) do assert(wotb.dava.release(object)) end\n"
            "dava_done = true\n"
            "end))",
            message, sizeof(message));
        Check(dava_script != nullptr,
              "the DAVA script queues its native work without running it inline");
        Check(dava_created() == 0u && MockAbi::main_dispatches.size() == 1u,
              "DAVA work stays queued until the host pumps MAIN");
        const uint32_t dava_pumped = MockAbi::PumpMainDispatch();
        std::memset(message, 0, sizeof(message));
        const uint32_t status = dava_script
            ? dava_eval(dava_script,
                        "if dava_done ~= true then error('MAIN did not run') end",
                        message, sizeof(message))
            : 1u;
        if (status != 0u) std::printf("DAVA Lua error: %s\n", message);
        if (dava_script) dava_destroy_script(dava_script);
        Check(dava_pumped == 1u,
              "the test host executes exactly one queued DAVA MAIN callback");
        Check(status == 0u,
              "Lua can use every reviewed DAVA handle family and operation");
        Check(dava_created() == 6u && dava_live() == 0u &&
                  dava_releases() == 6u,
              "explicit Lua releases return every private DAVA object once");
        Check(dava_mutations() == 10u && dava_swaps() == 1u &&
                  dava_tracers() == 1u,
              "material, mesh and stock-tracer calls reach their typed bridge slots");
        Check(MockAbi::vfs_calls.size() == 3u,
              "texture and both mesh constructors resolve sandboxed URIs through VFS");
        Check(MockAbi::LiveGenericHandles() == 0u,
              "run_on_main releases its temporary task handle before native work");

        MockAbi::Reset();
        dava_reset();
        std::memset(message, 0, sizeof(message));
        void* leaked_script = dava_create_script(
            "dava-owner-cleanup",
            "leak_done = false\n"
            "assert(wotb.dava.run_on_main(function()\n"
            "material = assert(wotb.dava.create_material('leaked'))\n"
            "tracer = assert(wotb.dava.create_stock_tracer({"
            "origin={0,0,0}, destination={1,0,0}}))\n"
            "leak_done = true\n"
            "end))",
            message, sizeof(message));
        Check(leaked_script != nullptr && dava_created() == 0u,
              "automatic DAVA cleanup test also starts asynchronously");
        Check(MockAbi::PumpMainDispatch() == 1u,
              "automatic cleanup setup runs through MAIN");
        std::memset(message, 0, sizeof(message));
        const uint32_t leak_status = leaked_script
            ? dava_eval(leaked_script,
                        "if leak_done ~= true then error('MAIN did not run') end",
                        message, sizeof(message))
            : 1u;
        Check(leak_status == 0u,
              "a script may leave DAVA objects for automatic teardown");
        if (leaked_script) dava_destroy_script(leaked_script);
        Check(dava_created() == 2u && dava_releases() == 2u &&
                  dava_live() == 0u,
              "script teardown revokes every unreleased private DAVA handle");

        MockAbi::Reset();
        dava_reset();
        const std::string workshop = ReadModApiFile(
            L"examples\\lua_dava_workshop\\main.lua");
        Check(!workshop.empty(),
              "the checked-in Lua DAVA Workshop is readable");
        std::memset(message, 0, sizeof(message));
        void* workshop_script = workshop.empty()
            ? nullptr
            : dava_create_script("example.lua_dava_workshop",
                                 workshop.c_str(), message, sizeof(message));
        if (!workshop_script && message[0] != '\0') {
            std::printf("Lua DAVA Workshop compile error: %s\n", message);
        }
        Check(workshop_script != nullptr,
              "the exact shipped Lua DAVA Workshop compiles");
        if (workshop_script) dava_destroy_script(workshop_script);
    }
    MockAbi::SetHostPermissions(
        {"core", "events.public", "storage", "ui.modify.game",
         "ui.create", "ui.modify.own", "battle.ui", "input"});
    Check(entry(&bootstrap, 1u, &info) == WOTBMOD_V3_OK,
          "the DAVA Lua test restores the default permission ceiling");

    using CallGlobalFn = uint32_t(WOTBMOD_V3_CALL*)(
        const char*, const char*, char*, uint32_t);
    const CallGlobalFn call_global = reinterpret_cast<CallGlobalFn>(
        GetProcAddress(module, "WotbLuaHost_RunScriptCallGlobalForTests"));
    Check(call_global != nullptr, "the CallGlobal test entry point is exported");
    if (call_global) {
        char message[512] = {};

        Check(call_global("function on_enable() end", "on_enable",
                           message, sizeof(message)) == 0u,
              "CallGlobal succeeds when the function exists and returns");

        Check(call_global("", "on_enable", message, sizeof(message)) == 0u,
              "CallGlobal succeeds when the function is absent");

        std::memset(message, 0, sizeof(message));
        Check(call_global("function boom() error('nope') end", "boom",
                           message, sizeof(message)) == 2u,
              "CallGlobal reports failure when the called function raises");
        Check(std::strlen(message) > 0u,
              "a CallGlobal failure carries a message");

        // Critical regression: a script sets a metatable on _G with an
        // __index that raises, then a handler CallGlobal probes for is not
        // actually defined. Before the fix, CallGlobal's lua_getglobal ran
        // that __index metamethod outside any lua_pcall frame; its error
        // found no error jump and reached the panic handler, aborting the
        // whole test process (not a Check() failure - the process would
        // simply not get here). Reaching this Check() at all, with a clean
        // "absent is not a failure" result, is the regression test.
        std::memset(message, 0, sizeof(message));
        Check(call_global(
                  "setmetatable(_G, {__index = function() error('trap') end})",
                  "on_enable_not_defined", message, sizeof(message)) == 0u,
              "a trapping _G metatable cannot turn CallGlobal into an abort");
    }

    // Every script-code entry is bounded. The host's native on_frame is a
    // reload pump rather than a Lua lifecycle callback, but CallGlobal is the
    // single path for lifecycle globals and is probed under all three names so
    // a later on_frame lifecycle cannot accidentally bypass the same fence.
    // Each probe is a child with a hard deadline; removing the implementation
    // turns these into seven clean failures, not one wedged build process.
    char executable[MAX_PATH] = {};
    const DWORD executable_length =
        GetModuleFileNameA(nullptr, executable, MAX_PATH);
    Check(executable_length > 0u && executable_length < MAX_PATH,
          "the instruction-limit probes can resolve their test executable");
    if (executable_length > 0u && executable_length < MAX_PATH) {
        for (const char* mode : {"chunk", "pcall", "on_enable", "on_disable",
                                 "on_frame", "event", "loaded_event"}) {
            char label[160] = {};
            _snprintf_s(label, sizeof(label), _TRUNCATE,
                        "the instruction budget interrupts %s script code",
                        mode);
            Check(RunInstructionLimitProbe(executable, argv[1], mode), label);
        }
    }

    // ---- wotb.storage: all 14 slots of WotbModV3StorageApiV1 --------------
    //
    // Every script below runs through the same `run` (WotbLuaHost_RunScriptForTests)
    // used above, unmodified: LuaScript::Create now queries wotbmod.storage
    // off the bootstrap and calls RegisterStorage before the chunk's own
    // top-level body executes, so `wotb.storage.*` is reachable from a plain
    // one-line script, not only from inside a later on_enable.
    if (run) {
        MockAbi::Reset();
        char message[512] = {};

        // set_json - a plain command, PushResult.
        Check(run("wotb.storage.set_json('k', '{\"a\":1}')", message,
                  sizeof(message)) == 0u,
              "set_json runs");
        Check(MockAbi::Called(MockAbi::storage_calls,
                              "storage.set_json(k,{\"a\":1})"),
              "set_json reached the ABI with both arguments intact");

        // get_json - the sized-string dance, through FetchSizedString.
        std::memset(message, 0, sizeof(message));
        Check(run("local v = wotb.storage.get_json('k'); "
                  "if v ~= '{\"probe\":1}' then error('got ' .. tostring(v)) end",
                  message, sizeof(message)) == 0u,
              "get_json returns the string, sized buffer handled internally");

        // get_bytes - a raw byte buffer. The mock's canned value has a real
        // embedded zero ("AB\0CD", 5 bytes); reading back exactly 5 bytes,
        // including a zero at position 3, is the proof this path did not
        // truncate the way routing it through the string helper would have.
        std::memset(message, 0, sizeof(message));
        Check(run("local v = wotb.storage.get_bytes('k'); "
                  "if #v ~= 5 then error('length ' .. #v) end; "
                  "local b1,b2,b3,b4,b5 = string.byte(v, 1, 5); "
                  "if b1~=65 or b2~=66 or b3~=0 or b4~=67 or b5~=68 then "
                  "error('bytes wrong: ' .. b1 ..','.. b2 ..','.. b3 ..','.. "
                  "b4 ..','.. b5) end",
                  message, sizeof(message)) == 0u,
              "get_bytes returns all 5 bytes, including the embedded zero, "
              "byte-exact");
        Check(MockAbi::Called(MockAbi::storage_calls, "storage.get_bytes(k)"),
              "get_bytes reached the ABI");

        // set_bytes - the reverse direction, same embedded-zero proof. The
        // mock records the exact bytes it received; MockAbi::Called cannot
        // be used for the assertion (its const char* record parameter would
        // itself truncate at the embedded zero when built into a
        // std::string), so this compares the recorded entry directly as an
        // explicit-length std::string instead.
        std::memset(message, 0, sizeof(message));
        Check(run("wotb.storage.set_bytes('k2', string.char(65,66,0,67,68))",
                  message, sizeof(message)) == 0u,
              "set_bytes accepts a byte string with an embedded zero");
        {
            const std::string expected =
                std::string("storage.set_bytes(k2,") +
                std::string("AB\0CD", 5) + ")";
            Check(!MockAbi::storage_calls.empty() &&
                      MockAbi::storage_calls.back() == expected,
                  "set_bytes reached the ABI with the embedded zero byte "
                  "intact, not truncated at it");
        }

        // erase - a plain command.
        std::memset(message, 0, sizeof(message));
        Check(run("wotb.storage.erase('k')", message, sizeof(message)) == 0u,
              "erase runs");
        Check(MockAbi::Called(MockAbi::storage_calls, "storage.erase(k)"),
              "erase reached the ABI");

        // contains - the out-parameter boolean. Both a present and an
        // absent key must answer `true, <bool>`, never a bare boolean:
        // `false` for "not there" is a real, successful answer, so rule 2
        // ("falsy only on failure") is upheld by wrapping it exactly the way
        // an absent optional handle is wrapped.
        std::memset(message, 0, sizeof(message));
        Check(run("local ok, exists = wotb.storage.contains('present'); "
                  "if ok ~= true or exists ~= true then "
                  "error('present: ' .. tostring(ok) .. ',' .. tostring(exists)) end",
                  message, sizeof(message)) == 0u,
              "contains(present key) is true, true");
        std::memset(message, 0, sizeof(message));
        Check(run("local ok, exists = wotb.storage.contains('absent'); "
                  "if ok ~= true or exists ~= false then "
                  "error('absent: ' .. tostring(ok) .. ',' .. tostring(exists)) end",
                  message, sizeof(message)) == 0u,
              "contains(absent key) is true, false - not a bare falsy");
        std::memset(message, 0, sizeof(message));
        Check(run("if not wotb.storage.contains('absent') then "
                  "error('an absent key read as a failed call') end",
                  message, sizeof(message)) == 0u,
              "and `if not contains(key) then` never fires merely because "
              "the key is absent");
        Check(MockAbi::Called(MockAbi::storage_calls, "storage.contains(present)") &&
                  MockAbi::Called(MockAbi::storage_calls, "storage.contains(absent)"),
              "contains reached the ABI for both keys");

        // flush - a plain command with no arguments at all.
        std::memset(message, 0, sizeof(message));
        Check(run("wotb.storage.flush()", message, sizeof(message)) == 0u,
              "flush runs");
        Check(MockAbi::Called(MockAbi::storage_calls, "storage.flush()"),
              "flush reached the ABI");

        // begin_transaction, transaction_set_json, transaction_set_bytes,
        // transaction_erase, commit - the token family, boxed exactly like a
        // handle (kHandleStorageTransaction). The mock always returns 777;
        // seeing 777 at every downstream call is proof the same token a
        // script received round-trips through CheckHandle unchanged.
        std::memset(message, 0, sizeof(message));
        Check(run("local tx = wotb.storage.begin_transaction(); "
                  "if type(tx) ~= 'userdata' then "
                  "error('transaction is not a handle: ' .. type(tx)) end; "
                  "wotb.storage.transaction_set_json(tx, 'tk', '{\"a\":1}'); "
                  "wotb.storage.transaction_set_bytes(tx, 'tk2', 'bytes'); "
                  "wotb.storage.transaction_erase(tx, 'tk3'); "
                  "wotb.storage.commit(tx)",
                  message, sizeof(message)) == 0u,
              "a full transaction: begin, two writes, an erase, commit - all "
              "against a userdata token, not a number");
        Check(MockAbi::Called(MockAbi::storage_calls,
                              "storage.begin_transaction()"),
              "begin_transaction reached the ABI");
        Check(MockAbi::Called(MockAbi::storage_calls,
                              "storage.transaction_set_json(777,tk,{\"a\":1})"),
              "transaction_set_json carried the same token begin_transaction "
              "returned");
        Check(MockAbi::Called(MockAbi::storage_calls,
                              "storage.transaction_set_bytes(777,tk2,bytes)"),
              "transaction_set_bytes carried the same token");
        Check(MockAbi::Called(MockAbi::storage_calls,
                              "storage.transaction_erase(777,tk3)"),
              "transaction_erase carried the same token");
        Check(MockAbi::Called(MockAbi::storage_calls, "storage.commit(777)"),
              "commit carried the same token");

        // rollback - the transaction family's other terminal call.
        std::memset(message, 0, sizeof(message));
        Check(run("local tx = wotb.storage.begin_transaction(); "
                  "wotb.storage.rollback(tx)",
                  message, sizeof(message)) == 0u,
              "begin then rollback");
        Check(MockAbi::Called(MockAbi::storage_calls, "storage.rollback(777)"),
              "rollback reached the ABI with the token");

        // The point of boxing the token at all: a bare number must not pass
        // where a transaction handle belongs, the same way lua_convert_tests
        // proves for a control handle.
        std::memset(message, 0, sizeof(message));
        Check(run("local ok, err = wotb.storage.commit(777); "
                  "if ok ~= nil or type(err) ~= 'string' then "
                  "error('a bare number was accepted as a transaction') end",
                  message, sizeof(message)) == 0u,
              "commit refuses a bare number where a transaction handle "
              "belongs, even the numerically-correct one");

        // get_path - the sized-string dance's second call site, plus the
        // named PATH_* constants RegisterStorage installs alongside it.
        std::memset(message, 0, sizeof(message));
        Check(run("local p = wotb.storage.get_path(wotb.storage.PATH_CONFIG); "
                  "if p ~= 'C:/mods/config' then error('got ' .. tostring(p)) end",
                  message, sizeof(message)) == 0u,
              "get_path uses the named PATH_CONFIG constant and returns the "
              "sized string");
        Check(MockAbi::Called(MockAbi::storage_calls, "storage.get_path(2)"),
              "get_path reached the ABI with the numeric path kind");

        // get_path checks the *value*, not only the type: LUA_TNUMBER accepts
        // a float, and a bare lua_tointeger fails on a non-integral one
        // (F2Ieq, lvm.c) and hands back a 0 - so get_path(2.5) would arrive as
        // path_kind 0, indistinguishable from a script that asked for it.
        // Neither a non-integral number nor an integer outside the four named
        // constants may reach the ABI.
        std::memset(message, 0, sizeof(message));
        Check(run("local ok, e = wotb.storage.get_path(2.5); "
                  "if ok ~= nil then error('a float was accepted') end; "
                  "if not string.find(e, 'argument 1') then "
                  "error('missing argument index: ' .. tostring(e)) end",
                  message, sizeof(message)) == 0u,
              "get_path(2.5) is refused, not silently read as path_kind 0");
        std::memset(message, 0, sizeof(message));
        Check(run("local ok, e = wotb.storage.get_path(0); "
                  "if ok ~= nil then error('an out-of-range integer was "
                  "accepted') end",
                  message, sizeof(message)) == 0u,
              "get_path(0) is refused - an integer, but not one of the four "
              "legal path kinds");
        std::memset(message, 0, sizeof(message));
        Check(run("local ok, e = wotb.storage.get_path(5); "
                  "if ok ~= nil then error('an out-of-range integer was "
                  "accepted') end",
                  message, sizeof(message)) == 0u,
              "and neither is get_path(5), one past the last legal kind");
    }

    // ---- get_bytes's heap tier, retry loop and error messages, exercised -
    // through the host, not only through PushByteBuffer directly. Every
    // mock slot used to return OK unconditionally, so none of this ever ran
    // in this suite - MockAbi::SetBytesMode drives the sequence of
    // BUFFER_TOO_SMALL answers each scenario needs.
    if (run) {
        MockAbi::Reset();
        char message[512] = {};

        MockAbi::SetBytesMode(MockAbi::BytesMode::kLarge);
        Check(run("local v = wotb.storage.get_bytes('k'); "
                  "if #v ~= 4096 then error('length ' .. #v) end",
                  message, sizeof(message)) == 0u,
              "get_bytes past the inline capacity reaches the malloc/heap "
              "tier through the host");

        std::memset(message, 0, sizeof(message));
        MockAbi::SetBytesMode(MockAbi::BytesMode::kGrows);
        Check(run("local v = wotb.storage.get_bytes('k'); "
                  "if #v ~= 2500 then error('length ' .. #v) end",
                  message, sizeof(message)) == 0u,
              "a value that grows between the probe and the read is still "
              "read whole, through the host's own retry headroom");

        std::memset(message, 0, sizeof(message));
        MockAbi::SetBytesMode(MockAbi::BytesMode::kNeverStabilizes);
        Check(run("local v, e = wotb.storage.get_bytes('k'); "
                  "if v ~= nil then error('should have given up') end; "
                  "if not string.find(e, 'storage.get_bytes') then "
                  "error('missing context: ' .. tostring(e)) end",
                  message, sizeof(message)) == 0u,
              "a value that never stops growing gives up after four "
              "attempts instead of looping, with the slot's own context");

        std::memset(message, 0, sizeof(message));
        MockAbi::SetBytesMode(MockAbi::BytesMode::kRefusesSameSize);
        Check(run("local v, e = wotb.storage.get_bytes('k'); "
                  "if v ~= nil or type(e) ~= 'string' then "
                  "error('should have failed') end",
                  message, sizeof(message)) == 0u,
              "a client that refuses a buffer of the very size it asked for "
              "fails rather than looping on identical arguments");

        std::memset(message, 0, sizeof(message));
        MockAbi::SetBytesMode(MockAbi::BytesMode::kOversized);
        Check(run("local v, e = wotb.storage.get_bytes('k'); "
                  "if v ~= nil or type(e) ~= 'string' then "
                  "error('should have refused') end",
                  message, sizeof(message)) == 0u,
              "and a value past the size ceiling is refused rather than "
              "allocated");

        MockAbi::SetBytesMode(MockAbi::BytesMode::kNormal);
    }

    // ---- the failure switch: every slot's own failure path, not only its
    // success path. Before MockAbi::ForceFailure existed, every mock slot
    // returned OK unconditionally, so contains' speculative `true, <bool>`
    // push was never actually cut back by PushResultWith - only ever
    // observed on success, which cannot tell a correct cutback from one
    // that silently does nothing.
    if (run) {
        MockAbi::Reset();
        char message[512] = {};

        MockAbi::ForceFailure(WOTBMOD_V3_E_PERMISSION_DENIED);

        Check(run("local ok, e = wotb.storage.set_json('k', 'v'); "
                  "if ok ~= nil or type(e) ~= 'string' then "
                  "error('should have failed') end",
                  message, sizeof(message)) == 0u,
              "set_json's failure path: a plain command answers nil, "
              "message, not true, when the ABI itself refuses");

        std::memset(message, 0, sizeof(message));
        Check(run("local ok, exists = wotb.storage.contains('k'); "
                  "if ok ~= nil then "
                  "error('contains should fail, not answer true,false') "
                  "end; "
                  "if not string.find(exists, 'storage.contains') then "
                  "error('missing context: ' .. tostring(exists)) end",
                  message, sizeof(message)) == 0u,
              "and contains' speculative true,<bool> push really is cut "
              "back to nil,message on a genuine ABI failure, not merely "
              "unobserved");

        std::memset(message, 0, sizeof(message));
        Check(run("local tx, e = wotb.storage.begin_transaction(); "
                  "if tx ~= nil or type(e) ~= 'string' then "
                  "error('should have failed') end",
                  message, sizeof(message)) == 0u,
              "and begin_transaction's speculative PushToken push is cut "
              "back the same way - PushToken never nils a value, but "
              "PushResultWith still replaces it with nil on failure");

        MockAbi::ClearFailure();
    }

    // ---- begin_transaction with a zero-valued token: PushToken, not
    // PushHandle. A mock hard-coded to a nonzero token could never see
    // PushHandle's zero-is-invalid rule misapplied to a type that makes no
    // such promise.
    if (run) {
        MockAbi::Reset();
        MockAbi::SetTransactionToken(0u);
        char message[512] = {};
        Check(run("local tx = wotb.storage.begin_transaction(); "
                  "if tx == nil then error('token 0 read as nil') end; "
                  "if type(tx) ~= 'userdata' then "
                  "error('not a handle: ' .. type(tx)) end; "
                  "local ok = wotb.storage.commit(tx); "
                  "if ok ~= true then error('commit on token 0 failed') end",
                  message, sizeof(message)) == 0u,
              "a zero-valued transaction token is still a live handle, not "
              "nil, and round-trips through commit");
        Check(MockAbi::Called(MockAbi::storage_calls, "storage.commit(0)"),
              "and the value 0 itself reached the ABI unchanged");
        MockAbi::SetTransactionToken(777u);
    }

    // ---- print() routed at wotbmod.core's log, not OutputDebugStringA ----
    //
    // The carry-forward item from Task 3: LuaScript had no bootstrap/mod
    // handle when the sandbox was built, so print() fell back to
    // OutputDebugStringA. Now that Create() is handed the bootstrap this
    // test's MockAbi installed, print() must reach wotbmod.core's log slot
    // instead - checked by asking the mock what it recorded, not by reading
    // a debug stream this test has no handle to.
    if (run) {
        MockAbi::Reset();
        char message[512] = {};
        Check(run("print('hello from lua')", message, sizeof(message)) == 0u,
              "print runs");
        Check(MockAbi::Called(MockAbi::core_calls,
                              "core.log(2,lua,hello from lua)"),
              "print reached wotbmod.core's log (level 2 = "
              "WOTBMOD_V3_LOG_INFO), not OutputDebugStringA, now that the "
              "host can query a real interface");
    }

    // ---- print() and __tostring: the one Lua call inside print that runs ---
    // ---- arbitrary script code, and can raise out of it -------------------
    //
    // luaL_tolstring invokes the value's own __tostring. A metamethod that
    // errors, or that returns a non-string, raises out of print by longjmp,
    // and print used to hold a live std::string across that call. Not one of
    // these three shapes had a test - which is the actual defect, because
    // "does a raise here unwind cleanly" was never a measured property of
    // this host at all.
    if (run) {
        MockAbi::Reset();
        char message[512] = {};

        // 1. A __tostring that errors. The error must reach the script as an
        //    ordinary catchable Lua error, and the host must still be usable
        //    afterwards - which the print() after the pcall is what proves.
        Check(run("local bomb = setmetatable({}, "
                  "{__tostring = function() error('boom') end})\n"
                  "local ok, err = pcall(print, 'before', bomb)\n"
                  "if ok then error('a raising __tostring should have "
                  "propagated out of print') end\n"
                  "if type(err) ~= 'string' then error('no message') end\n"
                  "print('still alive')",
                  message, sizeof(message)) == 0u,
              "a __tostring that errors raises out of print as a catchable "
              "Lua error rather than taking the host with it");
        Check(MockAbi::Called(MockAbi::core_calls, "core.log(2,lua,still alive)"),
              "and print still works afterwards, so the raise unwound the VM "
              "rather than corrupting it");
        Check(!MockAbi::CalledContaining(MockAbi::core_calls, "before"),
              "and the half-built line was never logged: print emits one "
              "record or none, never a truncated one");

        // 2. A __tostring that returns a non-string. luaL_tolstring's own
        //    refusal, and the second way this call raises.
        MockAbi::Reset();
        std::memset(message, 0, sizeof(message));
        Check(run("local bad = setmetatable({}, "
                  "{__tostring = function() return {} end})\n"
                  "local ok, err = pcall(print, bad)\n"
                  "if ok then error('a __tostring returning a table should "
                  "have raised') end\n"
                  "if not string.find(tostring(err), '__tostring') then "
                  "error('unexpected message: ' .. tostring(err)) end",
                  message, sizeof(message)) == 0u,
              "a __tostring that returns a non-string raises the same "
              "recoverable way");

        // 3. The well-behaved case, which is what says the rewrite still
        //    honours __tostring at all rather than sidestepping it - and that
        //    the tab separator survived moving the assembly onto the Lua
        //    stack.
        MockAbi::Reset();
        std::memset(message, 0, sizeof(message));
        Check(run("local good = setmetatable({}, "
                  "{__tostring = function() return 'GOOD' end})\n"
                  "print(good, 'tail')",
                  message, sizeof(message)) == 0u,
              "a well-behaved __tostring runs");
        Check(MockAbi::Called(MockAbi::core_calls, "core.log(2,lua,GOOD\ttail)"),
              "and its result is what print logged, tab-separated from the "
              "next argument");

        // 4. What a raise here costs, measured rather than argued - and this
        //    check is why the branch's story about it is now right. It was
        //    written expecting to fail against the old, std::string-holding
        //    print(), on the reasoning that a longjmp runs no C++ destructor.
        //    It passed. MSVC's x86 longjmp does unwind the /EHsc frames it
        //    passes: a scratch probe at build.cmd's own flags reports
        //    dtor_ran=1, while a deliberate 400 x 64 KB control leak shows up
        //    through this very counter as 26.5 MB, so the check is sensitive
        //    and the leak simply was not there. Task S3's dtor_ran=0 measured
        //    an SEH unwind to an __except, which is a different mechanism.
        //
        //    It stays because the property it pins - a script can raise out
        //    of print all day and the host gives nothing back - is one this
        //    suite should own rather than inherit from a compiler switch. The
        //    script never reaches core.log on any turn (the raise happens on
        //    argument two), so the mock's own call vector does not grow and
        //    cannot be mistaken for a leak.
        MockAbi::Reset();
        std::memset(message, 0, sizeof(message));
        const char* kBomb =
            "local big = string.rep('x', 65536)\n"
            "local bomb = setmetatable({}, "
            "{__tostring = function() error('boom') end})\n"
            "for i = 1, 400 do pcall(print, big, bomb) end";
        // One warm-up run first, so what is measured is the steady state
        // rather than the allocator's and the VM's own first-time growth.
        Check(run(kBomb, message, sizeof(message)) == 0u,
              "400 print() calls whose __tostring raises all report cleanly");
        PROCESS_MEMORY_COUNTERS before = {};
        before.cb = sizeof(before);
        GetProcessMemoryInfo(GetCurrentProcess(), &before, sizeof(before));
        std::memset(message, 0, sizeof(message));
        Check(run(kBomb, message, sizeof(message)) == 0u,
              "and again, this time measured");
        PROCESS_MEMORY_COUNTERS after = {};
        after.cb = sizeof(after);
        GetProcessMemoryInfo(GetCurrentProcess(), &after, sizeof(after));
        const SIZE_T grew = after.PagefileUsage > before.PagefileUsage
                                ? after.PagefileUsage - before.PagefileUsage
                                : 0u;
        // 25.6 MB is what leaks if the string is held across the raise. The
        // threshold is a third of that, which is far above any plausible
        // allocator noise and far below the defect.
        Check(grew < 8u * 1024u * 1024u,
              "and none of it leaked: a raising __tostring abandons no "
              "host-side allocation, because print holds none across the call");
        if (grew >= 8u * 1024u * 1024u) {
            std::fprintf(stderr, "    private bytes grew by %llu\n",
                         static_cast<unsigned long long>(grew));
        }
    }

    // ---- wotb.events: all 9 slots of WotbModV3EventsApiV1 ------------------
    //
    // The only interface where the client calls into Lua. Every script here
    // is created through WotbLuaHost_CreateScriptForTests and outlives the
    // call that made it, because the event has to be fired while the script
    // is still alive and the state then read back afterwards - three
    // separate moments that the run() shim used above collapses into one.
    using CreateScriptFn = void*(WOTBMOD_V3_CALL*)(
        const char*, const char*, char*, uint32_t);
    using EvalFn = uint32_t(WOTBMOD_V3_CALL*)(
        void*, const char*, char*, uint32_t);
    using DestroyScriptFn = void(WOTBMOD_V3_CALL*)(void*);
    using RegistrySlotsFn = uint32_t(WOTBMOD_V3_CALL*)(void*);
    using GeneratedOwnedCountFn = uint32_t(WOTBMOD_V3_CALL*)(void*);
    using CountFn = uint32_t(WOTBMOD_V3_CALL*)();
    const auto create_script = reinterpret_cast<CreateScriptFn>(
        GetProcAddress(module, "WotbLuaHost_CreateScriptForTests"));
    const auto eval = reinterpret_cast<EvalFn>(
        GetProcAddress(module, "WotbLuaHost_EvalInScriptForTests"));
    const auto destroy_script = reinterpret_cast<DestroyScriptFn>(
        GetProcAddress(module, "WotbLuaHost_DestroyScriptForTests"));
    const auto registry_slots = reinterpret_cast<RegistrySlotsFn>(
        GetProcAddress(module, "WotbLuaHost_RegistrySlotsForTests"));
    const auto generated_owned_count = reinterpret_cast<GeneratedOwnedCountFn>(
        GetProcAddress(module, "WotbLuaHost_OwnedCountForTests"));
    const auto input_cursor_owners = reinterpret_cast<CountFn>(
        GetProcAddress(module, "WotbLuaHost_InputCursorOwnersForTests"));
    Check(create_script != nullptr && eval != nullptr &&
               destroy_script != nullptr && registry_slots != nullptr &&
               generated_owned_count != nullptr &&
               input_cursor_owners != nullptr,
           "the persistent-script test entry points are exported");

    if (create_script && eval && destroy_script && registry_slots &&
        generated_owned_count && input_cursor_owners) {
        char message[512] = {};

        // wotb.ges: a GES subscription rides on wotb.events, the handler gets
        // a delivery-scoped event object, and publish lays the payload out
        // from the schema.
        {
            MockAbi::Reset();
            // The ceiling is measured at entry: grant the two ges.* names on
            // top of the mock's defaults and measure again, then put the
            // defaults back the same way when this block is done.
            MockAbi::SetHostPermissions(
                {"core", "events.public", "storage", "ui.modify.game",
                 "ui.create", "ui.modify.own", "battle.ui", "input",
                 "ges.observe", "ges.publish"});
            Check(entry(&bootstrap, 1u, &info) == WOTBMOD_V3_OK,
                  "the host re-measures a ceiling that grants ges.*");
            std::memset(message, 0, sizeof(message));
            void* ges_script = create_script(
                "ges-probe",
                "count = 0\n"
                "names = wotb.ges.types()\n"
                "h = wotb.ges.subscribe('Avatar.*', function(ev)\n"
                "  count = count + 1\n"
                "  last_type = ev.type\n"
                "  last_size = ev.size\n"
                "  last_rva = ev.publisher_rva\n"
                "  last_mode = ev:i32(0)\n"
                "  last_flag = ev:bool(4)\n"
                "  last_field = ev:field('mode')\n"
                "  last_field_flag = ev:field('flag')\n"
                "  schema_fields = ev.schema and #ev.schema or -1\n"
                "  schema_first = ev.schema and ev.schema[1].name or ''\n"
                "  was_expired = ev:expired()\n"
                "  stale = ev\n"
                "end)\n"
                "ok, err = wotb.ges.publish('Avatar::CameraModeChanged', "
                "{ mode = 5, flag = true })\n"
                "bad, bad_err = wotb.ges.publish('Avatar::Nope', {})\n"
                "short, short_err = wotb.ges.publish("
                "'Avatar::CameraModeChanged', { mode = 1 })\n",
                message, sizeof(message));
            Check(ges_script != nullptr, "a wotb.ges script compiles and runs");
            if (ges_script) {
                Check(MockAbi::LiveSubscriptions() == 1u &&
                          MockAbi::event_subscriptions.size() == 1u &&
                          MockAbi::event_subscriptions[0].pattern ==
                              "wotbmod.ges.Avatar.*" &&
                          MockAbi::event_subscriptions[0]
                                  .receive_system_events == 1u,
                      "ges.subscribe becomes an events subscription on the "
                      "wotbmod.ges.* topic that receives system events");
                std::memset(message, 0, sizeof(message));
                Check(eval(ges_script,
                           "if h == nil then error('no handle') end\n"
                           "if #names ~= 2 or names[1] ~= "
                           "'Avatar::CameraModeChanged' then "
                           "error('types: ' .. #names) end\n"
                           "if ok ~= true then error('publish: ' .. "
                           "tostring(err)) end\n"
                           "if bad ~= nil or bad_err == nil then "
                           "error('unknown type published') end\n"
                           "if short ~= nil or not string.find(short_err, "
                           "'flag') then error('missing field accepted: ' .. "
                           "tostring(short_err)) end\n",
                           message, sizeof(message)) == 0u,
                      message);
                Check(MockAbi::CountCalled(MockAbi::ges_calls,
                                           "ges.publish(Avatar::"
                                           "CameraModeChanged,8,0)") == 1u &&
                          MockAbi::ges_last_published.size() == 8u &&
                          MockAbi::ges_last_published[0] == 5u &&
                          MockAbi::ges_last_published[4] == 1u,
                      "ges.publish lays the schema fields out at their "
                      "offsets and publishes once");

                struct {
                    int32_t mode;
                    uint8_t flag;
                    uint8_t pad[3];
                } payload = {3, 1, {0u, 0u, 0u}};
                MockAbi::FireGesEvent("Avatar::CameraModeChanged", &payload,
                                      8u, 1u, 0x1234u);
                std::memset(message, 0, sizeof(message));
                Check(eval(ges_script,
                           "if count ~= 1 then error('count ' .. count) end\n"
                           "if last_type ~= 'Avatar::CameraModeChanged' then "
                           "error('type ' .. tostring(last_type)) end\n"
                           "if last_size ~= 8 or last_rva ~= 0x1234 then "
                           "error('size/rva') end\n"
                           "if last_mode ~= 3 or last_field ~= 3 then "
                           "error('mode ' .. tostring(last_mode)) end\n"
                           "if last_flag ~= true or last_field_flag ~= true "
                           "then error('flag') end\n"
                           "if schema_fields ~= 2 or schema_first ~= 'mode' "
                           "then error('schema') end\n"
                           "if was_expired then error('expired inside') end\n"
                           "if not stale:expired() then "
                           "error('still live') end\n",
                           message, sizeof(message)) == 0u,
                      message);

                MockAbi::FireGesEvent("Lobby::Survey::Accepted", &payload, 8u,
                                      0u);
                std::memset(message, 0, sizeof(message));
                Check(eval(ges_script,
                           "if count ~= 1 then error('pattern leaked') end",
                           message, sizeof(message)) == 0u,
                      "a type outside the pattern is not delivered");

                std::memset(message, 0, sizeof(message));
                Check(eval(ges_script, "return stale:i32(0)", message,
                           sizeof(message)) != 0u &&
                          std::strstr(message, "ges event expired") != nullptr,
                      "a stashed event object refuses reads after delivery");

                std::memset(message, 0, sizeof(message));
                Check(eval(ges_script,
                           "local ok2, e2 = wotb.ges.unsubscribe(h)\n"
                           "if not ok2 then error(tostring(e2)) end",
                           message, sizeof(message)) == 0u &&
                          MockAbi::LiveSubscriptions() == 0u,
                      "ges.unsubscribe releases the events subscription");
                destroy_script(ges_script);
            }
            MockAbi::Reset();
            Check(entry(&bootstrap, 1u, &info) == WOTBMOD_V3_OK,
                  "the default ceiling is measured again after the ges probe");
        }

        // ---- host-local input helpers: F8 edge and cursor ownership -------
        Check(input_cursor_owners() == 0u,
              "no script owns cursor release before the input probe");
        void* first_cursor_script = create_script(
            "cursor-owner-one",
            "if wotb.input.KEY_F8 ~= 0x77 then error('F8') end\n"
            "if type(wotb.input.hotkey_down(0x77)) ~= 'boolean' then "
            "error('down') end\n"
            "if type(wotb.input.hotkey_pressed(0x77)) ~= 'boolean' then "
            "error('pressed') end",
            message, sizeof(message));
        void* second_cursor_script = create_script(
            "cursor-owner-two", "", message, sizeof(message));
        Check(first_cursor_script && second_cursor_script,
              "two scripts can load the guarded input helper surface");
        if (first_cursor_script && second_cursor_script) {
            Check(eval(first_cursor_script,
                       "assert(wotb.input.set_cursor_unlocked(true)); "
                       "assert(wotb.input.cursor_unlocked())",
                       message, sizeof(message)) == 0u &&
                      input_cursor_owners() == 1u,
                  "one script can own cursor release");
            Check(eval(second_cursor_script,
                       "assert(wotb.input.set_cursor_unlocked(true))",
                       message, sizeof(message)) == 0u &&
                      input_cursor_owners() == 2u,
                  "cursor release is reference-counted across scripts");
            destroy_script(first_cursor_script);
            first_cursor_script = nullptr;
            Check(input_cursor_owners() == 1u,
                  "destroying one owner keeps the remaining owner active");
            destroy_script(second_cursor_script);
            second_cursor_script = nullptr;
            Check(input_cursor_owners() == 0u,
                  "script teardown cannot leave the mouse unlocked");
        }
        if (first_cursor_script) destroy_script(first_cursor_script);
        if (second_cursor_script) destroy_script(second_cursor_script);

        // ---- generated callback: native delivery and teardown ------------
        MockAbi::Reset();
        void* generated_script = create_script(
            "generated-callback",
            "seen_capability = nil\n"
            "capability_subscription = wotb.capabilities.subscribe(function(c)\n"
            "  seen_capability = c\n"
            "end)",
            message, sizeof(message));
        Check(generated_script != nullptr,
              "a generated callback binding creates a persistent script");
        Check(MockAbi::LiveCapabilitySubscriptions() == 1u,
              "the client holds the generated callback subscription");
        if (generated_script) {
            MockAbi::FireCapability();
            std::memset(message, 0, sizeof(message));
            Check(eval(generated_script,
                       "local c = seen_capability\n"
                       "if type(c) ~= 'table' then error('no callback table') end\n"
                       "if c.name ~= 'mock.capability' then "
                       "error('name ' .. tostring(c.name)) end\n"
                       "if c.interface_version ~= 7 then "
                       "error('version ' .. tostring(c.interface_version)) end\n"
                       "if c.allowed_contexts ~= 0x52 then "
                       "error('contexts ' .. tostring(c.allowed_contexts)) end",
                       message, sizeof(message)) == 0u,
                  "the generated native callback reaches Lua with the exact "
                  "aggregate fields supplied by the client");
            destroy_script(generated_script);
            generated_script = nullptr;
        }
        Check(MockAbi::LiveCapabilitySubscriptions() == 0u,
              "destroying the script releases every generated callback token");
        Check(MockAbi::Called(MockAbi::handles_calls,
                              "handles.release(9800)"),
              "generated callback teardown used the generic handles API on "
              "the exact token returned by subscribe");
        MockAbi::FireCapability();

        // ---- generated owned handles: output, retain, explicit release ----
        MockAbi::Reset();
        std::memset(message, 0, sizeof(message));
        void* owned_script = create_script(
            "generated-ownership",
            "owned = wotb.lifecycle.get_current()\n"
            "local ok = wotb.handles.retain(owned)\n"
            "if ok ~= true then error('retain failed') end",
            message, sizeof(message));
        Check(owned_script != nullptr,
              "a generated owned-handle output and retain both run");
        if (owned_script) {
            Check(generated_owned_count(owned_script) == 2u,
                  "the ownership ledger stores one entry per generated "
                  "reference, including retain");
            Check(MockAbi::GenericHandleReferences(9900u) == 2u,
                  "the client and Lua ownership ledger agree on both refs");
            destroy_script(owned_script);
        }
        Check(MockAbi::GenericHandleReferences(9900u) == 0u,
              "script teardown releases every generated handle reference");
        Check(MockAbi::CountCalled(MockAbi::handles_calls,
                                   "handles.release(9900)") == 2u,
              "teardown performs one generic release per owned reference");

        MockAbi::Reset();
        std::memset(message, 0, sizeof(message));
        owned_script = create_script(
            "generated-explicit-release",
            "owned = wotb.lifecycle.get_current()\n"
            "local ok = wotb.handles.release(owned)\n"
            "if ok ~= true then error('release failed') end",
            message, sizeof(message));
        Check(owned_script != nullptr,
              "a generated resource can be released explicitly");
        if (owned_script) {
            Check(generated_owned_count(owned_script) == 0u,
                  "successful generated release forgets the exact handle");
            destroy_script(owned_script);
        }
        Check(MockAbi::GenericHandleReferences(9900u) == 0u,
              "the explicit generated release reached the client");
        Check(MockAbi::CountCalled(MockAbi::handles_calls,
                                   "handles.release(9900)") == 1u,
              "only the script's explicit release ran; teardown did not "
              "release the forgotten handle again");

        // Generated UI getters return a generic generated handle. The
        // hand-written V2 UI subset must accept that same handle so callers
        // can immediately inspect the active screen it just returned.
        MockAbi::Reset();
        std::memset(message, 0, sizeof(message));
        void* ui_handle_script = create_script(
            "ui-generated-handle",
            "local screen = wotb.ui.get_active_screen()\n"
            "local width, height = wotb.ui.control_get_size(screen)\n"
            "if width ~= 1920 or height ~= 1080 then error('bad size') end\n"
            "local ok, alive = wotb.ui.control_is_alive(screen)\n"
            "if ok ~= true or alive ~= true then error('not alive') end\n"
            "local found, find_err = "
            "wotb.ui.control_find_by_id(screen, 'missing')\n"
            "if found ~= nil or not string.find(find_err, "
            "'ui.control_find_by_id') then error('bad find result') end",
            message, sizeof(message));
        Check(ui_handle_script != nullptr,
              "hand-written UI slots accept the generated active-screen "
              "handle");
        if (ui_handle_script) destroy_script(ui_handle_script);
        Check(MockAbi::GenericHandleReferences(6000u) == 0u,
              "the generated active-screen reference is released after the "
              "mixed binding call");

        // A UI callback is allowed to replace itself. Delivery must use the
        // subscriptions that existed when dispatch started: the callback it
        // adds is for the next event, and mutating the backing vector cannot
        // invalidate the active iteration.
        MockAbi::Reset();
        std::memset(message, 0, sizeof(message));
        void* ui_reentry_script = create_script(
            "ui-event-reentry",
            "button = wotb.ui.control_create()\n"
            "calls = 0\n"
            "first = nil\n"
            "second = nil\n"
            "first = button:on(wotb.ui.EVENT_CLICK, function()\n"
            "  calls = calls + 1\n"
            "  local ok, err = wotb.ui.event_unsubscribe(first)\n"
            "  if not ok then error('unsubscribe: ' .. tostring(err)) end\n"
            "  second, err = button:on(wotb.ui.EVENT_CLICK, function()\n"
            "    calls = calls + 10\n"
            "  end)\n"
            "  if second == nil then "
            "error('subscribe: ' .. tostring(err)) end\n"
            "end)",
            message, sizeof(message));
        Check(ui_reentry_script != nullptr,
              "a UI callback that replaces its own subscription compiles");
        if (ui_reentry_script) {
            MockAbi::FireUiEventForControl(6000u);
            Check(eval(ui_reentry_script,
                       "if calls ~= 1 then error('calls ' .. calls) end",
                       message, sizeof(message)) == 0u &&
                      MockAbi::LiveUiEventSubscriptions() == 1u,
                  "UI delivery survives self-unsubscribe plus subscribe and "
                  "does not invoke the new callback in the same dispatch");
            MockAbi::FireUiEvent();
            Check(eval(ui_reentry_script,
                       "if calls ~= 11 then error('calls ' .. calls) end",
                       message, sizeof(message)) == 0u,
                  "the replacement UI callback receives the next dispatch");
            destroy_script(ui_reentry_script);
        }
        Check(MockAbi::LiveControls() == 0u &&
                  MockAbi::LiveUiEventSubscriptions() == 0u,
              "UI callback re-entry leaves no control or subscription live");

        // ---- shipped Lua UI example: exact file, render, cleanup ---------
        MockAbi::Reset();
        MockAbi::publish_core_context = true;
        MockAbi::core_context_mask = WOTBMOD_V3_CONTEXT_BATTLE;
        MockAbi::event_context_mask = WOTBMOD_V3_CONTEXT_BATTLE;
        std::memset(message, 0, sizeof(message));
        const std::string ui_example = ReadModApiFile(
            L"examples\\lua_ui_framework\\main.lua");
        Check(!ui_example.empty(),
              "the checked-in Lua UI Framework example is readable");
        void* ui_example_script = ui_example.empty()
            ? nullptr
            : create_script("example.lua_ui_framework", ui_example.c_str(),
                            message, sizeof(message));
        Check(ui_example_script != nullptr,
              "the exact shipped Lua UI Framework example compiles");
        if (ui_example_script) {
            std::memset(message, 0, sizeof(message));
            Check(eval(ui_example_script,
                       "on_enable(); on_frame(120, 0.016)",
                       message, sizeof(message)) == 0u,
                  "the shipped example mounts and survives its frame probe");
            Check(MockAbi::LiveControls() == 35u,
                   "the example builds all 35 runtime controls from Lua");
            Check(MockAbi::CountCalled(MockAbi::ui_calls,
                                       "ui.get_active_screen()") == 2u,
                  "the UI example performs its periodic attachment recovery "
                  "probe after the initial mount");
            Check(MockAbi::LiveSubscriptions() == 5u &&
                      MockAbi::Called(
                          MockAbi::events_calls,
                          "events.subscribe(wotbmod.ui.screen_changed,0,1)"),
                  "the example follows active-screen and battle-context "
                  "changes");
            Check(MockAbi::LiveUiEventSubscriptions() == 17u,
                   "the example owns every button, list-row and slider "
                   "callback it installed");
            MockAbi::UiControlRecord* runtime_root =
                MockAbi::FindLatestUiControlById("RuntimeRoot");
            const WotbModV3UiHandle runtime_root_handle =
                runtime_root ? runtime_root->handle : 0u;
            const WotbModV3UiHandle first_screen_handle =
                runtime_root ? runtime_root->parent : 0u;
            MockAbi::UiControlRecord* open_button =
                MockAbi::FindLatestUiControlById("OpenButton");
            MockAbi::UiControlRecord* window =
                MockAbi::FindLatestUiControlById("Window");
            MockAbi::UiControlRecord* row_template =
                MockAbi::FindLatestUiControlById("PlayerRowTemplate");
            MockAbi::UiControlRecord* first_row =
                MockAbi::FindLatestUiControlById("PlayerRow1");
            MockAbi::UiControlRecord* last_row =
                MockAbi::FindLatestUiControlById("PlayerRow8");
            Check(runtime_root && runtime_root->texture_uri.empty() &&
                      runtime_root->parent == 6000u,
                  "the runtime root is attached to the exact active screen "
                  "without requesting an author template");
            Check(runtime_root && runtime_root->size.x == 1536.0f &&
                      runtime_root->size.y == 864.0f && open_button &&
                      open_button->position.x == 1206.0f && window &&
                      window->position.x == 268.0f &&
                      window->position.y == 52.0f,
                  "the runtime tree lays out against the physical host "
                  "viewport, not the logical active-screen size");
            Check(row_template && first_row && last_row &&
                      row_template->handle != first_row->handle &&
                      first_row->handle != last_row->handle &&
                      first_row->text == "01  Lumedas_" &&
                      last_row->text == "08  RuntimeUI",
                  "the list contains eight independently cloned and "
                  "runtime-populated rows");
            Check(open_button && open_button->visible &&
                      window && !window->visible,
                  "the launcher starts visible while the showcase window is "
                  "closed");

            if (open_button) {
                MockAbi::FireUiEventForControl(open_button->handle);
            }
            Check(open_button && !open_button->visible &&
                      window && window->visible,
                  "clicking the launcher opens the Lua-created window");

            MockAbi::UiControlRecord* volume_track =
                MockAbi::FindLatestUiControlById("VolumeTrack");
            MockAbi::UiControlRecord* volume_fill =
                MockAbi::FindLatestUiControlById("VolumeFill");
            MockAbi::UiControlRecord* volume_knob =
                MockAbi::FindLatestUiControlById("VolumeKnob");
            if (volume_track) {
                MockAbi::FireUiEventForControl(
                    volume_track->handle,
                    WOTBMOD_V3_UI_EVENT_POINTER_DOWN,
                    {538.0f, 240.0f});
            }
            MockAbi::UiControlRecord* volume_label =
                MockAbi::FindLatestUiControlById("VolumeLabel");
            Check(volume_fill && volume_fill->size.x == 210.0f &&
                      volume_knob && volume_knob->position.x == 257.0f &&
                      volume_label && volume_label->text ==
                          "Громкость: 50%",
                  "pointer input moves the runtime slider and immediately "
                  "changes native text through the object API");

            MockAbi::UiControlRecord* toggle_button =
                MockAbi::FindLatestUiControlById("ToggleButton");
            if (toggle_button) {
                MockAbi::FireUiEventForControl(toggle_button->handle);
            }
            Check(toggle_button && toggle_button->text == "ВЫКЛЮЧЕНО",
                  "the toggle click changes its native caption at runtime");

            MockAbi::UiControlRecord* action_button =
                MockAbi::FindLatestUiControlById("ActionButton");
            MockAbi::UiControlRecord* runtime_status =
                MockAbi::FindLatestUiControlById("RuntimeStatus");
            if (action_button) {
                MockAbi::FireUiEventForControl(action_button->handle);
            }
            Check(runtime_status && runtime_status->text ==
                      "Громкость: 50% · Масштаб: 70%" &&
                      runtime_status->color.r == 0.95f,
                  "the action button mutates text and color on an existing "
                  "UIStaticText");

            if (first_row) {
                MockAbi::FireUiEventForControl(first_row->handle);
            }
            MockAbi::UiControlRecord* selected_player =
                MockAbi::FindLatestUiControlById("SelectedPlayer");
            Check(selected_player &&
                      selected_player->text == "Выбран игрок: Lumedas_",
                  "each cloned list row owns an independent click handler");

            MockAbi::UiControlRecord* animation_status =
                MockAbi::FindLatestUiControlById("AnimationStatus");
            MockAbi::UiControlRecord* demo_image =
                MockAbi::FindLatestUiControlById("DemoImage");
            std::memset(message, 0, sizeof(message));
            Check(eval(ui_example_script, "on_frame(180, 0.016)", message,
                       sizeof(message)) == 0u && animation_status &&
                      animation_status->opacity != 1.0f && demo_image &&
                      demo_image->position.y != 548.0f,
                  "the frame callback animates opacity and position through "
                  "runtime property setters");

            MockAbi::UiControlRecord* reset_button =
                MockAbi::FindLatestUiControlById("ResetButton");
            if (reset_button) {
                MockAbi::FireUiEventForControl(reset_button->handle);
            }
            Check(volume_fill && volume_fill->size.x == 147.0f &&
                      toggle_button && toggle_button->text == "ВКЛЮЧЕНО" &&
                      runtime_status && runtime_status->text ==
                          "Значения сброшены из Lua",
                  "reset restores slider, toggle and runtime text state");

            MockAbi::UiControlRecord* close_button =
                MockAbi::FindLatestUiControlById("CloseButton");
            if (close_button) {
                MockAbi::FireUiEventForControl(close_button->handle);
            }
            Check(open_button && open_button->visible &&
                      window && !window->visible,
                  "the close button returns to the active launcher");
            const std::string root_snapshot_call =
                std::string("ui.control_get_snapshot(") +
                std::to_string(runtime_root_handle) + ")";
            Check(MockAbi::Called(MockAbi::ui_calls,
                                  root_snapshot_call.c_str()),
                   "the frame probe checks the API-owned root through V3 UI");

            for (MockAbi::UiControlRecord& record : MockAbi::ui_controls) {
                if (record.game_owned) record.live = false;
            }
            MockAbi::FireEvent("wotbmod.ui.screen_changed");
            std::memset(message, 0, sizeof(message));
            Check(eval(ui_example_script, "on_frame(121, 0.016)", message,
                       sizeof(message)) == 0u,
                  "a screen-change event is deferred safely to the next frame");
            MockAbi::UiControlRecord* replacement_root =
                MockAbi::FindLatestUiControlById("RuntimeRoot");
            Check(MockAbi::LiveControls() == 35u && replacement_root &&
                      replacement_root->handle == runtime_root_handle &&
                      replacement_root->parent != first_screen_handle &&
                      MockAbi::LiveUiEventSubscriptions() == 17u &&
                      volume_label && volume_label->text == "Громкость: 35%",
                  "screen replacement reparents the existing runtime tree "
                  "without losing controls, callbacks or state");

            MockAbi::core_context_mask =
                WOTBMOD_V3_CONTEXT_BATTLE |
                WOTBMOD_V3_CONTEXT_MOD_SCREEN;
            std::memset(message, 0, sizeof(message));
            Check(eval(ui_example_script, "on_frame(135, 0.016)", message,
                       sizeof(message)) == 0u &&
                      MockAbi::LiveControls() == 0u &&
                      MockAbi::LiveUiEventSubscriptions() == 0u,
                  "the example removes its entire UI while the mod catalog "
                  "context is active");

            std::memset(message, 0, sizeof(message));
            Check(eval(ui_example_script, "on_disable()", message,
                       sizeof(message)) == 0u,
                  "the shipped example disables without a Lua fault");
            Check(MockAbi::LiveControls() == 0u,
                  "on_disable destroys every control owned by the example");
            Check(MockAbi::LiveUiEventSubscriptions() == 0u,
                   "on_disable leaves no UI callback behind");
            Check(MockAbi::LiveSubscriptions() == 0u,
                  "on_disable removes every screen and battle subscription");
            Check(MockAbi::GenericHandleReferences(6000u) == 0u,
                  "screen reparenting releases the borrowed original "
                  "active-screen wrapper");
            destroy_script(ui_example_script);
        }
        Check(MockAbi::LiveControls() == 0u &&
                  MockAbi::LiveUiEventSubscriptions() == 0u,
              "destroying the tested example leaves no UI resources live");

        // ---- shipped battle telemetry: typed events become useful UI -----
        MockAbi::Reset();
        MockAbi::publish_core_context = true;
        MockAbi::core_context_mask = WOTBMOD_V3_CONTEXT_BATTLE;
        MockAbi::event_context_mask = WOTBMOD_V3_CONTEXT_BATTLE;
        std::memset(message, 0, sizeof(message));
        const std::string battle_telemetry = ReadModApiFile(
            L"examples\\lua_battle_telemetry\\main.lua");
        Check(!battle_telemetry.empty(),
              "the checked-in Lua Battle Telemetry mod is readable");
        void* telemetry_script = battle_telemetry.empty()
            ? nullptr
            : create_script("example.lua_battle_telemetry",
                            battle_telemetry.c_str(), message,
                            sizeof(message));
        Check(telemetry_script != nullptr,
              "the exact shipped Lua Battle Telemetry mod compiles");
        if (telemetry_script) {
            Check(eval(telemetry_script, "on_enable(); on_frame(30, 0.5)",
                       message, sizeof(message)) == 0u &&
                      MockAbi::LiveControls() == 15u &&
                      MockAbi::LiveUiEventSubscriptions() == 2u &&
                      MockAbi::LiveSubscriptions() == 15u &&
                      MockAbi::CountCalled(MockAbi::ui_calls,
                                           "ui.get_active_screen()") == 2u,
                  "Battle Telemetry mounts its live feed and all typed event "
                  "subscriptions and performs its attachment recovery probe");

            WotbModV3ClientEventEnvelope telemetry_event = {};
            WOTBMOD_V3_INIT_STRUCT(
                telemetry_event,
                WOTBMOD_V3_CLIENT_EVENT_ENVELOPE_VERSION);
            telemetry_event.type =
                WOTBMOD_V3_CLIENT_EVENT_VEHICLE_DAMAGED;
            telemetry_event.payload_size =
                sizeof(WotbModV3DamageEventData);
            telemetry_event.payload.damage.damage = 250;
            telemetry_event.payload.damage.previous_health = 1000;
            telemetry_event.payload.damage.health = 750;
            telemetry_event.payload.damage.source_entity_id = 201;
            MockAbi::event_payload.assign(
                reinterpret_cast<const char*>(&telemetry_event),
                sizeof(telemetry_event));
            MockAbi::FireEvent(WOTBMOD_V3_EVENT_DAMAGE_RECEIVED);
            std::memset(message, 0, sizeof(message));
            Check(eval(telemetry_script, "on_frame(31, 0.016)", message,
                       sizeof(message)) == 0u,
                  "Battle Telemetry renders a delivered event on the next "
                  "frame rather than mutating UI inside dispatch");
            MockAbi::UiControlRecord* telemetry_line =
                MockAbi::FindLatestUiControlById("TelemetryLine1");
            Check(telemetry_line && telemetry_line->visible &&
                      telemetry_line->text.find("УРОН 250") !=
                          std::string::npos &&
                      telemetry_line->text.find("HP 750") !=
                          std::string::npos,
                  "Battle Telemetry turns typed damage fields into a useful "
                   "live HUD line");

            MockAbi::UiControlRecord* telemetry_root =
                MockAbi::FindLatestUiControlById("BattleTelemetryRoot");
            const WotbModV3UiHandle telemetry_root_handle =
                telemetry_root ? telemetry_root->handle : 0u;
            const WotbModV3UiHandle telemetry_screen_handle =
                telemetry_root ? telemetry_root->parent : 0u;
            for (MockAbi::UiControlRecord& record : MockAbi::ui_controls) {
                if (record.game_owned) record.live = false;
            }
            MockAbi::FireEvent(WOTBMOD_V3_EVENT_UI_SCREEN_CHANGED);
            std::memset(message, 0, sizeof(message));
            Check(eval(telemetry_script, "on_frame(32, 0.016)", message,
                       sizeof(message)) == 0u && telemetry_root &&
                      telemetry_root->handle == telemetry_root_handle &&
                      telemetry_root->parent != telemetry_screen_handle &&
                      MockAbi::LiveControls() == 15u,
                  "Battle Telemetry reparents in place on a screen change");

            MockAbi::core_context_mask =
                WOTBMOD_V3_CONTEXT_BATTLE |
                WOTBMOD_V3_CONTEXT_MOD_SCREEN;
            std::memset(message, 0, sizeof(message));
            Check(eval(telemetry_script, "on_frame(45, 0.016)", message,
                       sizeof(message)) == 0u &&
                      MockAbi::LiveControls() == 0u &&
                      MockAbi::LiveUiEventSubscriptions() == 0u,
                  "Battle Telemetry fully disappears for the mod catalog");
            Check(eval(telemetry_script, "on_disable()", message,
                       sizeof(message)) == 0u &&
                      MockAbi::LiveSubscriptions() == 0u,
                  "Battle Telemetry leaves no event callback behind");
            destroy_script(telemetry_script);
        }

        // ---- shipped session statistics: truthful hangar aggregates ------
        MockAbi::Reset();
        MockAbi::SetHostPermissions(
            {"core", "events.public", "storage", "ui.modify.game",
             "ui.create", "ui.modify.own", "battle.ui", "input",
             "entity.public.visible", "game.entity.public"});
        Check(entry(&bootstrap, 1u, &info) == WOTBMOD_V3_OK,
              "the session-statistics test measures public player access");
        MockAbi::publish_core_context = true;
        MockAbi::core_context_mask = WOTBMOD_V3_CONTEXT_HANGAR;
        MockAbi::event_context_mask = WOTBMOD_V3_CONTEXT_HANGAR;
        std::memset(message, 0, sizeof(message));
        const std::string session_stats = ReadModApiFile(
            L"examples\\lua_session_stats\\main.lua");
        Check(!session_stats.empty(),
              "the checked-in Blitz Session Statistics mod is readable");
        void* session_script = session_stats.empty()
            ? nullptr
            : create_script("example.lua_session_stats",
                            session_stats.c_str(), message, sizeof(message));
        Check(session_script != nullptr,
              "the exact shipped Blitz Session Statistics mod compiles");
        if (session_script) {
            const uint32_t enable_status = eval(
                session_script, "on_enable(); on_frame(15, 0.5)",
                message, sizeof(message));
            if (enable_status != 0u) {
                std::fprintf(stderr, "    session stats probe: %s\n", message);
            }
            Check(enable_status == 0u &&
                      MockAbi::LiveControls() == 51u &&
                      MockAbi::LiveUiEventSubscriptions() == 4u &&
                      MockAbi::LiveSubscriptions() == 8u,
                  "Session Statistics builds its complete hangar dashboard "
                  "from the public HANGAR context and owns every callback "
                  "it installs");
            const auto idle_text_updates_before = std::count_if(
                MockAbi::ui_calls.begin(),
                MockAbi::ui_calls.end(),
                [](const std::string& call) {
                    return call.find("ui.control_set_text(") == 0u;
                });
            Check(eval(session_script, "on_frame(30, 15.0)", message,
                       sizeof(message)) == 0u &&
                      std::count_if(
                          MockAbi::ui_calls.begin(),
                          MockAbi::ui_calls.end(),
                          [](const std::string& call) {
                              return call.find("ui.control_set_text(") == 0u;
                          }) == idle_text_updates_before,
                  "Session Statistics leaves DAVA text resources untouched "
                  "during idle hangar frames");
            MockAbi::core_context_mask = WOTBMOD_V3_CONTEXT_NONE;
            MockAbi::event_context_mask = WOTBMOD_V3_CONTEXT_NONE;
            MockAbi::FireEvent(WOTBMOD_V3_EVENT_UI_SCREEN_CHANGED);
            Check(eval(session_script, "on_frame(16, 0.016)", message,
                       sizeof(message)) == 0u &&
                      MockAbi::LiveControls() == 0u,
                  "Session Statistics strictly removes its UI when the "
                  "native loader reports a non-hangar screen");
            MockAbi::core_context_mask = WOTBMOD_V3_CONTEXT_HANGAR;
            MockAbi::event_context_mask = WOTBMOD_V3_CONTEXT_HANGAR;
            MockAbi::FireEvent(WOTBMOD_V3_EVENT_UI_SCREEN_CHANGED);
            Check(eval(session_script, "on_frame(17, 0.016)", message,
                       sizeof(message)) == 0u &&
                      MockAbi::LiveControls() == 51u,
                  "Session Statistics remounts after the native loader "
                  "reports the hangar again");
            const uint32_t player_probe = eval(
                session_script,
                "local view, err = wotb.players.snapshot()\n"
                "if not view then error(tostring(err)) end\n"
                "if view.local_team ~= 1 then "
                "error('local team ' .. tostring(view.local_team)) end",
                message, sizeof(message));
            if (player_probe != 0u) {
                std::fprintf(stderr, "    session player probe: %s\n", message);
            }
            Check(player_probe == 0u,
                  "Session Statistics can read the local team used to "
                  "classify a trustworthy winner_team");

            WotbModV3ClientEventEnvelope session_event = {};
            WOTBMOD_V3_INIT_STRUCT(
                session_event,
                WOTBMOD_V3_CLIENT_EVENT_ENVELOPE_VERSION);
            session_event.type = WOTBMOD_V3_CLIENT_EVENT_BATTLE_ENTERED;
            session_event.payload_size = sizeof(WotbModV3BattleEventData);
            session_event.payload.battle.battle_id = 701u;
            session_event.payload.battle.arena_id = 41u;
            MockAbi::event_payload.assign(
                reinterpret_cast<const char*>(&session_event),
                sizeof(session_event));
            MockAbi::event_context_mask = WOTBMOD_V3_CONTEXT_BATTLE;
            MockAbi::FireEvent(WOTBMOD_V3_EVENT_BATTLE_ENTERED);
            MockAbi::core_context_mask = WOTBMOD_V3_CONTEXT_BATTLE;
            Check(eval(session_script, "on_frame(30, 1.5)", message,
                       sizeof(message)) == 0u &&
                      MockAbi::LiveControls() == 0u,
                  "Session Statistics keeps collecting while its hangar UI "
                  "is completely unmounted in battle");

            session_event = {};
            WOTBMOD_V3_INIT_STRUCT(
                session_event,
                WOTBMOD_V3_CLIENT_EVENT_ENVELOPE_VERSION);
            session_event.type = WOTBMOD_V3_CLIENT_EVENT_VEHICLE_DAMAGED;
            session_event.payload_size = sizeof(WotbModV3DamageEventData);
            session_event.payload.damage.damage = 350;
            session_event.payload.damage.previous_health = 1200;
            session_event.payload.damage.health = 850;
            MockAbi::event_payload.assign(
                reinterpret_cast<const char*>(&session_event),
                sizeof(session_event));
            MockAbi::FireEvent(WOTBMOD_V3_EVENT_DAMAGE_RECEIVED);
            MockAbi::event_payload.clear();
            MockAbi::FireEvent(WOTBMOD_V3_EVENT_LOCAL_SHELL_FIRED);
            MockAbi::FireEvent(WOTBMOD_V3_EVENT_LOCAL_SHELL_FIRED);
            MockAbi::FireEvent(WOTBMOD_V3_EVENT_LOCAL_VEHICLE_DESTROYED);

            session_event = {};
            WOTBMOD_V3_INIT_STRUCT(
                session_event,
                WOTBMOD_V3_CLIENT_EVENT_ENVELOPE_VERSION);
            session_event.type = WOTBMOD_V3_CLIENT_EVENT_BATTLE_ENDED;
            session_event.payload_size = sizeof(WotbModV3BattleEventData);
            session_event.payload.battle.battle_id = 701u;
            session_event.payload.battle.arena_id = 41u;
            session_event.payload.battle.winner_team = 1u;
            MockAbi::event_payload.assign(
                reinterpret_cast<const char*>(&session_event),
                sizeof(session_event));
            MockAbi::FireEvent(WOTBMOD_V3_EVENT_BATTLE_ENDED);
            MockAbi::core_context_mask = WOTBMOD_V3_CONTEXT_HANGAR;
            MockAbi::event_context_mask = WOTBMOD_V3_CONTEXT_HANGAR;
            Check(eval(session_script, "on_frame(45, 0.5)", message,
                       sizeof(message)) == 0u &&
                      MockAbi::LiveControls() == 51u,
                  "Session Statistics remounts the populated dashboard in "
                  "the hangar after a battle");

            MockAbi::UiControlRecord* battles_value =
                MockAbi::FindLatestUiControlById("SessionBattlesValue");
            MockAbi::UiControlRecord* winrate_value =
                MockAbi::FindLatestUiControlById("SessionWinrateValue");
            MockAbi::UiControlRecord* survival_value =
                MockAbi::FindLatestUiControlById("SessionSurvivalValue");
            MockAbi::UiControlRecord* damage_value =
                MockAbi::FindLatestUiControlById("SessionDamageValue");
            MockAbi::UiControlRecord* shots_value =
                MockAbi::FindLatestUiControlById("SessionShotsValue");
            MockAbi::UiControlRecord* history_row =
                MockAbi::FindLatestUiControlById("SessionHistoryRow1");
            if (!(battles_value && winrate_value && survival_value &&
                  damage_value && shots_value && history_row)) {
                std::fprintf(stderr,
                             "    session controls: battles=%p winrate=%p "
                             "survival=%p damage=%p shots=%p history=%p\n",
                             static_cast<void*>(battles_value),
                             static_cast<void*>(winrate_value),
                             static_cast<void*>(survival_value),
                             static_cast<void*>(damage_value),
                             static_cast<void*>(shots_value),
                             static_cast<void*>(history_row));
            } else if (battles_value->text != "1" ||
                       winrate_value->text != "100%" ||
                       survival_value->text != "0%" ||
                       damage_value->text != "350" ||
                       shots_value->text != "2") {
                std::fprintf(stderr,
                             "    session values: battles=%s winrate=%s "
                             "survival=%s damage=%s shots=%s history=%s\n",
                             battles_value->text.c_str(),
                             winrate_value->text.c_str(),
                             survival_value->text.c_str(),
                             damage_value->text.c_str(),
                             shots_value->text.c_str(),
                             history_row->text.c_str());
            }
            Check(battles_value && battles_value->text == "1" &&
                      winrate_value && winrate_value->text == "100%" &&
                      survival_value && survival_value->text == "0%" &&
                      damage_value && damage_value->text == "350" &&
                      shots_value && shots_value->text == "2" &&
                      history_row && history_row->visible &&
                      history_row->text.find("ПОБЕДА") != std::string::npos &&
                      history_row->text.find("УРОН 350") !=
                          std::string::npos &&
                      history_row->text.find("УНИЧТОЖЕН") !=
                          std::string::npos,
                  "confirmed battle events become a win, received damage, "
                  "shots and survival history without placeholder values");

            session_event = {};
            WOTBMOD_V3_INIT_STRUCT(
                session_event,
                WOTBMOD_V3_CLIENT_EVENT_ENVELOPE_VERSION);
            session_event.type = WOTBMOD_V3_CLIENT_EVENT_BATTLE_ENTERED;
            session_event.payload_size = sizeof(WotbModV3BattleEventData);
            session_event.payload.battle.battle_id = 702u;
            MockAbi::event_payload.assign(
                reinterpret_cast<const char*>(&session_event),
                sizeof(session_event));
            MockAbi::event_context_mask = WOTBMOD_V3_CONTEXT_BATTLE;
            MockAbi::FireEvent(WOTBMOD_V3_EVENT_BATTLE_ENTERED);
            MockAbi::core_context_mask = WOTBMOD_V3_CONTEXT_BATTLE;
            Check(eval(session_script, "on_frame(60, 1.0)", message,
                       sizeof(message)) == 0u,
                  "a second battle captures its local-player snapshot");
            session_event.type = WOTBMOD_V3_CLIENT_EVENT_BATTLE_ENDED;
            session_event.payload.battle.winner_team = 0u;
            MockAbi::event_payload.assign(
                reinterpret_cast<const char*>(&session_event),
                sizeof(session_event));
            MockAbi::FireEvent(WOTBMOD_V3_EVENT_BATTLE_ENDED);
            MockAbi::core_context_mask = WOTBMOD_V3_CONTEXT_HANGAR;
            MockAbi::event_context_mask = WOTBMOD_V3_CONTEXT_HANGAR;
            Check(eval(session_script, "on_frame(75, 0.5)", message,
                       sizeof(message)) == 0u,
                  "a battle with no trustworthy winner returns to the "
                  "hangar without a Lua fault");
            winrate_value =
                MockAbi::FindLatestUiControlById("SessionWinrateValue");
            MockAbi::UiControlRecord* coverage =
                MockAbi::FindLatestUiControlById("ResultCoverage");
            history_row =
                MockAbi::FindLatestUiControlById("SessionHistoryRow1");
            Check(winrate_value && winrate_value->text == "100%" &&
                      coverage && coverage->text.find("1/2") !=
                          std::string::npos &&
                      history_row && history_row->text.find("N/A") !=
                          std::string::npos,
                  "a zero winner_team is reported as N/A and excluded from "
                  "winrate instead of being fabricated as a draw or loss");

            session_event = {};
            WOTBMOD_V3_INIT_STRUCT(
                session_event,
                WOTBMOD_V3_CLIENT_EVENT_ENVELOPE_VERSION);
            session_event.type = WOTBMOD_V3_CLIENT_EVENT_BATTLE_ENTERED;
            session_event.payload_size = sizeof(WotbModV3BattleEventData);
            session_event.payload.battle.battle_id = 703u;
            MockAbi::event_payload.assign(
                reinterpret_cast<const char*>(&session_event),
                sizeof(session_event));
            MockAbi::event_context_mask = WOTBMOD_V3_CONTEXT_BATTLE;
            MockAbi::FireEvent(WOTBMOD_V3_EVENT_BATTLE_ENTERED);
            MockAbi::core_context_mask = WOTBMOD_V3_CONTEXT_BATTLE;
            Check(eval(session_script, "on_frame(90, 1.0)", message,
                       sizeof(message)) == 0u,
                  "an out-of-order battle captures its local-player snapshot");
            session_event.type = WOTBMOD_V3_CLIENT_EVENT_BATTLE_LEFT;
            session_event.payload.battle.winner_team = 0u;
            MockAbi::event_payload.assign(
                reinterpret_cast<const char*>(&session_event),
                sizeof(session_event));
            MockAbi::FireEvent(WOTBMOD_V3_EVENT_BATTLE_LEFT);
            session_event.type = WOTBMOD_V3_CLIENT_EVENT_BATTLE_ENDED;
            session_event.payload.battle.winner_team = 1u;
            MockAbi::event_payload.assign(
                reinterpret_cast<const char*>(&session_event),
                sizeof(session_event));
            MockAbi::FireEvent(WOTBMOD_V3_EVENT_BATTLE_ENDED);
            MockAbi::core_context_mask = WOTBMOD_V3_CONTEXT_HANGAR;
            MockAbi::event_context_mask = WOTBMOD_V3_CONTEXT_HANGAR;
            Check(eval(session_script, "on_frame(91, 0.5)", message,
                       sizeof(message)) == 0u,
                  "a result-bearing BATTLE_ENDED can follow BATTLE_LEFT");
            battles_value =
                MockAbi::FindLatestUiControlById("SessionBattlesValue");
            coverage =
                MockAbi::FindLatestUiControlById("ResultCoverage");
            history_row =
                MockAbi::FindLatestUiControlById("SessionHistoryRow1");
            Check(battles_value && battles_value->text == "3" &&
                      coverage && coverage->text.find("2/3") !=
                          std::string::npos &&
                      history_row && history_row->text.find("ПОБЕДА") !=
                          std::string::npos,
                  "BATTLE_LEFT does not discard a later trustworthy result");

            MockAbi::UiControlRecord* reset_button =
                MockAbi::FindLatestUiControlById("ResetButton");
            if (reset_button) {
                MockAbi::FireUiEventForControl(reset_button->handle);
                MockAbi::FireUiEventForControl(reset_button->handle);
            }
            battles_value =
                MockAbi::FindLatestUiControlById("SessionBattlesValue");
            Check(eval(session_script, "on_frame(76, 0.016)", message,
                       sizeof(message)) == 0u &&
                      battles_value && battles_value->text == "0",
                  "the guarded second reset click clears the in-memory "
                  "session");

            MockAbi::core_context_mask =
                WOTBMOD_V3_CONTEXT_HANGAR |
                WOTBMOD_V3_CONTEXT_MOD_SCREEN;
            Check(eval(session_script, "on_frame(90, 0.016)", message,
                       sizeof(message)) == 0u &&
                      MockAbi::LiveControls() == 0u &&
                      MockAbi::LiveUiEventSubscriptions() == 0u,
                  "Session Statistics removes every control when the native "
                  "loader publishes the mod-catalog context");
            MockAbi::core_context_mask = WOTBMOD_V3_CONTEXT_HANGAR;
            Check(eval(session_script, "on_frame(105, 0.016)", message,
                       sizeof(message)) == 0u &&
                      MockAbi::LiveControls() == 51u,
                  "Session Statistics returns after the mod catalog "
                  "closes");
            Check(eval(session_script, "on_disable()", message,
                       sizeof(message)) == 0u &&
                      MockAbi::LiveSubscriptions() == 0u,
                  "Session Statistics disables without leaked event "
                  "subscriptions");
            destroy_script(session_script);
        }
        MockAbi::SetHostPermissions(
            {"core", "events.public", "storage", "ui.modify.game",
             "ui.create", "ui.modify.own", "battle.ui", "input"});
        Check(entry(&bootstrap, 1u, &info) == WOTBMOD_V3_OK,
              "the session-statistics test restores the default ceiling");

        // ---- subscribe: the pattern, the defaults, and a real delivery ----
        MockAbi::Reset();
        std::memset(message, 0, sizeof(message));
        void* script = create_script(
            "events",
            "seen = nil\n"
            "seen_role = nil\n"
            "seen_time = nil\n"
            "seen_context = nil\n"
            "seen_flags = nil\n"
            "seen_dispatch = nil\n"
            "seen_publisher = nil\n"
            "seen_payload = 'unset'\n"
            "sub = wotb.events.subscribe('wotbmod.frame.update', function(e)\n"
            "  seen = e.topic\n"
            "  seen_role = e.thread_role\n"
            "  seen_time = e.timestamp_ns\n"
            "  seen_context = e.context_mask\n"
            "  seen_flags = e.flags\n"
            "  seen_dispatch = e.dispatch\n"
            "  seen_publisher = e.publisher\n"
            "  seen_payload = e.payload\n"
            "end)",
            message, sizeof(message));
        Check(script != nullptr, "a script that subscribes compiles and runs");
        Check(MockAbi::Called(MockAbi::events_calls,
                              "events.subscribe(wotbmod.frame.update,0,0)"),
              "subscribe reached the ABI with the pattern, PRIORITY_NORMAL as "
              "the default priority and receive_system_events off");

        if (script) {
            Check(eval(script,
                       "if type(sub) ~= 'userdata' then "
                       "error('subscription is a ' .. type(sub)) end",
                       message, sizeof(message)) == 0u,
                  "and handed back a subscription as userdata, not a number "
                  "the sandbox could forge");

            // Nothing has been fired yet: a subscription that "worked"
            // because the binding invoked the handler itself would already
            // have set `seen`.
            Check(eval(script, "if seen ~= nil then error('fired early') end",
                       message, sizeof(message)) == 0u,
                  "subscribing does not itself deliver anything");

            MockAbi::event_thread_role = WOTBMOD_V3_THREAD_RENDER;
            MockAbi::FireEvent("wotbmod.frame.update");

            std::memset(message, 0, sizeof(message));
            Check(eval(script,
                       "if seen ~= 'wotbmod.frame.update' then "
                       "error('topic ' .. tostring(seen)) end",
                       message, sizeof(message)) == 0u,
                  "the client's callback reached the Lua handler with the "
                  "topic");
            Check(eval(script,
                       "if seen_role ~= wotb.events.THREAD_RENDER then "
                       "error('role ' .. tostring(seen_role)) end",
                       message, sizeof(message)) == 0u,
                  "the event's thread_role crosses as the value the client "
                  "set, not as a constant this host invented");
            Check(eval(script,
                       "if seen_time ~= 1234567890123456789 then "
                       "error('timestamp ' .. tostring(seen_time)) end",
                       message, sizeof(message)) == 0u,
                  "timestamp_ns survives as a full 64-bit integer");
            Check(eval(script,
                       "if seen_context ~= 0xF4 then "
                       "error('context ' .. tostring(seen_context)) end",
                       message, sizeof(message)) == 0u,
                  "context_mask crosses distinctly from the timestamp");
            Check(eval(script,
                       "if seen_flags ~= wotb.events.FLAG_STOPPABLE then "
                       "error('flags ' .. tostring(seen_flags)) end",
                       message, sizeof(message)) == 0u,
                  "and so do the event's flags");
            Check(eval(script,
                       "if type(seen_dispatch) ~= 'userdata' then "
                       "error('dispatch is a ' .. type(seen_dispatch)) end",
                       message, sizeof(message)) == 0u,
                  "the dispatch token crosses as userdata - a script that "
                  "could write the number could stop another mod's dispatch");
            Check(eval(script,
                       "if type(seen_publisher) ~= 'userdata' then "
                       "error('publisher is a ' .. type(seen_publisher)) end",
                       message, sizeof(message)) == 0u,
                  "and so does the publishing mod handle");
            Check(eval(script, "if seen_payload ~= nil then "
                               "error('payload ' .. tostring(seen_payload)) end",
                       message, sizeof(message)) == 0u,
                  "an event with no payload arrives with payload nil, not an "
                  "empty string that would read as a zero-length one");

            // ---- the four dispatch readers, each against its own magic
            // value. get_thread bound to get_context (or to any other of the
            // four) answers the wrong number here, not merely nothing.
            std::memset(message, 0, sizeof(message));
            Check(eval(script,
                       "probe = {}\n"
                       "wotb.events.subscribe('wotbmod.dispatch.probe', "
                       "function(e)\n"
                       "  probe.thread = wotb.events.get_thread(e.dispatch)\n"
                       "  probe.time = wotb.events.get_timestamp(e.dispatch)\n"
                       "  probe.context = wotb.events.get_context(e.dispatch)\n"
                       "  probe.info = wotb.events.get_dispatch_info(e.dispatch)\n"
                       "end)",
                       message, sizeof(message)) == 0u,
                  "a second subscription on another topic");
            MockAbi::FireEvent("wotbmod.dispatch.probe");
            Check(eval(script,
                       "if probe.thread ~= wotb.events.THREAD_RENDER then "
                       "error('get_thread ' .. tostring(probe.thread)) end",
                       message, sizeof(message)) == 0u,
                  "get_thread answers this dispatch's thread role");
            Check(eval(script,
                       "if probe.time ~= 1234567890123456789 then "
                       "error('get_timestamp ' .. tostring(probe.time)) end",
                       message, sizeof(message)) == 0u,
                  "get_timestamp answers the timestamp, not the context mask");
            Check(eval(script,
                       "if probe.context ~= 0xF4 then "
                       "error('get_context ' .. tostring(probe.context)) end",
                       message, sizeof(message)) == 0u,
                  "get_context answers the context mask, not the timestamp");
            Check(eval(script,
                       "local i = probe.info\n"
                       "if type(i) ~= 'table' then error('info is a ' .. "
                       "type(i)) end\n"
                       "if i.timestamp_ns ~= 1234567890123456789 then "
                       "error('info.timestamp ' .. tostring(i.timestamp_ns)) end\n"
                       "if i.context_mask ~= 0xF4 then "
                       "error('info.context ' .. tostring(i.context_mask)) end\n"
                       "if i.thread_role ~= wotb.events.THREAD_RENDER then "
                       "error('info.thread_role ' .. tostring(i.thread_role)) end\n"
                       "if i.flags ~= wotb.events.FLAG_STOPPABLE then "
                       "error('info.flags ' .. tostring(i.flags)) end\n"
                       "if i.propagation_stopped ~= false then "
                       "error('info.propagation_stopped ' .. "
                       "tostring(i.propagation_stopped)) end\n"
                       "if type(i.dispatch) ~= 'userdata' then "
                       "error('info.dispatch is a ' .. type(i.dispatch)) end",
                       message, sizeof(message)) == 0u,
                  "get_dispatch_info answers every field of the dispatch at "
                  "once, each carrying its own distinct value");
            Check(MockAbi::Called(MockAbi::events_calls,
                                  "events.get_thread(2882400001)") &&
                      MockAbi::Called(MockAbi::events_calls,
                                      "events.get_timestamp(2882400001)") &&
                      MockAbi::Called(MockAbi::events_calls,
                                      "events.get_context(2882400001)") &&
                      MockAbi::Called(MockAbi::events_calls,
                                      "events.get_dispatch_info(2882400001)"),
                  "and all four reached their own ABI slot carrying the "
                  "dispatch token the event arrived with");

            // A dispatch token is exactly as forgeable as a handle, so the
            // number itself must not be accepted where the box belongs.
            std::memset(message, 0, sizeof(message));
            Check(eval(script,
                       "local v, e = wotb.events.get_thread(2882400001)\n"
                       "if v ~= nil or type(e) ~= 'string' then "
                       "error('a bare number was accepted as a dispatch "
                       "token') end",
                       message, sizeof(message)) == 0u,
                  "a bare number cannot forge a dispatch token, even the "
                  "numerically correct one");
            Check(eval(script,
                       "local v, e = wotb.events.stop_propagation(sub)\n"
                       "if v ~= nil or type(e) ~= 'string' then "
                       "error('a subscription passed as a dispatch token') end",
                       message, sizeof(message)) == 0u,
                  "and neither can a subscription handle - the type name in "
                  "the box is what stops it, not the C type underneath");

            destroy_script(script);
        }

        // ---- typed system payloads: raw bytes plus a safe Lua table ------
        MockAbi::Reset();
        std::memset(message, 0, sizeof(message));
        script = create_script(
            "typed-events",
            "typed = nil\n"
            "typed_raw_size = 0\n"
            "wotb.events.subscribe(wotb.events.TOPIC_DAMAGE_RECEIVED, "
            "function(e)\n"
            "  typed = e.data\n"
            "  typed_raw_size = e.payload and #e.payload or 0\n"
            "end, wotb.events.PRIORITY_NORMAL, true)",
            message, sizeof(message));
        Check(script != nullptr,
              "a script can subscribe through the named gameplay topic");
        if (script) {
            WotbModV3ClientEventEnvelope envelope = {};
            WOTBMOD_V3_INIT_STRUCT(
                envelope,
                WOTBMOD_V3_CLIENT_EVENT_ENVELOPE_VERSION);
            envelope.type = WOTBMOD_V3_CLIENT_EVENT_VEHICLE_DAMAGED;
            envelope.flags = 0x52u;
            envelope.sequence = 987654321u;
            envelope.primary_entity_id = 101u;
            envelope.other_entity_id = 201u;
            envelope.payload_size = sizeof(WotbModV3DamageEventData);
            envelope.payload.damage.damage = 275;
            envelope.payload.damage.previous_health = 1200;
            envelope.payload.damage.health = 925;
            envelope.payload.damage.reason_code = 7u;
            envelope.payload.damage.source_entity_id = 201u;
            MockAbi::event_payload.assign(
                reinterpret_cast<const char*>(&envelope),
                sizeof(envelope));
            MockAbi::FireEvent(WOTBMOD_V3_EVENT_DAMAGE_RECEIVED);

            char typed_probe[1024] = {};
            _snprintf_s(
                typed_probe,
                sizeof(typed_probe),
                _TRUNCATE,
                "if type(typed) ~= 'table' then error('data ' .. "
                "type(typed)) end\n"
                "if typed.kind ~= 'client_event' then error('kind') end\n"
                "if typed.type ~= wotb.events.TYPE_VEHICLE_DAMAGED then "
                "error('type') end\n"
                "if typed.primary_entity_id ~= 101 or "
                "typed.other_entity_id ~= 201 then error('ids') end\n"
                "if typed.damage ~= 275 or typed.previous_health ~= 1200 "
                "or typed.health ~= 925 then error('damage') end\n"
                "if typed.reason_code ~= 7 or typed.source_entity_id ~= 201 "
                "then error('source') end\n"
                "if typed_raw_size ~= %u then error('raw size ' .. "
                "tostring(typed_raw_size)) end",
                static_cast<unsigned int>(sizeof(envelope)));
            const uint32_t typed_status =
                eval(script, typed_probe, message, sizeof(message));
            if (typed_status != 0u) {
                std::fprintf(stderr, "    typed event probe: %s\n", message);
            }
            Check(typed_status == 0u,
                  "a damage event exposes typed fields without removing its "
                  "byte-exact raw payload");

            MockAbi::event_payload.resize(sizeof(envelope) - 1u);
            MockAbi::FireEvent(WOTBMOD_V3_EVENT_DAMAGE_RECEIVED);
            Check(eval(script,
                       "if typed ~= nil then error('truncated payload was "
                       "decoded') end",
                       message, sizeof(message)) == 0u,
                  "a truncated system payload stays raw and is never "
                  "partially decoded past its proven size");

            envelope.api_version =
                WOTBMOD_V3_CLIENT_EVENT_ENVELOPE_VERSION + 1u;
            MockAbi::event_payload.assign(
                reinterpret_cast<const char*>(&envelope),
                sizeof(envelope));
            MockAbi::FireEvent(WOTBMOD_V3_EVENT_DAMAGE_RECEIVED);
            Check(eval(script,
                       "if typed ~= nil then error('wrong version was "
                       "decoded') end",
                       message, sizeof(message)) == 0u,
                  "typed event decoding rejects an unknown envelope version");

            envelope.api_version =
                WOTBMOD_V3_CLIENT_EVENT_ENVELOPE_VERSION;
            envelope.type = WOTBMOD_V3_CLIENT_EVENT_CAMERA_MODE_CHANGED;
            MockAbi::event_payload.assign(
                reinterpret_cast<const char*>(&envelope),
                sizeof(envelope));
            MockAbi::FireEvent(WOTBMOD_V3_EVENT_DAMAGE_RECEIVED);
            Check(eval(script,
                       "if typed ~= nil then error('wrong type was decoded') "
                       "end",
                       message, sizeof(message)) == 0u,
                  "typed event decoding rejects a topic/type mismatch");

            envelope.type = WOTBMOD_V3_CLIENT_EVENT_VEHICLE_DAMAGED;
            envelope.payload_size =
                sizeof(WotbModV3DamageEventData) - 1u;
            MockAbi::event_payload.assign(
                reinterpret_cast<const char*>(&envelope),
                sizeof(envelope));
            MockAbi::FireEvent(WOTBMOD_V3_EVENT_DAMAGE_RECEIVED);
            Check(eval(script,
                       "if typed ~= nil then error('short nested payload was "
                       "decoded') end",
                       message, sizeof(message)) == 0u,
                  "typed event decoding rejects a short nested payload even "
                  "when the outer envelope is complete");
            destroy_script(script);
        }

        // ---- public entity lifecycle payload and player/team views --------
        MockAbi::Reset();
        std::memset(message, 0, sizeof(message));
        script = create_script(
            "entity-events",
            "entity_event = nil\n"
            "wotb.events.subscribe(wotb.events.TOPIC_PUBLIC_ENTITY_UPDATED, "
            "function(e) entity_event = e.data end, "
            "wotb.events.PRIORITY_NORMAL, true)",
            message, sizeof(message));
        Check(script != nullptr,
              "a public-entity lifecycle subscriber compiles");
        if (script) {
            WotbModV3PublicEntityLifecycleEvent entity_event = {};
            WOTBMOD_V3_INIT_STRUCT(
                entity_event,
                WOTBMOD_V3_ENTITY_PUBLIC_VERSION);
            entity_event.reason = WOTBMOD_V3_PUBLIC_ENTITY_REASON_UPDATED;
            entity_event.snapshot = MockAbi::PublicEntities()[2];
            MockAbi::event_payload.assign(
                reinterpret_cast<const char*>(&entity_event),
                sizeof(entity_event));
            MockAbi::FireEvent(WOTBMOD_V3_EVENT_PUBLIC_ENTITY_UPDATED);
            Check(eval(script,
                       "local s = entity_event and entity_event.snapshot\n"
                       "if entity_event.kind ~= 'public_entity' then "
                       "error('kind') end\n"
                       "if entity_event.reason ~= 3 then error('reason') end\n"
                       "if type(s.handle) ~= 'userdata' then error('handle') "
                       "end\n"
                       "if s.public_id ~= 201 or s.team ~= 2 or "
                       "s.display_name ~= 'Enemy' then error('snapshot') end\n"
                       "if not s.team_available or not "
                       "s.display_name_available then error('availability') "
                       "end\n"
                       "if s.position_available or s.direction_available "
                       "then error('invented transform') end",
                       message, sizeof(message)) == 0u,
                  "entity event data carries every sourced attribute and "
                  "marks unsourced transforms unavailable");
            destroy_script(script);
        }

        MockAbi::Reset();
        MockAbi::SetHostPermissions(
            {"core", "events.public", "storage", "ui.modify.game",
             "ui.create", "ui.modify.own", "battle.ui",
             "entity.public.visible", "game.entity.public"});
        Check(entry(&bootstrap, 1u, &info) == WOTBMOD_V3_OK,
              "the players test re-measures a ceiling with public entity "
              "access");
        std::memset(message, 0, sizeof(message));
        script = create_script(
            "players-view",
            "players_view, players_error = wotb.players.snapshot()",
            message, sizeof(message));
        Check(script != nullptr,
              "the wotb.players convenience layer loads over entity_public");
        if (script) {
            const uint32_t players_status = eval(script,
                       "if not players_view then error(players_error) end\n"
                       "if players_view.local_player.display_name ~= 'Local' "
                       "then error('local') end\n"
                       "if #players_view.our_team ~= 2 then error('our ' .. "
                       "#players_view.our_team) end\n"
                       "if #players_view.enemy_team ~= 1 then error('enemy') "
                       "end\n"
                       "if #players_view.unknown_team ~= 0 then "
                       "error('unknown') end\n"
                       "if players_view.enemy_team[1].relation ~= 'enemy' or "
                       "not players_view.enemy_team[1].is_enemy then "
                       "error('relation') end\n"
                       "if players_view.enemy_scope ~= "
                       "'currently_visible_only' then error('scope') end\n"
                       "local absent, absent_value = wotb.players.find(999)\n"
                       "if absent ~= true or absent_value ~= nil then "
                       "error('hidden placeholder') end",
                       message, sizeof(message));
            if (players_status != 0u) {
                std::fprintf(stderr, "    players probe: %s\n", message);
            }
            Check(players_status == 0u,
                  "players.snapshot groups local, own team and only visible "
                  "enemies without synthesizing a hidden-player record");
            destroy_script(script);
        }

        MockAbi::PublicEntities()[0].local_player = 0u;
        MockAbi::PublicEntities()[1].local_player = 0u;
        MockAbi::PublicEntities()[2].local_player = 0u;
        std::memset(message, 0, sizeof(message));
        script = create_script(
            "players-team-fallback",
            "players_view, players_error = wotb.players.snapshot()",
            message, sizeof(message));
        Check(script != nullptr,
              "players snapshot compiles without a local-player flag");
        if (script) {
            Check(eval(script,
                       "if not players_view then error(players_error) end\n"
                       "if players_view.local_player ~= nil then "
                       "error('fabricated local') end\n"
                       "if players_view.local_team ~= 1 or not "
                       "players_view.local_team_inferred then "
                       "error('team fallback') end\n"
                       "if #players_view.our_team ~= 2 or "
                       "#players_view.enemy_team ~= 1 then "
                       "error('grouping') end",
                       message, sizeof(message)) == 0u,
                  "players.snapshot infers only the unique visible majority "
                  "team without fabricating a local player");
            destroy_script(script);
        }
        MockAbi::PublicEntities()[0].local_player = 1u;

        MockAbi::publish_core_context = true;
        MockAbi::core_context_mask = WOTBMOD_V3_CONTEXT_BATTLE;
        MockAbi::event_context_mask = WOTBMOD_V3_CONTEXT_BATTLE;
        std::memset(message, 0, sizeof(message));
        const std::string ally_tracker = ReadModApiFile(
            L"examples\\lua_ally_tracker\\main.lua");
        Check(!ally_tracker.empty(),
              "the checked-in Lua Ally Tracker is readable");
        script = ally_tracker.empty()
            ? nullptr
            : create_script("example.lua_ally_tracker", ally_tracker.c_str(),
                            message, sizeof(message));
        Check(script != nullptr,
              "the exact shipped Lua Ally Tracker compiles");
        if (script) {
            const uint32_t enable_status = eval(
                script, "on_enable(); on_frame(30, 0.5)", message,
                sizeof(message));
            if (enable_status != 0u) {
                std::fprintf(stderr, "    ally tracker probe: %s\n", message);
            }
            Check(enable_status == 0u && MockAbi::LiveControls() == 15u &&
                      MockAbi::LiveUiEventSubscriptions() == 8u &&
                      MockAbi::LiveSubscriptions() == 8u &&
                      MockAbi::CountCalled(MockAbi::ui_calls,
                                           "ui.get_active_screen()") == 2u,
                  "the Ally Tracker mounts its battle-only panel, rows and "
                  "public event subscriptions with attachment recovery");
            MockAbi::UiControlRecord* ally_row =
                MockAbi::FindLatestUiControlById("AllyRow1");
            Check(ally_row && ally_row->visible &&
                      ally_row->text.find("Ally") != std::string::npos &&
                      ally_row->text.find("730/1000") != std::string::npos,
                  "the Ally Tracker renders sourced ally name and HP");
            const size_t unchanged_text_writes_before = std::count_if(
                MockAbi::ui_calls.begin(),
                MockAbi::ui_calls.end(),
                [](const std::string& call) {
                    return call.find("ui.control_set_text(") == 0u;
                });
            std::memset(message, 0, sizeof(message));
            Check(eval(script, "on_frame(60, 0.5)", message,
                       sizeof(message)) == 0u,
                  "the Ally Tracker survives a periodic snapshot refresh");
            const size_t unchanged_text_writes_after = std::count_if(
                MockAbi::ui_calls.begin(),
                MockAbi::ui_calls.end(),
                [](const std::string& call) {
                    return call.find("ui.control_set_text(") == 0u;
                });
            Check(unchanged_text_writes_after == unchanged_text_writes_before,
                  "an unchanged ally snapshot does not rewrite or rebuild "
                  "the visible rows");
            bool rendered_enemy = false;
            for (const MockAbi::UiControlRecord& record :
                 MockAbi::ui_controls) {
                if (record.live &&
                    record.text.find("Enemy") != std::string::npos) {
                    rendered_enemy = true;
                }
            }
            Check(!rendered_enemy,
                  "the Ally Tracker never renders or retains an enemy row");
            if (ally_row) MockAbi::FireUiEventForControl(ally_row->handle);
            std::memset(message, 0, sizeof(message));
            Check(eval(script, "on_frame(31, 0.016)", message,
                       sizeof(message)) == 0u,
                  "selecting an ally updates details on the next frame");
            MockAbi::UiControlRecord* ally_detail =
                MockAbi::FindLatestUiControlById("Detail");
            Check(ally_detail &&
                      ally_detail->text.find("позиция API недоступна") !=
                          std::string::npos,
                  "the Ally Tracker labels an unsourced position instead of "
                  "presenting a zero vector as live coordinates");

            WotbModV3PublicEntityLifecycleEvent ally_hidden = {};
            WOTBMOD_V3_INIT_STRUCT(
                ally_hidden, WOTBMOD_V3_ENTITY_PUBLIC_VERSION);
            ally_hidden.reason = WOTBMOD_V3_PUBLIC_ENTITY_REASON_HIDDEN;
            ally_hidden.snapshot = MockAbi::PublicEntities()[1];
            MockAbi::event_payload.assign(
                reinterpret_cast<const char*>(&ally_hidden),
                sizeof(ally_hidden));
            MockAbi::FireEvent(WOTBMOD_V3_EVENT_PUBLIC_ENTITY_REMOVED);
            std::memset(message, 0, sizeof(message));
            Check(eval(script, "on_frame(32, 0.5)", message,
                       sizeof(message)) == 0u && ally_row &&
                      ally_row->text.find("ПОСЛЕДНЕЕ") != std::string::npos,
                  "the Ally Tracker retains the last public ally update after "
                  "a removal event without querying hidden enemy state");

            MockAbi::UiControlRecord* ally_root =
                MockAbi::FindLatestUiControlById("AllyTrackerRoot");
            const WotbModV3UiHandle ally_root_handle =
                ally_root ? ally_root->handle : 0u;
            const WotbModV3UiHandle ally_screen_handle =
                ally_root ? ally_root->parent : 0u;
            for (MockAbi::UiControlRecord& record : MockAbi::ui_controls) {
                if (record.game_owned) record.live = false;
            }
            MockAbi::FireEvent(WOTBMOD_V3_EVENT_UI_SCREEN_CHANGED);
            std::memset(message, 0, sizeof(message));
            Check(eval(script, "on_frame(33, 0.016)", message,
                       sizeof(message)) == 0u && ally_root &&
                      ally_root->handle == ally_root_handle &&
                      ally_root->parent != ally_screen_handle &&
                      MockAbi::LiveControls() == 15u,
                  "Ally Tracker reparents in place on a screen change");

            MockAbi::core_context_mask =
                WOTBMOD_V3_CONTEXT_BATTLE |
                WOTBMOD_V3_CONTEXT_MOD_SCREEN;
            std::memset(message, 0, sizeof(message));
            Check(eval(script, "on_frame(45, 0.016)", message,
                       sizeof(message)) == 0u &&
                      MockAbi::LiveControls() == 0u &&
                      MockAbi::LiveUiEventSubscriptions() == 0u,
                  "the Ally Tracker fully disappears on the mod catalog "
                  "screen");
            std::memset(message, 0, sizeof(message));
            Check(eval(script, "on_disable()", message, sizeof(message)) ==
                      0u && MockAbi::LiveSubscriptions() == 0u,
                  "the Ally Tracker disables without leaving callbacks or UI");
            destroy_script(script);
        }
        MockAbi::Reset();
        Check(entry(&bootstrap, 1u, &info) == WOTBMOD_V3_OK,
              "the players test restores the default host permission ceiling");

        // ---- set_priority: proven by the order handlers actually run in ---
        MockAbi::Reset();
        std::memset(message, 0, sizeof(message));
        script = create_script(
            "priority",
            "order = ''\n"
            "a = wotb.events.subscribe('wotbmod.frame.update', function()\n"
            "  order = order .. 'a'\n"
            "end, wotb.events.PRIORITY_NORMAL)\n"
            "b = wotb.events.subscribe('wotbmod.frame.update', function()\n"
            "  order = order .. 'b'\n"
            "end, wotb.events.PRIORITY_HIGH)",
            message, sizeof(message));
        Check(script != nullptr, "two subscriptions at different priorities");
        if (script) {
            Check(MockAbi::Called(MockAbi::events_calls,
                                  "events.subscribe(wotbmod.frame.update,100,0)"),
              "the named PRIORITY_HIGH constant reached the ABI as 100");
            MockAbi::FireEvent("wotbmod.frame.update");
            Check(eval(script,
                       "if order ~= 'ba' then error('order ' .. order) end",
                       message, sizeof(message)) == 0u,
                  "the higher priority handler runs first");

            std::memset(message, 0, sizeof(message));
            Check(eval(script,
                       "order = ''\n"
                       "local ok = wotb.events.set_priority(a, "
                       "wotb.events.PRIORITY_HIGHEST)\n"
                       "if ok ~= true then error('set_priority failed') end",
                       message, sizeof(message)) == 0u,
                  "set_priority succeeds");
            Check(MockAbi::Called(MockAbi::events_calls,
                                  "events.set_priority(9000,1000)"),
                  "and reached the ABI carrying the token the *first* "
                  "subscribe returned, not the second - the two are one apart, "
                  "so a slot that lost track of which is which is visible");
            MockAbi::FireEvent("wotbmod.frame.update");
            Check(eval(script,
                       "if order ~= 'ab' then error('order ' .. order) end",
                       message, sizeof(message)) == 0u,
                  "and the delivery order really changed - a set_priority "
                  "that only recorded a call could not do this");

            // The priority argument is checked by value, not only by type.
            std::memset(message, 0, sizeof(message));
            Check(eval(script,
                       "local v, e = wotb.events.set_priority(a, 7)\n"
                       "if v ~= nil then error('an undeclared priority was "
                       "accepted') end\n"
                       "if not string.find(e, 'argument 2') then "
                       "error('missing argument index: ' .. tostring(e)) end",
                       message, sizeof(message)) == 0u,
                  "a priority the ABI never declared is refused by "
                  "CheckArgEnum, the accessor storage's get_path had to "
                  "hand-roll");
            std::memset(message, 0, sizeof(message));
            Check(eval(script,
                       "local v = wotb.events.subscribe('t', function() end, "
                       "2.5)\n"
                       "if v ~= nil then error('a float priority was "
                       "accepted') end",
                       message, sizeof(message)) == 0u,
                  "and so is a float, before the value test ever sees it");
            std::memset(message, 0, sizeof(message));
            Check(eval(script,
                       "local v = wotb.events.subscribe('t', 'not a function')\n"
                       "if v ~= nil then error('a string was accepted as a "
                       "handler') end",
                       message, sizeof(message)) == 0u,
                  "a handler that is not a function is refused at "
                  "subscription, not at the first delivery");

            destroy_script(script);
        }

        // ---- unsubscribe: the delivery stops, and the ref is released -----
        MockAbi::Reset();
        std::memset(message, 0, sizeof(message));
        script = create_script(
            "unsubscribe",
            "calls = 0\n"
            "sub = wotb.events.subscribe('wotbmod.frame.update', function()\n"
            "  calls = calls + 1\n"
            "end)",
            message, sizeof(message));
        Check(script != nullptr, "a subscription to unsubscribe");
        if (script) {
            MockAbi::FireEvent("wotbmod.frame.update");
            Check(eval(script, "if calls ~= 1 then error('calls ' .. calls) end",
                       message, sizeof(message)) == 0u,
                  "one fire, one call");
            std::memset(message, 0, sizeof(message));
            Check(eval(script,
                       "local ok = wotb.events.unsubscribe(sub)\n"
                       "if ok ~= true then error('unsubscribe failed') end",
                       message, sizeof(message)) == 0u,
                  "unsubscribe succeeds");
            Check(MockAbi::Called(MockAbi::events_calls,
                                  "events.unsubscribe(9000)"),
                  "and reached the ABI with the client's own token, which the "
                  "script never saw");
            MockAbi::FireEvent("wotbmod.frame.update");
            Check(eval(script, "if calls ~= 1 then error('calls ' .. calls) end",
                       message, sizeof(message)) == 0u,
                  "and the handler is not called again - the subscription is "
                  "gone, not merely marked");

            std::memset(message, 0, sizeof(message));
            Check(eval(script,
                       "local v, e = wotb.events.unsubscribe(sub)\n"
                       "if v ~= nil or type(e) ~= 'string' then "
                       "error('a second unsubscribe was accepted') end",
                       message, sizeof(message)) == 0u,
                  "unsubscribing twice answers nil, message rather than "
                  "unsubscribing whatever now holds that record");
            std::memset(message, 0, sizeof(message));
            Check(eval(script,
                       "local v, e = wotb.events.unsubscribe(42)\n"
                       "if v ~= nil or type(e) ~= 'string' then "
                       "error('a bare number was accepted') end",
                       message, sizeof(message)) == 0u,
                  "and a bare number is not a subscription token");

            // The registry reference is really released, not merely dropped
            // from this host's own table. luaL_unref returns the slot to the
            // registry's free list and luaL_ref reuses it, so a hundred
            // subscribe/unsubscribe cycles leave the registry exactly as
            // long as they found it - and a hundred slots longer if the
            // unref is ever removed. This is the leak the task brief warned
            // about, and it is invisible from inside the sandbox.
            const uint32_t slots_before = registry_slots(script);
            std::memset(message, 0, sizeof(message));
            Check(eval(script,
                       "for i = 1, 100 do\n"
                       "  local s = wotb.events.subscribe('churn', "
                       "function() end)\n"
                       "  if s == nil then error('subscribe ' .. i) end\n"
                       "  local ok = wotb.events.unsubscribe(s)\n"
                       "  if ok ~= true then error('unsubscribe ' .. i) end\n"
                       "end",
                       message, sizeof(message)) == 0u,
                  "a hundred subscribe/unsubscribe cycles run");
            const uint32_t slots_after = registry_slots(script);
            Check(slots_before != 0u && slots_after == slots_before,
                  "and left the Lua registry exactly as long as they found "
                  "it - unsubscribe released each reference rather than "
                  "leaking one per subscription for the life of the session");
            Check(MockAbi::CountCalled(MockAbi::events_calls,
                                       "events.subscribe(churn,0,0)") == 100u &&
                      MockAbi::CountCalled(MockAbi::events_calls,
                                           "events.unsubscribe(9100)") == 1u,
                  "with all hundred cycles reaching the ABI, the last of them "
                  "under its own token - so the registry staying level is a "
                  "released reference, not a subscription that never "
                  "happened");

            // The ABI refusing an unsubscribe must still leave this host
            // with nothing that could be delivered into: the record is
            // already dead and the reference already released by then, so a
            // client that keeps delivering finds nothing to call.
            std::memset(message, 0, sizeof(message));
            Check(eval(script,
                       "fired = false\n"
                       "stubborn = wotb.events.subscribe('mod.stubborn', "
                       "function() fired = true end)\n"
                       "if stubborn == nil then error('subscribe failed') end",
                       message, sizeof(message)) == 0u,
                  "a subscription for the refused-unsubscribe case");
            MockAbi::ForceFailure(WOTBMOD_V3_E_PERMISSION_DENIED);
            std::memset(message, 0, sizeof(message));
            Check(eval(script,
                       "local v, e = wotb.events.unsubscribe(stubborn)\n"
                       "if v ~= nil or type(e) ~= 'string' then "
                       "error('a refused unsubscribe reported success') end\n"
                       "if not string.find(e, 'events.unsubscribe') then "
                       "error('missing context: ' .. tostring(e)) end",
                       message, sizeof(message)) == 0u,
                  "an unsubscribe the client refuses is reported as nil, "
                  "message with the slot's own context");
            MockAbi::ClearFailure();
            MockAbi::FireEvent("mod.stubborn");
            std::memset(message, 0, sizeof(message));
            Check(eval(script,
                       "if fired then error('a refused unsubscribe left a "
                       "live handler') end",
                       message, sizeof(message)) == 0u,
                  "and the handler is still not called afterwards - a client "
                  "that goes on delivering finds a record this host has "
                  "already retired, not a released registry slot");
            destroy_script(script);
        }

        // ---- post: the other direction, payload and flags intact ----------
        MockAbi::Reset();
        std::memset(message, 0, sizeof(message));
        script = create_script(
            "post",
            "got = nil\n"
            "got_flags = nil\n"
            "wotb.events.subscribe('mod.test.ping', function(e)\n"
            "  got = e.payload\n"
            "  got_flags = e.flags\n"
            "end)",
            message, sizeof(message));
        Check(script != nullptr, "a subscriber for a mod's own topic");
        if (script) {
            Check(eval(script,
                       "local ok = wotb.events.post('mod.test.ping', "
                       "string.char(65,66,0,67,68), "
                       "wotb.events.FLAG_STOPPABLE | "
                       "wotb.events.FLAG_COALESCIBLE)\n"
                       "if ok ~= true then error('post failed') end",
                       message, sizeof(message)) == 0u,
                  "post runs with a payload and a flag mask");
            {
                const std::string expected =
                    std::string("events.post(mod.test.ping,") +
                    std::string("AB\0CD", 5) + ",9)";
                Check(!MockAbi::events_calls.empty() &&
                          MockAbi::events_calls.back() == expected,
                      "and reached the ABI with the payload's embedded zero "
                      "intact and both flag bits set");
            }
            Check(eval(script,
                       "if got ~= string.char(65,66,0,67,68) then "
                       "error('payload ' .. tostring(got)) end\n"
                       "if got_flags ~= 9 then error('flags ' .. "
                       "tostring(got_flags)) end",
                       message, sizeof(message)) == 0u,
                  "and the payload came back to a Lua handler byte-for-byte, "
                  "through the client, without a terminator being read into "
                  "the middle of it");

            std::memset(message, 0, sizeof(message));
            Check(eval(script,
                       "local ok = wotb.events.post('mod.test.quiet')\n"
                       "if ok ~= true then error('post with no payload "
                       "failed') end",
                       message, sizeof(message)) == 0u,
                  "post's payload and flags are genuinely optional");
            Check(MockAbi::Called(MockAbi::events_calls,
                                  "events.post(mod.test.quiet,,0)"),
                  "and an omitted payload reaches the ABI as no payload at "
                  "all, with FLAG_NONE");
            std::memset(message, 0, sizeof(message));
            Check(eval(script,
                       "local v, e = wotb.events.post('t', '', 1024)\n"
                       "if v ~= nil then error('an undefined flag bit was "
                       "accepted') end",
                       message, sizeof(message)) == 0u,
                  "a bit outside the four WotbModV3EventFlags declares is "
                  "refused rather than passed to the client");
            destroy_script(script);
        }

        // ---- stop_propagation: answered synchronously, mid-dispatch -------
        MockAbi::Reset();
        std::memset(message, 0, sizeof(message));
        script = create_script(
            "stop",
            "order = ''\n"
            "wotb.events.subscribe('wotbmod.frame.update', function(e)\n"
            "  order = order .. 'high'\n"
            "  local ok = wotb.events.stop_propagation(e.dispatch)\n"
            "  if ok ~= true then order = order .. '!' end\n"
            "end, wotb.events.PRIORITY_HIGH)\n"
            "wotb.events.subscribe('wotbmod.frame.update', function()\n"
            "  order = order .. 'low'\n"
            "end, wotb.events.PRIORITY_LOW)",
            message, sizeof(message));
        Check(script != nullptr, "a stopping handler above a listening one");
        if (script) {
            MockAbi::FireEvent("wotbmod.frame.update");
            Check(eval(script,
                       "if order ~= 'high' then error('order ' .. order) end",
                       message, sizeof(message)) == 0u,
                  "stop_propagation took effect inside the handler that "
                  "called it: the lower-priority handler never ran. A "
                  "delivery marshalled to another thread could not have "
                  "answered in time to do this");
            Check(MockAbi::Called(MockAbi::events_calls,
                                  "events.stop_propagation(2882400001)"),
                  "and it reached the ABI with this dispatch's own token");
            destroy_script(script);
        }

        // ---- a faulting handler: caught, logged, and still subscribed -----
        MockAbi::Reset();
        std::memset(message, 0, sizeof(message));
        script = create_script(
            "faulting",
            "calls = 0\n"
            "wotb.events.subscribe('wotbmod.frame.update', function()\n"
            "  calls = calls + 1\n"
            "  error('handler exploded')\n"
            "end)",
            message, sizeof(message));
        Check(script != nullptr, "a handler that raises every time");
        if (script) {
            MockAbi::FireEvent("wotbmod.frame.update");
            Check(eval(script, "if calls ~= 1 then error('calls ' .. calls) end",
                       message, sizeof(message)) == 0u,
                  "the raising handler ran once");
            // Reaching this line at all is half the assertion: an error
            // raised inside a delivery that was not caught would have found
            // no error jump and reached Lua's panic handler, which aborts
            // the process rather than failing a check.
            bool logged = false;
            for (const std::string& line : MockAbi::core_calls) {
                if (line.find("core.log(4,lua,event callback failed for "
                              "'wotbmod.frame.update'") == 0u &&
                    line.find("handler exploded") != std::string::npos) {
                    logged = true;
                }
            }
            Check(logged,
                  "the fault was reported through wotbmod.core's log at ERROR "
                  "with the topic and Lua's own message, not swallowed");
            MockAbi::FireEvent("wotbmod.frame.update");
            Check(eval(script, "if calls ~= 2 then error('calls ' .. calls) end",
                       message, sizeof(message)) == 0u,
                  "and the subscription survived it - one bad frame is not a "
                  "reason to lose a subscription");
            destroy_script(script);
        }

        // ---- teardown: a destroyed script cannot be called back into ------
        MockAbi::Reset();
        std::memset(message, 0, sizeof(message));
        script = create_script(
            "teardown",
            "wotb.events.subscribe('wotbmod.frame.update', function() end)",
            message, sizeof(message));
        Check(script != nullptr, "a script with a live subscription");
        if (script) {
            destroy_script(script);
            Check(MockAbi::Called(MockAbi::events_calls,
                                  "events.unsubscribe(9000)"),
                  "destroying a script unsubscribes what it left behind, "
                  "rather than leaving the client holding a callback into a "
                  "closed state");
            Check(MockAbi::LiveSubscriptions() == 0u,
                  "and the client is left holding nothing that could still "
                  "call back into it - not merely an unsubscribe recorded, "
                  "but no live subscription remaining");
            Check(MockAbi::CountCalled(MockAbi::events_calls,
                                       "events.unsubscribe(9000)") == 1u,
                  "unsubscribed exactly once, not once per release path");
            // Firing into a freed lua_State is an access violation, which
            // exits the process with 0xC0000005 - the build script compares
            // against 0 rather than using `if errorlevel 1` precisely so a
            // crash here cannot pass. Reaching the next check is the test.
            MockAbi::FireEvent("wotbmod.frame.update");
            Check(MockAbi::LiveSubscriptions() == 0u,
                  "and firing the topic afterwards reaches nothing, instead "
                  "of a state that has been closed");
        }

        // ---- teardown when the client refuses the unsubscribe -------------
        //
        // The other half of "exactly once", and until the final review the
        // half nothing tested. lua_ownership.h states one rule for all three
        // resource kinds - a token comes off the ledger only when the client
        // answered OK - and RevokeAll's own comment says the leftovers are the
        // sweep's whole reason to exist. The teardown path forgot the token
        // unconditionally, which meant the sweep provably never had anything
        // to do and a client that refused kept a subscription into a dead
        // script forever. Two records for one token is what "the sweep
        // retried" looks like from outside.
        MockAbi::Reset();
        std::memset(message, 0, sizeof(message));
        script = create_script(
            "teardown_refused",
            "wotb.events.subscribe('wotbmod.frame.update', function() end)",
            message, sizeof(message));
        Check(script != nullptr,
              "a script with a live subscription the client will refuse to "
              "release");
        if (script) {
            MockAbi::ForceFailure(WOTBMOD_V3_E_PERMISSION_DENIED);
            destroy_script(script);
            MockAbi::ClearFailure();
            Check(MockAbi::CountCalled(MockAbi::events_calls,
                                       "events.unsubscribe(9000)") == 2u,
                  "a refused release is retried by the ownership sweep rather "
                  "than dropped - the token stays on the ledger exactly as "
                  "lua_ownership.h says it must, so the one path that can ask "
                  "again does");
        }

        // ---- teardown against a delivery already inside the state --------
        //
        // The ordering this whole file turns on, and the thing that is
        // easiest to get wrong: ReleaseEventSubscriptions marks the
        // subscriptions dead and then *waits* for in-flight deliveries, and
        // ~LuaScript calls it before it takes its own lock so that the
        // delivery being waited for can acquire that lock and finish.
        //
        // Reaching that window needs a delivery stopped in the middle, which
        // the mock can do without a line of instrumentation in the host: the
        // handler's own wotb.events.get_thread lands back in the mock while
        // the delivery is holding the script lock, so blocking there parks a
        // delivery exactly where it matters.
        //
        // What each assertion below is worth, measured rather than assumed,
        // because a test of an ordering is worthless if every ordering
        // passes it:
        //
        //   The "did not return" one is over-determined and is documentation
        //   rather than a discriminator: a parked delivery holds the script
        //   lock, so ~LuaScript's own lock_guard would block even if
        //   ReleaseEventSubscriptions did nothing at all. It records the
        //   guarantee; it does not test it.
        //
        //   The LiveSubscriptions() one is the discriminator, and it was
        //   checked by breaking the thing it guards. Making step one skip
        //   records with a delivery in flight - the naive reading, and the
        //   one someone will reach for - fails this exact check and nothing
        //   else in the suite. Removing ReleaseEventSubscriptions entirely
        //   does not reach here at all: the earlier teardown test fires into
        //   a closed state and the process dies.
        //
        //   The "completed after release" one catches the deadlock: waiting
        //   for the in-flight count while holding the script lock the parked
        //   delivery needs would hang here rather than fail, which the build
        //   script surfaces as a suite that never finishes.
        MockAbi::Reset();
        std::memset(message, 0, sizeof(message));
        script = create_script(
            "teardown_race",
            "ran = false\n"
            "wotb.events.subscribe('wotbmod.frame.update', function(e)\n"
            "  wotb.events.get_thread(e.dispatch)\n"
            "  ran = true\n"
            "end)",
            message, sizeof(message));
        Check(script != nullptr, "a script for the teardown race");
        if (script) {
            MockAbi::ArmGate();
            std::thread deliver([]() {
                MockAbi::FireEvent("wotbmod.frame.update");
            });
            // The delivery is now parked inside the state, holding the
            // script lock, with its subscription marked in flight.
            //
            // Checked rather than assumed: if no delivery ever arrives - a
            // refused subscribe, a handler that raised first - everything below
            // would be asserting about a window that never opened, and an
            // unbounded wait would hang the process instead of saying so.
            Check(MockAbi::WaitForGateEntered(),
                  "a delivery parks inside the state for the teardown race");

            std::atomic<bool> destroyed(false);
            void* const doomed = script;
            std::thread teardown([&destroyed, doomed, destroy_script]() {
                destroy_script(doomed);
                destroyed.store(true);
            });

            // It must not have finished. Polled rather than slept once, so a
            // slow machine lengthens the evidence instead of weakening it:
            // the only thing that can let it through is the release below.
            for (int i = 0; i < 100 && !destroyed.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            Check(!destroyed.load(),
                  "destroying a script does not return while a delivery is "
                  "still inside its state - it waits, rather than closing a "
                  "lua_State a live callback is about to touch");

            MockAbi::ReleaseGate();
            teardown.join();
            deliver.join();
            Check(destroyed.load(),
                  "and it completes once that delivery leaves - the wait ends "
                  "rather than deadlocking, which it would if the release ran "
                  "under the lock the parked delivery holds");
            Check(MockAbi::LiveSubscriptions() == 0u,
                  "with the subscription revoked at the client afterwards, "
                  "not abandoned because a delivery happened to be running "
                  "when teardown started");
            script = nullptr;
        }

        // The same again through the run() shim, which destroys its script
        // before it returns: a subscription made by a throwaway script must
        // not outlive it either.
        MockAbi::Reset();
        if (run) {
            std::memset(message, 0, sizeof(message));
            Check(run("wotb.events.subscribe('wotbmod.frame.update', "
                      "function() end)",
                      message, sizeof(message)) == 0u,
                  "a throwaway script may subscribe");
            MockAbi::FireEvent("wotbmod.frame.update");
            Check(MockAbi::Called(MockAbi::events_calls,
                                  "events.unsubscribe(9000)"),
                  "and its subscription is revoked with it");
        }

        // ---- the threading rule, exercised -------------------------------
        //
        // A callback can arrive on the render thread or a worker while the
        // main thread is already inside the same state, and two threads
        // inside one lua_State is memory corruption rather than a race for a
        // value. Every delivery therefore takes the script's own lock.
        //
        // "It did not crash" is not evidence of that, so this was measured
        // rather than assumed: a build with LuaScript::Entry's exclusion
        // replaced by a per-thread mutex - everything else identical - was
        // run eight times. What it actually does, stated plainly because the
        // difference matters to whoever changes this next:
        //
        //   All eight died on a hard fault, seven of them inside phase one:
        //   five 0xC0000409 (fail-fast, a corrupted stack cookie) and two
        //   0xC0000005 (access violation). Every one of them was caught,
        //   because tests\build_lua_host_tests.cmd compares the exit code
        //   against 0 rather than using `if errorlevel 1` - which reads as
        //   >= 1 and would have walked straight past all eight.
        //
        //   None of the assertions below ever ran in those eight. The
        //   process dies inside the host's own delivery path - the event
        //   table a second thread is building in a VM the first thread is
        //   already mutating - before any handler-level or mock-level
        //   witness can fire. In the single run that survived phase one, the
        //   C++ witness had still never seen two deliveries inside the state
        //   at once.
        //
        // So: the crash is the signal, and the assertions below are the
        // backstop for the case where a violation happens to be survivable -
        // which is worth having, because the survivable case is the one that
        // ships. Nobody should read them as the primary evidence.
        MockAbi::Reset();
        MockAbi::event_thread_role =
            static_cast<uint32_t>(WOTBMOD_V3_THREAD_WORKER);
        MockAbi::witness_concurrency = true;
        std::memset(message, 0, sizeof(message));
        script = create_script(
            "threading_witness",
            "inside = 0\n"
            "overlap = false\n"
            "calls = 0\n"
            "wotb.events.subscribe('wotbmod.frame.update', function(e)\n"
            "  inside = inside + 1\n"
            "  if inside > 1 then overlap = true end\n"
            // These two calls out to the ABI bracket the witness span: the
            // mock raises a counter in get_thread and lowers it in
            // get_context, so it is raised for the whole of this handler
            // body rather than for an instant inside one slot. Two handlers
            // running at once are then two threads inside the span at once,
            // which the mock reports the moment it sees it.
            "  local role = wotb.events.get_thread(e.dispatch)\n"
            "  if role ~= wotb.events.THREAD_WORKER then "
            "    error('role ' .. tostring(role)) end\n"
            "  local t = {}\n"
            "  for i = 1, 20 do t[#t + 1] = i end\n"
            "  calls = calls + 1\n"
            "  inside = inside - 1\n"
            "  wotb.events.get_context(e.dispatch)\n"
            "end)",
            message, sizeof(message));
        Check(script != nullptr, "a script subscribed for the threading test");
        if (script) {
            std::thread other([]() {
                for (int i = 0; i < 400; ++i) {
                    MockAbi::FireEvent("wotbmod.frame.update");
                }
            });
            for (int i = 0; i < 400; ++i) {
                MockAbi::FireEvent("wotbmod.frame.update");
            }
            other.join();
            MockAbi::witness_concurrency = false;

            // The load-bearing one, and the only assertion here that does
            // not live in the memory it is making a claim about. The span it
            // measures is the whole handler body, not one call inside it -
            // see the mock's own comment for why that distinction is what
            // makes this a witness rather than a decoration.
            Check(MockAbi::max_deliveries_inside.load() == 1,
                  "across 800 deliveries from two threads, never more than "
                  "one was inside the script's state at a time - the "
                  "invariant LuaScript::Entry exists to hold, measured in "
                  "the test's own memory rather than in the state under "
                  "test");
            Check(MockAbi::deliveries_inside.load() == 0,
                  "and every delivery that entered the state also left it");
            Check(eval(script,
                       "if calls ~= 800 then error('calls ' .. calls) end",
                       message, sizeof(message)) == 0u,
                  "800 deliveries produced exactly 800 handler calls - no "
                  "update was lost to a second thread interleaving a read, "
                  "an add and a store on one VM");
            Check(eval(script,
                       "if overlap then error('two threads were inside one "
                       "lua_State at once') end",
                       message, sizeof(message)) == 0u,
                  "and the handler never observed itself running twice at "
                  "once");
            destroy_script(script);
        }

        // Phase two: the same two threads, but with a handler that allocates
        // - tables, strings, enough garbage to keep the collector working -
        // so that an unlocked second thread has something to corrupt rather
        // than an instant to slip through. Surviving this with a coherent
        // state is the claim.
        MockAbi::Reset();
        MockAbi::event_thread_role =
            static_cast<uint32_t>(WOTBMOD_V3_THREAD_WORKER);
        std::memset(message, 0, sizeof(message));
        script = create_script(
            "threading_load",
            "calls = 0\n"
            "checksum = 0\n"
            "wotb.events.subscribe('wotbmod.frame.update', function(e)\n"
            "  local t = {}\n"
            "  for i = 1, 60 do t[#t + 1] = tostring(i) .. e.topic end\n"
            "  checksum = checksum + #table.concat(t)\n"
            "  calls = calls + 1\n"
            "end)",
            message, sizeof(message));
        Check(script != nullptr, "a script whose handler allocates");
        if (script) {
            std::thread other([]() {
                for (int i = 0; i < 400; ++i) {
                    MockAbi::FireEvent("wotbmod.frame.update");
                }
            });
            for (int i = 0; i < 400; ++i) {
                MockAbi::FireEvent("wotbmod.frame.update");
            }
            other.join();

            Check(eval(script,
                       "if calls ~= 800 then error('calls ' .. calls) end\n"
                       // 60 entries of tostring(i) .. topic: 9 one-digit
                       // and 51 two-digit indices - 111 digits in all - plus
                       // 60 copies of the topic. Derived rather than typed,
                       // so the assertion cannot drift from the topic.
                       "local per = 111 + 60 * #'wotbmod.frame.update'\n"
                       "if checksum ~= 800 * per then "
                       "error('checksum ' .. checksum .. ' wanted ' .. "
                       "(800 * per)) end",
                       message, sizeof(message)) == 0u,
                  "800 allocating deliveries from two threads left the state "
                  "coherent: every call counted, and every one of them built "
                  "and measured its own garbage correctly");
            destroy_script(script);
        }

        // A delivery from another thread is not deferred to the main one:
        // the handler has already run by the time the firing thread returns,
        // which is what makes a synchronous stop_propagation possible at all.
        MockAbi::Reset();
        MockAbi::event_thread_role = WOTBMOD_V3_THREAD_RENDER;
        std::memset(message, 0, sizeof(message));
        script = create_script(
            "offthread",
            "role = nil\n"
            "wotb.events.subscribe('wotbmod.frame.update', function(e)\n"
            "  role = e.thread_role\n"
            "  wotb.events.stop_propagation(e.dispatch)\n"
            "end)",
            message, sizeof(message));
        Check(script != nullptr, "a script that handles an off-thread event");
        if (script) {
            bool stopped_during_the_call = false;
            std::thread render([&stopped_during_the_call]() {
                MockAbi::FireEvent("wotbmod.frame.update");
                // Recorded before this thread returns from FireEvent, so a
                // delivery that had been queued for the main thread could not
                // have got here.
                stopped_during_the_call = MockAbi::Called(
                    MockAbi::events_calls, "events.stop_propagation(2882400001)");
            });
            render.join();
            Check(stopped_during_the_call,
                  "a handler on a render-thread delivery answered "
                  "stop_propagation before the firing thread returned - "
                  "delivery is synchronous on the arriving thread, which is "
                  "what marshalling to the main thread would have cost");
            Check(eval(script,
                       "if role ~= wotb.events.THREAD_RENDER then "
                       "error('role ' .. tostring(role)) end",
                       message, sizeof(message)) == 0u,
                  "and the handler saw the render thread's own role");
            destroy_script(script);
        }

        // ---- the failure switch, on this interface's own slots ------------
        MockAbi::Reset();
        std::memset(message, 0, sizeof(message));
        script = create_script("eventfail", "", message, sizeof(message));
        Check(script != nullptr, "an empty script for the failure path");
        if (script) {
            MockAbi::ForceFailure(WOTBMOD_V3_E_PERMISSION_DENIED);
            Check(eval(script,
                       "local v, e = wotb.events.subscribe('t', function() "
                       "end)\n"
                       "if v ~= nil then error('subscribe should have "
                       "failed') end\n"
                       "if not string.find(e, 'events.subscribe') then "
                       "error('missing context: ' .. tostring(e)) end",
                       message, sizeof(message)) == 0u,
                  "a refused subscribe answers nil, message with the slot's "
                  "own context rather than a subscription handle for a "
                  "subscription that does not exist");
            MockAbi::ClearFailure();
            destroy_script(script);
            // And nothing was left behind by the refusal: the client sees no
            // unsubscribe for a subscription it never made.
            //
            // *After* destroy_script, and that is the whole assertion. Before
            // it this check could not fail for any reason: teardown is what
            // walks the ownership ledger and issues unsubscribe, so nothing
            // had yet had the chance to release anything, refused subscribe or
            // not. It was two facts short of a test - the token also cannot be
            // 9000 on a refused subscribe, because 9000 is what the mock hands
            // out on a *successful* one and the refusal returns before minting
            // it. Moved here, the check is the real question: a refused
            // subscribe must leave nothing on the ledger for RevokeAll to
            // release, and RevokeAll has now run.
            Check(!MockAbi::CalledContaining(MockAbi::events_calls,
                                             "events.unsubscribe("),
                  "and left nothing on the ledger for teardown to release: a "
                  "subscribe the client refused created nothing to take back");
        }

        // ---- the five convenience preludes -------------------------------
        //
        // wotb.log, wotb.json, wotb.timer, wotb.battle and wotb.config are
        // Lua source loaded into every state after the C bindings
        // (lua_preludes.cpp). They own no permission and no ABI resource;
        // everything they can create was created by a C binding that already
        // recorded it. What has to be proven about them is therefore not
        // "does the slot reach the ABI" but "does this stay a value-returning,
        // non-raising, non-hanging layer when the thing underneath it is not
        // there" - plus the two guarantees that would cost a player real
        // frames or real trust if they broke: the timer's lazy subscription
        // and the battle snapshot's refusal to answer with a zero.
        MockAbi::Reset();
        std::memset(message, 0, sizeof(message));
        script = create_script("preludes-present", "", message,
                               sizeof(message));
        Check(script != nullptr, "an empty script for the prelude probes");
        if (script) {
            Check(eval(script,
                       "for _, name in ipairs({'log', 'json', 'timer', "
                       "'battle', 'config'}) do\n"
                       "  if type(wotb[name]) ~= 'table' then\n"
                       "    error('wotb.' .. name .. ' is ' .. "
                       "type(wotb[name]))\n"
                       "  end\n"
                       "end",
                       message, sizeof(message)) == 0u,
                  "every convenience prelude is installed into a plain "
                  "script's state, alongside wotb.players and wotb.context");

            // Absence, module by module. Each prelude resolves the table it
            // stands on at call time, so clearing that table inside the
            // sandbox is exactly the state a client that never published the
            // interface leaves - and the answer must be nil plus a message,
            // never a raise and never a plausible-looking default.
            Check(eval(script,
                       "wotb.core = nil\n"
                       "local v, e = wotb.log.info('x')\n"
                       "if v ~= nil then error('log answered without core') "
                       "end\n"
                       "if type(e) ~= 'string' or not string.find(e, "
                       "'log.info') then error('log: ' .. tostring(e)) end\n"
                       "local v2, e2 = wotb.log.write(2, 'x')\n"
                       "if v2 ~= nil or type(e2) ~= 'string' then "
                       "error('log.write') end",
                       message, sizeof(message)) == 0u,
                  "wotb.log answers nil plus a prefixed message when "
                  "wotb.core is absent, and never raises");
            destroy_script(script);
        }

        MockAbi::Reset();
        std::memset(message, 0, sizeof(message));
        script = create_script("preludes-absent", "", message,
                               sizeof(message));
        Check(script != nullptr, "an empty script for the absence probes");
        if (script) {
            Check(eval(script,
                       "wotb.events = nil\n"
                       "local v, e = wotb.timer.after(10, function() end)\n"
                       "if v ~= nil then error('timer scheduled with no "
                       "events interface') end\n"
                       "if type(e) ~= 'string' or not string.find(e, "
                       "'timer.after') then error('timer: ' .. tostring(e)) "
                       "end\n"
                       "if wotb.timer.count() ~= 0 then error('a refused "
                       "schedule left a timer behind') end\n"
                       "if wotb.timer.subscribed() then error('a refused "
                       "schedule left a subscription behind') end\n"
                       "local bv, be = wotb.battle.snapshot()\n"
                       "if bv ~= nil then error('battle answered with no "
                       "events interface') end\n"
                       "if type(be) ~= 'string' then error('battle: ' .. "
                       "tostring(be)) end",
                       message, sizeof(message)) == 0u,
                  "wotb.timer and wotb.battle answer nil plus a message when "
                  "wotb.events is absent, leaving no half-made subscription");

            Check(eval(script,
                       "wotb.storage = nil\n"
                       "local store = wotb.config.new({ backend = 'storage', "
                       "key = 'k', schema = { a = { type = 'integer', "
                       "default = 1 } } })\n"
                       "if store == nil then error('config.new needs no "
                       "interface') end\n"
                       "local v, e = store:get('a')\n"
                       "if v ~= nil then error('config read with no storage') "
                       "end\n"
                       "if type(e) ~= 'string' or not string.find(e, "
                       "'wotb.storage is unavailable') then error('config: ' "
                       ".. tostring(e)) end",
                       message, sizeof(message)) == 0u,
                  "wotb.config answers nil plus a message when wotb.storage "
                  "is absent, and declaring a schema needs no interface");

            // WotbModV3SettingsApiV1 is genuinely absent from this mock
            // client - the synthetic bootstrap answers no settings query at
            // all - so this is a real absence rather than a simulated one.
            //
            // "Absent" here does not mean `wotb.settings == nil`, and the
            // difference is worth writing down because it changed under this
            // task: RegisterGeneratedConstants merges a constants table onto
            // wotb.<interface> without asking whether the client published
            // the interface, so wotb.settings can exist as a table of
            // constants with not one callable slot on it. The honest test is
            // therefore whether the *slot* is callable, and the honest answer
            // from the config layer names the slot it could not reach.
            //
            // The second half is the check that the schema layer was not
            // bolted onto wotb.settings itself: its entry point is
            // wotb.config.new, and nothing of it appears on wotb.settings,
            // where it would sit among slots the client never published.
            Check(eval(script,
                       "local settings = wotb.settings\n"
                       "if settings ~= nil and type(settings.get_int) == "
                       "'function' then error('this mock publishes no "
                       "settings interface, so no settings slot may be "
                       "callable') end\n"
                       "if settings ~= nil and settings.new ~= nil then "
                       "error('the schema layer was installed onto "
                       "wotb.settings') end\n"
                       "if type(wotb.config.new) ~= 'function' then "
                       "error('wotb.config.new is missing') end\n"
                       "local store = wotb.config.new({ schema = { a = { "
                       "type = 'integer', default = 1 } } })\n"
                       "local v, e = store:get('a')\n"
                       "if v ~= nil then error('a value came back from an "
                       "unpublished interface') end\n"
                       "if type(e) ~= 'string' or not string.find(e, "
                       "'unavailable') then error('settings: ' .. "
                       "tostring(e)) end",
                       message, sizeof(message)) == 0u,
                  "the settings-backed config layer names the slot it could "
                  "not reach when the client publishes no settings "
                  "interface, and lives on wotb.config rather than being "
                  "bolted onto wotb.settings");
            destroy_script(script);
        }

        // ---- wotb.json: round trip, depth, and malformed input ------------
        //
        // Run through the plain `run` shim: json touches no interface at all,
        // so there is nothing to fire at it and nothing to tear down.
        if (run) {
            MockAbi::Reset();
            std::memset(message, 0, sizeof(message));
            Check(run("local j = wotb.json\n"
                      "local cases = {\n"
                      "  '{\"a\":1,\"b\":[1,2,3],\"c\":\"x\",\"d\":true,"
                      "\"e\":null}',\n"
                      "  '[]', '{}', '[[[1]]]', '1.5', '-12',\n"
                      "  '{\"a\":{\"b\":{\"c\":[1,2,{\"d\":false}]}}}',\n"
                      "}\n"
                      "for _, text in ipairs(cases) do\n"
                      "  local value, decode_error = j.decode(text)\n"
                      "  if value == nil then error(text .. ' -> ' .. "
                      "tostring(decode_error)) end\n"
                      "  local encoded, encode_error = j.encode(value)\n"
                      "  if encoded ~= text then error(text .. ' round-trips "
                      "to ' .. tostring(encoded) .. ' ' .. "
                      "tostring(encode_error)) end\n"
                      "end",
                      message, sizeof(message)) == 0u,
                  "wotb.json round-trips objects, arrays, nesting, floats, "
                  "negative integers, booleans and null byte-for-byte");

            std::memset(message, 0, sizeof(message));
            Check(run("local j = wotb.json\n"
                      "if j.encode(j.as_array({})) ~= '[]' then error('empty "
                      "array') end\n"
                      "if j.encode({}) ~= '{}' then error('empty object') "
                      "end\n"
                      "if not j.is_array(j.decode('[]')) then error('decoded "
                      "[] is not tagged') end\n"
                      "if j.is_array(j.decode('{}')) then error('decoded {} "
                      "is tagged') end\n"
                      "if j.decode('null') ~= j.null then error('null "
                      "sentinel') end\n"
                      "if j.decode('false') ~= false then error('false') end",
                      message, sizeof(message)) == 0u,
                  "an empty JSON array and an empty JSON object stay "
                  "distinguishable in both directions, and null decodes to a "
                  "sentinel rather than to Lua nil");

            // The hazard this exists for: decode must not be recursive. A
            // 100,000-deep input against a recursive parser is a C stack
            // overflow, which is not a Lua error and cannot be caught - it is
            // the game's process. The depth limit refuses first; the explicit
            // stack is what makes the refusal cheap rather than lucky.
            std::memset(message, 0, sizeof(message));
            Check(run("local j = wotb.json\n"
                      "local hostile = string.rep('[', 100000) .. "
                      "string.rep(']', 100000)\n"
                      "local value, decode_error = j.decode(hostile)\n"
                      "if value ~= nil then error('100k levels accepted') "
                      "end\n"
                      "if type(decode_error) ~= 'string' then error('no "
                      "message') end\n"
                      "if j.MAX_DEPTH ~= 64 then error('documented depth "
                      "limit moved to ' .. tostring(j.MAX_DEPTH)) end\n"
                      "if j.decode(string.rep('[', 64) .. string.rep(']', 64)) "
                      "== nil then error('64 levels refused') end\n"
                      "if j.decode(string.rep('[', 65) .. string.rep(']', 65)) "
                      "~= nil then error('65 levels accepted') end\n"
                      "if j.decode(string.rep('[', 64) .. string.rep(']', 64), "
                      "{ max_depth = 63 }) ~= nil then error('max_depth "
                      "ignored') end",
                      message, sizeof(message)) == 0u,
                  "a 100,000-deep nested document is refused as a value "
                  "rather than overflowing the C stack, the documented "
                  "64-level limit is exactly where it refuses, and a caller "
                  "may lower it");

            std::memset(message, 0, sizeof(message));
            Check(run("local j = wotb.json\n"
                      "local bad = { '', '{', '[1,]', '{\"a\"}', '{\"a\":}', "
                      "'tru', '01', '1e', '\"open', '[1] 2', 'nul', '[1,2', "
                      "'1.', '--1', '[}', '\"\\\\q\"' }\n"
                      "for _, text in ipairs(bad) do\n"
                      "  local value, decode_error = j.decode(text)\n"
                      "  if value ~= nil then error('accepted <' .. text .. "
                      "'>') end\n"
                      "  if type(decode_error) ~= 'string' then error('no "
                      "message for <' .. text .. '>') end\n"
                      "end\n"
                      "if j.encode(0/0) ~= nil then error('nan encoded') end\n"
                      "if j.encode(math.huge) ~= nil then error('inf "
                      "encoded') end\n"
                      "local cycle = {}\n"
                      "cycle.self = cycle\n"
                      "if j.encode(cycle) ~= nil then error('cycle encoded') "
                      "end\n"
                      "if j.encode({ 1, 2, mixed = 3 }) ~= nil then "
                      "error('a mixed table encoded') end",
                      message, sizeof(message)) == 0u,
                  "sixteen malformed documents, a nan, an infinity, a cyclic "
                  "table and a table mixing array and object keys are each "
                  "refused as nil plus a message rather than raised");
        }

        // ---- wotb.timer: the FPS regression guard -------------------------
        //
        // docs/API_STATUS_RU.md:139-162 records a shipped FPS collapse caused
        // by a mod doing per-frame work on wotbmod.frame.update. The rule that
        // came out of it is that a script with no live timer must hold no
        // subscription to that topic at all - not a subscription whose handler
        // returns early.
        //
        // The measurement is therefore the client's own live subscription
        // table rather than its call log: "subscribe was called and
        // unsubscribe was called" would still be true of an implementation
        // that re-subscribed straight afterwards. What has to be zero is the
        // number of records the client would deliver a frame into.
        const auto live_frame_subscriptions = []() {
            size_t count = 0u;
            for (const MockAbi::EventSubscriptionRecord& record :
                     MockAbi::event_subscriptions) {
                if (record.live &&
                    record.pattern == WOTBMOD_V3_EVENT_FRAME_UPDATE) {
                    ++count;
                }
            }
            return count;
        };

        MockAbi::Reset();
        std::memset(message, 0, sizeof(message));
        script = create_script("timer-lazy", "fired = 0\n", message,
                               sizeof(message));
        Check(script != nullptr, "a script for the timer probes");
        if (script) {
            Check(live_frame_subscriptions() == 0u &&
                      !MockAbi::CalledContaining(
                          MockAbi::events_calls,
                          "events.subscribe(wotbmod.frame.update"),
                  "loading wotb.timer subscribes to nothing: a script that "
                  "never schedules a timer costs the frame pump nothing at "
                  "all");
            Check(eval(script,
                       "if wotb.timer.subscribed() then error('subscribed "
                       "before any timer') end\n"
                       "if wotb.timer.count() ~= 0 then error('count') end\n"
                       "handle = wotb.timer.after(100, function() fired = "
                       "fired + 1 end)\n"
                       "if math.type(handle) ~= 'integer' then error('after "
                       "returned ' .. tostring(handle)) end\n"
                       "if not wotb.timer.subscribed() then error('the first "
                       "timer did not subscribe') end",
                       message, sizeof(message)) == 0u,
                  "the first live timer is what creates the frame "
                  "subscription");
            Check(MockAbi::CalledContaining(
                      MockAbi::events_calls,
                      "events.subscribe(wotbmod.frame.update") &&
                      live_frame_subscriptions() == 1u,
                  "and it reached the client as exactly one live "
                  "subscription to wotbmod.frame.update");

            // Wall clock, taken from the event rather than from a frame
            // count: the frame event carries no delta field, so consecutive
            // timestamps are the only honest source. Fire three frames a
            // known number of nanoseconds apart and the timer must not fire
            // early and must fire once the wall clock has moved 100 ms.
            MockAbi::event_payload.clear();
            MockAbi::event_timestamp_ns = 1000000000ull;
            MockAbi::FireEvent(WOTBMOD_V3_EVENT_FRAME_UPDATE);
            MockAbi::event_timestamp_ns = 1050000000ull;
            MockAbi::FireEvent(WOTBMOD_V3_EVENT_FRAME_UPDATE);
            Check(eval(script,
                       "if fired ~= 0 then error('fired after 50 ms') end\n"
                       "if wotb.timer.count() ~= 1 then error('count') end",
                       message, sizeof(message)) == 0u,
                  "a 100 ms timer has not fired 50 ms of wall clock later, "
                  "however many frames have gone by");

            MockAbi::event_timestamp_ns = 1101000000ull;
            MockAbi::FireEvent(WOTBMOD_V3_EVENT_FRAME_UPDATE);
            Check(eval(script,
                       "if fired ~= 1 then error('fired ' .. fired) end\n"
                       "if wotb.timer.count() ~= 0 then error('the one-shot "
                       "is still live') end\n"
                       "if wotb.timer.subscribed() then error('the frame "
                       "subscription outlived the last timer') end",
                       message, sizeof(message)) == 0u,
                  "it fires once the wall clock passes its deadline, and the "
                  "frame subscription is dropped the moment the last timer "
                  "is gone");
            Check(MockAbi::CalledContaining(MockAbi::events_calls,
                                            "events.unsubscribe("),
                  "and the unsubscribe really reached the client while the "
                  "script was still alive - this is the FPS regression "
                  "guard, not teardown doing it later");

            // The regression this whole block exists for, stated as the
            // client would see it: after the last timer, the client holds no
            // record it would deliver a frame into. Not "a handler that
            // returns early" - no record.
            Check(live_frame_subscriptions() == 0u,
                  "and the client is left holding zero live records for "
                  "wotbmod.frame.update, so a rendered frame does not reach "
                  "this script at all");
            MockAbi::event_timestamp_ns = 2000000000ull;
            MockAbi::FireEvent(WOTBMOD_V3_EVENT_FRAME_UPDATE);
            Check(eval(script,
                       "if fired ~= 1 then error('a frame published after "
                       "the last timer still reached something: ' .. fired) "
                       "end",
                       message, sizeof(message)) == 0u,
                  "and a frame published afterwards changes nothing");

            // Cancelling the last timer has to drop the subscription too, not
            // only firing it.
            Check(eval(script,
                       "local id = wotb.timer.every(10, function() end)\n"
                       "if not wotb.timer.subscribed() then error('every did "
                       "not subscribe') end\n"
                       "if wotb.timer.cancel(id) ~= true then error('cancel') "
                       "end\n"
                       "if wotb.timer.subscribed() then error('cancelling "
                       "the last timer left the subscription') end\n"
                       "if wotb.timer.cancel(id) ~= nil then error('a second "
                       "cancel succeeded') end",
                       message, sizeof(message)) == 0u,
                  "cancelling the last timer drops the frame subscription as "
                  "surely as firing it does, and a second cancel of the same "
                  "id is refused as a value");
            Check(live_frame_subscriptions() == 0u,
                  "and the client holds no live frame record after the "
                  "cancel either");

            // One callback raising must not stop the tick, and a long gap
            // must not turn into a catch-up burst.
            Check(eval(script,
                       "ticks = 0\n"
                       "ok_ran = false\n"
                       "wotb.timer.after(1, function() error('boom') end)\n"
                       "wotb.timer.after(1, function() ok_ran = true end)\n"
                       "repeater = wotb.timer.every(10, function() ticks = "
                       "ticks + 1 end)",
                       message, sizeof(message)) == 0u,
                  "three timers, one of which raises");
            MockAbi::event_timestamp_ns = 3000000000ull;
            MockAbi::FireEvent(WOTBMOD_V3_EVENT_FRAME_UPDATE);
            MockAbi::event_timestamp_ns = 3002000000ull;
            MockAbi::FireEvent(WOTBMOD_V3_EVENT_FRAME_UPDATE);
            Check(eval(script,
                       "if not ok_ran then error('a raising callback stopped "
                       "the tick') end",
                       message, sizeof(message)) == 0u,
                  "a timer callback that raises is caught and the rest of "
                  "the tick still runs");
            MockAbi::event_timestamp_ns = 60000000000ull;   // a 57 s gap
            MockAbi::FireEvent(WOTBMOD_V3_EVENT_FRAME_UPDATE);
            Check(eval(script,
                       "if ticks ~= 2 then error('a 57 second gap produced ' "
                       ".. ticks .. ' ticks') end",
                       message, sizeof(message)) == 0u,
                  "a repeating timer that missed 5,700 periods while frames "
                  "were not being pumped fires once and re-bases, rather "
                  "than firing 5,700 times in one frame");
            destroy_script(script);
        }

        // ---- wotb.battle: unavailable, never a zero ----------------------
        //
        // The honesty requirement, made into an assertion. A field whose
        // source event has not fired is absent from the snapshot and carries
        // a reason; it is never reported as 0, and health is never attributed
        // to the local vehicle before the client has said which vehicle that
        // is.
        MockAbi::Reset();
        std::memset(message, 0, sizeof(message));
        script = create_script("battle-snapshot",
                               "if wotb.battle.start() ~= true then "
                               "error('start') end\n",
                               message, sizeof(message));
        Check(script != nullptr, "a script tracking battle state");
        if (script) {
            Check(eval(script,
                       "local snap = wotb.battle.snapshot()\n"
                       "if snap == nil then error('no snapshot') end\n"
                       "for _, name in ipairs({'health', 'ammo_count', "
                       "'reload_progress', 'camera_mode', 'lifecycle', "
                       "'local_entity_id'}) do\n"
                       "  if snap[name] ~= nil then error(name .. ' answered "
                       "' .. tostring(snap[name]) .. ' before its event "
                       "fired') end\n"
                       "  if type(snap.unavailable[name]) ~= 'string' then\n"
                       "    error(name .. ' is missing without a reason')\n"
                       "  end\n"
                       "end\n"
                       "if not string.find(snap.unavailable.max_health, "
                       "'permanently') then error('max_health reason') end\n"
                       "if not string.find(snap.unavailable.damage_dealt, "
                       "'deliberately') then error('damage_dealt reason') "
                       "end",
                       message, sizeof(message)) == 0u,
                  "every battle field whose source event has not fired is "
                  "absent with a written reason rather than reported as 0, "
                  "and max_health and damage_dealt say why they will never "
                  "arrive at all");

            // A health event for some vehicle, before the client has said
            // which vehicle is ours. There is no honest way to attribute it,
            // so it is counted and dropped - not recorded as the local
            // player's health.
            WotbModV3ClientEventEnvelope health = {};
            WOTBMOD_V3_INIT_STRUCT(
                health, WOTBMOD_V3_CLIENT_EVENT_ENVELOPE_VERSION);
            health.type = WOTBMOD_V3_CLIENT_EVENT_VEHICLE_HEALTH_CHANGED;
            health.primary_entity_id = 55u;
            health.payload_size = sizeof(WotbModV3VehicleEventData);
            health.payload.vehicle.entity_id = 55u;
            health.payload.vehicle.previous_health = 1000;
            health.payload.vehicle.health = 900;
            MockAbi::event_payload.assign(
                reinterpret_cast<const char*>(&health), sizeof(health));
            MockAbi::FireEvent(WOTBMOD_V3_EVENT_VEHICLE_HEALTH_CHANGED);
            Check(eval(script,
                       "local snap = wotb.battle.snapshot()\n"
                       "if snap.health ~= nil then error('unattributable "
                       "health was recorded as ' .. tostring(snap.health)) "
                       "end\n"
                       "if snap.ignored.health_without_local_entity_id ~= 1 "
                       "then error('the drop was not counted: ' .. "
                       "tostring(snap.ignored.health_without_local_entity_id))"
                       " end",
                       message, sizeof(message)) == 0u,
                  "a health event that arrives before the local vehicle's "
                  "entity id is known is counted and dropped, never "
                  "attributed to the local player on the strength of being "
                  "the only health event so far");

            WotbModV3ClientEventEnvelope local_vehicle = {};
            WOTBMOD_V3_INIT_STRUCT(
                local_vehicle, WOTBMOD_V3_CLIENT_EVENT_ENVELOPE_VERSION);
            local_vehicle.type =
                WOTBMOD_V3_CLIENT_EVENT_LOCAL_VEHICLE_CHANGED;
            local_vehicle.primary_entity_id = 55u;
            local_vehicle.payload_size = sizeof(WotbModV3VehicleEventData);
            local_vehicle.payload.vehicle.entity_id = 55u;
            MockAbi::event_payload.assign(
                reinterpret_cast<const char*>(&local_vehicle),
                sizeof(local_vehicle));
            MockAbi::FireEvent(WOTBMOD_V3_EVENT_LOCAL_VEHICLE_CHANGED);
            MockAbi::event_payload.assign(
                reinterpret_cast<const char*>(&health), sizeof(health));
            MockAbi::event_timestamp_ns = 4242424242ull;
            MockAbi::FireEvent(WOTBMOD_V3_EVENT_VEHICLE_HEALTH_CHANGED);
            Check(eval(script,
                       "local snap = wotb.battle.snapshot()\n"
                       "if snap.local_entity_id ~= 55 then error('local id ' "
                       ".. tostring(snap.local_entity_id)) end\n"
                       "if snap.health ~= 900 then error('health ' .. "
                       "tostring(snap.health)) end\n"
                       "if snap.previous_health ~= 1000 then error('previous "
                       "health') end\n"
                       "if snap.source.health ~= "
                       "wotb.events.TOPIC_VEHICLE_HEALTH_CHANGED then\n"
                       "  error('provenance ' .. tostring(snap.source.health))"
                       "\nend\n"
                       "if snap.updated_ns.health ~= 4242424242 then "
                       "error('timestamp ' .. "
                       "tostring(snap.updated_ns.health)) end",
                       message, sizeof(message)) == 0u,
                  "once the client has named the local vehicle, a health "
                  "event for that entity is recorded with the topic it came "
                  "from and the moment it arrived");

            // Another vehicle's health is not ours, and saying so is the same
            // rule as the one above rather than a second one.
            health.primary_entity_id = 77u;
            health.payload.vehicle.entity_id = 77u;
            health.payload.vehicle.health = 100;
            MockAbi::event_payload.assign(
                reinterpret_cast<const char*>(&health), sizeof(health));
            MockAbi::FireEvent(WOTBMOD_V3_EVENT_VEHICLE_HEALTH_CHANGED);
            Check(eval(script,
                       "if wotb.battle.snapshot().health ~= 900 then "
                       "error('another vehicle overwrote ours') end",
                       message, sizeof(message)) == 0u,
                  "a health event for a different entity does not overwrite "
                  "the local vehicle's health");

            // Leaving a battle clears every per-battle observation. Last
            // battle's health reported as this battle's is exactly the lie
            // the unavailable-not-zero rule exists to prevent.
            WotbModV3ClientEventEnvelope left = {};
            WOTBMOD_V3_INIT_STRUCT(
                left, WOTBMOD_V3_CLIENT_EVENT_ENVELOPE_VERSION);
            left.type = WOTBMOD_V3_CLIENT_EVENT_BATTLE_LEFT;
            left.payload_size = sizeof(WotbModV3BattleEventData);
            MockAbi::event_payload.assign(
                reinterpret_cast<const char*>(&left), sizeof(left));
            MockAbi::FireEvent(WOTBMOD_V3_EVENT_BATTLE_LEFT);
            Check(eval(script,
                       "local snap = wotb.battle.snapshot()\n"
                       "if snap.lifecycle ~= 'left' then error('lifecycle ' "
                       ".. tostring(snap.lifecycle)) end\n"
                       "if snap.health ~= nil or snap.local_entity_id ~= nil "
                       "then error('per-battle state survived leaving') end\n"
                       "if type(snap.unavailable.health) ~= 'string' then "
                       "error('and without a reason') end",
                       message, sizeof(message)) == 0u,
                  "leaving a battle puts every per-battle field back to "
                  "unavailable, so last battle's health can never be read as "
                  "this battle's");
            Check(eval(script,
                       "if wotb.battle.stop() ~= true then error('stop') "
                       "end\n"
                       "if wotb.battle.tracking() then error('still "
                       "tracking') end",
                       message, sizeof(message)) == 0u,
                  "battle.stop hands every subscription back");
            destroy_script(script);
        }

        // ---- wotb.config: a default never overwrites a stored value -------
        //
        // The mock's storage says "present" is the one key it contains and
        // answers get_json with {"probe":1} - a document this schema does not
        // declare a single field of. That is the whole point: a schema layer
        // that wrote its defaults on read, or that rebuilt the document from
        // its own fields on write, would destroy `probe`.
        MockAbi::Reset();
        std::memset(message, 0, sizeof(message));
        script = create_script(
            "config-store",
            "store = wotb.config.new({\n"
            "  backend = 'storage', key = 'present',\n"
            "  schema = {\n"
            "    opacity = { type = 'number', default = 0.8, min = 0, "
            "max = 1 },\n"
            "    slots = { type = 'integer', default = 3 },\n"
            "    label = { type = 'string', default = 'hi' },\n"
            "    on = { type = 'boolean', default = true },\n"
            "  },\n"
            "})\n"
            "if store == nil then error('config.new') end\n",
            message, sizeof(message));
        Check(script != nullptr, "a script with a declared config schema");
        if (script) {
            Check(!MockAbi::CalledContaining(MockAbi::storage_calls,
                                             "storage.set_json("),
                  "declaring a schema writes nothing: config.new validates "
                  "and returns, it does not seed the store with defaults");

            Check(eval(script,
                       "local value, origin = store:get('opacity')\n"
                       "if value ~= 0.8 or origin ~= 'default' then\n"
                       "  error('got ' .. tostring(value) .. ' from ' .. "
                       "tostring(origin))\n"
                       "end",
                       message, sizeof(message)) == 0u,
                  "a field the store has never held answers from its "
                  "declared default and says the answer came from the "
                  "default");
            Check(!MockAbi::CalledContaining(MockAbi::storage_calls,
                                             "storage.set_json("),
                  "and reading that default wrote nothing back - a default "
                  "that persisted itself on first read would overwrite a "
                  "value written by an older version of the same mod");

            Check(eval(script,
                       "local v, e = store:set('opacity', 'not a number')\n"
                       "if v ~= nil then error('a string was accepted for a "
                       "number field') end\n"
                       "if type(e) ~= 'string' or not string.find(e, "
                       "'expects a number') then error('message: ' .. "
                       "tostring(e)) end\n"
                       "local v2 = store:set('slots', 2.5)\n"
                       "if v2 ~= nil then error('a float was accepted for an "
                       "integer field') end\n"
                       "local v3 = store:set('opacity', 2)\n"
                       "if v3 ~= nil then error('a value past its declared "
                       "maximum was accepted') end\n"
                       "local v4 = store:set('label', 7)\n"
                       "if v4 ~= nil then error('a number was accepted for a "
                       "string field') end\n"
                       "local v5 = store:set('nosuch', 1)\n"
                       "if v5 ~= nil then error('an undeclared key was "
                       "accepted') end",
                       message, sizeof(message)) == 0u,
                  "a value that does not match its declared type, or falls "
                  "outside its declared range, or names a field the schema "
                  "does not declare, is refused as nil plus a message");
            Check(!MockAbi::CalledContaining(MockAbi::storage_calls,
                                             "storage.set_json("),
                  "and not one of those five refusals reached the store: a "
                  "rejected value is never half-written");

            Check(eval(script,
                       "if store:set('opacity', 0.5) ~= true then "
                       "error('a valid set was refused') end\n"
                       "local value, origin = store:get('opacity')\n"
                       "if value ~= 0.5 or origin ~= 'stored' then\n"
                       "  error('read back ' .. tostring(value) .. ' from ' "
                       ".. tostring(origin))\n"
                       "end",
                       message, sizeof(message)) == 0u,
                  "a valid set is written through and reads back as stored "
                  "rather than as a default");
            Check(MockAbi::Called(
                      MockAbi::storage_calls,
                      "storage.set_json(present,{\"opacity\":0.5,\"probe\":1})"),
                  "and the document that reached the client is the one that "
                  "was read with the set key laid over it: `probe`, which "
                  "this schema does not declare, survived untouched, and no "
                  "default was written alongside it");
            destroy_script(script);
        }

        // ---- wotb.available: published, absent, and constants-only ---------
        //
        // docs/LUA_MODS_RU.md:197-202 documents "if the client does not
        // publish an interface, its table is not created", so every author
        // writes `if wotb.settings then`. RegisterGeneratedConstants made that
        // false for any interface with constants by merging a constants table
        // onto wotb.<name> unconditionally, and this mock is exactly that
        // case: it publishes no settings interface, and wotb.settings is a
        // live table of numbers with not one callable slot on it.
        //
        // The third assertion below is therefore the whole point of this
        // function. The first two only establish that it is not answering
        // "yes" or "no" to everything.
        if (run) {
            MockAbi::Reset();
            std::memset(message, 0, sizeof(message));
            Check(run("for _, name in ipairs({'ui', 'storage', 'events', "
                      "'core'}) do\n"
                      "  if wotb.available(name) ~= true then\n"
                      "    error(name .. ' is published by this client and "
                      "available answered ' .. tostring(wotb.available(name)))\n"
                      "  end\n"
                      "end\n"
                      "if wotb.archive ~= nil then error('this mock publishes "
                      "no archive interface and archive has no constants "
                      "table, so nothing may have created wotb.archive') "
                      "end\n"
                      "if wotb.available('archive') ~= false then error('an "
                      "interface with no table at all answered available') "
                      "end\n"
                      "for _, name in ipairs({'settings', 'audio', 'http'}) "
                      "do\n"
                      "  if type(wotb[name]) ~= 'table' then\n"
                      "    error('wotb.' .. name .. ' is not a table, so this "
                      "test is measuring nothing: the constants pass is "
                      "supposed to have created it')\n"
                      "  end\n"
                      "  if wotb.available(name) ~= false then\n"
                      "    error('wotb.' .. name .. ' is a table of constants "
                      "with no callable slot and answered available - which "
                      "is the exact case this function exists for')\n"
                      "  end\n"
                      "end\n"
                      "local value, why = wotb.available(7)\n"
                      "if value ~= nil or type(why) ~= 'string' then error('a "
                      "bad argument must be nil plus a message') end",
                      message, sizeof(message)) == 0u,
                  "wotb.available answers by looking for a callable slot: yes "
                  "for the four interfaces this mock publishes, no for one "
                  "with no table at all, and no for settings, audio and http - "
                  "each of which exists, is a table, and is nothing but "
                  "constants");
        }

        // ---- wotb.panel: not built until touched, and what it costs --------
        //
        // Laziness is the requirement this module was written under, so it is
        // asserted rather than commented: before the first index the table is
        // empty and carries a metatable, and next() is raw so an __index
        // cannot make an unbuilt module look built.
        if (run) {
            MockAbi::Reset();
            std::memset(message, 0, sizeof(message));
            Check(run("if next(wotb.panel) ~= nil then error('the framework "
                      "was built at load: ' .. tostring(next(wotb.panel))) "
                      "end\n"
                      "if getmetatable(wotb.panel) == nil then error('there is "
                      "no lazy gate at all') end\n"
                      "if type(wotb.panel.new) ~= 'function' then error('the "
                      "first index produced ' .. type(wotb.panel.new)) end\n"
                      "if next(wotb.panel) == nil then error('the first index "
                      "did not build the module') end\n"
                      "if type(wotb.panel.ANCHORS) ~= 'table' then error('the "
                      "build stopped short') end\n"
                      "if getmetatable(wotb.panel) ~= nil then error('the gate "
                      "did not remove itself, so every later miss on this "
                      "table still costs a Lua call') end",
                      message, sizeof(message)) == 0u,
                  "wotb.panel is an empty table behind a metatable until it is "
                  "indexed; the first index builds the whole framework into "
                  "that same table and takes the gate away behind it");

            // The cost, measured with the budget mechanism as the yardstick
            // rather than estimated.
            //
            // A chunk gets 100,000 VM instructions (lua_script.cpp:31). An
            // empty numeric for loop is one FORLOOP per iteration, so the
            // largest loop a chunk can finish is the budget minus whatever
            // else that chunk did - which makes the *difference* between two
            // such searches a count of instructions, in the same unit the
            // budget is denominated in.
            const auto budget_headroom = [&](const char* prologue) -> long {
                long low = 0;
                long high = 120000;
                while (low < high) {
                    const long mid = low + (high - low + 1) / 2;
                    char probe[512] = {};
                    std::snprintf(probe, sizeof(probe),
                                  "%s\nfor probe_index = 1, %ld do end",
                                  prologue, mid);
                    char probe_error[512] = {};
                    if (run(probe, probe_error, sizeof(probe_error)) == 0u) {
                        low = mid;
                    } else {
                        high = mid - 1;
                    }
                }
                return low;
            };
            const long idle = budget_headroom("local unused = 0");
            const long touched =
                budget_headroom("local unused = wotb.panel.new");
            const long build_cost = idle - touched;
            std::printf(
                "wotb.panel: building the framework costs %ld of the 100000 "
                "VM instructions in one budget (%ld left idle, %ld after the "
                "first touch)\n",
                build_cost, idle, touched);
            Check(idle > 99000 && idle <= 100000,
                  "the yardstick is the instruction budget itself: an "
                  "otherwise empty chunk gets within 1% of all 100,000 of it");
            // 2,000 is not a target, it is a tripwire. The measured cost is
            // ~125; anything approaching this bound means the build stopped
            // being a list of closure definitions and started doing work,
            // which is the thing laziness was meant to make cheap rather than
            // merely defer.
            Check(build_cost > 0 && build_cost < 2000,
                  "and building the framework spends well under 2% of one "
                  "script's budget, once, and only in a script that asked for "
                  "it");
        }

        // ---- wotb.panel: a panel builds, shows, updates and tears down -----
        MockAbi::Reset();
        MockAbi::publish_core_context = true;
        MockAbi::core_context_mask = WOTBMOD_V3_CONTEXT_BATTLE;
        std::memset(message, 0, sizeof(message));
        script = create_script(
            "panel-lifecycle",
            "clicks = 0\n"
            "panel = wotb.panel.new({\n"
            "  id = 'tracker', width = 620, height = 570,\n"
            "  anchor = 'top-right',\n"
            "  margin = { left = 128, right = 28, top = 116, bottom = 72 },\n"
            "  rows = 3,\n"
            "  buttons = { { id = 'collapse', text = '-', x = 556, y = 10,\n"
            "                width = 42, height = 40,\n"
            "                on_click = function() clicks = clicks + 1 end } },\n"
            "})\n"
            "if panel == nil then error('panel.new refused the spec') end\n",
            message, sizeof(message));
        Check(script != nullptr, "a script that declares a panel");
        if (script) {
            Check(MockAbi::LiveControls() == 0u && MockAbi::ui_calls.empty(),
                  "declaring a panel creates nothing and crosses the ABI "
                  "nowhere: new() is validation and arithmetic, so a panel can "
                  "be declared at the top of a script on a client that "
                  "publishes no UI at all");

            Check(eval(script,
                       "local ok, why = panel:mount()\n"
                       "if not ok then error('mount: ' .. tostring(why)) end\n"
                       "if not panel:mounted() then error('mount answered ok "
                       "without mounting') end\n"
                       "local x, y = panel:position()\n"
                       "if math.abs(x - (1536 - 620 - 28)) > 0.001 then "
                       "error('anchored x ' .. x) end\n"
                       "if math.abs(y - 116) > 0.001 then error('anchored y ' "
                       ".. y) end",
                       message, sizeof(message)) == 0u,
                  "a panel mounts onto the client's own active screen and "
                  "anchors itself against the viewport the client reported "
                  "(1536x864), never against the stated 1920x1080 fallback");
            Check(MockAbi::LiveControls() == 6u,
                  "and it built exactly six controls - a root, the panel "
                  "frame, three rows and one button - with no hidden template "
                  "left behind");
            Check(MockAbi::LiveUiEventSubscriptions() == 1u,
                  "one UI subscription, for the one button that declared a "
                  "handler");
            Check(MockAbi::LiveSubscriptions() == 1u &&
                      MockAbi::Called(
                          MockAbi::events_calls,
                          "events.subscribe(wotbmod.ui.screen_changed,0,1)"),
                  "and it follows the active screen through the client's own "
                  "topic rather than only by polling");

            // The cheap path. Not "it returns 'unchanged'" - that a comment
            // could claim - but "the client saw one set_text for two hundred
            // and one calls".
            const auto set_text_calls = []() {
                size_t count = 0u;
                for (const std::string& entry : MockAbi::ui_calls) {
                    if (entry.rfind("ui.control_set_text(", 0u) == 0u) ++count;
                }
                return count;
            };
            const size_t text_before = set_text_calls();
            Check(eval(script,
                       "local ok, status = panel:set_row(1, 'ALLIES 7')\n"
                       "if ok ~= true or status ~= 'sent' then error('the "
                       "first write answered ' .. tostring(status)) end\n"
                       "for _ = 1, 200 do\n"
                       "  local again, again_status = panel:set_row(1, "
                       "'ALLIES 7')\n"
                       "  if again ~= true or again_status ~= 'unchanged' "
                       "then\n"
                       "    error('a repeat answered ' .. tostring("
                       "again_status))\n"
                       "  end\n"
                       "end\n"
                       "if panel:row(1) ~= 'ALLIES 7' then error('the row "
                       "forgot its text') end",
                       message, sizeof(message)) == 0u,
                  "writing a row's text answers 'sent' the first time and "
                  "'unchanged' every time after");
            Check(set_text_calls() == text_before + 1u,
                  "and two hundred further frames of writing the same string "
                  "cost the client nothing at all - the compare happens before "
                  "wotb.ui is so much as looked up, which is the shipped FPS "
                  "collapse in docs/API_STATUS_RU.md:139-162 written out of "
                  "the framework");

            const auto snapshot_calls = []() {
                size_t count = 0u;
                for (const std::string& entry : MockAbi::ui_calls) {
                    if (entry.rfind("ui.control_get_snapshot(", 0u) == 0u) {
                        ++count;
                    }
                }
                return count;
            };
            const size_t probes_before = snapshot_calls();
            Check(eval(script,
                       "for frame = 1, 59 do\n"
                       "  if panel:update(frame) ~= true then error('update "
                       "hid the panel on frame ' .. frame) end\n"
                       "end",
                       message, sizeof(message)) == 0u,
                  "fifty-nine frames of the one-call frame driver keep the "
                  "panel up");
            Check(snapshot_calls() == probes_before,
                  "and not one of them ran the liveness probe: the probe is "
                  "the once-a-second cadence, not per-frame work");
            Check(eval(script, "panel:update(60)", message,
                       sizeof(message)) == 0u,
                  "the probe frame runs");
            Check(snapshot_calls() == probes_before + 1u,
                  "and asks the client exactly once whether the tree is still "
                  "there");

            // A click really is wired to the author's handler, through the
            // client's own dispatch.
            MockAbi::FireUiEvent();
            Check(eval(script,
                       "if clicks < 1 then error('the click never reached the "
                       "handler') end",
                       message, sizeof(message)) == 0u,
                  "a button's on_click reaches the author's function through "
                  "the client's UI dispatch");

            // The context gate, which is the whole reason both shipped panels
            // carry a frame driver at all.
            MockAbi::core_context_mask = WOTBMOD_V3_CONTEXT_MOD_SCREEN;
            Check(eval(script,
                       "if panel:update(75) ~= false then error('the panel "
                       "stayed up over the mod catalog') end\n"
                       "if panel:mounted() then error('and did not unmount') "
                       "end",
                       message, sizeof(message)) == 0u,
                  "the mod catalog taking the screen hides the panel and hands "
                  "its controls back");
            Check(MockAbi::LiveControls() == 0u &&
                      MockAbi::LiveUiEventSubscriptions() == 0u,
                  "which means every control and every UI subscription, not "
                  "merely a set_visible(false)");
            MockAbi::core_context_mask = WOTBMOD_V3_CONTEXT_BATTLE;
            Check(eval(script,
                       "if panel:update(90) ~= true then error('battle did not "
                       "bring the panel back') end\n"
                       "if panel:row(1) ~= 'ALLIES 7' then error('it came back "
                       "showing ' .. tostring(panel:row(1))) end",
                       message, sizeof(message)) == 0u,
                  "and returning to battle remounts it on the very next "
                  "eligible frame, still showing what it was showing - the "
                  "retry deadline throttles a refusing client, never a "
                  "context change");

            Check(eval(script,
                       "if panel:unmount() ~= true then error('unmount') end\n"
                       "if panel:mounted() then error('still mounted') end\n"
                       "if panel:unmount() ~= true then error('unmount is not "
                       "idempotent') end\n"
                       "local errors = panel:errors()\n"
                       "if errors ~= 0 then error('the panel recorded ' .. "
                       "errors .. ' errors: ' .. tostring(select(2, "
                       "panel:errors()))) end",
                       message, sizeof(message)) == 0u,
                  "unmount succeeds, is idempotent, and the whole lifecycle "
                  "recorded no error at all");
            Check(MockAbi::LiveControls() == 0u &&
                      MockAbi::LiveUiEventSubscriptions() == 0u &&
                      MockAbi::LiveSubscriptions() == 0u &&
                      MockAbi::GenericHandleReferences(6000u) == 0u,
                  "and teardown left the client holding nothing: no control, "
                  "no UI subscription, no screen-change subscription and no "
                  "reference on the active screen");
            destroy_script(script);
        }
        MockAbi::publish_core_context = false;
        MockAbi::core_context_mask = WOTBMOD_V3_CONTEXT_NONE;

        // ---- wotb.panel with no UI interface: values, never a raise --------
        //
        // Clearing wotb.ui inside the sandbox is the state a client that never
        // published the interface leaves, and it is why this framework lives
        // at wotb.panel rather than at wotb.ui.panel: a module reachable only
        // through the table that is missing cannot be the thing that tells an
        // author it is missing.
        MockAbi::Reset();
        std::memset(message, 0, sizeof(message));
        script = create_script("panel-no-ui", "", message, sizeof(message));
        Check(script != nullptr, "an empty script for the absent-UI probe");
        if (script) {
            Check(eval(script,
                       "wotb.ui = nil\n"
                       "local panel, why = wotb.panel.new({ id = 'blind', "
                       "width = 100, height = 60, rows = 2, contexts = false "
                       "})\n"
                       "if panel == nil then error('new needs no interface: ' "
                       ".. tostring(why)) end\n"
                       "local ok, mount_error = panel:mount()\n"
                       "if ok ~= nil then error('mount answered ' .. "
                       "tostring(ok) .. ' with no UI interface') end\n"
                       "if type(mount_error) ~= 'string' or not string.find("
                       "mount_error, 'wotb.ui is unavailable', 1, true) then\n"
                       "  error('message: ' .. tostring(mount_error))\n"
                       "end\n"
                       "local showing, reason = panel:update(0)\n"
                       "if showing ~= false or type(reason) ~= 'string' then\n"
                       "  error('update answered ' .. tostring(showing) .. ', "
                       "' .. tostring(reason))\n"
                       "end\n"
                       "if panel:unmount() ~= true then error('unmount must "
                       "survive an interface that was never there') end\n"
                       "local cached, status = panel:set_row(1, 'x')\n"
                       "if cached ~= true or status ~= 'unchanged' then\n"
                       "  error('set_row answered ' .. tostring(status))\n"
                       "end\n"
                       "local bad, bad_error = panel:set_row(9, 'x')\n"
                       "if bad ~= nil or type(bad_error) ~= 'string' then "
                       "error('a row that does not exist') end",
                       message, sizeof(message)) == 0u,
                  "with wotb.ui cleared every entry point answers a value: "
                  "new() still validates, mount() and update() name the "
                  "interface they could not reach, unmount() succeeds, and "
                  "set_row keeps the text for a mount that may yet happen - "
                  "and not one of them raises");
            Check(MockAbi::LiveControls() == 0u,
                  "and a mount that could not start created nothing to leak");
            destroy_script(script);
        }
    }

    // ---- the facade layer -------------------------------------------------
    //
    // wotb.context aliases, wotb.players details, wotb.battle handlers,
    // wotb.mod, the wotb.ges extensions and wotb.hud: pure-Lua modules over
    // raw tables (docs/LUA_MODS_RU.md, "Facade-first"). Every check takes the
    // shape the raw-table preludes are held to - a value-returning,
    // non-raising layer that names the interface it could not reach, adds no
    // capability of its own, and never reports a number the client did not
    // publish.
    if (create_script && eval && destroy_script) {
        char message[512] = {};
        void* script = nullptr;

        // ---- wotb.context: is_battle and friends ---------------------------
        MockAbi::Reset();
        MockAbi::publish_core_context = true;
        MockAbi::core_context_mask = WOTBMOD_V3_CONTEXT_BATTLE;
        std::memset(message, 0, sizeof(message));
        script = create_script("context-aliases", "", message, sizeof(message));
        Check(script != nullptr, "an empty script for the context aliases");
        if (script) {
            Check(eval(script,
                       "if wotb.context.is_battle() ~= true then error('battle') "
                       "end\n"
                       "if wotb.context.is_hangar() ~= false then error('hangar') "
                       "end\n"
                       "if wotb.context.is_replay() ~= false then error('replay') "
                       "end\n"
                       "if wotb.context.is_text_input() ~= false then "
                       "error('text') end\n"
                       "if wotb.context.is_training() ~= false then "
                       "error('training') end\n"
                       "wotb.core = nil\n"
                       "local v, e = wotb.context.is_battle()\n"
                       "if v ~= nil or type(e) ~= 'string' or not string.find(e, "
                       "'context.is_battle', 1, true) then error('absent: ' .. "
                       "tostring(e)) end",
                       message, sizeof(message)) == 0u,
                  "context.is_* answer the current mask as booleans, and nil "
                  "plus a prefixed message once wotb.core is gone");
            destroy_script(script);
        }

        // ---- wotb.players: provenance of the pose, aliases, details --------
        MockAbi::Reset();
        MockAbi::SetHostPermissions(
            {"core", "events.public", "storage", "ui.modify.game", "ui.create",
             "ui.modify.own", "battle.ui", "entity.public.visible",
             "game.entity.public"});
        Check(entry(&bootstrap, 1u, &info) == WOTBMOD_V3_OK,
              "the players facade test re-measures a ceiling with public "
              "entity access");
        // The ally carries a sourced pose: a unit direction, as the loader's
        // ingress requires. The other two keep the zero vector an unsourced
        // record has.
        MockAbi::PublicEntities()[1].position.x = 10.0f;
        MockAbi::PublicEntities()[1].position.y = 2.0f;
        MockAbi::PublicEntities()[1].position.z = 30.0f;
        MockAbi::PublicEntities()[1].direction.x = 0.0f;
        MockAbi::PublicEntities()[1].direction.y = 0.0f;
        MockAbi::PublicEntities()[1].direction.z = 1.0f;
        std::memset(message, 0, sizeof(message));
        script = create_script("players-facade", "", message, sizeof(message));
        Check(script != nullptr, "an empty script for the players facade");
        if (script) {
            const uint32_t alias_status = eval(script,
                       "local me = wotb.players.me()\n"
                       "if type(me) ~= 'table' or me.public_id ~= 101 then "
                       "error('me') end\n"
                       "local allies = wotb.players.allies()\n"
                       "if #allies ~= 1 or allies[1].public_id ~= 102 then "
                       "error('allies ' .. #allies) end\n"
                       "local enemies = wotb.players.enemies()\n"
                       "if #enemies ~= 1 or enemies[1].public_id ~= 201 then "
                       "error('enemies') end\n"
                       "local found = wotb.players.by_id(201)\n"
                       "if type(found) ~= 'table' or found.display_name ~= "
                       "'Enemy' then error('by_id') end\n"
                       "local seen = 0\n"
                       "local visited = wotb.players.each_visible(function() "
                       "seen = seen + 1 return seen < 2 end)\n"
                       "if visited ~= 2 or seen ~= 2 then error('each_visible "
                       "' .. tostring(visited)) end\n"
                       "if not allies[1].position_available or not "
                       "allies[1].direction_available then error('sourced pose "
                       "not reported') end\n"
                       "if allies[1].position.x ~= 10 or allies[1].position.z "
                       "~= 30 then error('pose values') end\n"
                       "if me.position_available or me.direction_available "
                       "then error('zero vector reported as a pose') end\n"
                       "if enemies[1].position_available then error('enemy "
                       "zero vector reported as a pose') end",
                       message, sizeof(message));
            if (alias_status != 0u) {
                std::fprintf(stderr, "    players facade probe: %s\n", message);
            }
            Check(alias_status == 0u,
                  "players.me/allies/enemies/by_id/each_visible answer from "
                  "the same snapshot, and position_available follows the "
                  "unit-length direction the loader's ingress requires rather "
                  "than a constant");
            const uint32_t details_status = eval(script,
                       "local d, e = wotb.players.details(102)\n"
                       "if d == nil then error(tostring(e)) end\n"
                       "if d.clan_tag ~= 'ABC' then error('clan_tag ' .. "
                       "tostring(d.clan_tag)) end\n"
                       "if d.account_id ~= 5000123 then error('account_id') "
                       "end\n"
                       "if d.kills ~= 3 or d.frags ~= 3 then error('kills') "
                       "end\n"
                       "if d.vehicle_name ~= 'usa:A100_T49' or "
                       "d.vehicle_display_name ~= 'T49' then error('vehicle "
                       "names') end\n"
                       "if next(d.unavailable) ~= nil then error('unexpected "
                       "unavailable') end\n"
                       "local enemy = wotb.players.details("
                       "wotb.players.by_id(201))\n"
                       "if enemy == nil then error('enemy details') end\n"
                       "if enemy.clan_tag ~= nil or type(enemy.unavailable."
                       "clan_tag) ~= 'string' then error('refused field must "
                       "be absent with a reason') end\n"
                       "if enemy.kills ~= nil then error('refused kills "
                       "reported as a number') end\n"
                       "local none, none_error = wotb.players.details(999)\n"
                       "if none ~= nil or not string.find(none_error, '999', "
                       "1, true) then error('unknown id') end\n"
                       "local bad, bad_error = wotb.players.details('x')\n"
                       "if bad ~= nil or type(bad_error) ~= 'string' then "
                       "error('bad argument') end",
                       message, sizeof(message));
            if (details_status != 0u) {
                std::fprintf(stderr, "    players details probe: %s\n",
                             message);
            }
            Check(details_status == 0u,
                  "players.details fetches the five roster fields by name, "
                  "keeps a refused field absent with the client's reason, and "
                  "answers nil plus a message for an unknown id");
            Check(eval(script,
                       "wotb.entity_public = nil\n"
                       "local d, e = wotb.players.details(102)\n"
                       "if d ~= nil or not string.find(e, 'players.details', "
                       "1, true) then error(tostring(e)) end",
                       message, sizeof(message)) == 0u,
                  "players.details names itself when entity_public is absent");
            destroy_script(script);
        }
        MockAbi::PublicEntities()[1].position = WotbModV3Vec3{};
        MockAbi::PublicEntities()[1].direction = WotbModV3Vec3{};
        MockAbi::Reset();
        Check(entry(&bootstrap, 1u, &info) == WOTBMOD_V3_OK,
              "the default ceiling is measured again after the players "
              "facade probe");

        // ---- wotb.battle: on_*, once, off, is_active, state ----------------
        MockAbi::Reset();
        MockAbi::publish_core_context = true;
        MockAbi::core_context_mask = WOTBMOD_V3_CONTEXT_BATTLE;
        std::memset(message, 0, sizeof(message));
        script = create_script(
            "battle-handlers",
            "kills = {}\n"
            "shots = 0\n"
            "kill_handle = wotb.battle.on_vehicle_destroyed(function(data, "
            "event)\n"
            "  kills[#kills + 1] = { victim = data and data.victim_id, killer "
            "= data and data.killer_id, topic = event.topic }\n"
            "end)\n"
            "once_count = 0\n"
            "once_handle = wotb.battle.once('shot', function() once_count = "
            "once_count + 1 end)\n"
            "boom_handle = wotb.battle.on_shot(function() error('boom') end)\n"
            "shot_handle = wotb.battle.on_shot(function(data) shots = shots + "
            "1 last_shell = data and data.shell_id end)\n",
            message, sizeof(message));
        Check(script != nullptr, "a script registering battle handlers compiles");
        if (script) {
            Check(MockAbi::LiveSubscriptions() == 4u,
                  "four handlers are four events subscriptions, made only "
                  "when asked for");
            WotbModV3ClientEventEnvelope kill = {};
            WOTBMOD_V3_INIT_STRUCT(kill, WOTBMOD_V3_CLIENT_EVENT_ENVELOPE_VERSION);
            kill.type = WOTBMOD_V3_CLIENT_EVENT_VEHICLE_KILLED;
            kill.primary_entity_id = 55u;
            kill.payload_size = sizeof(WotbModV3VehicleKillEventData);
            kill.payload.kill.victim_id = 55u;
            kill.payload.kill.killer_id = 77u;
            kill.payload.kill.reason = 1u;
            MockAbi::event_payload.assign(
                reinterpret_cast<const char*>(&kill), sizeof(kill));
            MockAbi::FireEvent(WOTBMOD_V3_EVENT_VEHICLE_KILLED);

            WotbModV3ClientEventEnvelope shot = {};
            WOTBMOD_V3_INIT_STRUCT(shot, WOTBMOD_V3_CLIENT_EVENT_ENVELOPE_VERSION);
            shot.type = WOTBMOD_V3_CLIENT_EVENT_SHOT_FIRED;
            shot.payload_size = sizeof(WotbModV3ShotEventData);
            shot.payload.shot.shot_code = 9u;
            shot.payload.shot.shell_id = 4242u;
            MockAbi::event_payload.assign(
                reinterpret_cast<const char*>(&shot), sizeof(shot));
            MockAbi::FireEvent(WOTBMOD_V3_EVENT_SHOT_FIRED);
            MockAbi::FireEvent(WOTBMOD_V3_EVENT_SHOT_FIRED);
            const uint32_t battle_status = eval(script,
                       "if #kills ~= 1 or kills[1].victim ~= 55 or "
                       "kills[1].killer ~= 77 then error('kill') end\n"
                       "if kills[1].topic ~= wotb.events.TOPIC_VEHICLE_KILLED "
                       "then error('topic') end\n"
                       "if shots ~= 2 or last_shell ~= 4242 then error('shots "
                       "' .. tostring(shots)) end\n"
                       "if once_count ~= 1 then error('once fired ' .. "
                       "once_count) end\n"
                       "local ok, e = wotb.battle.off(once_handle)\n"
                       "if ok ~= nil or type(e) ~= 'string' then error('a once "
                       "handler is already gone') end\n"
                       "if wotb.battle.off(shot_handle) ~= true then "
                       "error('off') end\n"
                       "local bad, bad_e = wotb.battle.on('teleport', "
                       "function() end)\n"
                       "if bad ~= nil or not string.find(bad_e, "
                       "'battle.events()', 1, true) then error('unknown name') "
                       "end\n"
                       "local names = wotb.battle.events()\n"
                       "if #names ~= 16 or names[1] ~= 'ammo' then "
                       "error('events list ' .. #names) end\n"
                       "if wotb.battle.is_active() ~= true then "
                       "error('active') end\n"
                       "local state = wotb.battle.state()\n"
                       "if state.active ~= true or state.tracking ~= false or "
                       "type(state.lifecycle_unavailable) ~= 'string' then "
                       "error('state') end",
                       message, sizeof(message));
            if (battle_status != 0u) {
                std::fprintf(stderr, "    battle facade probe: %s\n", message);
            }
            Check(battle_status == 0u,
                  "battle.on_* delivers the typed payload first, once() fires "
                  "once and unsubscribes itself, off() gives a subscription "
                  "back, a raising handler is contained, and state()/"
                  "is_active() need no subscription");
            Check(MockAbi::CalledContaining(MockAbi::core_calls,
                                            "battle.on_shot handler raised"),
                  "the raising handler was reported through wotb.log");
            Check(MockAbi::LiveSubscriptions() == 2u,
                  "off() and once() released their events subscriptions (two "
                  "of four remain)");
            Check(eval(script,
                       "if wotb.battle.off_all() ~= 2 then error('off_all') "
                       "end",
                       message, sizeof(message)) == 0u,
                  "off_all gives back what remains");
            Check(MockAbi::LiveSubscriptions() == 0u,
                  "and nothing remains subscribed");
            Check(eval(script,
                       "wotb.events = nil\n"
                       "local h, e = wotb.battle.on_shot(function() end)\n"
                       "if h ~= nil or not string.find(e, 'battle.on', 1, "
                       "true) then error(tostring(e)) end",
                       message, sizeof(message)) == 0u,
                  "battle.on_* answers nil plus a message when wotb.events is "
                  "absent");
            destroy_script(script);
        }

        // ---- wotb.mod: identity, permissions, capability, on_disable -------
        using DeactivateFn = uint32_t(WOTBMOD_V3_CALL*)(void*, char*, uint32_t);
        const auto deactivate = reinterpret_cast<DeactivateFn>(
            GetProcAddress(module, "WotbLuaHost_DeactivateScriptForTests"));
        using CreateLimitedFn = void*(WOTBMOD_V3_CALL*)(
            const char*, const char*, const char*, char*, uint32_t);
        const auto create_limited = reinterpret_cast<CreateLimitedFn>(
            GetProcAddress(module,
                           "WotbLuaHost_CreateScriptWithPermissionsForTests"));
        Check(deactivate != nullptr && create_limited != nullptr,
              "the deactivate and manifest-limited entry points are exported");
        MockAbi::Reset();
        std::memset(message, 0, sizeof(message));
        script = create_script(
            "mod-probe",
            "wotb.mod.on_disable(function() wotb.storage.set_json('bye1', "
            "'1') end)\n"
            "second = wotb.mod.on_disable(function() error('boom') end)\n"
            "wotb.mod.on_disable(function() wotb.storage.set_json('bye3', "
            "'3') end)\n"
            "dropped = wotb.mod.on_disable(function() "
            "wotb.storage.set_json('dropped', '0') end)\n"
            "wotb.mod.off_disable(dropped)\n"
            "function on_disable() wotb.storage.set_json('author', '1') end\n",
            message, sizeof(message));
        Check(script != nullptr, "a script using wotb.mod compiles");
        if (script) {
            const uint32_t mod_status = eval(script,
                       "if wotb.mod.id() ~= 'mod-probe' then error('id ' .. "
                       "tostring(wotb.mod.id())) end\n"
                       "if wotb.mod.has_permission('core') ~= true then "
                       "error('core') end\n"
                       "if wotb.mod.has_permission('unsafe_native') ~= false "
                       "then error('unsafe') end\n"
                       "if wotb.mod.has_permission('nonsense') ~= false then "
                       "error('nonsense') end\n"
                       "local names = wotb.mod.permissions()\n"
                       "local has_core, has_storage = false, false\n"
                       "for _, name in ipairs(names) do\n"
                       "  if name == 'core' then has_core = true end\n"
                       "  if name == 'storage' then has_storage = true end\n"
                       "  if name == 'unsafe_native' then error('unsafe "
                       "listed') end\n"
                       "end\n"
                       "if not has_core or not has_storage then "
                       "error('permissions list') end\n"
                       "local info = wotb.mod.info()\n"
                       "if info.id ~= 'mod-probe' or type(info.permissions) ~= "
                       "'table' then error('info') end\n"
                       "if type(info.host) ~= 'table' and "
                       "type(info.host_unavailable) ~= 'string' then "
                       "error('host') end\n"
                       "local status, detail = wotb.mod.capability("
                       "'gameplay.tweak.hud')\n"
                       "if status ~= 'available' or detail.name ~= "
                       "'gameplay.tweak.hud' then error('capability ' .. "
                       "tostring(status) .. ' ' .. tostring(detail)) end\n"
                       "local degraded, why = wotb.mod.capability("
                       "'gameplay.tweak.camera')\n"
                       "if degraded ~= 'degraded' or not string.find(why."
                       "reason, 'FOV', 1, true) then error('degraded ' .. "
                       "tostring(degraded)) end\n"
                       "local missing, missing_e = wotb.mod.capability("
                       "'wotbmod.gameplay.hud')\n"
                       "if missing ~= nil or type(missing_e) ~= 'string' then "
                       "error('an interface id is not a capability') end\n"
                       "local all = wotb.mod.capabilities()\n"
                       "if #all ~= 2 or all[1].name ~= 'gameplay.tweak.camera' "
                       "or all[2].status ~= 'available' then error('list ' .. "
                       "#all) end\n"
                       "local bad, bad_e = wotb.mod.capability(7)\n"
                       "if bad ~= nil or type(bad_e) ~= 'string' then "
                       "error('bad name') end",
                       message, sizeof(message));
            if (mod_status != 0u) {
                std::fprintf(stderr, "    mod facade probe: %s\n", message);
            }
            Check(mod_status == 0u,
                  "wotb.mod answers the script's id, its effective permission "
                  "names, has_permission by name, info(), capability() in the "
                  "client's words for a registered capability, nil for an "
                  "interface id, and capabilities() as the sorted list");
            std::memset(message, 0, sizeof(message));
            const uint32_t deactivated =
                deactivate ? deactivate(script, message, sizeof(message)) : 2u;
            Check(deactivated == 1u,
                  "deactivation succeeds: a raising wotb.mod handler is "
                  "contained, not a failure of the teardown");
            Check(MockAbi::CalledContaining(MockAbi::core_calls,
                                            "on_disable handler 2 raised"),
                  "the raising handler was reported through wotb.log with its "
                  "handle");
            const auto position = [](const std::vector<std::string>& calls,
                                     const char* needle) -> long {
                for (size_t index = 0u; index < calls.size(); ++index) {
                    if (calls[index].find(needle) != std::string::npos) {
                        return static_cast<long>(index);
                    }
                }
                return -1;
            };
            const long bye3 = position(MockAbi::storage_calls, "set_json(bye3");
            const long bye1 = position(MockAbi::storage_calls, "set_json(bye1");
            const long author =
                position(MockAbi::storage_calls, "set_json(author");
            Check(bye3 >= 0 && bye1 > bye3 && author > bye1,
                  "handlers ran newest first and before the author's global "
                  "on_disable");
            Check(position(MockAbi::storage_calls, "set_json(dropped") < 0,
                  "a handler given back with off_disable did not run");
            destroy_script(script);
        }

        std::memset(message, 0, sizeof(message));
        script = create_limited
                     ? create_limited("{\"id\":\"limited\",\"permissions\":"
                                      "[\"core\"]}",
                                      "limited-mod", "", message,
                                      sizeof(message))
                     : nullptr;
        Check(script != nullptr, "a manifest-limited script compiles");
        if (script) {
            Check(eval(script,
                       "if wotb.mod.has_permission('core') ~= true then "
                       "error('core') end\n"
                       "if wotb.mod.has_permission('storage') ~= false then "
                       "error('storage leaked') end\n"
                       "local names = wotb.mod.permissions()\n"
                       "if #names ~= 1 or names[1] ~= 'core' then error('list "
                       "' .. #names) end\n"
                       "local ok, e = wotb.storage.set_json('k', '1')\n"
                       "if ok ~= nil then error('the fence must agree with "
                       "has_permission') end",
                       message, sizeof(message)) == 0u,
                  "a manifest that asks for core alone gets "
                  "has_permission('storage') == false, and the fence refuses "
                  "the call the same way");
            destroy_script(script);
        }

        // ---- wotb.ges: schema, on/off, decode, observe ---------------------
        MockAbi::Reset();
        MockAbi::SetHostPermissions(
            {"core", "events.public", "storage", "ui.modify.game", "ui.create",
             "ui.modify.own", "battle.ui", "input", "ges.observe",
             "ges.publish"});
        Check(entry(&bootstrap, 1u, &info) == WOTBMOD_V3_OK,
              "the ges facade test re-measures a ceiling that grants ges.*");
        std::memset(message, 0, sizeof(message));
        script = create_script(
            "ges-facade",
            "decoded = nil\n"
            "raw_count = 0\n"
            "once_count = 0\n"
            "h = wotb.ges.observe('Avatar.*', { decode = true }, "
            "function(fields, ev, unreadable)\n"
            "  decoded = fields\n"
            "  decoded_type = ev.type\n"
            "  had_unreadable = unreadable\n"
            "end)\n"
            "once = wotb.ges.observe('Avatar.*', { once = true }, "
            "function(ev) once_count = once_count + 1 end)\n"
            "plain = wotb.ges.on('Avatar.*', function(ev) raw_count = "
            "raw_count + 1 end)\n",
            message, sizeof(message));
        Check(script != nullptr, "a script using the ges facade compiles");
        if (script) {
            Check(MockAbi::LiveSubscriptions() == 3u,
                  "observe/on are ges subscriptions, one each");
            struct GesProbePayload {
                int32_t mode;
                uint8_t flag;
                uint8_t pad[3];
            } payload = {5, 1u, {0u, 0u, 0u}};
            MockAbi::FireGesEvent("Avatar::CameraModeChanged", &payload,
                                  sizeof(payload), 1u, 0x1234u);
            MockAbi::FireGesEvent("Avatar::CameraModeChanged", &payload,
                                  sizeof(payload), 1u, 0x1234u);
            const uint32_t ges_status = eval(script,
                       "if type(decoded) ~= 'table' or decoded.mode ~= 5 or "
                       "decoded.flag ~= true then error('decode ' .. "
                       "tostring(decoded and decoded.mode)) end\n"
                       "if decoded_type ~= 'Avatar::CameraModeChanged' then "
                       "error('type') end\n"
                       "if had_unreadable ~= nil then error('unreadable') "
                       "end\n"
                       "if once_count ~= 1 then error('once ' .. once_count) "
                       "end\n"
                       "if raw_count ~= 2 then error('raw ' .. raw_count) "
                       "end\n"
                       "local schema = wotb.ges.schema("
                       "'Avatar::CameraModeChanged')\n"
                       "if schema == nil or schema.id ~= 1 or schema.size ~= 8 "
                       "or schema.field_count ~= 2 then error('schema') end\n"
                       "if schema.fields[1].name ~= 'mode' or "
                       "schema.fields[2].name ~= 'flag' or "
                       "schema.fields[2].offset ~= 4 then error('fields') "
                       "end\n"
                       "local none, none_e = wotb.ges.schema('Avatar::Nope')\n"
                       "if none ~= nil or type(none_e) ~= 'string' then "
                       "error('unknown schema') end\n"
                       "if wotb.ges.is_available() ~= true then "
                       "error('available') end\n"
                       "if wotb.ges.off(plain) ~= true then error('off') end\n"
                       "local bad, bad_e = wotb.ges.decode({})\n"
                       "if bad ~= nil or type(bad_e) ~= 'string' then "
                       "error('decode of a non-event') end",
                       message, sizeof(message));
            if (ges_status != 0u) {
                std::fprintf(stderr, "    ges facade probe: %s\n", message);
            }
            Check(ges_status == 0u,
                  "ges.observe decodes by schema, once unsubscribes itself, "
                  "on/off are subscribe/unsubscribe, schema() reads the ABI's "
                  "field table, and decode refuses a non-event");
            Check(MockAbi::LiveSubscriptions() == 1u,
                  "once and off gave two of three subscriptions back");
            destroy_script(script);
        }
        MockAbi::Reset();
        Check(entry(&bootstrap, 1u, &info) == WOTBMOD_V3_OK,
              "the default ceiling is measured again after the ges facade "
              "probe");

        // ---- wotb.hud: over wotb.gameplay_hud, built on first touch --------
        MockAbi::Reset();
        MockAbi::SetHostPermissions(
            {"core", "events.public", "storage", "ui.modify.game", "ui.create",
             "ui.modify.own", "battle.ui", "input", "gameplay.tweak.hud"});
        Check(entry(&bootstrap, 1u, &info) == WOTBMOD_V3_OK,
              "the hud facade test re-measures a ceiling that grants "
              "gameplay.tweak.hud");
        std::memset(message, 0, sizeof(message));
        script = create_script("hud-facade", "", message, sizeof(message));
        Check(script != nullptr, "an empty script for the hud facade");
        if (script) {
            const uint32_t hud_status = eval(script,
                       "if next(wotb.hud) ~= nil then error('built before "
                       "first touch') end\n"
                       "if wotb.hud.mode() ~= 'native' then error('mode ' .. "
                       "tostring(wotb.hud.mode())) end\n"
                       "if wotb.hud.available() ~= true then "
                       "error('available') end\n"
                       "if wotb.hud.reticle.set_color({ r = 1, g = 0.25, b = "
                       "1, a = 1 }) ~= true then error('color') end\n"
                       "if wotb.hud.reticle.set_color(0x00FF00FF) ~= true then "
                       "error('int color') end\n"
                       "if wotb.hud.damage_log.hide() ~= true or "
                       "wotb.hud.damage_log.show() ~= true then "
                       "error('show/hide') end\n"
                       "if wotb.hud.damage_log.set_position('top-right') ~= "
                       "true then error('anchor name') end\n"
                       "if wotb.hud.session_stats.set_fields({ 'damage', "
                       "'shots' }) ~= true then error('stats names') end\n"
                       "if wotb.hud.hit_indicator.set_style('compact') ~= true "
                       "then error('style name') end\n"
                       "if wotb.hud.sixth_sense.set_position(12, 34) ~= true "
                       "then error('position') end\n"
                       "local id = wotb.hud.minimap.add_marker(10, 20, 'here', "
                       "{ r = 1, g = 0, b = 0 })\n"
                       "if id ~= 7 then error('marker id ' .. tostring(id)) "
                       "end\n"
                       "if wotb.hud.minimap.remove_marker(id) ~= true then "
                       "error('remove') end\n"
                       "local refused, why = "
                       "wotb.hud.minimap.set_show_last_known(true)\n"
                       "if refused ~= nil or not string.find(why, "
                       "'hud.minimap.set_show_last_known', 1, true) then "
                       "error('refusal not passed through: ' .. tostring(why)) "
                       "end\n"
                       "local bad, bad_e = wotb.hud.reticle.set_size('big')\n"
                       "if bad ~= nil or not string.find(bad_e, "
                       "'hud.reticle.set_size', 1, true) then "
                       "error('argument') end\n"
                       "local unknown, unknown_e = "
                       "wotb.hud.damage_log.set_position('sideways')\n"
                       "if unknown ~= nil or not string.find(unknown_e, "
                       "'sideways', 1, true) then error('unknown anchor') "
                       "end\n"
                       "if wotb.hud.reset() ~= true then error('reset') end\n"
                       "local status = wotb.hud.status()\n"
                       "if status.mode ~= 'native' or #status.published ~= 34 "
                       "or #status.missing ~= 0 then error('status ' .. "
                       "#status.published) end\n"
                       "if wotb.hud.rgba({ r = 0, g = 0, b = 0, a = 0 }) ~= 0 "
                       "then error('rgba') end",
                       message, sizeof(message));
            if (hud_status != 0u) {
                std::fprintf(stderr, "    hud facade probe: %s\n", message);
            }
            Check(hud_status == 0u,
                  "wotb.hud is empty until touched, packs colours, booleans "
                  "and names into the raw slots, passes the client's refusal "
                  "through under its own name, and refuses a bad argument "
                  "before crossing the ABI");
            Check(MockAbi::Called(MockAbi::hud_calls,
                                  "gameplay_hud.reticle_set_color(FF40FFFF)") &&
                      MockAbi::Called(MockAbi::hud_calls,
                                      "gameplay_hud.reticle_set_color(00FF00FF)"),
                  "colour tables and integers reach the slot as 0xRRGGBBAA");
            Check(MockAbi::Called(MockAbi::hud_calls,
                                  "gameplay_hud.damagelog_set_enabled(0)") &&
                      MockAbi::Called(MockAbi::hud_calls,
                                      "gameplay_hud.damagelog_set_enabled(1)"),
                  "show/hide are the enabled slot with 0 and 1");
            Check(MockAbi::Called(MockAbi::hud_calls,
                                  "gameplay_hud.damagelog_set_position(2)"),
                  "'top-right' resolved to ANCHOR_TOP_RIGHT");
            Check(MockAbi::Called(MockAbi::hud_calls,
                                  "gameplay_hud.session_stats_set_fields(17)"),
                  "{'damage', 'shots'} resolved to STAT_DAMAGE | STAT_SHOTS");
            Check(MockAbi::Called(MockAbi::hud_calls,
                                  "gameplay_hud.hit_indicator_set_style(1)"),
                  "'compact' resolved to HIT_STYLE_COMPACT");
            Check(MockAbi::Called(MockAbi::hud_calls,
                                  "gameplay_hud.sixth_sense_set_position(12,34)"),
                  "set_position packs the vec2");
            Check(MockAbi::Called(
                      MockAbi::hud_calls,
                      "gameplay_hud.minimap_add_marker(10,20,here,FF0000FF)") &&
                      MockAbi::Called(MockAbi::hud_calls,
                                      "gameplay_hud.minimap_remove_marker(7)"),
                  "markers cross with world x/z, the label and the packed "
                  "colour");
            Check(!MockAbi::CalledContaining(MockAbi::hud_calls,
                                             "reticle_set_size"),
                  "a refused argument never crossed the ABI");
            Check(eval(script,
                       "wotb.gameplay_hud = nil\n"
                       "local mode, why = wotb.hud.mode()\n"
                       "if mode ~= 'unavailable' or type(why) ~= 'string' then "
                       "error('mode') end\n"
                       "if wotb.hud.available() ~= false then "
                       "error('available') end\n"
                       "local v, e = wotb.hud.reticle.set_size(1.5)\n"
                       "if v ~= nil or not string.find(e, 'wotb.gameplay_hud "
                       "is unavailable', 1, true) then error(tostring(e)) "
                       "end\n"
                       "local r, re = wotb.hud.reset()\n"
                       "if r ~= nil or type(re) ~= 'string' then "
                       "error('reset') end",
                       message, sizeof(message)) == 0u,
                  "with wotb.gameplay_hud cleared every method answers a value "
                  "naming the interface, and none raises");
            destroy_script(script);
        }
        MockAbi::Reset();
        Check(entry(&bootstrap, 1u, &info) == WOTBMOD_V3_OK,
              "the default ceiling is measured again after the hud facade "
              "probe");

        // ---- the shipped facade tour runs against the mock -----------------
        //
        // The exact checked-in example, as the ally tracker and the others
        // are run: enable, a battle's worth of events, disable - and every
        // subscription and HUD change given back.
        MockAbi::Reset();
        MockAbi::publish_facade_interfaces = true;
        MockAbi::storage_map_enabled = true;
        MockAbi::SetHostPermissions(
            {"core", "events.public", "storage", "ui.modify.game", "ui.create",
             "ui.modify.own", "battle.ui", "input", "input.actions",
             "gameplay.tweak.hud", "ges.observe", "entity.public.visible",
             "game.entity.public", "camera.battle.read", "camera.hangar",
             "camera.replay", "gameplay.tweak.camera", "gameplay.tweak.freecam"});
        Check(entry(&bootstrap, 1u, &info) == WOTBMOD_V3_OK,
              "the facade tour test re-measures a ceiling with hud, ges, keys, "
              "camera and public entity access");
        MockAbi::publish_core_context = true;
        MockAbi::core_context_mask = WOTBMOD_V3_CONTEXT_BATTLE;
        const std::string tour =
            ReadModApiFile(L"examples\\lua_facade_tour\\main.lua");
        const std::string tour_manifest =
            ReadModApiFile(L"examples\\lua_facade_tour\\manifest.json");
        Check(!tour.empty() && !tour_manifest.empty(),
              "the checked-in Lua Facade Tour and its manifest are readable");
        std::memset(message, 0, sizeof(message));
        // Under the manifest's own permission list, not the dev ceiling: a
        // facade the manifest forgot to ask for fails here rather than in
        // the game (live 2026-09-06 16:09: "keys.bind: permission denied:
        // input.actions" from exactly that gap).
        script = (tour.empty() || tour_manifest.empty() || !create_limited)
                     ? nullptr
                     : create_limited(tour_manifest.c_str(), "example.lua_facade_tour",
                                      tour.c_str(), message, sizeof(message));
        if (!script && !tour.empty()) {
            std::fprintf(stderr, "    facade tour: %s\n", message);
        }
        Check(script != nullptr, "the exact shipped Lua Facade Tour compiles");
        if (script) {
            const uint32_t enable_status =
                eval(script, "on_enable()", message, sizeof(message));
            if (enable_status != 0u) {
                std::fprintf(stderr, "    facade tour enable: %s\n", message);
            }
            Check(enable_status == 0u &&
                      MockAbi::LiveSubscriptions() == 6u,
                  "on_enable subscribes five battle handlers and one GES "
                  "observation, and nothing else");
            Check(MockAbi::CalledContaining(MockAbi::facade_calls,
                                            "input.register_action(example.lua_facade_tour.notify,"),
                  "the tour binds F7 under its own id");
            MockAbi::FireInputAction(0u, 1.0f, 1u);
            Check(MockAbi::Called(MockAbi::facade_calls,
                                  "ui.toast_show(facade tour: F7,2)"),
                  "F7 shows the toast through wotb.screen.notify");
            Check(MockAbi::CalledContaining(MockAbi::core_calls,
                                            "facade_tour,hud mode=native"),
                  "the tour reports the HUD mode the mock publishes");
            Check(!MockAbi::CalledContaining(MockAbi::core_calls, "permission denied"),
                  "nothing the tour does is refused under its own manifest");
            WotbModV3ClientEventEnvelope started = {};
            WOTBMOD_V3_INIT_STRUCT(
                started, WOTBMOD_V3_CLIENT_EVENT_ENVELOPE_VERSION);
            started.type = WOTBMOD_V3_CLIENT_EVENT_BATTLE_STARTED;
            started.payload_size = sizeof(WotbModV3BattleEventData);
            started.payload.battle.battle_id = 9001u;
            MockAbi::event_payload.assign(
                reinterpret_cast<const char*>(&started), sizeof(started));
            MockAbi::FireEvent(WOTBMOD_V3_EVENT_BATTLE_STARTED);
            Check(!MockAbi::CalledContaining(MockAbi::hud_calls,
                                             "reticle_set_color"),
                  "nothing touches the HUD on the loading screen where "
                  "battle.started arrives");
            // Two frames 46 s apart: the first bases the 45 s timer, the
            // second fires it - the roster listing and the HUD tweaks.
            MockAbi::event_payload.clear();
            MockAbi::event_timestamp_ns = 1000000000ull;
            MockAbi::FireEvent(WOTBMOD_V3_EVENT_FRAME_UPDATE);
            MockAbi::event_timestamp_ns = 47000000000ull;
            MockAbi::FireEvent(WOTBMOD_V3_EVENT_FRAME_UPDATE);
            Check(MockAbi::CalledContaining(MockAbi::core_calls,
                                            "Ally [ABC] T49 kills=3") &&
                      MockAbi::CalledContaining(MockAbi::core_calls,
                                                "camera mode=arcade fov=75"),
                  "45 s into the battle the tour lists the roster through "
                  "players.details and the camera through wotb.view");
            WotbModV3ClientEventEnvelope kill = {};
            WOTBMOD_V3_INIT_STRUCT(kill, WOTBMOD_V3_CLIENT_EVENT_ENVELOPE_VERSION);
            kill.type = WOTBMOD_V3_CLIENT_EVENT_VEHICLE_KILLED;
            kill.payload_size = sizeof(WotbModV3VehicleKillEventData);
            kill.payload.kill.victim_id = 201u;
            kill.payload.kill.killer_id = 102u;
            MockAbi::event_payload.assign(
                reinterpret_cast<const char*>(&kill), sizeof(kill));
            MockAbi::FireEvent(WOTBMOD_V3_EVENT_VEHICLE_KILLED);
            WotbModV3ClientEventEnvelope ended = {};
            WOTBMOD_V3_INIT_STRUCT(ended, WOTBMOD_V3_CLIENT_EVENT_ENVELOPE_VERSION);
            ended.type = WOTBMOD_V3_CLIENT_EVENT_BATTLE_ENDED;
            ended.payload_size = sizeof(WotbModV3BattleEventData);
            ended.payload.battle.battle_id = 9001u;
            ended.payload.battle.winner_team = 1u;
            MockAbi::event_payload.assign(
                reinterpret_cast<const char*>(&ended), sizeof(ended));
            MockAbi::FireEvent(WOTBMOD_V3_EVENT_BATTLE_ENDED);
            Check(MockAbi::CalledContaining(MockAbi::core_calls,
                                            "Ally [ABC] T49 kills=3") &&
                      MockAbi::CalledContaining(MockAbi::core_calls,
                                                "Ally destroyed Enemy") &&
                      MockAbi::CalledContaining(
                          MockAbi::core_calls,
                          "battle ended: winner team 1, 0 shots seen") &&
                      MockAbi::CalledContaining(MockAbi::storage_calls,
                                                "storage.set_json(last_battle,"),
                  "the tour names vehicles through players.details at the end "
                  "of the battle, kills through players.by_id, and keeps the "
                  "result through wotb.store");
            Check(MockAbi::Called(MockAbi::hud_calls,
                                  "gameplay_hud.reticle_set_color(FF40FFFF)") &&
                      MockAbi::CalledContaining(
                          MockAbi::core_calls,
                          "minimap last-known: hud.minimap.set_show_last_known"),
                  "the tour recolours the reticle and prints the client's "
                  "refusal for the last-known slot rather than hiding it");
            const uint32_t disable_status =
                eval(script, "on_disable()", message, sizeof(message));
            if (disable_status != 0u) {
                std::fprintf(stderr, "    facade tour disable: %s\n", message);
            }
            Check(disable_status == 0u && MockAbi::LiveSubscriptions() == 0u &&
                      MockAbi::Called(MockAbi::hud_calls, "gameplay_hud.reset()"),
                  "on_disable gives every subscription back and resets the HUD");
            destroy_script(script);
        }
        MockAbi::Reset();
        Check(entry(&bootstrap, 1u, &info) == WOTBMOD_V3_OK,
              "the default ceiling is measured again after the facade tour");

        // ---- stage 1b: screen, vehicle, shells, view, sound, keys, store, ----
        // ---- files, and the panel's dynamic controls ----------------------
        //
        // The mock publishes the ten interfaces these stand on only for this
        // block (publish_facade_interfaces), with every slot recording its
        // call into facade_calls, so each facade is checked for what it sent
        // and for what it refused to send.
        const auto stage1b_permissions = [&]() {
            MockAbi::Reset();
            MockAbi::publish_facade_interfaces = true;
            MockAbi::SetHostPermissions(
                {"core", "events.public", "storage", "ui.modify.game", "ui.create",
                 "ui.modify.own", "battle.ui", "input.actions",
                 "camera.battle.read", "camera.hangar", "camera.replay",
                 "gameplay.tweak.camera", "gameplay.tweak.freecam", "audio.custom",
                 "audio.events", "gameplay.tweak.vehicle", "vehicle.local.cosmetic",
                 "game.entity.public", "entity.public.visible", "resources.mod",
                 "resources.overlay.game", "gameplay.tweak.projectile_visual",
                 "visible.projectile.events", "gameplay.tweak.hud", "ges.observe",
                 "session.cluster.read", "session.cluster.change"});
            Check(entry(&bootstrap, 1u, &info) == WOTBMOD_V3_OK,
                  "a stage-1b facade test re-measures a ceiling with every "
                  "family it stands on");
            MockAbi::publish_core_context = true;
            MockAbi::core_context_mask = WOTBMOD_V3_CONTEXT_BATTLE;
        };
        const auto probe = [&](void* target, const char* source, const char* what) {
            std::memset(message, 0, sizeof(message));
            const uint32_t status = eval(target, source, message, sizeof(message));
            if (status != 0u) std::fprintf(stderr, "    %s: %s\n", what, message);
            Check(status == 0u, what);
        };

        // ---- wotb.screen --------------------------------------------------
        stage1b_permissions();
        std::memset(message, 0, sizeof(message));
        script = create_script(
            "screen-facade",
            "panel = wotb.panel.new({ id = 'sf', width = 200, height = 100, "
            "rows = { { text = 'hello' } }, contexts = false })\n"
            "ok, err = panel:mount()\n",
            message, sizeof(message));
        Check(script != nullptr, "a script mounting a panel for wotb.screen compiles");
        if (script) {
            probe(script,
                  "if not ok then error(err) end\n"
                  "local row = panel:control('row1')\n"
                  "if wotb.screen.text(row) ~= 'hello' then error('text ' .. "
                  "tostring(wotb.screen.text(row))) end\n"
                  "if wotb.screen.live_text(row) ~= 'hello (live)' then error('live') end\n"
                  "local rect = wotb.screen.rect(row)\n"
                  "if type(rect) ~= 'table' or rect.width ~= 160 then error('rect ' .. "
                  "tostring(rect and rect.width)) end\n"
                  "if wotb.screen.visible(row) ~= true then error('visible') end\n"
                  "if wotb.screen.game_owned(row) ~= false then error('owned') end\n"
                  "local root = wotb.screen.root()\n"
                  "if root == nil then error('root') end\n"
                  "if wotb.screen.game_owned(root) ~= true then error('root owned') end\n"
                  "local kids = wotb.screen.children(root)\n"
                  "if type(kids) ~= 'table' or #kids < 1 then error('children') end\n"
                  "wotb.handles.release(root)\n"
                  "local found = wotb.screen.find('sf.row1')\n"
                  "if found == nil then error('find') end\n"
                  "if wotb.screen.set_text(row, 'bye') ~= true or wotb.screen.text(row) "
                  "~= 'bye' then error('set_text') end\n"
                  "if wotb.screen.set_visible(row, false) ~= true or "
                  "wotb.screen.visible(row) ~= false then error('set_visible') end\n"
                  "if wotb.screen.notify('hi', 2) ~= true then error('notify') end\n"
                  "local dialog = wotb.screen.popup({ title = 'T', message = 'M', "
                  "accept = 'Yes', cancel = 'No' })\n"
                  "if dialog == nil then error('popup') end\n"
                  "local bad, bad_e = wotb.screen.set_text(row, 7)\n"
                  "if bad ~= nil or not string.find(bad_e, 'screen.set_text', 1, true) "
                  "then error('bad text') end\n"
                  "local info = wotb.screen.info(row)\n"
                  "if info.id ~= 'sf.row1' or info.game_owned ~= false then "
                  "error('info ' .. tostring(info.id)) end\n"
                  "panel:unmount()",
                  "wotb.screen reads text, live text, rect, flags and children, "
                  "finds by id under the active screen, writes text and "
                  "visibility, and passes toast/dialog through");
            Check(MockAbi::Called(MockAbi::facade_calls, "ui.toast_show(hi,2)") &&
                      MockAbi::Called(MockAbi::facade_calls,
                                      "ui.confirm_show(T,M,Yes,No,1)"),
                  "notify and popup reached the client's toast and confirm slots");
            // The live 11.20 client has no toast: ui.toast_show answers
            // NOT_SUPPORTED. notify then draws its own one-line panel and
            // arms a timer to take it down, and says so in its second value.
            MockAbi::toast_not_supported = true;
            probe(script,
                  "local drawn, how = wotb.screen.notify('no toast here', 1)\n"
                  "if drawn ~= true or how ~= 'overlay' then "
                  "error('overlay ' .. tostring(how)) end\n"
                  "if wotb.timer.count() < 1 then error('no takedown timer') end\n"
                  "local again, again_how = wotb.screen.notify('twice', 1)\n"
                  "if again ~= true or again_how ~= 'overlay' then "
                  "error('second overlay ' .. tostring(again_how)) end",
                  "with the client's toast slot NOT_SUPPORTED, notify draws the "
                  "panel overlay, arms its takedown timer, and can do it again");
            MockAbi::toast_not_supported = false;
            Check(MockAbi::Called(MockAbi::facade_calls,
                                  "ui.toast_show(no toast here,1)"),
                  "the client's toast slot is still asked first");
            probe(script,
                  "wotb.ui = nil\n"
                  "local v, e = wotb.screen.find('x')\n"
                  "if v ~= nil or not string.find(e, 'wotb.ui is unavailable', 1, true) "
                  "then error(tostring(e)) end\n"
                  "local n, ne = wotb.screen.notify('x')\n"
                  "if n ~= nil or type(ne) ~= 'string' then error('notify') end",
                  "with wotb.ui cleared, wotb.screen names the interface");
            destroy_script(script);
        }
        // The game-owned guard. Every wotb.ui slot already needs ui.modify.game
        // (the family's guard wants all four names), so a script that can read
        // the screen holds it; the facade's own check is exercised by having
        // wotb.mod answer no - the state a differently fenced host would leave.
        stage1b_permissions();
        std::memset(message, 0, sizeof(message));
        script = create_script("screen-guard", "", message, sizeof(message));
        Check(script != nullptr, "an empty script for the game-owned guard");
        if (script) {
            probe(script,
                  "wotb.mod.has_permission = function() return false end\n"
                  "local root = wotb.screen.root()\n"
                  "if root == nil then error('root') end\n"
                  "local v, e = wotb.screen.set_text(root, 'x')\n"
                  "if v ~= nil or not string.find(e, 'game-owned', 1, true) then "
                  "error('guard: ' .. tostring(e)) end\n"
                  "wotb.handles.release(root)",
                  "a write to a game-owned control is refused in the facade's words "
                  "when the script does not hold ui.modify.game");
            Check(!MockAbi::CalledContaining(MockAbi::ui_calls, "ui.control_set_text("),
                  "and the refusal happened before any set_text crossed the ABI");
            destroy_script(script);
        }

        // ---- wotb.vehicle -------------------------------------------------
        stage1b_permissions();
        std::memset(message, 0, sizeof(message));
        script = create_script("vehicle-facade", "", message, sizeof(message));
        Check(script != nullptr, "an empty script for wotb.vehicle");
        if (script) {
            probe(script,
                  "local v = wotb.vehicle.local_vehicle()\n"
                  "if v == nil then error('local') end\n"
                  "if wotb.vehicle.is_local(v) ~= true then error('is_local') end\n"
                  "local p = wotb.vehicle.position(v)\n"
                  "if type(p) ~= 'table' then error('position') end\n"
                  "local state, code = wotb.vehicle.appearance_state(v)\n"
                  "if state ~= 'ready' or code ~= 2 then error('state ' .. tostring(state)) end\n"
                  "local pack = wotb.vehicle.skin.register({ id = 'pack', vehicle_name = "
                  "'usa:A100_T49', assets = { {}, {} } })\n"
                  "if pack == nil then error('register') end\n"
                  "if wotb.vehicle.skin.apply(pack, v) ~= true then error('apply') end\n"
                  "local st = wotb.vehicle.skin.state(pack)\n"
                  "if st.requires_model_reload ~= true or st.asset_count ~= 2 or "
                  "st.applied ~= true then error('state') end\n"
                  "if wotb.vehicle.skin.rollback(pack) ~= true or "
                  "wotb.vehicle.skin.release(pack) ~= true then error('rollback') end\n"
                  "if wotb.vehicle.appearance.reset(v) ~= true then error('reset') end\n"
                  "if wotb.vehicle.set_camouflage(v, 'mod://self/camo.dds') ~= true then "
                  "error('camo') end\n"
                  "local bad, bad_e = wotb.vehicle.skin.register({})\n"
                  "if bad ~= nil or not string.find(bad_e, 'vehicle.skin.register', 1, "
                  "true) then error('bad register') end\n"
                  "local vis = wotb.vehicle.visible()\n"
                  "if type(vis) ~= 'table' or #vis ~= 3 then error('visible') end\n"
                  "wotb.handles.release(v)",
                  "wotb.vehicle finds the local vehicle, reads its state, registers, "
                  "applies, reads back (requires_model_reload as a boolean), rolls "
                  "back and releases a skin pack");
            Check(MockAbi::Called(MockAbi::facade_calls,
                                  "vehicle_visual.skin_pack_register(pack,usa:A100_T49,2,0,0)"),
                  "asset_count is filled from the assets array");
            probe(script,
                  "wotb.vehicle_visual = nil\n"
                  "local v, e = wotb.vehicle.local_vehicle()\n"
                  "if v ~= nil or not string.find(e, 'vehicle.local_vehicle', 1, true) "
                  "then error(tostring(e)) end",
                  "with wotb.vehicle_visual cleared, wotb.vehicle names itself");
            destroy_script(script);
        }

        // ---- wotb.session -------------------------------------------------
        stage1b_permissions();
        std::memset(message, 0, sizeof(message));
        script = create_script(
            "session-facade",
            "seen = {}\n"
            "handle = wotb.session.on_cluster_changed(function(ev) seen[#seen + 1] = ev end)\n"
            "typed = nil\n"
            "wotb.events.subscribe(wotb.events.TOPIC_SESSION_CLUSTER_CHANGED, "
            "function(e) typed = e.data end, wotb.events.PRIORITY_NORMAL, true)\n",
            message, sizeof(message));
        Check(script != nullptr, "a script subscribing through wotb.session compiles");
        if (script) {
            probe(script,
                  "local list, err = wotb.session.clusters()\n"
                  "if type(list) ~= 'table' then error(tostring(err)) end\n"
                  "if #list ~= 4 or list[1].id ~= 0 or list[4].id ~= 4 then error('order') end\n"
                  "if list[3].name ~= 'EU_C3' or list[3].current ~= true then error('current') end\n"
                  "if list[1].alive ~= false or list[1].allowed ~= true then error('flags') end\n"
                  "if list[2].ccu ~= -1 then error('ccu') end\n"
                  "local now = wotb.session.cluster()\n"
                  "if now.name ~= 'EU_C3' or now.id ~= 3 then error('cluster') end\n"
                  "if wotb.session.change_cluster('EU_C4') ~= true then error('by name') end\n"
                  "if wotb.session.change_cluster(2) ~= true then error('by id') end\n"
                  "if wotb.session.change_cluster('auto') ~= true then error('auto') end\n"
                  "local bad, bad_e = wotb.session.change_cluster('NA_C9')\n"
                  "if bad ~= nil or not string.find(bad_e, 'NA_C9', 1, true) then error('unknown name') end\n"
                  "local bad2, bad2_e = wotb.session.change_cluster({})\n"
                  "if bad2 ~= nil or not string.find(bad2_e, 'argument 1', 1, true) then error('bad arg') end",
                  "wotb.session lists the region sorted, reads the current cluster and "
                  "resolves a name, an id and 'auto' into change() calls");
            Check(MockAbi::Called(MockAbi::facade_calls, "session_cluster.change(4)"),
                  "a cluster name resolves to its id");
            Check(MockAbi::Called(MockAbi::facade_calls, "session_cluster.change(2)"),
                  "an id passes through");
            Check(MockAbi::Called(MockAbi::facade_calls, "session_cluster.change(-1)"),
                  "'auto' is the ABI's -1");
            MockAbi::FireClusterChanged(3, 4, WOTBMOD_V3_CLUSTER_CHANGE_CONNECTED);
            probe(script,
                  "if #seen ~= 1 then error('delivered ' .. #seen) end\n"
                  "local ev = seen[1]\n"
                  "if ev.from ~= 3 or ev.to ~= 4 or ev.status ~= 'connected' then "
                  "error(tostring(ev.status)) end\n"
                  "if type(typed) ~= 'table' or typed.kind ~= 'cluster_changed' or "
                  "typed.to_cluster_id ~= 4 or typed.status ~= 3 then error('typed') end\n"
                  "if wotb.session.off_cluster_changed(handle) ~= true then error('off') end",
                  "on_cluster_changed decodes the typed event into words and "
                  "wotb.events exposes the same event as data.kind == 'cluster_changed'");
            MockAbi::FireClusterChanged(4, 2, WOTBMOD_V3_CLUSTER_CHANGE_FAILED);
            probe(script,
                  "if #seen ~= 1 then error('delivered after off') end",
                  "a removed handler is not called again");
            MockAbi::ForceFailure(WOTBMOD_V3_E_BUSY);
            probe(script,
                  "local ok, why = wotb.session.change_cluster(4)\n"
                  "if ok ~= nil or not string.find(why, 'session.change_cluster', 1, true) "
                  "then error(tostring(why)) end",
                  "a refused change comes back as nil plus the facade's name");
            MockAbi::ClearFailure();
            probe(script,
                  "wotb.session_cluster = nil\n"
                  "local v, e = wotb.session.clusters()\n"
                  "if v ~= nil or not string.find(e, 'session.clusters', 1, true) then "
                  "error(tostring(e)) end\n"
                  "local c, ce = wotb.session.change_cluster(4)\n"
                  "if c ~= nil or not string.find(ce, 'session.change_cluster', 1, true) then "
                  "error(tostring(ce)) end",
                  "with wotb.session_cluster cleared, wotb.session names itself");
            destroy_script(script);
        }

        // A script holding only the read grant is refused by the facade
        // before any change() reaches the ABI.
        MockAbi::Reset();
        MockAbi::publish_facade_interfaces = true;
        MockAbi::SetHostPermissions(
            {"core", "events.public", "storage", "session.cluster.read"});
        Check(entry(&bootstrap, 1u, &info) == WOTBMOD_V3_OK,
              "the host measures a read-only session ceiling");
        std::memset(message, 0, sizeof(message));
        script = create_script("session-readonly", "", message, sizeof(message));
        Check(script != nullptr, "an empty script for the read-only session case");
        if (script) {
            probe(script,
                  "local list = wotb.session.clusters()\n"
                  "if type(list) ~= 'table' or #list ~= 4 then error('read') end\n"
                  "local ok, why = wotb.session.change_cluster(4)\n"
                  "if ok ~= nil or why ~= 'permission denied: session.cluster.change' then "
                  "error(tostring(why)) end",
                  "session.cluster.read alone lists clusters but cannot change them");
            Check(!MockAbi::Called(MockAbi::facade_calls, "session_cluster.change(4)"),
                  "the refusal happened before change() crossed the ABI");
            destroy_script(script);
        }

        // ---- wotb.shells --------------------------------------------------
        stage1b_permissions();
        std::memset(message, 0, sizeof(message));
        script = create_script(
            "shells-facade",
            "count = 0\n"
            "h = wotb.shells.on_created(function(snapshot, data, event)\n"
            "  count = count + 1\n"
            "  last = snapshot\n"
            "end)\n",
            message, sizeof(message));
        Check(script != nullptr, "a script observing projectiles compiles");
        if (script) {
            WotbModV3ProjectileLifecycleEvent created = {};
            WOTBMOD_V3_INIT_STRUCT(created, WOTBMOD_V3_PROJECTILE_VERSION_2);
            created.reason = 0u;
            WOTBMOD_V3_INIT_STRUCT(created.snapshot, WOTBMOD_V3_PROJECTILE_VERSION_2);
            created.snapshot.projectile = 7001u;
            created.snapshot.sequence_id = 5u;
            created.snapshot.valid_fields = WOTBMOD_V3_PROJECTILE_FIELD_ORIGIN |
                                            WOTBMOD_V3_PROJECTILE_FIELD_DIRECTION;
            created.snapshot.origin = {1.0f, 2.0f, 3.0f};
            created.snapshot.visible_direction = {0.0f, 0.0f, 1.0f};
            MockAbi::event_payload.assign(
                reinterpret_cast<const char*>(&created), sizeof(created));
            MockAbi::FireEvent(WOTBMOD_V3_EVENT_PROJECTILE_CREATED);
            probe(script,
                  "if count ~= 1 then error('count ' .. count) end\n"
                  "if last.fields.origin ~= true or last.fields.impact_position ~= false "
                  "then error('fields') end\n"
                  "if last.origin.x ~= 1 then error('origin') end\n"
                  "local snap = wotb.shells.snapshot(last.projectile)\n"
                  "if snap == nil or snap.fields.origin ~= true or "
                  "snap.fields.visible_position ~= false or snap.sequence_id ~= 5 then "
                  "error('snapshot') end\n"
                  "local vis, ve = wotb.shells.visual(last.projectile)\n"
                  "if vis ~= nil or type(ve) ~= 'string' then error('visual should be "
                  "unavailable') end\n"
                  "local imp = wotb.shells.impact.show({ id = 'boom', scene_uri = "
                  "'mod://self/boom.sc2' })\n"
                  "if imp == nil then error('impact') end\n"
                  "if wotb.shells.impact.update(imp, { id = 'boom', scene_uri = "
                  "'mod://self/boom.sc2', uniform_scale = 2 }) ~= true then "
                  "error('update') end\n"
                  "if wotb.shells.impact.hide(imp) ~= true then error('hide') end\n"
                  "local tr = wotb.shells.tracer.register({ id = 'ap', width = 0.5 })\n"
                  "if tr == nil then error('tracer') end\n"
                  "if wotb.shells.tracer.unregister(tr) ~= true then error('unregister') end\n"
                  "if wotb.shells.off(h) ~= true then error('off') end\n"
                  "local bad, be = wotb.shells.on('teleport', print)\n"
                  "if bad ~= nil then error('unknown') end",
                  "wotb.shells delivers snapshots with valid_fields as booleans, reads "
                  "a snapshot by handle, refuses the visual the client does not "
                  "publish, and registers impacts and tracers");
            Check(MockAbi::Called(MockAbi::facade_calls,
                                  "projectile.impact_visual_register(boom,mod://self/boom.sc2)") &&
                      MockAbi::Called(MockAbi::facade_calls,
                                      "projectile.tracer_style_register(ap,0.5)"),
                  "impact and tracer descriptors reach the raw slots");
            Check(MockAbi::LiveSubscriptions() == 0u, "off() gave the subscription back");
            destroy_script(script);
        }

        // ---- wotb.view ----------------------------------------------------
        stage1b_permissions();
        std::memset(message, 0, sizeof(message));
        script = create_script("view-facade",
                               "changes = 0\n"
                               "vh = wotb.view.on_changed(function() changes = changes + 1 end)\n",
                               message, sizeof(message));
        Check(script != nullptr, "a script using wotb.view compiles");
        if (script) {
            WotbModV3ClientEventEnvelope camera = {};
            WOTBMOD_V3_INIT_STRUCT(camera, WOTBMOD_V3_CLIENT_EVENT_ENVELOPE_VERSION);
            camera.type = WOTBMOD_V3_CLIENT_EVENT_CAMERA_MODE_CHANGED;
            camera.payload_size = sizeof(WotbModV3CameraEventData);
            camera.payload.camera.mode = 3u;
            MockAbi::event_payload.assign(
                reinterpret_cast<const char*>(&camera), sizeof(camera));
            MockAbi::FireEvent(WOTBMOD_V3_EVENT_CAMERA_MODE_CHANGED);
            probe(script,
                  "if vh == nil then error('on_changed') end\n"
                  "if changes ~= 1 then error('changes ' .. changes) end\n"
                  "local cam = wotb.view.get()\n"
                  "if cam == nil then error('get') end\n"
                  "if cam.mode ~= 'arcade' or cam.fov ~= 75 or cam.transform.position.x "
                  "~= 10 then error('camera ' .. tostring(cam.mode)) end\n"
                  "if cam.observed.animation_state ~= 5 or cam.observed.view_mode ~= nil "
                  "then error('observed') end\n"
                  "if cam.unavailable.near_plane == nil then error('near_plane should be "
                  "unavailable') end\n"
                  "if wotb.view.set_fov(90) ~= true or wotb.view.fov() ~= 90 then "
                  "error('fov') end\n"
                  "if wotb.view.set_fov(80, 'sniper') ~= true then error('sniper') end\n"
                  "if wotb.view.reset() ~= true or wotb.view.fov() ~= 75 then "
                  "error('reset') end\n"
                  "local s = wotb.view.project({ x = 1, y = 2, z = 3 })\n"
                  "if s.x ~= 2 or s.y ~= 4 then error('project') end\n"
                  "local w = wotb.view.unproject({ x = 2, y = 4, z = 0 })\n"
                  "if w.x ~= 1 then error('unproject') end\n"
                  "if wotb.view.transition({ target = { position = { x = 0, y = 0, z = 0 "
                  "} }, duration = 1.5 }) ~= true then error('transition') end\n"
                  "if wotb.view.shake({ amplitude = 2, duration = 0.25 }) ~= true then "
                  "error('shake') end\n"
                  "local bad = wotb.view.set_fov(400)\n"
                  "if bad ~= nil then error('bad fov') end\n"
                  "local bad2 = wotb.view.set_fov(60, 'sideways')\n"
                  "if bad2 ~= nil then error('bad context') end\n"
                  "if wotb.view.off(vh) ~= true then error('off') end",
                  "wotb.view reads the camera with its refusals listed, sets and "
                  "resets the FOV, projects both ways, transitions, shakes and "
                  "delivers mode changes");
            Check(MockAbi::Called(MockAbi::facade_calls, "camera.transition_to(1.5,1)") &&
                      MockAbi::Called(MockAbi::facade_calls, "camera.add_shake(2,0.25)") &&
                      MockAbi::Called(MockAbi::facade_calls,
                                      "gameplay_camera.set_fov_sniper(80)"),
                  "transition, shake and the sniper FOV reached the raw slots");
            Check(MockAbi::LiveSubscriptions() == 0u, "view.off gave every topic back");
            probe(script,
                  "wotb.camera = nil\n"
                  "local v, e = wotb.view.get()\n"
                  "if v ~= nil or not string.find(e, 'wotb.camera is unavailable', 1, "
                  "true) then error(tostring(e)) end",
                  "with wotb.camera cleared, wotb.view names the interface");
            destroy_script(script);
        }

        // ---- wotb.sound ---------------------------------------------------
        stage1b_permissions();
        std::memset(message, 0, sizeof(message));
        script = create_script("sound-facade", "", message, sizeof(message));
        Check(script != nullptr, "an empty script for wotb.sound");
        if (script) {
            probe(script,
                  "local h = wotb.sound.play('mod://self/tone.wav', { loop = true, "
                  "volume = 0.5, bus = 'ui', on_finished = function() end })\n"
                  "if h == nil then error('play') end\n"
                  "if wotb.sound.is_playing(h) ~= true then error('playing') end\n"
                  "if wotb.sound.set_volume(h, 0.25) ~= true then error('volume') end\n"
                  "if wotb.sound.stop(h, 0.5) ~= true then error('stop') end\n"
                  "if wotb.sound.release(h) ~= true then error('release') end\n"
                  "local tok = wotb.sound.replace('Tank_Engine', 'mod://self/engine.wav', 3)\n"
                  "if tok == nil then error('replace') end\n"
                  "if wotb.sound.reset('Tank_Engine') ~= true then error('reset') end\n"
                  "local nope = wotb.sound.reset('Nothing')\n"
                  "if nope ~= nil then error('reset unknown') end\n"
                  "local bad = wotb.sound.play('')\n"
                  "if bad ~= nil then error('bad uri') end\n"
                  "local bad2 = wotb.sound.set_volume(h, 5)\n"
                  "if bad2 ~= nil then error('bad volume') end",
                  "wotb.sound plays with options, controls the handle, and registers "
                  "and resets an override by event name");
            Check(MockAbi::CalledContaining(MockAbi::facade_calls,
                                            "audio.create(mod://self/tone.wav,") &&
                      MockAbi::CalledContaining(MockAbi::facade_calls, ",0.5,ui)") &&
                      MockAbi::Called(MockAbi::facade_calls,
                                      "audio.sound_override_register(Tank_Engine,"
                                      "mod://self/engine.wav,3)"),
                  "the descriptor and the override reached the raw slots");
            probe(script,
                  "wotb.audio = nil\n"
                  "local v, e = wotb.sound.play('mod://self/x.wav')\n"
                  "if v ~= nil or not string.find(e, 'wotb.audio is unavailable', 1, true) "
                  "then error(tostring(e)) end",
                  "with wotb.audio cleared, wotb.sound names the interface");
            destroy_script(script);
        }

        // ---- wotb.keys ----------------------------------------------------
        stage1b_permissions();
        std::memset(message, 0, sizeof(message));
        script = create_script("keys-facade", "", message, sizeof(message));
        Check(script != nullptr, "an empty script for wotb.keys");
        if (script) {
            probe(script,
                  "local a = wotb.keys.bind('toggle', { key = 'F7', modifiers = { ctrl = "
                  "true } })\n"
                  "if a == nil then error('bind') end\n"
                  "if wotb.keys.pressed('toggle') ~= true or wotb.keys.down('toggle') ~= "
                  "false then error('state') end\n"
                  "hits = {}\n"
                  "tok = wotb.keys.on_pressed('toggle', function(id, value) hits[#hits + "
                  "1] = id .. ':' .. tostring(value) end)\n"
                  "if tok == nil then error('on_pressed') end\n"
                  "rel = 0\n"
                  "tok2 = wotb.keys.on_released('toggle', function() rel = rel + 1 end)",
                  "wotb.keys binds a namespaced action and subscribes to its edges");
            MockAbi::FireInputAction(0u, 1.0f, 1u);
            MockAbi::FireInputAction(0u, 0.0f, 0u);
            probe(script,
                  "if #hits ~= 1 or hits[1] ~= 'toggle:1.0' then error('hits ' .. "
                  "tostring(hits[1])) end\n"
                  "if rel ~= 1 then error('released ' .. rel) end\n"
                  "local c = wotb.keys.conflicts('toggle')\n"
                  "if type(c) ~= 'table' or #c ~= 1 or c[1].action_id ~= 'other.mod.toggle' "
                  "then error('conflicts') end\n"
                  "local b = wotb.keys.bindings('toggle')\n"
                  "if #b ~= 1 or b[1].code ~= 0x76 then error('bindings') end\n"
                  "if wotb.keys.capture_begin() ~= true then error('capture begin') end\n"
                  "local cap = wotb.keys.capture_end()\n"
                  "if cap.code ~= 0x76 then error('capture end') end\n"
                  "if wotb.keys.off(tok) ~= true then error('off') end\n"
                  "local dup = wotb.keys.bind('toggle', { key = 'F8' })\n"
                  "if dup ~= nil then error('duplicate bind') end\n"
                  "local bad, be = wotb.keys.bind('x', { key = 'NOPE' })\n"
                  "if bad ~= nil or not string.find(be, 'NOPE', 1, true) then error('bad "
                  "key') end\n"
                  "if wotb.keys.unbind('toggle') ~= true then error('unbind') end\n"
                  "if wotb.keys.unbind_all() ~= 0 then error('unbind_all') end",
                  "pressed/released edges reach the right handler, conflicts and "
                  "bindings are listed, capture round-trips, and unbind gives the "
                  "action back");
            Check(MockAbi::CalledContaining(MockAbi::facade_calls,
                                            "input.register_action(keys-facade.toggle,1,118,"),
                  "the action id carries the script id and the key resolved to VK 0x76");
            probe(script,
                  "wotb.input = nil\n"
                  "local v, e = wotb.keys.bind('t', { key = 'F1' })\n"
                  "if v ~= nil or not string.find(e, 'wotb.input is unavailable', 1, true) "
                  "then error(tostring(e)) end",
                  "with wotb.input cleared, wotb.keys names the interface");
            destroy_script(script);
        }

        // ---- wotb.store ---------------------------------------------------
        stage1b_permissions();
        MockAbi::storage_map_enabled = true;
        std::memset(message, 0, sizeof(message));
        script = create_script("store-facade", "", message, sizeof(message));
        Check(script != nullptr, "an empty script for wotb.store");
        if (script) {
            probe(script,
                  "if wotb.store.set('profile', { name = 'x', hp = { 1, 2 } }) ~= true then "
                  "error('set') end\n"
                  "local v, origin = wotb.store.get('profile')\n"
                  "if origin ~= 'stored' or v.name ~= 'x' or v.hp[2] ~= 2 then error('get "
                  "' .. tostring(origin)) end\n"
                  "local d, o2 = wotb.store.get('missing', 7)\n"
                  "if d ~= 7 or o2 ~= 'default' then error('default') end\n"
                  "if wotb.store.has('profile') ~= true or wotb.store.has('missing') ~= "
                  "false then error('has') end\n"
                  "wotb.store.set('other', 1)\n"
                  "local keys = wotb.store.keys()\n"
                  "if #keys ~= 2 or keys[1] ~= 'profile' then error('keys ' .. #keys) end\n"
                  "if wotb.store.delete('profile') ~= true or wotb.store.has('profile') ~= "
                  "false then error('delete') end\n"
                  "if wotb.store.clear() ~= 1 then error('clear') end\n"
                  "if #wotb.store.keys() ~= 0 then error('keys after clear') end\n"
                  "local bad = wotb.store.set('k')\n"
                  "if bad ~= nil then error('set nil') end\n"
                  "local bad2 = wotb.store.get('')\n"
                  "if bad2 ~= nil then error('empty key') end",
                  "wotb.store round-trips tables through JSON, answers defaults as "
                  "defaults, and keeps an index for keys() and clear()");
            probe(script,
                  "wotb.storage = nil\n"
                  "local v, e = wotb.store.get('x')\n"
                  "if v ~= nil or not string.find(e, 'wotb.storage is unavailable', 1, "
                  "true) then error(tostring(e)) end",
                  "with wotb.storage cleared, wotb.store names the interface");
            destroy_script(script);
        }

        // ---- wotb.files ---------------------------------------------------
        stage1b_permissions();
        std::memset(message, 0, sizeof(message));
        script = create_script("files-facade", "", message, sizeof(message));
        Check(script != nullptr, "an empty script for wotb.files");
        if (script) {
            probe(script,
                  "local t = wotb.files.read_text('mod://self/hello.txt')\n"
                  "if t ~= 'hello, tour' then error('text ' .. tostring(t)) end\n"
                  "local j = wotb.files.read_json('mod://self/hello.json')\n"
                  "if type(j) ~= 'table' or j.answer ~= 42 or j.list[2] ~= 2 then "
                  "error('json') end\n"
                  "local missing, me = wotb.files.read_text('mod://self/none.txt')\n"
                  "if missing ~= nil or not string.find(me, 'files.read_text', 1, true) "
                  "then error('missing') end\n"
                  "local bad = wotb.files.read_text('')\n"
                  "if bad ~= nil then error('bad uri') end\n"
                  "local tex = wotb.files.load_texture('mod://self/hud-overlay.png')\n"
                  "if tex == nil then error('texture') end\n"
                  "local info = wotb.files.info(tex)\n"
                  "if info.uri ~= 'mod://self/hud-overlay.png' or info.type ~= "
                  "wotb.resources.IMAGE then error('info') end\n"
                  "if wotb.files.release(tex) ~= true then error('release') end\n"
                  "local y, ye = wotb.files.read_yaml('mod://self/x.yaml')\n"
                  "if y ~= nil or not string.find(ye, 'load_yaml', 1, true) then "
                  "error('yaml: ' .. tostring(ye)) end\n"
                  "local ex, why = wotb.files.exists('mod://self/hello.txt')\n"
                  "if ex ~= false or type(why) ~= 'string' then error('exists') end",
                  "wotb.files reads text and JSON through the bounded loader, loads a "
                  "texture with the expected type filled in, and reports the client's "
                  "own answer for yaml and stat slots this mock does not publish");
            Check(MockAbi::Called(MockAbi::facade_calls,
                                  "resources.load(3,mod://self/hud-overlay.png)") &&
                      MockAbi::Called(MockAbi::facade_calls,
                                      "loaders.load_text_utf8(mod://self/hello.txt,4194304)"),
                  "the resource type and the byte limit reached the raw slots");
            destroy_script(script);
        }

        // ---- wotb.panel: dynamic controls, layout, listeners, destroy -----
        stage1b_permissions();
        std::memset(message, 0, sizeof(message));
        script = create_script(
            "panel-dynamic",
            "panel = wotb.panel.new({ id = 'dyn', width = 300, height = 200, contexts = "
            "false })\n"
            "events = {}\n"
            "panel:on('mounted', function() events[#events + 1] = 'mounted' end)\n"
            "panel:on('unmounted', function() events[#events + 1] = 'unmounted' end)\n"
            "l1 = panel:label({ text = 'one' })\n"
            "b1 = panel:button({ id = 'go', text = 'Go', x = 10, y = 100, width = 80, "
            "height = 30, on_click = function() clicked = true end })\n"
            "sc = panel:scroll({ x = 10, y = 140, width = 280, height = 50 })\n"
            "i1 = panel:image({ texture = 'mod://self/icon.png', x = 0, y = 0, width = 16, "
            "height = 16, parent = sc })\n"
            "names = panel:row({ x = 10, y = 60, items = { { text = 'a', width = 50 }, { "
            "kind = 'button', id = 'b', text = 'B', width = 40, height = 24 } } })\n"
            "ok, err = panel:mount()\n",
            message, sizeof(message));
        Check(script != nullptr, "a script building a panel dynamically compiles");
        if (script) {
            probe(script,
                  "if not ok then error(err) end\n"
                  "if l1 ~= 1 or b1 ~= 'go' or sc ~= 'scroll1' or i1 ~= 'image1' then "
                  "error('names') end\n"
                  "if #names ~= 2 or names[2] ~= 'b' then error('row') end\n"
                  "if panel:control('image1') == nil or panel:control('scroll1') == nil "
                  "then error('controls') end\n"
                  "local l2 = panel:label({ text = 'live' })\n"
                  "if l2 ~= 3 then error('live label ' .. tostring(l2)) end\n"
                  "if panel:control('row3') == nil then error('live control') end\n"
                  "if events[1] ~= 'mounted' then error('mounted event') end\n"
                  "if panel:destroy() ~= true then error('destroy') end\n"
                  "if events[2] ~= 'unmounted' then error('unmounted event') end\n"
                  "local u = panel:update(1)\n"
                  "if u ~= false then error('update after destroy') end\n"
                  "local x = panel:label({ text = 'no' })\n"
                  "if x ~= nil then error('label after destroy') end",
                  "panel:label/button/image/scroll/row add controls before and after "
                  "mount, listeners fire on mount and unmount, and destroy() forgets "
                  "the spec");
            Check(MockAbi::LiveControls() == 0u,
                  "destroy() left no control behind");
            Check(MockAbi::CalledContaining(
                      MockAbi::ui_calls,
                      ("ui.control_create(" +
                          std::to_string(WOTBMOD_V3_UI_CONTROL_IMAGE) + ",").c_str()),
                  "the image was created as CONTROL_IMAGE");
            destroy_script(script);
        }
        // ---- the other facade-first examples, under their manifests --------
        //
        // The same rule for each shipped example: it compiles, on_enable runs
        // without a permission refusal under the permissions its manifest
        // asks for, a battle's worth of events does not raise, and on_disable
        // leaves nothing subscribed. The examples are also the `wotbmod new
        // --type lua --template` sources, so this is the template test too.
        struct FacadeExample {
            const wchar_t* folder;
            const char* id;
        };
        const FacadeExample facade_examples[] = {
            {L"lua_facade_panel", "example.lua_facade_panel"},
            {L"lua_facade_battle", "example.lua_facade_battle"},
            {L"lua_hud_tweaks", "example.lua_hud_tweaks"},
            {L"lua_skin_switcher", "example.lua_skin_switcher"},
        };
        for (const FacadeExample& example : facade_examples) {
            stage1b_permissions();
            MockAbi::storage_map_enabled = true;
            std::wstring folder = std::wstring(L"examples\\") + example.folder;
            const std::string source = ReadModApiFile((folder + L"\\main.lua").c_str());
            const std::string manifest =
                ReadModApiFile((folder + L"\\manifest.json").c_str());
            Check(!source.empty() && !manifest.empty(), example.id);
            std::memset(message, 0, sizeof(message));
            script = (source.empty() || manifest.empty() || !create_limited)
                         ? nullptr
                         : create_limited(manifest.c_str(), example.id, source.c_str(),
                                          message, sizeof(message));
            if (!script) std::fprintf(stderr, "    %s: %s\n", example.id, message);
            Check(script != nullptr, "the shipped facade example compiles under its manifest");
            if (!script) continue;
            std::memset(message, 0, sizeof(message));
            const uint32_t enable_status =
                eval(script, "on_enable()", message, sizeof(message));
            if (enable_status != 0u) {
                std::fprintf(stderr, "    %s enable: %s\n", example.id, message);
            }
            Check(enable_status == 0u, "on_enable runs");
            WotbModV3ClientEventEnvelope started = {};
            WOTBMOD_V3_INIT_STRUCT(started, WOTBMOD_V3_CLIENT_EVENT_ENVELOPE_VERSION);
            started.type = WOTBMOD_V3_CLIENT_EVENT_BATTLE_STARTED;
            started.payload_size = sizeof(WotbModV3BattleEventData);
            MockAbi::event_payload.assign(
                reinterpret_cast<const char*>(&started), sizeof(started));
            MockAbi::FireEvent(WOTBMOD_V3_EVENT_BATTLE_STARTED);
            WotbModV3ClientEventEnvelope shot = {};
            WOTBMOD_V3_INIT_STRUCT(shot, WOTBMOD_V3_CLIENT_EVENT_ENVELOPE_VERSION);
            shot.type = WOTBMOD_V3_CLIENT_EVENT_SHOT_FIRED;
            shot.payload_size = sizeof(WotbModV3ShotEventData);
            MockAbi::event_payload.assign(
                reinterpret_cast<const char*>(&shot), sizeof(shot));
            MockAbi::FireEvent(WOTBMOD_V3_EVENT_SHOT_FIRED);
            MockAbi::event_payload.clear();
            MockAbi::event_timestamp_ns = 1000000000ull;
            MockAbi::FireEvent(WOTBMOD_V3_EVENT_FRAME_UPDATE);
            MockAbi::event_timestamp_ns = 47000000000ull;
            MockAbi::FireEvent(WOTBMOD_V3_EVENT_FRAME_UPDATE);
            MockAbi::FireInputAction(0u, 1.0f, 1u);
            WotbModV3ClientEventEnvelope ended = {};
            WOTBMOD_V3_INIT_STRUCT(ended, WOTBMOD_V3_CLIENT_EVENT_ENVELOPE_VERSION);
            ended.type = WOTBMOD_V3_CLIENT_EVENT_BATTLE_ENDED;
            ended.payload_size = sizeof(WotbModV3BattleEventData);
            ended.payload.battle.winner_team = 1u;
            MockAbi::event_payload.assign(
                reinterpret_cast<const char*>(&ended), sizeof(ended));
            MockAbi::FireEvent(WOTBMOD_V3_EVENT_BATTLE_ENDED);
            std::memset(message, 0, sizeof(message));
            const uint32_t frame_status = eval(script, "if on_frame then on_frame(30, 0.016) end",
                                               message, sizeof(message));
            if (frame_status != 0u) {
                std::fprintf(stderr, "    %s frame: %s\n", example.id, message);
            }
            Check(frame_status == 0u, "a frame runs");
            bool refused = false;
            for (const std::string& line : MockAbi::core_calls) {
                if (line.find("permission denied") != std::string::npos ||
                    line.find("handler raised") != std::string::npos) {
                    refused = true;
                    std::fprintf(stderr, "    %s: %s\n", example.id, line.c_str());
                }
            }
            Check(!refused,
                  "nothing was refused under the manifest and no handler raised");
            std::memset(message, 0, sizeof(message));
            const uint32_t disable_status =
                eval(script, "on_disable()", message, sizeof(message));
            if (disable_status != 0u) {
                std::fprintf(stderr, "    %s disable: %s\n", example.id, message);
            }
            Check(disable_status == 0u && MockAbi::LiveSubscriptions() == 0u &&
                      MockAbi::LiveControls() == 0u,
                  "on_disable gives every subscription and control back");
            destroy_script(script);
        }
        MockAbi::Reset();
        Check(entry(&bootstrap, 1u, &info) == WOTBMOD_V3_OK,
              "the default ceiling is measured again after the stage-1b facades");
    }

    // ---- wotb.ui: 18 of WotbModV3UiApiV2's 74 slots ------------------------
    //
    // Nothing here calls back into Lua - every one of these 18 is a
    // synchronous request-reply, so this runs through the same plain `run`
    // shim storage used, not the persistent-script machinery events needed.
    if (run) {
        MockAbi::Reset();
        char message[512] = {};

        Check(run(
                  "if wotb.ui.EVENT_CLICK ~= 1 or "
                  "wotb.ui.EVENT_POINTER_DOWN ~= 9 or "
                  "wotb.ui.EVENT_DRAG ~= 12 or "
                  "wotb.ui.EVENT_CANCEL ~= 16 then "
                  "error('UI event constants are incomplete') end",
                  message, sizeof(message)) == 0u,
              "the Lua UI table publishes the frozen UI event constants");

        MockAbi::Reset();
        std::memset(message, 0, sizeof(message));
        Check(run(
                  "local control = wotb.ui.control_create()\n"
                  "local style, err = control:push_style({\n"
                  "  fields = wotb.ui.STYLE_OPACITY, opacity = 0.5\n"
                  "})\n"
                  "if style == nil then error('push: ' .. tostring(err)) end\n"
                  "local popped, pop_err = style:pop()\n"
                  "if not popped then "
                  "error('pop: ' .. tostring(pop_err)) end\n"
                  "local updated, update_err = style:update({\n"
                  "  fields = wotb.ui.STYLE_OPACITY, opacity = 0.7\n"
                  "})\n"
                  "if updated ~= nil or not string.find(update_err, "
                  "'ui.style_update') then\n"
                  "  error('dead style accepted: ' .. tostring(update_err))\n"
                  "end\n"
                  "local other = wotb.lifecycle.get_current()\n"
                  "local wrong, wrong_err = wotb.ui.style_update(other, {\n"
                  "  fields = wotb.ui.STYLE_OPACITY, opacity = 0.9\n"
                  "})\n"
                  "if wrong ~= nil or not string.find(wrong_err, "
                  "'ui.style_update') then\n"
                  "  error('wrong handle accepted: ' .. tostring(wrong_err))\n"
                  "end",
                  message, sizeof(message)) == 0u,
              "style update rejects both an already-popped style and an "
              "unrelated generated handle");
        Check(MockAbi::CountCalled(MockAbi::ui_calls,
                                   "ui.style_update(9900)") == 1u &&
                  MockAbi::CountCalled(MockAbi::ui_calls,
                                       "ui.style_update(9901)") == 1u,
              "both stale-style and wrong-handle Lua calls reached the mock "
              "ABI and were rejected there");

        MockAbi::Reset();
        std::memset(message, 0, sizeof(message));
        Check(run(
                  "local label = wotb.ui.create({\n"
                  "  type = wotb.ui.CONTROL_TEXT, id = 'runtime_label',\n"
                  "  text = 'Громкость: 75%', x = 40, y = 60,\n"
                  "  width = 320, height = 48, visible = true,\n"
                  "  background_color = {r=0.2, g=0.3, b=0.4, a=0.8}\n"
                  "})\n"
                  "if label == nil then error('create returned nil') end\n"
                  "if not label:set_text('Громкость: 80%') then "
                  "error('method set_text failed') end\n"
                  "if not label:set_size(360, 52) then "
                  "error('method set_size failed') end\n"
                  "if not label:set_visible(false) then "
                  "error('method set_visible failed') end",
                  message, sizeof(message)) == 0u,
              "ui.create builds a runtime control from a Lua table and "
              "control handles expose colon-call methods");
        Check(MockAbi::ui_controls.size() == 1u &&
                  MockAbi::ui_controls[0].type ==
                      WOTBMOD_V3_UI_CONTROL_TEXT &&
                  MockAbi::ui_controls[0].id == "runtime_label" &&
                  MockAbi::ui_controls[0].text == "Громкость: 80%" &&
                  MockAbi::ui_controls[0].position.x == 40.0f &&
                  MockAbi::ui_controls[0].position.y == 60.0f &&
                  MockAbi::ui_controls[0].size.x == 360.0f &&
                  MockAbi::ui_controls[0].size.y == 52.0f &&
                  !MockAbi::ui_controls[0].visible,
              "the table descriptor and colon-call mutations reached the "
              "UI ABI with distinguishable values");
        Check(
            MockAbi::Called(
                MockAbi::ui_calls,
                "ui.style_push(6000,2,0.200000,0.800000)"),
            "ui.create forwards background_color through a managed style "
            "override rather than dropping it");

        MockAbi::Reset();
        // Step 1 from the brief: build a panel, read its size back, destroy
        // it, and confirm the destroyed handle is refused rather than
        // accepted or crashed on.
        Check(run("local c = wotb.ui.control_create()\n"
                  "wotb.ui.control_set_size(c, 200, 80)\n"
                  "local w, h = wotb.ui.control_get_size(c)\n"
                  "if w ~= 200 or h ~= 80 then error('size round trip') end\n"
                  "wotb.ui.control_destroy(c)\n"
                  "local ok, err = wotb.ui.control_set_text(c, 'x')\n"
                  "if ok ~= nil then "
                  "error('a destroyed control was accepted') end",
                  message, sizeof(message)) == 0u,
              "controls round-trip and a destroyed handle is refused");
        Check(MockAbi::Called(MockAbi::ui_calls, "ui.control_create(0,1)"),
              "control_create defaulted to CONTROL_CONTAINER with visible=1");
        Check(MockAbi::Called(MockAbi::ui_calls, "ui.control_destroy(6000)"),
              "control_destroy reached the ABI with the handle it was "
              "given");
        // The refused call still reached the ABI: liveness is enforced by
        // the client (this mock), not smuggled into the Lua binding layer -
        // the binding forwards any handle of the right *type* and only the
        // client knows whether it is still alive, exactly as control_destroy
        // really works.
        Check(MockAbi::Called(MockAbi::ui_calls, "ui.control_set_text(6000,x)"),
              "the refused control_set_text still reached the ABI - it is "
              "the client's liveness check that refused it, not an "
              "argument check the binding short-circuited");

        // ---- handle invalidation, generalized -----------------------------
        //
        // control_set_text above is the brief's own required case. Every
        // other slot that takes a *required* control handle must refuse a
        // destroyed one exactly the same way - the binding forwards any
        // handle of the right type uniformly, so this is really one proof
        // repeated, but "the binding is uniform" is an assumption until each
        // slot is actually driven through a dead handle once. One
        // independent script per slot: a handle is userdata, and unlike
        // storage's plain integer tokens it cannot be marshalled between
        // separate run() calls, so each iteration builds and destroys its
        // own control (and resolves a live slot too, for slot_attach/
        // slot_detach's own control argument). The first entry is
        // control_destroy called a second time on a handle it already
        // destroyed - the double-destroy case.
        //
        // This is exhaustive over every slot that takes a *required* control
        // handle - all 14 of them, including slot_attach and slot_detach's
        // own second argument, not just the 12 whose only argument type is a
        // control. control_is_alive and control_find_by_id are the only two
        // deliberately absent, and both are named exclusions rather than
        // gaps: is_alive must succeed with alive=false for a dead handle
        // rather than refuse (proven above in the tree block below), and
        // find_by_id's only handle argument is the optional root, covered by
        // its own four-case block below rather than this sweep.
        {
            static const char* const kRefusedOnDeadHandle[] = {
                "wotb.ui.control_destroy(c)",
                "wotb.ui.control_add_child(c, other)",
                "wotb.ui.control_add_child(other, c)",
                "wotb.ui.control_remove_child(other, c)",
                "wotb.ui.control_set_id(c, 'x')",
                "wotb.ui.control_set_text(c, 'x')",
                "wotb.ui.control_set_visible(c, true)",
                "wotb.ui.control_set_position(c, 1, 2)",
                "wotb.ui.control_get_position(c)",
                "wotb.ui.control_set_size(c, 1, 2)",
                "wotb.ui.control_get_size(c)",
                "wotb.ui.control_set_anchor(c, 1, 2)",
                "wotb.ui.control_set_pivot(c, 1, 2)",
                "wotb.ui.slot_attach(slot, c, 5)",
                "wotb.ui.slot_detach(slot, c)",
            };
            for (const char* op : kRefusedOnDeadHandle) {
                MockAbi::Reset();
                char script[512] = {};
                std::snprintf(
                    script, sizeof(script),
                    "local c = wotb.ui.control_create()\n"
                    "local other = wotb.ui.control_create()\n"
                    "local slot = wotb.ui.slot_find('wotb.mock_slot')\n"
                    "wotb.ui.control_destroy(c)\n"
                    "local ok = %s\n"
                    "if ok ~= nil then "
                    "error('a destroyed handle was accepted') end",
                    op);
                char op_message[512] = {};
                Check(run(script, op_message, sizeof(op_message)) == 0u, op);
            }
        }

        // ---- the tree: add_child, remove_child, set_id, find_by_id, ------
        // ---- is_alive, and control_find_by_id's own root argument ---------
        MockAbi::Reset();
        std::memset(message, 0, sizeof(message));
        Check(run(
            "local parent = wotb.ui.control_create()\n"
            "local child = wotb.ui.control_create()\n"
            "if not wotb.ui.control_add_child(parent, child) then "
            "error('add_child failed') end\n"
            "if not wotb.ui.control_set_id(child, 'my_child') then "
            "error('set_id failed') end\n"
            "local found = wotb.ui.control_find_by_id(nil, 'my_child')\n"
            "if found == nil then "
            "error('find_by_id missed it (root=nil)') end\n"
            "local scoped = wotb.ui.control_find_by_id(parent, 'my_child')\n"
            "if scoped == nil then "
            "error('find_by_id missed it under its own real ancestor') end\n"
            "local alive_ok, alive = wotb.ui.control_is_alive(child)\n"
            "if not alive_ok or not alive then "
            "error('should report alive') end\n"
            "if not wotb.ui.control_remove_child(parent, child) then "
            "error('remove_child failed') end\n"
            "if not wotb.ui.control_add_child(parent, child) then "
            "error('re-adding the child failed') end\n"
            "wotb.ui.control_destroy(child)\n"
            "local dead_ok, dead = wotb.ui.control_is_alive(child)\n"
            "if not dead_ok or dead then "
            "error('is_alive should report false, not fail, for a dead "
            "handle') end\n"
            "local missing = wotb.ui.control_find_by_id(nil, 'my_child')\n"
            "if missing ~= nil then "
            "error('find_by_id should miss a destroyed control') end",
            message, sizeof(message)) == 0u,
            "add_child, remove_child, set_id, find_by_id (with and without "
            "an explicit root) and is_alive all work, including is_alive "
            "answering false rather than failing for a destroyed handle");
        // add_child and remove_child are mutually transposable: both take a
        // control and a control and both return OK for two live handles, so
        // "if not wotb.ui.control_add_child(...) then error(...)" above
        // would pass exactly as written even if kUiFuncs had the two
        // swapped. Only the exact recorded call, naming which ABI slot
        // actually ran, closes that gap.
        //
        // And that only works if the two calls are distinguishable in the
        // record. They were not. This pair used to assert
        // add_child(6000,6001) and remove_child(6000,6001) - the same two
        // handles, once each - so transposing the two entries in kUiFuncs
        // produced exactly the same two strings with the labels swapped, and
        // both assertions stayed green. The check written to catch a
        // transposition was itself invariant under one.
        //
        // The script above now removes the child and puts it back, so the
        // counts differ: two add_child calls and one remove_child, on the same
        // pair. Verified by actually transposing the two kUiFuncs entries and
        // rebuilding, not by reading the code - which is the only way to know,
        // and is how the old pair's blindness was missed in the first place.
        // Transposed, the suite goes from 563/0 to 561/2: the script itself
        // fails at find_by_id (the parent link was never set, so the scoped
        // search misses) and the add_child count below reads 0 instead of 2.
        // The remove_child count happens to survive, because the script dies
        // before reaching that call - which is exactly why this is a pair and
        // not a single check.
        Check(MockAbi::CountCalled(MockAbi::ui_calls,
                                   "ui.control_add_child(6000,6001)") == 2u,
              "control_add_child reached the ABI's own add_child, not "
              "remove_child - twice, which is what makes the count say so");
        Check(MockAbi::CountCalled(MockAbi::ui_calls,
                                   "ui.control_remove_child(6000,6001)") == 1u,
              "and control_remove_child reached remove_child, not "
              "add_child - once");

        // control_find_by_id scopes its search to root's own descendants
        // when a root is given, not the whole tree - proven by a negative:
        // a real, live, unrelated root must miss a child that exists only
        // under a different parent.
        std::memset(message, 0, sizeof(message));
        Check(run("local parent = wotb.ui.control_create(); "
                  "local child = wotb.ui.control_create(); "
                  "local unrelated = wotb.ui.control_create(); "
                  "wotb.ui.control_add_child(parent, child); "
                  "wotb.ui.control_set_id(child, 'scoped_child'); "
                  "local ok, err = "
                  "wotb.ui.control_find_by_id(unrelated, 'scoped_child'); "
                  "if ok ~= nil then "
                  "error('found a control that is not a descendant of "
                  "root') end",
                  message, sizeof(message)) == 0u,
              "control_find_by_id scopes its search to root's own "
              "descendants when a root is given, not the whole tree");

        // The four argument shapes control_find_by_id's own optional root
        // can take: a live handle and an explicit/omitted nil are both
        // exercised above (found/scoped, and the root=nil calls). The
        // remaining two - a wrong handle type, and a destroyed handle - are
        // exactly what CheckOptArgHandle exists to refuse and what this
        // mock's own root resolution (added alongside CheckOptArgHandle)
        // exists to enforce; before that fix, control_find_by_id was the one
        // control-taking slot that accepted a destroyed handle silently.
        std::memset(message, 0, sizeof(message));
        Check(run("local slot = wotb.ui.slot_find('wotb.mock_slot'); "
                  "local ok, err = "
                  "wotb.ui.control_find_by_id(slot, 'anything'); "
                  "if ok ~= nil then "
                  "error('a slot handle was accepted as root') end; "
                  "if not string.find(err, 'wotb.control') then "
                  "error('wrong message: ' .. tostring(err)) end",
                  message, sizeof(message)) == 0u,
              "control_find_by_id refuses a root argument of the wrong "
              "handle type");

        std::memset(message, 0, sizeof(message));
        Check(run("local root = wotb.ui.control_create(); "
                  "wotb.ui.control_destroy(root); "
                  "local ok, err = "
                  "wotb.ui.control_find_by_id(root, 'anything'); "
                  "if ok ~= nil then "
                  "error('a destroyed root was silently accepted') end",
                  message, sizeof(message)) == 0u,
              "control_find_by_id refuses a destroyed root handle rather "
              "than silently searching as if root had been omitted");

        // ---- text, visibility and geometry: set_text, set_visible, -------
        // ---- set_position/get_position, set_anchor, set_pivot -------------
        MockAbi::Reset();
        std::memset(message, 0, sizeof(message));
        Check(run(
            "local c = wotb.ui.control_create(wotb.ui.CONTROL_TEXT)\n"
            "if not wotb.ui.control_set_text(c, 'hello') then "
            "error('set_text failed') end\n"
            "if not wotb.ui.control_set_visible(c, false) then "
            "error('set_visible failed') end\n"
            "if not wotb.ui.control_set_position(c, 10, 20) then "
            "error('set_position failed') end\n"
            "local x, y = wotb.ui.control_get_position(c)\n"
            "if x ~= 10 or y ~= 20 then error('position round trip') end\n"
            "if not wotb.ui.control_set_anchor(c, 0.5, 0.5) then "
            "error('set_anchor failed') end\n"
            "if not wotb.ui.control_set_pivot(c, 0.5, 1.0) then "
            "error('set_pivot failed') end",
            message, sizeof(message)) == 0u,
            "control_create with an explicit type, set_text, set_visible, "
            "a position round trip, set_anchor and set_pivot all work");
        Check(MockAbi::Called(MockAbi::ui_calls, "ui.control_create(1,1)"),
              "the explicit CONTROL_TEXT type reached the descriptor "
              "(WOTBMOD_V3_UI_CONTROL_TEXT == 1)");

        Check(MockAbi::Called(MockAbi::ui_calls, "ui.control_set_visible(6000,0)"),
              "control_set_visible reached the ABI with false converted to "
              "0, not merely returned truthy");
        // anchor and pivot are mutually transposable the same way add_child/
        // remove_child are: both take a control and two numbers, both
        // return OK for a live handle, so "if not
        // wotb.ui.control_set_anchor(...) then error(...)" alone cannot
        // tell anchor from pivot. Only the exact recorded call, with each
        // slot's own distinct values, proves which ABI slot actually ran.
        Check(MockAbi::Called(MockAbi::ui_calls,
                              "ui.control_set_anchor(6000,0.500000,0.500000)"),
              "control_set_anchor reached anchor, not pivot, with the "
              "values given");
        Check(MockAbi::Called(MockAbi::ui_calls,
                              "ui.control_set_pivot(6000,0.500000,1.000000)"),
              "and control_set_pivot reached pivot, not anchor, with its "
              "own distinct values");

        MockAbi::Reset();
        std::memset(message, 0, sizeof(message));
        Check(run(
            "local c = wotb.ui.control_create("
            "wotb.ui.CONTROL_CONTAINER, "
            "'LuaUiFrameworkPanel', "
            "'mod://self/ui/lua_ui_framework.yaml')\n"
            "if c == nil then error('template create failed') end",
            message, sizeof(message)) == 0u,
            "control_create accepts an optional object id and trusted UI "
            "template URI");
        Check(MockAbi::ui_controls.size() == 21u &&
                  MockAbi::ui_controls[0].id == "LuaUiFrameworkPanel" &&
                  MockAbi::ui_controls[0].texture_uri ==
                      "mod://self/ui/lua_ui_framework.yaml" &&
                  MockAbi::ui_controls[1].id == "OpenButton",
              "control_create forwards the template object id and URI in "
              "the frozen descriptor fields and loads its fixture tree");

        // control_create refuses a type outside WotbModV3UiControlType,
        // through CheckArgEnum rather than a hand-rolled range check
        // (lua_convert.h rule 6).
        std::memset(message, 0, sizeof(message));
        Check(run("local ok, err = wotb.ui.control_create(99); "
                  "if ok ~= nil then "
                  "error('an undefined control type was accepted') end",
                  message, sizeof(message)) == 0u,
              "control_create refuses a type value outside "
              "WotbModV3UiControlType");

        // ---- named slots: slot_find, slot_attach, slot_detach -------------
        MockAbi::Reset();
        std::memset(message, 0, sizeof(message));
        Check(run(
            "local slot = wotb.ui.slot_find('wotb.mock_slot')\n"
            "if slot == nil then "
            "error('slot_find missed the known slot') end\n"
            "local c = wotb.ui.control_create()\n"
            "if not wotb.ui.slot_attach(slot, c, 5) then "
            "error('slot_attach failed') end\n"
            "if not wotb.ui.slot_detach(slot, c) then "
            "error('slot_detach failed') end",
            message, sizeof(message)) == 0u,
            "slot_find resolves a known slot id, and slot_attach/"
            "slot_detach both work on it");
        // slot_attach and slot_detach are the same transposable shape as
        // add_child/remove_child - both take a slot and a control and both
        // return OK for two live handles - and priority is this file's only
        // narrowing lua_Integer -> int32_t cast, which "if not
        // wotb.ui.slot_attach(...)" alone does not prove survived intact
        // either. The exact recorded call proves both at once.
        Check(MockAbi::Called(MockAbi::ui_calls, "ui.slot_attach(7000,6000,5)"),
              "slot_attach reached attach, not detach, and the priority "
              "argument arrived intact through the int32_t cast");
        Check(MockAbi::Called(MockAbi::ui_calls, "ui.slot_detach(7000,6000)"),
              "and slot_detach reached detach, not attach");

        // The two candidates the brief names for the optional-handle shape
        // (rule 2's `true, handle_or_nil`) both turn out not to need it:
        // ui_v2.h's own comment documents slot_find failing with
        // WOTBMOD_V3_E_NOT_SUPPORTED for a declared-but-unresolved slot, and
        // docs/API_V3_RU.md documents WOTBMOD_V3_E_NOT_FOUND for an unknown
        // one - never WOTBMOD_V3_OK with an empty handle. A failing result
        // already answers `nil, message` through PushResultWith, which is
        // the correct shape for a call the ABI says did not succeed.
        std::memset(message, 0, sizeof(message));
        Check(run("local ok, err = "
                  "wotb.ui.slot_find('wotb.mock_unresolved_slot'); "
                  "if ok ~= nil then error('should have failed') end; "
                  "if not string.find(err, 'not supported') then "
                  "error('wrong message: ' .. tostring(err)) end",
                  message, sizeof(message)) == 0u,
              "slot_find answers nil, message - not true, nil - for a slot "
              "whose extension point is declared but not resolved");

        std::memset(message, 0, sizeof(message));
        Check(run("local ok, err = wotb.ui.slot_find('wotb.no_such_slot'); "
                  "if ok ~= nil then error('should have failed') end; "
                  "if not string.find(err, 'not found') then "
                  "error('wrong message: ' .. tostring(err)) end",
                  message, sizeof(message)) == 0u,
              "slot_find answers nil, message for an unknown slot id too");

        std::memset(message, 0, sizeof(message));
        Check(run("local ok, err = "
                  "wotb.ui.control_find_by_id(nil, "
                  "'nope-nobody-has-this-id'); "
                  "if ok ~= nil then error('should have failed') end",
                  message, sizeof(message)) == 0u,
              "control_find_by_id answers nil, message too - not true, nil "
              "- when nothing matches");

        // Rule 3, first direction: a handle carries its own type, so a
        // control cannot be passed where a slot belongs even though both
        // are userdata this same host minted.
        std::memset(message, 0, sizeof(message));
        Check(run("local c = wotb.ui.control_create(); "
                  "local ok, err = wotb.ui.slot_attach(c, c, 0); "
                  "if ok ~= nil then "
                  "error('a control handle was accepted as a slot') end; "
                  "if not string.find(err, 'wotb.slot') then "
                  "error('wrong message: ' .. tostring(err)) end",
                  message, sizeof(message)) == 0u,
              "slot_attach refuses a control handle passed where a slot "
              "handle belongs");

        // Rule 3, the other direction: a bare number cannot stand in for a
        // handle either - it is forgeable from inside the sandbox, which is
        // the entire reason a handle crosses as userdata rather than a
        // number.
        std::memset(message, 0, sizeof(message));
        Check(run("local ok, err = wotb.ui.control_set_text(42, 'x'); "
                  "if ok ~= nil then "
                  "error('a number was accepted as a handle') end; "
                  "if not string.find(err, 'wotb.control') then "
                  "error('wrong message: ' .. tostring(err)) end",
                  message, sizeof(message)) == 0u,
              "control_set_text refuses a bare number where a handle "
              "belongs");

        // ---- the failure switch, on this interface's own slots ------------
        MockAbi::Reset();
        std::memset(message, 0, sizeof(message));
        MockAbi::ForceFailure(WOTBMOD_V3_E_PERMISSION_DENIED);
        Check(run("local c, e = wotb.ui.control_create(); "
                  "if c ~= nil then error('create should have failed') end; "
                  "if not string.find(e, 'ui.control_create') then "
                  "error('missing context: ' .. tostring(e)) end",
                  message, sizeof(message)) == 0u,
              "the failure switch reaches ui.control_create with its own "
              "slot's context in the message");
        MockAbi::ClearFailure();
    }

    // ---- ownership: nothing a script created outlives it -------------------
    //
    // The decisive test of the prototype, and the one shaped like the failure
    // it exists to prevent. Fifty reloads is not an arbitrary number: a leak
    // of one control per reload is invisible once and obvious after half an
    // hour of editing, which is exactly the failure that reaches a player
    // rather than a test. Each iteration is a whole script lifetime - run()
    // creates the state, runs the chunk, and destroys it before it returns -
    // so fifty iterations are fifty reloads.
    //
    // Three resource kinds, because the registry tracks three: a subscription
    // the client can call back through, a control the player can see, and an
    // open storage transaction. None of these scripts defines on_disable and
    // none of them tidies up after itself, which is the point - correctness
    // must not depend on a script author remembering to.
    if (run) {
        MockAbi::Reset();
        char message[512] = {};

        for (int i = 0; i < 50; ++i) {
            std::memset(message, 0, sizeof(message));
            Check(run("wotb.events.subscribe('wotbmod.frame.update', "
                      "function() end)\n"
                      "local c = wotb.ui.control_create()\n"
                      "wotb.ui.control_set_id(c, 'panel')\n"
                      "wotb.storage.begin_transaction()",
                      message, sizeof(message)) == 0u,
                  "reload iteration runs");
        }

        // First that the cycles really did create something - fifty of each.
        // Without this the three assertions below would pass just as well
        // against a host where every create failed, which is the way a
        // leak test most often goes quietly wrong.
        Check(MockAbi::event_subscriptions.size() == 50u,
              "fifty reloads really did subscribe fifty times");
        Check(MockAbi::ui_controls.size() == 50u,
              "and really did create fifty controls");
        Check(MockAbi::CountCalled(MockAbi::storage_calls,
                                   "storage.begin_transaction()") == 50u,
              "and really did open fifty transactions");

        Check(MockAbi::LiveSubscriptions() == 0u,
              "no subscription survives its script");
        Check(MockAbi::LiveControls() == 0u,
              "no control survives its script");
        Check(MockAbi::LiveTransactions() == 0u,
              "no open transaction survives its script");

        // And the client can still be told to deliver without finding
        // anything: firing into a state that has been closed is an access
        // violation, which exits the process with 0xC0000005 - the build
        // script compares the exit code against 0 rather than using `if
        // errorlevel 1` precisely so that a crash here cannot pass. Reaching
        // the assertion after it is the test.
        MockAbi::FireEvent("wotbmod.frame.update");
        Check(MockAbi::LiveSubscriptions() == 0u,
              "and firing the topic afterwards reaches nothing at all");

        // A script that dies partway through its own top-level body. Nothing
        // ran an on_disable here, nothing could have: the chunk raised before
        // it finished. LuaScript::Create deletes the script on that path, and
        // deleting it is what revokes.
        MockAbi::Reset();
        std::memset(message, 0, sizeof(message));
        Check(run("wotb.ui.control_create()\n"
                  "wotb.storage.begin_transaction()\n"
                  "error('boom')",
                  message, sizeof(message)) == 2u,
              "a script that raises after creating things is a runtime error");
        Check(MockAbi::ui_controls.size() == 1u &&
                  MockAbi::LiveControls() == 0u &&
                  MockAbi::LiveTransactions() == 0u,
              "and what it created before it raised is still taken back - a "
              "script that never reached its own cleanup is the ordinary case, "
              "not the exceptional one");

        // A script whose cleanup is wrong rather than absent: it destroys the
        // same control twice, and the client refuses the second. The count is
        // the assertion - two attempts from the script and none added by
        // teardown, which is what Forget-on-destroy buys. Without it the host
        // would ask the client to destroy a handle it had already released,
        // and the ABI makes no promise that the number has not been reused.
        MockAbi::Reset();
        std::memset(message, 0, sizeof(message));
        Check(run("local c = wotb.ui.control_create()\n"
                  "wotb.ui.control_destroy(c)\n"
                  "local ok = wotb.ui.control_destroy(c)\n"
                  "if ok ~= nil then error('a double destroy was accepted') end",
                  message, sizeof(message)) == 0u,
              "a script may destroy the same control twice and be refused");
        Check(MockAbi::CountCalled(MockAbi::ui_calls,
                                   "ui.control_destroy(6000)") == 2u,
              "and teardown adds no third destroy of its own - a control the "
              "script handed back is off the ledger, so it is never released "
              "twice");
    }

    // The ledger's own side of the same question, and the shapes that need a
    // script which outlives the call that made it.
    using OwnedCountFn = uint32_t(WOTBMOD_V3_CALL*)(void*);
    using SubscriptionRecordsFn = uint32_t(WOTBMOD_V3_CALL*)();
    const auto owned_count = reinterpret_cast<OwnedCountFn>(
        GetProcAddress(module, "WotbLuaHost_OwnedCountForTests"));
    const auto host_records = reinterpret_cast<SubscriptionRecordsFn>(
        GetProcAddress(module, "WotbLuaHost_SubscriptionRecordsForTests"));
    Check(owned_count != nullptr && host_records != nullptr,
          "the ownership test entry points are exported");

    // The host's own half of "nothing survives its script", asked once here
    // because every script this suite has created has been destroyed by now.
    // The client's counters cannot see this one: a subscription is two
    // records, and revoking the client's says nothing about the host's - which
    // is the dangerous half, since it names a LuaScript that has been freed.
    if (host_records) {
        Check(host_records() == 0u,
              "the host is holding no subscription record of its own either, "
              "after every script this suite ran has been destroyed - "
              "including the fifty reloads above");
    }

    if (create_script && eval && destroy_script && owned_count && host_records) {
        char message[512] = {};

        // ---- the ledger tracks all three kinds, and lets go of each --------
        //
        // Checked from both sides on purpose. The mock's counters say what the
        // client is still holding; owned_count says what the host still
        // believes it owes. A ledger that quietly stopped recording would
        // leave the client's counters at zero for entirely the wrong reason,
        // and every leak assertion in this file would pass over it.
        MockAbi::Reset();
        std::memset(message, 0, sizeof(message));
        void* script = create_script(
            "ownership",
            "sub = wotb.events.subscribe('wotbmod.frame.update', "
            "function() end)\n"
            "panel = wotb.ui.control_create()\n"
            "txn = wotb.storage.begin_transaction()",
            message, sizeof(message));
        Check(script != nullptr, "a script holding one of each kind");
        if (script) {
            Check(owned_count(script) == 3u,
                  "the ledger holds a subscription, a control and a "
                  "transaction");
            Check(MockAbi::LiveSubscriptions() == 1u &&
                      MockAbi::LiveControls() == 1u &&
                      MockAbi::LiveTransactions() == 1u,
                  "and the client agrees it is holding one of each");

            std::memset(message, 0, sizeof(message));
            Check(eval(script,
                       "if wotb.events.unsubscribe(sub) ~= true then "
                       "error('unsubscribe') end\n"
                       "if wotb.ui.control_destroy(panel) ~= true then "
                       "error('destroy') end\n"
                       "if wotb.storage.commit(txn) ~= true then "
                       "error('commit') end",
                       message, sizeof(message)) == 0u,
                  "a script may hand all three back itself");
            Check(owned_count(script) == 0u,
                  "and the ledger lets go of each as it does - a script that "
                  "cleans up after itself is not then cleaned up after twice");

            destroy_script(script);
            Check(MockAbi::CountCalled(MockAbi::events_calls,
                                       "events.unsubscribe(9000)") == 1u &&
                      MockAbi::CountCalled(MockAbi::ui_calls,
                                           "ui.control_destroy(6000)") == 1u,
                  "so teardown released nothing a second time");
            Check(MockAbi::CountCalled(MockAbi::storage_calls,
                                       "storage.rollback(777)") == 0u,
                  "and a committed transaction is not rolled back behind the "
                  "script's back on the way out");
            script = nullptr;
        }

        // ---- a script that hands nothing back, and the order it is taken ---
        MockAbi::Reset();
        std::memset(message, 0, sizeof(message));
        script = create_script(
            "ownership_silent",
            "wotb.events.subscribe('wotbmod.frame.update', function() end)\n"
            "wotb.ui.control_create()\n"
            "wotb.storage.begin_transaction()",
            message, sizeof(message));
        Check(script != nullptr, "a script that keeps no handles at all");
        if (script) {
            Check(owned_count(script) == 3u,
                  "the ledger holds all three even though the script kept no "
                  "reference to any of them - what is revocable does not "
                  "depend on what the script remembered");
            destroy_script(script);
            script = nullptr;
            Check(MockAbi::LiveSubscriptions() == 0u &&
                      MockAbi::LiveControls() == 0u &&
                      MockAbi::LiveTransactions() == 0u,
                  "and the client is left holding none of them");
            // Asked of the host as well as of the client, here specifically:
            // this is the one block where a script held a subscription and
            // handed nothing back, so a per-script host-record leak would show
            // most directly. Experiment 4 in the report is the reason - a
            // leak test that only asks the mock cannot see the host's half.
            Check(host_records() == 0u,
                  "and the host kept no subscription record of its own");
            // Every revocation carried this script's own mod handle - the one
            // the host was loaded with, not a zero or a stale value. The
            // registry passes a mod_ captured at Bind time rather than reusing
            // a binding's, and this script took no part in its own teardown,
            // so all three of these were set by RevokeAll and by nothing else.
            Check(MockAbi::last_unsubscribe_mod == 1u &&
                      MockAbi::last_control_destroy_mod == 1u &&
                      MockAbi::last_rollback_mod == 1u,
                  "and each of the three revocations carried the mod handle "
                  "this host was loaded with");
            Check(MockAbi::Called(MockAbi::storage_calls,
                                  "storage.rollback(777)"),
                  "an unfinished transaction is rolled back, not committed - "
                  "a script destroyed mid-transaction never said its writes "
                  "were finished, and half an intended change written to a "
                  "player's storage is worse than none of it");
            // The ordering rule, stated as the invariant rather than as a
            // sequence of two calls in two different record vectors. See the
            // witness's own comment in the mock.
            Check(MockAbi::live_subscriptions_at_destroy == 0u,
                  "and no control was destroyed while a subscription was "
                  "still live - subscriptions are revoked first precisely so "
                  "that a callback cannot fire against a control that is "
                  "already gone");
        }

        // ---- a destroy the client refuses stays on the ledger --------------
        //
        // The other half of forgetting only on success. A control the client
        // would not destroy is still there, and a host that had struck it off
        // the moment the script asked would have nothing left to take back.
        MockAbi::Reset();
        std::memset(message, 0, sizeof(message));
        script = create_script("ownership_refused",
                               "c = wotb.ui.control_create()",
                               message, sizeof(message));
        Check(script != nullptr, "a script with one control");
        if (script) {
            MockAbi::ForceFailure(WOTBMOD_V3_E_PERMISSION_DENIED);
            std::memset(message, 0, sizeof(message));
            Check(eval(script,
                       "local ok = wotb.ui.control_destroy(c)\n"
                       "if ok ~= nil then error('a refused destroy reported "
                       "success') end",
                       message, sizeof(message)) == 0u,
                  "the client refuses to destroy it");
            MockAbi::ClearFailure();
            Check(owned_count(script) == 1u && MockAbi::LiveControls() == 1u,
                  "the control is still alive and still on the ledger");
            destroy_script(script);
            script = nullptr;
            Check(MockAbi::LiveControls() == 0u,
                  "and teardown takes it back - a destroy the client refused "
                  "does not take a control off the ledger");
        }

        // ---- an unsubscribe the client refuses is retried at teardown ------
        //
        // The symmetry with the two cases above, and the one path that gives
        // the registry's own backstop sweep something to do.
        //
        // This host retires its record the moment the script asks, so nothing
        // can be delivered into the script either way and
        // ReleaseEventSubscriptions will never see this subscription again.
        // The client, though, refused - it is still holding a live
        // subscription for a script that is about to die. Forgetting the token
        // then would make that permanent. Keeping it leaves exactly one thing
        // that can still retry, and that thing is the sweep.
        MockAbi::Reset();
        std::memset(message, 0, sizeof(message));
        script = create_script(
            "ownership_refused_unsubscribe",
            "sub = wotb.events.subscribe('wotbmod.frame.update', "
            "function() end)",
            message, sizeof(message));
        Check(script != nullptr, "a script with one subscription");
        if (script) {
            MockAbi::ForceFailure(WOTBMOD_V3_E_PERMISSION_DENIED);
            std::memset(message, 0, sizeof(message));
            Check(eval(script,
                       "local ok = wotb.events.unsubscribe(sub)\n"
                       "if ok ~= nil then error('a refused unsubscribe "
                       "reported success') end",
                       message, sizeof(message)) == 0u,
                  "the client refuses to unsubscribe it");
            MockAbi::ClearFailure();
            Check(MockAbi::LiveSubscriptions() == 1u && owned_count(script) == 1u,
                  "the client still holds it, and so does the ledger");
            destroy_script(script);
            script = nullptr;
            Check(MockAbi::CountCalled(MockAbi::events_calls,
                                       "events.unsubscribe(9000)") == 2u,
                  "teardown retried it - the registry's own sweep, which is "
                  "the only thing left that could, since this host's record "
                  "was already retired when the script asked");
            Check(MockAbi::LiveSubscriptions() == 0u,
                  "and the client is left holding nothing after all");
            Check(host_records() == 0u,
                  "with no host record left either");
        }

        // ---- the teardown window: a handler that creates while its own
        // ---- script is being destroyed ------------------------------------
        //
        // The defect this registry exists to make unreachable rather than
        // fixed. Teardown is not instantaneous: it marks a script's
        // subscriptions dead and then waits for deliveries already in flight.
        // A handler running in that window is ordinary Lua code and can call
        // subscribe, control_create and begin_transaction - and before the
        // ledger's closed gate, anything it made was made for a script whose
        // teardown pass had already gone by. For a subscription that meant the
        // client kept a callback into a LuaScript that was about to be freed.
        //
        // The window is reachable without a line of instrumentation in the
        // host, exactly as the existing teardown-race test reaches it: the
        // handler's own get_thread lands back in the mock while the delivery
        // holds the script lock, so blocking there parks it precisely where
        // teardown has to wait.
        MockAbi::Reset();
        std::memset(message, 0, sizeof(message));
        script = create_script(
            "teardown_late_create",
            "panel = wotb.ui.control_create()\n"
            "wotb.events.subscribe('wotbmod.frame.update', function(e)\n"
            "  wotb.events.get_thread(e.dispatch)\n"
            // The drain, witnessed from inside it. This handler is running
            // while teardown is in progress; the control it pokes was created
            // by its own script before any of this started. It must still be
            // there. A RevokeAll that told the client to stop delivering and
            // then destroyed controls *without waiting for deliveries already
            // in flight* would have destroyed this one by now, and the poke
            // would be refused - which is the whole content of "a callback
            // cannot fire against a control that is already gone".
            //
            // Printed rather than stored in a global: the state is closed
            // moments later, so nothing left in it could be read back.
            "  print('poke: ' .. tostring(wotb.ui.control_set_text(panel, "
            "'still here')))\n"
            "  local s, e1 = wotb.events.subscribe('mod.late', function() end)\n"
            "  local c, e2 = wotb.ui.control_create()\n"
            "  local t, e3 = wotb.storage.begin_transaction()\n"
            "  if s ~= nil or c ~= nil or t ~= nil then\n"
            "    print('late create succeeded')\n"
            "  end\n"
            // Reported out through print(), which this host routes at
            // wotbmod.core's log - the only channel a handler running inside
            // a script that is being destroyed still has. The state itself is
            // closed moments later, so nothing left in a global could be read
            // back afterwards.
            "  print('late: ' .. tostring(e1) .. '|' .. tostring(e2) .. "
            "'|' .. tostring(e3))\n"
            "end)",
            message, sizeof(message));
        Check(script != nullptr, "a handler that creates things late");
        if (script) {
            MockAbi::ArmGate();
            std::thread deliver([]() {
                MockAbi::FireEvent("wotbmod.frame.update");
            });
            Check(MockAbi::WaitForGateEntered(),
                  "a delivery parks inside the state for the late-creation "
                  "race");

            std::atomic<bool> destroyed(false);
            void* const doomed = script;
            std::thread teardown([&destroyed, doomed, destroy_script]() {
                destroy_script(doomed);
                destroyed.store(true);
            });
            for (int i = 0; i < 100 && !destroyed.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            Check(!destroyed.load(),
                  "teardown is waiting for the parked delivery, which is the "
                  "window the handler below runs in");

            MockAbi::ReleaseGate();
            teardown.join();
            deliver.join();
            script = nullptr;

            // First that the window was really entered. Without these three
            // the assertions after them would pass just as well against a
            // handler that never ran, which is how a race test most often
            // stops testing anything.
            Check(MockAbi::Called(MockAbi::events_calls,
                                  "events.subscribe(mod.late,0,0)"),
                  "the handler really did reach subscribe inside that window");
            // Counted, not merely seen. This script creates `panel` at top
            // level - it has done since the drain assertion below was added -
            // and a top-level create records the identical string, so
            // Called() here stopped discriminating the moment that line
            // appeared: it was green whether or not the handler in the
            // teardown window ever reached control_create. Two is the top
            // level's one plus the late one, and it fails if the late one
            // never happened.
            Check(MockAbi::CountCalled(MockAbi::ui_calls,
                                       "ui.control_create(0,1)") == 2u,
                  "and control_create - the late one, counted apart from the "
                  "top-level create that records the same string");
            Check(MockAbi::Called(MockAbi::storage_calls,
                                  "storage.begin_transaction()"),
                  "and begin_transaction");

            // The drain, and the half of the ordering rule the reversed-order
            // experiment cannot reach. Unsubscribing before destroying is the
            // easy half and the ordering witness already covers it; a teardown
            // that unsubscribed and then destroyed controls without waiting
            // for deliveries already inside the state would satisfy that
            // witness and still be wrong. This is that property stated
            // directly: a delivery that was in flight when teardown began
            // found the script's own control still alive.
            Check(MockAbi::Called(MockAbi::ui_calls,
                                  "ui.control_set_text(6000,still here)"),
                  "a delivery in flight during teardown reached the ABI with "
                  "the control its script created");
            Check(MockAbi::Called(MockAbi::core_calls, "core.log(2,lua,poke: true)"),
                  "and the client accepted it - teardown waits for in-flight "
                  "deliveries to leave the state before it destroys anything "
                  "they could still be holding, which is what makes 'a "
                  "callback cannot fire against a control that is already "
                  "gone' true");

            // Each late creation was taken straight back, by the slot that
            // made it, before it ever reached the script.
            Check(MockAbi::Called(MockAbi::events_calls,
                                  "events.unsubscribe(9001)"),
                  "the late subscription was unsubscribed at the client "
                  "immediately - not left for a teardown pass that had "
                  "already gone by");
            Check(MockAbi::Called(MockAbi::ui_calls, "ui.control_destroy(6001)"),
                  "the late control was destroyed immediately");
            Check(MockAbi::Called(MockAbi::storage_calls,
                                  "storage.rollback(777)"),
                  "and the late transaction was rolled back immediately");

            Check(MockAbi::LiveSubscriptions() == 0u,
                  "so nothing the handler made in the teardown window survived "
                  "its script - the client is left holding no callback into a "
                  "freed LuaScript");
            Check(MockAbi::LiveControls() == 0u &&
                      MockAbi::LiveTransactions() == 0u,
                  "and neither did the control or the transaction");
            // The half the client's own counters cannot see, and the one that
            // matters most: this host's subscription record holds the
            // LuaScript to re-enter. A record made in the teardown window and
            // never reaped names a script that has just been freed, and no
            // amount of revoking at the client changes that - a client that
            // delivers anyway, because it never got the unsubscribe or
            // ignored it, lands in freed memory.
            Check(host_records() == 0u,
                  "and this host kept no subscription record of its own for a "
                  "script that no longer exists");

            // And the script saw a refusal rather than a handle it could go on
            // using: `nil, message` with each slot's own context, which is
            // what every other refused call in this host answers. Asserted
            // exactly rather than by "it was nil", so that a slot that
            // answered nil for some entirely different reason - an argument it
            // did not like, a client that refused - could not stand in for
            // this one.
            Check(!MockAbi::Called(MockAbi::core_calls,
                                   "core.log(2,lua,late create succeeded)"),
                  "not one of the three handed the script something to hold");
            Check(MockAbi::Called(
                      MockAbi::core_calls,
                      "core.log(2,lua,late: events.subscribe: this script is "
                      "being unloaded|ui.control_create: this script is being "
                      "unloaded|storage.begin_transaction: this script is "
                      "being unloaded)"),
                  "each answered nil and said why, in its own slot's words");
        }
    }

    // =======================================================================
    // Hot reload
    // =======================================================================
    //
    // Three layers, tested separately because they fail for different reasons:
    // the debounce rule (arithmetic, no clock), the watcher (Win32 plumbing,
    // one end-to-end proof), and the host's reload path (the payoff).

    // ---- the debounce rule, with time supplied rather than waited for -------
    //
    // Every time point below is fabricated. That is the point: a debounce test
    // that sleeps is a test of the machine's scheduler as much as of the rule,
    // and on a loaded build agent two writes meant to land 10 ms apart can land
    // 300 ms apart and coalesce nothing. Here "the second write extended the
    // quiet period" is arithmetic and cannot pass or fail for the wrong reason.
    {
        using wotbmod::lua::ChangeQueue;
        using wotbmod::lua::kDefaultDebounce;
        const ChangeQueue::Clock::time_point t0 =
            ChangeQueue::Clock::time_point(std::chrono::milliseconds(0));
        const auto at = [t0](int ms) {
            return t0 + std::chrono::milliseconds(ms);
        };

        Check(kDefaultDebounce == std::chrono::milliseconds(200),
              "the debounce is 200 ms");

        // One write: nothing until the window has elapsed, then exactly one.
        {
            ChangeQueue queue(std::chrono::milliseconds(200));
            queue.Push(L"C:\\dev\\a.lua", t0);
            Check(queue.Take(at(0)).empty(),
                  "a change is not reported the instant it happens - an editor "
                  "that has truncated the file but not written it yet would be "
                  "compiled as an empty file");
            Check(queue.Take(at(199)).empty(),
                  "nor at one millisecond before the window closes");
            const std::vector<std::wstring> ready = queue.Take(at(200));
            Check(ready.size() == 1u && ready[0] == L"C:\\dev\\a.lua",
                  "and exactly one change, naming the file, once the window "
                  "has closed - the boundary is inclusive, so a debounce of "
                  "200 ms means 200 and not 201");
            Check(queue.Take(at(1000)).empty(),
                  "a change already reported is not reported a second time");
            Check(queue.PendingCount() == 0u,
                  "and nothing is left pending behind it");
        }

        // Two writes inside the window: one change, not two. This is the
        // brief's own case, and the assertion that a leading-edge rate limit
        // would fail - that shape reports the first write immediately, which
        // is exactly the half-written file this exists to avoid.
        {
            ChangeQueue queue(std::chrono::milliseconds(200));
            queue.Push(L"C:\\dev\\b.lua", t0);
            queue.Push(L"C:\\dev\\b.lua", at(100));
            Check(queue.PendingCount() == 1u,
                  "two writes to one file are one pending change, not two");
            Check(queue.Take(at(200)).empty(),
                  "and the second write pushed the deadline out - 200 ms after "
                  "the first is no longer enough");
            const std::vector<std::wstring> ready = queue.Take(at(300));
            Check(ready.size() == 1u && ready[0] == L"C:\\dev\\b.lua",
                  "one change, not two, 200 ms after the last write");
        }

        // A save that produces four notifications - truncate, write, size,
        // timestamp - is still one reload.
        {
            ChangeQueue queue(std::chrono::milliseconds(200));
            for (int i = 0; i < 4; ++i) {
                queue.Push(L"C:\\dev\\c.lua", at(i * 3));
            }
            Check(queue.Take(at(500)).size() == 1u,
                  "four notifications from one editor save are one change");
        }

        // Different files do not coalesce with each other.
        {
            ChangeQueue queue(std::chrono::milliseconds(200));
            queue.Push(L"C:\\dev\\d.lua", t0);
            queue.Push(L"C:\\dev\\e.lua", t0);
            Check(queue.Take(at(200)).size() == 2u,
                  "two different files are two changes");
        }

        // One file whose window has closed comes out; another still inside its
        // own window stays. A drain that returned everything pending would
        // report the half-written file this whole mechanism exists to hide.
        {
            ChangeQueue queue(std::chrono::milliseconds(200));
            queue.Push(L"C:\\dev\\f.lua", t0);
            queue.Push(L"C:\\dev\\g.lua", at(150));
            const std::vector<std::wstring> ready = queue.Take(at(200));
            Check(ready.size() == 1u && ready[0] == L"C:\\dev\\f.lua",
                  "a drain reports only the paths whose own window has closed, "
                  "not everything pending");
            Check(queue.PendingCount() == 1u,
                  "and leaves the one still being written where it was");
            Check(queue.Take(at(350)).size() == 1u,
                  "which comes out of the next drain");
        }

        // Windows' filesystem is case-insensitive, so two spellings of one
        // path are one file. Treating them as two would reload the same script
        // twice - and, worse, would let a single edit look like two files
        // changing, which is how a "leak per reload" test can be fooled.
        {
            ChangeQueue queue(std::chrono::milliseconds(200));
            queue.Push(L"C:\\dev\\Panel.lua", t0);
            queue.Push(L"C:\\dev\\panel.LUA", at(10));
            Check(queue.PendingCount() == 1u,
                  "two spellings of one path are one pending change");
            const std::vector<std::wstring> ready = queue.Take(at(400));
            Check(ready.size() == 1u && ready[0] == L"C:\\dev\\Panel.lua",
                  "reported once, spelled as it was first seen");
        }

        // Clear drops the lot: a watcher that is restarted must not deliver an
        // edit that happened while nothing was listening.
        {
            ChangeQueue queue(std::chrono::milliseconds(200));
            queue.Push(L"C:\\dev\\h.lua", t0);
            queue.Clear();
            Check(queue.PendingCount() == 0u && queue.Take(at(400)).empty(),
                  "Clear drops what was pending");
        }

        // A zero debounce still reports, rather than never reporting or
        // reporting twice. It is what the reload tests below would reach for
        // if the interval were not injectable, and a rule with a special case
        // at zero would bite exactly there.
        {
            ChangeQueue queue(std::chrono::milliseconds(0));
            queue.Push(L"C:\\dev\\i.lua", t0);
            Check(queue.Take(t0).size() == 1u,
                  "a zero-millisecond debounce reports immediately");
        }
    }

    // ---- one join, used by everything that turns a folder + a name into a
    //      path ---------------------------------------------------------------
    //
    // This was three inline copies until a review found that one of them
    // disagreed with the other two about a folder that already ends in a
    // separator. The consequence is not cosmetic: the path becomes a key in the
    // host's loaded-script map, FoldPathKey does not collapse `\`, and one file
    // arriving under two spellings is two loaded scripts, of which exactly one
    // is ever unloaded again. So the rule is asserted, not just shared.
    {
        using wotbmod::lua::FoldPathKey;
        using wotbmod::lua::JoinPath;
        Check(JoinPath(L"C:\\dev", L"a.lua") == L"C:\\dev\\a.lua",
              "a folder without a trailing separator gets one");
        Check(JoinPath(L"C:\\dev\\", L"a.lua") == L"C:\\dev\\a.lua",
              "a folder that already ends in one does not get a second - the "
              "bug this function exists to make impossible");
        Check(JoinPath(L"C:/dev/", L"a.lua") == L"C:/dev/a.lua",
              "and a forward slash counts as a separator too, because Windows "
              "accepts it and %WOTBMOD_LUA_DEV_DIR% may well contain it");
        Check(FoldPathKey(JoinPath(L"C:\\dev", L"a.lua")) ==
                  FoldPathKey(JoinPath(L"C:\\dev\\", L"a.lua")),
              "so both spellings of one folder produce one key - which is the "
              "property that keeps one file from becoming two loaded scripts");
        Check(JoinPath(L"", L"a.lua") == L"a.lua" &&
                  JoinPath(L"C:\\dev", L"") == L"C:\\dev",
              "and an empty half joins to the other one rather than to a "
              "dangling separator");
    }

    // ---- the overflow path, which no test can reach through the kernel ------
    //
    // PushFolderContents is what answers a completed read of zero bytes: the
    // notification buffer overflowed and the kernel cannot say what changed, so
    // the whole folder is treated as changed rather than an edit being lost.
    // Making a real overflow happen means thousands of changes inside one read
    // window and a test that passes or fails on how fast a disk is - so the
    // function is a free one, and this drives it directly. Reviewed code that
    // nothing executes is the one place a silent lost-edit bug can live.
    {
        using wotbmod::lua::ChangeQueue;
        using wotbmod::lua::PushFolderContents;
        const std::wstring folder = MakeTempFolder(L"overflow");
        Check(!folder.empty(), "a temp folder for the overflow path");
        if (!folder.empty()) {
            Check(WriteTextFile(folder + L"\\one.lua", "-- 1\n") &&
                      WriteTextFile(folder + L"\\two.lua", "-- 2\n") &&
                      WriteTextFile(folder + L"\\notes.txt", "hello\n"),
                  "three files in it");
            Check(CreateDirectoryW((folder + L"\\sub").c_str(), nullptr) != FALSE,
                  "and a subdirectory");

            ChangeQueue queue(std::chrono::milliseconds(0));
            PushFolderContents(queue, folder);
            std::vector<std::wstring> all = queue.Take(ChangeQueue::Clock::now());
            std::sort(all.begin(), all.end());
            Check(all.size() == 3u,
                  "every file in the folder is queued, and the subdirectory is "
                  "not - a folder of scripts is flat, and a subfolder is a "
                  "place to keep notes");
            Check(all.size() == 3u && all[0] == folder + L"\\notes.txt" &&
                      all[1] == folder + L"\\one.lua" &&
                      all[2] == folder + L"\\two.lua",
                  "as full paths - filtering by extension is the host's job, "
                  "not the queue's");

            // The regression the shared join exists for. This path used to
            // build `folder + L"\\" + name` unconditionally, so a folder given
            // with a trailing separator produced a doubled one here and a
            // single one everywhere else - two keys for one file.
            ChangeQueue trailing(std::chrono::milliseconds(0));
            PushFolderContents(trailing, folder + L"\\");
            std::vector<std::wstring> again =
                trailing.Take(ChangeQueue::Clock::now());
            std::sort(again.begin(), again.end());
            Check(again == all,
                  "and a folder passed with a trailing separator produces the "
                  "identical paths - the overflow path agrees with the "
                  "notification path about what a file is called");

            ChangeQueue missing(std::chrono::milliseconds(0));
            PushFolderContents(missing, folder + L"\\does-not-exist");
            Check(missing.PendingCount() == 0u,
                  "and a folder that is not there queues nothing rather than "
                  "failing");
        }
        RemoveTempFolder(folder);
    }

    // ---- the watcher itself: the plumbing, once, end to end ----------------
    //
    // Everything above is arithmetic; this is the part that can only be shown
    // against a real filesystem. One question: does a write to a watched folder
    // reach the queue at all, with the right full path? The debounce is not
    // re-tested here - it has been, without a clock.
    {
        using wotbmod::lua::Watcher;
        const std::wstring folder = MakeTempFolder(L"watch");
        Check(!folder.empty(), "a temp folder for the watcher test");
        if (!folder.empty()) {
            Watcher watcher;
            Check(!watcher.Running(), "a fresh watcher is not running");
            Check(watcher.Start(folder + L"\\does-not-exist") == false,
                  "starting on a folder that does not exist fails rather than "
                  "throwing or watching nothing silently");

            Check(watcher.Start(folder), "the watcher starts on a real folder");
            Check(watcher.Running(), "and reports itself running");
            Check(watcher.Debounce() == wotbmod::lua::kDefaultDebounce,
                  "and takes the 200 ms debounce unless told otherwise");
            watcher.Stop();

            // 60 ms rather than 200: this test is about the plumbing, and the
            // rule it would otherwise be waiting for has already been proved
            // without a clock.
            Check(watcher.Start(folder, std::chrono::milliseconds(60)),
                  "and restarts with a stated debounce");

            // Written with no pause after Start returned, deliberately. Start
            // spawns a thread and the folder is not actually watched until
            // that thread has issued its first ReadDirectoryChangesW, so a
            // Start that returned as soon as the thread existed would lose
            // every change made in the window between - silently, because
            // there is no notification to miss when the kernel was not
            // watching. That is not a hypothetical: it is what this file did,
            // and it showed up as this very assertion failing on roughly one
            // run in three. Start now waits for the worker to arm.
            const std::wstring path = folder + L"\\one.lua";
            Check(WriteTextFile(path, "-- hello\n"),
                  "a file is written the instant Start returns");

            std::vector<std::wstring> changed;
            for (int i = 0; i < kPumpLimit && changed.empty(); ++i) {
                changed = watcher.TakeChanged();
                if (changed.empty()) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(10));
                }
            }
            Check(changed.size() == 1u,
                  "the write reaches the queue as exactly one change");
            Check(!changed.empty() && changed[0] == path,
                  "as a full path - the folder joined to the name the "
                  "notification carried, which is relative");
            Check(watcher.TakeChanged().empty(),
                  "and draining again reports nothing");

            watcher.Stop();
            Check(!watcher.Running(), "Stop stops it");
            watcher.Stop();
            Check(!watcher.Running(), "and Stop is idempotent");
            Check(WriteTextFile(folder + L"\\two.lua", "-- ignored\n"),
                  "a file written after Stop");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            Check(watcher.TakeChanged().empty(),
                  "is not reported - a stopped watcher is stopped");
        }
        RemoveTempFolder(folder);
    }

    // ---- a worker that ends on its own says so ----------------------------
    //
    // Stop is not the only way a worker leaves. If the folder it is watching is
    // deleted or renamed, its next ReadDirectoryChangesW fails and the thread
    // returns - and `running_` used to be cleared only by Stop, so Running()
    // and the host's debounce accessor built on it went on reporting a live
    // 200 ms watcher for a thread that had already exited. Nothing else in this
    // suite could see that: RunningWatcherThreads() was right, and it is a
    // different counter.
    //
    // The assertion is the agreement between the two, which is meaningful
    // whichever way this platform behaves. If the worker exits, Running() must
    // have gone false with it; if it does not exit, Running() must still be
    // true. A flag that disagrees with the thread it describes fails either
    // way, and neither outcome makes the test silently assert nothing.
    {
        using wotbmod::lua::Watcher;
        const std::wstring folder = MakeTempFolder(L"vanish");
        Check(!folder.empty(), "a temp folder to delete under a watcher");
        if (!folder.empty()) {
            Watcher watcher;
            Check(watcher.Start(folder, std::chrono::milliseconds(10)),
                  "a watcher on a folder that is about to disappear");
            Check(watcher.Running() && wotbmod::lua::RunningWatcherThreads() > 0u,
                  "running, with a worker to match");

            RemoveTempFolder(folder);
            for (int spin = 0;
                 spin < kPumpLimit && wotbmod::lua::RunningWatcherThreads() > 0u;
                 ++spin) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            Check(watcher.Running() ==
                      (wotbmod::lua::RunningWatcherThreads() > 0u),
                  "and after the folder is deleted the flag still agrees with "
                  "the thread - a watcher that reports itself running for a "
                  "worker that has exited is a disable that thinks it has "
                  "something to stop and a debounce accessor that answers for "
                  "nothing");
            Check(watcher.Stop(),
                  "and Stop still succeeds on a worker that ended by itself");
        }
        RemoveTempFolder(folder);
    }

    // ---- the reload path: fifty reloads leak nothing -----------------------
    //
    // The payoff, and the reason the prototype chose Lua at all. Everything
    // before it existed to make this safe.
    //
    // Fifty is not arbitrary. A leak of one control per reload is invisible
    // once and obvious after half an hour of editing: ten panels stacked on
    // the screen and ten live subscriptions firing into dead states, in a
    // player's game rather than in a test. So the assertion is not "nothing
    // leaked at the end" - a host that leaked fifty and swept them up at
    // disable would pass that - but "at no point during the fifty was more
    // than one of anything alive".
    //
    // Six counters, and the reason there are six rather than three is Task 8's
    // experiment 4: a leak test that only asks the mock what the client holds
    // cannot see a host-side leak. Three of these ask the client
    // (LiveSubscriptions/LiveControls/LiveTransactions) and three ask the host
    // (the set of loaded scripts, the sum of their ownership ledgers, and this
    // host's own subscription records, each of which can leak with every
    // client-side counter reading a perfectly innocent zero).
    using SetDevFolderFn = void(WOTBMOD_V3_CALL*)(const wchar_t*, uint32_t);
    using SetLuaModsFolderFn = void(WOTBMOD_V3_CALL*)(const wchar_t*);
    using CountFn = uint32_t(WOTBMOD_V3_CALL*)();
    const auto set_dev_folder = reinterpret_cast<SetDevFolderFn>(
        GetProcAddress(module, "WotbLuaHost_SetDevFolderForTests"));
    const auto set_lua_mods_folder = reinterpret_cast<SetLuaModsFolderFn>(
        GetProcAddress(module, "WotbLuaHost_SetLuaModsFolderForTests"));
    const auto loaded_count = reinterpret_cast<CountFn>(
        GetProcAddress(module, "WotbLuaHost_LoadedScriptCountForTests"));
    const auto loaded_owned = reinterpret_cast<CountFn>(
        GetProcAddress(module, "WotbLuaHost_LoadedOwnedCountForTests"));
    const auto watcher_debounce = reinterpret_cast<CountFn>(
        GetProcAddress(module, "WotbLuaHost_DevWatcherDebounceMsForTests"));
    const auto watcher_threads = reinterpret_cast<CountFn>(
        GetProcAddress(module, "WotbLuaHost_WatcherThreadsForTests"));
    const auto live_scripts = reinterpret_cast<CountFn>(
        GetProcAddress(module, "WotbLuaHost_LiveScriptObjectsForTests"));
    Check(set_dev_folder != nullptr && set_lua_mods_folder != nullptr &&
              loaded_count != nullptr &&
              loaded_owned != nullptr && watcher_debounce != nullptr &&
              watcher_threads != nullptr && live_scripts != nullptr,
          "the hot reload test entry points are exported");

    // ---- installed Lua mods: manifest, entrypoint and immutable cycle -----
    //
    // This is the production caller for FromManifest. The dev-folder tests
    // cannot stand in for it: they deliberately grant the full measured
    // ceiling and watch loose files. An installed script must instead be cut
    // down to its own manifest and must not hot-reload when its package files
    // change underneath a running enable cycle.
    if (set_dev_folder && set_lua_mods_folder && loaded_count &&
        info.on_enable && info.on_disable && info.on_frame) {
        const std::wstring mods_root = MakeTempFolder(L"installed");
        const std::wstring dev_root = MakeTempFolder(L"installed_dev");
        Check(!mods_root.empty() && !dev_root.empty(),
              "temp roots for installed and dev Lua mods");
        if (!mods_root.empty() && !dev_root.empty()) {
            const std::wstring alpha = mods_root + L"\\prod.alpha";
            const std::wstring bad = mods_root + L"\\prod.bad";
            const std::wstring escape = mods_root + L"\\prod.escape";
            Check(CreateDirectoryW(alpha.c_str(), nullptr) != FALSE &&
                      CreateDirectoryW(bad.c_str(), nullptr) != FALSE &&
                      CreateDirectoryW(escape.c_str(), nullptr) != FALSE,
                  "installed mod directories are created");
            Check(WriteTextFile(
                      alpha + L"\\manifest.json",
                      "{\"id\":\"prod.alpha\",\"entrypoint\":\"boot.lua\","
                      "\"permissions\":[\"storage\"]}"),
                  "an installed manifest names its id, entrypoint and grant");
            Check(WriteTextFile(
                      alpha + L"\\boot.lua",
                      "function on_enable()\n"
                      "  local c, err = wotb.ui.control_create()\n"
                      "  if c ~= nil then error('ui escaped manifest') end\n"
                      "  if not string.find(err, 'ui.modify.game') then "
                      "error('wrong refusal') end\n"
                      "  wotb.storage.set_json('installed', 'yes')\n"
                      "end\n"
                      "function on_frame(frame, delta)\n"
                      "  if frame == 991 and delta == 0.125 then\n"
                      "    wotb.storage.set_json('installed_frame', 'yes')\n"
                      "  end\n"
                      "end\n"),
                  "the installed entrypoint is written");

            // A duplicate security-relevant key is not accepted, and its
            // source must never run. This assertion is against the ABI call,
            // not merely the loaded count, so a loader that ran then discarded
            // the script cannot pass it.
            Check(WriteTextFile(
                      bad + L"\\manifest.json",
                      "{\"id\":\"prod.bad\",\"id\":\"prod.bad\","
                      "\"permissions\":[\"storage\"]}"),
                  "a malformed installed manifest is written");
            Check(WriteTextFile(
                      bad + L"\\main.lua",
                      "wotb.storage.set_json('bad_manifest_ran', 'yes')\n"),
                  "and source exists behind the malformed manifest");
            Check(WriteTextFile(
                      escape + L"\\manifest.json",
                      "{\"id\":\"prod.escape\","
                      "\"entrypoint\":\"../outside.lua\","
                      "\"permissions\":[\"storage\"]}"),
                  "a traversal entrypoint manifest is written");
            Check(WriteTextFile(
                      mods_root + L"\\outside.lua",
                      "wotb.storage.set_json('path_escape_ran', 'yes')\n"),
                  "and executable source exists at the escaped path");

            // The dev file deliberately collides with the installed id. The
            // installed mod loads first and owns the id for this cycle; the
            // dev copy must not run into the same storage namespace.
            Check(WriteTextFile(
                      dev_root + L"\\prod.alpha.lua",
                      "wotb.storage.set_json('dev_collision_ran', 'yes')\n"),
                  "a dev script with the installed id is written");

            set_lua_mods_folder(mods_root.c_str());
            set_dev_folder(dev_root.c_str(), 20u);
            MockAbi::Reset();
            info.on_enable(&bootstrap, 1u);

            Check(loaded_count() == 1u,
                  "exactly the valid installed mod is loaded");
            Check(MockAbi::Called(MockAbi::storage_calls,
                                  "storage.set_json(installed,yes)"),
                  "its on_enable reaches the permission it requested");
            Check(MockAbi::ui_calls.empty(),
                  "but its denied UI call never reaches the ABI");
            Check(!MockAbi::CalledContaining(MockAbi::storage_calls,
                                             "bad_manifest_ran") &&
                      !MockAbi::CalledContaining(MockAbi::storage_calls,
                                                 "dev_collision_ran") &&
                      !MockAbi::CalledContaining(MockAbi::storage_calls,
                                                 "path_escape_ran"),
                  "refused manifests, traversal and duplicate dev ids do not run");

            info.on_frame(&bootstrap, 1u, 991u, 0.125);
            Check(MockAbi::Called(MockAbi::storage_calls,
                                  "storage.set_json(installed_frame,yes)"),
                  "an installed script receives the real Lua on_frame lifecycle");

            MockAbi::Reset();
            Check(WriteTextFile(
                      alpha + L"\\boot.lua",
                      "wotb.storage.set_json('installed_reloaded', 'no')\n"),
                  "the installed entrypoint is changed on disk");
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            info.on_frame(&bootstrap, 1u, 992u, 0.125);
            Check(!MockAbi::CalledContaining(MockAbi::storage_calls,
                                             "installed_reloaded"),
                  "but installed package files do not hot-reload mid-cycle");

            info.on_disable(&bootstrap, 1u);
            Check(loaded_count() == 0u,
                  "disable unloads the installed script with the dev set");
            set_lua_mods_folder(nullptr);
            set_dev_folder(nullptr, 0u);
        }
        RemoveTempFolder(mods_root);
        RemoveTempFolder(dev_root);
    }

    if (set_dev_folder && loaded_count && loaded_owned && watcher_debounce &&
        watcher_threads && live_scripts && host_records && info.on_enable &&
        info.on_disable && info.on_frame) {
        Check(watcher_threads() == 0u,
              "no watcher thread is running before the host is enabled");
        // Every script this suite has created so far has been destroyed by
        // hand, so this is a real zero to start counting from - and it is what
        // makes the high-water mark below mean "one script object at a time"
        // rather than "one more than whatever was already lying around".
        Check(live_scripts() == 0u,
              "and no LuaScript object survives the tests that ran before "
              "this one");
        const std::wstring folder = MakeTempFolder(L"reload");
        Check(!folder.empty(), "a temp folder for the reload test");
        if (!folder.empty()) {
            const std::wstring script_path = folder + L"\\hot.lua";
            Check(WriteTextFile(script_path, HotScriptSource(0)),
                  "a script in the dev folder before the host is enabled");

            // 40 ms rather than 200. Fifty reloads at the real interval is ten
            // seconds of a suite doing nothing but sleeping, and the interval
            // itself is already proved above without a clock; what is under
            // test here is the reload, not the wait. The host's own default is
            // asserted separately, two blocks down.
            set_dev_folder(folder.c_str(), 40u);
            MockAbi::Reset();
            info.on_enable(&bootstrap, 1u);

            Check(loaded_count() == 1u,
                  "enable loads the scripts already in the dev folder");
            Check(MockAbi::Called(MockAbi::core_calls,
                                  "core.log(2,lua,enabled 0)"),
                  "and calls on_enable on each - a script that is loaded but "
                  "never enabled is a script that silently does nothing");
            Check(MockAbi::LiveSubscriptions() == 1u &&
                      MockAbi::LiveControls() == 1u &&
                      MockAbi::LiveTransactions() == 1u,
                  "and it holds one of each tracked kind");
            Check(loaded_owned() == 3u,
                  "which the host's own ledger agrees with");
            Check(watcher_threads() == 1u,
                  "and one watcher thread is running, watching the folder");

            info.on_frame(&bootstrap, 1u, 777u, 0.25);
            Check(MockAbi::Called(MockAbi::storage_calls,
                                  "storage.set_json(frame_probe,ok)"),
                  "the native frame lifecycle calls Lua on_frame with the "
                  "exact frame index and delta seconds");

            // ---- reloads happen on the main thread, between frames ---------
            //
            // The rule the whole design turns on, and the one assertion in this
            // file that fails if it is broken. Everything else here drives
            // on_frame in a spin loop with a sleep in it, which means a host
            // that reloaded from the watcher's own thread would satisfy every
            // one of them: the reload would land during the sleep and the loop
            // would see the same result a frame-driven reload produces.
            //
            // So this block removes the pump. It writes an edit, waits ten
            // times the debounce window without calling on_frame once, and
            // requires that *nothing has happened* - which a watcher-thread
            // reload cannot satisfy, because by then it would long since have
            // called lua_close on a state the render thread may be inside.
            // Then one frame, and it happens.
            //
            // The thread witness is the second half, and it is here because a
            // call record cannot carry a thread: ui_calls would read
            // identically either way. MockAbi::last_control_create_thread is
            // the difference written down - it must be zero while no frame has
            // been pumped, and the calling thread's id immediately after one.
            {
                const size_t subs_before = MockAbi::event_subscriptions.size();
                const uint32_t frame_thread = GetCurrentThreadId();
                MockAbi::last_control_create_thread.store(0u);
                Check(WriteTextFile(script_path, HotScriptSource(900)),
                      "an edit is saved");
                std::this_thread::sleep_for(std::chrono::milliseconds(400));

                Check(MockAbi::event_subscriptions.size() == subs_before,
                      "ten debounce windows later, with no frame pumped, the "
                      "edit has not been acted on - the watcher thread queues "
                      "and does nothing else");
                Check(MockAbi::last_control_create_thread.load() == 0u,
                      "and no ABI call has been made on any thread, which is "
                      "the assertion a reload driven from the watcher's own "
                      "thread fails and nothing else in this file does");
                Check(!MockAbi::Called(MockAbi::core_calls,
                                       "core.log(2,lua,enabled 900)"),
                      "and the new generation has not run");

                // The absence check above is the discriminator and it is
                // deliberately a fixed wait: "nothing happened in 400 ms with
                // no frames" is only weakened by waiting longer, never made
                // flaky by it.
                //
                // The positive half must not be a single frame, though. It
                // would be asserting that the notification arrived inside that
                // same 400 ms, and ReadDirectoryChangesW's latency is not
                // bounded by anything - see kPumpLimit, whose value is the
                // result of watching this suite go red on a loaded machine. A
                // notification landing at 401 ms would fail four assertions for
                // a reason that has nothing to do with the code, which is the
                // exact flake this file has already been bitten by once.
                //
                // Pumping loses nothing: the block above has just established
                // that no reload happens without frames, so every frame in this
                // loop is a frame that could not have reloaded anything until
                // the notification arrived.
                for (int spin = 0;
                     spin < kPumpLimit &&
                     !MockAbi::Called(MockAbi::core_calls,
                                      "core.log(2,lua,enabled 900)");
                     ++spin) {
                    info.on_frame(&bootstrap, 1u, 900u, 0.016);
                    if (spin > 0) {
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(5));
                    }
                }

                Check(MockAbi::Called(MockAbi::core_calls,
                                      "core.log(2,lua,enabled 900)"),
                      "the on_frame pump is what performs the reload");
                Check(MockAbi::last_control_create_thread.load() == frame_thread,
                      "on the thread that called on_frame - the main thread, "
                      "between frames, where destroying a lua_State cannot "
                      "race a callback the render thread is inside");
                Check(MockAbi::Called(MockAbi::core_calls,
                                      "core.log(2,lua.host,hot.lua: loaded)"),
                      "and the host says so in its own category, so an author "
                      "can tell 'my script printed this' from 'the host "
                      "reloaded my script'");
                Check(MockAbi::event_subscriptions.size() == subs_before + 1u,
                      "exactly one reload, not one per notification the save "
                      "produced - the debounce, end to end");
                Check(loaded_count() == 1u && loaded_owned() == 3u,
                      "one script, replaced rather than added to");
            }

            size_t max_live_subs = 0u;
            size_t max_live_controls = 0u;
            size_t max_live_txns = 0u;
            uint32_t max_loaded = 0u;
            uint32_t max_owned = 0u;
            uint32_t max_host_records = 0u;
            uint32_t max_live_scripts = 0u;
            int observed = 0;

            for (int generation = 1; generation <= 50; ++generation) {
                const size_t before = MockAbi::event_subscriptions.size();
                if (!WriteTextFile(script_path, HotScriptSource(generation))) {
                    break;
                }
                // The pump. on_frame is the only place a reload may happen, so
                // the test drives it exactly as the client would - once per
                // frame, doing nothing most of the time.
                for (int spin = 0;
                     spin < kPumpLimit &&
                     MockAbi::event_subscriptions.size() == before;
                     ++spin) {
                    info.on_frame(&bootstrap, 1u,
                                  static_cast<uint64_t>(generation), 0.016);
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                if (MockAbi::event_subscriptions.size() == before) break;
                ++observed;

                if (MockAbi::LiveSubscriptions() > max_live_subs) {
                    max_live_subs = MockAbi::LiveSubscriptions();
                }
                if (MockAbi::LiveControls() > max_live_controls) {
                    max_live_controls = MockAbi::LiveControls();
                }
                if (MockAbi::LiveTransactions() > max_live_txns) {
                    max_live_txns = MockAbi::LiveTransactions();
                }
                if (loaded_count() > max_loaded) max_loaded = loaded_count();
                if (loaded_owned() > max_owned) max_owned = loaded_owned();
                if (host_records() > max_host_records) {
                    max_host_records = host_records();
                }
                if (live_scripts() > max_live_scripts) {
                    max_live_scripts = live_scripts();
                }
            }

            char label[256] = {};
            _snprintf_s(label, sizeof(label), _TRUNCATE,
                        "all fifty edits reloaded (observed %d)", observed);
            Check(observed == 50, label);

            // The new script really did replace the old one, rather than the
            // host reporting a reload it did not perform. Generation 50's
            // on_enable ran, and fifty-one scripts subscribed in total.
            Check(MockAbi::Called(MockAbi::core_calls,
                                  "core.log(2,lua,enabled 50)"),
                  "the fiftieth reload ran the fiftieth generation of the "
                  "file, and its on_enable");
            _snprintf_s(label, sizeof(label), _TRUNCATE,
                        "fifty reloads really did create fifty-one "
                        "subscriptions (%zu)",
                        MockAbi::event_subscriptions.size());
            Check(MockAbi::event_subscriptions.size() >= 51u, label);

            // The seven high-water marks, and what they are and are not.
            //
            // Each is sampled fifty times, once after each reload has landed -
            // so what is measured is fifty specific points, not a continuous
            // maximum. A transient spike *inside* a reload - between the moment
            // the new script subscribes and the moment the old one's
            // subscription is revoked, say - is not sampled and would not be
            // seen. That ordering is the subject of Task 8's teardown tests and
            // of the destroy-before-create ordering above; it is not what these
            // counters are for.
            //
            // What they are for is the leak shape that reaches a player: one
            // resource left behind per reload, invisible once and obvious after
            // half an hour of editing. That leak is *cumulative*, so a
            // between-reloads sample catches it on the very first iteration and
            // every one after it - which is why the sampling points are the
            // right ones even though they are points rather than an interval.
            //
            // Equality, not <=, throughout: a host that reloads nothing reads
            // zero on all seven, and zero for the wrong reason must fail as
            // loudly as fifty-one for the right one.
            _snprintf_s(label, sizeof(label), _TRUNCATE,
                        "no subscription accumulated across fifty reloads "
                        "(high water %zu, want 1)", max_live_subs);
            Check(max_live_subs == 1u, label);
            _snprintf_s(label, sizeof(label), _TRUNCATE,
                        "no control accumulated across fifty reloads "
                        "(high water %zu, want 1)", max_live_controls);
            Check(max_live_controls == 1u, label);
            _snprintf_s(label, sizeof(label), _TRUNCATE,
                        "no transaction accumulated across fifty reloads "
                        "(high water %zu, want 1)", max_live_txns);
            Check(max_live_txns == 1u, label);
            _snprintf_s(label, sizeof(label), _TRUNCATE,
                        "the host held one script, not fifty-one, at every "
                        "point (high water %u, want 1)", max_loaded);
            Check(max_loaded == 1u, label);
            _snprintf_s(label, sizeof(label), _TRUNCATE,
                        "and one ledger of three, not fifty ledgers of three "
                        "(high water %u, want 3)", max_owned);
            Check(max_owned == 3u, label);
            _snprintf_s(label, sizeof(label), _TRUNCATE,
                        "and one host-side subscription record, which the "
                        "client's own counters cannot see (high water %u, "
                        "want 1)", max_host_records);
            Check(max_host_records == 1u, label);
            // The seventh, and the only one that watches the script object
            // rather than what the script made. A reload that revoked every
            // resource correctly and then forgot to destroy the LuaScript reads
            // a perfect 1/1/1/1/3/1 on the six above while leaking a lua_State
            // per save - tens of kilobytes each, the largest allocation hot
            // reload churns, and the one kind nothing else here tracks.
            _snprintf_s(label, sizeof(label), _TRUNCATE,
                        "and one LuaScript object, not fifty-one - every "
                        "resource can be revoked and the state still leak "
                        "(high water %u, want 1)", max_live_scripts);
            Check(max_live_scripts == 1u, label);

            // ---- a compile error leaves the old script unloaded ------------
            //
            // Not "leaves the old script running". The old one is already gone
            // by the time the new source is compiled, because taking a script
            // back is what makes room for its replacement; a host that kept the
            // old one alive as a fallback would be running code the author has
            // already deleted, and its resources would be the leak this whole
            // task is about.
            const size_t subs_before_bad = MockAbi::event_subscriptions.size();
            Check(WriteTextFile(script_path, "this is not lua\n"),
                  "a file that does not compile is written");
            for (int spin = 0; spin < kPumpLimit && loaded_count() != 0u; ++spin) {
                info.on_frame(&bootstrap, 1u, 1000u, 0.016);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            Check(loaded_count() == 0u,
                  "a compile error leaves the script unloaded");
            Check(MockAbi::event_subscriptions.size() == subs_before_bad,
                  "and nothing new was created by a script that never ran");
            Check(MockAbi::LiveSubscriptions() == 0u &&
                      MockAbi::LiveControls() == 0u &&
                      MockAbi::LiveTransactions() == 0u,
                  "and everything the previous generation held was taken back "
                  "even though its replacement never loaded");
            Check(host_records() == 0u,
                  "and the host kept no record of the script it unloaded");
            // The seventh counter, in the block it matters most in. A compile
            // failure is the one path where LuaScript::Create itself owns the
            // destruction - it deletes the half-built script and returns null,
            // and the reload path never sees an object to delete. Every other
            // counter here is about what the *previous* generation held; this
            // is the only one that says the failed attempt left no lua_State
            // behind, and a Create that leaked on its failure path would read
            // a perfect zero on all six of the others.
            Check(live_scripts() == 0u,
                  "and no LuaScript object survived the failed compile - the "
                  "path where Create's own failure branch is what has to "
                  "delete it");

            // The message reaches the log, which is the only place a mod
            // author can read it. A hot reload that failed silently is worse
            // than one that failed loudly: the author edits, sees no change,
            // and blames the reload rather than the syntax error.
            bool reported = false;
            for (const std::string& logged : MockAbi::core_calls) {
                if (logged.find("core.log(4,lua.host,") != std::string::npos &&
                    logged.find("hot.lua") != std::string::npos) {
                    reported = true;
                }
            }
            Check(reported,
                  "and the compile error is logged at error level, naming the "
                  "file - a reload that fails silently is worse than one that "
                  "fails loudly");

            // ---- and the game keeps running --------------------------------
            //
            // The whole point of the paragraph above. Fixing the file loads it
            // again; a host that had wedged itself on the bad compile would
            // fail here and nowhere else.
            Check(WriteTextFile(script_path, HotScriptSource(51)),
                  "the file is fixed");
            for (int spin = 0; spin < kPumpLimit && loaded_count() != 1u; ++spin) {
                info.on_frame(&bootstrap, 1u, 1001u, 0.016);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            Check(loaded_count() == 1u,
                  "fixing the file loads it again - the game kept running "
                  "through the error");
            Check(MockAbi::Called(MockAbi::core_calls,
                                  "core.log(2,lua,enabled 51)"),
                  "and the fixed script was enabled");

            // ---- a script whose on_enable raises is not left enabled -------
            //
            // The brief's rule covers a runtime error as well as a compile
            // error, and this is the runtime half that a top-level error()
            // does not reach: the chunk compiles, the chunk runs, and the
            // failure is in on_enable. What it created before it raised must
            // still be taken back.
            Check(WriteTextFile(script_path,
                                "panel = wotb.ui.control_create()\n"
                                "function on_enable() error('no') end\n"),
                  "a script whose on_enable raises is written");
            for (int spin = 0; spin < kPumpLimit && loaded_count() != 0u; ++spin) {
                info.on_frame(&bootstrap, 1u, 1002u, 0.016);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            Check(loaded_count() == 0u,
                  "a script whose on_enable raises is not left enabled");
            Check(MockAbi::LiveControls() == 0u,
                  "and the control it created before raising is taken back - "
                  "the destruction that unloads it is a complete revocation, "
                  "whatever the script did or did not manage to do");
            Check(live_scripts() == 0u,
                  "and the object itself is gone, not merely emptied of what "
                  "it owned");

            // ---- on_disable runs before RevokeAll, with the ledger open ----
            //
            // The two questions the review asked of the on_disable call, since
            // it enlarges the script-facing API and so is a spec-level fact
            // rather than an implementation detail. Both are answered here
            // rather than argued in a comment.
            //
            // One: can a script still reach the ABI from on_disable? It is the
            // only moment an author has to persist state, so "yes" is the whole
            // reason the call exists - and it is true because DestroyScript
            // calls on_disable *before* `delete`, and `delete` is what runs
            // RevokeAll. The storage write below reaching the client is that
            // ordering, observed.
            //
            // Two: is the ledger still open? A script that *creates* something
            // in on_disable is creating it for a script that is one line from
            // destruction - and because RevokeAll has not run, the ledger has
            // not closed, so the create is recorded like any other and revoked
            // like any other. The control below is created in on_disable and
            // must not survive it. (The opposite ordering - revoke, then call
            // on_disable - would leave that control alive forever: the sweep
            // that would have collected it has already gone by. That is the
            // Task 8 defect shape, and this assertion is what stands between
            // this host and re-introducing it through the new call.)
            Check(WriteTextFile(script_path,
                                "function on_disable()\n"
                                "  wotb.storage.set_json('bye', '{\"n\":1}')\n"
                                "  late = wotb.ui.control_create()\n"
                                "end\n"),
                  "a script that writes to storage and creates a control from "
                  "its on_disable");
            for (int spin = 0; spin < kPumpLimit && loaded_count() != 1u; ++spin) {
                info.on_frame(&bootstrap, 1u, 1007u, 0.016);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            Check(loaded_count() == 1u, "loads");
            {
                const size_t controls_before = MockAbi::ui_controls.size();
                info.on_disable(&bootstrap, 1u);
                Check(MockAbi::Called(MockAbi::storage_calls,
                                      "storage.set_json(bye,{\"n\":1})"),
                      "on_disable reaches the ABI - it runs before RevokeAll, "
                      "so a script's last chance to persist its state is a real "
                      "one rather than a call into a closed ledger");
                Check(MockAbi::ui_controls.size() == controls_before + 1u,
                      "and a control created from on_disable really was created "
                      "- the ledger is still open at that point");
                Check(MockAbi::LiveControls() == 0u,
                      "and revoked all the same: what a script makes in its own "
                      "teardown is recorded and taken back like anything else, "
                      "which the opposite ordering could not do");
                Check(loaded_count() == 0u && live_scripts() == 0u &&
                          host_records() == 0u,
                      "and the script is gone");
            }
            // Re-enabled over an empty folder, so the next block's "loads"
            // moves from a real zero to a real one instead of finding the
            // count already at one and asserting nothing.
            Check(DeleteFileW(script_path.c_str()) != FALSE,
                  "that script's file is removed while disabled");
            info.on_enable(&bootstrap, 1u);
            Check(loaded_count() == 0u,
                  "re-enabled over an empty dev folder");

            // ---- on_disable: offered, and never relied on ------------------
            //
            // The other half of a pair. A host that calls on_enable and never
            // on_disable is a trap for an author who wants one moment to write
            // their state to storage before the script goes, and
            // lua_ownership.h already describes scripts that "define on_disable
            // and tidy up their own affairs" - which was not true of any script
            // this host ran until it called one.
            //
            // Two things asserted together, because the whole design is in the
            // gap between them: the courtesy call happens, and it changes
            // nothing about the teardown. The script below raises halfway
            // through its own on_disable, after taking one of its two controls
            // back and before touching the other - so a host that treated a
            // failed on_disable as a reason to stop, or that relied on the
            // script to finish tidying, would leave the second control alive.
            Check(WriteTextFile(script_path,
                                "a = wotb.ui.control_create()\n"
                                "b = wotb.ui.control_create()\n"
                                "sub = wotb.events.subscribe("
                                "'wotbmod.frame.update', function() end)\n"
                                "function on_disable()\n"
                                "  wotb.ui.control_destroy(a)\n"
                                "  error('halfway')\n"
                                "end\n"),
                  "a script whose on_disable raises halfway through");
            for (int spin = 0; spin < kPumpLimit && loaded_count() != 1u; ++spin) {
                info.on_frame(&bootstrap, 1u, 1010u, 0.016);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            Check(loaded_count() == 1u, "loads");
            {
                const size_t controls_before = MockAbi::LiveControls();
                Check(controls_before == 2u, "holding two controls");

                info.on_disable(&bootstrap, 1u);

                Check(MockAbi::CalledContaining(
                          MockAbi::core_calls,
                          "core.log(3,lua.host,hot.lua: on_disable failed:"),
                      "on_disable was called and its failure reported at "
                      "warning - the author's only sign that their cleanup is "
                      "broken");
                Check(MockAbi::LiveControls() == 0u,
                      "and both controls are gone anyway: the one the script "
                      "destroyed itself, and the one it never reached because "
                      "it raised first - teardown does not depend on a script's "
                      "cleanup being correct, or on it finishing");
                Check(MockAbi::LiveSubscriptions() == 0u &&
                          MockAbi::LiveTransactions() == 0u &&
                          host_records() == 0u && loaded_count() == 0u,
                      "and the rest of the ledger closed exactly as it does "
                      "for a script that defined no on_disable at all");
            }

            // ---- deleting the file unloads the script ----------------------
            //
            // Re-enabled from an empty folder, so that the count below moves
            // from a real zero to a real one. Deleting the file first and then
            // enabling is what makes that true: enabling over a folder that
            // still held the previous script would leave the count at one
            // before this section wrote anything, and the assertion would pass
            // without the write having done a thing.
            Check(DeleteFileW(script_path.c_str()) != FALSE,
                  "the previous script's file is removed while disabled");
            info.on_enable(&bootstrap, 1u);
            Check(loaded_count() == 0u, "re-enabled over an empty dev folder");
            Check(WriteTextFile(script_path, HotScriptSource(52)),
                  "a good script is written again");
            for (int spin = 0; spin < kPumpLimit && loaded_count() != 1u; ++spin) {
                info.on_frame(&bootstrap, 1u, 1003u, 0.016);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            Check(loaded_count() == 1u, "and loaded");
            Check(DeleteFileW(script_path.c_str()) != FALSE,
                  "the script file is deleted");
            for (int spin = 0; spin < kPumpLimit && loaded_count() != 0u; ++spin) {
                info.on_frame(&bootstrap, 1u, 1004u, 0.016);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            Check(loaded_count() == 0u,
                  "deleting a script's file unloads the script");
            Check(MockAbi::LiveSubscriptions() == 0u &&
                      MockAbi::LiveControls() == 0u &&
                      MockAbi::LiveTransactions() == 0u,
                  "taking back everything it held");

            // ---- disable takes back the rest -------------------------------
            //
            // Task 8 left on_disable unloading nothing, because the host owned
            // no set of live scripts. It owns one now, and this is that walk.
            Check(WriteTextFile(script_path, HotScriptSource(53)),
                  "one last script, left loaded on purpose");
            for (int spin = 0; spin < kPumpLimit && loaded_count() != 1u; ++spin) {
                info.on_frame(&bootstrap, 1u, 1005u, 0.016);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            Check(loaded_count() == 1u, "loaded");

            info.on_disable(&bootstrap, 1u);
            Check(loaded_count() == 0u,
                  "disable unloads every script the host was holding - the "
                  "walk Task 8 could not write, because the set did not exist");
            Check(loaded_owned() == 0u,
                  "and the host's own ledgers are empty");
            Check(MockAbi::LiveSubscriptions() == 0u &&
                      MockAbi::LiveControls() == 0u &&
                      MockAbi::LiveTransactions() == 0u,
                  "and after fifty-three reloads and a disable the client is "
                  "left holding nothing at all");
            Check(host_records() == 0u,
                  "and this host holds no subscription record of its own, "
                  "which is the half naming a freed LuaScript");
            Check(live_scripts() == 0u,
                  "and not one LuaScript object is left alive after fifty-odd "
                  "reloads and a disable - the counter that would still read "
                  "zero on all the others while a lua_State leaked per save");
            Check(watcher_debounce() == 0u,
                  "and the host is holding no watcher");
            // Asked of the thread, not of the pointer, and the difference is
            // not pedantry: a ShutDown that nulled g_watcher and leaked the
            // Watcher passed every other assertion in this file - the pointer
            // was null, no reload happened, every counter read zero - while a
            // worker went on holding a directory handle inside a DLL the
            // runtime was about to unmap. Stop() joins, so by the time
            // on_disable has returned this is an assertion rather than a poll.
            Check(watcher_threads() == 0u,
                  "and its worker thread is genuinely gone, not merely "
                  "forgotten about - a thread that outlives the disable "
                  "outlives the DLL it is running inside");

            // Frames keep arriving after a disable in this ABI. They must do
            // nothing rather than reload into a host that has been shut down.
            info.on_frame(&bootstrap, 1u, 1006u, 0.016);
            Check(loaded_count() == 0u,
                  "and a frame after disable loads nothing");
        }
        RemoveTempFolder(folder);

        // ---- the host's own default is 200 ms ------------------------------
        //
        // Every reload test above shortened the interval so the suite would
        // finish. This is the one that says what the host actually ships with,
        // and it is here rather than folded into the block above precisely
        // because that block overrides it.
        const std::wstring plain = MakeTempFolder(L"debounce");
        Check(!plain.empty(), "a temp folder for the default-debounce check");
        if (!plain.empty()) {
            set_dev_folder(plain.c_str(), 0u);   // 0 = leave the default alone
            info.on_enable(&bootstrap, 1u);
            Check(watcher_debounce() == 200u,
                  "the host watches its dev folder with the 200 ms debounce, "
                  "not whichever interval the tests above asked for");
            info.on_disable(&bootstrap, 1u);
            Check(watcher_threads() == 0u,
                  "and that one is stopped too - no watcher thread survives "
                  "this suite");
        }
        RemoveTempFolder(plain);
        set_dev_folder(nullptr, 0u);
    }

    // ---- where the dev folder actually is, for a real mod author -----------
    //
    // Every test above reaches the reload path through branch 1 of DevFolder,
    // the test override. Branches 2 and 3 - %WOTBMOD_LUA_DEV_DIR%, and this
    // DLL's own path truncated two levels and joined to `lua-dev` - are the
    // only ones anybody who is not this file will ever take, and until this
    // block they were argued rather than run.
    //
    // What that costs if it is wrong is the whole feature: truncate one level
    // too few and the host watches `mods/<id>/lua-dev`, which does not exist;
    // one too many and it watches the game's own root. Either way there is no
    // folder, no watcher, and one warning in a log nobody reads - while every
    // check above stays green, because all of them override the thing under
    // test. A feature that ships silently dead is exactly what a suite of 400
    // passing checks is supposed to make impossible.
    using ResolveDevFolderFn = uint32_t(WOTBMOD_V3_CALL*)(wchar_t*, uint32_t);
    const auto resolve_dev_folder = reinterpret_cast<ResolveDevFolderFn>(
        GetProcAddress(module, "WotbLuaHost_ResolveDevFolderForTests"));
    Check(resolve_dev_folder != nullptr, "the dev-folder resolver is exported");
    if (resolve_dev_folder && set_dev_folder) {
        set_dev_folder(nullptr, 0u);   // branch 1 off, so 2 and 3 are reachable

        // Two calls, sized by the first. The bug this whole block exists to
        // catch is a path silently shortened, so the test must not contain one
        // of its own - a fixed buffer here would fail the long-value case for
        // the test's reason rather than the host's.
        const auto resolved = [resolve_dev_folder]() {
            const uint32_t length = resolve_dev_folder(nullptr, 0u);
            if (length == 0u) return std::wstring();
            std::wstring value(static_cast<size_t>(length) + 1u, L'\0');
            const uint32_t written = resolve_dev_folder(
                &value[0], static_cast<uint32_t>(value.size()));
            if (written != length) return std::wstring();
            value.resize(static_cast<size_t>(length));
            return value;
        };

        // ---- branch 3: no override, no variable ---------------------------
        //
        // The expectation is derived on this side from the same HMODULE this
        // test loaded, so the assertion is arithmetic against arithmetic rather
        // than the host being invited to agree with itself.
        SetEnvironmentVariableW(L"WOTBMOD_LUA_DEV_DIR", nullptr);
        std::wstring expected;
        {
            std::wstring module_path(4096u, L'\0');
            const DWORD written = GetModuleFileNameW(
                module, &module_path[0],
                static_cast<DWORD>(module_path.size()));
            Check(written > 0u &&
                      static_cast<size_t>(written) < module_path.size(),
                  "the test can read the host DLL's own path");
            module_path.resize(static_cast<size_t>(written));
            for (int level = 0; level < 2; ++level) {
                const size_t slash = module_path.find_last_of(L"\\/");
                if (slash == std::wstring::npos) break;
                module_path.resize(slash);
            }
            expected = module_path + L"\\lua-dev";
        }
        const std::wstring fallback = resolved();
        Check(!fallback.empty(),
              "with no override and no variable set, the host still resolves a "
              "dev folder - the branch every real install takes and the only "
              "one no other check in this file reaches");
        const std::wstring suffix = L"\\lua-dev";
        Check(fallback.size() > suffix.size() &&
                  fallback.compare(fallback.size() - suffix.size(),
                                   suffix.size(), suffix) == 0,
              "and it is named lua-dev");
        Check(fallback == expected,
              "and it is this DLL's own directory minus two components, plus "
              "lua-dev: the host installs at mods\\<id>\\, so two levels up is "
              "mods\\ and its sibling is mods\\lua-dev. Off by one level in "
              "either direction is a folder that does not exist");

        // ---- branch 2: the author's escape hatch --------------------------
        const std::wstring named = L"C:\\somewhere\\else\\scripts";
        Check(SetEnvironmentVariableW(L"WOTBMOD_LUA_DEV_DIR",
                                      named.c_str()) != FALSE,
              "the dev-folder variable is set");
        Check(resolved() == named,
              "%WOTBMOD_LUA_DEV_DIR% outranks the module-relative default, and "
              "is used exactly as given");

        // A value past MAX_PATH. GetEnvironmentVariableW does not fail on a
        // short buffer - it reports the size it wants and writes nothing - so
        // reading it into a fixed wchar_t[MAX_PATH] and testing the result made
        // "too long" indistinguishable from "not set". An author whose scripts
        // live under a deep path would have found their own setting doing
        // nothing at all, with nothing logged to say why.
        std::wstring very_long = L"C:\\";
        while (very_long.size() < 400u) very_long += L"long_segment_name\\";
        very_long += L"scripts";
        Check(very_long.size() > MAX_PATH,
              "the long value really is longer than MAX_PATH");
        Check(SetEnvironmentVariableW(L"WOTBMOD_LUA_DEV_DIR",
                                      very_long.c_str()) != FALSE,
              "a dev-folder variable longer than MAX_PATH is set");
        Check(resolved() == very_long,
              "and it is honoured in full rather than silently dropped for "
              "being long - the failure mode where the author's own setting "
              "does nothing and says nothing");

        // Cleared again, so nothing after this block resolves a folder that is
        // not there, and so the default is shown to come back.
        SetEnvironmentVariableW(L"WOTBMOD_LUA_DEV_DIR", nullptr);
        Check(resolved() == expected,
              "and clearing the variable returns to the module-relative "
              "default");

        // Branch 1 outranks both, which every other test in this file has been
        // relying on without once saying so.
        set_dev_folder(L"C:\\override\\wins", 0u);
        Check(resolved() == L"C:\\override\\wins",
              "and the test override outranks both of them");
        set_dev_folder(nullptr, 0u);
    }

    // The installed root comes from core.get_game_directory rather than the
    // DLL path. A packaged native host runs from an extraction cache, so module
    // arithmetic would point at that cache and silently miss <game>\mods\lua.
    // Test the real source independently; override-only loader tests cannot see
    // a package-layout error.
    using ResolveLuaModsFolderFn =
        uint32_t(WOTBMOD_V3_CALL*)(wchar_t*, uint32_t);
    const auto resolve_lua_mods_folder =
        reinterpret_cast<ResolveLuaModsFolderFn>(GetProcAddress(
            module, "WotbLuaHost_ResolveLuaModsFolderForTests"));
    Check(resolve_lua_mods_folder != nullptr,
          "the installed Lua mod folder resolver is exported");
    if (resolve_lua_mods_folder && set_lua_mods_folder) {
        const auto resolved = [resolve_lua_mods_folder]() {
            const uint32_t length = resolve_lua_mods_folder(nullptr, 0u);
            if (length == 0u) return std::wstring();
            std::wstring value(static_cast<size_t>(length) + 1u, L'\0');
            const uint32_t written = resolve_lua_mods_folder(
                &value[0], static_cast<uint32_t>(value.size()));
            if (written != length) return std::wstring();
            value.resize(static_cast<size_t>(length));
            return value;
        };

        set_lua_mods_folder(nullptr);
        SetEnvironmentVariableW(L"WOTBMOD_LUA_MOD_DIR", nullptr);
        const std::wstring expected = L"C:\\mock-game\\mods\\lua";
        Check(resolved() == expected,
              "installed Lua mods resolve from core.get_game_directory, so a "
              "host DLL extracted into package cache still scans game mods");

        const std::wstring named = L"C:\\elsewhere\\installed-lua";
        Check(SetEnvironmentVariableW(L"WOTBMOD_LUA_MOD_DIR", named.c_str()) !=
                  FALSE,
              "the installed Lua mod directory variable is set");
        Check(resolved() == named,
              "%WOTBMOD_LUA_MOD_DIR% overrides the installed root exactly");

        set_lua_mods_folder(L"C:\\override\\installed");
        Check(resolved() == L"C:\\override\\installed",
              "the installed-root test override outranks the environment");
        set_lua_mods_folder(nullptr);
        SetEnvironmentVariableW(L"WOTBMOD_LUA_MOD_DIR", nullptr);
        Check(resolved() == expected,
              "clearing installed-root overrides returns to the package layout");
    }
    // mods.ini [mods] <id>=0 (what `wotbmod disable` writes) keeps an installed
    // folder out of the scan; a missing key, a damaged file, or a value other
    // than 0 all mean enabled, and ids fold like the folder rule.
    using DisabledByIniFn =
        uint32_t(WOTBMOD_V3_CALL*)(const wchar_t*, const wchar_t*);
    const auto disabled_by_ini = reinterpret_cast<DisabledByIniFn>(
        GetProcAddress(module, "WotbLuaHost_DisabledByIniForTests"));
    Check(disabled_by_ini != nullptr, "the mods.ini off-switch probe is exported");
    if (disabled_by_ini) {
        const std::wstring root = MakeTempFolder(L"ini");
        Check(!root.empty(), "a temp folder for the mods.ini off switch");
        if (!root.empty()) {
            Check(disabled_by_ini(root.c_str(), L"my.off") == 0u,
                  "no mods.ini at all means every folder loads");
            Check(WriteTextFile(root + L"\\mods.ini",
                                "; comment\r\n[mods]\r\nmy.off = 0\r\nmy.on=1\r\n"
                                "my.odd=zero\r\n[permissions]\r\nmy.perm=0\r\n"),
                  "mods.ini written");
            Check(disabled_by_ini(root.c_str(), L"my.off") == 1u,
                  "[mods] my.off=0 disables an installed Lua folder");
            Check(disabled_by_ini(root.c_str(), L"MY.OFF") == 1u,
                  "ids fold case like the folder rule, so My.Off is my.off");
            Check(disabled_by_ini(root.c_str(), L"my.on") == 0u,
                  "[mods] my.on=1 keeps it enabled");
            Check(disabled_by_ini(root.c_str(), L"my.absent") == 0u,
                  "a missing key means enabled");
            Check(disabled_by_ini(root.c_str(), L"my.odd") == 0u,
                  "only the literal 0 switches a folder off");
            Check(disabled_by_ini(root.c_str(), L"my.perm") == 0u,
                  "a 0 under [permissions] is a tier, not a switch");
            RemoveTempFolder(root);
        }
    }

    // ---- per-script permissions: the fence --------------------------------
    //
    // Everything above this line proved that a script *can* reach the ABI.
    // This block is the other half: what a particular script is allowed to
    // touch, and that a script which was not granted a family cannot reach it
    // by any route the sandbox leaves open.
    using RunWithPermissionsFn =
        uint32_t(WOTBMOD_V3_CALL*)(const char*, const char*, char*, uint32_t);
    const RunWithPermissionsFn run_with_permissions =
        reinterpret_cast<RunWithPermissionsFn>(GetProcAddress(
            module, "WotbLuaHost_RunScriptWithPermissionsForTests"));
    using ManifestAllowsFn =
        uint32_t(WOTBMOD_V3_CALL*)(const char*, const char*, char*, uint32_t);
    const ManifestAllowsFn manifest_allows =
        reinterpret_cast<ManifestAllowsFn>(
            GetProcAddress(module, "WotbLuaHost_ManifestAllowsForTests"));
    using CreateWithPermissionsFn = void*(WOTBMOD_V3_CALL*)(
        const char*, const char*, const char*, char*, uint32_t);
    const CreateWithPermissionsFn create_with_permissions =
        reinterpret_cast<CreateWithPermissionsFn>(GetProcAddress(
            module, "WotbLuaHost_CreateScriptWithPermissionsForTests"));
    Check(run_with_permissions != nullptr,
          "the permission test entry point is exported");
    Check(manifest_allows != nullptr,
          "and the one that asks a manifest a question by name");

    // wotb.packages, the bridge to wotbmod.exe. Fenced by packages.manage;
    // verbs come from a closed list, arguments from a small alphabet, and the
    // command line is assembled by the bridge. build\packages_stub.exe stands
    // in for the CLI and echoes what it was given, so the test reads the
    // assembled line, the launcher variables, the exit code and stderr back
    // through poll(). WOTBMOD_PACKAGES_EXE is the test-only override.
    if (run_with_permissions) {
        char message[2048] = {};
        Check(run_with_permissions(
                  "{\"permissions\":[\"core\"]}",
                  "local job, err = wotb.packages.run('list', {})\n"
                  "assert(job == nil and err:find('permission denied: "
                  "packages.manage', 1, true), tostring(err))",
                  message, sizeof(message)) == 0u,
              "wotb.packages refuses a script without packages.manage");
        wchar_t host_path[MAX_PATH] = {};
        GetModuleFileNameW(module, host_path, MAX_PATH);
        std::wstring stub = host_path;
        const size_t slash = stub.find_last_of(L"\\/");
        stub = (slash == std::wstring::npos ? std::wstring(L".")
                                            : stub.substr(0, slash)) +
               L"\\packages_stub.exe";
        Check(GetFileAttributesW(stub.c_str()) != INVALID_FILE_ATTRIBUTES,
              "build\\packages_stub.exe was compiled beside the host DLL");
        SetEnvironmentVariableW(L"WOTBMOD_PACKAGES_EXE", stub.c_str());
        std::memset(message, 0, sizeof(message));
        // The ceiling is what the synthetic client granted the host; a script
        // is cut down to it, so packages.manage has to be granted to the host
        // first and the host re-measured.
        MockAbi::SetHostPermissions(
            {"core", "events.public", "storage", "ui.modify.game",
             "ui.create", "ui.modify.own", "battle.ui", "input",
             "packages.manage"});
        Check(entry(&bootstrap, 1u, &info) == WOTBMOD_V3_OK,
              "the host re-measures its ceiling with packages.manage granted");
        const char* bridge_script = R"(
            local p = wotb.packages
            assert(p.run("format", {}) == nil, "an unknown verb is refused")
            assert(p.run("list", {"x"}) == nil, "list takes no argument")
            assert(p.run("uninstall", {}) == nil, "uninstall needs an id")
            assert(p.run("uninstall", {"my mod"}) == nil, "a space is refused")
            assert(p.run("uninstall", {'a"b'}) == nil, "a quote is refused")
            assert(p.run("launcher-open", {"https://evil"}) == nil,
                   "only wotbmod:// links open the launcher")
            local exe, exe_err = p.executable()
            assert(exe, "executable: " .. tostring(exe_err))
            assert(exe:find("packages_stub.exe", 1, true), exe)
            local job, job_err = p.run("list", {})
            assert(job, "run list: " .. tostring(job_err))
            local busy, why = p.run("list", {})
            assert(busy == nil and why == "busy", tostring(why))
            local state = p.poll(job, 5000)
            assert(not state.running, "the stub finishes within the wait")
            assert(state.exit_code == 0, tostring(state.exit_code))
            assert(state.stdout:find("arg:list\r\n", 1, true), state.stdout)
            assert(state.stdout:find("arg:--json\r\n", 1, true), state.stdout)
            assert(state.stdout:find("arg:--game-root\r\n", 1, true), state.stdout)
            assert(not state.stdout:find("arg:--yes", 1, true), state.stdout)
            assert(state.stdout:find("env:WOTBMOD_LAUNCHER_YES=1\r\n", 1, true), state.stdout)
            assert(state.stdout:find("env:WOTBMOD_LAUNCHER_NO_PAUSE=1\r\n", 1, true), state.stdout)
            local gone, gone_why = p.poll(job)
            assert(gone == nil and gone_why == "unknown job", tostring(gone_why))
            local failing = assert(p.run("uninstall", {"fail"}))
            local done = p.poll(failing, 5000)
            assert(done.exit_code == 3, tostring(done.exit_code))
            assert(done.stdout:find("arg:uninstall\r\narg:fail\r\narg:--yes\r\n", 1, true), done.stdout)
            assert(done.stderr:find("told to fail", 1, true), done.stderr)
            local link = assert(p.run("launcher-open",
                {"wotbmod://install/a.b@1.0.0?source=https%3A%2F%2Fblitz-forge.org%2Fapi%2Fv1"}))
            local opened = p.poll(link, 5000)
            assert(opened.exit_code == 0, tostring(opened.exit_code))
            assert(opened.stdout:find("arg:launcher\r\narg:open\r\narg:wotbmod://install/a.b@1.0.0?source=", 1, true), opened.stdout)
            assert(not opened.stdout:find("--game-root", 1, true), "the launcher finds the game itself")
            assert(p.cancel(999) == false, "cancelling an unknown job is false")
            local slow = assert(p.run("list", {}))
            assert(p.cancel(slow) == true, "a live or finished job can be cancelled")
            assert(p.poll(slow) == nil, "and is gone afterwards")
        )";
        const uint32_t bridge_result = run_with_permissions(
            "{\"permissions\":[\"core\",\"packages.manage\"]}",
            bridge_script, message, sizeof(message));
        std::string bridge_label =
            "wotb.packages runs the stub and reports back (code " +
            std::to_string(bridge_result) + "): " + message;
        Check(bridge_result == 0u, bridge_label.c_str());
        SetEnvironmentVariableW(L"WOTBMOD_PACKAGES_EXE", nullptr);
        MockAbi::SetHostPermissions(
            {"core", "events.public", "storage", "ui.modify.game",
             "ui.create", "ui.modify.own", "battle.ui", "input"});
        Check(entry(&bootstrap, 1u, &info) == WOTBMOD_V3_OK,
              "the packages test restores the default permission ceiling");
    }
    Check(create_with_permissions != nullptr,
          "and the one that makes a restricted script that outlives the call");

    if (run_with_permissions) {
        MockAbi::Reset();
        char message[512] = {};

        // `core` is a real permission in the manifest vocabulary, so print()
        // may use the client's core.log only when this script holds it. The
        // diagnostic itself remains available through OutputDebugStringA;
        // what the empty manifest must not reach is the client capability the
        // permission names. This was found in the post-handoff review: the bit
        // existed and was measured, but no call site consulted it.
        Check(run_with_permissions("{\"permissions\":[]}",
                  "print('zero-grant-core-log')",
                  message, sizeof(message)) == 0u,
              "print remains a usable diagnostic for a zero-grant script");
        Check(!MockAbi::CalledContaining(MockAbi::core_calls,
                                         "zero-grant-core-log"),
              "but a script without core cannot write through the client's "
              "core.log capability");

        // Lua 5.4's base library installs warn(). Once enabled with warn('@on')
        // it writes to stderr, the same process-global C-runtime stream print
        // was redirected away from. It is not a host capability and has no
        // safe routing here, so it must not exist in the sandbox at all.
        MockAbi::Reset();
        std::memset(message, 0, sizeof(message));
        Check(run_with_permissions("{\"permissions\":[]}",
                  "if warn ~= nil then error('warn survived the sandbox') end",
                  message, sizeof(message)) == 0u,
              "Lua 5.4 warn is removed with the other process-global base "
              "library escape hatches");

        // A script whose manifest grants only storage must not reach the UI.
        Check(run_with_permissions("{\"permissions\":[\"storage\"]}",
                  "local c, err = wotb.ui.control_create()\n"
                  "if c ~= nil then error('ui reached without a grant') end\n"
                  "if not string.find(err, 'ui.modify.game') then "
                  "error('the message does not name the missing grant') end",
                  message, sizeof(message)) == 0u,
              "the fence refuses UI to a storage-only script and says which grant");

        // The other half of that, and the half a guard which merely returns
        // `nil, "permission denied"` *after* the call would fail: the client
        // was never asked. A refusal that arrives after the ABI has already
        // acted has denied nothing.
        Check(MockAbi::ui_calls.empty(),
              "and not one ui slot was reached on the way to that refusal - "
              "the fence is in front of the ABI call, not behind it");
        // ui_controls, not LiveControls, and the difference was found by
        // opening the gate and watching this assertion stay green.
        //
        // LiveControls() counts what the client *still holds*, and
        // run_with_permissions destroys the script before it returns - so
        // RevokeAll takes back anything the script managed to create, and the
        // count is zero whether the fence refused the call or allowed it. The
        // assertion was true for a reason that had nothing to do with the
        // fence. ui_controls is every control ever created since Reset(), which
        // teardown does not undo, so it can tell "never created" from "created
        // and then cleaned up".
        Check(MockAbi::ui_controls.empty(),
              "so no control was ever created for the script to be holding - "
              "not merely none left over after its teardown");

        // A single member of a multi-permission family is not enough. The
        // outer runtime sees this host's aggregate native handle and would
        // otherwise satisfy the missing UI grants from the host itself.
        MockAbi::Reset();
        std::memset(message, 0, sizeof(message));
        Check(run_with_permissions(
                  "{\"permissions\":[\"ui.modify.game\"]}",
                  "local c, err = wotb.ui.control_create()\n"
                  "if c ~= nil then error('partial ui grant reached ABI') end\n"
                  "if not string.find(err, "
                  "'permission denied: ui.modify.game') then "
                  "error('got ' .. tostring(err)) end",
                  message, sizeof(message)) == 0u,
              "one UI permission cannot borrow the host's other UI grants");
        Check(MockAbi::ui_calls.empty() && MockAbi::ui_controls.empty(),
              "and the partial family grant is refused before the UI ABI");

        // The fence is in front of argument checking too. A refused slot must
        // not read its arguments first: control_set_text(nil, 'x') is a
        // wrong-type argument *and* a permission it does not hold, and the
        // permission is the answer it gets.
        std::memset(message, 0, sizeof(message));
        Check(run_with_permissions("{\"permissions\":[\"storage\"]}",
                  "local ok, err = wotb.ui.control_set_text(nil, 'x')\n"
                  "if ok ~= nil then error('reached') end\n"
                  "if not string.find(err, 'permission denied: ui.modify.game')"
                  " then error('got ' .. tostring(err)) end",
                  message, sizeof(message)) == 0u,
              "a refused slot answers with the permission, not with an "
              "argument error - nothing about the call is even looked at");

        // Storage is what this manifest did ask for, and it works: the fence
        // is a fence, not an off switch.
        MockAbi::Reset();
        std::memset(message, 0, sizeof(message));
        Check(run_with_permissions("{\"permissions\":[\"storage\"]}",
                  "wotb.storage.set_json('k', '1')",
                  message, sizeof(message)) == 0u,
              "the same script reaches the family its manifest did grant");
        Check(MockAbi::Called(MockAbi::storage_calls, "storage.set_json(k,1)"),
              "and that one really reached the ABI");

        // Events, the third family, refused by name.
        MockAbi::Reset();
        std::memset(message, 0, sizeof(message));
        Check(run_with_permissions("{\"permissions\":[\"storage\"]}",
                  "local s, err = wotb.events.subscribe('x', function() end)\n"
                  "if s ~= nil then error('subscribed without a grant') end\n"
                  "if not string.find(err, 'permission denied: events.public')"
                  " then error('got ' .. tostring(err)) end",
                  message, sizeof(message)) == 0u,
              "events is refused by name too, and refused at subscribe - "
              "which is what stops a delivery arriving later on a thread "
              "nothing here controls");
        // Same correction as the ui case above, for the same reason: the script
        // is already destroyed by the time this runs, so LiveSubscriptions()
        // reads zero whether the fence refused the subscribe or the teardown
        // undid it. event_subscriptions records every subscribe since Reset()
        // and is not undone by revocation.
        Check(MockAbi::event_subscriptions.empty(),
              "and no subscription was ever made - the client was never asked, "
              "rather than asked and then told to forget");
        Check(MockAbi::events_calls.empty(),
              "and not one events slot was reached on the way to that refusal");

        // The tables and their names exist whatever the manifest says. A
        // script written against the dev folder has to run unchanged once it
        // is distributed, so a missing grant may change the answer and never
        // the shape: `wotb.ui.control_create` has to be a function that
        // refuses, not a nil that raises "attempt to call a nil value".
        std::memset(message, 0, sizeof(message));
        Check(run_with_permissions("{\"permissions\":[]}",
                  "if type(wotb.ui) ~= 'table' then error('no ui table') end\n"
                  "if type(wotb.storage) ~= 'table' then error('no storage') end\n"
                  "if type(wotb.events) ~= 'table' then error('no events') end\n"
                  "if type(wotb.ui.control_create) ~= 'function' then "
                  "error('control_create is not a function') end\n"
                  "if wotb.ui.CONTROL_TEXT == nil then error('no constants') end",
                  message, sizeof(message)) == 0u,
              "a script granted nothing sees exactly the same API as one "
              "granted everything - same tables, same functions, same "
              "constants, different answers");

        // ---- no way round the fence -----------------------------------
        //
        // The routes a hostile script would actually try, one script each.

        // 1. A value captured before the fence could act. There is no such
        //    moment - the guard is what was installed, so the binding itself
        //    is not a value in this state at all - but the shape has to be
        //    tested rather than argued: hold the function in a local, in a
        //    table, in an upvalue, call it repeatedly, and it refuses every
        //    time.
        MockAbi::Reset();
        std::memset(message, 0, sizeof(message));
        Check(run_with_permissions("{\"permissions\":[\"storage\"]}",
                  "local f = wotb.ui.control_create\n"
                  "local t = {f = f}\n"
                  "local function closed() return f() end\n"
                  "for i = 1, 3 do\n"
                  "  if f() ~= nil then error('local alias reached') end\n"
                  "  if t.f() ~= nil then error('table alias reached') end\n"
                  "  if closed() ~= nil then error('upvalue alias reached') end\n"
                  "end",
                  message, sizeof(message)) == 0u,
              "aliasing a refused binding into a local, a table and an "
              "upvalue and calling it nine times refuses nine times - there "
              "is no unguarded reference to capture");
        Check(MockAbi::ui_calls.empty(),
              "and none of those nine reached the ABI");

        // 2. The metatable route. wotb and its sub-tables are plain tables
        //    with no metatable, so there is nothing to read a hidden field
        //    out of; and the one metatable this host does own - the handle
        //    box's - answers a string rather than itself, so a script cannot
        //    reach into it either.
        std::memset(message, 0, sizeof(message));
        Check(run_with_permissions(nullptr,
                  "if getmetatable(wotb) ~= nil then error('wotb has one') end\n"
                  "if getmetatable(wotb.ui) ~= nil then error('ui has one') end\n"
                  "if getmetatable(wotb.storage) ~= nil then "
                  "error('storage has one') end\n"
                  "if getmetatable(wotb.events) ~= nil then "
                  "error('events has one') end\n"
                  "local t = wotb.storage.begin_transaction()\n"
                  "if type(getmetatable(t)) ~= 'string' then "
                  "error('a handle hands out its metatable') end\n"
                  "wotb.storage.rollback(t)",
                  message, sizeof(message)) == 0u,
              "no table this host installs carries a metatable, and a handle "
              "answers __metatable rather than the table - so there is no "
              "second way to reach a binding or the value behind a handle");

        // 3. string.dump, the one remaining way to get at a function's
        //    innards from inside the sandbox. It cannot dump a C function,
        //    which every one of these is.
        std::memset(message, 0, sizeof(message));
        Check(run_with_permissions("{\"permissions\":[\"storage\"]}",
                  "local ok = pcall(string.dump, wotb.ui.control_create)\n"
                  "if ok then error('a binding could be dumped') end",
                  message, sizeof(message)) == 0u,
              "string.dump cannot open a bound slot, so the guard's upvalues "
              "- the script it belongs to and the permission it wants - are "
              "not reachable from script code");

        // 4. Replacing the table only breaks the script's own access. It is
        //    worth an assertion because the opposite - a script that could
        //    reinstall wotb.ui from somewhere and get an unguarded one - is
        //    what would make the fence cosmetic.
        std::memset(message, 0, sizeof(message));
        Check(run_with_permissions("{\"permissions\":[\"storage\"]}",
                  "wotb.ui = {}\n"
                  "if next(wotb.ui) ~= nil then error('not empty') end\n"
                  "local ok = pcall(function() return wotb.ui.control_create() end)\n"
                  "if ok then error('a replaced table produced a binding') end",
                  message, sizeof(message)) == 0u,
              "overwriting wotb.ui gets a script an empty table, not an "
              "unguarded one - there is nowhere else a binding lives");
    }

    // ---- what a manifest may say, and what this host refuses to read ------
    //
    // The scanner decides what a stranger's script may touch, so every one of
    // these is a security assertion rather than a parser nicety. A permissive
    // reading here is a grant nobody wrote.
    if (manifest_allows) {
        char error[512] = {};
        // Shorthand: does this manifest grant this permission? 0 no, 1 yes,
        // 2 the manifest was refused.
        const auto asks = [&](const char* json, const char* permission) {
            std::memset(error, 0, sizeof(error));
            return manifest_allows(json, permission, error, sizeof(error));
        };

        Check(asks("{\"permissions\":[\"storage\"]}", "storage") == 1u,
              "the simplest manifest grants what it names");
        Check(asks("{\"permissions\":[\"storage\"]}", "ui.modify.game") == 0u,
              "and nothing it does not");

        // Absent key: nothing, and not an error. A script that asks for
        // nothing gets nothing, which is a perfectly well formed request.
        Check(asks("{\"name\":\"m\"}", "storage") == 0u,
              "a manifest with no permissions key grants nothing");
        Check(std::strlen(error) == 0u, "and says nothing was wrong with it");
        Check(asks("{}", "storage") == 0u,
              "and so does an empty object");

        // Present but not an array.
        Check(asks("{\"permissions\":\"storage\"}", "storage") == 2u,
              "a permissions key that is not an array is refused");
        Check(std::strlen(error) > 0u, "and the refusal carries a message");
        Check(asks("{\"permissions\":null}", "storage") == 2u,
              "null is refused too, rather than read as an empty list");
        Check(asks("{\"permissions\":{\"storage\":true}}", "storage") == 2u,
              "and an object where the array belongs");
        Check(asks("{\"permissions\":[\"storage\", 7]}", "storage") == 2u,
              "an entry that is not a string is refused, and refused whole - "
              "not read up to the entry it could not understand");

        // The literal text `permissions` in someone else's value.
        Check(asks("{\"note\":\"permissions\",\"permissions\":[\"storage\"]}",
                   "storage") == 1u,
              "the word permissions inside another value does not confuse the "
              "real key");
        Check(asks("{\"note\":\"\\\"permissions\\\":[\\\"ui.modify.game\\\"]\"}",
                   "ui.modify.game") == 0u,
              "and a whole permissions clause written inside a string value "
              "grants nothing - this reads the document, it does not search "
              "it for a word");
        Check(asks("{\"inner\":{\"permissions\":[\"ui.modify.game\"]}}",
                   "ui.modify.game") == 0u,
              "nor does one nested a level down: only the top-level key is "
              "the manifest's request");

        // An escaped quote inside a permission string.
        Check(asks("{\"permissions\":[\"stor\\\"age\",\"storage\"]}",
                   "storage") == 1u,
              "an escaped quote inside a permission name is part of the name "
              "rather than the end of it, so the entry after it is still read");
        Check(asks("{\"permissions\":[\"stor\\\"age\"]}", "storage") == 0u,
              "and the name it makes is not storage");

        // A \u escape in a name is scanned, never decoded, and refused.
        //
        // Refused rather than dropped, and the difference is the whole point.
        // Declining to work out what a value means and then carrying on as if
        // the entry were not there is a guess wearing a refusal's clothes: the
        // author gets a script with no permissions and no explanation. This
        // scanner's stated rule is that every doubt resolves to a refusal, and
        // a name it would not decode is exactly a doubt.
        Check(asks("{\"permissions\":[\"\\u0073torage\"]}", "storage") == 2u,
              "a permission name written with a \\u escape is refused, not "
              "quietly dropped - an obfuscated spelling of a grant is not a "
              "grant, and silently granting less than a manifest asked for "
              "with no message is how an author loses an afternoon");
        Check(std::strlen(error) > 0u,
              "and the refusal says so, which is the half that was missing");
        Check(asks("{\"title\":\"caf\\u00e9\",\"permissions\":[\"storage\"]}",
                   "storage") == 1u,
              "but an escape in some other field costs nothing: this scanner "
              "reads what it needs and only skips the rest");
        Check(asks("{\"caf\\u00e9\":1,\"permissions\":[\"storage\"]}",
                   "storage") == 1u,
              "and an escape in another field's *key* costs nothing either - "
              "a manifest may carry keys this host never heard of");

        // The other reason a name might not be in the buffer exactly, and it
        // must not take the refusal path: a name too long to keep provably is
        // not one of the four this host knows (all under fifteen characters),
        // so it is an unknown name, and an unknown name is not an error.
        {
            std::string overlong = "{\"permissions\":[\"";
            overlong.append(200u, 'z');
            overlong += "\",\"storage\"]}";
            Check(asks(overlong.c_str(), "storage") == 1u,
                  "a permission name too long for this scanner to keep is an "
                  "unknown name rather than a malformed manifest, so the real "
                  "grant beside it still lands");
            Check(std::strlen(error) == 0u,
                  "and nothing is reported as wrong with it - 'could not keep "
                  "it' and 'would not decode it' are different facts and only "
                  "one of them is a doubt");
        }

        // Malformed shapes: a refusal, never a partial set and never a crash.
        Check(asks("{\"permissions\":[\"storage\",]}", "storage") == 2u,
              "a trailing comma is refused");
        Check(asks("{\"permissions\":[\"storage\"}", "storage") == 2u,
              "an unterminated array is refused");
        Check(asks("{\"permissions\":[\"storage]}", "storage") == 2u,
              "an unterminated string is refused");
        Check(asks("{\"permissions\":[\"storage\"],}", "storage") == 2u,
              "a trailing comma in the object is refused - and the grant that "
              "had already been read is dropped rather than kept");
        Check(asks("{\"permissions\":[\"storage\"]} junk", "storage") == 2u,
              "text after the object is refused");
        Check(asks("{\"permissions\":[\"storage\"],\"permissions\":[]}",
                   "storage") == 2u,
              "and a manifest that names permissions twice is refused rather "
              "than one of the two being picked");
        Check(asks("", "storage") == 2u, "an empty manifest is refused");
        Check(asks("[\"storage\"]", "storage") == 2u,
              "a manifest that is not an object is refused");
        {
            // Deep nesting is a stack overflow if it is recursed rather than
            // bounded, and a stack overflow in this process is a dead game,
            // not an exception. Deeper than the bound, so this is the refusal
            // rather than the arithmetic.
            std::string deep = "{\"a\":";
            for (int i = 0; i < 200; ++i) deep += "[";
            for (int i = 0; i < 200; ++i) deep += "]";
            deep += ",\"permissions\":[\"storage\"]}";
            Check(asks(deep.c_str(), "storage") == 2u,
                  "a manifest nested two hundred deep is refused rather than "
                  "recursed into a stack overflow");
        }

        // Known names outside the measured ceiling are not errors. They
        // simply intersect to nothing, which is the whole rule.
        Check(asks("{\"permissions\":[\"render.native\"]}",
                   "render.native") == 0u,
              "a known permission outside this host's measured ceiling is "
              "not an error");
        Check(std::strlen(error) == 0u,
              "and is not reported as a malformed manifest either");
        Check(asks("{\"permissions\":[\"unsafe_native\",\"hooks.symbol\","
                   "\"storage\"]}", "storage") == 1u,
              "a manifest asking for things well outside this host's ceiling "
              "still gets the one thing that is inside it");
        Check(asks("{\"permissions\":[\"unsafe_native\",\"hooks.symbol\","
                   "\"storage\"]}", "unsafe_native") == 0u,
              "and gets none of the rest: a manifest is a request, and the "
              "host cannot pass on what it does not hold itself");
    }

    // ---- the ceiling is measured, not written down ------------------------
    if (manifest_allows) {
        char error[512] = {};
        const auto ceiling_allows = [&](const char* permission) {
            std::memset(error, 0, sizeof(error));
            return manifest_allows(nullptr, permission, error, sizeof(error));
        };

        MockAbi::Reset();
        Check(entry(&bootstrap, 1u, &info) == WOTBMOD_V3_OK,
              "the host re-reads its own grants when it is loaded");
        for (const char* granted : {"core", "events.public", "storage",
                                    "ui.modify.game", "ui.create",
                                    "ui.modify.own", "battle.ui"}) {
            Check(ceiling_allows(granted) == 1u, granted);
        }
        Check(ceiling_allows("unsafe_native") == 0u,
              "and the ceiling holds none of the three the spec says this "
              "host must not ask for");
        Check(ceiling_allows("hooks.symbol") == 0u, "hooks.symbol");
        Check(ceiling_allows("render.native") == 0u, "render.native");

        // The native runtime currently defines 53 permission names. The old
        // inner fence represented only four in uint32_t; a generator exposing
        // the remaining interfaces would therefore deny every legitimate
        // manifest before the native outer fence was even consulted. Exercise
        // the complete vocabulary, including bits above 31 and the last bit,
        // so narrowing this back to uint32_t cannot pass silently.
        const char* const all_permission_names[] = {
            "core", "ui", "ui.create", "ui.modify.own", "localization",
            "audio", "audio.custom", "audio.events", "resources",
            "resources.mod", "filesystem.mod_data", "input", "content",
            "hangar.scene", "vehicle.local.cosmetic", "camera.hangar",
            "camera.replay", "network.http.allowlisted", "settings",
            "storage", "input.actions", "events.public",
            "entity.public.visible", "gameplay.tweak.camera",
            "gameplay.tweak.hud", "gameplay.tweak.hangar",
            "gameplay.tweak.replay", "gameplay.tweak.cosmetic",
            "gameplay.tweak.vehicle", "gameplay.tweak.projectile_visual",
            "gameplay.tweak.freecam", "ui.modify.game", "battle.ui",
            "resources.overlay.game", "hooks.symbol", "render.callbacks",
            "battle.render.overlay", "camera.battle.read",
            "visible.projectile.events", "game.entity.public",
            "bigworld.observe", "bigworld.rpc.observe",
            "bigworld.rpc.metadata", "client.leave_to_hangar",
            "network.http", "native.memory", "native.memory_patch",
            "native.hook.address", "native.hooks", "render.native",
            "bigworld.rpc.modify", "ges.observe", "ges.publish",
            "session.cluster.read", "session.cluster.change",
            "packages.manage"};
        static_assert(sizeof(all_permission_names) /
                              sizeof(all_permission_names[0]) ==
                          56u,
                      "permission vocabulary count");
        std::vector<std::string> all_permissions(
            std::begin(all_permission_names), std::end(all_permission_names));
        MockAbi::SetHostPermissions(std::move(all_permissions));
        Check(entry(&bootstrap, 1u, &info) == WOTBMOD_V3_OK,
              "the host measures the complete runtime permission vocabulary");
        for (const char* permission : all_permission_names) {
            Check(ceiling_allows(permission) == 1u, permission);
        }

        // A host granted less gives its scripts less, without anything in the
        // host being edited. This is the assertion a hardcoded ceiling could
        // not make: the names above are what this client happens to
        // grant, not what the host believes about itself.
        MockAbi::SetHostPermissions({"core", "storage"});
        Check(entry(&bootstrap, 1u, &info) == WOTBMOD_V3_OK,
              "the host re-reads them when the client's answer changes");
        Check(ceiling_allows("storage") == 1u,
              "a narrowed ceiling keeps what is still granted");
        Check(ceiling_allows("ui.modify.game") == 0u,
              "and loses what is not - measured off the client rather than "
              "assumed from the spec");
        Check(manifest_allows("{\"permissions\":[\"ui.modify.game\"]}",
                              "ui.modify.game", error, sizeof(error)) == 0u,
              "so a manifest asking for a permission the host itself no "
              "longer holds gets nothing: the intersection is with what is "
              "real, not with what was written down");

        // "Listed" is not "granted". The ABI carries a state per entry for
        // exactly this reason, and a host that read the names without it
        // would hand a script something the runtime refused the host.
        MockAbi::SetHostPermissions({"storage"});
        MockAbi::host_permissions_pending = {"ui.modify.game"};
        Check(entry(&bootstrap, 1u, &info) == WOTBMOD_V3_OK,
              "the host re-reads a list that has an ungranted entry in it");
        Check(ceiling_allows("ui.modify.game") == 0u,
              "a permission the client lists as still needing review is not "
              "in the ceiling - the entry's state is read, not just its name");
        Check(ceiling_allows("storage") == 1u,
              "while the granted one beside it still is");
        MockAbi::host_permissions_pending.clear();

        // A client that offers the interface and then cannot answer it. The
        // ceiling was not measured, so nothing is granted and the host says so
        // - the same branch as a missing interface, reached a different way,
        // and worth its own assertion because "the interface was there" is
        // exactly the reason a reader would assume this path could not be
        // taken.
        MockAbi::Reset();
        MockAbi::permissions_get_count_fails = true;
        Check(entry(&bootstrap, 1u, &info) == WOTBMOD_V3_OK,
              "a client whose permissions interface cannot count does not stop "
              "the host loading");
        Check(ceiling_allows("ui.modify.game") == 0u &&
                  ceiling_allows("storage") == 0u &&
                  ceiling_allows("events.public") == 0u &&
                  ceiling_allows("core") == 0u,
              "and grants nothing at all - a ceiling that could not be read is "
              "not a reason to assume the maximum");
        // At *load* time, and what it says there is the point. On a conforming
        // runtime the mod is not granted anything until on_enable, so
        // wotbmod.permissions may quite reasonably refuse this query - and an
        // error-level "no script will be loaded" here would be a false alarm
        // printed on every single load of a perfectly good client, before the
        // only stage that can decide has run.
        Check(MockAbi::CalledContaining(
                  MockAbi::core_calls,
                  "core.log(1,lua.host,wotbmod.permissions is not answering "
                  "yet at load time"),
              "a load-time measurement that fails says so calmly and at debug "
              "level, because at load time it has not failed at anything yet");
        Check(!MockAbi::CalledContaining(MockAbi::core_calls,
                                         "no script will be loaded"),
              "and does not cry wolf: the refusal belongs to on_enable, which "
              "is the only stage that can know the answer");
        MockAbi::permissions_get_count_fails = false;

        // One entry that cannot be read, out of several that can. This is the
        // other direction, and the difference matters: the answer is still a
        // measurement, so the ceiling narrows to what was readable rather than
        // widening to the fallback. Narrowing is the safe direction and it is
        // the one that must actually happen.
        MockAbi::Reset();
        MockAbi::SetHostPermissions({"storage", "ui.modify.game"});
        MockAbi::permissions_get_at_fails_at = 1;   // ui.modify.game
        Check(entry(&bootstrap, 1u, &info) == WOTBMOD_V3_OK,
              "a client that cannot read one entry back does not stop the host "
              "loading either");
        Check(ceiling_allows("storage") == 1u,
              "the entries that could be read are in the ceiling");
        Check(ceiling_allows("ui.modify.game") == 0u,
              "and the one that could not is left out rather than assumed - a "
              "half-read list narrows the fence, it does not widen it");
        // Stated against a message the loader actually emits. This used to
        // assert the absence of "does not offer wotbmod.permissions", which
        // appears nowhere in the host - not in lua_host_mod.cpp, not in
        // lua_permissions.cpp, not anywhere. Absent text is absent whatever
        // the code does, so the check could not fail and was not evidence of
        // anything. The two sentences the measurement path can actually
        // produce are "is not answering yet at load time" (a load-time
        // failure, at debug level) and "could not be read" (on_enable giving
        // up); a half-read list must produce neither.
        Check(!MockAbi::CalledContaining(MockAbi::core_calls,
                                         "wotbmod.permissions is not answering"),
              "and this is still a measurement, not a failure to make one: a "
              "client that answered nine questions out of ten has not stopped "
              "answering");
        Check(!MockAbi::CalledContaining(MockAbi::core_calls,
                                         "wotbmod.permissions could not be read"),
              "and nothing gives up over it either");
        MockAbi::permissions_get_at_fails_at = -1;

        // A client that answers and grants this host *nothing*. The whole
        // point of lua_permissions.h's "measured a ceiling of nothing" versus
        // "could not measure a ceiling" distinction, and until now the one
        // case never exercised: SetHostPermissions was called four times and
        // never with an empty list, so both facts were only ever tested where
        // they agreed on the answer. A regression that returned false from
        // MeasureHostCeiling whenever bits == 0 - which reads perfectly
        // sensible, and is one `if` - would brick the host on a zero-grant
        // client and still pass every other check in this suite.
        //
        // The observable difference is not the ceiling, which is empty either
        // way. It is whether a script runs at all.
        MockAbi::Reset();
        MockAbi::SetHostPermissions({});
        Check(entry(&bootstrap, 1u, &info) == WOTBMOD_V3_OK,
              "a client that grants this host nothing does not stop it "
              "loading");
        Check(ceiling_allows("storage") == 0u &&
                  ceiling_allows("core") == 0u &&
                  ceiling_allows("events.public") == 0u &&
                  ceiling_allows("ui.modify.game") == 0u,
              "and the ceiling it measured is empty, which is the correct "
              "answer rather than a failure to get one");
        Check(!MockAbi::CalledContaining(
                  MockAbi::core_calls,
                  "wotbmod.permissions could not be read"),
              "and nothing reports a failed measurement, because there was "
              "none: the client answered, and the answer was nothing");
        if (set_dev_folder && loaded_count && info.on_enable && info.on_disable) {
            const std::wstring nothing = MakeTempFolder(L"zerograntceiling");
            Check(!nothing.empty(), "a temp folder for the zero-grant check");
            if (!nothing.empty()) {
                // No wotb call in it: this asserts that a script *loads*, not
                // that it can do anything, and with an empty ceiling every
                // family would refuse it anyway.
                Check(WriteTextFile(nothing + L"\\hot.lua", "local x = 1 + 1\n"),
                      "a script that asks the host for nothing");
                set_dev_folder(nothing.c_str(), 40u);
                MockAbi::core_calls.clear();
                info.on_enable(&bootstrap, 1u);
                Check(loaded_count() == 1u,
                      "a zero-grant client still gets its scripts run - "
                      "'measured nothing' is a measurement, and the refusal "
                      "belongs only to 'could not measure'");
                Check(!MockAbi::CalledContaining(MockAbi::core_calls,
                                                 "no script will be loaded"),
                      "and on_enable does not give up, which is the branch "
                      "this whole distinction exists to keep apart");
                info.on_disable(&bootstrap, 1u);
            }
            RemoveTempFolder(nothing);
            set_dev_folder(nullptr, 0u);
        }

        // An older client with no permissions interface at all. The one
        // fail-open default this design had, now closed.
        //
        // It used to grant the four names the spec fixes for this host's slice
        // and log that the ceiling was a guess. That was deliberate, tested,
        // and capped by the native outer fence - and it was still the single
        // place where doubt granted instead of denying. A host that silently
        // runs every script at the maximum because it could not read its own
        // tier is the failure nobody ever notices: everything works, and the
        // fence is decoration.
        MockAbi::Reset();
        MockAbi::permissions_interface_available = false;
        Check(entry(&bootstrap, 1u, &info) == WOTBMOD_V3_OK,
              "a client with no permissions interface does not stop the host "
              "loading");
        Check(ceiling_allows("ui.modify.game") == 0u &&
                  ceiling_allows("storage") == 0u &&
                  ceiling_allows("events.public") == 0u &&
                  ceiling_allows("core") == 0u,
              "but the ceiling is empty rather than the spec's four - honest "
              "refusal beats coverage, and a host that cannot read the ceiling "
              "it enforces does not know what it is guarding");
        // And the refusal is a refusal, not merely an empty grant. A host that
        // loaded every script and then denied it everything would look, from
        // the outside, like a host whose scripts had all been written wrong.
        //
        // This is also where the error-level message belongs, and the only
        // place it is asserted: on_enable is the authoritative measurement, so
        // it is the stage entitled to say the host is giving up.
        if (set_dev_folder && loaded_count && info.on_enable && info.on_disable) {
            const std::wstring denied = MakeTempFolder(L"noceiling");
            Check(!denied.empty(), "a temp folder for the refusal check");
            if (!denied.empty()) {
                Check(WriteTextFile(denied + L"\\hot.lua",
                                    "wotb.storage.flush()\n"),
                      "a script the host would ordinarily load");
                set_dev_folder(denied.c_str(), 40u);
                MockAbi::core_calls.clear();
                info.on_enable(&bootstrap, 1u);
                Check(loaded_count() == 0u,
                      "no script is loaded at all when the ceiling could not "
                      "be measured - the host refuses to run code rather than "
                      "running it at a privilege level nobody measured");
                Check(MockAbi::CalledContaining(
                          MockAbi::core_calls,
                          "core.log(4,lua.host,wotbmod.permissions could not "
                          "be read"),
                      "and on_enable - the stage that is entitled to give up - "
                      "says why at error level, naming the interface it could "
                      "not read: the one message that turns a silently inert "
                      "host into a bug report rather than a mystery");
                Check(watcher_debounce() == 0u,
                      "and no watcher is started either, so a later save "
                      "cannot sneak one in through on_frame");
                info.on_frame(&bootstrap, 1u, 9000u, 0.016);
                Check(loaded_count() == 0u,
                      "and frames keep arriving to nothing, which is the state "
                      "an author can see and report");
                info.on_disable(&bootstrap, 1u);
            }
            RemoveTempFolder(denied);
            set_dev_folder(nullptr, 0u);
        }

        // Back to a client that answers, for everything after this point.
        MockAbi::Reset();
        Check(entry(&bootstrap, 1u, &info) == WOTBMOD_V3_OK,
              "and the measured ceiling comes back");
        Check(ceiling_allows("ui.modify.game") == 1u,
              "with ui.modify.game in it again");
    }

    // ---- a delivery on another thread is fenced too ------------------------
    //
    // The one route into a script that this host does not initiate. A handler
    // runs on whatever thread the client dispatched on, long after the call
    // that subscribed returned, so a fence that only held on the main thread
    // would hold nowhere that mattered.
    if (create_with_permissions && manifest_allows) {
        MockAbi::Reset();
        char message[512] = {};
        void* script = create_with_permissions(
            "{\"permissions\":[\"events.public\"]}", "fenced_handler",
            "denied = nil\n"
            "calls = 0\n"
            "wotb.events.subscribe('wotbmod.frame.update', function()\n"
            "  calls = calls + 1\n"
            "  local c, err = wotb.ui.control_create()\n"
            "  if c ~= nil then denied = 'reached' else denied = err end\n"
            "end)",
            message, sizeof(message));
        Check(script != nullptr,
              "a script granted events but not ui subscribes successfully");
        if (script) {
            std::thread other([]() {
                MockAbi::event_thread_role =
                    static_cast<uint32_t>(WOTBMOD_V3_THREAD_RENDER);
                MockAbi::FireEvent("wotbmod.frame.update");
            });
            other.join();

            Check(eval(script,
                       "if calls ~= 1 then error('calls ' .. calls) end\n"
                       "if denied == 'reached' then "
                       "error('ui reached from a callback') end\n"
                       "if not string.find(denied, "
                       "'permission denied: ui.modify.game') then "
                       "error('got ' .. tostring(denied)) end",
                       message, sizeof(message)) == 0u,
                  "a handler delivered on another thread is refused the same "
                  "family, with the same message - the fence is a property of "
                  "the script, not of the thread that happens to be asking");
            Check(MockAbi::LiveControls() == 0u,
                  "and no control was created behind it");
            destroy_script(script);
        }

        // The mirror image, so the assertion above is not passing because
        // callbacks are simply broken: the same delivery, from a script that
        // does hold ui.modify.game, does reach the ABI.
        MockAbi::Reset();
        std::memset(message, 0, sizeof(message));
        script = create_with_permissions(
            "{\"permissions\":[\"events.public\",\"ui.modify.game\","
            "\"ui.create\",\"ui.modify.own\",\"battle.ui\"]}",
            "granted_handler",
            "wotb.events.subscribe('wotbmod.frame.update', function()\n"
            "  made = wotb.ui.control_create()\n"
            "end)",
            message, sizeof(message));
        Check(script != nullptr, "and one granted both subscribes too");
        if (script) {
            std::thread other([]() {
                MockAbi::FireEvent("wotbmod.frame.update");
            });
            other.join();
            Check(eval(script,
                       "if made == nil then error('no control') end",
                       message, sizeof(message)) == 0u,
                  "the same callback on the same thread does create a control "
                  "when the manifest granted it - so the refusal above is the "
                  "fence and not a broken delivery path");
            Check(MockAbi::LiveControls() == 1u,
                  "and the client really is holding it");
            destroy_script(script);
        }
        MockAbi::Reset();
    }

    // ---- a reload comes back with the same fence --------------------------
    //
    // The worst outcome this task could have is a save that quietly widens or
    // drops a script's permissions, because nothing about the script would
    // look different afterwards. Driven through the real reload path - the
    // watcher, on_frame, ReloadOne - rather than by making two scripts by
    // hand, since it is the reload that has to preserve it.
    //
    // A dev-folder script always gets the ceiling, so the way to make one
    // restricted is to restrict the ceiling: this client grants the host no
    // ui.modify.game, and the script that loads under it must be refused
    // before the reload and after it.
    if (set_dev_folder && loaded_count && info.on_enable && info.on_disable &&
        info.on_frame) {
        // The same pump every reload test in this file uses: a frame, then a
        // short sleep from the second turn on, bounded by kPumpLimit. A reload
        // only ever happens inside on_frame, so spinning here is what gives
        // ReadDirectoryChangesW's unbounded notification latency somewhere to
        // land without the test asserting a deadline it cannot promise.
        const auto pump_frame = [&](int spin) {
            info.on_frame(&bootstrap, 1u, static_cast<uint64_t>(2000 + spin),
                          0.016);
            if (spin > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        };
        const std::wstring folder = MakeTempFolder(L"fence");
        Check(!folder.empty(), "a temp folder for the reload fence test");
        if (!folder.empty()) {
            MockAbi::Reset();
            MockAbi::SetHostPermissions({"core", "events.public", "storage"});
            const std::wstring file = folder + L"\\fenced.lua";
            // Each generation prints what it got, so the log says which run
            // produced which answer rather than the test assuming.
            const auto source = [](int generation) {
                char text[512] = {};
                _snprintf_s(text, sizeof(text), _TRUNCATE,
                            "-- generation %d\n"
                            "local c, err = wotb.ui.control_create()\n"
                            "if c ~= nil then print('gen %d reached ui') else "
                            "print('gen %d refused: ' .. err) end\n"
                            "wotb.storage.set_json('gen', '%d')\n",
                            generation, generation, generation, generation);
                return std::string(text);
            };
            Check(WriteTextFile(file, source(1)), "the fenced script is written");

            set_dev_folder(folder.c_str(), 5u);
            info.on_enable(&bootstrap, 1u);
            for (int i = 0; i < kPumpLimit && loaded_count() == 0u; ++i) {
                pump_frame(i);
            }
            Check(loaded_count() == 1u, "and it loads");
            Check(MockAbi::CalledContaining(
                      MockAbi::core_calls,
                      "gen 1 refused: permission denied: ui.modify.game"),
                  "a dev-folder script under a client that grants this host no "
                  "ui.modify.game is refused ui, and told which grant");
            Check(MockAbi::LiveControls() == 0u, "and creates no control");
            Check(MockAbi::Called(MockAbi::storage_calls,
                                  "storage.set_json(gen,1)"),
                  "while storage, which the ceiling does hold, still works - "
                  "so this is a fence rather than a script that did not run");

            MockAbi::core_calls.clear();
            MockAbi::storage_calls.clear();
            Check(WriteTextFile(file, source(2)), "the file is saved again");
            for (int i = 0; i < kPumpLimit &&
                            !MockAbi::Called(MockAbi::storage_calls,
                                             "storage.set_json(gen,2)");
                 ++i) {
                pump_frame(1000 + i);
            }
            Check(MockAbi::Called(MockAbi::storage_calls,
                                  "storage.set_json(gen,2)"),
                  "and the reload really ran the new generation");
            Check(MockAbi::CalledContaining(
                      MockAbi::core_calls,
                      "gen 2 refused: permission denied: ui.modify.game"),
                  "which comes back with exactly the fence it had - a reload "
                  "that re-derives permissions cannot widen or drop them, and "
                  "a reload that carried them over could not narrow them "
                  "either");
            Check(MockAbi::LiveControls() == 0u,
                  "and still no control, after a save");

            info.on_disable(&bootstrap, 1u);
            Check(loaded_count() == 0u, "and the disable unloads it");
        }
        RemoveTempFolder(folder);
        set_dev_folder(nullptr, 0u);
        MockAbi::Reset();
        Check(entry(&bootstrap, 1u, &info) == WOTBMOD_V3_OK,
              "the full ceiling is restored for anything after this point");
    }

    // ---- a counted array inside a struct is as long as it says it is ------
    //
    // The whole section runs against PresetProbe's client rather than the
    // mock's, and puts it back afterwards. See the notes on PresetProbe above
    // for why the settings interface has to be lent to the host here.
    if (run) {
        char message[512] = {};
        MockAbi::Reset();
        // The mock's default grants do not include "settings", and the fence
        // in front of every settings slot would otherwise refuse these scripts
        // before the reader they are aimed at ever ran - a green test that
        // proved nothing.
        MockAbi::SetHostPermissions({"core", "events.public", "storage",
                                     "settings"});
        bootstrap.query_interface = &PresetProbe::QueryInterface;
        Check(entry(&bootstrap, 1u, &info) == WOTBMOD_V3_OK,
              "the host re-measures its ceiling against a client that does "
              "publish settings");

        // The attack, written the way a script would write it: one value, and
        // a count that says there are sixty-four. Under the reader this
        // replaces it was accepted, and the client walked sixty-four elements
        // of a one-element allocation.
        PresetProbe::Reset();
        Check(run("local ok, err = wotb.settings.register_preset({\n"
                  "  id = 'probe', name = 'probe',\n"
                  "  values = {{key = 'alpha', type = 0}},\n"
                  "  value_count = 64})\n"
                  "if ok ~= nil then error('accepted') end\n"
                  "if not string.find(err, 'value_count', 1, true) then\n"
                  "  error('message was ' .. tostring(err)) end",
                  message, sizeof(message)) == 0u,
              "a preset whose value_count is larger than its values array is "
              "refused, and the message names the field that disagreed");
        Check(PresetProbe::calls == 0u,
              "and the client is never called at all - the refusal happens "
              "while reading the table, before any pointer reaches the ABI");

        // The same attack with no array whatsoever, which is the cheapest way
        // to write it and would have handed the client a null pointer and a
        // count of eight.
        PresetProbe::Reset();
        Check(run("local ok = wotb.settings.register_preset({\n"
                  "  id = 'probe', name = 'probe', value_count = 8})\n"
                  "if ok ~= nil then error('accepted') end",
                  message, sizeof(message)) == 0u,
              "a value_count with no values array at all is refused too");
        Check(PresetProbe::calls == 0u,
              "and that one does not reach the client either");

        // A count past UINT32_MAX cannot be truncated into something the
        // client will believe: it is compared against the array's length as a
        // 64-bit number and disagrees with it.
        PresetProbe::Reset();
        Check(run("local ok = wotb.settings.register_preset({\n"
                  "  id = 'probe', name = 'probe',\n"
                  "  values = {{key = 'alpha', type = 0}},\n"
                  "  value_count = 4294967296})\n"
                  "if ok ~= nil then error('accepted') end",
                  message, sizeof(message)) == 0u,
              "a count too large for the uint32_t it is stored in is refused "
              "rather than truncated into a plausible one");
        Check(PresetProbe::calls == 0u,
              "and it does not reach the client");

        // The honest call, which has to keep working: two values and a count
        // that agrees. The first key proves the elements were really read out
        // of the array - the reader this replaces treated the array table
        // itself as one struct, so every field of it came back empty.
        PresetProbe::Reset();
        Check(run("local ok = wotb.settings.register_preset({\n"
                  "  id = 'probe', name = 'probe',\n"
                  "  values = {{key = 'alpha', type = 0},\n"
                  "            {key = 'beta', type = 0}},\n"
                  "  value_count = 2})\n"
                  "if ok ~= true then error('refused') end",
                  message, sizeof(message)) == 0u,
              "a preset whose value_count agrees with its values array is "
              "accepted");
        Check(PresetProbe::calls == 1u && PresetProbe::last_value_count == 2u,
              "and the client is told there are two values");
        Check(!PresetProbe::values_were_null &&
                  std::strcmp(PresetProbe::first_key, "alpha") == 0,
              "and the first of them is the one the script wrote, so the "
              "array really was read element by element");

        // No count at all is the spelling a script should use, and the only
        // one that cannot be wrong: the length comes from the table.
        PresetProbe::Reset();
        Check(run("local ok = wotb.settings.register_preset({\n"
                  "  id = 'probe', name = 'probe',\n"
                  "  values = {{key = 'alpha', type = 0},\n"
                  "            {key = 'beta', type = 0}}})\n"
                  "if ok ~= true then error('refused') end",
                  message, sizeof(message)) == 0u,
              "a preset with no value_count at all is accepted");
        Check(PresetProbe::calls == 1u && PresetProbe::last_value_count == 2u,
              "and the count the client sees is the length of the table, not "
              "a number the script supplied");

        // An empty array is zero elements and a null pointer, not one empty
        // element: ArenaAllocate(0) is null by construction, and a client that
        // needs at least one value rejects that itself.
        PresetProbe::Reset();
        Check(run("local ok = wotb.settings.register_preset({\n"
                  "  id = 'probe', name = 'probe', values = {},\n"
                  "  value_count = 0})\n"
                  "if ok ~= true then error('refused') end",
                  message, sizeof(message)) == 0u,
              "an empty values array is accepted");
        Check(PresetProbe::calls == 1u && PresetProbe::last_value_count == 0u &&
                  PresetProbe::values_were_null,
              "and reaches the client as no values at all");

        // Anything that is not a table is refused where it is read, rather
        // than becoming a pointer.
        PresetProbe::Reset();
        Check(run("local ok, err = wotb.settings.register_preset({\n"
                  "  id = 'probe', name = 'probe', values = 'alpha'})\n"
                  "if ok ~= nil then error('accepted') end\n"
                  "if not string.find(err, 'array table', 1, true) then\n"
                  "  error('message was ' .. tostring(err)) end",
                  message, sizeof(message)) == 0u,
              "a values field that is not a table is refused and told what "
              "was expected");
        Check(PresetProbe::calls == 0u, "and never reaches the client");

        MockAbi::Install(&bootstrap);
        Check(entry(&bootstrap, 1u, &info) == WOTBMOD_V3_OK,
              "the mock client and the full ceiling are restored for anything "
              "after this point");
    }

    std::printf("Lua host: %u passed, %u failed\n",
                g_checks - g_failures, g_failures);
    return g_failures == 0u ? 0 : 1;
}
