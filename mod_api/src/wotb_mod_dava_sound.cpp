#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../include/wotb_mod_dava_sound.h"
#include "../loader/anchor_rvas.h"

namespace {

const uint32_t kContextMagic = 0x53415644u; /* DVAS */
const uint32_t kEventMagic = 0x544E4553u;   /* SENT */

struct DavaSoundContext {
    uint32_t magic;
    HMODULE game_module;
    uint8_t* image_base;
    uint32_t image_size;
    uint32_t sound_system_singleton_rva;
    uint32_t fast_name_ctor_rva;
    uint32_t ref_counted_release_rva;
    uint32_t default_sound_group_rva;
    uint32_t sound_system_vtable_rva;
    uint32_t sound_system_proxy_vtable_rva;
    uint32_t hybrid_event_vtable_rva;
    uint32_t wwise_event_vtable_rva;
    volatile LONG active_events;
};

struct DavaSoundEvent {
    uint32_t magic;
    DavaSoundContext* context;
    void* native_event;
    volatile LONG state;
};

static uint32_t SelectRva(uint32_t supplied, uint32_t fallback) {
    return supplied ? supplied : fallback;
}

static uint32_t SelectOptionRva(
    const WotbModDavaSoundOptions* options,
    size_t fieldOffset,
    uint32_t fallback) {
    if (!options ||
        options->struct_size < fieldOffset + sizeof(uint32_t)) {
        return fallback;
    }
    const uint32_t supplied = *reinterpret_cast<const uint32_t*>(
        reinterpret_cast<const uint8_t*>(options) + fieldOffset);
    return SelectRva(supplied, fallback);
}

static bool ReadImageSize(HMODULE module, uint32_t* outSize) {
    if (!module || !outSize) return false;
    __try {
        const uint8_t* base = reinterpret_cast<const uint8_t*>(module);
        const IMAGE_DOS_HEADER* dos =
            reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
            return false;
        }
        const IMAGE_NT_HEADERS32* nt =
            reinterpret_cast<const IMAGE_NT_HEADERS32*>(
                base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
            nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386) {
            return false;
        }
        *outSize = nt->OptionalHeader.SizeOfImage;
        return *outSize != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool RvaInImage(
    const DavaSoundContext* context,
    uint32_t rva,
    size_t size) {
    if (!context || !context->image_base || rva >= context->image_size) {
        return false;
    }
    return size <= context->image_size - rva;
}

static void* Address(
    const DavaSoundContext* context,
    uint32_t rva,
    size_t size = 1) {
    return RvaInImage(context, rva, size)
               ? context->image_base + rva
               : nullptr;
}

static bool IsExecutableAddress(const void* address) {
    if (!address) return false;
    MEMORY_BASIC_INFORMATION info = {};
    if (!VirtualQuery(address, &info, sizeof(info)) ||
        info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    const DWORD protection = info.Protect & 0xFFu;
    return protection == PAGE_EXECUTE ||
           protection == PAGE_EXECUTE_READ ||
           protection == PAGE_EXECUTE_READWRITE ||
           protection == PAGE_EXECUTE_WRITECOPY;
}

static DavaSoundContext* ValidContext(void* userData) {
    DavaSoundContext* context =
        static_cast<DavaSoundContext*>(userData);
    return context && context->magic == kContextMagic ? context : nullptr;
}

static DavaSoundEvent* ValidEvent(
    DavaSoundContext* context,
    void* nativeEvent) {
    DavaSoundEvent* event =
        static_cast<DavaSoundEvent*>(nativeEvent);
    return event &&
                   event->magic == kEventMagic &&
                   event->context == context &&
                   event->native_event
               ? event
               : nullptr;
}

static bool ValidateSystem(DavaSoundContext* context, void** outSystem) {
    if (!context || !outSystem) return false;
    *outSystem = nullptr;
    void** singleton = static_cast<void**>(Address(
        context,
        context->sound_system_singleton_rva,
        sizeof(void*)));
    if (!singleton) return false;

    __try {
        void* system = *singleton;
        if (!system) return false;
        void** vtable = *reinterpret_cast<void***>(system);
        void* expected = Address(
            context,
            context->sound_system_vtable_rva,
            sizeof(void*));
        void* expectedProxy = Address(
            context,
            context->sound_system_proxy_vtable_rva,
            sizeof(void*));
        if (!vtable ||
            (vtable != expected && vtable != expectedProxy)) {
            return false;
        }
        if (!IsExecutableAddress(vtable[3])) return false;
        *outSystem = system;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool ValidateNativeEvent(
    DavaSoundContext* context,
    void* nativeEvent) {
    if (!context || !nativeEvent) return false;
    __try {
        void** vtable = *reinterpret_cast<void***>(nativeEvent);
        void* expectedHybrid = Address(
            context,
            context->hybrid_event_vtable_rva,
            sizeof(void*));
        void* expectedWwise = Address(
            context,
            context->wwise_event_vtable_rva,
            sizeof(void*));
        return vtable &&
               (vtable == expectedHybrid || vtable == expectedWwise) &&
               IsExecutableAddress(vtable[4]) &&
               IsExecutableAddress(vtable[5]) &&
               IsExecutableAddress(vtable[6]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

typedef void*(__thiscall* FastNameCtorFn)(void*, const char*);
typedef void(__thiscall* RefCountedReleaseFn)(void*);
typedef void*(__thiscall* CreateSoundEventFn)(
    void*, const void*, const int32_t*);
typedef void(__thiscall* EventVoidFn)(void*);
typedef void(__thiscall* EventStopFn)(void*, bool);
typedef void(__thiscall* EventPausedFn)(void*, bool);
typedef void(__thiscall* EventVolumeFn)(void*, float);
typedef void(__thiscall* EventPositionFn)(void*, const float*);
typedef void(__thiscall* EventIntegerFn)(void*, int32_t);
typedef void(__thiscall* EventSetParameterFn)(
    void*, const void*, float);
typedef float(__thiscall* EventGetParameterFn)(void*, const void*);
typedef int(__thiscall* EventHasParameterFn)(void*, const void*);

struct FastNameScope {
    void* value;
};

static bool MakeFastName(
    DavaSoundContext* context,
    const char* name,
    FastNameScope* outName) {
    if (!context || !name || !name[0] || !outName) return false;
    outName->value = nullptr;
    FastNameCtorFn constructor =
        reinterpret_cast<FastNameCtorFn>(Address(
            context, context->fast_name_ctor_rva));
    if (!constructor || !IsExecutableAddress((const void*)constructor)) {
        return false;
    }
    constructor(outName, name);
    return outName->value != nullptr;
}

static void ReleaseFastName(
    DavaSoundContext* context,
    FastNameScope* name) {
    if (!context || !name || !name->value) return;
    RefCountedReleaseFn release =
        reinterpret_cast<RefCountedReleaseFn>(Address(
            context, context->ref_counted_release_rva));
    if (release && IsExecutableAddress((const void*)release)) {
        release(name->value);
    }
    name->value = nullptr;
}

static WotbModResult WOTBMOD_CALL CreateEvent(
    void* userData,
    const char* eventName,
    void** outNativeEvent) {
    DavaSoundContext* context = ValidContext(userData);
    if (!context || !eventName || !eventName[0] || !outNativeEvent) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    *outNativeEvent = nullptr;
    void* system = nullptr;
    if (!ValidateSystem(context, &system)) {
        return WOTBMOD_ERROR_PLATFORM;
    }

    FastNameScope fastName = {};
    if (!MakeFastName(context, eventName, &fastName)) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    void* eventObject = nullptr;
    __try {
        void** vtable = *reinterpret_cast<void***>(system);
        CreateSoundEventFn create =
            reinterpret_cast<CreateSoundEventFn>(vtable[3]);
        const int32_t* group = static_cast<const int32_t*>(Address(
            context, context->default_sound_group_rva, sizeof(int32_t)));
        if (create && group) {
            eventObject = create(system, &fastName, group);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        eventObject = nullptr;
    }
    ReleaseFastName(context, &fastName);
    if (!eventObject || !ValidateNativeEvent(context, eventObject)) {
        if (eventObject) {
            RefCountedReleaseFn release =
                reinterpret_cast<RefCountedReleaseFn>(Address(
                    context, context->ref_counted_release_rva));
            if (release) {
                __try {
                    release(eventObject);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                }
            }
        }
        return WOTBMOD_ERROR_NOT_FOUND;
    }

    DavaSoundEvent* event = static_cast<DavaSoundEvent*>(
        HeapAlloc(
            GetProcessHeap(),
            HEAP_ZERO_MEMORY,
            sizeof(DavaSoundEvent)));
    if (!event) {
        RefCountedReleaseFn release =
            reinterpret_cast<RefCountedReleaseFn>(Address(
                context, context->ref_counted_release_rva));
        if (release) release(eventObject);
        return WOTBMOD_ERROR_LIMIT_REACHED;
    }
    event->magic = kEventMagic;
    event->context = context;
    event->native_event = eventObject;
    event->state = WOTBMOD_AUDIO_STOPPED;
    InterlockedIncrement(&context->active_events);
    *outNativeEvent = event;
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL Trigger(
    void* userData,
    void* nativeEvent) {
    DavaSoundContext* context = ValidContext(userData);
    DavaSoundEvent* event = ValidEvent(context, nativeEvent);
    if (!event) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    void** vtable = *reinterpret_cast<void***>(event->native_event);
    EventVoidFn trigger = reinterpret_cast<EventVoidFn>(vtable[5]);
    if (!trigger || !IsExecutableAddress((const void*)trigger)) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    trigger(event->native_event);
    InterlockedExchange(&event->state, WOTBMOD_AUDIO_PLAYING);
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL StopWithForce(
    void* userData,
    void* nativeEvent,
    int32_t force) {
    DavaSoundContext* context = ValidContext(userData);
    DavaSoundEvent* event = ValidEvent(context, nativeEvent);
    if (!event || (force != 0 && force != 1)) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    __try {
        void** vtable = *reinterpret_cast<void***>(event->native_event);
        EventStopFn stop = reinterpret_cast<EventStopFn>(vtable[6]);
        if (!stop || !IsExecutableAddress((const void*)stop)) {
            return WOTBMOD_ERROR_PLATFORM;
        }
        stop(event->native_event, force != 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
    InterlockedExchange(&event->state, WOTBMOD_AUDIO_STOPPED);
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL Stop(
    void* userData,
    void* nativeEvent) {
    /*
     * Legacy ABI did not carry the force bit. Keep its historical forced-stop
     * behavior while the append-only callback above preserves V3 force=false.
     */
    return StopWithForce(userData, nativeEvent, 1);
}

static WotbModResult WOTBMOD_CALL SetPaused(
    void* userData,
    void* nativeEvent,
    int32_t paused) {
    DavaSoundContext* context = ValidContext(userData);
    DavaSoundEvent* event = ValidEvent(context, nativeEvent);
    if (!event || (paused != 0 && paused != 1)) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    void** vtable = *reinterpret_cast<void***>(event->native_event);
    EventPausedFn setPaused =
        reinterpret_cast<EventPausedFn>(vtable[7]);
    if (!setPaused || !IsExecutableAddress((const void*)setPaused)) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    setPaused(event->native_event, paused != 0);
    InterlockedExchange(
        &event->state,
        paused ? WOTBMOD_AUDIO_PAUSED : WOTBMOD_AUDIO_PLAYING);
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL SetVolume(
    void* userData,
    void* nativeEvent,
    float volume) {
    DavaSoundContext* context = ValidContext(userData);
    DavaSoundEvent* event = ValidEvent(context, nativeEvent);
    if (!event) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    void** vtable = *reinterpret_cast<void***>(event->native_event);
    EventVolumeFn setVolume =
        reinterpret_cast<EventVolumeFn>(vtable[8]);
    if (!setVolume || !IsExecutableAddress((const void*)setVolume)) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    setVolume(event->native_event, volume);
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL SetPosition(
    void* userData,
    void* nativeEvent,
    float x,
    float y,
    float z) {
    DavaSoundContext* context = ValidContext(userData);
    DavaSoundEvent* event = ValidEvent(context, nativeEvent);
    if (!event) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    float position[3] = {x, y, z};
    void** vtable = *reinterpret_cast<void***>(event->native_event);
    EventPositionFn setPosition =
        reinterpret_cast<EventPositionFn>(vtable[11]);
    if (!setPosition || !IsExecutableAddress((const void*)setPosition)) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    setPosition(event->native_event, position);
    return WOTBMOD_OK;
}

static WotbModResult SetScalarSlot(
    DavaSoundContext* context,
    DavaSoundEvent* event,
    size_t slot,
    float value) {
    if (!context || !event) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    __try {
        void** vtable = *reinterpret_cast<void***>(event->native_event);
        EventVolumeFn function =
            reinterpret_cast<EventVolumeFn>(vtable[slot]);
        if (!function ||
            !IsExecutableAddress((const void*)function)) {
            return WOTBMOD_ERROR_PLATFORM;
        }
        function(event->native_event, value);
        return WOTBMOD_OK;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

static WotbModResult SetVectorSlot(
    DavaSoundContext* context,
    DavaSoundEvent* event,
    size_t slot,
    float x,
    float y,
    float z) {
    if (!context || !event) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    float vector[3] = {x, y, z};
    __try {
        void** vtable = *reinterpret_cast<void***>(event->native_event);
        EventPositionFn function =
            reinterpret_cast<EventPositionFn>(vtable[slot]);
        if (!function ||
            !IsExecutableAddress((const void*)function)) {
            return WOTBMOD_ERROR_PLATFORM;
        }
        function(event->native_event, vector);
        return WOTBMOD_OK;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

static WotbModResult SetIntegerSlot(
    DavaSoundContext* context,
    DavaSoundEvent* event,
    size_t slot,
    int32_t value) {
    if (!context || !event) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    __try {
        void** vtable = *reinterpret_cast<void***>(event->native_event);
        EventIntegerFn function =
            reinterpret_cast<EventIntegerFn>(vtable[slot]);
        if (!function ||
            !IsExecutableAddress((const void*)function)) {
            return WOTBMOD_ERROR_PLATFORM;
        }
        function(event->native_event, value);
        return WOTBMOD_OK;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

static WotbModResult WOTBMOD_CALL SetSpeed(
    void* userData,
    void* nativeEvent,
    float speed) {
    DavaSoundContext* context = ValidContext(userData);
    DavaSoundEvent* event = ValidEvent(context, nativeEvent);
    return SetScalarSlot(context, event, 9u, speed);
}

static WotbModResult WOTBMOD_CALL SetDirection(
    void* userData,
    void* nativeEvent,
    float x,
    float y,
    float z) {
    DavaSoundContext* context = ValidContext(userData);
    DavaSoundEvent* event = ValidEvent(context, nativeEvent);
    return SetVectorSlot(context, event, 10u, x, y, z);
}

static WotbModResult WOTBMOD_CALL SetVelocity(
    void* userData,
    void* nativeEvent,
    float x,
    float y,
    float z) {
    DavaSoundContext* context = ValidContext(userData);
    DavaSoundEvent* event = ValidEvent(context, nativeEvent);
    return SetVectorSlot(context, event, 12u, x, y, z);
}

static WotbModResult WOTBMOD_CALL SetLoopCount(
    void* userData,
    void* nativeEvent,
    int32_t loopCount) {
    DavaSoundContext* context = ValidContext(userData);
    DavaSoundEvent* event = ValidEvent(context, nativeEvent);
    return SetIntegerSlot(context, event, 13u, loopCount);
}

static WotbModResult WOTBMOD_CALL SetPriority(
    void* userData,
    void* nativeEvent,
    int32_t priority) {
    DavaSoundContext* context = ValidContext(userData);
    DavaSoundEvent* event = ValidEvent(context, nativeEvent);
    return SetIntegerSlot(context, event, 14u, priority);
}

static WotbModResult WOTBMOD_CALL GetState(
    void* userData,
    void* nativeEvent,
    WotbModAudioState* outState) {
    DavaSoundContext* context = ValidContext(userData);
    DavaSoundEvent* event = ValidEvent(context, nativeEvent);
    if (!event || !outState) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    *outState = static_cast<WotbModAudioState>(
        InterlockedCompareExchange(&event->state, 0, 0));
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL SetParameter(
    void* userData,
    void* nativeEvent,
    const char* parameterName,
    float value) {
    DavaSoundContext* context = ValidContext(userData);
    DavaSoundEvent* event = ValidEvent(context, nativeEvent);
    if (!event || !parameterName || !parameterName[0]) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    FastNameScope fastName = {};
    if (!MakeFastName(context, parameterName, &fastName)) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    void** vtable = *reinterpret_cast<void***>(event->native_event);
    EventSetParameterFn setParameter =
        reinterpret_cast<EventSetParameterFn>(vtable[15]);
    if (!setParameter ||
        !IsExecutableAddress((const void*)setParameter)) {
        ReleaseFastName(context, &fastName);
        return WOTBMOD_ERROR_PLATFORM;
    }
    setParameter(event->native_event, &fastName, value);
    ReleaseFastName(context, &fastName);
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL GetParameter(
    void* userData,
    void* nativeEvent,
    const char* parameterName,
    float* outValue) {
    DavaSoundContext* context = ValidContext(userData);
    DavaSoundEvent* event = ValidEvent(context, nativeEvent);
    if (!event || !parameterName || !parameterName[0] || !outValue) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    FastNameScope fastName = {};
    if (!MakeFastName(context, parameterName, &fastName)) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    void** vtable = *reinterpret_cast<void***>(event->native_event);
    EventGetParameterFn getParameter =
        reinterpret_cast<EventGetParameterFn>(vtable[16]);
    if (!getParameter ||
        !IsExecutableAddress((const void*)getParameter)) {
        ReleaseFastName(context, &fastName);
        return WOTBMOD_ERROR_PLATFORM;
    }
    *outValue = getParameter(event->native_event, &fastName);
    ReleaseFastName(context, &fastName);
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL HasParameter(
    void* userData,
    void* nativeEvent,
    const char* parameterName,
    int32_t* outHasParameter) {
    DavaSoundContext* context = ValidContext(userData);
    DavaSoundEvent* event = ValidEvent(context, nativeEvent);
    if (!event ||
        !parameterName ||
        !parameterName[0] ||
        !outHasParameter) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    FastNameScope fastName = {};
    if (!MakeFastName(context, parameterName, &fastName)) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    void** vtable = *reinterpret_cast<void***>(event->native_event);
    EventHasParameterFn hasParameter =
        reinterpret_cast<EventHasParameterFn>(vtable[17]);
    if (!hasParameter ||
        !IsExecutableAddress((const void*)hasParameter)) {
        ReleaseFastName(context, &fastName);
        return WOTBMOD_ERROR_PLATFORM;
    }
    *outHasParameter =
        hasParameter(event->native_event, &fastName) ? 1 : 0;
    ReleaseFastName(context, &fastName);
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL Release(
    void* userData,
    void* nativeEvent) {
    DavaSoundContext* context = ValidContext(userData);
    DavaSoundEvent* event = ValidEvent(context, nativeEvent);
    if (!event) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    RefCountedReleaseFn release =
        reinterpret_cast<RefCountedReleaseFn>(Address(
            context, context->ref_counted_release_rva));
    if (!release || !IsExecutableAddress((const void*)release)) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    release(event->native_event);
    event->native_event = nullptr;
    event->magic = 0;
    event->context = nullptr;
    HeapFree(GetProcessHeap(), 0, event);
    InterlockedDecrement(&context->active_events);
    return WOTBMOD_OK;
}

} /* namespace */

extern "C" WotbModResult WOTBMOD_CALL WotbModDavaSound_Create(
    const WotbModDavaSoundOptions* options,
    WotbModDavaSoundHandle* outHandle,
    WotbModRuntimeSoundBackend* outBackend) {
    if (!outHandle || !outBackend) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    *outHandle = nullptr;
    ZeroMemory(outBackend, sizeof(*outBackend));

    const size_t minimumOptionsSize =
        offsetof(WotbModDavaSoundOptions, game_module) +
        sizeof(options->game_module);
    if (options && options->struct_size < minimumOptionsSize) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }

    HMODULE module = static_cast<HMODULE>(
        options && options->game_module
            ? options->game_module
            : GetModuleHandleA(nullptr));
    uint32_t imageSize = 0;
    if (!ReadImageSize(module, &imageSize)) {
        return WOTBMOD_ERROR_PLATFORM;
    }

    DavaSoundContext* context = static_cast<DavaSoundContext*>(
        HeapAlloc(
            GetProcessHeap(),
            HEAP_ZERO_MEMORY,
            sizeof(DavaSoundContext)));
    if (!context) return WOTBMOD_ERROR_LIMIT_REACHED;

    context->magic = kContextMagic;
    context->game_module = module;
    context->image_base = reinterpret_cast<uint8_t*>(module);
    context->image_size = imageSize;
    context->sound_system_singleton_rva = SelectOptionRva(
        options,
        offsetof(WotbModDavaSoundOptions, sound_system_singleton_rva),
        kDefaultSoundSystemSingletonRva);
    context->fast_name_ctor_rva = SelectOptionRva(
        options,
        offsetof(WotbModDavaSoundOptions, fast_name_ctor_rva),
        kDefaultFastNameCtorRva);
    context->ref_counted_release_rva = SelectOptionRva(
        options,
        offsetof(WotbModDavaSoundOptions, ref_counted_release_rva),
        kDefaultRefCountedReleaseRva);
    context->default_sound_group_rva = SelectOptionRva(
        options,
        offsetof(WotbModDavaSoundOptions, default_sound_group_rva),
        kDefaultSoundGroupRva);
    context->sound_system_vtable_rva = SelectOptionRva(
        options,
        offsetof(WotbModDavaSoundOptions, sound_system_vtable_rva),
        kDefaultSoundSystemVtableRva);
    context->hybrid_event_vtable_rva = SelectOptionRva(
        options,
        offsetof(WotbModDavaSoundOptions, hybrid_event_vtable_rva),
        kDefaultHybridEventVtableRva);
    context->wwise_event_vtable_rva = SelectOptionRva(
        options,
        offsetof(WotbModDavaSoundOptions, wwise_event_vtable_rva),
        kDefaultWwiseEventVtableRva);
    context->sound_system_proxy_vtable_rva = SelectOptionRva(
        options,
        offsetof(WotbModDavaSoundOptions, sound_system_proxy_vtable_rva),
        kDefaultSoundSystemProxyVtableRva);

    if (!RvaInImage(
            context,
            context->sound_system_singleton_rva,
            sizeof(void*)) ||
        !RvaInImage(
            context,
            context->fast_name_ctor_rva,
            1) ||
        !RvaInImage(
            context,
            context->ref_counted_release_rva,
            1) ||
        !RvaInImage(
            context,
            context->default_sound_group_rva,
            sizeof(int32_t)) ||
        !RvaInImage(
            context,
            context->sound_system_vtable_rva,
            sizeof(void*) * 4) ||
        !RvaInImage(
            context,
            context->sound_system_proxy_vtable_rva,
            sizeof(void*) * 4) ||
        !RvaInImage(
            context,
            context->hybrid_event_vtable_rva,
            sizeof(void*) * 18)) {
        context->magic = 0;
        HeapFree(GetProcessHeap(), 0, context);
        return WOTBMOD_ERROR_PLATFORM;
    }

    outBackend->struct_size = sizeof(*outBackend);
    outBackend->user_data = context;
    outBackend->create = &CreateEvent;
    outBackend->trigger = &Trigger;
    outBackend->stop = &Stop;
    outBackend->set_paused = &SetPaused;
    outBackend->set_volume = &SetVolume;
    outBackend->set_position = &SetPosition;
    outBackend->get_state = &GetState;
    outBackend->set_parameter = &SetParameter;
    outBackend->get_parameter = &GetParameter;
    outBackend->has_parameter = &HasParameter;
    outBackend->release = &Release;
    outBackend->stop_with_force = &StopWithForce;
    outBackend->set_speed = &SetSpeed;
    outBackend->set_direction = &SetDirection;
    outBackend->set_velocity = &SetVelocity;
    outBackend->set_loop_count = &SetLoopCount;
    outBackend->set_priority = &SetPriority;
    *outHandle = context;
    return WOTBMOD_OK;
}

extern "C" WotbModResult WOTBMOD_CALL WotbModDavaSound_Destroy(
    WotbModDavaSoundHandle handle) {
    DavaSoundContext* context =
        static_cast<DavaSoundContext*>(handle);
    if (!context || context->magic != kContextMagic) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    if (InterlockedCompareExchange(&context->active_events, 0, 0) != 0) {
        return WOTBMOD_ERROR_ACCESS_DENIED;
    }
    context->magic = 0;
    return HeapFree(GetProcessHeap(), 0, context)
               ? WOTBMOD_OK
               : WOTBMOD_ERROR_PLATFORM;
}
