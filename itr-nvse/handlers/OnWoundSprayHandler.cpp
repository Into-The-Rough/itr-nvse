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

constexpr UInt32 kCreateBloodCallSites[] = {0x87C105, 0x890CD8, 0x89CE24}; //Actor::CreateBlood 0x88E8D0
constexpr UInt32 kCreateBloodCallCount = sizeof(kCreateBloodCallSites) / sizeof(kCreateBloodCallSites[0]);
constexpr UInt32 kAddr_GetIsParallaxCall = 0x88F838; //BGSImpactData::GetIsParallax 0x4A4100
constexpr UInt32 kAddr_AddDecalCall = 0x88F9C5; //TESObjectCELL::AddDecal 0x4A3FE0

struct WoundCtx {
	void* pActor;
	ActorHitDataView* pHit;
	void* pImpactData;
};

using Actor_CreateBlood_t = void(__thiscall*)(void*, float, ActorHitDataView*);
using BGSImpactData_GetIsParallax_t = bool(__thiscall*)(void*);
using TESObjectCELL_AddDecal_t = void(__thiscall*)(void*, void*, int, UInt8);

static WoundCtx g_currentCtx;
static UInt32 g_dispatchDepth = 0;

static Detours::CallDetour s_createBloodCalls[kCreateBloodCallCount];
static Detours::CallDetour s_getIsParallaxDetour;
static Detours::CallDetour s_addDecalDetour;

template<UInt32 N>
static void __fastcall Hook_CreateBlood(void* this_, void* edx, float a2, ActorHitDataView* hit)
{
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
static bool __fastcall Hook_CaptureWoundIPCT(void* this_, void* edx)
{
	g_currentCtx.pImpactData = this_;
	return ((BGSImpactData_GetIsParallax_t)s_getIsParallaxDetour.GetOverwrittenAddr())(this_);
}

static void __fastcall Hook_WoundAddDecal(void* cell, void* edx, void* decalData, int type, UInt8 forceAdd)
{
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
		(int)g_currentCtx.pHit->hitLocation,
		(TESForm*)g_currentCtx.pHit->source,
		(TESForm*)g_currentCtx.pHit->weapon);
	g_dispatchDepth--;
}

namespace OnWoundSprayHandler {
	bool Init(void* nvseInterface)
	{
		NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
		if (nvse->isEditor) return false;

		using CreateBloodHook_t = void(__fastcall*)(void*, void*, float, ActorHitDataView*);
		static const CreateBloodHook_t hooks[kCreateBloodCallCount] = {
			Hook_CreateBlood<0>, Hook_CreateBlood<1>, Hook_CreateBlood<2>};

		for (UInt32 i = 0; i < kCreateBloodCallCount; i++) {
			if (!s_createBloodCalls[i].WriteRelCall(kCreateBloodCallSites[i], hooks[i]))
				return false;
		}

		return s_getIsParallaxDetour.WriteRelCall(kAddr_GetIsParallaxCall, Hook_CaptureWoundIPCT)
			&& s_addDecalDetour.WriteRelCall(kAddr_AddDecalCall, Hook_WoundAddDecal);
	}
}
