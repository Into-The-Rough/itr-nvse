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

	Log("ITR events registered with EventManager");
}
