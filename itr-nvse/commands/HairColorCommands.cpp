#include "HairColorCommands.h"
#include "internal/GameLayout.h"
#include "nvse/PluginAPI.h"
#include "nvse/CommandTable.h"
#include "nvse/ParamInfos.h"

extern const _ExtractArgs ExtractArgs;

static TESNPC* GetBaseNPC(TESObjectREFR* thisObj, TESForm* explicitTarget)
{
	TESForm* target = explicitTarget;
	if (!target && thisObj)
		target = thisObj->baseForm;
	if (!target) return nullptr;
	return (target->typeID == kFormType_NPC) ? static_cast<TESNPC*>(target) : nullptr;
}

static ParamInfo kParams_SetHairColorAlt[4] = {
	{"red",   kParamType_Integer, 0},
	{"green", kParamType_Integer, 0},
	{"blue",  kParamType_Integer, 0},
	{"npc",   kParamType_AnyForm, 1},
};

static ParamInfo kParams_GetHairColorAlt[2] = {
	{"channel", kParamType_Integer, 1}, //1=R, 2=G, 3=B, else packed
	{"npc",     kParamType_AnyForm, 1},
};

DEFINE_COMMAND_PLUGIN(SetHairColorAlt, "Set NPC hair color from r,g,b (0-255 each)", 1, 4, kParams_SetHairColorAlt);
DEFINE_COMMAND_PLUGIN(GetHairColorAlt, "Get NPC hair color; channel 1=R 2=G 3=B, else packed", 1, 2, kParams_GetHairColorAlt);

bool Cmd_SetHairColorAlt_Execute(COMMAND_ARGS)
{
	*result = 0;
	UInt32 r = 0, g = 0, b = 0;
	TESForm* explicitTarget = nullptr;
	if (!ExtractArgs(EXTRACT_ARGS, &r, &g, &b, &explicitTarget))
		return true;

	TESNPC* npc = GetBaseNPC(thisObj, explicitTarget);
	if (!npc) return true;

	npc->hairColor = ((b & 0xFF) << 16) | ((g & 0xFF) << 8) | (r & 0xFF);
	*result = 1;
	return true;
}

bool Cmd_GetHairColorAlt_Execute(COMMAND_ARGS)
{
	*result = 0;
	UInt32 channel = 0;
	TESForm* explicitTarget = nullptr;
	if (!ExtractArgs(EXTRACT_ARGS, &channel, &explicitTarget))
		return true;

	TESNPC* npc = GetBaseNPC(thisObj, explicitTarget);
	if (!npc) return true;

	UInt32 packed = npc->hairColor & 0x00FFFFFF;
	switch (channel) {
		case 1: *result = (double)(packed & 0xFF); break;
		case 2: *result = (double)((packed >> 8) & 0xFF); break;
		case 3: *result = (double)((packed >> 16) & 0xFF); break;
		default: *result = (double)packed; break;
	}
	return true;
}

namespace HairColorCommands {
void RegisterCommands(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterCommand(&kCommandInfo_SetHairColorAlt);
	nvse->RegisterCommand(&kCommandInfo_GetHairColorAlt);
}
}
