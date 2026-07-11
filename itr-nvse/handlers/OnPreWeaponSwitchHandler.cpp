//cancellable pre-weapon-switch event, owns the CombatProcedureSwitchWeapon::Update detour.
//verdict is decided off the AI thread by stalling the procedure one tick while a synchronous
//dispatch resolves on the main thread. handlers SetFunctionValue 0 to veto, sticky at 0.

#include <Windows.h>

#include "OnPreWeaponSwitchHandler.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/Detours.h"
#include "internal/ScopedLock.h"
#include "internal/EngineFunctions.h"
#include "internal/EventDispatch.h"
#include "internal/GameSDK.h"
#include "internal/layout/Combat.h"

extern void Log(const char* fmt, ...);

namespace {

constexpr char kEventName[] = "ITR:OnPreWeaponSwitch";
constexpr UInt16 kStaleFrames = 300;
constexpr int kMaxVerdicts = 32;

enum VerdictState : UInt8 { kPending, kInFlight, kAllowed, kDenied };

struct Verdict {
	UInt32 refID;
	UInt32 proposedFormID;
	UInt32 currentFormID;   //equipped weapon captured on the AI thread at verdict time
	UInt16 ageFrames;
	UInt8  state;
};

Verdict g_verdicts[kMaxVerdicts];
int g_verdictCount = 0;

CRITICAL_SECTION g_stateLock;
volatile LONG g_stateLockInit = 0;
DWORD g_mainThreadId = 0;

OnPreWeaponSwitchHandler::ExternalBlockCheck_t g_externalBlockCheck = nullptr;

//0x9DA7C0 = CombatProcedureSwitchWeapon::Update
typedef void (__thiscall *SwitchWeaponUpdate_t)(void* procedure);
Detours::JumpDetour s_detour;

void PreWeaponSwitchProbe(TESObjectREFR*, void*) {}
EventDispatch::ListenerProbe s_probe = { kEventName, "ITR_OnPreWeaponSwitchProbe", PreWeaponSwitchProbe };

void EnsureStateLockInit()
{
	InitCriticalSectionOnce(&g_stateLockInit, &g_stateLock);
}

int FindVerdict(UInt32 refID)
{
	for (int i = 0; i < g_verdictCount; i++)
		if (g_verdicts[i].refID == refID)
			return i;
	return -1;
}

void EraseVerdictAt(int idx)
{
	g_verdicts[idx] = g_verdicts[--g_verdictCount];
}

void CallOriginal(void* procedure)
{
	auto orig = s_detour.GetTrampoline<SwitchWeaponUpdate_t>();
	if (orig) orig(procedure);
}

//equipped weapon at hook time, same chain the engine uses at 0x9DA805-0x9DA840
UInt32 CurrentWeaponFormID(Actor* actor)
{
	void* process = Engine::Actor_GetProcess(actor);   //0x8D8520 MobileObject::GetBaseProcess
	if (!process) return 0;
	void** vtbl = *(void***)process;
	typedef void* (__thiscall *GetWeaponInfo_t)(void*);
	void* weaponInfo = ((GetWeaponInfo_t)vtbl[0x148 / 4])(process);   //process vfunc 0x148, WeaponInfo*, may be null
	if (!weaponInfo) return 0;
	TESForm* weapon = *(TESForm**)((UInt8*)weaponInfo + 0x08);   //0x08 equipped weapon
	return weapon ? weapon->refID : 0;
}

bool DispatchResultCb(NVSEArrayVarInterface::Element& result, void* addr)
{
	UInt32& shouldSwitch = *static_cast<UInt32*>(addr);
	if (shouldSwitch && result.IsValid())
	{
		if (result.type == NVSEArrayVarInterface::Element::kType_Numeric)
			shouldSwitch = (result.num != 0.0) ? 1 : 0;
	}
	return true;
}

void __fastcall Hook_SwitchWeaponUpdate(void* procedure, void*)
{
	void* controller = CombatProcedureGetCombatController(procedure);
	Actor* actor = controller ? (Actor*)Engine::CombatController_GetPackageOwner(controller) : nullptr;
	if (!actor)
	{
		CallOriginal(procedure);
		return;
	}

	//preserve PreventWeaponSwitch semantics exactly, block wins over everything
	OnPreWeaponSwitchHandler::ExternalBlockCheck_t blockCheck = g_externalBlockCheck;
	if (blockCheck && blockCheck(actor))
	{
		CombatProcedureSetStatus(procedure, 2);
		return;
	}

	if (*(UInt32*)((UInt8*)procedure + 0x14))   //0x14 switch already started, never interfere mid-switch
	{
		CallOriginal(procedure);
		return;
	}

	if (!s_probe.hasListeners || g_stateLockInit != 2)
	{
		CallOriginal(procedure);
		return;
	}

	TESForm* proposed = *(TESForm**)((UInt8*)procedure + 0x10);   //0x10 proposed weapon, null means unequip
	UInt32 proposedFormID = proposed ? proposed->refID : 0;
	UInt32 currentFormID = CurrentWeaponFormID(actor);   //capture on the AI thread, equip state may change before dispatch
	UInt32 refID = actor->refID;

	bool callOriginal = false;
	bool block = false;
	{
		ScopedLock lock(&g_stateLock);
		int idx = FindVerdict(refID);
		if (idx < 0)
		{
			if (g_verdictCount < kMaxVerdicts)
			{
				Verdict& v = g_verdicts[g_verdictCount++];
				v.refID = refID;
				v.proposedFormID = proposedFormID;
				v.currentFormID = currentFormID;
				v.ageFrames = 0;
				v.state = kPending;
				//stall this tick, main-thread Update dispatches next frame
			}
			else
				callOriginal = true;   //table full, fail open rather than deadlock the AI
		}
		else
		{
			Verdict& v = g_verdicts[idx];
			if (v.state == kAllowed)
			{
				EraseVerdictAt(idx);
				callOriginal = true;
			}
			else if (v.state == kDenied)
			{
				EraseVerdictAt(idx);
				block = true;
			}
			else if (v.proposedFormID != proposedFormID)   //AI re-planned while pending, requeue
			{
				v.proposedFormID = proposedFormID;
				v.currentFormID = currentFormID;
				v.ageFrames = 0;
				v.state = kPending;
			}
			//else still waiting, stall
		}
	}

	if (block)
	{
		CombatProcedureSetStatus(procedure, 2);
		return;
	}
	if (callOriginal)
		CallOriginal(procedure);
}

} //anonymous namespace

namespace OnPreWeaponSwitchHandler {

void SetExternalBlockCheck(ExternalBlockCheck_t fn)
{
	g_externalBlockCheck = fn;
}

bool Init(void* nvseInterface)
{
	NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
	if (nvse->isEditor) return false;

	EnsureStateLockInit();
	g_mainThreadId = GetCurrentThreadId();

	if (g_eventManagerInterface)
	{
		using P = NVSEEventManagerInterface::ParamType;
		using F = NVSEEventManagerInterface::EventFlags;
		static P params[] = { P::eParamType_AnyForm, P::eParamType_AnyForm, P::eParamType_AnyForm, P::eParamType_IntPtr };
		g_eventManagerInterface->RegisterEvent(kEventName, 4, params, F::kFlag_FlushOnLoad);
	}
	else
		Log("OnPreWeaponSwitch: event manager not ready at Init");

	if (!s_detour.WriteRelJump(0x9DA7C0, Hook_SwitchWeaponUpdate, 6))   //CombatProcedureSwitchWeapon::Update, 6-byte prologue
	{
		Log("OnPreWeaponSwitch: hook install failed at 0x9DA7C0");
		return false;
	}
	if (!s_detour.GetTrampoline<SwitchWeaponUpdate_t>())
	{
		s_detour.Remove();
		Log("OnPreWeaponSwitch: trampoline null after install");
		return false;
	}

	s_probe.Install();
	Log("OnPreWeaponSwitch: hook installed");
	return true;
}

void Update()
{
	if (g_stateLockInit != 2) return;

	s_probe.Refresh(false);

	DWORD tid = GetCurrentThreadId();
	if (!g_mainThreadId)
		g_mainThreadId = tid;
	if (tid != g_mainThreadId)
		return;

	if (!g_eventManagerInterface)
		return;

	struct Pending { UInt32 refID; UInt32 formID; UInt32 currentFormID; };
	Pending toDispatch[kMaxVerdicts];
	int dispatchCount = 0;
	{
		ScopedLock lock(&g_stateLock);
		for (int i = 0; i < g_verdictCount; )
		{
			Verdict& v = g_verdicts[i];
			//all states age, a stored verdict whose procedure died mid-decision must not leak a slot
			if (++v.ageFrames > kStaleFrames)
			{
				Log("OnPreWeaponSwitch: verdict for %08X timed out (state %u), dropping", v.refID, v.state);
				EraseVerdictAt(i);
				continue;
			}
			if (v.state == kPending)
			{
				v.state = kInFlight;
				toDispatch[dispatchCount].refID = v.refID;
				toDispatch[dispatchCount].formID = v.proposedFormID;
				toDispatch[dispatchCount].currentFormID = v.currentFormID;
				dispatchCount++;
			}
			i++;
		}
	}

	for (int i = 0; i < dispatchCount; i++)
	{
		UInt32 refID = toDispatch[i].refID;
		UInt32 formID = toDispatch[i].formID;
		UInt32 currentFormID = toDispatch[i].currentFormID;

		Actor* actor = (Actor*)Engine::LookupFormByID(refID);
		bool gone = !actor || Engine::Actor_IsDead(actor, false);

		UInt8 verdict;
		if (gone)
			verdict = 0xFF;   //resolve failed, erase
		else
		{
			TESForm* proposed = formID ? (TESForm*)Engine::LookupFormByID(formID) : nullptr;
			TESForm* current = currentFormID ? (TESForm*)Engine::LookupFormByID(currentFormID) : nullptr;
			UInt32 shouldSwitch = 1;
			g_eventManagerInterface->DispatchEventAlt(kEventName, DispatchResultCb, &shouldSwitch,
				(TESObjectREFR*)actor, actor, proposed, current, &shouldSwitch);
			verdict = shouldSwitch ? kAllowed : kDenied;
		}

		ScopedLock lock(&g_stateLock);
		int idx = FindVerdict(refID);
		if (idx < 0)
			continue;
		Verdict& v = g_verdicts[idx];
		if (v.state != kInFlight || v.proposedFormID != formID)
			continue;   //re-planned during dispatch, leave for re-dispatch
		if (verdict == 0xFF)
			EraseVerdictAt(idx);
		else
			v.state = verdict;
	}
}

void ClearState()
{
	EnsureStateLockInit();
	ScopedLock lock(&g_stateLock);
	g_verdictCount = 0;
}

}
