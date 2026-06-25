#include "ActorValueCommands.h"
#include "internal/GameSDK.h"
#include "internal/layout/Process.h"
#include "nvse/PluginAPI.h"
#include "nvse/CommandTable.h"
#include "nvse/GameAPI.h"
#include "nvse/ParamInfos.h"

extern const _ExtractArgs ExtractArgs;
extern void Log(const char* fmt, ...);

static bool IsActorRef(TESForm* form)
{
	if (!form) return false;
	return form->typeID == kFormType_ACHR || form->typeID == kFormType_ACRE;
}

static ParamInfo kParams_DamageActorValueAlt[3] = {
	{"avCode", kParamType_ActorValue, 0},
	{"amount", kParamType_Float, 0},
	{"attacker", kParamType_ObjectRef, 1},
};

DEFINE_COMMAND_PLUGIN(DamageActorValueAlt, "DamageActorValue with attacker for kill attribution", 1, 3, kParams_DamageActorValueAlt);

bool Cmd_DamageActorValueAlt_Execute(COMMAND_ARGS)
{
	*result = 0;
	if (!thisObj) return true;

	if (!IsActorRef(thisObj)) return true; //slot 0x3AC isn't on other vtables

	UInt32 avCode = 0;
	float amount = 0.0f;
	TESObjectREFR* attackerRef = nullptr;

	if (!ExtractArgs(EXTRACT_ARGS, &avCode, &amount, &attackerRef))
	{
		return true;
	}

	if (attackerRef)
	{
		if (!IsActorRef(attackerRef)) attackerRef = nullptr; //non-actor issuer would misread +0xA4
	}

	//increment fPlayerDamageDealt before damage so Actor::Kill sees it for XP
	if (avCode == 0x10 && amount > 0.0f && attackerRef) {
		BaseProcess* process = static_cast<Actor*>(thisObj)->baseProcess;
		if (auto* damageDealt = LowProcessGetDamageDealtCounter(process))
			*damageDealt += amount;
	}

	//0x3AC takes a signed delta; negative health deltas apply damage.
	typedef void (__thiscall *DamageAV_t)(void*, UInt32, float, void*);
	(*(DamageAV_t**)thisObj)[0x3AC / 4](thisObj, avCode, -amount, attackerRef);

	*result = 1;
	return true;
}

namespace ActorValueCommands {
void Init(void* nvse) {}

void RegisterCommands(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterCommand(&kCommandInfo_DamageActorValueAlt);
}
}
