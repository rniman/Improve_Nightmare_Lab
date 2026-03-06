# 클라이언트/서버 파이프라인

## 1) 시작 흐름

### 클라이언트 시작
1. `wWinMain`이 프로세스를 시작하고 메인 윈도우를 생성한다.
2. `CGameFramework::CreateEntryWindow()`가 연결 UI(IP 입력 + 연결 버튼)를 표시한다.
3. `CTcpClient::CreateSocket()`이 서버에 연결하고 비동기 소켓 이벤트를 등록한다 (`WSAAsyncSelect`).
4. 클라이언트 ID를 수신한 후, `CGameFramework::OnCreate()`가 DX12 디바이스/스왑체인/커맨드 리소스를 초기화한다.
5. `CGameFramework::BuildObjects()`가 로비 씬을 먼저 빌드하고, `WM_START_GAME` 이후 메인 씬을 빌드한다.

### 서버 시작
1. `main`이 메시지 기반 소켓 처리를 위한 Win32 윈도우를 생성한다.
2. `TCPServer::Init()`이 WinSock을 초기화하고, 리슨 소켓을 생성하며, 바인드/리슨 후 `WSAAsyncSelect`를 등록한다.
3. 서버가 충돌/월드 데이터를 로드하고 클라이언트를 대기한다.

## 2) 메인 루프

### 클라이언트 메인 루프
- 메시지 펌프(`PeekMessage/DispatchMessage`)가 윈도우/입력/소켓 메시지를 처리한다.
- 유휴 시, `CGameFramework::FrameAdvance()`가 호출된다.

### 서버 메인 루프
- 메시지 펌프가 `FD_ACCEPT/FD_READ/FD_WRITE/FD_CLOSE`를 처리한다.
- 유휴 시, `TCPServer::SimulationLoop()`이 권한 있는 게임 시뮬레이션을 실행한다.

## 3) 클라이언트 프레임 파이프라인

프레임별 (`CGameFramework::FrameAdvance()`):
1. 타이머 틱 + 사운드 시스템 업데이트.
2. 입력 처리 및 네트워크 전송 트리거 (`WM_SOCKET`/`FD_WRITE` 경로).
3. 씬 및 플레이어 애니메이션.
4. 현재 게임 상태에 대한 커맨드 리스트 빌드/기록.
5. 인게임 렌더 시퀀스:
   - 섀도우 렌더
   - 메인 씬 렌더 + 후처리
   - 블러 컴퓨트 디스패치
   - 포워드/UI 렌더
   - 풀스크린 처리
6. 커맨드 리스트 실행, 텍스트 UI 렌더, 스왑체인 프레젠트.
7. 프레임 동기화 진행 (`WaitForGpuComplete`, `MoveToNextFrame`).

## 4) 서버 시뮬레이션 루프

틱별 (게임 활성 시 `TCPServer::SimulationLoop()`):
1. 서버 타이머 틱.
2. 게임 종료 조건 확인.
3. 각 활성 플레이어 업데이트:
   - 입력/상태 적용 (네트워크에서 수신한 데이터)
   - 아이템/우클릭 로직
   - 이동/업데이트/충돌 처리
4. 충돌 관리자/월드 오브젝트 업데이트.
5. 모든 클라이언트를 위한 복제 상태 빌드 (`SC_UPDATE_INFO`).
6. 상태/이벤트 전송은 소켓 상태머신 기반 `FD_WRITE` 경로(및 일부 직접 전송 경로)를 통해 수행된다.

## 5) 네트워킹 이벤트 흐름 (요약)

- 클라이언트와 서버 모두 `WSAAsyncSelect` + Win32 메시지를 사용한다.
- `FD_READ`: 패킷 헤더/페이로드를 수신하고 파싱한다.
- `FD_WRITE`: 현재 소켓 상태에 따라 패킷을 전송한다.
- `FD_CLOSE`: 소켓을 닫고 연결/게임 상태를 업데이트한다.
