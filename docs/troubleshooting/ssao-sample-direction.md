# SSAO 샘플 방향 배열 동작 확인

작성일: 2026-08-02

> 현재 SSAO 구조와 개선 목표는 [SSAO 현황 및 개선 목표](../ssao.md)를 참고한다.
> 이 문서는 샘플 방향 배열 및 기존 8x8 노이즈 테이블에서 발견한 문제의 기록이다.

## 증상

좀비 단독 접속 상태와 생존자 접속 후 종료 상태에서 화면의 SSAO 강도와 최종 조명 결과가 다르게 보였다.

## 현재 확인 사항

`PSPostProcessingWithSSAO.hlsl`에서 SSAO 샘플 방향 배열을 전역 `const`로 선언했을 때,
`vec[0]`을 사용한 디버그 출력이 기대와 다르게 보였다.

```hlsl
const float2 vec[16] = { /* ... */ };
```

같은 배열을 `static const`로 선언하면 카메라 방향에 따라 나타나던 비결정적인 변화가 사라졌다.

```hlsl
static const float2 vec[16] = { /* ... */ };
```

```hlsl
	float2 rand = normalize(float2(1.0f, 0.0f));
	float2 reflected = reflect(vec[0], rand);
	return float4(reflected * 0.5f + 0.5f, 0.0f, 1.0f);
```

rand를 float2(1.0f, 0.0f)으로 고정했을 때, reflected * 0.5f + 0.5f의 기대값은 float2(0.0f, 0.5f)이다. static이 없는 전역 const 배열에서는 카메라가 -Z 방향을 볼 때 출력이 기대한 균일한 색이 아닌 주황색 계열로 변하는 현상이 관찰되었다.

## Noise 인덱스 범위 오류

기존 식은 8x8 배열을 반복 참조하려는 의도와 달리 framebuffer 폭을 인덱스 계산에 사용했다.

```hlsl
int noiseIndex = randUV.x + int(vFrameBuffer.x * randUV.y);
```

`noise` 배열은 64개뿐이지만, `randUV.y`에 framebuffer 폭이 곱해져 `noiseIndex`가
`0~63`을 크게 벗어날 수 있다. 이는 정의되지 않은 배열 범위 밖 접근이다.

수정 후에는 화면 픽셀 좌표를 8x8 범위로 나머지 연산해 noise 좌표를 만들고,
행 우선 인덱스를 계산한다.

```hlsl
uint2 pixelCoord = uint2(input.uv * float2(FRAME_BUFFER_WIDTH, FRAME_BUFFER_HEIGHT));
uint2 noiseCoord = pixelCoord % kNoiseDimension;
uint noiseIndex = noiseCoord.x + noiseCoord.y * kNoiseDimension;
float2 rand = normalize(kNoise[noiseIndex].xy * 2.0f - 1.0f);
```

이 식에서 `noiseIndex`는 항상 `0~63`이다. 시간 기반 offset은 제거해,
프레임마다 noise 타일이 바뀌지 않도록 했다.

## 결과 및 남은 확인

`static const` 선언과 유효 범위의 noise 인덱싱을 적용한 뒤, 생존자 접속 여부에 따라
좀비 렌더링이 달라지던 현상은 재현되지 않았다.

다만 이전에는 범위 밖 noise 접근이 샘플 패턴을 우연히 가리고 있었을 수 있으며,
정상화 후 원형 밴딩이 관찰되었다. 이 문제는 이후 2D 고정 반경 SSAO를 view-space
hemisphere kernel 방식으로 교체하면서 해결했다. 현재 구현과 남은 noise 완화 작업은
[SSAO 현황 및 개선 계획](../ssao.md)을 참고한다.
