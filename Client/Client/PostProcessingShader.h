#pragma once
#include "Shader.h"

class CMainScene;

enum SHADER_INDEX
{
	PostProcessing = 0,
	PostProcessingWithSSAO = 1,
};

class CPostProcessingShader : public CShader
{
public:
	CPostProcessingShader();
	virtual ~CPostProcessingShader();

	virtual D3D12_INPUT_LAYOUT_DESC CreateInputLayout();
	virtual D3D12_DEPTH_STENCIL_DESC CreateDepthStencilState();

	virtual D3D12_SHADER_BYTECODE CreateVertexShader();
	virtual D3D12_SHADER_BYTECODE CreatePixelShader();
	virtual D3D12_RASTERIZER_DESC CreateRasterizerState();
	virtual D3D12_SHADER_BYTECODE CreateComputeShader(ID3DBlob** ppd3dShaderBlob);;

	// 일반 셰이더 생성
	virtual void CreateShader(
		ID3D12Device* pd3dDevice,
		ID3D12GraphicsCommandList* pd3dCommandList,
		ID3D12RootSignature* pd3dGraphicsRootSignature,
		UINT nRenderTargets = 1,
		DXGI_FORMAT* pdxgiRtvFormats = nullptr,
		DXGI_FORMAT dxgiDsvFormat = DXGI_FORMAT_D24_UNORM_S8_UINT
	);
	// 컴퓨트 셰이더 생성
	virtual void CreateComputeShader(
		ID3D12Device* pd3dDevice,
		ID3D12GraphicsCommandList* pd3dCommandList,
		ID3D12RootSignature* pd3dRootSignature
	);
	virtual void CreateResourcesAndRtvsSrvs(
		ID3D12Device* pd3dDevice,
		ID3D12GraphicsCommandList* pd3dCommandList,
		UINT nRenderTargets, DXGI_FORMAT* pdxgiFormats,
		D3D12_CPU_DESCRIPTOR_HANDLE d3dRtvCPUDescriptorHandle
	);
	void CreateComputeResource(ID3D12Device* pd3dDevice, shared_ptr<CTexture>& pTexture);

	virtual void OnPrepareRenderTarget(ID3D12GraphicsCommandList* pd3dCommandList, int nRenderTargets, D3D12_CPU_DESCRIPTOR_HANDLE* pd3dRtvCPUHandles, D3D12_CPU_DESCRIPTOR_HANDLE* pd3dDsvCPUHandle);
	void OnPrepareRenderTargetForLight(ID3D12GraphicsCommandList* pd3dCommandList, int nRenderTargets, D3D12_CPU_DESCRIPTOR_HANDLE* pd3dRtvCPUHandles, D3D12_CPU_DESCRIPTOR_HANDLE* pd3dDsvCPUHandle);
	virtual void OnPrepareRenderTarget2(ID3D12GraphicsCommandList* pd3dCommandList, int nRenderTargets,
		D3D12_CPU_DESCRIPTOR_HANDLE* pd3dRtvCPUHandles, D3D12_CPU_DESCRIPTOR_HANDLE* pd3dDsvCPUHandle, D3D12_CPU_DESCRIPTOR_HANDLE* pd3dshadowRTVDescriptorHandle);
	virtual void TransitionGBuffer(ID3D12GraphicsCommandList* pd3dCommandList, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after, bool bAll = false);
	virtual void TransitionRenderTargetToCommonForLight(ID3D12GraphicsCommandList* pd3dCommandList);
	virtual void TransitionCommonToRenderTarget(ID3D12GraphicsCommandList* pd3dCommandList);

	virtual void Render(ID3D12GraphicsCommandList* pd3dCommandList, const shared_ptr<CCamera>& pCamera, const shared_ptr<CPlayer>& pPlayer);

	void PrepareDispatch(ID3D12GraphicsCommandList* pd3dCommandList);
	virtual void DispatchPostProcessing(ID3D12GraphicsCommandList* pd3dCommandList);
	virtual void DispatchBilateralBlurHorizontal(ID3D12GraphicsCommandList* pd3dCommandList);
	virtual void DispatchBilateralBlurVertical(ID3D12GraphicsCommandList* pd3dCommandList);
protected:
	shared_ptr<CTexture> m_pTexture;

	vector<unique_ptr<D3D12_CPU_DESCRIPTOR_HANDLE>> m_vpRtvCPUDescriptorHandles;

	FLOAT m_fClearValue[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

	ComPtr<ID3D12DescriptorHeap> m_pd3dDsvDescriptorHeap;
	ComPtr<ID3D12Resource> m_pd3dDepthBuffer;
	vector<unique_ptr<D3D12_CPU_DESCRIPTOR_HANDLE>> m_vpDsvDescriptorCPUHandles;

public:
	shared_ptr<CTexture>& GetTexture() { return(m_pTexture); }
	ID3D12Resource* GetTextureResource(UINT nIndex) { return(m_pTexture->GetResource(nIndex)); }

	D3D12_CPU_DESCRIPTOR_HANDLE GetRtvCPUDescriptorHandle(UINT nIndex) { return(*m_vpRtvCPUDescriptorHandles[nIndex]); }
	D3D12_CPU_DESCRIPTOR_HANDLE GetDsvCPUDesctriptorHandle(UINT nIndex) { return (*m_vpDsvDescriptorCPUHandles[nIndex]); }

	//ShadowMap Processing
protected:
	shared_ptr<CTexture> m_pShadowTextures;
	//vector<shared_ptr<CLightCamera>> m_pLightCamera;

	vector<unique_ptr<D3D12_CPU_DESCRIPTOR_HANDLE>> m_vpShadowRtvCPUDescriptorHandles;
	vector<unique_ptr<D3D12_CPU_DESCRIPTOR_HANDLE>> m_vpShadowDsvDescriptorCPUHandles;

	ComPtr<ID3D12DescriptorHeap> m_pd3dShadowRtvDescriptorHeap;
	ComPtr<ID3D12DescriptorHeap> m_pd3dShadowDsvDescriptorHeap;
	ComPtr<ID3D12Resource> m_pd3dShadowDepthBuffer;

	const UINT m_iShadowPipeLineIndex = 1;
public:
	shared_ptr<CTexture> GetShadowTexture() { return(m_pShadowTextures); }
	ID3D12Resource* GetShadowTextureResource(UINT nIndex) { return(m_pShadowTextures->GetResource(nIndex)); }

	D3D12_CPU_DESCRIPTOR_HANDLE GetShadowRtvCPUDescriptorHandle(UINT nIndex) { return(*m_vpShadowRtvCPUDescriptorHandles[nIndex]); }
	D3D12_CPU_DESCRIPTOR_HANDLE GetShadowDsvCPUDesctriptorHandle(UINT nIndex) { return (*m_vpShadowDsvDescriptorCPUHandles[nIndex]); }

	void CreateShadowMapResource(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList, UINT nlight, D3D12_CPU_DESCRIPTOR_HANDLE d3dRtvCPUDescriptorHandle);
	void OnShadowPrepareRenderTarget(ID3D12GraphicsCommandList* pd3dCommandList, int nclearcount = 0);
	void ShadowTextureWriteRender(ID3D12GraphicsCommandList* pd3dCommandList, const shared_ptr<CCamera>& pCamera, const shared_ptr<CPlayer>& pPlayer);

	void TransitionShadowMapRenderTargetToCommon(ID3D12GraphicsCommandList* pd3dCommandList, int nTransition = 0);

	void CreateLightCamera(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList, CMainScene* scene);
	//vector<shared_ptr<CCamera>>& GetLightCamera() { return  m_pLightCamera; }

public:
	void SetPipelineIndex(UINT nIndex) { m_nRenderPipelineIndex = nIndex; }
	UINT GetPipelineIndex() const { return m_nRenderPipelineIndex; }
private:
	shared_ptr<CTexture> m_pNoiseTexture;
	UINT m_nRenderPipelineIndex = 0;
	UINT m_nComputePipelineStartIndex = 0;

	ComPtr<ID3DBlob> m_pd3dComputeShaderBlob;
	shared_ptr<CTexture> m_pAmbientOcclusionTexture;
	shared_ptr<CTexture> m_pBlurHorizontalTexture;
	shared_ptr<CTexture> m_pBlurVerticalTexture;

	UINT m_cxThreadGroups = 0;
	UINT m_cyThreadGroups = 0;
	UINT m_czThreadGroups = 0;
};