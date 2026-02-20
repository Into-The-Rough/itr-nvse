//kNVSE event registration for ITR events
#include "NVSEMinimal.h"
#include "EventDispatch.h"
#include "globals.h"

EventManager::Interface* g_eventManagerInterface = nullptr;

void ITR_InitEventManager(void* nvseInterface)
{
	auto* nvse = reinterpret_cast<NVSEInterface*>(nvseInterface);
	g_eventManagerInterface = reinterpret_cast<EventManager::Interface*>(
		nvse->QueryInterface(EventManager::kInterface_EventManager));

	if (g_eventManagerInterface)
		Log("EventManager interface acquired");
	else
		Log("EventManager interface not available (kNVSE not installed?)");
}

void ITR_RegisterEvents()
{
	if (!g_eventManagerInterface) return;

	using namespace EventManager;

	unsigned char twoForms[] = { kParam_Form, kParam_Form };
	unsigned char oneForm[] = { kParam_Form };
	unsigned char stealParams[] = { kParam_Form, kParam_Form, kParam_Form, kParam_Form, kParam_Int };

	g_eventManagerInterface->RegisterEvent("ITR:OnWeaponJam", 2, twoForms, kFlag_FlushOnLoad);
	g_eventManagerInterface->RegisterEvent("ITR:OnWeaponDrop", 2, twoForms, kFlag_FlushOnLoad);
	g_eventManagerInterface->RegisterEvent("ITR:OnFrenzy", 1, oneForm, kFlag_FlushOnLoad);
	g_eventManagerInterface->RegisterEvent("ITR:OnSteal", 5, stealParams, kFlag_FlushOnLoad);

	//corner message: text(str), emotion(int), iconPath(str), soundPath(str), displayTime(float), metaType(int)
	unsigned char cornerParams[] = { kParam_String, kParam_Int, kParam_String, kParam_String, kParam_Float, kParam_Int };
	g_eventManagerInterface->RegisterEvent("ITR:OnCornerMessage", 6, cornerParams, kFlag_FlushOnLoad);

	//dialogue: speaker(form), topic(form), topicInfo(form), text(str), voicePath(str)
	unsigned char dialogueParams[] = { kParam_Form, kParam_Form, kParam_Form, kParam_String, kParam_String };
	g_eventManagerInterface->RegisterEvent("ITR:OnDialogueText", 5, dialogueParams, kFlag_FlushOnLoad);

	//double tap: key(int)
	unsigned char oneInt[] = { kParam_Int };
	g_eventManagerInterface->RegisterEvent("ITR:OnDoubleTap", 1, oneInt, kFlag_FlushOnLoad);

	//key held: key(int), duration(float)
	unsigned char keyHeldParams[] = { kParam_Int, kParam_Float };
	g_eventManagerInterface->RegisterEvent("ITR:OnKeyHeld", 2, keyHeldParams, kFlag_FlushOnLoad);

	//combat procedure: actor(form), procType(int), isAction(int)
	unsigned char combatProcParams[] = { kParam_Form, kParam_Int, kParam_Int };
	g_eventManagerInterface->RegisterEvent("ITR:OnCombatProcedure", 3, combatProcParams, kFlag_FlushOnLoad);

	//console open/close: no params
	g_eventManagerInterface->RegisterEvent("ITR:OnConsoleOpen", 0, nullptr, kFlag_FlushOnLoad);
	g_eventManagerInterface->RegisterEvent("ITR:OnConsoleClose", 0, nullptr, kFlag_FlushOnLoad);

	//entry point: perk(form), entryPointID(int), actor(form), filterForm(form)
	unsigned char entryPointParams[] = { kParam_Form, kParam_Int, kParam_Form, kParam_Form };
	g_eventManagerInterface->RegisterEvent("ITR:OnEntryPoint", 4, entryPointParams, kFlag_FlushOnLoad);

	//jump/land: actor(form), fallTime(float)
	unsigned char landedParams[] = { kParam_Form, kParam_Float };
	g_eventManagerInterface->RegisterEvent("ITR:OnActorLanded", 2, landedParams, kFlag_FlushOnLoad);
	g_eventManagerInterface->RegisterEvent("ITR:OnJumpStart", 1, oneForm, kFlag_FlushOnLoad);

	//key disabled/enabled: keycode(int), mask(int)
	unsigned char twoInts[] = { kParam_Int, kParam_Int };
	g_eventManagerInterface->RegisterEvent("ITR:OnKeyDisabled", 2, twoInts, kFlag_FlushOnLoad);
	g_eventManagerInterface->RegisterEvent("ITR:OnKeyEnabled", 2, twoInts, kFlag_FlushOnLoad);

	//menu filter change: menuID(int), oldFilter(int), newFilter(int), side(int)
	unsigned char fourInts[] = { kParam_Int, kParam_Int, kParam_Int, kParam_Int };
	g_eventManagerInterface->RegisterEvent("ITR:OnMenuFilterChange", 4, fourInts, kFlag_FlushOnLoad);

	//menu side change: menuID(int), oldSide(int), newSide(int)
	unsigned char threeInts[] = { kParam_Int, kParam_Int, kParam_Int };
	g_eventManagerInterface->RegisterEvent("ITR:OnMenuSideChange", 3, threeInts, kFlag_FlushOnLoad);

	//sound played/completed: filePath(str), flags(int), soundForm(form)
	unsigned char soundParams[] = { kParam_String, kParam_Int, kParam_Form };
	g_eventManagerInterface->RegisterEvent("ITR:OnSoundPlayed", 3, soundParams, kFlag_FlushOnLoad);
	g_eventManagerInterface->RegisterEvent("ITR:OnSoundCompleted", 3, soundParams, kFlag_FlushOnLoad);

	Log("ITR events registered with EventManager");
}
