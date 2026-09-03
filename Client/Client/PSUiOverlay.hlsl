Texture2D OverlayTexture : register(t0);
SamplerState OverlayClampSampler : register(s4);

struct PSInput
{
	float4 position : SV_POSITION;
	float2 uv : TEXCOORD;
	float4 color : COLOR;
};

float4 PSUiOverlay(PSInput input) : SV_TARGET
{
	float glyphAlpha = OverlayTexture.Sample(OverlayClampSampler, input.uv).a;
	return float4(input.color.rgb, glyphAlpha * input.color.a);
}
