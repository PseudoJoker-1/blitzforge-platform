#pragma once

#include "wotb_mod_api_v3.h"

#include <cstddef>
#include <type_traits>
#include <utility>

namespace wotbmod {
namespace v3 {

class Status final {
public:
    constexpr Status() noexcept = default;
    constexpr explicit Status(WotbModV3Result code) noexcept
        : code_(code) {}

    [[nodiscard]] constexpr bool ok() const noexcept {
        return code_ == WOTBMOD_V3_OK;
    }

    [[nodiscard]] constexpr WotbModV3Result code() const noexcept {
        return code_;
    }

    constexpr explicit operator bool() const noexcept {
        return ok();
    }

private:
    WotbModV3Result code_ = WOTBMOD_V3_OK;
};

[[nodiscard]] constexpr bool operator==(
    Status left,
    Status right) noexcept {
    return left.code() == right.code();
}

[[nodiscard]] constexpr bool operator!=(
    Status left,
    Status right) noexcept {
    return !(left == right);
}

[[nodiscard]] constexpr bool operator==(
    Status left,
    WotbModV3Result right) noexcept {
    return left.code() == right;
}

[[nodiscard]] constexpr bool operator!=(
    Status left,
    WotbModV3Result right) noexcept {
    return !(left == right);
}

template <typename T>
class Result final {
    static_assert(
        std::is_nothrow_default_constructible<T>::value,
        "wotbmod::v3::Result values must be nothrow default constructible");
    static_assert(
        std::is_nothrow_move_constructible<T>::value,
        "wotbmod::v3::Result values must be nothrow move constructible");

public:
    Result(const Result&) = delete;
    Result& operator=(const Result&) = delete;
    Result(Result&&) noexcept = default;
    Result& operator=(Result&&) = delete;

    [[nodiscard]] static Result success(T&& value) noexcept {
        return Result(Status(), std::move(value));
    }

    [[nodiscard]] static Result failure(
        WotbModV3Result code) noexcept {
        return Result(
            Status(
                code == WOTBMOD_V3_OK
                    ? WOTBMOD_V3_E_INVALID_ARGUMENT
                    : code),
            T{});
    }

    [[nodiscard]] static Result failure(Status status) noexcept {
        return failure(status.code());
    }

    [[nodiscard]] constexpr bool ok() const noexcept {
        return status_.ok();
    }

    [[nodiscard]] constexpr Status status() const noexcept {
        return status_;
    }

    constexpr explicit operator bool() const noexcept {
        return ok();
    }

    /*
     * Returns nullptr on failure, so reading a value never discards its
     * associated status. value_unchecked() is available after an explicit
     * status check.
     */
    [[nodiscard]] T* get_if() noexcept {
        return ok() ? &value_ : nullptr;
    }

    [[nodiscard]] const T* get_if() const noexcept {
        return ok() ? &value_ : nullptr;
    }

    [[nodiscard]] T& value_unchecked() noexcept {
        return value_;
    }

    [[nodiscard]] const T& value_unchecked() const noexcept {
        return value_;
    }

private:
    Result(Status status, T value) noexcept
        : status_(status), value_(std::move(value)) {}

    Status status_;
    T value_;
};

template <typename Interface>
struct InterfaceTraits;

#define WOTBMOD_V3_CPP_INTERFACE_TRAIT(                                \
    interface_type, interface_id, interface_version)                   \
    template <>                                                        \
    struct InterfaceTraits<interface_type> final {                     \
        [[nodiscard]] static constexpr const char* name() noexcept {   \
            return interface_id;                                      \
        }                                                              \
        [[nodiscard]] static constexpr uint32_t version() noexcept {   \
            return interface_version;                                 \
        }                                                              \
    }

WOTBMOD_V3_CPP_INTERFACE_TRAIT(
    WotbModV3CoreApiV1,
    WOTBMOD_V3_IFACE_CORE,
    WOTBMOD_V3_CORE_VERSION);
WOTBMOD_V3_CPP_INTERFACE_TRAIT(
    WotbModV3CapabilitiesApiV1,
    WOTBMOD_V3_IFACE_CAPABILITIES,
    WOTBMOD_V3_CAPABILITIES_VERSION);
WOTBMOD_V3_CPP_INTERFACE_TRAIT(
    WotbModV3PermissionsApiV1,
    WOTBMOD_V3_IFACE_PERMISSIONS,
    WOTBMOD_V3_PERMISSIONS_VERSION);
WOTBMOD_V3_CPP_INTERFACE_TRAIT(
    WotbModV3HandlesApiV1,
    WOTBMOD_V3_IFACE_HANDLES,
    WOTBMOD_V3_HANDLES_VERSION);
WOTBMOD_V3_CPP_INTERFACE_TRAIT(
    WotbModV3LifecycleApiV1,
    WOTBMOD_V3_IFACE_LIFECYCLE,
    WOTBMOD_V3_LIFECYCLE_VERSION);
WOTBMOD_V3_CPP_INTERFACE_TRAIT(
    WotbModV3HooksApiV1,
    WOTBMOD_V3_IFACE_HOOKS,
    WOTBMOD_V3_HOOKS_VERSION);
WOTBMOD_V3_CPP_INTERFACE_TRAIT(
    WotbModV3UnsafeNativeApiV1,
    WOTBMOD_V3_IFACE_UNSAFE_NATIVE,
    WOTBMOD_V3_UNSAFE_NATIVE_VERSION);
WOTBMOD_V3_CPP_INTERFACE_TRAIT(
    WotbModV3EventsApiV1,
    WOTBMOD_V3_IFACE_EVENTS,
    WOTBMOD_V3_EVENTS_VERSION);
WOTBMOD_V3_CPP_INTERFACE_TRAIT(
    WotbModV3UiApiV2,
    WOTBMOD_V3_IFACE_UI,
    WOTBMOD_V3_UI_VERSION);
WOTBMOD_V3_CPP_INTERFACE_TRAIT(
    WotbModV3UiApiV3,
    WOTBMOD_V3_IFACE_UI,
    WOTBMOD_V3_UI_VERSION_3);
WOTBMOD_V3_CPP_INTERFACE_TRAIT(
    WotbModV3SettingsApiV1,
    WOTBMOD_V3_IFACE_SETTINGS,
    WOTBMOD_V3_SETTINGS_VERSION);
WOTBMOD_V3_CPP_INTERFACE_TRAIT(
    WotbModV3StorageApiV1,
    WOTBMOD_V3_IFACE_STORAGE,
    WOTBMOD_V3_STORAGE_VERSION);
WOTBMOD_V3_CPP_INTERFACE_TRAIT(
    WotbModV3InputApiV1,
    WOTBMOD_V3_IFACE_INPUT,
    WOTBMOD_V3_INPUT_VERSION);
WOTBMOD_V3_CPP_INTERFACE_TRAIT(
    WotbModV3VfsApiV1,
    WOTBMOD_V3_IFACE_VFS,
    WOTBMOD_V3_VFS_VERSION);
WOTBMOD_V3_CPP_INTERFACE_TRAIT(
    WotbModV3ResourcesApiV1,
    WOTBMOD_V3_IFACE_RESOURCES,
    WOTBMOD_V3_RESOURCES_VERSION);
WOTBMOD_V3_CPP_INTERFACE_TRAIT(
    WotbModV3AsyncApiV1,
    WOTBMOD_V3_IFACE_ASYNC,
    WOTBMOD_V3_ASYNC_VERSION);
WOTBMOD_V3_CPP_INTERFACE_TRAIT(
    WotbModV3HttpApiV1,
    WOTBMOD_V3_IFACE_HTTP,
    WOTBMOD_V3_HTTP_VERSION);
WOTBMOD_V3_CPP_INTERFACE_TRAIT(
    WotbModV3IntermodApiV1,
    WOTBMOD_V3_IFACE_INTERMOD,
    WOTBMOD_V3_INTERMOD_VERSION);
WOTBMOD_V3_CPP_INTERFACE_TRAIT(
    WotbModV3RenderApiV1,
    WOTBMOD_V3_IFACE_RENDER,
    WOTBMOD_V3_RENDER_VERSION);
WOTBMOD_V3_CPP_INTERFACE_TRAIT(
    WotbModV3RenderNativeApiV1,
    WOTBMOD_V3_IFACE_RENDER_NATIVE,
    WOTBMOD_V3_RENDER_NATIVE_VERSION);
WOTBMOD_V3_CPP_INTERFACE_TRAIT(
    WotbModV3CameraApiV1,
    WOTBMOD_V3_IFACE_CAMERA,
    WOTBMOD_V3_CAMERA_VERSION);
WOTBMOD_V3_CPP_INTERFACE_TRAIT(
    WotbModV3SceneApiV1,
    WOTBMOD_V3_IFACE_SCENE,
    WOTBMOD_V3_SCENE_VERSION);
WOTBMOD_V3_CPP_INTERFACE_TRAIT(
    WotbModV3AudioApiV2,
    WOTBMOD_V3_IFACE_AUDIO,
    WOTBMOD_V3_AUDIO_VERSION);
WOTBMOD_V3_CPP_INTERFACE_TRAIT(
    WotbModV3VehicleVisualApiV1,
    WOTBMOD_V3_IFACE_VEHICLE_VISUAL,
    WOTBMOD_V3_VEHICLE_VISUAL_VERSION);
WOTBMOD_V3_CPP_INTERFACE_TRAIT(
    WotbModV3VehicleVisualApiV2,
    WOTBMOD_V3_IFACE_VEHICLE_VISUAL,
    WOTBMOD_V3_VEHICLE_VISUAL_VERSION_2);
WOTBMOD_V3_CPP_INTERFACE_TRAIT(
    WotbModV3GameplayCameraApiV1,
    WOTBMOD_V3_IFACE_GAMEPLAY_CAMERA,
    WOTBMOD_V3_GAMEPLAY_CAMERA_VERSION);
WOTBMOD_V3_CPP_INTERFACE_TRAIT(
    WotbModV3GameplayHudApiV1,
    WOTBMOD_V3_IFACE_GAMEPLAY_HUD,
    WOTBMOD_V3_GAMEPLAY_HUD_VERSION);
WOTBMOD_V3_CPP_INTERFACE_TRAIT(
    WotbModV3GameplayHangarApiV1,
    WOTBMOD_V3_IFACE_GAMEPLAY_HANGAR,
    WOTBMOD_V3_GAMEPLAY_HANGAR_VERSION);
WOTBMOD_V3_CPP_INTERFACE_TRAIT(
    WotbModV3GameplayReplayApiV1,
    WOTBMOD_V3_IFACE_GAMEPLAY_REPLAY,
    WOTBMOD_V3_GAMEPLAY_REPLAY_VERSION);
WOTBMOD_V3_CPP_INTERFACE_TRAIT(
    WotbModV3EntityPublicApiV1,
    WOTBMOD_V3_IFACE_ENTITY_PUBLIC,
    WOTBMOD_V3_ENTITY_PUBLIC_VERSION);
WOTBMOD_V3_CPP_INTERFACE_TRAIT(
    WotbModV3BigWorldRpcApiV1,
    WOTBMOD_V3_IFACE_BIGWORLD_RPC,
    WOTBMOD_V3_BIGWORLD_RPC_VERSION);
WOTBMOD_V3_CPP_INTERFACE_TRAIT(
    WotbModV3ProjectileApiV1,
    WOTBMOD_V3_IFACE_PROJECTILE,
    WOTBMOD_V3_PROJECTILE_VERSION);
WOTBMOD_V3_CPP_INTERFACE_TRAIT(
    WotbModV3ProjectileApiV2,
    WOTBMOD_V3_IFACE_PROJECTILE,
    WOTBMOD_V3_PROJECTILE_VERSION_2);
WOTBMOD_V3_CPP_INTERFACE_TRAIT(
    WotbModV3YamlApiV1,
    WOTBMOD_V3_IFACE_YAML,
    WOTBMOD_V3_YAML_VERSION);
WOTBMOD_V3_CPP_INTERFACE_TRAIT(
    WotbModV3ArchiveApiV1,
    WOTBMOD_V3_IFACE_ARCHIVE,
    WOTBMOD_V3_ARCHIVE_VERSION);
WOTBMOD_V3_CPP_INTERFACE_TRAIT(
    WotbModV3LoadersApiV1,
    WOTBMOD_V3_IFACE_LOADERS,
    WOTBMOD_V3_LOADERS_VERSION);
WOTBMOD_V3_CPP_INTERFACE_TRAIT(
    WotbModV3ClientApiV1,
    WOTBMOD_V3_IFACE_CLIENT,
    WOTBMOD_V3_CLIENT_VERSION);
WOTBMOD_V3_CPP_INTERFACE_TRAIT(
    WotbModV3DeviceApiV1,
    WOTBMOD_V3_IFACE_DEVICE,
    WOTBMOD_V3_DEVICE_VERSION);
WOTBMOD_V3_CPP_INTERFACE_TRAIT(
    WotbModV3DiagnosticsApiV1,
    WOTBMOD_V3_IFACE_DIAGNOSTICS,
    WOTBMOD_V3_DIAGNOSTICS_VERSION);
WOTBMOD_V3_CPP_INTERFACE_TRAIT(
    WotbModV3DiagnosticsApiV2,
    WOTBMOD_V3_IFACE_DIAGNOSTICS,
    WOTBMOD_V3_DIAGNOSTICS_VERSION_2);
WOTBMOD_V3_CPP_INTERFACE_TRAIT(
    WotbModV3DevtoolsApiV1,
    WOTBMOD_V3_IFACE_DEVTOOLS,
    WOTBMOD_V3_DEVTOOLS_VERSION);
WOTBMOD_V3_CPP_INTERFACE_TRAIT(
    WotbModV3DevtoolsApiV2,
    WOTBMOD_V3_IFACE_DEVTOOLS,
    WOTBMOD_V3_DEVTOOLS_VERSION_2);
WOTBMOD_V3_CPP_INTERFACE_TRAIT(
    WotbModV3DevtoolsApiV3,
    WOTBMOD_V3_IFACE_DEVTOOLS,
    WOTBMOD_V3_DEVTOOLS_VERSION_3);
WOTBMOD_V3_CPP_INTERFACE_TRAIT(
    WotbModV3ManifestApiV1,
    WOTBMOD_V3_IFACE_MANIFEST,
    WOTBMOD_V3_MANIFEST_VERSION);
WOTBMOD_V3_CPP_INTERFACE_TRAIT(
    WotbModV3CatalogApiV1,
    WOTBMOD_V3_IFACE_CATALOG,
    WOTBMOD_V3_CATALOG_VERSION);
WOTBMOD_V3_CPP_INTERFACE_TRAIT(
    WotbModV3ContentApiV1,
    WOTBMOD_V3_IFACE_CONTENT,
    WOTBMOD_V3_CONTENT_VERSION);

#undef WOTBMOD_V3_CPP_INTERFACE_TRAIT

class ModContext;

class BootstrapView final {
public:
    constexpr BootstrapView() noexcept = default;
    constexpr explicit BootstrapView(
        const WotbModV3Bootstrap* bootstrap) noexcept
        : bootstrap_(bootstrap) {}

    [[nodiscard]] Status status() const noexcept {
        if (!bootstrap_) {
            return Status(WOTBMOD_V3_E_INVALID_ARGUMENT);
        }
        const std::size_t minimum_size =
            offsetof(WotbModV3Bootstrap, get_client_info) +
            sizeof(bootstrap_->get_client_info);
        if (bootstrap_->struct_size < minimum_size ||
            (bootstrap_->api_version & 0xFFFF0000u) !=
                (WOTBMOD_V3_ABI_VERSION & 0xFFFF0000u) ||
            bootstrap_->bootstrap_version <
                WOTBMOD_V3_BOOTSTRAP_VERSION ||
            !bootstrap_->query_interface ||
            !bootstrap_->get_interface_info ||
            !bootstrap_->get_last_error ||
            !bootstrap_->get_client_info) {
            return Status(WOTBMOD_V3_E_INCOMPATIBLE);
        }
        return Status();
    }

    [[nodiscard]] bool valid() const noexcept {
        return status().ok();
    }

    [[nodiscard]] constexpr const WotbModV3Bootstrap* get()
        const noexcept {
        return bootstrap_;
    }

    template <typename Interface>
    [[nodiscard]] Result<const Interface*> query_interface(
        WotbModV3Handle mod,
        const char* interface_name,
        uint32_t minimum_version) const noexcept {
        const Status bootstrap_status = status();
        if (!bootstrap_status.ok()) {
            return Result<const Interface*>::failure(bootstrap_status);
        }
        if (mod == WOTBMOD_V3_INVALID_HANDLE ||
            !interface_name || !interface_name[0] ||
            minimum_version == 0u) {
            return Result<const Interface*>::failure(
                WOTBMOD_V3_E_INVALID_ARGUMENT);
        }
        const void* table = nullptr;
        const WotbModV3Result code = bootstrap_->query_interface(
            mod,
            interface_name,
            minimum_version,
            &table);
        if (code != WOTBMOD_V3_OK) {
            return Result<const Interface*>::failure(code);
        }
        if (!table) {
            return Result<const Interface*>::failure(
                WOTBMOD_V3_E_INCOMPATIBLE);
        }
        const WotbModV3StructHeader* header =
            static_cast<const WotbModV3StructHeader*>(table);
        if (header->struct_size < sizeof(Interface) ||
            header->api_version < minimum_version) {
            return Result<const Interface*>::failure(
                WOTBMOD_V3_E_INCOMPATIBLE);
        }
        return Result<const Interface*>::success(
            static_cast<const Interface*>(table));
    }

    template <typename Interface>
    [[nodiscard]] Result<const Interface*> query_interface(
        WotbModV3Handle mod) const noexcept {
        return query_interface<Interface>(
            mod,
            InterfaceTraits<Interface>::name(),
            InterfaceTraits<Interface>::version());
    }

    [[nodiscard]] ModContext context(
        WotbModV3Handle mod) const noexcept;

private:
    const WotbModV3Bootstrap* bootstrap_ = nullptr;
};

class ModContext final {
public:
    constexpr ModContext() noexcept = default;
    constexpr ModContext(
        const WotbModV3Bootstrap* bootstrap,
        WotbModV3Handle mod) noexcept
        : bootstrap_(bootstrap), mod_(mod) {}
    constexpr ModContext(
        BootstrapView bootstrap,
        WotbModV3Handle mod) noexcept
        : bootstrap_(bootstrap), mod_(mod) {}

    [[nodiscard]] Status status() const noexcept {
        const Status bootstrap_status = bootstrap_.status();
        if (!bootstrap_status.ok()) return bootstrap_status;
        if (mod_ == WOTBMOD_V3_INVALID_HANDLE) {
            return Status(WOTBMOD_V3_E_INVALID_HANDLE);
        }
        WotbModV3InterfaceInfo info = {};
        WOTBMOD_V3_INIT_STRUCT(
            info,
            WOTBMOD_V3_ABI_VERSION);
        return Status(
            bootstrap_.get()->get_interface_info(
                mod_,
                WOTBMOD_V3_IFACE_CORE,
                &info));
    }

    [[nodiscard]] bool valid() const noexcept {
        return status().ok();
    }

    [[nodiscard]] constexpr WotbModV3Handle mod() const noexcept {
        return mod_;
    }

    [[nodiscard]] constexpr BootstrapView bootstrap() const noexcept {
        return bootstrap_;
    }

    template <typename Interface>
    [[nodiscard]] Result<const Interface*> query_interface()
        const noexcept {
        const Status context_status = status();
        if (!context_status.ok()) {
            return Result<const Interface*>::failure(context_status);
        }
        return bootstrap_.query_interface<Interface>(mod_);
    }

    template <typename Interface>
    [[nodiscard]] Result<const Interface*> query_interface(
        const char* interface_name,
        uint32_t minimum_version) const noexcept {
        const Status context_status = status();
        if (!context_status.ok()) {
            return Result<const Interface*>::failure(context_status);
        }
        return bootstrap_.query_interface<Interface>(
            mod_,
            interface_name,
            minimum_version);
    }

    [[nodiscard]] Status get_interface_info(
        const char* interface_name,
        WotbModV3InterfaceInfo& out_info) const noexcept {
        out_info = {};
        WOTBMOD_V3_INIT_STRUCT(
            out_info,
            WOTBMOD_V3_ABI_VERSION);
        const Status context_status = status();
        if (!context_status.ok()) return context_status;
        if (!interface_name || !interface_name[0]) {
            return Status(WOTBMOD_V3_E_INVALID_ARGUMENT);
        }
        return Status(bootstrap_.get()->get_interface_info(
            mod_,
            interface_name,
            &out_info));
    }

    [[nodiscard]] Status get_last_error(
        WotbModV3ErrorInfo& out_error) const noexcept {
        out_error = {};
        WOTBMOD_V3_INIT_STRUCT(
            out_error,
            WOTBMOD_V3_ABI_VERSION);
        const Status context_status = status();
        if (!context_status.ok()) return context_status;
        return Status(
            bootstrap_.get()->get_last_error(mod_, &out_error));
    }

    [[nodiscard]] Status get_client_info(
        WotbModV3ClientInfo& out_info) const noexcept {
        out_info = {};
        WOTBMOD_V3_INIT_STRUCT(
            out_info,
            WOTBMOD_V3_ABI_VERSION);
        const Status context_status = status();
        if (!context_status.ok()) return context_status;
        return Status(
            bootstrap_.get()->get_client_info(mod_, &out_info));
    }

private:
    BootstrapView bootstrap_;
    WotbModV3Handle mod_ = WOTBMOD_V3_INVALID_HANDLE;
};

inline ModContext BootstrapView::context(
    WotbModV3Handle mod) const noexcept {
    return ModContext(*this, mod);
}

/*
 * Owns one runtime handle reference. adopt() consumes an already-owned
 * reference without retaining it; retain() and copies acquire a new
 * reference. Keep this object inside the lifetime of its runtime/mod.
 *
 * The destructor is necessarily best-effort. Call close() when a release
 * error must be observed. clone(), copy_from(), move_from(), info(), and
 * is_alive() always return the underlying ABI status.
 */
class OwnedHandle final {
public:
    constexpr OwnedHandle() noexcept = default;

    OwnedHandle(const OwnedHandle& other) noexcept {
        const Status copy_status = acquire_copy(other);
        last_status_ = copy_status;
    }

    OwnedHandle(OwnedHandle&& other) noexcept
        : api_(other.api_),
          mod_(other.mod_),
          handle_(other.handle_),
          last_status_(other.last_status_) {
        other.clear();
    }

    OwnedHandle& operator=(const OwnedHandle&) = delete;
    OwnedHandle& operator=(OwnedHandle&&) = delete;

    ~OwnedHandle() noexcept {
        if (handle_ != WOTBMOD_V3_INVALID_HANDLE &&
            api_ && api_->release) {
            (void)api_->release(mod_, handle_);
        }
    }

    [[nodiscard]] static Result<OwnedHandle> adopt(
        const ModContext& context,
        WotbModV3Handle handle) noexcept;

    [[nodiscard]] static Result<OwnedHandle> retain(
        const ModContext& context,
        WotbModV3Handle handle) noexcept;

    [[nodiscard]] Result<OwnedHandle> clone() const noexcept {
        if (handle_ == WOTBMOD_V3_INVALID_HANDLE) {
            if (!last_status_.ok()) {
                return Result<OwnedHandle>::failure(last_status_);
            }
            return Result<OwnedHandle>::failure(
                WOTBMOD_V3_E_INVALID_HANDLE);
        }
        if (!api_ || !api_->retain) {
            return Result<OwnedHandle>::failure(
                WOTBMOD_V3_E_NOT_SUPPORTED);
        }
        const WotbModV3Result code = api_->retain(mod_, handle_);
        if (code != WOTBMOD_V3_OK) {
            return Result<OwnedHandle>::failure(code);
        }
        return Result<OwnedHandle>::success(
            OwnedHandle(api_, mod_, handle_));
    }

    [[nodiscard]] Status copy_from(
        const OwnedHandle& other) noexcept {
        if (this == &other) {
            last_status_ = Status();
            return last_status_;
        }

        if (other.handle_ == WOTBMOD_V3_INVALID_HANDLE) {
            const Status close_status = close();
            if (!close_status.ok()) return close_status;
            last_status_ = other.last_status_;
            return last_status_;
        }
        if (!other.api_ || !other.api_->retain) {
            last_status_ = Status(WOTBMOD_V3_E_NOT_SUPPORTED);
            return last_status_;
        }

        const WotbModV3Result retain_code =
            other.api_->retain(other.mod_, other.handle_);
        if (retain_code != WOTBMOD_V3_OK) {
            last_status_ = Status(retain_code);
            return last_status_;
        }

        const Status close_status = close();
        if (!close_status.ok()) {
            if (other.api_->release) {
                (void)other.api_->release(
                    other.mod_,
                    other.handle_);
            }
            last_status_ = close_status;
            return last_status_;
        }

        api_ = other.api_;
        mod_ = other.mod_;
        handle_ = other.handle_;
        last_status_ = Status();
        return last_status_;
    }

    [[nodiscard]] Status move_from(OwnedHandle&& other) noexcept {
        if (this == &other) {
            last_status_ = Status();
            return last_status_;
        }
        const Status close_status = close();
        if (!close_status.ok()) return close_status;

        api_ = other.api_;
        mod_ = other.mod_;
        handle_ = other.handle_;
        last_status_ =
            handle_ == WOTBMOD_V3_INVALID_HANDLE
                ? other.last_status_
                : Status();
        other.clear();
        return last_status_;
    }

    [[nodiscard]] Status close() noexcept {
        if (handle_ == WOTBMOD_V3_INVALID_HANDLE) {
            last_status_ = Status();
            return last_status_;
        }
        if (!api_ || !api_->release) {
            last_status_ = Status(WOTBMOD_V3_E_NOT_SUPPORTED);
            return last_status_;
        }
        const WotbModV3Result code = api_->release(mod_, handle_);
        last_status_ = Status(code);
        if (code == WOTBMOD_V3_OK) clear();
        return Status(code);
    }

    [[nodiscard]] WotbModV3Handle detach() noexcept {
        const WotbModV3Handle value = handle_;
        clear();
        return value;
    }

    [[nodiscard]] Status info(
        WotbModV3HandleInfo& out_info) const noexcept {
        out_info = {};
        WOTBMOD_V3_INIT_STRUCT(
            out_info,
            WOTBMOD_V3_ABI_VERSION);
        if (handle_ == WOTBMOD_V3_INVALID_HANDLE) {
            return Status(WOTBMOD_V3_E_INVALID_HANDLE);
        }
        if (!api_ || !api_->get_info) {
            return Status(WOTBMOD_V3_E_NOT_SUPPORTED);
        }
        return Status(api_->get_info(mod_, handle_, &out_info));
    }

    [[nodiscard]] Status is_alive(uint32_t& out_alive) const noexcept {
        out_alive = 0u;
        if (handle_ == WOTBMOD_V3_INVALID_HANDLE) {
            return Status(WOTBMOD_V3_E_INVALID_HANDLE);
        }
        if (!api_ || !api_->is_alive) {
            return Status(WOTBMOD_V3_E_NOT_SUPPORTED);
        }
        return Status(api_->is_alive(mod_, handle_, &out_alive));
    }

    [[nodiscard]] constexpr WotbModV3Handle get() const noexcept {
        return handle_;
    }

    [[nodiscard]] constexpr WotbModV3Handle owner_mod()
        const noexcept {
        return mod_;
    }

    [[nodiscard]] constexpr bool owns_handle() const noexcept {
        return handle_ != WOTBMOD_V3_INVALID_HANDLE;
    }

    constexpr explicit operator bool() const noexcept {
        return owns_handle();
    }

    [[nodiscard]] constexpr Status last_status() const noexcept {
        return last_status_;
    }

private:
    constexpr OwnedHandle(
        const WotbModV3HandlesApiV1* api,
        WotbModV3Handle mod,
        WotbModV3Handle handle) noexcept
        : api_(api), mod_(mod), handle_(handle) {}

    [[nodiscard]] Status acquire_copy(
        const OwnedHandle& other) noexcept {
        if (other.handle_ == WOTBMOD_V3_INVALID_HANDLE) {
            clear();
            return other.last_status_;
        }
        if (!other.api_ || !other.api_->retain) {
            clear();
            return Status(WOTBMOD_V3_E_NOT_SUPPORTED);
        }
        const WotbModV3Result code =
            other.api_->retain(other.mod_, other.handle_);
        if (code != WOTBMOD_V3_OK) {
            clear();
            return Status(code);
        }
        api_ = other.api_;
        mod_ = other.mod_;
        handle_ = other.handle_;
        return Status();
    }

    constexpr void clear() noexcept {
        api_ = nullptr;
        mod_ = WOTBMOD_V3_INVALID_HANDLE;
        handle_ = WOTBMOD_V3_INVALID_HANDLE;
        last_status_ = Status();
    }

    const WotbModV3HandlesApiV1* api_ = nullptr;
    WotbModV3Handle mod_ = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Handle handle_ = WOTBMOD_V3_INVALID_HANDLE;
    Status last_status_;
};

inline Result<OwnedHandle> OwnedHandle::adopt(
    const ModContext& context,
    WotbModV3Handle handle) noexcept {
    if (handle == WOTBMOD_V3_INVALID_HANDLE) {
        return Result<OwnedHandle>::failure(
            WOTBMOD_V3_E_INVALID_ARGUMENT);
    }
    const Result<const WotbModV3HandlesApiV1*> query =
        context.query_interface<WotbModV3HandlesApiV1>();
    if (!query.ok()) {
        return Result<OwnedHandle>::failure(query.status());
    }
    const WotbModV3HandlesApiV1* api = query.value_unchecked();
    if (!api || !api->get_info || !api->release) {
        return Result<OwnedHandle>::failure(
            WOTBMOD_V3_E_NOT_SUPPORTED);
    }
    WotbModV3HandleInfo info = {};
    WOTBMOD_V3_INIT_STRUCT(info, WOTBMOD_V3_ABI_VERSION);
    const WotbModV3Result code =
        api->get_info(context.mod(), handle, &info);
    if (code != WOTBMOD_V3_OK) {
        return Result<OwnedHandle>::failure(code);
    }
    return Result<OwnedHandle>::success(
        OwnedHandle(api, context.mod(), handle));
}

inline Result<OwnedHandle> OwnedHandle::retain(
    const ModContext& context,
    WotbModV3Handle handle) noexcept {
    if (handle == WOTBMOD_V3_INVALID_HANDLE) {
        return Result<OwnedHandle>::failure(
            WOTBMOD_V3_E_INVALID_ARGUMENT);
    }
    const Result<const WotbModV3HandlesApiV1*> query =
        context.query_interface<WotbModV3HandlesApiV1>();
    if (!query.ok()) {
        return Result<OwnedHandle>::failure(query.status());
    }
    const WotbModV3HandlesApiV1* api = query.value_unchecked();
    if (!api || !api->retain || !api->release) {
        return Result<OwnedHandle>::failure(
            WOTBMOD_V3_E_NOT_SUPPORTED);
    }
    const WotbModV3Result code =
        api->retain(context.mod(), handle);
    if (code != WOTBMOD_V3_OK) {
        return Result<OwnedHandle>::failure(code);
    }
    return Result<OwnedHandle>::success(
        OwnedHandle(api, context.mod(), handle));
}

}  // namespace v3
}  // namespace wotbmod
