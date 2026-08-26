# Code Smells and Technical Debt

이 문서는 현재 확인된 문제와 기술 부채를 근거 및 상태와 함께 관리한다.
항목이 존재한다는 이유만으로 즉시 수정하지 않으며, 우선순위는
`docs/refactoring_plan.md`를 따른다.

## 분류

| 분류 | 의미 |
|---|---|
| `Bug` | 현재 재현되는 잘못된 동작 |
| `Risk` | 크래시, 데이터 손상, 연결 불안정 또는 성능·확장성 저하 가능성이 있으나 사용자 장애로는 아직 재현되지 않은 항목 |
| `Maintainability` | 반복 변경이나 원인 추적을 어렵게 만드는 구조 |
| `Deferred` | 현재 정상 동작하며 의도적으로 후순위에 둔 항목 |
| `Resolved` | 수정 및 기본 검증이 끝난 항목 |

## Active

### Bug

현재 추적 중인 재현 가능한 버그는 없다.

### Risk

| ID | 영역 | 항목 | 근거 및 확인 방법 |
|---|---|---|---|
| R-003 | Lifetime | 씬 전환 및 연결 종료 수명 | 로비에서 인게임 전환, 연결 종료, 게임 종료 경로의 null 접근과 소유 관계를 확인한다. |
| R-004 | Rendering | 렌더 상태 및 리소스 수명 결합 | 씬 전환이나 command list 재설정 시 root signature, descriptor heap, GPU 리소스 수명이 유효한지 변경 범위마다 확인한다. |
| R-005 | Replication | 최대 인원과 부하 조건의 잔여 검증 | 입력·플레이어·주변 오브젝트 책임과 주기를 분리하고 legacy 고정 배열을 제거했다. Release 로컬 3인에서 서버 TX는 `2092acd2`의 평균 2,743,023 byte/s에서 현재 평균 94,067 byte/s로 96.57% 감소했고, 출력 시점 큐 0과 연결별 구간 peak 최대 461 byte/1 packet을 확인했다. 프레임 종속·고정 10KB 복제 위험은 해소됐으며, 5인·느린 네트워크·강제 partial I/O와 30개 제한 도달 여부를 확인할 때까지 검증 위험으로 유지한다. |
| R-006 | Protocol | 클라이언트 방향·카메라 입력 신뢰 | 서버는 `viewMatrix`, `look`, `right`, `up`, `pitch`의 유한성과 right-click action의 0/1 범위를 검사한다. 다만 방향 벡터의 정규화·직교성이나 권위 위치와의 관계는 검사하지 않으므로, 새 입력 wire 구조를 설계하기 전에 허용 범위와 서버 재구성 가능한 필드를 정한다. |

### Maintainability

| ID | 영역 | 항목 | 영향 및 다음 작업 |
|---|---|---|---|
| M-001 | Framework | `CGameFramework` 책임 집중 | 씬 전환, 렌더링, 입력, 네트워크 라우팅을 함께 담당한다. 실제 수정이 발생하는 경로부터 작은 함수로 분리한다. |
| M-002 | Scene/Network | `Scene`과 `TCPClient` 직접 의존 | 렌더링 씬과 네트워크 정의의 결합이 변경 범위를 넓힌다. 게임 상태 전달 경계를 확인한 뒤 점진적으로 완화한다. |
| M-003 | Network | 패킷 파싱과 상태 적용 결합 | 수신 처리에서 프로토콜 해석과 게임 객체 변경이 섞여 있다. 안정성 수정이 필요한 처리부터 두 단계를 분리한다. |
| M-004 | Resource | 일부 raw pointer 및 수동 수명 관리 | 전체 일괄 교체는 하지 않는다. 소유권이 불명확하거나 오류가 재현되는 리소스부터 정리한다. |
| M-005 | Source layout | 큰 클래스와 긴 함수 | 월드 구성은 `ServerWorldBuilder`로 분리했다. 크기 자체를 문제로 보지 않고, `TCPServer`에 남은 패킷 처리·연결/송신·복제·세션 책임 중 반복 수정되는 경로만 동작 단계 기준으로 추출한다. |

## Deferred

| ID | 영역 | 항목 | 재개 조건 |
|---|---|---|---|
| D-001 | Shadow | shadow map 렌더링 최적화 | 그림자 패스가 실제 프레임 병목으로 측정될 때 재개한다. |
| D-002 | SSAO | SSAO 및 blur GPU 시간 측정 | 프레임 저하 또는 품질 옵션 요구가 생길 때 재개한다. |
| D-003 | Rendering | GPU 동기화 구조 개선 | `WaitForGpuComplete`가 병목으로 확인될 때 재개한다. |
| D-004 | UI | D2D text UI 및 readback 비용 분석 | UI가 프레임 저하 원인으로 확인될 때 재개한다. |
| D-005 | Structure | 디렉토리 및 대규모 API 재구성 | 모듈 경계가 안정되고 별도 작업으로 승인될 때 진행한다. |
| D-007 | Simulation | 메시지 부하에 따른 server tick 변동 | tick 지연이나 CPU 점유 문제가 재현되면 고정 tick과 대기 방식을 검토한다. |

## Resolved

| ID | 영역 | 항목 | 결과 |
|---|---|---|---|
| RS-001 | Lighting/SSAO | 좀비 단독 접속 시 조명 및 SSAO 누락 | player/light camera 매핑과 SSAO 처리 경로를 수정하고 실행을 확인했다. |
| RS-002 | SSAO | SSAO 비교 경로 부재 | `Disabled`, `Raw`, `Blurred` 세 상태를 M 키로 전환할 수 있게 했다. |
| RS-003 | Scene | Scene 클래스의 과도한 public 노출 | 접근 범위, 멤버 순서, 제한된 접근자를 정리했다. |
| RS-004 | Rendering | command list 공통 상태의 반복 설정 | `Reset()` 직후 공통 root signature와 descriptor heap을 설정하도록 정리했다. |
| RS-005 | Network | 연결 종료 및 접속 실패 정리 분산 | 소켓 오류, `FD_CLOSE`, 접속 등록 실패를 중복 호출에 안전한 단일 정리 경로로 통합하고 Client/Server x64 Debug 빌드를 확인했다. |
| RS-006 | Network | 슬롯 변경 시 수신 상태 누락 및 입력 인덱스 미검증 | `SocketInfo` 전체 이동·교환으로 수신 상태를 보존하고 slot 범위를 검증했다. 클라이언트 수신 헤더도 객체 멤버로 이동하고 Client/Server x64 Debug 빌드를 확인했다. |
| RS-007 | Network I/O | TCP partial recv 상태 소실 및 미완료 패킷 해석 | 수신 결과를 완료·대기·종료·오류로 구분하고 소켓별 헤더와 누적 바이트를 보존한다. 가변 길이 패킷은 크기와 구조 단위를 검증하며, 완성된 페이로드만 해석하도록 수정하고 Client/Server x64 Debug 빌드를 확인했다. |
| RS-008 | Network I/O | TCP partial send 데이터 유실 및 송신 요청 혼합 | 소켓별 송신 큐와 전송 위치를 유지해 partial send 및 `WSAEWOULDBLOCK` 이후 실제 `FD_WRITE`에서 재개한다. 애플리케이션 송신 요청은 `EnqueuePendingPacket()`로 분리하고 Client/Server x64 Debug 빌드와 서버 실행을 확인했다. |
| RS-009 | Protocol | 외부 패킷 값 및 버퍼 범위 미검증 | 서버는 client head, slot, key mask, transform을 검증하고, 클라이언트는 server head, payload 크기, client/object ID, 개수, transform을 검증한 뒤 상태를 적용한다. 등록되지 않은 head나 스트림을 복구할 수 없는 값은 연결을 종료한다. |
| RS-010 | Network diagnostics | 패킷별 통신량과 큐 상태 측정 부재 | 연결별·패킷 head별 TX/RX byte와 packet, 송신 큐 최고치, `WOULDBLOCK` 횟수를 1초 구간과 연결 전체 수명으로 측정하도록 분리했다. 이 통계로 R-005의 Release 기준선을 확보했다. |
| RS-011 | Network cadence | 입력 수신과 상태 복제 트리거 결합 | `KEYS_BUFFER`의 패킷별 `UPDATE_DATA` 응답을 제거하고 입력 deadline을 현재 시각에서 다시 잡지 않고 고정 간격으로 전진시켰다. Release 60/144/무제한 FPS에서 입력과 상태 복제가 각각 60 packet/s를 유지했고, 30 FPS 입력은 호출 가능한 프레임 수에 따라 30 packet/s였다. 로컬 3인에서도 상태 복제는 연결당 60 packet/s, 큐 0을 유지했으며 별도 사운드 이벤트 전송을 확인했다. |
| RS-012 | Network event | 상태 복제 분리 후 게임 종료 패킷 미전송 | 승리 상태 설정 후 `EnqueuePendingPacket()`가 없어 정기 복제 중단과 함께 종료 패킷도 누락됐다. 상태 전환 시 각 활성 연결에 승리 패킷을 한 번 즉시 등록하도록 수정하고 Release 2인 플레이에서 WIN 패킷 로그를 확인했다. |
| RS-013 | Network buffer | 연결 상태의 대형 인라인 저장소 | 클라이언트와 서버의 65,535 byte 수신 배열을 `vector<char>` 소유로 옮기고 약 16.5 KiB의 소켓 통계 저장소를 `unique_ptr` 소유로 옮겼다. `SocketInfo` 이동·교환의 대형 스택 임시 객체와 관련 경고를 제거하고 Release 2인 실행에서 통신 동작을 확인했다. |
| RS-014 | Replication | 문·서랍의 30 Hz 애니메이션과 관심 영역 진입 시 상태 pop | 문·서랍을 `NEARBY_OBJECTS`에서 제외하고 상태 변경 event를 모든 활성 클라이언트에 전달하며, 모든 클라이언트 로딩 완료 후 현재 상태 snapshot을 한 번 전송한다. 클라이언트는 서버 최종 상태를 적용해 렌더 프레임마다 애니메이션하고 서버의 행렬·충돌 권위는 유지한다. Release 실행에서 자신과 다른 클라이언트의 문·서랍 상태 및 서랍 애니메이션을 확인했다. |
| RS-015 | Server structure | TCPServer의 월드 구성 책임 집중 | 초기화를 네트워크와 월드 단계로 나누고, 씬 로딩·오브젝트 생성·탈출문 선택·아이템 배치와 `mDrawerEntries`를 `ServerWorldBuilder`로 이동했다. 프로젝트 XML 및 diff 정적 검사는 통과했으며 빌드·실행 검증은 후속 대상이다. |
| RS-016 | Server initialization | 월드 구성 실패가 초기화 결과에 반영되지 않음 | `InitializeWorld()`가 성공 여부를 반환하고 `TCPServer::Initialize()`가 이를 전달하도록 정리했다. 월드를 네트워크보다 먼저 구성해 실패 시 리슨 소켓을 열지 않으며, 호출부는 기존대로 창을 닫고 서버 시작을 중단한다. 정적 검사를 수행했으며 빌드·실행 검증은 후속 대상이다. |

## 항목 작성 규칙

- 버그에는 재현 조건을 기록한다.
- 위험 항목에는 확인 방법을 기록한다.
- 성능 항목에는 변경 전후 측정값을 기록한다.
- 해결된 항목에는 관련 커밋과 검증 결과를 추가할 수 있다.
- 우선순위가 바뀌면 삭제하지 않고 분류 또는 상태를 변경한다.
