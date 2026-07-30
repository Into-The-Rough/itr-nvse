#include "ParticleCommands.h"

#include "internal/GameSDK.h"
#include "internal/TempEffects.h"

#include "nvse/PluginAPI.h"
#include "nvse/GameAPI.h"
#include "nvse/GameObjects.h"
#include "nvse/CommandTable.h"
#include "nvse/ParamInfos.h"

extern const _ExtractArgs ExtractArgs;

namespace
{
	bool Cmd_SpawnTempParticle_Execute(COMMAND_ARGS)
	{
		*result = 0;
		if (!thisObj || !thisObj->parentCell)
			return true;

		char modelPath[260] = {};
		float duration = 0.0f;
		TempEffectLogic::Point3 position = {};
		TempEffectLogic::Point3 normal = {};
		float scale = 0.0f;
		float yawDegrees = 0.0f;

		if (!ExtractArgs(EXTRACT_ARGS, modelPath, &duration,
			&position.x, &position.y, &position.z,
			&normal.x, &normal.y, &normal.z, &scale, &yawDegrees))
		{
			return true;
		}

		if (!modelPath[0] || !TempEffectLogic::IsValid(duration, position, normal, scale, yawDegrees))
			return true;

		if (!TempEffects::SpawnSurfaceParticle(
			thisObj->parentCell, duration, modelPath, position, normal, scale, yawDegrees))
		{
			return true;
		}

		*result = 1;
		return true;
	}

	ParamInfo kParams_SpawnTempParticle[10] = {
		{ "modelPath", kParamType_String, 0 },
		{ "duration", kParamType_Float, 0 },
		{ "x", kParamType_Float, 0 },
		{ "y", kParamType_Float, 0 },
		{ "z", kParamType_Float, 0 },
		{ "normalX", kParamType_Float, 0 },
		{ "normalY", kParamType_Float, 0 },
		{ "normalZ", kParamType_Float, 0 },
		{ "scale", kParamType_Float, 0 },
		{ "yawDegrees", kParamType_Float, 1 },
	};

	CommandInfo kCommandInfo_SpawnTempParticle = {
		"SpawnTempParticle", "", 0,
		"spawn a temporary particle NIF at a world position, aligned to a surface normal",
		1, 10, kParams_SpawnTempParticle,
		Cmd_SpawnTempParticle_Execute, nullptr, nullptr, 0
	};
}

namespace ParticleCommands
{
	void RegisterCommands(void* nvsePtr)
	{
		auto* nvse = static_cast<NVSEInterface*>(nvsePtr);
		nvse->RegisterCommand(&kCommandInfo_SpawnTempParticle);
	}
}
