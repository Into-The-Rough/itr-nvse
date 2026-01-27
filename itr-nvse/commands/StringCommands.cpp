#include "nvse/PluginAPI.h"
#include "nvse/CommandTable.h"
#include "nvse/GameAPI.h"
#include "nvse/ParamInfos.h"
#include "StringCommands.h"
#include <cctype>
#include <cstring>

extern void Log(const char* fmt, ...);

static NVSEStringVarInterface* g_strInterface = nullptr;
static bool (*ExtractArgsEx)(ParamInfo* paramInfo, void* scriptData, UInt32* opcodeOffsetPtr, Script* scriptObj, ScriptEventList* eventList, ...) = nullptr;

#define EXTRACT_ARGS_EX paramInfo, scriptData, opcodeOffsetPtr, scriptObj, eventList

DEFINE_COMMAND_PLUGIN(Sv_TrimStr, "trims whitespace from string", 0, 1, kParams_OneString)

bool Cmd_Sv_TrimStr_Execute(COMMAND_ARGS)
{
	*result = 0;
	char srcString[0x200];
	srcString[0] = 0;

	if (!ExtractArgsEx(EXTRACT_ARGS_EX, &srcString))
		return true;

	//ltrim
	char* start = srcString;
	while (*start && isspace((unsigned char)*start))
		start++;

	//rtrim
	char* end = start + strlen(start);
	while (end > start && isspace((unsigned char)*(end - 1)))
		end--;
	*end = 0;

	g_strInterface->Assign(PASS_COMMAND_ARGS, start);
	return true;
}

bool StringCommands_Init(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;

	g_strInterface = (NVSEStringVarInterface*)nvse->QueryInterface(kInterface_StringVar);
	if (!g_strInterface)
	{
		Log("StringCommands: Failed to get string interface");
		return false;
	}

	NVSEScriptInterface* scriptInterface = (NVSEScriptInterface*)nvse->QueryInterface(kInterface_Script);
	if (!scriptInterface)
	{
		Log("StringCommands: Failed to get script interface");
		return false;
	}

	ExtractArgsEx = scriptInterface->ExtractArgsEx;

	nvse->SetOpcodeBase(0x4042);
	nvse->RegisterTypedCommand(&kCommandInfo_Sv_TrimStr, kRetnType_String);

	return true;
}
