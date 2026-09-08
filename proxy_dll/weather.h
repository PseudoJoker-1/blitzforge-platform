// Cosmetic weather overlay: an alpha-blended fullscreen pass drawn between the
// 3d scene and the game UI, so rain falls behind the interface rather than over
// it.
//
// Deliberately self-contained. The only thing overlay.cpp has to do is forward
// the OMSetRenderTargets calls and tell us when a frame ends.
#pragma once

#include <d3d11.h>

// Runtime-tunable knobs. These exist as live values (rather than the compile
// time constants the shader-side night grade uses) because the hangar palette
// UI needs something it can write to while the game is running.
struct WeatherParams {
    bool  enabled;
    float rainIntensity;   // 0 = dry, 1 = downpour
    float rainSpeed;       // fall speed multiplier
    float rainSlant;       // horizontal drift; wind
    float rainBrightness;  // alpha of the brightest streaks
    float rainColor[3];    // streak tint, linear rgb
};

extern "C" {

// Called once a device is known. Safe to call repeatedly.
void Weather_Init(ID3D11Device* device);

// Release every device resource. Call before the swap chain is resized or the
// device goes away.
void Weather_Shutdown();

// Forwarded from the OMSetRenderTargets hook, before the original call runs.
// Fires the weather pass on the first switch back to the swap chain's back
// buffer in a frame, which is where DAVA stops drawing the world and starts
// drawing the interface.
void Weather_OnSetRenderTargets(ID3D11DeviceContext* context,
                                UINT numViews,
                                ID3D11RenderTargetView* const* views);

// Tells us which RTV belongs to the back buffer, so we can recognise the
// scene -> UI transition. Call whenever the back buffer RTV is (re)created.
void Weather_SetBackBufferView(ID3D11RenderTargetView* view);

// Ends the frame: clears the once-per-frame latch.
void Weather_OnPresent();

// The live parameter block. Never null.
WeatherParams* Weather_GetParams();

}  // extern "C"
