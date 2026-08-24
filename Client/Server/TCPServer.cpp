#include "stdafx.h"
#include "TCPServer.h"
#include "ServerObject.h"
#include "ServerEnvironmentObject.h"
#include "ServerPlayer.h"
#include "ServerCollision.h"

#include <cmath>
#include <iostream>

namespace
{
	constexpr size_t MAX_PENDING_SEND_BYTES = 4 * 1024 * 1024;
	constexpr auto STATE_REPLICATION_INTERVAL = std::chrono::microseconds{ 16'667 };
	constexpr auto NEARBY_OBJECT_REPLICATION_INTERVAL = std::chrono::microseconds{ 33'333 };
	constexpr WORD VALID_CLIENT_KEY_MASK =
		KEY_W | KEY_S | KEY_A | KEY_D |
		KEY_1 | KEY_2 | KEY_3 | KEY_4 |
		KEY_E | KEY_LSHIFT | KEY_LBUTTON;
	constexpr size_t CLIENT_INPUT_PAYLOAD_SIZE =
		sizeof(WORD) +
		sizeof(XMFLOAT4X4) +
		sizeof(XMFLOAT3) * 3 +
		sizeof(float) +
		sizeof(std::uint8_t);
	static_assert(CLIENT_INPUT_PAYLOAD_SIZE == 107);

	struct ClientInputData
	{
		WORD keyBuffer = 0;
		XMFLOAT4X4 viewMatrix = {};
		XMFLOAT3 look = {};
		XMFLOAT3 right = {};
		XMFLOAT3 up = {};
		float pitch = 0.0f;
		std::uint8_t rightClick = 0;
	};

	void LogSocketError(const char* operation, int errorCode)
	{
		LPSTR errorMessage = nullptr;
		FormatMessageA(
			FORMAT_MESSAGE_ALLOCATE_BUFFER |
			FORMAT_MESSAGE_FROM_SYSTEM |
			FORMAT_MESSAGE_IGNORE_INSERTS |
			FORMAT_MESSAGE_MAX_WIDTH_MASK,
			nullptr,
			errorCode,
			MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
			reinterpret_cast<LPSTR>(&errorMessage),
			0,
			nullptr);

		std::cerr << '[' << operation << "] error=" << errorCode;
		if (errorMessage)
		{
			std::cerr << ": " << errorMessage;
		}
		std::cerr << '\n';
		LocalFree(errorMessage);
	}

	void LogServerNotice(const char* message)
	{
		std::cerr << "[Server] " << message << '\n';
	}

	bool IsValidKeyBuffer(WORD keyBuffer)
	{
		// 정의되지 않은 비트가 있으면 클라이언트가 서버가 모르는 입력 형식을 보낸 것이다.
		const WORD invalidKeyMask = static_cast<WORD>(~VALID_CLIENT_KEY_MASK);
		return (keyBuffer & invalidKeyMask) == 0;
	}

	bool IsFiniteVector(const XMFLOAT3& value)
	{
		// NaN이나 무한대가 물리·충돌 계산으로 전파되지 않도록 모든 성분을 확인한다.
		return std::isfinite(value.x) &&
			std::isfinite(value.y) &&
			std::isfinite(value.z);
	}

	bool IsFiniteMatrix(const XMFLOAT4X4& value)
	{
		// 행렬의 한 성분이라도 유효하지 않으면 이후 방향 및 위치 계산 전체가 오염될 수 있다.
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

	void ConvertCharToWideString(const char* source, LPWSTR destination, int destinationSize)
	{
		MultiByteToWideChar(
			CP_UTF8,
			0,
			source,
			-1,
			destination,
			destinationSize);
	}
}

default_random_engine TCPServer::m_mt19937Gen;
HWND TCPServer::m_hWnd;
INT8 TCPServer::sClientCount = 0;

TCPServer::TCPServer()
{
	mPlayerStartPositions = {
		XMFLOAT3(10.0f, 0.0f, 13.5),
		XMFLOAT3(10.0f, 0.0f, -13.5),
		XMFLOAT3(-10.0f, 0.0f, 18.5),
		XMFLOAT3(-10.0f, 0.0f, -13.5),

		XMFLOAT3(10.0f, 4.5f, 13.5),
		XMFLOAT3(10.0f, 4.5f, -13.5),
		XMFLOAT3(-10.0f, 4.5f, 13.5),
		XMFLOAT3(-10.0f, 4.5f, -13.5),

		XMFLOAT3(10.0f, 9.0f, 13.5),
		XMFLOAT3(10.0f, 9.0f, -13.5),
		XMFLOAT3(-10.0f, 9.0f, 13.5),
		XMFLOAT3(-10.0f, 9.0f, -13.5),

		XMFLOAT3(10.0f, 13.5f, 13.5),
		XMFLOAT3(10.0f, 13.5f, -13.5),
		XMFLOAT3(-10.0f, 13.5f, 13.5),
		XMFLOAT3(-10.0f, 13.5f, -13.5),

		XMFLOAT3(23.0f, 13.5f, -18.f),
		XMFLOAT3(22.0f, 13.5f, -2.f),
		XMFLOAT3(17.0f, 13.5f, 19.f),
		XMFLOAT3(24.0f, 9.f, -3.f),
		XMFLOAT3(-20.0f, 9.f, -20.f),
		XMFLOAT3(23.0f, 9.f, 17.f),
		XMFLOAT3(23.0f, 4.5f, 16.f),
		XMFLOAT3(34.0f, 4.5f, -30.f),
		XMFLOAT3(33.0f, 4.5f, -13.f),
		XMFLOAT3(20.0f, 4.5f, -32.f),
		XMFLOAT3(-20.0f, 4.5f, -20.f),
		XMFLOAT3(-30.0f, 4.5f, 12.f),
	};

	mPlayerStartPositionIndices = { -1, -1, -1, -1, -1 };
	for (auto& objectSnapshot : mNearbyObjectSnapshots)
	{
		objectSnapshot.reserve(MAX_NEARBY_OBJECTS);
	}
}

TCPServer::~TCPServer()
{}

void TCPServer::OnProcessingWindowMessage(HWND hWnd, UINT nMessageID, WPARAM wParam, LPARAM lParam)
{
	switch (nMessageID)
	{
	case WM_CREATE:
		mTimer.Start();
		break;
	case WM_SOUND:
	{
		const int clientIndex = static_cast<int>(lParam);
		if (clientIndex < 0 || clientIndex >= static_cast<int>(mSocketInfos.size()) ||
			!mSocketInfos[clientIndex].isUsed)
		{
			break;
		}

		switch (wParam)
		{
		case SOUND_MESSAGE::OPEN_DRAWER:
			mSocketInfos[clientIndex].sendState = SOCKET_STATE::SEND_OPEN_DRAWER_SOUND;
			break;
		case SOUND_MESSAGE::CLOSE_DRAWER:
			mSocketInfos[clientIndex].sendState = SOCKET_STATE::SEND_CLOSE_DRAWER_SOUND;
			break;
		case SOUND_MESSAGE::OPEN_DOOR:
			mSocketInfos[clientIndex].sendState = SOCKET_STATE::SEND_OPEN_DOOR_SOUND;
			break;
		case SOUND_MESSAGE::CLOSE_DOOR:
			mSocketInfos[clientIndex].sendState = SOCKET_STATE::SEND_CLOSE_DOOR_SOUND;
			break;
		case SOUND_MESSAGE::BLUE_SUIT_DEAD:
			mSocketInfos[clientIndex].sendState = SOCKET_STATE::SEND_BLUE_SUIT_DEAD;
			break;
		default:
			return;
		}
		RequestSend(clientIndex);
		break;
	}
	case WM_OPENABLE_OBJECT_STATE:
	{
		const int objectId = static_cast<int>(wParam);
		const OpenableObjectType objectType =
			static_cast<OpenableObjectType>(LOWORD(lParam));
		const bool opened = HIWORD(lParam) != 0;
		BroadcastOpenableObjectState(objectId, objectType, opened);
		break;
	}
	default:
		break;
	}
}

void TCPServer::OnProcessingSocketMessage(HWND hWnd, UINT nMessageID, WPARAM wParam, LPARAM lParam)
{
	// 소켓 이벤트가 계속 발생해 SimulationLoop가 실행되지 않는 상황에서도 통계를 출력한다.
	ReportNetworkStatisticsIfDue();

	const int nSocketEvent = WSAGETSELECTEVENT(lParam);
	const int nSocketError = WSAGETSELECTERROR(lParam);
	if (nSocketError != 0)
	{
		LogSocketError("socket event", nSocketError);
		if (nSocketEvent != FD_ACCEPT)
		{
			DisconnectClient(static_cast<SOCKET>(wParam));
		}
		return;
	}

	switch (nSocketEvent)
	{
	case FD_ACCEPT:
		ProcessAcceptEvent(hWnd, static_cast<SOCKET>(wParam));
		break;
	case FD_READ:
		ProcessReadEvent(static_cast<SOCKET>(wParam));
		break;
	case FD_WRITE:
		ProcessWriteEvent(static_cast<SOCKET>(wParam));
		break;
	case FD_CLOSE:
		ProcessCloseEvent(static_cast<SOCKET>(wParam));
		break;
	default:
		break;
	}

	return;
}

void TCPServer::ProcessAcceptEvent(HWND hWnd, SOCKET listenSocket)
{
	struct sockaddr_in clientAddress;
	int clientAddressLength = sizeof(sockaddr_in);
	const SOCKET clientSocket = accept(
		listenSocket,
		reinterpret_cast<struct sockaddr*>(&clientAddress),
		&clientAddressLength);

	if (clientSocket == INVALID_SOCKET)
	{
		LogSocketError("accept()", WSAGetLastError());
		return;
	}

	if (mGameState == GAME_STATE::IN_GAME)
	{
		closesocket(clientSocket);
		LogServerNotice("Connection refused because the game has already started.");
		return;
	}

	const INT8 clientIndex = RegisterClientSocket(clientSocket, clientAddress, clientAddressLength);

	// MAX_CLIENT보다 더 많은 접속 요구
	if (clientIndex == -1)
	{
		closesocket(clientSocket); // 클라이언트 소켓 종료
		LogServerNotice("Connection refused because the server is full.");
		return;
	}

	const int retval = WSAAsyncSelect(clientSocket, hWnd, WM_SOCKET, FD_READ | FD_WRITE | FD_CLOSE);
	if (retval == SOCKET_ERROR)
	{
		const int errorCode = WSAGetLastError();
		LogSocketError("WSAAsyncSelect()", errorCode);
		DisconnectClient(clientSocket);
		return;
	}
	WCHAR pszList[256];
	WCHAR pszIP[16];
	ConvertCharToWideString(mSocketInfos[clientIndex].ipAddress, pszIP, 16);
	wsprintf(pszList, L"CLIENT[%d], IP: %s, 포트 번호: %d\n", clientIndex, pszIP, ntohs(mSocketInfos[clientIndex].clientAddress.sin_port));
	SendMessage(mClientListBox, LB_ADDSTRING, 0, (LPARAM)pszList);

	if (clientIndex == ZOMBIEPLAYER) // ZombiePlayer는 0번 소켓에만 생성
	{
		mPlayers[clientIndex] = make_shared<CServerZombiePlayer>();
		mPlayers[clientIndex]->SetPlayerId(clientIndex);
		++mZombieCount;
	}
	else
	{
		mPlayers[clientIndex] = make_shared<CServerBlueSuitPlayer>();
		mPlayers[clientIndex]->SetPlayerId(clientIndex);
		++mBlueSuitCount;
	}

	mCollisionManager->AddCollisionPlayer(mPlayers[clientIndex], clientIndex);
	RequestSend(clientIndex);

	for (auto& socketInfo : mSocketInfos)
	{
		if (!socketInfo.isUsed || socketInfo.socket == clientSocket)
		{
			continue;
		}
		socketInfo.sendState = SOCKET_STATE::SEND_NUM_OF_CLIENT;
		RequestSend(FindClientIndex(socketInfo.socket));
	}

	return;
}

bool TCPServer::HandleReceiveResult(ReceiveResult result, SOCKET socket)
{
	if (result == ReceiveResult::Complete)
	{
		return true;
	}

	if (result == ReceiveResult::Closed || result == ReceiveResult::Error)
	{
		DisconnectClient(socket);
	}
	return false;
}

void TCPServer::ProcessReadEvent(SOCKET socket)
{
	int clientIndex = FindClientIndex(socket);
	if (clientIndex < 0)
	{
		return;
	}

	std::shared_ptr<CServerPlayer> player = mPlayers[clientIndex];

	if (!mSocketInfos[clientIndex].hasReceiveHead)
	{
		const ReceiveResult result = ReceiveData(clientIndex, sizeof(INT8));
		if (!HandleReceiveResult(result, socket))
		{
			return;
		}

		INT8 rawReceiveHead = -1;
		memcpy(
			&rawReceiveHead,
			mSocketInfos[clientIndex].receiveBuffer.data(),
			sizeof(rawReceiveHead));
		std::fill(
			mSocketInfos[clientIndex].receiveBuffer.begin(),
			mSocketInfos[clientIndex].receiveBuffer.end(),
			0);

		// 등록되지 않은 HEAD는 payload 크기와 형식을 결정할 수 없으므로 더 이상 스트림을 해석하지 않는다.
		// 연결을 종료해 잘못된 바이트를 다음 패킷의 HEAD로 오인하는 상황도 방지한다.
		if (!IsValidReceiveHead(rawReceiveHead))
		{
			std::cerr << "Invalid receive packet head: client=" << clientIndex
				<< ", head=" << static_cast<int>(rawReceiveHead) << '\n';
			DisconnectClient(socket);
			return;
		}

		mSocketInfos[clientIndex].receiveHead = static_cast<ReceiveHead>(rawReceiveHead);
		mSocketInfos[clientIndex].hasReceiveHead = true;
	}

	bool shouldRequestSend = false;
	switch (mSocketInfos[clientIndex].receiveHead)
	{
	case ReceiveHead::GameStart:
		ProcessGameStartPacket(clientIndex);
		shouldRequestSend = true;
		break;
	case ReceiveHead::ChangeSlot:
		if (!TryProcessChangeSlotPacket(socket, clientIndex))
		{
			return;
		}
		shouldRequestSend = true;
		break;
	case ReceiveHead::KeysBuffer:
		if (!TryProcessClientInputPacket(socket, clientIndex, player))
		{
			return;
		}
		break;
	case ReceiveHead::LoadingComplete:
		shouldRequestSend = ProcessLoadingCompletePacket(clientIndex);
		break;
	default:
		break;
	}

	// HEAD와 DATA가 모두 도착해 하나의 애플리케이션 패킷이 완성된 시점에만 패킷 수를 증가시킨다.
	SocketInfo& socketInfo = mSocketInfos[clientIndex];
	socketInfo.networkStatistics.RecordReceivedPacket(
		static_cast<std::uint8_t>(socketInfo.receiveHead),
		socketInfo.currentPacketReceivedBytes);
	ResetReceiveState(socketInfo);
	if (shouldRequestSend)
	{
		RequestSend(clientIndex);
	}
}

void TCPServer::ProcessGameStartPacket(int clientIndex)
{
	mGameState = GAME_STATE::IN_GAME;
	mCanReplicateState = false;
	mZombieCount = 0;
	mBlueSuitCount = 0;
	for (int i = 0; i < MAX_CLIENT; ++i)
	{
		if (!mSocketInfos[i].isUsed)
		{
			continue;
		}

		if (i == 0)
		{
			++mZombieCount;
		}
		else
		{
			++mBlueSuitCount;
		}

		InitializePlayerPosition(mPlayers[i], i);
		mCollisionManager->AddCollisionPlayer(mPlayers[i], i);
		mSocketInfos[i].sendState = SOCKET_STATE::SEND_GAME_START;

		if (i != clientIndex)
		{
			RequestSend(i);
		}
	}
}

bool TCPServer::TryProcessChangeSlotPacket(SOCKET socket, int& clientIndex)
{
	if (!HandleReceiveResult(ReceiveData(clientIndex, sizeof(INT8)), socket))
	{
		return false;
	}

	INT8 selectedSlot = -1;
	memcpy(&selectedSlot, mSocketInfos[clientIndex].receiveBuffer.data(), sizeof(selectedSlot));
	// 슬롯 번호는 플레이어 및 소켓 배열의 인덱스로 사용되므로 범위를 벗어난 값은 무시할 수 없다.
	// 잘못된 연결을 종료해 이후 패킷이 비정상적인 서버 상태에 반영되는 것을 방지한다.
	if (selectedSlot < 0 || selectedSlot >= static_cast<INT8>(MAX_CLIENT))
	{
		std::cerr << "Invalid selected slot: client=" << clientIndex
			<< ", slot=" << static_cast<int>(selectedSlot) << '\n';
		DisconnectClient(socket);
		return false;
	}

	if (!mPlayers[selectedSlot])
	{
		mPlayers[selectedSlot] = make_shared<CServerBlueSuitPlayer>();
	}

	if (mPlayers[selectedSlot]->GetPlayerId() == -1)
	{
		mPlayers[selectedSlot]->SetPlayerId(selectedSlot);

		// 소켓과 수신 진행 상태는 하나의 단위이므로 전체를 함께 이동한다.
		mSocketInfos[selectedSlot] = std::move(mSocketInfos[clientIndex]);
		mSocketInfos[clientIndex] = SocketInfo{};

		mPlayerStates[selectedSlot].m_nClientId = selectedSlot;
		mPlayerStates[clientIndex].m_nClientId = -1;
		mPlayers[clientIndex]->SetPlayerId(-1);
	}
	else
	{
		// 역할 슬롯을 교환해도 각 소켓의 수신 상태는 해당 소켓과 함께 이동해야 한다.
		std::swap(mSocketInfos[clientIndex], mSocketInfos[selectedSlot]);
		mPlayerStates[selectedSlot].m_nClientId = selectedSlot;
	}

	mSocketInfos[selectedSlot].sendState = SOCKET_STATE::SEND_CHANGE_SLOT;
	// 이후 통계, 수신 상태 초기화, 응답 전송도 이동한 소켓 슬롯을 대상으로 해야 한다.
	clientIndex = selectedSlot;
	return true;
}

bool TCPServer::TryProcessClientInputPacket(
	SOCKET socket,
	int clientIndex,
	const std::shared_ptr<CServerPlayer>& player)
{
	// 소켓 슬롯과 플레이어 슬롯이 일치할 때만 입력을 해당 플레이어에게 적용한다.
	// 포인터가 없거나 ID가 다르면 서버 내부의 연결/플레이어 상태가 이미 불일치한 것이다.
	if (!player || player->GetPlayerId() != clientIndex)
	{
		const int playerId = player ? static_cast<int>(player->GetPlayerId()) : -1;
		std::cerr << "Invalid player for input packet: client=" << clientIndex
			<< ", playerId=" << playerId << '\n';
		DisconnectClient(socket);
		return false;
	}

	// KeysBuffer(WORD), viewMatrix, vecLook, vecRight, vecUp, pitch, rightClick
	if (!HandleReceiveResult(ReceiveData(clientIndex, CLIENT_INPUT_PAYLOAD_SIZE), socket))
	{
		return false;
	}

	// 수신 버퍼의 모든 필드를 지역 변수로 먼저 역직렬화한다.
	// 검증이 끝나기 전에는 일부 값만 플레이어 상태에 반영되는 일이 없어야 한다.
	size_t readOffset = 0;
	auto readValue = [&socketInfo = mSocketInfos[clientIndex], &readOffset](auto& value)
		{
			memcpy(&value, socketInfo.receiveBuffer.data() + readOffset, sizeof(value));
			readOffset += sizeof(value);
		};

	ClientInputData input;
	readValue(input.keyBuffer);
	readValue(input.viewMatrix);
	readValue(input.look);
	readValue(input.right);
	readValue(input.up);
	readValue(input.pitch);
	readValue(input.rightClick);
	assert(readOffset == CLIENT_INPUT_PAYLOAD_SIZE);

	if (!IsValidKeyBuffer(input.keyBuffer))
	{
		std::cerr << "Invalid key buffer: client=" << clientIndex
			<< ", value=" << input.keyBuffer << '\n';
		DisconnectClient(socket);
		return false;
	}
	if (!IsFiniteMatrix(input.viewMatrix) ||
		!IsFiniteVector(input.look) ||
		!IsFiniteVector(input.right) ||
		!IsFiniteVector(input.up) ||
		!std::isfinite(input.pitch))
	{
		std::cerr << "Non-finite transform in input packet: client=" << clientIndex << '\n';
		DisconnectClient(socket);
		return false;
	}
	if (input.rightClick > 1)
	{
		std::cerr << "Invalid right-click action: client=" << clientIndex
			<< ", value=" << static_cast<int>(input.rightClick) << '\n';
		DisconnectClient(socket);
		return false;
	}

	// 모든 역직렬화와 입력 검증을 통과한 뒤 서버의 플레이어 상태를 갱신한다.
	if (!player->IsRecvData())
	{
		player->SetRecvData(true);
	}
	player->SetKeyBuffer(input.keyBuffer);
	player->SetViewMatrix(input.viewMatrix);
	player->SetLook(input.look);
	player->SetRight(input.right);
	player->SetUp(input.up);
	mPlayerStates[clientIndex].m_fPitch = input.pitch;
	player->SetRightClick(input.rightClick != 0);
	return true;
}

bool TCPServer::ProcessLoadingCompletePacket(int clientIndex)
{
	if (mCanReplicateState)
	{
		return false;
	}

	mSocketInfos[clientIndex].isLoadingComplete = true;
	if (!AreAllClientsLoadingComplete())
	{
		return false;
	}

	mCanReplicateState = true;
	const auto currentTime = std::chrono::steady_clock::now();
	mNextStateReplicationTime = currentTime;
	mNextNearbyObjectReplicationTime = currentTime;
	mSocketInfos[clientIndex].sendState = SOCKET_STATE::SEND_LOADING_COMPLETE;
	return true;
}

bool TCPServer::AreAllClientsLoadingComplete() const
{
	bool hasActiveClient = false;
	for (const auto& socketInfo : mSocketInfos)
	{
		if (!socketInfo.isUsed)
		{
			continue;
		}

		hasActiveClient = true;
		if (!socketInfo.isLoadingComplete)
		{
			return false;
		}
	}
	return hasActiveClient;
}

void TCPServer::ProcessWriteEvent(SOCKET socket)
{
	const int clientIndex = FindClientIndex(socket);
	if (clientIndex < 0)
	{
		return;
	}

	if (FlushSendQueue(clientIndex) == SendResult::Error)
	{
		LogSocketError("send()", WSAGetLastError());
		DisconnectClient(socket);
	}
}

void TCPServer::RequestSend(int clientIndex)
{
	if (clientIndex < 0 || clientIndex >= static_cast<int>(mSocketInfos.size()) ||
		!mSocketInfos[clientIndex].isUsed)
	{
		return;
	}

	switch (mSocketInfos[clientIndex].sendState)
	{
	case SOCKET_STATE::SEND_GAME_START:
		if (SubmitSendData(clientIndex, static_cast<INT8>(SOCKET_STATE::SEND_GAME_START)))
		{
			mSocketInfos[clientIndex].sendState = SOCKET_STATE::SEND_PLAYER_STATE;
		}
		break;
	case SOCKET_STATE::SEND_CHANGE_SLOT:
		for (int i = 0; i < MAX_CLIENT; ++i)
		{
			if (!mSocketInfos[i].isUsed)
			{
				continue;
			}

			SubmitSendData(
				i,
				static_cast<INT8>(SOCKET_STATE::SEND_CHANGE_SLOT),
				mPlayerStates[i].m_nClientId,
				mPlayerStates);
		}
		if (mSocketInfos[clientIndex].isUsed)
		{
			mSocketInfos[clientIndex].sendState = SOCKET_STATE::SEND_PLAYER_STATE;
		}
		break;
	case SOCKET_STATE::SEND_ID:
		if (SubmitSendData(
			clientIndex,
			static_cast<INT8>(SOCKET_STATE::SEND_ID),
			mPlayerStates[clientIndex].m_nClientId,
			sClientCount,
			mPlayerStates))
		{
			mSocketInfos[clientIndex].sendState = SOCKET_STATE::SEND_PLAYER_STATE;
		}
		break;
	case SOCKET_STATE::SEND_PLAYER_STATE:
		if (mGameState == GAME_STATE::IN_LOBBY)
		{
			break;
		}
		SubmitSendData(
			clientIndex,
			static_cast<INT8>(SOCKET_STATE::SEND_PLAYER_STATE),
			mPlayerStates);
		break;
	case SOCKET_STATE::SEND_NUM_OF_CLIENT:
		if (SubmitSendData(
			clientIndex,
			static_cast<INT8>(SOCKET_STATE::SEND_NUM_OF_CLIENT),
			sClientCount,
			mPlayerStates))
		{
			mSocketInfos[clientIndex].sendState = SOCKET_STATE::SEND_PLAYER_STATE;
		}
		break;
	case SOCKET_STATE::SEND_BLUE_SUIT_WIN:
		if (SubmitSendData(clientIndex, static_cast<INT8>(SOCKET_STATE::SEND_BLUE_SUIT_WIN)))
		{
			mSocketInfos[clientIndex].sendState = SOCKET_STATE::SEND_PLAYER_STATE;
		}
		break;
	case SOCKET_STATE::SEND_ZOMBIE_WIN:
		if (SubmitSendData(clientIndex, static_cast<INT8>(SOCKET_STATE::SEND_ZOMBIE_WIN)))
		{
			mSocketInfos[clientIndex].sendState = SOCKET_STATE::SEND_PLAYER_STATE;
		}
		break;
	case SOCKET_STATE::SEND_OPEN_DRAWER_SOUND:
		if (SubmitSendData(clientIndex, static_cast<INT8>(SOCKET_STATE::SEND_OPEN_DRAWER_SOUND)))
		{
			mSocketInfos[clientIndex].sendState = SOCKET_STATE::SEND_PLAYER_STATE;
		}
		break;
	case SOCKET_STATE::SEND_CLOSE_DRAWER_SOUND:
		if (SubmitSendData(clientIndex, static_cast<INT8>(SOCKET_STATE::SEND_CLOSE_DRAWER_SOUND)))
		{
			mSocketInfos[clientIndex].sendState = SOCKET_STATE::SEND_PLAYER_STATE;
		}
		break;
	case SOCKET_STATE::SEND_OPEN_DOOR_SOUND:
		if (SubmitSendData(clientIndex, static_cast<INT8>(SOCKET_STATE::SEND_OPEN_DOOR_SOUND)))
		{
			mSocketInfos[clientIndex].sendState = SOCKET_STATE::SEND_PLAYER_STATE;
		}
		break;
	case SOCKET_STATE::SEND_CLOSE_DOOR_SOUND:
		if (SubmitSendData(clientIndex, static_cast<INT8>(SOCKET_STATE::SEND_CLOSE_DOOR_SOUND)))
		{
			mSocketInfos[clientIndex].sendState = SOCKET_STATE::SEND_PLAYER_STATE;
		}
		break;
	case SOCKET_STATE::SEND_BLUE_SUIT_DEAD:
	{
		const char deadUserId = static_cast<char>(clientIndex);
		for (int i = 0; i < MAX_CLIENT; ++i)
		{
			if (mSocketInfos[i].isUsed)
			{
				SubmitSendData(i, static_cast<INT8>(SOCKET_STATE::SEND_BLUE_SUIT_DEAD), deadUserId);
			}
		}
		if (mSocketInfos[clientIndex].isUsed)
		{
			mSocketInfos[clientIndex].sendState = SOCKET_STATE::SEND_PLAYER_STATE;
		}
		break;
	}
	case SOCKET_STATE::SEND_LOADING_COMPLETE:
	{
		for (int i = 0; i < MAX_CLIENT; ++i)
		{
			if (mSocketInfos[i].isUsed &&
				SubmitSendData(i, static_cast<INT8>(SOCKET_STATE::SEND_LOADING_COMPLETE)) &&
				mPlayers[i])
			{
				mPlayers[i]->GameStartLogic();
			}
		}
		BroadcastOpenableObjectSnapshot();
		if (mSocketInfos[clientIndex].isUsed)
		{
			mSocketInfos[clientIndex].sendState = SOCKET_STATE::SEND_PLAYER_STATE;
		}
		break;
	}
	default:
		break;
	}
}

void TCPServer::ProcessCloseEvent(SOCKET socket)
{
	DisconnectClient(socket);
}

bool TCPServer::Initialize(HWND hWnd)
{
	m_mt19937Gen = default_random_engine(random_device()());

	m_hWnd = hWnd;
	// 윈속 초기화
	WSADATA wsa;
	const int startupResult = WSAStartup(MAKEWORD(2, 2), &wsa);
	if (startupResult != 0)
	{
		// WSAStartup은 WSAGetLastError()가 아니라 반환값 자체가 오류 코드다.
		LogSocketError("WSAStartup", startupResult);
		return false;
	}

	// 소켓 생성
	const SOCKET listenSocket = socket(AF_INET, SOCK_STREAM, 0);
	if (listenSocket == INVALID_SOCKET)
	{
		LogSocketError("socket()", WSAGetLastError());
		WSACleanup();
		return false;
	}

	// bind()
	struct sockaddr_in serveraddr;
	memset(&serveraddr, 0, sizeof(serveraddr));
	serveraddr.sin_family = AF_INET;
	serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
	serveraddr.sin_port = htons(SERVERPORT);
	int retval = bind(listenSocket, reinterpret_cast<struct sockaddr*>(&serveraddr), sizeof(serveraddr));
	if (retval == SOCKET_ERROR)
	{
		const int errorCode = WSAGetLastError();
		LogSocketError("bind()", errorCode);
		closesocket(listenSocket);
		WSACleanup();
		return false;
	}

	// listen()
	retval = listen(listenSocket, SOMAXCONN);
	if (retval == SOCKET_ERROR)
	{
		const int errorCode = WSAGetLastError();
		LogSocketError("listen()", errorCode);
		closesocket(listenSocket);
		WSACleanup();
		return false;
	}

	// WSAAsyncSelect()
	retval = WSAAsyncSelect(listenSocket, hWnd, WM_SOCKET, FD_ACCEPT | FD_CLOSE);
	if (retval == SOCKET_ERROR)
	{
		const int errorCode = WSAGetLastError();
		LogSocketError("WSAAsyncSelect()", errorCode);
		closesocket(listenSocket);
		WSACleanup();
		return false;
	}

	mGameState = GAME_STATE::IN_LOBBY;

	mCollisionManager = make_shared<CServerCollisionManager>();
	mCollisionManager->CreateCollision(SPACE_FLOOR, SPACE_WIDTH, SPACE_DEPTH);

	// 씬 생성
	LoadScene();
	vector<int> vDoor;
	for (int i = 0; i < mCollisionManager->GetNumberOfCollisionObject(); ++i) {
		shared_ptr<CServerGameObject> object = mCollisionManager->GetCollisionObjectWithNumber(i);
		auto pElevaterDoor = dynamic_pointer_cast<CServerElevatorDoorObject>(object);

		if (pElevaterDoor) {
			if (strcmp(pElevaterDoor->m_pstrFrameName, "Door1")) {
				continue;
			}
			vDoor.push_back(i);
		}
	}
	int ELEVATORDOORCOUNT = vDoor.size();

	uniform_int_distribution<int> disInt(0, ELEVATORDOORCOUNT - 1);

	int random_escape_index = disInt(m_mt19937Gen);
	for (int i = 0; i < ELEVATORDOORCOUNT; ++i) {
		shared_ptr<CServerGameObject> object = mCollisionManager->GetCollisionObjectWithNumber(vDoor[i]);
		auto pElevaterDoor = dynamic_pointer_cast<CServerElevatorDoorObject>(object);
		if (!pElevaterDoor) {
			//std::cout << "엘리베이터 문이 아닙니다.!" << std::endl;
			assert(0); //반드시 CServerElevatorDoorObject 일것임. 아니면 시스템 종료 씬 오브젝트 정렬의 문제 발생
		}

		if (i == random_escape_index) {
			pElevaterDoor->SetEscapeDoor(true);
			for (int pi = 0; pi < MAX_CLIENT; ++pi) {
				mPlayerStates[pi].m_playerInfo.m_iEscapeDoor = vDoor[i];
			}
		}
		//pElevaterDoor->SetEscapeDoor(false); // 디버그를 위해서 모든 문을 잠금
	}

	//std::cout << "생성된 충돌객체 = " << mCollisionManager->GetNumberOfCollisionObject() << std::endl;
	// 아이템 생성
	CreateItemObject();
	//std::cout << "아이템 생성후 생성된 충돌객체 = " << mCollisionManager->GetNumberOfCollisionObject() << std::endl;


	return true;
}
void TCPServer::SimulationLoop()
{
	mTimer.Tick();
	ReportNetworkStatisticsIfDue();

	if (mGameState != GAME_STATE::IN_GAME)
	{
		return;
	}

	mGameState = DetermineEndGameState();
	if (mGameState != GAME_STATE::IN_GAME)
	{
		QueueEndGameNotifications(mGameState);
		return;
	}

	// 실제 시뮬레이션이 일어날곳
	float elapsedTime = mTimer.GetTimeElapsed();
	for (auto& player : mPlayers)
	{
		if (!player || player->GetPlayerId() == -1)
		{
			continue;
		}
		player->SetPickedObject(mCollisionManager);

		player->RightClickProcess(mCollisionManager);
		player->UseItem(mCollisionManager);
		player->Update(elapsedTime, mCollisionManager);
		player->UpdatePicking(player->GetPlayerId());

		mCollisionManager->Collide(elapsedTime, player);

		player->OnUpdateToParent();
		player->Declare(elapsedTime);
	}

	mCollisionManager->Update(elapsedTime);

	UpdatePlayerReplicationData();
	ProcessOutOfSpaceObjectReplication();
	UpdateNearbyObjectReplicationDataIfDue();
	ReplicateStateIfDue();
}

void TCPServer::ReplicateStateIfDue()
{
	if (!mCanReplicateState)
	{
		return;
	}

	const auto currentTime = std::chrono::steady_clock::now();
	if (currentTime < mNextStateReplicationTime)
	{
		return;
	}

	// 지연된 복제 횟수를 몰아서 보내지 않고 가장 최근 상태 하나만 전송한다.
	mNextStateReplicationTime = currentTime + STATE_REPLICATION_INTERVAL;
	for (int clientIndex = 0; clientIndex < static_cast<int>(mSocketInfos.size()); ++clientIndex)
	{
		const SocketInfo& socketInfo = mSocketInfos[clientIndex];
		if (!socketInfo.isUsed || socketInfo.sendState != SOCKET_STATE::SEND_PLAYER_STATE)
		{
			continue;
		}

		RequestSend(clientIndex);
	}
}

int TCPServer::DetermineEndGameState()
{
	int endGameState = GAME_STATE::IN_GAME;

	if (mZombieCount == 1 && mBlueSuitCount > 0)
	{
		int nAliveBlueSuit = 0;
		for (int i = 1; i < MAX_CLIENT; ++i)
		{
			if (!mPlayers[i] || mPlayers[i]->GetPlayerId() == -1)
			{
				continue;
			}

			if (mPlayers[i]->IsAlive())
			{
				++nAliveBlueSuit;
			}
		}

		if (nAliveBlueSuit == 0)
		{
			endGameState = GAME_STATE::ZOMBIE_WIN;
			return endGameState;
		}
	}

	for (const auto& player : mPlayers)
	{
		if (!player || player->GetPlayerId() == -1)
		{
			continue;
		}

		if (player->IsWinner())
		{
			if (dynamic_pointer_cast<CServerBlueSuitPlayer>(player))
			{
				endGameState = GAME_STATE::BLUE_SUIT_WIN;
			}
			break;
		}
	}

	return endGameState;
}

void TCPServer::QueueEndGameNotifications(int endGameState)
{
	for (int clientIndex = 0; clientIndex < static_cast<int>(mSocketInfos.size()); ++clientIndex)
	{
		SocketInfo& socketInfo = mSocketInfos[clientIndex];
		if (!socketInfo.isUsed)
		{
			continue;
		}

		if (endGameState == GAME_STATE::BLUE_SUIT_WIN) // BLUE SUIT WIN
		{
			socketInfo.sendState = SOCKET_STATE::SEND_BLUE_SUIT_WIN;
		}
		else // ZOMBIE WIN
		{
			socketInfo.sendState = SOCKET_STATE::SEND_ZOMBIE_WIN;
		}

		// 정기 상태 복제가 종료된 뒤에도 승리 결과는 상태 전환 시 즉시 한 번 전송한다.
		RequestSend(clientIndex);
	}
}

void TCPServer::BroadcastOpenableObjectState(
	int objectId,
	OpenableObjectType objectType,
	bool opened)
{
	SC_OPENABLE_OBJECT_STATE objectState = {};
	objectState.objectId = objectId;
	objectState.objectType = objectType;
	objectState.opened = opened ? 1 : 0;
	if (objectState.objectId < 0 || !objectState.IsValidOpenableObjectType())
	{
		return;
	}

	for (int clientIndex = 0; clientIndex < static_cast<int>(mSocketInfos.size()); ++clientIndex)
	{
		if (!mSocketInfos[clientIndex].isUsed)
		{
			continue;
		}

		SubmitSendData(
			clientIndex,
			static_cast<INT8>(SOCKET_STATE::SEND_OPENABLE_OBJECT_STATE),
			objectState);
	}
}

void TCPServer::BroadcastOpenableObjectSnapshot()
{
	const int collisionObjectCount = CServerCollisionManager::GetNumberOfCollisionObject();
	std::vector<SC_OPENABLE_OBJECT_STATE> objectStates;
	objectStates.reserve(collisionObjectCount);

	for (int objectId = 0; objectId < collisionObjectCount; ++objectId)
	{
		const shared_ptr<CServerGameObject> gameObject =
			mCollisionManager->GetCollisionObjectWithNumber(objectId);

		SC_OPENABLE_OBJECT_STATE objectState = {};
		objectState.objectId = objectId;
		if (const shared_ptr<CServerDoorObject> door =
			dynamic_pointer_cast<CServerDoorObject>(gameObject))
		{
			objectState.objectType = OpenableObjectType::Door;
			objectState.opened = door->IsOpen() ? 1 : 0;
		}
		else if (const shared_ptr<CServerDrawerObject> drawer =
			dynamic_pointer_cast<CServerDrawerObject>(gameObject))
		{
			objectState.objectType = OpenableObjectType::Drawer;
			objectState.opened = drawer->IsOpen() ? 1 : 0;
		}
		else
		{
			continue;
		}

		objectStates.push_back(objectState);
	}

	const size_t payloadBytes = sizeof(SC_OPENABLE_OBJECT_STATE) * objectStates.size();
	if (payloadBytes > MAX_PACKET_PAYLOAD_SIZE)
	{
		return;
	}

	const std::uint16_t wirePayloadBytes = static_cast<std::uint16_t>(payloadBytes);
	std::vector<char> packetBuffer;
	packetBuffer.reserve(sizeof(INT8) + sizeof(wirePayloadBytes) + payloadBytes);
	packetBuffer.push_back(static_cast<INT8>(SOCKET_STATE::SEND_OPENABLE_OBJECT_SNAPSHOT));
	AppendBufferData(packetBuffer, &wirePayloadBytes, sizeof(wirePayloadBytes));
	if (!objectStates.empty())
	{
		AppendBufferData(packetBuffer, objectStates.data(), payloadBytes);
	}

	for (int clientIndex = 0; clientIndex < static_cast<int>(mSocketInfos.size()); ++clientIndex)
	{
		if (mSocketInfos[clientIndex].isUsed)
		{
			EnqueueSendBuffer(clientIndex, packetBuffer);
		}
	}
}

// 소켓 정보 추가
INT8 TCPServer::RegisterClientSocket(
	SOCKET clientSocket,
	struct sockaddr_in clientAddress,
	int clientAddressLength)
{
	INT8 clientIndex = -1;
	if (sClientCount >= MAX_CLIENT)
	{
		return clientIndex;
	}
	SocketInfo socketInfo;

	socketInfo.isUsed = true;
	socketInfo.socket = clientSocket;
	socketInfo.clientAddress = clientAddress;
	socketInfo.clientAddressLength = clientAddressLength;

	getpeername(socketInfo.socket, (struct sockaddr*)&socketInfo.clientAddress, &socketInfo.clientAddressLength);
	inet_ntop(AF_INET, &socketInfo.clientAddress.sin_addr, socketInfo.ipAddress, sizeof(socketInfo.ipAddress));

	socketInfo.sendState = SOCKET_STATE::SEND_ID;

	// 배열에 정보 추가 
	for (int i = 0; i < sClientCount + 1; ++i)
	{
		if (mSocketInfos[i].isUsed)
		{
			continue;
		}
		sClientCount++;

		// 클라이언트 정보 초기화
		mPlayerStates[i].m_nClientId = i;
		mSocketInfos[i] = std::move(socketInfo);
		clientIndex = i;
		break;
	}

	return clientIndex;
}

// 소켓 정보 얻기
INT8 TCPServer::FindClientIndex(SOCKET clientSocket) const
{
	for (size_t index = 0; index < mSocketInfos.size(); ++index)
	{
		const SocketInfo& socketInfo = mSocketInfos[index];
		if (!socketInfo.isUsed)
		{
			continue;
		}
		if (socketInfo.socket == clientSocket)
		{
			return static_cast<INT8>(index);
		}
	}
	return -1;
}

bool TCPServer::IsValidReceiveHead(INT8 head) const
{
	// 서버가 payload 크기와 처리 방법을 알고 있는 클라이언트 패킷만 허용한다.
	switch (static_cast<ReceiveHead>(head))
	{
	case ReceiveHead::KeysBuffer:
	case ReceiveHead::GameStart:
	case ReceiveHead::ChangeSlot:
	case ReceiveHead::LoadingComplete:
		return true;
	default:
		return false;
	}
}

bool TCPServer::DisconnectClient(SOCKET clientSocket)
{
	const INT8 clientIndex = FindClientIndex(clientSocket);
	if (clientIndex < 0)
	{
		return false;
	}

	// SocketInfo가 초기화되기 전에 이 연결의 누적 측정값을 남긴다.
	mNetworkStatisticsReporter.ReportDisconnected(
		clientIndex,
		mSocketInfos[clientIndex].networkStatistics);

	INT8 nListBoxIndex = -1;
	for (INT8 i = 0; i <= clientIndex; ++i)
	{
		if (mSocketInfos[i].isUsed)
		{
			++nListBoxIndex;
		}
	}

	const bool hadPlayer = (mPlayers[clientIndex] != nullptr);
	if (hadPlayer)
	{
		SendMessage(mClientListBox, LB_DELETESTRING, static_cast<WPARAM>(nListBoxIndex), 0);
		if (clientIndex == ZOMBIEPLAYER)
		{
			mZombieCount = max(0, mZombieCount - 1);
		}
		else
		{
			mBlueSuitCount = max(0, mBlueSuitCount - 1);
		}
	}

	WSAAsyncSelect(clientSocket, m_hWnd, 0, 0);
	shutdown(clientSocket, SD_BOTH);
	closesocket(clientSocket);

	mPlayers[clientIndex].reset();
	mPlayerStartPositionIndices[clientIndex] = -1;
	mPlayerStates[clientIndex] = SC_PLAYER_STATE{};
	mSocketInfos[clientIndex] = SocketInfo{};
	sClientCount = max<INT8>(0, sClientCount - 1);

	if (hadPlayer)
	{
		for (auto& otherSocketInfo : mSocketInfos)
		{
			if (!otherSocketInfo.isUsed)
			{
				continue;
			}

			otherSocketInfo.sendState = SOCKET_STATE::SEND_NUM_OF_CLIENT;
			RequestSend(FindClientIndex(otherSocketInfo.socket));
		}
	}

	// 로딩 중이던 마지막 미완료 클라이언트가 나가면 남은 클라이언트의 게임을 시작한다.
	if (mGameState == GAME_STATE::IN_GAME &&
		!mCanReplicateState &&
		AreAllClientsLoadingComplete())
	{
		for (int index = 0; index < static_cast<int>(mSocketInfos.size()); ++index)
		{
			if (!mSocketInfos[index].isUsed)
			{
				continue;
			}

			mCanReplicateState = true;
			const auto currentTime = std::chrono::steady_clock::now();
			mNextStateReplicationTime = currentTime;
			mNextNearbyObjectReplicationTime = currentTime;
			mSocketInfos[index].sendState = SOCKET_STATE::SEND_LOADING_COMPLETE;
			RequestSend(index);
			break;
		}
	}

	const bool shouldCloseServer = mGameState != GAME_STATE::IN_LOBBY && sClientCount == 0;
	if (shouldCloseServer)
	{
		// 소켓 이벤트 처리 중 창을 직접 파괴하지 않고 메시지 루프에서 정상 종료한다.
		LogServerNotice("Closing because the last client disconnected after the game started.");
		PostMessage(m_hWnd, WM_CLOSE, 0, 0);
	}

	return true;
}

void TCPServer::UpdatePlayerReplicationData()
{
	for (const auto& player : mPlayers)
	{
		if (!player || player->GetPlayerId() == -1)
		{
			continue;
		}

		const INT8 playerId = player->GetPlayerId();
		auto& playerState = mPlayerStates[playerId];
		playerState.m_bAlive = player->IsAlive();
		playerState.m_bRunning = player->IsRunning();
		playerState.m_xmf3Position = player->GetPosition();
		playerState.m_xmf3Velocity = player->GetVelocity();
		playerState.m_xmf3Look = player->GetLook();

		const auto pickedObject = player->GetPickedObject().lock();
		playerState.m_nPickedObjectNum = pickedObject
			? pickedObject->GetCollisionNum()
			: -1;

		if (playerId == ZOMBIEPLAYER)
		{
			shared_ptr<CServerZombiePlayer> zombiePlayer = dynamic_pointer_cast<CServerZombiePlayer>(player);
			if (zombiePlayer)
			{
				playerState.m_nSlotObjectNum[0] = zombiePlayer->IsTracking() ? 1 : -1;		// 추적
				playerState.m_nSlotObjectNum[1] = zombiePlayer->IsInterruption() ? 1 : -1;	// 시야방해
				playerState.m_nSlotObjectNum[2] = zombiePlayer->IsAttack() ? 1 : -1;			// 공격

				// 짧은 지뢰 충돌 상태를 복제한 뒤 서버 상태를 원래 값으로 되돌린다.
				if (zombiePlayer->GetCollideMineRef() == -1)
				{
					zombiePlayer->SetExplosionDelay(0.0f);
				}
				else if (zombiePlayer->GetExplosionDelay() > 0.05f)
				{
					zombiePlayer->SetCollideMineRef(-1);
				}
				playerState.m_playerInfo.m_iMineobjectNum = zombiePlayer->GetCollideMineRef();
			}
		}
		else
		{
			shared_ptr<CServerBlueSuitPlayer> blueSuitPlayer = dynamic_pointer_cast<CServerBlueSuitPlayer>(player);
			if (blueSuitPlayer)
			{
				for (int i = 0; i < 3; ++i)
				{
					playerState.m_nSlotObjectNum[i] = blueSuitPlayer->GetReferenceSlotItemNum(i);
					playerState.m_nFuseObjectNum[i] = blueSuitPlayer->GetReferenceFuseItemNum(i);
				}
				playerState.m_playerInfo.m_bAttacked = blueSuitPlayer->IsAttacked();
				playerState.m_playerInfo.m_selectItem = blueSuitPlayer->GetRightItem();
				playerState.m_playerInfo.m_bTeleportItemUse = blueSuitPlayer->IsTeleportUse();
			}
		}
	}
}

void TCPServer::LoadScene()
{
	FILE* pInFile = NULL;
	::fopen_s(&pInFile, (char*)"ServerScene.bin", "rb");
	::rewind(pInFile);
	int fileEnd{};
	int nReads;
	while (true)
	{
		char pstrToken[128] = { '\0' };
		for (; ; )
		{
			if (::ReadStringFromFile(pInFile, pstrToken))
			{
				if (!strcmp(pstrToken, "<Hierarchy>:"))
				{
					char pStrFrameName[64];
					int nChild, nBoxCollider;
					XMFLOAT3 xmf3AABBCenter, xmf3AABBExtents;
					std::vector<BoundingOrientedBox> voobb;
					for (;;)
					{
						if (::ReadStringFromFile(pInFile, pstrToken))
						{
							if (!strcmp(pstrToken, "<Frame>:"))
							{
								::ReadIntegerFromFile(pInFile);
								::ReadStringFromFile(pInFile, pStrFrameName);
								//std::cout << pStrFrameName << endl;
							}
							else if (!strcmp(pstrToken, "<Children>:"))
							{
								nChild = ::ReadIntegerFromFile(pInFile);
							}
							else if (!strcmp(pstrToken, "<BoxColliders>:"))
							{
								nBoxCollider = ::ReadIntegerFromFile(pInFile);
								voobb.reserve(nBoxCollider);
								for (int i = 0; i < nBoxCollider; ++i)
								{
									::ReadStringFromFile(pInFile, pstrToken);	// <Bound>
									int nIndex = 0;
									nReads = fread(&nIndex, sizeof(int), 1, pInFile);
									nReads = (UINT)::fread(&xmf3AABBCenter, sizeof(XMFLOAT3), 1, pInFile);
									nReads = (UINT)::fread(&xmf3AABBExtents, sizeof(XMFLOAT3), 1, pInFile);
									XMFLOAT4 xmf4Orientation;
									XMStoreFloat4(&xmf4Orientation, XMQuaternionIdentity());
									voobb.emplace_back(xmf3AABBCenter, xmf3AABBExtents, xmf4Orientation);
								}
							}
							else if (!strcmp(pstrToken, "<Matrix>:"))
							{
								nChild = ::ReadIntegerFromFile(pInFile);
								XMFLOAT4X4* xmf4x4World = new XMFLOAT4X4[nChild];
								nReads = (UINT)::fread(xmf4x4World, sizeof(XMFLOAT4X4), nChild, pInFile);
								for (int i = 0; i < nChild; ++i)
								{
									// 오브젝트 생성
									CreateSceneObject(pStrFrameName, Matrix4x4::Transpose(xmf4x4World[i]), voobb);
								}
								delete[] xmf4x4World;
							}
							else if (!strcmp(pstrToken, "</Frame>"))
							{
								break;
							}
						}
					}
				}
				else if (!strcmp(pstrToken, "</Hierarchy>"))
				{
					break;
				}
				else if (!strcmp(pstrToken, "</Scene>:"))
				{
					fileEnd = 1;
					break;
				}
			}
			else
			{
				break;
			}
		}
		if (fileEnd)
		{
			break;
		}
	}


}

void TCPServer::CreateSceneObject(char* pstrFrameName, const XMFLOAT4X4& xmf4x4World, const vector<BoundingOrientedBox>& voobb)
{
	static int nServerObjectNum = 0;
	shared_ptr<CServerGameObject> pGameObject;

	if (!strcmp(pstrFrameName, "Door_1"))
	{
		pGameObject = make_shared<CServerDoorObject>(pstrFrameName, xmf4x4World, voobb);
	}
	else if (!strcmp(pstrFrameName, "Drawer_1"))
	{
		/*if (m_nStartDrawer1 == -1)
		{
			m_nStartDrawer1 = nServerObjectNum;
			m_nEndDrawer1 = nServerObjectNum - 1;
		}
		m_nEndDrawer1++;*/
		mDrawerIds.push_back(pair<int, int>(nServerObjectNum, 1));
		pGameObject = make_shared<CServerDrawerObject>(pstrFrameName, xmf4x4World, voobb);
	}
	else if (!strcmp(pstrFrameName, "Drawer_2"))
	{
		/*if (m_nStartDrawer2 == -1)
		{
			m_nStartDrawer2 = nServerObjectNum;
			m_nEndDrawer2 = nServerObjectNum - 1;
		}
		m_nEndDrawer2++;*/
		mDrawerIds.push_back(pair<int, int>(nServerObjectNum, 2));
		pGameObject = make_shared<CServerDrawerObject>(pstrFrameName, xmf4x4World, voobb);
	}
	else if (!strcmp(pstrFrameName, "Door1"))
	{
		pGameObject = make_shared<CServerElevatorDoorObject>(pstrFrameName, xmf4x4World, voobb);
	}
	else if (!strcmp(pstrFrameName, "Emergency_Handle"))
	{
		pGameObject = make_shared<CServerElevatorDoorObject>(pstrFrameName, xmf4x4World, voobb);
	}
	else if (!strcmp(pstrFrameName, "Laboratory_Wall_1_Corner_1") || !strcmp(pstrFrameName, "Laboratory_Wall_1_Corner_2"))
	{
		pGameObject = make_shared<CServerEnvironmentObject>(pstrFrameName, xmf4x4World, voobb);
	}
	else if (!strcmp(pstrFrameName, "BoxCollide_Wall"))
	{
		pGameObject = make_shared<CServerEnvironmentObject>(pstrFrameName, xmf4x4World, voobb);
	}
	else if (!strcmp(pstrFrameName, "Laboratory_Wall_1_Corner") || !strcmp(pstrFrameName, "Laboratory_Wall_1_Corner2") || !strcmp(pstrFrameName, "Laboratory_Wall_1"))
	{
		pGameObject = make_shared<CServerEnvironmentObject>(pstrFrameName, xmf4x4World, voobb);
	}
	else if (!strcmp(pstrFrameName, "Laboratory_Wall_Door_1") || !strcmp(pstrFrameName, "Laboratory_Wall_Door_1_2"))
	{
		pGameObject = make_shared<CServerEnvironmentObject>(pstrFrameName, xmf4x4World, voobb);
	}
	else if (!strcmp(pstrFrameName, "Biological_Capsule_1") || !strcmp(pstrFrameName, "Laboratory_Table_1") || !strcmp(pstrFrameName, "Laboratory_Stool_1"))
	{
		pGameObject = make_shared<CServerEnvironmentObject>(pstrFrameName, xmf4x4World, voobb);
	}
	else if (!strcmp(pstrFrameName, "Laboratory_Tunnel_1_Stairs") || !strcmp(pstrFrameName, "Laboratory_Tunnel_1") || !strcmp(pstrFrameName, "Laboratory_Desk_Drawers_1"))
	{
		pGameObject = make_shared<CServerEnvironmentObject>(pstrFrameName, xmf4x4World, voobb);
	}
	else if (!strcmp(pstrFrameName, "SM_Prop_Vents_Straight_01") || !strcmp(pstrFrameName, "SM_Prop_Crate_01")
		|| !strcmp(pstrFrameName, "SM_Prop_Pipe_Curve_02") || !strcmp(pstrFrameName, "SM_Prop_Billboard_Roof_01")
		|| !strcmp(pstrFrameName, "SM_Prop_Roof_Aircon_03") || !strcmp(pstrFrameName, "SM_Prop_Vents_End_01")
		|| !strcmp(pstrFrameName, "SM_Prop_ShopInterior_Table_01") || !strcmp(pstrFrameName, "SM_Prop_Couch_01")
		|| !strcmp(pstrFrameName, "SM_Prop_PotPlant_02") || !strcmp(pstrFrameName, "Table1of10"))
	{
		pGameObject = make_shared<CServerEnvironmentObject>(pstrFrameName, xmf4x4World, voobb);
	}
	else if (!strcmp(pstrFrameName, "BoxCollider_Stair_Start"))
	{
		pGameObject = make_shared<CServerStairTriggerObject>(pstrFrameName, xmf4x4World, voobb);
	}
	else
	{
		pGameObject = make_shared<CServerGameObject>(pstrFrameName, xmf4x4World, voobb);
		pGameObject->SetStatic(true);
	}

	strcpy(pGameObject->m_pstrFrameName, pstrFrameName);

	nServerObjectNum++;
	mCollisionManager->AddCollisionObject(pGameObject);
}

void TCPServer::CreateItemObject()
{
	//CServerItemObject::SetDrawerStartEnd(m_nStartDrawer1, m_nEndDrawer1, m_nStartDrawer2, m_nEndDrawer2);
	// 확률: fus 30, mine 30, tp 30, radar 10
	uniform_int_distribution<int> dis(0, mDrawerIds.size() - 1); //[CJI 0525] mDrawerIds 에 번호를 저장하는 방식으로 변경하여 랜덤으로 뽑아 사용
	uniform_int_distribution<int> item_dis(0, 99);
	uniform_int_distribution<int> rotation_dis(1, 360);
	uniform_real_distribution<float> pos_dis(-0.2f, 0.2f);
	CServerItemObject::SetDrawerIdContainer(mDrawerIds);

	for (int i = 0; i < ITEM_COUNT; ++i)
	{
		int rd_Num = dis(m_mt19937Gen);
		int nDrawerNum = mDrawerIds[rd_Num].first;
		shared_ptr<CServerDrawerObject> pDrawerObject = dynamic_pointer_cast<CServerDrawerObject>(mCollisionManager->GetCollisionObjectWithNumber(nDrawerNum));
		if (!pDrawerObject) //error
			assert(0);
		//exit(1);

		if (pDrawerObject->m_pStoredItem)	// 이미 다른 아이템이 들어왔음
		{
			--i;
			continue;
		}
		XMFLOAT4X4 xmf4x4World = mCollisionManager->GetCollisionObjectWithNumber(nDrawerNum)->GetWorldMatrix();

		int nCreateItem = item_dis(m_mt19937Gen);
		shared_ptr<CServerItemObject> pItemObject;

		XMFLOAT3 xmf3RandOffset = XMFLOAT3(pos_dis(m_mt19937Gen), 0.0f, pos_dis(m_mt19937Gen));
		XMFLOAT3 xmf3RandRotation = XMFLOAT3(0.0f, 0.0f, (float)rotation_dis(m_mt19937Gen));

		if (i < 9)		// Fuse
		{
			pItemObject = make_shared<CServerFuseObject>();
			pItemObject->SetDrawerNumber(nDrawerNum);
			pItemObject->SetDrawer(pDrawerObject);
			pItemObject->SetDrawerType(mDrawerIds[rd_Num].second);
			pDrawerObject->m_pStoredItem = pItemObject;

			pItemObject->SetRandomRotation(xmf3RandRotation);
			pItemObject->SetRandomOffset(xmf3RandOffset);

			pItemObject->SetWorldMatrix(xmf4x4World);
			mCollisionManager->AddCollisionObject(pItemObject);
		}
		else if (i < 24)	// tp
		{
			pItemObject = make_shared<CServerTeleportObject>();
			pItemObject->SetDrawerNumber(nDrawerNum);
			pItemObject->SetDrawer(pDrawerObject);
			pItemObject->SetDrawerType(mDrawerIds[rd_Num].second);
			pDrawerObject->m_pStoredItem = pItemObject;

			pItemObject->SetRandomRotation(xmf3RandRotation);
			pItemObject->SetRandomOffset(xmf3RandOffset);
			pItemObject->SetWorldMatrix(xmf4x4World);
			mCollisionManager->AddCollisionObject(pItemObject);
		}
		else if (i < 26)	// Rader
		{
			xmf3RandRotation = XMFLOAT3(90.0f, 0.0f, 0.0f);
			pItemObject = make_shared<CServerRadarObject>();
			pItemObject->SetDrawerNumber(nDrawerNum);
			pItemObject->SetDrawer(pDrawerObject);
			pItemObject->SetDrawerType(mDrawerIds[rd_Num].second);
			pDrawerObject->m_pStoredItem = pItemObject;

			pItemObject->SetRandomRotation(xmf3RandRotation);
			pItemObject->SetRandomOffset(xmf3RandOffset);
			pItemObject->SetWorldMatrix(xmf4x4World);
			mCollisionManager->AddCollisionObject(pItemObject);
		}
		else if (i < 76)	// Mine
		{
			xmf3RandRotation = XMFLOAT3(90.0f, 0.0f, 0.0f);

			pItemObject = make_shared<CServerMineObject>();
			pItemObject->SetDrawerNumber(nDrawerNum);
			pItemObject->SetDrawer(pDrawerObject);
			pItemObject->SetDrawerType(mDrawerIds[rd_Num].second);
			pDrawerObject->m_pStoredItem = pItemObject;

			pItemObject->SetRandomRotation(xmf3RandRotation);
			pItemObject->SetRandomOffset(xmf3RandOffset);
			pItemObject->SetWorldMatrix(xmf4x4World);
			mCollisionManager->AddCollisionObject(pItemObject);
		}

	}
}

void TCPServer::ProcessOutOfSpaceObjectReplication()
{
	// 시뮬레이션에서 발생한 공간 이동은 정기 snapshot을 기다리지 않고 즉시 큐에 추가한다.
	const std::vector<SC_SPACEOUT_OBJECT> outOfSpaceObjects = CollectOutOfSpaceObjects();
	EnqueueOutOfSpaceObjectPackets(outOfSpaceObjects);
}

void TCPServer::UpdateNearbyObjectReplicationDataIfDue()
{
	if (!mCanReplicateState)
	{
		return;
	}

	const auto currentTime = std::chrono::steady_clock::now();
	if (currentTime < mNextNearbyObjectReplicationTime)
	{
		return;
	}

	// 지연된 조사를 몰아서 실행하지 않고 가장 최근 상태만 갱신한다.
	mNextNearbyObjectReplicationTime = currentTime + NEARBY_OBJECT_REPLICATION_INTERVAL;
	UpdateNearbyObjectReplicationData();
	ReplicateNearbyObjectData();
}

std::vector<SC_SPACEOUT_OBJECT> TCPServer::CollectOutOfSpaceObjects()
{
	std::vector<SC_SPACEOUT_OBJECT> objectUpdates;
	auto& outOfSpaceObjects = mCollisionManager->GetOutSpaceObject();
	objectUpdates.reserve(outOfSpaceObjects.size());

	// 충돌 관리자가 이번 시뮬레이션에서 별도 전송이 필요하다고 표시한 오브젝트를
	// 네트워크 전용 구조체로 복사한다. null 항목은 패킷에 포함하지 않는다.
	for (const auto& gameObject : outOfSpaceObjects)
	{
		if (!gameObject)
		{
			continue;
		}
		objectUpdates.emplace_back(SC_SPACEOUT_OBJECT(
			gameObject->GetCollisionNum(),
			gameObject->GetWorldMatrix()));
	}

	// 이 목록은 프레임 간 누적 목록이 아니다. 복사한 뒤 비워 다음 시뮬레이션의 변경만 수집한다.
	outOfSpaceObjects.clear();
	return objectUpdates;
}

void TCPServer::EnqueueOutOfSpaceObjectPackets(const std::vector<SC_SPACEOUT_OBJECT>& objectUpdates)
{
	if (objectUpdates.empty())
	{
		return;
	}

	// payload 크기 필드가 uint16_t이므로 한 패킷이 표현 가능한 최대 개수로 나눈다.
	constexpr size_t maxObjectsPerPacket =
		MAX_PACKET_PAYLOAD_SIZE / sizeof(SC_SPACEOUT_OBJECT);
	static_assert(maxObjectsPerPacket > 0);

	for (size_t firstObjectIndex = 0;
		firstObjectIndex < objectUpdates.size();
		firstObjectIndex += maxObjectsPerPacket)
	{
		const size_t objectCount = (std::min)(
			maxObjectsPerPacket,
			objectUpdates.size() - firstObjectIndex);
		const size_t payloadBytes = sizeof(SC_SPACEOUT_OBJECT) * objectCount;
		const std::uint16_t wirePayloadBytes = static_cast<std::uint16_t>(payloadBytes);

		std::vector<char> packetBuffer;
		packetBuffer.reserve(sizeof(INT8) + sizeof(wirePayloadBytes) + payloadBytes);
		packetBuffer.push_back(static_cast<INT8>(SOCKET_STATE::SEND_SPACEOUT_OBJECTS));
		AppendBufferData(packetBuffer, &wirePayloadBytes, sizeof(wirePayloadBytes));
		AppendBufferData(packetBuffer, objectUpdates.data() + firstObjectIndex, payloadBytes);

		for (const auto& player : mPlayers)
		{
			if (!player)
			{
				continue;
			}
			const INT8 playerId = player->GetPlayerId();
			if (playerId == -1)
			{
				continue;
			}

			// 소켓마다 partial send 위치가 다르므로 각 송신 큐가 버퍼 복사본을 소유한다.
			EnqueueSendBuffer(playerId, packetBuffer);
		}
	}
}

void TCPServer::UpdateNearbyObjectReplicationData()
{
	constexpr size_t maxNearbyObjects = MAX_NEARBY_OBJECTS;
	// 수신 플레이어가 속한 셀을 중심으로 3x3 셀만 탐색하고 최대 30개까지 기록한다.
	for (const auto& player : mPlayers)
	{
		if (!player || player->GetPlayerId() == -1)
		{
			continue;
		}

		const INT8 playerId = player->GetPlayerId();
		auto& objectSnapshot = mNearbyObjectSnapshots[playerId];
		objectSnapshot.clear();

		// 현재는 플레이어와 같은 층만 검사한다. 계단 주변의 인접 층 검사는 별도 게임 규칙이다.
		for (int widthIndex = player->GetWidth() - 1;
			widthIndex <= player->GetWidth() + 1 && objectSnapshot.size() < maxNearbyObjects;
			++widthIndex)
		{
			if (widthIndex < 0 || widthIndex >= mCollisionManager->GetWidth())
			{
				continue;
			}

			for (int depthIndex = player->GetDepth() - 1;
				depthIndex <= player->GetDepth() + 1 && objectSnapshot.size() < maxNearbyObjects;
				++depthIndex)
			{
				if (depthIndex < 0 || depthIndex >= mCollisionManager->GetDepth())
				{
					continue;
				}

				for (const auto& gameObject : mCollisionManager->GetSpaceGameObjects(
					player->GetFloor(),
					widthIndex,
					depthIndex))
				{
					if (!gameObject || !gameObject->ShouldReplicateNearbyTransform())
					{
						continue;
					}

					const int objectId = gameObject->GetCollisionNum();
					const bool alreadyIncluded = std::any_of(
						objectSnapshot.begin(),
						objectSnapshot.end(),
						[objectId](const SC_NEARBY_OBJECT& objectInfo)
						{
							return objectInfo.m_nObjectId == objectId;
						});
					if (alreadyIncluded)
					{
						continue;
					}

					objectSnapshot.push_back(SC_NEARBY_OBJECT{ objectId, gameObject->GetWorldMatrix() });
					if (objectSnapshot.size() == maxNearbyObjects)
					{
						break;
					}
				}
			}
		}
	}
}

void TCPServer::ReplicateNearbyObjectData()
{
	for (int clientIndex = 0; clientIndex < static_cast<int>(mSocketInfos.size()); ++clientIndex)
	{
		if (!mSocketInfos[clientIndex].isUsed)
		{
			continue;
		}

		const auto& objectSnapshot = mNearbyObjectSnapshots[clientIndex];
		const size_t payloadBytes = sizeof(SC_NEARBY_OBJECT) * objectSnapshot.size();
		const std::uint16_t wirePayloadBytes = static_cast<std::uint16_t>(payloadBytes);

		std::vector<char> packetBuffer;
		packetBuffer.reserve(sizeof(INT8) + sizeof(wirePayloadBytes) + payloadBytes);
		packetBuffer.push_back(static_cast<INT8>(SOCKET_STATE::SEND_NEARBY_OBJECTS));
		AppendBufferData(packetBuffer, &wirePayloadBytes, sizeof(wirePayloadBytes));
		if (!objectSnapshot.empty())
		{
			AppendBufferData(packetBuffer, objectSnapshot.data(), payloadBytes);
		}
		EnqueueSendBuffer(clientIndex, std::move(packetBuffer));
	}
}

void TCPServer::InitializePlayerPosition(shared_ptr<CServerPlayer>& serverPlayer, int index)
{
	// 후보지를 두고 int 값에 따라 그곳에 가도록 해야할듯
	uniform_int_distribution<int> disIntPosition(0, mPlayerStartPositions.size() - 1);

	int nStartPosNum = disIntPosition(m_mt19937Gen);
	bool bEmpty = false;
	while (!bEmpty)
	{
		bEmpty = true;
		nStartPosNum = disIntPosition(m_mt19937Gen);
		for (const auto& nPlayerStartPos : mPlayerStartPositionIndices)
		{
			if (nPlayerStartPos == nStartPosNum)
			{
				bEmpty = false;
				break;
			}
		}
	}

	mPlayerStartPositionIndices[index] = nStartPosNum;
	XMFLOAT3 xmf3Position = mPlayerStartPositions[nStartPosNum];
	serverPlayer->SetPlayerPosition(xmf3Position);
	serverPlayer->SetPlayerOldPosition(xmf3Position);
}

template<class... Args>
bool TCPServer::SubmitSendData(int clientIndex, Args&&... args)
{
	const size_t bufferSize = (sizeof(args) + ... + 0);
	std::vector<char> buffer(bufferSize);
	size_t offset = 0;
	((memcpy(buffer.data() + offset, &args, sizeof(args)), offset += sizeof(args)), ...);

	return EnqueueSendBuffer(clientIndex, std::move(buffer));
}

bool TCPServer::EnqueueSendBuffer(int clientIndex, vector<char> buffer)
{
	if (clientIndex < 0 || clientIndex >= static_cast<int>(mSocketInfos.size()))
	{
		return false;
	}

	SocketInfo& socketInfo = mSocketInfos[clientIndex];
	const bool isInvalidBufferSize = !socketInfo.isUsed || buffer.empty() ||
		socketInfo.pendingSendBytes > MAX_PENDING_SEND_BYTES ||
		buffer.size() > MAX_PENDING_SEND_BYTES - socketInfo.pendingSendBytes;
	if (isInvalidBufferSize)
	{
		if (socketInfo.isUsed)
		{
			DisconnectClient(socketInfo.socket);
		}
		return false;
	}

	socketInfo.pendingSendBytes += buffer.size();
	socketInfo.unsentSendBytes += buffer.size();
	socketInfo.sendQueue.push_back(PendingSend{ std::move(buffer), 0 });

	// 최고 대기량은 송신 생산 속도가 소켓 처리 속도를 앞서는지 판단하는 값이다.
	socketInfo.networkStatistics.RecordQueueState(
		socketInfo.unsentSendBytes,
		socketInfo.sendQueue.size());
	if (FlushSendQueue(clientIndex) == SendResult::Error)
	{
		const SOCKET socket = socketInfo.socket;
		LogSocketError("send()", WSAGetLastError());
		DisconnectClient(socket);
		return false;
	}
	return true;
}

TCPServer::SendResult TCPServer::FlushSendQueue(int clientIndex)
{
	SocketInfo& socketInfo = mSocketInfos[clientIndex];
	while (!socketInfo.sendQueue.empty())
	{
		PendingSend& pending = socketInfo.sendQueue.front();
		const size_t remainingBytes = pending.buffer.size() - pending.sentBytes;
		const int sentBytes = send(
			socketInfo.socket,
			pending.buffer.data() + pending.sentBytes,
			static_cast<int>(remainingBytes),
			0);

		if (sentBytes > 0)
		{
			const std::uint8_t head = static_cast<std::uint8_t>(pending.buffer.front());
			// send()가 양수를 반환한 바이트만 실제 송신 처리량으로 기록한다.
			socketInfo.networkStatistics.RecordSentBytes(head, static_cast<size_t>(sentBytes));

			pending.sentBytes += static_cast<size_t>(sentBytes);
			socketInfo.unsentSendBytes -= static_cast<size_t>(sentBytes);
			if (pending.sentBytes == pending.buffer.size())
			{
				socketInfo.networkStatistics.RecordSentPacket(head);
				socketInfo.pendingSendBytes -= pending.buffer.size();
				socketInfo.sendQueue.pop_front();
			}
			continue;
		}

		if (sentBytes == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)
		{
			socketInfo.networkStatistics.RecordSendWouldBlock();
			return SendResult::Pending;
		}
		return SendResult::Error;
	}
	return SendResult::Complete;
}

void TCPServer::AppendBufferData(vector<char>& buffer, const void* data, size_t size)
{
	const char* bytes = static_cast<const char*>(data);
	buffer.insert(buffer.end(), bytes, bytes + size);
}

void TCPServer::ResetReceiveState(SocketInfo& socketInfo)
{
	socketInfo.receiveHead = ReceiveHead::Invalid;
	socketInfo.hasReceiveHead = false;
	socketInfo.receivedBytes = 0;
	socketInfo.currentPacketReceivedBytes = 0;
	std::fill(socketInfo.receiveBuffer.begin(), socketInfo.receiveBuffer.end(), 0);
}

TCPServer::ReceiveResult TCPServer::ReceiveData(int clientIndex, size_t expectedBytes)
{
	SocketInfo& socketInfo = mSocketInfos[clientIndex];

	bool isInvalidBufferSize =
		expectedBytes > MAX_PACKET_PAYLOAD_SIZE ||
		socketInfo.receivedBytes < 0 ||
		static_cast<size_t>(socketInfo.receivedBytes) > expectedBytes;
	if (isInvalidBufferSize)
	{
		std::cerr << "Invalid receive buffer state: client=" << clientIndex
			<< ", expectedBytes=" << expectedBytes
			<< ", receivedBytes=" << socketInfo.receivedBytes << '\n';
		return ReceiveResult::Error;
	}

	if (expectedBytes == 0)
	{
		return ReceiveResult::Complete;
	}

	const int remainRecvByte = static_cast<int>(expectedBytes) - socketInfo.receivedBytes;
	const int retval = recv(
		socketInfo.socket,
		socketInfo.receiveBuffer.data() + socketInfo.receivedBytes,
		remainRecvByte,
		0);
	if (retval > 0)
	{
		socketInfo.receivedBytes += retval;
		socketInfo.currentPacketReceivedBytes += static_cast<size_t>(retval);
		socketInfo.networkStatistics.RecordReceivedBytes(static_cast<size_t>(retval));
	}
	else if (retval == 0)
	{
		// recv()가 0이면 상대가 정상적으로 연결을 종료한 것이므로 재시도하지 않는다.
		std::cout << "Client closed connection while receiving: client=" << clientIndex << '\n';
		return ReceiveResult::Closed;
	}
	else
	{
		const int errorCode = WSAGetLastError();
		if (errorCode == WSAEWOULDBLOCK)
		{
			// 아직 필요한 바이트가 도착하지 않은 정상적인 논블로킹 상태다.
			// HEAD, 버퍼, 현재 수신 위치를 유지하고 다음 FD_READ에서 이어서 받는다.
			socketInfo.networkStatistics.RecordReceiveWouldBlock();
			return ReceiveResult::Pending;
		}

		std::cerr << "Receive failed: client=" << clientIndex
			<< ", error=" << errorCode << '\n';
		return ReceiveResult::Error;
	}

	if (static_cast<size_t>(socketInfo.receivedBytes) < expectedBytes)
	{
		// partial recv도 오류가 아니다. 현재까지 받은 바이트를 보존하고 다음 FD_READ를 기다린다.
		return ReceiveResult::Pending;
	}

	socketInfo.receivedBytes = 0;
	return ReceiveResult::Complete;
}

void TCPServer::ReportNetworkStatisticsIfDue()
{
	std::array<NetworkClientStatisticsView, MAX_CLIENT> clients = {};
	size_t clientCount = 0;
	for (size_t clientIndex = 0; clientIndex < mSocketInfos.size(); ++clientIndex)
	{
		SocketInfo& socketInfo = mSocketInfos[clientIndex];
		if (!socketInfo.isUsed)
		{
			continue;
		}

		clients[clientCount] = NetworkClientStatisticsView{
			clientIndex,
			socketInfo.unsentSendBytes,
			socketInfo.sendQueue.size(),
			&socketInfo.networkStatistics
		};
		++clientCount;
	}

	mNetworkStatisticsReporter.ReportIfDue(clients.data(), clientCount);
}
