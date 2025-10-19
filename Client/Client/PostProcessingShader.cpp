#include "stdafx.h"
#include "PostProcessingShader.h"
#include "Scene.h"

CPostProcessingShader::CPostProcessingShader()
{
}

CPostProcessingShader::~CPostProcessingShader()
{

}

D3D12_INPUT_LAYOUT_DESC CPostProcessingShader::CreateInputLayout()
{
	D3D12_INPUT_LAYOUT_DESC d3dInputLayoutDesc;
	d3dInputLayoutDesc.pInputElementDescs = NULL;
	d3dInputLayoutDesc.NumElements = 0;

	return(d3dInputLayoutDesc);
}

D3D12_DEPTH_STENCIL_DESC CPostProcessingShader::CreateDepthStencilState()
{
	D3D12_DEPTH_STENCIL_DESC d3dDepthStencilDesc;
	::ZeroMemory(&d3dDepthStencilDesc, sizeof(D3D12_DEPTH_STENCIL_DESC));
	d3dDepthStencilDesc.DepthEnable = FALSE;
	d3dDepthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;//D3D12_DEPTH_WRITE_MASK_ALL;
	d3dDepthStencilDesc.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
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

D3D12_SHADER_BYTECODE CPostProcessingShader::CreateVertexShader()
{
	return CShader::ReadCompiledShaderFromFile(L"cso/VSPostProcessing.cso", m_pd3dVertexShaderBlob.GetAddressOf());
}

D3D12_SHADER_BYTECODE CPostProcessingShader::CreatePixelShader()
{
	if (m_nPipeLineIndex == 0)
	{
		return CShader::ReadCompiledShaderFromFile(L"cso/PSPostProcessingWithSSAO.cso", m_pd3dPixelShaderBlob.GetAddressOf());
	}
	else if (m_nPipeLineIndex == 1)
	{
		return CShader::ReadCompiledShaderFromFile(L"cso/PSPostProcessing.cso", m_pd3dPixelShaderBlob.GetAddressOf());
	}
}

D3D12_RASTERIZER_DESC CPostProcessingShader::CreateRasterizerState()
{
	return(CShader::CreateRasterizerState());
}

D3D12_SHADER_BYTECODE CPostProcessingShader::CreateComputeShader(ID3DBlob** ppd3dShaderBlob)
{
	if(m_nPipeLineIndex == 2)
	{
		return CShader::ReadCompiledShaderFromFile(L"cso/CSSSAOProcessing.cso", m_pd3dComputeShaderBlob.GetAddressOf());
	}
	else if (m_nPipeLineIndex == 3)
	{
		return CShader::ReadCompiledShaderFromFile(L"cso/CSBilateralBlurHorizontal.cso", m_pd3dComputeShaderBlob.GetAddressOf());
	}
	else if (m_nPipeLineIndex == 4)
	{
		return CShader::ReadCompiledShaderFromFile(L"cso/CSBilateralBlurVertical.cso", m_pd3dComputeShaderBlob.GetAddressOf());
	}
}

void CPostProcessingShader::CreateShader(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList, ID3D12RootSignature* pd3dGraphicsRootSignature, UINT nRenderTargets, DXGI_FORMAT* pdxgiRtvFormats, DXGI_FORMAT dxgiDsvFormat)
{
	m_nComputePipelineStartIndex = 2;
	m_nPipelineState = 5;
	m_vpd3dPipelineState.reserve(m_nPipelineState);
	for (int i = 0; i < m_nPipelineState; ++i)
	{
		m_vpd3dPipelineState.emplace_back();
		if (i >= m_nComputePipelineStartIndex)
		{
			CreateComputeShader(pd3dDevice, pd3dCommandList, pd3dGraphicsRootSignature);
		}
		else
		{
			CShader::CreateShader(pd3dDevice, pd3dCommandList, pd3dGraphicsRootSignature, nRenderTargets, pdxgiRtvFormats, dxgiDsvFormat);
		}
	}
	
	m_cxThreadGroups = ceil(FRAME_BUFFER_WIDTH / 32.0f);	//50
	m_cyThreadGroups = ceil(FRAME_BUFFER_HEIGHT / 32.0f);	//32
	m_czThreadGroups = 1;
}

void CPostProcessingShader::CreateComputeShader(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList, ID3D12RootSignature* pd3dRootSignature)
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
}

void CPostProcessingShader::CreateResourcesAndRtvsSrvs(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList, UINT nRenderTargets, DXGI_FORMAT* pdxgiFormats, D3D12_CPU_DESCRIPTOR_HANDLE d3dRtvCPUDescriptorHandle)
{
	m_pTexture = make_shared<CTexture>(nRenderTargets, RESOURCE_TEXTURE2D, 0, 1);

	D3D12_CLEAR_VALUE d3dClearValue;

	for (UINT i = 0; i < nRenderTargets; i++)
	{
		if (pdxgiFormats[i] == DXGI_FORMAT_R32_FLOAT) {
			d3dClearValue = { DXGI_FORMAT_R32_FLOAT, { 1.0f, 1.0f, 1.0f, 1.0f } };
		}
		else {
			d3dClearValue = { pdxgiFormats[i], {m_fClearValue[0],m_fClearValue[1],m_fClearValue[2],m_fClearValue[3]} };
		}

		d3dClearValue.Format = pdxgiFormats[i];
		m_pTexture->CreateTexture(
			pd3dDevice, 
			i, 
			RESOURCE_TEXTURE2D, 
			FRAME_BUFFER_WIDTH,
			FRAME_BUFFER_HEIGHT,
			1,
			0,
			pdxgiFormats[i], 
			D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
			D3D12_RESOURCE_STATE_COMMON,
			&d3dClearValue
		);
	}

	CreateShaderVariables(pd3dDevice, pd3dCommandList);
	CScene::CreateShaderResourceViews(pd3dDevice, m_pTexture, 0, 10);

	D3D12_RENDER_TARGET_VIEW_DESC d3dRenderTargetViewDesc;
	d3dRenderTargetViewDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
	d3dRenderTargetViewDesc.Texture2D.MipSlice = 0;
	d3dRenderTargetViewDesc.Texture2D.PlaneSlice = 0;

	for (UINT i = 0; i < nRenderTargets; i++)
	{
		d3dRenderTargetViewDesc.Format = pdxgiFormats[i];
		ID3D12Resource* pd3dTextureResource = m_pTexture->GetResource(i);
		pd3dDevice->CreateRenderTargetView(pd3dTextureResource, &d3dRenderTargetViewDesc, d3dRtvCPUDescriptorHandle);
		m_vpRtvCPUDescriptorHandles.push_back(make_unique<D3D12_CPU_DESCRIPTOR_HANDLE>(d3dRtvCPUDescriptorHandle));
		d3dRtvCPUDescriptorHandle.ptr += ::gnRtvDescriptorIncrementSize;
	}

	D3D12_DESCRIPTOR_HEAP_DESC d3dDescriptorHeapDesc;
	::ZeroMemory(&d3dDescriptorHeapDesc, sizeof(D3D12_DESCRIPTOR_HEAP_DESC));
	d3dDescriptorHeapDesc.NumDescriptors = 1;
	d3dDescriptorHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	d3dDescriptorHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	d3dDescriptorHeapDesc.NodeMask = 0;
	d3dDescriptorHeapDesc.NumDescriptors = 1;
	d3dDescriptorHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	ThrowIfFailed(pd3dDevice->CreateDescriptorHeap(&d3dDescriptorHeapDesc, __uuidof(ID3D12DescriptorHeap), (void**)&m_pd3dDsvDescriptorHeap));

	D3D12_RESOURCE_DESC d3dResourceDesc;
	d3dResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	d3dResourceDesc.Alignment = 0;
	d3dResourceDesc.Width = FRAME_BUFFER_WIDTH;
	d3dResourceDesc.Height = FRAME_BUFFER_HEIGHT;
	d3dResourceDesc.DepthOrArraySize = 1;
	d3dResourceDesc.MipLevels = 1;
	d3dResourceDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	d3dResourceDesc.SampleDesc.Count = 1;
	d3dResourceDesc.SampleDesc.Quality = 0;
	d3dResourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	d3dResourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	D3D12_HEAP_PROPERTIES d3dHeapProperties;
	::ZeroMemory(&d3dHeapProperties, sizeof(D3D12_HEAP_PROPERTIES));
	d3dHeapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
	d3dHeapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	d3dHeapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	d3dHeapProperties.CreationNodeMask = 1;
	d3dHeapProperties.VisibleNodeMask = 1;

	d3dClearValue.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	d3dClearValue.DepthStencil.Depth = 1.0f;
	d3dClearValue.DepthStencil.Stencil = 0;

	pd3dDevice->CreateCommittedResource(&d3dHeapProperties, D3D12_HEAP_FLAG_NONE, &d3dResourceDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &d3dClearValue, __uuidof(ID3D12Resource), (void**)m_pd3dDepthBuffer.GetAddressOf());

	D3D12_DEPTH_STENCIL_VIEW_DESC d3dDepthStencilViewDesc;
	::ZeroMemory(&d3dDepthStencilViewDesc, sizeof(D3D12_DEPTH_STENCIL_VIEW_DESC));
	d3dDepthStencilViewDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	d3dDepthStencilViewDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	d3dDepthStencilViewDesc.Flags = D3D12_DSV_FLAG_NONE;

	//m_vpDsvDescriptorCPUHandles = new D3D12_CPU_DESCRIPTOR_HANDLE[ADD_DEPTH_MAP_COUNT];
	D3D12_CPU_DESCRIPTOR_HANDLE handle = m_pd3dDsvDescriptorHeap.Get()->GetCPUDescriptorHandleForHeapStart();
	for (int i = 0; i < ADD_DEPTH_MAP_COUNT; ++i) {
		m_vpDsvDescriptorCPUHandles.push_back(make_unique<D3D12_CPU_DESCRIPTOR_HANDLE>(handle));
		pd3dDevice->CreateDepthStencilView(m_pd3dDepthBuffer.Get(), &d3dDepthStencilViewDesc, *m_vpDsvDescriptorCPUHandles[i]);
		handle.ptr = ::gnDsvDescriptorIncrementSize;
	}

	//m_pNoiseTexture = make_shared<CTexture>(1, RESOURCE_TEXTURE2D, 0, 1);
	//m_pNoiseTexture->LoadTextureFromDDSFile(pd3dDevice, pd3dCommandList, (wchar_t*)L"Asset/noise8x8(2).dds", RESOURCE_TEXTURE2D, 0);
	////m_pNoiseTexture->LoadTextureFromDDSFile(pd3dDevice, pd3dCommandList, (wchar_t*)L"Asset/noise1600x1024.dds", RESOURCE_TEXTURE2D, 0);
	//CScene::CreateShaderResourceViews(pd3dDevice, m_pNoiseTexture, 0, 3);

	m_pAmbientOcclusionTexture = make_shared<CTexture>(1, RESOURCE_TEXTURE2D, 0, 2, 1, 1);
	m_pAmbientOcclusionTexture->CreateTexture(
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

	CScene::CreateShaderResourceViews(pd3dDevice, m_pAmbientOcclusionTexture, 0, 0);
	CScene::CreateUnorderedAccessViews(pd3dDevice, m_pAmbientOcclusionTexture, 0, 0);
	m_pAmbientOcclusionTexture->SetRootParameterIndex(0, 17);
	m_pAmbientOcclusionTexture->SetRootParameterIndex(1, 16);

	m_pBlurHorizontalTexture = make_shared<CTexture>(1, RESOURCE_TEXTURE2D, 0, 2, 1, 1);
	m_pBlurHorizontalTexture->CreateTexture(
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

	CScene::CreateShaderResourceViews(pd3dDevice, m_pBlurHorizontalTexture, 0, 0);
	CScene::CreateUnorderedAccessViews(pd3dDevice, m_pBlurHorizontalTexture, 0, 0);
	m_pBlurHorizontalTexture->SetRootParameterIndex(0, 17);
	m_pBlurHorizontalTexture->SetRootParameterIndex(1, 16);

	m_pBlurVerticalTexture = make_shared<CTexture>(1, RESOURCE_TEXTURE2D, 0, 2, 1, 1);
	m_pBlurVerticalTexture->CreateTexture(
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

	CScene::CreateShaderResourceViews(pd3dDevice, m_pBlurVerticalTexture, 0, 0);
	CScene::CreateUnorderedAccessViews(pd3dDevice, m_pBlurVerticalTexture, 0, 0);
	m_pBlurVerticalTexture->SetRootParameterIndex(0, 17);
	m_pBlurVerticalTexture->SetRootParameterIndex(1, 16);
}

void CPostProcessingShader::CreateComputeResource(ID3D12Device* pd3dDevice, shared_ptr<CTexture>& pTexture)
{
	pTexture = make_shared<CTexture>(1, RESOURCE_TEXTURE2D, 0, 2, 1, 1);
	pTexture->CreateTexture(
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

	CScene::CreateShaderResourceViews(pd3dDevice, pTexture, 0, 0);
	CScene::CreateUnorderedAccessViews(pd3dDevice, pTexture, 0, 0);
	pTexture->SetRootParameterIndex(0, 17);
	pTexture->SetRootParameterIndex(1, 16);
}

void CPostProcessingShader::OnPrepareRenderTarget(ID3D12GraphicsCommandList* pd3dCommandList, int nRenderTargets, D3D12_CPU_DESCRIPTOR_HANDLE* pd3dRtvCPUHandles, D3D12_CPU_DESCRIPTOR_HANDLE* pd3dDsvCPUHandle)
{
	int nResources = m_pTexture->GetTextures();
	D3D12_CPU_DESCRIPTOR_HANDLE* pd3dAllRtvCPUHandles = new D3D12_CPU_DESCRIPTOR_HANDLE[nRenderTargets + nResources];

	for (int i = 0; i < nRenderTargets; i++)
	{
		pd3dAllRtvCPUHandles[i] = pd3dRtvCPUHandles[i];
		pd3dCommandList->ClearRenderTargetView(pd3dRtvCPUHandles[i], m_fClearValue, 0, NULL);
	}

	for (int i = 0; i < nResources; i++)
	{
		::SynchronizeResourceTransition(pd3dCommandList, GetTextureResource(i), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);

		D3D12_CPU_DESCRIPTOR_HANDLE d3dRtvCPUDescriptorHandle = GetRtvCPUDescriptorHandle(i);
		if (i == 2) 
		{ // 깊이값을 저장하는 렌더타겟
			FLOAT value[4] = { 1.0f,1.0f,1.0f,1.0f };
			pd3dCommandList->ClearRenderTargetView(d3dRtvCPUDescriptorHandle, value, 0, NULL);
		}
		else 
		{
			pd3dCommandList->ClearRenderTargetView(d3dRtvCPUDescriptorHandle, m_fClearValue, 0, NULL);
		}
		pd3dAllRtvCPUHandles[nRenderTargets + i] = d3dRtvCPUDescriptorHandle;
	}
	pd3dCommandList->OMSetRenderTargets(nRenderTargets + nResources, pd3dAllRtvCPUHandles, FALSE, pd3dDsvCPUHandle);

	if (pd3dAllRtvCPUHandles) delete[] pd3dAllRtvCPUHandles;
}

void CPostProcessingShader::OnPrepareRenderTargetForLight(ID3D12GraphicsCommandList* pd3dCommandList, int nRenderTargets, D3D12_CPU_DESCRIPTOR_HANDLE* pd3dRtvCPUHandles, D3D12_CPU_DESCRIPTOR_HANDLE* pd3dDsvCPUHandle)
{
	int nResources = m_pTexture->GetTextures();
	D3D12_CPU_DESCRIPTOR_HANDLE* pd3dAllRtvCPUHandles = new D3D12_CPU_DESCRIPTOR_HANDLE[1];

	for (int i = nResources - 1; i < nResources; ++i)
	{
		D3D12_CPU_DESCRIPTOR_HANDLE d3dRtvCPUDescriptorHandle = GetRtvCPUDescriptorHandle(i);

		pd3dAllRtvCPUHandles[i - nResources + 1] = d3dRtvCPUDescriptorHandle;
	}
	pd3dCommandList->OMSetRenderTargets(1, pd3dAllRtvCPUHandles, FALSE, pd3dDsvCPUHandle);

	if (pd3dAllRtvCPUHandles) delete[] pd3dAllRtvCPUHandles;
}

void CPostProcessingShader::OnPrepareRenderTarget2(ID3D12GraphicsCommandList* pd3dCommandList, int nRenderTargets, D3D12_CPU_DESCRIPTOR_HANDLE* pd3dRtvCPUHandles,
	D3D12_CPU_DESCRIPTOR_HANDLE* pd3dDsvCPUHandle, D3D12_CPU_DESCRIPTOR_HANDLE* pd3dshadowRTVDescriptorHandle)
{
	int nResources = m_pTexture->GetTextures();
	D3D12_CPU_DESCRIPTOR_HANDLE* pd3dAllRtvCPUHandles = new D3D12_CPU_DESCRIPTOR_HANDLE[nRenderTargets + nResources];

	for (int i = 0; i < nRenderTargets; i++)
	{
		pd3dAllRtvCPUHandles[i] = pd3dRtvCPUHandles[i];
		pd3dCommandList->ClearRenderTargetView(pd3dRtvCPUHandles[i], m_fClearValue, 0, NULL);
	}

	for (int i = 0; i < nResources; i++)
	{
		::SynchronizeResourceTransition(pd3dCommandList, GetTextureResource(i), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);

		D3D12_CPU_DESCRIPTOR_HANDLE d3dRtvCPUDescriptorHandle = GetRtvCPUDescriptorHandle(i);
		if (i == 2) { // 깊이값을 저장하는 렌더타겟
			FLOAT value[4] = { 1.0f,1.0f,1.0f,1.0f };
			pd3dCommandList->ClearRenderTargetView(*pd3dshadowRTVDescriptorHandle, value, 0, NULL);
			d3dRtvCPUDescriptorHandle = *pd3dshadowRTVDescriptorHandle;
		}
		else {
			pd3dCommandList->ClearRenderTargetView(d3dRtvCPUDescriptorHandle, m_fClearValue, 0, NULL);
		}
		pd3dAllRtvCPUHandles[nRenderTargets + i] = d3dRtvCPUDescriptorHandle;
	}
	pd3dCommandList->OMSetRenderTargets(nRenderTargets + nResources, pd3dAllRtvCPUHandles, FALSE, pd3dDsvCPUHandle);

	if (pd3dAllRtvCPUHandles) delete[] pd3dAllRtvCPUHandles;
}

void CPostProcessingShader::TransitionGBuffer(ID3D12GraphicsCommandList* pd3dCommandList, D3D12_RESOURCE_STATES stateBefore, D3D12_RESOURCE_STATES stateAfter, bool bAll)
{
	int nResources = m_pTexture->GetTextures();
	for (int i = 0; i < nResources - 1; i++) // 뒤에 1개 제외
	{
		::SynchronizeResourceTransition(pd3dCommandList, GetTextureResource(i), stateBefore, stateAfter);
	}

	if (bAll)
	{
		for (int i = nResources - 1; i < nResources; i++) // 뒤에 1개까지
		{
			::SynchronizeResourceTransition(pd3dCommandList, GetTextureResource(i), stateBefore, stateAfter);
		}
	}
}

void CPostProcessingShader::TransitionRenderTargetToCommonForLight(ID3D12GraphicsCommandList* pd3dCommandList)
{
	int nResources = m_pTexture->GetTextures();
	for (int i = nResources - 1; i < nResources; i++) // 뒤에 1개 제외
	{
		::SynchronizeResourceTransition(pd3dCommandList, GetTextureResource(i), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
	}
}

void CPostProcessingShader::TransitionShadowMapRenderTargetToCommon(ID3D12GraphicsCommandList* pd3dCommandList, int nTransition)
{
	int nResources = m_pShadowTextures->GetTextures();
	if (nTransition != 0) {
		nResources = nTransition;
	}

	for (int i = 0; i < nResources; i++)
	{
		::SynchronizeResourceTransition(pd3dCommandList, GetShadowTextureResource(i), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
	}
}

void CPostProcessingShader::TransitionCommonToRenderTarget(ID3D12GraphicsCommandList* pd3dCommandList)
{
	int nResources = m_pTexture->GetTextures();
	for (int i = 0; i < nResources; i++)
	{
		::SynchronizeResourceTransition(pd3dCommandList, GetTextureResource(i), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
	}
}

void CPostProcessingShader::Render(ID3D12GraphicsCommandList* pd3dCommandList, const shared_ptr<CCamera>& pCamera, const shared_ptr<CPlayer>& pPlayer)
{
	UpdatePipeLineState(pd3dCommandList, m_nRenderPipelineIndex);

	if (m_pTexture) m_pTexture->UpdateShaderVariables(pd3dCommandList);
	if (m_pShadowTextures) m_pShadowTextures->UpdateShaderVariables(pd3dCommandList);
	if (m_pNoiseTexture) m_pNoiseTexture->UpdateShaderVariables(pd3dCommandList);
	if (m_pBlurVerticalTexture) m_pBlurVerticalTexture->UpdateShaderVariables(pd3dCommandList);
	
	pd3dCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	pd3dCommandList->DrawInstanced(6, 1, 0, 0);
}

void CPostProcessingShader::PrepareDispatch(ID3D12GraphicsCommandList* pd3dCommandList)
{
	::SynchronizeResourceTransition(pd3dCommandList, m_pAmbientOcclusionTexture->GetResource(0), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	::SynchronizeResourceTransition(pd3dCommandList, m_pBlurHorizontalTexture->GetResource(0), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	::SynchronizeResourceTransition(pd3dCommandList, m_pBlurVerticalTexture->GetResource(0), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

void CPostProcessingShader::DispatchPostProcessing(ID3D12GraphicsCommandList * pd3dCommandList)
{
	if (m_nPipelineState == 1)
	{
		return;
	}

	if (m_vpd3dPipelineState[2])
	{
		pd3dCommandList->SetPipelineState(m_vpd3dPipelineState[2].Get());
	}
	UpdateShaderVariables(pd3dCommandList);

	if (m_pAmbientOcclusionTexture)
	{
		m_pAmbientOcclusionTexture->UpdateUavShaderVariable(pd3dCommandList, 16, 0);
	}
	if (m_pTexture)
	{
		m_pTexture->UpdateSrvShaderVariableForCompute(pd3dCommandList, 0, 0);
	}

	for (int i = 0; i < 1; i++)
	{

		pd3dCommandList->Dispatch(m_cxThreadGroups, m_cyThreadGroups, m_czThreadGroups);

		ID3D12Resource* pd3dSrvSource = m_pAmbientOcclusionTexture->GetResource(0);
		::SynchronizeResourceTransition(pd3dCommandList, pd3dSrvSource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	}
}

void CPostProcessingShader::DispatchBilateralBlurHorizontal(ID3D12GraphicsCommandList* pd3dCommandList)
{
	if (m_nPipelineState == 1)
	{
		return;
	}

	if (m_vpd3dPipelineState[3])
	{
		pd3dCommandList->SetPipelineState(m_vpd3dPipelineState[3].Get());
	}
	UpdateShaderVariables(pd3dCommandList);

	if (m_pBlurHorizontalTexture)
	{
		m_pBlurHorizontalTexture->UpdateUavShaderVariable(pd3dCommandList, 16, 0);
	}
	if (m_pAmbientOcclusionTexture)
	{
		m_pAmbientOcclusionTexture->UpdateUavShaderVariable(pd3dCommandList, 17, 0);
	}
	if (m_pTexture)
	{
		m_pTexture->UpdateSrvShaderVariableForCompute(pd3dCommandList, 0, 0);
	}

	for (int i = 0; i < 1; i++)
	{
		pd3dCommandList->Dispatch(m_cxThreadGroups, m_cyThreadGroups, m_czThreadGroups);

		ID3D12Resource* pd3dSource = m_pBlurHorizontalTexture->GetResource(0);
		::SynchronizeResourceTransition(pd3dCommandList, pd3dSource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	}
}

void CPostProcessingShader::DispatchBilateralBlurVertical(ID3D12GraphicsCommandList* pd3dCommandList)
{
	if (m_nPipelineState == 1)
	{
		return;
	}

	if (m_vpd3dPipelineState[4])
	{
		pd3dCommandList->SetPipelineState(m_vpd3dPipelineState[4].Get());
	}
	UpdateShaderVariables(pd3dCommandList);

	if (m_pBlurHorizontalTexture)
	{
		m_pBlurHorizontalTexture->UpdateUavShaderVariable(pd3dCommandList, 17, 0);
	}
	if (m_pBlurVerticalTexture)
	{
		m_pBlurVerticalTexture->UpdateUavShaderVariable(pd3dCommandList, 16, 0);
	}
	if (m_pTexture)
	{
		m_pTexture->UpdateSrvShaderVariableForCompute(pd3dCommandList, 0, 0);
	}

	for (int i = 0; i < 1; i++)
	{
		pd3dCommandList->Dispatch(m_cxThreadGroups, m_cyThreadGroups, m_czThreadGroups);

		ID3D12Resource* pd3dSource = m_pBlurVerticalTexture->GetResource(0);
		::SynchronizeResourceTransition(pd3dCommandList, pd3dSource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	}
}


void CPostProcessingShader::ShadowTextureWriteRender(ID3D12GraphicsCommandList* pd3dCommandList, const shared_ptr<CCamera>& pCamera, const shared_ptr<CPlayer>& pPlayer)
{
	CShader::Render(pd3dCommandList, pCamera, pPlayer, m_iShadowPipeLineIndex);

	if (m_pTexture) m_pTexture->UpdateShaderVariables(pd3dCommandList);
	if (m_pShadowTextures) m_pShadowTextures->UpdateShaderVariables(pd3dCommandList);

	pd3dCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	pd3dCommandList->DrawInstanced(6, 1, 0, 0);
}

void CPostProcessingShader::CreateShadowMapResource(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList, UINT nlight, D3D12_CPU_DESCRIPTOR_HANDLE d3dRtvCPUDescriptorHandle)
{
	int nLight = nlight;
	if (nLight >= MAX_LIGHTS) 
	{
		nLight = MAX_LIGHTS;
	}
	D3D12_DESCRIPTOR_HEAP_DESC d3dDescriptorHeapDesc;
	::ZeroMemory(&d3dDescriptorHeapDesc, sizeof(D3D12_DESCRIPTOR_HEAP_DESC));
	d3dDescriptorHeapDesc.NumDescriptors = nLight; // 빛의 개수만큼 렌더타겟을 생성?
	d3dDescriptorHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	d3dDescriptorHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	d3dDescriptorHeapDesc.NodeMask = 0;
	HRESULT hResult = pd3dDevice->CreateDescriptorHeap(&d3dDescriptorHeapDesc, __uuidof(ID3D12DescriptorHeap), (void**)m_pd3dShadowRtvDescriptorHeap.GetAddressOf());

	D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_pd3dShadowRtvDescriptorHeap->GetCPUDescriptorHandleForHeapStart();

	m_pShadowTextures = make_shared<CTexture>(nLight, RESOURCE_TEXTURE2D, 0, 1);

	DXGI_FORMAT pdxgiFormat = DXGI_FORMAT_R32_FLOAT;
	D3D12_CLEAR_VALUE d3dClearValue(pdxgiFormat, { 1.0f, 1.0f, 1.0f, 1.0f });

	for (UINT i = 0; i < nLight; i++)
	{
		m_pShadowTextures->CreateTexture(pd3dDevice, i, RESOURCE_TEXTURE2D, FRAME_BUFFER_WIDTH, FRAME_BUFFER_HEIGHT, 1, 0, pdxgiFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON, &d3dClearValue);

	}

	CreateShaderVariables(pd3dDevice, pd3dCommandList);
	CScene::CreateShaderResourceViews(pd3dDevice, m_pShadowTextures, 0, 11);

	D3D12_RENDER_TARGET_VIEW_DESC d3dRenderTargetViewDesc;
	d3dRenderTargetViewDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
	d3dRenderTargetViewDesc.Texture2D.MipSlice = 0;
	d3dRenderTargetViewDesc.Texture2D.PlaneSlice = 0;
	d3dRenderTargetViewDesc.Format = pdxgiFormat;

	for (UINT i = 0; i < nLight; i++)
	{
		ID3D12Resource* pd3dTextureResource = m_pShadowTextures->GetResource(i);
		pd3dDevice->CreateRenderTargetView(pd3dTextureResource, &d3dRenderTargetViewDesc, rtvHandle);
		m_vpShadowRtvCPUDescriptorHandles.push_back(make_unique<D3D12_CPU_DESCRIPTOR_HANDLE>(rtvHandle));
		rtvHandle.ptr += ::gnRtvDescriptorIncrementSize;
		/*pd3dDevice->CreateRenderTargetView(pd3dTextureResource, &d3dRenderTargetViewDesc, d3dRtvCPUDescriptorHandle);
		m_vpShadowRtvCPUDescriptorHandles.push_back(make_unique<D3D12_CPU_DESCRIPTOR_HANDLE>(d3dRtvCPUDescriptorHandle));
		d3dRtvCPUDescriptorHandle.ptr += ::gnRtvDescriptorIncrementSize;*/
	}
}

void CPostProcessingShader::CreateLightCamera(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList, CMainScene* scene)
{
	//vector<XMFLOAT3> positions = scene->GetLightPositions();
	//vector<XMFLOAT3> looks = scene->GetLightLooks();

	//XMFLOAT3 xmf3Right = XMFLOAT3(1.0f, 0.0f, 0.0f);

	//XMFLOAT4X4 xmf4x4ToTexture = {
	//	0.5f, 0.0f, 0.0f, 0.0f,
	//	0.0f, -0.5f, 0.0f, 0.0f,
	//	0.0f, 0.0f, 1.0f, 0.0f,
	//	0.5f, 0.5f, 0.0f, 1.0f };

	//XMMATRIX xmProjectionToTexture = XMLoadFloat4x4(&xmf4x4ToTexture);
	//XMMATRIX xmmtxViewProjection;

	//for (int i = 0;i < scene->m_nLights;++i) {
	//	m_pLightCamera.push_back(make_shared<CCamera>());

	//	XMFLOAT3 xmf3Up = Vector3::CrossProduct(looks[i], xmf3Right);
	//	XMFLOAT3 lookAtPosition = Vector3::Add(positions[i], looks[i]);
	//	m_pLightCamera[i]->GenerateViewMatrix(positions[i], lookAtPosition, xmf3Up);
	//	if(i >= MAX_SURVIVOR)
	//	{
	//		m_pLightCamera[i]->GenerateProjectionMatrix(1.01f, 5.0f, ASPECT_RATIO, 90.0f);	//[0513] 근평면이 있어야  그림자를 그림
	//	}
	//	m_pLightCamera[i]->GenerateFrustum();
	//	m_pLightCamera[i]->MultiplyViewProjection();

	//	XMFLOAT4X4 viewProjection = m_pLightCamera[i]->GetViewProjection();
	//	xmmtxViewProjection = XMLoadFloat4x4(&viewProjection);
	//	XMStoreFloat4x4(&scene->m_pLights[i].m_xmf4x4ViewProjection, XMMatrixTranspose(xmmtxViewProjection * xmProjectionToTexture));

	//	m_pLightCamera[i]->CreateShaderVariables(pd3dDevice, pd3dCommandList);
	//}

	//// 빛의 카메라 파티션 설정
	//unique_ptr<PartitionInsStandardShader> PtShader(static_cast<PartitionInsStandardShader*>(scene->m_vPreRenderShader[PARTITION_SHADER].release()));
	//auto vBB = PtShader->GetPartitionBB();

	//for (int i = 0; i < scene->m_nLights;++i) {
	//	BoundingBox camerabb;
	//	camerabb.Center = m_pLightCamera[i]->GetPosition();
	//	camerabb.Extents = XMFLOAT3(0.1f, 0.1f, 0.1f);
	//	int curFloor = static_cast<int>(std::floor(camerabb.Center.y / 4.5f));

	//	m_pLightCamera[i]->SetFloor(curFloor);
	//	for (int bbIdx = 0; bbIdx < vBB.size();++bbIdx) {
	//		if (vBB[bbIdx]->Intersects(camerabb)) {
	//			m_pLightCamera[i]->SetPartition(bbIdx);
	//			break;
	//		}
	//	}
	//}

	//scene->m_vPreRenderShader[PARTITION_SHADER].reset(PtShader.release());
}


void CPostProcessingShader::OnShadowPrepareRenderTarget(ID3D12GraphicsCommandList* pd3dCommandList, int nclearcount)
{
	int nResources = m_pShadowTextures->GetTextures();
	if (nclearcount != 0) {
		nResources = nclearcount;
	}
	FLOAT clearValue[4] = { 1.0f,1.0f,1.0f,1.0f };
	for (int i = 0; i < nResources; i++)
	{
		::SynchronizeResourceTransition(pd3dCommandList, GetShadowTextureResource(i), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);

		D3D12_CPU_DESCRIPTOR_HANDLE d3dRtvCPUDescriptorHandle = GetShadowRtvCPUDescriptorHandle(i);
		pd3dCommandList->ClearRenderTargetView(d3dRtvCPUDescriptorHandle, clearValue, 0, NULL);
	}
}