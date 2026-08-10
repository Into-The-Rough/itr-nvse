
#include <Windows.h>

#include "OnPreDamageHandler.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/EventDispatch.h"
#include "internal/Detours.h"
#include "internal/SafeWrite.h"
#include "internal/GameGlobals.h"
#include "internal/GameLayout.h"
#include "internal/globals.h"

namespace OnPreDamageHandler {

constexpr char kEventHit[] = "ITR:OnPreHitDamage";
constexpr char kEventHealth[] = "ITR:OnPreHealthDamage";

//the complete xref set of Actor::HitMe 0x89A760, every one a direct E8 call
constexpr UInt32 kHitCallSites[] = { 0x87C4DA, 0x89A738, 0x8B91E1, 0x9B0503, 0x9C1E96, 0x9CBDE8 };
constexpr UInt32 kHitCallCount = sizeof(kHitCallSites) / sizeof(kHitCallSites[0]);

//actor vtables, slot 0xEB = DamageActorValue(this, avCode, float delta, source).
//delta is negative for damage, avCode 16 = Health. each class overrides the slot with
//a distinct function, so we keep a per-vtable original.
constexpr UInt32 kVtbl_Character       = 0x1086A6C;
constexpr UInt32 kVtbl_Creature        = 0x10870AC;
constexpr UInt32 kVtbl_PlayerCharacter = 0x108AA3C;
constexpr UInt32 kSlot_DamageActorValue = 0xEB * 4; //0x3AC
constexpr UInt32 kAV_Health = 16;

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

//HitMe ends in retn 8, so the two args are callee-cleaned, eax is dead but forwarded anyway
typedef UInt32 (__thiscall* HitMe_t)(Actor*, ActorHitData*, char);
typedef void (__thiscall* DamageActorValue_t)(void*, UInt32, float, void*);

static Detours::CallDetour s_hitCalls[kHitCallCount];

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

static void DispatchHitEvent(Actor* target, ActorHitData* hitData)
{
	if (!s_probeHit.hasListeners)
		return;
	if (GetCurrentThreadId() != g_mainThreadId) {
		if (!s_offMainLoggedHit) {
			Log("OnPreHitDamage: Actor::HitMe off main thread, waiting for main-thread replay");
			s_offMainLoggedHit = true;
		}
		return;
	}
	if (!target || !target->baseProcess || !hitData)
		return;
	if (target == (Actor*)*g_thePlayerPtr && hitData->hitLocation == 14)
		return;
	float damage = hitData->healthDmg;
	//location 14 moves health damage into limb damage for weapon-condition hits
	if (damage <= 0.0f)
		damage = hitData->limbDmg;
	if (damage <= 0.0f)
		return;

	float product = 1.0f;
	g_eventManagerInterface->DispatchEventAlt(kEventHit, MultiplierResultCb, &product,
		reinterpret_cast<TESObjectREFR*>(target),
		reinterpret_cast<TESForm*>(target),
		reinterpret_cast<TESForm*>(hitData->source),
		reinterpret_cast<TESForm*>(hitData->weapon),
		PackEventFloatArg(damage), hitData->hitLocation, &product);

	if (product != 1.0f) {
		if (product < 0.0f) product = 0.0f;
		hitData->healthDmg *= product;
		hitData->limbDmg *= product;
		hitData->fatigueDmg *= product;
	}
}

//each site chains its own recorded original, so a plugin owning one call site alone stays intact
template <UInt32 N>
static UInt32 __fastcall Hook_HitMe(Actor* target, void*, ActorHitData* hitData, char attackClass)
{
	DispatchHitEvent(target, hitData);
	return ((HitMe_t)s_hitCalls[N].GetOverwrittenAddr())(target, hitData, attackClass);
}

static bool InstallHitCalls()
{
	typedef UInt32 (__fastcall* Thunk_t)(Actor*, void*, ActorHitData*, char);
	static const Thunk_t thunks[kHitCallCount] = {
		Hook_HitMe<0>, Hook_HitMe<1>, Hook_HitMe<2>, Hook_HitMe<3>, Hook_HitMe<4>, Hook_HitMe<5>
	};

	for (UInt32 i = 0; i < kHitCallCount; i++) {
		if (s_hitCalls[i].WriteRelCall(kHitCallSites[i], thunks[i]) && s_hitCalls[i].GetOverwrittenAddr())
			continue;
		Log("OnPreHitDamage: call site %08X unusable, backing out all HitMe detours", kHitCallSites[i]);
		for (UInt32 j = 0; j <= i; j++)
			s_hitCalls[j].Remove();
		return false;
	}

	for (UInt32 i = 0; i < kHitCallCount; i++)
		Log("OnPreHitDamage: %08X hooked, original=%08X", kHitCallSites[i], s_hitCalls[i].GetOverwrittenAddr());
	return true;
}

static DamageActorValue_t PickDamageOrig(void* actor)
{
	UInt32 vtbl = *(UInt32*)actor;
	if (vtbl == kVtbl_Character) return s_origDamageChar;
	if (vtbl == kVtbl_Creature) return s_origDamageCreature;
	if (vtbl == kVtbl_PlayerCharacter || actor == (void*)*g_thePlayerPtr) return s_origDamagePlayer;
	static bool s_unknownVtblLogged = false;
	if (!s_unknownVtblLogged) {
		s_unknownVtblLogged = true;
		Log("OnPreHealthDamage: unknown vtable %08X, using Character original", vtbl);
	}
	return s_origDamageChar;
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

	bool hitInstalled = InstallHitCalls();

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

	return hitInstalled || t2;
}

}
