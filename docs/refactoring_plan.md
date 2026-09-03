# Refactoring Roadmap

이 문서는 Nightmare Lab에서 **현재 무엇을 우선할지** 관리한다.
완료된 구현 과정과 측정 기록은 반복하지 않고 관련 문서에 연결한다.

## 목표

- 현재 동작을 보존하면서 클라이언트의 책임 경계와 렌더링 구조를 개선한다.
- DirectX 12 렌더링 경로에 섞인 D3D11On12·Direct2D·DirectWrite 문자 출력을 교체하고
  DX11 계열 코드와 빌드 의존성을 모두 제거한다.
- 큰 파일과 클래스는 크기 자체가 아니라 섞여 있는 책임과 변경 위험을 기준으로 나눈다.
- 그래픽 품질 개선은 구조와 수명 경계를 안정시킨 뒤 작은 단위로 진행한다.
- 서버는 새 문제가 관찰될 때만 안정화 및 부하 검증을 재개한다.

## 작업 원칙

- 기존 동작 보존과 작은 변경 단위를 우선한다.
- 분석과 책임 경계 합의 후 리팩터링을 구현한다.
- 포맷 변경, 파일 이동, 동작 변경은 가능한 한 분리한다.
- 레거시 코드는 실제로 수정하는 범위부터 컨벤션을 적용한다.
- 파일 크기, raw pointer 또는 전역 상태가 있다는 이유만으로 일괄 변경하지 않는다.
- 디렉토리 재구성은 모듈 경계를 코드에서 먼저 확인한 뒤 별도 작업으로 수행한다.
- 성능 문제는 변경 전후를 측정할 수 있을 때만 최적화한다.
- 외부 및 자동 생성 파일은 수정하지 않는다:
  `d3dx12.h`, `DDSTextureLoader12.*`, `Resource.h`, `targetver.h`.

## 현재 상태

- 현재 추적 중인 재현 가능한 치명적 버그는 없다.
- 서버 TCP partial I/O, 연결 종료, 입력 검증과 송신 큐의 핵심 안정화는 완료했다.
- 프레임 종속 고정 크기 상태 복제를 플레이어 상태와 오브젝트별 event/snapshot으로 교체했다.
- 상태 복제 구조 개선 후 Release 로컬 3인 측정에서 서버 TX는 변경 전보다 96.57% 감소했다.
- 서버 월드 구성은 `ServerWorldBuilder`로 분리했다.
- SSAO의 비결정적 결과와 원형 밴딩을 수정하고 bilateral blur 경로를 구성했다.
- `GameFramework`와 네트워크의 일부 역의존 및 공통 상수·메시지 헤더 의존을 정리했다.
- 서버 추가 분리와 최대 인원·느린 네트워크 검증은 현재 보류한다.

상세 구현과 측정 근거는 [관련 문서](#관련-문서)에서 관리한다.

## 현재 우선순위

### P1. DX11 계열 문자 출력 완전 제거

현재 `CGameFramework`는 D3D11On12, Direct2D, DirectWrite를 사용해 DX12 swap-chain의
wrapped back buffer에 문자를 그린다. 이 경로는 다음 UI를 담당한다.

- 레이더 아이템의 탈출구 거리
- 생존자 게임 시작 안내
- 좀비 게임 시작 카운트다운 및 목표 안내

문자 출력은 DX12 command list 실행 후 매 프레임 wrapped resource를 획득·반환하고
D3D11 device context를 `Flush()`한다. swap-chain resize도 D3D11On12/D2D 리소스의
해제와 재생성에 결합되어 있다. 일부 카운트다운 상태 갱신도 `RenderTextUI()` 안에 있어
렌더링과 게임 로직의 책임이 섞여 있다.

가장 작은 단계부터 다음 순서로 진행한다.

1. 표시 항목, 입력 데이터, 표시 조건과 현재 결과를 기록한다. — 코드 기준선 기록 완료
2. 카운트다운 시간과 상태 변경을 프레임 업데이트 경로로 옮기고 문자 출력은 읽기만 하게 한다.
   — 구현 완료, 생존자·좀비·레이더 문자 출력과 게임 시작 흐름 실행 확인
3. `UiOverlayFrameData`로 UI 종류, 숫자 값, 픽셀 위치·크기, 색상·투명도와 표시 여부를
   기록한다. — 표시 데이터 경계 구현 완료, 기존 D2D 출력은 비교 기준으로 유지
4. 고정 한글 안내 DDS와 카운트다운·거리 glyph atlas/DDS·FNT를 준비한다.
   — 로컬 리소스 준비 완료, 기존 `Asset/` ignore 정책 유지
5. full-screen 처리 이후 DX12 overlay pass를 추가하고 문자 표시 후 back buffer를
   `RENDER_TARGET`에서 `PRESENT`로 명시적으로 전환한다.
   — `UiOverlayRenderer`의 DDS/FNT·glyph metric, 전용 overlay HLSL/PSO, CPU 동적 quad와 draw command 연결 완료
   — `layoutSize`는 중앙 정렬 영역으로만 사용하고 고정 문구와 glyph quad는 리소스 원본 크기를 사용
   — 전환 플래그 기본값은 실행 확인된 DX12이며, DX12와 D2D 경로는 같은 프레임에 실행되지 않음
6. 결과 비교 후 D3D11On12, Direct2D, DirectWrite와 wrapped back buffer 코드를 제거한다.
7. D3D11/D2D/DirectWrite 관련 헤더 include와 링크 라이브러리를 제거한다.
8. 코드베이스와 프로젝트 설정에 DX11 계열 의존성이 남지 않았는지 확인한다.
9. resize, 씬 전환과 인게임 종료 흐름을 다시 검증한다.

완료 기준:

- 기존 거리와 안내 문구가 같은 조건에서 표시된다.
- 문자 렌더링 함수가 타이머나 게임 상태를 변경하지 않는다.
- 프레임 경로에서 D3D11On12 acquire/release와 D3D11 `Flush()`가 제거된다.
- D3D11 device/context, D3D11On12 device, wrapped back buffer와 D2D/DirectWrite 리소스가 제거된다.
- `d3d11on12.h`, `d2d1_3.h`, `dwrite.h` 및 관련 include가 제거된다.
- `d3d11.lib`, `d2d1.lib`, `dwrite.lib` 링크 의존성이 제거된다.
- 소스와 프로젝트 설정 검색에서 의도하지 않은 DX11 계열 참조가 발견되지 않는다.
- resize, 로비→인게임 전환, 승패와 종료가 정상 동작한다.

### P2. 클라이언트 코드 구조 개선

P1을 첫 번째 실제 사례로 삼아 클라이언트의 책임 경계를 점진적으로 정리한다.

진행 순서:

1. `GameFramework`–`Scene`–렌더 패스–UI 사이의 책임과 데이터 흐름을 명확히 한다.
2. 실제 수정이 잦은 긴 함수와 코드 덩이를 동작 단계 기준의 private 함수로 분리한다.
3. `Shader.cpp`, `Scene.cpp`, `Object.cpp`는 책임 경계가 확인된 구현부터 별도 `.cpp`로 나눈다.
4. 클라이언트 패킷은 실제 수정이 필요한 종류부터 파싱과 게임 상태 적용을 분리한다.
5. 수정하는 모듈부터 직접 include와 코딩 컨벤션을 적용한다.
6. 파일 분리로 모듈 경계를 검증한 뒤 디렉토리 재구성을 별도 작업으로 수행한다.

제약:

- `CMainScene` 렌더링 파이프라인과 public API를 한 번에 재설계하지 않는다.
- 파일 크기만을 근거로 클래스를 분리하지 않는다.
- 전체 헤더, 네이밍, 포맷을 일괄 변경하지 않는다.
- 서버 책임 추가 분리는 서버 코드의 실제 변경 필요가 생길 때 재개한다.

### P3. 안정성 및 회귀 검증

별도 대규모 리팩터링보다 P1/P2 변경마다 함께 확인한다.

- 씬 전환, 연결 종료와 게임 종료 중 객체 수명
- swap-chain resize와 GPU 리소스 수명
- command list `Reset()` 이후 root signature와 descriptor heap 상태
- DirectX debug layer 오류 및 경고
- Client/Server 대상 구성 빌드
- 로비→인게임→승패→종료 smoke test
- 생존자·좀비의 문자 UI와 레이더 거리 표시
- SSAO `Disabled`/`Raw`/`Blurred` 결과

### P4. 그래픽 품질 개선 — 후순위

현재 SSAO와 blur의 주요 결함은 개선됐으며, 그림자도 즉시 해결해야 할 버그로 분류하지 않는다.
구조 개선 이후 시각적 결함이나 품질 목표가 구체화되면 다음 순서로 검토한다.

1. shadow map 디버그 출력과 아티팩트 기준선
2. depth/normal bias와 projection 범위
3. 그림자 필터링과 해상도
4. 병목이 측정된 경우에만 렌더링 횟수와 대상 조명 최적화
5. SSAO/blur 품질 단계와 material AO

## 보류 항목과 재개 조건

| 항목 | 재개 조건 |
|---|---|
| 최대 5인·느린 네트워크·강제 partial I/O | 큐 증가, 전송 주기 불안정 또는 연결 문제가 관찰될 때 |
| 서버 tick 및 CPU 측정 | tick 지연이나 CPU 점유 문제가 관찰될 때 |
| 입력 전송을 렌더 루프 밖으로 이동 | 낮은 FPS에서 조작 문제가 재현될 때 |
| 패킷 wire 직렬화와 protocol version | 패킷 스키마 변경 요구가 생길 때 |
| 그림자 렌더링 최적화 | 그림자 패스가 병목으로 측정될 때 |
| SSAO/blur GPU 시간 측정 | 프레임 저하 또는 품질 옵션 요구가 생길 때 |
| GPU 프레임 동기화 개선 | `WaitForGpuComplete`가 병목으로 확인될 때 |
| 디렉토리 재구성 | 모듈 경계가 코드 분리로 검증되고 별도 작업으로 승인될 때 |
| 대규모 API·네이밍 변경 | 작은 변경으로 해결할 수 없고 별도 작업으로 승인될 때 |

## 완료된 핵심 작업

- 좀비 단독 접속 시 발생하던 조명 및 SSAO 문제 수정
- SSAO `Disabled`/`Raw`/`Blurred` 비교 경로와 bilateral blur 구성
- Scene 선언 구조와 command list 공통 상태 설정 정리
- TCP partial recv/send, 연결 종료와 `FD_WRITE` 재개 경로 안정화
- 클라이언트·서버 패킷 값과 인덱스 검증
- 입력과 서버 상태 복제를 최대 60 Hz의 독립 주기로 분리
- 고정 `UPDATE_DATA`와 대상 없는 `NEARBY_OBJECTS` 제거
- 문·서랍과 아이템을 event/snapshot 기반 동기화로 전환
- 서버 네트워크 통계와 `ServerWorldBuilder` 분리
- 공통 인원 제한, Win32 메시지 ID와 일부 클라이언트 역의존 정리

## 관련 문서

- `docs/architecture.md`: 현재 시스템 책임과 고위험 의존성
- `docs/pipeline.md`: 클라이언트/서버 런타임 및 네트워크 파이프라인
- `docs/code_smells.md`: 활성 위험, 기술 부채와 해결된 항목
- `docs/ssao.md`: SSAO와 blur 구현 및 검증 기준
- `docs/memo/text_rendering.md`: 현재 D3D11On12 문자 출력과 DX12 대체 방향
- `docs/memo/network_traffic_comparison.md`: 네트워크 전송량 비교
- `docs/memo/network_protocol_draft.md`: 패킷 구조와 필드 검토
- `docs/memo/server.md`: 서버 조사 및 구현 기록
- `docs/coding_convention.md`: 수정 범위에 적용할 코드 규칙
- `docs/git_convention.md`: 브랜치와 커밋 규칙

## 공통 검증 기준

- Client와 Server가 대상 구성에서 빌드되어야 한다.
- 서버 접속, 로비 진입, 게임 시작, 승패와 종료 흐름이 유지되어야 한다.
- 버그 수정은 재현 조건과 수정 후 결과를 기록한다.
- 성능 개선은 변경 전후 측정값이 있을 때만 완료로 판단한다.
- 구조 변경은 관련 문서의 책임과 파이프라인 설명을 함께 갱신한다.
- 각 작업은 독립적으로 검토하고 되돌릴 수 있는 커밋 단위로 유지한다.
