//two mutable damage events, multiplier-combine semantics (handlers multiply together,
//negatives clamp to 0).
//tier 1 ITR:OnPreHitDamage - CallDetour the 5 E8 sites that call CalculateHitDamage
//(0x9B5A30) and mutate ActorHitData after the original returns, so we read the final
//value including JIP's perk/ammo modifiers. JIP prologue-jumps 0x9B5A30 itself, so we
//never patch it - our recorded call target runs JIP's version when present, vanilla
//otherwise, order-independent.
//tier 2 ITR:OnPreHealthDamage - chain-tolerant vtable swap of DamageActorValue
//(slot 0xEB) on the three actor classes, scaling the negative health delta before apply.
//JIP prologue-jumps the downstream dispatcher 0x66EE72, which we never touch.

#include <Windows.h>

#include "OnPreDamageHandler.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/EventDispatch.h"
#include "internal/Detours.h"
#include "internal/SafeWrite.h"
#include "internal/globals.h"

namespace OnPreDamageHandler {

constexpr char kEventHit[] = "ITR:OnPreHitDamage";
constexpr char kEventHealth[] = "ITR:OnPreHealthDamage";

//CalculateHitDamage 0x9B5A30, __thiscall(ActorHitData* ecx, UInt32 noBlock stack), retn 4
constexpr UInt32 kCallSites[5] = { 0x9B5623, 0x9B5702, 0x9B575C, 0x9B58AE, 0x9B5A10 };

//actor vtables, slot 0xEB = DamageActorValue(this, avCode, float delta, source).
//delta is negative for damage, avCode 16 = Health. each class overrides the slot with
//a distinct function, so we keep a per-vtable original.
constexpr UInt32 kVtbl_Character       = 0x1086A6C;
constexpr UInt32 kVtbl_Creature        = 0x10870AC;
constexpr UInt32 kVtbl_PlayerCharacter = 0x108AA3C;
constexpr UInt32 kSlot_DamageActorValue = 0xEB * 4; //0x3AC
constexpr UInt32 kAV_Health = 16;

//layout matches handlers/FakeHitHandler.cpp, verified against sub_9B5A30 decompile.
//only the fields up to the weapon pointer are needed here.
struct ActorHitData {
	void* source;        //0x00 attacker
	void* target;        //0x04 victim
	void* projectile;    //0x08
	UInt32 weaponAV;     //0x0C
	SInt32 hitLocation;  //0x10
	float healthDmg;     //0x14
	float wpnBaseDmg;    //0x18
	float fatigueDmg;    //0x1C
	float limbDmg;       //0x20
	float blockDTMod;    //0x24
	float armorDmg;      //0x28
	float weaponDmg;     //0x2C
	void* weapon;        //0x30
};

typedef void (__thiscall* CalcHitDamage_t)(ActorHitData*, UInt32);
typedef void (__thiscall* DamageActorValue_t)(void*, UInt32, float, void*);

static Detours::CallDetour s_hitDetours[5];

static DamageActorValue_t s_origDamageChar = nullptr;
static DamageActorValue_t s_origDamageCreature = nullptr;
static DamageActorValue_t s_origDamagePlayer = nullptr;

static DWORD g_mainThreadId = 0;
static bool s_offMainLoggedHit = false;
static UInt32 s_healthDeltaLogCount = 0;
static UInt32 s_reentryDepth = 0; //written only on the main thread

static void HitProbe(TESObjectREFR*, void*) {}
static void HealthProbe(TESObjectREFR*, void*) {}
static EventDispatch::ListenerProbe s_probeHit = { kEventHit, "ITR_OnPreHitDamageProbe", HitProbe };
static EventDispatch::ListenerProbe s_probeHealth = { kEventHealth, "ITR_OnPreHealthDamageProbe", HealthProbe };

static bool MultiplierResultCb(NVSEArrayVarInterface::Element& result, void* productAddr)
{
	float& product = *static_cast<float*>(productAddr);
	if (result.IsValid() && result.type == NVSEArrayVarInterface::Element::kType_Numeric) {
		float m = (float)result.num;
		if (m < 0.0f) m = 0.0f;
		product *= m;
	}
	return true;
}

//each call site gets its own thunk so it chains ITS site's recorded original, correct even
//if another plugin has separately patched one of the five sites. the worker holds the shared
//post-call logic
static void CalcHitWork(ActorHitData* hitData, UInt32 noBlock, CalcHitDamage_t orig)
{
	if (orig)
		orig(hitData, noBlock);

	if (!s_probeHit.hasListeners)
		return;
	if (GetCurrentThreadId() != g_mainThreadId) {
		if (!s_offMainLoggedHit) {
			Log("OnPreHitDamage: CalculateHitDamage off main thread, skipping dispatch");
			s_offMainLoggedHit = true;
		}
		return;
	}
	if (!hitData || !hitData->target)
		return;
	float healthDmg = hitData->healthDmg;
	if (healthDmg <= 0.0f)
		return;

	float product = 1.0f;
	g_eventManagerInterface->DispatchEventAlt(kEventHit, MultiplierResultCb, &product,
		reinterpret_cast<TESObjectREFR*>(hitData->target),
		reinterpret_cast<TESForm*>(hitData->target),
		reinterpret_cast<TESForm*>(hitData->source),
		reinterpret_cast<TESForm*>(hitData->weapon),
		PackEventFloatArg(healthDmg), hitData->hitLocation, &product);

	if (product != 1.0f) {
		if (product < 0.0f) product = 0.0f;
		hitData->healthDmg *= product;
		hitData->limbDmg *= product;
		hitData->fatigueDmg *= product;
	}
}

#define HIT_THUNK(N) static void __fastcall Hook_CalcHit_##N(ActorHitData* h, void*, UInt32 nb) \
	{ CalcHitWork(h, nb, (CalcHitDamage_t)s_hitDetours[N].GetOverwrittenAddr()); }
HIT_THUNK(0) HIT_THUNK(1) HIT_THUNK(2) HIT_THUNK(3) HIT_THUNK(4)
#undef HIT_THUNK
static void* const kHitThunks[5] = {
	(void*)Hook_CalcHit_0, (void*)Hook_CalcHit_1, (void*)Hook_CalcHit_2,
	(void*)Hook_CalcHit_3, (void*)Hook_CalcHit_4,
};

//our wrapper is only ever installed on these three vtables, so this always matches
static DamageActorValue_t PickDamageOrig(void* actor)
{
	UInt32 vtbl = *(UInt32*)actor;
	if (vtbl == kVtbl_Character) return s_origDamageChar;
	if (vtbl == kVtbl_Creature) return s_origDamageCreature;
	return s_origDamagePlayer;
}

static void __fastcall Hook_DamageActorValue(void* actor, void*, UInt32 avCode, float delta, void* source)
{
	DamageActorValue_t orig = PickDamageOrig(actor);

	if (avCode != kAV_Health || delta >= 0.0f || !s_probeHealth.hasListeners
		|| s_reentryDepth > 0 || GetCurrentThreadId() != g_mainThreadId) {
		if (orig) orig(actor, avCode, delta, source);
		return;
	}

	//guard spans the whole window: a handler that damages an actor during dispatch must
	//re-enter the fast path (s_reentryDepth>0) and apply unmodified, not recurse. depth is
	//written only on the main thread, which we already gated on above
	s_reentryDepth++;

	float product = 1.0f;
	g_eventManagerInterface->DispatchEventAlt(kEventHealth, MultiplierResultCb, &product,
		reinterpret_cast<TESObjectREFR*>(actor),
		reinterpret_cast<TESForm*>(actor),
		reinterpret_cast<TESForm*>(source),
		PackEventFloatArg(delta), &product);

	float newDelta = delta;
	if (product != 1.0f) {
		if (product < 0.0f) product = 0.0f;
		newDelta = delta * product;
	}

	if (newDelta != delta && s_healthDeltaLogCount < 5) {
		Log("OnPreHealthDamage: delta %.3f -> %.3f (x%.3f) actor=%p", delta, newDelta, product, actor);
		s_healthDeltaLogCount++;
	}

	if (orig) orig(actor, avCode, newDelta, source);
	s_reentryDepth--;
}

static bool InstallDamageSwap(UInt32 vtbl, DamageActorValue_t& origOut)
{
	UInt32 slotAddr = vtbl + kSlot_DamageActorValue;
	DamageActorValue_t orig = (DamageActorValue_t)*(UInt32*)slotAddr;
	if (!orig)
		return false;
	origOut = orig;
	SafeWrite::Write32(slotAddr, (UInt32)&Hook_DamageActorValue);
	return true;
}

static void RestoreDamageSwap(UInt32 vtbl, DamageActorValue_t orig)
{
	if (orig)
		SafeWrite::Write32(vtbl + kSlot_DamageActorValue, (UInt32)orig);
}

void InstallListenerProbe()
{
	s_probeHit.Install();
	s_probeHealth.Install();
}

void Update()
{
	s_probeHit.Refresh(false);
	s_probeHealth.Refresh(false);
}

bool Init(void* nvseInterface)
{
	NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
	if (nvse->isEditor) return false;

	if (!g_eventManagerInterface) {
		Log("OnPreDamageHandler: g_eventManagerInterface not ready, aborting Init");
		return false;
	}

	g_mainThreadId = GetCurrentThreadId();

	using P = NVSEEventManagerInterface::ParamType;
	using F = NVSEEventManagerInterface::EventFlags;

	static P hitParams[] = {
		P::eParamType_AnyForm, P::eParamType_AnyForm, P::eParamType_AnyForm,
		P::eParamType_Float, P::eParamType_Int, P::eParamType_IntPtr,
	};
	g_eventManagerInterface->RegisterEvent(kEventHit, 6, hitParams, F::kFlag_FlushOnLoad);

	static P healthParams[] = {
		P::eParamType_AnyForm, P::eParamType_AnyForm, P::eParamType_Float, P::eParamType_IntPtr,
	};
	g_eventManagerInterface->RegisterEvent(kEventHealth, 4, healthParams, F::kFlag_FlushOnLoad);

	int hitInstalled = 0;
	for (int i = 0; i < 5; i++) {
		if (s_hitDetours[i].WriteRelCall(kCallSites[i], (UInt32)kHitThunks[i]))
			hitInstalled++;
		else
			Log("OnPreHitDamage: call site 0x%X could not be detoured", kCallSites[i]);
	}
	Log("OnPreHitDamage: %d/5 call sites detoured", hitInstalled);

	bool t2 = InstallDamageSwap(kVtbl_Character, s_origDamageChar)
		&& InstallDamageSwap(kVtbl_Creature, s_origDamageCreature)
		&& InstallDamageSwap(kVtbl_PlayerCharacter, s_origDamagePlayer);
	if (!t2) {
		RestoreDamageSwap(kVtbl_Character, s_origDamageChar);
		RestoreDamageSwap(kVtbl_Creature, s_origDamageCreature);
		RestoreDamageSwap(kVtbl_PlayerCharacter, s_origDamagePlayer);
		s_origDamageChar = nullptr;
		s_origDamageCreature = nullptr;
		s_origDamagePlayer = nullptr;
		Log("OnPreHealthDamage: vtable swap failed, tier 2 disabled");
	}
	else {
		Log("OnPreHealthDamage: DamageActorValue slot swapped, originals Char=%08X Creature=%08X Player=%08X",
			(UInt32)s_origDamageChar, (UInt32)s_origDamageCreature, (UInt32)s_origDamagePlayer);
	}

	return hitInstalled > 0 || t2;
}

}
