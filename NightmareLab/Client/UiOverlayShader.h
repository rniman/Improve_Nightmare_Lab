#pragma once

#include "Shader.h"

struct UiOverlayVertex
{
	XMFLOAT2 position = {};
	XMFLOAT2 uv = {};
	XMFLOAT4 color = {};
};

class UiOverlayShader final : public CShader
{
public:
	D3D12_INPUT_LAYOUT_DESC CreateInputLayout() override;
	D3D12_RASTERIZER_DESC CreateRasterizerState() override;
	D3D12_BLEND_DESC CreateBlendState() override;
	D3D12_DEPTH_STENCIL_DESC CreateDepthStencilState() override;
	D3D12_SHADER_BYTECODE CreateVertexShader() override;
	D3D12_SHADER_BYTECODE CreatePixelShader() override;

	void CreateShader(
		ID3D12Device* device,
		ID3D12GraphicsCommandList* commandList,
		ID3D12RootSignature* rootSignature,
		UINT renderTargetCount = 1,
		DXGI_FORMAT* renderTargetFormats = nullptr,
		DXGI_FORMAT depthStencilFormat = DXGI_FORMAT_UNKNOWN
	) override;
	void PrepareRender(ID3D12GraphicsCommandList* commandList);
};
