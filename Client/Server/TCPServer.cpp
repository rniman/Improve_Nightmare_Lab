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

	mGameState = GameState::InLobby;

	mCollisionManager = make_shared<CServerCollisionManager>();
	mCollisionManager->CreateCollision(SPACE_FLOOR, SPACE_WIDTH, SPACE_DEPTH);

	// 씬 생성
	LoadServerScene();
	vector<int> elevatorDoorIds;
	for (int i = 0; i < mCollisionManager->GetNumberOfCollisionObject(); ++i) {
		shared_ptr<CServerGameObject> object = mCollisionManager->GetCollisionObjectWithNumber(i);
		auto elevatorDoor = dynamic_pointer_cast<CServerElevatorDoorObject>(object);

		if (elevatorDoor) {
			if (strcmp(elevatorDoor->m_pstrFrameName, "Door1")) {
				continue;
			}
			elevatorDoorIds.push_back(i);
		}
	}
	int elevatorDoorCount = static_cast<int>(elevatorDoorIds.size());

	uniform_int_distribution<int> elevatorDoorDistribution(0, elevatorDoorCount - 1);

	int escapeDoorIndex = elevatorDoorDistribution(mRandomEngine);
	for (int i = 0; i < elevatorDoorCount; ++i) {
		shared_ptr<CServerGameObject> object = mCollisionManager->GetCollisionObjectWithNumber(elevatorDoorIds[i]);
		auto elevatorDoor = dynamic_pointer_cast<CServerElevatorDoorObject>(object);
		if (!elevatorDoor) {
			//std::cout << "엘리베이터 문이 아닙니다.!" << std::endl;
			assert(0); //반드시 CServerElevatorDoorObject 일것임. 아니면 시스템 종료 씬 오브젝트 정렬의 문제 발생
		}

		if (i == escapeDoorIndex) {
			elevatorDoor->SetEscapeDoor(true);
			for (int playerIndex = 0; playerIndex < MAX_CLIENT; ++playerIndex) {
				mPlayerReplicationStates[playerIndex].playerInfo.escapeDoorId = elevatorDoorIds[i];
			}
		}
		//elevatorDoor->SetEscapeDoor(false); // 디버그를 위해서 모든 문을 잠금
	}

	//std::cout << "생성된 충돌객체 = " << mCollisionManager->GetNumberOfCollisionObject() << std::endl;
	// 아이템 생성
	PopulateSceneItems();
	//std::cout << "아이템 생성후 생성된 충돌객체 = " << mCollisionManager->GetNumberOfCollisionObject() << std::endl;


	return true;
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
		if (clientIndex < 0 || clientIndex >= static_cast<int>(mConnections.size()) ||
			!mConnections[clientIndex].isUsed)
		{
			break;
		}

		switch (static_cast<SoundMessage>(wParam))
		{
		case SoundMessage::OpenDrawer:
			mConnections[clientIndex].pendingPacketType = ServerPacketType::OpenDrawerSound;
			break;
		case SoundMessage::CloseDrawer:
			mConnections[clientIndex].pendingPacketType = ServerPacketType::CloseDrawerSound;
			break;
		case SoundMessage::OpenDoor:
			mConnections[clientIndex].pendingPacketType = ServerPacketType::OpenDoorSound;
			break;
		case SoundMessage::CloseDoor:
			mConnections[clientIndex].pendingPacketType = ServerPacketType::CloseDoorSound;
			break;
		case SoundMessage::BlueSuitDead:
			mConnections[clientIndex].pendingPacketType = ServerPacketType::BlueSuitDead;
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
	if (clientIndex == -1)
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
	WCHAR clientListEntry[256];
	WCHAR clientIpAddress[16];
	ConvertCharToWideString(mConnections[clientIndex].ipAddress, clientIpAddress, 16);
	wsprintf(clientListEntry, L"CLIENT[%d], IP: %s, 포트 번호: %d\n", clientIndex, clientIpAddress, ntohs(mConnections[clientIndex].clientAddress.sin_port));
	SendMessage(mClientListBox, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(clientListEntry));

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
	if (clientIndex < 0)
	{
		return;
	}

	std::shared_ptr<CServerPlayer> player = mPlayers[clientIndex];

	if (!mConnections[clientIndex].hasReceivePacketType)
	{
		const ReceiveResult result = ReceiveData(clientIndex, sizeof(INT8));
		if (!HandleReceiveResult(result, socket))
		{
			return;
		}

		INT8 rawPacketType = -1;
		memcpy(
			&rawPacketType,
			mConnections[clientIndex].receiveBuffer.data(),
			sizeof(rawPacketType));
		std::fill(
			mConnections[clientIndex].receiveBuffer.begin(),
			mConnections[clientIndex].receiveBuffer.end(),
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

		mConnections[clientIndex].receivePacketType = static_cast<ClientPacketType>(rawPacketType);
		mConnections[clientIndex].hasReceivePacketType = true;
	}

	bool shouldEnqueuePendingPacket = false;
	switch (mConnections[clientIndex].receivePacketType)
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
	ClientConnectionState& socketInfo = mConnections[clientIndex];
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
	ClientConnectionState& socketInfo = mConnections[clientIndex];

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
	for (int i = 0; i < MAX_CLIENT; ++i)
	{
		if (!mConnections[i].isUsed)
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

		AssignUniquePlayerSpawnPosition(mPlayers[i], i);
		mCollisionManager->AddCollisionPlayer(mPlayers[i], i);
		mConnections[i].pendingPacketType = ServerPacketType::GameStart;

		if (i != clientIndex)
		{
			EnqueuePendingPacket(i);
		}
	}
}

bool TCPServer::TryHandleChangeSlotPacket(SOCKET socket, int& clientIndex)
{
	if (!HandleReceiveResult(ReceiveData(clientIndex, sizeof(INT8)), socket))
	{
		return false;
	}

	INT8 selectedSlot = -1;
	memcpy(&selectedSlot, mConnections[clientIndex].receiveBuffer.data(), sizeof(selectedSlot));
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
		mConnections[selectedSlot] = std::move(mConnections[clientIndex]);
		mConnections[clientIndex] = ClientConnectionState{};

		mPlayerReplicationStates[selectedSlot].clientId = selectedSlot;
		mPlayerReplicationStates[clientIndex].clientId = -1;
		mPlayers[clientIndex]->SetPlayerId(-1);
	}
	else
	{
		// 역할 슬롯을 교환해도 각 소켓의 수신 상태는 해당 소켓과 함께 이동해야 한다.
		std::swap(mConnections[clientIndex], mConnections[selectedSlot]);
		mPlayerReplicationStates[selectedSlot].clientId = selectedSlot;
	}

	mConnections[selectedSlot].pendingPacketType = ServerPacketType::ChangeSlot;
	// 이후 통계, 수신 상태 초기화, 응답 전송도 이동한 소켓 슬롯을 대상으로 해야 한다.
	clientIndex = selectedSlot;
	return true;
}

bool TCPServer::TryHandleClientInputPacket(
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
	auto readValue = [&socketInfo = mConnections[clientIndex], &readOffset](auto& value)
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
	mPlayerReplicationStates[clientIndex].pitch = input.pitch;
	player->SetRightClick(input.rightClick != 0);
	return true;
}

bool TCPServer::HandleLoadingCompletePacket(int clientIndex)
{
	if (mCanReplicateState)
	{
		return false;
	}

	mConnections[clientIndex].isLoadingComplete = true;
	if (!AreAllClientsLoadingComplete())
	{
		return false;
	}

	mCanReplicateState = true;
	const auto currentTime = std::chrono::steady_clock::now();
	mNextStateReplicationTime = currentTime;
	mNextNearbyObjectReplicationTime = currentTime;
	mConnections[clientIndex].pendingPacketType = ServerPacketType::LoadingComplete;
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
	if (mClientCount >= MAX_CLIENT)
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
	for (int i = 0; i < mClientCount + 1; ++i)
	{
		if (mConnections[i].isUsed)
		{
			continue;
		}
		mClientCount++;

		// 클라이언트 정보 초기화
		mPlayerReplicationStates[i].clientId = i;
		mConnections[i] = std::move(socketInfo);
		clientIndex = i;
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
	if (clientIndex < 0)
	{
		return false;
	}

	// ClientConnectionState가 초기화되기 전에 이 연결의 누적 측정값을 남긴다.
	mNetworkStatisticsReporter.ReportDisconnected(
		clientIndex,
		mConnections[clientIndex].networkStatistics);

	INT8 listBoxIndex = -1;
	for (INT8 i = 0; i <= clientIndex; ++i)
	{
		if (mConnections[i].isUsed)
		{
			++listBoxIndex;
		}
	}

	const bool hadPlayer = (mPlayers[clientIndex] != nullptr);
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

	mPlayers[clientIndex].reset();
	mPlayerStartPositionIndices[clientIndex] = -1;
	mPlayerReplicationStates[clientIndex] = PlayerReplicationState{};
	mConnections[clientIndex] = ClientConnectionState{};
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
		for (int index = 0; index < static_cast<int>(mConnections.size()); ++index)
		{
			if (!mConnections[index].isUsed)
			{
				continue;
			}

			mCanReplicateState = true;
			const auto currentTime = std::chrono::steady_clock::now();
			mNextStateReplicationTime = currentTime;
			mNextNearbyObjectReplicationTime = currentTime;
			mConnections[index].pendingPacketType = ServerPacketType::LoadingComplete;
			EnqueuePendingPacket(index);
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

	ClientConnectionState& socketInfo = mConnections[clientIndex];
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
	ClientConnectionState& socketInfo = mConnections[clientIndex];
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
	if (clientIndex < 0 || clientIndex >= static_cast<int>(mConnections.size()) ||
		!mConnections[clientIndex].isUsed)
	{
		return;
	}

	switch (mConnections[clientIndex].pendingPacketType)
	{
	case ServerPacketType::GameStart:
		if (EnqueuePacketFields(clientIndex, static_cast<INT8>(ServerPacketType::GameStart)))
		{
			mConnections[clientIndex].pendingPacketType = ServerPacketType::PlayerState;
		}
		break;
	case ServerPacketType::ChangeSlot:
		for (int i = 0; i < MAX_CLIENT; ++i)
		{
			if (!mConnections[i].isUsed)
			{
				continue;
			}

			EnqueuePacketFields(
				i,
				static_cast<INT8>(ServerPacketType::ChangeSlot),
				mPlayerReplicationStates[i].clientId,
				mPlayerReplicationStates);
		}
		if (mConnections[clientIndex].isUsed)
		{
			mConnections[clientIndex].pendingPacketType = ServerPacketType::PlayerState;
		}
		break;
	case ServerPacketType::Init:
		if (EnqueuePacketFields(
			clientIndex,
			static_cast<INT8>(ServerPacketType::Init),
			mPlayerReplicationStates[clientIndex].clientId,
			mClientCount,
			mPlayerReplicationStates))
		{
			mConnections[clientIndex].pendingPacketType = ServerPacketType::PlayerState;
		}
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
		if (EnqueuePacketFields(
			clientIndex,
			static_cast<INT8>(ServerPacketType::ClientCount),
			mClientCount,
			mPlayerReplicationStates))
		{
			mConnections[clientIndex].pendingPacketType = ServerPacketType::PlayerState;
		}
		break;
	case ServerPacketType::BlueSuitWin:
		if (EnqueuePacketFields(clientIndex, static_cast<INT8>(ServerPacketType::BlueSuitWin)))
		{
			mConnections[clientIndex].pendingPacketType = ServerPacketType::PlayerState;
		}
		break;
	case ServerPacketType::ZombieWin:
		if (EnqueuePacketFields(clientIndex, static_cast<INT8>(ServerPacketType::ZombieWin)))
		{
			mConnections[clientIndex].pendingPacketType = ServerPacketType::PlayerState;
		}
		break;
	case ServerPacketType::OpenDrawerSound:
		if (EnqueuePacketFields(clientIndex, static_cast<INT8>(ServerPacketType::OpenDrawerSound)))
		{
			mConnections[clientIndex].pendingPacketType = ServerPacketType::PlayerState;
		}
		break;
	case ServerPacketType::CloseDrawerSound:
		if (EnqueuePacketFields(clientIndex, static_cast<INT8>(ServerPacketType::CloseDrawerSound)))
		{
			mConnections[clientIndex].pendingPacketType = ServerPacketType::PlayerState;
		}
		break;
	case ServerPacketType::OpenDoorSound:
		if (EnqueuePacketFields(clientIndex, static_cast<INT8>(ServerPacketType::OpenDoorSound)))
		{
			mConnections[clientIndex].pendingPacketType = ServerPacketType::PlayerState;
		}
		break;
	case ServerPacketType::CloseDoorSound:
		if (EnqueuePacketFields(clientIndex, static_cast<INT8>(ServerPacketType::CloseDoorSound)))
		{
			mConnections[clientIndex].pendingPacketType = ServerPacketType::PlayerState;
		}
		break;
	case ServerPacketType::BlueSuitDead:
	{
		const char deadUserId = static_cast<char>(clientIndex);
		for (int i = 0; i < MAX_CLIENT; ++i)
		{
			if (mConnections[i].isUsed)
			{
				EnqueuePacketFields(i, static_cast<INT8>(ServerPacketType::BlueSuitDead), deadUserId);
			}
		}
		if (mConnections[clientIndex].isUsed)
		{
			mConnections[clientIndex].pendingPacketType = ServerPacketType::PlayerState;
		}
		break;
	}
	case ServerPacketType::LoadingComplete:
	{
		for (int i = 0; i < MAX_CLIENT; ++i)
		{
			if (mConnections[i].isUsed &&
				EnqueuePacketFields(i, static_cast<INT8>(ServerPacketType::LoadingComplete)) &&
				mPlayers[i])
			{
				mPlayers[i]->GameStartLogic();
			}
		}
		BroadcastOpenableObjectSnapshot();
		if (mConnections[clientIndex].isUsed)
		{
			mConnections[clientIndex].pendingPacketType = ServerPacketType::PlayerState;
		}
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
		for (int i = 1; i < MAX_CLIENT; ++i)
		{
			if (!mPlayers[i] || mPlayers[i]->GetPlayerId() == -1)
			{
				continue;
			}

			if (mPlayers[i]->IsAlive())
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
	for (int clientIndex = 0; clientIndex < static_cast<int>(mConnections.size()); ++clientIndex)
	{
		ClientConnectionState& socketInfo = mConnections[clientIndex];
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
		EnqueuePendingPacket(clientIndex);
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
		auto& playerState = mPlayerReplicationStates[playerId];
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
	for (int clientIndex = 0; clientIndex < static_cast<int>(mConnections.size()); ++clientIndex)
	{
		const ClientConnectionState& socketInfo = mConnections[clientIndex];
		if (!socketInfo.isUsed || socketInfo.pendingPacketType != ServerPacketType::PlayerState)
		{
			continue;
		}

		EnqueuePendingPacket(clientIndex);
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

	for (int clientIndex = 0; clientIndex < static_cast<int>(mConnections.size()); ++clientIndex)
	{
		if (!mConnections[clientIndex].isUsed)
		{
			continue;
		}

		EnqueuePacketFields(
			clientIndex,
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

	for (int clientIndex = 0; clientIndex < static_cast<int>(mConnections.size()); ++clientIndex)
	{
		if (mConnections[clientIndex].isUsed)
		{
			EnqueuePacketBuffer(clientIndex, packetBuffer);
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
	for (int clientIndex = 0; clientIndex < static_cast<int>(mConnections.size()); ++clientIndex)
	{
		if (!mConnections[clientIndex].isUsed)
		{
			continue;
		}

		const auto& objectSnapshot = mNearbyObjectSnapshots[clientIndex];
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
		EnqueuePacketBuffer(clientIndex, std::move(packetBuffer));
	}
}

// World initialization
void TCPServer::LoadServerScene()
{
	FILE* inputFile = NULL;
	::fopen_s(&inputFile, (char*)"ServerScene.bin", "rb");
	::rewind(inputFile);
	int reachedSceneEnd{};
	int readCount;
	while (true)
	{
		char token[128] = { '\0' };
		for (; ; )
		{
			if (::ReadStringFromFile(inputFile, token))
			{
				if (!strcmp(token, "<Hierarchy>:"))
				{
					char frameName[64];
					int childCount, boxColliderCount;
					XMFLOAT3 aabbCenter, aabbExtents;
					std::vector<BoundingOrientedBox> boundingBoxes;
					for (;;)
					{
						if (::ReadStringFromFile(inputFile, token))
						{
							if (!strcmp(token, "<Frame>:"))
							{
								::ReadIntegerFromFile(inputFile);
								::ReadStringFromFile(inputFile, frameName);
								//std::cout << frameName << endl;
							}
							else if (!strcmp(token, "<Children>:"))
							{
								childCount = ::ReadIntegerFromFile(inputFile);
							}
							else if (!strcmp(token, "<BoxColliders>:"))
							{
								boxColliderCount = ::ReadIntegerFromFile(inputFile);
								boundingBoxes.reserve(boxColliderCount);
								for (int i = 0; i < boxColliderCount; ++i)
								{
									::ReadStringFromFile(inputFile, token);	// <Bound>
									int colliderIndex = 0;
									readCount = fread(&colliderIndex, sizeof(int), 1, inputFile);
									readCount = (UINT)::fread(&aabbCenter, sizeof(XMFLOAT3), 1, inputFile);
									readCount = (UINT)::fread(&aabbExtents, sizeof(XMFLOAT3), 1, inputFile);
									XMFLOAT4 orientation;
									XMStoreFloat4(&orientation, XMQuaternionIdentity());
									boundingBoxes.emplace_back(aabbCenter, aabbExtents, orientation);
								}
							}
							else if (!strcmp(token, "<Matrix>:"))
							{
								childCount = ::ReadIntegerFromFile(inputFile);
								XMFLOAT4X4* worldMatrices = new XMFLOAT4X4[childCount];
								readCount = (UINT)::fread(worldMatrices, sizeof(XMFLOAT4X4), childCount, inputFile);
								for (int i = 0; i < childCount; ++i)
								{
									// 오브젝트 생성
									CreateObjectFromSceneFrame(frameName, Matrix4x4::Transpose(worldMatrices[i]), boundingBoxes);
								}
								delete[] worldMatrices;
							}
							else if (!strcmp(token, "</Frame>"))
							{
								break;
							}
						}
					}
				}
				else if (!strcmp(token, "</Hierarchy>"))
				{
					break;
				}
				else if (!strcmp(token, "</Scene>:"))
				{
					reachedSceneEnd = 1;
					break;
				}
			}
			else
			{
				break;
			}
		}
		if (reachedSceneEnd)
		{
			break;
		}
	}


}

void TCPServer::CreateObjectFromSceneFrame(char* frameName, const XMFLOAT4X4& world, const vector<BoundingOrientedBox>& boundingBoxes)
{
	static int serverObjectId = 0;
	shared_ptr<CServerGameObject> gameObject;

	if (!strcmp(frameName, "Door_1"))
	{
		gameObject = make_shared<CServerDoorObject>(frameName, world, boundingBoxes);
	}
	else if (!strcmp(frameName, "Drawer_1"))
	{
		mDrawerEntries.push_back(pair<int, int>(serverObjectId, 1));
		gameObject = make_shared<CServerDrawerObject>(frameName, world, boundingBoxes);
	}
	else if (!strcmp(frameName, "Drawer_2"))
	{
		mDrawerEntries.push_back(pair<int, int>(serverObjectId, 2));
		gameObject = make_shared<CServerDrawerObject>(frameName, world, boundingBoxes);
	}
	else if (!strcmp(frameName, "Door1"))
	{
		gameObject = make_shared<CServerElevatorDoorObject>(frameName, world, boundingBoxes);
	}
	else if (!strcmp(frameName, "Emergency_Handle"))
	{
		gameObject = make_shared<CServerElevatorDoorObject>(frameName, world, boundingBoxes);
	}
	else if (!strcmp(frameName, "Laboratory_Wall_1_Corner_1") || !strcmp(frameName, "Laboratory_Wall_1_Corner_2"))
	{
		gameObject = make_shared<CServerEnvironmentObject>(frameName, world, boundingBoxes);
	}
	else if (!strcmp(frameName, "BoxCollide_Wall"))
	{
		gameObject = make_shared<CServerEnvironmentObject>(frameName, world, boundingBoxes);
	}
	else if (!strcmp(frameName, "Laboratory_Wall_1_Corner") || !strcmp(frameName, "Laboratory_Wall_1_Corner2") || !strcmp(frameName, "Laboratory_Wall_1"))
	{
		gameObject = make_shared<CServerEnvironmentObject>(frameName, world, boundingBoxes);
	}
	else if (!strcmp(frameName, "Laboratory_Wall_Door_1") || !strcmp(frameName, "Laboratory_Wall_Door_1_2"))
	{
		gameObject = make_shared<CServerEnvironmentObject>(frameName, world, boundingBoxes);
	}
	else if (!strcmp(frameName, "Biological_Capsule_1") || !strcmp(frameName, "Laboratory_Table_1") || !strcmp(frameName, "Laboratory_Stool_1"))
	{
		gameObject = make_shared<CServerEnvironmentObject>(frameName, world, boundingBoxes);
	}
	else if (!strcmp(frameName, "Laboratory_Tunnel_1_Stairs") || !strcmp(frameName, "Laboratory_Tunnel_1") || !strcmp(frameName, "Laboratory_Desk_Drawers_1"))
	{
		gameObject = make_shared<CServerEnvironmentObject>(frameName, world, boundingBoxes);
	}
	else if (!strcmp(frameName, "SM_Prop_Vents_Straight_01") || !strcmp(frameName, "SM_Prop_Crate_01")
		|| !strcmp(frameName, "SM_Prop_Pipe_Curve_02") || !strcmp(frameName, "SM_Prop_Billboard_Roof_01")
		|| !strcmp(frameName, "SM_Prop_Roof_Aircon_03") || !strcmp(frameName, "SM_Prop_Vents_End_01")
		|| !strcmp(frameName, "SM_Prop_ShopInterior_Table_01") || !strcmp(frameName, "SM_Prop_Couch_01")
		|| !strcmp(frameName, "SM_Prop_PotPlant_02") || !strcmp(frameName, "Table1of10"))
	{
		gameObject = make_shared<CServerEnvironmentObject>(frameName, world, boundingBoxes);
	}
	else if (!strcmp(frameName, "BoxCollider_Stair_Start"))
	{
		gameObject = make_shared<CServerStairTriggerObject>(frameName, world, boundingBoxes);
	}
	else
	{
		gameObject = make_shared<CServerGameObject>(frameName, world, boundingBoxes);
		gameObject->SetStatic(true);
	}

	strcpy(gameObject->m_pstrFrameName, frameName);

	serverObjectId++;
	mCollisionManager->AddCollisionObject(gameObject);
}

void TCPServer::PopulateSceneItems()
{
	// 확률: fus 30, mine 30, tp 30, radar 10
	uniform_int_distribution<int> drawerDistribution(0, mDrawerEntries.size() - 1); //[CJI 0525] mDrawerEntries 에 번호를 저장하는 방식으로 변경하여 랜덤으로 뽑아 사용
	uniform_int_distribution<int> itemTypeDistribution(0, 99);
	uniform_int_distribution<int> rotationDistribution(1, 360);
	uniform_real_distribution<float> offsetDistribution(-0.2f, 0.2f);
	CServerItemObject::SetDrawerIdContainer(mDrawerEntries);

	for (int i = 0; i < ITEM_COUNT; ++i)
	{
		int drawerEntryIndex = drawerDistribution(mRandomEngine);
		int drawerObjectId = mDrawerEntries[drawerEntryIndex].first;
		shared_ptr<CServerDrawerObject> drawerObject = dynamic_pointer_cast<CServerDrawerObject>(mCollisionManager->GetCollisionObjectWithNumber(drawerObjectId));
		if (!drawerObject) //error
			assert(0);
		//exit(1);

		if (drawerObject->m_pStoredItem)	// 이미 다른 아이템이 들어왔음
		{
			--i;
			continue;
		}
		XMFLOAT4X4 drawerWorld = mCollisionManager->GetCollisionObjectWithNumber(drawerObjectId)->GetWorldMatrix();

		int itemTypeRoll = itemTypeDistribution(mRandomEngine);
		shared_ptr<CServerItemObject> itemObject;

		XMFLOAT3 randomOffset = XMFLOAT3(offsetDistribution(mRandomEngine), 0.0f, offsetDistribution(mRandomEngine));
		XMFLOAT3 randomRotation = XMFLOAT3(0.0f, 0.0f, (float)rotationDistribution(mRandomEngine));

		if (i < 9)		// Fuse
		{
			itemObject = make_shared<CServerFuseObject>();
			itemObject->SetDrawerNumber(drawerObjectId);
			itemObject->SetDrawer(drawerObject);
			itemObject->SetDrawerType(mDrawerEntries[drawerEntryIndex].second);
			drawerObject->m_pStoredItem = itemObject;

			itemObject->SetRandomRotation(randomRotation);
			itemObject->SetRandomOffset(randomOffset);

			itemObject->SetWorldMatrix(drawerWorld);
			mCollisionManager->AddCollisionObject(itemObject);
		}
		else if (i < 24)	// tp
		{
			itemObject = make_shared<CServerTeleportObject>();
			itemObject->SetDrawerNumber(drawerObjectId);
			itemObject->SetDrawer(drawerObject);
			itemObject->SetDrawerType(mDrawerEntries[drawerEntryIndex].second);
			drawerObject->m_pStoredItem = itemObject;

			itemObject->SetRandomRotation(randomRotation);
			itemObject->SetRandomOffset(randomOffset);
			itemObject->SetWorldMatrix(drawerWorld);
			mCollisionManager->AddCollisionObject(itemObject);
		}
		else if (i < 26)	// Rader
		{
			randomRotation = XMFLOAT3(90.0f, 0.0f, 0.0f);
			itemObject = make_shared<CServerRadarObject>();
			itemObject->SetDrawerNumber(drawerObjectId);
			itemObject->SetDrawer(drawerObject);
			itemObject->SetDrawerType(mDrawerEntries[drawerEntryIndex].second);
			drawerObject->m_pStoredItem = itemObject;

			itemObject->SetRandomRotation(randomRotation);
			itemObject->SetRandomOffset(randomOffset);
			itemObject->SetWorldMatrix(drawerWorld);
			mCollisionManager->AddCollisionObject(itemObject);
		}
		else if (i < 76)	// Mine
		{
			randomRotation = XMFLOAT3(90.0f, 0.0f, 0.0f);

			itemObject = make_shared<CServerMineObject>();
			itemObject->SetDrawerNumber(drawerObjectId);
			itemObject->SetDrawer(drawerObject);
			itemObject->SetDrawerType(mDrawerEntries[drawerEntryIndex].second);
			drawerObject->m_pStoredItem = itemObject;

			itemObject->SetRandomRotation(randomRotation);
			itemObject->SetRandomOffset(randomOffset);
			itemObject->SetWorldMatrix(drawerWorld);
			mCollisionManager->AddCollisionObject(itemObject);
		}

	}
}

void TCPServer::AssignUniquePlayerSpawnPosition(shared_ptr<CServerPlayer>& serverPlayer, int index)
{
	// 후보지를 두고 int 값에 따라 그곳에 가도록 해야할듯
	uniform_int_distribution<int> spawnPositionDistribution(0, mPlayerStartPositions.size() - 1);

	int spawnPositionIndex = spawnPositionDistribution(mRandomEngine);
	bool isAvailable = false;
	while (!isAvailable)
	{
		isAvailable = true;
		spawnPositionIndex = spawnPositionDistribution(mRandomEngine);
		for (const auto& assignedSpawnIndex : mPlayerStartPositionIndices)
		{
			if (assignedSpawnIndex == spawnPositionIndex)
			{
				isAvailable = false;
				break;
			}
		}
	}

	mPlayerStartPositionIndices[index] = spawnPositionIndex;
	XMFLOAT3 spawnPosition = mPlayerStartPositions[spawnPositionIndex];
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
