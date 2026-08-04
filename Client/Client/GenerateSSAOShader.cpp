#include "stdafx.h"
#include "GenerateSSAOShader.h"
#include "Scene.h"

D3D12_INPUT_LAYOUT_DESC CGenerateSSAOShader::CreateInputLayout()
{
	// VSGenerateSSAO generates the fullscreen quad with SV_VertexID.
	return { nullptr, 0 };
}

D3D12_RASTERIZER_DESC CGenerateSSAOShader::CreateRasterizerState()
{
	return CShader::CreateRasterizerState();
}

D3D12_BLEND_DESC CGenerateSSAOShader::CreateBlendState()
{
	// Write the raw AO value directly without blending with the previous target.
	return CShader::CreateBlendState();
}

D3D12_DEPTH_STENCIL_DESC CGenerateSSAOShader::CreateDepthStencilState()
{
	D3D12_DEPTH_STENCIL_DESC d3dDepthStencilDesc = {};
	d3dDepthStencilDesc.DepthEnable = FALSE;
	d3dDepthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	d3dDepthStencilDesc.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
	d3dDepthStencilDesc.StencilEnable = FALSE;
	d3dDepthStencilDesc.StencilReadMask = 0x00;
	d3dDepthStencilDesc.StencilWriteMask = 0x00;
	d3dDepthStencilDesc.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
	d3dDepthStencilDesc.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
	d3dDepthStencilDesc.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
	d3dDepthStencilDesc.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
	d3dDepthStencilDesc.BackFace = d3dDepthStencilDesc.FrontFace;

	return d3dDepthStencilDesc;
}

D3D12_SHADER_BYTECODE CGenerateSSAOShader::CreateVertexShader()
{
	return CShader::ReadCompiledShaderFromFile(L"cso/VSGenerateSSAO.cso", m_pd3dVertexShaderBlob.GetAddressOf());
}

D3D12_SHADER_BYTECODE CGenerateSSAOShader::CreatePixelShader()
{
	return CShader::ReadCompiledShaderFromFile(L"cso/PSGenerateSSAO.cso", m_pd3dPixelShaderBlob.GetAddressOf());
}

void CGenerateSSAOShader::CreateShader(
	ID3D12Device* pd3dDevice,
	ID3D12GraphicsCommandList* pd3dCommandList,
	ID3D12RootSignature* pd3dGraphicsRootSignature,
	UINT nRenderTargets,
	DXGI_FORMAT* pdxgiRtvFormats,
	DXGI_FORMAT dxgiDsvFormat
)
{
	m_nPipelineState = 1;
	m_vpd3dPipelineState.reserve(m_nPipelineState);
	for (int i = 0; i < m_nPipelineState; ++i)
	{
		m_vpd3dPipelineState.emplace_back();
		CShader::CreateShader(pd3dDevice, pd3dCommandList, pd3dGraphicsRootSignature, nRenderTargets, pdxgiRtvFormats, dxgiDsvFormat);
	}
}

void CGenerateSSAOShader::CreateResourcesAndRtvsSrvs(
	ID3D12Device* pd3dDevice,
	ID3D12GraphicsCommandList* pd3dCommandList,
	UINT nRenderTargets,
	DXGI_FORMAT* pdxgiFormats,
	D3D12_CPU_DESCRIPTOR_HANDLE d3dRtvCPUDescriptorHandle
)
{
	// Raw AO stores one visibility value per pixel: 1.0f means unoccluded.
	m_pAmbientOcclusionTexture = make_shared<CTexture>(1, RESOURCE_TEXTURE2D, 0, 1);

	D3D12_CLEAR_VALUE d3dClearValue = { pdxgiFormats[0], { 1.0f, 1.0f, 1.0f, 1.0f } };
	m_pAmbientOcclusionTexture->CreateTexture(
		pd3dDevice,
		0,
		RESOURCE_TEXTURE2D,
		FRAME_BUFFER_WIDTH,
		FRAME_BUFFER_HEIGHT,
		1,
		0,
		pdxgiFormats[0],
		D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
		D3D12_RESOURCE_STATE_COMMON,
		&d3dClearValue
	);

	// The final post-processing pass will read this texture through t0.
	CScene::CreateShaderResourceViews(pd3dDevice, m_pAmbientOcclusionTexture, 0, 3);

	D3D12_RENDER_TARGET_VIEW_DESC d3dRenderTargetViewDesc = {};
	d3dRenderTargetViewDesc.Format = pdxgiFormats[0];
	d3dRenderTargetViewDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
	d3dRenderTargetViewDesc.Texture2D.MipSlice = 0;
	d3dRenderTargetViewDesc.Texture2D.PlaneSlice = 0;

	pd3dDevice->CreateRenderTargetView(
		m_pAmbientOcclusionTexture->GetResource(0),
		&d3dRenderTargetViewDesc,
		d3dRtvCPUDescriptorHandle
	);
	m_pAmbientOcclusionRtvCPUDescriptorHandle = make_unique<D3D12_CPU_DESCRIPTOR_HANDLE>(d3dRtvCPUDescriptorHandle);

	// Blue-noise is an input to the SSAO generation pass, not a post-processing resource.
	m_pNoiseTexture = make_shared<CTexture>(1, RESOURCE_TEXTURE2D, 0, 1);
	m_pNoiseTexture->LoadTextureFromDDSFile(pd3dDevice, pd3dCommandList, (wchar_t*)L"Asset/Textures/LDR_LLL1_0.dds", RESOURCE_TEXTURE2D, 0);
	CScene::CreateShaderResourceViews(pd3dDevice, m_pNoiseTexture, 0, 3);
}

void CGenerateSSAOShader::Render(ID3D12GraphicsCommandList* pd3dCommandList, const shared_ptr<CCamera>& pCamera, const shared_ptr<CPlayer>& pPlayer, int nPipelineState)
{
	UpdateShaderVariables(pd3dCommandList);

	ID3D12Resource* pd3dAmbientOcclusionResource = m_pAmbientOcclusionTexture->GetResource(0);
	::SynchronizeResourceTransition(
		pd3dCommandList,
		pd3dAmbientOcclusionResource,
		D3D12_RESOURCE_STATE_COMMON,
		D3D12_RESOURCE_STATE_RENDER_TARGET
	);

	D3D12_CPU_DESCRIPTOR_HANDLE d3dAmbientOcclusionRtvCPUDescriptorHandle = *m_pAmbientOcclusionRtvCPUDescriptorHandle;
	const FLOAT clearValue[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	pd3dCommandList->ClearRenderTargetView(d3dAmbientOcclusionRtvCPUDescriptorHandle, clearValue, 0, nullptr);
	pd3dCommandList->OMSetRenderTargets(1, &d3dAmbientOcclusionRtvCPUDescriptorHandle, FALSE, nullptr);

	m_pNoiseTexture->UpdateShaderVariables(pd3dCommandList);
	UpdatePipeLineState(pd3dCommandList, nPipelineState);
	pd3dCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	pd3dCommandList->DrawInstanced(6, 1, 0, 0);

	// The next pass reads the raw AO result through an SRV.
	::SynchronizeResourceTransition(
		pd3dCommandList,
		pd3dAmbientOcclusionResource,
		D3D12_RESOURCE_STATE_RENDER_TARGET,
		D3D12_RESOURCE_STATE_COMMON
	);
}
