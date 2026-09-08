// version.dll proxy — World of Tanks Blitz "Exit to Hangar always visible" mod
// Build: see build.cmd. Loaded by wotblitz.exe via implicit VERSION.dll import.

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <dbghelp.h>

// Forwarder exports — every imported VERSION.dll function is forwarded
// to vorig.dll (which is the original C:\Windows\SysWOW64\version.dll
// copied next to the EXE and renamed). MSVC linker accepts this syntax.
#pragma comment(linker, "/EXPORT:GetFileVersionInfoA=vorig.GetFileVersionInfoA")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoByHandle=vorig.GetFileVersionInfoByHandle")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoExA=vorig.GetFileVersionInfoExA")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoExW=vorig.GetFileVersionInfoExW")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoSizeA=vorig.GetFileVersionInfoSizeA")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoSizeExA=vorig.GetFileVersionInfoSizeExA")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoSizeExW=vorig.GetFileVersionInfoSizeExW")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoSizeW=vorig.GetFileVersionInfoSizeW")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoW=vorig.GetFileVersionInfoW")
#pragma comment(linker, "/EXPORT:VerFindFileA=vorig.VerFindFileA")
#pragma comment(linker, "/EXPORT:VerFindFileW=vorig.VerFindFileW")
#pragma comment(linker, "/EXPORT:VerInstallFileA=vorig.VerInstallFileA")
#pragma comment(linker, "/EXPORT:VerInstallFileW=vorig.VerInstallFileW")
#pragma comment(linker, "/EXPORT:VerQueryValueA=vorig.VerQueryValueA")
#pragma comment(linker, "/EXPORT:VerQueryValueW=vorig.VerQueryValueW")

static FILE* g_log = nullptr;
static CRITICAL_SECTION g_logCS;
static void* g_vehHandle = nullptr;
static volatile LONG g_vehLogCount = 0;
static volatile LONG g_unhandledCrashObserved = 0;
static volatile LONG g_crashContextSeq = 0;
static DWORD g_crashContextThread = 0;
static char g_crashContext[128] = {};
static void* g_crashArg0 = nullptr;
static void* g_crashArg1 = nullptr;
static void* g_crashArg2 = nullptr;
static char g_runtimeMarkerPath[MAX_PATH] = {};

extern "C" __declspec(thread) int g_modSuppressCrashLog = 0;

extern "C" void ModLog(const char* fmt, ...) {
    if (!g_log) return;
    EnterCriticalSection(&g_logCS);
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d pid=%lu] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentProcessId());
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_logCS);
}

extern "C" void ModSetCrashContext(const char* where, void* a, void* b, void* c) {
    InterlockedIncrement(&g_crashContextSeq);
    g_crashContextThread = GetCurrentThreadId();
    _snprintf(g_crashContext, sizeof(g_crashContext) - 1, "%s", where ? where : "(null)");
    g_crashContext[sizeof(g_crashContext) - 1] = 0;
    g_crashArg0 = a;
    g_crashArg1 = b;
    g_crashArg2 = c;
    InterlockedIncrement(&g_crashContextSeq);
}

static void LogExceptionDetails(const char* tag, const char* where, EXCEPTION_POINTERS* ep) {
    DWORD code = 0;
    void* addr = nullptr;
    ULONG_PTR info0 = 0;
    ULONG_PTR info1 = 0;
    ULONG_PTR info2 = 0;
    void* ip = nullptr;
    void* sp = nullptr;
    void* bp = nullptr;

    if (ep && ep->ExceptionRecord) {
        code = ep->ExceptionRecord->ExceptionCode;
        addr = ep->ExceptionRecord->ExceptionAddress;
        if (ep->ExceptionRecord->NumberParameters > 0) info0 = ep->ExceptionRecord->ExceptionInformation[0];
        if (ep->ExceptionRecord->NumberParameters > 1) info1 = ep->ExceptionRecord->ExceptionInformation[1];
        if (ep->ExceptionRecord->NumberParameters > 2) info2 = ep->ExceptionRecord->ExceptionInformation[2];
    }

    if (ep && ep->ContextRecord) {
#if defined(_M_IX86)
        ip = (void*)ep->ContextRecord->Eip;
        sp = (void*)ep->ContextRecord->Esp;
        bp = (void*)ep->ContextRecord->Ebp;
#elif defined(_M_X64)
        ip = (void*)ep->ContextRecord->Rip;
        sp = (void*)ep->ContextRecord->Rsp;
        bp = (void*)ep->ContextRecord->Rbp;
#endif
    }

    char modulePath[MAX_PATH] = {};
    uintptr_t moduleOff = 0;
    HMODULE mod = nullptr;
    if (addr && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)addr,
                                   &mod)) {
        GetModuleFileNameA(mod, modulePath, sizeof(modulePath));
        moduleOff = (uintptr_t)addr - (uintptr_t)mod;
    } else {
        strcpy(modulePath, "(unknown)");
    }

    char ctx[128] = {};
    DWORD ctxThread = 0;
    void* ctxA = nullptr;
    void* ctxB = nullptr;
    void* ctxC = nullptr;
    for (int tries = 0; tries < 3; ++tries) {
        LONG seq0 = g_crashContextSeq;
        if (seq0 & 1) continue;
        _snprintf(ctx, sizeof(ctx) - 1, "%s", g_crashContext[0] ? g_crashContext : "(none)");
        ctx[sizeof(ctx) - 1] = 0;
        ctxThread = g_crashContextThread;
        ctxA = g_crashArg0;
        ctxB = g_crashArg1;
        ctxC = g_crashArg2;
        LONG seq1 = g_crashContextSeq;
        if (seq0 == seq1 && !(seq1 & 1)) break;
    }

    ModLog("%s: where=%s code=0x%08lX addr=%p ip=%p sp=%p bp=%p info=%p/%p/%p module=%s+0x%Ix lastCtx[t%lu]=%s args=%p/%p/%p",
           tag ? tag : "Exception",
           where ? where : "(unknown)",
           code,
           addr,
           ip,
           sp,
           bp,
           (void*)info0,
           (void*)info1,
           (void*)info2,
           modulePath,
           moduleOff,
           ctxThread,
           ctx,
           ctxA,
           ctxB,
           ctxC);
}

extern "C" int ModLogException(const char* where, EXCEPTION_POINTERS* ep) {
    LogExceptionDetails("SEH caught", where, ep);
    return EXCEPTION_EXECUTE_HANDLER;
}

static LONG CALLBACK ModVectoredExceptionHandler(EXCEPTION_POINTERS* ep) {
    if (g_modSuppressCrashLog > 0 || !ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;

    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION &&
        code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_FLT_DIVIDE_BY_ZERO) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    LONG n = InterlockedIncrement(&g_vehLogCount);
    if (n <= 32) {
        LogExceptionDetails("VEH first-chance", nullptr, ep);
    } else if (n == 33) {
        ModLog("VEH first-chance: suppressing further exception logs after 32 entries");
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// Write a minidump next to wotb_mod.log so we can post-mortem debug the crash.
static void WriteCrashMiniDump(EXCEPTION_POINTERS* ep) {
    char path[MAX_PATH];
    GetModuleFileNameA(GetModuleHandleA(nullptr), path, sizeof(path));
    char* slash = strrchr(path, '\\');
    if (slash) *(slash + 1) = 0; else path[0] = 0;
    SYSTEMTIME t; GetLocalTime(&t);
    char fname[64];
    _snprintf(fname, sizeof(fname) - 1,
              "wotb_mod_crash_%04d%02d%02d_%02d%02d%02d_%lu.dmp",
              t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
              GetCurrentProcessId());
    fname[sizeof(fname) - 1] = 0;
    strncat(path, fname, sizeof(path) - strlen(path) - 1);

    HANDLE hFile = CreateFileA(path, GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        ModLog("MiniDump: CreateFile failed err=%lu path=%s", GetLastError(), path);
        return;
    }
    MINIDUMP_EXCEPTION_INFORMATION mei = {};
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers = FALSE;
    BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                                hFile,
                                (MINIDUMP_TYPE)(MiniDumpWithDataSegs |
                                                MiniDumpWithIndirectlyReferencedMemory |
                                                MiniDumpWithThreadInfo),
                                ep ? &mei : nullptr, nullptr, nullptr);
    CloseHandle(hFile);
    ModLog("MiniDump: %s file=%s", ok ? "OK" : "FAILED", path);
}

static LONG WINAPI ModUnhandledExceptionFilter(EXCEPTION_POINTERS* ep) {
    InterlockedExchange(&g_unhandledCrashObserved, 1);
    LogExceptionDetails("UNHANDLED", nullptr, ep);
    WriteCrashMiniDump(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void Log(const char* fmt, ...) {
    if (!g_log) return;
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf)-1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf)-1] = 0;
    ModLog("%s", buf);
}

static void OpenLog() {
    InitializeCriticalSection(&g_logCS);
    char path[MAX_PATH];
    GetModuleFileNameA(GetModuleHandleA(nullptr), path, sizeof(path));
    char* slash = strrchr(path, '\\');
    const char* exeName = slash ? slash + 1 : path;
    bool isMainGameProcess = (_stricmp(exeName, "wotblitz.exe") == 0);
    if (isMainGameProcess) {
        _snprintf(g_runtimeMarkerPath, sizeof(g_runtimeMarkerPath) - 1, "%s", path);
        g_runtimeMarkerPath[sizeof(g_runtimeMarkerPath) - 1] = 0;
        char* markerName = strrchr(g_runtimeMarkerPath, '\\');
        if (markerName) {
            const size_t prefix =
                static_cast<size_t>(markerName + 1 - g_runtimeMarkerPath);
            _snprintf(
                markerName + 1,
                sizeof(g_runtimeMarkerPath) - prefix,
                "%s",
                "mods\\cache\\runtime_session.marker");
            g_runtimeMarkerPath[sizeof(g_runtimeMarkerPath) - 1] = 0;
        } else {
            g_runtimeMarkerPath[0] = 0;
        }
    }
    if (slash) strcpy(slash + 1, "wotb_mod.log"); else strcpy(path, "wotb_mod.log");
    g_log = fopen(path, isMainGameProcess ? "w" : "a");
    Log("=== wotb_mod proxy loaded ===");
    if (!g_vehHandle) {
        g_vehHandle = AddVectoredExceptionHandler(1, ModVectoredExceptionHandler);
        Log("Crash logger VEH installed handle=%p", g_vehHandle);
    }
    SetUnhandledExceptionFilter(ModUnhandledExceptionFilter);
    Log("Unhandled exception filter installed (minidump on crash)");
}

// Forward declarations
DWORD WINAPI ModThread(LPVOID);

extern "C" BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hInst);
        OpenLog();
        // Defer hook installation: spin a worker that waits for game image to settle.
        CreateThread(nullptr, 0, ModThread, nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH && reserved != nullptr &&
               InterlockedCompareExchange(
                   &g_unhandledCrashObserved, 0, 0) == 0 &&
               g_runtimeMarkerPath[0]) {
        // The client does not run the loader DLL's CRT atexit callback on its
        // ordinary shutdown path. Kernel32 deletion is safe at process detach;
        // keep the marker only when our unhandled-exception filter saw a crash.
        DeleteFileA(g_runtimeMarkerPath);
    }
    return TRUE;
}

DWORD WINAPI ModThread(LPVOID) {
    // Wait until the main module is fully mapped & TLS callbacks executed.
    Sleep(500);
    HMODULE hExe = GetModuleHandleA(nullptr);
    if (!hExe) { Log("GetModuleHandleA(NULL) failed"); return 1; }

    // Confirm we are inside wotblitz.exe.
    char exePath[MAX_PATH];
    GetModuleFileNameA(hExe, exePath, sizeof(exePath));
    Log("Host EXE: %s", exePath);
    Log("Image base: 0x%p", (void*)hExe);

    // Only install hooks in the main game process. wotblitz-browser.exe is
    // a tiny CEF helper that has no GameMenuUI; patching at the same RVA
    // there hits unrelated memory and would crash or no-op.
    const char* base = strrchr(exePath, '\\');
    base = base ? base + 1 : exePath;
    if (_stricmp(base, "wotblitz.exe") != 0) {
        Log("Skipping hook install (not main game exe)");
        return 0;
    }

    // Bootstrap the public mod runtime for ordinary Steam launches. Keep the
    // loader as a separate DLL so the VERSION proxy remains a tiny, stable
    // forwarding boundary and can report loader failures before native hooks
    // are touched.
    char loaderPath[MAX_PATH] = {};
    _snprintf(loaderPath, sizeof(loaderPath) - 1, "%s", exePath);
    loaderPath[sizeof(loaderPath) - 1] = 0;
    char* loaderName = strrchr(loaderPath, '\\');
    if (loaderName) {
        strcpy(loaderName + 1, "wotb_mod_loader.dll");
    } else {
        strcpy(loaderPath, "wotb_mod_loader.dll");
    }

    HMODULE loader = LoadLibraryA(loaderPath);
    if (!loader) {
        Log("Native loader bootstrap failed: path=%s error=%lu",
            loaderPath,
            GetLastError());
        return 1;
    }
    Log("Native loader bootstrapped: path=%s module=%p", loaderPath, loader);
    Log("Mod thread complete");
    return 0;
}

// =====================================================================
// Export forwarding for version.dll lives in version.def
// =====================================================================
