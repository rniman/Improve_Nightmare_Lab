#include "stdafx.h"
#include "ServerWorldBuilder.h"

#include "ServerCollision.h"
#include "ServerEnvironmentObject.h"
#include "ServerObject.h"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <iterator>
#include <memory>

namespace
{
	bool ReadExact(
		FILE* inputFile,
		void* destination,
		std::size_t elementSize,
		std::size_t elementCount)
	{
		return ::fread(destination, elementSize, elementCount, inputFile) == elementCount;
	}

	bool ReadLengthPrefixedString(FILE* inputFile, char* destination, std::size_t destinationSize)
	{
		BYTE stringLength = 0;
		if (!ReadExact(inputFile, &stringLength, sizeof(stringLength), 1) ||
			stringLength == 0 ||
			static_cast<std::size_t>(stringLength) >= destinationSize)
		{
			return false;
		}

		if (!ReadExact(inputFile, destination, sizeof(char), stringLength))
		{
			return false;
		}

		destination[stringLength] = '\0';
		return true;
	}

	void LogWorldBuildNotice(const char* message)
	{
		std::cerr << "[ServerWorldBuilder] " << message << '\n';
	}
}

ServerWorldBuilder::ServerWorldBuilder(
	CServerCollisionManager& collisionManager,
	std::default_random_engine& randomEngine)
	: mCollisionManager(collisionManager),
	mRandomEngine(randomEngine)
{}

ServerWorldBuildResult ServerWorldBuilder::Build()
{
	// 난수 소비 순서를 유지하기 위해 탈출문 선택 후 아이템을 배치한다.
	if (!LoadScene())
	{
		return {};
	}

	const int escapeDoorId = SelectEscapeDoor();
	if (escapeDoorId < 0)
	{
		return {};
	}

	if (!PopulateItems())
	{
		return {};
	}

	return ServerWorldBuildResult{ true, escapeDoorId };
}

bool ServerWorldBuilder::LoadScene()
{
	FILE* inputFile = nullptr;
	const errno_t openResult = ::fopen_s(&inputFile, "ServerScene.bin", "rb");
	if (openResult != 0 || inputFile == nullptr)
	{
		std::cerr << "[ServerWorldBuilder] Failed to open ServerScene.bin: error="
			<< openResult << '\n';
		return false;
	}

	std::unique_ptr<FILE, decltype(&::fclose)> inputFileGuard(inputFile, &::fclose);
	const auto reportReadFailure = [](const char* message)
	{
		LogWorldBuildNotice(message);
		return false;
	};

	bool reachedSceneEnd = false;
	while (!reachedSceneEnd)
	{
		char token[128] = { '\0' };
		for (;;)
		{
			if (!ReadLengthPrefixedString(inputFile, token, std::size(token)))
			{
				return reportReadFailure("Failed to read a server scene token.");
			}

			if (!strcmp(token, "<Hierarchy>:"))
			{
				char frameName[64] = { '\0' };
				std::vector<BoundingOrientedBox> boundingBoxes;
				for (;;)
				{
					if (!ReadLengthPrefixedString(inputFile, token, std::size(token)))
					{
						return reportReadFailure("Failed to read a server scene frame token.");
					}

					if (!strcmp(token, "<Frame>:"))
					{
						int frameIndex = 0;
						if (!ReadExact(inputFile, &frameIndex, sizeof(frameIndex), 1) ||
							!ReadLengthPrefixedString(inputFile, frameName, std::size(frameName)))
						{
							return reportReadFailure("Failed to read server scene frame data.");
						}
					}
					else if (!strcmp(token, "<Children>:"))
					{
						int childCount = 0;
						if (!ReadExact(inputFile, &childCount, sizeof(childCount), 1) || childCount < 0)
						{
							return reportReadFailure("Invalid server scene child count.");
						}
					}
					else if (!strcmp(token, "<BoxColliders>:"))
					{
						int boxColliderCount = 0;
						if (!ReadExact(inputFile, &boxColliderCount, sizeof(boxColliderCount), 1) ||
							boxColliderCount < 0)
						{
							return reportReadFailure("Invalid server scene collider count.");
						}

						boundingBoxes.reserve(static_cast<std::size_t>(boxColliderCount));
						for (int collider = 0; collider < boxColliderCount; ++collider)
						{
							if (!ReadLengthPrefixedString(inputFile, token, std::size(token)))
							{
								return reportReadFailure("Failed to read a server scene collider token.");
							}

							int colliderIndex = 0;
							XMFLOAT3 aabbCenter = {};
							XMFLOAT3 aabbExtents = {};
							if (!ReadExact(inputFile, &colliderIndex, sizeof(colliderIndex), 1) ||
								!ReadExact(inputFile, &aabbCenter, sizeof(aabbCenter), 1) ||
								!ReadExact(inputFile, &aabbExtents, sizeof(aabbExtents), 1))
							{
								return reportReadFailure("Failed to read server scene collider data.");
							}

							XMFLOAT4 orientation;
							XMStoreFloat4(&orientation, XMQuaternionIdentity());
							boundingBoxes.emplace_back(aabbCenter, aabbExtents, orientation);
						}
					}
					else if (!strcmp(token, "<Matrix>:"))
					{
						int matrixCount = 0;
						if (!ReadExact(inputFile, &matrixCount, sizeof(matrixCount), 1) || matrixCount < 0)
						{
							return reportReadFailure("Invalid server scene matrix count.");
						}

						std::vector<XMFLOAT4X4> worldMatrices(static_cast<std::size_t>(matrixCount));
						if (!worldMatrices.empty() && !ReadExact(
							inputFile,
							worldMatrices.data(),
							sizeof(XMFLOAT4X4),
							worldMatrices.size()))
						{
							return reportReadFailure("Failed to read server scene matrices.");
						}

						for (XMFLOAT4X4& worldMatrix : worldMatrices)
						{
							CreateObjectFromSceneFrame(
								frameName,
								Matrix4x4::Transpose(worldMatrix),
								boundingBoxes);
						}
					}
					else if (!strcmp(token, "</Frame>"))
					{
						break;
					}
				}
			}
			else if (!strcmp(token, "</Hierarchy>"))
			{
				break;
			}
			else if (!strcmp(token, "</Scene>:"))
			{
				reachedSceneEnd = true;
				break;
			}
		}
	}

	return true;
}

void ServerWorldBuilder::CreateObjectFromSceneFrame(
	char* frameName,
	const XMFLOAT4X4& world,
	const std::vector<BoundingOrientedBox>& boundingBoxes)
{
	const int serverObjectId = CServerCollisionManager::GetNumberOfCollisionObject();
	shared_ptr<CServerGameObject> gameObject;

	if (!strcmp(frameName, "Door_1"))
	{
		gameObject = make_shared<CServerDoorObject>(frameName, world, boundingBoxes);
	}
	else if (!strcmp(frameName, "Drawer_1"))
	{
		mDrawerEntries.emplace_back(serverObjectId, 1);
		gameObject = make_shared<CServerDrawerObject>(frameName, world, boundingBoxes);
	}
	else if (!strcmp(frameName, "Drawer_2"))
	{
		mDrawerEntries.emplace_back(serverObjectId, 2);
		gameObject = make_shared<CServerDrawerObject>(frameName, world, boundingBoxes);
	}
	else if (!strcmp(frameName, "Door1"))
	{
		gameObject = make_shared<CServerElevatorDoorObject>(frameName, world, boundingBoxes);
	}
	else if (!strcmp(frameName, "Emergency_Handle"))
	{
		gameObject = make_shared<CServerElevatorDoorObject>(frameName, world, boundingBoxes);
	}
	else if (!strcmp(frameName, "Laboratory_Wall_1_Corner_1") || !strcmp(frameName, "Laboratory_Wall_1_Corner_2"))
	{
		gameObject = make_shared<CServerEnvironmentObject>(frameName, world, boundingBoxes);
	}
	else if (!strcmp(frameName, "BoxCollide_Wall"))
	{
		gameObject = make_shared<CServerEnvironmentObject>(frameName, world, boundingBoxes);
	}
	else if (!strcmp(frameName, "Laboratory_Wall_1_Corner") || !strcmp(frameName, "Laboratory_Wall_1_Corner2") || !strcmp(frameName, "Laboratory_Wall_1"))
	{
		gameObject = make_shared<CServerEnvironmentObject>(frameName, world, boundingBoxes);
	}
	else if (!strcmp(frameName, "Laboratory_Wall_Door_1") || !strcmp(frameName, "Laboratory_Wall_Door_1_2"))
	{
		gameObject = make_shared<CServerEnvironmentObject>(frameName, world, boundingBoxes);
	}
	else if (!strcmp(frameName, "Biological_Capsule_1") || !strcmp(frameName, "Laboratory_Table_1") || !strcmp(frameName, "Laboratory_Stool_1"))
	{
		gameObject = make_shared<CServerEnvironmentObject>(frameName, world, boundingBoxes);
	}
	else if (!strcmp(frameName, "Laboratory_Tunnel_1_Stairs") || !strcmp(frameName, "Laboratory_Tunnel_1") || !strcmp(frameName, "Laboratory_Desk_Drawers_1"))
	{
		gameObject = make_shared<CServerEnvironmentObject>(frameName, world, boundingBoxes);
	}
	else if (!strcmp(frameName, "SM_Prop_Vents_Straight_01") || !strcmp(frameName, "SM_Prop_Crate_01")
		|| !strcmp(frameName, "SM_Prop_Pipe_Curve_02") || !strcmp(frameName, "SM_Prop_Billboard_Roof_01")
		|| !strcmp(frameName, "SM_Prop_Roof_Aircon_03") || !strcmp(frameName, "SM_Prop_Vents_End_01")
		|| !strcmp(frameName, "SM_Prop_ShopInterior_Table_01") || !strcmp(frameName, "SM_Prop_Couch_01")
		|| !strcmp(frameName, "SM_Prop_PotPlant_02") || !strcmp(frameName, "Table1of10"))
	{
		gameObject = make_shared<CServerEnvironmentObject>(frameName, world, boundingBoxes);
	}
	else if (!strcmp(frameName, "BoxCollider_Stair_Start"))
	{
		gameObject = make_shared<CServerStairTriggerObject>(frameName, world, boundingBoxes);
	}
	else
	{
		gameObject = make_shared<CServerGameObject>(frameName, world, boundingBoxes);
		gameObject->SetStatic(true);
	}

	strcpy(gameObject->m_pstrFrameName, frameName);
	mCollisionManager.AddCollisionObject(gameObject);
}

int ServerWorldBuilder::SelectEscapeDoor()
{
	std::vector<std::pair<int, shared_ptr<CServerElevatorDoorObject>>> escapeDoorCandidates;
	for (int objectId = 0; objectId < mCollisionManager.GetNumberOfCollisionObject(); ++objectId)
	{
		shared_ptr<CServerGameObject> object = mCollisionManager.GetCollisionObjectWithNumber(objectId);
		auto elevatorDoor = dynamic_pointer_cast<CServerElevatorDoorObject>(object);
		if (!elevatorDoor || strcmp(elevatorDoor->m_pstrFrameName, "Door1") != 0)
		{
			continue;
		}

		escapeDoorCandidates.emplace_back(objectId, std::move(elevatorDoor));
	}

	if (escapeDoorCandidates.empty())
	{
		LogWorldBuildNotice("No escape door candidate was found in the server scene.");
		return -1;
	}

	std::uniform_int_distribution<std::size_t> candidateDistribution(
		0,
		escapeDoorCandidates.size() - 1);
	const auto& [escapeDoorId, escapeDoor] =
		escapeDoorCandidates[candidateDistribution(mRandomEngine)];
	escapeDoor->SetEscapeDoor(true);
	return escapeDoorId;
}

bool ServerWorldBuilder::PopulateItems()
{
	if (mDrawerEntries.size() < static_cast<std::size_t>(ITEM_COUNT))
	{
		LogWorldBuildNotice("The server scene does not contain enough drawers for item placement.");
		return false;
	}

	const int drawerCount = static_cast<int>(mDrawerEntries.size());
	std::uniform_int_distribution<int> drawerDistribution(0, drawerCount - 1);
	std::uniform_int_distribution<int> itemTypeDistribution(0, 99);
	std::uniform_int_distribution<int> rotationDistribution(1, 360);
	std::uniform_real_distribution<float> offsetDistribution(-0.2f, 0.2f);
	CServerItemObject::SetDrawerIdContainer(mDrawerEntries);

	for (int itemIndex = 0; itemIndex < ITEM_COUNT; ++itemIndex)
	{
		const int drawerEntryIndex = drawerDistribution(mRandomEngine);
		const int drawerObjectId = mDrawerEntries[drawerEntryIndex].first;
		shared_ptr<CServerDrawerObject> drawerObject = dynamic_pointer_cast<CServerDrawerObject>(
			mCollisionManager.GetCollisionObjectWithNumber(drawerObjectId));
		if (!drawerObject)
		{
			LogWorldBuildNotice("A drawer entry does not reference a drawer object.");
			return false;
		}

		if (drawerObject->m_pStoredItem)
		{
			--itemIndex;
			continue;
		}

		const XMFLOAT4X4 drawerWorld = drawerObject->GetWorldMatrix();
		// 기존 아이템 배치와 이후 난수 결과가 달라지지 않도록 타입 난수를 소비한다.
		(void)itemTypeDistribution(mRandomEngine);
		const XMFLOAT3 randomOffset = XMFLOAT3(
			offsetDistribution(mRandomEngine),
			0.0f,
			offsetDistribution(mRandomEngine));
		XMFLOAT3 randomRotation = XMFLOAT3(
			0.0f,
			0.0f,
			static_cast<float>(rotationDistribution(mRandomEngine)));

		shared_ptr<CServerItemObject> itemObject;
		if (itemIndex < 9)
		{
			itemObject = make_shared<CServerFuseObject>();
		}
		else if (itemIndex < 24)
		{
			itemObject = make_shared<CServerTeleportObject>();
		}
		else if (itemIndex < 26)
		{
			randomRotation = XMFLOAT3(90.0f, 0.0f, 0.0f);
			itemObject = make_shared<CServerRadarObject>();
		}
		else
		{
			randomRotation = XMFLOAT3(90.0f, 0.0f, 0.0f);
			itemObject = make_shared<CServerMineObject>();
		}

		itemObject->SetDrawerNumber(drawerObjectId);
		itemObject->SetDrawer(drawerObject);
		itemObject->SetDrawerType(mDrawerEntries[drawerEntryIndex].second);
		drawerObject->m_pStoredItem = itemObject;
		itemObject->SetRandomRotation(randomRotation);
		itemObject->SetRandomOffset(randomOffset);
		itemObject->SetWorldMatrix(drawerWorld);
		mCollisionManager.AddCollisionObject(itemObject);
	}

	return true;
}
