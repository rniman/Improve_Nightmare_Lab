#pragma once

#include "Object.h"
#include "Camera.h"

enum GAME_STATE
{
	IN_LOBBY = 0,
	IN_GAME,
	BLUE_SUIT_WIN,
	ZOMBIE_WIN,
	IN_LOADING
};

class CShader
{
public:
	CShader();
	~CShader();

	virtual D3D12_INPUT_LAYOUT_DESC CreateInputLayout();
	virtual D3D12_RASTERIZER_DESC CreateRasterizerState();
	virtual D3D12_BLEND_DESC CreateBlendState();
	virtual D3D12_DEPTH_STENCIL_DESC CreateDepthStencilState();

	virtual D3D12_SHADER_BYTECODE CreateVertexShader();
	virtual D3D12_SHADER_BYTECODE CreatePixelShader();

	D3D12_SHADER_BYTECODE CompileShaderFromFile(const WCHAR* pszFileName, LPCSTR pszShaderName, LPCSTR pszShaderProfile, ID3DBlob** ppd3dShaderBlob);
	D3D12_SHADER_BYTECODE ReadCompiledShaderFromFile(const WCHAR* pszFileName, ID3DBlob** ppd3dShaderBlob = NULL);

	virtual void CreateShader(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList, ID3D12RootSignature* pd3dGraphicsRootSignature, UINT nRenderTargets = 1,
		DXGI_FORMAT* pdxgiRtvFormats = nullptr, DXGI_FORMAT dxgiDsvFormat = DXGI_FORMAT_D24_UNORM_S8_UINT);

	virtual void CreateShaderVariables(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList) { }
	virtual void UpdateShaderVariables(ID3D12GraphicsCommandList* pd3dCommandList) { }
	virtual void ReleaseShaderVariables() { }

	virtual void UpdateShaderVariable(ID3D12GraphicsCommandList* pd3dCommandList, XMFLOAT4X4* pxmf4x4World) { }

	virtual void Render(ID3D12GraphicsCommandList* pd3dCommandList, const shared_ptr<CCamera>& pCamera, const shared_ptr<CPlayer>& pPlayer, int nPipelineState = 0);
	virtual void PartitionRender(ID3D12GraphicsCommandList* pd3dCommandList, const shared_ptr<CCamera>& pCamera, int nPipelineState = 0) {}//그림자렌더링용
	//virtual void FloorRender(ID3D12GraphicsCommandList* pd3dCommandList, const shared_ptr<CCamera>& pCamera, const shared_ptr<CPlayer>& pPlayer, int nPipelineState = 0) {}//인스턴싱전용
	virtual void PrevRender(ID3D12GraphicsCommandList* pd3dCommandList, const shared_ptr<CCamera>& pCamera, int nPipelineState = 0);

	virtual void UpdatePipeLineState(ID3D12GraphicsCommandList* pd3dCommandList, int nPipelineState);

	virtual void ReleaseUploadBuffers();

	virtual void BuildObjects(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList, ID3D12RootSignature* pd3dGraphicsRootSignatureconst) { }

	virtual void AnimateObjects(float fElapsedTime);
	virtual void ParticleUpdate(float fCurTime){}
	virtual void ReleaseObjects() { }

	virtual void AddGameObject(const shared_ptr<CGameObject>& pGameObject);

	// Interface
	vector<shared_ptr<CGameObject>> GetGameObjects() const { return m_vGameObjects; };
	
protected:
	// 게임내 오브젝트는 쉐이더가 관리한다.
	vector<shared_ptr<CGameObject>> m_vGameObjects;
	int object_count{};

	// m_ppd3dPipelineState 를 만들때 Blob을 사용하므로 ComPtr 사용x (오류발생가능)
	ComPtr<ID3DBlob> m_pd3dVertexShaderBlob;
	ComPtr<ID3DBlob> m_pd3dPixelShaderBlob;
	ComPtr<ID3DBlob> m_pd3dGeometryShaderBlob;

	UINT					m_nPipelineState = 1;
	vector<ComPtr<ID3D12PipelineState>> m_vpd3dPipelineState;
	UINT					m_nPipeLineIndex = 0;
	//ID3D12PipelineState**	m_ppd3dPipelineState = NULL;

	D3D12_GRAPHICS_PIPELINE_STATE_DESC	m_d3dPipelineStateDesc;

	float								m_fElapsedTime = 0.0f;

	D3D12_PRIMITIVE_TOPOLOGY_TYPE m_topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////

class StandardShader : public CShader {
public:
	StandardShader();
	virtual ~StandardShader();

	virtual D3D12_INPUT_LAYOUT_DESC CreateInputLayout();

	virtual D3D12_RASTERIZER_DESC CreateRasterizerState();
	virtual D3D12_SHADER_BYTECODE CreateVertexShader();
	virtual D3D12_SHADER_BYTECODE CreatePixelShader();

	virtual void CreateShader(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList, ID3D12RootSignature* pd3dGraphicsRootSignature,  UINT nRenderTargets = 1,
		DXGI_FORMAT* pdxgiRtvFormats = nullptr, DXGI_FORMAT dxgiDsvFormat = DXGI_FORMAT_D24_UNORM_S8_UINT);
	virtual void BuildObjects(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList, ID3D12RootSignature* pd3dGraphicsRootSignature) override { }

};

////////////////////////////////////////////////////////////////////////////////////////////////////////////

class InstanceStandardShader : public StandardShader {
public:
	InstanceStandardShader();
	virtual ~InstanceStandardShader();

	virtual D3D12_INPUT_LAYOUT_DESC CreateInputLayout();
	virtual D3D12_SHADER_BYTECODE CreateVertexShader();

	virtual void Render(ID3D12GraphicsCommandList* pd3dCommandList, const shared_ptr<CCamera>& pCamera, const shared_ptr<CPlayer>& pPlayer, int nPipelineState = 0);
	//virtual void FloorRender(ID3D12GraphicsCommandList* pd3dCommandList, const shared_ptr<CCamera>& pCamera,const shared_ptr<CPlayer>& pPlayer, int nPipelineState = 0);
	virtual void AnimateObjects(float fElapsedTime);

	vector<vector<shared_ptr<CGameObject>>> m_vFloorObjects;
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////

class PartitionInsStandardShader : public InstanceStandardShader {
public:
	PartitionInsStandardShader();
	virtual ~PartitionInsStandardShader();

	virtual void AddPartitionGameObject(const shared_ptr<CGameObject>& pGameObject,int nPartition);
	virtual void Render(ID3D12GraphicsCommandList* pd3dCommandList, const shared_ptr<CCamera>& pCamera, const shared_ptr<CPlayer>& pPlayer, int nPipelineState = 0);
	// 렌더링하는 오브젝트를 담은 컨테이너가 다르므로 함수 분리
	virtual void PartitionRender(ID3D12GraphicsCommandList* pd3dCommandList, const shared_ptr<CCamera>& pCamera, int nPipelineState = 0);
	//virtual void AnimateObjects(float fElapsedTime);

	void AddPartition();
	void AddPartitionBB(shared_ptr<BoundingBox>& bb);

	vector<shared_ptr<BoundingBox>> GetPartitionBB() { return m_vPartitionBB; }
	vector<vector<shared_ptr<CGameObject>>>& GetPartitionObjects() { return m_vPartitionObject; }
protected:
	vector<vector<shared_ptr<CGameObject>>> m_vPartitionObject;//index == partitionNumber
	vector<shared_ptr<BoundingBox>> m_vPartitionBB; //index == partitionNumber
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////

class TransparentShader : public InstanceStandardShader {
public:
	TransparentShader();
	virtual ~TransparentShader();

	virtual D3D12_RASTERIZER_DESC CreateRasterizerState();
	virtual D3D12_BLEND_DESC CreateBlendState();
	virtual D3D12_DEPTH_STENCIL_DESC CreateDepthStencilState();

	virtual D3D12_SHADER_BYTECODE CreatePixelShader();

	virtual void Render(ID3D12GraphicsCommandList* pd3dCommandList, const shared_ptr<CCamera>& pCamera, const shared_ptr<CPlayer>& pPlayer, int nPipelineState = 0);
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////

class CSkinnedAnimationStandardShader : public StandardShader
{
public:
	CSkinnedAnimationStandardShader();
	virtual ~CSkinnedAnimationStandardShader();

	virtual D3D12_INPUT_LAYOUT_DESC CreateInputLayout();
	virtual D3D12_SHADER_BYTECODE CreateVertexShader();
};

/// <CShader - StandardShader - CSkinnedAnimationStandardShader>
////// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// ///  
/// <CShader - CBlueSuitUserInterfaceShader>

class CBlueSuitPlayer;
class CZombiePlayer;

enum PLAYER_RESULT
{
	WIN,
	OVER
};

// [0504] UI SHADER
class CBlueSuitUserInterfaceShader : public CShader
{
public:
	virtual D3D12_INPUT_LAYOUT_DESC CreateInputLayout();
	virtual D3D12_SHADER_BYTECODE CreateVertexShader();
	virtual D3D12_SHADER_BYTECODE CreatePixelShader();
	virtual D3D12_RASTERIZER_DESC CreateRasterizerState();
	virtual D3D12_BLEND_DESC CreateBlendState();
	
	void CreateShader(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList, ID3D12RootSignature* pd3dGraphicsRootSignature, UINT nRenderTargets, DXGI_FORMAT* pdxgiRtvFormats, DXGI_FORMAT dxgiDsvFormat);

	virtual void BuildObjects(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList, ID3D12RootSignature* pd3dGraphicsRootSignature);
	virtual void AnimateObjects(float fElapsedTime) override;
	virtual void Render(ID3D12GraphicsCommandList* pd3dCommandList, const shared_ptr<CCamera>& pCamera, const shared_ptr<CPlayer>& pPlayer, int nPipelineState = 0) override;

	void AnimateObjectBlueSuit(float fElapsedTime);

	void AnimateInGame();

	virtual void AddGameObject(const shared_ptr<CGameObject>& pGameObject) override;

	void SetGameState(int nGameState) { m_nGameState = nGameState; };
private:
	shared_ptr<CBlueSuitPlayer> m_pBlueSuitPlayer;

	shared_ptr<CGameObject> m_pTeleport;
	shared_ptr<CGameObject> m_pRadar;
	shared_ptr<CGameObject> m_pMine;
	shared_ptr<CGameObject> m_pFuse;

	array<shared_ptr<CMaterial>, 2> m_vpmatTeleport;
	array<shared_ptr<CMaterial>, 2> m_vpmatRadar;
	array<shared_ptr<CMaterial>, 2> m_vpmatMine;
	array<shared_ptr<CMaterial>, 4> m_vpmatFuse;

	shared_ptr<CGameObject> m_pSelectRect;
	shared_ptr<CMaterial> m_pmatSelect;

	array<shared_ptr<CGameObject>, 2> m_vpStamina;
	array<shared_ptr<CMaterial>, 2> m_vpmatStamina;
	shared_ptr<CUserInterfaceRectMesh> m_pmeshStaminaRect;

	array<shared_ptr<CMaterial>, 2> m_vpmatGameEnding;
	shared_ptr<CGameObject> m_pGameEnding;

	float m_fEndingElapsedTime = 0.0f;
	int m_nGameState = GAME_STATE::IN_GAME;
};

/// <CShader - CBlueSuitUserInterfaceShader>
////// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// ///  
/// <CShader - CZombieUserInterfaceShader>

class CZombieUserInterfaceShader : public CShader
{
public:
	virtual D3D12_INPUT_LAYOUT_DESC CreateInputLayout();
	virtual D3D12_SHADER_BYTECODE CreateVertexShader();
	virtual D3D12_SHADER_BYTECODE CreatePixelShader();
	virtual D3D12_RASTERIZER_DESC CreateRasterizerState();
	virtual D3D12_BLEND_DESC CreateBlendState();

	void CreateShader(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList, ID3D12RootSignature* pd3dGraphicsRootSignature, UINT nRenderTargets, DXGI_FORMAT* pdxgiRtvFormats, DXGI_FORMAT dxgiDsvFormat);
	virtual void BuildObjects(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList, ID3D12RootSignature* pd3dGraphicsRootSignature);
	virtual void AnimateObjects(float fElapsedTime) override;
	virtual void Render(ID3D12GraphicsCommandList* pd3dCommandList, const shared_ptr<CCamera>& pCamera, const shared_ptr<CPlayer>& pPlayer, int nPipelineState = 0) override;

	void AnimateObjectZombie(float fElapsedTime);

	virtual void AddGameObject(const shared_ptr<CGameObject>& pGameObject) override;

	void SetGameState(int nGameState) { m_nGameState = nGameState; };
private:
	shared_ptr<CZombiePlayer> m_pZombiePlayer;

	array<shared_ptr<CMaterial>, 3> m_vpmatTracking;
	array<shared_ptr<CMaterial>, 3> m_vpmatInterruption;
	array<shared_ptr<CMaterial>, 3> m_vpmatRunning;

	shared_ptr<CGameObject> m_pTracking;
	shared_ptr<CGameObject> m_pInterruption;
	shared_ptr<CGameObject> m_pRunning;

	array<shared_ptr<CMaterial>, 2> m_vpmatGameEnding;
	shared_ptr<CGameObject> m_pGameEnding;

	float m_fEndingElapsedTime = 0.0f;
	int m_nGameState = GAME_STATE::IN_GAME;
};

/// <CShader - UserInterfaceShader>
////// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// ///  
/// <CShader - OutLineShader>

class CPostProcessingShader;

constexpr int STANDARD_OUT_LINE_MASK{ 0 };
constexpr int INSTANCE_OUT_LINE_MASK{ 1 };
constexpr int SKINNING_OUT_LINE_MASK{ 2 };
constexpr int STANDARD_OUT_LINE{ 3 };
constexpr int INSTANCE_OUT_LINE{ 4 };
constexpr int SKINNING_OUT_LINE{ 5 };

//[0505] OutLine
class COutLineShader : public CShader
{
public:
	COutLineShader(int nMainPlayer);
	virtual ~COutLineShader() {};

	virtual D3D12_INPUT_LAYOUT_DESC CreateInputLayout();
	virtual D3D12_SHADER_BYTECODE CreateVertexShader();
	virtual D3D12_SHADER_BYTECODE CreatePixelShader();

	virtual D3D12_DEPTH_STENCIL_DESC CreateDepthStencilState();
	virtual D3D12_RASTERIZER_DESC CreateRasterizerState();

	virtual void CreateShader(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList, ID3D12RootSignature* pd3dGraphicsRootSignature, UINT nRenderTargets, DXGI_FORMAT* pdxgiRtvFormats, DXGI_FORMAT dxgiDsvFormat);

	virtual void Render(ID3D12GraphicsCommandList* pd3dCommandList, const shared_ptr<CCamera>& pCamera,const shared_ptr<CPlayer>& pPlayer, int nPipelineState = 0);

	virtual void AddGameObject(const shared_ptr<CGameObject>& pGameObject);

	void SetPostProcessingShader(CPostProcessingShader* pPostProcessingShader) { m_pPostProcessingShader = pPostProcessingShader; }
private:
	int m_nMainPlayer;
	shared_ptr<CGameObject> m_pPickedObject;

	shared_ptr<CPlayer> m_pMainPlayer;
	shared_ptr<CZombiePlayer> m_pZombiePlayer;
	vector<shared_ptr<CBlueSuitPlayer>> m_vpBlueSuitPlayer;

	bool m_bOutLine = false;
	CPostProcessingShader* m_pPostProcessingShader = nullptr;
};

/// <CShader - COutLineShader>
////// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// ///  
/// <CShader - CLobbyStandardShader>

//[0629] LOBBY 
class CLobbyStandardShader : public CShader
{
public:
	CLobbyStandardShader() {};
	virtual ~CLobbyStandardShader() {};

	virtual D3D12_INPUT_LAYOUT_DESC CreateInputLayout();
	virtual D3D12_SHADER_BYTECODE CreateVertexShader();
	virtual D3D12_SHADER_BYTECODE CreatePixelShader();

	virtual void CreateShader(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList, ID3D12RootSignature* pd3dGraphicsRootSignature, UINT nRenderTargets, DXGI_FORMAT* pdxgiRtvFormats, DXGI_FORMAT dxgiDsvFormat);

private:
};

/// <CShader - CLobbyStandardShader>
////// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// /// ///  
/// <CShader - CLobbyUserInterfaceShader>

enum LOBBY_PROCESS_INPUT
{
	START_BUTTON_NON = 0,
	START_BUTTON_SEL,
	START_BUTTON_DOWN,
	START_BUTTON_UP,
	CHANGE_BUTTON_NON,
	CHANGE_BUTTON_SEL,
	CHANGE_BUTTON_DOWN,
	CHANGE_BUTTON_UP,
	BORDER_NON,
	BORDER_SEL

};

class CLobbyUserInterfaceShader : public CShader
{
public:
	CLobbyUserInterfaceShader(int m_nMainClientID);;
	virtual ~CLobbyUserInterfaceShader() {};

	virtual D3D12_INPUT_LAYOUT_DESC CreateInputLayout();
	virtual D3D12_SHADER_BYTECODE CreateVertexShader();
	virtual D3D12_SHADER_BYTECODE CreatePixelShader();
	virtual D3D12_RASTERIZER_DESC CreateRasterizerState();
	virtual D3D12_BLEND_DESC CreateBlendState();

	virtual void CreateShader(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList, ID3D12RootSignature* pd3dGraphicsRootSignature, UINT nRenderTargets, DXGI_FORMAT* pdxgiRtvFormats, DXGI_FORMAT dxgiDsvFormat);

	virtual void BuildObjects(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList, ID3D12RootSignature* pd3dGraphicsRootSignature);

	int ProcessInput(int nProcessInput);

	void UpdateShaderMainPlayer(int nMainClientID);
	int GetSelectedBorder() const { return m_nSelectedBorder; }
private:
	int m_nMainClientID = -1;

	int m_nSelectedBorder = -1;
	array<shared_ptr<CMaterial>, 3> m_apmatLobbyBorder; // 0: default, 1: client, 2: selected
	array<shared_ptr<CGameObject>, 5> m_apLobbyBorderObjects;

	bool m_bStartButtonPressed = false;
	array<shared_ptr<CMaterial>, 4> m_apmatStartButton;
	shared_ptr<CGameObject> m_pStartButton;

	bool m_bChangeButtonPressed = false;
	array<shared_ptr<CMaterial>, 4> m_apmatChangeButton;
	shared_ptr<CGameObject> m_pChangeButton;
};

class CFullScreenProcessingShader : public CShader
{
public:
	CFullScreenProcessingShader() {};
	virtual ~CFullScreenProcessingShader() {};

	D3D12_SHADER_BYTECODE CreateVertexShader() override;
	D3D12_SHADER_BYTECODE CreatePixelShader() override;
	D3D12_BLEND_DESC CreateBlendState() override;
	D3D12_DEPTH_STENCIL_DESC CreateDepthStencilState() override;

	void CreateShader(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList, ID3D12RootSignature* pd3dGraphicsRootSignature, UINT nRenderTargets = 1,
		DXGI_FORMAT* pdxgiRtvFormats = nullptr, DXGI_FORMAT dxgiDsvFormat = DXGI_FORMAT_D24_UNORM_S8_UINT) override;
	void BuildObjects(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList, ID3D12RootSignature* pd3dGraphicsRootSignature,
		const WCHAR* strGameState, shared_ptr<CPlayer>& mainPlayer);
	void Render(ID3D12GraphicsCommandList* pd3dCommandList, const shared_ptr<CCamera>& pCamera, const shared_ptr<CPlayer>& pPlayer, int nPipelineState = 0) override;

private:
	shared_ptr<CPlayer> m_pMainPlayer;
};