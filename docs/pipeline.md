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
2. 입력 처리 및 네트워크 전송 요청 (`CTcpClient::RequestSend()`).
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
5. `UpdateInformation()`이 플레이어 상태를 갱신한다.
6. `CreateSendObject()`가 다음 두 작업을 수행한다.
   - 플레이어별 주변 동적 오브젝트를 `SC_UPDATE_INFO`에 기록한다. 이 단계에서는 즉시 송신하지 않는다.
   - 영역에서 이탈한 오브젝트가 있으면 가변 길이 `SEND_SPACEOUT_OBJECTS` 패킷을 직렬화해 각 소켓의 송신 큐에 등록한다.

일반 `SC_UPDATE_INFO` 패킷은 `SimulationLoop()`에서 매 틱 직접 송신하지 않는다.
클라이언트 패킷 처리나 서버 상태 변경으로 `RequestSend()`가 호출될 때 가장 최근에
작성된 복제 상태를 직렬화해 전송한다.

## 5) 네트워킹 이벤트 흐름 (요약)

- 클라이언트와 서버 모두 `WSAAsyncSelect` + Win32 메시지를 사용한다.
- `FD_ACCEPT`: 서버가 연결을 수락하고 클라이언트별 소켓 상태를 준비한다.
- `FD_READ`: 패킷 헤더/페이로드를 수신하고 입력 또는 게임 상태에 반영한다.
- `FD_WRITE`: 소켓이 다시 쓰기 가능한 시점에 대기 중인 송신을 재개하는 이벤트다.
- `FD_CLOSE`: 소켓을 닫고 연결/게임 상태를 업데이트한다.

애플리케이션에서 새 패킷이 필요한 일반 경로는 `RequestSend()`이며, 현재 소켓
상태에 맞는 패킷을 직렬화해 큐에 추가한다. 예외적으로 시뮬레이션에서 발생한 영역
이탈 오브젝트 이벤트는 `CreateSendObject()`가 가변 길이 패킷을 직접 직렬화해
`EnqueueSendBuffer()`로 전달한다. 두 경로 모두 같은 소켓별 큐와
`FlushSendQueue()`를 사용한다.

큐는 패킷 순서와 각 패킷의 전송 위치를 보존하며 즉시 보낼 수 있는 범위까지
전송한다. `send()`가 partial send 또는 `WSAEWOULDBLOCK`을 반환하면 남은 데이터는
큐에 유지하고, Winsock이 전달한 실제 `FD_WRITE` 이벤트에서 전송을 재개한다.
따라서 애플리케이션 송신 요청과 소켓의 쓰기 가능 알림은 서로 다른 경로로 처리된다.

## 6) 서버 네트워크 안정화 전후 비교

이번 변경은 `WSAAsyncSelect`와 기존 패킷 포맷을 유지하면서 TCP 스트림 처리와
연결 수명 관리의 안전성을 보완한 작업이다.

| 구분 | 이전 서버 | 현재 서버 |
|---|---|---|
| 연결 종료 | 오류, `FD_CLOSE`, 접속 실패 경로에서 정리 코드와 상태 변경이 분산됨 | `DisconnectClient()`가 소켓, 슬롯, 플레이어 및 송수신 진행 상태를 한 경로에서 정리하며 반복 호출을 허용함 |
| 수신 완료 판단 | 한 번의 `recv()`가 요청한 헤더 또는 페이로드 전체를 반환한다고 가정하는 경로가 존재함 | 소켓별로 헤더, 기대 크기, 누적 바이트를 보존하고 `Complete`일 때만 패킷을 해석함 |
| 수신 대기 | 미완성 데이터와 `WSAEWOULDBLOCK`의 의미가 호출부마다 불명확함 | `Pending`, `Closed`, `Error`를 구분하고 `Pending`이면 다음 `FD_READ`에서 이어 받음 |
| 송신 버퍼 수명 | 임시 버퍼를 한 번 `send()`한 직후 해제해 partial send의 나머지 데이터를 잃을 수 있음 | 각 소켓의 송신 큐가 버퍼와 전송 위치를 소유하며 완료될 때까지 보존함 |
| 송신 대기 | partial send와 `WSAEWOULDBLOCK` 이후 재개할 상태가 없음 | 보내지 못한 위치부터 실제 `FD_WRITE` 이벤트에서 재개함 |
| 송신 요청 | 새 패킷 요청을 인위적인 `FD_WRITE` 메시지로 전달해 Winsock 알림과 의미가 섞임 | 일반 패킷은 `RequestSend()`, 시뮬레이션 이벤트는 명시적인 큐 등록, `FD_WRITE`는 대기 중인 큐 재개만 담당함 |
| 패킷 생성과 전송 | 패킷별 직렬화 함수가 직접 `send()`를 호출해 오류 및 큐 처리가 반복됨 | 고정 필드는 `SubmitSendData()`, 가변 데이터는 호출부에서 직렬화하고 모두 `EnqueueSendBuffer()`로 큐 등록을 통합함 |
| 대기 데이터 제한 | 명시적인 소켓별 송신 대기량 제한이 없음 | 연결별 대기 송신량을 4 MiB로 제한하고 초과 시 연결을 정리함 |

### 현재 송신 경로

패킷을 생성하거나 송신을 재개하는 진입점은 다음과 같이 구분한다.

| 발생 조건 | 처리 경로 | 결과 |
|---|---|---|
| 클라이언트 패킷 처리 또는 서버 상태 변경 | `RequestSend()` → `SubmitSendData()` | 고정 필드 패킷을 직렬화하고 송신 큐에 등록 |
| `SimulationLoop()`에서 영역 이탈 오브젝트 발견 | `CreateSendObject()` → 가변 패킷 직렬화 | 완성된 패킷을 송신 큐에 등록 |
| `SimulationLoop()`에서 플레이어 주변 오브젝트 갱신 | `CreateSendObject()` → `SC_UPDATE_INFO` 갱신 | 데이터만 기록하며 즉시 송신하지 않음 |
| Winsock의 실제 `FD_WRITE` 알림 | `FlushSendQueue()` | 새 패킷을 만들지 않고 저장된 전송 위치부터 재개 |

패킷이 송신 큐에 등록된 뒤에는 생성 경로와 관계없이 동일하게 처리한다.

1. `EnqueueSendBuffer()`가 소켓과 대기 용량을 검사한다.
2. 패킷 버퍼를 해당 소켓의 큐에 넣고 `FlushSendQueue()`를 호출한다.
3. 전송 결과에 따라 다음과 같이 처리한다.
   - `Complete`: 전송을 마친 패킷을 큐에서 제거한다.
   - `Pending`: 패킷과 전송 위치를 유지하고 `FD_WRITE`를 기다린다.
   - `Error`: `DisconnectClient()`로 연결을 정리한다.

### 유지되는 구조와 남은 제약

- 단일 스레드 Win32 메시지 루프와 `WSAAsyncSelect` 방식은 유지한다.
- 최대 5명, 요청-응답 중심의 게임 상태 흐름과 기존 wire 패킷 값은 유지한다.
- 구조체 메모리를 그대로 복사하는 패킹 방식은 컴파일러 ABI, 정렬 및 양 끝의 동일한
  구조체 정의에 의존한다.
- 패킷 파싱과 게임 상태 적용, 서버 시뮬레이션 책임은 여전히 `TCPServer`에 집중되어 있다.
- 고정 tick, 전송량 절감, IOCP 또는 다중 스레드 전환은 이번 안정화 범위에 포함하지 않는다.

Client/Server x64 Debug 빌드가 성공했으며 서버 실행도 확인했다. 다만 partial
send/recv와 `WSAEWOULDBLOCK`을 의도적으로 발생시키는 네트워크 부하 테스트 및 5인
장시간 접속 검증은 아직 별도 수행 대상이다.

구체적인 결함과 개선 후보는 `code_smells.md`, 작업 순서는 `refactoring_plan.md`에서 관리한다.
