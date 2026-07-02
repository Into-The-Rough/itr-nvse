//environmental blood splatter behind a wounded actor. hook Actor::CreateBlood to capture
//actor+hit, snapshot the body-part IPCT, then trampoline TESObjectCELL::AddDecal to fire
//one event per splatter.

#include "OnWoundSprayHandler.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/Detours.h"
#include "internal/EventDispatch.h"
#include "internal/GameLayout.h"

constexpr UInt32 kAddr_Actor_CreateBlood = 0x88E8D0;
constexpr UInt32 kAddr_BGSImpactData_GetIsParallax = 0x4A4120;
constexpr UInt32 kAddr_TESObjectCELL_AddDecal = 0x4A3FE0;

struct HitData {
	void*  pSource;                  //+0x00
	UInt8  pad04[0x10 - 0x04];
	SInt32 eDamageLimb;              //+0x10
	UInt8  pad14[0x30 - 0x14];
	void*  pWeapon;                  //+0x30
};

struct WoundCtx {
	void*    pActor;
	HitData* pHit;
	void*    pImpactData;
};

typedef void (__thiscall* Actor_CreateBlood_t)(void*, float, HitData*);
typedef bool (__thiscall* BGSImpactData_GetIsParallax_t)(void*);
typedef void (__thiscall* TESObjectCELL_AddDecal_t)(void*, void*, int, UInt8);

static WoundCtx g_currentCtx;

static Detours::JumpDetour s_createBloodDetour;
static Detours::CallDetour s_getIsParallaxDetour;
static Detours::CallDetour s_addDecalDetour;

static void __fastcall HookCreateBlood(void* this_, void* edx, float a2, HitData* hit) {
	WoundCtx saved = g_currentCtx;
	g_currentCtx.pActor = this_;
	g_currentCtx.pHit = hit;
	g_currentCtx.pImpactData = nullptr;
	s_createBloodDetour.GetTrampoline<Actor_CreateBlood_t>()(this_, a2, hit);
	g_currentCtx = saved;
}

//ECX holds the live body-part IPCT (v117 reassigned by GetNthImpactData a few
//lines above the wall-splatter dispatch).
static bool __fastcall HookCaptureWoundIPCT(void* this_, void* edx) {
	g_currentCtx.pImpactData = this_;
	return ((BGSImpactData_GetIsParallax_t)kAddr_BGSImpactData_GetIsParallax)(this_);
}

static void __fastcall HookWoundAddDecal(void* cell, void* edx, void* decalData, int type, UInt8 forceAdd) {
	((TESObjectCELL_AddDecal_t)kAddr_TESObjectCELL_AddDecal)(cell, decalData, type, forceAdd);

	if (!g_eventManagerInterface) return;
	if (!g_currentCtx.pActor || !g_currentCtx.pHit || !g_currentCtx.pImpactData) return;
	if (!decalData) return;

	const float* origin = DecalCreationDataGetOrigin(decalData);
	const float* direction = DecalCreationDataGetDirection(decalData);

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
}

namespace OnWoundSprayHandler {
bool Init(void* nvseInterface) {
	NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
	if (nvse->isEditor) return false;

	//prologue: push ebx; mov ebx,esp; sub esp,8; and esp,0xFFFFFFF0; add esp,4 = 1+2+3+3+3 = 12
	if (!s_createBloodDetour.WriteRelJump(kAddr_Actor_CreateBlood, HookCreateBlood, 12))
		return false;

	if (!s_getIsParallaxDetour.WriteRelCall(0x88F838, HookCaptureWoundIPCT))
		return false;

	if (!s_addDecalDetour.WriteRelCall(0x88F9C5, HookWoundAddDecal))
		return false;

	return true;
}
}
