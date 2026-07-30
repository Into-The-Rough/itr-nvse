#pragma once

#include "internal/TempEffectLogic.h"

class TESObjectCELL;

namespace TempEffects
{
	void* AddParticle(TESObjectCELL* cell, float duration, const char* modelPath,
		const TempEffectLogic::Point3& rotation, const TempEffectLogic::Point3& position,
		float scale, int flags, void* attachNode);

	void* SpawnSurfaceParticle(TESObjectCELL* cell, float duration, const char* modelPath,
		const TempEffectLogic::Point3& position, const TempEffectLogic::Point3& normal,
		float scale, float yawDegrees);
}
