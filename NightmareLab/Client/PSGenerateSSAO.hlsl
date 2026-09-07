#include "Common.hlsl"

// SSAO sampling configuration.
static const int kSsaoSampleCount = 16;
static const float kMinimumSsaoRadius = 0.001f;
static const float kMinimumDepthBias = 0.001f;
static const float kEpsilon = 0.0001f;
static const uint2 kFrameBufferSize = uint2(FRAME_BUFFER_WIDTH, FRAME_BUFFER_HEIGHT);
static const uint2 kBlueNoiseTextureSize = uint2(128, 128);
static const float kTwoPi = 6.28318530718f;

// View-space normal을 중심으로 회전할 hemisphere 표본이다.
// +Z 성분을 사용하므로 TBN 변환 후 표면 바깥쪽 반구를 샘플링한다.
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

// Noise 방향을 normal 평면에 투영해 TBN의 tangent 축을 만든다.
float3 CreateSsaoTangent(float3 normalV, float3 randomVectorV)
{
	float3 tangentV = randomVectorV - normalV * dot(randomVectorV, normalV);

	// Noise와 normal이 거의 평행하면 안정적인 기준 축으로 다시 계산한다.
	if (dot(tangentV, tangentV) < kEpsilon)
	{
		float3 referenceAxis = abs(normalV.z) < 0.999f
			? float3(0.0f, 0.0f, 1.0f)
			: float3(0.0f, 1.0f, 0.0f);
		tangentV = cross(referenceAxis, normalV);
	}

	return normalize(tangentV);
}

// View-space 위치를 G-buffer를 조회할 texture UV로 투영한다.
float2 ProjectViewPositionToUv(float3 positionV)
{
	float4 positionH = mul(float4(positionV, 1.0f), gmtxProjection);
	float2 positionNdc = positionH.xy / positionH.w;

	// DirectX NDC의 +Y는 화면 위쪽이므로 texture V 좌표로 바꿀 때 반전한다.
	return positionNdc * float2(0.5f, -0.5f) + 0.5f;
}

float CalculateAmbientVisibility(uint2 pixelCoord, float3 positionV, float3 normalV)
{
	// 화면에 반복 배치한 blue-noise로 커널을 픽셀마다 회전시킨다.
	// 동일한 커널 패턴이 화면에 드러나는 현상을 줄이기 위한 과정이다.
	uint2 noiseCoord = pixelCoord % kBlueNoiseTextureSize;
	float noise = AlbedoTexture.Load(int3(noiseCoord, 0)).r;
	float noiseAngle = noise * kTwoPi;
	float3 randomVectorV = float3(cos(noiseAngle), sin(noiseAngle), 0.0f);

	// View-space normal을 기준으로 hemisphere kernel을 회전할 TBN 기저를 구성한다.
	float3 tangentV = CreateSsaoTangent(normalV, randomVectorV);
	float3 bitangentV = normalize(cross(normalV, tangentV));

	float sampleRadius = max(abs(gfScale), kMinimumSsaoRadius);
	float depthBias = max(abs(gfBias), kMinimumDepthBias);
	float occlusion = 0.0f;

	[unroll]
	for (int sampleIndex = 0; sampleIndex < kSsaoSampleCount; ++sampleIndex)
	{
		// 중심 근처에 표본이 더 많이 분포하도록 표본 거리를 제곱 비율로 조절한다.
		float3 kernelDirection = normalize(kSsaoKernel[sampleIndex]);
		float3 sampleDirectionV =
			tangentV * kernelDirection.x
			+ bitangentV * kernelDirection.y
			+ normalV * kernelDirection.z;
		float normalizedIndex = (float)sampleIndex / (float)(kSsaoSampleCount - 1);
		float sampleScale = lerp(0.1f, 1.0f, normalizedIndex * normalizedIndex);
		float3 expectedSamplePositionV = positionV + sampleDirectionV * sampleRadius * sampleScale;

		// 예상 표본 위치를 화면에 투영해 같은 위치의 G-buffer 값을 조회한다.
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

		// 실제 표면이 예상 표본보다 카메라에 가까우면 해당 방향이 가려진 것으로 판단한다.
		float3 sampledPositionV = mul(DFPositionTexture.Load(int3(samplePixelCoord, 0)), gmtxView).xyz;
		float depthDifference = abs(positionV.z - sampledPositionV.z);

		// 중심 표면과 지나치게 멀리 떨어진 깊이는 AO 기여도에서 제외한다.
		float rangeWeight = smoothstep(
			0.0f,
			1.0f,
			sampleRadius / max(depthDifference, kEpsilon)
		);

		// Left-handed view space에서는 더 작은 +Z가 카메라에 더 가까운 위치다.
		float isOccluded = sampledPositionV.z <= expectedSamplePositionV.z - depthBias ? 1.0f : 0.0f;
		occlusion += isOccluded * rangeWeight;
	}

	// Visibility 규약: 1.0은 차폐 없음, 0.0은 완전 차폐를 의미한다.
	occlusion /= (float)kSsaoSampleCount;
	return 1.0f - saturate(occlusion * max(gfIntesity, 0.0f));
}

struct PS_GENERATE_SSAO_INPUT
{
	float4 position : SV_POSITION;
};

// G-buffer로부터 Raw AO visibility texture를 생성한다.
float PSGenerateSSAO(PS_GENERATE_SSAO_INPUT input) : SV_TARGET
{
	uint2 pixelCoord = min(uint2(input.position.xy), kFrameBufferSize - 1);

	// 배경에는 AO를 적용하지 않으므로 완전 가시성인 1.0을 기록한다.
	float centerDepth = DFzDepthTexture.Load(int3(pixelCoord, 0));
	if (centerDepth >= 1.0f)
	{
		return 1.0f;
	}

	// G-buffer의 world-space 위치와 normal을 동일한 view space로 변환한다.
	float4 positionW = DFPositionTexture.Load(int3(pixelCoord, 0));
	float3 positionV = mul(positionW, gmtxView).xyz;

	float3 normalW = DFNormalTexture.Load(int3(pixelCoord, 0)).xyz;
	normalW = normalize(normalW * 2.0f - 1.0f);
	float3 normalV = normalize(mul(normalW, (float3x3) gmtxView));

	return CalculateAmbientVisibility(pixelCoord, positionV, normalV);
}
