#pragma once
#include "Shader.h"

class CGenerateSSAOShader : public CShader
{
public:
	CGenerateSSAOShader() = default;
	~CGenerateSSAOShader() = default;

	D3D12_INPUT_LAYOUT_DESC CreateInputLayout() override;
	D3D12_RASTERIZER_DESC CreateRasterizerState() override;
	D3D12_BLEND_DESC CreateBlendState() override;
	D3D12_DEPTH_STENCIL_DESC CreateDepthStencilState() override;
	D3D12_SHADER_BYTECODE CreateVertexShader() override;
	D3D12_SHADER_BYTECODE CreatePixelShader() override;

	void CreateShader(
		ID3D12Device* pd3dDevice,
		ID3D12GraphicsCommandList* pd3dCommandList,
		ID3D12RootSignature* pd3dGraphicsRootSignature,
		UINT nRenderTargets,
		DXGI_FORMAT* pdxgiRtvFormats,
		DXGI_FORMAT dxgiDsvFormat
	) override;

	void CreateResourcesAndRtvsSrvs(
		ID3D12Device* pd3dDevice,
		ID3D12GraphicsCommandList* pd3dCommandList,
		UINT nRenderTargets,
		DXGI_FORMAT* pdxgiFormats,
		D3D12_CPU_DESCRIPTOR_HANDLE d3dRtvCPUDescriptorHandle
	);

	void Render(ID3D12GraphicsCommandList* pd3dCommandList, const shared_ptr<CCamera>& pCamera, const shared_ptr<CPlayer>& pPlayer, int nPipelineState = 0) override;
	shared_ptr<CTexture>& GetAmbientOcclusionTexture() { return m_pAmbientOcclusionTexture; }

private:
	shared_ptr<CTexture> m_pAmbientOcclusionTexture;
	shared_ptr<CTexture> m_pNoiseTexture;
	unique_ptr<D3D12_CPU_DESCRIPTOR_HANDLE> m_pAmbientOcclusionRtvCPUDescriptorHandle;
};
