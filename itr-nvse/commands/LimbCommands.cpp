#include "LimbCommands.h"
#include <cstdint>
#include "internal/EngineFunctions.h"
#include "internal/layout/ExtraData.h"

#include "nvse/PluginAPI.h"
#include "nvse/GameAPI.h"
#include "nvse/GameObjects.h"
#include "nvse/CommandTable.h"
#include "nvse/ParamInfos.h"

extern const _ExtractArgs ExtractArgs;

namespace {
	constexpr int kBodyPartCount = 15;

	enum LimbGoneType : int {
		kLimbGone_Intact = 0,
		kLimbGone_Dismembered = 1,
		kLimbGone_Exploded = 2,
	};

	const char* const kLimbGoneTypeNames[] = {"intact", "dismembered", "exploded"};

	int GetLimbGoneType(TESObjectREFR* ref, int bodyPart)
	{
		auto* limbs = static_cast<ExtraDismemberedLimbsView*>(
			Engine::BaseExtraList_GetByType(&ref->extraDataList, kExtraData_DismemberedLimbs));
		if (!limbs || !(limbs->dismemberedMask & (1u << bodyPart)))
			return kLimbGone_Intact;

		for (uint32_t i = 0; i < limbs->numLimbs; i++) {
			const DismemberedLimbView* limb = limbs->limbs ? limbs->limbs[i] : nullptr;
			if (limb && limb->bodyPart == bodyPart)
				return limb->exploded ? kLimbGone_Exploded : kLimbGone_Dismembered;
		}
		return kLimbGone_Dismembered;
	}

	static ParamInfo kParams_GetLimbGoneType[1] = {
		{"bodyPart", kParamType_Integer, 0},
	};
}

DEFINE_COMMAND_PLUGIN(GetLimbGoneType, "returns 0 if the body part is intact, 1 if it was dismembered, 2 if it was exploded; bodyPart uses DismemberLimb ids", 1, 1, kParams_GetLimbGoneType);

bool Cmd_GetLimbGoneType_Execute(COMMAND_ARGS)
{
	*result = 0;
	int bodyPart = -1;
	if (!ExtractArgs(EXTRACT_ARGS, &bodyPart))
		return true;
	if (!Engine::TESObjectREFR_IsActor(thisObj) || bodyPart < 0 || bodyPart >= kBodyPartCount)
		return true;

	const int type = GetLimbGoneType(thisObj, bodyPart);
	*result = type;
	if (IsConsoleMode())
		Console_Print("GetLimbGoneType >> %d (%s)", type, kLimbGoneTypeNames[type]);
	return true;
}

namespace LimbCommands {

	void RegisterCommands(void* nvsePtr)
	{
		NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
		nvse->RegisterCommand(&kCommandInfo_GetLimbGoneType);
	}

}
