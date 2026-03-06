# Git Convention

이 문서는 Nightmare Lab 리팩토링 프로젝트의 Git 사용 규칙을 정의합니다.
개인 프로젝트 규모에 맞춰 간결하게 운영합니다.

---

## Commit Message

### 형식

```
<type>(<scope>): <subject>

<body>
```

subject와 body 사이에는 **빈 줄 1개**를 둡니다.
body는 선택이며, 간단한 작업은 subject만 작성합니다.

### 길이 제한

| 구성 요소 | 제한 |
|---|---|
| Subject 전체 | 72자 이내 (50자 권장) |
| Body 한 줄 | 72자 이내 |

### Type

| Type | 용도 |
|---|---|
| feat | 새 기능 추가 |
| fix | 버그 수정 |
| refactor | 동작 변경 없는 코드 구조 개선 |
| perf | 성능 개선 |
| docs | 문서 변경 |
| style | 포맷팅, 세미콜론 등 의미 없는 변경 |
| build | 빌드 설정, 의존성 변경 |
| chore | 기타 유지보수 (.gitignore, 폴더 정리 등) |

### Scope

영향받는 모듈을 소문자 영어로 표기합니다.
명확할 때만 사용하며, 애매하면 생략합니다.

자주 쓰이는 scope 예시:
`renderer`, `scene`, `player`, `shader`, `network`, `sound`, `camera`, `mesh`, `object`

### Subject 작성 규칙

- 영어, 동사 원형으로 시작 (add, fix, extract, remove ...)
- 첫 글자 소문자
- 끝에 마침표 없음
- 명령문 ("added" 가 아닌 "add")

### Body 작성 규칙

- **무엇을(What)** 보다 **왜(Why)** 에 집중
- 리스트 형식은 `-` 로 시작
- 72자 넘으면 직접 줄바꿈

### Breaking Change

호환성을 깨는 변경이 있을 경우:

```
feat!(save): migrate save format from text to binary

BREAKING CHANGE: existing save files are no longer compatible
```

---

## Branch

개인 프로젝트이므로 `main` 단일 브랜치를 기본으로 사용합니다.

작업 규모가 클 때만 브랜치를 분리하고, 완료 후 main에 머지합니다.
브랜치를 main에 통합할 때는 히스토리 정리를 위해 squash merge를 기본으로 사용합니다.

### 브랜치가 필요한 경우

- 여러 파일에 걸친 구조 변경
- 실패 시 되돌리기 어려운 실험적 작업
- 장기간에 걸친 리팩토링

### 브랜치 네이밍

```
<type>/<short-description>
```

- 소문자, kebab-case
- 2~4 단어로 간결하게

| Type | 용도 |
|---|---|
| feature/ | 새 기능 |
| refactor/ | 구조 개선 |
| fix/ | 버그 수정 |
| docs/ | 문서 작업 |
| experiment/ | 실험적 시도 |
참고: 브랜치 prefix `feature/`는 커밋 type `feat`와 대응합니다.

---

## 예시

간단한 작업 (body 생략):
```
chore: add build folder to .gitignore
```

일반적인 리팩토링:
```
refactor(renderer): extract command list recording from Shader

- move draw call logic to Renderer class
- Shader now only manages PSO creation
```

브랜치를 분리하는 큰 작업:
```
branch:   refactor/separate-renderer-responsibility
commit:   refactor(renderer): extract command list recording from Shader
```
