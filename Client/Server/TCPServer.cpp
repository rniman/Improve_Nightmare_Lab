#include "stdafx.h"
#include "TCPServer.h"
#include "ServerObject.h"
#include "ServerEnvironmentObject.h"
#include "ServerPlayer.h"
#include "ServerCollision.h"

namespace
{
	constexpr size_t MAX_PENDING_SEND_BYTES = 4 * 1024 * 1024;
}

default_random_engine TCPServer::m_mt19937Gen;
HWND TCPServer::m_hWnd;
INT8 TCPServer::m_nClient = 0;

void ConvertCharToLPWSTR(const char* pstr, LPWSTR dest, int destSize)
{
	// MultiByteToWideChar 함수를 사용하여 char*을 LPWSTR로 변환
	MultiByteToWideChar(
		CP_UTF8,
		0,                   // 변환 옵션
		pstr,                 // 변환할 문자열
		-1,                  // 자동으로 문자열 길이 계산
		dest,                // 대상 버퍼
		destSize             // 대상 버퍼의 크기
	);
}

TCPServer::TCPServer()
{
	m_axmf3Positions = {
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

	m_anPlayerStartPosNum = { -1, -1, -1, -1, -1 };
}

TCPServer::~TCPServer()
{}

void TCPServer::OnProcessingWindowMessage(HWND hWnd, UINT nMessageID, WPARAM wParam, LPARAM lParam)
{
	switch (nMessageID)
	{
	case WM_CREATE:
		m_timer.Start();
		break;
	case WM_SOUND:
	{
		const int nSocketIndex = static_cast<int>(lParam);
		if (nSocketIndex < 0 || nSocketIndex >= static_cast<int>(m_vSocketInfoList.size()) ||
			!m_vSocketInfoList[nSocketIndex].m_bUsed)
		{
			break;
		}

		switch (wParam)
		{
		case SOUND_MESSAGE::OPEN_DRAWER:
			m_vSocketInfoList[nSocketIndex].m_socketState = SOCKET_STATE::SEND_OPEN_DRAWER_SOUND;
			break;
		case SOUND_MESSAGE::CLOSE_DRAWER:
			m_vSocketInfoList[nSocketIndex].m_socketState = SOCKET_STATE::SEND_CLOSE_DRAWER_SOUND;
			break;
		case SOUND_MESSAGE::OPEN_DOOR:
			m_vSocketInfoList[nSocketIndex].m_socketState = SOCKET_STATE::SEND_OPEN_DOOR_SOUND;
			break;
		case SOUND_MESSAGE::CLOSE_DOOR:
			m_vSocketInfoList[nSocketIndex].m_socketState = SOCKET_STATE::SEND_CLOSE_DOOR_SOUND;
			break;
		case SOUND_MESSAGE::BLUE_SUIT_DEAD:
			m_vSocketInfoList[nSocketIndex].m_socketState = SOCKET_STATE::SEND_BLUE_SUIT_DEAD;
			break;
		default:
			return;
		}
		RequestSend(nSocketIndex);
		break;
	}
	default:
		break;
	}
}

void TCPServer::OnProcessingSocketMessage(HWND hWnd, UINT nMessageID, WPARAM wParam, LPARAM lParam)
{
	const int nSocketEvent = WSAGETSELECTEVENT(lParam);
	const int nSocketError = WSAGETSELECTERROR(lParam);
	if (nSocketError != 0)
	{
		err_display(nSocketError);
		if (nSocketEvent != FD_ACCEPT)
		{
			DisconnectClient(static_cast<SOCKET>(wParam));
		}
		return;
	}

	switch (nSocketEvent)
	{
	case FD_ACCEPT:
		OnProcessingAcceptMessage(hWnd, nMessageID, wParam, lParam);
		break;
	case FD_READ:
		OnProcessingReadMessage(hWnd, nMessageID, wParam, lParam);
		break;
	case FD_WRITE:
		OnProcessingWriteMessage(hWnd, nMessageID, wParam, lParam);
		break;
	case FD_CLOSE:
		OnProcessingCloseMessage(hWnd, nMessageID, wParam, lParam);
		break;
	default:
		break;
	}

	return;
}

void TCPServer::OnProcessingAcceptMessage(HWND hWnd, UINT nMessageID, WPARAM wParam, LPARAM lParam)
{
	struct sockaddr_in addrClient;
	int nAddrlen = sizeof(sockaddr_in);
	const SOCKET sockClient = accept(static_cast<SOCKET>(wParam), reinterpret_cast<struct sockaddr*>(&addrClient), &nAddrlen);

	if (sockClient == INVALID_SOCKET)
	{
		err_display("accept()");
		return;
	}

	if (m_nGameState == GAME_STATE::IN_GAME)
	{
		closesocket(sockClient);
		err_display("Game that has already started.");
		return;
	}

	const INT8 nSocketIndex = AddSocketInfo(sockClient, addrClient, nAddrlen);

	// MAX_CLIENT보다 더 많은 접속 요구
	if (nSocketIndex == -1)
	{
		closesocket(sockClient); // 클라이언트 소켓 종료
		err_display("Maximum number of clients reached. Connection refused."); // 연결 거부 메시지 표시
		return;
	}

	const int retval = WSAAsyncSelect(sockClient, hWnd, WM_SOCKET, FD_READ | FD_WRITE | FD_CLOSE);
	if (retval == SOCKET_ERROR)
	{
		err_display("WSAAsyncSelect()");
		DisconnectClient(sockClient);
		return;
	}
	WCHAR pszList[256];
	WCHAR pszIP[16];
	ConvertCharToLPWSTR(m_vSocketInfoList[nSocketIndex].m_pAddr, pszIP, 16);
	wsprintf(pszList, L"CLIENT[%d], IP: %s, 포트 번호: %d\n", nSocketIndex, pszIP, ntohs(m_vSocketInfoList[nSocketIndex].m_addrClient.sin_port));
	SendMessage(m_hClientListBox, LB_ADDSTRING, 0, (LPARAM)pszList);

	if (nSocketIndex == ZOMBIEPLAYER) // ZombiePlayer는 0번 소켓에만 생성
	{
		m_apPlayers[nSocketIndex] = make_shared<CServerZombiePlayer>();
		m_apPlayers[nSocketIndex]->SetPlayerId(nSocketIndex);
		++m_nZombie;
	}
	else
	{
		m_apPlayers[nSocketIndex] = make_shared<CServerBlueSuitPlayer>();
		m_apPlayers[nSocketIndex]->SetPlayerId(nSocketIndex);
		++m_nBlueSuit;
	}

	m_pCollisionManager->AddCollisionPlayer(m_apPlayers[nSocketIndex], nSocketIndex);
	RequestSend(nSocketIndex);

	for (auto& sockInfo : m_vSocketInfoList)
	{
		if (!sockInfo.m_bUsed || sockInfo.m_sock == sockClient)
		{
			continue;
		}
		sockInfo.m_socketState = SOCKET_STATE::SEND_NUM_OF_CLIENT;
		RequestSend(GetSocketIndex(sockInfo.m_sock));
	}

	return;
}

void TCPServer::OnProcessingReadMessage(HWND hWnd, UINT nMessageID, WPARAM wParam, LPARAM lParam)
{
	const SOCKET socket = static_cast<SOCKET>(wParam);
	int nSocketIndex = GetSocketIndex(socket);
	if (nSocketIndex < 0)
	{
		return;
	}

	auto handleReceiveResult = [this, socket](ReceiveResult result)
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
		};

	std::shared_ptr<CServerPlayer> pPlayer = m_apPlayers[nSocketIndex];

	if (!m_vSocketInfoList[nSocketIndex].m_bRecvHead)
	{
		const ReceiveResult result = RecvData(nSocketIndex, sizeof(INT8));
		if (!handleReceiveResult(result))
		{
			return;
		}

		m_vSocketInfoList[nSocketIndex].m_bRecvHead = true;
		memcpy(&m_vSocketInfoList[nSocketIndex].m_nHead, m_vSocketInfoList[nSocketIndex].m_pCurrentBuffer, sizeof(INT8));
		memset(m_vSocketInfoList[nSocketIndex].m_pCurrentBuffer, 0, MAX_PACKET_PAYLOAD_SIZE);
	}

	switch (m_vSocketInfoList[nSocketIndex].m_nHead)
	{
	case HEAD_GAME_START:
	{
		m_nGameState = GAME_STATE::IN_GAME;
		m_nZombie = 0;
		m_nBlueSuit = 0;
		for (int i = 0; i < MAX_CLIENT; ++i)
		{
			if (!m_vSocketInfoList[i].m_bUsed)
			{
				continue;
			}

			if (i == 0)
			{
				m_nZombie++;
			}
			else
			{
				m_nBlueSuit++;
			}

			InitPlayerPosition(m_apPlayers[i], i);
			m_pCollisionManager->AddCollisionPlayer(m_apPlayers[i], i);

			m_vSocketInfoList[i].m_socketState = SOCKET_STATE::SEND_GAME_START;

			if (i == nSocketIndex)
			{
				continue;
			}
			RequestSend(i);
		}
		break;
	}
	case HEAD_CHANGE_SLOT:
	{
		if (!handleReceiveResult(RecvData(nSocketIndex, sizeof(INT8))))
		{
			return;
		}

		INT8 nSelectedSlot;
		memcpy(&nSelectedSlot, m_vSocketInfoList[nSocketIndex].m_pCurrentBuffer, sizeof(INT8));
		if (nSelectedSlot < 0 || nSelectedSlot >= static_cast<INT8>(MAX_CLIENT))
		{
			break;
		}

		if (!m_apPlayers[nSelectedSlot]) // 없으면 만들어서
		{
			m_apPlayers[nSelectedSlot] = make_shared<CServerBlueSuitPlayer>();
		}

		if (m_apPlayers[nSelectedSlot]->GetPlayerId() == -1)
		{
			m_apPlayers[nSelectedSlot]->SetPlayerId(nSelectedSlot);

			// 소켓과 수신 진행 상태는 하나의 단위이므로 전체를 함께 이동한다.
			m_vSocketInfoList[nSelectedSlot] = m_vSocketInfoList[nSocketIndex];
			m_vSocketInfoList[nSocketIndex] = SOCKETINFO{};

			m_aUpdateInfo[nSelectedSlot].m_nClientId = nSelectedSlot;

			m_aUpdateInfo[nSocketIndex].m_nClientId = -1;
			m_apPlayers[nSocketIndex]->SetPlayerId(-1);
		}
		else // 교환해야함
		{
			// 역할 슬롯을 교환해도 각 소켓의 수신 상태는 해당 소켓과 함께 이동해야 한다.
			std::swap(m_vSocketInfoList[nSocketIndex], m_vSocketInfoList[nSelectedSlot]);

			m_aUpdateInfo[nSelectedSlot].m_nClientId = nSelectedSlot;
		}

		m_vSocketInfoList[nSelectedSlot].m_socketState = SOCKET_STATE::SEND_CHANGE_SLOT;
		nSocketIndex = nSelectedSlot;
		break;
	}
	case HEAD_KEYS_BUFFER:
	{
		// Time, KeysBuffer(WORD), viewMatrix, vecLook, vecRight
		const size_t bufferSize = sizeof(__int64) + sizeof(WORD) + sizeof(XMFLOAT4X4) + sizeof(XMFLOAT3) * 3 + sizeof(SC_ANIMATION_INFO) + sizeof(SC_PLAYER_INFO);
		if (!handleReceiveResult(RecvData(nSocketIndex, bufferSize)))
		{
			return;
		}
		m_vSocketInfoList[nSocketIndex].RecvNum++;

		if (!pPlayer->IsRecvData())
		{
			pPlayer->SetRecvData(true);
		}

		size_t sizeOffset = 0;
		sizeOffset += sizeof(__int64);

		WORD wKeyBuffer = 0;
		memcpy(&wKeyBuffer, m_vSocketInfoList[nSocketIndex].m_pCurrentBuffer + sizeOffset, sizeof(WORD));
		pPlayer->SetKeyBuffer(wKeyBuffer);

		sizeOffset += sizeof(WORD);

		XMFLOAT4X4 xmf4x4View;
		memcpy(&xmf4x4View, m_vSocketInfoList[nSocketIndex].m_pCurrentBuffer + sizeOffset, sizeof(XMFLOAT4X4));
		pPlayer->SetViewMatrix(xmf4x4View);
		sizeOffset += sizeof(XMFLOAT4X4);

		XMFLOAT3 xmf3Look, xmf3Right, xmf3Up;
		memcpy(&xmf3Look, m_vSocketInfoList[nSocketIndex].m_pCurrentBuffer + sizeOffset, sizeof(XMFLOAT3));
		sizeOffset += sizeof(XMFLOAT3);
		memcpy(&xmf3Right, m_vSocketInfoList[nSocketIndex].m_pCurrentBuffer + sizeOffset, sizeof(XMFLOAT3));
		sizeOffset += sizeof(XMFLOAT3);
		memcpy(&xmf3Up, m_vSocketInfoList[nSocketIndex].m_pCurrentBuffer + sizeOffset, sizeof(XMFLOAT3));
		sizeOffset += sizeof(XMFLOAT3);

		pPlayer->SetLook(xmf3Look);
		pPlayer->SetRight(xmf3Right);
		pPlayer->SetUp(xmf3Up);

		SC_ANIMATION_INFO animationInfo;
		memcpy(&animationInfo, m_vSocketInfoList[nSocketIndex].m_pCurrentBuffer + sizeOffset, sizeof(SC_ANIMATION_INFO));
		m_aUpdateInfo[nSocketIndex].m_animationInfo = animationInfo;
		sizeOffset += sizeof(SC_ANIMATION_INFO);

		SC_PLAYER_INFO playerInfo;
		memcpy(&playerInfo, m_vSocketInfoList[nSocketIndex].m_pCurrentBuffer + sizeOffset, sizeof(SC_PLAYER_INFO));
		pPlayer->SetRightClick(playerInfo.m_bRightClick);

		break;
	}
	case HEAD_LOADING_COMPLETE:
	{
		m_vSocketInfoList[nSocketIndex].m_bLoadComplete = true;
		int connectCount = 0;
		int loadCompleteCount = 0;
		for (const auto& socketInfo : m_vSocketInfoList)
		{
			if (!socketInfo.m_bUsed)
			{
				continue;
			}

			++connectCount;
			if (socketInfo.m_bLoadComplete)
			{
				++loadCompleteCount;
			}
		}

		if (loadCompleteCount == connectCount)
		{
			m_vSocketInfoList[nSocketIndex].m_socketState = SOCKET_STATE::SEND_LOADING_COMPLETE;
		}
		break;
	}
	default:
		break;
	}

	ResetReceiveState(m_vSocketInfoList[nSocketIndex]);
	RequestSend(nSocketIndex);
}

void TCPServer::OnProcessingWriteMessage(HWND hWnd, UINT nMessageID, WPARAM wParam, LPARAM lParam)
{
	const SOCKET socket = static_cast<SOCKET>(wParam);
	const int nSocketIndex = GetSocketIndex(socket);
	if (nSocketIndex < 0)
	{
		return;
	}

	if (FlushSendQueue(nSocketIndex) == SendResult::Error)
	{
		err_display("send()");
		DisconnectClient(socket);
	}
}

void TCPServer::RequestSend(int nSocketIndex)
{
	if (nSocketIndex < 0 || nSocketIndex >= static_cast<int>(m_vSocketInfoList.size()) ||
		!m_vSocketInfoList[nSocketIndex].m_bUsed)
	{
		return;
	}

	switch (m_vSocketInfoList[nSocketIndex].m_socketState)
	{
	case SOCKET_STATE::SEND_GAME_START:
		if (SubmitSendData(nSocketIndex, static_cast<INT8>(5)))
		{
			m_vSocketInfoList[nSocketIndex].m_socketState = SOCKET_STATE::SEND_UPDATE_DATA;
		}
		break;
	case SOCKET_STATE::SEND_CHANGE_SLOT:
		for (int i = 0; i < MAX_CLIENT; ++i)
		{
			if (!m_vSocketInfoList[i].m_bUsed)
			{
				continue;
			}

			SubmitSendData(i, static_cast<INT8>(6), m_aUpdateInfo[i].m_nClientId, m_aUpdateInfo);
		}
		if (m_vSocketInfoList[nSocketIndex].m_bUsed)
		{
			m_vSocketInfoList[nSocketIndex].m_socketState = SOCKET_STATE::SEND_UPDATE_DATA;
		}
		break;
	case SOCKET_STATE::SEND_ID:
		if (SubmitSendData(
			nSocketIndex,
			static_cast<INT8>(0),
			m_aUpdateInfo[nSocketIndex].m_nClientId,
			m_nClient,
			m_aUpdateInfo))
		{
			m_vSocketInfoList[nSocketIndex].SendNum++;
			m_vSocketInfoList[nSocketIndex].m_socketState = SOCKET_STATE::SEND_UPDATE_DATA;
		}
		break;
	case SOCKET_STATE::SEND_UPDATE_DATA:
		if (m_nGameState == GAME_STATE::IN_LOBBY)
		{
			break;
		}
		if (SubmitSendData(nSocketIndex, static_cast<INT8>(1), m_aUpdateInfo))
		{
			m_vSocketInfoList[nSocketIndex].SendNum++;
			m_bDataSend[nSocketIndex] = true;
		}
		break;
	case SOCKET_STATE::SEND_NUM_OF_CLIENT:
		if (SubmitSendData(nSocketIndex, static_cast<INT8>(2), m_nClient, m_aUpdateInfo))
		{
			m_vSocketInfoList[nSocketIndex].m_socketState = SOCKET_STATE::SEND_UPDATE_DATA;
		}
		break;
	case SOCKET_STATE::SEND_BLUE_SUIT_WIN:
		if (SubmitSendData(nSocketIndex, static_cast<INT8>(3)))
		{
			m_vSocketInfoList[nSocketIndex].m_socketState = SOCKET_STATE::SEND_UPDATE_DATA;
		}
		break;
	case SOCKET_STATE::SEND_ZOMBIE_WIN:
		if (SubmitSendData(nSocketIndex, static_cast<INT8>(4)))
		{
			m_vSocketInfoList[nSocketIndex].m_socketState = SOCKET_STATE::SEND_UPDATE_DATA;
		}
		break;
	case SOCKET_STATE::SEND_OPEN_DRAWER_SOUND:
		if (SubmitSendData(nSocketIndex, static_cast<INT8>(7)))
		{
			m_vSocketInfoList[nSocketIndex].m_socketState = SOCKET_STATE::SEND_UPDATE_DATA;
		}
		break;
	case SOCKET_STATE::SEND_CLOSE_DRAWER_SOUND:
		if (SubmitSendData(nSocketIndex, static_cast<INT8>(8)))
		{
			m_vSocketInfoList[nSocketIndex].m_socketState = SOCKET_STATE::SEND_UPDATE_DATA;
		}
		break;
	case SOCKET_STATE::SEND_OPEN_DOOR_SOUND:
		if (SubmitSendData(nSocketIndex, static_cast<INT8>(9)))
		{
			m_vSocketInfoList[nSocketIndex].m_socketState = SOCKET_STATE::SEND_UPDATE_DATA;
		}
		break;
	case SOCKET_STATE::SEND_CLOSE_DOOR_SOUND:
		if (SubmitSendData(nSocketIndex, static_cast<INT8>(10)))
		{
			m_vSocketInfoList[nSocketIndex].m_socketState = SOCKET_STATE::SEND_UPDATE_DATA;
		}
		break;
	case SOCKET_STATE::SEND_BLUE_SUIT_DEAD:
	{
		const char deadUserId = static_cast<char>(nSocketIndex);
		for (int i = 0; i < MAX_CLIENT; ++i)
		{
			if (m_vSocketInfoList[i].m_bUsed)
			{
				SubmitSendData(i, static_cast<INT8>(11), deadUserId);
			}
		}
		if (m_vSocketInfoList[nSocketIndex].m_bUsed)
		{
			m_vSocketInfoList[nSocketIndex].m_socketState = SOCKET_STATE::SEND_UPDATE_DATA;
		}
		break;
	}
	case SOCKET_STATE::SEND_LOADING_COMPLETE:
	{
		for (int i = 0; i < MAX_CLIENT; ++i)
		{
			if (m_vSocketInfoList[i].m_bUsed)
			{
				if (SubmitSendData(i, static_cast<INT8>(13)) && m_apPlayers[i])
				{
					m_apPlayers[i]->GameStartLogic();
				}
			}
		}
		if (m_vSocketInfoList[nSocketIndex].m_bUsed)
		{
			m_vSocketInfoList[nSocketIndex].m_socketState = SOCKET_STATE::SEND_UPDATE_DATA;
		}
		break;
	}
	default:
		break;
	}
}

void TCPServer::OnProcessingCloseMessage(HWND hWnd, UINT nMessageID, WPARAM wParam, LPARAM lParam)
{
	DisconnectClient(static_cast<SOCKET>(wParam));
}

bool TCPServer::Init(HWND hWnd)
{
	m_mt19937Gen = default_random_engine(random_device()());

	m_hWnd = hWnd;
	// 윈속 초기화
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
	{
		return false;
	}

	// 소켓 생성
	SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, 0);
	if (listen_sock == INVALID_SOCKET) err_quit("socket()");

	// bind()
	struct sockaddr_in serveraddr;
	memset(&serveraddr, 0, sizeof(serveraddr));
	serveraddr.sin_family = AF_INET;
	serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
	serveraddr.sin_port = htons(SERVERPORT);
	int retval = bind(listen_sock, (struct sockaddr*)&serveraddr, sizeof(serveraddr));
	if (retval == SOCKET_ERROR) err_quit("bind()");

	// listen()
	retval = listen(listen_sock, SOMAXCONN);
	if (retval == SOCKET_ERROR)
	{
		err_quit("listen()");
	}

	// WSAAsyncSelect()
	retval = WSAAsyncSelect(listen_sock, hWnd, WM_SOCKET, FD_ACCEPT | FD_CLOSE);
	if (retval == SOCKET_ERROR)
	{
		err_quit("WSAAsyncSelect()");
	}

	m_nGameState = GAME_STATE::IN_LOBBY;
	//m_nGameState = GAME_STATE::IN_GAME;

	m_pCollisionManager = make_shared<CServerCollisionManager>();
	m_pCollisionManager->CreateCollision(SPACE_FLOOR, SPACE_WIDTH, SPACE_DEPTH);

	// 씬 생성
	LoadScene();
	vector<int> vDoor;
	for (int i = 0; i < m_pCollisionManager->GetNumberOfCollisionObject(); ++i) {
		shared_ptr<CServerGameObject> object = m_pCollisionManager->GetCollisionObjectWithNumber(i);
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
		shared_ptr<CServerGameObject> object = m_pCollisionManager->GetCollisionObjectWithNumber(vDoor[i]);
		auto pElevaterDoor = dynamic_pointer_cast<CServerElevatorDoorObject>(object);
		if (!pElevaterDoor) {
			//std::cout << "엘리베이터 문이 아닙니다.!" << std::endl;
			assert(0); //반드시 CServerElevatorDoorObject 일것임. 아니면 시스템 종료 씬 오브젝트 정렬의 문제 발생
		}

		if (i == random_escape_index) {
			pElevaterDoor->SetEscapeDoor(true);
			for (int pi = 0; pi < MAX_CLIENT; ++pi) {
				m_aUpdateInfo[pi].m_playerInfo.m_iEscapeDoor = vDoor[i];
			}
		}
		//pElevaterDoor->SetEscapeDoor(false); // 디버그를 위해서 모든 문을 잠금
	}

	//std::cout << "생성된 충돌객체 = " << m_pCollisionManager->GetNumberOfCollisionObject() << std::endl;
	// 아이템 생성
	CreateItemObject();
	//std::cout << "아이템 생성후 생성된 충돌객체 = " << m_pCollisionManager->GetNumberOfCollisionObject() << std::endl;


	return true;
}
void TCPServer::SimulationLoop()
{
	m_timer.Tick();

	if (m_nGameState == GAME_STATE::IN_LOBBY)
	{
		return;
	}

	m_nGameState = CheckEndGame();
	if (m_nGameState != GAME_STATE::IN_GAME)
	{
		UpdateEndGame(m_nGameState);
		return;
	}

	// 실제 시뮬레이션이 일어날곳
	float fElapsedTime = m_timer.GetTimeElapsed();
	for (auto& pPlayer : m_apPlayers)
	{
		if (!pPlayer || pPlayer->GetPlayerId() == -1)
		{
			continue;
		}
		pPlayer->SetPickedObject(m_pCollisionManager);

		pPlayer->RightClickProcess(m_pCollisionManager);
		pPlayer->UseItem(m_pCollisionManager);
		pPlayer->Update(fElapsedTime, m_pCollisionManager);
		pPlayer->UpdatePicking(pPlayer->GetPlayerId());
		//UpdateInformation(pPlayer);
		m_pCollisionManager->Collide(fElapsedTime, pPlayer);

		pPlayer->OnUpdateToParent();
		pPlayer->Declare(fElapsedTime);
	}

	m_pCollisionManager->Update(fElapsedTime);

	UpdateInformation();
	CreateSendObject();
}

int TCPServer::CheckLobby()
{
	return 0;
}

int TCPServer::CheckEndGame()
{
	int nEndGame = GAME_STATE::IN_GAME;

	if (m_nZombie == 1 && m_nBlueSuit > 0)
	{
		int nAliveBlueSuit = 0;
		for (int i = 1; i < MAX_CLIENT; ++i)
		{
			if (!m_apPlayers[i] || m_apPlayers[i]->GetPlayerId() == -1)
			{
				continue;
			}

			if (m_apPlayers[i]->IsAlive())
			{
				++nAliveBlueSuit;
			}
		}

		if (nAliveBlueSuit == 0)
		{
			nEndGame = GAME_STATE::ZOMBIE_WIN;
			return nEndGame;
		}
	}

	for (const auto& pPlayer : m_apPlayers)
	{
		if (!pPlayer || pPlayer->GetPlayerId() == -1)
		{
			continue;
		}

		if (pPlayer->IsWinner())
		{
			if (dynamic_pointer_cast<CServerBlueSuitPlayer>(pPlayer))
			{
				nEndGame = GAME_STATE::BLUE_SUIT_WIN;
			}
			//else
			//{
			//	nEndGame = GAME_STATE::ZOMBIE_WIN;
			//}
			break;
		}
	}

	return nEndGame;
}

void TCPServer::UpdateEndGame(int nEndGame)
{
	for (auto& sockInfo : m_vSocketInfoList)
	{
		if (!sockInfo.m_bUsed)
		{
			continue;
		}

		if (nEndGame == GAME_STATE::BLUE_SUIT_WIN) // BLUE SUIT WIN
		{
			sockInfo.m_socketState = SOCKET_STATE::SEND_BLUE_SUIT_WIN;
		}
		else // ZOMBIE WIN
		{
			sockInfo.m_socketState = SOCKET_STATE::SEND_ZOMBIE_WIN;
		}
	}
}

// 소켓 정보 추가
INT8 TCPServer::AddSocketInfo(SOCKET sockClient, struct sockaddr_in addrClient, int nAddrLen)
{
	INT8 nSocketIndex = -1;
	if (m_nClient >= MAX_CLIENT)
	{
		return nSocketIndex;
	}
	SOCKETINFO sockInfo;

	sockInfo.m_bUsed = true;
	sockInfo.m_sock = sockClient;
	sockInfo.m_addrClient = addrClient;
	sockInfo.m_nAddrlen = nAddrLen;

	getpeername(sockInfo.m_sock, (struct sockaddr*)&sockInfo.m_addrClient, &sockInfo.m_nAddrlen);
	inet_ntop(AF_INET, &sockInfo.m_addrClient.sin_addr, sockInfo.m_pAddr, sizeof(sockInfo.m_pAddr));

	sockInfo.m_socketState = SOCKET_STATE::SEND_ID;

	// 배열에 정보 추가 
	for (int i = 0; i < m_nClient + 1; ++i)
	{
		if (m_vSocketInfoList[i].m_bUsed)
		{
			continue;
		}
		m_nClient++;

		// 클라이언트 정보 초기화
		m_aUpdateInfo[i].m_nClientId = i;
		m_vSocketInfoList[i] = sockInfo;
		nSocketIndex = i;
		break;
	}

	return nSocketIndex;
}

// 소켓 정보 얻기
INT8 TCPServer::GetSocketIndex(SOCKET sock)
{
	for (size_t index = 0; index < m_vSocketInfoList.size(); ++index)
	{
		const SOCKETINFO& sockInfo = m_vSocketInfoList[index];
		if (!sockInfo.m_bUsed)
		{
			continue;
		}
		if (sockInfo.m_sock == sock)
		{
			return static_cast<INT8>(index);
		}
	}
	return -1;
}

bool TCPServer::DisconnectClient(SOCKET sockClient)
{
	const INT8 nSocketIndex = GetSocketIndex(sockClient);
	if (nSocketIndex < 0)
	{
		return false;
	}

	INT8 nListBoxIndex = -1;
	for (INT8 i = 0; i <= nSocketIndex; ++i)
	{
		if (m_vSocketInfoList[i].m_bUsed)
		{
			++nListBoxIndex;
		}
	}

	const bool bHadPlayer = (m_apPlayers[nSocketIndex] != nullptr);
	if (bHadPlayer)
	{
		SendMessage(m_hClientListBox, LB_DELETESTRING, static_cast<WPARAM>(nListBoxIndex), 0);
		if (nSocketIndex == ZOMBIEPLAYER)
		{
			m_nZombie = max(0, m_nZombie - 1);
		}
		else
		{
			m_nBlueSuit = max(0, m_nBlueSuit - 1);
		}
	}

	WSAAsyncSelect(sockClient, m_hWnd, 0, 0);
	shutdown(sockClient, SD_BOTH);
	closesocket(sockClient);

	m_apPlayers[nSocketIndex].reset();
	m_anPlayerStartPosNum[nSocketIndex] = -1;
	m_aUpdateInfo[nSocketIndex] = SC_UPDATE_INFO{};
	m_bDataSend[nSocketIndex] = false;
	m_vSocketInfoList[nSocketIndex] = SOCKETINFO{};
	m_nClient = max<INT8>(0, m_nClient - 1);

	if (bHadPlayer)
	{
		for (auto& otherSocketInfo : m_vSocketInfoList)
		{
			if (!otherSocketInfo.m_bUsed)
			{
				continue;
			}

			otherSocketInfo.m_socketState = SOCKET_STATE::SEND_NUM_OF_CLIENT;
			RequestSend(GetSocketIndex(otherSocketInfo.m_sock));
		}
	}

	return true;
}

int TCPServer::CheckAllClientsSentData(int cur_nPlayer)
{
	int sendClientCount{};
	for (int i = 0; i < cur_nPlayer; ++i) {
		if (m_bDataSend[i]) {
			sendClientCount++;
		}
	}
	return sendClientCount;
}

void TCPServer::SetAllClientsSendStatus(int cur_nPlayer, bool val)
{
	for (int i = 0; i < cur_nPlayer; ++i) {
		m_bDataSend[i] = val;
	}
}

void TCPServer::UpdateInformation()
{
	int cur_nPlayer{};
	for (const auto& pPlayer : m_apPlayers)
	{
		if (!pPlayer || pPlayer->GetPlayerId() == -1)
		{
			continue;
		}
		++cur_nPlayer;
	}

	for (const auto& pPlayer : m_apPlayers)
	{
		INT8 nPlayerId;
		if (!pPlayer || pPlayer->GetPlayerId() == -1)
		{
			continue;
		}
		nPlayerId = pPlayer->GetPlayerId();

		m_aUpdateInfo[nPlayerId].m_bAlive = pPlayer->IsAlive();
		m_aUpdateInfo[nPlayerId].m_bRunning = pPlayer->IsRunning();
		m_aUpdateInfo[nPlayerId].m_xmf3Position = pPlayer->GetPosition();
		m_aUpdateInfo[nPlayerId].m_xmf3Velocity = pPlayer->GetVelocity();
		m_aUpdateInfo[nPlayerId].m_xmf3Look = pPlayer->GetLook();

		if (pPlayer->GetPickedObject().lock())
			m_aUpdateInfo[nPlayerId].m_nPickedObjectNum = pPlayer->GetPickedObject().lock()->GetCollisionNum();
		else
			m_aUpdateInfo[nPlayerId].m_nPickedObjectNum = -1;

		// 지금은 일단 이렇게 해뒀지만 나중에는 0번이 Enemy고정일듯
		if (nPlayerId == ZOMBIEPLAYER)	//Enemy
		{
			shared_ptr<CServerZombiePlayer> pZombiePlayer = dynamic_pointer_cast<CServerZombiePlayer>(pPlayer);
			if (pZombiePlayer) {
				m_aUpdateInfo[nPlayerId].m_nSlotObjectNum[0] = pZombiePlayer->IsTracking() ? 1 : -1;		// 추적
				m_aUpdateInfo[nPlayerId].m_nSlotObjectNum[1] = pZombiePlayer->IsInterruption() ? 1 : -1;	// 시야방해
				m_aUpdateInfo[nPlayerId].m_nSlotObjectNum[2] = pZombiePlayer->IsAttack() ? 1 : -1;			// 공격

				m_aUpdateInfo[nPlayerId].m_playerInfo.m_iMineobjectNum = pZombiePlayer->GetCollideMineRef();
				// 지뢰충돌에 대한 데이터 로직
				if (m_aUpdateInfo[nPlayerId].m_playerInfo.m_iMineobjectNum == -1) {
					m_aUpdateInfo[nPlayerId].m_playerInfo.m_iMineobjectNum = pZombiePlayer->GetCollideMineRef();
					pZombiePlayer->SetExplosionDelay(0.0f);
				}
				else {
					if (pZombiePlayer->GetExplosionDelay() > 0.05f) {
						pZombiePlayer->SetCollideMineRef(-1);
						m_aUpdateInfo[nPlayerId].m_playerInfo.m_iMineobjectNum = pZombiePlayer->GetCollideMineRef();
					}
				}
			}
		}
		else
		{
			shared_ptr<CServerBlueSuitPlayer> pBlueSuitPlayer = dynamic_pointer_cast<CServerBlueSuitPlayer>(pPlayer);
			if (pBlueSuitPlayer)
			{
				for (int i = 0; i < 3; ++i)
				{
					m_aUpdateInfo[nPlayerId].m_nSlotObjectNum[i] = pBlueSuitPlayer->GetReferenceSlotItemNum(i);
					m_aUpdateInfo[nPlayerId].m_nFuseObjectNum[i] = pBlueSuitPlayer->GetReferenceFuseItemNum(i);
				}
				m_aUpdateInfo[nPlayerId].m_playerInfo.m_bAttacked = pBlueSuitPlayer->IsAttacked();
				m_aUpdateInfo[nPlayerId].m_playerInfo.m_selectItem = pBlueSuitPlayer->GetRightItem();
				m_aUpdateInfo[nPlayerId].m_playerInfo.m_bTeleportItemUse = pBlueSuitPlayer->IsTeleportUse();
			}

		}
		// 업데이트 오브젝트는 리셋
		m_aUpdateInfo[nPlayerId].m_nNumOfObject = 0;
		for (int i = 0; i < MAX_SEND_OBJECT_INFO; ++i)
		{
			m_aUpdateInfo[nPlayerId].m_anObjectNum[i] = -1;
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
		m_vDrawerId.push_back(pair<int, int>(nServerObjectNum, 1));
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
		m_vDrawerId.push_back(pair<int, int>(nServerObjectNum, 2));
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
	m_pCollisionManager->AddCollisionObject(pGameObject);
}

void TCPServer::CreateItemObject()
{
	//CServerItemObject::SetDrawerStartEnd(m_nStartDrawer1, m_nEndDrawer1, m_nStartDrawer2, m_nEndDrawer2);
	// 확률: fus 30, mine 30, tp 30, radar 10
	uniform_int_distribution<int> dis(0, m_vDrawerId.size() - 1); //[CJI 0525] m_vDrawerId 에 번호를 저장하는 방식으로 변경하여 랜덤으로 뽑아 사용
	uniform_int_distribution<int> item_dis(0, 99);
	uniform_int_distribution<int> rotation_dis(1, 360);
	uniform_real_distribution<float> pos_dis(-0.2f, 0.2f);
	CServerItemObject::SetDrawerIdContainer(m_vDrawerId);

	for (int i = 0; i < ITEM_COUNT; ++i)
	{
		int rd_Num = dis(m_mt19937Gen);
		int nDrawerNum = m_vDrawerId[rd_Num].first;
		shared_ptr<CServerDrawerObject> pDrawerObject = dynamic_pointer_cast<CServerDrawerObject>(m_pCollisionManager->GetCollisionObjectWithNumber(nDrawerNum));
		if (!pDrawerObject) //error
			assert(0);
		//exit(1);

		if (pDrawerObject->m_pStoredItem)	// 이미 다른 아이템이 들어왔음
		{
			--i;
			continue;
		}
		XMFLOAT4X4 xmf4x4World = m_pCollisionManager->GetCollisionObjectWithNumber(nDrawerNum)->GetWorldMatrix();

		int nCreateItem = item_dis(m_mt19937Gen);
		shared_ptr<CServerItemObject> pItemObject;

		XMFLOAT3 xmf3RandOffset = XMFLOAT3(pos_dis(m_mt19937Gen), 0.0f, pos_dis(m_mt19937Gen));
		XMFLOAT3 xmf3RandRotation = XMFLOAT3(0.0f, 0.0f, (float)rotation_dis(m_mt19937Gen));

		if (i < 9)		// Fuse
		{
			pItemObject = make_shared<CServerFuseObject>();
			pItemObject->SetDrawerNumber(nDrawerNum);
			pItemObject->SetDrawer(pDrawerObject);
			pItemObject->SetDrawerType(m_vDrawerId[rd_Num].second);
			pDrawerObject->m_pStoredItem = pItemObject;

			pItemObject->SetRandomRotation(xmf3RandRotation);
			pItemObject->SetRandomOffset(xmf3RandOffset);

			pItemObject->SetWorldMatrix(xmf4x4World);
			m_pCollisionManager->AddCollisionObject(pItemObject);
		}
		else if (i < 24)	// tp
		{
			pItemObject = make_shared<CServerTeleportObject>();
			pItemObject->SetDrawerNumber(nDrawerNum);
			pItemObject->SetDrawer(pDrawerObject);
			pItemObject->SetDrawerType(m_vDrawerId[rd_Num].second);
			pDrawerObject->m_pStoredItem = pItemObject;

			pItemObject->SetRandomRotation(xmf3RandRotation);
			pItemObject->SetRandomOffset(xmf3RandOffset);
			pItemObject->SetWorldMatrix(xmf4x4World);
			m_pCollisionManager->AddCollisionObject(pItemObject);
		}
		else if (i < 26)	// Rader
		{
			xmf3RandRotation = XMFLOAT3(90.0f, 0.0f, 0.0f);
			pItemObject = make_shared<CServerRadarObject>();
			pItemObject->SetDrawerNumber(nDrawerNum);
			pItemObject->SetDrawer(pDrawerObject);
			pItemObject->SetDrawerType(m_vDrawerId[rd_Num].second);
			pDrawerObject->m_pStoredItem = pItemObject;

			pItemObject->SetRandomRotation(xmf3RandRotation);
			pItemObject->SetRandomOffset(xmf3RandOffset);
			pItemObject->SetWorldMatrix(xmf4x4World);
			m_pCollisionManager->AddCollisionObject(pItemObject);
		}
		else if (i < 76)	// Mine
		{
			xmf3RandRotation = XMFLOAT3(90.0f, 0.0f, 0.0f);

			pItemObject = make_shared<CServerMineObject>();
			pItemObject->SetDrawerNumber(nDrawerNum);
			pItemObject->SetDrawer(pDrawerObject);
			pItemObject->SetDrawerType(m_vDrawerId[rd_Num].second);
			pDrawerObject->m_pStoredItem = pItemObject;

			pItemObject->SetRandomRotation(xmf3RandRotation);
			pItemObject->SetRandomOffset(xmf3RandOffset);
			pItemObject->SetWorldMatrix(xmf4x4World);
			m_pCollisionManager->AddCollisionObject(pItemObject);
		}

	}
}

void TCPServer::CreateSendObject()
{
	vector<SC_SPACEOUT_OBJECT> vSO_objects;
	vSO_objects.reserve(m_pCollisionManager->GetOutSpaceObject().size());

	for (auto& pGameObject : m_pCollisionManager->GetOutSpaceObject())
	{
		if (!pGameObject)
		{
			continue;
		}
		vSO_objects.emplace_back(SC_SPACEOUT_OBJECT(pGameObject->GetCollisionNum(), pGameObject->GetWorldMatrix()));
	}
	m_pCollisionManager->GetOutSpaceObject().clear();

	if (!vSO_objects.empty())
	{
		constexpr size_t maxObjectsPerPacket =
			MAX_PACKET_PAYLOAD_SIZE / sizeof(SC_SPACEOUT_OBJECT);
		static_assert(maxObjectsPerPacket > 0);

		for (size_t beginIndex = 0; beginIndex < vSO_objects.size(); beginIndex += maxObjectsPerPacket)
		{
			const size_t objectCount = (std::min)(maxObjectsPerPacket, vSO_objects.size() - beginIndex);
			const size_t payloadSize = sizeof(SC_SPACEOUT_OBJECT) * objectCount;
			const std::uint16_t wirePayloadSize = static_cast<std::uint16_t>(payloadSize);

			vector<char> buffer;
			buffer.reserve(sizeof(INT8) + sizeof(wirePayloadSize) + payloadSize);
			buffer.push_back(static_cast<INT8>(SOCKET_STATE::SEND_SPACEOUT_OBJECTS));
			PushBufferData(buffer, &wirePayloadSize, sizeof(wirePayloadSize));
			PushBufferData(buffer, vSO_objects.data() + beginIndex, payloadSize);

			for (const auto& pPlayer : m_apPlayers)
			{
				if (!pPlayer) continue;
				const INT8 playerId = pPlayer->GetPlayerId();
				if (playerId == -1) continue;
				// 소켓마다 partial send 위치가 다르므로 각 송신 큐가 버퍼 복사본을 소유한다.
				EnqueueSendBuffer(playerId, buffer);
			}
		}
	}

	for (const auto& pPlayer : m_apPlayers)
	{
		int nIndex = 0;
		if (!pPlayer || pPlayer->GetPlayerId() == -1)
		{
			continue;
		}

		INT8 nId = pPlayer->GetPlayerId();

		// 층은 나중에 계단쪽에서만 추가할수있도록해야할듯
		// if(계단 쪽이면 위층 or 아래층범위 검사)
		for (int j = pPlayer->GetWidth() - 1; j <= pPlayer->GetWidth() + 1 && nIndex < MAX_SEND_OBJECT_INFO; ++j)
		{
			if (j < 0 || j > m_pCollisionManager->GetWidth() - 1)
			{
				continue;
			}

			for (int k = pPlayer->GetDepth() - 1; k <= pPlayer->GetDepth() + 1 && nIndex < MAX_SEND_OBJECT_INFO; ++k)
			{
				if (k < 0 || k > m_pCollisionManager->GetDepth() - 1)
				{
					continue;
				}

				for (const auto& pGameObject : m_pCollisionManager->GetSpaceGameObjects(pPlayer->GetFloor(), j, k))
				{
					if (!pGameObject || pGameObject->IsStatic())
					{
						continue;
					}

					m_aUpdateInfo[nId].m_anObjectNum[nIndex] = pGameObject->GetCollisionNum();
					m_aUpdateInfo[nId].m_axmf4x4World[nIndex] = pGameObject->GetWorldMatrix();

					nIndex++;
					//if (m_aUpdateInfo[nId].m_anObjectNum[nIndex] >= m_pCollisionManager->GetNumberOfCollisionObject()) {
					//	std::cout << "index 범위를 넘어서는 오브젝트를 담았습니다.\n";
					//}
					if (nIndex == MAX_SEND_OBJECT_INFO)
					{
						//std::cout << "보내려고 하는 객체가 MAX_SEND_OBJECT_INFO 개수가 되었습니다.\n";
						break;
					}
				}
			}
		}
		m_aUpdateInfo[nId].m_nNumOfObject = nIndex;
	}

}

void TCPServer::InitPlayerPosition(shared_ptr<CServerPlayer>& pServerPlayer, int nIndex)
{
	// 후보지를 두고 int 값에 따라 그곳에 가도록 해야할듯
	uniform_int_distribution<int> disIntPosition(0, m_axmf3Positions.size() - 1);

	int nStartPosNum = disIntPosition(m_mt19937Gen);
	bool bEmpty = false;
	while (!bEmpty)
	{
		bEmpty = true;
		nStartPosNum = disIntPosition(m_mt19937Gen);
		for (const auto& nPlayerStartPos : m_anPlayerStartPosNum)
		{
			if (nPlayerStartPos == nStartPosNum)
			{
				bEmpty = false;
				break;
			}
		}
	}

	m_anPlayerStartPosNum[nIndex] = nStartPosNum;
	XMFLOAT3 xmf3Position = m_axmf3Positions[nStartPosNum];
	pServerPlayer->SetPlayerPosition(xmf3Position);
	pServerPlayer->SetPlayerOldPosition(xmf3Position);
}

template<class... Args>
bool TCPServer::SubmitSendData(int nSocketIndex, Args&&... args)
{
	const size_t bufferSize = (sizeof(args) + ... + 0);
	std::vector<char> buffer(bufferSize);
	size_t offset = 0;
	((memcpy(buffer.data() + offset, &args, sizeof(args)), offset += sizeof(args)), ...);

	return EnqueueSendBuffer(nSocketIndex, std::move(buffer));
}

bool TCPServer::EnqueueSendBuffer(int nSocketIndex, vector<char> buffer)
{
	if (nSocketIndex < 0 || nSocketIndex >= static_cast<int>(m_vSocketInfoList.size()))
	{
		return false;
	}

	SOCKETINFO& socketInfo = m_vSocketInfoList[nSocketIndex];
	bool isInvalidBufferSize = !socketInfo.m_bUsed || buffer.empty() ||
		socketInfo.m_nPendingSendBytes > MAX_PENDING_SEND_BYTES ||
		buffer.size() > MAX_PENDING_SEND_BYTES - socketInfo.m_nPendingSendBytes;
	if (isInvalidBufferSize)
	{
		if (socketInfo.m_bUsed)
		{
			DisconnectClient(socketInfo.m_sock);
		}
		return false;
	}

	socketInfo.m_nPendingSendBytes += buffer.size();
	socketInfo.m_sendQueue.push_back(PendingSend{ std::move(buffer), 0 });
	if (FlushSendQueue(nSocketIndex) == SendResult::Error)
	{
		const SOCKET socket = socketInfo.m_sock;
		err_display("send()");
		DisconnectClient(socket);
		return false;
	}
	return true;
}

TCPServer::SendResult TCPServer::FlushSendQueue(int nSocketIndex)
{
	SOCKETINFO& socketInfo = m_vSocketInfoList[nSocketIndex];
	while (!socketInfo.m_sendQueue.empty())
	{
		PendingSend& pending = socketInfo.m_sendQueue.front();
		const size_t remainingBytes = pending.buffer.size() - pending.sentBytes;
		const int sentBytes = send(
			socketInfo.m_sock,
			pending.buffer.data() + pending.sentBytes,
			static_cast<int>(remainingBytes),
			0);

		if (sentBytes > 0)
		{
			pending.sentBytes += static_cast<size_t>(sentBytes);
			if (pending.sentBytes == pending.buffer.size())
			{
				socketInfo.m_nPendingSendBytes -= pending.buffer.size();
				socketInfo.m_sendQueue.pop_front();
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

void TCPServer::PushBufferData(vector<char>& buffer, const void* data, size_t size)
{
	const char* bytes = static_cast<const char*>(data);
	buffer.insert(buffer.end(), bytes, bytes + size);
}

void TCPServer::ResetReceiveState(SOCKETINFO& socketInfo)
{
	socketInfo.m_nHead = -1;
	socketInfo.m_bRecvHead = false;
	socketInfo.m_nCurrentRecvByte = 0;
	memset(socketInfo.m_pCurrentBuffer, 0, MAX_PACKET_PAYLOAD_SIZE);
}

TCPServer::ReceiveResult TCPServer::RecvData(int nSocketIndex, size_t nBufferSize)
{
	SOCKETINFO& socketInfo = m_vSocketInfoList[nSocketIndex];

	bool isInvalidBufferSize =
		nBufferSize > MAX_PACKET_PAYLOAD_SIZE ||
		socketInfo.m_nCurrentRecvByte < 0 ||
		static_cast<size_t>(socketInfo.m_nCurrentRecvByte) > nBufferSize;
	if (isInvalidBufferSize)
	{
		return ReceiveResult::Error;
	}

	if (nBufferSize == 0)
	{
		return ReceiveResult::Complete;
	}

	const int remainRecvByte = static_cast<int>(nBufferSize) - socketInfo.m_nCurrentRecvByte;
	const int retval = recv(socketInfo.m_sock, socketInfo.m_pCurrentBuffer + socketInfo.m_nCurrentRecvByte, remainRecvByte, 0);
	if (retval > 0)
	{
		socketInfo.m_nCurrentRecvByte += retval;
	}
	else if (retval == 0)
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

	if (static_cast<size_t>(socketInfo.m_nCurrentRecvByte) < nBufferSize)
	{
		return ReceiveResult::Pending;
	}

	socketInfo.m_nCurrentRecvByte = 0;
	return ReceiveResult::Complete;
}

// 소켓 함수 오류 출력 후 종료
void err_quit(const char* msg)
{
	LPVOID lpMsgBuf;
	FormatMessageA(
		FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
		NULL,
		WSAGetLastError(),
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(char*)&lpMsgBuf,
		0,
		NULL);

	MessageBoxA(NULL, (const char*)lpMsgBuf, msg, MB_ICONERROR);
	LocalFree(lpMsgBuf);
	exit(1);
}
// 소켓 함수 오류 출력
void err_display(const char* msg)
{
	LPVOID lpMsgBuf;
	FormatMessageA(
		FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
		NULL,
		WSAGetLastError(),
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(char*)&lpMsgBuf, 0, NULL);

	LocalFree(lpMsgBuf);
}
// 소켓 함수 오류 출력
void err_display(int errcode)
{
	LPVOID lpMsgBuf;
	FormatMessageA(
		FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
		NULL,
		errcode,
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(char*)&lpMsgBuf,
		0,
		NULL);

	LocalFree(lpMsgBuf);
}
