#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <stdio.h>
#include <wchar.h>

static void ParentDirectory(wchar_t* path) {
    if (!path) return;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) *slash = L'\0';
}

int wmain(int argc, wchar_t** argv) {
    wchar_t launcherDirectory[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, launcherDirectory, MAX_PATH);
    ParentDirectory(launcherDirectory);

    wchar_t dllPath[MAX_PATH] = {};
    if (argc >= 2) {
        wcsncpy_s(dllPath, argv[1], _TRUNCATE);
    } else {
        swprintf_s(
            dllPath,
            L"%s\\wotb_mod_loader.dll",
            launcherDirectory);
    }
    if (GetFileAttributesW(dllPath) == INVALID_FILE_ATTRIBUTES) {
        fwprintf(stderr, L"loader DLL not found: %s\n", dllPath);
        return 1;
    }

    wchar_t gamePath[MAX_PATH] = {};
    if (argc >= 3) {
        wcsncpy_s(gamePath, argv[2], _TRUNCATE);
    } else {
        wcsncpy_s(gamePath, launcherDirectory, _TRUNCATE);
        ParentDirectory(gamePath);
        ParentDirectory(gamePath);
        ParentDirectory(gamePath);
        wcscat_s(gamePath, L"\\wotblitz.exe");
    }
    if (GetFileAttributesW(gamePath) == INVALID_FILE_ATTRIBUTES) {
        fwprintf(stderr, L"game executable not found: %s\n", gamePath);
        return 2;
    }

    wchar_t gameDirectory[MAX_PATH] = {};
    wcsncpy_s(gameDirectory, gamePath, _TRUNCATE);
    ParentDirectory(gameDirectory);

#ifndef WOTBMOD_GAME_MODS_LAUNCHER
    wchar_t liveDirectory[MAX_PATH] = {};
    wchar_t liveModsDirectory[MAX_PATH] = {};
    wchar_t liveLogPath[MAX_PATH] = {};
    swprintf_s(
        liveDirectory,
        L"%s\\live_env",
        launcherDirectory);
    swprintf_s(
        liveModsDirectory,
        L"%s\\mods",
        liveDirectory);
    swprintf_s(
        liveLogPath,
        L"%s\\live_host.log",
        liveDirectory);
    SetEnvironmentVariableW(
        L"WOTBMOD_MODS_DIRECTORY",
        liveModsDirectory);
    SetEnvironmentVariableW(
        L"WOTBMOD_LOG_PATH",
        liveLogPath);
#else
    SetEnvironmentVariableW(L"WOTBMOD_MODS_DIRECTORY", nullptr);
    SetEnvironmentVariableW(L"WOTBMOD_LOG_PATH", nullptr);
    SetEnvironmentVariableW(L"WOTBMOD_EARLY_LOAD", L"1");
#endif

#ifndef WOTBMOD_GAME_MODS_LAUNCHER
    SetEnvironmentVariableW(L"WOTBMOD_EARLY_LOAD", nullptr);
#endif

    /*
     * Anything after the game path is forwarded to the client verbatim.
     * The client takes a .wotbreplay path this way, which is the only
     * hands-off route into a battle scene: a replay drives the real
     * scene - vehicles, shots, tracers - without a matchmaker queue and
     * without any live player in the match. With no extra arguments the
     * command line stays nullptr, exactly as before.
     */
    wchar_t gameCommandLine[32768] = {};
    wchar_t* gameCommandLinePtr = nullptr;
    if (argc >= 4) {
        swprintf_s(gameCommandLine, L"\"%s\"", gamePath);
        for (int index = 3; index < argc; ++index) {
            wcscat_s(gameCommandLine, L" \"");
            wcscat_s(gameCommandLine, argv[index]);
            wcscat_s(gameCommandLine, L"\"");
        }
        gameCommandLinePtr = gameCommandLine;
    }

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(
            gamePath,
            gameCommandLinePtr,
            nullptr,
            nullptr,
            FALSE,
            CREATE_SUSPENDED,
            nullptr,
            gameDirectory,
            &startup,
            &process)) {
        fwprintf(
            stderr,
            L"CreateProcess failed: %lu\n",
            GetLastError());
        return 3;
    }

    const SIZE_T dllBytes = (wcslen(dllPath) + 1) * sizeof(wchar_t);
    void* remotePath = VirtualAllocEx(
        process.hProcess,
        nullptr,
        dllBytes,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE);
    if (!remotePath ||
        !WriteProcessMemory(
            process.hProcess,
            remotePath,
            dllPath,
            dllBytes,
            nullptr)) {
        fwprintf(
            stderr,
            L"remote path setup failed: %lu\n",
            GetLastError());
        TerminateProcess(process.hProcess, 10);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return 4;
    }

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC loadLibrary = GetProcAddress(kernel32, "LoadLibraryW");
    HANDLE remoteThread = CreateRemoteThread(
        process.hProcess,
        nullptr,
        0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLibrary),
        remotePath,
        0,
        nullptr);
    if (!remoteThread) {
        fwprintf(
            stderr,
            L"CreateRemoteThread failed: %lu\n",
            GetLastError());
        TerminateProcess(process.hProcess, 11);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return 5;
    }

    const DWORD waitResult = WaitForSingleObject(remoteThread, 30000);
    DWORD remoteResult = 0;
    GetExitCodeThread(remoteThread, &remoteResult);
    CloseHandle(remoteThread);
    VirtualFreeEx(process.hProcess, remotePath, 0, MEM_RELEASE);
    if (waitResult != WAIT_OBJECT_0 || remoteResult == 0) {
        fwprintf(
            stderr,
            L"LoadLibraryW failed: wait=%lu result=%lu\n",
            waitResult,
            remoteResult);
        TerminateProcess(process.hProcess, 12);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return 6;
    }

    ResumeThread(process.hThread);
    wprintf(
        L"WOTB MOD CLIENT STARTED pid=%lu loader=0x%08lX mode=%s\n",
        process.dwProcessId,
        remoteResult,
#ifdef WOTBMOD_GAME_MODS_LAUNCHER
        L"game-mods"
#else
        L"live-test"
#endif
    );
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 0;
}
