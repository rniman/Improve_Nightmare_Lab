#include "stdafx.h"
#include "TCPClient.h"
#include "GameFramework.h"
#include "Player.h"
#include "SharedObject.h"
#include "Sound.h"

namespace
{
	constexpr size_t MAX_PENDING_SEND_BYTES = 4 * 1024 * 1024;
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

	if (m_bWsaStarted)
	{
		WSACleanup();
		m_bWsaStarted = false;
	}

	m_sendQueue.clear();
	m_nPendingSendBytes = 0;
	ResetReceiveState();
}

void CTcpClient::ResetReceiveState()
{
	m_nCurrentRecvByte = 0;
	m_bRecvHead = false;
	m_bPayloadSizeReceived = false;
	m_nHead = -1;
	m_nExpectedPayloadSize = 0;
	memset(m_pCurrentBuffer, 0, MAX_PACKET_PAYLOAD_SIZE);
}

bool CTcpClient::CreateSocket(HWND hWnd, TCHAR* pszIPAddress)
{
	int nRetval;
	CloseConnection();

	// 윈속 초기화
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
	{
		err_quit("WSAStartup");
		return false;
	}
	m_bWsaStarted = true;

	// 소켓 생성
	SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
	m_sock = s;
	//m_sock = socket(AF_INET, SOCK_STREAM, 0);
	if (s == INVALID_SOCKET)
	{
		err_quit("socket()");
		CloseConnection();
		return false;
	}

	char pIPAddress[20];
	ConvertLPWSTRToChar(pszIPAddress, pIPAddress, 20);

	// connect()
	struct sockaddr_in serveraddr;
	memset(&serveraddr, 0, sizeof(serveraddr));
	serveraddr.sin_family = AF_INET;
	//inet_pton(AF_INET, SERVERIP, &serveraddr.sin_addr);
	inet_pton(AF_INET, pIPAddress, &serveraddr.sin_addr);
	serveraddr.sin_port = htons(SERVERPORT);

	//nRetval = connect(m_sock, (struct sockaddr*)&serveraddr, sizeof(serveraddr));
	nRetval = connect(s, (struct sockaddr*)&serveraddr, sizeof(serveraddr));
	if (nRetval == SOCKET_ERROR)
	{
		err_display("connect()");
		CloseConnection();
		return false;
	}

	//nRetval = WSAAsyncSelect(m_sock, hWnd, WM_SOCKET, FD_CLOSE | FD_READ | FD_WRITE);	// FD_WRITE가 발생할것이다.
	nRetval = WSAAsyncSelect(s, hWnd, WM_SOCKET, FD_CLOSE | FD_READ | FD_WRITE);	// FD_WRITE가 발생할것이다.
	if (nRetval == SOCKET_ERROR)
	{
		err_display("WSAAsyncSelect()");
		CloseConnection();
		return false;
	}

	return true;
}

std::array<CS_CLIENTS_INFO, 5>& CTcpClient::GetArrayClientsInfo()
{
	return m_aClientInfo;
}

void CTcpClient::OnProcessingSocketMessage(HWND hWnd, UINT nMessageID, WPARAM wParam, LPARAM lParam)
{
	const int nSocketError = WSAGETSELECTERROR(lParam);
	if (nSocketError != 0)
	{
		err_display(nSocketError);
		if (static_cast<SOCKET>(wParam) == m_sock)
		{
			CloseConnection();
		}
		return;
	}

	switch (WSAGETSELECTEVENT(lParam))
	{
	case FD_READ:	// 소켓이 데이터를 읽을 준비가 되었다.
		OnProcessingReadMessage(hWnd, nMessageID, wParam, lParam);
		break;
	case FD_WRITE:	// 소켓이 데이터를 전송할 준비가 되었다.
		OnProcessingWriteMessage(hWnd, nMessageID, wParam, lParam);
		break;
	case FD_CLOSE:
		if ((SOCKET)wParam == m_sock)
		{
			CloseConnection();
		}
		break;
	default:
		break;
	}
}

void CTcpClient::OnProcessingReadMessage(HWND hWnd, UINT nMessageID, WPARAM wParam, LPARAM lParam)
{
	const SOCKET socket = static_cast<SOCKET>(wParam);
	auto handleReceiveResult = [this](ReceiveResult result)
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
		};

	if (!m_bRecvHead)
	{
		const ReceiveResult result = RecvData(socket, sizeof(INT8));
		if (!handleReceiveResult(result))
		{
			return;
		}

		m_bRecvHead = true;
		memcpy(&m_nHead, m_pCurrentBuffer, sizeof(INT8));
		memset(m_pCurrentBuffer, 0, MAX_PACKET_PAYLOAD_SIZE);
	}

	switch (m_nHead)
	{
	case HEAD_GAME_START:
		PostMessage(hWnd, WM_START_GAME, 0, 0);
		m_socketState = SOCKET_STATE::SEND_KEY_BUFFER;
		break;
	case HEAD_CHANGE_SLOT:
	{
		if (!handleReceiveResult(RecvData(socket, sizeof(INT8) + sizeof(m_aClientInfo))))
		{
			return;
		}
		RecvNum++;
		const INT8 nPrevMainClientId = m_nMainClientId;
		memcpy(&m_nMainClientId, m_pCurrentBuffer, sizeof(INT8));
		memcpy(&m_aClientInfo, m_pCurrentBuffer + sizeof(INT8), sizeof(m_aClientInfo));
		for (int i = 0; i < MAX_CLIENT; ++i)
		{
			if (m_apPlayers[i])
			{
				m_apPlayers[i]->SetClientId(m_aClientInfo[i].m_nClientId);
			}
		}

		if (nPrevMainClientId != m_nMainClientId)
		{
			PostMessage(hWnd, WM_CHANGE_SLOT, 1, 0);
		}
	}
	break;
	case HEAD_INIT:
	{
		if (!handleReceiveResult(RecvData(socket, sizeof(INT8) * 2 + sizeof(m_aClientInfo))))
		{
			return;
		}
		RecvNum++;

		memcpy(&m_nMainClientId, m_pCurrentBuffer, sizeof(INT8));
		memcpy(&m_nClient, m_pCurrentBuffer + sizeof(INT8), sizeof(INT8));
		memcpy(&m_aClientInfo, m_pCurrentBuffer + sizeof(INT8) * 2, sizeof(m_aClientInfo));

		break;
	}
	case HEAD_UPDATE_DATA:
	{
		if (!handleReceiveResult(RecvData(socket, sizeof(m_aClientInfo))))
		{
			return;
		}
		RecvNum++;

		memcpy(m_aClientInfo.data(), m_pCurrentBuffer, sizeof(m_aClientInfo));

		UpdateDataFromServer();

		break;
	}
	case HEAD_NUM_OF_CLIENT:
	{
		if (!handleReceiveResult(RecvData(socket, sizeof(INT8) + sizeof(m_aClientInfo))))
		{
			return;
		}
		RecvNum++;

		memcpy(&m_nClient, m_pCurrentBuffer, sizeof(INT8));
		memcpy(&m_aClientInfo, m_pCurrentBuffer + sizeof(INT8), sizeof(m_aClientInfo));
		for (int i = 0; i < MAX_CLIENT; ++i)
		{
			if (m_apPlayers[i])
			{
				m_apPlayers[i]->SetClientId(m_aClientInfo[i].m_nClientId);
			}
		}
		break;
	}
	case HEAD_BLUE_SUIT_WIN:
		PostMessage(hWnd, WM_END_GAME, 0, 0);
		break;
	case HEAD_ZOMBIE_WIN:
		PostMessage(hWnd, WM_END_GAME, 1, 0);
		break;
	case HEAD_OPEN_DRAWER_SOUND:
	{
		SoundManager& soundManager = soundManager.GetInstance();
		soundManager.PlaySoundWithName(sound::OPEN_DRAWER);
		break;
	}
	case HEAD_CLOSE_DRAWER_SOUND:
	{
		SoundManager& soundManager = soundManager.GetInstance();
		soundManager.PlaySoundWithName(sound::CLOSE_DRAWER);
		break;
	}
	case HEAD_OPEN_DOOR_SOUND:
	{
		SoundManager& soundManager = soundManager.GetInstance();
		soundManager.PlaySoundWithName(sound::OPEN_DOOR);
		break;
	}
	case HEAD_CLOSE_DOOR_SOUND:
	{
		SoundManager& soundManager = soundManager.GetInstance();
		soundManager.PlaySoundWithName(sound::CLOSE_DOOR);
		break;
	}
	case HEAD_BLUE_SUIT_DEAD:
	{
		if (!handleReceiveResult(RecvData(socket, sizeof(char))))
		{
			return;
		}

		char deadUserId = -1;
		memcpy(&deadUserId, m_pCurrentBuffer, sizeof(deadUserId));
		if (deadUserId < 0 || deadUserId >= static_cast<char>(MAX_CLIENT) || !m_apPlayers[deadUserId])
		{
			CloseConnection();
			return;
		}

		SoundManager& soundManager = soundManager.GetInstance();
		soundManager.PlaySoundWithName(sound::DEAD_BLUESUIT);
		soundManager.SetVolume(sound::DEAD_BLUESUIT, m_apPlayers[deadUserId]->GetPlayerVolume());
		break;
	}
	case SEND_SPACEOUT_OBJECTS:
	{
		if (!m_bPayloadSizeReceived)
		{
			if (!handleReceiveResult(RecvData(socket, sizeof(unsigned short))))
			{
				return;
			}

			std::uint16_t bufferSize = 0;
			memcpy(&bufferSize, m_pCurrentBuffer, sizeof(bufferSize));
			m_nExpectedPayloadSize = bufferSize;
			m_bPayloadSizeReceived = true;
			memset(m_pCurrentBuffer, 0, MAX_PACKET_PAYLOAD_SIZE);

			if (m_nExpectedPayloadSize > MAX_PACKET_PAYLOAD_SIZE ||
				m_nExpectedPayloadSize % sizeof(SC_SPACEOUT_OBJECT) != 0)
			{
				CloseConnection();
				return;
			}
		}

		if (!handleReceiveResult(RecvData(socket, m_nExpectedPayloadSize)))
		{
			return;
		}

		const size_t objectCount = m_nExpectedPayloadSize / sizeof(SC_SPACEOUT_OBJECT);
		vector<SC_SPACEOUT_OBJECT> spaceOutObjects(objectCount);

		if (m_nExpectedPayloadSize > 0)
		{
			memcpy(spaceOutObjects.data(), m_pCurrentBuffer, m_nExpectedPayloadSize);
		}
		for (const auto& obj : spaceOutObjects)
		{
			shared_ptr<CGameObject> pGameObject = g_collisionManager.GetCollisionObjectWithNumber(obj.m_iObjectId).lock();
			if (pGameObject)
			{
				pGameObject->m_xmf4x4World = obj.m_xmf4x4World;
				pGameObject->m_xmf4x4ToParent = obj.m_xmf4x4World;
				pGameObject->SetObtain(false);
			}
		}
		break;
	}
	case HEAD_LOADING_COMPLETE:
	{
		m_bRecvLoadComplete = true;
		break;
	}
	default:
		break;
	}

	ResetReceiveState();
}

void CTcpClient::UpdateDataFromServer()
{
	for (int i = 0; i < MAX_CLIENT; ++i)
	{
		if (m_apPlayers[i])
		{
			m_apPlayers[i]->SetAlive(m_aClientInfo[i].m_bAlive);
			m_apPlayers[i]->SetRunning(m_aClientInfo[i].m_bRunning);
			m_apPlayers[i]->SetClientId(m_aClientInfo[i].m_nClientId);
			m_apPlayers[i]->SetPosition(m_aClientInfo[i].m_xmf3Position);
			m_apPlayers[i]->SetVelocity(m_aClientInfo[i].m_xmf3Velocity);
			if (i != m_nMainClientId)
			{
				m_apPlayers[i]->SetPitch(m_aClientInfo[i].m_animationInfo.pitch);
			}


			if (i != m_nMainClientId)
			{
				m_apPlayers[i]->SetLook(m_aClientInfo[i].m_xmf3Look);
				XMFLOAT3 xmf3Right = XMFLOAT3(0.0f, 1.0f, 0.0f);
				xmf3Right = Vector3::CrossProduct(xmf3Right, m_aClientInfo[i].m_xmf3Look, true);
				m_apPlayers[i]->SetRight(xmf3Right);
			}

			//[0523] 피킹 오브젝트 설정(외곽선 작업에 필요)
			UpdatePickedObject(i);

			// 지뢰 충돌
			int nObjectNum = m_aClientInfo[i].m_playerInfo.m_iMineobjectNum;
			if (nObjectNum >= 0) {
				shared_ptr<CGameObject> pGameObject = g_collisionManager.GetCollisionObjectWithNumber(nObjectNum).lock();
				auto mine = dynamic_pointer_cast<CMineObject>(pGameObject);
				if (mine)
				{
					mine->SetCollide(true);
					shared_ptr<CZombiePlayer> pZombiePlayer = dynamic_pointer_cast<CZombiePlayer>(m_apPlayers[i]);
					if (pZombiePlayer)
					{
						pZombiePlayer->SetEectricShock();
					}

					float fVolume = m_apPlayers[i]->GetPlayerVolume();
					SoundManager& soundManager = soundManager.GetInstance();
					if (m_apPlayers[i]->GetPlayerVolume() - EPSILON >= 0.0f)
					{
						soundManager.PlaySoundWithName(sound::ACTIVE_MINE, fVolume);
					}
				}
			}
		}

		if (i == ZOMBIEPLAYER)
		{
			UpdateZombiePlayer();
		}
		else
		{
			UpdatePlayer(i);
		}

		int nNumOfGameObject = m_aClientInfo[i].m_nNumOfObject;
		for (int j = 0; j < nNumOfGameObject; ++j)
		{
			int nObjectNum = m_aClientInfo[i].m_anObjectNum[j];

			if (nObjectNum <= -1 || nObjectNum >= g_collisionManager.GetNumOfCollisionObject())
			{
				continue;
			}
#ifdef LOADSCENE
			shared_ptr<CGameObject> pGameObject = g_collisionManager.GetCollisionObjectWithNumber(nObjectNum).lock();
			if (pGameObject)
			{
				pGameObject->m_xmf4x4World = m_aClientInfo[i].m_axmf4x4World[j];
				pGameObject->m_xmf4x4ToParent = m_aClientInfo[i].m_axmf4x4World[j];
			}
#endif LOADSCENE
		}
	}
}

void CTcpClient::UpdatePickedObject(int i)
{
	if (i == m_nMainClientId)
	{
		if (m_aClientInfo[i].m_nPickedObjectNum == -1)
		{
			m_apPlayers[i]->SetPickedObject(nullptr);
		}
		else
		{
			int nObjectNum = m_aClientInfo[i].m_nPickedObjectNum;
			shared_ptr<CGameObject> pGameObject = g_collisionManager.GetCollisionObjectWithNumber(nObjectNum).lock();
			if (pGameObject)
			{
				m_apPlayers[i]->SetPickedObject(pGameObject);
			}
		}
	}
}

void CTcpClient::OnProcessingWriteMessage(HWND hWnd, UINT nMessageID, WPARAM wParam, LPARAM lParam)
{
	if (m_sendQueue.empty() && m_socketState == SOCKET_STATE::SEND_GAME_START)
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

void CTcpClient::RequestSend()
{
	// TCP 송수신은 독립적이므로 partial recv 대기 중에도 클라이언트 입력은 계속 전송한다.
	bool isInvalid = m_sock == INVALID_SOCKET || m_nMainClientId < 0 ||
		m_nMainClientId >= static_cast<INT8>(MAX_CLIENT) || !m_apPlayers[m_nMainClientId];
	if (isInvalid)
	{
		return;
	}

	//데이터 갱신 후 전송
	m_aClientInfo[m_nMainClientId].m_animationInfo.pitch = m_apPlayers[m_nMainClientId]->GetPitch();
	m_aClientInfo[m_nMainClientId].m_playerInfo.m_bRightClick = m_apPlayers[m_nMainClientId]->IsRightClick();
	m_apPlayers[m_nMainClientId]->SetRightClick(false);

	switch (m_socketState)
	{
	case SOCKET_STATE::SEND_GAME_START:
		SubmitSendData(static_cast<INT8>(1));
		break;
	case SOCKET_STATE::SEND_CHANGE_SLOT:
		if (SubmitSendData(static_cast<INT8>(2), m_nSelectedSlot))
		{
			m_socketState = SOCKET_STATE::SEND_GAME_START;
		}
		break;
	case SOCKET_STATE::SEND_KEY_BUFFER:
	{
		UCHAR* pKeysBuffer = CGameFramework::GetKeysBuffer();
		WORD wKeyBuffer = 0;
		UpdateKeyBitMask(pKeysBuffer, wKeyBuffer);

		const std::chrono::time_point<std::chrono::steady_clock> now = std::chrono::steady_clock::now();
		bool submitted = false;
		// 키버퍼, 카메라Matrix, LOOK,RIGHT 같이 보내주기
		if (m_apPlayers[m_nMainClientId]->m_pSkinnedAnimationController->IsAnimation())
		{
			submitted = SubmitSendData(
				static_cast<INT8>(0),
				now,
				wKeyBuffer,
				m_apPlayers[m_nMainClientId]->GetCamera()->GetViewMatrix(),
				m_apPlayers[m_nMainClientId]->GetLook(),
				m_apPlayers[m_nMainClientId]->GetRight(),
				m_apPlayers[m_nMainClientId]->GetUp(),
				m_aClientInfo[m_nMainClientId].m_animationInfo,
				m_aClientInfo[m_nMainClientId].m_playerInfo
			);
		}
		else
		{
			submitted = SubmitSendData(
				static_cast<INT8>(0),
				now,
				wKeyBuffer,
				m_apPlayers[m_nMainClientId]->GetCamera()->GetViewMatrix(),
				m_apPlayers[m_nMainClientId]->GetCamera()->GetLookVector(),
				m_apPlayers[m_nMainClientId]->GetCamera()->GetRightVector(),
				m_apPlayers[m_nMainClientId]->GetCamera()->GetUpVector(),
				m_aClientInfo[m_nMainClientId].m_animationInfo,
				m_aClientInfo[m_nMainClientId].m_playerInfo
			);
		}

		if (submitted)
		{
			SendNum++;
		}
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
		m_nPendingSendBytes > MAX_PENDING_SEND_BYTES ||
		bufferSize > MAX_PENDING_SEND_BYTES - m_nPendingSendBytes)
	{
		CloseConnection();
		return false;
	}

	std::vector<char> buffer(bufferSize);
	size_t nOffset = 0;
	((memcpy(buffer.data() + nOffset, &args, sizeof(args)), nOffset += sizeof(args)), ...);

	m_nPendingSendBytes += buffer.size();
	m_sendQueue.push_back(PendingSend{ std::move(buffer), 0 });
	if (FlushSendQueue() == SendResult::Error)
	{
		CloseConnection();
		return false;
	}
	return true;
}

CTcpClient::SendResult CTcpClient::FlushSendQueue()
{
	while (!m_sendQueue.empty())
	{
		PendingSend& pending = m_sendQueue.front();
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
				m_nPendingSendBytes -= pending.buffer.size();
				m_sendQueue.pop_front();
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

CTcpClient::ReceiveResult CTcpClient::RecvData(SOCKET socket, size_t nBufferSize)
{
	bool isInvalidBuffer =
		nBufferSize > MAX_PACKET_PAYLOAD_SIZE ||
		m_nCurrentRecvByte < 0 ||
		static_cast<size_t>(m_nCurrentRecvByte) > nBufferSize;

	if (isInvalidBuffer)
	{
		return ReceiveResult::Error;
	}

	if (nBufferSize == 0)
	{
		return ReceiveResult::Complete;
	}

	const int remainRecvByte = static_cast<int>(nBufferSize) - m_nCurrentRecvByte;
	const int retval = recv(socket, m_pCurrentBuffer + m_nCurrentRecvByte, remainRecvByte, 0);
	if (retval > 0)
	{
		m_nCurrentRecvByte += retval;
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

	if (static_cast<size_t>(m_nCurrentRecvByte) < nBufferSize)
	{
		return ReceiveResult::Pending;
	}

	m_nCurrentRecvByte = 0;
	return ReceiveResult::Complete;
}

void CTcpClient::UpdateKeyBitMask(UCHAR* pKeysBuffer, WORD& wKeyBuffer)	// 보낼 키 버퍼를 업데이트
{
	if (pKeysBuffer['W'] & 0xF0)wKeyBuffer |= KEY_W;
	if (pKeysBuffer['S'] & 0xF0)wKeyBuffer |= KEY_S;
	if (pKeysBuffer['A'] & 0xF0)wKeyBuffer |= KEY_A;
	if (pKeysBuffer['D'] & 0xF0)wKeyBuffer |= KEY_D;
	if (pKeysBuffer['1'] & 0xF0)wKeyBuffer |= KEY_1;
	if (pKeysBuffer['2'] & 0xF0)wKeyBuffer |= KEY_2;
	if (pKeysBuffer['3'] & 0xF0)wKeyBuffer |= KEY_3;
	if (pKeysBuffer['4'] & 0xF0)wKeyBuffer |= KEY_4;
	if (pKeysBuffer['E'] & 0xF0)wKeyBuffer |= KEY_E;
	if (pKeysBuffer[VK_LSHIFT] & 0xF0)wKeyBuffer |= KEY_LSHIFT;
	if (pKeysBuffer[VK_LBUTTON] & 0xF0)wKeyBuffer |= KEY_LBUTTON;
	if (pKeysBuffer[VK_RBUTTON] & 0xF0)wKeyBuffer |= KEY_RBUTTON;
}

void CTcpClient::UpdateZombiePlayer()
{
	shared_ptr<CZombiePlayer> pZombiePlayer = dynamic_pointer_cast<CZombiePlayer>(m_apPlayers[0]);
	if (!pZombiePlayer)
	{
		return;
	}

	for (int i = 0; i < MAX_CLIENT; ++i)
	{
		if (m_nMainClientId == ZOMBIEPLAYER)	// 추적
		{
			if (m_aClientInfo[0].m_nSlotObjectNum[0] == 1)
			{
				m_apPlayers[i]->SetTracking(true);
			}
			else
			{
				m_apPlayers[i]->SetTracking(false);
			}
		}

		if (m_apPlayers[i]->GetClientId() != m_nMainClientId || i == ZOMBIEPLAYER)
		{
			continue;
		}
		if (m_aClientInfo[0].m_nSlotObjectNum[1] == 1)
		{
			m_apPlayers[i]->SetInterruption(true);
		}
		else
		{
			m_apPlayers[i]->SetInterruption(false);
		}
	}

	// 시야 방해(zombie 플레이어)
	if (m_nMainClientId == ZOMBIEPLAYER)
	{
		if (m_aClientInfo[0].m_nSlotObjectNum[1] == 1)
		{
			m_apPlayers[0]->SetInterruption(true);
		}
		else
		{
			m_apPlayers[0]->SetInterruption(false);
		}
	}

	if (m_aClientInfo[0].m_nSlotObjectNum[2] == 1)	// 공격을 시도
	{
		pZombiePlayer->m_pSkinnedAnimationController->SetTrackEnable(2, true);
	}
}

void CTcpClient::UpdatePlayer(int nIndex)
{
	shared_ptr<CBlueSuitPlayer> pBlueSuitPlayer = dynamic_pointer_cast<CBlueSuitPlayer>(m_apPlayers[nIndex]);

	if (!pBlueSuitPlayer) // 생존자가 아니면 수행 x
	{
		return;
	}

	if (m_nEscapeDoor == -1)
	{
		m_nEscapeDoor = m_aClientInfo[nIndex].m_playerInfo.m_iEscapeDoor;
	}
	if (m_nEscapeDoor != -1)
	{
		shared_ptr<CGameObject> pGameObject = g_collisionManager.GetCollisionObjectWithNumber(m_nEscapeDoor).lock();
		pBlueSuitPlayer->SetEscapePos(pGameObject->GetPosition());
	}

	pBlueSuitPlayer->SelectItem(m_aClientInfo[nIndex].m_playerInfo.m_selectItem);
	for (int j = 0; j < 3; ++j)
	{
		if (m_aClientInfo[nIndex].m_nSlotObjectNum[j] != -1)
		{
			shared_ptr<CGameObject> pGameObject = g_collisionManager.GetCollisionObjectWithNumber(m_aClientInfo[nIndex].m_nSlotObjectNum[j]).lock();
			shared_ptr<CItemObject> pItemObject = dynamic_pointer_cast<CItemObject>(pGameObject);
			if (pItemObject)
			{
				if (!pItemObject->IsObtained())
				{
					// 아이템을 획득한 순간
					sharedobject.EnableItemGetParticle(pItemObject);
				}
				pItemObject->SetObtain(true);
				if (nIndex == m_nMainClientId && !pBlueSuitPlayer->IsSlotItemObtain(j))
				{
					SoundManager& soundManager = soundManager.GetInstance();
					soundManager.PlaySoundWithName(sound::GET_ITEM_BLUESUIT);
				}
				pBlueSuitPlayer->SetSlotItem(j, m_aClientInfo[nIndex].m_nSlotObjectNum[j]);
			}
		}
		else // -1을 받았는데 플레이어가 가진 Reference값이 -1이 아닌 경우를 생각해야함
		{
			if (pBlueSuitPlayer->GetReferenceSlotItemNum(j) != -1)
			{
				shared_ptr<CGameObject> pGameObject = g_collisionManager.GetCollisionObjectWithNumber(pBlueSuitPlayer->GetReferenceSlotItemNum(j)).lock();
				shared_ptr<CItemObject> pItemObject = dynamic_pointer_cast<CItemObject>(pGameObject);
				if (pItemObject)
				{
					pItemObject->SetObtain(false);
					pBlueSuitPlayer->SetSlotItemEmpty(j);
				}
			}
		}
	}

	for (int j = 0; j < 3; ++j)
	{
		if (m_aClientInfo[nIndex].m_nFuseObjectNum[j] != -1)
		{
			shared_ptr<CGameObject> pGameObject = g_collisionManager.GetCollisionObjectWithNumber(m_aClientInfo[nIndex].m_nFuseObjectNum[j]).lock();
			shared_ptr<CItemObject> pItemObject = dynamic_pointer_cast<CItemObject>(pGameObject);
			if (pItemObject) {
				if (!pItemObject->IsObtained())
				{
					// 아이템을 획득한 순간
					sharedobject.EnableItemGetParticle(pItemObject);
				}
				pItemObject->SetObtain(true);
				if (nIndex == m_nMainClientId && !pBlueSuitPlayer->IsFuseObtain(j))
				{
					SoundManager& soundManager = soundManager.GetInstance();
					soundManager.PlaySoundWithName(sound::GET_ITEM_BLUESUIT);
				}
				pBlueSuitPlayer->SetFuseItem(j, m_aClientInfo[nIndex].m_nFuseObjectNum[j]);
			}
		}
		else // -1을 받았는데 플레이어가 가진 Reference값이 -1이 아닌 경우를 생각해야함
		{
			if (pBlueSuitPlayer->GetReferenceFuseItemNum(j) != -1)
			{
				shared_ptr<CGameObject> pGameObject = g_collisionManager.GetCollisionObjectWithNumber(pBlueSuitPlayer->GetReferenceFuseItemNum(j)).lock();
				shared_ptr<CItemObject> pItemObject = dynamic_pointer_cast<CItemObject>(pGameObject);
				if (pItemObject)
				{
					pItemObject->SetObtain(false);
					pBlueSuitPlayer->SetFuseItemEmpty(j);
				}
			}
		}
	}

	if (m_aClientInfo[nIndex].m_playerInfo.m_bAttacked) {
		pBlueSuitPlayer->SetHitEvent();
	}

	if (m_aClientInfo[nIndex].m_playerInfo.m_bTeleportItemUse) {
		pBlueSuitPlayer->Teleport();
	}
}

void CTcpClient::LoadCompleteSend()
{
	SubmitSendData(static_cast<INT8>(SOCKET_STATE::SEND_LOADING_COMPLETE));
}

void ConvertLPWSTRToChar(LPWSTR lpwstr, char* dest, int destSize)
{
	// WideCharToMultiByte 함수를 사용하여 LPWSTR을 char*로 변환
	WideCharToMultiByte(
		CP_UTF8,
		0,                   // 변환 옵션
		lpwstr,              // 변환할 유니코드 문자열
		-1,                  // 자동으로 문자열 길이 계산
		dest,                // 대상 버퍼
		destSize,            // 대상 버퍼의 크기
		NULL,                // 기본 문자 사용 안 함
		NULL                 // 기본 문자 사용 여부를 저장할 변수의 주소
	);
}

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

