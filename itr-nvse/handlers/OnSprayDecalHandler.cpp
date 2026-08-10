//blood spray decals (limb sever/explode). detour the call to BGSDecalEmitter::Update to
//capture the active emitter, then TESObjectCELL::AddDecal in its raycast loop to fire one
//event per decal. main-loop only, so the static emitter ptr is safe.

#include "OnSprayDecalHandler.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/Detours.h"
#include "internal/EventDispatch.h"
#include "internal/GameLayout.h"
#include "internal/globals.h"

//sole call site of BGSDecalEmitter::Update 0x4A2D50, ecx holds the emitter for each list entry
constexpr UInt32 kAddr_EmitterUpdateCallSite = 0x4A0430;
constexpr UInt32 kAddr_AddDecalCallSite = 0x4A36FD;

struct BGSDecalEmitter {
	UInt32 uiDecalsToEmit;   // +0x00
	UInt8  bFinished;        // +0x04
	UInt8  pad05[3];
	void*  pImpactData;      // +0x08
};

typedef void (__thiscall* BGSDecalEmitter_Update_t)(BGSDecalEmitter*);
typedef void (__thiscall* TESObjectCELL_AddDecal_t)(void*, void*, int, UInt8);

static BGSDecalEmitter* g_currentEmitter = nullptr;
static Detours::CallDetour s_emitterUpdateDetour;
static Detours::CallDetour s_addDecalDetour;

static void __fastcall HookEmitterUpdate(BGSDecalEmitter* this_, void* edx) {
	BGSDecalEmitter* saved = g_currentEmitter;
	g_currentEmitter = this_;
	((BGSDecalEmitter_Update_t)s_emitterUpdateDetour.GetOverwrittenAddr())(this_);
	g_currentEmitter = saved;
}

//replaces the call AddDecal at 0x4A36FD; same signature as TESObjectCELL::AddDecal
static void __fastcall HookSprayAddDecal(void* cell, void* edx, void* decalData, int type, UInt8 forceAdd) {
	((TESObjectCELL_AddDecal_t)s_addDecalDetour.GetOverwrittenAddr())(cell, decalData, type, forceAdd);

	if (!g_eventManagerInterface) return;
	if (!g_currentEmitter || !g_currentEmitter->pImpactData) return;
	if (!decalData) return;

	const float* origin = DecalCreationDataGetOrigin(decalData);
	const float* direction = DecalCreationDataGetDirection(decalData);

	//BGSDecalEmitter::Update is reached only from Main::Update (0x86E650), main thread
	g_eventManagerInterface->DispatchEvent(
		"ITR:OnSprayDecal", nullptr,
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

	if (!s_emitterUpdateDetour.WriteRelCall(kAddr_EmitterUpdateCallSite, HookEmitterUpdate)) {
		Log("OnSprayDecal: emitter update call site at 0x%X is not an E8 call, disabled", kAddr_EmitterUpdateCallSite);
		return false;
	}

	if (!s_addDecalDetour.WriteRelCall(kAddr_AddDecalCallSite, HookSprayAddDecal)) {
		Log("OnSprayDecal: AddDecal call site at 0x%X is not an E8 call, disabled", kAddr_AddDecalCallSite);
		s_emitterUpdateDetour.Remove();
		return false;
	}

	return true;
}
}
