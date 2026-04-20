//detects blood spray decal placements (limb sever/explode).
//hooks BGSDecalEmitter::Update to capture the active emitter, then intercepts
//the AddDecal call inside its raycast loop to fire one event per landed decal.
//
//design: decal placement happens inside BGSDecalEmitter::Update's loop. Each
//successful raycast calls TESObjectCELL::AddDecal with a DECAL_CREATION_DATA
//whose kOrigin is the world hit point. Single-threaded (main loop), so a
//static "current emitter" pointer is safe across the entry/AddDecal hooks.

#include "OnSprayDecalHandler.h"
#include "internal/NVSEMinimal.h"
#include "internal/Detours.h"
#include "internal/EventDispatch.h"

constexpr UInt32 kAddr_BGSDecalEmitter_Update = 0x4A2D50;
constexpr UInt32 kAddr_AddDecalCallSite = 0x4A36FD;
constexpr UInt32 kAddr_TESObjectCELL_AddDecal = 0x4A3FE0;

struct BGSDecalEmitter {
	UInt32 uiDecalsToEmit;   // +0x00
	UInt8  bFinished;        // +0x04
	UInt8  pad05[3];
	void*  pImpactData;      // +0x08
};

typedef void (__thiscall* BGSDecalEmitter_Update_t)(BGSDecalEmitter*);
typedef void (__thiscall* TESObjectCELL_AddDecal_t)(void*, void*, int, UInt8);

static BGSDecalEmitter* g_currentEmitter = nullptr;
static Detours::JumpDetour s_emitterUpdateDetour;
static Detours::CallDetour s_addDecalDetour;

static void __fastcall HookEmitterUpdate(BGSDecalEmitter* this_, void* edx) {
	BGSDecalEmitter* saved = g_currentEmitter;
	g_currentEmitter = this_;
	s_emitterUpdateDetour.GetTrampoline<BGSDecalEmitter_Update_t>()(this_);
	g_currentEmitter = saved;
}

//replaces the call AddDecal at 0x4A36FD; same signature as TESObjectCELL::AddDecal
static void __fastcall HookSprayAddDecal(void* cell, void* edx, void* decalData, int type, UInt8 forceAdd) {
	((TESObjectCELL_AddDecal_t)kAddr_TESObjectCELL_AddDecal)(cell, decalData, type, forceAdd);

	if (!g_eventManagerInterface) return;
	if (!g_currentEmitter || !g_currentEmitter->pImpactData) return;
	if (!decalData) return;

	//DECAL_CREATION_DATA: kOrigin (NiPoint3) at +0x00, kDirection (NiPoint3) at +0x0C
	float* origin = (float*)decalData;
	float* direction = (float*)((UInt8*)decalData + 0x0C);

	g_eventManagerInterface->DispatchEventThreadSafe(
		"ITR:OnSprayDecal", nullptr, nullptr,
		(TESForm*)g_currentEmitter->pImpactData,
		PackEventFloatArg(origin[0]),
		PackEventFloatArg(origin[1]),
		PackEventFloatArg(origin[2]),
		PackEventFloatArg(direction[0]),
		PackEventFloatArg(direction[1]),
		PackEventFloatArg(direction[2])
	);
}

namespace OnSprayDecalHandler {
bool Init(void* nvseInterface) {
	NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
	if (nvse->isEditor) return false;

	//prologue: push ebx; mov ebx,esp; sub esp,8; and esp,0xFFFFFFF0; add esp,4 = 1+2+3+3+3 = 12
	if (!s_emitterUpdateDetour.WriteRelJump(kAddr_BGSDecalEmitter_Update, HookEmitterUpdate, 12))
		return false;

	if (!s_addDecalDetour.WriteRelCall(kAddr_AddDecalCallSite, HookSprayAddDecal))
		return false;

	return true;
}
}
