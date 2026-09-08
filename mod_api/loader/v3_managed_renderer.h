#pragma once

#include "../include/wotbmod/base.h"
#include "../src/v3/client_services_backend.h"

namespace wotbmod {
namespace loader {

WotbModV3Result ManagedRendererCreate();

WotbModV3Result ManagedRendererUpdateFrame(
    void* swap_chain,
    void* device,
    void* device_context,
    uint32_t width,
    uint32_t height,
    uint64_t frame_index,
    double delta_seconds);

WotbModV3Result ManagedRendererInvoke(
    WotbModV3Handle mod,
    const char* operation,
    const v3::ClientHostObjectRequest* request,
    v3::ClientHostObjectResponse* response);

void ManagedRendererShutdown();

}  // namespace loader
}  // namespace wotbmod
