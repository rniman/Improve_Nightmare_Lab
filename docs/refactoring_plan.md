# Refactoring Plan

이 문서는 Nightmare Lab의 점진적 리팩터링 실행 로드맵이다.
목표는 기존 동작을 보존하면서 코드 품질, 안정성, 렌더링 성능, 네트워크 부하를 작은 단위로 개선하는 것이다.

## 목표

- 코드 품질 개선
- 기존 체감 버그 개선
- 렌더링 프레임/품질 개선
- 서버 통신 부하 완화
- 모듈 책임과 경계 점진 개선

## 원칙

- 기존 동작 보존을 우선한다.
- 변경은 작고 검증 가능한 단위로 유지한다.
- 포맷 변경과 동작 변경은 분리한다.
- 인코딩/라인엔딩 변경은 별도 커밋으로 분리한다.
- 레거시 코드는 수정하는 범위부터 컨벤션을 점진 적용한다.
- `.editorconfig`와 `coding_convention.md`를 기준으로 삼되, 대량 포맷팅은 별도 작업으로 수행한다.
- 디렉토리 재구성은 모듈 경계가 안정된 뒤 별도 작업으로 수행한다.
- 성능 개선은 기준선 측정과 병목 확인 후 진행한다.
- 외부/자동 생성 파일은 수정하지 않는다: `d3dx12.h`, `DDSTextureLoader12.*`, `Resource.h`, `targetver.h`.

## 현재 알려진 문제

### 코드 품질

- 큰 파일과 큰 함수가 많아 변경 영향 범위를 추적하기 어렵다.
- 레거시 네이밍, 주석, 인코딩이 혼재되어 있다.
- `NULL`, raw pointer, 수동 `new/delete` 사용이 남아 있다.
- `GameFramework`, `Scene`, `TCPClient`, `TCPServer`에 책임이 집중되어 있다.

### 버그/동작 문제

- 좀비 플레이어 1명만 접속했을 때 조명 렌더링이 누락되는 현상이 있다.
- 일반 플레이어가 추가 접속된 뒤 조명 상태가 정상화되는 듯한 현상이 있다.

### 렌더링/프레임 문제

- shadow, deferred, SSAO, blur 패스의 비용 분석이 필요하다.
- 매 프레임 GPU wait가 프레임 저하를 만드는지 확인이 필요하다.
- D2D text UI, readback 작업의 프레임 영향 확인이 필요하다.

### 네트워크 문제

- 서버 tick마다 전송되는 상태량이 많다.
- 전체 player/object 상태 전송 구조를 점검해야 한다.
- `FD_WRITE`, `PostMessage`, partial send/recv, `WOULDBLOCK` 처리 안정성을 확인해야 한다.

## Phase 0. 프레임워크 재파악 및 기준선 확보

- 클라이언트 실행 흐름을 코드 기준으로 다시 추적한다: `Client.cpp`, `CGameFramework::OnCreate`, `BuildObjects`, `FrameAdvance`.
- 씬 전환 흐름을 확인한다: 접속 UI, 로비, 로딩, 인게임, 종료.
- 렌더링 파이프라인을 다시 추적한다: shadow, deferred/post-processing, SSAO, blur, forward/UI, text UI.
- 네트워크 흐름을 다시 추적한다: `CTcpClient` read/write 처리, `TCPServer::SimulationLoop`, 상태 복제 전송.
- 전역/공유 상태를 확인한다: `g_collisionManager`, `CScene` 정적 디스크립터 핸들, `SoundManager`, `gGameTimer`.
- Client/Server x64 Debug 빌드를 확인한다.
- 서버 실행, 클라이언트 접속, 로비 진입, 게임 시작을 확인한다.
- 좀비 단독 접속 조명 문제의 재현 조건을 기록한다.
- FPS, 프레임 타임, 패킷 크기/빈도 측정 방법을 결정한다.

## Phase 1. 안전성 버그 우선 수정

- 좀비 단독 접속 조명 문제를 분석하고 수정한다.
- player/light camera index 매핑을 검증한다.
- null/range 방어를 추가한다.
- TCP partial send/recv, `WOULDBLOCK` 처리 경로를 점검한다.

## Phase 2. 서버 통신 부하 개선

- 패킷 타입별 크기와 초당 전송 횟수를 측정한다.
- `SC_UPDATE_INFO` 전송량을 분석한다.
- 변경되지 않은 object matrix 전송을 줄인다.
- 전체 상태 전송과 이벤트성 전송을 분리할 수 있는지 검토한다.
- 송신 큐 또는 전송 상태 관리 개선은 측정 후 결정한다.

## Phase 3. 렌더링 병목 개선

- `WaitForGpuComplete`, `MoveToNextFrame` 동기화 구조를 확인한다.
- shadow map 렌더링 횟수와 대상 조명 수를 분석한다.
- SSAO/post-processing 비용을 측정한다.
- blur compute dispatch 비용을 측정한다.
- 품질 옵션화 또는 패스별 최적화는 측정 결과 기반으로 진행한다.

## Phase 4. 모듈 경계 개선

- `GameFramework`의 씬 전환, 렌더링, 네트워크 라우팅 책임을 분리한다.
- `Scene`과 `TCPClient`의 직접 의존을 완화한다.
- 네트워크 패킷 파싱과 게임 상태 적용을 분리한다.
- 렌더링 패스 오케스트레이션을 작은 함수 단위로 정리한다.

## Phase 5. 보류 항목

- 디렉토리 재구성
- 대규모 네이밍 변경
- public API 일괄 변경
- 렌더링 파이프라인 대규모 재설계

## 검증 기준

- `architecture.md`, `pipeline.md`, `coding_convention.md`와 문서 역할이 겹치지 않아야 한다.
- 각 Phase는 실제 작업 단위로 커밋 분리 가능해야 한다.
- 조명 버그, 서버 통신 과부하, 프레임 저하 문제가 계획에 포함되어 있어야 한다.
