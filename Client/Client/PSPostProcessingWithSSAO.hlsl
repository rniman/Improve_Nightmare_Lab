#include "Common.hlsl"
#include "Light.hlsl"

float doAmbientOcclusion(float2 tcoord, float2 uv, float3 p, float3 cnorm)
{
	// 샘플링 위치 계산
	float3 samplePos = mul(DFPositionTexture.Sample(gssWrap, tcoord + uv), gmtxView).xyz;
	float3 diff = samplePos - p;
	float3 v = normalize(diff);
	float d = length(diff) * gfScale;

	// Ambient Occlusion 값 반환
	// 법선 벡터 cnorm과 방향 벡터 v의 내적 값에서 바이어스를 뺀 값을 사용하여 occlusion 값을 계산
	// 거리 d에 따라 1 / (1 + d)로 감쇠를 적용하고 강도 값 intesity를 곱하여 최종 occlusion 값을 반환
	return max(0.0, dot(cnorm, v) - gfBias) * (1.0 / (1.0 + d)) * gfIntesity;
}

// SSAO에서 사용할 고정 샘플 방향 배열이다.
static const float2 kSampleDirections[16] =
{
	float2(1, 0), float2(-1, 0), float2(0, 1), float2(0, -1),
	float2(0.707, 0.707), float2(-0.707, -0.707), float2(-0.707, 0.707), float2(0.707, -0.707),
	normalize(float2(0.25, 0.75)), normalize(float2(0.25, -0.75)), normalize(float2(-0.25, -0.75)), normalize(float2(-0.25, 0.75)),
	normalize(float2(0.75, 0.25)), normalize(float2(0.75, -0.25)), normalize(float2(-0.75, -0.25)), normalize(float2(-0.75, 0.25))
};

#include "NoiseData.hlsl"

float4 PSPostProcessingWithSSAO(PS_POSTPROCESSING_OUT input) : SV_Target
{
	float4 cColor = DFTextureTexture.Sample(gssWrap, input.uv);
	float4 cEmissiveColor = DFTextureEmissive.Sample(gssWrap, input.uv);

	float4 positionW = DFPositionTexture.Sample(gssWrap, input.uv);
	float4 positionV = mul(positionW, gmtxView); // 뷰 공간 위치

	float3 normal = DFNormalTexture.Sample(gssWrap, input.uv).xyz;
	normal = normalize((normal * 2.0f) - 1.0f);

	float3x3 viewMatrixRotation = (float3x3) gmtxView;
	float3 normalV = normalize(mul(normal, viewMatrixRotation)); // 뷰 공간 노말

	// 화면 픽셀 좌표를 8x8 noise 테이블에 반복 매핑한다.
	// noiseIndex는 항상 유효 범위인 0~63을 유지한다.
	uint2 pixelCoord = uint2(input.uv * float2(FRAME_BUFFER_WIDTH, FRAME_BUFFER_HEIGHT));
	uint2 noiseCoord = pixelCoord % kNoiseDimension;
	uint noiseIndex = noiseCoord.x + noiseCoord.y * kNoiseDimension;
	// Noise 값으로 샘플 방향을 회전해 방향 편향을 줄인다.
	float2 rand = normalize(kNoise[noiseIndex].xy * 2.0f - 1.0f);
	float rad = 0.5f / positionV.z;

	// SSAO 누적: 방향마다 4개 반경으로 주변 지오메트리를 샘플링한다.
	float ssao = 0.0f;
	int numSamples = 16;
	[unroll(numSamples)]
	for (int j = 0; j < numSamples; ++j)
	{
		float2 coord1 = reflect(kSampleDirections[j], rand) * rad;
		float2 coord2 = float2(coord1.x * 0.707 - coord1.y * 0.707, coord1.x * 0.707 + coord1.y * 0.707);
		ssao += doAmbientOcclusion(input.uv, coord1 * 0.25, positionV.xyz, normalV);
		ssao += doAmbientOcclusion(input.uv, coord2 * 0.5, positionV.xyz, normalV);
		ssao += doAmbientOcclusion(input.uv, coord1 * 0.75, positionV.xyz, normalV);
		ssao += doAmbientOcclusion(input.uv, coord2, positionV.xyz, normalV);
	}
	ssao /= (float) numSamples * 4.0;
	ssao = ssao * 0.15f;

	//**Light Calculation**//
	float4 light = Lighting(positionW.xyz, normal, float4(ssao, ssao, ssao, 1.0f));
	cColor = cColor * light; // SSAO와 라이트를 곱하여 최종 색상 계산

	cColor += cEmissiveColor;

	//**FOG Calculation**//
	//float3 vCameraPosition = gvCameraPosition.xyz;
	//float3 vPostionToCamera = vCameraPosition - positionW.xyz;
	//float fDistanceToCamera = length(vPostionToCamera);
	//float fFogFactor = saturate(1.0f / pow(gvfFogInfo.y + gvfFogInfo.x, pow(fDistanceToCamera * gvfFogInfo.z, 2))) * gvfFogInfo.w;
	//cColor = lerp(gvFogColor, cColor, fFogFactor);

	return cColor;
}
