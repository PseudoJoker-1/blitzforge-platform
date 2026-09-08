#include "common.slh"
#include "blending.slh"
#include "srgb.slh"
#include "pbr-lighting.slh"
#include "normal-blending.slh"
#include "highlight-animation.slh"
#if LOD_TRANSITION
    #include "lod-transition.slh"
#endif

#ensuredefined PBR_DECAL 0
#ensuredefined PBR_DETAIL 0

fragment_in
{
    #if TILED_DECAL_MASK
        float4 varTexCoord0 : TEXCOORD0;
    #else
        float2 varTexCoord0 : TEXCOORD0;
    #endif

    #if PBR_LIGHTMAP || PBR_DECAL
        float2 varTexCoord1 : TEXCOORD1;
    #endif

    #if PBR_DETAIL
        float2 varTexCoord2 : TEXCOORD2;
    #endif

    [lowp] half3 varToLightVec : COLOR1;

    half3 tangentToWorld0 : TANGENTTOWORLD0;
    half3 tangentToWorld1 : TANGENTTOWORLD1;
    half3 tangentToWorld2 : TANGENTTOWORLD2;


    #if VERTEX_COLOR
        [lowp] half4 varVertexColor : COLOR1;
    #endif

    #if USE_VERTEX_FOG
        [lowp] half4 varFog : TEXCOORD5;
    #endif

    #if RECEIVE_SHADOW || LOD_TRANSITION
        float4 projPos : COLOR2;
    #endif

    float4 worldPos : POSITION3;

    #if RECEIVE_SHADOW
        float3 shadowPos : COLOR5;
    #endif

    #if TILED_DECAL_MASK && TILED_DECAL_ANIMATED_EMISSION
        [lowp] half4 aniCamoParams : COLOR3;
    #endif

    #if NEEDS_LOCAL_POSITION
        half4 localPos : POSITION2; // xyz - position, w - normal.z
    #endif

    #if MULTIPLE_DECAL_TEXTURES
        half index : COLOR4;
    #endif
};

fragment_out
{
    half4 color : SV_TARGET0;
};

[auto][a] property float4x4 invViewMatrix;

[auto][a] property float3 lightColor0;
[auto][a] property float4x4 pointLights; // 0,1: (position, radius); 2,3: (color, falloff exponent)

#if PBR_LIGHTMAP
    uniform sampler2D pbrLightmap; // RG or AG(DXT5NM) or GA(ASTC) - directional shadow / ambient occlusion
#endif

#if ALPHATEST && ALPHATESTVALUE
    [material][a] property float alphatestThreshold = 0.0;
#endif

#if ALPHASTEPVALUE && ALPHABLEND
    [material][a] property float alphaStepValue = 0.5;
#endif

[material][a] property float normalScale = 1.0;
[auto][a] property float4 lightPosition0;

#if TILED_DECAL_MASK
    uniform sampler2DArray decalColorMap; // RGB - decal color, A - decal alpha
    #if TILED_DECAL_OVERRIDE_ROUGHNESS_METALLIC
        uniform sampler2DArray decalRMMap; // RG or AG(DXT5NM) or GA(ASTC) - roughness / metallic
    #endif

    #if TILED_DECAL_BLEND_NORMAL
        uniform sampler2DArray decalNormalMap; // RG or AG(DXT5NM) or GA(ASTC) - normal
        [material][a] property float decalNormalBlend = 0.5;
    #endif

    #if TILED_DECAL_SPREADING
        #include "decal-spreading.slh"
    #endif
#endif

#if DIRT_COVERAGE
    uniform sampler2D dirtNormalMap; // RG or AG(DXT5NM) or GA(ASTC) - normal
    uniform sampler2D dirtHeightMap; // R or A (API specific) - dirt height

    [material][a] property float4 dirtColor = float4(1.0, 1.0, 1.0, 1.0);
    [material][a] property float dirtRoughness = 0.9;
    [material][a] property float dirtStrength = 0.0;
#endif

#if WETNESS_MULTILEVEL || WETNESS_SIMPLIFIED
    #if WETNESS_MULTILEVEL
        #include "wetness.slh"
    #endif

    #if WETNESS_SIMPLIFIED
        // MOD: rain. 0 is the stock dry default; this is the global "everything is
    // rained on" amount. Darkens porous surfaces and drops roughness so they
    // catch a specular sheen, which is what reads as wet.
    // 0.75 crushed tanks to silhouettes: painted hull has porosity ~1.0, so
    // wetnessMultiplier lands near 0.2 and baseColor drops to 40%, which then
    // gets multiplied again by the night grade in debug-modify-color-half.slh.
    // 0.30 keeps the darkening at ~24% and leaves the specular sheen readable.
    [material][a] property float simpleWetnessStrength = 0.30;
    #endif
#elif GLOBAL_PBR_TINT
    #if !IGNORE_BASE_COLOR_PBR_TINT
        [material][a] property float3 baseColorPbrTint = float3(0.5, 0.5, 0.5);
    #endif
    #if !IGNORE_ROUGHNESS_PBR_TINT
        [material][a] property float roughnessPbrTint = 0.5;
    #endif
#endif

#if BLEND_WITH_CONST_ALPHA
    [material][a] property float flatAlpha = 1.0;
#endif

uniform sampler2D baseColorMap; // RGB - albedo color, A - alpha
uniform sampler2D baseNormalMap; // RG or AG(DXT5NM) or GA(ASTC) - normal
uniform sampler2D baseRMMap; // RG or AG(DXT5NM) or GA(ASTC) - roughness / metallic
uniform sampler2D miscMap; // RG or AG(DXT5NM) or GA(ASTC) - ambient occlusion / emissive
uniform sampler2D maskMap; // RG or AG(DXT5NM) or GA(ASTC) - decal mask / dirt mask

#if PBR_DECAL
    uniform sampler2D pbrDecalColorRoughnessMap; // RGB - albedo color, A - roughness
    uniform sampler2D pbrDecalLightmap; // RG or AG(DXT5NM) or GA(ASTC) - directional shadow / ambient occlusion
#endif

#if PBR_DETAIL
    uniform sampler2D pbrDetailColorMap; // RGB - albedo color
    uniform sampler2D pbrDetailNormalMap; // RG or AG(DXT5NM) or GA(ASTC) - normal
    uniform sampler2D pbrDetailRoughnessAOMap; // RG or AG(DXT5NM) or GA(ASTC) - roughness / ambient occlusion
#endif

[auto][a] property float4x4 invProjMatrix;
[auto][a] property float4x4 viewMatrix;
[auto][a] property float lightIntensity0;

[auto][a] property float pointLightIntensity0;
[auto][a] property float pointLightIntensity1;

[auto][a] property float3 cameraPosition;

[material][a] property float4 baseColorFactor = float4(1.0, 1.0, 1.0, 1.0);
[material][a] property float2 roughnessMetallicFactor = float2(1.0, 1.0);

#if AMBIENT_ATTENUATION_BOX
    [material][a] property float3 attenuationBoxPosition = float3(0,0,0);
    [material][a] property float3 attenuationBoxHalfSize = float3(0,0,0);
    [material][a] property float3 attenuationBoxSmoothness = float3(0,0,0);
#endif

#if LOD_TRANSITION
    [material][a] property float lodTransitionThreshold = 0.0;
#endif

#if EMISSIVE_COLOR
    [material][a] property float3 emissiveColor = float3(1.0, 1.0, 1.0);
#endif
#if TILED_DECAL_EMISSIVE_COLOR
    [material][a] property float3 tiledDecalEmissiveColor = float3(1.0, 1.0, 1.0);
#endif

#if TILED_DECAL_EMISSIVE_ALBEDO
    [material][a] property float tiledDecalEmissiveAlbedoFactor = 1.001;
#endif

#if EMISSIVE_ALBEDO
    [material][a] property float emissiveAlbedoFactor = 1.001;
#endif

fragment_out fp_main(fragment_in input)
{
    fragment_out output;

    float2 texCoord = input.varTexCoord0.xy;
    half4 baseColor = half4(tex2D(baseColorMap, texCoord));

    #if LOD_TRANSITION
        float2 xyNDC = input.projPos.xy / input.projPos.w;
        baseColor.a *= CalculateLodTransitionAlpha(xyNDC, half(lodTransitionThreshold));
    #endif

    #if ALPHATEST && !VIEW_MODE_OVERDRAW_HEAT
        half alpha = baseColor.a;

        #if VERTEX_COLOR
            alpha *= half(input.varVertexColor.a);
        #endif

        #if ALPHATESTVALUE
            if(alpha < half(alphatestThreshold)) discard;
        #else
            if(alpha < half(0.5)) discard;
        #endif
    #endif

    #if ALPHASTEPVALUE && ALPHABLEND
        baseColor.a = step(half(alphaStepValue), half(baseColor.a));
    #endif

    baseColor *= half4(baseColorFactor);

    half3 baseNormal = half3(half2(FP_SWIZZLE(tex2D(baseNormalMap, texCoord))), half(1.0));
    baseNormal.xy = baseNormal.xy * half(2.0) - half(1.0);
    baseNormal.z = sqrt(half(1.0) - saturate(dot(baseNormal.xy, baseNormal.xy)));

    half2 roughnessMetallic = half2(FP_SWIZZLE(tex2D(baseRMMap, texCoord)));

    half2 miscellaneous = half2(FP_SWIZZLE(tex2D(miscMap, texCoord)));
    half2 mask = half2(FP_SWIZZLE(tex2D(maskMap, texCoord)));
    half occlusion = miscellaneous.r;

    roughnessMetallic *= half2(roughnessMetallicFactor);

    #if PBR_DECAL
        half4 decalColorAndRoughness = half4(tex2D(pbrDecalColorRoughnessMap, input.varTexCoord1));

        baseColor.rgb *= decalColorAndRoughness.rgb * half(2.0);
        roughnessMetallic.x *= decalColorAndRoughness.a * half(2.0);
    #endif
    #if PBR_DETAIL
        half3 detailColor = half3(tex2D(pbrDetailColorMap, input.varTexCoord2).rgb);

        half3 detailNormal = half3(half2(FP_SWIZZLE(tex2D(pbrDetailNormalMap, input.varTexCoord2))), half(1.0));
        detailNormal.xy = detailNormal.xy * half(2.0) - half(1.0);
        detailNormal.z = sqrt(half(1.0) - saturate(dot(detailNormal.xy, detailNormal.xy)));

        half2 detailRoughnessAO = half2(FP_SWIZZLE(tex2D(pbrDetailRoughnessAOMap, input.varTexCoord2)));

        baseColor.rgb *= detailColor * half(2.0);
        baseNormal = NormalBlendUDN(detailNormal, baseNormal);
        roughnessMetallic.x *= detailRoughnessAO.x * half(2.0);
        occlusion *= detailRoughnessAO.y;
    #endif

    half3 N = baseNormal;
    half3 emission = half3(0.0, 0.0, 0.0);

    #if TILED_DECAL_MASK
        #if MULTIPLE_DECAL_TEXTURES
            half index = floor(input.index + 0.5);
        #else
            half index = half(0.0);
        #endif

        half4 decalColor = half4(tex2Darray(decalColorMap, input.varTexCoord0.zw, index));

        half fullMask = mask.r;
        #if TILED_DECAL_EMISSIVE_ALBEDO
            emission += decalColor.rgb * (mask.r * decalColor.a) * max(half(tiledDecalEmissiveAlbedoFactor), half(0.0));
        #elif TILED_DECAL_EMISSIVE_COLOR
            emission += mask.r * decalColor.a * half3(tiledDecalEmissiveColor);
        #elif !TILED_DECAL_ANIM_MASK
            half decalMask = mask.r;
            fullMask = decalColor.a * decalMask;
        #endif

        #if TILED_DECAL_ANIMATED_EMISSION
            half brightness = dot(decalColor.rgb, half3(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0)); // (r + g + b) / 3.0
            half selector = saturate(brightness * half(input.aniCamoParams.z) - half(input.aniCamoParams.x))
                           - saturate(brightness * half(input.aniCamoParams.z) - half(input.aniCamoParams.y));
            #if TILED_DECAL_ANIM_MASK
                half animMask = decalColor.a;
            #else
                half animMask = fullMask;
            #endif

            half3 animEmission = decalColor.rgb * (animMask * half(input.aniCamoParams.w) * selector);

            decalColor.rgb += animEmission; // sRGB animEmission
        #endif

        #if TILED_DECAL_SPREADING
            half spreading = half(1.0);
            #if TILED_DECAL_NOISE_SPREADING
                spreading *= GetNoiseBasedSpreading(half(spreadingProgress), input.varTexCoord0.zw);
            #endif
            #if TILED_DECAL_SPATIAL_SPREADING
                spreading *= GetSpatialBasedSpreading(half(spreadingProgress), input.localPos.xyz);
            #endif

            half boundary = half(1.0) - abs(half(0.5) - spreading) * half(2.0);
            emission += lerp(emission.rgb, half3(spreadingBorderColor * 2.0), boundary * fullMask);

            fullMask *= spreading;
        #endif

        decalColor.rgb = half3(SRGBToLinear(decalColor.r), SRGBToLinear(decalColor.g), SRGBToLinear(decalColor.b));
        baseColor.rgb = lerp(baseColor.rgb, decalColor.rgb, fullMask);

        #if TILED_DECAL_BLEND_NORMAL
            half3 decalNormal = half3(half2(FP_SWIZZLE(tex2Darray(decalNormalMap, input.varTexCoord0.zw, index))), half(1.0));
            decalNormal.xy = decalNormal.xy * half(2.0) - half(1.0);
            decalNormal.z = sqrt(half(1.0) - saturate(dot(decalNormal.xy, decalNormal.xy)));

            half3 blendedNormal = NormalBlendUDN(N, decalNormal);
            half blendFactor = decalNormalBlend * fullMask;

            // linear interpolation [0.0 .. 0.5 .. 1.0] -> [baseNormal .. blendedNormal .. decalNormal]
            N = lerp(N, blendedNormal, saturate(half(2.0) * blendFactor));
            N = lerp(N, decalNormal,   saturate(half(2.0) * (blendFactor - half(0.5))));
        #endif

        #if TILED_DECAL_OVERRIDE_ROUGHNESS_METALLIC
            half2 decalRoughnessMetallic = half2(FP_SWIZZLE(tex2Darray(decalRMMap, input.varTexCoord0.zw, index)));
            roughnessMetallic.xy = lerp(roughnessMetallic.xy, decalRoughnessMetallic, fullMask);
        #endif
    #endif

    #if DIRT_COVERAGE
        const half dirtMask = mask.g;
        const half dirtHeight = half(tex2D(dirtHeightMap, texCoord).g);

        half dirtFactor = half(1.0) - smoothstep(half(dirtStrength) - half(0.1), half(dirtStrength) + half(0.1), half(1.0) - dirtMask);
        dirtFactor += smoothstep(half(0.2), half(0.8), half(dirtStrength) * dirtHeight);
        dirtFactor = saturate(dirtFactor);

        half3 dirtNormal = half3(half2(FP_SWIZZLE(tex2D(dirtNormalMap, texCoord))), half(1.0));
        dirtNormal.xy = dirtNormal.xy * half(2.0) - half(1.0);
        dirtNormal.z = sqrt(half(1.0) - saturate(dot(dirtNormal.xy, dirtNormal.xy)));

        half3 linearDirtColor = half3(SRGBToLinear(half(dirtColor.r)), SRGBToLinear(half(dirtColor.g)), SRGBToLinear(half(dirtColor.b)));
        baseColor.rgb = lerp(baseColor.rgb, linearDirtColor.rgb, dirtFactor * half(dirtColor.a));
        roughnessMetallic.x = lerp(roughnessMetallic.x, half(dirtRoughness), dirtFactor);
        roughnessMetallic.y = lerp(roughnessMetallic.y, half(0.0), dirtFactor);

        const half3 blendedDirtNormal = NormalBlendUDN(N, dirtNormal);
        dirtNormal = normalize(lerp(blendedDirtNormal, dirtNormal, half(dirtStrength) * half(dirtStrength)));
        N = normalize(lerp(N, dirtNormal, dirtFactor));
    #endif

    #if WETNESS_MULTILEVEL || WETNESS_SIMPLIFIED
        #if WETNESS_MULTILEVEL
            const half wetnessStrength = CalculateWetnessStrength(input.localPos);
        #else
            const half wetnessStrength = half(simpleWetnessStrength);
        #endif

        const half porosity = saturate((roughnessMetallic.x - half(0.2)) / half(0.7 - 0.2));
        const half wetnessMultiplier = lerp(half(1.0), half(0.2), porosity * (half(1.0) - roughnessMetallic.y));
        const half specularFactor = roughnessMetallic.x * roughnessMetallic.x * wetnessMultiplier;
        const half roughnessFactor = lerp(half(1.0), specularFactor, wetnessStrength * half(0.5));

        baseColor.rgb *= lerp(half(1.0), wetnessMultiplier, wetnessStrength);
        roughnessMetallic.x = lerp(half(0.0), roughnessMetallic.x, roughnessFactor);
    #elif GLOBAL_PBR_TINT
        #if !IGNORE_BASE_COLOR_PBR_TINT
            baseColor.rgb *= half3(baseColorPbrTint) * half(2.0);
        #endif
        #if !IGNORE_ROUGHNESS_PBR_TINT
            roughnessMetallic.x *= half(roughnessPbrTint) * half(2.0);
        #endif
    #endif

    #if AMBIENT_ATTENUATION_BOX
        half3 ambientAttenuation = half3(1.0, 1.0, 1.0) - smoothstep(half3(attenuationBoxHalfSize) - half3(attenuationBoxSmoothness), half3(attenuationBoxHalfSize), abs(input.localPos.xyz - half3(attenuationBoxPosition)));

        occlusion *= half(1.0) - ambientAttenuation.x * ambientAttenuation.y * ambientAttenuation.z;
    #endif

    N.xy *= half(normalScale);

    N = normalize(half3(
        dot(N, input.tangentToWorld0),
        dot(N, input.tangentToWorld1),
        dot(N, input.tangentToWorld2)));

    half3 polygonN = normalize(half3(input.tangentToWorld0.z, input.tangentToWorld1.z, input.tangentToWorld2.z));

    #if PBR_LIGHTMAP || PBR_DECAL
        float2 directionalAndAO = float2(1.0, 1.0);
        #if PBR_LIGHTMAP
            directionalAndAO *= FP_SWIZZLE(tex2D(pbrLightmap, input.varTexCoord1));
        #endif
        #if PBR_DECAL
            directionalAndAO *= FP_SWIZZLE(tex2D(pbrDecalLightmap, input.varTexCoord1));
        #endif
        occlusion *= half(directionalAndAO.y);
    #endif

    // saturate for energy conservation
    baseColor = saturate(baseColor);
    roughnessMetallic = saturate(roughnessMetallic);
    occlusion = saturate(occlusion);

    #if EMISSIVE_ALBEDO
        emission += baseColor.rgb * miscellaneous.g * max(half(emissiveAlbedoFactor), half(0.0));
    #elif EMISSIVE_COLOR
        emission += miscellaneous.g * half3(emissiveColor);
    #endif

    half3 V = half3(normalize(cameraPosition - input.worldPos.xyz));

    half3 L = half3(lightPosition0.xyz);
    L.xyz = half3(mul(float4(float3(L.xyz), 0.0), invViewMatrix).xyz); // fix me to world space

    half4 shadowInfo = half4(1.0, 1.0, 1.0, 1.0);
    half shadow = 1.0f;
    #if RECEIVE_SHADOW
        half3 normalShady = baseNormal;

        normalShady.xy *= half(shadowLitNormalScale);
        normalShady = normalize(normalShady);

        half3 toLightNormalized = normalize(half3(input.varToLightVec.xyz));
        half NdotLShady = max(dot(normalShady, toLightNormalized), half(0.0));

        half3 worldNormal = half3(dot(input.tangentToWorld0.xyz, normalShady), dot(input.tangentToWorld1.xyz, normalShady), dot(input.tangentToWorld2.xyz, normalShady));

        shadowInfo = getCascadedShadow(input.worldPos, input.shadowPos, half4(input.projPos), worldNormal, NdotLShady);
        #if PBR_LIGHTMAP || PBR_DECAL
            shadow = lerp(half(directionalAndAO.x), shadowInfo.w, shadowInfo.z);
        #else
            shadow = shadowInfo.x;
        #endif

    #elif PBR_LIGHTMAP || PBR_DECAL
        shadow = half(directionalAndAO.x);
    #endif

    output.color.rgb = getPBR(polygonN, N, V, L, half3(lightColor0), half(lightIntensity0), baseColor.rgb, roughnessMetallic.y, roughnessMetallic.x, occlusion, shadow, emission);

    #if MAX_POINT_LIGHTS > 0
        float3 toPointLight = mul(float4(pointLights[0].xyz, 1.0), invViewMatrix).xyz - input.worldPos.xyz; // fix me to world space
        half distanceToLight = max(half(0.01), half(length(toPointLight.xyz)));
        half smoothEdgeScale = (half(1.0) - smoothstep(half(pointLights[0].w) * half(0.95), half(pointLights[0].w), distanceToLight));
        half attenuation = half(4.0 * _PI) / pow(distanceToLight, half(pointLights[2].w));

        L = half3(normalize(toPointLight));// fix me interpolate it from VS
        output.color.rgb += getPointLightPBR(N, V, L, half3(saturate(pointLights[2].xyz)), smoothEdgeScale * attenuation * pointLightIntensity0, baseColor.rgb, roughnessMetallic.y, roughnessMetallic.x);

        #if MAX_POINT_LIGHTS > 1
            toPointLight = mul(float4(pointLights[1].xyz, 1.0), invViewMatrix).xyz - input.worldPos.xyz; // fix me to world space
            distanceToLight = max(half(0.01), half(length(toPointLight.xyz)));
            smoothEdgeScale = (half(1.0) - smoothstep(half(pointLights[1].w) * half(0.95), half(pointLights[1].w), distanceToLight));
            attenuation = half(4.0 * _PI) / pow(distanceToLight, half(pointLights[3].w));

            L = half3(normalize(toPointLight));// fix me interpolate it from VS
            output.color.rgb += getPointLightPBR(N, V, L, half3(saturate(pointLights[3].xyz)), smoothEdgeScale * attenuation * pointLightIntensity1, baseColor.rgb, roughnessMetallic.y, roughnessMetallic.x);
        #endif

    #endif

    #if ALPHABLEND
        output.color.a = baseColor.a;
    #else
        output.color.a = half(1.0);
    #endif

    //half A = 0.22;
    //half B = 0.30;
    //half C = 0.10;
    //half D = 0.20;
    //half E = 0.01;
    //half F = 0.30;
    //half WP = 11.2;

    // Uncharted 2 tonemapping
    //output.color.rgb = ((output.color.rgb * (A * output.color.rgb + C * B) + D * E) / (output.color.rgb * (A * output.color.rgb + B) + D * F)) - E / F;
    //output.color.rgb /= ((WP * (A * WP + C * B) + D * E) / (WP * (A * WP + B) + D * F)) - E / F;
    //output.color.rgb = half3(LinearToSRGB(output.color.r), LinearToSRGB(output.color.g), LinearToSRGB(output.color.b));

    // Jim Hejl and Richard Burgess-Dawson tonemapping
    // include gamma conversion!!!
    //output.color.rgb = max(0.0, output.color.rgb - 0.004);
    //output.color.rgb = (output.color.rgb * (6.2 * output.color.rgb + 0.5)) / (output.color.rgb * (6.2 * output.color.rgb + 1.7) + 0.06);

    // Linear to sRGB conversion without tonemapping
    output.color.rgb = half3(LinearToSRGB(output.color.r), LinearToSRGB(output.color.g), LinearToSRGB(output.color.b));

    #if BLEND_WITH_CONST_ALPHA
        output.color.a = flatAlpha;
    #endif

    #if HIGHLIGHT_COLOR || HIGHLIGHT_WAVE_ANIM
        float4 highlightAnim;
        highlightAnim = ApplyHighlightAnimation(float4(output.color), input.worldPos.z);
        output.color = half4(highlightAnim);
    #endif

    #if USE_VERTEX_FOG
        half varFogAmoung = half(input.varFog.a);
        half3 varFogColor  = half3(input.varFog.rgb);
        output.color.rgb = lerp(output.color.rgb, varFogColor, varFogAmoung);
    #endif

    #include "debug-modify-color-half.slh"

    #if VIEW_ALBEDO && !VIEW_AMBIENT && !VIEW_DIFFUSE && !VIEW_SPECULAR
        output.color = half4(LinearToSRGB(baseColor.r), LinearToSRGB(baseColor.g), LinearToSRGB(baseColor.b), baseColor.a);
    #endif

    #if VIEW_ALL
        #if VIEW_NORMAL
            output.color = half4(baseNormal.xyz * half(0.5) + half(0.5), half(1.0));
        #endif

        #if VIEW_NORMAL_FINAL
            output.color = half4(N.xyz * half(0.5) + half(0.5), half(1.0));
        #endif

        #if VIEW_ROUGHNESS
            output.color = half4(roughnessMetallic.xxx, half(1.0));
        #endif

        #if VIEW_METALLIC
            output.color = half4(roughnessMetallic.yyy, half(1.0));
        #endif

        #if VIEW_AMBIENTOCCLUSION
            output.color = half4(occlusion, occlusion, occlusion, half(1.0));
        #endif
    #endif

    #if RECEIVE_SHADOW && DEBUG_SHADOW_CASCADES
        half3 shadowColor;
        shadowColor = getShadowColor(shadowInfo);
        output.color.rgb = shadowColor;
    #endif
    return output;
}
