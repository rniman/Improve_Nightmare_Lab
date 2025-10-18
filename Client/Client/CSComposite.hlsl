#include "Common.hlsl"
// 1차 블러 패스의 결과
Texture2D<float4> gInputTexture : register(t14);
// 2차 블러 패스의 출력
RWTexture2D<float4> gOutputTexture : register(u0);

static const float bayer_matrix_8x8[64] =
{
    0, 32, 8, 40, 2, 34, 10, 42,
    48, 16, 56, 24, 50, 18, 58, 26,
    12, 44, 4, 36, 14, 46, 6, 38,
    60, 28, 52, 20, 62, 30, 54, 22,
     3, 35, 11, 43, 1, 33, 9, 41,
    51, 19, 59, 27, 49, 17, 57, 25,
    15, 47, 7, 39, 13, 45, 5, 37,
    63, 31, 55, 23, 61, 29, 53, 21
};

[numthreads(32, 32, 1)]
void CSComposite(uint3 n3DispatchThreadID : SV_DispatchThreadID)
{
    // 1. 씬 컬러와 포지션 로드
    float4 finalColor = DFLightTexture[n3DispatchThreadID.xy];
    float4 positionW = DFPositionTexture[n3DispatchThreadID.xy];
    
    // 2. 안개 계산 및 적용
    float3 vCameraPosition = gvCameraPosition.xyz;
    float3 vPostionToCamera = vCameraPosition - positionW.xyz;
    float fDistanceToCamera = length(vPostionToCamera);
    
    float fFogDensity = gvfFogInfo.z;
    float fFogFactor = saturate(exp(-fDistanceToCamera * fFogDensity));
    finalColor = lerp(gvFogColor, finalColor, fFogFactor);
    
    // 3. 최종 블러 결과물 로드 (계산 X)
    float4 blurredColor = gInputTexture[n3DispatchThreadID.xy];

    // 4. 블룸 합성
    if(length(blurredColor.xyz) < 1.f)
    {
        finalColor *= (1.0f - length(blurredColor.xyz));
        finalColor += blurredColor;
    }
    else
    {
        finalColor *= blurredColor * 2.0f;
    }
    
    // 5. 디더링
    int2 screenPos = n3DispatchThreadID.xy;
    int ditherX = screenPos.x % 8;
    int ditherY = screenPos.y % 8;
    float ditherValue = bayer_matrix_8x8[ditherY * 8 + ditherX];
    float ditherOffset = (ditherValue / 64.0 - 0.5) / 255.0;
    finalColor.rgb += ditherOffset;
        
    // 6. 최종 출력
    gOutputTexture[n3DispatchThreadID.xy] = finalColor;
}