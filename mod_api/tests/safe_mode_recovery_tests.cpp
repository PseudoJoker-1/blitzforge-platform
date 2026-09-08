#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "../include/wotb_mod_runtime.h"
#include "../include/wotb_mod_runtime_v3.h"

namespace {

char g_logs[32768] = {};
size_t g_logLength = 0u;
uint32_t g_checks = 0u;

struct MarkerUpdateStress {
    volatile LONG stop;
};

DWORD WINAPI MarkerUpdateWorker(void* context) {
    MarkerUpdateStress* stress =
        static_cast<MarkerUpdateStress*>(context);
    while (InterlockedCompareExchange(&stress->stop, 0, 0) == 0) {
        WotbModRuntime_SetCrashLoopPhase(
            "concurrent_shutdown_update");
        SwitchToThread();
    }
    return 0;
}

void WOTBMOD_CALL LogSink(
    WotbModLogLevel,
    const char* message,
    void*) {
    if (!message || !message[0] ||
        g_logLength >= sizeof(g_logs) - 2u) {
        return;
    }
    const size_t remaining = sizeof(g_logs) - g_logLength;
    const int written = _snprintf_s(
        g_logs + g_logLength,
        remaining,
        _TRUNCATE,
        "%s\n",
        message);
    if (written > 0) {
        g_logLength += static_cast<size_t>(written);
    }
}

void ResetLogs() {
    ZeroMemory(g_logs, sizeof(g_logs));
    g_logLength = 0u;
}

int Fail(const char* message) {
    fprintf(stderr, "SAFE MODE TEST FAILED: %s\n", message);
    if (g_logLength != 0u) {
        fprintf(stderr, "--- captured runtime log ---\n%s", g_logs);
    }
    SetEnvironmentVariableA("WOTBMOD_SAFE_MODE_OVERRIDE", nullptr);
    SetEnvironmentVariableA(
        "WOTBMOD_SAFE_MODE_PORTABLE_ONLY",
        nullptr);
    WotbModRuntime_Shutdown();
    return 1;
}

#define SAFE_CHECK(condition, message) \
    do {                               \
        ++g_checks;                    \
        if (!(condition)) {            \
            return Fail(message);      \
        }                              \
    } while (0)

bool JoinPath(
    char* output,
    size_t outputCapacity,
    const char* left,
    const char* right) {
    return _snprintf_s(
               output,
               outputCapacity,
               _TRUNCATE,
               "%s\\%s",
               left,
               right) >= 0;
}

WotbModResult Initialize(
    const char* gameDirectory,
    const char* modsDirectory) {
    WotbModRuntimeOptions options = {};
    options.struct_size = sizeof(options);
    options.game_directory = gameDirectory;
    options.mods_directory = modsDirectory;
    options.game_module = GetModuleHandleA(nullptr);
    options.log_sink = &LogSink;
    return WotbModRuntime_Initialize(&options);
}

bool FileExists(const char* path) {
    const DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0u;
}

bool FileContains(const char* path, const char* needle) {
    if (!path || !needle || !needle[0]) return false;
    HANDLE file = CreateFileA(
        path,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    char contents[4096] = {};
    DWORD bytesRead = 0;
    const BOOL readOk = ReadFile(
        file,
        contents,
        static_cast<DWORD>(sizeof(contents) - 1u),
        &bytesRead,
        nullptr);
    CloseHandle(file);
    if (!readOk) return false;
    contents[bytesRead] = '\0';
    return strstr(contents, needle) != nullptr;
}

bool WriteFileContents(const char* path, const char* contents) {
    if (!path || !path[0] || !contents) return false;
    HANDLE file = CreateFileA(
        path,
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const DWORD size = static_cast<DWORD>(strlen(contents));
    DWORD written = 0u;
    const BOOL ok = WriteFile(
        file,
        contents,
        size,
        &written,
        nullptr);
    CloseHandle(file);
    return ok && written == size;
}

bool CreatePortableContentFixture(const char* modsDirectory) {
    char packageDirectory[MAX_PATH] = {};
    char assetsDirectory[MAX_PATH] = {};
    char manifestPath[MAX_PATH] = {};
    char contentPath[MAX_PATH] = {};
    char assetPath[MAX_PATH] = {};
    char settingsPath[MAX_PATH] = {};
    if (!JoinPath(
            packageDirectory,
            sizeof(packageDirectory),
            modsDirectory,
            "portable_safe_mode") ||
        !JoinPath(
            assetsDirectory,
            sizeof(assetsDirectory),
            packageDirectory,
            "assets") ||
        !JoinPath(
            manifestPath,
            sizeof(manifestPath),
            packageDirectory,
            "manifest.json") ||
        !JoinPath(
            contentPath,
            sizeof(contentPath),
            packageDirectory,
            "content.json") ||
        !JoinPath(
            assetPath,
            sizeof(assetPath),
            assetsDirectory,
            "marker.txt") ||
        !JoinPath(
            settingsPath,
            sizeof(settingsPath),
            modsDirectory,
            "mods.ini")) {
        return false;
    }
    if ((!CreateDirectoryA(packageDirectory, nullptr) &&
         GetLastError() != ERROR_ALREADY_EXISTS) ||
        (!CreateDirectoryA(assetsDirectory, nullptr) &&
         GetLastError() != ERROR_ALREADY_EXISTS)) {
        return false;
    }
    return WriteFileContents(
               manifestPath,
               "{\"manifest_version\":1,\"type\":\"content\","
               "\"id\":\"test.portable-safe-mode\","
               "\"name\":\"Portable safe-mode fixture\","
               "\"version\":\"1.0.0\",\"developer\":\"tests\","
               "\"content\":\"content.json\","
               "\"permissions\":[\"content\",\"resources.mod\","
               "\"resources.overlay.game\"]}") &&
           WriteFileContents(
               contentPath,
               "{\"type\":\"content\","
               "\"id\":\"test.portable-safe-mode\","
               "\"version\":\"1.0.0\",\"overrides\":{"
               "\"textures\":{"
               "\"game://Data/wotbmod_rc1_safe_mode_marker.txt\":"
               "\"assets/marker.txt\"}}}") &&
           WriteFileContents(assetPath, "portable-safe-mode-ok\n") &&
           WriteFileContents(
               settingsPath,
               "[permissions]\r\n"
               "test.portable-safe-mode=2\r\n");
}

bool RunCrashChild(const char* gameDirectory) {
    char executable[MAX_PATH] = {};
    const DWORD executableLength = GetModuleFileNameA(
        nullptr,
        executable,
        static_cast<DWORD>(sizeof(executable)));
    if (executableLength == 0u ||
        executableLength >= sizeof(executable)) {
        return false;
    }
    char commandLine[MAX_PATH * 3u] = {};
    if (_snprintf_s(
            commandLine,
            sizeof(commandLine),
            _TRUNCATE,
            "\"%s\" --simulate-unclean \"%s\"",
            executable,
            gameDirectory) < 0) {
        return false;
    }

    STARTUPINFOA startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    if (!CreateProcessA(
            executable,
            commandLine,
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &startup,
            &process)) {
        return false;
    }
    const DWORD waitResult =
        WaitForSingleObject(process.hProcess, 30000u);
    DWORD exitCode = STILL_ACTIVE;
    const BOOL exitCodeOk = waitResult == WAIT_OBJECT_0
        ? GetExitCodeProcess(process.hProcess, &exitCode)
        : FALSE;
    if (waitResult == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 74u);
        WaitForSingleObject(process.hProcess, 5000u);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return exitCodeOk && exitCode == 73u;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 &&
        strcmp(argv[1], "--simulate-unclean") == 0) {
        char childModsDirectory[MAX_PATH] = {};
        if (!JoinPath(
                childModsDirectory,
                sizeof(childModsDirectory),
                argv[2],
                "mods")) {
            return 70;
        }
        SetEnvironmentVariableA(
            "WOTBMOD_TEST_CRASH_IN_DLLMAIN",
            "1");
        int32_t childSafeMode = -1;
        if (WotbModRuntime_PreflightCrashLoop(
                childModsDirectory,
                &LogSink,
                nullptr,
                &childSafeMode) != WOTBMOD_OK ||
            childSafeMode != 0) {
            return 71;
        }
        if (Initialize(argv[2], childModsDirectory) != WOTBMOD_OK) {
            return 71;
        }
        const WotbModResult loadResult = WotbModRuntime_LoadAll();
        return loadResult == WOTBMOD_OK ? 72 : 71;
    }
    if (argc != 2) {
        fprintf(
            stderr,
            "usage: safe_mode_recovery_tests.exe <test-game-directory>\n");
        return 2;
    }

    char modsDirectory[MAX_PATH] = {};
    char cacheDirectory[MAX_PATH] = {};
    char markerPath[MAX_PATH] = {};
    char markerTempPath[MAX_PATH] = {};
    char crashHistoryPath[MAX_PATH] = {};
    char autoDisabledPath[MAX_PATH] = {};
    SAFE_CHECK(
        JoinPath(
            modsDirectory,
            sizeof(modsDirectory),
            argv[1],
            "mods"),
        "test paths");
    SAFE_CHECK(
        JoinPath(
            cacheDirectory,
            sizeof(cacheDirectory),
            modsDirectory,
            "cache"),
        "test paths");
    SAFE_CHECK(
        JoinPath(
            markerPath,
            sizeof(markerPath),
            cacheDirectory,
            "runtime_session.marker"),
        "test paths");
    SAFE_CHECK(
        JoinPath(
            markerTempPath,
            sizeof(markerTempPath),
            cacheDirectory,
            "runtime_session.marker.tmp"),
        "test paths");
    SAFE_CHECK(
        JoinPath(
            crashHistoryPath,
            sizeof(crashHistoryPath),
            cacheDirectory,
            "crash_history.ini"),
        "test paths");
    SAFE_CHECK(
        JoinPath(
            autoDisabledPath,
            sizeof(autoDisabledPath),
            cacheDirectory,
            "auto_disabled_mod.ini"),
        "test paths");

    SetEnvironmentVariableA("WOTBMOD_SAFE_MODE_OVERRIDE", nullptr);
    SetEnvironmentVariableA(
        "WOTBMOD_TEST_CRASH_IN_DLLMAIN",
        nullptr);
    SetEnvironmentVariableA(
        "WOTBMOD_SAFE_MODE_PORTABLE_ONLY",
        nullptr);
    DeleteFileA(markerPath);
    DeleteFileA(markerTempPath);
    DeleteFileA(crashHistoryPath);
    DeleteFileA(autoDisabledPath);

    ResetLogs();
    int32_t preflightSafeMode = -1;
    SAFE_CHECK(
        WotbModRuntime_PreflightCrashLoop(
            modsDirectory,
            &LogSink,
            nullptr,
            &preflightSafeMode) == WOTBMOD_OK &&
            preflightSafeMode == 0,
        "normal preflight");
    SAFE_CHECK(
        Initialize(argv[1], modsDirectory) == WOTBMOD_OK,
        "normal initialization");
    SAFE_CHECK(
        FileExists(markerPath),
        "normal startup did not create session marker");
    SAFE_CHECK(
        FileContains(markerPath, "session_token="),
        "normal session marker has no ownership token");
    SAFE_CHECK(
        WotbModRuntime_LoadAll() == WOTBMOD_OK,
        "normal LoadAll");
    const WotbModHostApi* host = WotbModRuntime_GetHostApi();
    SAFE_CHECK(
        host &&
            host->get_mod_count &&
            host->get_mod_count() == 1u &&
            WotbModV3Runtime_GetBootstrap(),
        "normal loader/runtime surfaces");
    MarkerUpdateStress updateStress = {};
    HANDLE updateThreads[2] = {};
    for (uint32_t index = 0; index < 2u; ++index) {
        updateThreads[index] = CreateThread(
            nullptr,
            0,
            &MarkerUpdateWorker,
            &updateStress,
            0,
            nullptr);
        SAFE_CHECK(
            updateThreads[index] != nullptr,
            "marker update stress thread");
    }
    Sleep(25u);
    WotbModRuntime_Shutdown();
    InterlockedExchange(&updateStress.stop, 1);
    SAFE_CHECK(
        WaitForMultipleObjects(
            2u,
            updateThreads,
            TRUE,
            10000u) == WAIT_OBJECT_0,
        "marker update stress join");
    for (uint32_t index = 0; index < 2u; ++index) {
        CloseHandle(updateThreads[index]);
    }
    SAFE_CHECK(
        !FileExists(markerPath),
        "concurrent clean shutdown retained owned session marker");

    SAFE_CHECK(
        Initialize(argv[1], modsDirectory) == WOTBMOD_OK,
        "process-detach fallback initialization");
    SAFE_CHECK(
        FileExists(markerPath),
        "process-detach fallback did not create session marker");
    WotbModRuntime_ProcessDetach();
    SAFE_CHECK(
        !FileExists(markerPath),
        "orderly process-detach fallback retained session marker");
    WotbModRuntime_Shutdown();

    for (uint32_t cycle = 0; cycle < 16u; ++cycle) {
        SAFE_CHECK(
            Initialize(argv[1], modsDirectory) == WOTBMOD_OK,
            "repeated clean initialization");
        for (uint32_t update = 0; update < 16u; ++update) {
            WotbModRuntime_SetCrashLoopPhase(
                "repeated_clean_update");
        }
        WotbModRuntime_Shutdown();
        SAFE_CHECK(
            !FileExists(markerPath),
            "repeated clean shutdown retained session marker");
    }

    SAFE_CHECK(
        CreatePortableContentFixture(modsDirectory),
        "portable content fixture creation");

    SAFE_CHECK(
        RunCrashChild(argv[1]),
        "unclean child did not terminate from mod DllMain");
    SAFE_CHECK(
        FileExists(markerPath),
        "unclean child did not retain session marker");
    SAFE_CHECK(
        FileContains(markerPath, "session_token="),
        "unclean child marker has no ownership token");
    SAFE_CHECK(
        FileContains(markerPath, "last_callback_owner=") &&
            FileContains(markerPath, "last_callback=") &&
            FileContains(markerPath, "last_native_binding=") &&
            FileContains(markerPath, "last_event=") &&
            FileContains(markerPath, "last_async_operation=") &&
            FileContains(markerPath, "last_resource_transaction="),
        "crash marker omits RC1 attribution fields");
    ResetLogs();
    preflightSafeMode = -1;
    SAFE_CHECK(
        WotbModRuntime_PreflightCrashLoop(
            modsDirectory,
            &LogSink,
            nullptr,
            &preflightSafeMode) == WOTBMOD_OK &&
            preflightSafeMode == 1,
        "safe-mode preflight");
    SAFE_CHECK(
        Initialize(argv[1], modsDirectory) == WOTBMOD_OK,
        "safe-mode initialization");
    SAFE_CHECK(
        WotbModRuntime_LoadAll() == WOTBMOD_OK,
        "safe-mode LoadAll must be a diagnostic no-op");
    host = WotbModRuntime_GetHostApi();
    SAFE_CHECK(
        host &&
            WotbModV3Runtime_GetBootstrap() &&
            host->get_mod_count() == 0u,
        "safe mode did not preserve diagnostics or block mods");
    SAFE_CHECK(
        host->set_mod_enabled("test.safe-mode-crash", 1) ==
            WOTBMOD_ERROR_DISABLED,
        "safe mode did not block explicit enable");
    SAFE_CHECK(
        strstr(g_logs, "SAFE MODE:") &&
            strstr(g_logs, "last_phase=loading_native_mod") &&
            strstr(g_logs, "last_mod=safe_mode_crash_mod") &&
            strstr(g_logs, markerPath),
        "safe-mode diagnostic reason");
    WotbModRuntime_Shutdown();
    SAFE_CHECK(
        FileExists(markerPath),
        "safe shutdown removed forensic stale marker");
    WotbModRuntime_ProcessDetach();
    SAFE_CHECK(
        FileExists(markerPath),
        "process-detach fallback removed an unowned forensic marker");

    SetEnvironmentVariableA(
        "WOTBMOD_SAFE_MODE_PORTABLE_ONLY",
        "1");
    ResetLogs();
    preflightSafeMode = -1;
    SAFE_CHECK(
        WotbModRuntime_PreflightCrashLoop(
            modsDirectory,
            &LogSink,
            nullptr,
            &preflightSafeMode) == WOTBMOD_OK &&
            preflightSafeMode == 1,
        "portable-only safe-mode preflight");
    SAFE_CHECK(
        Initialize(argv[1], modsDirectory) == WOTBMOD_OK,
        "portable-only safe-mode initialization");
    SAFE_CHECK(
        WotbModRuntime_LoadAll() == WOTBMOD_OK,
        "portable-only safe-mode LoadAll");
    host = WotbModRuntime_GetHostApi();
    SAFE_CHECK(
        host && host->get_mod_count() == 1u &&
            strstr(g_logs, "SAFE MODE PORTABLE-ONLY") &&
            strstr(g_logs, "native package skipped") == nullptr &&
            strstr(g_logs, "loose DLL skipped by portable-only safe mode"),
        "portable-only mode did not load only the content package");
    WotbModRuntime_Shutdown();
    SetEnvironmentVariableA(
        "WOTBMOD_SAFE_MODE_PORTABLE_ONLY",
        nullptr);
    SAFE_CHECK(
        FileExists(markerPath),
        "portable-only recovery removed stale forensic marker");

    SetEnvironmentVariableA("WOTBMOD_SAFE_MODE_OVERRIDE", "1");
    ResetLogs();
    preflightSafeMode = -1;
    SAFE_CHECK(
        WotbModRuntime_PreflightCrashLoop(
            modsDirectory,
            &LogSink,
            nullptr,
            &preflightSafeMode) == WOTBMOD_OK &&
            preflightSafeMode == 0,
        "explicit override preflight");
    SAFE_CHECK(
        Initialize(argv[1], modsDirectory) == WOTBMOD_OK,
        "explicit override initialization");
    SAFE_CHECK(
        FileExists(markerPath) &&
            strstr(g_logs, "WOTBMOD_SAFE_MODE_OVERRIDE bypassed"),
        "explicit override did not claim a new session");
    SAFE_CHECK(
        WotbModRuntime_LoadAll() == WOTBMOD_OK,
        "override LoadAll");
    host = WotbModRuntime_GetHostApi();
    SAFE_CHECK(
        host && host->get_mod_count() == 2u,
        "override did not restore third-party mod loading");
    WotbModRuntime_Shutdown();
    SetEnvironmentVariableA("WOTBMOD_SAFE_MODE_OVERRIDE", nullptr);
    SAFE_CHECK(
        !FileExists(markerPath),
        "clean override shutdown retained session marker");

    SAFE_CHECK(
        RunCrashChild(argv[1]),
        "second attributed crash child did not terminate");
    SAFE_CHECK(
        FileExists(markerPath),
        "second attributed crash did not retain session marker");
    ResetLogs();
    preflightSafeMode = -1;
    SAFE_CHECK(
        WotbModRuntime_PreflightCrashLoop(
            modsDirectory,
            &LogSink,
            nullptr,
            &preflightSafeMode) == WOTBMOD_OK &&
            preflightSafeMode == 1,
        "repeated-crash safe-mode preflight");
    SAFE_CHECK(
        FileContains(
            autoDisabledPath,
            "id=safe_mode_crash_mod") &&
            FileContains(
                autoDisabledPath,
                "reason=repeated_crash_attribution") &&
            FileContains(crashHistoryPath, "count=2"),
        "repeated crash did not create an attributed auto-disable marker");
    SAFE_CHECK(
        Initialize(argv[1], modsDirectory) == WOTBMOD_OK &&
            WotbModRuntime_LoadAll() == WOTBMOD_OK,
        "repeated-crash safe-mode diagnostic startup");
    WotbModRuntime_Shutdown();

    SetEnvironmentVariableA("WOTBMOD_SAFE_MODE_OVERRIDE", "1");
    ResetLogs();
    preflightSafeMode = -1;
    SAFE_CHECK(
        WotbModRuntime_PreflightCrashLoop(
            modsDirectory,
            &LogSink,
            nullptr,
            &preflightSafeMode) == WOTBMOD_OK &&
            preflightSafeMode == 0,
        "auto-disabled mod recovery override preflight");
    SAFE_CHECK(
        Initialize(argv[1], modsDirectory) == WOTBMOD_OK &&
            WotbModRuntime_LoadAll() == WOTBMOD_OK,
        "auto-disabled mod recovery startup");
    host = WotbModRuntime_GetHostApi();
    SAFE_CHECK(
        host && host->get_mod_count() == 1u &&
            strstr(
                g_logs,
                "repeated-crash auto-disable marker"),
        "auto-disabled crashing mod was loaded during recovery");
    WotbModRuntime_Shutdown();
    SetEnvironmentVariableA("WOTBMOD_SAFE_MODE_OVERRIDE", nullptr);
    SAFE_CHECK(
        !FileExists(markerPath),
        "clean auto-disabled recovery retained session marker");

    printf(
        "SAFE MODE RECOVERY OK: checks=%u normal, stale-marker block, "
        "portable-only, repeated-crash auto-disable, forensics, override, "
        "and clean recovery passed\n",
        g_checks);
    return 0;
}
