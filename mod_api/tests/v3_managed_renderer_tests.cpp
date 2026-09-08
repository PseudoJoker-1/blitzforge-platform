#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d11.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../include/wotbmod/render_v1.h"
#include "../loader/v3_managed_renderer.h"

namespace {

using wotbmod::loader::ManagedRendererCreate;
using wotbmod::loader::ManagedRendererInvoke;
using wotbmod::loader::ManagedRendererShutdown;
using wotbmod::loader::ManagedRendererUpdateFrame;
using wotbmod::v3::ClientHostObjectRequest;
using wotbmod::v3::ClientHostObjectResponse;

template <typename T>
void ReleaseCom(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

ClientHostObjectRequest MakeRequest() {
    ClientHostObjectRequest request = {};
    request.struct_size = sizeof(request);
    request.api_version =
        WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION;
    return request;
}

ClientHostObjectResponse MakeResponse() {
    ClientHostObjectResponse response = {};
    response.struct_size = sizeof(response);
    response.api_version =
        WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION;
    return response;
}

struct TestState {
    uint32_t passed = 0u;
    uint32_t failed = 0u;

    bool Check(
        bool condition,
        const char* expression,
        int line) {
        if (condition) {
            ++passed;
            return true;
        }
        ++failed;
        std::printf(
            "FAIL line %d: %s\n",
            line,
            expression ? expression : "(null)");
        return false;
    }
};

#define CHECK(state, expression) \
    (state).Check(                \
        static_cast<bool>(expression), #expression, __LINE__)

struct D3D11Fixture {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    ID3D11Texture2D* target = nullptr;
    ID3D11RenderTargetView* target_view = nullptr;
    bool renderer_created = false;

    ~D3D11Fixture() {
        if (renderer_created) {
            ManagedRendererShutdown();
        }
        if (context) {
            context->OMSetRenderTargets(0u, nullptr, nullptr);
        }
        ReleaseCom(target_view);
        ReleaseCom(target);
        ReleaseCom(context);
        ReleaseCom(device);
    }

    bool Initialize() {
        D3D_FEATURE_LEVEL selected =
            D3D_FEATURE_LEVEL_9_1;
        const D3D_FEATURE_LEVEL requested[] = {
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0};
        HRESULT result = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            0u,
            requested,
            static_cast<UINT>(
                sizeof(requested) / sizeof(requested[0])),
            D3D11_SDK_VERSION,
            &device,
            &selected,
            &context);
        if (FAILED(result) || !device || !context) {
            std::printf(
                "D3D11 WARP creation failed: 0x%08lX\n",
                static_cast<unsigned long>(result));
            return false;
        }
        D3D11_TEXTURE2D_DESC descriptor = {};
        descriptor.Width = 128u;
        descriptor.Height = 128u;
        descriptor.MipLevels = 1u;
        descriptor.ArraySize = 1u;
        descriptor.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        descriptor.SampleDesc.Count = 1u;
        descriptor.Usage = D3D11_USAGE_DEFAULT;
        descriptor.BindFlags = D3D11_BIND_RENDER_TARGET;
        result = device->CreateTexture2D(
            &descriptor, nullptr, &target);
        if (SUCCEEDED(result)) {
            result = device->CreateRenderTargetView(
                target, nullptr, &target_view);
        }
        if (FAILED(result) || !target || !target_view) {
            std::printf(
                "offscreen render target creation failed: "
                "0x%08lX\n",
                static_cast<unsigned long>(result));
            return false;
        }
        context->OMSetRenderTargets(
            1u, &target_view, nullptr);
        const float clear[4] = {};
        context->ClearRenderTargetView(target_view, clear);
        context->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP);
        if (ManagedRendererCreate() != WOTBMOD_V3_OK) {
            return false;
        }
        renderer_created = true;
        return ManagedRendererUpdateFrame(
                   nullptr,
                   device,
                   context,
                   128u,
                   128u,
                   1u,
                   1.0 / 60.0) == WOTBMOD_V3_OK;
    }

    bool ReadPixels(
        uint32_t* out_lit_pixels,
        uint8_t out_sprite_pixel[4]) {
        if (!out_lit_pixels || !out_sprite_pixel) {
            return false;
        }
        *out_lit_pixels = 0u;
        std::memset(out_sprite_pixel, 0, 4u);
        D3D11_TEXTURE2D_DESC descriptor = {};
        target->GetDesc(&descriptor);
        descriptor.Usage = D3D11_USAGE_STAGING;
        descriptor.BindFlags = 0u;
        descriptor.CPUAccessFlags =
            D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D* staging = nullptr;
        HRESULT result = device->CreateTexture2D(
            &descriptor, nullptr, &staging);
        if (FAILED(result) || !staging) {
            return false;
        }
        context->CopyResource(staging, target);
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        result = context->Map(
            staging, 0u, D3D11_MAP_READ, 0u, &mapped);
        if (SUCCEEDED(result) && mapped.pData) {
            for (uint32_t y = 0u; y < 128u; ++y) {
                const uint8_t* row =
                    static_cast<const uint8_t*>(
                        mapped.pData) +
                    static_cast<size_t>(y) *
                        mapped.RowPitch;
                for (uint32_t x = 0u; x < 128u; ++x) {
                    if (row[x * 4u + 0u] != 0u ||
                        row[x * 4u + 1u] != 0u ||
                        row[x * 4u + 2u] != 0u) {
                        ++*out_lit_pixels;
                    }
                }
            }
            const uint8_t* sprite =
                static_cast<const uint8_t*>(
                    mapped.pData) +
                static_cast<size_t>(16u) *
                    mapped.RowPitch +
                16u * 4u;
            std::memcpy(out_sprite_pixel, sprite, 4u);
            context->Unmap(staging, 0u);
        }
        ReleaseCom(staging);
        return SUCCEEDED(result);
    }
};

WotbModV3Result SetMaterialParameter(
    WotbModV3Handle owner,
    uint64_t material,
    WotbModV3RenderParameter* parameter,
    uint64_t texture = 0u) {
    ClientHostObjectRequest request = MakeRequest();
    request.object = material;
    request.auxiliary_object = texture;
    request.payload = parameter;
    request.payload_size = sizeof(*parameter);
    return ManagedRendererInvoke(
        owner,
        "render_set_material_parameter",
        &request,
        nullptr);
}

bool TestManagedRenderer(TestState* state) {
    if (!state) {
        return false;
    }
    D3D11Fixture fixture;
    if (!CHECK(*state, fixture.Initialize())) {
        return false;
    }

    const WotbModV3Handle owner = 0x101u;
    const WotbModV3Handle other_owner = 0x202u;

    CHECK(
        *state,
        ManagedRendererInvoke(
            owner,
            "render_pop_state",
            nullptr,
            nullptr) == WOTBMOD_V3_E_CONFLICT);
    CHECK(
        *state,
        ManagedRendererInvoke(
            owner,
            "render_push_state",
            nullptr,
            nullptr) == WOTBMOD_V3_OK);
    fixture.context->IASetPrimitiveTopology(
        D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    CHECK(
        *state,
        ManagedRendererInvoke(
            other_owner,
            "render_pop_state",
            nullptr,
            nullptr) == WOTBMOD_V3_E_CONFLICT);
    CHECK(
        *state,
        ManagedRendererInvoke(
            owner,
            "render_pop_state",
            nullptr,
            nullptr) == WOTBMOD_V3_OK);
    D3D11_PRIMITIVE_TOPOLOGY topology =
        D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    fixture.context->IAGetPrimitiveTopology(&topology);
    CHECK(
        *state,
        topology ==
            D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP);

    uint8_t white_pixels[16] = {};
    WotbModV3TextureDescriptor texture_descriptor = {};
    texture_descriptor.struct_size =
        sizeof(texture_descriptor);
    texture_descriptor.api_version =
        WOTBMOD_V3_RENDER_VERSION;
    texture_descriptor.width = 2u;
    texture_descriptor.height = 2u;
    texture_descriptor.format =
        WOTBMOD_V3_TEXTURE_RGBA8_UNORM;
    texture_descriptor.row_pitch = 8u;
    texture_descriptor.initial_data = white_pixels;
    texture_descriptor.initial_data_size =
        sizeof(white_pixels);
    texture_descriptor.debug_name = "warp-smoke";
    ClientHostObjectRequest request = MakeRequest();
    ClientHostObjectResponse response = MakeResponse();
    request.payload = &texture_descriptor;
    request.payload_size = sizeof(texture_descriptor);
    CHECK(
        *state,
        ManagedRendererInvoke(
            owner,
            "render_create_texture",
            &request,
            &response) == WOTBMOD_V3_OK);
    const uint64_t texture = response.object;
    CHECK(*state, texture != 0u);

    const uint8_t red_pixels[16] = {
        255u, 0u, 0u, 255u,
        255u, 0u, 0u, 255u,
        255u, 0u, 0u, 255u,
        255u, 0u, 0u, 255u};
    request = MakeRequest();
    request.object = texture;
    request.scalar0 = 8.0;
    request.payload = red_pixels;
    request.payload_size = sizeof(red_pixels);
    CHECK(
        *state,
        ManagedRendererInvoke(
            owner,
            "render_update_texture",
            &request,
            nullptr) == WOTBMOD_V3_OK);

    request = MakeRequest();
    request.object = texture;
    CHECK(
        *state,
        ManagedRendererInvoke(
            other_owner,
            "render_destroy_texture",
            &request,
            nullptr) ==
            WOTBMOD_V3_E_INVALID_HANDLE);

    WotbModV3MaterialDescriptor material_descriptor = {};
    material_descriptor.struct_size =
        sizeof(material_descriptor);
    material_descriptor.api_version =
        WOTBMOD_V3_RENDER_VERSION;
    material_descriptor.shader_uri =
        "builtin://sprite";
    material_descriptor.debug_name = "warp-smoke";
    material_descriptor.blend_enabled = 1u;
    material_descriptor.depth_test_enabled = 0u;
    request = MakeRequest();
    response = MakeResponse();
    request.payload = &material_descriptor;
    request.payload_size = sizeof(material_descriptor);
    CHECK(
        *state,
        ManagedRendererInvoke(
            owner,
            "render_create_material",
            &request,
            &response) == WOTBMOD_V3_OK);
    const uint64_t material = response.object;
    CHECK(*state, material != 0u);

    WotbModV3RenderParameter parameter = {};
    parameter.struct_size = sizeof(parameter);
    parameter.api_version =
        WOTBMOD_V3_RENDER_VERSION;
    parameter.type =
        WOTBMOD_V3_RENDER_PARAMETER_COLOR;
    parameter.name = "tint";
    parameter.value.color =
        {1.0f, 1.0f, 1.0f, 1.0f};
    CHECK(
        *state,
        SetMaterialParameter(
            owner, material, &parameter) ==
            WOTBMOD_V3_OK);
    parameter.type =
        WOTBMOD_V3_RENDER_PARAMETER_FLOAT;
    parameter.name = "opacity";
    parameter.value.scalar = 1.0f;
    CHECK(
        *state,
        SetMaterialParameter(
            owner, material, &parameter) ==
            WOTBMOD_V3_OK);
    parameter.type =
        WOTBMOD_V3_RENDER_PARAMETER_TEXTURE;
    parameter.name = "texture";
    parameter.value.texture = 1u;
    CHECK(
        *state,
        SetMaterialParameter(
            owner, material, &parameter, texture) ==
            WOTBMOD_V3_OK);

    WotbModV3DrawSprite sprite = {};
    sprite.struct_size = sizeof(sprite);
    sprite.api_version = WOTBMOD_V3_RENDER_VERSION;
    sprite.destination =
        {8.0f, 8.0f, 56.0f, 48.0f};
    sprite.source_uv =
        {0.0f, 0.0f, 1.0f, 1.0f};
    sprite.color = {1.0f, 1.0f, 1.0f, 1.0f};
    sprite.z = 0.0f;
    request = MakeRequest();
    request.object = texture;
    request.related_object = material;
    request.payload = &sprite;
    request.payload_size = sizeof(sprite);
    CHECK(
        *state,
        ManagedRendererInvoke(
            owner,
            "render_draw_sprite",
            &request,
            nullptr) == WOTBMOD_V3_OK);
    fixture.context->IAGetPrimitiveTopology(&topology);
    CHECK(
        *state,
        topology ==
            D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP);

    WotbModV3DrawLine line = {};
    line.struct_size = sizeof(line);
    line.api_version = WOTBMOD_V3_RENDER_VERSION;
    line.from = {5.0f, 64.0f, 0.0f};
    line.to = {122.0f, 64.0f, 0.0f};
    line.color = {0.0f, 1.0f, 0.0f, 1.0f};
    line.width = 3.0f;
    request = MakeRequest();
    request.payload = &line;
    request.payload_size = sizeof(line);
    CHECK(
        *state,
        ManagedRendererInvoke(
            owner,
            "render_draw_line",
            &request,
            nullptr) == WOTBMOD_V3_OK);
    line.from = {118.0f, 12.0f, 0.0f};
    line.to = line.from;
    line.width = 5.0f;
    CHECK(
        *state,
        ManagedRendererInvoke(
            owner,
            "render_draw_line",
            &request,
            nullptr) == WOTBMOD_V3_OK);

    WotbModV3DrawText text = {};
    text.struct_size = sizeof(text);
    text.api_version = WOTBMOD_V3_RENDER_VERSION;
    text.text = "WOTB V3";
    text.font_uri = "builtin://ascii";
    text.position = {8.0f, 80.0f};
    text.color = {1.0f, 1.0f, 1.0f, 1.0f};
    text.font_size = 14.0f;
    text.max_width = 112.0f;
    request = MakeRequest();
    request.payload = &text;
    request.payload_size = sizeof(text);
    CHECK(
        *state,
        ManagedRendererInvoke(
            owner,
            "render_draw_text",
            &request,
            nullptr) == WOTBMOD_V3_OK);
    text.font_uri = "mod://fonts/unsupported.ttf";
    CHECK(
        *state,
        ManagedRendererInvoke(
            owner,
            "render_draw_text",
            &request,
            nullptr) ==
            WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(
        *state,
        ManagedRendererInvoke(
            owner,
            "render_draw_mesh",
            &request,
            nullptr) ==
            WOTBMOD_V3_E_NOT_SUPPORTED);

    uint32_t lit_pixels = 0u;
    uint8_t sprite_pixel[4] = {};
    CHECK(
        *state,
        fixture.ReadPixels(
            &lit_pixels, sprite_pixel));
    CHECK(*state, lit_pixels >= 100u);
    CHECK(*state, sprite_pixel[0] >= 240u);
    CHECK(*state, sprite_pixel[1] <= 10u);
    CHECK(*state, sprite_pixel[2] <= 10u);
    CHECK(*state, sprite_pixel[3] >= 240u);

    request = MakeRequest();
    request.object = material;
    CHECK(
        *state,
        ManagedRendererInvoke(
            owner,
            "render_destroy_material",
            &request,
            nullptr) == WOTBMOD_V3_OK);
    CHECK(
        *state,
        ManagedRendererInvoke(
            owner,
            "render_destroy_material",
            &request,
            nullptr) ==
            WOTBMOD_V3_E_INVALID_HANDLE);
    request = MakeRequest();
    request.object = texture;
    CHECK(
        *state,
        ManagedRendererInvoke(
            owner,
            "render_destroy_texture",
            &request,
            nullptr) == WOTBMOD_V3_OK);
    CHECK(
        *state,
        ManagedRendererInvoke(
            owner,
            "render_destroy_texture",
            &request,
            nullptr) ==
            WOTBMOD_V3_E_INVALID_HANDLE);

    std::printf(
        "D3D11 WARP rendered %u lit pixels\n",
        lit_pixels);
    return state->failed == 0u;
}

}  // namespace

int main() {
    TestState state;
    const bool result = TestManagedRenderer(&state);
    std::printf(
        "V3 Managed Renderer: %u passed, %u failed\n",
        state.passed,
        state.failed);
    return result && state.failed == 0u ? 0 : 1;
}
