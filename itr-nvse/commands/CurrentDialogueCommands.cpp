//GetCurrentDialogue - reads the line DialogueTextFilter confirmed, process structs hold
//nothing for barks

#include "CurrentDialogueCommands.h"
#include "nvse/PluginAPI.h"
#include "nvse/CommandTable.h"
#include "nvse/GameAPI.h"
#include "nvse/GameForms.h"
#include "nvse/GameObjects.h"
#include "handlers/DialogueTextFilter.h"

static NVSEStringVarInterface* g_strInterface = nullptr;

DEFINE_COMMAND_PLUGIN(GetCurrentDialogue, "Returns the dialogue line the actor is speaking right now, empty string when silent", 1, 0, NULL)

bool Cmd_GetCurrentDialogue_Execute(COMMAND_ARGS)
{
	*result = 0;
	if (!g_strInterface)
		return true;

	const char* text = nullptr;
	if (thisObj && (thisObj->typeID == kFormType_ACHR || thisObj->typeID == kFormType_ACRE))
		text = DialogueTextFilter::GetSpokenLine(thisObj->refID);

	g_strInterface->Assign(PASS_COMMAND_ARGS, text ? text : "");
	return true;
}

namespace CurrentDialogueCommands {
void Init(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	g_strInterface = (NVSEStringVarInterface*)nvse->QueryInterface(kInterface_StringVar);
}

void RegisterCommands(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterTypedCommand(&kCommandInfo_GetCurrentDialogue, kRetnType_String);
}
}
