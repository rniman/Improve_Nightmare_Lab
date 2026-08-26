# Emissive Blur Compute 패스 입력 및 샘플링 축 오류

작성일: 2026-08-26

## 문제 범위

Emissive Bloom은 원본 Emissive 텍스처에 1차원 Gaussian Blur를 두 번 적용하는
분리형 2D Blur로 구성된다. 코드 분석 당시 두 번째 패스가 첫 번째 패스 결과를
사용하지 않았고, Horizontal/Vertical 셰이더의 실제 샘플링 축도 이름과 반대로
구현되어 있었다.

## 의도한 리소스 흐름

```text
DFTextureEmissive (t9)
  -> CSBlurVertical: Y축 블러
  -> m_pTextureFirPassUav
  -> gInputTexture (t14)
  -> CSBlurHorizontal: X축 블러
  -> m_pTextureSecPassUav
  -> gInputTexture (t14)
  -> CSComposite
```

`t9`는 G-Buffer에 기록된 블러 전 Emissive 원본이다. `t14`는 특정 텍스처에
고정된 슬롯이 아니라, 직전 Compute 패스의 결과를 다음 Compute 패스에 전달하는
핑퐁 SRV 슬롯이다.

## 원인 1: 두 번째 패스가 원본 Emissive를 다시 읽음

`CBlurComputeShader::PassSecond()`는 첫 번째 패스 결과인
`m_pTextureFirPassUav`를 루트 파라미터 17에 연결한다. 루트 시그니처에서 이
파라미터는 `t14` SRV이다.

하지만 기존 `CSBlurHorizontal.hlsl`은 `t14`가 아닌 `DFTextureEmissive(t9)`를
계속 읽었다.

```hlsl
blurredColor += DFTextureEmissive[samplePos] * weight[abs(y)];
```

이 경우 첫 번째 패스 결과는 두 번째 패스에 반영되지 않는다. 최종 결과는 두 축을
순차 적용한 2D Blur가 아니라, 두 번째 패스가 수행한 한 방향 Blur만 반영한다.

두 번째 패스가 `t14`를 읽도록 입력을 분리했다.

```hlsl
Texture2D<float4> gInputTexture : register(t14);

blurredColor += gInputTexture[samplePos] * weight[abs(x)];
```

## 원인 2: Horizontal/Vertical 샘플링 축이 반대임

텍스처 좌표에서 X는 수평축이고 Y는 수직축이다. 기존 구현은 셰이더 이름과 반대
방향을 샘플링했다.

| 셰이더 | 기존 오프셋 | 기존 동작 | 수정 오프셋 | 수정 동작 |
|---|---:|---|---:|---|
| `CSBlurHorizontal` | `(0, y)` | 수직 Blur | `(x, 0)` | 수평 Blur |
| `CSBlurVertical` | `(x, 0)` | 수평 Blur | `(0, y)` | 수직 Blur |

Gaussian 가중치가 두 축에서 같으면 X와 Y 패스의 실행 순서를 바꿔도 최종 수학적
결과는 같다. 하지만 이름과 구현이 반대이면 패스 입력이나 최적화를 변경할 때 오류를
유발하기 쉬우므로 실제 샘플링 축을 이름에 맞췄다.

## 원인 3: Compute 입력에 Pixel Shader 전용 상태 사용

중간 블러 결과는 Compute Shader가 SRV로 읽지만 기존 코드는 UAV 출력 후
`D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE`로 전환했다.

Compute Shader 입력에는 다음 상태를 사용해야 한다.

```cpp
D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
```

현재 중간 텍스처의 상태 흐름은 다음과 같다.

```text
m_pTextureFirPassUav:
  UNORDERED_ACCESS
  -> NON_PIXEL_SHADER_RESOURCE  // 두 번째 Blur CS 입력
  -> UNORDERED_ACCESS           // 다음 프레임 출력 준비

m_pTextureSecPassUav:
  UNORDERED_ACCESS
  -> NON_PIXEL_SHADER_RESOURCE  // Composite CS 입력
  -> UNORDERED_ACCESS           // 다음 프레임 출력 준비

m_pTextureCompositeUav:
  UNORDERED_ACCESS
  -> PIXEL_SHADER_RESOURCE      // 최종 화면 출력 PS 입력
```

## 기존에도 동작해 보일 수 있었던 이유

첫 번째 중간 결과는 두 번째 패스가 실제로 읽지 않았기 때문에 잘못된 상태가 바로
문제로 이어지지 않았다. Composite CS의 상태 불일치도 일부 GPU와 드라이버에서는
UAV-SRV 배리어가 필요한 실행 순서와 캐시 동기화를 수행해 정상처럼 보일 수 있다.

그러나 `PIXEL_SHADER_RESOURCE` 상태의 리소스를 Compute Shader에서 읽는 것은
D3D12 규칙에 맞지 않으며 결과는 보장되지 않는다. Debug Layer가 비활성화되어 있으면
리소스 상태 불일치 메시지도 확인하기 어렵다.

## 수정 파일

- `Client/Client/CSBlurHorizontal.hlsl`
  - 두 번째 패스 입력 `gInputTexture(t14)` 추가
  - X축 샘플링으로 수정
- `Client/Client/CSBlurVertical.hlsl`
  - Y축 샘플링으로 수정
- `Client/Client/BlurComputeShader.cpp`
  - 중간 텍스처를 `NON_PIXEL_SHADER_RESOURCE` 상태로 전환

## 검증 방법

1. `CSBlurHorizontal.hlsl`과 `CSBlurVertical.hlsl`을 각각 Shader Model 5.1 Compute
   Shader로 컴파일한다.
2. Visual Studio에서 Debug x64 클라이언트 프로젝트를 빌드해 실행용 `.cso`를
   갱신한다.
3. D3D12 Debug Layer를 활성화하고 Blur 및 Composite 디스패치에서 리소스 상태
   불일치 메시지가 없는지 확인한다.
4. 밝은 Emissive 오브젝트 하나를 어두운 배경에 배치한다.
5. Bloom을 끈 화면과 비교해 원본 씬 색상에는 의도하지 않은 변화가 없는지 확인한다.

## 남은 개선 사항

- 프레임 버퍼 크기가 스레드 그룹 크기 32의 배수가 아닐 때를 대비해 Dispatch Thread
  좌표 경계 검사를 추가한다.
- `[numthreads(32, 32, 1)]`은 그룹당 1024스레드를 사용해 FXC 성능 경고가 발생할 수
  있으므로, 별도 성능 작업에서 그룹 크기와 메모리 접근 방식을 측정한다.
- Bloom의 화면 품질을 확인한 뒤 Gaussian 가중치와 반경을 조정한다.
