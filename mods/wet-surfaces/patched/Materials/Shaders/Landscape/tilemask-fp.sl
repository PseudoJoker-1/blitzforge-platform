#include "common.slh"

#define USE_LANDSCAPE_SCALED_TILES_NON_PBR (LANDSCAPE_SCALED_TILES_NON_PBR && (DEVICE_TIER != DEVICE_TIER_ULTRA_LOW) && (DEVICE_TIER != DEVICE_TIER_LOW) && LANDSCAPE_HEIGHT_BLEND_ALLOWED)

#if DRAW_DEPTH_ONLY
    #include "depth-only-fragment-shader.slh"
#else

#if LANDSCAPE_PBR
    #include "srgb.slh"
    #include "pbr-lighting.slh"
    #include "normal-blending.slh"
#elif RECEIVE_SHADOW
    #include "shadow-mapping.slh"
#endif

fragment_in
{
    float2 texCoord : TEXCOORD0;
    float2 texCoordTiled : TEXCOORD1;

    #if LANDSCAPE_MORPHING_COLOR
        float4 morphColor : COLOR0;
    #endif

    #if RECEIVE_SHADOW || LANDSCAPE_PBR
        float4 worldPos : COLOR1;
    #endif
    #if RECEIVE_SHADOW
        float4 projectedPosition : COLOR2;
        float3 shadowPos : COLOR5;
    #endif

    #if USE_VERTEX_FOG
        float4 varFog : TEXCOORD5;
    #endif
};

fragment_out
{
    float4 color : SV_TARGET0;
};

#if (LANDSCAPE_HEIGHT_BLEND_ALLOWED && LANDSCAPE_HEIGHT_BLEND) || LANDSCAPE_PBR
    uniform sampler2D tileMaskHeightBlend;
    #if !LANDSCAPE_PBR
        uniform sampler2D tileHeightTexture;
    #endif
#else
    uniform sampler2D tileMask;
#endif

#if !LANDSCAPE_PBR
    uniform sampler2D tileTexture0;
    uniform sampler2D colorTexture;
#endif

#if LANDSCAPE_PBR
    uniform sampler2D pbrAlbedoRoughnessMap; // RGB - global Albedo attenuation, A - global Roughness attenuation
    uniform sampler2D pbrLandscapeNormalMap; // RG or AG(DXT5NM) or GA(ASTC) - global Normal
    uniform sampler2D pbrLandscapeLightmap; // RG or AG(DXT5NM) or GA(ASTC) - baked shadow / baked AO

    uniform sampler2DArray tileAlbedoHeightArray; // RGB - Albedo, A - Height for heightblending
    uniform sampler2DArray tileNormalArray; // RG or AG(DXT5NM) or GA(ASTC) - normal
    #if LANDSCAPE_HAS_METALLIC_AND_EMISSION
        uniform sampler2DArray tileRoughnessMetallicArray; // RG or AG(DXT5NM) or GA(ASTC) - roughness / metallic
        uniform sampler2DArray tileAOEmissionArray; // RG or AG(DXT5NM) or GA(ASTC) - AO / emission
    #else
        uniform sampler2DArray tileRoughnessAOArray; // RG or AG(DXT5NM) or GA(ASTC) - roughness / AO
    #endif

    [auto][a] property float4 lightPosition0;
    [auto][a] property float3 lightColor0;
    [auto][a] property float lightIntensity0;

    [auto][a] property float3 cameraPosition;

    [auto][a] property float4x4 invViewMatrix;
#endif

#if LANDSCAPE_PBR
    #if GLOBAL_PBR_TINT
        #if !IGNORE_BASE_COLOR_PBR_TINT
            [material][a] property float3 baseColorPbrTint = float3(0.5, 0.5, 0.5);
        #endif
        [material][a] property float4 landscapeTilesRoughnessPbrTint = float4(0.5, 0.5, 0.5, 0.5);
    #endif
#else
    #if GLOBAL_TINT
        #if !IGNORE_GLOBAL_FLAT_COLOR
            [material][a] property float3 globalFlatColor = float3(0.5, 0.5, 0.5);
        #endif
        #if LANDSCAPE_SEPARATE_LIGHTMAP_CHANNEL
            [material][a] property float3 landscapeLightmapAdjustment = float3(0.0, 1.0, 1.0);
        #endif
    #endif
#endif

#if LANDSCAPE_USE_RELAXMAP
    uniform sampler2D relaxmap;

    [material][instance] property float relaxmapScale = 1.0;
#endif

[material][instance] property float3 tileColor0 = float3(1, 1, 1);
[material][instance] property float3 tileColor1 = float3(1, 1, 1);
[material][instance] property float3 tileColor2 = float3(1, 1, 1);
[material][instance] property float3 tileColor3 = float3(1, 1, 1);

#if LANDSCAPE_PBR || USE_LANDSCAPE_SCALED_TILES_NON_PBR
    [material][instance] property float tileScale0 = 1.0;
    [material][instance] property float tileScale1 = 1.0;
    [material][instance] property float tileScale2 = 1.0;
    [material][instance] property float tileScale3 = 1.0;
#endif

#if (LANDSCAPE_HEIGHT_BLEND_ALLOWED && LANDSCAPE_HEIGHT_BLEND) || LANDSCAPE_PBR
    [material][instance] property float4 heightMapScaleColor;
    [material][instance] property float4 heightMapOffsetColor;
    [material][instance] property float4 heightMapSoftnessColor;

    [material][instance] property float tilemaskWeight = 0.15;
#endif

#if CURSOR
    uniform sampler2D cursorTexture;
    [material][instance] property float4 cursorCoordSize = float4(0,0,1,1);
#endif

#if LANDSCAPE_HEIGHT_BLEND_ALLOWED && LANDSCAPE_HEIGHT_BLEND
    inline float3 HeightBlend(float3 input1, float3 input2, float3 input3, float3 input4, float4 height)
    {
        float4 heightStart = max(max(height.x, height.y), max(height.z, height.w)) - heightMapSoftnessColor;
        float4 b = max(height - heightStart,  0.001);
        return ((input1 * b.x) + (input2 * b.y) + (input3 * b.z) + (input4 * b.w)) / (b.x + b.y + b.z + b.w);
    }
#endif

#if LANDSCAPE_PBR
    inline half4 GetHeightBlendFactor(half4 height)
    {
        half4 heightStart = max(max(height.x, height.y), max(height.z, height.w)) - half4(heightMapSoftnessColor);
        half4 b = max(height - heightStart,  half(0.0));
        return b / (b.x + b.y + b.z + b.w);
    }
#endif

#if DEBUG_UNLIT
    [material][a] property float4 debugFlatColor = float4(1.0, 0.0, 1.0, 1.0);
#endif

fragment_out fp_main(fragment_in input)
{
    fragment_out output;

    float2 texCoord = input.texCoord;
    float2 texCoordTiled = input.texCoordTiled;

    #if LANDSCAPE_RELAXMAP && LANDSCAPE_USE_RELAXMAP
        float2 relaxedDelta = tex2D(relaxmap, texCoord).xy;
        texCoordTiled *= texCoord + (relaxedDelta - 0.5) / relaxmapScale;
    #endif

    #if LANDSCAPE_PBR || USE_LANDSCAPE_SCALED_TILES_NON_PBR
        float2 texCoordTiled0 = texCoordTiled * tileScale0;
        float2 texCoordTiled1 = texCoordTiled * tileScale1;
        float2 texCoordTiled2 = texCoordTiled * tileScale2;
        float2 texCoordTiled3 = texCoordTiled * tileScale3;
    #endif

    #if (LANDSCAPE_HEIGHT_BLEND_ALLOWED && LANDSCAPE_HEIGHT_BLEND) || LANDSCAPE_PBR
        float4 mask = tex2D(tileMaskHeightBlend, texCoord);
    #else
        float4 mask = tex2D(tileMask, texCoord);
    #endif
    
    half3 shadowColor = half3(1.0, 1.0, 1.0);
    half4 shadowMapInfo = half4(1.0, 1.0, 1.0, 1.0);
    half3 shadowMapValue;
    half shadowFinalVal = 0.0f;

    float4 outColor = float4(1.0f, 1.0f, 1.0f, 1.0f);

    #if LANDSCAPE_PBR
        half4 globalAlbedoRoughnessFetch = half4(tex2D(pbrAlbedoRoughnessMap, texCoord));
        half3 globalAlbedo = globalAlbedoRoughnessFetch.rgb * half(2.0); // multiply by 2.0 because 0.5 is neutral
        half globalRoughness = globalAlbedoRoughnessFetch.a * half(2.0); // multiply by 2.0 because 0.5 is neutral

        half3 globalNormal = half3(half2(FP_SWIZZLE(tex2D(pbrLandscapeNormalMap, float2(texCoord.x, 1.0 - texCoord.y)))), half(1.0));
        globalNormal.xy = globalNormal.xy * half(2.0) - half(1.0);
        globalNormal.z = sqrt(half(1.0) - saturate(dot(globalNormal.xy, globalNormal.xy)));

        float2 tileTexCoord[4];
        tileTexCoord[0] = texCoordTiled0;
        tileTexCoord[1] = texCoordTiled1;
        tileTexCoord[2] = texCoordTiled2;
        tileTexCoord[3] = texCoordTiled3;

        half3 tileAlbedo[4];
        half4 tileHeight;
        half2 tileNormal[4];
        half4 tileRoughness;
        half4 tileMetallic;
        half4 tileAO;
        half4 tileEmission;

        for (int i = 0; i < 4; i++)
        {
            half4 tileAlbedoHeightFetch = half4(tex2Darray(tileAlbedoHeightArray, tileTexCoord[i], i));
            tileAlbedo[i] = tileAlbedoHeightFetch.rgb;
            tileHeight[i] = tileAlbedoHeightFetch.a;
            tileNormal[i] = half2(FP_SWIZZLE(tex2Darray(tileNormalArray, tileTexCoord[i], i)));

        #if LANDSCAPE_HAS_METALLIC_AND_EMISSION
            half2 tileRoughnessMetallicFetch = half2(FP_SWIZZLE(tex2Darray(tileRoughnessMetallicArray, tileTexCoord[i], i)));
            half2 tileAOEmissionFetch = half2(FP_SWIZZLE(tex2Darray(tileAOEmissionArray, tileTexCoord[i], i)));

            tileRoughness[i] = tileRoughnessMetallicFetch.x;
            tileMetallic[i] = tileRoughnessMetallicFetch.y;
            tileAO[i] = tileAOEmissionFetch.x;
            tileEmission[i] = tileAOEmissionFetch.y;
        #else
            half2 tileRoughnessAOFetch = half2(FP_SWIZZLE(tex2Darray(tileRoughnessAOArray, tileTexCoord[i], i)));

            tileRoughness[i] = tileRoughnessAOFetch.x;
            tileMetallic[i] = half(0.0);
            tileAO[i] = tileRoughnessAOFetch.y;
            tileEmission[i] = half(0.0);
        #endif
        }
        
        #if GLOBAL_PBR_TINT
            tileRoughness *= half4(landscapeTilesRoughnessPbrTint) * half(2.0);
        #endif

        half4 legacyHeight = saturate(half(tilemaskWeight) * (half4(mask) * half(2.0) - half(1.0)) + tileHeight * half4(heightMapScaleColor) + half4(heightMapOffsetColor));
        half4 blendFactor = GetHeightBlendFactor(legacyHeight);

        half3 blendedAlbedo = tileAlbedo[0] * blendFactor.x + tileAlbedo[1] * blendFactor.y + tileAlbedo[2] * blendFactor.z + tileAlbedo[3] * blendFactor.w;
        half2 blendedNormal = tileNormal[0] * blendFactor.x + tileNormal[1] * blendFactor.y + tileNormal[2] * blendFactor.z + tileNormal[3] * blendFactor.w;
        half blendedRoughness = dot(tileRoughness, blendFactor);
        half blendedMetallic = dot(tileMetallic, blendFactor);
        half blendedAO = dot(tileAO, blendFactor);
        half blendedEmission = dot(tileEmission, blendFactor);

        half3 finalAlbedo = blendedAlbedo * globalAlbedo;

        half3 finalNormal = half3(blendedNormal, half(1.0));
        finalNormal.xy = finalNormal.xy * half(2.0) - half(1.0);
        finalNormal.z = sqrt(half(1.0) - saturate(dot(finalNormal.xy, finalNormal.xy)));
        finalNormal = NormalBlendUDN(globalNormal, finalNormal);

        half finalRoughness = blendedRoughness * globalRoughness;
        half finalMetallic = blendedMetallic;
        
        #if !IGNORE_BASE_COLOR_PBR_TINT && GLOBAL_PBR_TINT
            finalAlbedo *= half3(baseColorPbrTint) * half(2.0);
        #endif

        half3 V = half3(normalize(cameraPosition - input.worldPos.xyz));

        half3 L = half3(lightPosition0.xyz);
        L.xyz = half3(mul(float4(float3(L.xyz), 0.0), invViewMatrix).xyz);

        float2 bakedDirAO = FP_SWIZZLE(tex2D(pbrLandscapeLightmap, texCoord));

        #if RECEIVE_SHADOW
            shadowMapInfo = getCascadedShadow(input.worldPos, input.shadowPos, half4(input.projectedPosition), finalNormal, max(dot(finalNormal, L), half(0.0)));
            shadowFinalVal = lerp(half(bakedDirAO.x), shadowMapInfo.w, shadowMapInfo.z);
        #else
            shadowFinalVal = half(bakedDirAO.x);
        #endif

        // saturate for energy conservation
        finalAlbedo = saturate(finalAlbedo);
        finalMetallic = saturate(finalMetallic);
        finalRoughness = saturate(finalRoughness);
        half finalAO = saturate(blendedAO * half(bakedDirAO.y));
        half3 finalEmission = finalAlbedo * blendedEmission;

        // MOD: rain wetness for the landscape. Same curve the engine already
        // uses in pbr-fp.sl, written out inline rather than shared through a
        // header: common.slh is pulled in by vertex shaders too, and half-typed
        // helpers have no business there. Sits after finalEmission so glowing
        // surfaces are not dimmed, and before getPBR so the lighting reacts to
        // the lowered roughness instead of only seeing a darker albedo.
        {
            const half wetStrength = half(0.30);
            const half wetPorosity = saturate((finalRoughness - half(0.2)) / half(0.5));
            const half wetMultiplier = lerp(half(1.0), half(0.2), wetPorosity * (half(1.0) - finalMetallic));
            const half wetSpecular = finalRoughness * finalRoughness * wetMultiplier;
            const half wetRoughnessFactor = lerp(half(1.0), wetSpecular, wetStrength * half(0.5));

            finalAlbedo *= lerp(half(1.0), wetMultiplier, wetStrength);
            finalRoughness = lerp(half(0.0), finalRoughness, wetRoughnessFactor);
        }

        half3 pbrCol = getPBR(globalNormal, finalNormal, V, L, half3(lightColor0), half(lightIntensity0),
                              finalAlbedo, finalMetallic, finalRoughness, finalAO,
                              shadowFinalVal, finalEmission);

        // Linear to sRGB conversion without tonemapping
        outColor.rgb = float3(LinearToSRGB(pbrCol.r), LinearToSRGB(pbrCol.g), LinearToSRGB(pbrCol.b));

    #else // ! LANDSCAPE_PBR
        float4 colorMapFetch = tex2D(colorTexture, texCoord);
        float3 colorMapAlbedo = colorMapFetch.rgb;

        #if GLOBAL_TINT
            #if !IGNORE_GLOBAL_FLAT_COLOR
                colorMapAlbedo *= globalFlatColor.rgb * 2.0;
            #endif
        
            #if LANDSCAPE_SEPARATE_LIGHTMAP_CHANNEL
                float lightmapBrightness = landscapeLightmapAdjustment.r;
                float lightmapContrast = landscapeLightmapAdjustment.g;
                float lightmapGamma = landscapeLightmapAdjustment.b;
                
                colorMapFetch.a = pow(colorMapFetch.a, lightmapGamma);
                colorMapFetch.a = (colorMapFetch.a - 0.5) * lightmapContrast + 0.5;
                colorMapFetch.a += lightmapBrightness;
            #endif
        #endif

        #if RECEIVE_SHADOW
            shadowMapInfo = getCascadedShadow(input.worldPos, input.shadowPos, half4(input.projectedPosition), half3(0.0, 0.0, 1.0), half(1.0));
        
            #if LANDSCAPE_SEPARATE_LIGHTMAP_CHANNEL
                float lmLighteningMult = lerp(shadowLMGateFactor.w, 1.0, saturate(colorMapFetch.a * shadowLMGateFactor.z));
                shadowMapValue = getShadowColorMultLM(shadowMapInfo, half(lmLighteningMult));
                shadowColor = half3(colorMapAlbedo) * shadowMapValue * half(colorMapFetch.a);
            #else
                shadowMapValue = getShadowColor(shadowMapInfo);
                float lmBrightness = (colorMapAlbedo.x + colorMapAlbedo.y + colorMapAlbedo.z) * 0.33f;
                float3 lightenColorMap = lerp(colorMapAlbedo * shadowLMGateFactor.w, colorMapAlbedo, saturate(lmBrightness * shadowLMGateFactor.z));
                shadowColor = lerp(half3(colorMapAlbedo), half3(lightenColorMap) * shadowMapValue, shadowMapInfo.z);
            #endif
        #else
            #if LANDSCAPE_SEPARATE_LIGHTMAP_CHANNEL
                shadowColor = half3(colorMapAlbedo) * half(colorMapFetch.a);
            #else
                shadowColor = half3(colorMapAlbedo);
            #endif
        #endif

        #if USE_LANDSCAPE_SCALED_TILES_NON_PBR
            float tileColorFetch0 = tex2D(tileTexture0, texCoordTiled0).r;
            float tileColorFetch1 = tex2D(tileTexture0, texCoordTiled1).g;
            float tileColorFetch2 = tex2D(tileTexture0, texCoordTiled2).b;
            float tileColorFetch3 = tex2D(tileTexture0, texCoordTiled3).a;

            float4 tileColor = float4(tileColorFetch0, tileColorFetch1, tileColorFetch2, tileColorFetch3);
        #else
            float4 tileColor = tex2D(tileTexture0, texCoordTiled);
        #endif

        #if LANDSCAPE_HEIGHT_BLEND_ALLOWED && LANDSCAPE_HEIGHT_BLEND
            #if USE_LANDSCAPE_SCALED_TILES_NON_PBR
                float hMapFetch0 = tex2D(tileHeightTexture, texCoordTiled0).r;
                float hMapFetch1 = tex2D(tileHeightTexture, texCoordTiled1).g;
                float hMapFetch2 = tex2D(tileHeightTexture, texCoordTiled2).b;
                float hMapFetch3 = tex2D(tileHeightTexture, texCoordTiled3).a;

                float4 hMap = float4(hMapFetch0, hMapFetch1, hMapFetch2, hMapFetch3);
            #else
                float4 hMap = tex2D(tileHeightTexture, texCoordTiled);
            #endif

            float4 mask2 = saturate(tilemaskWeight*(mask * 2.0 - 1.0) + hMap * heightMapScaleColor + heightMapOffsetColor);

            outColor.rgb = HeightBlend(tileColor.r * tileColor0.rgb, 
                                        tileColor.g * tileColor1.rgb, 
                                        tileColor.b * tileColor2.rgb, 
                                        tileColor.a * tileColor3.rgb, mask2)* float3(shadowColor) * 2.0;
        #else
            outColor.rgb = (tileColor.r * mask.r * tileColor0.rgb +
                             tileColor.g * mask.g * tileColor1.rgb +
                             tileColor.b * mask.b * tileColor2.rgb +
                             tileColor.a * mask.a * tileColor3.rgb ) * float3(shadowColor) * 2.0;
    
        #endif

    #endif // LANDSCAPE_PBR

    #if LANDSCAPE_LOD_MORPHING && LANDSCAPE_MORPHING_COLOR
        outColor = outColor * 0.25 + input.morphColor * 0.75;
    #endif

    #if CURSOR
        float2 cursorCoord = (texCoord + cursorCoordSize.xy) / cursorCoordSize.zw + float2(0.5, 0.5);
        float4 cursorColor = tex2D(cursorTexture, cursorCoord);
        outColor.rgb *= 1.0 - cursorColor.a;
        outColor.rgb += cursorColor.rgb * cursorColor.a;
    #endif

    #if USE_VERTEX_FOG
        float   varFogAmoung = input.varFog.a;
        float3  varFogColor  = input.varFog.rgb;

        outColor.rgb = lerp(outColor.rgb, varFogColor, varFogAmoung);
    #endif

    output.color = outColor;
    #if LANDSCAPE_PBR
        #if VIEW_ALBEDO && !VIEW_AMBIENT && !VIEW_DIFFUSE && !VIEW_SPECULAR
            output.color.rgb = float3(LinearToSRGB(half(finalAlbedo.r)), LinearToSRGB(half(finalAlbedo.g)), LinearToSRGB(half(finalAlbedo.b)));
        #endif
        #if VIEW_ALL
            #if VIEW_NORMAL
                half3 normal = half3(blendedNormal, half(1.0));
                normal.xy = normal.xy * half(2.0) - half(1.0);
                normal.z = sqrt(half(1.0) - saturate(dot(normal.xy, normal.xy)));

                output.color.rgb = float3(normal) * 0.5 + 0.5;
            #endif

            #if VIEW_NORMAL_FINAL
                output.color.rgb = float3(finalNormal) * 0.5 + 0.5;
            #endif

            #if VIEW_ROUGHNESS
                output.color.rgb = float3(finalRoughness, finalRoughness, finalRoughness);
            #endif

            #if VIEW_METALLIC
                output.color.rgb = float3(finalMetallic, finalMetallic, finalMetallic);
            #endif

            #if VIEW_AMBIENTOCCLUSION
                output.color.rgb = float3(finalAO, finalAO, finalAO);
            #endif
        #endif
    #endif

    #if RECEIVE_SHADOW && DEBUG_SHADOW_CASCADES
        half3 debugShadowColor = getShadowColor(shadowMapInfo);
        output.color.rgb = float3(debugShadowColor);
    #endif

    #include "debug-modify-color.slh"
    return output;
}
#endif
