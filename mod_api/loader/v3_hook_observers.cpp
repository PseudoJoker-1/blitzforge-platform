#include "v3_hook_observers.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstdio>

namespace wotbmod {
namespace loader {
namespace observers {
namespace {

struct Target {
    const void* address;
    uint32_t modes;
    std::shared_ptr<const Snapshot> snapshot;   /* never null once registered */
};

struct State {
    SRWLOCK lock = SRWLOCK_INIT;
    std::vector<Target> targets;
    uint64_t next_token = 1u;
    uint32_t faulted = 0u;
    void (*log)(const char*) = nullptr;
};

State& S() {
    static State state;
    return state;
}

struct SharedLock {
    explicit SharedLock(SRWLOCK* lock) : lock_(lock) { AcquireSRWLockShared(lock_); }
    ~SharedLock() { ReleaseSRWLockShared(lock_); }
    SharedLock(const SharedLock&) = delete;
    SharedLock& operator=(const SharedLock&) = delete;
    SRWLOCK* lock_;
};

struct ExclusiveLock {
    explicit ExclusiveLock(SRWLOCK* lock) : lock_(lock) { AcquireSRWLockExclusive(lock_); }
    ~ExclusiveLock() { ReleaseSRWLockExclusive(lock_); }
    ExclusiveLock(const ExclusiveLock&) = delete;
    ExclusiveLock& operator=(const ExclusiveLock&) = delete;
    SRWLOCK* lock_;
};

Target* FindTargetLocked(const void* address) {
    for (Target& target : S().targets) {
        if (target.address == address) return &target;
    }
    return nullptr;
}

Target* FindByTokenLocked(uint64_t token, size_t* out_index) {
    for (Target& target : S().targets) {
        const std::vector<Observer>& items = target.snapshot->items;
        for (size_t i = 0u; i < items.size(); ++i) {
            if (items[i].token == token) {
                *out_index = i;
                return &target;
            }
        }
    }
    return nullptr;
}

bool Earlier(const Observer& left, const Observer& right) {
    if (left.priority != right.priority) return left.priority > right.priority;
    return left.token < right.token;
}

/* Replaces the target's snapshot; the previous one stays alive for whoever
 * is walking it. */
void PublishLocked(Target& target, std::vector<Observer> items) {
    std::stable_sort(items.begin(), items.end(), &Earlier);
    std::shared_ptr<Snapshot> next = std::make_shared<Snapshot>();
    next->items = std::move(items);
    target.snapshot = next;
}

void Log(const char* line) {
    void (*log)(const char*) = S().log;
    if (log) log(line);
}

/* POD-only frame: SEH is legal here. An MSVC C++ exception is an SEH
 * exception (0xE06D7363) and is caught the same way. */
bool TryInvoke(void (*invoke)(void*), void* context) {
    __try {
        invoke(context);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

std::shared_ptr<const Snapshot> Acquire(const void* target) {
    State& s = S();
    SharedLock lock(&s.lock);
    Target* found = FindTargetLocked(target);
    return found ? found->snapshot : std::shared_ptr<const Snapshot>();
}

bool GuardedInvoke(uint64_t token, void (*invoke)(void*), void* context) {
    if (TryInvoke(invoke, context)) return true;
    State& s = S();
    char line[160] = {};
    {
        ExclusiveLock lock(&s.lock);
        size_t index = 0u;
        Target* target = FindByTokenLocked(token, &index);
        if (!target || target->snapshot->items[index].faulted) return false;
        std::vector<Observer> items = target->snapshot->items;
        items[index].faulted = true;
        items[index].enabled = false;
        ++s.faulted;
        PublishLocked(*target, std::move(items));
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "[v3] hook observer faulted: target=%p token=%llu; observer disabled",
                    target->address, static_cast<unsigned long long>(token));
    }
    Log(line);
    return false;
}

void RegisterTarget(const void* target, uint32_t modes) {
    if (!target) return;
    State& s = S();
    ExclusiveLock lock(&s.lock);
    Target* existing = FindTargetLocked(target);
    if (existing) {
        existing->modes = modes;
        return;
    }
    Target entry;
    entry.address = target;
    entry.modes = modes;
    entry.snapshot = std::make_shared<const Snapshot>();
    s.targets.push_back(entry);
}

void UnregisterAll() {
    State& s = S();
    ExclusiveLock lock(&s.lock);
    s.targets.clear();
}

uint32_t Describe(const void* target) {
    State& s = S();
    SharedLock lock(&s.lock);
    Target* found = FindTargetLocked(target);
    return found ? found->modes : 0u;
}

WotbModV3Result Attach(const void* target, uint32_t mode, int32_t priority,
                       void* detour, uint64_t* out_token) {
    if (!out_token) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    *out_token = 0u;
    if (!target || !detour || mode > WOTBMOD_V3_HOOK_OBSERVE) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    State& s = S();
    ExclusiveLock lock(&s.lock);
    Target* found = FindTargetLocked(target);
    if (!found) return WOTBMOD_V3_E_NOT_FOUND;
    if ((found->modes & (1u << mode)) == 0u) return WOTBMOD_V3_E_NOT_SUPPORTED;
    Observer observer = {};
    observer.token = s.next_token++;
    observer.mode = mode;
    observer.priority = priority;
    observer.detour = detour;
    observer.enabled = true;
    observer.faulted = false;
    std::vector<Observer> items = found->snapshot->items;
    items.push_back(observer);
    PublishLocked(*found, std::move(items));
    *out_token = observer.token;
    return WOTBMOD_V3_OK;
}

WotbModV3Result Detach(uint64_t token) {
    State& s = S();
    ExclusiveLock lock(&s.lock);
    size_t index = 0u;
    Target* target = FindByTokenLocked(token, &index);
    if (!target) return WOTBMOD_V3_E_NOT_FOUND;
    std::vector<Observer> items = target->snapshot->items;
    items.erase(items.begin() + static_cast<std::ptrdiff_t>(index));
    PublishLocked(*target, std::move(items));
    return WOTBMOD_V3_OK;
}

WotbModV3Result SetEnabled(uint64_t token, bool enabled) {
    State& s = S();
    ExclusiveLock lock(&s.lock);
    size_t index = 0u;
    Target* target = FindByTokenLocked(token, &index);
    if (!target) return WOTBMOD_V3_E_NOT_FOUND;
    if (target->snapshot->items[index].enabled == enabled) return WOTBMOD_V3_OK;
    std::vector<Observer> items = target->snapshot->items;
    items[index].enabled = enabled;
    PublishLocked(*target, std::move(items));
    return WOTBMOD_V3_OK;
}

uint32_t ObserverCount(const void* target) {
    State& s = S();
    SharedLock lock(&s.lock);
    Target* found = FindTargetLocked(target);
    return found ? static_cast<uint32_t>(found->snapshot->items.size()) : 0u;
}

uint32_t FaultedCount() {
    State& s = S();
    SharedLock lock(&s.lock);
    return s.faulted;
}

void SetLog(void (*log)(const char*)) {
    State& s = S();
    ExclusiveLock lock(&s.lock);
    s.log = log;
}

}  // namespace observers
}  // namespace loader
}  // namespace wotbmod
