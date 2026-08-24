#include "stdafx.h"
#include "TCPClient.h"
#include "GameFramework.h"
#include "Player.h"
#include "SharedObject.h"
#include "Sound.h"

#include <cmath>
#include <cstdio>

namespace
{
	constexpr size_t MAX_PENDING_SEND_BYTES = 4 * 1024 * 1024;
	constexpr UINT SERVER_PORT = 9000;
	constexpr auto INPUT_SEND_INTERVAL = std::chrono::microseconds{ 16'667 };
	constexpr size_t MAX_SPACEOUT_OBJECTS_PER_PACKET =
		MAX_PACKET_PAYLOAD_SIZE / sizeof(SC_SPACEOUT_OBJECT);
	static_assert(MAX_SPACEOUT_OBJECTS_PER_PACKET > 0);

	void ShowSocketError(HWND window, const char* operation, int errorCode)
	{
		LPSTR errorMessage = nullptr;
		FormatMessageA(
			FORMAT_MESSAGE_ALLOCATE_BUFFER |
			FORMAT_MESSAGE_FROM_SYSTEM |
			FORMAT_MESSAGE_IGNORE_INSERTS,
			nullptr,
			errorCode,
			MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
			reinterpret_cast<LPSTR>(&errorMessage),
			0,
			nullptr);

		MessageBoxA(
			window,
			errorMessage ? errorMessage : "Unknown socket error.",
			operation,
			MB_OK | MB_ICONERROR);
		LocalFree(errorMessage);
	}

	void LogSocketEventError(int errorCode)
	{
		char message[128] = {};
		sprintf_s(message, "[Network] Socket event failed: error=%d\n", errorCode);
		OutputDebugStringA(message);
	}

	void ConvertWideStringToUtf8(const wchar_t* source, char* destination, int destinationSize)
	{
		WideCharToMultiByte(
			CP_UTF8,
			0,
			source,
			-1,
			destination,
			destinationSize,
			nullptr,
			nullptr);
	}

	void LogInvalidServerPacket(const char* packetName, const char* fieldName, int value)
	{
		char message[256] = {};
		sprintf_s(
			message,
			"[Network] Invalid server packet: packet=%s, field=%s, value=%d\n",
			packetName,
			fieldName,
			value);
		OutputDebugStringA(message);
	}

	void LogInvalidServerPacket(
		const char* packetName,
		const char* fieldName,
		size_t index,
		int value)
	{
		char message[256] = {};
		sprintf_s(
			message,
			"[Network] Invalid server packet: packet=%s, field=%s, index=%zu, value=%d\n",
			packetName,
			fieldName,
			index,
			value);
		OutputDebugStringA(message);
	}

	void LogInvalidServerPacketAtIndex(
		const char* packetName,
		const char* fieldName,
		size_t index)
	{
		char message[256] = {};
		sprintf_s(
			message,
			"[Network] Invalid server packet: packet=%s, field=%s, index=%zu\n",
			packetName,
			fieldName,
			index);
		OutputDebugStringA(message);
	}

	bool IsAssignedClientId(INT8 clientId)
	{
		return clientId >= 0 && clientId < static_cast<INT8>(MAX_CLIENT);
	}

	bool IsValidClientCount(INT8 clientCount)
	{
		return clientCount >= 0 && clientCount <= static_cast<INT8>(MAX_CLIENT);
	}

	bool IsValidObjectId(int objectId)
	{
		return objectId >= 0 && objectId < g_collisionManager.GetNumOfCollisionObject();
	}

	bool IsFiniteVector(const XMFLOAT3& value)
	{
		// NaN이나 무한대가 카메라·애니메이션·충돌 계산으로 전파되지 않도록 한다.
		return std::isfinite(value.x) &&
			std::isfinite(value.y) &&
			std::isfinite(value.z);
	}

	bool IsFiniteMatrix(const XMFLOAT4X4& value)
	{
		for (size_t row = 0; row < 4; ++row)
		{
			for (size_t column = 0; column < 4; ++column)
			{
				if (!std::isfinite(value.m[row][column]))
				{
					return false;
				}
			}
		}
		return true;
	}

	bool ValidateRosterPlayerStates(const std::array<CS_PLAYER_STATE, MAX_CLIENT>& playerStates)
	{
		for (size_t index = 0; index < playerStates.size(); ++index)
		{
			const CS_PLAYER_STATE& playerState = playerStates[index];
			const bool hasValidClientId =
				playerState.m_nClientId == -1 || IsAssignedClientId(playerState.m_nClientId);
			if (!hasValidClientId)
			{
				LogInvalidServerPacket(
					"CLIENT_INFO",
					"clientId",
					index,
					static_cast<int>(playerState.m_nClientId));
				return false;
			}
		}
		return true;
	}

	bool ValidateReplicatedPlayerStates(const std::array<CS_PLAYER_STATE, MAX_CLIENT>& playerStates)
	{
		for (size_t playerIndex = 0; playerIndex < playerStates.size(); ++playerIndex)
		{
			const CS_PLAYER_STATE& state = playerStates[playerIndex];
			if (state.m_nClientId == -1)
			{
				continue;
			}

			if (!IsAssignedClientId(state.m_nClientId))
			{
				LogInvalidServerPacket(
					"PLAYER_STATE",
					"clientId",
					playerIndex,
					static_cast<int>(state.m_nClientId));
				return false;
			}
			if (!IsFiniteVector(state.m_xmf3Position) ||
				!IsFiniteVector(state.m_xmf3Velocity) ||
				!IsFiniteVector(state.m_xmf3Look) ||
				!std::isfinite(state.m_fPitch))
			{
				LogInvalidServerPacketAtIndex(
					"PLAYER_STATE",
					"transform",
					playerIndex);
				return false;
			}
		}
		return true;
	}

}

CTcpClient::CTcpClient()
{}

CTcpClient::~CTcpClient()
{
	CloseConnection();
}

void CTcpClient::CloseConnection()
{
	if (m_sock != INVALID_SOCKET)
	{
		shutdown(m_sock, SD_BOTH);
		closesocket(m_sock);
		m_sock = INVALID_SOCKET;
	}

	if (mWsaStarted)
	{
		WSACleanup();
		mWsaStarted = false;
	}

	mSendQueue.clear();
	mPendingSendBytes = 0;
	mNextInputSendTime = {};
	ResetReceiveState();
}

void CTcpClient::ResetReceiveState()
{
	mReceivedBytes = 0;
	mHasReceiveHead = false;
	mHasPayloadSize = false;
	mReceiveHead = ReceiveHead::Invalid;
	mExpectedPayloadBytes = 0;
	std::fill(mReceiveBuffer.begin(), mReceiveBuffer.end(), 0);
}

bool CTcpClient::CreateSocket(HWND window, const TCHAR* ipAddress)
{
	CloseConnection();

	// 윈속 초기화
	WSADATA wsa;
	const int startupResult = WSAStartup(MAKEWORD(2, 2), &wsa);
	if (startupResult != 0)
	{
		// WSAStartup은 WSAGetLastError()가 아니라 반환값 자체가 오류 코드다.
		ShowSocketError(window, "WSAStartup", startupResult);
		return false;
	}
	mWsaStarted = true;

	// 소켓 생성
	m_sock = socket(AF_INET, SOCK_STREAM, 0);
	if (m_sock == INVALID_SOCKET)
	{
		const int errorCode = WSAGetLastError();
		ShowSocketError(window, "socket()", errorCode);
		CloseConnection();
		return false;
	}

	char ipAddressBuffer[20] = {};
	ConvertWideStringToUtf8(ipAddress, ipAddressBuffer, static_cast<int>(std::size(ipAddressBuffer)));

	struct sockaddr_in serverAddress = {};
	serverAddress.sin_family = AF_INET;
	inet_pton(AF_INET, ipAddressBuffer, &serverAddress.sin_addr);
	serverAddress.sin_port = htons(SERVER_PORT);

	int result = connect(
		m_sock,
		reinterpret_cast<sockaddr*>(&serverAddress),
		sizeof(serverAddress));
	if (result == SOCKET_ERROR)
	{
		const int errorCode = WSAGetLastError();
		ShowSocketError(window, "connect()", errorCode);
		CloseConnection();
		return false;
	}

	result = WSAAsyncSelect(m_sock, window, WM_SOCKET, FD_CLOSE | FD_READ | FD_WRITE);
	if (result == SOCKET_ERROR)
	{
		const int errorCode = WSAGetLastError();
		ShowSocketError(window, "WSAAsyncSelect()", errorCode);
		CloseConnection();
		return false;
	}

	return true;
}

void CTcpClient::OnProcessingSocketMessage(HWND window, UINT, WPARAM wParam, LPARAM lParam)
{
	const int socketError = WSAGETSELECTERROR(lParam);
	if (socketError != 0)
	{
		// 사용자 알림은 GameFramework의 연결 종료 경로에서 한 번만 표시한다.
		LogSocketEventError(socketError);
		if (static_cast<SOCKET>(wParam) == m_sock)
		{
			CloseConnection();
		}
		return;
	}

	switch (WSAGETSELECTEVENT(lParam))
	{
	case FD_READ:	// 소켓이 데이터를 읽을 준비가 되었다.
		ProcessReadEvent(window, static_cast<SOCKET>(wParam));
		break;
	case FD_WRITE:	// 소켓이 데이터를 전송할 준비가 되었다.
		ProcessWriteEvent();
		break;
	case FD_CLOSE:
		if (static_cast<SOCKET>(wParam) == m_sock)
		{
			CloseConnection();
		}
		break;
	default:
		break;
	}
}

bool CTcpClient::HandleReceiveResult(ReceiveResult result)
{
	if (result == ReceiveResult::Complete)
	{
		return true;
	}

	if (result == ReceiveResult::Closed || result == ReceiveResult::Error)
	{
		CloseConnection();
	}
	return false;
}

void CTcpClient::ProcessReadEvent(HWND window, SOCKET socket)
{

	if (!mHasReceiveHead)
	{
		const ReceiveResult result = ReceiveData(socket, sizeof(INT8));
		if (!HandleReceiveResult(result))
		{
			return;
		}

		INT8 rawReceiveHead = -1;
		memcpy(&rawReceiveHead, mReceiveBuffer.data(), sizeof(rawReceiveHead));
		std::fill(mReceiveBuffer.begin(), mReceiveBuffer.end(), 0);

		// 등록되지 않은 HEAD는 payload 크기와 형식을 결정할 수 없으므로 스트림 해석을 중단한다.
		// 연결을 종료해 이후 바이트를 다음 패킷의 HEAD로 잘못 처리하는 상황도 방지한다.
		if (!IsValidReceiveHead(rawReceiveHead))
		{
			LogInvalidServerPacket("HEADER", "head", static_cast<int>(rawReceiveHead));
			CloseConnection();
			return;
		}

		mReceiveHead = static_cast<ReceiveHead>(rawReceiveHead);
		mHasReceiveHead = true;
	}

	switch (mReceiveHead)
	{
	case ReceiveHead::GameStart:
		PostMessage(window, WM_START_GAME, 0, 0);
		mSocketState = SOCKET_STATE::SEND_KEY_BUFFER;
		mNextInputSendTime = std::chrono::steady_clock::now();
		break;
	case ReceiveHead::ChangeSlot:
		if (!TryProcessChangeSlotPacket(window, socket))
		{
			return;
		}
		break;
	case ReceiveHead::Init:
		if (!TryProcessInitPacket(socket))
		{
			return;
		}
		break;
	case ReceiveHead::PlayerState:
		if (!TryProcessPlayerStatePacket(socket))
		{
			return;
		}
		break;
	case ReceiveHead::NearbyObjects:
		if (!TryProcessNearbyObjectsPacket(socket))
		{
			return;
		}
		break;
	case ReceiveHead::ClientCount:
		if (!TryProcessClientCountPacket(socket))
		{
			return;
		}
		break;
	case ReceiveHead::BlueSuitWin:
		PostMessage(window, WM_END_GAME, 0, 0);
		break;
	case ReceiveHead::ZombieWin:
		PostMessage(window, WM_END_GAME, 1, 0);
		break;
	case ReceiveHead::OpenDrawerSound:
	{
		SoundManager& soundManager = SoundManager::GetInstance();
		soundManager.PlaySoundWithName(sound::OPEN_DRAWER);
		break;
	}
	case ReceiveHead::CloseDrawerSound:
	{
		SoundManager& soundManager = SoundManager::GetInstance();
		soundManager.PlaySoundWithName(sound::CLOSE_DRAWER);
		break;
	}
	case ReceiveHead::OpenDoorSound:
	{
		SoundManager& soundManager = SoundManager::GetInstance();
		soundManager.PlaySoundWithName(sound::OPEN_DOOR);
		break;
	}
	case ReceiveHead::CloseDoorSound:
	{
		SoundManager& soundManager = SoundManager::GetInstance();
		soundManager.PlaySoundWithName(sound::CLOSE_DOOR);
		break;
	}
	case ReceiveHead::BlueSuitDead:
		if (!TryProcessBlueSuitDeadPacket(socket))
		{
			return;
		}
		break;
	case ReceiveHead::SpaceOutObjects:
		if (!TryProcessSpaceOutObjectsPacket(socket))
		{
			return;
		}
		break;
	case ReceiveHead::LoadingComplete:
	{
		mLoadingCompleteReceived = true;
		break;
	}
	default:
		break;
	}

	ResetReceiveState();
}

bool CTcpClient::TryProcessChangeSlotPacket(HWND window, SOCKET socket)
{
	if (!HandleReceiveResult(ReceiveData(socket, sizeof(INT8) + sizeof(mClientInfo))))
	{
		return false;
	}

	// 고정 패킷 전체를 지역 변수에 먼저 역직렬화한다.
	// 이후 검증이 추가되더라도 일부 멤버만 먼저 변경되는 상황을 방지한다.
	INT8 receivedMainClientId = -1;
	std::array<CS_PLAYER_STATE, MAX_CLIENT> receivedClientInfo = {};
	memcpy(&receivedMainClientId, mReceiveBuffer.data(), sizeof(receivedMainClientId));
	memcpy(
		receivedClientInfo.data(),
		mReceiveBuffer.data() + sizeof(receivedMainClientId),
		sizeof(receivedClientInfo));

	if (!IsAssignedClientId(receivedMainClientId))
	{
		LogInvalidServerPacket(
			"CHANGE_SLOT",
			"mainClientId",
			static_cast<int>(receivedMainClientId));
		CloseConnection();
		return false;
	}
	if (!ValidateRosterPlayerStates(receivedClientInfo))
	{
		CloseConnection();
		return false;
	}

	const INT8 previousMainClientId = mMainClientId;
	mMainClientId = receivedMainClientId;
	mClientInfo = receivedClientInfo;
	for (size_t playerIndex = 0; playerIndex < MAX_CLIENT; ++playerIndex)
	{
		if (mPlayers[playerIndex])
		{
			mPlayers[playerIndex]->SetClientId(mClientInfo[playerIndex].m_nClientId);
		}
	}

	if (previousMainClientId != mMainClientId)
	{
		PostMessage(window, WM_CHANGE_SLOT, 1, 0);
	}
	return true;
}

bool CTcpClient::TryProcessInitPacket(SOCKET socket)
{
	if (!HandleReceiveResult(ReceiveData(socket, sizeof(INT8) * 2 + sizeof(mClientInfo))))
	{
		return false;
	}

	INT8 receivedMainClientId = -1;
	INT8 receivedClientCount = -1;
	std::array<CS_PLAYER_STATE, MAX_CLIENT> receivedClientInfo = {};
	memcpy(&receivedMainClientId, mReceiveBuffer.data(), sizeof(receivedMainClientId));
	memcpy(
		&receivedClientCount,
		mReceiveBuffer.data() + sizeof(receivedMainClientId),
		sizeof(receivedClientCount));
	memcpy(
		receivedClientInfo.data(),
		mReceiveBuffer.data() + sizeof(receivedMainClientId) + sizeof(receivedClientCount),
		sizeof(receivedClientInfo));

	if (!IsAssignedClientId(receivedMainClientId))
	{
		LogInvalidServerPacket(
			"INIT",
			"mainClientId",
			static_cast<int>(receivedMainClientId));
		CloseConnection();
		return false;
	}
	if (!IsValidClientCount(receivedClientCount))
	{
		LogInvalidServerPacket(
			"INIT",
			"clientCount",
			static_cast<int>(receivedClientCount));
		CloseConnection();
		return false;
	}
	if (!ValidateRosterPlayerStates(receivedClientInfo))
	{
		CloseConnection();
		return false;
	}

	// 패킷 전체를 읽은 뒤 관련 멤버를 함께 갱신한다.
	mMainClientId = receivedMainClientId;
	mClientCount = receivedClientCount;
	mClientInfo = receivedClientInfo;
	return true;
}

bool CTcpClient::TryProcessNearbyObjectsPacket(SOCKET socket)
{
	if (!mHasPayloadSize)
	{
		if (!HandleReceiveResult(ReceiveData(socket, sizeof(std::uint16_t))))
		{
			return false;
		}

		std::uint16_t payloadBytes = 0;
		memcpy(&payloadBytes, mReceiveBuffer.data(), sizeof(payloadBytes));
		mExpectedPayloadBytes = payloadBytes;
		mHasPayloadSize = true;
		std::fill(mReceiveBuffer.begin(), mReceiveBuffer.end(), 0);

		if (mExpectedPayloadBytes > sizeof(CS_NEARBY_OBJECT) * MAX_NEARBY_OBJECTS ||
			mExpectedPayloadBytes % sizeof(CS_NEARBY_OBJECT) != 0)
		{
			LogInvalidServerPacket(
				"NEARBY_OBJECTS",
				"payloadSize",
				static_cast<int>(mExpectedPayloadBytes));
			CloseConnection();
			return false;
		}
	}

	if (!HandleReceiveResult(ReceiveData(socket, mExpectedPayloadBytes)))
	{
		return false;
	}

	const size_t objectCount = mExpectedPayloadBytes / sizeof(CS_NEARBY_OBJECT);
	const auto readNearbyObject = [this](size_t objectIndex)
		{
			CS_NEARBY_OBJECT objectInfo = {};
			memcpy(
				&objectInfo,
				mReceiveBuffer.data() + objectIndex * sizeof(CS_NEARBY_OBJECT),
				sizeof(objectInfo));
			return objectInfo;
		};

	// 전체 entry를 먼저 검증해 잘못된 패킷이 게임 상태에 일부만 적용되지 않게 한다.
	for (size_t objectIndex = 0; objectIndex < objectCount; ++objectIndex)
	{
		const CS_NEARBY_OBJECT objectInfo = readNearbyObject(objectIndex);
		if (!IsValidObjectId(objectInfo.m_nObjectId))
		{
			LogInvalidServerPacket(
				"NEARBY_OBJECTS",
				"objectId",
				objectIndex,
				objectInfo.m_nObjectId);
			CloseConnection();
			return false;
		}
		if (!IsFiniteMatrix(objectInfo.m_xmf4x4World))
		{
			LogInvalidServerPacketAtIndex(
				"NEARBY_OBJECTS",
				"worldTransform",
				objectIndex);
			CloseConnection();
			return false;
		}
	}

#ifdef LOADSCENE
	for (size_t objectIndex = 0; objectIndex < objectCount; ++objectIndex)
	{
		const CS_NEARBY_OBJECT objectInfo = readNearbyObject(objectIndex);
		shared_ptr<CGameObject> gameObject =
			g_collisionManager.GetCollisionObjectWithNumber(objectInfo.m_nObjectId).lock();
		if (gameObject)
		{
			gameObject->m_xmf4x4World = objectInfo.m_xmf4x4World;
			gameObject->m_xmf4x4ToParent = objectInfo.m_xmf4x4World;
		}
	}
#endif // LOADSCENE
	return true;
}

bool CTcpClient::TryProcessPlayerStatePacket(SOCKET socket)
{
	std::array<CS_PLAYER_STATE, MAX_CLIENT> playerStates = {};
	if (!HandleReceiveResult(ReceiveData(socket, sizeof(playerStates))))
	{
		return false;
	}

	memcpy(playerStates.data(), mReceiveBuffer.data(), sizeof(playerStates));
	if (!ValidateReplicatedPlayerStates(playerStates))
	{
		CloseConnection();
		return false;
	}

	mClientInfo = playerStates;
	ApplyServerUpdate();
	return true;
}

bool CTcpClient::TryProcessClientCountPacket(SOCKET socket)
{
	if (!HandleReceiveResult(ReceiveData(socket, sizeof(INT8) + sizeof(mClientInfo))))
	{
		return false;
	}

	INT8 receivedClientCount = -1;
	std::array<CS_PLAYER_STATE, MAX_CLIENT> receivedClientInfo = {};
	memcpy(&receivedClientCount, mReceiveBuffer.data(), sizeof(receivedClientCount));
	memcpy(
		receivedClientInfo.data(),
		mReceiveBuffer.data() + sizeof(receivedClientCount),
		sizeof(receivedClientInfo));

	if (!IsValidClientCount(receivedClientCount))
	{
		LogInvalidServerPacket(
			"NUM_OF_CLIENT",
			"clientCount",
			static_cast<int>(receivedClientCount));
		CloseConnection();
		return false;
	}
	if (!ValidateRosterPlayerStates(receivedClientInfo))
	{
		CloseConnection();
		return false;
	}

	mClientCount = receivedClientCount;
	mClientInfo = receivedClientInfo;
	for (size_t playerIndex = 0; playerIndex < MAX_CLIENT; ++playerIndex)
	{
		if (mPlayers[playerIndex])
		{
			mPlayers[playerIndex]->SetClientId(mClientInfo[playerIndex].m_nClientId);
		}
	}
	return true;
}

bool CTcpClient::TryProcessBlueSuitDeadPacket(SOCKET socket)
{
	if (!HandleReceiveResult(ReceiveData(socket, sizeof(char))))
	{
		return false;
	}

	char deadUserId = -1;
	memcpy(&deadUserId, mReceiveBuffer.data(), sizeof(deadUserId));
	if (deadUserId < 0 || deadUserId >= static_cast<char>(MAX_CLIENT) || !mPlayers[deadUserId])
	{
		CloseConnection();
		return false;
	}

	SoundManager& soundManager = SoundManager::GetInstance();
	soundManager.PlaySoundWithName(sound::DEAD_BLUESUIT);
	soundManager.SetVolume(sound::DEAD_BLUESUIT, mPlayers[deadUserId]->GetPlayerVolume());
	return true;
}

bool CTcpClient::TryProcessSpaceOutObjectsPacket(SOCKET socket)
{
	if (!mHasPayloadSize)
	{
		if (!HandleReceiveResult(ReceiveData(socket, sizeof(std::uint16_t))))
		{
			return false;
		}

		std::uint16_t bufferSize = 0;
		memcpy(&bufferSize, mReceiveBuffer.data(), sizeof(bufferSize));
		mExpectedPayloadBytes = bufferSize;
		mHasPayloadSize = true;
		std::fill(mReceiveBuffer.begin(), mReceiveBuffer.end(), 0);

		if (mExpectedPayloadBytes == 0 ||
			mExpectedPayloadBytes > MAX_PACKET_PAYLOAD_SIZE ||
			mExpectedPayloadBytes % sizeof(SC_SPACEOUT_OBJECT) != 0)
		{
			LogInvalidServerPacket(
				"SPACEOUT_OBJECTS",
				"payloadSize",
				static_cast<int>(mExpectedPayloadBytes));
			CloseConnection();
			return false;
		}
	}

	if (!HandleReceiveResult(ReceiveData(socket, mExpectedPayloadBytes)))
	{
		return false;
	}

	const size_t objectCount = mExpectedPayloadBytes / sizeof(SC_SPACEOUT_OBJECT);
	if (objectCount == 0 || objectCount > MAX_SPACEOUT_OBJECTS_PER_PACKET)
	{
		LogInvalidServerPacket(
			"SPACEOUT_OBJECTS",
			"objectCount",
			static_cast<int>(objectCount));
		CloseConnection();
		return false;
	}

	vector<SC_SPACEOUT_OBJECT> spaceOutObjects(objectCount);
	memcpy(spaceOutObjects.data(), mReceiveBuffer.data(), mExpectedPayloadBytes);

	// 하나라도 잘못된 ID나 변환값이 있으면 일부 오브젝트만 먼저 갱신하지 않고 패킷 전체를 거부한다.
	for (size_t objectIndex = 0; objectIndex < spaceOutObjects.size(); ++objectIndex)
	{
		const SC_SPACEOUT_OBJECT& objectInfo = spaceOutObjects[objectIndex];
		const int objectId = objectInfo.m_iObjectId;
		if (!IsValidObjectId(objectId))
		{
			LogInvalidServerPacket(
				"SPACEOUT_OBJECTS",
				"objectId",
				objectIndex,
				objectId);
			CloseConnection();
			return false;
		}
		if (!IsFiniteMatrix(objectInfo.m_xmf4x4World))
		{
			LogInvalidServerPacketAtIndex(
				"SPACEOUT_OBJECTS",
				"worldTransform",
				objectIndex);
			CloseConnection();
			return false;
		}
	}

	for (const SC_SPACEOUT_OBJECT& objectInfo : spaceOutObjects)
	{
		shared_ptr<CGameObject> gameObject =
			g_collisionManager.GetCollisionObjectWithNumber(objectInfo.m_iObjectId).lock();
		if (gameObject)
		{
			gameObject->m_xmf4x4World = objectInfo.m_xmf4x4World;
			gameObject->m_xmf4x4ToParent = objectInfo.m_xmf4x4World;
			gameObject->SetObtain(false);
		}
	}
	return true;
}

bool CTcpClient::IsValidReceiveHead(INT8 head) const
{
	// 클라이언트가 payload 크기와 처리 방법을 알고 있는 서버 패킷만 허용한다.
	switch (static_cast<ReceiveHead>(head))
	{
	case ReceiveHead::Init:
	case ReceiveHead::ClientCount:
	case ReceiveHead::BlueSuitWin:
	case ReceiveHead::ZombieWin:
	case ReceiveHead::GameStart:
	case ReceiveHead::ChangeSlot:
	case ReceiveHead::OpenDrawerSound:
	case ReceiveHead::CloseDrawerSound:
	case ReceiveHead::OpenDoorSound:
	case ReceiveHead::CloseDoorSound:
	case ReceiveHead::BlueSuitDead:
	case ReceiveHead::SpaceOutObjects:
	case ReceiveHead::LoadingComplete:
	case ReceiveHead::PlayerState:
	case ReceiveHead::NearbyObjects:
		return true;
	default:
		return false;
	}
}

bool CTcpClient::TryGetCollisionObject(
	int objectId,
	const char* fieldName,
	shared_ptr<CGameObject>& gameObject)
{
	// 충돌 관리자의 조회 함수는 vector::operator[]를 사용하므로 반드시 먼저 범위를 검사한다.
	if (!IsValidObjectId(objectId))
	{
		LogInvalidServerPacket("CLIENT_INFO", fieldName, objectId);
		CloseConnection();
		gameObject.reset();
		return false;
	}

	gameObject = g_collisionManager.GetCollisionObjectWithNumber(objectId).lock();
	return true;
}

void CTcpClient::ApplyServerUpdate()
{
	for (int playerIndex = 0; playerIndex < MAX_CLIENT; ++playerIndex)
	{
		const auto& player = mPlayers[playerIndex];
		const auto& clientInfo = mClientInfo[playerIndex];

		if (player)
		{
			player->SetAlive(clientInfo.m_bAlive);
			player->SetRunning(clientInfo.m_bRunning);
			player->SetClientId(clientInfo.m_nClientId);
			player->SetPosition(clientInfo.m_xmf3Position);
			player->SetVelocity(clientInfo.m_xmf3Velocity);

			if (playerIndex != mMainClientId)
			{
				player->SetPitch(clientInfo.m_fPitch);
				player->SetLook(clientInfo.m_xmf3Look);
				XMFLOAT3 worldUp(0.0f, 1.0f, 0.0f);
				XMFLOAT3 look = clientInfo.m_xmf3Look;
				const XMFLOAT3 right = Vector3::CrossProduct(worldUp, look, true);
				player->SetRight(right);
			}

			UpdatePickedObject(playerIndex);

			// 지뢰 충돌
			const int mineObjectId = clientInfo.m_playerInfo.m_iMineobjectNum;
			if (mineObjectId != -1)
			{
				shared_ptr<CGameObject> gameObject;
				if (!TryGetCollisionObject(mineObjectId, "mineObjectId", gameObject))
				{
					return;
				}
				auto mine = dynamic_pointer_cast<CMineObject>(gameObject);
				if (mine)
				{
					mine->SetCollide(true);
					shared_ptr<CZombiePlayer> zombiePlayer =
						dynamic_pointer_cast<CZombiePlayer>(player);
					if (zombiePlayer)
					{
						zombiePlayer->SetEectricShock();
					}

					const float volume = player->GetPlayerVolume();
					SoundManager& soundManager = SoundManager::GetInstance();
					if (volume - EPSILON >= 0.0f)
					{
						soundManager.PlaySoundWithName(sound::ACTIVE_MINE, volume);
					}
				}
			}
		}

		if (playerIndex == ZOMBIEPLAYER)
		{
			UpdateZombiePlayer();
		}
		else
		{
			UpdateSurvivorPlayer(playerIndex);
		}

	}
}

void CTcpClient::UpdatePickedObject(int playerIndex)
{
	if (playerIndex != mMainClientId)
	{
		return;
	}

	const int objectId = mClientInfo[playerIndex].m_nPickedObjectNum;
	if (objectId == -1)
	{
		mPlayers[playerIndex]->SetPickedObject(nullptr);
		return;
	}

	shared_ptr<CGameObject> gameObject;
	if (!TryGetCollisionObject(objectId, "pickedObjectId", gameObject))
	{
		return;
	}
	if (gameObject)
	{
		mPlayers[playerIndex]->SetPickedObject(gameObject);
	}
}

void CTcpClient::ProcessWriteEvent()
{
	if (mSendQueue.empty() && mSocketState == SOCKET_STATE::SEND_GAME_START)
	{
		// 기존 연결 직후 최초 FD_WRITE가 게임 시작 요청을 발생시키던 동작을 유지한다.
		RequestSend();
		return;
	}

	if (FlushSendQueue() == SendResult::Error)
	{
		CloseConnection();
	}
}

void CTcpClient::SendInputIfDue()
{
	if (mSocketState != SOCKET_STATE::SEND_KEY_BUFFER)
	{
		return;
	}

	const auto currentTime = std::chrono::steady_clock::now();
	if (currentTime < mNextInputSendTime)
	{
		return;
	}

	RequestSend();

	// 놓친 주기는 건너뛰되 기존 기준 시각을 유지해 프레임별 초과 시간이 누적되지 않게 한다.
	const auto elapsedIntervals = (currentTime - mNextInputSendTime) / INPUT_SEND_INTERVAL + 1;
	mNextInputSendTime += INPUT_SEND_INTERVAL * elapsedIntervals;
}

void CTcpClient::RequestSend()
{
	// TCP 송수신은 독립적이므로 partial recv 대기 중에도 클라이언트 입력은 계속 전송한다.
	const bool cannotSend =
		m_sock == INVALID_SOCKET ||
		mMainClientId < 0 ||
		mMainClientId >= static_cast<INT8>(MAX_CLIENT) ||
		!mPlayers[mMainClientId];
	if (cannotSend)
	{
		return;
	}

	switch (mSocketState)
	{
	case SOCKET_STATE::SEND_GAME_START:
		SubmitSendData(static_cast<INT8>(SOCKET_STATE::SEND_GAME_START));
		break;
	case SOCKET_STATE::SEND_CHANGE_SLOT:
		if (SubmitSendData(static_cast<INT8>(SOCKET_STATE::SEND_CHANGE_SLOT), mSelectedSlot))
		{
			mSocketState = SOCKET_STATE::SEND_GAME_START;
		}
		break;
	case SOCKET_STATE::SEND_KEY_BUFFER:
	{
		const shared_ptr<CPlayer>& player = mPlayers[mMainClientId];
		const shared_ptr<CCamera> camera = player->GetCamera();
		const UCHAR* keysBuffer = CGameFramework::GetKeysBuffer();
		const WORD keyMask = BuildKeyBitMask(keysBuffer);
		const bool usesPlayerBasis = player->m_pSkinnedAnimationController->IsAnimation();
		const XMFLOAT3 look = usesPlayerBasis ? player->GetLook() : camera->GetLookVector();
		const XMFLOAT3 right = usesPlayerBasis ? player->GetRight() : camera->GetRightVector();
		const XMFLOAT3 up = usesPlayerBasis ? player->GetUp() : camera->GetUpVector();
		const float pitch = player->GetPitch();
		const std::uint8_t rightClick = player->IsRightClick() ? 1 : 0;

		mClientInfo[mMainClientId].m_fPitch = pitch;
		player->SetRightClick(false);
		SubmitSendData(
			static_cast<INT8>(SOCKET_STATE::SEND_KEY_BUFFER),
			keyMask,
			camera->GetViewMatrix(),
			look,
			right,
			up,
			pitch,
			rightClick
		);
		break;
	}
	default:
		break;
	}
}

template<class... Args>
bool CTcpClient::SubmitSendData(Args&&... args)
{
	const size_t bufferSize = (sizeof(args) + ... + 0);
	if (bufferSize == 0 ||
		mPendingSendBytes > MAX_PENDING_SEND_BYTES ||
		bufferSize > MAX_PENDING_SEND_BYTES - mPendingSendBytes)
	{
		CloseConnection();
		return false;
	}

	std::vector<char> buffer(bufferSize);
	size_t offset = 0;
	((memcpy(buffer.data() + offset, &args, sizeof(args)), offset += sizeof(args)), ...);

	mPendingSendBytes += buffer.size();
	mSendQueue.push_back(PendingSend{ std::move(buffer), 0 });
	if (FlushSendQueue() == SendResult::Error)
	{
		CloseConnection();
		return false;
	}
	return true;
}

CTcpClient::SendResult CTcpClient::FlushSendQueue()
{
	while (!mSendQueue.empty())
	{
		PendingSend& pending = mSendQueue.front();
		const size_t remainingBytes = pending.buffer.size() - pending.sentBytes;
		const int sentBytes = send(
			m_sock,
			pending.buffer.data() + pending.sentBytes,
			static_cast<int>(remainingBytes),
			0);

		if (sentBytes > 0)
		{
			pending.sentBytes += static_cast<size_t>(sentBytes);
			if (pending.sentBytes == pending.buffer.size())
			{
				mPendingSendBytes -= pending.buffer.size();
				mSendQueue.pop_front();
			}
			continue;
		}

		if (sentBytes == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)
		{
			return SendResult::Pending;
		}
		return SendResult::Error;
	}
	return SendResult::Complete;
}

CTcpClient::ReceiveResult CTcpClient::ReceiveData(SOCKET socket, size_t expectedBytes)
{
	const bool hasInvalidBufferState =
		expectedBytes > MAX_PACKET_PAYLOAD_SIZE ||
		mReceivedBytes < 0 ||
		static_cast<size_t>(mReceivedBytes) > expectedBytes;

	if (hasInvalidBufferState)
	{
		return ReceiveResult::Error;
	}

	if (expectedBytes == 0)
	{
		return ReceiveResult::Complete;
	}

	const int remainingBytes = static_cast<int>(expectedBytes) - mReceivedBytes;
	const int receivedBytes = recv(socket, mReceiveBuffer.data() + mReceivedBytes, remainingBytes, 0);
	if (receivedBytes > 0)
	{
		mReceivedBytes += receivedBytes;
	}
	else if (receivedBytes == 0)
	{
		return ReceiveResult::Closed;
	}
	else
	{
		const int errorCode = WSAGetLastError();
		if (errorCode == WSAEWOULDBLOCK)
		{
			return ReceiveResult::Pending;
		}
		return ReceiveResult::Error;
	}

	if (static_cast<size_t>(mReceivedBytes) < expectedBytes)
	{
		return ReceiveResult::Pending;
	}

	mReceivedBytes = 0;
	return ReceiveResult::Complete;
}

WORD CTcpClient::BuildKeyBitMask(const UCHAR* keysBuffer) const
{
	WORD keyMask = 0;
	if (keysBuffer['W'] & 0xF0) { keyMask |= KEY_W; }
	if (keysBuffer['S'] & 0xF0) { keyMask |= KEY_S; }
	if (keysBuffer['A'] & 0xF0) { keyMask |= KEY_A; }
	if (keysBuffer['D'] & 0xF0) { keyMask |= KEY_D; }
	if (keysBuffer['1'] & 0xF0) { keyMask |= KEY_1; }
	if (keysBuffer['2'] & 0xF0) { keyMask |= KEY_2; }
	if (keysBuffer['3'] & 0xF0) { keyMask |= KEY_3; }
	if (keysBuffer['4'] & 0xF0) { keyMask |= KEY_4; }
	if (keysBuffer['E'] & 0xF0) { keyMask |= KEY_E; }
	if (keysBuffer[VK_LSHIFT] & 0xF0) { keyMask |= KEY_LSHIFT; }
	if (keysBuffer[VK_LBUTTON] & 0xF0) { keyMask |= KEY_LBUTTON; }
	return keyMask;
}

void CTcpClient::UpdateZombiePlayer()
{
	shared_ptr<CZombiePlayer> zombiePlayer = dynamic_pointer_cast<CZombiePlayer>(mPlayers[ZOMBIEPLAYER]);
	if (!zombiePlayer)
	{
		return;
	}

	const bool isTrackingEnabled = mClientInfo[ZOMBIEPLAYER].m_nSlotObjectNum[0] == 1;
	const bool isInterruptionEnabled = mClientInfo[ZOMBIEPLAYER].m_nSlotObjectNum[1] == 1;

	for (int playerIndex = 0; playerIndex < MAX_CLIENT; ++playerIndex)
	{
		if (mMainClientId == ZOMBIEPLAYER)	// 추적
		{
			mPlayers[playerIndex]->SetTracking(isTrackingEnabled);
		}

		if (mPlayers[playerIndex]->GetClientId() != mMainClientId || playerIndex == ZOMBIEPLAYER)
		{
			continue;
		}
		mPlayers[playerIndex]->SetInterruption(isInterruptionEnabled);
	}

	// 시야 방해(zombie 플레이어)
	if (mMainClientId == ZOMBIEPLAYER)
	{
		mPlayers[ZOMBIEPLAYER]->SetInterruption(isInterruptionEnabled);
	}

	if (mClientInfo[ZOMBIEPLAYER].m_nSlotObjectNum[2] == 1)	// 공격을 시도
	{
		zombiePlayer->m_pSkinnedAnimationController->SetTrackEnable(2, true);
	}
}

void CTcpClient::UpdateSurvivorPlayer(int playerIndex)
{
	shared_ptr<CBlueSuitPlayer> survivorPlayer =
		dynamic_pointer_cast<CBlueSuitPlayer>(mPlayers[playerIndex]);

	if (!survivorPlayer)
	{
		return;
	}

	int escapeDoorId = mEscapeDoorId;
	if (escapeDoorId == -1)
	{
		escapeDoorId = mClientInfo[playerIndex].m_playerInfo.m_iEscapeDoor;
	}
	if (escapeDoorId != -1)
	{
		shared_ptr<CGameObject> gameObject;
		if (!TryGetCollisionObject(escapeDoorId, "escapeDoorId", gameObject))
		{
			return;
		}
		if (!gameObject)
		{
			return;
		}

		mEscapeDoorId = escapeDoorId;
		survivorPlayer->SetEscapePos(gameObject->GetPosition());
	}

	survivorPlayer->SelectItem(mClientInfo[playerIndex].m_playerInfo.m_selectItem);
	for (int slotIndex = 0; slotIndex < 3; ++slotIndex)
	{
		const int slotObjectId = mClientInfo[playerIndex].m_nSlotObjectNum[slotIndex];
		if (slotObjectId != -1)
		{
			shared_ptr<CGameObject> gameObject;
			if (!TryGetCollisionObject(slotObjectId, "slotObjectId", gameObject))
			{
				return;
			}
			shared_ptr<CItemObject> itemObject = dynamic_pointer_cast<CItemObject>(gameObject);
			if (itemObject)
			{
				if (!itemObject->IsObtained())
				{
					sharedobject.EnableItemGetParticle(itemObject);
				}
				itemObject->SetObtain(true);
				if (playerIndex == mMainClientId && !survivorPlayer->IsSlotItemObtain(slotIndex))
				{
					SoundManager& soundManager = SoundManager::GetInstance();
					soundManager.PlaySoundWithName(sound::GET_ITEM_BLUESUIT);
				}
				survivorPlayer->SetSlotItem(slotIndex, slotObjectId);
			}
		}
		else
		{
			const int referenceObjectId = survivorPlayer->GetReferenceSlotItemNum(slotIndex);
			if (referenceObjectId != -1)
			{
				shared_ptr<CGameObject> gameObject;
				if (!TryGetCollisionObject(referenceObjectId, "referenceSlotObjectId", gameObject))
				{
					return;
				}
				shared_ptr<CItemObject> itemObject = dynamic_pointer_cast<CItemObject>(gameObject);
				if (itemObject)
				{
					itemObject->SetObtain(false);
					survivorPlayer->SetSlotItemEmpty(slotIndex);
				}
			}
		}
	}

	for (int fuseIndex = 0; fuseIndex < 3; ++fuseIndex)
	{
		const int fuseObjectId = mClientInfo[playerIndex].m_nFuseObjectNum[fuseIndex];
		if (fuseObjectId != -1)
		{
			shared_ptr<CGameObject> gameObject;
			if (!TryGetCollisionObject(fuseObjectId, "fuseObjectId", gameObject))
			{
				return;
			}
			shared_ptr<CItemObject> itemObject = dynamic_pointer_cast<CItemObject>(gameObject);
			if (itemObject)
			{
				if (!itemObject->IsObtained())
				{
					sharedobject.EnableItemGetParticle(itemObject);
				}
				itemObject->SetObtain(true);
				if (playerIndex == mMainClientId && !survivorPlayer->IsFuseObtain(fuseIndex))
				{
					SoundManager& soundManager = SoundManager::GetInstance();
					soundManager.PlaySoundWithName(sound::GET_ITEM_BLUESUIT);
				}
				survivorPlayer->SetFuseItem(fuseIndex, fuseObjectId);
			}
		}
		else
		{
			const int referenceObjectId = survivorPlayer->GetReferenceFuseItemNum(fuseIndex);
			if (referenceObjectId != -1)
			{
				shared_ptr<CGameObject> gameObject;
				if (!TryGetCollisionObject(referenceObjectId, "referenceFuseObjectId", gameObject))
				{
					return;
				}
				shared_ptr<CItemObject> itemObject = dynamic_pointer_cast<CItemObject>(gameObject);
				if (itemObject)
				{
					itemObject->SetObtain(false);
					survivorPlayer->SetFuseItemEmpty(fuseIndex);
				}
			}
		}
	}

	if (mClientInfo[playerIndex].m_playerInfo.m_bAttacked)
	{
		survivorPlayer->SetHitEvent();
	}

	if (mClientInfo[playerIndex].m_playerInfo.m_bTeleportItemUse)
	{
		survivorPlayer->Teleport();
	}
}

void CTcpClient::SendLoadingComplete()
{
	SubmitSendData(static_cast<INT8>(SOCKET_STATE::SEND_LOADING_COMPLETE));
}

