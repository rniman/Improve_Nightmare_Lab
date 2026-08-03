#include "Common.hlsl"
RWTexture2D<float4> gtxtRWOutput : register(u0);

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
void CSBloomOff(uint3 n3DispatchThreadID : SV_DispatchThreadID)
{
	float4 finalColor = DFLightTexture[n3DispatchThreadID.xy];
	float4 positionW = DFPositionTexture[n3DispatchThreadID.xy];

	float3 vCameraPosition = gvCameraPosition.xyz;
	float3 vPostionToCamera = vCameraPosition - positionW.xyz;
	float fDistanceToCamera = length(vPostionToCamera);
  
	float fFogDensity = gvfFogInfo.z;
	float fFogFactor = saturate(exp(-fDistanceToCamera * fFogDensity));
	finalColor = lerp(gvFogColor, finalColor, fFogFactor);
	
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
	
	gtxtRWOutput[n3DispatchThreadID.xy] = finalColor;
}
