# GameOver / Win UI가 게임 종료 시 표시되지 않음

작성일: 2026-07-31

## 증상

게임이 종료된 뒤 `GameOver` 또는 `Win` UI가 화면에 나타나지 않았다.

## 원인 및 수정

`CMainScene::ForwardRender()`의 게임 상태 조건이 종료 상태에서 UI 셰이더를 렌더링하지 못하게 되어 있었다.

`GAME_STATE::BLUE_SUIT_WIN`과 `GAME_STATE::ZOMBIE_WIN`일 때
`USER_INTERFACE_SHADER`를 렌더링하도록 조건을 수정했다.

## UI 페이드인 알파 계산

`UserInterfaceShader::AnimateObjects()`는 종료 UI의 머티리얼 알파에 다음 값을 설정한다.

```cpp
2.0f - m_fEndingElapsedTime / 3.0f
```

`PSUserInterface.hlsl`은 최종 알파를 다음과 같이 계산한다.

```hlsl
cAlbedoColor.a = (1.0f - gMaterial.m_cAlbedo.a) + cAlbedoColor.a;
```

종료 UI 텍스처의 알파가 `1.0f`이면 최종 알파는
`m_fEndingElapsedTime / 3.0f`가 된다. 따라서 위 값은 종료 UI를
3초 동안 0에서 1까지 페이드인하기 위한 역산 값이다.

## 반복 수식 정리

이 계산은 블루슈트와 좀비 UI 셰이더에 반복되어 있었다. `Shader.cpp`의
파일 내부에 다음 상수와 헬퍼 함수를 두어 수식과 셰이더 의존 이유를 한 곳에서 관리한다.

```cpp
constexpr float kGameEndingFadeDurationSeconds = 3.0f;

float CalculateGameEndingFadeMaterialAlpha(float elapsedTime)
{
    // PSUserInterface calculates final alpha as 1.0f - materialAlpha + textureAlpha.
    // With an opaque texture, this produces an elapsedTime / duration fade-in.
    return 2.0f - elapsedTime / kGameEndingFadeDurationSeconds;
}
```

각 종료 UI는 `CalculateGameEndingFadeMaterialAlpha(m_fEndingElapsedTime)`를 사용한다.
페이드 시간 제한과 관련 비율 계산도 `kGameEndingFadeDurationSeconds`를 사용해
3초라는 기준값을 일관되게 유지한다.
