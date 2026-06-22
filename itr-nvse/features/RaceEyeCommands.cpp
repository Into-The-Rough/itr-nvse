#include "RaceEyeCommands.h"

#include "nvse/PluginAPI.h"
#include "nvse/CommandTable.h"
#include "nvse/ParamInfos.h"
#include "nvse/GameForms.h"

extern const _ExtractArgs ExtractArgs;

namespace
{
	//AddEye dedups by formID, both free nodes on the game heap
	using AddEye_t = void(__thiscall*)(TESRace*, TESEyes*);
	using ClearEyes_t = void(__thiscall*)(TESRace*);
	const AddEye_t TESRace_AddEye = reinterpret_cast<AddEye_t>(0x613910);
	const ClearEyes_t TESRace_ClearEyes = reinterpret_cast<ClearEyes_t>(0x613950);

	TESEyes* AsEyes(TESForm* form)
	{
		return (form && form->typeID == kFormType_Eyes) ? static_cast<TESEyes*>(form) : nullptr;
	}

	struct EyeMatch
	{
		TESEyes* target;
		bool Accept(TESEyes* eye) { return eye == target; }
	};
}

bool Cmd_AddRaceEye_Execute(COMMAND_ARGS)
{
	*result = 0;
	TESRace* race = nullptr;
	TESForm* eyeForm = nullptr;
	if (!ExtractArgs(EXTRACT_ARGS, &race, &eyeForm))
		return true;

	TESEyes* eyes = AsEyes(eyeForm);
	if (!race || !eyes)
	{
		if (IsConsoleMode())
			Console_Print("AddRaceEye >> needs a race and an eyes form");
		return true;
	}

	TESRace_AddEye(race, eyes);
	*result = 1;
	return true;
}

bool Cmd_RemoveRaceEye_Execute(COMMAND_ARGS)
{
	*result = 0;
	TESRace* race = nullptr;
	TESForm* eyeForm = nullptr;
	if (!ExtractArgs(EXTRACT_ARGS, &race, &eyeForm))
		return true;

	TESEyes* eyes = AsEyes(eyeForm);
	if (!race || !eyes)
	{
		if (IsConsoleMode())
			Console_Print("RemoveRaceEye >> needs a race and an eyes form");
		return true;
	}

	EyeMatch match{ eyes };
	if (race->eyes.RemoveIf(match))
		*result = 1;
	return true;
}

bool Cmd_ClearRaceEyes_Execute(COMMAND_ARGS)
{
	*result = 0;
	TESRace* race = nullptr;
	if (!ExtractArgs(EXTRACT_ARGS, &race))
		return true;

	if (!race) return true;

	TESRace_ClearEyes(race);
	*result = 1;
	return true;
}

static ParamInfo kParams_AddRaceEye[2] = {
	{"race", kParamType_Race, 0},
	{"eyes", kParamType_AnyForm, 0},
};

static ParamInfo kParams_ClearRaceEyes[1] = {
	{"race", kParamType_Race, 0},
};

DEFINE_COMMAND_PLUGIN(AddRaceEye, "Make an eyes form selectable for a race in the character-creation menu (the eye must be playable). Not saved, re-run each session", 0, 2, kParams_AddRaceEye);
DEFINE_COMMAND_PLUGIN(RemoveRaceEye, "Remove an eyes form from a race's selectable eye list", 0, 2, kParams_AddRaceEye);
DEFINE_COMMAND_PLUGIN(ClearRaceEyes, "Empty a race's selectable eye list", 0, 1, kParams_ClearRaceEyes);

namespace RaceEyeCommands
{
	void RegisterCommands(void* nvsePtr)
	{
		NVSEInterface* nvse = static_cast<NVSEInterface*>(nvsePtr);
		nvse->RegisterCommand(&kCommandInfo_AddRaceEye);
		nvse->RegisterCommand(&kCommandInfo_RemoveRaceEye);
		nvse->RegisterCommand(&kCommandInfo_ClearRaceEyes);
	}
}
