#include "Common.hlsl"
groupshared float3 g_posVCache[32][32];
groupshared float3 g_normalVCache[32][32];

RWTexture2D<float4> gOutputTexture : register(u0);

float doAmbientOcclusion(
    float2 tcoord, // Current pixel UV (0-1)
    float2 uv, // Sample UV offset (e.g., coord1 * 0.75)
    float3 p, // Current pixel view-space position (from cache)
    float3 cnorm, // Current pixel view-space normal (from cache)
    uint2 groupCoord, // Current pixel's group-local coord (0-31)
    float2 vFrameBuffer // Screen dimensions
)
{
    float2 sampleUV = tcoord + uv;

    // 1. 샘플의 좌표가 32x32 그룹 내에 있는지 계산
    // (픽셀 단위로 변환)
    int2 relativeOffset = int2(round(uv * vFrameBuffer));
    int2 cacheCoord = int2(groupCoord) + relativeOffset;

    float3 samplePosV; // 샘플 위치

    // 2. 캐시 히트(Cache Hit) 검사
    if(cacheCoord.x >= 0 && cacheCoord.x < 32 && cacheCoord.y >= 0 && cacheCoord.y < 32)
    {
        // 3. [초고속 경로] 샘플이 우리 캐시 안에 있음!
        samplePosV = g_posVCache[cacheCoord.y][cacheCoord.x];
    }
    else
    {
        // 4. [느린 경로] 샘플이 캐시 밖(다른 그룹)에 있음.
        // 어쩔 수 없이 VRAM(비디오 메모리)에서 읽어옴 (Fallback)
        samplePosV = mul(DFPositionTexture.SampleLevel(gssWrap, sampleUV, 0.0f), gmtxView).xyz;
    }

    // 5. 캐시에서 읽었든 VRAM에서 읽었든, AO 계산 로직은 동일
    float3 diff = samplePosV - p;
    float3 v = normalize(diff);
    float d = length(diff) * gfScale;
    
    return max(0.0, dot(cnorm, v) - gfBias) * (1.0 / (1.0 + d)) * gfIntesity;
}

// ---- 개선된 샘플링 패턴 (Spiral 분포) ----
float2 GetSpiralSample(int index, int sampleCount, float2 rand)
{
    float angle = (float) index / (float) sampleCount * 6.28318f; // 2*PI
    float radius = (float) index / (float) sampleCount;
    radius = sqrt(radius); // 균등 분포를 위한 제곱근
    
    float2 offset = float2(cos(angle), sin(angle)) * radius;
    
    // 랜덤 회전 적용
    float2x2 rotation = float2x2(
        rand.x, -rand.y,
        rand.y, rand.x
    );
    
    return mul(offset, rotation);
}

#include "NoiseData.hlsl" // float3 noise[8 * 8] 

[numthreads(32, 32, 1)]
void CSSSAOProcessing(uint3 n3DispatchThreadID : SV_DispatchThreadID, uint3 n3GroupThreadID : SV_GroupThreadID)
{
    uint2 intCoord = n3DispatchThreadID.xy;
    uint2 groupCoord = n3GroupThreadID.xy; // 그룹 내 좌표 (0~31)
    
    float4 positionW = DFPositionTexture[intCoord];
    float4 positionV = mul(positionW, gmtxView);
    float3 normal = DFNormalTexture[intCoord].xyz;
    normal = normalize((normal * 2.0f) - 1.0f);
    float3x3 viewMatrixRotation = (float3x3) gmtxView;
    float3 normalV = normalize(mul(normal, viewMatrixRotation));

    // 초고속 캐시에 저장
    g_posVCache[groupCoord.y][groupCoord.x] = positionV.xyz;
    g_normalVCache[groupCoord.y][groupCoord.x] = normalV;

    // [!!!] 2단계: 동기화 [!!!]
    // 그룹 내 모든 쓰레드가 캐시 쓰기를 마칠 때까지 대기
    GroupMemoryBarrierWithGroupSync();
    
    float2 vFrameBuffer = float2((float) FRAME_BUFFER_WIDTH, (float) FRAME_BUFFER_HEIGHT);
    float2 inputUv = (float2(intCoord) + 0.5f) / vFrameBuffer; // PS의 'input.uv'와 동일
                          
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
    int numSamples = 16;
    [unroll(numSamples)]
    for(int i = 0; i < numSamples; ++i)
    {
        //float2 coord1 = reflect(vec[j], rand) * rad;
        // 1. [교체] vec[j]와 reflect 대신 GetSpiralSample 호출
        // 이 함수가 이미 [-1, 1] 범위의 랜덤 회전된 오프셋을 줍니다.
        float2 sampleOffset = GetSpiralSample(i, numSamples, rand);
        
        // 2. [수정] adaptive radius (rad) 적용
        // GetSpiralSample은 0~1 범위의 radius를 반환하므로, 
        // 여기에 우리가 계산한 rad를 곱해 최종 오프셋을 만듭니다.
        sampleOffset *= rad;

        ssao += doAmbientOcclusion(inputUv, sampleOffset, g_posVCache[groupCoord.y][groupCoord.x], g_normalVCache[groupCoord.y][groupCoord.x], groupCoord, vFrameBuffer);
    }
    ssao /= (float) numSamples * 1.0;
    ssao = ssao * 0.2f;
        
    gOutputTexture[n3DispatchThreadID.xy] = float4(ssao, ssao, ssao, 1.0f);
}