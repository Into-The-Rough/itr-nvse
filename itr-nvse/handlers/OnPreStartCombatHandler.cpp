//fires before the vanilla startcombat command evaluates whether combat can begin

#include <Windows.h>

#include "OnPreStartCombatHandler.h"
#include "internal/Detours.h"
#include "internal/EventDispatch.h"
#include "internal/GameLayout.h"
#include "internal/NVSEPluginAPI.h"

extern void Log(const char* fmt, ...);

namespace OnPreStartCombatHandler {
namespace {

constexpr char kEventName[] = "ITR:OnPreStartCombat";
constexpr UInt32 kAddr_ExtractArgs = 0x5ACCB0;
constexpr UInt32 kAddr_ExtractArgsCall = 0x5C177D;

using ExtractStartCombatArgs_t = bool(__cdecl*)(ParamInfo*, void*, UInt32*, TESObjectREFR*,
	TESObjectREFR*, Script*, ScriptEventList*, TESObjectREFR**);

Detours::CallDetour s_extractArgsDetour;
DWORD s_mainThreadId = 0;
UInt32 s_dispatchDepth = 0;

void Probe(TESObjectREFR*, void*) {}
EventDispatch::ListenerProbe s_probe = { kEventName, "ITR_OnPreStartCombatProbe", Probe };

bool __cdecl Hook_ExtractArgs(ParamInfo* paramInfo, void* scriptData, UInt32* opcodeOffsetPtr,
	TESObjectREFR* thisObj, TESObjectREFR* containingObj, Script* scriptObj,
	ScriptEventList* eventList, TESObjectREFR** targetOut)
{
	auto extractArgs = reinterpret_cast<ExtractStartCombatArgs_t>(s_extractArgsDetour.GetOverwrittenAddr());
	if (!extractArgs)
		return false;

	bool extracted = extractArgs(paramInfo, scriptData, opcodeOffsetPtr, thisObj, containingObj,
		scriptObj, eventList, targetOut);
	if (!extracted || !g_eventManagerInterface)
		return extracted;
	if (!s_probe.hasListeners || GetCurrentThreadId() != s_mainThreadId)
		return extracted;
	if (!thisObj || !targetOut || !*targetOut)
		return extracted;
	if (!TESFormIsActorRef(thisObj) || !TESFormIsActorRef(*targetOut))
		return extracted;

	if (s_dispatchDepth >= 16)
		return extracted;

	s_dispatchDepth++;
	g_eventManagerInterface->DispatchEvent(kEventName, thisObj, *targetOut);
	s_dispatchDepth--;
	return extracted;
}

}

void InstallListenerProbe()
{
	s_probe.Install();
}

void Update()
{
	s_probe.Refresh(false);
}

bool Init(void* nvseInterface)
{
	NVSEInterface* nvse = static_cast<NVSEInterface*>(nvseInterface);
	if (nvse->isEditor)
		return false;

	if (!g_eventManagerInterface)
	{
		Log("OnPreStartCombatHandler: event manager not ready");
		return false;
	}
	s_mainThreadId = GetCurrentThreadId();

	using P = NVSEEventManagerInterface::ParamType;
	using F = NVSEEventManagerInterface::EventFlags;
	static P params[] = { P::eParamType_Reference };
	if (!g_eventManagerInterface->RegisterEvent(kEventName, 1, params, F::kFlag_FlushOnLoad))
	{
		Log("OnPreStartCombatHandler: event registration failed");
		return false;
	}

	if (!s_extractArgsDetour.WriteRelCall(kAddr_ExtractArgsCall, Hook_ExtractArgs))
	{
		Log("OnPreStartCombatHandler: StartCombat ExtractArgs call could not be detoured");
		return false;
	}
	UInt32 original = s_extractArgsDetour.GetOverwrittenAddr();
	Log("OnPreStartCombat: %08X hooked, original=%08X vanilla=%08X", kAddr_ExtractArgsCall,
		original, kAddr_ExtractArgs);

	Log("OnPreStartCombatHandler: hook installed");
	return true;
}

}
