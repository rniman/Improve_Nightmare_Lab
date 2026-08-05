#include "Common.hlsl"

// Input horizontal blur result. Bound through the existing t14 SRV slot.
Texture2D<float> SSAOInputTexture : register(t14);

// Final vertically blurred AO result. Bound through the existing u0 UAV slot.
RWTexture2D<float> SSAOOutputTexture : register(u0);

static const int kBlurRadius = 3;
static const float kGaussianWeights[kBlurRadius + 1] =
{
	0.227027f,
	0.194594f,
	0.121621f,
	0.054054f
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
	float depthWeight = exp(-abs(centerDepth - sampleDepth) * kDepthWeightScale);
	float normalWeight = pow(saturate(dot(centerNormal, sampleNormal)), kNormalWeightPower);
	return gaussianWeight * depthWeight * normalWeight;
}

// Second bilateral pass: blur only along the Y axis.
[numthreads(16, 16, 1)]
void CSBlurSSAOVertical(uint3 dispatchThreadID : SV_DispatchThreadID)
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
				int2(pixelCoord) + int2(0, direction),
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
