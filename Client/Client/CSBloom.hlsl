#include "Common.hlsl"
RWTexture2D<float4> gtxtRWOutput : register(u0);

// --- 디더링 추가 시작 (Bayer Matrix 정의) ---
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
void CSBloom(uint3 n3DispatchThreadID : SV_DispatchThreadID,
             uint3 groupThreadID : SV_GroupThreadID,
             uint3 groupID : SV_GroupID)
{
    // ... (기존의 안개, 블룸 계산 코드는 그대로 둡니다) ...
    float4 finalColor = DFLightTexture[n3DispatchThreadID.xy];
    float4 positionW = DFPositionTexture[n3DispatchThreadID.xy];

    float3 vCameraPosition = gvCameraPosition.xyz;
    float3 vPostionToCamera = vCameraPosition - positionW.xyz;
    
    float fFogDensity = gvfFogInfo.z;
    float fDistanceToCamera = length(vPostionToCamera);
    float fFogFactor = saturate(exp(-fDistanceToCamera * fFogDensity));
    finalColor = lerp(gvFogColor, finalColor, fFogFactor);

    float4 blurredColor = float4(0.0f, 0.0f, 0.0f, 1.0f);

    int radius = lerp(15, 1, saturate(fDistanceToCamera / 35.0f));
    int sampleCount = 0;
    for(int x = -radius; x <= radius; x++)
    {
        for(int y = -radius; y <= radius; y++)
        {
            int2 samplePos = int2(n3DispatchThreadID.xy) + int2(x, y);
            samplePos = clamp(samplePos, int2(0, 0), int2(FRAME_BUFFER_WIDTH - 1, FRAME_BUFFER_HEIGHT - 1));
            blurredColor += DFTextureEmissive[samplePos];
            sampleCount++;
        }
    }

    if(sampleCount != 0)
        blurredColor /= sampleCount;
    if(length(blurredColor.xyz) < 1.f)
    {
        finalColor *= (1.0f - length(blurredColor.xyz));
        finalColor += blurredColor;
    }
    else
    {
        finalColor *= blurredColor * 2.0f;
    }

    // --- 디더링 추가 시작 (적용 부분) ---
    // 1. 현재 쓰레드의 화면 좌표를 가져옵니다.
    int2 screenPos = n3DispatchThreadID.xy;

    // 2. 화면 좌표를 이용해 8x8 매트릭스에서 사용할 인덱스를 구합니다.
    int ditherX = screenPos.x % 8;
    int ditherY = screenPos.y % 8;

    // 3. Bayer 매트릭스에서 값을 가져와 -0.5 ~ 0.5 범위로 정규화하고 8비트 단계에 맞게 스케일링합니다.
    float ditherValue = bayer_matrix_8x8[ditherY * 8 + ditherX];
    float ditherOffset = (ditherValue / 64.0 - 0.5) / 255.0;

    // 4. 최종 색상(RGB)에 디더링 오프셋을 더합니다.
    finalColor.rgb += ditherOffset;
    // --- 디더링 추가 끝 ---

    gtxtRWOutput[n3DispatchThreadID.xy] = finalColor;
}