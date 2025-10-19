#include "stdafx.h"
#include "BlurComputeShader.h"
#include "Scene.h"
#include "Object.h"

CTextureToScreenShader::CTextureToScreenShader(shared_ptr<CTexture>& pTexture) 
{
	m_pTexture = pTexture;
}

D3D12_INPUT_LAYOUT_DESC CTextureToScreenShader::CreateInputLayout()
{
	UINT nInputElementDescs = 2;
	D3D12_INPUT_ELEMENT_DESC* pd3dInputElementDescs = new D3D12_INPUT_ELEMENT_DESC[nInputElementDescs];

	pd3dInputElementDescs[0] = { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 };
	pd3dInputElementDescs[1] = { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 };
	
	D3D12_INPUT_LAYOUT_DESC d3dInputLayoutDesc;
	d3dInputLayoutDesc.pInputElementDescs = pd3dInputElementDescs;
	d3dInputLayoutDesc.NumElements = nInputElementDescs;

	return(d3dInputLayoutDesc);
}

D3D12_DEPTH_STENCIL_DESC CTextureToScreenShader::CreateDepthStencilState()
{
	D3D12_DEPTH_STENCIL_DESC d3dDepthStencilDesc;
	::ZeroMemory(&d3dDepthStencilDesc, sizeof(D3D12_DEPTH_STENCIL_DESC));
	d3dDepthStencilDesc.DepthEnable = FALSE;
	d3dDepthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	d3dDepthStencilDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
	d3dDepthStencilDesc.StencilEnable = FALSE;
	d3dDepthStencilDesc.StencilReadMask = 0x00;
	d3dDepthStencilDesc.StencilWriteMask = 0x00;
	d3dDepthStencilDesc.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
	d3dDepthStencilDesc.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
	d3dDepthStencilDesc.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
	d3dDepthStencilDesc.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_NEVER;
	d3dDepthStencilDesc.BackFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
	d3dDepthStencilDesc.BackFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
	d3dDepthStencilDesc.BackFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
	d3dDepthStencilDesc.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_NEVER;

	return(d3dDepthStencilDesc);
}

D3D12_SHADER_BYTECODE CTextureToScreenShader::CreateVertexShader()
{
	return CShader::ReadCompiledShaderFromFile(L"cso/VSTextureToScreen.cso", m_pd3dVertexShaderBlob.GetAddressOf());
}

D3D12_SHADER_BYTECODE CTextureToScreenShader::CreatePixelShader()
{
	return CShader::ReadCompiledShaderFromFile(L"cso/PSTextureToScreen.cso", m_pd3dPixelShaderBlob.GetAddressOf());
}

D3D12_RASTERIZER_DESC CTextureToScreenShader::CreateRasterizerState()
{
	D3D12_RASTERIZER_DESC d3dRasterizerDesc;
	::ZeroMemory(&d3dRasterizerDesc, sizeof(D3D12_RASTERIZER_DESC));
	d3dRasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;
	d3dRasterizerDesc.CullMode = D3D12_CULL_MODE_BACK;
	d3dRasterizerDesc.FrontCounterClockwise = FALSE;
	d3dRasterizerDesc.DepthBias = 0;
	d3dRasterizerDesc.DepthBiasClamp = 0.0f;
	d3dRasterizerDesc.SlopeScaledDepthBias = 0.0f;
	d3dRasterizerDesc.DepthClipEnable = TRUE;
	d3dRasterizerDesc.MultisampleEnable = FALSE;
	d3dRasterizerDesc.AntialiasedLineEnable = FALSE;
	d3dRasterizerDesc.ForcedSampleCount = 0;
	d3dRasterizerDesc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

	return(d3dRasterizerDesc);
}

void CTextureToScreenShader::CreateShader(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList, ID3D12RootSignature* pd3dGraphicsRootSignature, UINT nRenderTargets, DXGI_FORMAT* pdxgiRtvFormats, DXGI_FORMAT dxgiDsvFormat)
{
	m_nPipelineState = 1;
	m_vpd3dPipelineState.reserve(m_nPipelineState);
	for (int i = 0; i < m_nPipelineState; ++i)
	{
		m_vpd3dPipelineState.emplace_back();
	}

	CShader::CreateShader(pd3dDevice, pd3dCommandList, pd3dGraphicsRootSignature, nRenderTargets, pdxgiRtvFormats, dxgiDsvFormat);

	CreateShaderVariables(pd3dDevice, pd3dCommandList);
}

void CTextureToScreenShader::Render(ID3D12GraphicsCommandList* pd3dCommandList, const shared_ptr<CCamera>& pCamera, const shared_ptr<CPlayer>& pPlayer)
{
	if (m_vpd3dPipelineState[0].Get())
	{
		pd3dCommandList->SetPipelineState(m_vpd3dPipelineState[0].Get());
	}

	if (m_pTexture) 
	{
		m_pTexture->UpdateShaderVariables(pd3dCommandList);
	}

	pd3dCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	pd3dCommandList->DrawInstanced(6, 1, 0, 0);
}

/// <CShader - CTextureToFullScreenShader>
////// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// ///  
/// <CShader - CComputeShader>

D3D12_SHADER_BYTECODE CComputeShader::CreateComputeShader(ID3DBlob** ppd3dShaderBlob)
{
	return D3D12_SHADER_BYTECODE();
}

void CComputeShader::CreateShader(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList, ID3D12RootSignature* pd3dRootSignature, UINT cxThreadGroups, UINT cyThreadGroups, UINT czThreadGroups)
{
	ID3DBlob* pd3dComputeShaderBlob = NULL;

	D3D12_CACHED_PIPELINE_STATE d3dCachedPipelineState = { };
	D3D12_COMPUTE_PIPELINE_STATE_DESC d3dPipelineStateDesc;
	::ZeroMemory(&d3dPipelineStateDesc, sizeof(D3D12_COMPUTE_PIPELINE_STATE_DESC));
	d3dPipelineStateDesc.pRootSignature = pd3dRootSignature;
	d3dPipelineStateDesc.CS = CreateComputeShader(&pd3dComputeShaderBlob);
	d3dPipelineStateDesc.NodeMask = 0;
	d3dPipelineStateDesc.CachedPSO = d3dCachedPipelineState;
	d3dPipelineStateDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
	
	HRESULT hResult = pd3dDevice->CreateComputePipelineState(&d3dPipelineStateDesc, __uuidof(ID3D12PipelineState), (void**)m_vpd3dPipelineState[m_nPipeLineIndex++].GetAddressOf());
	if (pd3dComputeShaderBlob) 
	{
		pd3dComputeShaderBlob->Release();
	}

	m_cxThreadGroups = cxThreadGroups;
	m_cyThreadGroups = cyThreadGroups;
	m_czThreadGroups = czThreadGroups;
}

void CComputeShader::Dispatch(ID3D12GraphicsCommandList* pd3dCommandList)
{
	UpdateShaderVariables(pd3dCommandList);
	pd3dCommandList->Dispatch(m_cxThreadGroups, m_cyThreadGroups, m_czThreadGroups);
}

void CComputeShader::Dispatch(ID3D12GraphicsCommandList* pd3dCommandList, UINT cxThreadGroups, UINT cyThreadGroups, UINT czThreadGroups)
{
	UpdateShaderVariables(pd3dCommandList);
	pd3dCommandList->Dispatch(cxThreadGroups, cyThreadGroups, czThreadGroups);
}

/// <CShader - CComputeShader>
////// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// ///  
/// <CShader - CComputeShader - CBlurComputeShader>

D3D12_SHADER_BYTECODE CBlurComputeShader::CreateComputeShader(ID3DBlob** ppd3dShaderBlob)
{
	switch (m_nPipeLineIndex)
	{
	case 0:
		return CShader::ReadCompiledShaderFromFile(L"cso/CSBloomOff.cso", m_pd3dComputeShaderBlob.GetAddressOf());
	case 1:
		return CShader::ReadCompiledShaderFromFile(L"cso/CSBlurVertical.cso", m_pd3dComputeShaderBlob.GetAddressOf());
	case 2:
		return CShader::ReadCompiledShaderFromFile(L"cso/CSBlurHorizontal.cso", m_pd3dComputeShaderBlob.GetAddressOf());
	case 3:
		return CShader::ReadCompiledShaderFromFile(L"cso/CSComposite.cso", m_pd3dComputeShaderBlob.GetAddressOf());
	default:
		break;
	}
}

void CBlurComputeShader::CreateShader(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList, ID3D12RootSignature* pd3dRootSignature, UINT cxThreadGroups, UINT cyThreadGroups, UINT czThreadGroups)
{
	m_nPipelineState = 4;
	m_vpd3dPipelineState.reserve(m_nPipelineState);
	for (int i = 0; i < m_nPipelineState; ++i)
	{
		m_vpd3dPipelineState.emplace_back();
		CComputeShader::CreateShader(pd3dDevice, pd3dCommandList, pd3dRootSignature, cxThreadGroups, cyThreadGroups, czThreadGroups);
	}

	CreateShaderVariables(pd3dDevice, pd3dCommandList);
}

void CBlurComputeShader::CreateShaderVariables(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList)
{
	m_pTextureFirPassUav = make_shared<CTexture>(1, RESOURCE_TEXTURE2D, 0, 2, 1, 1);
	m_pTextureSecPassUav = make_shared<CTexture>(1, RESOURCE_TEXTURE2D, 0, 2, 1, 1);
	m_pTextureCompositeUav = make_shared<CTexture>(1, RESOURCE_TEXTURE2D, 0, 2, 1, 1);

	m_pTextureFirPassUav->CreateTexture(
		pd3dDevice,
		0,
		RESOURCE_TEXTURE2D,
		FRAME_BUFFER_WIDTH,
		FRAME_BUFFER_HEIGHT,
		1,
		0,
		DXGI_FORMAT_R8G8B8A8_UNORM,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		NULL
	);
	m_pTextureSecPassUav->CreateTexture(
		pd3dDevice, 
		0, 
		RESOURCE_TEXTURE2D, 
		FRAME_BUFFER_WIDTH,
		FRAME_BUFFER_HEIGHT, 
		1, 
		0, 
		DXGI_FORMAT_R8G8B8A8_UNORM, 
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		NULL
	);
	m_pTextureCompositeUav->CreateTexture(
		pd3dDevice,
		0,
		RESOURCE_TEXTURE2D,
		FRAME_BUFFER_WIDTH,
		FRAME_BUFFER_HEIGHT,
		1,
		0,
		DXGI_FORMAT_R8G8B8A8_UNORM,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		NULL
	);

	// SRV 디스크립터 힙 생성
	CScene::CreateShaderResourceViews(pd3dDevice, m_pTextureFirPassUav, 0, 0);
	CScene::CreateShaderResourceViews(pd3dDevice, m_pTextureSecPassUav, 0, 0);
	CScene::CreateShaderResourceViews(pd3dDevice, m_pTextureCompositeUav, 0, 0);

	// UAV 디스크립터 힙 생성
	CScene::CreateUnorderedAccessViews(pd3dDevice, m_pTextureFirPassUav, 0, 0);
	CScene::CreateUnorderedAccessViews(pd3dDevice, m_pTextureSecPassUav, 0, 0);
	CScene::CreateUnorderedAccessViews(pd3dDevice, m_pTextureCompositeUav, 0, 0);

	// 0번 인덱스의 파라미터는 17(SRV), 1번 인덱스의 파라미터는 16(UAV)
	m_pTextureFirPassUav->SetRootParameterIndex(1, 16);
	m_pTextureFirPassUav->SetRootParameterIndex(0, 17);
	m_pTextureSecPassUav->SetRootParameterIndex(1, 16);
	m_pTextureSecPassUav->SetRootParameterIndex(0, 17);
	m_pTextureCompositeUav->SetRootParameterIndex(1, 16);
	m_pTextureCompositeUav->SetRootParameterIndex(0, 17);

	m_cxThreadGroups = ceil(FRAME_BUFFER_WIDTH / 32.0f);	//50
	m_cyThreadGroups = ceil(FRAME_BUFFER_HEIGHT / 32.0f);	//32
}

void CBlurComputeShader::UpdateShaderVariables(ID3D12GraphicsCommandList* pd3dCommandList)
{
}

void CBlurComputeShader::ReleaseShaderVariables()
{
}

void CBlurComputeShader::ReleaseUploadBuffers()
{
}

void CBlurComputeShader::Dispatch(ID3D12GraphicsCommandList* pd3dCommandList, int nPipelineState)
{
	if (m_vpd3dPipelineState[nPipelineState])
	{
		pd3dCommandList->SetPipelineState(m_vpd3dPipelineState[nPipelineState].Get());
	}
	UpdateShaderVariables(pd3dCommandList);

	if (m_pTextureCompositeUav)
	{
		m_pTextureCompositeUav->UpdateUavShaderVariable(pd3dCommandList, 16, 0);
	}
	if (m_pTextureRtv)
	{
		m_pTextureRtv->UpdateSrvShaderVariableForCompute(pd3dCommandList, 0, 0);
	}

	for (int i = 0; i < 1; i++)
	{
		ID3D12Resource* pd3dSource = m_pTextureCompositeUav->GetResource(0);
		::SynchronizeResourceTransition(pd3dCommandList, pd3dSource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		pd3dCommandList->Dispatch(m_cxThreadGroups, m_cyThreadGroups, m_czThreadGroups);

		::SynchronizeResourceTransition(pd3dCommandList, pd3dSource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	}
}

void CBlurComputeShader::PassFirst(ID3D12GraphicsCommandList* pd3dCommandList, int nPipelineState)
{
	if (m_vpd3dPipelineState[nPipelineState])
	{
		pd3dCommandList->SetPipelineState(m_vpd3dPipelineState[nPipelineState].Get());
	}
	UpdateShaderVariables(pd3dCommandList);

	if (m_pTextureFirPassUav)
	{
		m_pTextureFirPassUav->UpdateUavShaderVariable(pd3dCommandList, 16, 0);
	}
	if (m_pTextureRtv)
	{
		m_pTextureRtv->UpdateSrvShaderVariableForCompute(pd3dCommandList, 0, 0);
	}

	for (int i = 0; i < 1; i++)
	{
		pd3dCommandList->Dispatch(m_cxThreadGroups, m_cyThreadGroups, m_czThreadGroups);

		ID3D12Resource* pd3dTargetSource = m_pTextureFirPassUav->GetResource(0);
		::SynchronizeResourceTransition(pd3dCommandList, pd3dTargetSource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

		ID3D12Resource* pd3dIdleSource = m_pTextureCompositeUav->GetResource(0);
		::SynchronizeResourceTransition(pd3dCommandList, pd3dIdleSource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	}
}

void CBlurComputeShader::PassSecond(ID3D12GraphicsCommandList* pd3dCommandList, int nPipelineState)
{
	if (m_vpd3dPipelineState[nPipelineState])
	{
		pd3dCommandList->SetPipelineState(m_vpd3dPipelineState[nPipelineState].Get());
	}
	UpdateShaderVariables(pd3dCommandList);

	if (m_pTextureFirPassUav)
	{
		m_pTextureFirPassUav->UpdateSrvShaderVariableForCompute(pd3dCommandList, 0, 0);
	}
	if (m_pTextureSecPassUav)
	{
		m_pTextureSecPassUav->UpdateUavShaderVariable(pd3dCommandList, 16, 0);
	}
	if (m_pTextureRtv)
	{
		m_pTextureRtv->UpdateSrvShaderVariableForCompute(pd3dCommandList, 0, 0);
	}

	for (int i = 0; i < 1; i++)
	{
		pd3dCommandList->Dispatch(m_cxThreadGroups, m_cyThreadGroups, m_czThreadGroups);

		ID3D12Resource* pd3dTargetSource = m_pTextureSecPassUav->GetResource(0);
		::SynchronizeResourceTransition(pd3dCommandList, pd3dTargetSource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

		ID3D12Resource* pd3dIdleSource = m_pTextureFirPassUav->GetResource(0);
		::SynchronizeResourceTransition(pd3dCommandList, pd3dIdleSource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	}
}

void CBlurComputeShader::PassComposite(ID3D12GraphicsCommandList* pd3dCommandList, int nPipelineState)
{
	if (m_vpd3dPipelineState[nPipelineState])
	{
		pd3dCommandList->SetPipelineState(m_vpd3dPipelineState[nPipelineState].Get());
	}
	UpdateShaderVariables(pd3dCommandList);

	if (m_pTextureSecPassUav)
	{
		m_pTextureSecPassUav->UpdateSrvShaderVariableForCompute(pd3dCommandList, 0, 0);
	}
	if (m_pTextureCompositeUav)
	{
		m_pTextureCompositeUav->UpdateUavShaderVariable(pd3dCommandList, 16, 0);
	}
	if (m_pTextureRtv)
	{
		m_pTextureRtv->UpdateSrvShaderVariableForCompute(pd3dCommandList, 0, 0);
	}

	for (int i = 0; i < 1; i++)
	{
		pd3dCommandList->Dispatch(m_cxThreadGroups, m_cyThreadGroups, m_czThreadGroups);

		ID3D12Resource* pd3dTargetSource = m_pTextureCompositeUav->GetResource(0);
		::SynchronizeResourceTransition(pd3dCommandList, pd3dTargetSource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

		ID3D12Resource* pd3dIdleSource = m_pTextureSecPassUav->GetResource(0);
		::SynchronizeResourceTransition(pd3dCommandList, pd3dIdleSource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	}
}
