#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../include/wotb_mod_runtime.h"
#include "../include/wotbmod/client_v1.h"

namespace {

namespace fs = std::filesystem;

std::vector<std::string> g_logs;

void WOTBMOD_CALL LogSink(
    WotbModLogLevel,
    const char* message,
    void*) {
    g_logs.emplace_back(message ? message : "");
}

WotbModV3Result WOTBMOD_V3_CALL NativeInvokeUnavailable(
    void*,
    WotbModV3Handle,
    const char*,
    const void*,
    uint32_t,
    void*,
    uint32_t) {
    return WOTBMOD_V3_E_NOT_SUPPORTED;
}

bool WriteText(
    const fs::path& path,
    const std::string& text) {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (error) return false;
    std::ofstream output(
        path,
        std::ios::binary | std::ios::trunc);
    output.write(
        text.data(),
        static_cast<std::streamsize>(text.size()));
    return output.good();
}

bool WriteBinary(
    const fs::path& path,
    const std::vector<uint8_t>& bytes) {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (error) return false;
    std::ofstream output(
        path,
        std::ios::binary | std::ios::trunc);
    if (!bytes.empty()) {
        output.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    }
    return output.good();
}

uint32_t Crc32(
    const uint8_t* bytes,
    size_t size) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t index = 0u; index < size; ++index) {
        crc ^= bytes[index];
        for (uint32_t bit = 0u; bit < 8u; ++bit) {
            const uint32_t mask =
                0u - static_cast<uint32_t>(crc & 1u);
            crc = (crc >> 1u) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

void AppendLe32(
    std::vector<uint8_t>* bytes,
    uint32_t value) {
    bytes->push_back(static_cast<uint8_t>(value));
    bytes->push_back(static_cast<uint8_t>(value >> 8u));
    bytes->push_back(static_cast<uint8_t>(value >> 16u));
    bytes->push_back(static_cast<uint8_t>(value >> 24u));
}

std::vector<uint8_t> BuildRawDvpl(
    const char* text) {
    const size_t size = std::strlen(text);
    std::vector<uint8_t> bytes(
        reinterpret_cast<const uint8_t*>(text),
        reinterpret_cast<const uint8_t*>(text) + size);
    const uint32_t payloadSize = static_cast<uint32_t>(size);
    const uint32_t crc = Crc32(bytes.data(), bytes.size());
    AppendLe32(&bytes, payloadSize);
    AppendLe32(&bytes, payloadSize);
    AppendLe32(&bytes, crc);
    AppendLe32(&bytes, 0u);
    AppendLe32(&bytes, 0x4C505644u);
    return bytes;
}

bool CopyBinary(
    const fs::path& source,
    const fs::path& destination) {
    std::error_code error;
    fs::create_directories(destination.parent_path(), error);
    if (error) return false;
    fs::copy_file(
        source,
        destination,
        fs::copy_options::overwrite_existing,
        error);
    return !error;
}

size_t CountLog(const char* needle) {
    size_t count = 0u;
    for (const std::string& line : g_logs) {
        if (line.find(needle) != std::string::npos) ++count;
    }
    return count;
}

std::string EnvText(const char* name) {
    char text[256] = {};
    const DWORD length = GetEnvironmentVariableA(name, text, sizeof(text));
    return length > 0u && length < sizeof(text) ? std::string(text)
                                                : std::string();
}

bool ContainsLog(const char* needle) {
    for (const std::string& line : g_logs) {
        if (line.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

int Fail(const char* message) {
    std::cerr << "V3 PACKAGE RUNTIME FAILED: "
              << message << "\n";
    for (const std::string& line : g_logs) {
        std::cerr << "  " << line << "\n";
    }
    WotbModRuntime_Shutdown();
    return 1;
}

std::string ClientPolicy() {
    return
        "\"client\":{\"builds\":[\"package-test-build\"],"
        "\"executable_hashes\":[\""
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"]},";
}

bool PrepareEnvironment(
    const fs::path& root,
    const fs::path& packageMod,
    const fs::path& looseMod) {
    const fs::path mods = root / "mods";
    const fs::path valid = mods / "valid";
    const fs::path mismatch = mods / "mismatch";
    const fs::path content = mods / "content";
    std::error_code error;
    fs::remove_all(root, error);
    error.clear();
    fs::create_directories(mods, error);
    if (error) return false;

    const std::string validManifest =
        "{\"manifest_version\":1,\"type\":\"native\","
        "\"id\":\"tests.package-runtime\","
        "\"name\":\"Package Runtime Test\","
        "\"version\":\"1.2.3\",\"developer\":\"tests\"," +
        ClientPolicy() +
        "\"entrypoints\":{\"windows-x86\":\"bin/package_mod.dll\"},"
        "\"dependencies\":{\"tests.package-content\":\"^1.0\"},"
        "\"optional_dependencies\":{"
        "\"tests.package-optional\":\"^1.0\"},"
        "\"permissions\":[\"native.hook.address\",\"resources.mod\"]}";
    const std::string mismatchManifest =
        "{\"manifest_version\":1,\"type\":\"native\","
        "\"id\":\"tests.package-mismatch\","
        "\"name\":\"Mismatch\",\"version\":\"9.9.9\","
        "\"developer\":\"tests\"," +
        ClientPolicy() +
        "\"entrypoints\":{\"windows-x86\":\"package_mod.dll\"},"
        "\"permissions\":[\"native.hook.address\",\"resources.mod\"]}";
    const std::string contentManifest =
        "{\"manifest_version\":1,\"type\":\"content\","
        "\"id\":\"tests.package-content\","
        "\"name\":\"Content\",\"version\":\"1.0.0\","
        "\"developer\":\"tests\",\"content\":\"content.json\","
        "\"permissions\":[\"resources.mod\","
        "\"resources.overlay.game\"]}";
    const std::string contentDescriptor =
        "{\"type\":\"content\","
        "\"id\":\"tests.package-content\","
        "\"name\":\"Content\",\"version\":\"1.0.0\","
        "\"overrides\":{\"ui\":{"
        "\"game://ui/package-content.txt\":{"
        "\"asset\":\"assets/content.txt\","
        "\"priority\":7}}}}";
    const std::string blockedManifest =
        "{\"manifest_version\":1,\"type\":\"native\","
        "\"id\":\"tests.package-blocked\","
        "\"name\":\"Blocked\",\"version\":\"1.0.0\","
        "\"developer\":\"tests\","
        "\"client\":{\"builds\":[\"different-build\"]},"
        "\"entrypoints\":{\"windows-x86\":\"blocked.dll\"},"
        "\"permissions\":[\"core\"]}";
    const std::vector<uint8_t> packedDvpl =
        BuildRawDvpl("dvpl-mounted");

    return
        CopyBinary(packageMod, valid / "bin" / "package_mod.dll") &&
        CopyBinary(packageMod, mismatch / "package_mod.dll") &&
        CopyBinary(packageMod, mods / "blocked.dll") &&
        CopyBinary(looseMod, mods / "loose.dll") &&
        WriteText(valid / "manifest.json", validManifest) &&
        WriteText(valid / "asset.txt", "mounted") &&
        WriteBinary(valid / "packed.dvpl", packedDvpl) &&
        WriteText(mismatch / "manifest.json", mismatchManifest) &&
        WriteText(mismatch / "asset.txt", "mounted") &&
        WriteBinary(mismatch / "packed.dvpl", packedDvpl) &&
        WriteText(content / "manifest.json", contentManifest) &&
        WriteText(content / "content.json", contentDescriptor) &&
        WriteText(
            content / "assets" / "content.txt",
            "content-overlay") &&
        WriteText(
            mods / "blocked.manifest.json",
            blockedManifest) &&
        WriteText(
            mods / "mods.ini",
            "[mods]\n"
            "tests.package-runtime=1\n"
            "tests.package-loose=1\n"
            "tests.package-content=1\n"
            "\n[permissions]\n"
            "tests.package-runtime=3\n"
            "tests.package-mismatch=3\n"
            "tests.package-content=2\n");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr
            << "usage: v3_package_runtime_tests.exe "
            << "<environment> <package-mod.dll> <loose-mod.dll>\n";
        return 2;
    }
    std::error_code error;
    const fs::path root = fs::absolute(fs::u8path(argv[1]), error);
    const fs::path packageMod =
        fs::absolute(fs::u8path(argv[2]), error);
    const fs::path looseMod =
        fs::absolute(fs::u8path(argv[3]), error);
    if (error || root == root.root_path() ||
        root.filename().empty() ||
        !PrepareEnvironment(root, packageMod, looseMod)) {
        return Fail("test environment preparation");
    }

    WotbModRuntimeV3ClientBackend backend = {};
    backend.struct_size = sizeof(backend);
    backend.api_version =
        WOTBMOD_RUNTIME_V3_CLIENT_BACKEND_VERSION;
    backend.binding_pack_version = 1u;
    backend.compatibility_state =
        WOTBMOD_V3_CLIENT_COMPATIBILITY_SUPPORTED;
    backend.invoke = &NativeInvokeUnavailable;
    strcpy_s(
        backend.client_build,
        sizeof(backend.client_build),
        "package-test-build");
    strcpy_s(
        backend.client_executable_sha256,
        sizeof(backend.client_executable_sha256),
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");

    const std::string rootText = root.u8string();
    WotbModRuntimeOptions options = {};
    options.struct_size = sizeof(options);
    options.game_directory = rootText.c_str();
    options.game_module = GetModuleHandleA(nullptr);
    options.log_sink = &LogSink;
    options.v3_client_backend = &backend;
    if (WotbModRuntime_Initialize(&options) != WOTBMOD_OK) {
        return Fail("runtime initialization");
    }
    const WotbModResult loadResult = WotbModRuntime_LoadAll();
    if (loadResult != WOTBMOD_ERROR_PLATFORM) {
        return Fail(
            "blocked/mismatched/content packages must surface failure");
    }

    const WotbModHostApi* host = WotbModRuntime_GetHostApi();
    if (!host || host->get_mod_count() != 3u) {
        return Fail(
            "valid native, content and unclaimed loose packages must load");
    }
    bool foundPackage = false;
    bool foundLoose = false;
    bool foundContent = false;
    for (uint32_t index = 0u;
         index < host->get_mod_count(); ++index) {
        WotbModPublicInfo info = {};
        info.struct_size = sizeof(info);
        if (host->get_mod_info(index, &info) != WOTBMOD_OK) {
            return Fail("loaded mod metadata");
        }
        foundPackage |=
            strcmp(info.id, "tests.package-runtime") == 0;
        foundLoose |=
            strcmp(info.id, "tests.package-loose") == 0;
        foundContent |=
            strcmp(info.id, "tests.package-content") == 0;
    }
    if (!foundPackage || !foundLoose || !foundContent) {
        return Fail(
            "expected native, content and loose identities");
    }
    if (!ContainsLog("package blocked") ||
        !ContainsLog("loaded content-only package") ||
        !ContainsLog("does not exactly match manifest") ||
        !ContainsLog("loose fallback suppressed")) {
        return Fail("required package diagnostics were not logged");
    }

    char contentPath[WOTBMOD_MAX_RESOURCE_PATH] = {};
    uint32_t contentPathSize = sizeof(contentPath);
    if (WotbModRuntime_ResolveResourcePath(
            "~res:/UI/package-content.txt",
            contentPath,
            &contentPathSize) != WOTBMOD_OK) {
        return Fail("content-only DAVA overlay resolution");
    }
    std::ifstream contentInput(contentPath, std::ios::binary);
    std::string contentText(
        (std::istreambuf_iterator<char>(contentInput)),
        std::istreambuf_iterator<char>());
    if (contentText != "content-overlay") {
        return Fail("content-only overlay payload");
    }
    if (host->set_mod_enabled(
            "tests.package-content", 0) != WOTBMOD_OK) {
        return Fail("content-only disable");
    }
    contentPathSize = sizeof(contentPath);
    if (WotbModRuntime_ResolveResourcePath(
            "~res:/UI/package-content.txt",
            contentPath,
            &contentPathSize) != WOTBMOD_ERROR_NOT_FOUND) {
        return Fail("content-only overlay removed on disable");
    }
    if (host->set_mod_enabled(
            "tests.package-content", 1) != WOTBMOD_OK) {
        return Fail("content-only re-enable");
    }
    contentPathSize = sizeof(contentPath);
    if (WotbModRuntime_ResolveResourcePath(
            "~res:/UI/package-content.txt",
            contentPath,
            &contentPathSize) != WOTBMOD_OK) {
        return Fail("content-only overlay restored on re-enable");
    }

    const fs::path validDll =
        root / "mods" / "valid" / "bin" / "package_mod.dll";
    HMODULE module =
        GetModuleHandleA(validDll.u8string().c_str());
    if (!module) {
        return Fail("valid package module handle");
    }
    using ProbeFn = uint32_t(WOTBMOD_V3_CALL*)();
    ProbeFn probeCount = reinterpret_cast<ProbeFn>(
        GetProcAddress(module, "PackageRuntimeProbeCount"));
    ProbeFn probeFailures = reinterpret_cast<ProbeFn>(
        GetProcAddress(module, "PackageRuntimeProbeFailures"));
    ProbeFn probeFailureMask = reinterpret_cast<ProbeFn>(
        GetProcAddress(module, "PackageRuntimeProbeFailureMask"));
    const uint32_t initialProbeCount =
        probeCount ? probeCount() : 0u;
    const uint32_t initialProbeFailures =
        probeFailures ? probeFailures() : UINT32_MAX;
    if (!probeCount || !probeFailures || !probeFailureMask ||
        initialProbeCount != 2u ||
        initialProbeFailures != 0u) {
        std::cerr
            << "probe-count=" << initialProbeCount
            << " probe-failures=" << initialProbeFailures
            << " failure-mask="
            << (probeFailureMask ? probeFailureMask() : UINT32_MAX)
            << "\n";
        return Fail(
            "package mount/client identity during entry and enable");
    }
    if (host->set_mod_enabled(
            "tests.package-runtime", 0) != WOTBMOD_OK ||
        host->set_mod_enabled(
            "tests.package-runtime", 1) != WOTBMOD_OK ||
        probeCount() != 3u || probeFailures() != 0u) {
        return Fail("package mount restoration on re-enable");
    }

    // Hot reload, end to end through the public lifecycle interface. The mod
    // requests its own reload from on_enable (see MaybeRequestReload in
    // v3_package_runtime_mod.cpp); nothing happens until the frame boundary,
    // where the runtime tears the record down, frees the DLL and loads the
    // package again through the same preflight as a cold boot.
    // The mismatch package copies the same DLL and its entry ran too, so the
    // counter is compared relative to where it stands now, not to 1.
    const std::string generationBefore =
        EnvText("WOTBMOD_TEST_LOAD_GENERATION");
    if (generationBefore.empty()) {
        return Fail("the package mod counts its loads");
    }
    const std::string generationAfter =
        std::to_string(std::atoi(generationBefore.c_str()) + 1);
    SetEnvironmentVariableA("WOTBMOD_TEST_REQUEST_RELOAD", "1");
    if (host->set_mod_enabled(
            "tests.package-runtime", 0) != WOTBMOD_OK ||
        host->set_mod_enabled(
            "tests.package-runtime", 1) != WOTBMOD_OK) {
        return Fail("re-enable that carries the reload request");
    }
    if (EnvText("WOTBMOD_TEST_RELOAD_RESULT") !=
        "before=0/1 request=0 after=0/0") {
        std::cerr << "reload-result="
                  << EnvText("WOTBMOD_TEST_RELOAD_RESULT") << "\n";
        return Fail(
            "can_hot_reload says yes, request_reload queues, and a queued "
            "reload is reported as not possible (flag 0, call still OK) until it runs");
    }
    if (!ContainsLog("hot reload requested") ||
        ContainsLog("hot reload complete") ||
        EnvText("WOTBMOD_TEST_LOAD_GENERATION") != generationBefore) {
        return Fail("request_reload only queues; the module stays loaded");
    }
    WotbModRuntime_DispatchFrame(nullptr, nullptr, nullptr, 1920, 1080, 0.016);
    if (!ContainsLog("hot reload: tearing down for reload") ||
        !ContainsLog("hot reload complete") ||
        ContainsLog("hot reload failed")) {
        return Fail("the frame boundary performs the queued reload");
    }
    if (EnvText("WOTBMOD_TEST_LOAD_GENERATION") != generationAfter) {
        return Fail("a fresh instance of the package mod came up");
    }
    if (host->get_mod_count() != 3u) {
        return Fail("the reloaded mod reuses its slot; the table did not grow");
    }
    module = GetModuleHandleA(validDll.u8string().c_str());
    probeCount = module ? reinterpret_cast<ProbeFn>(
        GetProcAddress(module, "PackageRuntimeProbeCount")) : nullptr;
    probeFailures = module ? reinterpret_cast<ProbeFn>(
        GetProcAddress(module, "PackageRuntimeProbeFailures")) : nullptr;
    if (!probeCount || !probeFailures || probeCount() != 2u ||
        probeFailures() != 0u) {
        return Fail(
            "the reloaded instance ran entry and enable once each and its "
            "package mount and client identity held");
    }
    const size_t completeLines = CountLog("hot reload complete");
    WotbModRuntime_DispatchFrame(nullptr, nullptr, nullptr, 1920, 1080, 0.016);
    if (CountLog("hot reload complete") != completeLines ||
        EnvText("WOTBMOD_TEST_LOAD_GENERATION") != generationAfter) {
        return Fail("a reload runs once; the next frame does not repeat it");
    }

    WotbModRuntime_Shutdown();
    fs::remove_all(root, error);
    std::cout << "V3 package runtime integration: OK\n";
    return 0;
}
