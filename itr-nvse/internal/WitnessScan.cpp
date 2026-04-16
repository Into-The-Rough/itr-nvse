#include "WitnessScan.h"
#include "EngineFunctions.h"
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

	TESObjectCELL* cell = perpetrator->parentCell;

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

		//skip teammates — they never witness crimes committed by the player
		if (*reinterpret_cast<UInt8*>(reinterpret_cast<char*>(actor) + 0x18D))
			continue;

		float dx = actor->posX - cx;
		float dy = actor->posY - cy;
		float dz = actor->posZ - cz;
		float distSq = dx * dx + dy * dy + dz * dz;
		if (distSq > radiusSq) continue;

		int detVal = Engine::Actor_GetDetectionValue(actor, perpetrator);
		if (detVal >= detectionThreshold)
			out.push_back({ actor, detVal });
	}
}

}
