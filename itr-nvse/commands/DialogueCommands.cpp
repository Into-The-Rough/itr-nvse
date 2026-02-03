//dialogue info commands
//GetDialogueInfoFlags - returns combined flags for a TESTopicInfo
//SetDialogueInfoFlags - sets combined flags for a TESTopicInfo (runtime only)

#include "DialogueCommands.h"
#include "nvse/PluginAPI.h"
#include "nvse/CommandTable.h"
#include "nvse/GameAPI.h"
#include "nvse/GameForms.h"
#include "nvse/ParamInfos.h"

extern const _ExtractArgs ExtractArgs;

constexpr UInt8 kFormType_TopicInfo = 0x46;

//TESTopicInfo layout (relevant offsets):
//0x18: ConditionList conditions
//0x20: UInt16 unk20
//0x22: UInt8 saidOnce
//0x23: UInt8 type
//0x24: UInt8 nextSpeaker
//0x25: UInt8 flags1
//0x26: UInt8 flags2

//combined flags (flags1 | flags2 << 8):
//0x0001 = Goodbye
//0x0002 = Random
//0x0004 = Say Once
//0x0008 = Run Immediately
//0x0010 = Info Refusal
//0x0020 = Random End
//0x0040 = Run For Rumors
//0x0080 = Speech Challenge
//0x0100 = Say Once A Day
//0x0200 = Always Darken
//0x1000 = Low Intelligence
//0x2000 = High Intelligence

DEFINE_COMMAND_PLUGIN(GetDialogueInfoFlags, "Returns combined flags for a dialogue info", 0, 1, kParams_OneForm)
DEFINE_COMMAND_PLUGIN(SetDialogueInfoFlags, "Sets combined flags for a dialogue info (runtime only)", 0, 2, kParams_OneForm_OneInt)

bool Cmd_GetDialogueInfoFlags_Execute(COMMAND_ARGS)
{
	*result = 0;
	TESForm* form = nullptr;

	if (!ExtractArgs(EXTRACT_ARGS, &form))
		return true;

	if (!form || form->typeID != kFormType_TopicInfo)
		return true;

	UInt8* info = (UInt8*)form;
	UInt8 flags1 = info[0x25];
	UInt8 flags2 = info[0x26];

	*result = flags1 | (flags2 << 8);
	return true;
}

bool Cmd_SetDialogueInfoFlags_Execute(COMMAND_ARGS)
{
	*result = 0;
	TESForm* form = nullptr;
	UInt32 flags = 0;

	if (!ExtractArgs(EXTRACT_ARGS, &form, &flags))
		return true;

	if (!form || form->typeID != kFormType_TopicInfo)
		return true;

	UInt8* info = (UInt8*)form;
	info[0x25] = flags & 0xFF;
	info[0x26] = (flags >> 8) & 0xFF;

	*result = 1;
	return true;
}

void DialogueCommands_Init(void* nvse)
{
	NVSEInterface* nvseIntf = (NVSEInterface*)nvse;
	nvseIntf->SetOpcodeBase(0x4038);
	nvseIntf->RegisterCommand(&kCommandInfo_GetDialogueInfoFlags);
	nvseIntf->RegisterCommand(&kCommandInfo_SetDialogueInfoFlags);
}
