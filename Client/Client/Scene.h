#pragma once
#include "../GameLimits.h"
#include "Timer.h"
#include "Shader.h"
#include "TextureBlendObject.h"

class CGenerateSSAOShader;
class CBlurSSAOComputeShader;

// m_vShader 쉐이더에 AddDefaultObject 시에 접근할 각 쉐이더 인덱스를 의미
#define STANDARD_SHADER 0
#define INSTANCE_STANDARD_SHADER 1
#define SKINNEDANIMATION_STANDARD_SHADER 2

// m_vForwardRenderShader
#define TRANSPARENT_SHADER 0 // 투명객체에 대한 쉐이더는 항상 후순위로 배치
#define PARTICLE_SHADER 1 
#define TEXTUREBLEND_SHADER 2
#define TRAIL_SHADER 3
#define USER_INTERFACE_SHADER 4
#define OUT_LINE_SHADER 5

// m_vPartitionShader
#define PARTITION_SHADER 0


//#define NOTRENDERING_SHADER 3

// m_vMesh 메쉬에 접근할 각 인덱스를 의미
#define HEXAHEDRONMESH 0

#define MAX_LIGHTS						24 + MAX_SURVIVOR

#define POINT_LIGHT						1
#define SPOT_LIGHT						2
#define DIRECTIONAL_LIGHT				3

#define WALK_SOUND_DISTANCE 16.0f

struct LIGHT
{
	XMFLOAT4X4 m_xmf4x4ViewProjection = Matrix4x4::Identity();
	XMFLOAT4 m_xmf4Ambient;
	XMFLOAT4 m_xmf4Diffuse;
	XMFLOAT4 m_xmf4Specular;
	XMFLOAT3 m_xmf3Position;
	bool m_bEnable = false;
	float m_fFalloff;
	XMFLOAT3 m_xmf3Direction;
	float m_fTheta; //cos(m_fTheta)
	XMFLOAT3 m_xmf3Attenuation;
	float m_fPhi; //cos(m_fPhi)
	int m_nType;
	float m_fRange;
	float padding;
};

struct LIGHTS
{
	LIGHT m_pLights[MAX_LIGHTS];
	XMFLOAT4 m_xmf4GlobalAmbient;
	int m_nLights;
	float bias;
};

struct FrameTimeInfo
{
	float time = 0.0f;
	float localTime = 0.0f;
	float usePattern = -1.0f; // shaders에서 패턴텍스처를 사용하는가? 0보다 큰값이면 사용하는 것. 최적화 필요. 쉐이더를 나누면 분기문 줄일수있음.

	float fTrackingTime = 0.0f;

	// Occlusion Info
	float gfScale = 0.2f;
	float gfBias = 0.002f;
	float gfIntesity = 1.0f;
};

class CPlayer;
class CLoadedModelInfo;
class CTeleportObject;

class CScene
{
public:
	CScene() = default;
	virtual ~CScene() = default;

	virtual bool OnProcessingMouseMessage(HWND hWnd, UINT nMessageID, WPARAM wParam, LPARAM lParam) { return false; }
	virtual bool OnProcessingKeyboardMessage(HWND hWnd, UINT nMessageID, WPARAM wParam, LPARAM lParam) { return false; }

	virtual void CreateShaderVariables(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList) {}
	virtual void BuildObjects(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList, int mainPlayerId) {}
	virtual void ReleaseUploadBuffers() {}
	virtual bool ProcessInput(UCHAR* pKeysBuffer) { return false; }
	virtual void AnimateObjects(float fElapsedTime, float fCurTime) {}
	// Command list를 Reset한 뒤 Scene 공통 GPU 상태를 한 번 설정한다.
	virtual void PrepareCommandListState(ID3D12GraphicsCommandList* pd3dCommandList);
	virtual void PrepareRender(ID3D12GraphicsCommandList* pd3dCommandList, const shared_ptr<CCamera>& pCamera) {}
	virtual void Render(ID3D12GraphicsCommandList* pd3dCommandList, const shared_ptr<CCamera>& pCamera, int nPipelineState) {}
	virtual void RenderLoading(ID3D12GraphicsCommandList* pd3dCommandList) {}

	virtual void SetParticleTest(float fCurTime) {}
	virtual void ParticleReadByteTask() {}

	void SetPlayer(shared_ptr<CPlayer> pPlayer, int nIndex) { m_apPlayer[nIndex] = pPlayer; }
	void SetMainPlayer(const shared_ptr<CPlayer>& pMainPlayer) { m_pMainPlayer = pMainPlayer; }
	void SetRTVDescriptorHeap(const ComPtr<ID3D12DescriptorHeap>& d3dRtvDescriptorHeap) { m_d3dRtvDescriptorHeap = d3dRtvDescriptorHeap; }
	void SetNumOfSwapChainBuffers(UINT nSwapChainBuffers) { m_nSwapChainBuffers = nSwapChainBuffers; }

	static void CreateCbvSrvUavDescriptorHeaps(ID3D12Device* pd3dDevice, int nConstantBufferViews, int nShaderResourceViews, int nUnorderedAccessViews);
	static D3D12_GPU_DESCRIPTOR_HANDLE CreateConstantBufferViews(ID3D12Device* pd3dDevice, int nConstantBufferViews, ID3D12Resource* pd3dConstantBuffers, UINT nStride);
	static void CreateShaderResourceViews(ID3D12Device* pd3dDevice, const shared_ptr<CTexture>& pTexture, UINT nDescriptorHeapIndex, UINT nRootParameterStartIndex);
	static D3D12_GPU_DESCRIPTOR_HANDLE CreateShaderResourceView(ID3D12Device* pd3dDevice, ID3D12Resource* pd3dResource, DXGI_FORMAT dxgiSrvFormat);
	static void CreateUnorderedAccessViews(ID3D12Device* pd3dDevice, const shared_ptr<CTexture>& pTexture, UINT nDescriptorHeapIndex, UINT nRootParameterStartIndex);

	const shared_ptr<CPlayer>& GetPlayer(int nIndex) const { return m_apPlayer[nIndex]; }
	ComPtr<ID3D12RootSignature> GetGraphicsRootSignature() const { return m_pd3dGraphicsRootSignature; }

protected:
	void CreateGraphicsRootSignature(ID3D12Device* pd3dDevice);

	std::array<shared_ptr<CPlayer>, MAX_CLIENT> m_apPlayer;
	std::shared_ptr<CPlayer> m_pMainPlayer;
	ComPtr<ID3D12DescriptorHeap> m_d3dRtvDescriptorHeap;
	ComPtr<ID3D12RootSignature> m_pd3dGraphicsRootSignature;
	UINT m_nSwapChainBuffers;

private:
	// 모든 Scene이 공유하는 descriptor heap을 선형으로 할당하기 위한 내부 상태이다.
	static ComPtr<ID3D12DescriptorHeap> m_pd3dCbvSrvUavDescriptorHeap;

	static D3D12_CPU_DESCRIPTOR_HANDLE m_d3dCbvCPUDescriptorStartHandle;
	static D3D12_GPU_DESCRIPTOR_HANDLE m_d3dCbvGPUDescriptorStartHandle;
	static D3D12_CPU_DESCRIPTOR_HANDLE m_d3dSrvCPUDescriptorStartHandle;
	static D3D12_GPU_DESCRIPTOR_HANDLE m_d3dSrvGPUDescriptorStartHandle;
	static D3D12_CPU_DESCRIPTOR_HANDLE m_d3dUavCPUDescriptorStartHandle;
	static D3D12_GPU_DESCRIPTOR_HANDLE m_d3dUavGPUDescriptorStartHandle;

	static D3D12_CPU_DESCRIPTOR_HANDLE m_d3dCbvCPUDescriptorNextHandle;
	static D3D12_GPU_DESCRIPTOR_HANDLE m_d3dCbvGPUDescriptorNextHandle;
	static D3D12_CPU_DESCRIPTOR_HANDLE m_d3dSrvCPUDescriptorNextHandle;
	static D3D12_GPU_DESCRIPTOR_HANDLE m_d3dSrvGPUDescriptorNextHandle;
	static D3D12_CPU_DESCRIPTOR_HANDLE m_d3dUavCPUDescriptorNextHandle;
	static D3D12_GPU_DESCRIPTOR_HANDLE m_d3dUavGPUDescriptorNextHandle;

	static int m_nCntCbv;
	static int m_nCntSrv;
	static int m_nCntUav;
};

/// <CScene>
/////////////////////////////////////////////////////////////////////
/// <CScene - CLobbyScene>

class CLobbyScene : public CScene
{
public:
	CLobbyScene(HWND hWnd, weak_ptr<CCamera>& pCamera);
	~CLobbyScene() override = default;

	bool OnProcessingMouseMessage(HWND hWnd, UINT nMessageID, WPARAM wParam, LPARAM lParam) override;
	bool OnProcessingKeyboardMessage(HWND hWnd, UINT nMessageID, WPARAM wParam, LPARAM lParam) override;
	bool ProcessInput(UCHAR* pKeysBuffer) override;

	void BuildObjects(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList, int mainPlayerId) override;
	void AnimateObjects(float fElapsedTime, float fCurTime) override;
	void PrepareRender(ID3D12GraphicsCommandList* pd3dCommandList, const shared_ptr<CCamera>& pCamera) override;
	void Render(ID3D12GraphicsCommandList* pd3dCommandList, const shared_ptr<CCamera>& pCamera, int nPipelineState) override;
	void RenderLoading(ID3D12GraphicsCommandList* pd3dCommandList) override;

	void UpdateShaderMainPlayer(int nMainClientId);
	int GetSelectedSlot() const { return m_nSelectedSlot; }

private:
	enum LOBBY_SHADER
	{
		LOBBY_SATANDARD_SHADER = 0,
		LOBBY_UI_SHADER
	};

	void ProcessButtonDown(const POINT& ptCursorPos, std::shared_ptr<CLobbyUserInterfaceShader>& pLobbyUIShader);
	void ProcessClickBorder(const POINT& ptCursorPos, std::shared_ptr<CLobbyUserInterfaceShader>& pLobbyUIShader);
	static bool CheckCursor(POINT ptCursor, float fCenterX, float fCenterY, float fWidth, float fHeight);

	vector<shared_ptr<CShader>> m_vpShader;
	unique_ptr<CFullScreenProcessingShader> m_vFullScreenProcessingShader;
	shared_ptr<CCamera> m_pCamera;
	HWND m_hWnd;
	POINT m_ptCursor = {};
	int m_nSelectedSlot = -1;
};

/// <CScene - CLobbyScene>
/////////////////////////////////////////////////////////////////////
/// <CScene - CMainScene>

class CBlurComputeShader;
class CTextureToScreenShader;
class UiOverlayRenderer;
struct UiOverlayFrameData;

class CMainScene : public CScene
{
public:
	CMainScene();
	virtual ~CMainScene() override;

	bool OnProcessingMouseMessage(HWND hWnd, UINT nMessageID, WPARAM wParam, LPARAM lParam) override;
	bool OnProcessingKeyboardMessage(HWND hWnd, UINT nMessageID, WPARAM wParam, LPARAM lParam) override;
	bool ProcessInput(UCHAR* pKeysBuffer) override;

	void CreateShaderVariables(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList) override;
	void BuildObjects(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList, int mainPlayerId) override;
	void AnimateObjects(float fElapsedTime, float fCurTime) override;

	// Render Functions ordered by render order
	void PrevRenderTask(ID3D12GraphicsCommandList* pd3dCommandList);
	// Command list를 Reset한 뒤 공통 root signature와 descriptor heap을 한 번 설정한다.
	void PrepareCommandListState(ID3D12GraphicsCommandList* pd3dCommandList) override;
	void PrepareRender(ID3D12GraphicsCommandList* pd3dCommandList, const shared_ptr<CCamera>& pCamera) override;
	void Render(ID3D12GraphicsCommandList* pd3dCommandList, const shared_ptr<CCamera>& pCamera, int nPipelineState) override;
	void ShadowRender(ID3D12GraphicsCommandList* pd3dCommandList, const shared_ptr<CCamera>& pCamera, int nPipelineState);
	void AmbientOcclusionRender(
		ID3D12GraphicsCommandList* pd3dCommandList,
		const shared_ptr<CCamera>& pCamera,
		D3D12_CPU_DESCRIPTOR_HANDLE d3dRtvCPUDescriptorHandle);
	void PostProcessingRender(
		ID3D12GraphicsCommandList* pd3dCommandList,
		const shared_ptr<CCamera>& pCamera,
		D3D12_CPU_DESCRIPTOR_HANDLE d3dRtvCPUDescriptorHandle);
	void ForwardRender(
		int nGameState,
		ID3D12GraphicsCommandList* pd3dCommandList,
		const std::shared_ptr<CCamera>& pCamera);
	void BlurDispatch(
		ID3D12GraphicsCommandList* pd3dCommandList,
		const shared_ptr<CCamera>& pCamera,
		D3D12_CPU_DESCRIPTOR_HANDLE d3dRtvCPUDescriptorHandle);
	void FullScreenProcessingRender(ID3D12GraphicsCommandList* pd3dCommandList);
	void BuildUiOverlayFrameGeometry(
		const UiOverlayFrameData& frameData,
		const XMFLOAT2& viewportSize
	);
	void RenderUiOverlay(
		ID3D12GraphicsCommandList* pd3dCommandList,
		D3D12_CPU_DESCRIPTOR_HANDLE renderTargetView
	);

	void ReleaseObjects();
	void ReleaseShaderVariables();
	void ReleaseUploadBuffers() override;
	void SetParticleTest(float fCurTime) override;
	void ParticleReadByteTask() override;

	void ClampLightCount(int maximumLightCount);
	vector<shared_ptr<CLightCamera>>& GetLightCamera() { return m_pLightCamera; }
	CPostProcessingShader* GetPostProcessingShader() const { return m_pPostProcessingShader.get(); }
	CShader* GetForwardRenderShader(int shaderIndex) const;

	bool IsSsaoEnabled() const;
	float GetScale() const { return m_pcbMappedTime->gfScale; }
	float GetIntesity() const { return m_pcbMappedTime->gfIntesity; }
	float GetBias() const { return m_pcbMappedTime->gfBias; }

private:
	enum class SsaoMode
	{
		Disabled,
		Raw,
		Blurred,
	};

	void LoadScene(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList);
	void AddDefaultObject(
		ID3D12Device* pd3dDevice,
		ID3D12GraphicsCommandList* pd3dCommandList,
		ObjectType type,
		XMFLOAT3 position,
		int shader,
		int mesh);
	void BuildLights(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList);
	void UpdateShaderVariables(ID3D12GraphicsCommandList* pd3dCommandList);
	void ShadowPreRender(ID3D12GraphicsCommandList* pd3dCommandList, const shared_ptr<CCamera>& pCamera, int nPipelineState);

	void CycleSsaoMode();
	bool IsSsaoBlurEnabled() const { return m_ssaoMode == SsaoMode::Blurred; }
	void UpdatePostProcessingPipelineForSsaoMode();
	void BindAmbientOcclusionForComposite(ID3D12GraphicsCommandList* pd3dCommandList);
	void RestoreRawAmbientOcclusionResourceState(ID3D12GraphicsCommandList* pd3dCommandList);

	vector<unique_ptr<CShader>> m_vShader;
	vector<unique_ptr<CShader>> m_vForwardRenderShader;
	vector<unique_ptr<CShader>> m_vPreRenderShader;
	vector<shared_ptr<TextureBlendObject>> m_vTextureBlendObjects;
	vector<shared_ptr<CMesh>> m_vMesh;
	vector<shared_ptr<CLightCamera>> m_pLightCamera;
	vector<XMFLOAT3> m_xmf3lightPositions;
	vector<XMFLOAT3> m_xmf3lightLooks;

	shared_ptr<CMaterial> mt_Electirc;
	shared_ptr<CGenerateSSAOShader> m_pGenerateSSAOShader;
	shared_ptr<CPostProcessingShader> m_pPostProcessingShader;
	shared_ptr<CBlurSSAOComputeShader> m_pBlurSSAOComputeShader;
	shared_ptr<CBlurComputeShader> m_pBlurComputeShader;
	shared_ptr<CTextureToScreenShader> m_pTextureToScreenShaderShader;
	unique_ptr<CFullScreenProcessingShader> m_vFullScreenProcessingShader;
	unique_ptr<UiOverlayRenderer> mUiOverlayRenderer;

	ComPtr<ID3D12Resource> m_pd3dcbLights;
	ComPtr<ID3D12Resource> m_pd3dcbTime;
	D3D12_GPU_DESCRIPTOR_HANDLE m_d3dLightCbvGPUDescriptorHandle;
	D3D12_GPU_DESCRIPTOR_HANDLE m_d3dTimeCbvGPUDescriptorHandle;
	LIGHT* m_pLights = nullptr;
	LIGHTS* m_pcbMappedLights = nullptr;
	FrameTimeInfo* m_pcbMappedTime = nullptr;
	XMFLOAT4 m_xmf4GlobalAmbient;
	int m_nLights = 0;
	float m_fElapsedTime = 0.0f;
	SsaoMode m_ssaoMode = SsaoMode::Blurred;
};

