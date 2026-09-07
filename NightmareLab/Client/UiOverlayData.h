#pragma once

#include <array>
#include <cstddef>

#include <DirectXMath.h>

enum class UiOverlayType
{
	SurvivorStartMessage,
	ZombieCountdown,
	ZombieObjective,
	RadarDistance,
	Count
};

struct UiOverlayElement
{
	UiOverlayType type = UiOverlayType::SurvivorStartMessage;
	DirectX::XMFLOAT2 centerPosition = {};
	DirectX::XMFLOAT2 layoutSize = {};
	DirectX::XMFLOAT4 color = { 1.0f, 1.0f, 1.0f, 1.0f };
	int value = 0;
	bool visible = false;
};

struct UiOverlayFrameData
{
	UiOverlayFrameData()
	{
		for (std::size_t index = 0; index < elements.size(); ++index)
		{
			elements[index].type = static_cast<UiOverlayType>(index);
		}
	}

	UiOverlayElement& GetElement(UiOverlayType type)
	{
		return elements[static_cast<std::size_t>(type)];
	}

	const UiOverlayElement& GetElement(UiOverlayType type) const
	{
		return elements[static_cast<std::size_t>(type)];
	}

	std::array<UiOverlayElement, static_cast<std::size_t>(UiOverlayType::Count)> elements;
};
