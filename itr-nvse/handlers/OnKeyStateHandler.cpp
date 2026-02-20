//provides DisableKeyEx/EnableKeyEx wrapper commands that fire events

#include <cstdio>
#include <Windows.h>

#include "OnKeyStateHandler.h"
#include "internal/NVSEMinimal.h"
#include "internal/EventDispatch.h"

static NVSEConsoleInterface* g_okshConsole = nullptr;
static bool (*g_ExtractArgsEx)(ParamInfo*, void*, UInt32*, Script*, ScriptEventList*, ...) = nullptr;

static void DispatchKeyDisabledEvent(UInt32 keycode, UInt32 mask)
{
	if (g_eventManagerInterface)
		g_eventManagerInterface->DispatchEvent("ITR:OnKeyDisabled", nullptr, (int)keycode, (int)mask);
}

static void DispatchKeyEnabledEvent(UInt32 keycode, UInt32 mask)
{
	if (g_eventManagerInterface)
		g_eventManagerInterface->DispatchEvent("ITR:OnKeyEnabled", nullptr, (int)keycode, (int)mask);
}

static ParamInfo kParams_KeyEx[2] = {
	{"keycode", kParamType_Integer, 0},
	{"mask",    kParamType_Integer, 1},
};

DEFINE_COMMAND_PLUGIN(DisableKeyEx,
	"Disables a key and fires OnKeyDisabled event. Args: keycode, mask (optional)",
	0, 2, kParams_KeyEx);

bool Cmd_DisableKeyEx_Execute(COMMAND_ARGS)
{
	*result = 0;
	UInt32 keycode = 0;
	UInt32 mask = 0;

	if (!g_ExtractArgsEx(
			reinterpret_cast<ParamInfo*>(paramInfo),
			scriptData, opcodeOffsetPtr, scriptObj, eventList,
			&keycode, &mask))
		return true;

	char cmd[64];
	if (mask)
		sprintf_s(cmd, "DisableKey %d %d", keycode, mask);
	else
		sprintf_s(cmd, "DisableKey %d", keycode);

	if (g_okshConsole)
		g_okshConsole->RunScriptLine(cmd, nullptr);

	DispatchKeyDisabledEvent(keycode, mask);
	*result = 1;
	return true;
}

DEFINE_COMMAND_PLUGIN(EnableKeyEx,
	"Enables a key and fires OnKeyEnabled event. Args: keycode, mask (optional)",
	0, 2, kParams_KeyEx);

bool Cmd_EnableKeyEx_Execute(COMMAND_ARGS)
{
	*result = 0;
	UInt32 keycode = 0;
	UInt32 mask = 0;

	if (!g_ExtractArgsEx(
			reinterpret_cast<ParamInfo*>(paramInfo),
			scriptData, opcodeOffsetPtr, scriptObj, eventList,
			&keycode, &mask))
		return true;

	char cmd[64];
	if (mask)
		sprintf_s(cmd, "EnableKey %d %d", keycode, mask);
	else
		sprintf_s(cmd, "EnableKey %d", keycode);

	if (g_okshConsole)
		g_okshConsole->RunScriptLine(cmd, nullptr);

	DispatchKeyEnabledEvent(keycode, mask);
	*result = 1;
	return true;
}

bool OKSH_Init(void* nvseInterface)
{
	NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
	if (nvse->isEditor) return false;

	auto* script = (NVSEScriptInterface*)nvse->QueryInterface(kInterface_Script);
	if (!script) return false;
	g_ExtractArgsEx = script->ExtractArgsEx;

	g_okshConsole = (NVSEConsoleInterface*)nvse->QueryInterface(kInterface_Console);
	if (!g_okshConsole) return false;

	nvse->SetOpcodeBase(0x4008);
	nvse->RegisterCommand(&kCommandInfo_DisableKeyEx);
	nvse->SetOpcodeBase(0x4009);
	nvse->RegisterCommand(&kCommandInfo_EnableKeyEx);

	return true;
}
