#include "Common.hlsl"
#include "Light.hlsl"

float4 PSPostProcessingWithSSAO(PS_POSTPROCESSING_OUT input) : SV_Target
{
	uint2 pixelCoord = min(uint2(input.position.xy), uint2(FRAME_BUFFER_WIDTH, FRAME_BUFFER_HEIGHT) - 1);
	float4 cColor = DFTextureTexture.Load(int3(pixelCoord, 0));
	float4 cEmissiveColor = DFTextureEmissive.Load(int3(pixelCoord, 0));

	float4 positionW = DFPositionTexture.Load(int3(pixelCoord, 0));
	float3 normalW = DFNormalTexture.Load(int3(pixelCoord, 0)).xyz;
	normalW = normalize(normalW * 2.0f - 1.0f);

	// PSGenerateSSAO writes 1.0 for the background and the unoccluded visibility for geometry.
	float ambientVisibility = AlbedoTexture.Load(int3(pixelCoord, 0)).r;

	float4 light = Lighting(positionW.xyz, normalW, ambientVisibility);
	cColor *= light;
	cColor += cEmissiveColor;

	return cColor;
}
