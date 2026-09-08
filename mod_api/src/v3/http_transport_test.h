#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../include/wotbmod/base.h"

namespace wotbmod {
namespace v3 {
namespace testing {

struct HttpTransportRequest {
    std::string method;
    std::string url;
    std::unordered_map<std::string, std::string> headers;
    std::vector<uint8_t> body;
    uint32_t timeout_ms = 0u;
    uint32_t max_response_size = 0u;
};

struct HttpTransportContext {
    std::atomic<bool>* cancellation_requested = nullptr;
    std::atomic<void*>* active_native_request = nullptr;
};

struct HttpTransportResponse {
    uint32_t status = 0u;
    std::unordered_map<std::string, std::string> headers;
    std::vector<uint8_t> body;
};

typedef WotbModV3Result (*HttpTransportFn)(
    const HttpTransportRequest& request,
    const HttpTransportContext& context,
    HttpTransportResponse* response);

void SetHttpTransportForTesting(HttpTransportFn transport);
void ResetHttpTransportForTesting();
bool IsPublicIpLiteralForTesting(const char* address);

}  // namespace testing
}  // namespace v3
}  // namespace wotbmod
