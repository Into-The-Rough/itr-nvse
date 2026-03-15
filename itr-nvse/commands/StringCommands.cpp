#include "nvse/PluginAPI.h"
#include "nvse/CommandTable.h"
#include "nvse/GameAPI.h"
#include "nvse/ParamInfos.h"
#include "StringCommands.h"
#include <cctype>
#include <cstring>
#include <string>
#include <algorithm>

#include "internal/globals.h"
extern NVSEArrayVarInterface* g_arrInterface;

static NVSEStringVarInterface* g_strInterface = nullptr;
static bool (*ExtractArgsEx)(ParamInfo* paramInfo, void* scriptData, UInt32* opcodeOffsetPtr, Script* scriptObj, ScriptEventList* eventList, ...) = nullptr;

#define EXTRACT_ARGS_EX paramInfo, scriptData, opcodeOffsetPtr, scriptObj, eventList

DEFINE_COMMAND_PLUGIN(Sv_TrimStr, "trims whitespace from string", 0, 1, kParams_OneString)

static ParamInfo kParams_OneArray_OneOptionalString[2] = {
	{"array", kParamType_Integer, 0},
	{"delimiter", kParamType_String, 1}
};
DEFINE_COMMAND_PLUGIN(Sv_Join, "joins array elements into string", 0, 2, kParams_OneArray_OneOptionalString)
DEFINE_COMMAND_PLUGIN(Sv_Reverse, "reverses a string", 0, 1, kParams_OneString)

bool Cmd_Sv_Reverse_Execute(COMMAND_ARGS)
{
	*result = 0;
	char srcString[0x200];
	srcString[0] = 0;

	if (!ExtractArgsEx(EXTRACT_ARGS_EX, &srcString))
		return true;

	std::string str(srcString);
	std::reverse(str.begin(), str.end());
	g_strInterface->Assign(PASS_COMMAND_ARGS, str.c_str());
	return true;
}

bool Cmd_Sv_Join_Execute(COMMAND_ARGS)
{
	*result = 0;
	UInt32 arrID = 0;
	char delimiter[0x100] = "";

	if (!ExtractArgsEx(EXTRACT_ARGS_EX, &arrID, &delimiter))
		return true;

	NVSEArrayVarInterface::Array* arr = g_arrInterface->LookupArrayByID(arrID);
	if (!arr)
	{
		g_strInterface->Assign(PASS_COMMAND_ARGS, "");
		return true;
	}

	UInt32 size = g_arrInterface->GetArraySize(arr);
	if (size == 0)
	{
		g_strInterface->Assign(PASS_COMMAND_ARGS, "");
		return true;
	}

	NVSEArrayVarInterface::Element* elements = new NVSEArrayVarInterface::Element[size];
	g_arrInterface->GetElements(arr, elements, nullptr);

	std::string joined;
	for (UInt32 i = 0; i < size; i++)
	{
		if (i > 0 && delimiter[0])
			joined += delimiter;

		if (elements[i].GetType() == NVSEArrayVarInterface::Element::kType_String)
			joined += elements[i].String();
		else if (elements[i].GetType() == NVSEArrayVarInterface::Element::kType_Numeric)
			joined += std::to_string(elements[i].Number());
	}

	delete[] elements;
	g_strInterface->Assign(PASS_COMMAND_ARGS, joined.c_str());
	return true;
}

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

	return true;
}

void StringCommands_RegisterCommands(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterTypedCommand(&kCommandInfo_Sv_TrimStr, kRetnType_String);
	nvse->RegisterTypedCommand(&kCommandInfo_Sv_Join, kRetnType_String);
	nvse->RegisterTypedCommand(&kCommandInfo_Sv_Reverse, kRetnType_String);
}
