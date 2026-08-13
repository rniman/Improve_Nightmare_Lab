#include "stdafx.h"
#include "TCPServer.h"
#include "ServerObject.h"
#include "ServerEnvironmentObject.h"
#include "ServerPlayer.h"
#include "ServerCollision.h"

#include <cmath>

namespace
{
	constexpr size_t MAX_PENDING_SEND_BYTES = 4 * 1024 * 1024;
	constexpr auto NETWORK_STATISTICS_INTERVAL = std::chrono::seconds(1);
	constexpr WORD VALID_CLIENT_KEY_MASK =
		KEY_W | KEY_S | KEY_A | KEY_D |
		KEY_1 | KEY_2 | KEY_3 | KEY_4 |
		KEY_E | KEY_LSHIFT | KEY_LBUTTON | KEY_RBUTTON;
	constexpr size_t CLIENT_INPUT_PAYLOAD_SIZE =
		sizeof(WORD) +
		sizeof(XMFLOAT4X4) +
		sizeof(XMFLOAT3) * 3 +
		sizeof(SC_ANIMATION_INFO) +
		sizeof(SC_PLAYER_INFO);

	struct ClientInputData
	{
		WORD keyBuffer = 0;
		XMFLOAT4X4 viewMatrix = {};
		XMFLOAT3 look = {};
		XMFLOAT3 right = {};
		XMFLOAT3 up = {};
		SC_ANIMATION_INFO animationInfo = {};
		SC_PLAYER_INFO playerInfo = {};
	};

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

	const char* GetSendPacketName(std::uint8_t head)
	{
		switch (static_cast<SOCKET_STATE>(head))
		{
		case SOCKET_STATE::SEND_ID: return "ID";
		case SOCKET_STATE::SEND_UPDATE_DATA: return "UPDATE_DATA";
		case SOCKET_STATE::SEND_NUM_OF_CLIENT: return "NUM_OF_CLIENT";
		case SOCKET_STATE::SEND_BLUE_SUIT_WIN: return "BLUE_SUIT_WIN";
		case SOCKET_STATE::SEND_ZOMBIE_WIN: return "ZOMBIE_WIN";
		case SOCKET_STATE::SEND_GAME_START: return "GAME_START";
		case SOCKET_STATE::SEND_CHANGE_SLOT: return "CHANGE_SLOT";
		case SOCKET_STATE::SEND_OPEN_DRAWER_SOUND: return "OPEN_DRAWER_SOUND";
		case SOCKET_STATE::SEND_CLOSE_DRAWER_SOUND: return "CLOSE_DRAWER_SOUND";
		case SOCKET_STATE::SEND_OPEN_DOOR_SOUND: return "OPEN_DOOR_SOUND";
		case SOCKET_STATE::SEND_CLOSE_DOOR_SOUND: return "CLOSE_DOOR_SOUND";
		case SOCKET_STATE::SEND_BLUE_SUIT_DEAD: return "BLUE_SUIT_DEAD";
		case SOCKET_STATE::SEND_SPACEOUT_OBJECTS: return "SPACEOUT_OBJECTS";
		case SOCKET_STATE::SEND_LOADING_COMPLETE: return "LOADING_COMPLETE";
		default: return "UNKNOWN";
		}
	}

	const char* GetReceivePacketName(std::uint8_t head)
	{
		switch (static_cast<RECV_HEAD>(head))
		{
		case HEAD_KEYS_BUFFER: return "KEYS_BUFFER";
		case HEAD_GAME_START: return "GAME_START";
		case HEAD_CHANGE_SLOT: return "CHANGE_SLOT";
		case HEAD_LOADING_COMPLETE: return "LOADING_COMPLETE";
		default: return "UNKNOWN";
		}
	}

	void AccumulateNetworkStatistics(NetworkStatistics& destination, const NetworkStatistics& source)
	{
		destination.sentBytes += source.sentBytes;
		destination.receivedBytes += source.receivedBytes;
		destination.sentPackets += source.sentPackets;
		destination.receivedPackets += source.receivedPackets;
		destination.sendWouldBlockCount += source.sendWouldBlockCount;
		destination.receiveWouldBlockCount += source.receiveWouldBlockCount;
		destination.peakUnsentBytes = (std::max)(destination.peakUnsentBytes, source.peakUnsentBytes);
		destination.peakPendingPackets = (std::max)(destination.peakPendingPackets, source.peakPendingPackets);

		for (size_t i = 0; i < NetworkStatistics::PACKET_TYPE_COUNT; ++i)
		{
			destination.sentByHead[i].bytes += source.sentByHead[i].bytes;
			destination.sentByHead[i].packets += source.sentByHead[i].packets;
			destination.receivedByHead[i].bytes += source.receivedByHead[i].bytes;
			destination.receivedByHead[i].packets += source.receivedByHead[i].packets;
		}
	}

	void PrintPacketBreakdown(
		const char* direction,
		const std::array<NetworkPacketStatistics, NetworkStatistics::PACKET_TYPE_COUNT>& statistics,
		bool isSend)
	{
		bool hasPacket = false;
		std::cout << "  " << direction << " by head:";
		for (size_t i = 0; i < statistics.size(); ++i)
		{
			if (statistics[i].bytes == 0 && statistics[i].packets == 0)
			{
				continue;
			}

			hasPacket = true;
			const char* packetName = isSend
				? GetSendPacketName(static_cast<std::uint8_t>(i))
				: GetReceivePacketName(static_cast<std::uint8_t>(i));
			std::cout << ' ' << packetName << '=' << statistics[i].bytes
				<< "B/" << statistics[i].packets << "pkt";
		}

		if (!hasPacket)
		{
			std::cout << " none";
		}
		std::cout << '\n';
	}
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
	m_lastNetworkStatisticsReportTime = std::chrono::steady_clock::now();

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
	// 소켓 이벤트가 계속 발생해 SimulationLoop가 실행되지 않는 상황에서도 통계를 출력한다.
	ReportNetworkStatisticsIfDue();

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

		// 등록되지 않은 HEAD는 payload 크기와 형식을 결정할 수 없으므로 더 이상 스트림을 해석하지 않는다.
		// 연결을 종료해 잘못된 바이트를 다음 패킷의 HEAD로 오인하는 상황도 방지한다.
		if (!IsValidReceiveHead(m_vSocketInfoList[nSocketIndex].m_nHead))
		{
			std::cerr << "Invalid receive packet head: client=" << nSocketIndex
				<< ", head=" << static_cast<int>(m_vSocketInfoList[nSocketIndex].m_nHead) << '\n';
			DisconnectClient(socket);
			return;
		}
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
		// 슬롯 번호는 플레이어 및 소켓 배열의 인덱스로 사용되므로 범위를 벗어난 값은 무시할 수 없다.
		// 잘못된 연결을 종료해 이후 패킷이 비정상적인 서버 상태에 반영되는 것을 방지한다.
		if (nSelectedSlot < 0 || nSelectedSlot >= static_cast<INT8>(MAX_CLIENT))
		{
			std::cerr << "Invalid selected slot: client=" << nSocketIndex
				<< ", slot=" << static_cast<int>(nSelectedSlot) << '\n';
			DisconnectClient(socket);
			return;
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
		// 소켓 슬롯과 플레이어 슬롯이 일치할 때만 입력을 해당 플레이어에게 적용한다.
		// 포인터가 없거나 ID가 다르면 서버 내부의 연결/플레이어 상태가 이미 불일치한 것이다.
		if (!pPlayer || pPlayer->GetPlayerId() != nSocketIndex)
		{
			const int playerId = pPlayer ? static_cast<int>(pPlayer->GetPlayerId()) : -1;
			std::cerr << "Invalid player for input packet: client=" << nSocketIndex
				<< ", playerId=" << playerId << '\n';
			DisconnectClient(socket);
			return;
		}

		// KeysBuffer(WORD), viewMatrix, vecLook, vecRight, vecUp, animationInfo, playerInfo
		if (!handleReceiveResult(RecvData(nSocketIndex, CLIENT_INPUT_PAYLOAD_SIZE)))
		{
			return;
		}

		// 수신 버퍼의 모든 필드를 지역 변수로 먼저 역직렬화한다.
		// 검증이 끝나기 전에는 일부 값만 플레이어 상태에 반영되는 일이 없어야 한다.
		size_t readOffset = 0;
		auto readValue = [&socketInfo = m_vSocketInfoList[nSocketIndex], &readOffset](auto& value)
			{
				memcpy(&value, socketInfo.m_pCurrentBuffer + readOffset, sizeof(value));
				readOffset += sizeof(value);
			};

		ClientInputData input;
		readValue(input.keyBuffer);
		readValue(input.viewMatrix);
		readValue(input.look);
		readValue(input.right);
		readValue(input.up);
		readValue(input.animationInfo);
		readValue(input.playerInfo);
		assert(readOffset == CLIENT_INPUT_PAYLOAD_SIZE);

		if (!IsValidKeyBuffer(input.keyBuffer))
		{
			std::cerr << "Invalid key buffer: client=" << nSocketIndex
				<< ", value=" << input.keyBuffer << '\n';
			DisconnectClient(socket);
			return;
		}
		if (!IsFiniteMatrix(input.viewMatrix) ||
			!IsFiniteVector(input.look) ||
			!IsFiniteVector(input.right) ||
			!IsFiniteVector(input.up))
		{
			std::cerr << "Non-finite transform in input packet: client=" << nSocketIndex << '\n';
			DisconnectClient(socket);
			return;
		}

		// 모든 역직렬화와 입력 검증을 통과한 뒤 서버의 플레이어 상태를 갱신한다.
		if (!pPlayer->IsRecvData())
		{
			pPlayer->SetRecvData(true);
		}
		pPlayer->SetKeyBuffer(input.keyBuffer);
		pPlayer->SetViewMatrix(input.viewMatrix);
		pPlayer->SetLook(input.look);
		pPlayer->SetRight(input.right);
		pPlayer->SetUp(input.up);
		m_aUpdateInfo[nSocketIndex].m_animationInfo = input.animationInfo;
		pPlayer->SetRightClick(input.playerInfo.m_bRightClick);

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

	// HEAD와 DATA가 모두 도착해 하나의 애플리케이션 패킷이 완성된 시점에만 패킷 수를 증가시킨다.
	RecordReceivedPacket(m_vSocketInfoList[nSocketIndex]);
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
	ReportNetworkStatisticsIfDue();

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
	ProcessObjectReplication();
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

bool TCPServer::IsValidReceiveHead(INT8 head) const
{
	// 서버가 payload 크기와 처리 방법을 알고 있는 클라이언트 패킷만 허용한다.
	switch (head)
	{
	case HEAD_KEYS_BUFFER:
	case HEAD_GAME_START:
	case HEAD_CHANGE_SLOT:
	case HEAD_LOADING_COMPLETE:
		return true;
	default:
		return false;
	}
}

bool TCPServer::DisconnectClient(SOCKET sockClient)
{
	const INT8 nSocketIndex = GetSocketIndex(sockClient);
	if (nSocketIndex < 0)
	{
		return false;
	}

	// SOCKETINFO가 초기화되기 전에 이 연결의 누적 측정값을 남긴다.
	ReportDisconnectedClientStatistics(nSocketIndex, m_vSocketInfoList[nSocketIndex]);

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

void TCPServer::ProcessObjectReplication()
{
	// 한 번의 시뮬레이션에서 발생한 오브젝트 변경을 정해진 순서로 네트워크 상태에 반영한다.
	// 공간 이동 알림은 별도 패킷으로 보내고, 주변 오브젝트는 다음 UPDATE_DATA에 포함한다.
	const std::vector<SC_SPACEOUT_OBJECT> outOfSpaceObjects = CollectOutOfSpaceObjects();
	EnqueueOutOfSpaceObjectPackets(outOfSpaceObjects);
	UpdateNearbyObjectReplicationData();
}

std::vector<SC_SPACEOUT_OBJECT> TCPServer::CollectOutOfSpaceObjects()
{
	std::vector<SC_SPACEOUT_OBJECT> objectUpdates;
	auto& outOfSpaceObjects = m_pCollisionManager->GetOutSpaceObject();
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
		PushBufferData(packetBuffer, &wirePayloadBytes, sizeof(wirePayloadBytes));
		PushBufferData(packetBuffer, objectUpdates.data() + firstObjectIndex, payloadBytes);

		for (const auto& player : m_apPlayers)
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
	// 고정 크기 UPDATE_DATA 패킷에 포함할 플레이어별 주변 동적 오브젝트를 갱신한다.
	// 현재 플레이어가 속한 셀을 중심으로 3x3 셀만 탐색하고 최대 30개까지 기록한다.
	for (const auto& player : m_apPlayers)
	{
		if (!player || player->GetPlayerId() == -1)
		{
			continue;
		}

		const INT8 playerId = player->GetPlayerId();
		int objectCount = 0;

		// 현재는 플레이어와 같은 층만 검사한다. 계단 주변의 인접 층 검사는 별도 게임 규칙이다.
		for (int widthIndex = player->GetWidth() - 1;
			widthIndex <= player->GetWidth() + 1 && objectCount < MAX_SEND_OBJECT_INFO;
			++widthIndex)
		{
			if (widthIndex < 0 || widthIndex >= m_pCollisionManager->GetWidth())
			{
				continue;
			}

			for (int depthIndex = player->GetDepth() - 1;
				depthIndex <= player->GetDepth() + 1 && objectCount < MAX_SEND_OBJECT_INFO;
				++depthIndex)
			{
				if (depthIndex < 0 || depthIndex >= m_pCollisionManager->GetDepth())
				{
					continue;
				}

				for (const auto& gameObject : m_pCollisionManager->GetSpaceGameObjects(
					player->GetFloor(),
					widthIndex,
					depthIndex))
				{
					if (!gameObject || gameObject->IsStatic())
					{
						continue;
					}

					m_aUpdateInfo[playerId].m_anObjectNum[objectCount] = gameObject->GetCollisionNum();
					m_aUpdateInfo[playerId].m_axmf4x4World[objectCount] = gameObject->GetWorldMatrix();

					++objectCount;
					if (objectCount == MAX_SEND_OBJECT_INFO)
					{
						break;
					}
				}
			}
		}
		m_aUpdateInfo[playerId].m_nNumOfObject = objectCount;
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
	const bool isInvalidBufferSize = !socketInfo.m_bUsed || buffer.empty() ||
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
	socketInfo.m_nUnsentSendBytes += buffer.size();
	socketInfo.m_sendQueue.push_back(PendingSend{ std::move(buffer), 0 });

	// 최고 대기량은 송신 생산 속도가 소켓 처리 속도를 앞서는지 판단하는 값이다.
	for (NetworkStatistics* statistics : {
		&socketInfo.m_totalNetworkStatistics,
		&socketInfo.m_intervalNetworkStatistics })
	{
		statistics->peakUnsentBytes = (std::max)(statistics->peakUnsentBytes, socketInfo.m_nUnsentSendBytes);
		statistics->peakPendingPackets = (std::max)(statistics->peakPendingPackets, socketInfo.m_sendQueue.size());
	}
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
			const std::uint8_t head = static_cast<std::uint8_t>(pending.buffer.front());
			for (NetworkStatistics* statistics : {
				&socketInfo.m_totalNetworkStatistics,
				&socketInfo.m_intervalNetworkStatistics })
			{
				// send()가 양수를 반환한 바이트만 실제 송신 처리량으로 기록한다.
				statistics->sentBytes += static_cast<std::uint64_t>(sentBytes);
				statistics->sentByHead[head].bytes += static_cast<std::uint64_t>(sentBytes);
			}

			pending.sentBytes += static_cast<size_t>(sentBytes);
			socketInfo.m_nUnsentSendBytes -= static_cast<size_t>(sentBytes);
			if (pending.sentBytes == pending.buffer.size())
			{
				for (NetworkStatistics* statistics : {
					&socketInfo.m_totalNetworkStatistics,
					&socketInfo.m_intervalNetworkStatistics })
				{
					++statistics->sentPackets;
					++statistics->sentByHead[head].packets;
				}
				socketInfo.m_nPendingSendBytes -= pending.buffer.size();
				socketInfo.m_sendQueue.pop_front();
			}
			continue;
		}

		if (sentBytes == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)
		{
			++socketInfo.m_totalNetworkStatistics.sendWouldBlockCount;
			++socketInfo.m_intervalNetworkStatistics.sendWouldBlockCount;
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
	socketInfo.m_nCurrentPacketReceivedBytes = 0;
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
		std::cerr << "Invalid receive buffer state: client=" << nSocketIndex
			<< ", expectedBytes=" << nBufferSize
			<< ", receivedBytes=" << socketInfo.m_nCurrentRecvByte << '\n';
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
		socketInfo.m_nCurrentPacketReceivedBytes += static_cast<size_t>(retval);
		socketInfo.m_totalNetworkStatistics.receivedBytes += static_cast<std::uint64_t>(retval);
		socketInfo.m_intervalNetworkStatistics.receivedBytes += static_cast<std::uint64_t>(retval);
	}
	else if (retval == 0)
	{
		// recv()가 0이면 상대가 정상적으로 연결을 종료한 것이므로 재시도하지 않는다.
		std::cout << "Client closed connection while receiving: client=" << nSocketIndex << '\n';
		return ReceiveResult::Closed;
	}
	else
	{
		const int errorCode = WSAGetLastError();
		if (errorCode == WSAEWOULDBLOCK)
		{
			// 아직 필요한 바이트가 도착하지 않은 정상적인 논블로킹 상태다.
			// HEAD, 버퍼, 현재 수신 위치를 유지하고 다음 FD_READ에서 이어서 받는다.
			++socketInfo.m_totalNetworkStatistics.receiveWouldBlockCount;
			++socketInfo.m_intervalNetworkStatistics.receiveWouldBlockCount;
			return ReceiveResult::Pending;
		}

		std::cerr << "Receive failed: client=" << nSocketIndex
			<< ", error=" << errorCode << '\n';
		return ReceiveResult::Error;
	}

	if (static_cast<size_t>(socketInfo.m_nCurrentRecvByte) < nBufferSize)
	{
		// partial recv도 오류가 아니다. 현재까지 받은 바이트를 보존하고 다음 FD_READ를 기다린다.
		return ReceiveResult::Pending;
	}

	socketInfo.m_nCurrentRecvByte = 0;
	return ReceiveResult::Complete;
}

void TCPServer::RecordReceivedPacket(SOCKETINFO& socketInfo)
{
	const std::uint8_t head = static_cast<std::uint8_t>(socketInfo.m_nHead);
	for (NetworkStatistics* statistics : {
		&socketInfo.m_totalNetworkStatistics,
		&socketInfo.m_intervalNetworkStatistics })
	{
		++statistics->receivedPackets;
		++statistics->receivedByHead[head].packets;
		statistics->receivedByHead[head].bytes += socketInfo.m_nCurrentPacketReceivedBytes;
	}
}

void TCPServer::ReportNetworkStatisticsIfDue()
{
	const auto currentTime = std::chrono::steady_clock::now();
	const auto elapsedTime = currentTime - m_lastNetworkStatisticsReportTime;
	if (elapsedTime < NETWORK_STATISTICS_INTERVAL)
	{
		return;
	}
	m_lastNetworkStatisticsReportTime = currentTime;

	NetworkStatistics aggregateStatistics;
	size_t currentUnsentBytes = 0;
	size_t currentPendingPackets = 0;
	bool hasConnectedClient = false;

	for (const SOCKETINFO& socketInfo : m_vSocketInfoList)
	{
		if (!socketInfo.m_bUsed)
		{
			continue;
		}

		hasConnectedClient = true;
		AccumulateNetworkStatistics(aggregateStatistics, socketInfo.m_intervalNetworkStatistics);
		currentUnsentBytes += socketInfo.m_nUnsentSendBytes;
		currentPendingPackets += socketInfo.m_sendQueue.size();
	}

	if (!hasConnectedClient)
	{
		return;
	}

	const auto elapsedMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(elapsedTime).count();
	std::cout << "[NET " << elapsedMilliseconds << "ms] TX="
		<< aggregateStatistics.sentBytes << "B/"
		<< aggregateStatistics.sentPackets << "pkt, RX="
		<< aggregateStatistics.receivedBytes << "B/"
		<< aggregateStatistics.receivedPackets << "pkt, Queue="
		<< currentUnsentBytes << "B/" << currentPendingPackets
		<< "pkt, MaxClientPeak=" << aggregateStatistics.peakUnsentBytes << "B/"
		<< aggregateStatistics.peakPendingPackets << "pkt, WouldBlock(TX/RX)="
		<< aggregateStatistics.sendWouldBlockCount << '/'
		<< aggregateStatistics.receiveWouldBlockCount << '\n';
	PrintPacketBreakdown("TX", aggregateStatistics.sentByHead, true);
	PrintPacketBreakdown("RX", aggregateStatistics.receivedByHead, false);

	for (size_t i = 0; i < m_vSocketInfoList.size(); ++i)
	{
		SOCKETINFO& socketInfo = m_vSocketInfoList[i];
		if (!socketInfo.m_bUsed)
		{
			continue;
		}

		const NetworkStatistics& statistics = socketInfo.m_intervalNetworkStatistics;
		std::cout << "  client[" << i << "]: TX=" << statistics.sentBytes
			<< "B/" << statistics.sentPackets << "pkt, RX="
			<< statistics.receivedBytes << "B/" << statistics.receivedPackets
			<< "pkt, Queue=" << socketInfo.m_nUnsentSendBytes << "B/"
			<< socketInfo.m_sendQueue.size() << "pkt, Peak="
			<< statistics.peakUnsentBytes << "B/"
			<< statistics.peakPendingPackets << "pkt\n";

		// 다음 1초 구간은 현재 backlog를 시작점으로 삼아 새 최고치를 측정한다.
		socketInfo.m_intervalNetworkStatistics = NetworkStatistics{};
		socketInfo.m_intervalNetworkStatistics.peakUnsentBytes = socketInfo.m_nUnsentSendBytes;
		socketInfo.m_intervalNetworkStatistics.peakPendingPackets = socketInfo.m_sendQueue.size();
	}
}

void TCPServer::ReportDisconnectedClientStatistics(int nSocketIndex, const SOCKETINFO& socketInfo) const
{
	const NetworkStatistics& statistics = socketInfo.m_totalNetworkStatistics;
	std::cout << "[NET END client=" << nSocketIndex << "] TX="
		<< statistics.sentBytes << "B/" << statistics.sentPackets
		<< "pkt, RX=" << statistics.receivedBytes << "B/"
		<< statistics.receivedPackets << "pkt, PeakQueue="
		<< statistics.peakUnsentBytes << "B/" << statistics.peakPendingPackets
		<< "pkt, WouldBlock(TX/RX)=" << statistics.sendWouldBlockCount
		<< '/' << statistics.receiveWouldBlockCount << '\n';
	PrintPacketBreakdown("TX total", statistics.sentByHead, true);
	PrintPacketBreakdown("RX total", statistics.receivedByHead, false);
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
