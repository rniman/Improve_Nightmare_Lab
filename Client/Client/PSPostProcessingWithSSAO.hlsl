#include "Common.hlsl"
#include "Light.hlsl"

Texture2D<float4> gInputTexture : register(t14);

float4 PSPostProcessingWithSSAO(PS_POSTPROCESSING_OUT input) : SV_Target
{   
    float4 cColor = DFTextureTexture.Sample(gssWrap, input.uv);
    float4 cEmissiveColor = DFTextureEmissive.Sample(gssWrap, input.uv);
           
    float4 positionW = DFPositionTexture.Sample(gssWrap, input.uv);
    
    float3 normal = DFNormalTexture.Sample(gssWrap, input.uv).xyz;
    normal = normalize((normal * 2.0f) - 1.0f);
           
    float fSSAO = gInputTexture.Sample(gssWrap, input.uv);
    
    //**Light Calculation**//
    float4 light = Lighting(positionW.xyz, normal, float4(fSSAO, fSSAO, fSSAO, 1.0f));
    cColor = cColor * light; // SSAO와 라이트를 곱하여 최종 색상 계산
    
    cColor += cEmissiveColor;
            
    return cColor;
}