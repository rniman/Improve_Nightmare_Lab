# C++ 코딩 컨벤션

Modern C++ (C++17/20) 기반 범용 코딩 컨벤션입니다.
특정 엔진이나 프레임워크에 종속되지 않으며, 어떤 C++ 프로젝트에든 적용 가능합니다.

플랫폼별 확장(DirectX/COM, ECS 등)은 별도 문서로 관리합니다.

## 목차

1. [파일 및 디렉토리 구조](#1-파일-및-디렉토리-구조)
2. [네이밍 규칙](#2-네이밍-규칙)
3. [코드 스타일](#3-코드-스타일)
4. [상수 정의](#4-상수-정의)
5. [const 사용 원칙](#5-const-사용-원칙)
6. [주석 작성 규칙](#6-주석-작성-규칙)
7. [네임스페이스](#7-네임스페이스)
8. [클래스 멤버 접근](#8-클래스-멤버-접근)
9. [함수 인자 정렬](#9-함수-인자-정렬)
10. [클래스 구성](#10-클래스-구성)
11. [함수 접근 수준](#11-함수-접근-수준)
12. [헤더 파일 include](#12-헤더-파일-include)
13. [전처리 지시자 및 매크로](#13-전처리-지시자-및-매크로)
14. [리소스 수명주기 메서드 네이밍](#14-리소스-수명주기-메서드-네이밍)
15. [안티패턴](#15-안티패턴)

---

## 1. 파일 및 디렉토리 구조

- `.h`, `.cpp` 확장자 사용
- `pch.h`, `pch.cpp` 혹은 'stdafx.h', 'stdafx.cpp' → 프리컴파일 헤더
- 본 저장소는 `stdafx.h`/`stdafx.cpp`를 프리컴파일 헤더로 사용합니다.
- (`pch.h`/`pch.cpp`와 동일한 개념)
- 시스템/모듈 단위 디렉토리 구성 (예: `Audio/`, `Physics/`, `Network/`)
  - 참고: 이 지침은 신규 모듈, 향후 리팩토링, 구조 개편 전용 태스크에 우선 적용합니다.

---

## 2. 네이밍 규칙

### 클래스
- `PascalCase`

### 함수
- `PascalCase`
- **항상 동사로 시작**
- `bool` 반환 함수는 `Is`, `Has`, `Can`, `Should` 등 의미 있는 접두어 사용

### 변수
- 지역 변수: `camelCase`
- 멤버 변수: `m` 접두어 + `PascalCase`
  - 예: `mFrameIndex`, `mCapacity`
- 정적 멤버 변수: `s` 접두어 + `PascalCase`

### 전역 변수
- `g` 접두어 + `PascalCase`
  - 예: `gAllocator`, `gInputSystem`

### 구조체 멤버
- `camelCase`, 접두어 없음

### 열거형
```cpp
enum class ResourceState
{
    Idle,
    Loading,
    Ready,
    Error
};
```
- 열거형 클래스 이름: `PascalCase`
- 열거형 멤버: `PascalCase`
- 열거형 이름 반복 금지 (타입 안전성이 보장되므로)

---

## 3. 코드 스타일

### 기본 스타일
- 중괄호 `{}`는 항상 사용 (한 줄 if문도 예외 없음)
- 포인터는 타입과 붙인다 (`Node* node`)
- `nullptr` 사용, `NULL` 금지
- `auto`는 타입이 명확할 때만 사용
- C 스타일 배열 지양, STL 컨테이너 우선
  - 정적 크기: `std::array`
  - 가변 크기: `std::vector`

### Formatting Rules
- Encoding: `UTF-8`
- Line endings: `LF`
- Indentation: `4 spaces`
- 파일 끝에는 개행 1개를 유지

---

### 변수 초기화

**기본 원칙**: 단순 값은 `=`, 명시적 생성이 필요할 때만 `{}`

| 상황 | 사용 문법 | 예시 |
|------|----------|------|
| **단순 값** | `=` | `int x = 0;` |
| **집합 초기화** | `= {}` | `std::vector<int> v = {1,2,3};` |
| **explicit 생성자** | `{}` | `Allocator alloc{1024};` |
| **복합 객체 배열** | `= {}` + 내부 `{}` | `std::array<T, 3> arr = {T{}, T{}, T{}};` |
```cpp
// 단순 값: = 사용
int count = 0;
float pi = 3.14f;
std::string name = "Engine";

// 멤버 변수
class Renderer
{
private:
    uint32_t mFrameIndex = 0;
    float mDeltaTime = 0.0f;
    bool mInitialized = false;
};

// 집합 초기화: = {} 조합
std::vector<int> values = {1, 2, 3};
std::array<float, 3> position = {0.0f, 1.0f, 0.0f};

// explicit 생성자: {} 사용
class Allocator
{
public:
    explicit Allocator(size_t size);
};
Allocator allocator{1024};  // {} 필요

// 복합 객체 배열
std::array<Endpoint, 3> endpoints = {
    Endpoint{"localhost", 8080},
    Endpoint{"192.168.0.1", 9090},
    Endpoint{"10.0.0.1", 7070},
};
```

**Most Vexing Parse 주의**:
```cpp
// ❌ 함수 선언으로 해석됨!
Widget widget();

// ✅ 해결책
Widget widget = Widget();  // 또는
Widget widget{};
```

---

## 4. 상수 정의

### 기본 원칙
- `#define` 금지
- 컴파일 타임 상수: `constexpr` (전역은 **ALL_CAPS**)
- 런타임 상수: `const`

```cpp
constexpr int MAX_FRAME_COUNT = 3;
constexpr float PI = 3.14159f;
```

### 예외
- 외부 API 매크로는 허용 (`MAX_PATH` 등)
- **상수 정의 용도로 `enum` 사용 금지**

---

## 5. const 사용 원칙

- 읽기 전용 매개변수: `const T&`
- 읽기 전용 멤버 함수: `...() const`
- 불변 지역 변수: `const` 또는 `constexpr`
- 포인터는 대상/자체 불변을 명확히 구분

```cpp
const int* p;        // 대상 불변 (가리키는 값 변경 불가)
int* const p;        // 포인터 자체 불변 (다른 곳 가리킬 수 없음)
const int* const p;  // 둘 다 불변
```

---

## 6. 주석 작성 규칙

### 기본 원칙
1. **Self-documenting code 우선** - 주석보다 명확한 코드가 최선
2. **최소주의** - 코드로 표현 불가능한 것만 주석으로
3. **What보다 Why** - 무엇을 하는지보다 왜 하는지 설명
4. **주석 스타일** - 한 줄 `//`, 블록/헤더 `/* ... */`(짧게), API 문서화는 `///`만 사용
5. **주석 언어** - C/C++ 코드는 한글 주석 허용, `*.hlsl` 주석은 영어(ASCII)만 사용

---

### Public API 문서화 - Doxygen 스타일

**헤더 파일(.h)의 public 인터페이스에만 적용**
```cpp
/// @brief 프레임 임시 데이터를 위한 선형 할당자
/// @param size 할당할 바이트 크기
/// @param alignment 정렬 요구사항 (기본: 16)
/// @return 할당된 메모리 포인터, 실패 시 nullptr
void* Allocate(size_t size, size_t alignment = 16);
```

**필수 태그**: `@brief`, `@param`, `@return`
**선택 태그**: `@note`, `@warning`, `@throws`

**예외**: 다음의 경우 `@param`, `@return` 생략 가능
- 매개변수/반환값 이름이 목적을 완전히 설명하는 경우
- Getter/Setter 같은 자명한 함수

---

### 문서화가 불필요한 경우

다음은 Doxygen 주석을 생략합니다:
```cpp
// 1. 자명한 Getter/Setter
size_t GetCapacity() const { return mCapacity; }
void SetEnabled(bool enabled) { mEnabled = enabled; }

// 2. Override 함수 (기본 클래스에 문서화되어 있는 경우)
void Update() override;

// 3. private 헬퍼 함수 중 이름이 명확한 경우
private:
    void SortByPriority();
    void ClearExpiredEntries();
```

---

### 구현 파일(.cpp) 주석 - // 스타일

**복잡한 로직이나 의도 설명에만 사용**
```cpp
void PhysicsSystem::Update()
{
    // Broadphase 결과를 기반으로 Narrowphase 실행
    mBroadphase.Query(mActiveBodies);

    // CRITICAL: 충돌 응답 순서가 결과에 영향 (결정적 시뮬레이션)
    SortContactsByPenetration();
}
```

**좋은 주석**: 비직관적인 알고리즘, 최적화 이유, 제약사항
**나쁜 주석**: 코드를 그대로 반복하는 설명
```cpp
// ❌ 나쁜 예: 코드 반복
i++;  // i를 1 증가시킨다

// ✅ 좋은 예: 의도 설명
i++;  // 다음 프레임 인덱스로 이동
```

---

### 복잡한 알고리즘 설명
```cpp
// Separating Axis Theorem (SAT)를 이용한 OBB 충돌 검사
// 참고: Real-Time Collision Detection (Christer Ericson) Chapter 4.4
bool CheckOBBCollision(const OBB& a, const OBB& b)
{
    // 15개 분리축 검사 (면 법선 6개 + 엣지 교차 9개)
    for (int i = 0; i < 15; ++i)
    {
        ...
    }
}
```

---

### TODO 태그
```cpp
// TODO: [P1] 다음 스프린트 - 멀티스레드 업데이트 지원
// FIXME: [P0] 즉시 수정 필요 - 메모리 누수 발생 (Issue #42)
// OPTIMIZE: [P2] 성능 개선 - 현재 100ms, 목표 16ms
// HACK: [P2] 임시 해결책 - 라이브러리 버그 우회
// NOTE: 이 함수는 메인 스레드에서만 호출 가능
```

**우선순위**: P0(긴급) > P1(높음) > P2(중간) > P3(낮음)

---

### 디버깅은 로깅 시스템 사용
```cpp
// ❌ 나쁜 예: 주석으로 디버깅
void Update()
{
    // mOffset = 1024
    // mCapacity = 2048
    mOffset += size;
}

// ✅ 좋은 예: 로깅 시스템
void Update()
{
    LOG_TRACE("Allocator: Offset={}, Capacity={}", mOffset, mCapacity);
    mOffset += size;
}
```

---

### 요약

| 위치 | 스타일 | 사용 시점 |
|------|--------|----------|
| **헤더 public** | `/** */` Doxygen | Public API만 |
| **헤더 private** | 주석 생략 | 이름이 명확하면 불필요 |
| **구현 파일** | `//` 일반 주석 | 복잡한 로직, Why 설명 |
| **TODO 태그** | `// TODO:` | 향후 작업 표시 |

---

## 7. 네임스페이스

### 기본 원칙

**헤더 파일 (.h)**: `using namespace` 절대 금지, 명시적 타입 사용 필수
```cpp
// ❌ 금지
using namespace std;

// ✅ 허용
std::vector<int> GetData() const;

// ✅ 타입 별칭 허용
template<typename T>
using UniqueArray = std::unique_ptr<T[]>;
```

**구현 파일 (.cpp)**: `using namespace` 자유롭게 사용 가능

---

### 네임스페이스 구조

모듈별 구분, 중첩은 최대 2단계까지
```cpp
// ✅ 좋은 예
namespace Audio::Mixer { }
namespace Physics::Collision { }

// ❌ 피해야 할 예
namespace Engine::Physics::Collision::Impl::Details { }
```

---

### 익명 네임스페이스

구현 파일 전용 함수/상수 정의에 사용
```cpp
// PhysicsWorld.cpp
namespace Physics
{
    namespace  // 파일 내부에서만 사용
    {
        constexpr uint32_t MAX_CONTACT_POINTS = 8;
        bool IsOverlapping(const AABB& a, const AABB& b);
    }
}
```

---

### 요약

| 위치 | using namespace | 명시적 타입 | 타입 별칭 |
|------|----------------|------------|----------|
| **헤더 (.h)** | 금지 | 필수 | 허용 |
| **구현 (.cpp)** | 허용 | 선택 | 허용 |

**원칙**: 헤더는 명시적으로, 구현은 편하게

---

## 8. 클래스 멤버 접근

### 기본 원칙
- `private` 멤버는 직접 접근 금지
- `SetX()`, `GetX()` 접근자/설정자 사용

```cpp
class AudioSource
{
public:
    void SetVolume(float volume);
    float GetVolume() const;

private:
    float mVolume = 1.0f;
};
```

### 스마트 포인터 내부 접근 허용
```cpp
Mesh* GetMesh() const { return mMesh.get(); }
```

---

## 9. 함수 인자 정렬

### 줄바꿈 기준

다음 중 하나라도 해당하면 인자를 줄바꿈을 **권장**합니다:
1. 인자 4개 이상
2. 한 줄 길이 100자 초과
3. 템플릿 타입 2개 이상 포함

> **Note**: 기준에 해당하더라도 가독성이 좋다면 한 줄 작성 가능

### 줄바꿈 스타일 (단일 패턴)

**모든 인자를 새 줄에 작성**
```cpp
// 함수 선언
void CreateBuffer(
    BufferUsage usage,
    size_t size,
    const void* initialData,
    Buffer** outBuffer
);

// 함수 호출
CreateBuffer(
    BufferUsage::Vertex,
    vertexCount * sizeof(Vertex),
    vertices.data(),
    &mVertexBuffer
);
```

### 줄바꿈 하지 않아도 되는 경우
```cpp
// 인자 4개지만 짧고 명확한 경우
void SetRect(int x, int y, int w, int h);
void SetColor(float r, float g, float b, float a);

// 인자 3개 이하이고 한 줄이 100자 이하
void SetPosition(float x, float y, float z);
void Init(Window* window, Renderer* renderer);

// 타입이 짧고 의미가 명확
bool CheckCollision(const AABB& a, const AABB& b, Vec3& normal);
```

### 반드시 줄바꿈 해야 하는 경우
```cpp
// 복잡한 템플릿이나 긴 타입명
std::unique_ptr<RenderPass> CreateRenderPass(
    const FramebufferDesc& framebuffer,
    std::span<const AttachmentDesc> attachments,
    SubpassDependency dependency
);

// 한 줄이 100자를 명백히 초과
void InitializeGraphicsSystem(
    const WindowDesc& windowDesc,
    const RenderSettings& settings,
    bool enableValidation
);
```

### 판단 기준

**한 줄 작성 가능한 경우:**
- 모든 인자 타입과 이름이 간결함
- 전체 길이가 80-100자 이내
- 의미 파악이 즉시 가능

**줄바꿈 해야 하는 경우:**
- 인자를 읽기 위해 스크롤 필요
- 타입이나 이름이 복잡함
- 비슷한 타입 인자가 많아 구분 어려움

### 예시 비교
```cpp
// OK: 짧고 명확
void SetViewport(float x, float y, float w, float h);

// 권장하지 않음: 스크롤 필요, 가독성 떨어짐
void CreatePipelineState(ShaderProgram* shader, const BlendStateDesc* blendDesc, const RasterizerStateDesc* rasterDesc, PipelineState** outState);

// 권장: 인자별로 명확히 구분
void CreatePipelineState(
    ShaderProgram* shader,
    const BlendStateDesc* blendDesc,
    const RasterizerStateDesc* rasterDesc,
    PipelineState** outState
);
```

### 실전 팁

**일관성 우선**
```cpp
// 같은 클래스 내 비슷한 함수는 같은 스타일 사용
class Renderer
{
public:
    // 둘 다 한 줄 또는 둘 다 여러 줄
    void SetViewport(float x, float y, float w, float h);
    void SetScissor(int x, int y, int w, int h);
};
```

**애매하면 줄바꿈**
- 확신이 서지 않으면 줄바꿈 선택
- 나중에 인자 추가 시 수정 최소화

### if/while 조건문에서 함수 호출

**권장: 조건 변수 추출**
```cpp
// 가장 권장 - 가독성과 디버깅
bool success = mRenderer->Initialize(
    mWindow.get(),
    mConfig.width,
    mConfig.height,
    mConfig.vsync
);

if (!success)
{
    LOG_ERROR("Initialization failed");
    return false;
}
```

### 닫는 괄호 위치

닫는 괄호는 여는 괄호/함수명과 같은 들여쓰기 레벨에 배치합니다.
```cpp
// 함수 선언/정의
void CreateBuffer(
    BufferUsage usage,
    size_t size,
    const void* initialData
);

// 함수 호출
auto transform = Math::CreateScaleMatrix(
    scale.x,
    scale.y,
    scale.z
);

// if문 조건 (조건 변수로 추출 권장)
if (!Initialize(
    window,
    renderer,
    audioSystem
))
{
    return false;
}
```

---

## 10. 클래스 구성

### 접근 지정자 순서
```cpp
public:      // 외부 인터페이스
protected:   // 파생 클래스용
private:     // 내부 구현
```

---

### 각 블록 내 정렬 순서

#### public 블록
1. 타입 정의 (using, enum class)
2. 생성자, 소멸자
3. 복사/이동 연산자 (삭제 포함)
4. 주요 API 함수 (수명 주기 순서: Initialize → Update → Render → Shutdown)
5. Getter / Setter
6. 연산자 오버로딩

#### protected 블록
1. 파생 클래스용 헬퍼 함수
2. 파생 클래스용 멤버 변수

#### private 블록
1. 내부 헬퍼 함수
2. 멤버 변수 (맨 아래)

---

### 기본 템플릿
```cpp
class ClassName
{
public:
    // 1. 타입 정의
    using ResourcePtr = std::shared_ptr<Resource>;
    enum class State { Idle, Active, Paused };
    
    // 2. 생성자/소멸자
    ClassName();
    explicit ClassName(int size);
    ~ClassName();
    
    // 3. 복사/이동
    ClassName(const ClassName&) = delete;
    ClassName& operator=(const ClassName&) = delete;
    ClassName(ClassName&&) noexcept = default;
    ClassName& operator=(ClassName&&) noexcept = default;
    
    // 4. 주요 API (수명 주기 순서)
    bool Initialize();
    void Update(float deltaTime);
    void Render();
    void Shutdown();
    
    // 5. Getter/Setter
    int GetSize() const { return mSize; }
    void SetSize(int size) { mSize = size; }
    
    // 6. 연산자
    bool operator==(const ClassName& other) const;

protected:
    // 파생 클래스용
    void ProtectedHelper();
    int mProtectedValue = 0;

private:
    // 내부 헬퍼
    void InitializeInternal();
    
    // 멤버 변수 (맨 아래)
    int mSize = 0;
    State mState = State::Idle;
    std::vector<ResourcePtr> mResources;
};
```

---

### 멤버 변수 정렬

**우선순위**: 크기가 큰 것부터 → 자주 함께 사용되는 것끼리 그룹화
```cpp
class Example
{
private:
    // 스마트 포인터/컨테이너 (큰 객체)
    std::unique_ptr<PhysicsWorld> mPhysicsWorld;
    std::unique_ptr<AudioEngine> mAudioEngine;
    
    // STL 컨테이너
    std::vector<Entity> mEntities;
    std::string mName;
    
    // 기본 타입
    uint64_t mFrameCount = 0;
    float mDeltaTime = 0.0f;
    int mWidth = 0;
    int mHeight = 0;
    
    // bool은 맨 아래 (1바이트)
    bool mInitialized = false;
    bool mEnabled = true;
};
```

---

### 요약

**기본 원칙**:
- public이 먼저 (사용자가 먼저 볼 것)
- 수명 주기 순서대로 (생성 → 사용 → 소멸)
- 멤버 변수는 마지막 (구현 세부사항)

---

## 11. 함수 접근 수준

### 기본 원칙
- 외부 인터페이스: `public`
- 내부 구현 전용: `private`
- 파생 클래스용: `protected`

```cpp
class PhysicsWorld
{
public:
    void StepSimulation(float deltaTime);

private:
    void BroadPhase();
    void NarrowPhase();
    void ResolveContacts();
};
```

---

## 12. 헤더 파일 include

### 기본 원칙
- 헤더에서는 전방 선언(forward declaration) 우선
- **직접 사용하는 헤더는 명시적으로 include** (.h에 있어도 .cpp에서 다시 명시)
- **예외**: 부모 클래스 헤더 + PCH는 생략 가능

---

### include 정렬 순서

**6단계 구조**
1. PCH (`pch.h`)
2. 자기 헤더 (.cpp만)
3. 같은 모듈 헤더
4. 다른 모듈 헤더
5. 서드파티 → 표준 라이브러리

---

### 정렬 규칙

**그룹 구분**
- **10줄 미만**: 정렬 순서만 준수 혹은 추가 빈 줄만으로 구분
- **10줄 이상**: 빈 줄 + 모듈별 주석 추가 (`// Audio`, `// Physics` 등)

**그룹 내**
- 알파벳 순서 정렬

---

### 전방 선언

포인터/참조만 사용 시 헤더에서 전방 선언 우선, 실제 include는 .cpp에서만.
```cpp
// AudioSource.h
class AudioBuffer;  // 전방 선언 (포인터만 사용)
class Mixer;

class AudioSource
{
public:
    void Play(AudioBuffer* buffer);
    void SetMixer(Mixer* mixer);

private:
    Mixer* mMixer = nullptr;
};
```

---

### 요약

| 순서 | 그룹 |
|------|------|
| 0 | PCH |
| 1 | 자기 헤더 (.cpp만) |
| 2 | 같은 모듈 |
| 3 | 다른 모듈 |
| 4 | 서드파티 |
| 5 | 표준 라이브러리 |

**핵심**: 10줄 이상 시 그룹 빈 줄 + 주석, 그룹 내 알파벳 순, 직접 사용하면 명시, 부모 클래스+PCH(또는 공통 헤더) 제외

---

## 13. 전처리 지시자 및 매크로

### 전처리 지시자 위치

**규칙: 항상 맨 왼쪽 (들여쓰기 없음)**
```cpp
// 좋은 예: 전처리문은 맨 왼쪽
#if defined(_DEBUG)
if (condition)
{
    DebugFunction();
}
#endif

// 나쁜 예: 전처리문 들여쓰기
    #if defined(_DEBUG)
    if (condition)
    {
        DebugFunction();
    }
    #endif
```

**이유:**
- 전처리는 컴파일 전 단계로 코드 구조와 별개
- 들여쓰면 오히려 혼란스러움
- 대부분의 IDE가 자동으로 맨 왼쪽 정렬

---

### 매크로 작성 규칙

#### 기본 원칙: inline 함수 우선
```cpp
// 권장: inline 함수
inline void ThrowIfFalse(bool condition, const char* msg)
{
    if (!condition)
    {
        LOG_ERROR("{}", msg);
        throw std::runtime_error(msg);
    }
}

// 사용
ThrowIfFalse(initialized, "System not initialized");
```

**inline 함수의 장점:**
- 타입 안전성
- 디버깅 용이 (스택 추적 가능)
- 네임스페이스 사용 가능
- IDE 자동완성 지원
- 컴파일러 최적화

**매크로가 필요한 경우만 사용:**
```cpp
// __FILE__, __LINE__ 같은 메타 정보 필요
#define LOG_LOCATION(msg) \
    LOG_INFO("[{}:{}] {}", __FILE__, __LINE__, msg)

// 조건부 컴파일
#if defined(_DEBUG)
    #define DEBUG_ONLY(x) x
#else
    #define DEBUG_ONLY(x)
#endif
```

---

### 헤더 가드
- `#pragma once` 사용 (간결하고 에러 방지)
- `#ifndef` 가드 지양 (레거시)

```cpp
// 권장
#pragma once

// 지양
#ifndef MY_HEADER_H
#define MY_HEADER_H
// ...
#endif
```

---

### 매크로 네이밍
- 함수형 매크로: `ALL_CAPS` with underscore
  - 예: `LOG_INFO`, `CORE_ASSERT`, `CHECK_RESULT`
- 조건부 컴파일: `ALL_CAPS`
  - 예: `NDEBUG`, `_DEBUG`, `USE_SIMD`

---

### 매크로 사용 지침
- `#define` 상수 금지 → `constexpr` 사용
- 디버그 전용 매크로는 Release에서 `((void)0)`으로 치환
- 여러 줄 매크로는 `\` 로 정렬

```cpp
// 좋은 예: 여러 줄 매크로 정렬
#define LOG_ERROR(format, ...)              \
    Logger::GetInstance().Log(              \
        LogLevel::Error,                    \
        LogCategory::General,              \
        FormatLog(format, ##__VA_ARGS__),  \
        GetFileName(__FILE__), __LINE__)

// Release 최적화
#ifdef NDEBUG
#define LOG_TRACE(format, ...) ((void)0)
#define LOG_DEBUG(format, ...) ((void)0)
#endif
```

---

## 14. 리소스 수명주기 메서드 네이밍

### 짝 메서드 규칙

| 생성/시작 | 정리/종료 | 용도 |
|----------|----------|------|
| `Initialize()` | `Shutdown()` | 시스템/매니저 수명주기 |
| `Create()` | `Destroy()` | 개별 리소스 생성 |
| `Acquire()` | `Release()` | 풀에서 핸들/참조 획득 |
| `Load()` | `Unload()` | 파일/에셋 로딩 |
| `Open()` | `Close()` | 스트림/연결 |
| `Begin()` | `End()` | 스코프/세션 경계 |

### 단독 메서드

| 메서드 | 용도 |
|--------|------|
| `Reset()` | 상태 초기화, 즉시 재사용 가능 |
| `Clear()` | 컨테이너 내용 비우기 |

### 선택 기준
```cpp
// Initialize/Shutdown: 시스템 전체 수명
class AudioEngine
{
    bool Initialize();
    void Shutdown();
};

// Create/Destroy: 개별 리소스
bool CreateTexture(const std::string& path);
void DestroyTexture(TextureHandle handle);

// Acquire/Release: 풀링된 리소스
ConnectionHandle Acquire();
void Release(ConnectionHandle handle);

// Reset: 재사용 (매 프레임 초기화 등)
bool Reset();

// Clear: 내용만 비움 (STL 의미와 동일)
void Clear();
```

### 필수 규칙
- **짝 메서드는 반드시 쌍으로 구현**
- **Shutdown/Destroy는 멱등성 보장** (여러 번 호출해도 안전)
- **소멸자에서 방어적으로 Shutdown 호출** (누락 방지)

---

## 15. 안티패턴

### 매크로 남용
```cpp
// ❌ 전역 접근 매크로
#define RENDERER Engine::GetInstance().GetRenderer()

// ✅ 명시적 전달
void Init(Renderer* renderer);
```

### 불필요한 스마트 포인터
```cpp
// ❌ 소유권 필요 없는데 shared_ptr
void Draw(const std::shared_ptr<Mesh>& mesh);

// ✅ 참조로 충분
void Draw(const Mesh& mesh);
```

