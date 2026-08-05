# SSAO 구현 현황

작성일: 2026-08-03  
최종 갱신: 2026-08-06

## 목적

프로젝트의 SSAO(Screen-Space Ambient Occlusion) 구현 구조, 해결한 문제와
현재 품질 조정 항목을 기록한다.

## 관련 문서

- [SSAO 샘플 방향 배열 동작 확인](troubleshooting/ssao-sample-direction.md): 이전의 비결정적 샘플 방향과
  8x8 노이즈 배열 범위 밖 접근 문제를 기록한다.

## 현재 렌더링 흐름

```text
G-buffer 작성
  → PSGenerateSSAO.hlsl
	  → Raw ambient visibility 생성 (`R16_FLOAT`)
  → CSBlurSSAOHorizontal.hlsl
	  → depth/normal-aware horizontal bilateral blur
  → CSBlurSSAOVertical.hlsl
	  → depth/normal-aware vertical bilateral blur
  → PSPostProcessingWithSSAO.hlsl
	  → blurred ambient visibility를 전역 ambient에 적용해 조명 결과 기록
  → CSComposite.hlsl
	  → fog, bloom, 최종 색상 디더링
  → 화면 출력
```

`CSComposite.hlsl`의 Bayer 디더링은 fog/bloom 등의 최종 LDR 색상 양자화로 인한
밴딩을 완화한다. raw SSAO 결과 자체의 noise나 공간적 패턴을 제거하는 용도는 아니다.

## 개선 경위

SSAO는 원형 밴딩을 완화하고, 이후 남은 고주파 noise를 줄이는 순서로 개선했다.

```text
기존 2D 화면 공간 반경 기반 SSAO
  → 위치 기반 반경 계산으로 원형 밴딩이 드러남
  → 전역 배열 선언과 noise 인덱스 범위 밖 접근으로 결과가 비결정적일 수 있었음

View-space hemisphere SSAO
  → 위치·법선을 view space에서 계산하고 hemisphere kernel을 투영
  → 원형 밴딩과 비결정적 샘플 방향 문제를 완화
  → 제한된 kernel 수와 픽셀별 noise 회전으로 점상 noise는 남음

Depth/normal-aware bilateral blur
  → 같은 표면의 점상 noise를 완화
  → 깊이·법선 경계를 보존해 다른 물체나 배경으로 AO가 번지는 현상을 억제
```

즉, view-space hemisphere 방식은 AO를 생성하는 식의 공간적 패턴을 바로잡은 변경이고,
bilateral blur는 그 결과에 남는 undersampling noise를 후처리로 완화하는 변경이다.

## SSAO에 사용하는 G-buffer

| 입력 | 리소스 | 저장 내용 | 형식 |
| --- | --- | --- | --- |
| 표면 색상 | `DFTextureTexture` | Albedo 등 기본 색상 | `R8G8B8A8_UNORM` |
| 표면 법선 | `DFNormalTexture` | 월드 법선을 `0~1`로 인코딩 | `R8G8B8A8_UNORM` |
| 깊이 | `DFzDepthTexture` | 래스터화된 깊이 값 | `R32_FLOAT` |
| 표면 위치 | `DFPositionTexture` | 월드 공간 위치 | `R32G32B32A32_FLOAT` |
| 조명 결과 | `DFLightTexture` | 후처리 조명 결과 | `R8G8B8A8_UNORM` |

월드 위치와 법선은 `gmtxView`로 view space로 변환한다. 현재 G-buffer만으로
표준적인 SSAO 구현에 필요한 위치, 법선, 깊이 정보를 모두 제공한다.

## 현재 구현

### Raw AO 생성: view-space hemisphere kernel

`PSGenerateSSAO.hlsl`은 16개의 3D hemisphere kernel을 사용한다.
각 픽셀에서 법선과 블루 노이즈 방향으로 TBN basis를 만들고, kernel을 view space에서 회전한다.

```text
expectedSamplePositionV = positionV + TBN * kernelSample * sampleRadius
```

예상 샘플 위치는 projection matrix를 통해 화면 UV로 변환한다. 이후 G-buffer의 실제 위치와
깊이를 읽어 occluder 여부를 판단한다.

### 깊이 비교와 range check

왼손 view space에서 작은 +Z 값은 카메라에 더 가깝다. 따라서 실제 G-buffer 샘플이
예상 샘플 위치보다 충분히 카메라 쪽에 있을 때만 차폐로 누적한다.

```text
sampledPositionV.z <= expectedSamplePositionV.z - depthBias
```

중심과 샘플의 깊이 차가 반경에 비해 크면 range weight를 낮춘다. 화면 밖 UV와
배경 깊이(`depth >= 1.0`)는 제외한다.

G-buffer와 블루 노이즈는 `Load()`로 읽어 wrap sampler 또는 선형 보간이
깊이 비교에 영향을 주지 않도록 한다.

### Visibility 기반 ambient 결합

누적값은 `0~1` occlusion으로 제한한 뒤 ambient visibility로 변환한다.

```hlsl
float ambientVisibility = 1.0f - saturate(occlusion * gfIntesity);
```

`Light.hlsl`은 SSAO를 직접광 또는 shadow map 결과에 감산하지 않고,
전역 ambient에만 적용한다.

```hlsl
cColor += gcGlobalAmbientLight * saturate(ambientVisibility);
```

기존의 `gcGlobalAmbientLight - ssao` 감산 방식은 제거했다. 이로써 음수 ambient와
UNORM clamp로 인해 AO 대비가 과장될 가능성을 줄였다.

### Bilateral blur

Raw AO는 제한된 수의 kernel과 픽셀별 블루 노이즈 회전 때문에 고주파 noise가 남는다.
이를 완화하기 위해 `CBlurSSAOComputeShader`가 아래 두 Compute Shader 패스를 순서대로
실행한다.

```text
Raw AO (`R16_FLOAT`)
  → Horizontal blur (`R16_FLOAT`)
  → Vertical blur (`R16_FLOAT`)
  → final lighting의 ambient visibility로 사용
```

두 패스는 입력 AO를 `t14` SRV로 읽고, 출력 텍스처를 `u0` UAV로 기록한다.
각 픽셀은 중심 AO와 좌우 또는 상하 3픽셀의 AO를 Gaussian weight로 평균낸다.

단순 Gaussian blur와 달리, G-buffer의 view-space 깊이와 법선을 함께 비교해
각 샘플의 bilateral weight를 계산한다.

```text
weight = gaussianWeight
       × depthWeight(centerDepth, sampleDepth)
       × normalWeight(centerNormal, sampleNormal)
```

- 깊이 차이가 큰 샘플은 서로 다른 물체 또는 앞·뒤 배경일 가능성이 높으므로 가중치를 낮춘다.
- 법선 방향이 크게 다른 샘플은 다른 표면일 가능성이 높으므로 가중치를 낮춘다.
- 따라서 같은 표면의 점상 noise는 완화하면서, 캐릭터 윤곽의 AO가 뒤쪽 벽으로 번지는 현상을 줄인다.

현재 기본값은 `kDepthWeightScale = 4.0f`, `kNormalWeightPower = 8.0f`이며,
값을 낮추면 noise 제거는 강해지지만 표면 경계를 넘는 번짐 위험이 커진다.

## 블루 노이즈 리소스

`CGenerateSSAOShader`는 `Asset/Textures/LDR_LLL1_0.dds`를 t0에 바인딩한다.
128×128 블루 노이즈 텍스처의 단일 채널 값은 각 픽셀의 kernel 회전각으로 사용한다.

```text
noise angle = noise.r * 2π
```

이 DDS는 현재 `Asset/` gitignore 규칙에 의해 저장소에서 추적하지 않는다. 따라서
새 환경에서 실행하려면 해당 경로에 리소스를 별도로 준비해야 한다.

## 현재 튜닝값

`FrameTimeInfo`의 AO 관련 값은 기존 구현과 의미가 달라졌다.

| 값 | 현재 의미 | 현재 기본값 |
| --- | --- | --- |
| `gfScale` | view-space AO 반경 | `0.1f` |
| `gfBias` | occluder로 판정할 최소 깊이 차이 | `0.002f` |
| `gfIntesity` | occlusion을 ambient visibility로 변환하는 강도 | `1.0f` |

`gfBias`가 `0`에 가까우면 재질의 미세 변화도 차폐로 누적되어 디테일이 강해 보일 수 있다.
그러나 같은 표면의 깊이 오차까지 차폐로 인식하는 self-occlusion/noise가 증가한다.
`gfBias`는 강도 조절값이 아니라 이를 거르는 임계값이다.

## 확인된 결과

- 전역 `const` 배열 및 범위 밖 8x8 noise 접근으로 인한 비결정적 렌더링 문제를 제거했다.
- 생존자 접속/종료 상태에 따라 좀비 렌더링이 달라지던 문제는 재현되지 않는다.
- raw SSAO 출력에서 관찰되던 원형 밴딩은 view-space hemisphere 방식으로 전환한 뒤 사라졌다.
- AO 반경을 크게 설정하면 오브젝트 주변 차폐가 넓어지므로, `gfScale`은 씬 단위에 맞춰 조절해야 한다.
- Raw AO → horizontal blur → vertical blur → 조명 합성의 분리된 SSAO 파이프라인을 적용했다.
- AO blur 결과가 조명 합성에서 사용되도록 Raw AO, 중간 blur, 최종 blur 텍스처의 리소스 상태 전환을 구성했다.

## 후속 작업

### Material AO

철망처럼 재질 내부에 반복되는 미세한 음영은 SSAO보다 material AO가 적합하다.
현재 제공된 텍스처에는 AO 정보가 없으므로, SSAO의 bias를 낮춰 이를 표현하려 하면
noise와 self-occlusion이 함께 증가한다.

향후 AO를 베이크할 수 있다면 `MetallicSmoothness` 텍스처를 다음처럼 패킹하는 방식을 검토한다.

```text
R = Metallic
G = Material AO
B = Reserved / Detail mask
A = Smoothness
```

이는 현재 SSAO noise 제거와는 별도의 재질/G-buffer 개선 작업이다.

## 검증 기준

- Raw, horizontal blur, vertical blur 결과를 각각 grayscale으로 출력했을 때 단계별로 noise가 완화된다.
- 캐릭터 윤곽과 뒤쪽 벽처럼 깊이 또는 법선이 크게 다른 경계에서 AO가 배경으로 번지지 않는다.
- 접촉부와 틈은 자연스럽게 어두워지며, AO는 직접광 또는 shadow map 결과에 직접 감산되지 않는다.
- 단독 접속과 다른 플레이어의 접속/종료 상태 사이에서 SSAO 결과가 비결정적으로 달라지지 않는다.
- Blur의 depth/normal 가중치를 낮춘 단순 Gaussian에 가까운 설정과 비교했을 때, 경계 보존 효과를 확인한다.
