#include "HairColorCommands.h"
#include "nvse/PluginAPI.h"
#include "nvse/CommandTable.h"
#include "nvse/ParamInfos.h"

extern const _ExtractArgs ExtractArgs;

//TESNPC.uiHairColor at +0x1D8, packed 0x00BBGGRR (R low, B high), 24 bits used
constexpr UInt32 kOffset_HairColor = 0x1D8;

static TESForm* GetBaseNPC(TESObjectREFR* thisObj, TESForm* explicitTarget)
{
	TESForm* target = explicitTarget;
	if (!target && thisObj)
		target = *(TESForm**)((UInt8*)thisObj + 0x20);
	if (!target) return nullptr;
	UInt8 typeID = *((UInt8*)target + 0x04);
	return (typeID == kFormType_NPC) ? target : nullptr;
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

	TESForm* npc = GetBaseNPC(thisObj, explicitTarget);
	if (!npc) return true;

	*(UInt32*)((UInt8*)npc + kOffset_HairColor) =
		((b & 0xFF) << 16) | ((g & 0xFF) << 8) | (r & 0xFF);
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

	TESForm* npc = GetBaseNPC(thisObj, explicitTarget);
	if (!npc) return true;

	UInt32 packed = *(UInt32*)((UInt8*)npc + kOffset_HairColor) & 0x00FFFFFF;
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
