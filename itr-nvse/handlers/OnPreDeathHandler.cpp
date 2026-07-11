//cancellable pre-death event. veto forces the essential-down survive branch inside Actor::Kill
//(knockdown, minimal-health regen). we detour the two mid-function call sites 0x89DE2D (setting
//getter) and 0x89DE40 (GetIsEssential) rather than Kill's prologue, which JG and others may own.
//fail-loud, all-or-nothing, never touches the base form flag or the GameSetting

#include <Windows.h>

#include "OnPreDeathHandler.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/Detours.h"
#include "internal/EngineFunctions.h"
#include "internal/EventDispatch.h"
#include "internal/GameGlobals.h"
#include "internal/GameSDK.h"

extern void Log(const char* fmt, ...);

namespace OnPreDeathHandler {

constexpr char kEventName[] = "ITR:OnPreDeath";

typedef char* (__thiscall* SettingGetter_t)(void* settingObj);   //sub_408D60, returns ptr to setting data byte
typedef char  (__thiscall* GetIsEssential_t)(void* actor);       //sub_87F3D0 Actor::GetIsEssential

static Detours::CallDetour s_settingDetour;
static Detours::CallDetour s_essentialDetour;
static SettingGetter_t s_origSettingGetter = nullptr;
static GetIsEssential_t s_origGetIsEssential = nullptr;

static DWORD s_mainThreadId = 0;

//held at 1 so the caller's movzx edx,[eax] reads a nonzero setting value and reaches the
//essential check. never written after init.
static char s_forcedEssentialDown = 1;

static void PreDeathProbe(TESObjectREFR*, void*) {}
static EventDispatch::ListenerProbe s_probe = { kEventName, "ITR_OnPreDeathProbe", PreDeathProbe };

//while a PreDeath listener is registered, force the essential-down setting on for the essential
//check inside Kill. globally-disabled essential-down is overridden here, matching the
//deterministic-veto requirement. no dispatch happens at this site.
static char* __fastcall Hook_EssentialDownSetting(void* settingObj, void*)
{
	char* value = s_origSettingGetter(settingObj);
	if (*value) return value;                          //setting already on, vanilla
	if (s_probe.hasListeners) return &s_forcedEssentialDown;
	return value;                                      //off and no listeners, vanilla lethal path
}

static bool DispatchResultCb(NVSEArrayVarInterface::Element& result, void* shouldDieAddr)
{
	UInt32& shouldDie = *static_cast<UInt32*>(shouldDieAddr);
	if (shouldDie && result.IsValid())
	{
		if (result.type == NVSEArrayVarInterface::Element::kType_Numeric)
			shouldDie = (result.num != 0.0) ? 1 : 0;
	}
	return true;
}

//returns the value Kill uses as GetIsEssential's result: 1 forces the engine essential-down
//survive path, 0 continues to the lethal path.
static char __cdecl DecidePreDeath(Actor* actor, Actor* killer)
{
	if (s_origGetIsEssential(actor))
		return 1;                                      //vanilla essential survives, no event

	if (!s_probe.hasListeners)
		return 0;

	if ((void*)actor == (void*)*g_thePlayerPtr)
		return 0;                                      //player death excluded

	if (!Engine::Actor_GetProcess(actor))              //0x8D8520 MobileObject::GetBaseProcess
		return 0;                                      //essential-down needs a base process

	if (GetCurrentThreadId() != s_mainThreadId)
	{
		static volatile LONG loggedOffThread = 0;
		if (InterlockedCompareExchange(&loggedOffThread, 1, 0) == 0)
			Log("OnPreDeath: Kill ran off main thread, declining to dispatch");
		return 0;
	}

	if (!g_eventManagerInterface)
		return 0;

	//lifeState is not yet committed here, so two handlers killing each other's actor recurse
	//unbounded. cap the depth and let deaths past it proceed, breaking the cycle
	static UInt32 s_depth = 0;
	if (s_depth >= 16)
	{
		static volatile LONG loggedDepth = 0;
		if (InterlockedCompareExchange(&loggedDepth, 1, 0) == 0)
			Log("OnPreDeath: recursion cap hit, letting death proceed");
		return 0;
	}

	UInt32 shouldDie = 1;
	s_depth++;
	g_eventManagerInterface->DispatchEventAlt(kEventName, DispatchResultCb, &shouldDie,
		(TESObjectREFR*)actor, actor, killer, &shouldDie);
	s_depth--;

	return shouldDie ? 0 : 1;                           //veto forces essential-down
}

//naked shim on the 0x89DE40 call site. reads Kill's caller frame directly: ecx already holds
//the dying actor (mov ecx,[ebp-18h] precedes the call), killer is at [ebx+8]. emulates the
//original sub_87F3D0 (__thiscall no-args, plain retn) so a bare ret is the correct cleanup.
__declspec(naked) void EssentialCheck_Shim()
{
	__asm
	{
		push [ebx+8]        //killer, Kill uses ebx as its aligned-frame arg pointer
		push ecx            //dying actor
		call DecidePreDeath
		add  esp, 8
		ret                 //al = result, matches sub_87F3D0 plain retn
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
	NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
	if (nvse->isEditor) return false;

	if (!g_eventManagerInterface)
	{
		Log("OnPreDeathHandler: g_eventManagerInterface not ready, aborting Init");
		return false;
	}

	s_mainThreadId = GetCurrentThreadId();

	using P = NVSEEventManagerInterface::ParamType;
	using F = NVSEEventManagerInterface::EventFlags;
	static P params[] = {
		P::eParamType_AnyForm,   //actor
		P::eParamType_AnyForm,   //killer
		P::eParamType_IntPtr,    //shouldDie
	};
	g_eventManagerInterface->RegisterEvent(kEventName, 3, params, F::kFlag_FlushOnLoad);

	if (!s_settingDetour.WriteRelCall(0x89DE2D, Hook_EssentialDownSetting))
	{
		Log("OnPreDeath: setting-getter call site at 0x89DE2D is not an E8 call, disabled");
		return false;
	}
	s_origSettingGetter = (SettingGetter_t)s_settingDetour.GetOverwrittenAddr();

	if (!s_essentialDetour.WriteRelCall(0x89DE40, EssentialCheck_Shim))
	{
		Log("OnPreDeath: GetIsEssential call site at 0x89DE40 is not an E8 call, disabled");
		s_settingDetour.Remove();
		s_origSettingGetter = nullptr;
		return false;
	}
	s_origGetIsEssential = (GetIsEssential_t)s_essentialDetour.GetOverwrittenAddr();

	if (!s_origSettingGetter || !s_origGetIsEssential)
	{
		Log("OnPreDeath: recorded original missing, backing out both detours");
		s_essentialDetour.Remove();
		s_settingDetour.Remove();
		s_origSettingGetter = nullptr;
		s_origGetIsEssential = nullptr;
		return false;
	}

	s_probe.Install();
	Log("OnPreDeath: both Kill call-site detours installed");
	return true;
}

}
