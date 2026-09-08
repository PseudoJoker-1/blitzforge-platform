#include "../include/wotb_mod_runtime_v3.h"
#include "../include/wotbmod/content_v1.h"
#include "../include/wotbmod/vfs_v1.h"
#include "../include/wotbmod/vfs_v2.h"

#include <Windows.h>

#include <cstdio>
#include <cstring>

namespace {

uint32_t g_checks = 0u;
uint32_t g_failures = 0u;
uint32_t g_requestedTier = WOTBMOD_V3_PERMISSION_SAFE;

#define CHECK(expression)                                                \
    do {                                                                 \
        ++g_checks;                                                      \
        if (!(expression)) {                                             \
            ++g_failures;                                                \
            std::fprintf(                                                \
                stderr,                                                  \
                "check failed at line %d: %s\n",                       \
                __LINE__,                                                \
                #expression);                                            \
        }                                                                \
    } while (0)

WotbModV3Result WOTBMOD_V3_CALL TestEntry(
    const WotbModV3Bootstrap*,
    WotbModV3Handle,
    WotbModV3Info* outInfo) {
    if (!outInfo) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    *outInfo = {};
    WOTBMOD_V3_INIT_STRUCT(*outInfo, WOTBMOD_V3_ABI_VERSION);
    outInfo->requested_permission_tier = g_requestedTier;
    strcpy_s(
        outInfo->id,
        sizeof(outInfo->id),
        "tests.data-named-permissions");
    strcpy_s(
        outInfo->name,
        sizeof(outInfo->name),
        "Data named permission test");
    strcpy_s(
        outInfo->version,
        sizeof(outInfo->version),
        "1.0.0");
    strcpy_s(
        outInfo->author,
        sizeof(outInfo->author),
        "tests");
    return WOTBMOD_V3_OK;
}

template <typename T>
const T* Query(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod,
    const char* name,
    uint32_t version) {
    const void* table = nullptr;
    CHECK(
        bootstrap->query_interface(
            mod, name, version, &table) == WOTBMOD_V3_OK);
    CHECK(table != nullptr);
    return static_cast<const T*>(table);
}

WotbModV3Handle CreateRestrictedMod(
    const char* moduleName,
    const char* const* grants,
    uint32_t grantCount,
    uint32_t grantedTier) {
    WotbModV3Handle mod = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        WotbModV3Runtime_CreateMod(
            moduleName, grantedTier, &mod) == WOTBMOD_V3_OK);
    CHECK(mod != WOTBMOD_V3_INVALID_HANDLE);
    CHECK(
        WotbModV3Runtime_SetPermissionGrants(
            mod, grants, grantCount, 1u) == WOTBMOD_V3_OK);
    g_requestedTier = grantedTier;
    WotbModV3RuntimeModuleInfo module = {};
    WOTBMOD_V3_INIT_STRUCT(module, WOTBMOD_V3_ABI_VERSION);
    CHECK(
        WotbModV3Runtime_InvokeEntry(
            mod, &TestEntry, &module) == WOTBMOD_V3_OK);
    CHECK(WotbModV3Runtime_Enable(mod) == WOTBMOD_V3_OK);
    return mod;
}

void DestroyMod(WotbModV3Handle mod) {
    CHECK(WotbModV3Runtime_Disable(mod) == WOTBMOD_V3_OK);
    CHECK(WotbModV3Runtime_DestroyMod(mod) == WOTBMOD_V3_OK);
}

}  // namespace

int main() {
    WotbModV3RuntimeOptions options = {};
    options.struct_size = sizeof(options);
    options.api_version = WOTBMOD_V3_ABI_VERSION;
    options.game_directory = ".";
    options.mods_directory =
        "build\\v3_data_named_permissions\\mods";
    options.cache_directory =
        "build\\v3_data_named_permissions\\cache";
    options.config_directory =
        "build\\v3_data_named_permissions\\config";
    options.client_version = "data-named-permissions-test";
    CHECK(WotbModV3Runtime_Initialize(&options) == WOTBMOD_V3_OK);

    const WotbModV3Bootstrap* bootstrap =
        WotbModV3Runtime_GetBootstrap();
    CHECK(bootstrap != nullptr);

    const char* modOnlyGrants[] = {"resources.mod"};
    const WotbModV3Handle modOnly = CreateRestrictedMod(
        "data_mod_only.dll",
        modOnlyGrants,
        1u,
        WOTBMOD_V3_PERMISSION_REVIEWED);
    const WotbModV3VfsApiV1* modOnlyVfs =
        Query<WotbModV3VfsApiV1>(
            bootstrap,
            modOnly,
            WOTBMOD_V3_IFACE_VFS,
            WOTBMOD_V3_VFS_VERSION);
    const WotbModV3ContentApiV1* modOnlyContent =
        Query<WotbModV3ContentApiV1>(
            bootstrap,
            modOnly,
            WOTBMOD_V3_IFACE_CONTENT,
            WOTBMOD_V3_CONTENT_VERSION);
    WotbModV3Handle mount = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        modOnlyVfs->mount_overlay(
            modOnly,
            nullptr,
            nullptr,
            nullptr,
            0,
            &mount) == WOTBMOD_V3_E_PERMISSION_DENIED);
    CHECK(mount == WOTBMOD_V3_INVALID_HANDLE);
    CHECK(
        modOnlyContent->apply(
            modOnly,
            WOTBMOD_V3_INVALID_HANDLE) ==
        WOTBMOD_V3_E_PERMISSION_DENIED);
    DestroyMod(modOnly);

    const char* broadSafeGrants[] = {"resources"};
    const WotbModV3Handle broadSafe = CreateRestrictedMod(
        "data_broad_safe.dll",
        broadSafeGrants,
        1u,
        WOTBMOD_V3_PERMISSION_SAFE);
    const WotbModV3VfsApiV1* broadSafeVfs =
        Query<WotbModV3VfsApiV1>(
            bootstrap,
            broadSafe,
            WOTBMOD_V3_IFACE_VFS,
            WOTBMOD_V3_VFS_VERSION);
    CHECK(
        broadSafeVfs->mount_overlay(
            broadSafe,
            nullptr,
            nullptr,
            nullptr,
            0,
            &mount) == WOTBMOD_V3_E_PERMISSION_DENIED);
    DestroyMod(broadSafe);

    const char* overlayGrants[] = {
        "resources.mod",
        "resources.overlay.game"
    };
    const WotbModV3Handle overlay = CreateRestrictedMod(
        "data_overlay.dll",
        overlayGrants,
        2u,
        WOTBMOD_V3_PERMISSION_REVIEWED);
    const WotbModV3VfsApiV1* overlayVfs =
        Query<WotbModV3VfsApiV1>(
            bootstrap,
            overlay,
            WOTBMOD_V3_IFACE_VFS,
            WOTBMOD_V3_VFS_VERSION);
    CHECK(
        overlayVfs->mount_overlay(
            overlay,
            nullptr,
            nullptr,
            nullptr,
            0,
            &mount) == WOTBMOD_V3_E_INVALID_ARGUMENT);
    const WotbModV3ContentApiV1* overlayContent =
        Query<WotbModV3ContentApiV1>(
            bootstrap,
            overlay,
            WOTBMOD_V3_IFACE_CONTENT,
            WOTBMOD_V3_CONTENT_VERSION);
    CHECK(
        overlayContent->apply(
            overlay,
            WOTBMOD_V3_INVALID_HANDLE) ==
        WOTBMOD_V3_E_INVALID_HANDLE);
    const char* packageRoot =
        "build\\v3_data_named_permissions\\mods\\data\\data_overlay";
    const char* assetDirectory =
        "build\\v3_data_named_permissions\\mods\\data\\data_overlay\\assets";
    const char* assetPath =
        "build\\v3_data_named_permissions\\mods\\data\\data_overlay\\assets\\test.asset";
    CHECK(
        CreateDirectoryA(assetDirectory, nullptr) != 0 ||
        GetLastError() == ERROR_ALREADY_EXISTS);
    std::FILE* assetFile = nullptr;
    CHECK(
        fopen_s(&assetFile, assetPath, "wb") == 0 &&
        assetFile != nullptr);
    if (assetFile) {
        const char payload[] = "content-overlay";
        CHECK(
            std::fwrite(
                payload,
                1u,
                sizeof(payload) - 1u,
                assetFile) ==
            sizeof(payload) - 1u);
        std::fclose(assetFile);
    }
    WotbModV3Handle packageMount =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        overlayVfs->mount_package(
            overlay,
            "tests.content.package",
            packageRoot,
            0,
            &packageMount) == WOTBMOD_V3_OK);
    CHECK(packageMount != WOTBMOD_V3_INVALID_HANDLE);

    struct FileOverlayCase {
        const char* category;
        const char* target;
    };
    const FileOverlayCase fileOverlayCases[] = {
        {"ui", "game://truth-contract/ui.asset"},
        {"textures", "game://truth-contract/texture.asset"},
        {"models", "game://truth-contract/model.asset"}
    };
    for (const FileOverlayCase& testCase : fileOverlayCases) {
        char descriptor[1024] = {};
        const int descriptorSize = std::snprintf(
            descriptor,
            sizeof(descriptor),
            "{\"type\":\"content\",\"id\":\"tests.content\","
            "\"overrides\":{\"%s\":{\"%s\":"
            "\"assets/test.asset\"}}}",
            testCase.category,
            testCase.target);
        CHECK(
            descriptorSize > 0 &&
            static_cast<size_t>(descriptorSize) <
                sizeof(descriptor));
        WotbModV3ConstBuffer buffer = {};
        WOTBMOD_V3_INIT_STRUCT(
            buffer,
            WOTBMOD_V3_CONTENT_VERSION);
        buffer.data = descriptor;
        buffer.size = static_cast<uint32_t>(descriptorSize);
        WotbModV3Handle content =
            WOTBMOD_V3_INVALID_HANDLE;
        CHECK(
            overlayContent->parse_json(
                overlay, &buffer, &content) ==
            WOTBMOD_V3_OK);
        CHECK(content != WOTBMOD_V3_INVALID_HANDLE);
        CHECK(
            overlayContent->apply(overlay, content) ==
            WOTBMOD_V3_OK);
        char resolved[WOTBMOD_V3_MAX_PATH] = {};
        uint32_t resolvedSize = sizeof(resolved);
        CHECK(
            overlayVfs->resolve(
                overlay,
                testCase.target,
                resolved,
                &resolvedSize) == WOTBMOD_V3_OK);
        CHECK(
            std::strstr(resolved, "test.asset") != nullptr);
        CHECK(
            overlayContent->unapply(overlay, content) ==
            WOTBMOD_V3_OK);
        uint32_t providerCount = 0u;
        CHECK(
            overlayVfs->get_providers(
                overlay,
                testCase.target,
                nullptr,
                &providerCount) == WOTBMOD_V3_OK);
        CHECK(providerCount == 0u);
    }

    const char* const unsupportedSemanticCategories[] = {
        "audio",
        "hangar",
        "localization"
    };
    for (const char* category :
         unsupportedSemanticCategories) {
        char descriptor[1024] = {};
        const int descriptorSize = std::snprintf(
            descriptor,
            sizeof(descriptor),
            "{\"type\":\"content\",\"id\":\"tests.semantic\","
            "\"overrides\":{\"%s\":{\"truth.contract\":"
            "\"assets/test.asset\"}}}",
            category);
        CHECK(
            descriptorSize > 0 &&
            static_cast<size_t>(descriptorSize) <
                sizeof(descriptor));
        WotbModV3ConstBuffer buffer = {};
        WOTBMOD_V3_INIT_STRUCT(
            buffer,
            WOTBMOD_V3_CONTENT_VERSION);
        buffer.data = descriptor;
        buffer.size = static_cast<uint32_t>(descriptorSize);
        WotbModV3Handle content =
            WOTBMOD_V3_INVALID_HANDLE;
        CHECK(
            overlayContent->parse_json(
                overlay, &buffer, &content) ==
            WOTBMOD_V3_OK);
        CHECK(content != WOTBMOD_V3_INVALID_HANDLE);
        CHECK(
            overlayContent->apply(overlay, content) ==
            WOTBMOD_V3_E_NOT_SUPPORTED);
        WotbModV3ErrorInfo error = {};
        WOTBMOD_V3_INIT_STRUCT(
            error,
            WOTBMOD_V3_ABI_VERSION);
        CHECK(
            bootstrap->get_last_error(overlay, &error) ==
            WOTBMOD_V3_OK);
        CHECK(
            std::strstr(
                error.message,
                "semantic audio, hangar and localization") !=
            nullptr);
    }
    CHECK(
        overlayVfs->unmount(overlay, packageMount) ==
        WOTBMOD_V3_OK);
    DestroyMod(overlay);
    DeleteFileA(assetPath);
    RemoveDirectoryA(assetDirectory);

    /*
     * VFS V2's four write routes.
     *
     * THE TWO GATES ARE CHECKED SEPARATELY, and that is the point of this
     * block. A single gate that happened to be the TIER would admit a mod that
     * never asked for the permission; a single gate that happened to be the
     * PERMISSION would admit a SAFE mod carrying a grant it should never have
     * been given. Only a mod that clears both writes anything, and each half is
     * shown refusing on its own below.
     */
    {
        WotbModV3ConstBuffer payload = {};
        WOTBMOD_V3_INIT_STRUCT(payload, WOTBMOD_V3_ABI_VERSION);
        static const char kPayload[] = "camo:\n";
        payload.data = kPayload;
        payload.size =
            static_cast<uint32_t>(sizeof(kPayload) - 1u);

        const char* readGrants[] = {"resources.mod"};
        const WotbModV3Handle reader = CreateRestrictedMod(
            "data_vfs_reader.dll",
            readGrants,
            1u,
            WOTBMOD_V3_PERMISSION_REVIEWED);
        const WotbModV3VfsApiV2* readerVfs =
            Query<WotbModV3VfsApiV2>(
                bootstrap,
                reader,
                WOTBMOD_V3_IFACE_VFS,
                WOTBMOD_V3_VFS_VERSION_2);
        // REVIEWED, but never asked to write: all four refuse.
        CHECK(
            readerVfs->write_file(
                reader, "data://self/w.txt", &payload) ==
            WOTBMOD_V3_E_PERMISSION_DENIED);
        CHECK(
            readerVfs->append_file(
                reader, "data://self/w.txt", &payload) ==
            WOTBMOD_V3_E_PERMISSION_DENIED);
        CHECK(
            readerVfs->copy_file(
                reader, "data://self/a", "data://self/b") ==
            WOTBMOD_V3_E_PERMISSION_DENIED);
        CHECK(
            readerVfs->remove_file(
                reader, "data://self/w.txt") ==
            WOTBMOD_V3_E_PERMISSION_DENIED);
        // The V1 table is still reachable at V1, unchanged by any of this.
        CHECK(
            Query<WotbModV3VfsApiV1>(
                bootstrap,
                reader,
                WOTBMOD_V3_IFACE_VFS,
                WOTBMOD_V3_VFS_VERSION) != nullptr);
        DestroyMod(reader);

        const char* writeGrants[] = {
            "resources.mod",
            "resources.write.mod_data",
            "resources.overlay.game"
        };
        const WotbModV3Handle safeWriter = CreateRestrictedMod(
            "data_vfs_safe_writer.dll",
            writeGrants,
            3u,
            WOTBMOD_V3_PERMISSION_SAFE);
        const WotbModV3VfsApiV2* safeVfs =
            Query<WotbModV3VfsApiV2>(
                bootstrap,
                safeWriter,
                WOTBMOD_V3_IFACE_VFS,
                WOTBMOD_V3_VFS_VERSION_2);
        // Holds the name, sits below the tier: still refused.
        CHECK(
            safeVfs->write_file(
                safeWriter, "data://self/w.txt", &payload) ==
            WOTBMOD_V3_E_PERMISSION_DENIED);
        DestroyMod(safeWriter);

        const WotbModV3Handle writer = CreateRestrictedMod(
            "data_vfs_writer.dll",
            writeGrants,
            3u,
            WOTBMOD_V3_PERMISSION_REVIEWED);
        const WotbModV3VfsApiV2* writerVfs =
            Query<WotbModV3VfsApiV2>(
                bootstrap,
                writer,
                WOTBMOD_V3_IFACE_VFS,
                WOTBMOD_V3_VFS_VERSION_2);
        CHECK(
            writerVfs->write_file(
                writer, "data://self/atelier/w.yaml", &payload) ==
            WOTBMOD_V3_OK);

        /*
         * THE COMPOSITION THIS INTERFACE EXISTS FOR, end to end: copy a base
         * document natively, append a fragment to it, and point a `game://`
         * overlay at the result. That is how a user camouflage becomes an entry
         * the CLIENT's own reader sees - the game opens
         * `~res:/Data/camouflages.yaml.dvpl`, the loader's DAVA::File::Create
         * detour asks this same overlay chain, and gets the composed file back.
         * The detour itself needs a running client; the resolution below is the
         * half that can be proven here, and it is the half that decides.
         */
        CHECK(
            writerVfs->copy_file(
                writer,
                "data://self/atelier/w.yaml",
                "data://self/atelier/camouflages.yaml") ==
            WOTBMOD_V3_OK);
        WotbModV3ConstBuffer fragment = {};
        WOTBMOD_V3_INIT_STRUCT(fragment, WOTBMOD_V3_ABI_VERSION);
        static const char kFragment[] =
            "user_camo_01:\n    type: 1\n";
        fragment.data = kFragment;
        fragment.size =
            static_cast<uint32_t>(sizeof(kFragment) - 1u);
        CHECK(
            writerVfs->append_file(
                writer,
                "data://self/atelier/camouflages.yaml",
                &fragment) == WOTBMOD_V3_OK);
        {
            // The appended file is base + fragment, in that order, and the
            // caller never held the base.
            WotbModV3Handle composed = WOTBMOD_V3_INVALID_HANDLE;
            CHECK(
                writerVfs->v1.open(
                    writer,
                    "data://self/atelier/camouflages.yaml",
                    &composed) == WOTBMOD_V3_OK);
            char bytes[128] = {};
            WotbModV3Buffer read = {};
            WOTBMOD_V3_INIT_STRUCT(read, WOTBMOD_V3_ABI_VERSION);
            read.data = bytes;
            read.capacity = sizeof(bytes);
            read.size = 0u;
            CHECK(
                writerVfs->v1.read(writer, composed, 0u, &read) ==
                WOTBMOD_V3_OK);
            CHECK(
                read.size ==
                payload.size + fragment.size);
            CHECK(std::strncmp(bytes, "camo:\n", 6u) == 0);
            CHECK(
                std::strstr(bytes, "user_camo_01:") != nullptr);
        }
        {
            // And a game:// overlay now answers with that composed file.
            WotbModV3Handle overlayMount =
                WOTBMOD_V3_INVALID_HANDLE;
            CHECK(
                writerVfs->v1.mount_overlay(
                    writer,
                    "atelier",
                    "game://Data/camouflages.yaml.dvpl",
                    "data://self/atelier/camouflages.yaml",
                    100,
                    &overlayMount) == WOTBMOD_V3_OK);
            char resolved[WOTBMOD_V3_VFS_URI_MAX] = {};
            uint32_t resolvedSize =
                static_cast<uint32_t>(sizeof(resolved));
            CHECK(
                writerVfs->v1.resolve(
                    writer,
                    "game://Data/camouflages.yaml.dvpl",
                    resolved,
                    &resolvedSize) == WOTBMOD_V3_OK);
            CHECK(
                std::strstr(resolved, "camouflages.yaml") !=
                nullptr);
            // Not the game's own file: the overlay won.
            CHECK(std::strstr(resolved, ".dvpl") == nullptr);
            CHECK(
                writerVfs->v1.unmount(writer, overlayMount) ==
                WOTBMOD_V3_OK);
        }
        CHECK(
            writerVfs->remove_file(
                writer, "data://self/atelier/camouflages.yaml") ==
            WOTBMOD_V3_OK);
        /*
         * THE DESTINATION FENCE, which is the entire safety argument for this
         * interface existing. `game://` is the one a camouflage overlay would
         * most want and the one that must never be writable: the composed file
         * goes to data:// and an OVERLAY points the game at it.
         */
        CHECK(
            writerVfs->write_file(
                writer, "game://Data/camouflages.yaml", &payload) ==
            WOTBMOD_V3_E_PERMISSION_DENIED);
        CHECK(
            writerVfs->copy_file(
                writer,
                "data://self/atelier/w.yaml",
                "game://Data/camouflages.yaml") ==
            WOTBMOD_V3_E_PERMISSION_DENIED);
        CHECK(
            writerVfs->append_file(
                writer, "game://Data/camouflages.yaml", &payload) ==
            WOTBMOD_V3_E_PERMISSION_DENIED);
        CHECK(
            writerVfs->remove_file(
                writer, "game://Data/camouflages.yaml") ==
            WOTBMOD_V3_E_PERMISSION_DENIED);
        /*
         * Removing answers NOT_FOUND rather than reporting success over an
         * absent file, so "make sure this is gone" is a call whose result means
         * something.
         */
        CHECK(
            writerVfs->remove_file(
                writer, "data://self/atelier/w.yaml") ==
            WOTBMOD_V3_OK);
        CHECK(
            writerVfs->remove_file(
                writer, "data://self/atelier/w.yaml") ==
            WOTBMOD_V3_E_NOT_FOUND);
        DestroyMod(writer);
    }

    WotbModV3Runtime_Shutdown();
    std::printf(
        "V3 data named permissions: %u checks, %u failures\n",
        g_checks,
        g_failures);
    return g_failures == 0u ? 0 : 1;
}
