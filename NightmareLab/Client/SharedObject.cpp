#pragma once
#include "stdafx.h"
#include "SharedObject.h"

void SharedObject::EnableItemGetParticle(const shared_ptr<CGameObject>& object, float totalTime)
{
	XMFLOAT3 pos = object->GetPosition();
	m_vParticleObjects[CParticleMesh::SPARK]->SetParticleInsEnable(-1, true, totalTime, pos);
}

void SharedObject::AddParticle(CParticleMesh::TYPE particleType, XMFLOAT3 pos, float totalTime)
{
	m_vParticleObjects[CParticleMesh::FOOTPRINT]->AddParticle(pos, totalTime);
}
