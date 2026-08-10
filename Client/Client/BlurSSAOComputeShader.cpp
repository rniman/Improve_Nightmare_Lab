#include "stdafx.h"
#include "BlurSSAOComputeShader.h"

#include "Scene.h"

namespace
{
	constexpr UINT kThreadGroupSize = 16;
	constexpr UINT kHorizontalBlurPipelineIndex = 0;
	constexpr UINT kVerticalBlurPipelineIndex = 1;
	constexpr UINT kGBufferRootParameterIndex = 10;
	constexpr UINT kBlurInputRootParameterIndex = 17;
	constexpr UINT kBlurOutputRootParameterIndex = 16;
}

D3D12_SHADER_BYTECODE CBlurSSAOComputeShader::CreateComputeShader(ID3DBlob** ppd3dShaderBlob)
{
	switch (m_PipeLineIndex)
	{
	case kHorizontalBlurPipelineIndex:
		return CShader::ReadCompiledShaderFromFile(L"cso/CSBlurSSAOHorizontal.cso", m_pd3dComputeShaderBlob.GetAddressOf());
	case kVerticalBlurPipelineIndex:
		return CShader::ReadCompiledShaderFromFile(L"cso/CSBlurSSAOVertical.cso", m_pd3dComputeShaderBlob.GetAddressOf());
	default:
		return D3D12_SHADER_BYTECODE();
	}
}

void CBlurSSAOComputeShader::CreateShader(
	ID3D12Device* pd3dDevice,
	ID3D12GraphicsCommandList* pd3dCommandList,
	ID3D12RootSignature* pd3dRootSignature
)
{
	m_nPipelineState = 2;
	m_vpd3dPipelineState.reserve(m_nPipelineState);

	const UINT cxThreadGroups = (FRAME_BUFFER_WIDTH + kThreadGroupSize - 1) / kThreadGroupSize;
	const UINT cyThreadGroups = (FRAME_BUFFER_HEIGHT + kThreadGroupSize - 1) / kThreadGroupSize;

	for (UINT i = 0; i < m_nPipelineState; ++i)
	{
		m_vpd3dPipelineState.emplace_back();
		CComputeShader::CreateShader(pd3dDevice, pd3dCommandList, pd3dRootSignature, cxThreadGroups, cyThreadGroups, 1);
	}

	CreateShaderVariables(pd3dDevice, pd3dCommandList);
}

void CBlurSSAOComputeShader::CreateShaderVariables(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList)
{
	m_pHorizontalBlurTexture = make_shared<CTexture>(1, RESOURCE_TEXTURE2D, 0, 1, 1, 1);
	m_pVerticalBlurTexture = make_shared<CTexture>(1, RESOURCE_TEXTURE2D, 0, 1, 1, 1);

	m_pHorizontalBlurTexture->CreateTexture(
		pd3dDevice,
		0,
		RESOURCE_TEXTURE2D,
		FRAME_BUFFER_WIDTH,
		FRAME_BUFFER_HEIGHT,
		1,
		0,
		DXGI_FORMAT_R16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		nullptr
	);
	m_pVerticalBlurTexture->CreateTexture(
		pd3dDevice,
		0,
		RESOURCE_TEXTURE2D,
		FRAME_BUFFER_WIDTH,
		FRAME_BUFFER_HEIGHT,
		1,
		0,
		DXGI_FORMAT_R16_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		nullptr
	);

	CScene::CreateShaderResourceViews(pd3dDevice, m_pHorizontalBlurTexture, 0, 0);
	CScene::CreateShaderResourceViews(pd3dDevice, m_pVerticalBlurTexture, 0, 0);
	CScene::CreateUnorderedAccessViews(pd3dDevice, m_pHorizontalBlurTexture, 0, 0);
	CScene::CreateUnorderedAccessViews(pd3dDevice, m_pVerticalBlurTexture, 0, 0);

	// The vertical result is sampled by PSPostProcessingWithSSAO through t0.
	m_pVerticalBlurTexture->SetRootParameterIndex(0, 3);
}

void CBlurSSAOComputeShader::Blur(ID3D12GraphicsCommandList* pd3dCommandList, const shared_ptr<CCamera>& pCamera)
{
	if (!m_pRawAmbientOcclusionTexture || !m_pGBufferTexture)
	{
		return;
	}

	m_pGBufferTexture->UpdateSrvShaderVariable(pd3dCommandList, kGBufferRootParameterIndex, 0);

	DispatchHorizontal(pd3dCommandList);
	DispatchVertical(pd3dCommandList);
}

void CBlurSSAOComputeShader::DispatchHorizontal(ID3D12GraphicsCommandList* pd3dCommandList)
{
	::SynchronizeResourceTransition(
		pd3dCommandList,
		m_pRawAmbientOcclusionTexture->GetResource(0),
		D3D12_RESOURCE_STATE_COMMON,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
	);

	pd3dCommandList->SetPipelineState(m_vpd3dPipelineState[kHorizontalBlurPipelineIndex].Get());
	m_pRawAmbientOcclusionTexture->UpdateSrvShaderVariable(pd3dCommandList, kBlurInputRootParameterIndex, 0);
	m_pHorizontalBlurTexture->UpdateUavShaderVariable(pd3dCommandList, kBlurOutputRootParameterIndex, 0);
	pd3dCommandList->Dispatch(m_cxThreadGroups, m_cyThreadGroups, m_czThreadGroups);

	::SynchronizeResourceTransition(
		pd3dCommandList,
		m_pRawAmbientOcclusionTexture->GetResource(0),
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		D3D12_RESOURCE_STATE_COMMON
	);
	::SynchronizeResourceTransition(
		pd3dCommandList,
		m_pHorizontalBlurTexture->GetResource(0),
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
	);
}

void CBlurSSAOComputeShader::DispatchVertical(ID3D12GraphicsCommandList* pd3dCommandList)
{
	::SynchronizeResourceTransition(
		pd3dCommandList,
		m_pVerticalBlurTexture->GetResource(0),
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS
	);

	pd3dCommandList->SetPipelineState(m_vpd3dPipelineState[kVerticalBlurPipelineIndex].Get());
	m_pHorizontalBlurTexture->UpdateSrvShaderVariable(pd3dCommandList, kBlurInputRootParameterIndex, 0);
	m_pVerticalBlurTexture->UpdateUavShaderVariable(pd3dCommandList, kBlurOutputRootParameterIndex, 0);
	pd3dCommandList->Dispatch(m_cxThreadGroups, m_cyThreadGroups, m_czThreadGroups);

	::SynchronizeResourceTransition(
		pd3dCommandList,
		m_pHorizontalBlurTexture->GetResource(0),
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS
	);
	::SynchronizeResourceTransition(
		pd3dCommandList,
		m_pVerticalBlurTexture->GetResource(0),
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
	);
}
