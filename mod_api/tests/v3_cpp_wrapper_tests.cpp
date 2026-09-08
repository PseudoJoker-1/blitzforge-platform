#include "../include/wotbmod/wotbmod.hpp"
#include "../include/wotb_mod_runtime_v3.h"
#include "../src/v3/wotb_mod_v3_internal.h"

#include <cstdio>
#include <cstring>
#include <type_traits>
#include <utility>

namespace wotbmod {
namespace v3 {

void SetNativeHookBackend(const WotbModV3NativeHookBackend*) {}
void RegisterRuntimeServices() {}
void RegisterDataServices() {}
void RegisterClientServices() {}
void RegisterToolingServices() {}

// This focused build links wotb_mod_v3_runtime.cpp without the services
// layer, so every services symbol it references must be stubbed here.
// wotb_mod_v3_runtime.cpp announces capability changes on the event bus
// via PublishSystemEvent; the event bus itself is not under test here.
WotbModV3Result PublishSystemEvent(
    const char*,
    const void*,
    uint32_t,
    uint32_t) {
    return WOTBMOD_V3_OK;
}

}  // namespace v3
}  // namespace wotbmod

namespace {

int g_checks = 0;
int g_failures = 0;
int g_destroyed = 0;
int g_payload = 0;

void Check(bool condition, const char* expression, int line) {
    ++g_checks;
    if (condition) return;
    ++g_failures;
    std::fprintf(
        stderr,
        "V3 C++ WRAPPER FAIL line %d: %s\n",
        line,
        expression);
}

#define CHECK(expression) Check((expression), #expression, __LINE__)

void CopyText(char* destination, size_t capacity, const char* source) {
    if (!destination || capacity == 0u) return;
    destination[0] = '\0';
    if (!source) return;
    strncpy_s(destination, capacity, source, _TRUNCATE);
}

WotbModV3Result WOTBMOD_V3_CALL TestEntry(
    const WotbModV3Bootstrap*,
    WotbModV3Handle,
    WotbModV3Info* out_info) {
    if (!out_info) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    *out_info = {};
    WOTBMOD_V3_INIT_STRUCT(*out_info, WOTBMOD_V3_ABI_VERSION);
    out_info->requested_permission_tier =
        WOTBMOD_V3_PERMISSION_SAFE;
    CopyText(out_info->id, sizeof(out_info->id), "tests.cpp-wrapper");
    CopyText(out_info->name, sizeof(out_info->name), "C++ wrapper test");
    CopyText(out_info->version, sizeof(out_info->version), "1.0.0");
    CopyText(out_info->author, sizeof(out_info->author), "tests");
    return WOTBMOD_V3_OK;
}

void DestroyProbe(void* object) {
    CHECK(object == &g_payload);
    ++g_destroyed;
}

WotbModV3HandleInfo GetInfo(
    const wotbmod::v3::OwnedHandle& handle) {
    WotbModV3HandleInfo info = {};
    CHECK(handle.info(info) == WOTBMOD_V3_OK);
    return info;
}

}  // namespace

static_assert(
    std::is_copy_constructible<wotbmod::v3::OwnedHandle>::value,
    "OwnedHandle must support retaining copies");
static_assert(
    !std::is_copy_assignable<wotbmod::v3::OwnedHandle>::value,
    "OwnedHandle copy assignment must use checked copy_from");
static_assert(
    !std::is_move_assignable<wotbmod::v3::OwnedHandle>::value,
    "OwnedHandle move assignment must use checked move_from");
static_assert(
    std::is_nothrow_move_constructible<
        wotbmod::v3::OwnedHandle>::value,
    "OwnedHandle moves must not throw");
static_assert(
    std::is_nothrow_destructible<wotbmod::v3::OwnedHandle>::value,
    "OwnedHandle destruction must not throw");
static_assert(
    !std::is_copy_constructible<
        wotbmod::v3::Result<wotbmod::v3::OwnedHandle>>::value,
    "Result must not duplicate an owned reference implicitly");
static_assert(
    !std::is_move_assignable<
        wotbmod::v3::Result<wotbmod::v3::OwnedHandle>>::value,
    "Result move assignment must not hide a release failure");
static_assert(
    wotbmod::v3::InterfaceTraits<WotbModV3AudioApiV2>::version() ==
        WOTBMOD_V3_AUDIO_VERSION,
    "typed interface traits must preserve the ABI version");
static_assert(
    wotbmod::v3::InterfaceTraits<WotbModV3UiApiV3>::version() ==
        WOTBMOD_V3_UI_VERSION_3,
    "UI V3 typed interface trait must preserve the ABI version");
static_assert(
    wotbmod::v3::InterfaceTraits<
        WotbModV3VehicleVisualApiV2>::version() ==
        WOTBMOD_V3_VEHICLE_VISUAL_VERSION_2,
    "vehicle visual V2 typed interface trait must preserve the ABI version");
static_assert(
    wotbmod::v3::InterfaceTraits<
        WotbModV3ProjectileApiV2>::version() ==
        WOTBMOD_V3_PROJECTILE_VERSION_2,
    "projectile V2 typed interface trait must preserve the ABI version");
static_assert(
    wotbmod::v3::InterfaceTraits<
        WotbModV3DevtoolsApiV3>::version() ==
        WOTBMOD_V3_DEVTOOLS_VERSION_3,
    "devtools V3 typed interface trait must preserve the ABI version");

int main() {
    using wotbmod::v3::BootstrapView;
    using wotbmod::v3::ModContext;
    using wotbmod::v3::OwnedHandle;
    using wotbmod::v3::Status;

    const Status ok;
    CHECK(ok.ok());
    CHECK(ok == WOTBMOD_V3_OK);
    CHECK(Status(WOTBMOD_V3_E_IO) != WOTBMOD_V3_OK);

    const BootstrapView empty_bootstrap;
    CHECK(!empty_bootstrap.valid());
    CHECK(
        empty_bootstrap.status() ==
        WOTBMOD_V3_E_INVALID_ARGUMENT);

    WotbModV3Bootstrap too_small = {};
    too_small.struct_size = sizeof(WotbModV3StructHeader);
    too_small.api_version = WOTBMOD_V3_ABI_VERSION;
    too_small.bootstrap_version = WOTBMOD_V3_BOOTSTRAP_VERSION;
    CHECK(
        BootstrapView(&too_small).status() ==
        WOTBMOD_V3_E_INCOMPATIBLE);

    WotbModV3RuntimeOptions options = {};
    options.struct_size = sizeof(options);
    options.api_version = WOTBMOD_V3_ABI_VERSION;
    options.game_directory = "build\\v3_cpp_wrapper_env";
    options.mods_directory = "build\\v3_cpp_wrapper_env\\mods";
    options.cache_directory = "build\\v3_cpp_wrapper_env\\cache";
    options.config_directory = "build\\v3_cpp_wrapper_env\\config";
    options.client_version = "cpp-wrapper-test";
    CHECK(
        WotbModV3Runtime_Initialize(&options) ==
        WOTBMOD_V3_OK);

    WotbModV3Handle mod = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        WotbModV3Runtime_CreateMod(
            "v3_cpp_wrapper_test.dll",
            WOTBMOD_V3_PERMISSION_SAFE,
            &mod) == WOTBMOD_V3_OK);

    WotbModV3RuntimeModuleInfo module = {};
    module.struct_size = sizeof(module);
    module.api_version = WOTBMOD_V3_ABI_VERSION;
    CHECK(
        WotbModV3Runtime_InvokeEntry(
            mod,
            &TestEntry,
            &module) == WOTBMOD_V3_OK);

    const BootstrapView bootstrap(
        WotbModV3Runtime_GetBootstrap());
    CHECK(bootstrap.valid());
    CHECK(bootstrap.get() != nullptr);

    const ModContext invalid_context =
        bootstrap.context(WOTBMOD_V3_INVALID_HANDLE);
    CHECK(!invalid_context.valid());
    CHECK(
        invalid_context.status() ==
        WOTBMOD_V3_E_INVALID_HANDLE);
    const ModContext fabricated_context =
        bootstrap.context(UINT64_MAX);
    CHECK(!fabricated_context.valid());
    CHECK(
        fabricated_context.status() ==
        WOTBMOD_V3_E_INVALID_HANDLE);

    const ModContext context = bootstrap.context(mod);
    CHECK(context.valid());
    CHECK(context.mod() == mod);
    CHECK(context.bootstrap().get() == bootstrap.get());

    const auto core =
        context.query_interface<WotbModV3CoreApiV1>();
    CHECK(core.ok());
    CHECK(core.get_if() != nullptr);
    CHECK(core.value_unchecked() != nullptr);
    CHECK(
        core.value_unchecked()->api_version ==
        WOTBMOD_V3_CORE_VERSION);

    const auto explicit_core =
        context.query_interface<WotbModV3CoreApiV1>(
            WOTBMOD_V3_IFACE_CORE,
            WOTBMOD_V3_CORE_VERSION);
    CHECK(explicit_core.ok());
    CHECK(
        explicit_core.value_unchecked() ==
        core.value_unchecked());

    const auto unsupported =
        context.query_interface<WotbModV3CoreApiV1>(
            "wotbmod.does.not.exist",
            1u);
    CHECK(!unsupported.ok());
    CHECK(
        unsupported.status() ==
        WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(unsupported.get_if() == nullptr);

    WotbModV3ErrorInfo error = {};
    CHECK(context.get_last_error(error) == WOTBMOD_V3_OK);
    CHECK(error.code == WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(error.owner_mod == mod);
    CHECK(error.message[0] != '\0');

    WotbModV3InterfaceInfo interface_info = {};
    CHECK(
        context.get_interface_info(
            WOTBMOD_V3_IFACE_HANDLES,
            interface_info) == WOTBMOD_V3_OK);
    CHECK(
        interface_info.interface_version ==
        WOTBMOD_V3_HANDLES_VERSION);

    WotbModV3ClientInfo client_info = {};
    CHECK(context.get_client_info(client_info) == WOTBMOD_V3_OK);
    CHECK(
        std::strcmp(
            client_info.client_version,
            "cpp-wrapper-test") == 0);

    const auto handles_result =
        context.query_interface<WotbModV3HandlesApiV1>();
    CHECK(handles_result.ok());
    const WotbModV3HandlesApiV1* handles =
        handles_result.value_unchecked();
    CHECK(handles != nullptr);

    WotbModV3Handle raw = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        wotbmod::v3::CreateOwnedHandle(
            mod,
            WOTBMOD_V3_HANDLE_RESOURCE,
            &g_payload,
            &DestroyProbe,
            &raw) == WOTBMOD_V3_OK);
    CHECK(raw != WOTBMOD_V3_INVALID_HANDLE);

    auto adopted_result = OwnedHandle::adopt(context, raw);
    CHECK(adopted_result.ok());
    OwnedHandle original =
        std::move(adopted_result.value_unchecked());
    CHECK(original.owns_handle());
    CHECK(original.get() == raw);
    CHECK(original.owner_mod() == mod);
    CHECK(original.last_status().ok());
    CHECK(GetInfo(original).reference_count == 1u);

    OwnedHandle copied(original);
    CHECK(copied.owns_handle());
    CHECK(copied.last_status().ok());
    CHECK(GetInfo(original).reference_count == 2u);

    auto clone_result = original.clone();
    CHECK(clone_result.ok());
    OwnedHandle cloned =
        std::move(clone_result.value_unchecked());
    CHECK(cloned.owns_handle());
    CHECK(GetInfo(original).reference_count == 3u);

    OwnedHandle assigned;
    CHECK(assigned.copy_from(original) == WOTBMOD_V3_OK);
    CHECK(assigned.get() == raw);
    CHECK(GetInfo(original).reference_count == 4u);

    OwnedHandle moved(std::move(copied));
    CHECK(moved.owns_handle());
    CHECK(!copied.owns_handle());
    CHECK(GetInfo(original).reference_count == 4u);

    OwnedHandle move_assigned;
    CHECK(
        move_assigned.move_from(std::move(moved)) ==
        WOTBMOD_V3_OK);
    CHECK(move_assigned.owns_handle());
    CHECK(!moved.owns_handle());
    CHECK(GetInfo(original).reference_count == 4u);

    CHECK(move_assigned.close() == WOTBMOD_V3_OK);
    CHECK(assigned.close() == WOTBMOD_V3_OK);
    CHECK(cloned.close() == WOTBMOD_V3_OK);
    CHECK(GetInfo(original).reference_count == 1u);

    WotbModV3Handle foreign_mod = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        WotbModV3Runtime_CreateMod(
            "v3_cpp_wrapper_foreign.dll",
            WOTBMOD_V3_PERMISSION_SAFE,
            &foreign_mod) == WOTBMOD_V3_OK);
    const ModContext foreign_context =
        bootstrap.context(foreign_mod);
    const auto foreign_retain =
        OwnedHandle::retain(foreign_context, raw);
    CHECK(!foreign_retain.ok());
    CHECK(
        foreign_retain.status() ==
        WOTBMOD_V3_E_INVALID_HANDLE);
    CHECK(foreign_retain.get_if() == nullptr);

    const auto invalid_retain =
        OwnedHandle::retain(
            context,
            WOTBMOD_V3_INVALID_HANDLE);
    CHECK(!invalid_retain.ok());
    CHECK(
        invalid_retain.status() ==
        WOTBMOD_V3_E_INVALID_ARGUMENT);

    uint32_t alive = 0u;
    CHECK(original.is_alive(alive) == WOTBMOD_V3_OK);
    CHECK(alive == 1u);
    CHECK(original.close() == WOTBMOD_V3_OK);
    CHECK(g_destroyed == 1);
    CHECK(original.close() == WOTBMOD_V3_OK);
    CHECK(original.is_alive(alive) == WOTBMOD_V3_E_INVALID_HANDLE);
    CHECK(alive == 0u);
    CHECK(handles->is_alive(mod, raw, &alive) == WOTBMOD_V3_OK);
    CHECK(alive == 0u);

    WotbModV3Handle stale_raw = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        wotbmod::v3::CreateOwnedHandle(
            mod,
            WOTBMOD_V3_HANDLE_RESOURCE,
            &g_payload,
            &DestroyProbe,
            &stale_raw) == WOTBMOD_V3_OK);
    auto stale_result = OwnedHandle::adopt(context, stale_raw);
    CHECK(stale_result.ok());
    OwnedHandle stale_source =
        std::move(stale_result.value_unchecked());
    CHECK(handles->release(mod, stale_raw) == WOTBMOD_V3_OK);
    CHECK(g_destroyed == 2);

    OwnedHandle failed_copy(stale_source);
    CHECK(!failed_copy.owns_handle());
    CHECK(
        failed_copy.last_status() ==
        WOTBMOD_V3_E_INVALID_HANDLE);

    WotbModV3Handle preserved_raw = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        wotbmod::v3::CreateOwnedHandle(
            mod,
            WOTBMOD_V3_HANDLE_RESOURCE,
            &g_payload,
            &DestroyProbe,
            &preserved_raw) == WOTBMOD_V3_OK);
    auto preserved_result =
        OwnedHandle::adopt(context, preserved_raw);
    CHECK(preserved_result.ok());
    OwnedHandle preserved =
        std::move(preserved_result.value_unchecked());
    CHECK(
        preserved.copy_from(stale_source) ==
        WOTBMOD_V3_E_INVALID_HANDLE);
    CHECK(preserved.owns_handle());
    CHECK(preserved.get() == preserved_raw);
    CHECK(
        preserved.last_status() ==
        WOTBMOD_V3_E_INVALID_HANDLE);
    CHECK(preserved.close() == WOTBMOD_V3_OK);
    CHECK(g_destroyed == 3);

    CHECK(
        stale_source.close() ==
        WOTBMOD_V3_E_INVALID_HANDLE);
    CHECK(stale_source.owns_handle());
    CHECK(
        stale_source.last_status() ==
        WOTBMOD_V3_E_INVALID_HANDLE);
    (void)stale_source.detach();

    WotbModV3Handle stale_move_raw =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        wotbmod::v3::CreateOwnedHandle(
            mod,
            WOTBMOD_V3_HANDLE_RESOURCE,
            &g_payload,
            &DestroyProbe,
            &stale_move_raw) == WOTBMOD_V3_OK);
    auto stale_move_result =
        OwnedHandle::adopt(context, stale_move_raw);
    CHECK(stale_move_result.ok());
    OwnedHandle stale_move_target =
        std::move(stale_move_result.value_unchecked());
    CHECK(
        handles->release(mod, stale_move_raw) ==
        WOTBMOD_V3_OK);
    CHECK(g_destroyed == 4);

    WotbModV3Handle move_source_raw =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        wotbmod::v3::CreateOwnedHandle(
            mod,
            WOTBMOD_V3_HANDLE_RESOURCE,
            &g_payload,
            &DestroyProbe,
            &move_source_raw) == WOTBMOD_V3_OK);
    auto move_source_result =
        OwnedHandle::adopt(context, move_source_raw);
    CHECK(move_source_result.ok());
    OwnedHandle move_source =
        std::move(move_source_result.value_unchecked());
    CHECK(
        stale_move_target.move_from(std::move(move_source)) ==
        WOTBMOD_V3_E_INVALID_HANDLE);
    CHECK(stale_move_target.owns_handle());
    CHECK(stale_move_target.get() == stale_move_raw);
    CHECK(move_source.owns_handle());
    CHECK(move_source.get() == move_source_raw);
    (void)stale_move_target.detach();
    CHECK(move_source.close() == WOTBMOD_V3_OK);
    CHECK(g_destroyed == 5);

    CHECK(
        WotbModV3Runtime_DestroyMod(foreign_mod) ==
        WOTBMOD_V3_OK);
    CHECK(!foreign_context.valid());
    CHECK(
        foreign_context.status() ==
        WOTBMOD_V3_E_INVALID_HANDLE);
    CHECK(
        WotbModV3Runtime_DestroyMod(mod) ==
        WOTBMOD_V3_OK);
    CHECK(!context.valid());
    CHECK(
        context.status() ==
        WOTBMOD_V3_E_INVALID_HANDLE);
    WotbModV3Runtime_Shutdown();

    std::printf(
        "V3 C++ WRAPPER: %d checks, %d failures\n",
        g_checks,
        g_failures);
    return g_failures == 0 ? 0 : 1;
}
