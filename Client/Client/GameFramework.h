#pragma once
#include "Timer.h"
#include "Scene.h"
#include "TCPClient.h"

struct CB_FRAMEWORK_INFO
{
	float m_fCurrentTime;
	float m_fElapsedTime;
	float m_fSecondsPerFirework = 1.0f;
	int m_nFlareParticlesToEmit = -1;
	XMFLOAT3 m_xmf3Gravity = XMFLOAT3(0.0f, -9.8f, 0.0f);
	int m_nMaxFlareType2Particles = -1;
};

//constexpr size_t SWAPCHAIN_BUFFER_NUM = 2;
//class TextObject;

constexpr UINT WM_CREATE_TCP{ WM_USER + 2 };
constexpr UINT WM_END_GAME{ WM_USER + 3 };
constexpr UINT WM_START_GAME{ WM_USER + 4 };
constexpr UINT BUTTON_CREATE_TCP_ID{ 1 };
constexpr UINT EDIT_INPUT_ADDRESS_ID{ 2 };

class CGameFramework
{
public:
	// 생성/소멸
	CGameFramework();
	~CGameFramework();

	// Lifecycle
	bool OnCreate(HINSTANCE hInstance, HWND hMainWnd);
	void OnDestroy();
	void BuildObjects();
	void ReleaseObjects();

	// Entry UI
	void CreateEntryWindow(HWND hWnd);
	void OnDestroyEntryWindow();
	void OnButtonClick(HWND hWnd);

	// DX12 Setup
	void CreateSwapChain();
	void CreateDirect3DDevice();
	void CreateCommandQueueAndList();
	void CreateRtvAndDsvDescriptorHeaps();
	void CreateRenderTargetViews();
	void CreateDepthStencilView();
	void ChangeSwapChainState();

	// Frame
	void ProcessInput();
	void AnimateObjects();
	void AnimateEnding();
	void PreRenderTasks(shared_ptr<CMainScene>& pMainScene);
	void FrameAdvance();
	void LoadingRender();
    void ExecuteCommandListAndWaitForGpu();
	void WaitForGpuComplete();
	void MoveToNextFrame();
	void UpdateFrameworkShaderVariable();

	// Window message dispatch
	void OnProcessingWindowMessage(HWND hWnd, UINT nMessageID, WPARAM wParam, LPARAM lParam);
	void OnProcessingCommandMessage(HWND hWnd, UINT nMessageID, WPARAM wParam, LPARAM lParam);
	void OnProcessingSocketMessage(HWND hWnd, UINT nMessageID, WPARAM wParam, LPARAM lParam);
	void OnProcessingMouseMessage(HWND hWnd, UINT nMessageID, WPARAM wParam, LPARAM lParam);
	void OnProcessingKeyboardMessage(HWND hWnd, UINT nMessageID, WPARAM wParam, LPARAM lParam);
	void OnProcessingEndGameMessage(WPARAM& wParam);

	// Text rendering
	void PrepareDrawText();
	void RenderTextUI();

	// Interface
	INT8 GetClientIdFromTcpClient() const { return m_pTcpClient->GetMainClientId(); }
	void SetPlayerObjectOfClient(int nClientId);

	void SetConnected(bool bConnected) { m_bConnected = bConnected; }
	bool IsConnected() const { return m_bConnected; }
	bool IsTcpClient() const { return m_bTcpClient; }
	void SetMousePoint(POINT ptMouse) { m_ptOldCursorPos = ptMouse; }
	void SetGameState(int nGameState) { m_nGameState = nGameState; }

	// Static Interface
	static UCHAR* GetKeysBuffer();
	static int GetMainClientId() { return m_nMainClientId; }
	static int GetSwapChainNum() { return m_nSwapChainBuffers; }
	static POINT GetClientWindowSize();

	// Shared public state (legacy)
	static std::shared_ptr<CPlayer> m_pMainPlayer; // 클라이언트ID에 해당하는 인덱스가 해당 클라이언트의 Main플레이어로 설정된다
	static shared_ptr<CPlayer>& GetMainPlayer() { return m_pMainPlayer; }

	static ComPtr<IDWriteTextFormat> m_idwGameCountTextFormat;
	static ComPtr<IDWriteTextFormat> m_idwSpeakerTextFormat;

private:
    void BuildLobbyObjects();
    void BuildMainObjects();
    void BindPlayersToTcpClient();

	/* 윈도우 플랫폼 관련 멤버 */
	HINSTANCE m_hInstance = nullptr;
	HWND m_hWnd = nullptr;

	static int m_nWndClientWidth;
	static int m_nWndClientHeight;

	_TCHAR m_pszFrameRate[200] = {};

	/* Input 관련 */
	static UCHAR m_pKeysBuffer[256];
	POINT m_ptOldCursorPos = {};

	/* DX12 멤버 */
	ComPtr<IDXGIFactory4> m_dxgiFactory;
	ComPtr<IDXGISwapChain3> m_dxgiSwapChain;
	ComPtr<ID3D12Device> m_d3d12Device;
	bool m_bMsaa4xEnable = false;
	UINT m_nMsaa4xQualityLevels = 0;

	static const UINT m_nSwapChainBuffers = 2;
	UINT m_nSwapChainBufferIndex = 0;

	std::array<ComPtr<ID3D12Resource>, m_nSwapChainBuffers> m_d3dSwapChainBackBuffers;
	ComPtr<ID3D12DescriptorHeap> m_d3dRtvDescriptorHeap;
	std::array<D3D12_CPU_DESCRIPTOR_HANDLE, m_nSwapChainBuffers> m_pd3dSwapChainBackBufferRTVCPUHandles{};

	ComPtr<ID3D12Resource> m_d3dDepthStencilBuffer;
	ComPtr<ID3D12DescriptorHeap> m_d3dDsvDescriptorHeap;

	ComPtr<ID3D12CommandAllocator> m_d3dCommandAllocator[m_nSwapChainBuffers];
	ComPtr<ID3D12CommandQueue> m_d3dCommandQueue;
	ComPtr<ID3D12GraphicsCommandList> m_d3dCommandList;

	ComPtr<ID3D12Fence> m_d3dFence;
	std::array<UINT64, m_nSwapChainBuffers>	m_nFenceValues{};
	HANDLE m_hFenceEvent = nullptr;

#if defined(_DEBUG)
	ID3D12Debug* m_pd3dDebugController = nullptr;
#endif

	/* 씬과 플레이어 */
	shared_ptr<CScene> m_pScene;
	std::array<shared_ptr<CPlayer>, MAX_CLIENT> m_apPlayer; // 클라이언트ID와 인덱스는 동일하다.
	weak_ptr<CCamera> m_pCamera;

	/* DX11 for Text */
	ComPtr<ID3D11DeviceContext> m_d3d11DeviceContext;
	ComPtr<ID3D11On12Device> m_d3d11On12Device;
	ComPtr<IDWriteFactory> m_dWriteFactory;
	ComPtr<ID3D11Resource> m_wrappedBackBuffers[m_nSwapChainBuffers];
	ComPtr<ID2D1Factory3> m_d2dFactory;
	ComPtr<ID2D1Device2> m_d2dDevice;
	ComPtr<ID2D1Bitmap1> m_d2dRenderTargets[m_nSwapChainBuffers];
	ComPtr<ID2D1DeviceContext2> m_d2dDeviceContext;

	ComPtr<ID2D1SolidColorBrush> m_textBrush;
	ComPtr<IDWriteTextFormat> m_textFormat;

	bool m_bPrepareDrawText = false;

	/* TCP 관련 */
	unique_ptr<CTcpClient> m_pTcpClient;
	static int m_nMainClientId; // TcpClient에서 받게 된다. -> 플레이어 1인칭으로 그릴때 비교해서 그려주게 하기위해

	bool m_bConnected = false;
	HWND m_hConnectButton = nullptr;

	bool m_bTcpClient = false;
	UINT m_nEventCreateTcpClient = 0;

	HWND m_hIPAddressEdit = nullptr;
	_TCHAR m_pszIPAddress[16] = {};

	/* Game State */
	// 일단 로비가 없으니 IN_GAME으로 시작
	int m_nGameState = GAME_STATE::IN_GAME;
	float m_fEndingElapsedTime = 0.0f;
	XMFLOAT4 m_xmf4EndFog = XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);

	/* Particle CB */
	D3D12_GPU_DESCRIPTOR_HANDLE m_d3dFramework_info_CbvGPUDescriptorHandle = {};
	ComPtr<ID3D12Resource> m_d3dFramework_info_Resource;
	CB_FRAMEWORK_INFO* m_cbFramework_info = nullptr;

	/* Sound */
	float m_fBGMVolume = 0.5f;
};

