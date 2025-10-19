#include "Common.hlsl"
Texture2D<float4> gInputTexture : register(t14);
RWTexture2D<float4> gOutputTexture : register(u0);

static float gGaussianWeights[16] =
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
void CSBilateralBlurVertical(uint3 n3DispatchThreadID : SV_DispatchThreadID)
{
    uint2 intCoord = n3DispatchThreadID.xy;
    
    float myDepth = DFzDepthTexture[intCoord].r;
    float3 myNormal = normalize(DFNormalTexture[intCoord].xyz * 2.0f - 1.0f);
    
    float4 blurredSSAO = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float fTotalWeight = 0.0f;
    int radius = 15;
    
    float fDepthThreshold = 0.1f;
    float fNormalThreshold = 0.95f;
    
    for(int y = -radius; y <= radius; y++)
    {
        int2 sampleCoord = intCoord + int2(0, y);
        sampleCoord = clamp(sampleCoord, int2(0, 0), int2(FRAME_BUFFER_WIDTH - 1, FRAME_BUFFER_HEIGHT - 1));

        // 2. "이웃" 픽셀의 뎁스와 노멀 값을 읽어옵니다.
        float neighborDepth = DFzDepthTexture[sampleCoord].r;
        float3 neighborNormal = normalize(DFNormalTexture[sampleCoord].xyz * 2.0f - 1.0f);
        float neighborSSAO = gInputTexture[sampleCoord].r;

        // 3. [핵심] 가중치 계산
        // 3-1. 공간 가중치 (기존 가우시안 블러)
        float spatialWeight = gGaussianWeights[abs(y)];

        // 3-2. 뎁스 가중치 (스마트!)
        // (나와 이웃의 뎁스 차이가 임계값(gDepthThreshold) 이내면 1.0, 아니면 0.0)
        float depthDiff = abs(myDepth - neighborDepth);
        float depthWeight = saturate(1.0 - smoothstep(0.0, fDepthThreshold, depthDiff));

        // 3-3. 노멀 가중치 (스마트!)
        // (나와 이웃의 노멀 내적 값이 임계값(gNormalThreshold) 이상이면 1.0, 아니면 0.0)
        float normalDot = dot(myNormal, neighborNormal);
        float normalWeight = saturate(smoothstep(fNormalThreshold, 1.0, normalDot));

        // 4. 모든 가중치를 곱합니다.
        // (하나라도 0이면(경계선 너머이면) 이 픽셀은 무시됩니다)
        float finalWeight = spatialWeight * depthWeight * normalWeight;

        // 5. 누적
        blurredSSAO.r += neighborSSAO * finalWeight;
        fTotalWeight += finalWeight;
    }
    
    if(fTotalWeight > 0.0f)
    {
        blurredSSAO.r /= fTotalWeight;
    }
    else
    {
        // 모든 이웃이 거부되면(예: 뾰족한 모서리) 원본 값을 그냥 씁니다.
        blurredSSAO.r = gInputTexture[intCoord].r;
    }
    
    gOutputTexture[n3DispatchThreadID.xy] = float4(blurredSSAO.r, blurredSSAO.r, blurredSSAO.r, 1.0f);
}