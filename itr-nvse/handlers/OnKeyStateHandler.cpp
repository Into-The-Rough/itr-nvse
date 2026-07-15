//provides DisableKeyEx/EnableKeyEx wrapper commands that fire events

#include "OnKeyStateHandler.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/EventDispatch.h"
#include "internal/DIHookView.h"

static DIHook::ControlView* g_diHookControl = nullptr;
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

	if (!g_diHookControl || !g_ExtractArgsEx)
		return true;

	if (!g_ExtractArgsEx(
			reinterpret_cast<ParamInfo*>(paramInfo),
			scriptData, opcodeOffsetPtr, scriptObj, eventList,
			&keycode, &mask))
		return true;

	UInt32 effectiveMask = mask ? mask : DIHook::kDisable_All;
	g_diHookControl->SetKeyDisableState(keycode, true, effectiveMask);

	DispatchKeyDisabledEvent(keycode, effectiveMask);
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

	if (!g_diHookControl || !g_ExtractArgsEx)
		return true;

	if (!g_ExtractArgsEx(
			reinterpret_cast<ParamInfo*>(paramInfo),
			scriptData, opcodeOffsetPtr, scriptObj, eventList,
			&keycode, &mask))
		return true;

	UInt32 effectiveMask = mask ? mask : DIHook::kDisable_All;
	g_diHookControl->SetKeyDisableState(keycode, false, effectiveMask);

	DispatchKeyEnabledEvent(keycode, effectiveMask);
	*result = 1;
	return true;
}

namespace OnKeyStateHandler {
bool Init(void* nvseInterface)
{
	NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
	if (nvse->isEditor) return false;

	auto* script = (NVSEScriptInterface*)nvse->QueryInterface(kInterface_Script);
	if (!script) return false;
	g_ExtractArgsEx = script->ExtractArgsEx;

	auto* dataInterface = reinterpret_cast<NVSEDataInterface*>(nvse->QueryInterface(kInterface_Data));
	if (!dataInterface) return false;

	g_diHookControl = reinterpret_cast<DIHook::ControlView*>(dataInterface->GetSingleton(DIHook::kNVSEData_DIHookControl));
	if (!g_diHookControl) return false;

	return true;
}

void RegisterCommands(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterCommand(&kCommandInfo_DisableKeyEx);
	nvse->RegisterCommand(&kCommandInfo_EnableKeyEx);
}
}
