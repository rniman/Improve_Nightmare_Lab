#include "Common.hlsl"

// Input raw AO / previous blur result. Bound through the existing t14 SRV slot.
Texture2D<float> SSAOInputTexture : register(t14);

// Output AO texture. Bound through the existing u0 UAV slot.
RWTexture2D<float> SSAOOutputTexture : register(u0);

// 5x5 footprint keeps the filter from turning undersampling noise into broad blotches.
static const int kBlurRadius = 2;
static const float kGaussianWeights[kBlurRadius + 1] =
{
	0.402620f,
	0.244201f,
	0.054489f
};

// These values control how strongly the blur preserves G-buffer boundaries.
static const float kDepthWeightScale = 4.0f;
static const float kNormalWeightPower = 8.0f;
static const uint2 kFrameBufferSize = uint2(FRAME_BUFFER_WIDTH, FRAME_BUFFER_HEIGHT);

float3 LoadViewSpaceNormal(uint2 pixelCoord)
{
	float3 normalW = DFNormalTexture.Load(int3(pixelCoord, 0)).xyz;
	normalW = normalize(normalW * 2.0f - 1.0f);
	return normalize(mul(normalW, (float3x3) gmtxView));
}

float LoadViewSpaceDepth(uint2 pixelCoord)
{
	float3 positionW = DFPositionTexture.Load(int3(pixelCoord, 0)).xyz;
	return mul(float4(positionW, 1.0f), gmtxView).z;
}

float CalculateBilateralWeight(float gaussianWeight, float centerDepth, float sampleDepth, float3 centerNormal, float3 sampleNormal)
{
	// AO radius is expressed in view-space units, so scale the depth threshold with it.
	// This avoids a fixed world-space blur range when gfScale changes.
	float depthWeight = exp(-abs(centerDepth - sampleDepth) * kDepthWeightScale / max(abs(gfScale), 0.001f));
	float normalWeight = pow(saturate(dot(centerNormal, sampleNormal)), kNormalWeightPower);
	return gaussianWeight * depthWeight * normalWeight;
}

// First bilateral pass: blur only along the X axis.
[numthreads(16, 16, 1)]
void CSBlurSSAOHorizontal(uint3 dispatchThreadID : SV_DispatchThreadID)
{
	uint2 pixelCoord = dispatchThreadID.xy;
	if (any(pixelCoord >= kFrameBufferSize))
	{
		return;
	}

	float centerAo = SSAOInputTexture.Load(int3(pixelCoord, 0));
	float centerDepth = LoadViewSpaceDepth(pixelCoord);
	float3 centerNormal = LoadViewSpaceNormal(pixelCoord);

	float weightedAo = centerAo * kGaussianWeights[0];
	float totalWeight = kGaussianWeights[0];

	for (int offset = 1; offset <= kBlurRadius; ++offset)
	{
		for (int side = 0; side < 2; ++side)
		{
			int direction = side == 0 ? -offset : offset;
			int2 sampleCoord = clamp(
				int2(pixelCoord) + int2(direction, 0),
				int2(0, 0),
				int2(FRAME_BUFFER_WIDTH - 1, FRAME_BUFFER_HEIGHT - 1)
			);
			float sampleAo = SSAOInputTexture.Load(int3(sampleCoord, 0));
			float sampleDepth = LoadViewSpaceDepth(sampleCoord);
			float3 sampleNormal = LoadViewSpaceNormal(sampleCoord);

			float weight = CalculateBilateralWeight(
				kGaussianWeights[offset],
				centerDepth,
				sampleDepth,
				centerNormal,
				sampleNormal
			);
			weightedAo += sampleAo * weight;
			totalWeight += weight;
		}
	}

	SSAOOutputTexture[pixelCoord] = weightedAo / max(totalWeight, 0.0001f);
}
