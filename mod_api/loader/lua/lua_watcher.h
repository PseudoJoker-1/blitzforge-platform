#ifndef WOTBMOD_LUA_WATCHER_H_
#define WOTBMOD_LUA_WATCHER_H_

// Watching a folder of .lua files, and the debounce that makes an editor's
// save look like one edit instead of three.
//
// Deliberately free of every other header in this host: no Lua, no ABI, no
// LuaScript. It reports paths that changed and nothing else, which is what
// lets the test binary compile this file straight in and drive the classes
// below directly rather than through the DLL's exports.
//
// ---------------------------------------------------------------------------
// Two classes, because there are two separable things here
// ---------------------------------------------------------------------------
//
// ChangeQueue is the debounce rule: a pure function of (what was pushed, when)
// with no threads, no filesystem and no clock of its own - the caller passes
// the time in. Watcher is the Win32 machinery that feeds it.
//
// The split is not tidiness. A debounce tested only through a real watcher is
// tested against the wall clock, and a wall-clock test cannot tell a working
// debounce from a machine slow enough that the two writes it meant to coalesce
// landed 300 ms apart. Driving ChangeQueue with fabricated time points removes
// the clock from the question entirely: "the second write extended the quiet
// period" becomes an assertion about arithmetic, and no amount of machine load
// can make it pass or fail for the wrong reason. What remains for the real
// Watcher to prove is only that the plumbing works - a write reaches the queue
// at all - which one end-to-end test covers.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace wotbmod {
namespace lua {

// 200 ms of quiet before a change is reported.
//
// Editors do not write a file; they write a file in several steps. Notepad,
// VS Code and vim all produce two or three notifications for one Ctrl-S -
// truncate, write, rename-into-place, touch the timestamp - and a host that
// reacted to the first of them would compile an empty or half-written file
// and report a syntax error the author never made. Worse, it would then leave
// the script unloaded and the author staring at a message about line 1 of a
// file that is fine.
//
// So the rule is a *quiet period*, not a rate limit: a path is reported once
// nothing has touched it for this long. Two writes 10 ms apart are one change
// reported 200 ms after the second, not one change reported immediately and
// another later.
inline constexpr std::chrono::milliseconds kDefaultDebounce{200};

// The canonical form two spellings of one path have to share.
//
// Windows' filesystem is case-insensitive and treats / and \ alike, so
// `lua-dev/Panel.lua` and `lua-dev\panel.LUA` are one file. Anything keying a
// map on a path needs them to be one key, and there are two such maps in this
// host: this file's pending-change map, and the loaded-script map in
// lua_host_mod.cpp. They are separate maps with the same requirement, so the
// rule is one function rather than two that agree today.
//
// Exposed for that second caller rather than because a path helper belongs in
// a watcher: the alternative was the host folding paths its own way, and two
// spellings of one script becoming two loaded scripts is exactly the leak this
// task exists to prevent.
//
// A simplification for characters outside the Basic Multilingual Plane and for
// the few whose case folding is not one-to-one. A script named with one of
// those still reloads; it merely reloads twice. Stated here rather than
// discovered later.
std::wstring FoldPathKey(const std::wstring& path);

// `folder` and `name` joined with exactly one separator between them, whether
// or not the folder already ends in one.
//
// This existed three times, inline, before a review found that one of the three
// disagreed: two of them appended a separator only when the folder lacked one,
// and the third appended one unconditionally. A dev folder ending in a
// separator - which %WOTBMOD_LUA_DEV_DIR% and the test override both accept -
// therefore produced `C:\dev\\hot.lua` from one path and `C:\dev\hot.lua` from
// the others.
//
// FoldPathKey does not collapse a doubled separator, so those are two keys:
// two entries in the loaded-script map for one file, and a LuaScript that is
// never detached again because the next notification arrives under the other
// spelling. That is precisely the "two spellings of one path become two loaded
// scripts" leak FoldPathKey exists to prevent, reintroduced by hand. One
// function, used by every caller, so the rule cannot disagree with itself.
std::wstring JoinPath(const std::wstring& folder, const std::wstring& name);

// How many watcher worker threads are running in this module right now.
//
// It exists because "a disable stops the watcher" was otherwise a claim no
// test could check. A host that dropped its Watcher pointer without stopping
// the thread looks identical from every other angle - the pointer is null, no
// reload happens, every counter reads zero - while a worker goes on holding a
// directory handle inside a DLL that may be unloaded from under it. That was
// not hypothetical: a deliberately neutered disable passed the whole suite
// until this counter existed.
//
// Incremented as the worker enters its loop and decremented as it leaves, so
// it reaches zero only after the thread is genuinely on its way out. A caller
// that has just returned from Stop() - which joins - can read it as an
// assertion rather than a poll.
size_t RunningWatcherThreads() noexcept;

// The changed-path queue and the debounce rule over it.
//
// Pushed from the watcher thread, drained from the main thread; the lock is a
// leaf and nothing is called while it is held.
class ChangeQueue {
  public:
    using Clock = std::chrono::steady_clock;

    explicit ChangeQueue(
        std::chrono::milliseconds debounce = kDefaultDebounce) noexcept
        : debounce_(debounce) {}

    ChangeQueue(const ChangeQueue&) = delete;
    ChangeQueue& operator=(const ChangeQueue&) = delete;

    // Records that `path` changed at `now`. Pushing a path that is already
    // pending does not add a second entry - it restarts that path's quiet
    // period, which is the whole of the debounce.
    //
    // noexcept because the caller is a worker thread whose only sensible
    // reaction to std::bad_alloc is to keep watching; a dropped notification
    // is a missed reload, a thrown one out of a detached-in-spirit thread is a
    // terminated game. Rule 5's reasoning, in a place Lua never reaches.
    void Push(const std::wstring& path, Clock::time_point now) noexcept;

    // Every path whose quiet period has elapsed by `now`, in no particular
    // order, each returned once. Paths still inside their window stay pending
    // and come out of a later call.
    //
    // The spelling returned is the one first seen for that file. Matching is
    // case-insensitive, because Windows' filesystem is and two spellings of
    // one path must not become two reloads.
    std::vector<std::wstring> Take(Clock::time_point now) noexcept;

    // Everything pending, dropped. Stop() calls it so a restarted watcher does
    // not deliver an edit that happened while nothing was listening for it.
    void Clear() noexcept;

    // How many paths are pending, whether or not their window has elapsed.
    size_t PendingCount() const noexcept;

    std::chrono::milliseconds Debounce() const noexcept;

    // Changes the quiet period. Only meaningful between runs - Watcher::Start
    // calls it after Stop() has joined the worker, so no push can be in flight
    // - and it is not a knob for a running watcher: half the pending entries
    // would be judged by the old window and half by the new one.
    void SetDebounce(std::chrono::milliseconds debounce) noexcept;

  private:
    struct Pending {
        std::wstring path;         // as first seen
        Clock::time_point last;    // when it was last touched
    };

    mutable std::mutex lock_;
    std::chrono::milliseconds debounce_;
    std::map<std::wstring, Pending> pending_;   // key: case-folded path
};

// Pushes every *file* in `folder` into `queue`, as a full path, timestamped
// now. Subdirectories are skipped: `mods/lua-dev/` is a flat folder of scripts
// and a subfolder is a place to keep notes.
//
// A free function rather than a member of Watcher because it is the answer to
// the one case ReadDirectoryChangesW cannot describe - a completed read of zero
// bytes, meaning the notification buffer overflowed and the changes were
// discarded - and that case is unreachable from a test. Making the kernel
// discard notifications means thousands of changes inside one read window, and
// a test that tries is a test that passes or fails on how fast the machine's
// filesystem is. As a free function taking the queue it fills it is testable
// directly, which is the difference between "reviewed" and "exercised" for the
// only path in this file that can silently lose an author's edit.
void PushFolderContents(ChangeQueue& queue, const std::wstring& folder) noexcept;

// ReadDirectoryChangesW on a worker thread, appending to a ChangeQueue that
// TakeChanged drains.
//
// The division of labour is the safety property of this task. The worker
// thread only ever *queues*; it never touches a lua_State, never calls the
// ABI, never so much as learns that scripts exist. Reloading from here would
// mean destroying a lua_State from a thread that has no idea whether the
// render thread is currently inside one of that state's callbacks - and
// ~LuaScript's wait for in-flight deliveries protects against a delivery that
// is running, not against a second thread deciding to reload while the main
// thread is mid-frame in the same script. So the queue is the whole interface
// between the two threads, and the host drains it from on_frame.
//
// Start/Stop are for one thread (the host's main thread); TakeChanged may be
// called from any, and is what the worker's output crosses on.
class Watcher {
  public:
    Watcher() noexcept = default;
    ~Watcher();

    Watcher(const Watcher&) = delete;
    Watcher& operator=(const Watcher&) = delete;

    // Opens `folder` and starts watching it. False if the folder cannot be
    // opened - it does not exist, or is a file, or the process may not read
    // it - and in that case nothing is started and TakeChanged stays empty
    // forever. A host that cannot watch its dev folder is a host without hot
    // reload, not a host that crashes.
    //
    // Not recursive: `mods/lua-dev/` is a flat folder of scripts. A subfolder
    // is a place to keep notes, not a place this host looks for code.
    //
    // Calling Start on a running watcher stops it first.
    bool Start(const std::wstring& folder);

    // The same, with the quiet period stated. Exists so the reload tests can
    // run fifty cycles without spending fifty times 200 ms of wall clock in a
    // suite that has to stay runnable; the host itself uses kDefaultDebounce
    // and there is a test that says so.
    bool Start(const std::wstring& folder, std::chrono::milliseconds debounce);

    // Stops the worker and joins it. Idempotent, and safe to call on a watcher
    // that never started. After it returns true, no further path can appear in
    // the queue and this object may be destroyed.
    //
    // **False means the worker could not be joined, and this object must be
    // leaked rather than destroyed.** std::thread::join throws
    // std::system_error, and out of a noexcept function that is std::terminate
    // - a hard kill of the game, which is the outcome the whole project's
    // no-crash rule exists to prevent, arriving by a different route. So the
    // join is guarded. But the recovery cannot be "carry on and delete": a
    // worker this call failed to join may still be inside the kernel, writing
    // into buffer_ and signalling change_event_, and freeing those under it is
    // strictly worse than the crash. Leaking one watcher and its handles is the
    // least bad third option, and the caller is the only one that can choose
    // it, so the answer is returned rather than swallowed.
    //
    // In practice join() throws only for what are programming errors here
    // (joining from the worker itself, or a thread that was never valid). This
    // is a boundary, not an expected path.
    bool Stop() noexcept;

    // Whether this object may be deleted. False once any Stop() has failed to
    // join the worker, for the life of the object - see Stop.
    //
    // Stop()'s own return value already tells a caller that owns the call.
    // This exists for the caller that does not: **Start() stops internally,
    // twice**, and its bool answers only "is it watching". A Start that
    // returned false because its internal Stop failed leaves a detached worker
    // still writing into buffer_ and signalling change_event_, and a caller
    // that responds by deleting the Watcher frees that memory under the
    // running thread. ~Watcher cannot save it either: the destructor's own
    // Stop() sees the poisoned state and declines to touch a handle, but the
    // object's storage is gone the moment delete returns.
    //
    // So: a Watcher whose Start returned false must be asked this before it is
    // freed, exactly as ShutDown asks Stop() before freeing g_watcher.
    // Leaking one watcher costs a thread and four handles for the life of the
    // process, which is the cheaper of the two failures by a long way.
    bool SafeToDestroy() const noexcept { return !detached_.load(); }

    // Whether a worker thread is alive for this watcher.
    //
    // True from a successful Start until the worker leaves - which is usually
    // Stop, but is also the worker ending on its own when the folder it was
    // watching is deleted or renamed out from under it. That second case used
    // to leave this true forever, so Running() and the host's debounce accessor
    // both reported a live 200 ms watcher for a thread that had already exited.
    // The worker clears it as it leaves, which is why it is atomic.
    bool Running() const noexcept { return running_.load(); }

    // Every path that has changed and then gone quiet for the debounce window.
    // Empty is the ordinary answer - it is called once a frame.
    std::vector<std::wstring> TakeChanged() noexcept;

    const std::wstring& Folder() const noexcept { return folder_; }
    std::chrono::milliseconds Debounce() const noexcept {
        return queue_.Debounce();
    }

  private:
    void Run() noexcept;

    // HANDLEs. Kept as void* so this header pulls in no windows.h - HANDLE is
    // void* already, so the .cpp needs no cast in either direction. nullptr,
    // never INVALID_HANDLE_VALUE, is what "closed" means here: one spelling
    // for "there is no handle" is worth more than fidelity to CreateFileW's
    // two.
    void* directory_ = nullptr;
    // Signalled by the kernel when a read completes. A member rather than a
    // local in Run() for the same reason buffer_ is: Stop() cancels an I/O
    // that is still allowed to complete into both of them.
    void* change_event_ = nullptr;
    void* stop_event_ = nullptr;
    // Signalled by the worker once its first ReadDirectoryChangesW has been
    // issued, and waited on by Start before it returns.
    //
    // Without it, Start returns as soon as the thread exists - which is before
    // the folder is actually being watched, because arming the read is the
    // first thing the new thread does and it has not necessarily been
    // scheduled yet. Every change made in that window is lost, silently and
    // permanently: there is no notification to miss, the kernel was not
    // watching. It reached this codebase as an intermittently failing test,
    // which is the lucky version of a mod author saving a file during startup
    // and wondering why it never loaded.
    void* armed_event_ = nullptr;

    std::thread worker_;
    std::wstring folder_;
    // Written by Start on the caller's thread and by the worker as it leaves,
    // read from either. See Running().
    std::atomic<bool> running_{false};
    // Written by the worker before it signals armed_event_, read by Start
    // after it waits on it - which is the whole of the synchronisation. False
    // means the first read failed and there is nothing to watch, so Start
    // reports failure rather than a watcher that will never report anything.
    // atomic, because it is written on the worker and read on the caller's
    // thread. WaitForSingleObject on the event beside it does order the two in
    // practice, but a Win32 wait is not something the C++ memory model has an
    // opinion about, and "the platform happens to make this safe" is not what
    // the next reader should have to work out.
    std::atomic<bool> first_read_ok_{false};

    // Set by Stop() when a join fails and the worker has been detached, and
    // never cleared: nothing can prove a detached thread has left, so no later
    // call may decide it has. Read by SafeToDestroy on another thread, hence
    // atomic. See Stop.
    std::atomic<bool> detached_{false};

    // 64 KB, and a member rather than a local in Run() because the buffer must
    // stay alive until an I/O cancelled by Stop() has actually completed.
    std::vector<unsigned char> buffer_;

    // Last, so it outlives the worker that pushes into it: members are
    // destroyed in reverse order, and ~Watcher joins the worker before any of
    // this runs anyway, but the declaration order says the same thing to a
    // reader who has not read the destructor yet.
    ChangeQueue queue_;
};

}  // namespace lua
}  // namespace wotbmod

#endif  // WOTBMOD_LUA_WATCHER_H_
