//IsSaying - check if an actor is currently saying a specific topic or topic info

#include "IsSayingCommand.h"
#include "nvse/PluginAPI.h"
#include "nvse/CommandTable.h"
#include "nvse/GameAPI.h"
#include "nvse/GameForms.h"
#include "nvse/GameObjects.h"
#include "nvse/ParamInfos.h"
#include "internal/CallTemplates.h"

extern const _ExtractArgs ExtractArgs;

static ParamInfo kParams_IsSaying[] = {
	{"topicOrInfo", kParamType_AnyForm, 0},
};

DEFINE_COMMAND_PLUGIN(IsSaying, "Check if actor is currently saying a specific topic or topic info", 1, 1, kParams_IsSaying)

bool Cmd_IsSaying_Execute(COMMAND_ARGS)
{
	*result = 0;

	TESForm* form = nullptr;
	if (!ExtractArgs(EXTRACT_ARGS, &form))
		return true;
	if (!form || !thisObj)
		return true;
	if (thisObj->typeID != kFormType_ACHR && thisObj->typeID != kFormType_ACRE)
		return true;

	if (form->typeID == kFormType_DIAL)
	{
		TESTopic* cur = ThisCall<TESTopic*>(0x57BD40, thisObj); //Actor::GetExtraSayToTopic
		if (cur == (TESTopic*)form)
			*result = 1;
	}
	else if (form->typeID == kFormType_INFO)
	{
		TESTopicInfo* cur = ThisCall<TESTopicInfo*>(0x934430, thisObj); //Actor::GetExtraSayToTopicInfo
		if (cur == (TESTopicInfo*)form)
			*result = 1;
	}

	return true;
}

namespace IsSayingCommand {
void RegisterCommands(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterCommand(&kCommandInfo_IsSaying);
}
}
