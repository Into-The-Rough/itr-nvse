#include "TempEffects.h"

#include "common/ITypes.h"

namespace
{
	using AddParticle_t = void* (__cdecl*)(TESObjectCELL*, float, const char*,
		TempEffectLogic::Point3, TempEffectLogic::Point3, float, int, void*);
	using SpawnParticle_t = void* (__cdecl*)(TESObjectCELL*, float, const char*,
		TempEffectLogic::Matrix3, TempEffectLogic::Point3, float, UInt32, void*);
}

namespace TempEffects
{
	void* AddParticle(TESObjectCELL* cell, float duration, const char* modelPath,
		const TempEffectLogic::Point3& rotation, const TempEffectLogic::Point3& position,
		float scale, int flags, void* attachNode)
	{
		return reinterpret_cast<AddParticle_t>(0x6890B0)(
			cell, duration, modelPath, rotation, position, scale, flags, attachNode);
	}

	void* SpawnSurfaceParticle(TESObjectCELL* cell, float duration, const char* modelPath,
		const TempEffectLogic::Point3& position, const TempEffectLogic::Point3& normal,
		float scale, float yawDegrees)
	{
		if (!cell || !modelPath || !modelPath[0] ||
			!TempEffectLogic::IsValid(duration, position, normal, scale, yawDegrees))
		{
			return nullptr;
		}

		TempEffectLogic::Matrix3 rotation;
		if (!TempEffectLogic::MakeSurfaceRotation(normal, yawDegrees, rotation))
			return nullptr;

		return reinterpret_cast<SpawnParticle_t>(0x689210)(
			cell, duration, modelPath, rotation, position, scale, 1, nullptr);
	}
}
