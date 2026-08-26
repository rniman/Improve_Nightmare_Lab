#include "stdafx.h"
#include "TCPServer.h"
#include "ServerObject.h"
#include "ServerEnvironmentObject.h"
#include "ServerPlayer.h"
#include "ServerCollision.h"
#include "ServerWorldBuilder.h"

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

bool TCPServer::Initialize(HWND window)
{
	mRandomEngine = default_random_engine(random_device()());

	if (!InitializeWorld())
	{
		return false;
	}

	return InitializeNetworking(window);
}

void TCPServer::RunSimulationTick()
{
	mTimer.Tick();
	ReportNetworkStatisticsIfDue();

	if (mGameState != GameState::InGame)
	{
		return;
	}

	mGameState = DetermineGameOutcome();
	if (mGameState != GameState::InGame)
	{
		EnqueueGameOutcomePackets(mGameState);
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

	BuildPlayerReplicationStates();
	ReplicateOutOfSpaceObjects();
	ReplicateNearbyObjectsIfDue();
	ReplicateStateIfDue();
}

// Messages
void TCPServer::HandleWindowMessage(HWND window, UINT messageId, WPARAM wParam, LPARAM lParam)
{
	switch (messageId)
	{
	case WM_CREATE:
		mTimer.Start();
		break;
	case WM_SOUND:
	{
		const int clientIndex = static_cast<int>(lParam);
		if (clientIndex < 0 || clientIndex >= static_cast<int>(mConnections.size()))
		{
			break;
		}

		const std::size_t connectionIndex = static_cast<std::size_t>(clientIndex);
		ClientConnectionState& connection = mConnections[connectionIndex];
		if (!connection.isUsed)
		{
			break;
		}

		switch (static_cast<SoundMessage>(wParam))
		{
		case SoundMessage::OpenDrawer:
			connection.pendingPacketType = ServerPacketType::OpenDrawerSound;
			break;
		case SoundMessage::CloseDrawer:
			connection.pendingPacketType = ServerPacketType::CloseDrawerSound;
			break;
		case SoundMessage::OpenDoor:
			connection.pendingPacketType = ServerPacketType::OpenDoorSound;
			break;
		case SoundMessage::CloseDoor:
			connection.pendingPacketType = ServerPacketType::CloseDoorSound;
			break;
		case SoundMessage::BlueSuitDead:
			connection.pendingPacketType = ServerPacketType::BlueSuitDead;
			break;
		default:
			return;
		}
		EnqueuePendingPacket(clientIndex);
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

void TCPServer::HandleSocketMessage(HWND window, UINT messageId, WPARAM wParam, LPARAM lParam)
{
	// 소켓 이벤트가 계속 발생해 RunSimulationTick가 실행되지 않는 상황에서도 통계를 출력한다.
	ReportNetworkStatisticsIfDue();

	const int socketEvent = WSAGETSELECTEVENT(lParam);
	const int socketError = WSAGETSELECTERROR(lParam);
	if (socketError != 0)
	{
		LogSocketError("socket event", socketError);
		if (socketEvent != FD_ACCEPT)
		{
			DisconnectClient(static_cast<SOCKET>(wParam));
		}
		return;
	}

	switch (socketEvent)
	{
	case FD_ACCEPT:
		HandleAcceptEvent(window, static_cast<SOCKET>(wParam));
		break;
	case FD_READ:
		HandleReadEvent(static_cast<SOCKET>(wParam));
		break;
	case FD_WRITE:
		HandleWriteEvent(static_cast<SOCKET>(wParam));
		break;
	case FD_CLOSE:
		HandleCloseEvent(static_cast<SOCKET>(wParam));
		break;
	default:
		break;
	}

	return;
}

shared_ptr<CServerPlayer> TCPServer::GetPlayer(int clientIndex)
{
	if (clientIndex < 0 || clientIndex >= static_cast<int>(mPlayers.size()))
	{
		return nullptr;
	}
	return mPlayers[static_cast<std::size_t>(clientIndex)];
}

// Lifecycle
bool TCPServer::InitializeNetworking(HWND window)
{
	mWindowHandle = window;
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
	struct sockaddr_in serverAddress;
	memset(&serverAddress, 0, sizeof(serverAddress));
	serverAddress.sin_family = AF_INET;
	serverAddress.sin_addr.s_addr = htonl(INADDR_ANY);
	serverAddress.sin_port = htons(SERVERPORT);
	int result = bind(listenSocket, reinterpret_cast<struct sockaddr*>(&serverAddress), sizeof(serverAddress));
	if (result == SOCKET_ERROR)
	{
		const int errorCode = WSAGetLastError();
		LogSocketError("bind()", errorCode);
		closesocket(listenSocket);
		WSACleanup();
		return false;
	}

	// listen()
	result = listen(listenSocket, SOMAXCONN);
	if (result == SOCKET_ERROR)
	{
		const int errorCode = WSAGetLastError();
		LogSocketError("listen()", errorCode);
		closesocket(listenSocket);
		WSACleanup();
		return false;
	}

	// WSAAsyncSelect()
	result = WSAAsyncSelect(listenSocket, window, WM_SOCKET, FD_ACCEPT | FD_CLOSE);
	if (result == SOCKET_ERROR)
	{
		const int errorCode = WSAGetLastError();
		LogSocketError("WSAAsyncSelect()", errorCode);
		closesocket(listenSocket);
		WSACleanup();
		return false;
	}

	return true;
}

bool TCPServer::InitializeWorld()
{
	mGameState = GameState::InLobby;

	mCollisionManager = make_shared<CServerCollisionManager>();
	mCollisionManager->CreateCollision(SPACE_FLOOR, SPACE_WIDTH, SPACE_DEPTH);

	ServerWorldBuilder worldBuilder(*mCollisionManager, mRandomEngine);
	const ServerWorldBuildResult buildResult = worldBuilder.Build();
	if (!buildResult.succeeded)
	{
		return false;
	}

	if (buildResult.escapeDoorId < 0)
	{
		return false;
	}

	for (PlayerReplicationState& playerState : mPlayerReplicationStates)
	{
		playerState.playerInfo.escapeDoorId = buildResult.escapeDoorId;
	}

	return true;
}

// Socket events
void TCPServer::HandleAcceptEvent(HWND window, SOCKET listenSocket)
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

	if (mGameState == GameState::InGame)
	{
		closesocket(clientSocket);
		LogServerNotice("Connection refused because the game has already started.");
		return;
	}

	const INT8 clientIndex = RegisterClientConnection(clientSocket, clientAddress, clientAddressLength);

	// MAX_CLIENT보다 더 많은 접속 요구
	if (clientIndex < 0 || clientIndex >= static_cast<INT8>(mConnections.size()))
	{
		closesocket(clientSocket); // 클라이언트 소켓 종료
		LogServerNotice("Connection refused because the server is full.");
		return;
	}

	const int result = WSAAsyncSelect(clientSocket, window, WM_SOCKET, FD_READ | FD_WRITE | FD_CLOSE);
	if (result == SOCKET_ERROR)
	{
		const int errorCode = WSAGetLastError();
		LogSocketError("WSAAsyncSelect()", errorCode);
		DisconnectClient(clientSocket);
		return;
	}

	const std::size_t connectionIndex = static_cast<std::size_t>(clientIndex);
	ClientConnectionState& connection = mConnections[connectionIndex];
	std::shared_ptr<CServerPlayer>& player = mPlayers[connectionIndex];

	WCHAR clientListEntry[256];
	WCHAR clientIpAddress[16];
	ConvertCharToWideString(connection.ipAddress, clientIpAddress, 16);
	wsprintf(clientListEntry, L"CLIENT[%d], IP: %s, 포트 번호: %d\n", clientIndex, clientIpAddress, ntohs(connection.clientAddress.sin_port));
	SendMessage(mClientListBox, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(clientListEntry));

	if (clientIndex == ZOMBIEPLAYER) // ZombiePlayer는 0번 소켓에만 생성
	{
		player = make_shared<CServerZombiePlayer>();
		player->SetPlayerId(clientIndex);
		++mZombieCount;
	}
	else
	{
		player = make_shared<CServerBlueSuitPlayer>();
		player->SetPlayerId(clientIndex);
		++mBlueSuitCount;
	}

	mCollisionManager->AddCollisionPlayer(player, clientIndex);
	EnqueuePendingPacket(clientIndex);

	for (auto& socketInfo : mConnections)
	{
		if (!socketInfo.isUsed || socketInfo.socket == clientSocket)
		{
			continue;
		}
		socketInfo.pendingPacketType = ServerPacketType::ClientCount;
		EnqueuePendingPacket(FindClientIndex(socketInfo.socket));
	}

	return;
}

void TCPServer::HandleReadEvent(SOCKET socket)
{
	int clientIndex = FindClientIndex(socket);
	if (clientIndex < 0 || clientIndex >= static_cast<int>(mConnections.size()))
	{
		return;
	}

	const std::size_t initialConnectionIndex = static_cast<std::size_t>(clientIndex);
	ClientConnectionState& connection = mConnections[initialConnectionIndex];
	std::shared_ptr<CServerPlayer> player = mPlayers[initialConnectionIndex];

	if (!connection.hasReceivePacketType)
	{
		const ReceiveResult result = ReceiveData(clientIndex, sizeof(INT8));
		if (!HandleReceiveResult(result, socket))
		{
			return;
		}

		INT8 rawPacketType = -1;
		memcpy(
			&rawPacketType,
			connection.receiveBuffer.data(),
			sizeof(rawPacketType));
		std::fill(
			connection.receiveBuffer.begin(),
			connection.receiveBuffer.end(),
			0);

		// 등록되지 않은 HEAD는 payload 크기와 형식을 결정할 수 없으므로 더 이상 스트림을 해석하지 않는다.
		// 연결을 종료해 잘못된 바이트를 다음 패킷의 HEAD로 오인하는 상황도 방지한다.
		if (!IsValidClientPacketType(rawPacketType))
		{
			std::cerr << "Invalid receive packet head: client=" << clientIndex
				<< ", head=" << static_cast<int>(rawPacketType) << '\n';
			DisconnectClient(socket);
			return;
		}

		connection.receivePacketType = static_cast<ClientPacketType>(rawPacketType);
		connection.hasReceivePacketType = true;
	}

	bool shouldEnqueuePendingPacket = false;
	switch (connection.receivePacketType)
	{
	case ClientPacketType::GameStart:
		HandleGameStartPacket(clientIndex);
		shouldEnqueuePendingPacket = true;
		break;
	case ClientPacketType::ChangeSlot:
		if (!TryHandleChangeSlotPacket(socket, clientIndex))
		{
			return;
		}
		shouldEnqueuePendingPacket = true;
		break;
	case ClientPacketType::KeysBuffer:
		if (!TryHandleClientInputPacket(socket, clientIndex, player))
		{
			return;
		}
		break;
	case ClientPacketType::LoadingComplete:
		shouldEnqueuePendingPacket = HandleLoadingCompletePacket(clientIndex);
		break;
	default:
		break;
	}

	// HEAD와 DATA가 모두 도착해 하나의 애플리케이션 패킷이 완성된 시점에만 패킷 수를 증가시킨다.
	if (clientIndex < 0 || clientIndex >= static_cast<int>(mConnections.size()))
	{
		return;
	}

	const std::size_t finalConnectionIndex = static_cast<std::size_t>(clientIndex);
	ClientConnectionState& socketInfo = mConnections[finalConnectionIndex];
	socketInfo.networkStatistics.RecordReceivedPacket(
		static_cast<std::uint8_t>(socketInfo.receivePacketType),
		socketInfo.currentPacketReceivedBytes);
	ResetReceiveState(socketInfo);
	if (shouldEnqueuePendingPacket)
	{
		EnqueuePendingPacket(clientIndex);
	}
}

void TCPServer::HandleWriteEvent(SOCKET socket)
{
	const int clientIndex = FindClientIndex(socket);
	if (clientIndex < 0 || clientIndex >= static_cast<int>(mConnections.size()))
	{
		return;
	}

	if (FlushSendQueue(clientIndex) == SendResult::Error)
	{
		LogSocketError("send()", WSAGetLastError());
		DisconnectClient(socket);
	}
}

void TCPServer::HandleCloseEvent(SOCKET socket)
{
	DisconnectClient(socket);
}

// Receive
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

bool TCPServer::IsValidClientPacketType(INT8 packetType) const
{
	// 서버가 payload 크기와 처리 방법을 알고 있는 클라이언트 패킷만 허용한다.
	switch (static_cast<ClientPacketType>(packetType))
	{
	case ClientPacketType::KeysBuffer:
	case ClientPacketType::GameStart:
	case ClientPacketType::ChangeSlot:
	case ClientPacketType::LoadingComplete:
		return true;
	default:
		return false;
	}
}

TCPServer::ReceiveResult TCPServer::ReceiveData(int clientIndex, size_t expectedBytes)
{
	if (clientIndex < 0 || clientIndex >= static_cast<int>(mConnections.size()))
	{
		return ReceiveResult::Error;
	}

	const std::size_t connectionIndex = static_cast<std::size_t>(clientIndex);
	ClientConnectionState& socketInfo = mConnections[connectionIndex];

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

	const int remainingBytes = static_cast<int>(expectedBytes) - socketInfo.receivedBytes;
	const int result = recv(
		socketInfo.socket,
		socketInfo.receiveBuffer.data() + socketInfo.receivedBytes,
		remainingBytes,
		0);
	if (result > 0)
	{
		socketInfo.receivedBytes += result;
		socketInfo.currentPacketReceivedBytes += static_cast<size_t>(result);
		socketInfo.networkStatistics.RecordReceivedBytes(static_cast<size_t>(result));
	}
	else if (result == 0)
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

void TCPServer::ResetReceiveState(ClientConnectionState& socketInfo)
{
	socketInfo.receivePacketType = ClientPacketType::Invalid;
	socketInfo.hasReceivePacketType = false;
	socketInfo.receivedBytes = 0;
	socketInfo.currentPacketReceivedBytes = 0;
	std::fill(socketInfo.receiveBuffer.begin(), socketInfo.receiveBuffer.end(), 0);
}

// Client packets
void TCPServer::HandleGameStartPacket(int clientIndex)
{
	mGameState = GameState::InGame;
	mCanReplicateState = false;
	mZombieCount = 0;
	mBlueSuitCount = 0;
	for (std::size_t connectionIndex = 0; connectionIndex < mConnections.size(); ++connectionIndex)
	{
		ClientConnectionState& connection = mConnections[connectionIndex];
		if (!connection.isUsed)
		{
			continue;
		}

		if (connectionIndex == 0)
		{
			++mZombieCount;
		}
		else
		{
			++mBlueSuitCount;
		}

		const int currentClientIndex = static_cast<int>(connectionIndex);
		std::shared_ptr<CServerPlayer>& player = mPlayers[connectionIndex];
		AssignUniquePlayerSpawnPosition(player, currentClientIndex);
		mCollisionManager->AddCollisionPlayer(player, currentClientIndex);
		connection.pendingPacketType = ServerPacketType::GameStart;

		if (currentClientIndex != clientIndex)
		{
			EnqueuePendingPacket(currentClientIndex);
		}
	}
}

bool TCPServer::TryHandleChangeSlotPacket(SOCKET socket, int& clientIndex)
{
	if (clientIndex < 0 || clientIndex >= static_cast<int>(mConnections.size()))
	{
		return false;
	}

	const std::size_t currentConnectionIndex = static_cast<std::size_t>(clientIndex);
	if (!HandleReceiveResult(ReceiveData(clientIndex, sizeof(INT8)), socket))
	{
		return false;
	}

	INT8 selectedSlot = -1;
	memcpy(
		&selectedSlot,
		mConnections[currentConnectionIndex].receiveBuffer.data(),
		sizeof(selectedSlot));
	// 슬롯 번호는 플레이어 및 소켓 배열의 인덱스로 사용되므로 범위를 벗어난 값은 무시할 수 없다.
	// 잘못된 연결을 종료해 이후 패킷이 비정상적인 서버 상태에 반영되는 것을 방지한다.
	if (selectedSlot < 0 || selectedSlot >= static_cast<INT8>(MAX_CLIENT))
	{
		std::cerr << "Invalid selected slot: client=" << clientIndex
			<< ", slot=" << static_cast<int>(selectedSlot) << '\n';
		DisconnectClient(socket);
		return false;
	}

	const std::size_t selectedConnectionIndex = static_cast<std::size_t>(selectedSlot);
	std::shared_ptr<CServerPlayer>& selectedPlayer = mPlayers[selectedConnectionIndex];
	std::shared_ptr<CServerPlayer>& currentPlayer = mPlayers[currentConnectionIndex];
	ClientConnectionState& selectedConnection = mConnections[selectedConnectionIndex];
	ClientConnectionState& currentConnection = mConnections[currentConnectionIndex];
	PlayerReplicationState& selectedPlayerState = mPlayerReplicationStates[selectedConnectionIndex];
	PlayerReplicationState& currentPlayerState = mPlayerReplicationStates[currentConnectionIndex];

	if (!selectedPlayer)
	{
		selectedPlayer = make_shared<CServerBlueSuitPlayer>();
	}

	if (selectedPlayer->GetPlayerId() == -1)
	{
		selectedPlayer->SetPlayerId(selectedSlot);

		// 소켓과 수신 진행 상태는 하나의 단위이므로 전체를 함께 이동한다.
		selectedConnection = std::move(currentConnection);
		currentConnection = ClientConnectionState{};

		selectedPlayerState.clientId = selectedSlot;
		currentPlayerState.clientId = -1;
		currentPlayer->SetPlayerId(-1);
	}
	else
	{
		// 역할 슬롯을 교환해도 각 소켓의 수신 상태는 해당 소켓과 함께 이동해야 한다.
		std::swap(currentConnection, selectedConnection);
		selectedPlayerState.clientId = selectedSlot;
	}

	selectedConnection.pendingPacketType = ServerPacketType::ChangeSlot;
	// 이후 통계, 수신 상태 초기화, 응답 전송도 이동한 소켓 슬롯을 대상으로 해야 한다.
	clientIndex = selectedSlot;
	return true;
}

bool TCPServer::TryHandleClientInputPacket(
	SOCKET socket,
	int clientIndex,
	const std::shared_ptr<CServerPlayer>& player)
{
	if (clientIndex < 0 || clientIndex >= static_cast<int>(mConnections.size()))
	{
		return false;
	}

	const std::size_t connectionIndex = static_cast<std::size_t>(clientIndex);
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
	ClientConnectionState& socketInfo = mConnections[connectionIndex];
	auto readValue = [&socketInfo, &readOffset](auto& value)
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
	mPlayerReplicationStates[connectionIndex].pitch = input.pitch;
	player->SetRightClick(input.rightClick != 0);
	return true;
}

bool TCPServer::HandleLoadingCompletePacket(int clientIndex)
{
	if (mCanReplicateState ||
		clientIndex < 0 ||
		clientIndex >= static_cast<int>(mConnections.size()))
	{
		return false;
	}

	const std::size_t connectionIndex = static_cast<std::size_t>(clientIndex);
	ClientConnectionState& connection = mConnections[connectionIndex];
	connection.isLoadingComplete = true;
	if (!AreAllClientsLoadingComplete())
	{
		return false;
	}

	mCanReplicateState = true;
	const auto currentTime = std::chrono::steady_clock::now();
	mNextStateReplicationTime = currentTime;
	mNextNearbyObjectReplicationTime = currentTime;
	connection.pendingPacketType = ServerPacketType::LoadingComplete;
	return true;
}

bool TCPServer::AreAllClientsLoadingComplete() const
{
	bool hasActiveClient = false;
	for (const auto& socketInfo : mConnections)
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

// Connections
INT8 TCPServer::RegisterClientConnection(
	SOCKET clientSocket,
	struct sockaddr_in clientAddress,
	int clientAddressLength)
{
	INT8 clientIndex = -1;
	if (mClientCount >= static_cast<INT8>(mConnections.size()))
	{
		return clientIndex;
	}
	ClientConnectionState socketInfo;

	socketInfo.isUsed = true;
	socketInfo.socket = clientSocket;
	socketInfo.clientAddress = clientAddress;
	socketInfo.clientAddressLength = clientAddressLength;

	getpeername(socketInfo.socket, (struct sockaddr*)&socketInfo.clientAddress, &socketInfo.clientAddressLength);
	inet_ntop(AF_INET, &socketInfo.clientAddress.sin_addr, socketInfo.ipAddress, sizeof(socketInfo.ipAddress));

	socketInfo.pendingPacketType = ServerPacketType::Init;

	// 배열에 정보 추가
	for (std::size_t connectionIndex = 0; connectionIndex < mConnections.size(); ++connectionIndex)
	{
		ClientConnectionState& connection = mConnections[connectionIndex];
		if (connection.isUsed)
		{
			continue;
		}
		mClientCount++;

		// 클라이언트 정보 초기화
		clientIndex = static_cast<INT8>(connectionIndex);
		mPlayerReplicationStates[connectionIndex].clientId = clientIndex;
		connection = std::move(socketInfo);
		break;
	}

	return clientIndex;
}

INT8 TCPServer::FindClientIndex(SOCKET clientSocket) const
{
	for (size_t index = 0; index < mConnections.size(); ++index)
	{
		const ClientConnectionState& socketInfo = mConnections[index];
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

bool TCPServer::DisconnectClient(SOCKET clientSocket)
{
	const INT8 clientIndex = FindClientIndex(clientSocket);
	if (clientIndex < 0 || clientIndex >= static_cast<INT8>(mConnections.size()))
	{
		return false;
	}

	const std::size_t connectionIndex = static_cast<std::size_t>(clientIndex);
	ClientConnectionState& connection = mConnections[connectionIndex];
	std::shared_ptr<CServerPlayer>& player = mPlayers[connectionIndex];

	// ClientConnectionState가 초기화되기 전에 이 연결의 누적 측정값을 남긴다.
	mNetworkStatisticsReporter.ReportDisconnected(
		clientIndex,
		connection.networkStatistics);

	INT8 listBoxIndex = -1;
	for (std::size_t index = 0; index <= connectionIndex; ++index)
	{
		if (mConnections[index].isUsed)
		{
			++listBoxIndex;
		}
	}

	const bool hadPlayer = (player != nullptr);
	if (hadPlayer)
	{
		SendMessage(mClientListBox, LB_DELETESTRING, static_cast<WPARAM>(listBoxIndex), 0);
		if (clientIndex == ZOMBIEPLAYER)
		{
			mZombieCount = max(0, mZombieCount - 1);
		}
		else
		{
			mBlueSuitCount = max(0, mBlueSuitCount - 1);
		}
	}

	WSAAsyncSelect(clientSocket, mWindowHandle, 0, 0);
	shutdown(clientSocket, SD_BOTH);
	closesocket(clientSocket);

	player.reset();
	mPlayerStartPositionIndices[connectionIndex] = -1;
	mPlayerReplicationStates[connectionIndex] = PlayerReplicationState{};
	connection = ClientConnectionState{};
	mClientCount = max<INT8>(0, mClientCount - 1);

	if (hadPlayer)
	{
		for (auto& otherClientConnectionState : mConnections)
		{
			if (!otherClientConnectionState.isUsed)
			{
				continue;
			}

			otherClientConnectionState.pendingPacketType = ServerPacketType::ClientCount;
			EnqueuePendingPacket(FindClientIndex(otherClientConnectionState.socket));
		}
	}

	// 로딩 중이던 마지막 미완료 클라이언트가 나가면 남은 클라이언트의 게임을 시작한다.
	if (mGameState == GameState::InGame &&
		!mCanReplicateState &&
		AreAllClientsLoadingComplete())
	{
		for (std::size_t connectionIndex = 0; connectionIndex < mConnections.size(); ++connectionIndex)
		{
			ClientConnectionState& otherConnection = mConnections[connectionIndex];
			if (!otherConnection.isUsed)
			{
				continue;
			}

			mCanReplicateState = true;
			const auto currentTime = std::chrono::steady_clock::now();
			mNextStateReplicationTime = currentTime;
			mNextNearbyObjectReplicationTime = currentTime;
			otherConnection.pendingPacketType = ServerPacketType::LoadingComplete;
			EnqueuePendingPacket(static_cast<int>(connectionIndex));
			break;
		}
	}

	const bool shouldCloseServer = mGameState != GameState::InLobby && mClientCount == 0;
	if (shouldCloseServer)
	{
		// 소켓 이벤트 처리 중 창을 직접 파괴하지 않고 메시지 루프에서 정상 종료한다.
		LogServerNotice("Closing because the last client disconnected after the game started.");
		PostMessage(mWindowHandle, WM_CLOSE, 0, 0);
	}

	return true;
}

// Send
template<class... Args>
bool TCPServer::EnqueuePacketFields(int clientIndex, Args&&... args)
{
	const size_t bufferSize = (sizeof(args) + ... + 0);
	std::vector<char> buffer(bufferSize);
	size_t offset = 0;
	((memcpy(buffer.data() + offset, &args, sizeof(args)), offset += sizeof(args)), ...);

	return EnqueuePacketBuffer(clientIndex, std::move(buffer));
}

bool TCPServer::EnqueuePacketBuffer(int clientIndex, vector<char> buffer)
{
	if (clientIndex < 0 || clientIndex >= static_cast<int>(mConnections.size()))
	{
		return false;
	}

	const std::size_t connectionIndex = static_cast<std::size_t>(clientIndex);
	ClientConnectionState& socketInfo = mConnections[connectionIndex];
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
	if (clientIndex < 0 || clientIndex >= static_cast<int>(mConnections.size()))
	{
		return SendResult::Error;
	}

	const std::size_t connectionIndex = static_cast<std::size_t>(clientIndex);
	ClientConnectionState& socketInfo = mConnections[connectionIndex];
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

void TCPServer::EnqueuePendingPacket(int clientIndex)
{
	if (clientIndex < 0 || clientIndex >= static_cast<int>(mConnections.size()))
	{
		return;
	}

	const std::size_t connectionIndex = static_cast<std::size_t>(clientIndex);
	ClientConnectionState& connection = mConnections[connectionIndex];
	if (!connection.isUsed)
	{
		return;
	}

	switch (connection.pendingPacketType)
	{
	case ServerPacketType::GameStart:
		connection.pendingPacketType = ServerPacketType::PlayerState;
		EnqueuePacketFields(clientIndex, static_cast<INT8>(ServerPacketType::GameStart));
		break;
	case ServerPacketType::ChangeSlot:
		connection.pendingPacketType = ServerPacketType::PlayerState;
		for (std::size_t recipientIndex = 0; recipientIndex < mConnections.size(); ++recipientIndex)
		{
			if (!mConnections[recipientIndex].isUsed)
			{
				continue;
			}

			EnqueuePacketFields(
				static_cast<int>(recipientIndex),
				static_cast<INT8>(ServerPacketType::ChangeSlot),
				mPlayerReplicationStates[recipientIndex].clientId,
				mPlayerReplicationStates);
		}
		break;
	case ServerPacketType::Init:
		connection.pendingPacketType = ServerPacketType::PlayerState;
		EnqueuePacketFields(
			clientIndex,
			static_cast<INT8>(ServerPacketType::Init),
			mPlayerReplicationStates[connectionIndex].clientId,
			mClientCount,
			mPlayerReplicationStates);
		break;
	case ServerPacketType::PlayerState:
		if (mGameState == GameState::InLobby)
		{
			break;
		}
		EnqueuePacketFields(
			clientIndex,
			static_cast<INT8>(ServerPacketType::PlayerState),
			mPlayerReplicationStates);
		break;
	case ServerPacketType::ClientCount:
		connection.pendingPacketType = ServerPacketType::PlayerState;
		EnqueuePacketFields(
			clientIndex,
			static_cast<INT8>(ServerPacketType::ClientCount),
			mClientCount,
			mPlayerReplicationStates);
		break;
	case ServerPacketType::BlueSuitWin:
		connection.pendingPacketType = ServerPacketType::PlayerState;
		EnqueuePacketFields(clientIndex, static_cast<INT8>(ServerPacketType::BlueSuitWin));
		break;
	case ServerPacketType::ZombieWin:
		connection.pendingPacketType = ServerPacketType::PlayerState;
		EnqueuePacketFields(clientIndex, static_cast<INT8>(ServerPacketType::ZombieWin));
		break;
	case ServerPacketType::OpenDrawerSound:
		connection.pendingPacketType = ServerPacketType::PlayerState;
		EnqueuePacketFields(clientIndex, static_cast<INT8>(ServerPacketType::OpenDrawerSound));
		break;
	case ServerPacketType::CloseDrawerSound:
		connection.pendingPacketType = ServerPacketType::PlayerState;
		EnqueuePacketFields(clientIndex, static_cast<INT8>(ServerPacketType::CloseDrawerSound));
		break;
	case ServerPacketType::OpenDoorSound:
		connection.pendingPacketType = ServerPacketType::PlayerState;
		EnqueuePacketFields(clientIndex, static_cast<INT8>(ServerPacketType::OpenDoorSound));
		break;
	case ServerPacketType::CloseDoorSound:
		connection.pendingPacketType = ServerPacketType::PlayerState;
		EnqueuePacketFields(clientIndex, static_cast<INT8>(ServerPacketType::CloseDoorSound));
		break;
	case ServerPacketType::BlueSuitDead:
	{
		connection.pendingPacketType = ServerPacketType::PlayerState;
		const char deadUserId = static_cast<char>(clientIndex);
		for (std::size_t recipientIndex = 0; recipientIndex < mConnections.size(); ++recipientIndex)
		{
			if (mConnections[recipientIndex].isUsed)
			{
				EnqueuePacketFields(
					static_cast<int>(recipientIndex),
					static_cast<INT8>(ServerPacketType::BlueSuitDead),
					deadUserId);
			}
		}
		break;
	}
	case ServerPacketType::LoadingComplete:
	{
		connection.pendingPacketType = ServerPacketType::PlayerState;
		for (std::size_t recipientIndex = 0; recipientIndex < mConnections.size(); ++recipientIndex)
		{
			const int recipientClientIndex = static_cast<int>(recipientIndex);
			const std::shared_ptr<CServerPlayer>& player = mPlayers[recipientIndex];
			if (mConnections[recipientIndex].isUsed &&
				EnqueuePacketFields(
					recipientClientIndex,
					static_cast<INT8>(ServerPacketType::LoadingComplete)) &&
				player)
			{
				player->GameStartLogic();
			}
		}
		BroadcastOpenableObjectSnapshot();
		break;
	}
	default:
		break;
	}
}

void TCPServer::AppendBytes(vector<char>& buffer, const void* data, size_t size)
{
	const char* bytes = static_cast<const char*>(data);
	buffer.insert(buffer.end(), bytes, bytes + size);
}

// Game session
GameState TCPServer::DetermineGameOutcome()
{
	GameState gameState = GameState::InGame;

	if (mZombieCount == 1 && mBlueSuitCount > 0)
	{
		int aliveBlueSuitCount = 0;
		for (std::size_t playerIndex = 1; playerIndex < mPlayers.size(); ++playerIndex)
		{
			const std::shared_ptr<CServerPlayer>& player = mPlayers[playerIndex];
			if (!player || player->GetPlayerId() == -1)
			{
				continue;
			}

			if (player->IsAlive())
			{
				++aliveBlueSuitCount;
			}
		}

		if (aliveBlueSuitCount == 0)
		{
			gameState = GameState::ZombieWin;
			return gameState;
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
				gameState = GameState::BlueSuitWin;
			}
			break;
		}
	}

	return gameState;
}

void TCPServer::EnqueueGameOutcomePackets(GameState gameState)
{
	for (std::size_t connectionIndex = 0; connectionIndex < mConnections.size(); ++connectionIndex)
	{
		ClientConnectionState& socketInfo = mConnections[connectionIndex];
		if (!socketInfo.isUsed)
		{
			continue;
		}

		if (gameState == GameState::BlueSuitWin) // BLUE SUIT WIN
		{
			socketInfo.pendingPacketType = ServerPacketType::BlueSuitWin;
		}
		else // ZOMBIE WIN
		{
			socketInfo.pendingPacketType = ServerPacketType::ZombieWin;
		}

		// 정기 상태 복제가 종료된 뒤에도 승리 결과는 상태 전환 시 즉시 한 번 전송한다.
		EnqueuePendingPacket(static_cast<int>(connectionIndex));
	}
}

// Replication
void TCPServer::BuildPlayerReplicationStates()
{
	for (const auto& player : mPlayers)
	{
		if (!player || player->GetPlayerId() == -1)
		{
			continue;
		}

		const INT8 playerId = player->GetPlayerId();
		if (playerId < 0 || playerId >= static_cast<INT8>(mPlayerReplicationStates.size()))
		{
			continue;
		}

		const std::size_t playerIndex = static_cast<std::size_t>(playerId);
		auto& playerState = mPlayerReplicationStates[playerIndex];
		playerState.alive = player->IsAlive();
		playerState.running = player->IsRunning();
		playerState.position = player->GetPosition();
		playerState.velocity = player->GetVelocity();
		playerState.look = player->GetLook();

		const auto pickedObject = player->GetPickedObject().lock();
		playerState.pickedObjectId = pickedObject
			? pickedObject->GetCollisionNum()
			: -1;

		if (playerId == ZOMBIEPLAYER)
		{
			shared_ptr<CServerZombiePlayer> zombiePlayer = dynamic_pointer_cast<CServerZombiePlayer>(player);
			if (zombiePlayer)
			{
				playerState.slotObjectIds[0] = zombiePlayer->IsTracking() ? 1 : -1;		// 추적
				playerState.slotObjectIds[1] = zombiePlayer->IsInterruption() ? 1 : -1;	// 시야방해
				playerState.slotObjectIds[2] = zombiePlayer->IsAttack() ? 1 : -1;			// 공격

				// 짧은 지뢰 충돌 상태를 복제한 뒤 서버 상태를 원래 값으로 되돌린다.
				if (zombiePlayer->GetCollideMineRef() == -1)
				{
					zombiePlayer->SetExplosionDelay(0.0f);
				}
				else if (zombiePlayer->GetExplosionDelay() > 0.05f)
				{
					zombiePlayer->SetCollideMineRef(-1);
				}
				playerState.playerInfo.mineObjectId = zombiePlayer->GetCollideMineRef();
			}
		}
		else
		{
			shared_ptr<CServerBlueSuitPlayer> blueSuitPlayer = dynamic_pointer_cast<CServerBlueSuitPlayer>(player);
			if (blueSuitPlayer)
			{
				for (int i = 0; i < 3; ++i)
				{
					playerState.slotObjectIds[i] = blueSuitPlayer->GetReferenceSlotItemNum(i);
					playerState.fuseObjectIds[i] = blueSuitPlayer->GetReferenceFuseItemNum(i);
				}
				playerState.playerInfo.attacked = blueSuitPlayer->IsAttacked();
				playerState.playerInfo.selectedItem = blueSuitPlayer->GetRightItem();
				playerState.playerInfo.teleportItemUsed = blueSuitPlayer->IsTeleportUse();
			}
		}
	}
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
	for (std::size_t connectionIndex = 0; connectionIndex < mConnections.size(); ++connectionIndex)
	{
		const ClientConnectionState& socketInfo = mConnections[connectionIndex];
		if (!socketInfo.isUsed || socketInfo.pendingPacketType != ServerPacketType::PlayerState)
		{
			continue;
		}

		EnqueuePendingPacket(static_cast<int>(connectionIndex));
	}
}

void TCPServer::BroadcastOpenableObjectState(
	int objectId,
	OpenableObjectType objectType,
	bool opened)
{
	OpenableObjectState objectState = {};
	objectState.objectId = objectId;
	objectState.objectType = objectType;
	objectState.opened = opened ? 1 : 0;
	if (objectState.objectId < 0 || !objectState.IsValidOpenableObjectType())
	{
		return;
	}

	for (std::size_t connectionIndex = 0; connectionIndex < mConnections.size(); ++connectionIndex)
	{
		if (!mConnections[connectionIndex].isUsed)
		{
			continue;
		}

		EnqueuePacketFields(
			static_cast<int>(connectionIndex),
			static_cast<INT8>(ServerPacketType::OpenableObjectState),
			objectState);
	}
}

void TCPServer::BroadcastOpenableObjectSnapshot()
{
	const int collisionObjectCount = CServerCollisionManager::GetNumberOfCollisionObject();
	std::vector<OpenableObjectState> objectStates;
	objectStates.reserve(collisionObjectCount);

	for (int objectId = 0; objectId < collisionObjectCount; ++objectId)
	{
		const shared_ptr<CServerGameObject> gameObject =
			mCollisionManager->GetCollisionObjectWithNumber(objectId);

		OpenableObjectState objectState = {};
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

	const size_t payloadBytes = sizeof(OpenableObjectState) * objectStates.size();
	if (payloadBytes > MAX_PACKET_PAYLOAD_SIZE)
	{
		return;
	}

	const std::uint16_t wirePayloadBytes = static_cast<std::uint16_t>(payloadBytes);
	std::vector<char> packetBuffer;
	packetBuffer.reserve(sizeof(INT8) + sizeof(wirePayloadBytes) + payloadBytes);
	packetBuffer.push_back(static_cast<INT8>(ServerPacketType::OpenableObjectSnapshot));
	AppendBytes(packetBuffer, &wirePayloadBytes, sizeof(wirePayloadBytes));
	if (!objectStates.empty())
	{
		AppendBytes(packetBuffer, objectStates.data(), payloadBytes);
	}

	for (std::size_t connectionIndex = 0; connectionIndex < mConnections.size(); ++connectionIndex)
	{
		if (mConnections[connectionIndex].isUsed)
		{
			EnqueuePacketBuffer(static_cast<int>(connectionIndex), packetBuffer);
		}
	}
}

void TCPServer::ReplicateOutOfSpaceObjects()
{
	// 시뮬레이션에서 발생한 공간 이동은 정기 snapshot을 기다리지 않고 즉시 큐에 추가한다.
	const std::vector<SC_SPACEOUT_OBJECT> outOfSpaceObjects = CollectOutOfSpaceObjects();
	EnqueueOutOfSpaceObjectPackets(outOfSpaceObjects);
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
		packetBuffer.push_back(static_cast<INT8>(ServerPacketType::SpaceOutObjects));
		AppendBytes(packetBuffer, &wirePayloadBytes, sizeof(wirePayloadBytes));
		AppendBytes(packetBuffer, objectUpdates.data() + firstObjectIndex, payloadBytes);

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
			EnqueuePacketBuffer(playerId, packetBuffer);
		}
	}
}

void TCPServer::ReplicateNearbyObjectsIfDue()
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
	BuildNearbyObjectSnapshots();
	EnqueueNearbyObjectSnapshots();
}

void TCPServer::BuildNearbyObjectSnapshots()
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
		if (playerId < 0 || playerId >= static_cast<INT8>(mNearbyObjectSnapshots.size()))
		{
			continue;
		}

		const std::size_t playerIndex = static_cast<std::size_t>(playerId);
		auto& objectSnapshot = mNearbyObjectSnapshots[playerIndex];
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
						[objectId](const NearbyObjectState& objectInfo)
						{
							return objectInfo.objectId == objectId;
						});
					if (alreadyIncluded)
					{
						continue;
					}

					objectSnapshot.push_back(NearbyObjectState{ objectId, gameObject->GetWorldMatrix() });
					if (objectSnapshot.size() == maxNearbyObjects)
					{
						break;
					}
				}
			}
		}
	}
}

void TCPServer::EnqueueNearbyObjectSnapshots()
{
	for (std::size_t connectionIndex = 0; connectionIndex < mConnections.size(); ++connectionIndex)
	{
		if (!mConnections[connectionIndex].isUsed)
		{
			continue;
		}

		const auto& objectSnapshot = mNearbyObjectSnapshots[connectionIndex];
		const size_t payloadBytes = sizeof(NearbyObjectState) * objectSnapshot.size();
		const std::uint16_t wirePayloadBytes = static_cast<std::uint16_t>(payloadBytes);

		std::vector<char> packetBuffer;
		packetBuffer.reserve(sizeof(INT8) + sizeof(wirePayloadBytes) + payloadBytes);
		packetBuffer.push_back(static_cast<INT8>(ServerPacketType::NearbyObjects));
		AppendBytes(packetBuffer, &wirePayloadBytes, sizeof(wirePayloadBytes));
		if (!objectSnapshot.empty())
		{
			AppendBytes(packetBuffer, objectSnapshot.data(), payloadBytes);
		}
		EnqueuePacketBuffer(static_cast<int>(connectionIndex), std::move(packetBuffer));
	}
}

void TCPServer::AssignUniquePlayerSpawnPosition(
	shared_ptr<CServerPlayer>& serverPlayer,
	int clientIndex)
{
	if (!serverPlayer ||
		clientIndex < 0 ||
		clientIndex >= static_cast<int>(mPlayerStartPositionIndices.size()))
	{
		return;
	}

	std::uniform_int_distribution<int> spawnPositionDistribution(
		0,
		static_cast<int>(mPlayerStartPositions.size()) - 1);

	// 기존 코드가 첫 반복 전에 소비하던 난수까지 유지해 이후 월드 난수 결과가 달라지지 않게 한다.
	int spawnPositionIndex = spawnPositionDistribution(mRandomEngine);
	bool isAvailable = false;
	while (!isAvailable)
	{
		isAvailable = true;
		spawnPositionIndex = spawnPositionDistribution(mRandomEngine);
		for (const int assignedSpawnIndex : mPlayerStartPositionIndices)
		{
			if (assignedSpawnIndex == spawnPositionIndex)
			{
				isAvailable = false;
				break;
			}
		}
	}

	if (spawnPositionIndex < 0 ||
		spawnPositionIndex >= static_cast<int>(mPlayerStartPositions.size()))
	{
		return;
	}

	const std::size_t playerIndex = static_cast<std::size_t>(clientIndex);
	const std::size_t positionIndex = static_cast<std::size_t>(spawnPositionIndex);
	mPlayerStartPositionIndices[playerIndex] = spawnPositionIndex;
	const XMFLOAT3 spawnPosition = mPlayerStartPositions[positionIndex];
	serverPlayer->SetPlayerPosition(spawnPosition);
	serverPlayer->SetPlayerOldPosition(spawnPosition);
}

// Diagnostics
void TCPServer::ReportNetworkStatisticsIfDue()
{
	std::array<NetworkClientStatisticsView, MAX_CLIENT> clients = {};
	size_t clientCount = 0;
	for (size_t clientIndex = 0; clientIndex < mConnections.size(); ++clientIndex)
	{
		ClientConnectionState& socketInfo = mConnections[clientIndex];
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
