// Cosmetic weather overlay. See weather.h for the contract.
//
// The pass binds the back buffer itself and restores every piece of pipeline
// state it touches, so it does not care whether it runs before or after the
// game's own OMSetRenderTargets call.

#include "weather.h"

#include <d3dcompiler.h>

#include <cstdint>
#include <cstring>

extern "C" void ModLog(const char* fmt, ...);

namespace {

// ---------------------------------------------------------------- parameters

WeatherParams g_params = {
    /*enabled*/        false,
    /*rainIntensity*/  0.55f,
    /*rainSpeed*/      1.0f,
    /*rainSlant*/      0.18f,
    /*rainBrightness*/ 0.34f,
    /*rainColor*/      {0.62f, 0.70f, 0.86f},
};

// ------------------------------------------------------------------ device

ID3D11Device*             g_device        = nullptr;
ID3D11VertexShader*       g_vs            = nullptr;
ID3D11PixelShader*        g_ps            = nullptr;
ID3D11Buffer*             g_cb            = nullptr;
ID3D11BlendState*         g_blend         = nullptr;
ID3D11DepthStencilState*  g_depth         = nullptr;
ID3D11RasterizerState*    g_raster        = nullptr;
ID3D11RenderTargetView*   g_backBufferRtv = nullptr;  // not owned

bool     g_ready       = false;
bool     g_failed      = false;   // compile failed once; do not retry every frame
bool     g_drawnThisFrame = false;
uint64_t g_startTick   = 0;

struct WeatherCB {
    float time;
    float intensity;
    float speed;
    float slant;
    float color[3];
    float brightness;
};

// ------------------------------------------------------------------- shaders

// Fullscreen triangle from SV_VertexID; no vertex or index buffer needed.
const char kVertexShader[] =
    "struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
    "VSOut main(uint vid : SV_VertexID) {\n"
    "    VSOut o;\n"
    "    float2 t = float2((vid << 1) & 2, vid & 2);\n"
    "    o.uv  = t;\n"
    "    o.pos = float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
    "    return o;\n"
    "}\n";

// Three parallax layers of falling streaks. Cheap hash noise, no textures.
const char kPixelShader[] =
    "cbuffer WeatherCB : register(b0) {\n"
    "    float  gTime;\n"
    "    float  gIntensity;\n"
    "    float  gSpeed;\n"
    "    float  gSlant;\n"
    "    float3 gColor;\n"
    "    float  gBrightness;\n"
    "};\n"
    "\n"
    "float hash21(float2 p) {\n"
    "    p = frac(p * float2(123.34, 456.21));\n"
    "    p += dot(p, p + 45.32);\n"
    "    return frac(p.x * p.y);\n"
    "}\n"
    "\n"
    "// One grid of drops. Each cell either holds a streak or does not.\n"
    "float rainLayer(float2 uv, float scale, float speed, float density) {\n"
    "    uv.x += uv.y * gSlant;\n"
    "    float2 st = uv * scale;\n"
    "    st.y += gTime * speed * gSpeed;\n"
    "    float2 id = floor(st);\n"
    "    float2 f  = frac(st);\n"
    "    if (hash21(id) > density) return 0.0;\n"
    "    float xoff   = (hash21(id + 7.3) - 0.5) * 0.7;\n"
    "    float d      = abs(f.x - 0.5 - xoff);\n"
    "    float streak = smoothstep(0.055, 0.0, d);\n"
    "    // fade each streak in and out inside its own cell so it reads as a\n"
    "    // finite drop rather than an endless line\n"
    "    float head = smoothstep(0.00, 0.18, f.y);\n"
    "    float tail = smoothstep(1.00, 0.45, f.y);\n"
    "    return streak * head * tail;\n"
    "}\n"
    "\n"
    "float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {\n"
    "    float acc = 0.0;\n"
    "    acc += rainLayer(uv, float2(38.0, 9.0),  1.10, 0.26) * 1.00;\n"
    "    acc += rainLayer(uv, float2(24.0, 6.0),  0.72, 0.20) * 0.70;\n"
    "    acc += rainLayer(uv, float2(58.0, 14.0), 1.55, 0.30) * 0.45;\n"
    "    acc = saturate(acc) * gIntensity * gBrightness;\n"
    "    return float4(gColor * acc, acc);\n"
    "}\n";

bool CompileShaders() {
    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    ID3DBlob* err    = nullptr;

    HRESULT hr = D3DCompile(kVertexShader, sizeof(kVertexShader) - 1, "weather_vs", nullptr,
                            nullptr, "main", "vs_4_0", 0, 0, &vsBlob, &err);
    if (FAILED(hr)) {
        ModLog("Weather: vertex shader compile failed hr=0x%08X %s",
               hr, err ? (const char*)err->GetBufferPointer() : "");
        if (err) err->Release();
        return false;
    }
    if (err) { err->Release(); err = nullptr; }

    hr = D3DCompile(kPixelShader, sizeof(kPixelShader) - 1, "weather_ps", nullptr,
                    nullptr, "main", "ps_4_0", 0, 0, &psBlob, &err);
    if (FAILED(hr)) {
        ModLog("Weather: pixel shader compile failed hr=0x%08X %s",
               hr, err ? (const char*)err->GetBufferPointer() : "");
        if (err) err->Release();
        vsBlob->Release();
        return false;
    }
    if (err) err->Release();

    hr = g_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                      nullptr, &g_vs);
    if (SUCCEEDED(hr)) {
        hr = g_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
                                         nullptr, &g_ps);
    }
    vsBlob->Release();
    psBlob->Release();

    if (FAILED(hr)) {
        ModLog("Weather: shader creation failed hr=0x%08X", hr);
        return false;
    }
    return true;
}

bool CreateStates() {
    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth      = (sizeof(WeatherCB) + 15) & ~15u;
    cbd.Usage          = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g_device->CreateBuffer(&cbd, nullptr, &g_cb))) return false;

    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable           = TRUE;
    bd.RenderTarget[0].SrcBlend              = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(g_device->CreateBlendState(&bd, &g_blend))) return false;

    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable    = FALSE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.StencilEnable  = FALSE;
    if (FAILED(g_device->CreateDepthStencilState(&dd, &g_depth))) return false;

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode        = D3D11_FILL_SOLID;
    rd.CullMode        = D3D11_CULL_NONE;
    rd.DepthClipEnable = FALSE;
    if (FAILED(g_device->CreateRasterizerState(&rd, &g_raster))) return false;

    return true;
}

// Everything the pass overwrites, so the game's own rendering continues
// undisturbed afterwards.
struct SavedState {
    ID3D11RenderTargetView*  rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    ID3D11DepthStencilView*  dsv;
    ID3D11BlendState*        blend;
    FLOAT                    blendFactor[4];
    UINT                     sampleMask;
    ID3D11DepthStencilState* depth;
    UINT                     stencilRef;
    ID3D11RasterizerState*   raster;
    D3D11_VIEWPORT           viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    UINT                     viewportCount;
    ID3D11VertexShader*      vs;
    ID3D11PixelShader*       ps;
    ID3D11GeometryShader*    gs;
    ID3D11Buffer*            psCb0;
    ID3D11InputLayout*       layout;
    D3D11_PRIMITIVE_TOPOLOGY topology;
};

void SaveState(ID3D11DeviceContext* c, SavedState& s) {
    std::memset(&s, 0, sizeof(s));
    c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s.rtv, &s.dsv);
    c->OMGetBlendState(&s.blend, s.blendFactor, &s.sampleMask);
    c->OMGetDepthStencilState(&s.depth, &s.stencilRef);
    c->RSGetState(&s.raster);
    s.viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    c->RSGetViewports(&s.viewportCount, s.viewports);
    c->VSGetShader(&s.vs, nullptr, nullptr);
    c->PSGetShader(&s.ps, nullptr, nullptr);
    c->GSGetShader(&s.gs, nullptr, nullptr);
    c->PSGetConstantBuffers(0, 1, &s.psCb0);
    c->IAGetInputLayout(&s.layout);
    c->IAGetPrimitiveTopology(&s.topology);
}

template <typename T>
void SafeRelease(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

void RestoreState(ID3D11DeviceContext* c, SavedState& s) {
    c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s.rtv, s.dsv);
    c->OMSetBlendState(s.blend, s.blendFactor, s.sampleMask);
    c->OMSetDepthStencilState(s.depth, s.stencilRef);
    c->RSSetState(s.raster);
    if (s.viewportCount) c->RSSetViewports(s.viewportCount, s.viewports);
    c->VSSetShader(s.vs, nullptr, 0);
    c->PSSetShader(s.ps, nullptr, 0);
    c->GSSetShader(s.gs, nullptr, 0);
    c->PSSetConstantBuffers(0, 1, &s.psCb0);
    c->IASetInputLayout(s.layout);
    c->IASetPrimitiveTopology(s.topology);

    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) SafeRelease(s.rtv[i]);
    SafeRelease(s.dsv);
    SafeRelease(s.blend);
    SafeRelease(s.depth);
    SafeRelease(s.raster);
    SafeRelease(s.vs);
    SafeRelease(s.ps);
    SafeRelease(s.gs);
    SafeRelease(s.psCb0);
    SafeRelease(s.layout);
}

// Back buffer size, so the fullscreen triangle covers exactly the window.
bool GetViewSize(ID3D11RenderTargetView* rtv, float& w, float& h) {
    ID3D11Resource* res = nullptr;
    rtv->GetResource(&res);
    if (!res) return false;
    ID3D11Texture2D* tex = nullptr;
    HRESULT hr = res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
    res->Release();
    if (FAILED(hr) || !tex) return false;
    D3D11_TEXTURE2D_DESC d = {};
    tex->GetDesc(&d);
    tex->Release();
    w = (float)d.Width;
    h = (float)d.Height;
    return d.Width > 0 && d.Height > 0;
}

void RenderPass(ID3D11DeviceContext* c) {
    float vw = 0.0f, vh = 0.0f;
    if (!GetViewSize(g_backBufferRtv, vw, vh)) return;

    SavedState saved;
    SaveState(c, saved);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(c->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        WeatherCB cb;
        cb.time       = (float)((GetTickCount64() - g_startTick) / 1000.0);
        cb.intensity  = g_params.rainIntensity;
        cb.speed      = g_params.rainSpeed;
        cb.slant      = g_params.rainSlant;
        cb.color[0]   = g_params.rainColor[0];
        cb.color[1]   = g_params.rainColor[1];
        cb.color[2]   = g_params.rainColor[2];
        cb.brightness = g_params.rainBrightness;
        std::memcpy(mapped.pData, &cb, sizeof(cb));
        c->Unmap(g_cb, 0);
    }

    D3D11_VIEWPORT vp = {0.0f, 0.0f, vw, vh, 0.0f, 1.0f};
    const FLOAT blendFactor[4] = {0.0f, 0.0f, 0.0f, 0.0f};

    c->OMSetRenderTargets(1, &g_backBufferRtv, nullptr);
    c->RSSetViewports(1, &vp);
    c->RSSetState(g_raster);
    c->OMSetBlendState(g_blend, blendFactor, 0xffffffff);
    c->OMSetDepthStencilState(g_depth, 0);
    c->IASetInputLayout(nullptr);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(g_vs, nullptr, 0);
    c->PSSetShader(g_ps, nullptr, 0);
    c->GSSetShader(nullptr, nullptr, 0);
    c->PSSetConstantBuffers(0, 1, &g_cb);
    c->Draw(3, 0);

    RestoreState(c, saved);
}

}  // namespace

// ------------------------------------------------------------------- exports

extern "C" {

void Weather_Init(ID3D11Device* device) {
    if (g_ready || g_failed || !device) return;
    g_device = device;
    if (!CompileShaders() || !CreateStates()) {
        ModLog("Weather: initialisation failed, overlay disabled");
        Weather_Shutdown();
        g_failed = true;
        return;
    }
    g_startTick = GetTickCount64();
    g_ready = true;
    ModLog("Weather: fullscreen rain pass ready");
}

void Weather_Shutdown() {
    SafeRelease(g_vs);
    SafeRelease(g_ps);
    SafeRelease(g_cb);
    SafeRelease(g_blend);
    SafeRelease(g_depth);
    SafeRelease(g_raster);
    g_backBufferRtv = nullptr;
    g_device = nullptr;
    g_ready = false;
}

void Weather_SetBackBufferView(ID3D11RenderTargetView* view) {
    g_backBufferRtv = view;
}

void Weather_OnSetRenderTargets(ID3D11DeviceContext* context,
                                UINT numViews,
                                ID3D11RenderTargetView* const* views) {
    if (!g_ready || !g_params.enabled || g_drawnThisFrame) return;
    if (!context || !g_backBufferRtv || numViews == 0 || !views) return;

    // The world is finished the moment the engine points itself back at the
    // back buffer to draw the interface. That is our slot.
    if (views[0] != g_backBufferRtv) return;

    g_drawnThisFrame = true;
    RenderPass(context);
}

void Weather_OnPresent() {
    g_drawnThisFrame = false;
}

WeatherParams* Weather_GetParams() {
    return &g_params;
}

}  // extern "C"
