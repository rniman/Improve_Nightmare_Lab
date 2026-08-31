# 아키텍처 초안

## 시스템 개요
Nightmare Lab은 Win32 기반 멀티플레이어 게임으로, 두 개의 실행 파일로 구성된다:
- **클라이언트** (`Client/Client`): 렌더링(DirectX 12), 로컬 입력, 씬 제어, 오디오, 클라이언트 네트워킹
- **서버** (`Client/Server`): 권한 있는 시뮬레이션, 충돌/게임 규칙 갱신, 상태 복제

상위 수준 런타임 흐름:
1. 클라이언트는 `wWinMain`에서 시작하여 연결 UI를 생성한 뒤, 서버 연결 후 프레임 루프에 진입한다.
2. `CGameFramework::FrameAdvance()`가 매 프레임마다 입력/업데이트/렌더/프레젠트를 구동한다.
3. 서버는 소켓 이벤트를 위한 Win32 메시지 루프를 실행하고, 유휴 시 시뮬레이션 루프를 수행한다.
4. 서버가 복제된 상태를 전송하면, 클라이언트가 이를 플레이어/월드에 적용하고 렌더링한다.

## 핵심 모듈
### 1) 애플리케이션 / 프레임워크
- `CGameFramework`는 윈도우 생명주기, DX12 디바이스/스왑체인/커맨드 리스트 설정, 씬 전환, 입력 라우팅, 소켓 메시지 라우팅을 관리한다.
- 현재는 D3D11On12·Direct2D·DirectWrite 문자 출력 리소스와 swap-chain wrapped back buffer도 소유한다.

### 2) 씬 / 게임 로직
- `CScene`은 기본 인터페이스이다.
- `CLobbyScene`은 로비 UI 흐름을 처리한다.
- `CMainScene`은 월드 오브젝트 설정, 프레임별 씬 애니메이션, 렌더 패스 오케스트레이션을 처리한다.
- `CMainScene`의 렌더 리소스와 SSAO 모드 상태는 내부에서 소유하며, `CGameFramework`는 렌더 패스 API와 제한된 접근자를 통해 사용한다.

### 3) 렌더러
- `CShader` 계층 구조가 파이프라인 상태와 드로우 동작을 캡슐화한다.
- 렌더 단계는 섀도우 패스, 메인/디퍼드 패스, 후처리, 블러 컴퓨트, 포워드/UI 패스, 풀스크린 합성으로 구성된다.
- command list 공통 상태인 graphics/compute root signature와 CBV/SRV/UAV descriptor heap은 각 `Reset()` 직후 한 번 설정한다.
- 문자 UI는 DX12 패스 실행 후 D3D11On12 wrapped back buffer를 통해 D2D로 그린다. 레이더 거리와 게임 시작 안내를 표시하며, DX12 UI로 교체한 뒤 D3D11On12·D2D·DirectWrite 코드와 빌드 의존성을 모두 제거할 최우선 정리 대상이다.

### 4) 리소스 관리
- `CTexture`, `CMaterial`, `CGameObject`가 런타임 리소스, 모델 로딩, 오브젝트별 셰이더 데이터를 관리한다.
- 디스크립터 힙 할당 헬퍼는 `CScene` 정적 멤버에 중앙 집중되어 있다.

### 5) 네트워크 / 서버 상호작용
- 클라이언트: `CTcpClient`가 비동기 소켓 I/O, 패킷 파싱, 복제 상태 적용을 처리한다.
- 서버: `TCPServer`가 accept/read/write/close 이벤트, 플레이어/월드 시뮬레이션, 패킷 브로드캐스팅을 처리한다.
- 서버 월드 구성: `ServerWorldBuilder`가 서버 씬 파일 로딩, 프레임별 오브젝트 생성, 탈출문 선택과 초기 아이템 배치를 담당한다.
- 서버 네트워크 측정값의 누적, 구간 초기화와 콘솔 출력은 `SocketNetworkStatistics` 및 `ServerNetworkStatisticsReporter`가 담당한다.

현재 서버는 최대 5명의 클라이언트를 전제로 한 단일 스레드 `WSAAsyncSelect` 구조다.
작은 규모에서 상태 공유와 동기화 복잡도를 줄이는 선택으로는 유효하며, 현재 단계에서
IOCP나 다중 스레드 서버로 교체할 필요는 없다. 다만 이 선택과 별개로 TCP 스트림의
partial send/recv, 송신 대기 데이터 보존, 입력값 검증은 보장되어야 한다.

## 의존성 개요
개념적 의존성 맵:
```text
Client.cpp
  -> CGameFramework
      -> 씬 (CScene / CLobbyScene / CMainScene)
      -> 렌더러 (Shader/Object/Camera/Mesh)
      -> D3D11On12 / D2D / DirectWrite 문자 UI
      -> CTcpClient
      -> SoundManager / Timer / CollisionManager

Server.cpp
  -> TCPServer
      -> ServerWorldBuilder -> ServerObject / ServerCollisionManager
      -> ServerPlayer / ServerCollisionManager
      -> 소켓 상태 머신 + 복제 패킷
      -> ServerNetworkStatisticsReporter
```

주요 결합 지점:
- 공통 인원 제한은 `GameLimits.h`가 소유하며 이를 사용하는 Client/Server 헤더가 직접 참조한다.
- 키 입력은 `GameFramework`가 `CTcpClient::SendInputIfDue()`에 명시적으로 전달하며,
  `TCPClient.cpp`는 `GameFramework` 정적 상태에 의존하지 않는다.
- 클라이언트와 서버의 Win32 메시지 ID는 공통 `WindowMessages.h`가 네임스페이스별로 소유하며,
  메시지를 발신하거나 처리하는 구현 파일이 직접 참조한다.
- 전역/싱글턴 방식의 접근이 일반적이다 (`g_collisionManager`, `gGameTimer`, `SharedObject`, `SoundManager`).

## 알려진 아키텍처 이슈
1. **과도한 책임 집중**
   - `CGameFramework`, `CMainScene`, `TCPServer` 각각이 여러 관심사를 혼합하고 있다 (오케스트레이션 + 도메인 로직 + 인프라).
   - `TCPServer`의 월드 구성은 `ServerWorldBuilder`로 분리됐지만, 패킷 파싱과 상태 적용,
     연결/송신, 복제 및 게임 세션 책임은 아직 함께 남아 있다.

2. **모듈 간 강한 결합**
   - 네트워킹, 씬, 프레임워크 레이어가 직접적으로 상호 연결되어 있어 변경 영향 범위가 크다.

3. **암묵적 공유 상태**
   - 전역/싱글턴 상태와 정적 디스크립터/공유 컨테이너가 소유권 경계를 숨기고 있다.

4. **프로토콜과 게임플레이 로직의 얽힘**
   - 네트워크 핸들러가 게임플레이 변경을 직접 적용하므로, 패킷/스키마 변경이 위험하다.

5. **이벤트 기반 상태 머신의 복잡성**
   - 애플리케이션 송신 요청과 `FD_WRITE` 알림은 분리했지만, 패킷 선택을 위한 소켓 상태와 Win32 비동기 이벤트의 시퀀싱은 여전히 `TCPServer` 내부에 함께 존재한다.

6. **상태 복제 주기와 패킷 크기**
   - 입력은 108 byte `KEYS_BUFFER`로 렌더 루프에서 `steady_clock` 기준 최대 60 Hz로 제한하고, 서버 상태 복제는 입력 수신과 분리된 `steady_clock` 기준 최대 60 Hz로 실행한다. 서버는 모든 활성 클라이언트의 로딩 완료 이후에만 최신 상태를 복제한다.
   - 플레이어 상태는 461 byte `PLAYER_STATE`로 최대 60 Hz 전송한다. 문·서랍과 아이템은 상태 변경 event와 로딩 완료 후 snapshot으로 동기화하며, 기존 `UPDATE_DATA`와 `NEARBY_OBJECTS` head는 제거됐다.

7. **DX12 렌더링과 D3D11On12 문자 UI 혼용**
   - `CGameFramework`가 D3D11On12·D2D·DirectWrite 초기화, wrapped back buffer, resize 재생성과 프레임별 acquire/release를 담당한다.
   - `CBlueSuitPlayer::RenderTextUI()`와 `CZombiePlayer::RenderTextUI()`는 출력뿐 아니라 일부 게임 시작 카운트다운 상태도 변경한다.
   - 먼저 상태 갱신을 프레임 업데이트로 옮기고 출력 책임을 작은 UI 경계로 격리한 뒤 DX12 기반 렌더링으로 교체한다.
   - 교체 완료 후 D3D11 device/context, D3D11On12, D2D, DirectWrite, wrapped back buffer와 관련 헤더·링크 라이브러리를 모두 제거한다.

## 의존성 고위험 영역
1. **GameFramework ↔ Scene ↔ TCPClient 경계**
   - 한 레이어의 변경이 프레임 루프, 메시지 처리, 네트워크 업데이트 경로 전반에 파급될 수 있다.

2. **패킷 스키마 및 상태 적용 경로**
   - 클라이언트/서버의 패킷 헤더/상태 열거형과 페이로드 레이아웃이 게임플레이 오브젝트 업데이트에 강하게 결합되어 있다.

3. **MainScene 렌더링 파이프라인 오케스트레이션**
   - 섀도우/메인/후처리/컴퓨트/포워드/풀스크린 패스가 한 곳에서 조율되며, 패스 변경이 여러 모듈에 영향을 줄 수 있다.

4. **충돌 오브젝트 인덱스 기반 참조**
   - 클라이언트와 서버 로직 모두 공유 오브젝트 번호 매기기와 외부 조회 패턴에 의존한다.

5. **정적 공유 렌더링 상태**
   - 씬 관련 코드의 정적 디스크립터/카운터 상태가 숨겨진 씬 간 의존성을 발생시킬 수 있다.

6. **입력·복제 주기와 패킷 크기**
   - 입력 호출점은 렌더 루프에 남아 있어 낮은 FPS에서는 입력 빈도도 낮아진다. 상태 복제는 입력 수신과 분리했으며, 고정 `UPDATE_DATA`는 `PLAYER_STATE`와 오브젝트별 event/snapshot으로 교체했다. `NEARBY_OBJECTS` 제거 후 Release 로컬 2인에서 서버 TX 55,320 byte/s, 120 packet/s와 빈 송신 큐를 확인했으며, 5인·느린 네트워크·강제 partial I/O 검증은 현재 보류 상태다.

7. **swap-chain ↔ D3D11On12/D2D 문자 UI 경계**
   - 매 프레임 DX12 command 실행 이후 wrapped back buffer를 D2D가 획득하고 반환하며 D3D11 context를 `Flush()`한다. resize 시 관련 래퍼와 렌더 타겟을 모두 다시 만들므로 프레임 순서와 swap-chain 수명 변경의 영향 범위가 넓다.

## 점진적 리팩토링 참고 사항
- 동작 보존을 최우선으로 하며, 작고 모듈 내부에 국한된 변경을 선호한다.
- 경계 개선 순서: 프레임워크 ↔ 문자 UI, 프레임워크 ↔ 씬, 씬 ↔ 네트워크, 네트워크 ↔ 게임플레이 적용.
- 디렉토리 재구성은 모듈 경계가 안정된 이후 별도 작업으로 수행한다.
