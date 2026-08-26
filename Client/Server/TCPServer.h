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
constexpr UINT WM_OPENABLE_OBJECT_STATE{ WM_USER + 3 };

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

enum class SoundMessage
{
	OpenDrawer,
	CloseDrawer,
	OpenDoor,
	CloseDoor,
	BlueSuitDead,
};

enum class GameState
{
	InLobby = 0,
	InGame,
	BlueSuitWin,
	ZombieWin,
	Loading
};

struct PlayerReplicationInfo
{
	RightItem selectedItem = NONE;

	int mineObjectId = -1;
	bool attacked = false;

	int escapeDoorId = -1;

	bool teleportItemUsed = false;
};

struct PlayerReplicationState
{
	INT8 clientId = -1;
	bool alive = true;
	bool running = false;
	XMFLOAT3 position = {};
	XMFLOAT3 velocity = {};
	XMFLOAT3 look = {};
	int pickedObjectId = -1;
	int slotObjectIds[3] = { -1, -1, -1 };
	int fuseObjectIds[3] = { -1, -1, -1 };
	float pitch = 1.0f;
	PlayerReplicationInfo playerInfo;
};
static_assert(sizeof(PlayerReplicationInfo) == 20);
static_assert(sizeof(PlayerReplicationState) == 92);

struct NearbyObjectState
{
	int objectId = -1;
	XMFLOAT4X4 world;
};
static_assert(sizeof(NearbyObjectState) == 68);
static_assert(sizeof(NearbyObjectState) * MAX_NEARBY_OBJECTS <= MAX_PACKET_PAYLOAD_SIZE);

enum class OpenableObjectType : std::uint8_t
{
	Invalid = 0,
	Door = 1,
	Drawer = 2,
	Count
};

struct OpenableObjectState
{
	std::int32_t objectId = -1;
	OpenableObjectType objectType = OpenableObjectType::Invalid;
	std::uint8_t opened = 0;
	std::uint16_t padding = 0;

	constexpr bool IsValidOpenableObjectType() const noexcept
	{
		const std::uint8_t value = static_cast<std::uint8_t>(objectType);
		return value > static_cast<std::uint8_t>(OpenableObjectType::Invalid) &&
			value < static_cast<std::uint8_t>(OpenableObjectType::Count);
	}
};
static_assert(sizeof(OpenableObjectType) == 1);
static_assert(sizeof(OpenableObjectState) == 8);

enum class ServerPacketType : INT8
{
	Init = 0,
	// wire 값 1은 폐기된 head이므로 재사용하지 않는다.
	ClientCount = 2,
	BlueSuitWin = 3,
	ZombieWin = 4,
	GameStart = 5,
	ChangeSlot = 6,

	OpenDrawerSound = 7,
	CloseDrawerSound = 8,
	OpenDoorSound = 9,
	CloseDoorSound = 10,

	BlueSuitDead = 11,
	SpaceOutObjects = 12,
	LoadingComplete = 13,
	PlayerState = 14,
	NearbyObjects = 15,
	OpenableObjectState = 16,
	OpenableObjectSnapshot = 17
};

enum class ClientPacketType : INT8
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

struct ClientConnectionState
{
	bool isUsed = false;
	SOCKET socket = INVALID_SOCKET;

	struct sockaddr_in clientAddress;
	int clientAddressLength = 0;
	char ipAddress[INET_ADDRSTRLEN] = {};

	ClientPacketType receivePacketType = ClientPacketType::Invalid;

	// 소켓마다 HEAD와 DATA의 partial recv 진행 상태를 함께 보존한다.
	bool hasReceivePacketType = false;
	int receivedBytes = 0;		// 현재까지 받은 데이터의 길이
	std::vector<char> receiveBuffer = std::vector<char>(MAX_PACKET_PAYLOAD_SIZE);
	std::deque<PendingSend> sendQueue;
	// 큐가 소유한 전체 버퍼 크기와 아직 send()하지 못한 바이트를 구분한다.
	size_t pendingSendBytes = 0;
	size_t unsentSendBytes = 0;
	size_t currentPacketReceivedBytes = 0;

	SocketNetworkStatistics networkStatistics;

	ServerPacketType pendingPacketType = ServerPacketType::Init;

	bool isLoadingComplete = false;
};

class TCPServer
{
public:
	TCPServer();
	~TCPServer();

	bool Initialize(HWND window);
	void RunSimulationTick();

	// Win32 진입점에서 전달되는 메시지만 외부에 공개한다.
	void HandleWindowMessage(HWND window, UINT messageId, WPARAM wParam, LPARAM lParam);
	void HandleSocketMessage(HWND window, UINT messageId, WPARAM wParam, LPARAM lParam);

	void SetGameState(GameState gameState) { mGameState = gameState; }
	void SetZombieCount(int zombieCount) { mZombieCount = zombieCount; }
	void SetBlueSuitCount(int blueSuitCount) { mBlueSuitCount = blueSuitCount; }
	void SetClientListBox(HWND clientListBox) { mClientListBox = clientListBox; }

	int GetZombieCount() const { return mZombieCount; }
	int GetBlueSuitCount() const { return mBlueSuitCount; }
	shared_ptr<CServerPlayer> GetPlayer(int index) { return mPlayers[index]; }
	default_random_engine& GetRandomEngine() { return mRandomEngine; }
	HWND GetWindowHandle() const { return mWindowHandle; }

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

	// Lifecycle
	bool InitializeNetworking(HWND window);
	void InitializeWorld();

	// Socket events
	void HandleAcceptEvent(HWND window, SOCKET listenSocket);
	void HandleReadEvent(SOCKET socket);
	void HandleWriteEvent(SOCKET socket);
	void HandleCloseEvent(SOCKET socket);

	// Receive
	bool HandleReceiveResult(ReceiveResult result, SOCKET socket);
	bool IsValidClientPacketType(INT8 packetType) const;
	ReceiveResult ReceiveData(int clientIndex, size_t expectedBytes);
	void ResetReceiveState(ClientConnectionState& socketInfo);

	// Client packets
	void HandleGameStartPacket(int clientIndex);
	bool TryHandleChangeSlotPacket(SOCKET socket, int& clientIndex);
	bool TryHandleClientInputPacket(
		SOCKET socket,
		int clientIndex,
		const std::shared_ptr<CServerPlayer>& player);
	bool HandleLoadingCompletePacket(int clientIndex);
	bool AreAllClientsLoadingComplete() const;

	// Connections
	INT8 RegisterClientConnection(SOCKET clientSocket, struct sockaddr_in clientAddress, int clientAddressLength);
	INT8 FindClientIndex(SOCKET clientSocket) const;
	// 연결 종료 원인과 관계없이 소켓 및 플레이어 상태를 한 번만 정리한다.
	bool DisconnectClient(SOCKET clientSocket);

	// Send
	template<class... Args>
	bool EnqueuePacketFields(int clientIndex, Args&&... args);
	bool EnqueuePacketBuffer(int clientIndex, vector<char> buffer);
	// partial send와 WSAEWOULDBLOCK 이후에도 소켓별 전송 위치를 유지한다.
	SendResult FlushSendQueue(int clientIndex);
	void EnqueuePendingPacket(int clientIndex);
	void AppendBytes(vector<char>& buffer, const void* data, size_t size);

	// Game session
	GameState DetermineGameOutcome();
	void EnqueueGameOutcomePackets(GameState gameState);

	// Replication
	void BuildPlayerReplicationStates();
	void ReplicateStateIfDue();
	void BroadcastOpenableObjectState(
		int objectId,
		OpenableObjectType objectType,
		bool opened);
	void BroadcastOpenableObjectSnapshot();
	void ReplicateOutOfSpaceObjects();
	std::vector<SC_SPACEOUT_OBJECT> CollectOutOfSpaceObjects();
	void EnqueueOutOfSpaceObjectPackets(const std::vector<SC_SPACEOUT_OBJECT>& objectUpdates);
	void ReplicateNearbyObjectsIfDue();
	void BuildNearbyObjectSnapshots();
	void EnqueueNearbyObjectSnapshots();

	// World initialization
	bool LoadServerScene();
	void CreateObjectFromSceneFrame(char* frameName, const XMFLOAT4X4& world, const vector<BoundingOrientedBox>& boundingBoxes);
	void PopulateSceneItems();
	void AssignUniquePlayerSpawnPosition(shared_ptr<CServerPlayer>& serverPlayer, int index);

	// Diagnostics
	void ReportNetworkStatisticsIfDue();

	GameState mGameState = GameState::InLobby;
	CTimer mTimer;
	ServerNetworkStatisticsReporter mNetworkStatisticsReporter;
	std::chrono::steady_clock::time_point mNextStateReplicationTime = {};
	std::chrono::steady_clock::time_point mNextNearbyObjectReplicationTime = {};
	default_random_engine mRandomEngine;

	// 접속한 클라이언트들의 정보를 저장.
	std::array<ClientConnectionState, MAX_CLIENT> mConnections;	// 소켓 인덱스는 순차적으로 배정받는다

	int mZombieCount = 0;
	int mBlueSuitCount = 0;
	std::array<std::shared_ptr<CServerPlayer>, MAX_CLIENT> mPlayers;
	std::array<PlayerReplicationState, MAX_CLIENT> mPlayerReplicationStates = {};
	std::array<std::vector<NearbyObjectState>, MAX_CLIENT> mNearbyObjectSnapshots = {};
	std::vector<shared_ptr<CServerGameObject>> mGameObjects;
	std::shared_ptr<CServerCollisionManager> mCollisionManager;

	vector<pair<int, int>> mDrawerEntries; // <ObjectCount,type>

	HWND mWindowHandle = nullptr;
	HWND mClientListBox = nullptr;
	INT8 mClientCount = 0;

	array<XMFLOAT3, 28> mPlayerStartPositions;
	array<int, MAX_CLIENT> mPlayerStartPositionIndices;
	bool mCanReplicateState = false;
};

extern TCPServer gTcpServer;
