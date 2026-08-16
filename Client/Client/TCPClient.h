#pragma once
#include <array>
#include <cstddef>
#include <deque>
#include <memory>
#include <vector>

#include "GlobalDefine.h"

constexpr UINT WM_SOCKET{ WM_USER + 1 };

constexpr std::size_t MAX_CLIENT{ 5 };
constexpr std::size_t MAX_SURVIVOR{ 4 };
constexpr std::size_t MAX_RECV_OBJECT_INFO{ 30 };

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
constexpr WORD KEY_RBUTTON{ 0x800 };

class CGameObject;
class CPlayer;

enum class SOCKET_STATE
{
	SEND_KEY_BUFFER,
	SEND_GAME_START,
	SEND_CHANGE_SLOT,
	SEND_LOADING_COMPLETE
};

enum RECV_HEAD
{
	HEAD_INIT = 0,
	HEAD_UPDATE_DATA,
	HEAD_NUM_OF_CLIENT,
	HEAD_BLUE_SUIT_WIN,
	HEAD_ZOMBIE_WIN,
	HEAD_GAME_START,
	HEAD_CHANGE_SLOT,

	HEAD_OPEN_DRAWER_SOUND,
	HEAD_CLOSE_DRAWER_SOUND,
	HEAD_OPEN_DOOR_SOUND,
	HEAD_CLOSE_DOOR_SOUND,

	HEAD_BLUE_SUIT_DEAD,
	SEND_SPACEOUT_OBJECTS,
	HEAD_LOADING_COMPLETE
};

struct CS_ANIMATION_INFO
{
	float pitch = 0.0f;
};

struct CS_PLAYER_INFO
{
	RightItem m_selectItem;
	bool m_bRightClick = false;

	int m_iMineobjectNum = -1;
	bool m_bAttacked = false;

	int m_iEscapeDoor = -1;

	bool m_bTeleportItemUse = false;
};

struct CS_CLIENTS_INFO
{
	INT8 m_nClientId = -1;
	bool m_bAlive = true;
	bool m_bRunning = false;
	XMFLOAT3 m_xmf3Position;
	XMFLOAT3 m_xmf3Velocity;
	XMFLOAT3 m_xmf3Look;
	int m_nPickedObjectNum = -1;

	int m_nSlotObjectNum[3];	// 각 슬롯에 포함된 오브젝트 번호(없으면 -1)
	int m_nFuseObjectNum[3];	// 퓨즈 오브젝트 번호(없으면 -1)

	int m_nNumOfObject = -1;
	std::array<int, MAX_RECV_OBJECT_INFO> m_anObjectNum;
	std::array<XMFLOAT4X4, MAX_RECV_OBJECT_INFO> m_axmf4x4World;

	CS_ANIMATION_INFO m_animationInfo;
	CS_PLAYER_INFO m_playerInfo;
};

class CTcpClient
{
public:
	CTcpClient();
	~CTcpClient();

	bool CreateSocket(HWND window, const TCHAR* ipAddress);
	void OnProcessingSocketMessage(HWND window, UINT messageId, WPARAM wParam, LPARAM lParam);
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

	void ProcessReadEvent(HWND window, SOCKET socket);
	void ProcessWriteEvent();
	bool HandleReceiveResult(ReceiveResult result);

	// 생성 실패, 소켓 오류, FD_CLOSE 및 소멸 시 동일한 경로로 정리한다.
	void CloseConnection();
	bool IsValidReceiveHead(INT8 head) const;
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
	char mReceiveBuffer[MAX_PACKET_PAYLOAD_SIZE];
	std::size_t mExpectedPayloadBytes = 0;
	int mReceivedBytes = 0;
	INT8 mReceiveHead = -1;
	bool mHasReceiveHead = false;
	bool mHasPayloadSize = false;

	std::array<CS_CLIENTS_INFO, MAX_CLIENT> mClientInfo;
	std::array<std::shared_ptr<CPlayer>, MAX_CLIENT> mPlayers;
	std::deque<PendingSend> mSendQueue;
	std::size_t mPendingSendBytes = 0;

	SOCKET_STATE mSocketState = SOCKET_STATE::SEND_GAME_START;
	INT8 mMainClientId = -1;
	INT8 mClientCount = -1;
	INT8 mSelectedSlot = -1;
	int mEscapeDoorId = -1;
	bool mLoadingCompleteReceived = false;
	bool mWsaStarted = false;
};

