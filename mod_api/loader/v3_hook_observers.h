#pragma once
/*
 * Observer registry for wotbmod.hooks OBSERVE and AFTER.
 *
 * A target is a function the loader already detours. Its detour calls
 * Before<Fn>() before the original and After<Fn>() after it, and every mod
 * callback attached to that target runs there with the target's real
 * arguments, in the target's own calling convention (Fn is the detour's
 * typedef). Callbacks are ordered by descending priority, then by attach
 * order; a callback that raises an SEH exception is disabled and logged once;
 * the original always runs.
 *
 * Readers take an immutable snapshot of the observer list, so Attach, Detach
 * and SetEnabled - which publish a new snapshot - never disturb a walk in
 * progress, including a Detach issued from inside a callback.
 *
 * Spec: docs/superpowers/specs/2026-09-04-hook-observers-design.md.
 */
#include <cstdint>
#include <memory>
#include <tuple>
#include <vector>

#include "../include/wotbmod/base.h"
#include "../include/wotbmod/hooks_v1.h"

namespace wotbmod {
namespace loader {
namespace observers {

constexpr uint32_t kModeAfter = 1u << WOTBMOD_V3_HOOK_AFTER;
constexpr uint32_t kModeObserve = 1u << WOTBMOD_V3_HOOK_OBSERVE;

struct Observer {
    uint64_t token;
    uint32_t mode;      /* WotbModV3HookMode */
    int32_t priority;
    void* detour;
    bool enabled;
    bool faulted;
};

struct Snapshot {
    std::vector<Observer> items;   /* sorted: priority desc, token asc */
};

/* Current snapshot of a target's observers, or null for an unknown target. */
std::shared_ptr<const Snapshot> Acquire(const void* target);

/* Calls invoke(context) under an SEH guard. On a fault the observer named by
 * token is disabled, marked faulted and logged once; returns false. */
bool GuardedInvoke(uint64_t token, void (*invoke)(void*), void* context);

/* Targets are registered by the code that installed their detour; `modes` is
 * a mask of kMode* bits the detour actually calls. Re-registering updates the
 * mask and keeps the observers. */
void RegisterTarget(const void* target, uint32_t modes);
/* Forgets every target and observer; walks in progress finish on their own
 * snapshot. */
void UnregisterAll();
/* Mode mask for a registered target, 0 otherwise. */
uint32_t Describe(const void* target);

WotbModV3Result Attach(const void* target, uint32_t mode, int32_t priority,
                       void* detour, uint64_t* out_token);
WotbModV3Result Detach(uint64_t token);
WotbModV3Result SetEnabled(uint64_t token, bool enabled);

uint32_t ObserverCount(const void* target);
uint32_t FaultedCount();
void SetLog(void (*log)(const char*));

namespace detail {

template <typename Fn, typename... Args>
struct Call {
    Fn fn;
    std::tuple<Args...> args;
    static void Invoke(void* context) {
        Call* self = static_cast<Call*>(context);
        std::apply(self->fn, self->args);
    }
};

template <typename Fn, typename... Args>
void Run(const void* target, uint32_t mode, Args... args) {
    const std::shared_ptr<const Snapshot> snapshot = Acquire(target);
    if (!snapshot) return;
    for (const Observer& observer : snapshot->items) {
        if (observer.mode != mode || !observer.enabled || observer.faulted) {
            continue;
        }
        Call<Fn, Args...> call = {reinterpret_cast<Fn>(observer.detour),
                                  std::tuple<Args...>(args...)};
        GuardedInvoke(observer.token, &Call<Fn, Args...>::Invoke, &call);
    }
}

}  // namespace detail

/* OBSERVE phase: call from the detour before the original. */
template <typename Fn, typename... Args>
void Before(const void* target, Args... args) {
    detail::Run<Fn, Args...>(target, WOTBMOD_V3_HOOK_OBSERVE, args...);
}

/* AFTER phase: call from the detour after the original returned. */
template <typename Fn, typename... Args>
void After(const void* target, Args... args) {
    detail::Run<Fn, Args...>(target, WOTBMOD_V3_HOOK_AFTER, args...);
}

}  // namespace observers
}  // namespace loader
}  // namespace wotbmod
