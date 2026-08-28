#pragma once
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

#include "../GameLimits.h"
#include "GlobalDefine.h"

constexpr std::size_t MAX_NEARBY_OBJECTS{ 30 };

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

class CGameObject;
class CPlayer;

enum class SOCKET_STATE
{
	SEND_KEY_BUFFER,
	SEND_GAME_START,
	SEND_CHANGE_SLOT,
	SEND_LOADING_COMPLETE
};

enum class ReceiveHead : INT8
{
	Invalid = -1,
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

struct CS_PLAYER_INFO
{
	RightItem m_selectItem = NONE;

	int m_iMineobjectNum = -1;
	bool m_bAttacked = false;

	int m_iEscapeDoor = -1;

	bool m_bTeleportItemUse = false;
};

struct CS_PLAYER_STATE
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
	float m_fPitch = 0.0f;
	CS_PLAYER_INFO m_playerInfo;
};
static_assert(sizeof(CS_PLAYER_INFO) == 20);
static_assert(sizeof(CS_PLAYER_STATE) == 92);

struct CS_NEARBY_OBJECT
{
	int m_nObjectId = -1;
	XMFLOAT4X4 m_xmf4x4World;
};
static_assert(sizeof(CS_NEARBY_OBJECT) == 68);
static_assert(sizeof(CS_NEARBY_OBJECT) * MAX_NEARBY_OBJECTS <= MAX_PACKET_PAYLOAD_SIZE);

enum class OpenableObjectType : std::uint8_t
{
	Invalid = 0,
	Door = 1,
	Drawer = 2,
	Count
};

struct CS_OPENABLE_OBJECT_STATE
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
static_assert(sizeof(CS_OPENABLE_OBJECT_STATE) == 8);

class CTcpClient
{
public:
	CTcpClient();
	~CTcpClient();

	bool CreateSocket(HWND window, const TCHAR* ipAddress);
	void OnProcessingSocketMessage(HWND window, UINT messageId, WPARAM wParam, LPARAM lParam);
	void SendInputIfDue(const UCHAR* keysBuffer);
	void RequestSend();
	void SendLoadingComplete();

	INT8 GetMainClientId() const { return mMainClientId; }
	INT8 GetClientId(int index) const { return mClientInfo[index].m_nClientId; }
	INT8 GetClientCount() const { return mClientCount; }
	int GetEscapeDoorId() const { return mEscapeDoorId; }
	bool IsLoadingCompleteReceived() const { return mLoadingCompleteReceived; }

	void SetPlayer(const std::shared_ptr<CPlayer>& player, int index = 0) { mPlayers[index] = player; }
	void SetSelectedSlot(INT8 selectedSlot) { mSelectedSlot = selectedSlot; }
	void SetSocketState(SOCKET_STATE socketState) { mSocketState = socketState; }

	// 기존 프레임워크가 소켓 핸들을 직접 사용한다. 접근자 전환은 별도 작업으로 둔다.
	SOCKET m_sock = INVALID_SOCKET;

private:
	// recv() 한 번이 요청한 크기를 모두 채운다는 가정 없이 수신 진행 상태를 구분한다.
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

	struct PendingSend
	{
		std::vector<char> buffer;
		std::size_t sentBytes = 0;
	};

	bool HandleReceiveResult(ReceiveResult result);
	void ProcessReadEvent(HWND window, SOCKET socket);
	void ProcessWriteEvent();
	bool TryProcessChangeSlotPacket(HWND window, SOCKET socket);
	bool TryProcessInitPacket(SOCKET socket);
	bool TryProcessPlayerStatePacket(SOCKET socket);
	bool TryProcessNearbyObjectsPacket(SOCKET socket);
	bool TryProcessOpenableObjectStatePacket(SOCKET socket);
	bool TryProcessOpenableObjectSnapshotPacket(SOCKET socket);
	bool TryProcessClientCountPacket(SOCKET socket);
	bool TryProcessBlueSuitDeadPacket(SOCKET socket);
	bool TryProcessSpaceOutObjectsPacket(SOCKET socket);

	// 생성 실패, 소켓 오류, FD_CLOSE 및 소멸 시 동일한 경로로 정리한다.
	void CloseConnection();
	bool IsValidReceiveHead(INT8 head) const;
	bool ValidateOpenableObjectState(
		const CS_OPENABLE_OBJECT_STATE& objectState,
		const char* packetName) const;
	void ApplyOpenableObjectState(const CS_OPENABLE_OBJECT_STATE& objectState);
	void ResetReceiveState();
	ReceiveResult ReceiveData(SOCKET socket, std::size_t expectedBytes);
	bool TryGetCollisionObject(
		int objectId,
		const char* fieldName,
		std::shared_ptr<CGameObject>& gameObject);

	template<class... Args>
	bool SubmitSendData(Args&&... args);
	// partial send와 WSAEWOULDBLOCK 이후에도 큐의 전송 위치를 유지한다.
	SendResult FlushSendQueue();

	void ApplyServerUpdate();
	void UpdatePickedObject(int playerIndex);
	WORD BuildKeyBitMask(const UCHAR* keysBuffer) const;
	void UpdateZombiePlayer();
	void UpdateSurvivorPlayer(int playerIndex);

	// HEAD와 DATA가 여러 FD_READ에 나뉘어 도착할 때 이어 받기 위한 상태다.
	std::vector<char> mReceiveBuffer = std::vector<char>(MAX_PACKET_PAYLOAD_SIZE);
	std::size_t mExpectedPayloadBytes = 0;
	int mReceivedBytes = 0;
	ReceiveHead mReceiveHead = ReceiveHead::Invalid;
	bool mHasReceiveHead = false;
	bool mHasPayloadSize = false;

	std::array<CS_PLAYER_STATE, MAX_CLIENT> mClientInfo = {};
	std::array<std::shared_ptr<CPlayer>, MAX_CLIENT> mPlayers;
	std::deque<PendingSend> mSendQueue;
	std::size_t mPendingSendBytes = 0;
	std::chrono::steady_clock::time_point mNextInputSendTime = {};
	WORD mLatestInputKeyMask = 0;

	SOCKET_STATE mSocketState = SOCKET_STATE::SEND_GAME_START;
	INT8 mMainClientId = -1;
	INT8 mClientCount = -1;
	INT8 mSelectedSlot = -1;
	int mEscapeDoorId = -1;
	bool mLoadingCompleteReceived = false;
	bool mWsaStarted = false;
};

