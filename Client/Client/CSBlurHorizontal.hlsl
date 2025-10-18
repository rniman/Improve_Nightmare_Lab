#include "Common.hlsl"
// 1차 블러 패스의 결과
Texture2D<float4> gInputTexture : register(t14);
// 2차 블러 패스의 출력
RWTexture2D<float4> gOutputTexture : register(u0);

static float weight[16] =
{
    0.055,
    0.055,
    0.053,
    0.051,
    0.048,
    0.044,
    0.04,
    0.036,
    0.031,
    0.027,
    0.023,
    0.019,
    0.015,
    0.012,
    0.01,
    0.007,
};

[numthreads(32, 32, 1)]
void CSBlurHorizontal(uint3 n3DispatchThreadID : SV_DispatchThreadID)
{
    float4 blurredColor = float4(0.0f, 0.0f, 0.0f, 0.0f);
    
    const int radius = 15;
        
    for(int y = -radius; y <= radius; y++)
    {
        int2 samplePos = int2(n3DispatchThreadID.xy) + int2(0, y);
        samplePos = clamp(samplePos, int2(0, 0), int2(FRAME_BUFFER_WIDTH - 1, FRAME_BUFFER_HEIGHT - 1));

        blurredColor += DFTextureEmissive[samplePos] * weight[abs(y)];
    }
              
    gOutputTexture[n3DispatchThreadID.xy] = blurredColor;
}