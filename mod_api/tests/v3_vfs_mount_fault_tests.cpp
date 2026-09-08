/*
 * Regression test for the g_vfs_mutex leak on the mod-enable fault path.
 *
 * The defect: VfsMountPackage scanned g_mounts under a std::lock_guard on
 * g_vfs_mutex, dereferencing raw VfsMount pointers while it did. That scan
 * is reachable from mod code running under __try/__except -- a mod calling
 * set_mod_enabled(self, true) from its own callback goes HostSetModEnabled
 * -> SetRecordEnabled -> MountV3PackageDuringEnable ->
 * EnsureV3PackageMounted -> MountVfsPackageForRuntime -> VfsMountPackage --
 * and everything compiles /EHsc, under which MSVC runs no C++ destructor
 * while unwinding to an __except. An access violation in that scan left
 * g_vfs_mutex, a plain non-recursive std::mutex, locked forever by a thread
 * the handler let keep running. Every later mod:// or game:// resolution,
 * mount, unmount and overlay in the process then blocked on it: a hang
 * minutes later on an unrelated thread, with nothing pointing back here.
 *
 * WHAT THIS TEST PROVES
 *
 *   1. That /EHsc really does skip the destructor. Measured, not assumed:
 *      a std::lock_guard scope that takes an access violation and unwinds
 *      to an __except leaves its mutex locked. This is the premise the fix
 *      exists for, and it is checked here so a toolchain change cannot
 *      quietly invalidate it.
 *   2. That a real access violation, raised inside the real guarded region
 *      of the real VfsMountPackage in mod_api/src/v3/data_services.cpp,
 *      leaves g_vfs_mutex acquirable afterwards. That is the fix.
 *   3. That the runtime still mounts and unmounts through the public path
 *      after that fault -- the property a player's client depends on.
 *   4. That the ordinary early return from inside the guarded region (a
 *      duplicate provider id) also releases the lock. That path is the one
 *      most likely to be broken by a careless edit to the fix, because it
 *      leaves the region without faulting.
 *
 * WHAT IT DOES NOT PROVE
 *
 *   It does not drive the fault through the enable path itself. Reaching
 *   the scan through HostSetModEnabled needs the legacy facade, a loaded
 *   mod module and a callback in flight; this test calls
 *   MountVfsPackageForRuntime, which is the exact function that path calls,
 *   from a __try/__except of its own that stands in for the mod-callback
 *   handler. The unwind, the guarded region and the mutex are all real; the
 *   caller above them is not.
 *
 *   It says nothing about the ten other g_vfs_mutex critical sections in
 *   data_services.cpp, which are the same class of hazard and are not
 *   fixed.
 *
 * HOW THE FAULT IS RAISED
 *
 *   g_vfs_mutex and g_mounts have internal linkage -- they sit in an
 *   anonymous namespace inside wotbmod::v3 -- so no separate translation
 *   unit can reach them, and reaching them is the only way to make the scan
 *   fault on demand. data_services.cpp is therefore compiled into this test
 *   by #include, and the build script deliberately does not compile it a
 *   second time. A pointer into a PAGE_NOACCESS page is pushed into
 *   g_mounts; the scan's first field read through it is an access
 *   violation, at the exact instruction class the defect is about.
 *
 *   A test that deadlocks is a test that hangs the build, so a detached
 *   watchdog thread kills the process with a non-zero exit code if the main
 *   thread is still running after thirty seconds. A leaked g_vfs_mutex
 *   shows up as that timeout rather than as a stalled build.
 */

#include "../src/v3/data_services.cpp"

#include "../include/wotb_mod_runtime_v3.h"
#include "../src/v3/data_services_backend.h"
#include "../src/v3/wotb_mod_v3_internal.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

namespace wotbmod {
namespace v3 {

/*
 * client_services.cpp and tooling_services.cpp are not in this test's link,
 * and WotbModV3Runtime_Initialize calls both registrars unconditionally.
 * Same stubs as v3_data_truth_tests.cpp.
 */
void RegisterClientServices() {}
/* runtime_services.cpp pumps the HUD from the main thread; the HUD lives in
 * client_services.cpp, which this test does not compile. */
void HudMainThreadTick() {}
void RegisterToolingServices() {}

}  // namespace v3
}  // namespace wotbmod

namespace {

uint32_t g_passed = 0u;
uint32_t g_failed = 0u;
std::atomic<bool> g_finished{false};
const char* volatile g_stage = "startup";

/*
 * One page that can never be read or written. It stands in for a corrupt
 * VfsMount pointer inside g_mounts, and for the control experiment's
 * faulting store. Deliberately never freed.
 */
void* g_poison_page = nullptr;

/*
 * Deliberately leaked. The control experiment below is expected to leave
 * this mutex locked forever -- that is the measurement -- and destroying a
 * locked std::mutex is undefined behaviour.
 */
std::mutex* g_control_mutex = nullptr;

void Check(bool condition, const char* label) {
    if (condition) {
        ++g_passed;
        return;
    }
    ++g_failed;
    std::printf("FAIL: %s\n", label);
}

void StartWatchdog() {
    std::thread watchdog([]() {
        const std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!g_finished.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                const char* const stage = g_stage;
                std::printf(
                    "FAIL: the test never finished; it is still blocked in "
                    "stage \"%s\", which is exactly what a g_vfs_mutex "
                    "leaked through an SEH unwind looks like\n",
                    stage);
                std::printf("V3 VFS mount fault: deadlocked\n");
                std::fflush(stdout);
                ExitProcess(1u);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });
    watchdog.detach();
}

/*
 * try_lock from a thread that is not the one that may still own the mutex.
 * Calling try_lock on a std::mutex this thread already holds is undefined,
 * so the probe never runs on the faulting thread. The loop is bounded, so
 * this always joins.
 */
bool MutexAcquirableFromAnotherThread(
    std::mutex* mutex,
    unsigned int budget_milliseconds) {
    std::atomic<bool> acquired{false};
    std::thread prober([mutex, budget_milliseconds, &acquired]() {
        const std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::now() +
            std::chrono::milliseconds(budget_milliseconds);
        while (std::chrono::steady_clock::now() < deadline) {
            if (mutex->try_lock()) {
                mutex->unlock();
                acquired.store(true, std::memory_order_release);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
    prober.join();
    return acquired.load(std::memory_order_acquire);
}

/*
 * The control experiment, in two functions because it has to be. A function
 * that owns an object requiring unwinding cannot contain __try under /EHsc
 * (C2712), so the lock_guard lives in one frame and the handler in another
 * -- which is also the exact shape production has, with the guard inside
 * the runtime and the handler around the mod callback.
 */
__declspec(noinline) int FaultUnderLockGuard() {
    std::lock_guard<std::mutex> lock(*g_control_mutex);
    volatile int* const unmapped =
        reinterpret_cast<volatile int*>(g_poison_page);
    *unmapped = 1;
    return 0;
}

int FaultUnderLockGuardBehindHandler() {
    __try {
        return FaultUnderLockGuard();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

/*
 * Stands in for the mod-callback handler that a set_mod_enabled from inside
 * a callback would put underneath VfsMountPackage. Nothing in this frame
 * has a destructor, again because of C2712.
 */
int MountBehindHandler(
    WotbModV3Handle mod,
    const char* provider_id,
    const char* directory) {
    __try {
        WotbModV3Handle mount = WOTBMOD_V3_INVALID_HANDLE;
        wotbmod::v3::MountVfsPackageForRuntime(
            mod, provider_id, directory, 0, &mount);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

WotbModV3Result WOTBMOD_V3_CALL TestEntry(
    const WotbModV3Bootstrap*,
    WotbModV3Handle,
    WotbModV3Info* info) {
    if (!info) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    *info = {};
    WOTBMOD_V3_INIT_STRUCT(*info, WOTBMOD_V3_ABI_VERSION);
    info->requested_permission_tier = WOTBMOD_V3_PERMISSION_SAFE;
    strncpy_s(info->id, sizeof(info->id), "tests.vfs-mount-fault", _TRUNCATE);
    strncpy_s(
        info->name, sizeof(info->name), "VFS mount fault test", _TRUNCATE);
    strncpy_s(info->version, sizeof(info->version), "1.0.0", _TRUNCATE);
    strncpy_s(info->author, sizeof(info->author), "tests", _TRUNCATE);
    return WOTBMOD_V3_OK;
}

}  // namespace

int main() {
    StartWatchdog();

    g_poison_page = VirtualAlloc(
        nullptr, 4096u, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    Check(
        g_poison_page != nullptr,
        "the test can reserve a page that faults on every access");
    if (!g_poison_page) {
        g_finished.store(true, std::memory_order_release);
        std::printf(
            "V3 VFS mount fault: %u passed, %u failed\n", g_passed, g_failed);
        return 1;
    }
    g_control_mutex = new std::mutex();

    /*
     * Premise. If this check ever fails, MSVC has started running C++
     * destructors while unwinding to an __except; the __try/__finally in
     * VfsMountPackage would then be redundant rather than wrong, and should
     * be re-justified before anyone removes it.
     */
    g_stage = "measuring whether a lock_guard survives an SEH unwind";
    Check(
        FaultUnderLockGuardBehindHandler() == 1,
        "an access violation under a std::lock_guard unwinds to the "
        "caller's __except handler instead of killing the process");
    Check(
        !MutexAcquirableFromAnotherThread(g_control_mutex, 500u),
        "a std::lock_guard does not release its mutex when the unwind goes "
        "to an __except, which is the toolchain fact the fix exists for");

    g_stage = "initializing the runtime";
    WotbModV3RuntimeOptions options = {};
    options.struct_size = sizeof(options);
    options.api_version = WOTBMOD_V3_ABI_VERSION;
    options.game_directory = ".";
    options.mods_directory = "build\\v3_vfs_mount_fault\\mods";
    options.cache_directory = "build\\v3_vfs_mount_fault\\cache";
    options.config_directory = "build\\v3_vfs_mount_fault\\config";
    options.client_version = "vfs-mount-fault-test";
    Check(
        WotbModV3Runtime_Initialize(&options) == WOTBMOD_V3_OK,
        "the v3 runtime initializes against the test directories");

    g_stage = "creating and enabling the test mod";
    WotbModV3Handle mod = WOTBMOD_V3_INVALID_HANDLE;
    Check(
        WotbModV3Runtime_CreateMod(
            "vfs_fault_mod.dll",
            WOTBMOD_V3_PERMISSION_SAFE,
            &mod) == WOTBMOD_V3_OK,
        "the runtime creates the mod record the mounts will be owned by");
    const char* const grants[] = {"resources.mod"};
    Check(
        WotbModV3Runtime_SetPermissionGrants(mod, grants, 1u, 1u) ==
            WOTBMOD_V3_OK,
        "the test mod is restricted to the resources.mod grant");
    WotbModV3RuntimeModuleInfo module = {};
    WOTBMOD_V3_INIT_STRUCT(module, WOTBMOD_V3_ABI_VERSION);
    Check(
        WotbModV3Runtime_InvokeEntry(mod, &TestEntry, &module) ==
            WOTBMOD_V3_OK,
        "the test mod reports its info through the normal entry point");
    Check(
        WotbModV3Runtime_Enable(mod) == WOTBMOD_V3_OK,
        "the test mod enables so that it can own VFS handles");

    /*
     * CreateMod creates this directory; it is one of the mod's own owned
     * roots, which is what IsAllowedOwnedPhysicalPath requires.
     */
    const char* const package_root =
        "build\\v3_vfs_mount_fault\\mods\\data\\vfs_fault_mod";

    g_stage = "mounting through the unfaulted path";
    WotbModV3Handle alpha = WOTBMOD_V3_INVALID_HANDLE;
    Check(
        wotbmod::v3::MountVfsPackageForRuntime(
            mod, "tests.alpha", package_root, 0, &alpha) == WOTBMOD_V3_OK,
        "a first package mount succeeds through the runtime bridge");

    /*
     * The duplicate leaves the guarded region by returning from inside it,
     * which before the fix was a plain `return` under a lock_guard and is
     * now a return through a __finally. If that __finally does not unlock,
     * the very next line never comes back and the watchdog kills the run.
     */
    g_stage = "rejecting a duplicate provider id from inside the lock";
    WotbModV3Handle duplicate = WOTBMOD_V3_INVALID_HANDLE;
    Check(
        wotbmod::v3::MountVfsPackageForRuntime(
            mod, "tests.alpha", package_root, 0, &duplicate) ==
            WOTBMOD_V3_E_ALREADY_EXISTS,
        "a duplicate provider id is refused from inside the guarded scan");
    Check(
        duplicate == WOTBMOD_V3_INVALID_HANDLE,
        "the refused duplicate hands back no mount handle");

    g_stage = "mounting again after the in-region early return";
    WotbModV3Handle beta = WOTBMOD_V3_INVALID_HANDLE;
    Check(
        wotbmod::v3::MountVfsPackageForRuntime(
            mod, "tests.beta", package_root, 0, &beta) == WOTBMOD_V3_OK,
        "the guarded scan released g_vfs_mutex on its early-return path, so "
        "a second provider id still mounts");

    /*
     * Now the real thing. The poison entry goes in last, so the scan walks
     * the two live mounts, reaches the third element and faults on the
     * `existing->owner` read -- inside the region g_vfs_mutex guards, at
     * the exact instruction class the defect is about.
     */
    g_stage = "poisoning g_mounts";
    {
        std::lock_guard<std::mutex> lock(wotbmod::v3::g_vfs_mutex);
        wotbmod::v3::g_mounts.push_back(
            reinterpret_cast<wotbmod::v3::VfsMount*>(g_poison_page));
    }

    g_stage = "faulting inside the guarded mount scan";
    const int faulted =
        MountBehindHandler(mod, "tests.faulting", package_root);
    Check(
        faulted == 1,
        "dereferencing an unmapped VfsMount inside the guarded scan raises "
        "an access violation that unwinds to the caller's __except handler");

    g_stage = "checking that g_vfs_mutex survived the unwind";
    const bool vfs_mutex_free =
        MutexAcquirableFromAnotherThread(&wotbmod::v3::g_vfs_mutex, 2000u);
    Check(
        vfs_mutex_free,
        "g_vfs_mutex is acquirable again after an access violation unwound "
        "out of its critical section, so the fault does not wedge the VFS");

    if (!vfs_mutex_free) {
        /*
         * The lock is gone. Every remaining step would block on it, and the
         * watchdog would report a timeout instead of this much more useful
         * message. Report now and leave without touching the VFS again;
         * static destructors are skipped on purpose, because one of them
         * would destroy a locked std::mutex.
         */
        std::printf(
            "V3 VFS mount fault: %u passed, %u failed\n", g_passed, g_failed);
        std::fflush(stdout);
        ExitProcess(1u);
    }

    g_stage = "removing the poison entry";
    {
        std::lock_guard<std::mutex> lock(wotbmod::v3::g_vfs_mutex);
        Check(
            !wotbmod::v3::g_mounts.empty() &&
                wotbmod::v3::g_mounts.back() ==
                    reinterpret_cast<wotbmod::v3::VfsMount*>(g_poison_page),
            "the faulting mount never reached the registration step, so the "
            "poison entry is still the last one");
        if (!wotbmod::v3::g_mounts.empty()) {
            wotbmod::v3::g_mounts.pop_back();
        }
    }

    g_stage = "using the VFS again after the fault";
    WotbModV3Handle gamma = WOTBMOD_V3_INVALID_HANDLE;
    Check(
        wotbmod::v3::MountVfsPackageForRuntime(
            mod, "tests.gamma", package_root, 0, &gamma) == WOTBMOD_V3_OK,
        "a package still mounts after the fault, which is the property a "
        "player's client depends on");
    Check(
        wotbmod::v3::UnmountVfsForRuntime(mod, alpha) == WOTBMOD_V3_OK,
        "an unmount still completes after the fault, exercising the "
        "g_vfs_mutex that VfsMount's destructor takes");

    g_stage = "shutting the mod down";
    Check(
        WotbModV3Runtime_Disable(mod) == WOTBMOD_V3_OK,
        "the test mod disables and releases its remaining mounts");
    Check(
        WotbModV3Runtime_DestroyMod(mod) == WOTBMOD_V3_OK,
        "the test mod record is destroyed");
    WotbModV3Runtime_Shutdown();

    g_finished.store(true, std::memory_order_release);
    std::printf(
        "V3 VFS mount fault: %u passed, %u failed\n", g_passed, g_failed);
    /*
     * A plain return is safe here even though the control experiment left
     * its mutex locked forever: that mutex is heap-allocated and never
     * deleted, so no destructor ever runs on a locked one. The path above
     * that calls ExitProcess is the one where g_vfs_mutex itself may still
     * be held, and that one is a namespace-scope std::mutex which does get
     * destroyed on a normal exit.
     */
    return g_failed == 0u ? 0 : 1;
}
