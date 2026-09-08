/*
 * Host double for loader/v3_hook_observers: a fake detour with the exact
 * shape the loader's detours have (observers, original, observers) drives
 * ordering, enable/disable, fault isolation and detach-during-walk.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../loader/v3_hook_observers.h"

#define CHECK(expression)                                          \
    do {                                                           \
        if (!(expression)) {                                       \
            std::fprintf(stderr, "check failed at line %d: %s\n", \
                         __LINE__, #expression);                   \
            return 1;                                              \
        }                                                          \
    } while (0)

using namespace wotbmod::loader::observers;

namespace {

typedef void(__thiscall* SetHealthFn)(void* self, const int16_t* health);

std::vector<std::string> g_calls;
uint64_t g_self_token = 0u;
int g_target_marker = 0;

const void* Target() { return &g_target_marker; }

void __fastcall ObserverA(void*, void*, const int16_t* health) {
    g_calls.push_back("A:" + std::to_string(*health));
}
void __fastcall ObserverB(void*, void*, const int16_t* health) {
    g_calls.push_back("B:" + std::to_string(*health));
}
void __fastcall ObserverAfter(void*, void*, const int16_t* health) {
    g_calls.push_back("after:" + std::to_string(*health));
}
void __fastcall ObserverFaulting(void*, void*, const int16_t*) {
    volatile int* bad = nullptr;
    *bad = 1;
}
void __fastcall ObserverDetachingSelf(void*, void*, const int16_t*) {
    g_calls.push_back("self");
    Detach(g_self_token);
}

/* The loader detour, verbatim shape. */
void __fastcall FakeDetour(void* self, void*, const int16_t* health) {
    Before<SetHealthFn>(Target(), self, health);
    g_calls.push_back("original");
    After<SetHealthFn>(Target(), self, health);
}

void Log(const char* line) { g_calls.push_back(std::string("log:") + line); }

}  // namespace

int main() {
    SetLog(&Log);
    CHECK(Describe(Target()) == 0u);
    uint64_t token = 0u;
    CHECK(Attach(Target(), WOTBMOD_V3_HOOK_OBSERVE, 0,
                 reinterpret_cast<void*>(&ObserverA), &token) == WOTBMOD_V3_E_NOT_FOUND);
    RegisterTarget(Target(), kModeObserve | kModeAfter);
    CHECK(Describe(Target()) == (kModeObserve | kModeAfter));
    CHECK(Attach(Target(), WOTBMOD_V3_HOOK_BEFORE, 0,
                 reinterpret_cast<void*>(&ObserverA), &token) == WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(Attach(Target(), WOTBMOD_V3_HOOK_OBSERVE, 0, nullptr, &token) ==
          WOTBMOD_V3_E_INVALID_ARGUMENT);

    uint64_t a = 0u;
    uint64_t b = 0u;
    uint64_t after = 0u;
    CHECK(Attach(Target(), WOTBMOD_V3_HOOK_OBSERVE, 0,
                 reinterpret_cast<void*>(&ObserverA), &a) == WOTBMOD_V3_OK);
    CHECK(Attach(Target(), WOTBMOD_V3_HOOK_OBSERVE, 10,
                 reinterpret_cast<void*>(&ObserverB), &b) == WOTBMOD_V3_OK);
    CHECK(Attach(Target(), WOTBMOD_V3_HOOK_AFTER, 0,
                 reinterpret_cast<void*>(&ObserverAfter), &after) == WOTBMOD_V3_OK);
    CHECK(a != 0u && b != 0u && after != 0u && a != b && b != after);
    CHECK(ObserverCount(Target()) == 3u);

    int16_t health = 7;
    FakeDetour(&g_target_marker, nullptr, &health);
    CHECK(g_calls.size() == 4u && g_calls[0] == "B:7" && g_calls[1] == "A:7" &&
          g_calls[2] == "original" && g_calls[3] == "after:7");

    g_calls.clear();
    CHECK(SetEnabled(b, false) == WOTBMOD_V3_OK);
    FakeDetour(&g_target_marker, nullptr, &health);
    CHECK(g_calls.size() == 3u && g_calls[0] == "A:7");
    CHECK(SetEnabled(b, true) == WOTBMOD_V3_OK);
    CHECK(SetEnabled(12345u, true) == WOTBMOD_V3_E_NOT_FOUND);

    /* A faulting observer is disabled and logged once; the original and the
     * other observers still run. */
    uint64_t faulting = 0u;
    CHECK(Attach(Target(), WOTBMOD_V3_HOOK_OBSERVE, 100,
                 reinterpret_cast<void*>(&ObserverFaulting), &faulting) == WOTBMOD_V3_OK);
    g_calls.clear();
    FakeDetour(&g_target_marker, nullptr, &health);
    CHECK(FaultedCount() == 1u);
    CHECK(g_calls.size() == 5u && g_calls[0].rfind("log:", 0) == 0 &&
          g_calls[0].find("observer disabled") != std::string::npos &&
          g_calls[1] == "B:7" && g_calls[2] == "A:7" && g_calls[3] == "original" &&
          g_calls[4] == "after:7");
    g_calls.clear();
    FakeDetour(&g_target_marker, nullptr, &health);
    CHECK(g_calls.size() == 4u && FaultedCount() == 1u);

    /* Detach from inside a callback is safe: the walk continues on its
     * snapshot and the entry is gone afterwards. */
    CHECK(Attach(Target(), WOTBMOD_V3_HOOK_OBSERVE, 50,
                 reinterpret_cast<void*>(&ObserverDetachingSelf), &g_self_token) == WOTBMOD_V3_OK);
    g_calls.clear();
    FakeDetour(&g_target_marker, nullptr, &health);
    CHECK(g_calls.size() == 5u && g_calls[0] == "self" && g_calls[1] == "B:7");
    CHECK(ObserverCount(Target()) == 4u);
    CHECK(Detach(g_self_token) == WOTBMOD_V3_E_NOT_FOUND);

    CHECK(Detach(a) == WOTBMOD_V3_OK && Detach(b) == WOTBMOD_V3_OK &&
          Detach(after) == WOTBMOD_V3_OK && Detach(faulting) == WOTBMOD_V3_OK);
    CHECK(ObserverCount(Target()) == 0u);
    uint64_t again = 0u;
    CHECK(Attach(Target(), WOTBMOD_V3_HOOK_AFTER, 0,
                 reinterpret_cast<void*>(&ObserverAfter), &again) == WOTBMOD_V3_OK);
    CHECK(again > faulting);
    UnregisterAll();
    CHECK(Describe(Target()) == 0u && ObserverCount(Target()) == 0u);
    CHECK(Detach(again) == WOTBMOD_V3_E_NOT_FOUND);

    std::printf("V3 hook observers: host-double checks passed\n");
    return 0;
}
