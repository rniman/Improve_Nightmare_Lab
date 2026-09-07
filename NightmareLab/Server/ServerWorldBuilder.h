#pragma once

#include <random>
#include <utility>
#include <vector>

#include <DirectXMath.h>
#include <DirectXCollision.h>

class CServerCollisionManager;

struct ServerWorldBuildResult
{
	bool succeeded = false;
	int escapeDoorId = -1;
};

class ServerWorldBuilder
{
public:
	ServerWorldBuilder(CServerCollisionManager& collisionManager, std::default_random_engine& randomEngine);

	ServerWorldBuildResult Build();

private:
	bool LoadScene();
	void CreateObjectFromSceneFrame(
		char* frameName,
		const DirectX::XMFLOAT4X4& world,
		const std::vector<DirectX::BoundingOrientedBox>& boundingBoxes);
	int SelectEscapeDoor();
	bool PopulateItems();

	CServerCollisionManager& mCollisionManager;
	std::default_random_engine& mRandomEngine;
	std::vector<std::pair<int, int>> mDrawerEntries;
};
