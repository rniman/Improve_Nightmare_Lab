//================================================================================
// 최종 씬 합성 컴퓨트 셰이더 (CSComposite)
//
// 이 셰이더는 후처리(Post-Processing)의 마지막 단계로,
// 여러 버퍼의 데이터를 조합하여 최종 화면을 생성합니다.
//
// 수행 작업:
// 1. G-Buffer (조명, 위치) 로드
// 2. 안개(Fog) 계산 및 적용
// 3. 블룸(Emissive Blur) 텍스처 로드
// 4. 블룸을 씬에 합성 (Additive Blending)
// 5. 디더링(Dithering) 적용
// 6. 최종 결과를 출력 텍스처에 씀
//================================================================================

#include "Common.hlsl"

// 입력 리소스
// t14: 이전 블러 패스(가로/세로)의 최종 결과 (블룸 텍스처)
Texture2D<float4> gInputTexture : register(t14);

// 출력 리소스
// u0: 최종 씬 결과를 쓸 RWTexture (UAV)
RWTexture2D<float4> gOutputTexture : register(u0);

// 상수
// 8x8 베이어(Bayer) 매트릭스 (디더링용)
static const float g_mBayerMatrix8x8[64] =
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
    uint2 vPixelCoord = n3DispatchThreadID.xy;

    // 1. G-Buffer 및 조명 결과 로드
    // DFLightTexture: 조명 계산이 완료된 씬(Scene) 컬러
    // DFPositionTexture: 월드 공간 위치
    float4 vFinalColor = DFLightTexture[vPixelCoord];
    float4 vPositionW = DFPositionTexture[vPixelCoord];
    
    // 2. 안개(Fog) 계산 및 적용
    // (지수 안개 - Exponential Fog 기준)
    float3 vCameraPos = gvCameraPosition.xyz;
    float3 vPixelToCamera = vCameraPos - vPositionW.xyz;
    float fDistanceToCamera = length(vPixelToCamera);
 
    float fFogDensity = gvfFogInfo.z;
    
    // fFogFactor: 1.0 = 안개 없음(카메라와 가까움), 0.0 = 안개로 완전히 덮임
    float fFogFactor = saturate(exp(-fDistanceToCamera * fFogDensity));
    
    vFinalColor = lerp(gvFogColor, vFinalColor, fFogFactor);
    
    float4 vBloomColor = gInputTexture[vPixelCoord];

    // 블룸 합성 (커스텀)
    float fBloomIntensity = length(vBloomColor.xyz);
    if(fBloomIntensity < 1.f)
    {
        // 블룸이 약할 때: 원본 씬의 밝기를 블룸 강도만큼 감소시킨 후, 블룸을 더합니다.
        vFinalColor *= (1.0f - fBloomIntensity);
        vFinalColor += vBloomColor;
    }
    else
    {
        // 블룸이 강할 때: 원본 씬에 블룸 색상(증폭)을 곱합니다.
        vFinalColor *= vBloomColor * 2.0f;
    }
    
    // 디더링(Dithering) 적용
    // 8비트(LDR) 출력 환경에서 발생할 수 있는 컬러 밴딩(계단 현상)을 완화합니다.
    int ditherX = vPixelCoord.x % 8;
    int ditherY = vPixelCoord.y % 8;
    
    float fDitherValue = g_mBayerMatrix8x8[ditherY * 8 + ditherX];
    float fDitherNorm = (fDitherValue / 64.0f) - 0.5f;
    
    // 8비트(1/255) 컬러 스텝보다 작은 값으로 스케일링하여 미세한 노이즈 추가
    float fDitherOffset = fDitherNorm / 255.0f;
    
    vFinalColor.rgb += fDitherOffset;
    
    // 6. 최종 출력
    gOutputTexture[vPixelCoord] = vFinalColor;
}