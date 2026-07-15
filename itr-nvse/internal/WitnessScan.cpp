#include "WitnessScan.h"
#include "EngineFunctions.h"
#include "EngineHelpers.h"
#include "GameLayout.h"
#include "settings.h"
#include "nvse/GameObjects.h"

namespace WitnessScan {

int GetDetectionValue(Actor* observer, Actor* target)
{
	if (!observer || !target) return -100;
	return Engine::Actor_GetDetectionValue(observer, target);
}

void FindWitnesses(Actor* perpetrator, const float* crimeLocXYZ,
                   float radius, int detectionThreshold,
                   std::vector<Hit>& out)
{
	out.clear();
	if (!perpetrator || !perpetrator->parentCell) return;

	if (radius <= 0.0f) radius = Settings::fWitnessSearchRadius;
	if (detectionThreshold <= 0) detectionThreshold = Settings::iWitnessDetectionThreshold;

	float cx = crimeLocXYZ ? crimeLocXYZ[0] : perpetrator->posX;
	float cy = crimeLocXYZ ? crimeLocXYZ[1] : perpetrator->posY;
	float cz = crimeLocXYZ ? crimeLocXYZ[2] : perpetrator->posZ;
	float radiusSq = radius * radius;

	//bounded scratch, detection and push_back run after the cell ref lock is released
	Actor* candidates[256];
	int candidateCount = 0;

	auto collectCell = [&](TESObjectCELL* cell)
	{
		if (!cell) return;
		ScopedCellRefLock refLock(cell);

		for (auto iter = cell->objectList.Begin(); !iter.End(); ++iter)
		{
			TESObjectREFR* ref = iter.Get();
			if (!ref) continue;
			if (ref->baseForm == nullptr) continue;
			UInt8 baseType = ref->baseForm->typeID;
			if (baseType != kFormType_NPC && baseType != kFormType_Creature) continue;

			Actor* actor = static_cast<Actor*>(ref);
			if (actor == perpetrator) continue;
			if (!actor->baseProcess) continue;

			//teammates never raise alarms against their own side
			if (ActorIsTeammate(actor))
				continue;

			float dx = actor->posX - cx;
			float dy = actor->posY - cy;
			float dz = actor->posZ - cz;
			float distSq = dx * dx + dy * dy + dz * dz;
			if (distSq > radiusSq) continue;

			if (candidateCount < 256)
				candidates[candidateCount++] = actor;
		}
	};

	TESObjectCELL* cell = perpetrator->parentCell;
	collectCell(cell);

	//the default radius crosses exterior cell boundaries, walk the 3x3 neighbour grid like ResurrectAll
	TESWorldSpace* world = cell->worldSpace;
	if (world && world->cellMap && !cell->IsInterior() && cell->coords)
	{
		SInt32 baseX = (SInt32)cell->coords->x;
		SInt32 baseY = (SInt32)cell->coords->y;

		for (SInt32 gx = -1; gx <= 1; gx++)
		{
			for (SInt32 gy = -1; gy <= 1; gy++)
			{
				if (gx == 0 && gy == 0) continue;
				//mask before shifting, negative cell coords make the raw shift formally UB
				UInt32 key = (((UInt32)(baseX + gx) & 0xFFFF) << 16) | ((UInt32)(baseY + gy) & 0xFFFF);
				collectCell(world->cellMap->Lookup(key));
			}
		}
	}

	for (int i = 0; i < candidateCount; i++)
	{
		Actor* actor = candidates[i];
		int detVal = Engine::Actor_GetDetectionValue(actor, perpetrator);
		if (detVal >= detectionThreshold)
			out.push_back({ actor, detVal, actor->refID });
	}
}

}
