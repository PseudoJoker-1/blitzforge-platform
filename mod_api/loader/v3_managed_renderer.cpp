#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#include "v3_managed_renderer.h"
#include "../include/wotbmod/render_v1.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "gdi32.lib")

namespace wotbmod {
namespace loader {
namespace {

using v3::ClientHostObjectRequest;
using v3::ClientHostObjectResponse;

const uint64_t kTextureTokenTag = UINT64_C(0x5458000000000000);
const uint64_t kMaterialTokenTag = UINT64_C(0x4D54000000000000);
const uint64_t kTokenCounterMask = UINT64_C(0x0000FFFFFFFFFFFF);
const size_t kMaximumTextureBytes = 256u * 1024u * 1024u;
const size_t kMaximumTotalTextureBytes = 512u * 1024u * 1024u;
const size_t kMaximumTextureCount = 1024u;
const size_t kMaximumMaterialCount = 1024u;
const size_t kMaximumStateDepth = 16u;
const UINT kMaximumVertices = 4096u;
const UINT kMaximumClassInstances = 256u;
const UINT kFontAtlasWidth = 192u;
const UINT kFontAtlasHeight = 120u;
const UINT kFontCellWidth = 12u;
const UINT kFontCellHeight = 20u;
const UINT kFontColumns = 16u;
const unsigned char kFirstGlyph = 32u;
const unsigned char kLastGlyph = 126u;

template <typename T>
void ReleaseCom(T*& object) {
    if (object) {
        object->Release();
        object = nullptr;
    }
}

bool IsFinite(float value) {
    return std::isfinite(static_cast<double>(value)) != 0;
}

bool IsFinite(double value) {
    return std::isfinite(value) != 0;
}

bool IsFinite(const WotbModV3Color& value) {
    return IsFinite(value.r) && IsFinite(value.g) &&
           IsFinite(value.b) && IsFinite(value.a);
}

bool IsFinite(const WotbModV3Rect& value) {
    return IsFinite(value.x) && IsFinite(value.y) &&
           IsFinite(value.width) && IsFinite(value.height);
}

bool IsFinite(const WotbModV3Vec2& value) {
    return IsFinite(value.x) && IsFinite(value.y);
}

bool IsFinite(const WotbModV3Vec3& value) {
    return IsFinite(value.x) && IsFinite(value.y) &&
           IsFinite(value.z);
}

size_t BoundedStringLength(const char* value, size_t maximum) {
    if (!value) {
        return maximum + 1u;
    }
    size_t length = 0u;
    while (length <= maximum && value[length] != '\0') {
        ++length;
    }
    return length;
}

bool StringEquals(const char* left, const char* right) {
    return left && right && std::strcmp(left, right) == 0;
}

uint32_t BytesPerPixel(uint32_t format) {
    switch (format) {
        case WOTBMOD_V3_TEXTURE_RGBA8_UNORM:
        case WOTBMOD_V3_TEXTURE_BGRA8_UNORM:
            return 4u;
        case WOTBMOD_V3_TEXTURE_R8_UNORM:
            return 1u;
        case WOTBMOD_V3_TEXTURE_RGBA16_FLOAT:
            return 8u;
        default:
            return 0u;
    }
}

DXGI_FORMAT ToDxgiFormat(uint32_t format) {
    switch (format) {
        case WOTBMOD_V3_TEXTURE_RGBA8_UNORM:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case WOTBMOD_V3_TEXTURE_BGRA8_UNORM:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case WOTBMOD_V3_TEXTURE_R8_UNORM:
            return DXGI_FORMAT_R8_UNORM;
        case WOTBMOD_V3_TEXTURE_RGBA16_FLOAT:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default:
            return DXGI_FORMAT_UNKNOWN;
    }
}

struct Vertex {
    float x;
    float y;
    float z;
    float u;
    float v;
    float r;
    float g;
    float b;
    float a;
};

struct TextureResource {
    uint64_t token = 0u;
    WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t format = 0u;
    uint32_t row_pitch = 0u;
    bool dynamic = false;
    std::vector<uint8_t> pixels;
    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* view = nullptr;

    ~TextureResource() {
        ReleaseCom(view);
        ReleaseCom(texture);
    }
};

enum class MaterialKind {
    Sprite,
    Solid
};

struct MaterialResource {
    uint64_t token = 0u;
    WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
    MaterialKind kind = MaterialKind::Sprite;
    bool blend = true;
    bool depth = false;
    WotbModV3Color color = {1.0f, 1.0f, 1.0f, 1.0f};
    float opacity = 1.0f;
    uint64_t texture = 0u;
};

class StateSnapshot {
public:
    StateSnapshot() = default;
    StateSnapshot(const StateSnapshot&) = delete;
    StateSnapshot& operator=(const StateSnapshot&) = delete;

    ~StateSnapshot() {
        Release();
    }

    void Capture(ID3D11DeviceContext* context) {
        Release();
        if (!context) {
            return;
        }
        context_ = context;
        context_->AddRef();
        context_->IAGetInputLayout(&input_layout_);
        context_->IAGetVertexBuffers(
            0u, 1u, &vertex_buffer_, &vertex_stride_, &vertex_offset_);
        context_->IAGetPrimitiveTopology(&topology_);
        vertex_class_count_ = kMaximumClassInstances;
        context_->VSGetShader(
            &vertex_shader_,
            vertex_classes_,
            &vertex_class_count_);
        pixel_class_count_ = kMaximumClassInstances;
        context_->PSGetShader(
            &pixel_shader_,
            pixel_classes_,
            &pixel_class_count_);
        context_->PSGetShaderResources(0u, 1u, &pixel_resource_);
        context_->PSGetSamplers(0u, 1u, &pixel_sampler_);
        context_->OMGetBlendState(
            &blend_state_, blend_factor_, &sample_mask_);
        context_->OMGetDepthStencilState(
            &depth_state_, &stencil_reference_);
        context_->RSGetState(&rasterizer_state_);
        viewport_count_ =
            D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        context_->RSGetViewports(&viewport_count_, viewports_);
        captured_ = true;
    }

    void Restore() {
        if (!captured_ || !context_) {
            Release();
            return;
        }
        context_->IASetInputLayout(input_layout_);
        context_->IASetVertexBuffers(
            0u,
            1u,
            &vertex_buffer_,
            &vertex_stride_,
            &vertex_offset_);
        context_->IASetPrimitiveTopology(topology_);
        context_->VSSetShader(
            vertex_shader_,
            vertex_classes_,
            vertex_class_count_);
        context_->PSSetShader(
            pixel_shader_,
            pixel_classes_,
            pixel_class_count_);
        context_->PSSetShaderResources(0u, 1u, &pixel_resource_);
        context_->PSSetSamplers(0u, 1u, &pixel_sampler_);
        context_->OMSetBlendState(
            blend_state_, blend_factor_, sample_mask_);
        context_->OMSetDepthStencilState(
            depth_state_, stencil_reference_);
        context_->RSSetState(rasterizer_state_);
        context_->RSSetViewports(
            viewport_count_,
            viewport_count_ != 0u ? viewports_ : nullptr);
        Release();
    }

private:
    void Release() {
        ReleaseCom(input_layout_);
        ReleaseCom(vertex_buffer_);
        ReleaseCom(vertex_shader_);
        for (UINT index = 0u;
             index < vertex_class_count_ &&
             index < kMaximumClassInstances;
             ++index) {
            ReleaseCom(vertex_classes_[index]);
        }
        vertex_class_count_ = 0u;
        ReleaseCom(pixel_shader_);
        for (UINT index = 0u;
             index < pixel_class_count_ &&
             index < kMaximumClassInstances;
             ++index) {
            ReleaseCom(pixel_classes_[index]);
        }
        pixel_class_count_ = 0u;
        ReleaseCom(pixel_resource_);
        ReleaseCom(pixel_sampler_);
        ReleaseCom(blend_state_);
        ReleaseCom(depth_state_);
        ReleaseCom(rasterizer_state_);
        ReleaseCom(context_);
        captured_ = false;
    }

    ID3D11DeviceContext* context_ = nullptr;
    ID3D11InputLayout* input_layout_ = nullptr;
    ID3D11Buffer* vertex_buffer_ = nullptr;
    UINT vertex_stride_ = 0u;
    UINT vertex_offset_ = 0u;
    D3D11_PRIMITIVE_TOPOLOGY topology_ =
        D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11VertexShader* vertex_shader_ = nullptr;
    ID3D11ClassInstance*
        vertex_classes_[kMaximumClassInstances] = {};
    UINT vertex_class_count_ = 0u;
    ID3D11PixelShader* pixel_shader_ = nullptr;
    ID3D11ClassInstance*
        pixel_classes_[kMaximumClassInstances] = {};
    UINT pixel_class_count_ = 0u;
    ID3D11ShaderResourceView* pixel_resource_ = nullptr;
    ID3D11SamplerState* pixel_sampler_ = nullptr;
    ID3D11BlendState* blend_state_ = nullptr;
    FLOAT blend_factor_[4] = {};
    UINT sample_mask_ = 0u;
    ID3D11DepthStencilState* depth_state_ = nullptr;
    UINT stencil_reference_ = 0u;
    ID3D11RasterizerState* rasterizer_state_ = nullptr;
    D3D11_VIEWPORT viewports_[
        D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT viewport_count_ = 0u;
    bool captured_ = false;
};

class ScopedState {
public:
    explicit ScopedState(ID3D11DeviceContext* context) {
        state_.Capture(context);
    }

    ScopedState(const ScopedState&) = delete;
    ScopedState& operator=(const ScopedState&) = delete;

    ~ScopedState() {
        state_.Restore();
    }

private:
    StateSnapshot state_;
};

struct ExplicitState {
    WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
    uint64_t frame_index = 0u;
    std::unique_ptr<StateSnapshot> state;
};

struct RendererState {
    bool created = false;
    uint64_t next_token = 1u;
    size_t texture_bytes = 0u;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swap_chain = nullptr;
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint64_t frame_index = 0u;
    double delta_seconds = 0.0;
    ID3D11VertexShader* vertex_shader = nullptr;
    ID3D11PixelShader* texture_shader = nullptr;
    ID3D11PixelShader* color_shader = nullptr;
    ID3D11InputLayout* input_layout = nullptr;
    ID3D11Buffer* vertex_buffer = nullptr;
    ID3D11SamplerState* sampler = nullptr;
    ID3D11BlendState* blend_enabled = nullptr;
    ID3D11BlendState* blend_disabled = nullptr;
    ID3D11DepthStencilState* depth_enabled = nullptr;
    ID3D11DepthStencilState* depth_disabled = nullptr;
    ID3D11RasterizerState* rasterizer = nullptr;
    ID3D11Texture2D* font_texture = nullptr;
    ID3D11ShaderResourceView* font_view = nullptr;
    std::map<uint64_t, std::unique_ptr<TextureResource>> textures;
    std::map<uint64_t, std::unique_ptr<MaterialResource>> materials;
    std::vector<ExplicitState> state_stack;
};

std::mutex g_mutex;
RendererState g_renderer;

void ReleasePipelineLocked() {
    ReleaseCom(g_renderer.font_view);
    ReleaseCom(g_renderer.font_texture);
    ReleaseCom(g_renderer.rasterizer);
    ReleaseCom(g_renderer.depth_disabled);
    ReleaseCom(g_renderer.depth_enabled);
    ReleaseCom(g_renderer.blend_disabled);
    ReleaseCom(g_renderer.blend_enabled);
    ReleaseCom(g_renderer.sampler);
    ReleaseCom(g_renderer.vertex_buffer);
    ReleaseCom(g_renderer.input_layout);
    ReleaseCom(g_renderer.color_shader);
    ReleaseCom(g_renderer.texture_shader);
    ReleaseCom(g_renderer.vertex_shader);
}

void ReleaseGpuTexturesLocked() {
    for (auto& entry : g_renderer.textures) {
        ReleaseCom(entry.second->view);
        ReleaseCom(entry.second->texture);
    }
}

void RestoreStateStackLocked() {
    while (!g_renderer.state_stack.empty()) {
        ExplicitState& entry = g_renderer.state_stack.back();
        if (entry.state) {
            entry.state->Restore();
        }
        g_renderer.state_stack.pop_back();
    }
}

void ReleaseFrameObjectsLocked() {
    RestoreStateStackLocked();
    ReleasePipelineLocked();
    ReleaseGpuTexturesLocked();
    ReleaseCom(g_renderer.swap_chain);
    ReleaseCom(g_renderer.context);
    ReleaseCom(g_renderer.device);
    g_renderer.width = 0u;
    g_renderer.height = 0u;
    g_renderer.frame_index = 0u;
    g_renderer.delta_seconds = 0.0;
}

uint64_t AllocateTokenLocked(uint64_t tag) {
    for (uint64_t attempts = 0u;
         attempts < kTokenCounterMask;
         ++attempts) {
        const uint64_t counter =
            g_renderer.next_token++ & kTokenCounterMask;
        if (counter == 0u) {
            continue;
        }
        const uint64_t token = tag | counter;
        if (g_renderer.textures.find(token) ==
                g_renderer.textures.end() &&
            g_renderer.materials.find(token) ==
                g_renderer.materials.end()) {
            return token;
        }
    }
    return 0u;
}

TextureResource* FindTextureLocked(
    WotbModV3Handle owner,
    uint64_t token) {
    const auto found = g_renderer.textures.find(token);
    if (found == g_renderer.textures.end() ||
        !found->second || found->second->owner != owner) {
        return nullptr;
    }
    return found->second.get();
}

MaterialResource* FindMaterialLocked(
    WotbModV3Handle owner,
    uint64_t token) {
    const auto found = g_renderer.materials.find(token);
    if (found == g_renderer.materials.end() ||
        !found->second || found->second->owner != owner) {
        return nullptr;
    }
    return found->second.get();
}

bool IsSupportedMaterial(
    const char* shader_uri,
    MaterialKind* out_kind) {
    if (!shader_uri || !out_kind) {
        return false;
    }
    if (StringEquals(shader_uri, "builtin://sprite") ||
        StringEquals(shader_uri, "builtin:sprite") ||
        StringEquals(shader_uri, "builtin://default") ||
        StringEquals(shader_uri, "builtin:default") ||
        StringEquals(shader_uri, "builtin://unlit") ||
        StringEquals(shader_uri, "builtin:unlit") ||
        StringEquals(shader_uri, "wotbmod://builtin/sprite")) {
        *out_kind = MaterialKind::Sprite;
        return true;
    }
    if (StringEquals(shader_uri, "builtin://solid") ||
        StringEquals(shader_uri, "builtin:solid") ||
        StringEquals(shader_uri, "wotbmod://builtin/solid")) {
        *out_kind = MaterialKind::Solid;
        return true;
    }
    return false;
}

bool IsSupportedFont(const char* font_uri) {
    return StringEquals(font_uri, "builtin://ascii") ||
           StringEquals(font_uri, "builtin:ascii") ||
           StringEquals(font_uri, "builtin://default") ||
           StringEquals(font_uri, "builtin:default") ||
           StringEquals(font_uri, "wotbmod://builtin/ascii");
}

bool CreateGpuTextureLocked(TextureResource* resource) {
    if (!resource || !g_renderer.device) {
        return false;
    }
    const DXGI_FORMAT format = ToDxgiFormat(resource->format);
    if (format == DXGI_FORMAT_UNKNOWN ||
        resource->pixels.empty()) {
        return false;
    }
    D3D11_TEXTURE2D_DESC descriptor = {};
    descriptor.Width = resource->width;
    descriptor.Height = resource->height;
    descriptor.MipLevels = 1u;
    descriptor.ArraySize = 1u;
    descriptor.Format = format;
    descriptor.SampleDesc.Count = 1u;
    descriptor.Usage = D3D11_USAGE_DEFAULT;
    descriptor.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial = {};
    initial.pSysMem = resource->pixels.data();
    initial.SysMemPitch = resource->row_pitch;
    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* view = nullptr;
    const HRESULT texture_result =
        g_renderer.device->CreateTexture2D(
            &descriptor, &initial, &texture);
    if (FAILED(texture_result) ||
        FAILED(g_renderer.device->CreateShaderResourceView(
            texture, nullptr, &view))) {
        ReleaseCom(view);
        ReleaseCom(texture);
        return false;
    }
    ReleaseCom(resource->view);
    ReleaseCom(resource->texture);
    resource->texture = texture;
    resource->view = view;
    return true;
}

bool CompileShader(
    const char* source,
    const char* entry,
    const char* profile,
    ID3DBlob** out_blob) {
    if (!source || !entry || !profile || !out_blob) {
        return false;
    }
    *out_blob = nullptr;
    ID3DBlob* errors = nullptr;
    const UINT flags =
        D3DCOMPILE_ENABLE_STRICTNESS |
        D3DCOMPILE_OPTIMIZATION_LEVEL3;
    const HRESULT result = D3DCompile(
        source,
        std::strlen(source),
        "wotbmod.v3.managed",
        nullptr,
        nullptr,
        entry,
        profile,
        flags,
        0u,
        out_blob,
        &errors);
    ReleaseCom(errors);
    return SUCCEEDED(result) && *out_blob;
}

bool EnsurePipelineLocked() {
    if (g_renderer.vertex_shader &&
        g_renderer.texture_shader &&
        g_renderer.color_shader &&
        g_renderer.input_layout &&
        g_renderer.vertex_buffer &&
        g_renderer.sampler &&
        g_renderer.blend_enabled &&
        g_renderer.blend_disabled &&
        g_renderer.depth_enabled &&
        g_renderer.depth_disabled &&
        g_renderer.rasterizer) {
        return true;
    }
    if (!g_renderer.device) {
        return false;
    }
    ReleasePipelineLocked();
    static const char shader_source[] =
        "struct VSInput {"
        " float3 position : POSITION;"
        " float2 uv : TEXCOORD0;"
        " float4 color : COLOR0;"
        "};"
        "struct PSInput {"
        " float4 position : SV_POSITION;"
        " float2 uv : TEXCOORD0;"
        " float4 color : COLOR0;"
        "};"
        "PSInput VSMain(VSInput input) {"
        " PSInput output;"
        " output.position = float4(input.position, 1.0);"
        " output.uv = input.uv;"
        " output.color = input.color;"
        " return output;"
        "}"
        "Texture2D texture0 : register(t0);"
        "SamplerState sampler0 : register(s0);"
        "float4 PSTexture(PSInput input) : SV_TARGET {"
        " return texture0.Sample(sampler0, input.uv) * input.color;"
        "}"
        "float4 PSColor(PSInput input) : SV_TARGET {"
        " return input.color;"
        "}";
    ID3DBlob* vertex_blob = nullptr;
    ID3DBlob* texture_blob = nullptr;
    ID3DBlob* color_blob = nullptr;
    const bool compiled =
        CompileShader(
            shader_source, "VSMain", "vs_4_0", &vertex_blob) &&
        CompileShader(
            shader_source, "PSTexture", "ps_4_0", &texture_blob) &&
        CompileShader(
            shader_source, "PSColor", "ps_4_0", &color_blob);
    if (!compiled) {
        ReleaseCom(color_blob);
        ReleaseCom(texture_blob);
        ReleaseCom(vertex_blob);
        return false;
    }
    HRESULT result = g_renderer.device->CreateVertexShader(
        vertex_blob->GetBufferPointer(),
        vertex_blob->GetBufferSize(),
        nullptr,
        &g_renderer.vertex_shader);
    if (SUCCEEDED(result)) {
        result = g_renderer.device->CreatePixelShader(
            texture_blob->GetBufferPointer(),
            texture_blob->GetBufferSize(),
            nullptr,
            &g_renderer.texture_shader);
    }
    if (SUCCEEDED(result)) {
        result = g_renderer.device->CreatePixelShader(
            color_blob->GetBufferPointer(),
            color_blob->GetBufferSize(),
            nullptr,
            &g_renderer.color_shader);
    }
    const D3D11_INPUT_ELEMENT_DESC input_elements[] = {
        {
            "POSITION",
            0u,
            DXGI_FORMAT_R32G32B32_FLOAT,
            0u,
            static_cast<UINT>(offsetof(Vertex, x)),
            D3D11_INPUT_PER_VERTEX_DATA,
            0u},
        {
            "TEXCOORD",
            0u,
            DXGI_FORMAT_R32G32_FLOAT,
            0u,
            static_cast<UINT>(offsetof(Vertex, u)),
            D3D11_INPUT_PER_VERTEX_DATA,
            0u},
        {
            "COLOR",
            0u,
            DXGI_FORMAT_R32G32B32A32_FLOAT,
            0u,
            static_cast<UINT>(offsetof(Vertex, r)),
            D3D11_INPUT_PER_VERTEX_DATA,
            0u}};
    if (SUCCEEDED(result)) {
        result = g_renderer.device->CreateInputLayout(
            input_elements,
            static_cast<UINT>(
                sizeof(input_elements) / sizeof(input_elements[0])),
            vertex_blob->GetBufferPointer(),
            vertex_blob->GetBufferSize(),
            &g_renderer.input_layout);
    }
    ReleaseCom(color_blob);
    ReleaseCom(texture_blob);
    ReleaseCom(vertex_blob);

    D3D11_BUFFER_DESC vertex_descriptor = {};
    vertex_descriptor.ByteWidth =
        static_cast<UINT>(sizeof(Vertex)) * kMaximumVertices;
    vertex_descriptor.Usage = D3D11_USAGE_DYNAMIC;
    vertex_descriptor.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vertex_descriptor.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (SUCCEEDED(result)) {
        result = g_renderer.device->CreateBuffer(
            &vertex_descriptor, nullptr, &g_renderer.vertex_buffer);
    }

    D3D11_SAMPLER_DESC sampler_descriptor = {};
    sampler_descriptor.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_descriptor.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_descriptor.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_descriptor.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_descriptor.MaxLOD = D3D11_FLOAT32_MAX;
    if (SUCCEEDED(result)) {
        result = g_renderer.device->CreateSamplerState(
            &sampler_descriptor, &g_renderer.sampler);
    }

    D3D11_BLEND_DESC blend_descriptor = {};
    blend_descriptor.RenderTarget[0].BlendEnable = TRUE;
    blend_descriptor.RenderTarget[0].SrcBlend =
        D3D11_BLEND_SRC_ALPHA;
    blend_descriptor.RenderTarget[0].DestBlend =
        D3D11_BLEND_INV_SRC_ALPHA;
    blend_descriptor.RenderTarget[0].BlendOp =
        D3D11_BLEND_OP_ADD;
    blend_descriptor.RenderTarget[0].SrcBlendAlpha =
        D3D11_BLEND_ONE;
    blend_descriptor.RenderTarget[0].DestBlendAlpha =
        D3D11_BLEND_INV_SRC_ALPHA;
    blend_descriptor.RenderTarget[0].BlendOpAlpha =
        D3D11_BLEND_OP_ADD;
    blend_descriptor.RenderTarget[0].RenderTargetWriteMask =
        D3D11_COLOR_WRITE_ENABLE_ALL;
    if (SUCCEEDED(result)) {
        result = g_renderer.device->CreateBlendState(
            &blend_descriptor, &g_renderer.blend_enabled);
    }
    blend_descriptor.RenderTarget[0].BlendEnable = FALSE;
    if (SUCCEEDED(result)) {
        result = g_renderer.device->CreateBlendState(
            &blend_descriptor, &g_renderer.blend_disabled);
    }

    D3D11_DEPTH_STENCIL_DESC depth_descriptor = {};
    depth_descriptor.DepthEnable = TRUE;
    depth_descriptor.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    depth_descriptor.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    depth_descriptor.StencilEnable = FALSE;
    if (SUCCEEDED(result)) {
        result = g_renderer.device->CreateDepthStencilState(
            &depth_descriptor, &g_renderer.depth_enabled);
    }
    depth_descriptor.DepthEnable = FALSE;
    if (SUCCEEDED(result)) {
        result = g_renderer.device->CreateDepthStencilState(
            &depth_descriptor, &g_renderer.depth_disabled);
    }

    D3D11_RASTERIZER_DESC raster_descriptor = {};
    raster_descriptor.FillMode = D3D11_FILL_SOLID;
    raster_descriptor.CullMode = D3D11_CULL_NONE;
    raster_descriptor.DepthClipEnable = TRUE;
    raster_descriptor.ScissorEnable = FALSE;
    raster_descriptor.MultisampleEnable = TRUE;
    if (SUCCEEDED(result)) {
        result = g_renderer.device->CreateRasterizerState(
            &raster_descriptor, &g_renderer.rasterizer);
    }
    if (FAILED(result)) {
        ReleasePipelineLocked();
        return false;
    }
    return true;
}

bool BuildFontAtlasPixels(std::vector<uint8_t>* out_pixels) {
    if (!out_pixels) {
        return false;
    }
    try {
        out_pixels->assign(
            static_cast<size_t>(kFontAtlasWidth) *
                kFontAtlasHeight * 4u,
            0u);
    } catch (const std::bad_alloc&) {
        return false;
    }

    BITMAPINFO bitmap_info = {};
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth =
        static_cast<LONG>(kFontAtlasWidth);
    bitmap_info.bmiHeader.biHeight =
        -static_cast<LONG>(kFontAtlasHeight);
    bitmap_info.bmiHeader.biPlanes = 1u;
    bitmap_info.bmiHeader.biBitCount = 32u;
    bitmap_info.bmiHeader.biCompression = BI_RGB;
    void* bitmap_pixels = nullptr;
    HDC device_context = CreateCompatibleDC(nullptr);
    if (!device_context) {
        return false;
    }
    HBITMAP bitmap = CreateDIBSection(
        device_context,
        &bitmap_info,
        DIB_RGB_COLORS,
        &bitmap_pixels,
        nullptr,
        0u);
    if (!bitmap || !bitmap_pixels) {
        if (bitmap) {
            DeleteObject(bitmap);
        }
        DeleteDC(device_context);
        return false;
    }
    HGDIOBJ old_bitmap =
        SelectObject(device_context, bitmap);
    HFONT font = CreateFontW(
        -16,
        0,
        0,
        0,
        FW_NORMAL,
        FALSE,
        FALSE,
        FALSE,
        ANSI_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY,
        FIXED_PITCH | FF_MODERN,
        L"Consolas");
    bool owns_font = font != nullptr;
    if (!font) {
        font = static_cast<HFONT>(
            GetStockObject(SYSTEM_FIXED_FONT));
    }
    HGDIOBJ old_font =
        SelectObject(device_context, font);
    SetBkMode(device_context, TRANSPARENT);
    SetTextColor(device_context, RGB(255, 255, 255));
    std::memset(
        bitmap_pixels,
        0,
        static_cast<size_t>(kFontAtlasWidth) *
            kFontAtlasHeight * 4u);
    for (unsigned int character = kFirstGlyph;
         character <= kLastGlyph;
         ++character) {
        const unsigned int glyph_index =
            character - kFirstGlyph;
        const int x = static_cast<int>(
            (glyph_index % kFontColumns) * kFontCellWidth);
        const int y = static_cast<int>(
            (glyph_index / kFontColumns) * kFontCellHeight);
        const wchar_t glyph =
            static_cast<wchar_t>(character);
        TextOutW(device_context, x + 1, y + 1, &glyph, 1);
    }

    const uint8_t* source =
        static_cast<const uint8_t*>(bitmap_pixels);
    size_t covered_pixels = 0u;
    for (size_t pixel = 0u;
         pixel < static_cast<size_t>(kFontAtlasWidth) *
                     kFontAtlasHeight;
         ++pixel) {
        const uint8_t coverage = std::max(
            source[pixel * 4u + 0u],
            std::max(
                source[pixel * 4u + 1u],
                source[pixel * 4u + 2u]));
        (*out_pixels)[pixel * 4u + 0u] = 255u;
        (*out_pixels)[pixel * 4u + 1u] = 255u;
        (*out_pixels)[pixel * 4u + 2u] = 255u;
        (*out_pixels)[pixel * 4u + 3u] = coverage;
        if (coverage != 0u) {
            ++covered_pixels;
        }
    }

    SelectObject(device_context, old_font);
    SelectObject(device_context, old_bitmap);
    if (owns_font) {
        DeleteObject(font);
    }
    DeleteObject(bitmap);
    DeleteDC(device_context);
    return covered_pixels != 0u;
}

bool EnsureFontAtlasLocked() {
    if (g_renderer.font_texture && g_renderer.font_view) {
        return true;
    }
    if (!g_renderer.device) {
        return false;
    }
    std::vector<uint8_t> pixels;
    if (!BuildFontAtlasPixels(&pixels)) {
        return false;
    }
    D3D11_TEXTURE2D_DESC descriptor = {};
    descriptor.Width = kFontAtlasWidth;
    descriptor.Height = kFontAtlasHeight;
    descriptor.MipLevels = 1u;
    descriptor.ArraySize = 1u;
    descriptor.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    descriptor.SampleDesc.Count = 1u;
    descriptor.Usage = D3D11_USAGE_IMMUTABLE;
    descriptor.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial = {};
    initial.pSysMem = pixels.data();
    initial.SysMemPitch = kFontAtlasWidth * 4u;
    const HRESULT texture_result =
        g_renderer.device->CreateTexture2D(
            &descriptor, &initial, &g_renderer.font_texture);
    if (FAILED(texture_result) ||
        FAILED(g_renderer.device->CreateShaderResourceView(
            g_renderer.font_texture,
            nullptr,
            &g_renderer.font_view))) {
        ReleaseCom(g_renderer.font_view);
        ReleaseCom(g_renderer.font_texture);
        return false;
    }
    return true;
}

bool HasRenderTargetLocked() {
    if (!g_renderer.context) {
        return false;
    }
    ID3D11RenderTargetView* render_target = nullptr;
    ID3D11DepthStencilView* depth_view = nullptr;
    g_renderer.context->OMGetRenderTargets(
        1u, &render_target, &depth_view);
    const bool has_target = render_target != nullptr;
    ReleaseCom(depth_view);
    ReleaseCom(render_target);
    return has_target;
}

void PixelToClip(
    float x,
    float y,
    float* out_x,
    float* out_y) {
    *out_x =
        x / static_cast<float>(g_renderer.width) * 2.0f - 1.0f;
    *out_y =
        1.0f - y / static_cast<float>(g_renderer.height) * 2.0f;
}

WotbModV3Color MultiplyColor(
    const WotbModV3Color& left,
    const WotbModV3Color& right,
    float opacity) {
    return {
        left.r * right.r,
        left.g * right.g,
        left.b * right.b,
        left.a * right.a * opacity};
}

Vertex MakeVertex(
    float pixel_x,
    float pixel_y,
    float z,
    float u,
    float v,
    const WotbModV3Color& color) {
    Vertex vertex = {};
    PixelToClip(pixel_x, pixel_y, &vertex.x, &vertex.y);
    vertex.z = z;
    vertex.u = u;
    vertex.v = v;
    vertex.r = color.r;
    vertex.g = color.g;
    vertex.b = color.b;
    vertex.a = color.a;
    return vertex;
}

WotbModV3Result DrawVerticesLocked(
    const Vertex* vertices,
    UINT count,
    ID3D11ShaderResourceView* texture,
    bool use_texture,
    bool blend,
    bool depth) {
    if (!vertices || count == 0u ||
        count > kMaximumVertices) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (!g_renderer.context ||
        g_renderer.width == 0u ||
        g_renderer.height == 0u ||
        !EnsurePipelineLocked()) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    if (use_texture && !texture) {
        return WOTBMOD_V3_E_OBJECT_DESTROYED;
    }
    if (!HasRenderTargetLocked()) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }

    ScopedState saved_state(g_renderer.context);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    const HRESULT map_result = g_renderer.context->Map(
        g_renderer.vertex_buffer,
        0u,
        D3D11_MAP_WRITE_DISCARD,
        0u,
        &mapped);
    if (FAILED(map_result) || !mapped.pData) {
        return WOTBMOD_V3_E_PLATFORM;
    }
    std::memcpy(
        mapped.pData,
        vertices,
        static_cast<size_t>(count) * sizeof(Vertex));
    g_renderer.context->Unmap(g_renderer.vertex_buffer, 0u);

    const UINT stride = sizeof(Vertex);
    const UINT offset = 0u;
    g_renderer.context->IASetInputLayout(
        g_renderer.input_layout);
    g_renderer.context->IASetVertexBuffers(
        0u,
        1u,
        &g_renderer.vertex_buffer,
        &stride,
        &offset);
    g_renderer.context->IASetPrimitiveTopology(
        D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_renderer.context->VSSetShader(
        g_renderer.vertex_shader, nullptr, 0u);
    g_renderer.context->PSSetShader(
        use_texture
            ? g_renderer.texture_shader
            : g_renderer.color_shader,
        nullptr,
        0u);
    g_renderer.context->PSSetShaderResources(
        0u, 1u, &texture);
    g_renderer.context->PSSetSamplers(
        0u, 1u, &g_renderer.sampler);
    const FLOAT blend_factor[4] = {
        1.0f, 1.0f, 1.0f, 1.0f};
    ID3D11BlendState* blend_state =
        blend
            ? g_renderer.blend_enabled
            : g_renderer.blend_disabled;
    g_renderer.context->OMSetBlendState(
        blend_state, blend_factor, 0xFFFFFFFFu);
    g_renderer.context->OMSetDepthStencilState(
        depth
            ? g_renderer.depth_enabled
            : g_renderer.depth_disabled,
        0u);
    g_renderer.context->RSSetState(g_renderer.rasterizer);
    const D3D11_VIEWPORT viewport = {
        0.0f,
        0.0f,
        static_cast<float>(g_renderer.width),
        static_cast<float>(g_renderer.height),
        0.0f,
        1.0f};
    g_renderer.context->RSSetViewports(1u, &viewport);
    g_renderer.context->Draw(count, 0u);
    return WOTBMOD_V3_OK;
}

WotbModV3Result CreateTextureLocked(
    WotbModV3Handle owner,
    const WotbModV3TextureDescriptor* descriptor,
    ClientHostObjectResponse* response) {
    if (!descriptor || !response ||
        descriptor->struct_size < sizeof(*descriptor) ||
        descriptor->api_version != WOTBMOD_V3_RENDER_VERSION ||
        descriptor->width == 0u ||
        descriptor->height == 0u ||
        descriptor->width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        descriptor->height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        descriptor->dynamic > 1u) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (!g_renderer.device) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    if (g_renderer.textures.size() >= kMaximumTextureCount) {
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }
    const uint32_t bytes_per_pixel =
        BytesPerPixel(descriptor->format);
    if (bytes_per_pixel == 0u ||
        ToDxgiFormat(descriptor->format) ==
            DXGI_FORMAT_UNKNOWN) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    if (descriptor->width >
        UINT32_MAX / bytes_per_pixel) {
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }
    const uint32_t tight_pitch =
        descriptor->width * bytes_per_pixel;
    const uint32_t source_pitch =
        descriptor->row_pitch != 0u
            ? descriptor->row_pitch
            : tight_pitch;
    if (source_pitch < tight_pitch ||
        descriptor->height >
            SIZE_MAX / static_cast<size_t>(tight_pitch)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    const size_t texture_size =
        static_cast<size_t>(tight_pitch) * descriptor->height;
    if (texture_size > kMaximumTextureBytes ||
        texture_size >
            kMaximumTotalTextureBytes -
                std::min(
                    g_renderer.texture_bytes,
                    kMaximumTotalTextureBytes)) {
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }
    if (descriptor->initial_data) {
        if (descriptor->height >
            SIZE_MAX / static_cast<size_t>(source_pitch)) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        const size_t required_source_size =
            static_cast<size_t>(source_pitch) *
            descriptor->height;
        if (descriptor->initial_data_size <
            required_source_size) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
    } else if (descriptor->initial_data_size != 0u) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }

    std::unique_ptr<TextureResource> texture(
        new (std::nothrow) TextureResource());
    if (!texture) {
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }
    texture->token = AllocateTokenLocked(kTextureTokenTag);
    texture->owner = owner;
    texture->width = descriptor->width;
    texture->height = descriptor->height;
    texture->format = descriptor->format;
    texture->row_pitch = tight_pitch;
    texture->dynamic = descriptor->dynamic != 0u;
    if (texture->token == 0u) {
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }
    try {
        texture->pixels.assign(texture_size, 0u);
    } catch (const std::bad_alloc&) {
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }
    if (descriptor->initial_data) {
        const uint8_t* source =
            static_cast<const uint8_t*>(
                descriptor->initial_data);
        for (uint32_t row = 0u;
             row < descriptor->height;
             ++row) {
            std::memcpy(
                texture->pixels.data() +
                    static_cast<size_t>(row) * tight_pitch,
                source +
                    static_cast<size_t>(row) * source_pitch,
                tight_pitch);
        }
    }
    if (!CreateGpuTextureLocked(texture.get())) {
        return WOTBMOD_V3_E_PLATFORM;
    }
    const uint64_t token = texture->token;
    try {
        g_renderer.textures.emplace(token, std::move(texture));
    } catch (const std::bad_alloc&) {
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }
    g_renderer.texture_bytes += texture_size;
    response->object = token;
    return WOTBMOD_V3_OK;
}

WotbModV3Result UpdateTextureLocked(
    WotbModV3Handle owner,
    const ClientHostObjectRequest* request) {
    if (!request || !request->payload ||
        request->payload_size == 0u ||
        !IsFinite(request->scalar0) ||
        request->scalar0 < 1.0 ||
        request->scalar0 >
            static_cast<double>(UINT32_MAX)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    TextureResource* texture =
        FindTextureLocked(owner, request->object);
    if (!texture) {
        return WOTBMOD_V3_E_INVALID_HANDLE;
    }
    const uint32_t bytes_per_pixel =
        BytesPerPixel(texture->format);
    uint32_t x = 0u;
    uint32_t y = 0u;
    uint32_t width = texture->width;
    uint32_t height = texture->height;
    if (request->selector != 0u) {
        if (!IsFinite(request->vector.x) ||
            !IsFinite(request->vector.y) ||
            !IsFinite(request->vector.z) ||
            !IsFinite(request->vector.w) ||
            request->vector.x < 0.0f ||
            request->vector.y < 0.0f ||
            request->vector.z < 1.0f ||
            request->vector.w < 1.0f ||
            std::floor(request->vector.x) !=
                request->vector.x ||
            std::floor(request->vector.y) !=
                request->vector.y ||
            std::floor(request->vector.z) !=
                request->vector.z ||
            std::floor(request->vector.w) !=
                request->vector.w) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        x = static_cast<uint32_t>(request->vector.x);
        y = static_cast<uint32_t>(request->vector.y);
        width = static_cast<uint32_t>(request->vector.z);
        height = static_cast<uint32_t>(request->vector.w);
    }
    if (x > texture->width ||
        width > texture->width - x ||
        y > texture->height ||
        height > texture->height - y ||
        width > UINT32_MAX / bytes_per_pixel) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    const uint32_t copy_width = width * bytes_per_pixel;
    const uint32_t source_pitch =
        static_cast<uint32_t>(request->scalar0);
    if (source_pitch < copy_width ||
        height >
            SIZE_MAX / static_cast<size_t>(source_pitch) ||
        request->payload_size <
            static_cast<size_t>(source_pitch) * height) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    const uint8_t* source =
        static_cast<const uint8_t*>(request->payload);
    for (uint32_t row = 0u; row < height; ++row) {
        std::memcpy(
            texture->pixels.data() +
                static_cast<size_t>(y + row) *
                    texture->row_pitch +
                static_cast<size_t>(x) * bytes_per_pixel,
            source +
                static_cast<size_t>(row) * source_pitch,
            copy_width);
    }
    if (!CreateGpuTextureLocked(texture)) {
        return WOTBMOD_V3_E_PLATFORM;
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result DestroyTextureLocked(
    WotbModV3Handle owner,
    uint64_t token) {
    const auto found = g_renderer.textures.find(token);
    if (found == g_renderer.textures.end() ||
        !found->second || found->second->owner != owner) {
        return WOTBMOD_V3_E_INVALID_HANDLE;
    }
    for (auto& entry : g_renderer.materials) {
        if (entry.second &&
            entry.second->owner == owner &&
            entry.second->texture == token) {
            entry.second->texture = 0u;
        }
    }
    g_renderer.texture_bytes -=
        std::min(
            g_renderer.texture_bytes,
            found->second->pixels.size());
    g_renderer.textures.erase(found);
    return WOTBMOD_V3_OK;
}

WotbModV3Result CreateMaterialLocked(
    WotbModV3Handle owner,
    const WotbModV3MaterialDescriptor* descriptor,
    ClientHostObjectResponse* response) {
    if (!descriptor || !response ||
        descriptor->struct_size < sizeof(*descriptor) ||
        descriptor->api_version != WOTBMOD_V3_RENDER_VERSION ||
        descriptor->blend_enabled > 1u ||
        descriptor->depth_test_enabled > 1u) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (g_renderer.materials.size() >=
        kMaximumMaterialCount) {
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }
    MaterialKind kind = MaterialKind::Sprite;
    if (!IsSupportedMaterial(
            descriptor->shader_uri, &kind)) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    std::unique_ptr<MaterialResource> material(
        new (std::nothrow) MaterialResource());
    if (!material) {
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }
    material->token =
        AllocateTokenLocked(kMaterialTokenTag);
    material->owner = owner;
    material->kind = kind;
    material->blend = descriptor->blend_enabled != 0u;
    material->depth =
        descriptor->depth_test_enabled != 0u;
    if (material->token == 0u) {
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }
    const uint64_t token = material->token;
    try {
        g_renderer.materials.emplace(
            token, std::move(material));
    } catch (const std::bad_alloc&) {
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }
    response->object = token;
    return WOTBMOD_V3_OK;
}

bool IsColorParameterName(const char* name) {
    return StringEquals(name, "color") ||
           StringEquals(name, "tint") ||
           StringEquals(name, "u_color");
}

bool IsTextureParameterName(const char* name) {
    return StringEquals(name, "texture") ||
           StringEquals(name, "main_texture") ||
           StringEquals(name, "texture0");
}

WotbModV3Result SetMaterialParameterLocked(
    WotbModV3Handle owner,
    const ClientHostObjectRequest* request) {
    if (!request || !request->payload ||
        request->payload_size <
            sizeof(WotbModV3RenderParameter)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    MaterialResource* material =
        FindMaterialLocked(owner, request->object);
    if (!material) {
        return WOTBMOD_V3_E_INVALID_HANDLE;
    }
    const WotbModV3RenderParameter* parameter =
        static_cast<const WotbModV3RenderParameter*>(
            request->payload);
    if (parameter->struct_size < sizeof(*parameter) ||
        parameter->api_version !=
            WOTBMOD_V3_RENDER_VERSION ||
        BoundedStringLength(
            parameter->name,
            WOTBMOD_V3_MAX_NAME) >
            WOTBMOD_V3_MAX_NAME) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (parameter->type ==
            WOTBMOD_V3_RENDER_PARAMETER_COLOR &&
        IsColorParameterName(parameter->name)) {
        if (!IsFinite(parameter->value.color)) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        material->color = parameter->value.color;
        return WOTBMOD_V3_OK;
    }
    if (parameter->type ==
            WOTBMOD_V3_RENDER_PARAMETER_VEC4 &&
        IsColorParameterName(parameter->name)) {
        const WotbModV3Color color = {
            parameter->value.vec4.x,
            parameter->value.vec4.y,
            parameter->value.vec4.z,
            parameter->value.vec4.w};
        if (!IsFinite(color)) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        material->color = color;
        return WOTBMOD_V3_OK;
    }
    if (parameter->type ==
            WOTBMOD_V3_RENDER_PARAMETER_FLOAT &&
        StringEquals(parameter->name, "opacity")) {
        if (!IsFinite(parameter->value.scalar) ||
            parameter->value.scalar < 0.0f ||
            parameter->value.scalar > 1.0f) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        material->opacity = parameter->value.scalar;
        return WOTBMOD_V3_OK;
    }
    if (parameter->type ==
            WOTBMOD_V3_RENDER_PARAMETER_TEXTURE &&
        IsTextureParameterName(parameter->name)) {
        if (material->kind != MaterialKind::Sprite) {
            return WOTBMOD_V3_E_NOT_SUPPORTED;
        }
        if (request->auxiliary_object == 0u) {
            material->texture = 0u;
            return WOTBMOD_V3_OK;
        }
        if (!FindTextureLocked(
                owner, request->auxiliary_object)) {
            return WOTBMOD_V3_E_INVALID_HANDLE;
        }
        material->texture = request->auxiliary_object;
        return WOTBMOD_V3_OK;
    }
    return WOTBMOD_V3_E_NOT_SUPPORTED;
}

WotbModV3Result DestroyMaterialLocked(
    WotbModV3Handle owner,
    uint64_t token) {
    const auto found = g_renderer.materials.find(token);
    if (found == g_renderer.materials.end() ||
        !found->second || found->second->owner != owner) {
        return WOTBMOD_V3_E_INVALID_HANDLE;
    }
    g_renderer.materials.erase(found);
    return WOTBMOD_V3_OK;
}

WotbModV3Result DrawSpriteLocked(
    WotbModV3Handle owner,
    const ClientHostObjectRequest* request) {
    if (!request || !request->payload ||
        request->payload_size <
            sizeof(WotbModV3DrawSprite)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    const WotbModV3DrawSprite* draw =
        static_cast<const WotbModV3DrawSprite*>(
            request->payload);
    if (draw->struct_size < sizeof(*draw) ||
        draw->api_version != WOTBMOD_V3_RENDER_VERSION ||
        !IsFinite(draw->destination) ||
        !IsFinite(draw->source_uv) ||
        !IsFinite(draw->color) ||
        !IsFinite(draw->rotation_radians) ||
        !IsFinite(draw->z)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    TextureResource* texture =
        FindTextureLocked(owner, request->object);
    if (!texture) {
        return WOTBMOD_V3_E_INVALID_HANDLE;
    }
    MaterialResource* material = nullptr;
    if (request->related_object != 0u) {
        material = FindMaterialLocked(
            owner, request->related_object);
        if (!material) {
            return WOTBMOD_V3_E_INVALID_HANDLE;
        }
        if (material->kind == MaterialKind::Sprite &&
            material->texture != 0u) {
            texture = FindTextureLocked(
                owner, material->texture);
            if (!texture) {
                return WOTBMOD_V3_E_OBJECT_DESTROYED;
            }
        }
    }
    const bool use_texture =
        !material || material->kind == MaterialKind::Sprite;
    if (use_texture && !texture->view &&
        !CreateGpuTextureLocked(texture)) {
        return WOTBMOD_V3_E_OBJECT_DESTROYED;
    }
    if (draw->destination.width == 0.0f ||
        draw->destination.height == 0.0f) {
        return WOTBMOD_V3_OK;
    }
    const WotbModV3Color material_color =
        material
            ? material->color
            : WotbModV3Color{
                  1.0f, 1.0f, 1.0f, 1.0f};
    const WotbModV3Color color = MultiplyColor(
        draw->color,
        material_color,
        material ? material->opacity : 1.0f);
    const float center_x =
        draw->destination.x +
        draw->destination.width * 0.5f;
    const float center_y =
        draw->destination.y +
        draw->destination.height * 0.5f;
    const float half_width =
        draw->destination.width * 0.5f;
    const float half_height =
        draw->destination.height * 0.5f;
    const float cosine =
        std::cos(draw->rotation_radians);
    const float sine =
        std::sin(draw->rotation_radians);
    const float local_x[4] = {
        -half_width, half_width, half_width, -half_width};
    const float local_y[4] = {
        -half_height, -half_height, half_height, half_height};
    float x[4] = {};
    float y[4] = {};
    for (size_t index = 0u; index < 4u; ++index) {
        x[index] =
            center_x +
            local_x[index] * cosine -
            local_y[index] * sine;
        y[index] =
            center_y +
            local_x[index] * sine +
            local_y[index] * cosine;
    }
    const float u0 = draw->source_uv.x;
    const float v0 = draw->source_uv.y;
    const float u1 =
        draw->source_uv.x + draw->source_uv.width;
    const float v1 =
        draw->source_uv.y + draw->source_uv.height;
    const Vertex corners[4] = {
        MakeVertex(x[0], y[0], draw->z, u0, v0, color),
        MakeVertex(x[1], y[1], draw->z, u1, v0, color),
        MakeVertex(x[2], y[2], draw->z, u1, v1, color),
        MakeVertex(x[3], y[3], draw->z, u0, v1, color)};
    const Vertex vertices[6] = {
        corners[0],
        corners[1],
        corners[2],
        corners[0],
        corners[2],
        corners[3]};
    return DrawVerticesLocked(
        vertices,
        static_cast<UINT>(
            sizeof(vertices) / sizeof(vertices[0])),
        use_texture ? texture->view : nullptr,
        use_texture,
        material ? material->blend : true,
        material ? material->depth : false);
}

void AppendGlyph(
    std::vector<Vertex>* vertices,
    unsigned char character,
    float x,
    float y,
    float width,
    float height,
    const WotbModV3Color& color) {
    const unsigned int glyph_index =
        static_cast<unsigned int>(character - kFirstGlyph);
    const unsigned int column =
        glyph_index % kFontColumns;
    const unsigned int row =
        glyph_index / kFontColumns;
    const float u0 =
        static_cast<float>(column * kFontCellWidth) /
        static_cast<float>(kFontAtlasWidth);
    const float v0 =
        static_cast<float>(row * kFontCellHeight) /
        static_cast<float>(kFontAtlasHeight);
    const float u1 =
        static_cast<float>(
            (column + 1u) * kFontCellWidth) /
        static_cast<float>(kFontAtlasWidth);
    const float v1 =
        static_cast<float>(
            (row + 1u) * kFontCellHeight) /
        static_cast<float>(kFontAtlasHeight);
    const Vertex corners[4] = {
        MakeVertex(x, y, 0.0f, u0, v0, color),
        MakeVertex(x + width, y, 0.0f, u1, v0, color),
        MakeVertex(
            x + width, y + height, 0.0f, u1, v1, color),
        MakeVertex(x, y + height, 0.0f, u0, v1, color)};
    vertices->push_back(corners[0]);
    vertices->push_back(corners[1]);
    vertices->push_back(corners[2]);
    vertices->push_back(corners[0]);
    vertices->push_back(corners[2]);
    vertices->push_back(corners[3]);
}

WotbModV3Result DrawTextLocked(
    const ClientHostObjectRequest* request) {
    if (!request || !request->payload ||
        request->payload_size <
            sizeof(WotbModV3DrawText)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    const WotbModV3DrawText* draw =
        static_cast<const WotbModV3DrawText*>(
            request->payload);
    const size_t text_length =
        BoundedStringLength(
            draw->text, WOTBMOD_V3_MAX_MESSAGE);
    if (draw->struct_size < sizeof(*draw) ||
        draw->api_version != WOTBMOD_V3_RENDER_VERSION ||
        text_length > WOTBMOD_V3_MAX_MESSAGE ||
        !IsSupportedFont(draw->font_uri) ||
        !IsFinite(draw->position) ||
        !IsFinite(draw->color) ||
        !IsFinite(draw->font_size) ||
        draw->font_size <= 0.0f ||
        !IsFinite(draw->max_width) ||
        draw->max_width < 0.0f) {
        return IsSupportedFont(draw->font_uri)
            ? WOTBMOD_V3_E_INVALID_ARGUMENT
            : WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    if (text_length == 0u) {
        return WOTBMOD_V3_OK;
    }
    if (!EnsurePipelineLocked()) {
        return WOTBMOD_V3_E_PLATFORM;
    }
    if (!EnsureFontAtlasLocked()) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    const float advance = draw->font_size * 0.625f;
    const float glyph_width = draw->font_size * 0.75f;
    const float glyph_height = draw->font_size * 1.25f;
    const float line_height = draw->font_size * 1.25f;
    if (!IsFinite(advance) || !IsFinite(glyph_width) ||
        !IsFinite(glyph_height) || !IsFinite(line_height)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    float cursor_x = draw->position.x;
    float cursor_y = draw->position.y;
    const float line_start = draw->position.x;
    std::vector<Vertex> vertices;
    try {
        vertices.reserve(
            std::min(
                text_length * 6u,
                static_cast<size_t>(kMaximumVertices)));
        for (size_t index = 0u;
             index < text_length;
             ++index) {
            unsigned char character =
                static_cast<unsigned char>(draw->text[index]);
            if (character == '\r') {
                continue;
            }
            if (character == '\n') {
                cursor_x = line_start;
                cursor_y += line_height;
                continue;
            }
            if (character == '\t') {
                cursor_x += advance * 4.0f;
                continue;
            }
            if (draw->max_width > 0.0f &&
                cursor_x > line_start &&
                cursor_x - line_start + advance >
                    draw->max_width) {
                cursor_x = line_start;
                cursor_y += line_height;
            }
            if (character < kFirstGlyph ||
                character > kLastGlyph) {
                character = static_cast<unsigned char>('?');
            }
            if (character !=
                static_cast<unsigned char>(' ')) {
                AppendGlyph(
                    &vertices,
                    character,
                    cursor_x,
                    cursor_y,
                    glyph_width,
                    glyph_height,
                    draw->color);
            }
            cursor_x += advance;
        }
    } catch (const std::bad_alloc&) {
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }
    if (vertices.empty()) {
        return WOTBMOD_V3_OK;
    }
    if (vertices.size() > kMaximumVertices) {
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }
    return DrawVerticesLocked(
        vertices.data(),
        static_cast<UINT>(vertices.size()),
        g_renderer.font_view,
        true,
        true,
        false);
}

WotbModV3Result DrawLineLocked(
    const ClientHostObjectRequest* request) {
    if (!request || !request->payload ||
        request->payload_size <
            sizeof(WotbModV3DrawLine)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    const WotbModV3DrawLine* draw =
        static_cast<const WotbModV3DrawLine*>(
            request->payload);
    if (draw->struct_size < sizeof(*draw) ||
        draw->api_version != WOTBMOD_V3_RENDER_VERSION ||
        !IsFinite(draw->from) ||
        !IsFinite(draw->to) ||
        !IsFinite(draw->color) ||
        !IsFinite(draw->width) ||
        draw->width <= 0.0f) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    const float delta_x = draw->to.x - draw->from.x;
    const float delta_y = draw->to.y - draw->from.y;
    const float length =
        std::sqrt(delta_x * delta_x + delta_y * delta_y);
    if (length <= 0.0001f) {
        const float half = draw->width * 0.5f;
        const Vertex corners[4] = {
            MakeVertex(
                draw->from.x - half,
                draw->from.y - half,
                draw->from.z,
                0.0f,
                0.0f,
                draw->color),
            MakeVertex(
                draw->from.x + half,
                draw->from.y - half,
                draw->from.z,
                0.0f,
                0.0f,
                draw->color),
            MakeVertex(
                draw->from.x + half,
                draw->from.y + half,
                draw->from.z,
                0.0f,
                0.0f,
                draw->color),
            MakeVertex(
                draw->from.x - half,
                draw->from.y + half,
                draw->from.z,
                0.0f,
                0.0f,
                draw->color)};
        const Vertex vertices[6] = {
            corners[0],
            corners[1],
            corners[2],
            corners[0],
            corners[2],
            corners[3]};
        return DrawVerticesLocked(
            vertices,
            static_cast<UINT>(
                sizeof(vertices) / sizeof(vertices[0])),
            nullptr,
            false,
            true,
            false);
    }
    float perpendicular_x = 0.0f;
    float perpendicular_y = 0.0f;
    perpendicular_x =
        -delta_y / length * draw->width * 0.5f;
    perpendicular_y =
        delta_x / length * draw->width * 0.5f;
    const float z0 = draw->from.z;
    const float z1 = draw->to.z;
    const Vertex corners[4] = {
        MakeVertex(
            draw->from.x - perpendicular_x,
            draw->from.y - perpendicular_y,
            z0,
            0.0f,
            0.0f,
            draw->color),
        MakeVertex(
            draw->from.x + perpendicular_x,
            draw->from.y + perpendicular_y,
            z0,
            0.0f,
            0.0f,
            draw->color),
        MakeVertex(
            draw->to.x + perpendicular_x,
            draw->to.y + perpendicular_y,
            z1,
            0.0f,
            0.0f,
            draw->color),
        MakeVertex(
            draw->to.x - perpendicular_x,
            draw->to.y - perpendicular_y,
            z1,
            0.0f,
            0.0f,
            draw->color)};
    const Vertex vertices[6] = {
        corners[0],
        corners[1],
        corners[2],
        corners[0],
        corners[2],
        corners[3]};
    return DrawVerticesLocked(
        vertices,
        static_cast<UINT>(
            sizeof(vertices) / sizeof(vertices[0])),
        nullptr,
        false,
        true,
        false);
}

WotbModV3Result PushStateLocked(WotbModV3Handle owner) {
    if (!g_renderer.context) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    if (g_renderer.state_stack.size() >=
        kMaximumStateDepth) {
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }
    if (!g_renderer.state_stack.empty() &&
        g_renderer.state_stack.back().owner != owner) {
        return WOTBMOD_V3_E_CONFLICT;
    }
    ExplicitState entry;
    entry.owner = owner;
    entry.frame_index = g_renderer.frame_index;
    entry.state.reset(new (std::nothrow) StateSnapshot());
    if (!entry.state) {
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }
    entry.state->Capture(g_renderer.context);
    try {
        g_renderer.state_stack.push_back(std::move(entry));
    } catch (const std::bad_alloc&) {
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result PopStateLocked(WotbModV3Handle owner) {
    if (g_renderer.state_stack.empty()) {
        return WOTBMOD_V3_E_CONFLICT;
    }
    ExplicitState& entry = g_renderer.state_stack.back();
    if (entry.owner != owner ||
        entry.frame_index != g_renderer.frame_index) {
        return WOTBMOD_V3_E_CONFLICT;
    }
    if (entry.state) {
        entry.state->Restore();
    }
    g_renderer.state_stack.pop_back();
    return WOTBMOD_V3_OK;
}

bool IsRenderOperation(const char* operation) {
    return operation &&
           std::strncmp(operation, "render_", 7u) == 0;
}

}  // namespace

WotbModV3Result ManagedRendererCreate() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_renderer.created) {
        return WOTBMOD_V3_E_ALREADY_EXISTS;
    }
    g_renderer.created = true;
    g_renderer.next_token = 1u;
    return WOTBMOD_V3_OK;
}

WotbModV3Result ManagedRendererUpdateFrame(
    void* swap_chain,
    void* device,
    void* device_context,
    uint32_t width,
    uint32_t height,
    uint64_t frame_index,
    double delta_seconds) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_renderer.created) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    if (!IsFinite(delta_seconds) || delta_seconds < 0.0) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    ID3D11Device* next_device =
        static_cast<ID3D11Device*>(device);
    ID3D11DeviceContext* next_context =
        static_cast<ID3D11DeviceContext*>(device_context);
    IDXGISwapChain* next_swap_chain =
        static_cast<IDXGISwapChain*>(swap_chain);
    if (frame_index != g_renderer.frame_index) {
        RestoreStateStackLocked();
    }
    if (next_device != g_renderer.device ||
        next_context != g_renderer.context) {
        RestoreStateStackLocked();
        ReleasePipelineLocked();
        ReleaseGpuTexturesLocked();
        if (next_device) {
            next_device->AddRef();
        }
        if (next_context) {
            next_context->AddRef();
        }
        ReleaseCom(g_renderer.context);
        ReleaseCom(g_renderer.device);
        g_renderer.device = next_device;
        g_renderer.context = next_context;
    }
    if (next_swap_chain != g_renderer.swap_chain) {
        if (next_swap_chain) {
            next_swap_chain->AddRef();
        }
        ReleaseCom(g_renderer.swap_chain);
        g_renderer.swap_chain = next_swap_chain;
    }
    g_renderer.width = width;
    g_renderer.height = height;
    g_renderer.frame_index = frame_index;
    g_renderer.delta_seconds = delta_seconds;
    if (!g_renderer.device || !g_renderer.context ||
        width == 0u || height == 0u) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result ManagedRendererInvoke(
    WotbModV3Handle mod,
    const char* operation,
    const ClientHostObjectRequest* request,
    ClientHostObjectResponse* response) {
    if (!operation || operation[0] == '\0' ||
        !IsRenderOperation(operation) ||
        mod == WOTBMOD_V3_INVALID_HANDLE) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_renderer.created) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    if (StringEquals(operation, "render_create_texture")) {
        return request && request->payload
            ? CreateTextureLocked(
                  mod,
                  static_cast<
                      const WotbModV3TextureDescriptor*>(
                      request->payload),
                  response)
            : WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (StringEquals(operation, "render_update_texture")) {
        return UpdateTextureLocked(mod, request);
    }
    if (StringEquals(operation, "render_destroy_texture")) {
        return request
            ? DestroyTextureLocked(mod, request->object)
            : WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (StringEquals(operation, "render_create_material")) {
        return request && request->payload
            ? CreateMaterialLocked(
                  mod,
                  static_cast<
                      const WotbModV3MaterialDescriptor*>(
                      request->payload),
                  response)
            : WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (StringEquals(
            operation, "render_set_material_parameter")) {
        return SetMaterialParameterLocked(mod, request);
    }
    if (StringEquals(operation, "render_destroy_material")) {
        return request
            ? DestroyMaterialLocked(mod, request->object)
            : WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (StringEquals(operation, "render_draw_sprite")) {
        return DrawSpriteLocked(mod, request);
    }
    if (StringEquals(operation, "render_draw_text")) {
        return DrawTextLocked(request);
    }
    if (StringEquals(operation, "render_draw_line")) {
        return DrawLineLocked(request);
    }
    if (StringEquals(operation, "render_draw_mesh")) {
        /*
         * request->object is a retained DAVA Scene/Entity token, not a
         * D3D11 vertex/index buffer. The native Scene bridge must consume
         * this operation before falling back to the managed renderer.
         */
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    if (StringEquals(operation, "render_push_state")) {
        return PushStateLocked(mod);
    }
    if (StringEquals(operation, "render_pop_state")) {
        return PopStateLocked(mod);
    }
    return WOTBMOD_V3_E_NOT_SUPPORTED;
}

void ManagedRendererShutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_renderer.created) {
        return;
    }
    RestoreStateStackLocked();
    g_renderer.materials.clear();
    g_renderer.textures.clear();
    g_renderer.texture_bytes = 0u;
    ReleaseFrameObjectsLocked();
    g_renderer.created = false;
    g_renderer.next_token = 1u;
}

}  // namespace loader
}  // namespace wotbmod
