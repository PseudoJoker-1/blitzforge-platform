#include "../include/wotb_mod_runtime_v3.h"
#include "../include/wotbmod/async_v1.h"
#include "../include/wotbmod/handles_v1.h"
#include "../include/wotbmod/http_v1.h"
#include "../include/wotbmod/interface_ids.h"
#include "../src/v3/http_transport_test.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace wotbmod {
namespace v3 {

#if defined(WOTBMOD_V3_HTTP_STANDALONE_TEST)
void RegisterDataServices() {}
void RegisterClientServices() {}
/* runtime_services.cpp pumps the HUD from the main thread; the HUD lives in
 * client_services.cpp, which this test does not compile. */
void HudMainThreadTick() {}
void RegisterToolingServices() {}
#endif

}  // namespace v3
}  // namespace wotbmod

namespace {

enum FakeMode {
    FAKE_SUCCESS = 0,
    FAKE_WAIT_FOR_CANCEL = 1,
    FAKE_WAIT_FOR_RELEASE = 2,
    FAKE_OVERSIZE = 3,
    FAKE_TIMEOUT = 4
};

std::atomic<uint32_t> g_fake_mode{FAKE_SUCCESS};
std::atomic<uint32_t> g_fake_started{0u};
std::atomic<uint32_t> g_fake_finished{0u};
std::atomic<uint32_t> g_fake_gate{0u};
std::atomic<uint32_t> g_callback_count{0u};
std::atomic<uint32_t> g_callback_role{WOTBMOD_V3_THREAD_UNKNOWN};
std::atomic<uint32_t> g_callback_result{WOTBMOD_V3_E_PLATFORM};
std::atomic<uint64_t> g_frame_index{1u};
std::mutex g_capture_mutex;
wotbmod::v3::testing::HttpTransportRequest g_captured_request;
const WotbModV3AsyncApiV1* g_async = nullptr;

uint32_t g_passed = 0u;
uint32_t g_failed = 0u;

void Check(bool condition, const char* label) {
    if (condition) {
        ++g_passed;
        return;
    }
    ++g_failed;
    std::printf("FAIL: %s\n", label ? label : "(null)");
}

void Copy(char* output, size_t capacity, const char* value) {
#if defined(_MSC_VER)
    strncpy_s(output, capacity, value, _TRUNCATE);
#else
    std::strncpy(output, value, capacity - 1u);
    output[capacity - 1u] = '\0';
#endif
}

WotbModV3Result WOTBMOD_V3_CALL TestEntry(
    const WotbModV3Bootstrap*,
    WotbModV3Handle,
    WotbModV3Info* info) {
    if (!info) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    info->struct_size = sizeof(*info);
    info->api_version = WOTBMOD_V3_ABI_VERSION;
    info->requested_permission_tier = WOTBMOD_V3_PERMISSION_REVIEWED;
    Copy(info->id, sizeof(info->id), "tests.http-transport");
    Copy(info->name, sizeof(info->name), "HTTP transport tests");
    Copy(info->version, sizeof(info->version), "1.0.0");
    Copy(info->author, sizeof(info->author), "tests");
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL LiveTestEntry(
    const WotbModV3Bootstrap*,
    WotbModV3Handle,
    WotbModV3Info* info) {
    if (!info) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    info->struct_size = sizeof(*info);
    info->api_version = WOTBMOD_V3_ABI_VERSION;
    info->requested_permission_tier = WOTBMOD_V3_PERMISSION_REVIEWED;
    Copy(info->id, sizeof(info->id), "tests.http-transport-live");
    Copy(info->name, sizeof(info->name), "HTTP transport live test");
    Copy(info->version, sizeof(info->version), "1.0.0");
    Copy(info->author, sizeof(info->author), "tests");
    return WOTBMOD_V3_OK;
}

WotbModV3Result FakeTransport(
    const wotbmod::v3::testing::HttpTransportRequest& request,
    const wotbmod::v3::testing::HttpTransportContext& context,
    wotbmod::v3::testing::HttpTransportResponse* response) {
    {
        std::lock_guard<std::mutex> lock(g_capture_mutex);
        g_captured_request = request;
    }
    g_fake_started.store(1u);
    const uint32_t mode = g_fake_mode.load();
    if (mode == FAKE_WAIT_FOR_CANCEL ||
        mode == FAKE_WAIT_FOR_RELEASE) {
        const auto deadline =
            std::chrono::steady_clock::now() +
            std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            if (context.cancellation_requested &&
                context.cancellation_requested->load()) {
                g_fake_finished.store(1u);
                return WOTBMOD_V3_E_CANCELLED;
            }
            if (mode == FAKE_WAIT_FOR_RELEASE &&
                g_fake_gate.load() != 0u) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    if (mode == FAKE_TIMEOUT) {
        g_fake_finished.store(1u);
        return WOTBMOD_V3_E_TIMEOUT;
    }
    if (!response) {
        g_fake_finished.store(1u);
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    response->status = 201u;
    response->headers["content-type"] = "application/octet-stream";
    response->headers["x-runtime-test"] = "yes";
    if (mode == FAKE_OVERSIZE) {
        response->body.resize(
            static_cast<size_t>(request.max_response_size) + 1u,
            0xCDu);
    } else {
        response->body = {0x10u, 0x20u, 0x30u};
    }
    g_fake_finished.store(1u);
    return WOTBMOD_V3_OK;
}

void WOTBMOD_V3_CALL HttpCompletion(
    WotbModV3Handle mod,
    WotbModV3HttpHandle,
    WotbModV3Result result,
    void*) {
    uint32_t role = WOTBMOD_V3_THREAD_UNKNOWN;
    if (g_async) g_async->thread_get_current_role(mod, &role);
    g_callback_role.store(role);
    g_callback_result.store(result);
    g_callback_count.fetch_add(1u);
}

void ResetFake(uint32_t mode) {
    g_fake_mode.store(mode);
    g_fake_started.store(0u);
    g_fake_finished.store(0u);
    g_fake_gate.store(0u);
    std::lock_guard<std::mutex> lock(g_capture_mutex);
    g_captured_request =
        wotbmod::v3::testing::HttpTransportRequest();
}

bool WaitForAtomic(
    const std::atomic<uint32_t>& value,
    uint32_t expected,
    uint32_t timeout_ms = 3000u) {
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (value.load() == expected) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return value.load() == expected;
}

bool WaitForState(
    const WotbModV3HttpApiV1* http,
    WotbModV3Handle mod,
    WotbModV3HttpHandle request,
    uint32_t expected,
    uint32_t timeout_ms = 3000u) {
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        WotbModV3HttpRequestInfo info = {};
        WOTBMOD_V3_INIT_STRUCT(info, WOTBMOD_V3_HTTP_VERSION);
        if (http->request_get_info(mod, request, &info) ==
                WOTBMOD_V3_OK &&
            info.state == expected) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

bool PumpUntilCallback(
    WotbModV3Handle,
    uint32_t expected_count,
    uint32_t timeout_ms = 3000u) {
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        WotbModV3Runtime_DispatchFrame(
            g_frame_index.fetch_add(1u),
            0.001);
        if (g_callback_count.load() == expected_count) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return g_callback_count.load() == expected_count;
}

WotbModV3HttpHandle CreateConfiguredRequest(
    const WotbModV3HttpApiV1* http,
    WotbModV3Handle mod) {
    WotbModV3HttpHandle request = WOTBMOD_V3_INVALID_HANDLE;
    Check(
        http->request_create(mod, &request) == WOTBMOD_V3_OK,
        "request_create succeeds");
    Check(
        http->request_set_url(
            mod,
            request,
            "https://api.example.com/v3/test?case=1") ==
            WOTBMOD_V3_OK,
        "public HTTPS URL is accepted");
    return request;
}

void TestIpClassification() {
    using wotbmod::v3::testing::IsPublicIpLiteralForTesting;
    Check(
        IsPublicIpLiteralForTesting("8.8.8.8"),
        "public IPv4 is allowed");
    Check(
        IsPublicIpLiteralForTesting("1.1.1.1"),
        "second public IPv4 is allowed");
    Check(
        !IsPublicIpLiteralForTesting("0.0.0.0"),
        "unspecified IPv4 is blocked");
    Check(
        !IsPublicIpLiteralForTesting("10.0.0.1"),
        "private IPv4 is blocked");
    Check(
        !IsPublicIpLiteralForTesting("100.64.0.1"),
        "carrier-grade NAT IPv4 is blocked");
    Check(
        !IsPublicIpLiteralForTesting("127.0.0.1"),
        "loopback IPv4 is blocked");
    Check(
        !IsPublicIpLiteralForTesting("169.254.1.1"),
        "link-local IPv4 is blocked");
    Check(
        !IsPublicIpLiteralForTesting("172.16.0.1"),
        "172.16/12 IPv4 is blocked");
    Check(
        !IsPublicIpLiteralForTesting("192.168.1.1"),
        "private 192.168 IPv4 is blocked");
    Check(
        !IsPublicIpLiteralForTesting("224.0.0.1"),
        "multicast IPv4 is blocked");
    Check(
        IsPublicIpLiteralForTesting("2606:4700:4700::1111"),
        "public IPv6 is allowed");
    Check(
        !IsPublicIpLiteralForTesting("::"),
        "unspecified IPv6 is blocked");
    Check(
        !IsPublicIpLiteralForTesting("::1"),
        "loopback IPv6 is blocked");
    Check(
        !IsPublicIpLiteralForTesting("fc00::1"),
        "unique-local IPv6 is blocked");
    Check(
        !IsPublicIpLiteralForTesting("fe80::1"),
        "link-local IPv6 is blocked");
    Check(
        !IsPublicIpLiteralForTesting("2001:db8::1"),
        "documentation IPv6 is blocked");
    Check(
        IsPublicIpLiteralForTesting("::ffff:8.8.8.8"),
        "mapped public IPv4 is allowed");
    Check(
        !IsPublicIpLiteralForTesting("::ffff:127.0.0.1"),
        "mapped loopback IPv4 is blocked");
}

void TestUrlAndHeaderPolicy(
    const WotbModV3HttpApiV1* http,
    WotbModV3Handle mod) {
    WotbModV3HttpHandle request = WOTBMOD_V3_INVALID_HANDLE;
    Check(
        http->request_create(mod, &request) == WOTBMOD_V3_OK,
        "policy request is created");
    Check(
        http->request_set_url(
            mod, request, "http://api.example.com/") ==
            WOTBMOD_V3_E_PERMISSION_DENIED,
        "plain HTTP is blocked");
    Check(
        http->request_set_url(
            mod, request, "https://127.0.0.1/") ==
            WOTBMOD_V3_E_PERMISSION_DENIED,
        "literal IPv4 URL is blocked");
    Check(
        http->request_set_url(
            mod, request, "https://[::1]/") ==
            WOTBMOD_V3_E_PERMISSION_DENIED,
        "literal IPv6 URL is blocked");
    Check(
        http->request_set_url(
            mod, request, "https://localhost/") ==
            WOTBMOD_V3_E_PERMISSION_DENIED,
        "localhost URL is blocked");
    Check(
        http->request_set_url(
            mod, request, "https://api.example.com:8443/") ==
            WOTBMOD_V3_E_PERMISSION_DENIED,
        "custom port is blocked");
    Check(
        http->request_set_url(
            mod, request, "https://u:p@api.example.com/") ==
            WOTBMOD_V3_E_PERMISSION_DENIED,
        "URL credentials are blocked");
    Check(
        http->request_set_header(
            mod, request, "Content-Length", "9") ==
            WOTBMOD_V3_E_PERMISSION_DENIED,
        "content-length injection is blocked");
    Check(
        http->request_set_header(
            mod, request, "X-Test", "bad\r\nInjected: yes") ==
            WOTBMOD_V3_E_INVALID_ARGUMENT,
        "header newline injection is blocked");

    WotbModV3HttpHandle suffix_request =
        WOTBMOD_V3_INVALID_HANDLE;
    Check(
        http->request_create(mod, &suffix_request) ==
            WOTBMOD_V3_OK,
        "suffix-isolation request is created");
    Check(
        http->request_set_url(
            mod,
            suffix_request,
            "https://api.example.com.evil/test") ==
            WOTBMOD_V3_OK,
        "suffix-isolation URL is syntactically valid");
    Check(
        http->request_send_async(
            mod, suffix_request, nullptr, nullptr) ==
            WOTBMOD_V3_E_PERMISSION_DENIED,
        "exact host grant does not cover host plus evil suffix");

    WotbModV3HttpHandle other_request =
        WOTBMOD_V3_INVALID_HANDLE;
    Check(
        http->request_create(mod, &other_request) ==
            WOTBMOD_V3_OK,
        "different-host request is created");
    Check(
        http->request_set_url(
            mod,
            other_request,
            "https://other.example.com/test") ==
            WOTBMOD_V3_OK,
        "different host URL is syntactically valid");
    Check(
        http->request_send_async(
            mod, other_request, nullptr, nullptr) ==
            WOTBMOD_V3_E_PERMISSION_DENIED,
        "exact host grant does not cover another host");
}

void TestSuccess(
    const WotbModV3HttpApiV1* http,
    WotbModV3Handle mod) {
    ResetFake(FAKE_SUCCESS);
    const uint32_t callback_before = g_callback_count.load();
    const WotbModV3HttpHandle request =
        CreateConfiguredRequest(http, mod);
    const uint8_t body[] = {1u, 2u, 3u, 4u};
    Check(
        http->request_set_method(mod, request, "POST") ==
            WOTBMOD_V3_OK,
        "request method is configured");
    Check(
        http->request_set_header(
            mod, request, "Content-Type", "application/test") ==
            WOTBMOD_V3_OK,
        "request header is configured");
    Check(
        http->request_set_body(mod, request, body, sizeof(body)) ==
            WOTBMOD_V3_OK,
        "request body is configured");
    Check(
        http->request_set_timeout(mod, request, 1234u) ==
            WOTBMOD_V3_OK,
        "request timeout is configured");
    Check(
        http->request_set_max_response_size(mod, request, 4096u) ==
            WOTBMOD_V3_OK,
        "response limit is configured");
    Check(
        http->request_send_async(
            mod, request, &HttpCompletion, nullptr) ==
            WOTBMOD_V3_OK,
        "asynchronous send is accepted");
    Check(
        WaitForState(
            http, mod, request, WOTBMOD_V3_HTTP_COMPLETED),
        "successful transport reaches COMPLETED");
    Check(
        g_callback_count.load() == callback_before,
        "completion is not invoked on the worker thread");
    Check(
        PumpUntilCallback(mod, callback_before + 1u),
        "completion is delivered by the render-frame pump");
    Check(
        g_callback_result.load() == WOTBMOD_V3_OK,
        "completion reports success");
    Check(
        g_callback_role.load() == WOTBMOD_V3_THREAD_RENDER,
        "completion runs with RENDER role");
    {
        std::lock_guard<std::mutex> lock(g_capture_mutex);
        Check(
            g_captured_request.method == "POST",
            "transport receives the configured method");
        Check(
            g_captured_request.url ==
                "https://api.example.com/v3/test?case=1",
            "transport receives the configured URL");
        Check(
            g_captured_request.timeout_ms == 1234u,
            "transport receives the configured timeout");
        Check(
            g_captured_request.max_response_size == 4096u,
            "transport receives the response limit");
        Check(
            g_captured_request.body ==
                std::vector<uint8_t>(body, body + sizeof(body)),
            "transport receives the request body");
        const auto content_type =
            g_captured_request.headers.find("content-type");
        Check(
            content_type != g_captured_request.headers.end() &&
                content_type->second == "application/test",
            "transport receives normalized request headers");
    }

    uint32_t status = 0u;
    Check(
        http->response_get_status(mod, request, &status) ==
                WOTBMOD_V3_OK &&
            status == 201u,
        "response status is truthful");
    uint32_t header_size = 0u;
    Check(
        http->response_get_header(
            mod,
            request,
            "X-Runtime-Test",
            nullptr,
            &header_size) == WOTBMOD_V3_E_BUFFER_TOO_SMALL &&
            header_size == 4u,
        "response header reports the required buffer size");
    char header[8] = {};
    header_size = sizeof(header);
    Check(
        http->response_get_header(
            mod,
            request,
            "x-runtime-test",
            header,
            &header_size) == WOTBMOD_V3_OK &&
            std::strcmp(header, "yes") == 0,
        "response header is copied case-insensitively");
    WotbModV3Buffer response_body = {};
    WOTBMOD_V3_INIT_STRUCT(
        response_body, WOTBMOD_V3_ASYNC_VERSION);
    Check(
        http->response_get_body(mod, request, &response_body) ==
                WOTBMOD_V3_E_BUFFER_TOO_SMALL &&
            response_body.size == 3u,
        "response body reports the required buffer size");
    uint8_t response_bytes[3] = {};
    response_body.data = response_bytes;
    response_body.capacity = sizeof(response_bytes);
    Check(
        http->response_get_body(mod, request, &response_body) ==
                WOTBMOD_V3_OK &&
            response_body.size == 3u &&
            response_bytes[0] == 0x10u &&
            response_bytes[2] == 0x30u,
        "response body is copied");
    Check(
        http->request_set_method(mod, request, "GET") ==
            WOTBMOD_V3_E_BUSY,
        "completed request cannot be mutated");
}

void TestCancellation(
    const WotbModV3HttpApiV1* http,
    WotbModV3Handle mod) {
    ResetFake(FAKE_WAIT_FOR_CANCEL);
    const uint32_t callback_before = g_callback_count.load();
    const WotbModV3HttpHandle request =
        CreateConfiguredRequest(http, mod);
    Check(
        http->request_send_async(
            mod, request, &HttpCompletion, nullptr) ==
            WOTBMOD_V3_OK,
        "cancellable send is accepted");
    Check(
        WaitForAtomic(g_fake_started, 1u),
        "cancellable transport starts");
    Check(
        http->request_cancel(mod, request) == WOTBMOD_V3_OK,
        "request_cancel succeeds");
    Check(
        WaitForState(
            http, mod, request, WOTBMOD_V3_HTTP_CANCELLED),
        "cancelled transport reaches CANCELLED");
    Check(
        PumpUntilCallback(mod, callback_before + 1u),
        "cancel completion is delivered on the render thread");
    Check(
        g_callback_result.load() == WOTBMOD_V3_E_CANCELLED,
        "cancel completion reports E_CANCELLED");
    uint32_t status = 0u;
    Check(
        http->response_get_status(mod, request, &status) ==
            WOTBMOD_V3_E_CANCELLED,
        "cancelled request has no fabricated response");
}

void TestResponseLimit(
    const WotbModV3HttpApiV1* http,
    WotbModV3Handle mod) {
    ResetFake(FAKE_OVERSIZE);
    const uint32_t callback_before = g_callback_count.load();
    const WotbModV3HttpHandle request =
        CreateConfiguredRequest(http, mod);
    Check(
        http->request_set_max_response_size(mod, request, 2u) ==
            WOTBMOD_V3_OK,
        "small response bound is accepted");
    Check(
        http->request_send_async(
            mod, request, &HttpCompletion, nullptr) ==
            WOTBMOD_V3_OK,
        "oversize test send is accepted");
    Check(
        WaitForState(
            http, mod, request, WOTBMOD_V3_HTTP_FAILED),
        "oversize response reaches FAILED");
    Check(
        PumpUntilCallback(mod, callback_before + 1u),
        "oversize completion is delivered");
    Check(
        g_callback_result.load() == WOTBMOD_V3_E_LIMIT_REACHED,
        "oversize completion reports E_LIMIT_REACHED");
    WotbModV3Buffer body = {};
    WOTBMOD_V3_INIT_STRUCT(body, WOTBMOD_V3_ASYNC_VERSION);
    Check(
        http->response_get_body(mod, request, &body) ==
            WOTBMOD_V3_E_LIMIT_REACHED,
        "oversize response body is not exposed");
}

void TestTimeout(
    const WotbModV3HttpApiV1* http,
    WotbModV3Handle mod) {
    ResetFake(FAKE_TIMEOUT);
    const uint32_t callback_before = g_callback_count.load();
    const WotbModV3HttpHandle request =
        CreateConfiguredRequest(http, mod);
    Check(
        http->request_send_async(
            mod, request, &HttpCompletion, nullptr) ==
            WOTBMOD_V3_OK,
        "timeout test send is accepted");
    Check(
        WaitForState(
            http, mod, request, WOTBMOD_V3_HTTP_FAILED),
        "transport timeout reaches FAILED");
    Check(
        PumpUntilCallback(mod, callback_before + 1u),
        "timeout completion is delivered");
    Check(
        g_callback_result.load() == WOTBMOD_V3_E_TIMEOUT,
        "timeout completion reports E_TIMEOUT");
    uint32_t status = 0u;
    Check(
        http->response_get_status(mod, request, &status) ==
            WOTBMOD_V3_E_TIMEOUT,
        "timed-out request has no fabricated response");
}

void TestHandleRelease(
    const WotbModV3HttpApiV1* http,
    const WotbModV3HandlesApiV1* handles,
    WotbModV3Handle mod) {
    ResetFake(FAKE_WAIT_FOR_RELEASE);
    const uint32_t callback_before = g_callback_count.load();
    const WotbModV3HttpHandle request =
        CreateConfiguredRequest(http, mod);
    Check(
        http->request_send_async(
            mod, request, &HttpCompletion, nullptr) ==
            WOTBMOD_V3_OK,
        "release test send is accepted");
    Check(
        WaitForAtomic(g_fake_started, 1u),
        "release test transport starts");
    Check(
        handles->release(mod, request) == WOTBMOD_V3_OK,
        "HTTP handle can be released in flight");
    g_fake_gate.store(1u);
    Check(
        WaitForAtomic(g_fake_finished, 1u),
        "released request worker exits safely");
    for (uint32_t attempt = 0u; attempt < 20u; ++attempt) {
        WotbModV3Runtime_DispatchFrame(
            g_frame_index.fetch_add(1u),
            0.001);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Check(
        g_callback_count.load() == callback_before,
        "released request suppresses its callback");
}

void TestDisableCancelsOwner(
    const WotbModV3HttpApiV1* http,
    WotbModV3Handle mod) {
    ResetFake(FAKE_WAIT_FOR_CANCEL);
    const uint32_t callback_before = g_callback_count.load();
    const WotbModV3HttpHandle request =
        CreateConfiguredRequest(http, mod);
    Check(
        http->request_send_async(
            mod, request, &HttpCompletion, nullptr) ==
            WOTBMOD_V3_OK,
        "owner-stop test send is accepted");
    Check(
        WaitForAtomic(g_fake_started, 1u),
        "owner-stop transport starts");
    Check(
        WotbModV3Runtime_Disable(mod) == WOTBMOD_V3_OK,
        "mod disable succeeds with HTTP in flight");
    Check(
        WaitForAtomic(g_fake_finished, 1u),
        "mod disable cancels active HTTP work");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Check(
        g_callback_count.load() == callback_before,
        "disabled mod receives no HTTP callback");
}

void RunOptionalLiveSmoke(
    const WotbModV3HttpApiV1* http) {
    char* enabled = nullptr;
    size_t enabled_size = 0u;
    const errno_t env_result = _dupenv_s(
        &enabled, &enabled_size, "WOTBMOD_V3_HTTP_LIVE_TEST");
    const bool run_live =
        env_result == 0 && enabled != nullptr &&
        std::strcmp(enabled, "1") == 0;
    std::free(enabled);
    if (!run_live) return;

    wotbmod::v3::testing::ResetHttpTransportForTesting();
    WotbModV3Handle mod = WOTBMOD_V3_INVALID_HANDLE;
    Check(
        WotbModV3Runtime_CreateMod(
            "v3_http_transport_live_tests.dll",
            WOTBMOD_V3_PERMISSION_REVIEWED,
            &mod) == WOTBMOD_V3_OK,
        "live test mod is created");
    const char* live_grants[] = {
        "network:https://example.com"
    };
    Check(
        WotbModV3Runtime_SetPermissionGrants(
            mod, live_grants, 1u, 1u) == WOTBMOD_V3_OK,
        "live host permission is installed");
    WotbModV3RuntimeModuleInfo module = {};
    WOTBMOD_V3_INIT_STRUCT(module, WOTBMOD_V3_ABI_VERSION);
    Check(
        WotbModV3Runtime_InvokeEntry(
            mod, &LiveTestEntry, &module) == WOTBMOD_V3_OK,
        "live test mod entry succeeds");
    Check(
        WotbModV3Runtime_Enable(mod) == WOTBMOD_V3_OK,
        "live test mod is enabled");
    const uint32_t callback_before = g_callback_count.load();
    WotbModV3HttpHandle request = WOTBMOD_V3_INVALID_HANDLE;
    Check(
        http->request_create(mod, &request) == WOTBMOD_V3_OK,
        "live request is created");
    Check(
        http->request_set_url(
            mod, request, "https://example.com/") ==
            WOTBMOD_V3_OK,
        "live URL is accepted");
    Check(
        http->request_set_timeout(mod, request, 15000u) ==
            WOTBMOD_V3_OK,
        "live timeout is configured");
    Check(
        http->request_set_max_response_size(
            mod, request, 1024u * 1024u) == WOTBMOD_V3_OK,
        "live response limit is configured");
    Check(
        http->request_send_async(
            mod, request, &HttpCompletion, nullptr) ==
            WOTBMOD_V3_OK,
        "live HTTPS send is accepted");

    uint32_t final_state = WOTBMOD_V3_HTTP_SENDING;
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline) {
        WotbModV3HttpRequestInfo info = {};
        WOTBMOD_V3_INIT_STRUCT(info, WOTBMOD_V3_HTTP_VERSION);
        if (http->request_get_info(mod, request, &info) ==
            WOTBMOD_V3_OK) {
            final_state = info.state;
            if (final_state == WOTBMOD_V3_HTTP_COMPLETED ||
                final_state == WOTBMOD_V3_HTTP_FAILED ||
                final_state == WOTBMOD_V3_HTTP_CANCELLED) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const bool live_completed =
        final_state == WOTBMOD_V3_HTTP_COMPLETED;
    Check(live_completed, "live HTTPS request completes");
    const bool live_callback =
        PumpUntilCallback(mod, callback_before + 1u);
    Check(live_callback, "live completion is delivered");
    if (!live_completed || !live_callback) {
        std::printf(
            "Live HTTP diagnostics: state=%u result=%d\n",
            final_state,
            static_cast<int>(g_callback_result.load()));
    }
    uint32_t status = 0u;
    Check(
        http->response_get_status(mod, request, &status) ==
                WOTBMOD_V3_OK &&
            status == 200u,
        "live example.com response status is 200");

    Check(
        WotbModV3Runtime_DestroyMod(mod) == WOTBMOD_V3_OK,
        "live test mod is destroyed");
    wotbmod::v3::testing::SetHttpTransportForTesting(&FakeTransport);
}

}  // namespace

int main() {
    wotbmod::v3::testing::SetHttpTransportForTesting(&FakeTransport);
    TestIpClassification();

    WotbModV3RuntimeOptions options = {};
    WOTBMOD_V3_INIT_STRUCT(options, WOTBMOD_V3_ABI_VERSION);
    options.game_directory = ".";
    options.mods_directory = "build\\v3_http_test\\mods";
    options.cache_directory = "build\\v3_http_test\\cache";
    options.config_directory = "build\\v3_http_test\\config";
    options.client_version = "test";
    Check(
        WotbModV3Runtime_Initialize(&options) == WOTBMOD_V3_OK,
        "runtime initializes");

    WotbModV3Handle mod = WOTBMOD_V3_INVALID_HANDLE;
    Check(
        WotbModV3Runtime_CreateMod(
            "v3_http_transport_tests.dll",
            WOTBMOD_V3_PERMISSION_REVIEWED,
            &mod) == WOTBMOD_V3_OK,
        "test mod is created");
    const char* permission_grants[] = {
        "network:https://api.example.com"
    };
    Check(
        WotbModV3Runtime_SetPermissionGrants(
            mod,
            permission_grants,
            1u,
            1u) == WOTBMOD_V3_OK,
        "exact HTTP host permission is installed");
    WotbModV3RuntimeModuleInfo module = {};
    WOTBMOD_V3_INIT_STRUCT(module, WOTBMOD_V3_ABI_VERSION);
    Check(
        WotbModV3Runtime_InvokeEntry(
            mod, &TestEntry, &module) == WOTBMOD_V3_OK,
        "test mod entry succeeds");
    Check(
        WotbModV3Runtime_Enable(mod) == WOTBMOD_V3_OK,
        "test mod is enabled");

    const WotbModV3Bootstrap* bootstrap =
        WotbModV3Runtime_GetBootstrap();
    Check(bootstrap != nullptr, "bootstrap is available");
    const void* table = nullptr;
    Check(
        bootstrap->query_interface(
            mod,
            WOTBMOD_V3_IFACE_HTTP,
            WOTBMOD_V3_HTTP_VERSION,
            &table) == WOTBMOD_V3_OK,
        "HTTP interface is queryable");
    const WotbModV3HttpApiV1* http =
        static_cast<const WotbModV3HttpApiV1*>(table);
    table = nullptr;
    Check(
        bootstrap->query_interface(
            mod,
            WOTBMOD_V3_IFACE_ASYNC,
            WOTBMOD_V3_ASYNC_VERSION,
            &table) == WOTBMOD_V3_OK,
        "async interface is queryable");
    g_async = static_cast<const WotbModV3AsyncApiV1*>(table);
    table = nullptr;
    Check(
        bootstrap->query_interface(
            mod,
            WOTBMOD_V3_IFACE_HANDLES,
            WOTBMOD_V3_HANDLES_VERSION,
            &table) == WOTBMOD_V3_OK,
        "handles interface is queryable");
    const WotbModV3HandlesApiV1* handles =
        static_cast<const WotbModV3HandlesApiV1*>(table);

    if (http && g_async && handles) {
        TestUrlAndHeaderPolicy(http, mod);
        TestSuccess(http, mod);
        TestCancellation(http, mod);
        TestResponseLimit(http, mod);
        TestTimeout(http, mod);
        TestHandleRelease(http, handles, mod);
        RunOptionalLiveSmoke(http);
        TestDisableCancelsOwner(http, mod);
    }
    Check(
        WotbModV3Runtime_DestroyMod(mod) == WOTBMOD_V3_OK,
        "test mod is destroyed");
    WotbModV3Runtime_Shutdown();
    wotbmod::v3::testing::ResetHttpTransportForTesting();

    std::printf(
        "V3 HTTP transport tests: %u passed, %u failed\n",
        g_passed,
        g_failed);
    return g_failed == 0u ? 0 : 1;
}
