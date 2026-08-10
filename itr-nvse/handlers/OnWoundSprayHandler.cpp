//environmental blood splatter behind a wounded actor. intercept the three create-blood callers
//to capture actor+hit, snapshot the body-part impact data, then intercept the cell decal call to fire
//one event per splatter.

#include <cstddef>

#include "OnWoundSprayHandler.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/Detours.h"
#include "internal/EventDispatch.h"
#include "internal/GameLayout.h"
#include "internal/globals.h"

constexpr UInt32 kAddr_Actor_CreateBlood = 0x88E8D0;
constexpr UInt32 kCreateBloodCallSites[] = { 0x87C105, 0x890CD8, 0x89CE24 };
constexpr UInt32 kCreateBloodCallCount = sizeof(kCreateBloodCallSites) / sizeof(kCreateBloodCallSites[0]);
constexpr UInt32 kAddr_GetIsParallaxCall = 0x88F838;
constexpr UInt32 kAddr_GetIsParallax = 0x4A4100;
constexpr UInt32 kAddr_AddDecalCall = 0x88F9C5;
constexpr UInt32 kAddr_AddDecal = 0x4A3FE0;

struct HitData {
	void*  pSource;                  //+0x00
	UInt8  pad04[0x10 - 0x04];
	SInt32 eDamageLimb;              //+0x10
	UInt8  pad14[0x30 - 0x14];
	void*  pWeapon;                  //+0x30
};

static_assert(offsetof(HitData, eDamageLimb) == 0x10, "HitData limb offset changed");
static_assert(offsetof(HitData, pWeapon) == 0x30, "HitData weapon offset changed");

struct WoundCtx {
	void*    pActor;
	HitData* pHit;
	void*    pImpactData;
};

typedef void (__thiscall* Actor_CreateBlood_t)(void*, float, HitData*);
typedef bool (__thiscall* BGSImpactData_GetIsParallax_t)(void*);
typedef void (__thiscall* TESObjectCELL_AddDecal_t)(void*, void*, int, UInt8);

static WoundCtx g_currentCtx;
static UInt32 g_dispatchDepth = 0;

static Detours::CallDetour s_createBloodCalls[kCreateBloodCallCount];
static Detours::CallDetour s_getIsParallaxDetour;
static Detours::CallDetour s_addDecalDetour;

template <UInt32 N>
static void __fastcall HookCreateBlood(void* this_, void*, float a2, HitData* hit) {
	WoundCtx saved = g_currentCtx;
	g_currentCtx.pActor = this_;
	g_currentCtx.pHit = hit;
	g_currentCtx.pImpactData = nullptr;
	auto original = reinterpret_cast<Actor_CreateBlood_t>(s_createBloodCalls[N].GetOverwrittenAddr());
	original(this_, a2, hit);
	g_currentCtx = saved;
}

//ecx holds the live body-part impact data (v117 reassigned a few
//lines above the wall-splatter dispatch).
static bool __fastcall HookCaptureWoundIPCT(void* this_, void* edx) {
	g_currentCtx.pImpactData = this_;
	return ((BGSImpactData_GetIsParallax_t)s_getIsParallaxDetour.GetOverwrittenAddr())(this_);
}

static void __fastcall HookWoundAddDecal(void* cell, void* edx, void* decalData, int type, UInt8 forceAdd) {
	((TESObjectCELL_AddDecal_t)s_addDecalDetour.GetOverwrittenAddr())(cell, decalData, type, forceAdd);

	if (!g_eventManagerInterface) return;
	if (!g_currentCtx.pActor || !g_currentCtx.pHit || !g_currentCtx.pImpactData) return;
	if (!decalData) return;
	if (g_dispatchDepth >= 16) return;

	const float* origin = DecalCreationDataGetOrigin(decalData);
	const float* direction = DecalCreationDataGetDirection(decalData);

	g_dispatchDepth++;
	g_eventManagerInterface->DispatchEvent(
		"ITR:OnWoundSpray", nullptr,
		(TESForm*)g_currentCtx.pActor,
		(TESForm*)g_currentCtx.pImpactData,
		PackEventFloatArg(origin[0]),
		PackEventFloatArg(origin[1]),
		PackEventFloatArg(origin[2]),
		PackEventFloatArg(direction[0]),
		PackEventFloatArg(direction[1]),
		PackEventFloatArg(direction[2]),
		(int)g_currentCtx.pHit->eDamageLimb,
		(TESForm*)g_currentCtx.pHit->pSource,
		(TESForm*)g_currentCtx.pHit->pWeapon
	);
	g_dispatchDepth--;
}

namespace OnWoundSprayHandler {
static void RemoveHooks() {
	s_addDecalDetour.Remove();
	s_getIsParallaxDetour.Remove();
	for (UInt32 i = kCreateBloodCallCount; i > 0; i--)
		s_createBloodCalls[i - 1].Remove();
}

bool Init(void* nvseInterface) {
	NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
	if (nvse->isEditor) return false;

	typedef void (__fastcall* CreateBloodHook_t)(void*, void*, float, HitData*);
	static const CreateBloodHook_t hooks[kCreateBloodCallCount] = {
		HookCreateBlood<0>, HookCreateBlood<1>, HookCreateBlood<2>
	};

	for (UInt32 i = 0; i < kCreateBloodCallCount; i++) {
		if (!s_createBloodCalls[i].WriteRelCall(kCreateBloodCallSites[i], hooks[i])) {
			Log("OnWoundSpray: CreateBlood call site %08X unusable, backing out", kCreateBloodCallSites[i]);
			RemoveHooks();
			return false;
		}
		Log("OnWoundSpray: %08X hooked, original=%08X vanilla=%08X", kCreateBloodCallSites[i],
			s_createBloodCalls[i].GetOverwrittenAddr(), kAddr_Actor_CreateBlood);
	}

	if (!s_getIsParallaxDetour.WriteRelCall(kAddr_GetIsParallaxCall, HookCaptureWoundIPCT)) {
		Log("OnWoundSpray: GetIsParallax call site %08X unusable, backing out", kAddr_GetIsParallaxCall);
		RemoveHooks();
		return false;
	}
	Log("OnWoundSpray: %08X hooked, original=%08X vanilla=%08X", kAddr_GetIsParallaxCall,
		s_getIsParallaxDetour.GetOverwrittenAddr(), kAddr_GetIsParallax);

	if (!s_addDecalDetour.WriteRelCall(kAddr_AddDecalCall, HookWoundAddDecal)) {
		Log("OnWoundSpray: AddDecal call site %08X unusable, backing out", kAddr_AddDecalCall);
		RemoveHooks();
		return false;
	}
	Log("OnWoundSpray: %08X hooked, original=%08X vanilla=%08X", kAddr_AddDecalCall,
		s_addDecalDetour.GetOverwrittenAddr(), kAddr_AddDecal);

	return true;
}
}
