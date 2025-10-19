#include "Common.hlsl"
// uav 출력
RWTexture2D<float4> gOutputTexture : register(u0);

float doAmbientOcclusion(float2 tcoord, float2 uv, float3 p, float3 cnorm)
{
    // [수정] G-Buffer에서 읽도록 gPositionTexture로 변경, .Sample() 사용
    float3 samplePos = mul(DFPositionTexture.SampleLevel(gssWrap, tcoord + uv, 0.0f), gmtxView).xyz;
    float3 diff = samplePos - p;
    float3 v = normalize(diff);
    float d = length(diff) * gfScale;
    
    // 샘플링 위치 계산
    //float3 samplePos = mul(DFPositionTexture[tcoord + uv], gmtxView).xyz;
    //float3 diff = samplePos - p;
    //float3 v = normalize(diff);
    //float d = length(diff) * gfScale;
    
    // Ambient Occlusion 값 반환
    // 법선 벡터 cnorm과 방향 벡터 v의 내적 값에서 바이어스를 뺀 값을 사용하여 occlusion 값을 계산
    // 거리 d에 따라 1 / (1 + d)로 감쇠를 적용하고 강도 값 intesity를 곱하여 최종 occlusion 값을 반환
    return max(0.0, dot(cnorm, v) - gfBias) * (1.0 / (1.0 + d)) * gfIntesity;
}

// 샘플링 벡터 배열
static const float2 vec[16] =
{
    float2(1, 0), float2(-1, 0), float2(0, 1), float2(0, -1),
    float2(0.707, 0.707), float2(-0.707, -0.707), float2(-0.707, 0.707), float2(0.707, -0.707),
    normalize(float2(0.25, 0.75)), normalize(float2(0.25, -0.75)), normalize(float2(-0.25, -0.75)), normalize(float2(-0.25, 0.75)),
    normalize(float2(0.75, 0.25)), normalize(float2(0.75, -0.25)), normalize(float2(-0.75, -0.25)), normalize(float2(-0.75, 0.25))
};

#include "NoiseData.hlsl" // float3 noise[8 * 8] 

[numthreads(32, 32, 1)]
void CSSSAOProcessing(uint3 n3DispatchThreadID : SV_DispatchThreadID)
{
    uint2 intCoord = n3DispatchThreadID.xy;
    float2 vFrameBuffer = float2((float) FRAME_BUFFER_WIDTH, (float) FRAME_BUFFER_HEIGHT);
    float2 inputUv = (float2(intCoord) + 0.5f) / vFrameBuffer; // PS의 'input.uv'와 동일
                  
    float4 positionW = DFPositionTexture[intCoord];
    float4 positionV = mul(positionW, gmtxView); // 뷰 공간 위치
    
    float3 normal = DFNormalTexture[intCoord].xyz;
    normal = normalize((normal * 2.0f) - 1.0f);
    
    float3x3 viewMatrixRotation = (float3x3) gmtxView;
    float3 normalV = normalize(mul(normal, viewMatrixRotation)); // 뷰 공간 노말
        
    // 노이즈 텍스쳐 UV
    float fFractTime = frac(time); // (frac(time)은 노이즈를 흔들기 위한 용도, 여기서는 단순 타일링으로 수정)
   
    //uint noiseX = uint(intCoord.x) % 8; // 0~7
    //uint noiseY = uint(intCoord.y) % 8; // 0~7
    //int noiseIndex = noiseX + noiseY * 8; // 0~63
    
    // (만약 시간으로 흔들고 싶다면)
    int noiseX = (int(intCoord.x + fFractTime * 10.0f) % 8);
    int noiseY = (int(intCoord.y + fFractTime * 10.0f) % 8);
    int noiseIndex = noiseX + noiseY * 8;
    
    float2 rand = normalize(noise[noiseIndex].xy * 2.0f - 1.0f);
    
    float ssao = 0.0f;
    float rad = 0.5f / max(0.001f, positionV.z); // [수정] 0으로 나누기 방지
    
    //**SSAO Calculation**//
    int numSamples = 8;
    [unroll(numSamples)]
    for(int j = 0; j < numSamples; ++j)
    {
        float2 coord1 = reflect(vec[j], rand) * rad;
        
        // 방향당 1번만 샘플링 (0.5는 중간 거리 예시)
        //ssao += doAmbientOcclusion(inputUv, coord1 * 0.5, positionV.xyz, normalV);
        
        //float2 coord1 = reflect(vec[j], rand) * rad;
        float2 coord2 = float2(coord1.x * 0.707 - coord1.y * 0.707, coord1.x * 0.707 + coord1.y * 0.707);
        
        //// doAmbientOcclusion에는 'input_uv' (float)를 넘겨줍니다.
        ssao += doAmbientOcclusion(inputUv, coord1 * 0.25, positionV.xyz, normalV);
        //ssao += doAmbientOcclusion(inputUv, coord2 * 0.5, positionV.xyz, normalV);
        ssao += doAmbientOcclusion(inputUv, coord1 * 0.75, positionV.xyz, normalV);
        //ssao += doAmbientOcclusion(inputUv, coord2, positionV.xyz, normalV);
    }
    ssao /= (float) numSamples * 2.0;
    ssao = ssao * 0.15f;
        
    gOutputTexture[n3DispatchThreadID.xy] = float4(ssao, ssao, ssao, 1.0f);
}