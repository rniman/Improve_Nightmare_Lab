#include "Common.hlsl"
#include "Light.hlsl"

static const int kSsaoSampleCount = 16;
static const float kMinimumSsaoRadius = 0.001f;
static const float kMinimumDepthBias = 0.001f;
static const float kMinimumDifference = 0.0001f;
static const uint2 kFrameBufferSize = uint2(FRAME_BUFFER_WIDTH, FRAME_BUFFER_HEIGHT);
static const uint2 kBlueNoiseTextureSize = uint2(128, 128);
static const float kTwoPi = 6.28318530718f;

// View-space normal을 기준으로 회전할 hemisphere 샘플 방향이다.
// 샘플별 반경은 루프에서 별도로 조절해 중심 부근에 더 많은 샘플을 배치한다.
static const float3 kSsaoKernel[kSsaoSampleCount] =
{
	float3(0.5381f, 0.1856f, 0.8210f),
	float3(-0.5123f, -0.8361f, 0.1965f),
	float3(0.0689f, -0.8169f, 0.5727f),
	float3(0.9251f, 0.2164f, 0.3121f),
	float3(-0.1379f, 0.2486f, 0.9587f),
	float3(0.8844f, 0.4544f, 0.1102f),
	float3(-0.4920f, 0.7136f, 0.4981f),
	float3(-0.1702f, -0.9285f, 0.3300f),
	float3(-0.6999f, -0.0451f, 0.7128f),
	float3(-0.2895f, 0.9535f, 0.0831f),
	float3(0.6254f, -0.4452f, 0.6410f),
	float3(-0.9482f, 0.1552f, 0.2768f),
	float3(0.3371f, 0.5679f, 0.7510f),
	float3(0.7198f, -0.6617f, 0.2101f),
	float3(-0.8412f, -0.3490f, 0.4131f),
	float3(0.3208f, 0.8881f, 0.3291f)
};

float3 CreateSsaoTangent(float3 normalV, float3 randomVectorV)
{
	float3 tangentV = randomVectorV - normalV * dot(randomVectorV, normalV);
	float tangentLengthSquared = dot(tangentV, tangentV);

	// 노이즈 벡터가 normal과 거의 평행하면 안정적인 축으로 tangent를 다시 만든다.
	if (tangentLengthSquared < kMinimumDifference)
	{
		float3 referenceAxis = abs(normalV.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(0.0f, 1.0f, 0.0f);
		tangentV = cross(referenceAxis, normalV);
	}

	return normalize(tangentV);
}

float2 ProjectViewPositionToUv(float3 positionV)
{
	float4 positionH = mul(float4(positionV, 1.0f), gmtxProjection);
	float2 positionNdc = positionH.xy / positionH.w;

	// DirectX NDC의 +Y는 화면 위쪽이므로 texture V 좌표로 변환할 때 뒤집는다.
	return positionNdc * float2(0.5f, -0.5f) + 0.5f;
}

float CalculateAmbientVisibility(uint2 pixelCoord, float3 positionV, float3 normalV)
{
	uint2 noiseCoord = pixelCoord % kBlueNoiseTextureSize;
	float noise = AlbedoTexture.Load(int3(noiseCoord, 0)).r;
	float noiseAngle = noise * kTwoPi;
	float3 randomVectorV = float3(cos(noiseAngle), sin(noiseAngle), 0.0f);

	float3 tangentV = CreateSsaoTangent(normalV, randomVectorV);
	float3 bitangentV = normalize(cross(normalV, tangentV));

	float sampleRadius = max(abs(gfScale), kMinimumSsaoRadius);
	float depthBias = max(abs(gfBias), kMinimumDepthBias);
	float occlusion = 0.0f;

	[unroll(kSsaoSampleCount)]
	for (int sampleIndex = 0; sampleIndex < kSsaoSampleCount; ++sampleIndex)
	{
		float3 kernelDirection = normalize(kSsaoKernel[sampleIndex]);
		float3 sampleDirectionV =
			tangentV * kernelDirection.x
			+ bitangentV * kernelDirection.y
			+ normalV * kernelDirection.z;

		float normalizedIndex = (float) sampleIndex / (float) (kSsaoSampleCount - 1);
		float sampleScale = lerp(0.1f, 1.0f, normalizedIndex * normalizedIndex);
		float3 expectedSamplePositionV = positionV + sampleDirectionV * sampleRadius * sampleScale;

		float2 sampleUv = ProjectViewPositionToUv(expectedSamplePositionV);
		if (any(sampleUv <= 0.0f) || any(sampleUv >= 1.0f))
		{
			continue;
		}

		uint2 samplePixelCoord = min(uint2(sampleUv * kFrameBufferSize), kFrameBufferSize - 1);
		float sampledDepth = DFzDepthTexture.Load(int3(samplePixelCoord, 0));
		if (sampledDepth >= 1.0f)
		{
			continue;
		}

		float4 sampledPositionW = DFPositionTexture.Load(int3(samplePixelCoord, 0));
		float3 sampledPositionV = mul(sampledPositionW, gmtxView).xyz;
		float depthDifference = abs(positionV.z - sampledPositionV.z);

		float rangeWeight = smoothstep(
			0.0f,
			1.0f,
			sampleRadius / max(depthDifference, kMinimumDifference
		));

		// Left-handed view space에서는 더 작은 +Z가 카메라에 더 가까운 위치다.
		float isOccluded = sampledPositionV.z <= expectedSamplePositionV.z - depthBias
			? 1.0f
			: 0.0f;

		occlusion += isOccluded * rangeWeight;
	}

	occlusion /= (float) kSsaoSampleCount;
	return 1.0f - saturate(occlusion * max(gfIntesity, 0.0f));
}

float4 PSPostProcessingWithSSAO(PS_POSTPROCESSING_OUT input) : SV_Target
{
	uint2 pixelCoord = min(uint2(input.position.xy), kFrameBufferSize - 1);
	float4 cColor = DFTextureTexture.Load(int3(pixelCoord, 0));
	float4 cEmissiveColor = DFTextureEmissive.Load(int3(pixelCoord, 0));
	float centerDepth = DFzDepthTexture.Load(int3(pixelCoord, 0));

	float4 positionW = DFPositionTexture.Load(int3(pixelCoord, 0));
	float3 positionV = mul(positionW, gmtxView).xyz;

	float3 normalW = DFNormalTexture.Load(int3(pixelCoord, 0)).xyz;
	normalW = normalize(normalW * 2.0f - 1.0f);
	float3 normalV = normalize(mul(normalW, (float3x3) gmtxView));

	// 지오메트리가 없는 배경 픽셀에는 AO를 적용하지 않는다.
	float ambientVisibility = centerDepth < 1.0f
		? CalculateAmbientVisibility(pixelCoord, positionV, normalV)
		: 1.0f;

	float4 light = Lighting(positionW.xyz, normalW, ambientVisibility);
	cColor *= light;
	cColor += cEmissiveColor;

	return cColor;
}
