#include "lua_watcher.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cwctype>

namespace wotbmod {
namespace lua {
namespace {

// 64 KiB of FILE_NOTIFY_INFORMATION records. A dev folder holds a handful of
// scripts and an editor saves one at a time, so this is generous rather than
// tuned - and an overflow is *handled* (PushFolderContents) rather than merely
// made unlikely, because "the save silently did not reload" is the one failure
// this feature cannot afford.
constexpr size_t kBufferBytes = 64u * 1024u;

constexpr DWORD kNotifyFilter =
    FILE_NOTIFY_CHANGE_FILE_NAME |
    FILE_NOTIFY_CHANGE_LAST_WRITE |
    FILE_NOTIFY_CHANGE_SIZE |
    FILE_NOTIFY_CHANGE_CREATION;

void CloseIfOpen(void*& handle) noexcept {
    if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    handle = nullptr;
}

// Read from a thread that is not the one writing it, by definition. See
// RunningWatcherThreads in the header for why it exists at all.
std::atomic<size_t>& WorkerCount() noexcept {
    static std::atomic<size_t> count{0u};
    return count;
}

// Increments while a worker's loop is running. Scope-based rather than a pair
// of calls, because every path out of Run() is a return and one of them is
// easy to add without noticing.
struct WorkerTicket {
    WorkerTicket() noexcept { WorkerCount().fetch_add(1u); }
    ~WorkerTicket() { WorkerCount().fetch_sub(1u); }
    WorkerTicket(const WorkerTicket&) = delete;
    WorkerTicket& operator=(const WorkerTicket&) = delete;
};

}  // namespace

size_t RunningWatcherThreads() noexcept { return WorkerCount().load(); }

std::wstring JoinPath(const std::wstring& folder, const std::wstring& name) {
    if (folder.empty()) return name;
    if (name.empty()) return folder;
    std::wstring joined = folder;
    const wchar_t last = joined.back();
    if (last != L'\\' && last != L'/') joined.push_back(L'\\');
    return joined + name;
}

void PushFolderContents(ChangeQueue& queue, const std::wstring& folder) noexcept {
    WIN32_FIND_DATAW found = {};
    HANDLE search = INVALID_HANDLE_VALUE;
    try {
        search = FindFirstFileW(JoinPath(folder, L"*").c_str(), &found);
    } catch (...) {
        return;
    }
    if (search == INVALID_HANDLE_VALUE) return;
    do {
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        try {
            queue.Push(JoinPath(folder, found.cFileName),
                       ChangeQueue::Clock::now());
        } catch (...) {
            // One name dropped for want of memory; the rest still go in.
        }
    } while (FindNextFileW(search, &found));
    FindClose(search);
}

// towlower per character rather than CompareStringOrdinal: this produces a map
// key, which needs a canonical form rather than a comparison. See the header
// for what that simplification costs and why it is acceptable.
std::wstring FoldPathKey(const std::wstring& path) {
    std::wstring folded;
    folded.reserve(path.size());
    for (const wchar_t character : path) {
        const wchar_t normalized = character == L'/' ? L'\\' : character;
        folded.push_back(static_cast<wchar_t>(towlower(normalized)));
    }
    return folded;
}

// ---------------------------------------------------------------------------
// ChangeQueue
// ---------------------------------------------------------------------------

void ChangeQueue::Push(const std::wstring& path,
                       Clock::time_point now) noexcept {
    if (path.empty()) return;
    try {
        const std::wstring key = FoldPathKey(path);
        std::lock_guard<std::mutex> guard(lock_);
        const auto existing = pending_.find(key);
        if (existing != pending_.end()) {
            // Not a second entry: the same entry, with its quiet period
            // restarted. This one line is the whole debounce - a save that
            // produces four notifications produces one reload because the
            // second, third and fourth land here.
            existing->second.last = now;
            return;
        }
        Pending entry;
        entry.path = path;
        entry.last = now;
        pending_.emplace(key, std::move(entry));
    } catch (...) {
        // Out of memory recording one change, on a thread with nobody to
        // report it to. Dropping it costs one reload; letting it escape costs
        // the game. The next save records the file again.
    }
}

std::vector<std::wstring> ChangeQueue::Take(Clock::time_point now) noexcept {
    std::vector<std::wstring> ready;
    try {
        std::lock_guard<std::mutex> guard(lock_);
        for (auto it = pending_.begin(); it != pending_.end();) {
            // >=, not >: a debounce of 200 ms means a file quiet for exactly
            // 200 ms is ready. The strict form would make the interval
            // "200 ms and one tick", which is untestable and not what anyone
            // means by it.
            if (now - it->second.last >= debounce_) {
                ready.push_back(it->second.path);
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
    } catch (...) {
        // Whatever was collected before the allocation failed is returned and
        // has been erased, so nothing is reported twice and nothing is lost
        // that a later save will not report again.
    }
    return ready;
}

void ChangeQueue::Clear() noexcept {
    std::lock_guard<std::mutex> guard(lock_);
    pending_.clear();
}

size_t ChangeQueue::PendingCount() const noexcept {
    std::lock_guard<std::mutex> guard(lock_);
    return pending_.size();
}

std::chrono::milliseconds ChangeQueue::Debounce() const noexcept {
    std::lock_guard<std::mutex> guard(lock_);
    return debounce_;
}

void ChangeQueue::SetDebounce(std::chrono::milliseconds debounce) noexcept {
    std::lock_guard<std::mutex> guard(lock_);
    debounce_ = debounce;
}

// ---------------------------------------------------------------------------
// Watcher
// ---------------------------------------------------------------------------

// The one caller that cannot act on a failed Stop, and the reason the host
// allocates its watcher on the heap rather than holding one by value: there is
// no third option left here. If the join failed, the members below are about to
// be freed under a worker that may still be writing into them - so a Watcher
// that could not be stopped must never reach this destructor at all. See
// ShutDown in lua_host_mod.cpp, which leaks the object instead.
Watcher::~Watcher() { (void)Stop(); }

bool Watcher::Start(const std::wstring& folder) {
    return Start(folder, kDefaultDebounce);
}

bool Watcher::Start(const std::wstring& folder,
                    std::chrono::milliseconds debounce) {
    // A Start that cannot stop the previous run cannot proceed: the handles
    // below would be overwritten while a worker still held them.
    if (!Stop()) return false;
    if (folder.empty()) return false;

    void* directory = CreateFileW(
        folder.c_str(), FILE_LIST_DIRECTORY,
        // Every share flag, deliberately. This handle must never be the reason
        // an editor cannot save, a build cannot replace a file, or the author
        // cannot delete the folder while the game is running. A watcher that
        // locks what it watches is worse than no watcher.
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING,
        // BACKUP_SEMANTICS is what lets CreateFileW open a directory at all.
        // OVERLAPPED is what lets Stop() interrupt the read below instead of
        // leaving the worker blocked in the kernel until something happens to
        // change - which, in a folder nobody is editing, is never.
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (directory == INVALID_HANDLE_VALUE) return false;

    // Both manual-reset. The change event is reset by this side before each
    // read, which is the caller's job in overlapped I/O; the stop event must
    // stay signalled once set, or a worker that was inside the kernel at the
    // moment it was signalled would never see it and Stop() would hang on the
    // join.
    void* change_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    void* stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    void* armed_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!change_event || !stop_event || !armed_event) {
        CloseIfOpen(change_event);
        CloseIfOpen(stop_event);
        CloseIfOpen(armed_event);
        CloseHandle(directory);
        return false;
    }

    directory_ = directory;
    change_event_ = change_event;
    stop_event_ = stop_event;
    armed_event_ = armed_event;
    first_read_ok_.store(false);
    queue_.SetDebounce(debounce);
    queue_.Clear();
    // Set *before* the thread exists, not after it starts.
    //
    // The worker clears this flag on its way out, which is what makes Running()
    // agree with reality when the watched folder is deleted from under it. With
    // the store after the constructor, a worker that started and exited inside
    // that window would clear a flag that had not been set yet and then have it
    // set true behind its back - Running() stuck true for a thread that is
    // gone. That is the same defect the self-terminating-worker fix closed,
    // reintroduced purely by ordering.
    //
    // It is masked today: the only exit reachable in that window is a failed
    // first read, which first_read_ok_ catches and turns into a false return
    // anyway. "Masked by a second mechanism" is exactly the shape this branch
    // has already agreed not to rely on - see RevokeAll in lua_ownership.h - so
    // the ordering is fixed rather than the masking documented.
    running_.store(true);
    try {
        folder_ = folder;
        buffer_.assign(kBufferBytes, 0u);
        worker_ = std::thread(&Watcher::Run, this);
    } catch (...) {
        // std::thread's constructor throws when a thread cannot be created,
        // and both assignments above can throw bad_alloc. Whichever it was,
        // this object has to come back to exactly the state it was in before
        // the call: otherwise a later Stop() closes handles nothing owns and a
        // later Start() leaks the ones it left behind.
        running_.store(false);
        CloseIfOpen(change_event_);
        CloseIfOpen(stop_event_);
        CloseIfOpen(armed_event_);
        CloseIfOpen(directory_);
        folder_.clear();
        return false;
    }

    // The folder is not being watched until the worker has issued its first
    // read, and Start must not return before then - see armed_event_. A bounded
    // wait rather than INFINITE: this blocks the caller's thread, and a worker
    // that somehow never arms would otherwise hang the game's enable path
    // rather than merely fail to give it hot reload.
    if (WaitForSingleObject(armed_event_, 5000u) != WAIT_OBJECT_0 ||
        !first_read_ok_.load()) {
        // The answer is not dropped, it is *recorded*: a failed join here sets
        // detached_, and SafeToDestroy() is what carries that to the caller.
        // Start's own bool cannot carry both facts - "not watching" and "not
        // safe to free" - and a caller reacting to a false Start by deleting
        // this object needs the second one.
        (void)Stop();
        return false;
    }
    return true;
}

bool Watcher::Stop() noexcept {
    // Sticky, and checked before anything else touches a handle. Once a join
    // has failed, a worker of this object's is running with no way to observe
    // it; a later Stop() that fell through to the CloseIfOpen block below
    // would close change_event_ and buffer_'s directory handle underneath it,
    // which is exactly the damage the failed join declined to do. ~Watcher
    // calls Stop() unconditionally, so without this the destructor of a
    // leaked-on-purpose watcher would undo the reason it was leaked.
    if (detached_.load()) return false;
    if (stop_event_) SetEvent(stop_event_);
    if (worker_.joinable()) {
        try {
            worker_.join();
        } catch (...) {
            // std::thread::join throws std::system_error, and this function is
            // noexcept, so letting it out is std::terminate - a hard kill of
            // the game. That is the outcome this project's no-crash rule exists
            // to prevent, and it arriving from the host rather than from a
            // script does not make it better.
            //
            // But there is no recovery here, only a choice of damage. The
            // worker may still be inside the kernel, writing into buffer_ and
            // signalling change_event_; closing those handles or freeing this
            // object would be strictly worse than the crash. So nothing is
            // closed, nothing is cleared, the thread object is detached so its
            // own destructor cannot terminate either, and the caller is told -
            // the only party that can decide to leak this watcher instead of
            // destroying it.
            //
            // Recorded on the object as well as returned, because the caller
            // that most needs this answer is not the one calling Stop: Start()
            // stops internally and reports only whether it is watching. See
            // SafeToDestroy.
            detached_.store(true);
            try {
                worker_.detach();
            } catch (...) {
            }
            return false;
        }
    }
    CloseIfOpen(change_event_);
    CloseIfOpen(stop_event_);
    CloseIfOpen(armed_event_);
    CloseIfOpen(directory_);
    running_.store(false);
    first_read_ok_.store(false);
    // Dropped rather than left for the next Start: an edit made while nothing
    // was listening is not a change this watcher saw, and delivering it after
    // a restart would reload a script for a reason that no longer exists.
    queue_.Clear();
    return true;
}

std::vector<std::wstring> Watcher::TakeChanged() noexcept {
    return queue_.Take(ChangeQueue::Clock::now());
}

void Watcher::Run() noexcept {
    const WorkerTicket ticket;
    // Cleared on every path out of this function, not only the one Stop takes.
    // The worker also ends on its own when the folder it was watching is
    // deleted or renamed, and leaving this set made Running() - and the host's
    // debounce accessor built on it - report a live 200 ms watcher for a thread
    // that had already exited.
    struct RunningFlag {
        std::atomic<bool>& flag;
        ~RunningFlag() { flag.store(false); }
    } running_flag{running_};
    OVERLAPPED overlapped = {};
    overlapped.hEvent = change_event_;
    HANDLE waits[2] = {change_event_, stop_event_};
    const DWORD buffer_bytes = static_cast<DWORD>(buffer_.size());

    bool armed = false;
    for (;;) {
        ResetEvent(change_event_);
        DWORD returned = 0u;
        const BOOL issued = ReadDirectoryChangesW(
            directory_, buffer_.data(), buffer_bytes, FALSE, kNotifyFilter,
            &returned, &overlapped, nullptr);
        // Released on the *first* read whether it succeeded or not, and before
        // acting on the result - Start is blocked until this happens, so a
        // path out of here that skipped it would hang the caller for the full
        // five-second timeout.
        if (!armed) {
            armed = true;
            first_read_ok_.store(issued != FALSE);
            SetEvent(armed_event_);
        }
        if (!issued) {
            // The directory is gone - deleted, renamed or unmounted under us.
            // There is nothing left to watch and nothing to retry against, so
            // the worker ends here. The host keeps the scripts it has already
            // loaded and simply stops hearing about changes; it does not lose
            // them, and it does not crash.
            return;
        }

        const DWORD signalled =
            WaitForMultipleObjects(2u, waits, FALSE, INFINITE);
        if (signalled != WAIT_OBJECT_0) {
            // Stop, or a wait that failed. Either way a read is still
            // outstanding against buffer_, and it is the kernel - not this
            // thread - that writes into it. Returning without cancelling and
            // then *waiting for the cancellation to land* would let Stop()
            // free the buffer while an I/O was still entitled to complete into
            // it. CancelIo covers only requests this thread issued, which is
            // exactly the set that matters: this thread issued them all.
            CancelIo(directory_);
            DWORD cancelled = 0u;
            GetOverlappedResult(directory_, &overlapped, &cancelled, TRUE);
            return;
        }

        DWORD bytes = 0u;
        if (!GetOverlappedResult(directory_, &overlapped, &bytes, FALSE)) {
            return;
        }
        if (bytes == 0u) {
            // A completed read of zero bytes is how ReadDirectoryChangesW says
            // its buffer overflowed and the notifications were discarded. It
            // is not an error and it must not be ignored: the kernel has said
            // that something changed and that it cannot say what.
            PushFolderContents(queue_, folder_);
            continue;
        }

        const unsigned char* base = buffer_.data();
        DWORD offset = 0u;
        for (;;) {
            const FILE_NOTIFY_INFORMATION* info =
                reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(base + offset);
            // FileNameLength is a count of *bytes*, and FileName is not null
            // terminated - the single most common way this API is misread, and
            // one that reads past the record when it is.
            const size_t characters = info->FileNameLength / sizeof(WCHAR);
            if (characters != 0u) {
                try {
                    queue_.Push(
                        JoinPath(folder_,
                                 std::wstring(info->FileName, characters)),
                        ChangeQueue::Clock::now());
                } catch (...) {
                    // One notification dropped rather than a thread unwound
                    // out of a noexcept function. See ChangeQueue::Push.
                }
            }
            if (info->NextEntryOffset == 0u) break;
            offset += info->NextEntryOffset;
            // A chain that walks past what the kernel actually wrote would
            // read whatever the buffer held from the previous notification.
            // Nothing observed has ever done this; the check costs a compare.
            if (offset >= bytes) break;
        }
    }
}

}  // namespace lua
}  // namespace wotbmod
