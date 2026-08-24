#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

#include "../Client/GlobalDefine.h"
#include "NetworkStatistics.h"
#include "Timer.h"
constexpr size_t MAX_CLIENT{ 5 };
constexpr size_t MAX_NEARBY_OBJECTS{ 30 };

constexpr WORD KEY_W{ 0x01 };
constexpr WORD KEY_S{ 0x02 };
constexpr WORD KEY_A{ 0x04 };
constexpr WORD KEY_D{ 0x08 };
constexpr WORD KEY_1{ 0x10 };
constexpr WORD KEY_2{ 0x20 };
constexpr WORD KEY_3{ 0x40 };
constexpr WORD KEY_4{ 0x80 };
constexpr WORD KEY_E{ 0x100 };
constexpr WORD KEY_LSHIFT{ 0x200 };
constexpr WORD KEY_LBUTTON{ 0x400 };

// 소켓 정보 저장을 위한 구조체와 변수
class CServerGameObject;
class CServerPlayer;
class CServerCollisionManager;

enum SOUND_MESSAGE
{
	OPEN_DRAWER,
	CLOSE_DRAWER,
	OPEN_DOOR,
	CLOSE_DOOR,
	BLUE_SUIT_DEAD,
};

enum GAME_STATE
{
	IN_LOBBY = 0,
	IN_GAME,
	BLUE_SUIT_WIN,
	ZOMBIE_WIN,
	IN_LODING
};

struct SC_PLAYER_INFO
{
	RightItem m_selectItem = NONE;

	int m_iMineobjectNum = -1;
	bool m_bAttacked = false;

	int m_iEscapeDoor = -1;

	bool m_bTeleportItemUse = false;
};

struct SC_PLAYER_STATE
{
	INT8 m_nClientId = -1;
	bool m_bAlive = true;
	bool m_bRunning = false;
	XMFLOAT3 m_xmf3Position = {};
	XMFLOAT3 m_xmf3Velocity = {};
	XMFLOAT3 m_xmf3Look = {};
	int m_nPickedObjectNum = -1;
	int m_nSlotObjectNum[3] = { -1, -1, -1 };
	int m_nFuseObjectNum[3] = { -1, -1, -1 };
	float m_fPitch = 1.0f;
	SC_PLAYER_INFO m_playerInfo;
};
static_assert(sizeof(SC_PLAYER_INFO) == 20);
static_assert(sizeof(SC_PLAYER_STATE) == 92);

struct SC_NEARBY_OBJECT
{
	int m_nObjectId = -1;
	XMFLOAT4X4 m_xmf4x4World;
};
static_assert(sizeof(SC_NEARBY_OBJECT) == 68);
static_assert(sizeof(SC_NEARBY_OBJECT) * MAX_NEARBY_OBJECTS <= MAX_PACKET_PAYLOAD_SIZE);

enum class SOCKET_STATE : INT8
{
	SEND_ID = 0,
	// wire 값 1은 폐기된 head이므로 재사용하지 않는다.
	SEND_NUM_OF_CLIENT = 2,
	SEND_BLUE_SUIT_WIN = 3,
	SEND_ZOMBIE_WIN = 4,
	SEND_GAME_START = 5,
	SEND_CHANGE_SLOT = 6,

	SEND_OPEN_DRAWER_SOUND = 7,
	SEND_CLOSE_DRAWER_SOUND = 8,
	SEND_OPEN_DOOR_SOUND = 9,
	SEND_CLOSE_DOOR_SOUND = 10,

	SEND_BLUE_SUIT_DEAD = 11,
	SEND_SPACEOUT_OBJECTS = 12,
	SEND_LOADING_COMPLETE = 13,
	SEND_PLAYER_STATE = 14,
	SEND_NEARBY_OBJECTS = 15
};

enum class ReceiveHead : INT8
{
	Invalid = -1,
	KeysBuffer = 0,
	GameStart = 1,
	ChangeSlot = 2,
	LoadingComplete = 3
};

struct PendingSend
{
	std::vector<char> buffer;
	size_t sentBytes = 0;
};

struct SocketInfo
{
	bool isUsed = false;
	SOCKET socket = INVALID_SOCKET;

	struct sockaddr_in clientAddress;
	int clientAddressLength = 0;
	char ipAddress[INET_ADDRSTRLEN] = {};

	ReceiveHead receiveHead = ReceiveHead::Invalid;

	// 소켓마다 HEAD와 DATA의 partial recv 진행 상태를 함께 보존한다.
	bool hasReceiveHead = false;
	int receivedBytes = 0;		// 현재까지 받은 데이터의 길이
	std::vector<char> receiveBuffer = std::vector<char>(MAX_PACKET_PAYLOAD_SIZE);
	std::deque<PendingSend> sendQueue;
	// 큐가 소유한 전체 버퍼 크기와 아직 send()하지 못한 바이트를 구분한다.
	size_t pendingSendBytes = 0;
	size_t unsentSendBytes = 0;
	size_t currentPacketReceivedBytes = 0;

	SocketNetworkStatistics networkStatistics;

	SOCKET_STATE sendState = SOCKET_STATE::SEND_ID;

	bool isLoadingComplete = false;
};

class TCPServer
{
public:
	TCPServer();
	~TCPServer();

	bool Initialize(HWND hWnd);
	void SimulationLoop();

	// Win32 진입점에서 전달되는 메시지만 외부에 공개한다.
	void OnProcessingWindowMessage(HWND hWnd, UINT nMessageID, WPARAM wParam, LPARAM lParam);
	void OnProcessingSocketMessage(HWND hWnd, UINT nMessageID, WPARAM wParam, LPARAM lParam);

	void SetGameState(int gameState) { mGameState = gameState; }
	void SetZombieCount(int zombieCount) { mZombieCount = zombieCount; }
	void SetBlueSuitCount(int blueSuitCount) { mBlueSuitCount = blueSuitCount; }
	void SetClientListBox(HWND clientListBox) { mClientListBox = clientListBox; }

	int GetZombieCount() const { return mZombieCount; }
	int GetBlueSuitCount() const { return mBlueSuitCount; }
	shared_ptr<CServerPlayer> GetPlayer(int index) { return mPlayers[index]; }

	static default_random_engine m_mt19937Gen;
	static HWND m_hWnd;

private:
	// recv() 결과를 호출부가 임의의 정수값으로 해석하지 않도록 의미를 고정한다.
	enum class ReceiveResult
	{
		Complete,
		Pending,
		Closed,
		Error
	};

	enum class SendResult
	{
		Complete,
		Pending,
		Error
	};

	void ProcessAcceptEvent(HWND hWnd, SOCKET listenSocket);
	void ProcessReadEvent(SOCKET socket);
	void ProcessWriteEvent(SOCKET socket);
	void ProcessCloseEvent(SOCKET socket);
	void ProcessGameStartPacket(int clientIndex);
	bool TryProcessChangeSlotPacket(SOCKET socket, int& clientIndex);
	bool TryProcessClientInputPacket(
		SOCKET socket,
		int clientIndex,
		const std::shared_ptr<CServerPlayer>& player);
	bool ProcessLoadingCompletePacket(int clientIndex);
	bool AreAllClientsLoadingComplete() const;

	// 연결 종료 원인과 관계없이 소켓 및 플레이어 상태를 한 번만 정리한다.
	bool DisconnectClient(SOCKET clientSocket);
	INT8 RegisterClientSocket(SOCKET clientSocket, struct sockaddr_in clientAddress, int clientAddressLength);
	INT8 FindClientIndex(SOCKET clientSocket) const;
	bool IsValidReceiveHead(INT8 head) const;
	bool HandleReceiveResult(ReceiveResult result, SOCKET socket);
	void ResetReceiveState(SocketInfo& socketInfo);
	ReceiveResult ReceiveData(int clientIndex, size_t expectedBytes);
	void ReportNetworkStatisticsIfDue();

	template<class... Args>
	bool SubmitSendData(int clientIndex, Args&&... args);
	bool EnqueueSendBuffer(int clientIndex, vector<char> buffer);
	// partial send와 WSAEWOULDBLOCK 이후에도 소켓별 전송 위치를 유지한다.
	SendResult FlushSendQueue(int clientIndex);
	void RequestSend(int clientIndex);
	void AppendBufferData(vector<char>& buffer, const void* data, size_t size);

	int DetermineEndGameState();
	void QueueEndGameNotifications(int endGameState);
	void UpdatePlayerReplicationData();
	void ReplicateStateIfDue();

	void LoadScene();
	void CreateSceneObject(char* pstrFrameName, const XMFLOAT4X4& xmf4x4World, const vector<BoundingOrientedBox>& voobb);
	void CreateItemObject();
	void ProcessOutOfSpaceObjectReplication();
	std::vector<SC_SPACEOUT_OBJECT> CollectOutOfSpaceObjects();
	void EnqueueOutOfSpaceObjectPackets(const std::vector<SC_SPACEOUT_OBJECT>& objectUpdates);
	void UpdateNearbyObjectReplicationDataIfDue();
	void UpdateNearbyObjectReplicationData();
	void ReplicateNearbyObjectData();
	void InitializePlayerPosition(shared_ptr<CServerPlayer>& serverPlayer, int index);

	int mGameState = GAME_STATE::IN_LOBBY;
	CTimer mTimer;
	ServerNetworkStatisticsReporter mNetworkStatisticsReporter;
	std::chrono::steady_clock::time_point mNextStateReplicationTime = {};
	std::chrono::steady_clock::time_point mNextNearbyObjectReplicationTime = {};
	static INT8 sClientCount;

	// 접속한 클라이언트들의 정보를 저장.
	std::array<SocketInfo, MAX_CLIENT> mSocketInfos;	// 소켓 인덱스는 순차적으로 배정받는다

	int mZombieCount = 0;
	int mBlueSuitCount = 0;
	std::array<std::shared_ptr<CServerPlayer>, MAX_CLIENT> mPlayers;
	std::array<SC_PLAYER_STATE, MAX_CLIENT> mPlayerStates = {};
	std::array<std::vector<SC_NEARBY_OBJECT>, MAX_CLIENT> mNearbyObjectSnapshots = {};
	std::vector<shared_ptr<CServerGameObject>> mGameObjects;
	std::shared_ptr<CServerCollisionManager> mCollisionManager;

	vector<pair<int, int>> mDrawerIds; // <ObjectCount,type>

	HWND mClientListBox = nullptr;

	array<XMFLOAT3, 28> mPlayerStartPositions;
	array<int, MAX_CLIENT> mPlayerStartPositionIndices;
	bool mCanReplicateState = false;
};
