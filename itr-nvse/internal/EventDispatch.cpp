//kNVSE event registration for ITR events
#include "NVSEMinimal.h"
#include "EventDispatch.h"
#include "globals.h"

NVSEEventManagerInterface* g_eventManagerInterface = nullptr;

void ITR_InitEventManager(void* nvseInterface)
{
	auto* nvse = reinterpret_cast<NVSEInterface*>(nvseInterface);
	g_eventManagerInterface = reinterpret_cast<NVSEEventManagerInterface*>(
		nvse->QueryInterface(kInterface_EventManager));

	if (g_eventManagerInterface)
		Log("EventManager interface acquired");
	else
		Log("EventManager interface not available (kNVSE not installed?)");
}

void ITR_RegisterEvents()
{
	if (!g_eventManagerInterface) return;

	using P = NVSEEventManagerInterface::ParamType;
	using F = NVSEEventManagerInterface::EventFlags;

	//xNVSE stores the param array pointer, not a copy - must be static
	static P twoForms[] = { P::eParamType_AnyForm, P::eParamType_AnyForm };
	static P oneForm[] = { P::eParamType_AnyForm };
	static P stealParams[] = { P::eParamType_AnyForm, P::eParamType_AnyForm, P::eParamType_AnyForm, P::eParamType_AnyForm, P::eParamType_Int };

	g_eventManagerInterface->RegisterEvent("ITR:OnWeaponJam", 2, twoForms, F::kFlag_FlushOnLoad);
	g_eventManagerInterface->RegisterEvent("ITR:OnWeaponDrop", 2, twoForms, F::kFlag_FlushOnLoad);
	g_eventManagerInterface->RegisterEvent("ITR:OnFrenzy", 1, oneForm, F::kFlag_FlushOnLoad);
	g_eventManagerInterface->RegisterEvent("ITR:OnSteal", 5, stealParams, F::kFlag_FlushOnLoad);

	static P cornerParams[] = { P::eParamType_String, P::eParamType_Int, P::eParamType_String, P::eParamType_String, P::eParamType_Float, P::eParamType_Int };
	g_eventManagerInterface->RegisterEvent("ITR:OnCornerMessage", 6, cornerParams, F::kFlag_FlushOnLoad);

	static P dialogueParams[] = { P::eParamType_AnyForm, P::eParamType_AnyForm, P::eParamType_AnyForm, P::eParamType_String, P::eParamType_String };
	g_eventManagerInterface->RegisterEvent("ITR:OnDialogueText", 5, dialogueParams, F::kFlag_FlushOnLoad);

	static P oneInt[] = { P::eParamType_Int };
	g_eventManagerInterface->RegisterEvent("ITR:OnDoubleTap", 1, oneInt, F::kFlag_FlushOnLoad);

	static P keyHeldParams[] = { P::eParamType_Int, P::eParamType_Float };
	g_eventManagerInterface->RegisterEvent("ITR:OnKeyHeld", 2, keyHeldParams, F::kFlag_FlushOnLoad);

	static P combatProcParams[] = { P::eParamType_AnyForm, P::eParamType_Int, P::eParamType_Int };
	g_eventManagerInterface->RegisterEvent("ITR:OnCombatProcedure", 3, combatProcParams, F::kFlag_FlushOnLoad);

	g_eventManagerInterface->RegisterEvent("ITR:OnConsoleOpen", 0, nullptr, F::kFlag_FlushOnLoad);
	g_eventManagerInterface->RegisterEvent("ITR:OnConsoleClose", 0, nullptr, F::kFlag_FlushOnLoad);

	static P entryPointParams[] = { P::eParamType_AnyForm, P::eParamType_Int, P::eParamType_AnyForm, P::eParamType_AnyForm };
	g_eventManagerInterface->RegisterEvent("ITR:OnEntryPoint", 4, entryPointParams, F::kFlag_FlushOnLoad);

	static P landedParams[] = { P::eParamType_AnyForm, P::eParamType_Float };
	g_eventManagerInterface->RegisterEvent("ITR:OnActorLanded", 2, landedParams, F::kFlag_FlushOnLoad);
	g_eventManagerInterface->RegisterEvent("ITR:OnJumpStart", 1, oneForm, F::kFlag_FlushOnLoad);

	static P twoInts[] = { P::eParamType_Int, P::eParamType_Int };
	g_eventManagerInterface->RegisterEvent("ITR:OnKeyDisabled", 2, twoInts, F::kFlag_FlushOnLoad);
	g_eventManagerInterface->RegisterEvent("ITR:OnKeyEnabled", 2, twoInts, F::kFlag_FlushOnLoad);

	static P fourInts[] = { P::eParamType_Int, P::eParamType_Int, P::eParamType_Int, P::eParamType_Int };
	g_eventManagerInterface->RegisterEvent("ITR:OnMenuFilterChange", 4, fourInts, F::kFlag_FlushOnLoad);

	static P threeInts[] = { P::eParamType_Int, P::eParamType_Int, P::eParamType_Int };
	g_eventManagerInterface->RegisterEvent("ITR:OnMenuSideChange", 3, threeInts, F::kFlag_FlushOnLoad);

	static P soundParams[] = { P::eParamType_String, P::eParamType_Int, P::eParamType_AnyForm };
	g_eventManagerInterface->RegisterEvent("ITR:OnSoundPlayed", 3, soundParams, F::kFlag_FlushOnLoad);
	g_eventManagerInterface->RegisterEvent("ITR:OnSoundCompleted", 3, soundParams, F::kFlag_FlushOnLoad);

	Log("ITR events registered with EventManager");
}
