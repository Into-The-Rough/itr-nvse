#include "ActorValueCommands.h"
#include "nvse/PluginAPI.h"
#include "nvse/CommandTable.h"
#include "nvse/GameAPI.h"
#include "nvse/ParamInfos.h"

extern const _ExtractArgs ExtractArgs;
extern void Log(const char* fmt, ...);

static ParamInfo kParams_DamageActorValueAlt[3] = {
	{"avCode", kParamType_ActorValue, 0},
	{"amount", kParamType_Float, 0},
	{"attacker", kParamType_AnyForm, 1},
};

DEFINE_COMMAND_PLUGIN(DamageActorValueAlt, "DamageActorValue with attacker for kill attribution", 1, 3, kParams_DamageActorValueAlt);

bool Cmd_DamageActorValueAlt_Execute(COMMAND_ARGS)
{
	*result = 0;
	if (!thisObj) return true;

	UInt32 avCode = 0;
	float amount = 0.0f;
	TESObjectREFR* attackerRef = nullptr;

	if (!ExtractArgs(EXTRACT_ARGS, &avCode, &amount, &attackerRef))
	{
		Log("DamageActorValueAlt: ExtractArgs failed");
		return true;
	}

	Log("DamageActorValueAlt: target=%08X av=%d amount=%.1f attacker=%08X",
		thisObj ? *(UInt32*)((UInt8*)thisObj + 0x0C) : 0,
		avCode, amount,
		attackerRef ? *(UInt32*)((UInt8*)attackerRef + 0x0C) : 0);

	//increment fPlayerDamageDealt (process+0xAC) BEFORE damage so Actor::Kill sees it for XP
	if (avCode == 0x10 && amount > 0.0f && attackerRef) {
		void* process = *(void**)((UInt8*)thisObj + 0x68);
		if (process)
			*(float*)((UInt8*)process + 0xAC) += amount;
	}

	//0x3AC = Actor::DamageActorValue(avCode, damage, attacker)
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
