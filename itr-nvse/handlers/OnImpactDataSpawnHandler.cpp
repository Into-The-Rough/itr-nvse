//detours the sole call to Projectile::SpawnCollisionEffects to detect when an ImpactData
//spawns visuals. fires once per non-actor projectile impact, after the engine resolves the
//per-material ImpactData via the weapon's ImpactDataSet.

#include "OnImpactDataSpawnHandler.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/Detours.h"
#include "internal/EventDispatch.h"
#include "internal/GameSDK.h"
#include "internal/globals.h"
#include "internal/layout/Projectile.h"

//sole call site of Projectile::SpawnCollisionEffects 0x9C20E0, in Projectile::ProcessImpacts
constexpr UInt32 kAddr_SpawnCollisionEffectsCall = 0x9C2058;
constexpr UInt32 kAddr_SpawnCollisionEffects = 0x9C20E0;
constexpr UInt32 kAddr_GetImpactDataForMaterial = 0x522BA0;

struct NiPoint3 { float x, y, z; };

typedef void (__thiscall* SpawnCollisionEffects_t)(void*, TESObjectREFR*, NiPoint3*, NiPoint3*, int, UInt32);
typedef void* (__thiscall* GetImpactDataForMaterial_t)(void*, UInt32);

static Detours::CallDetour s_spawnCall;

static bool IsActorTypeID(UInt8 typeID) {
	return typeID == kFormType_ACHR || typeID == kFormType_ACRE;
}

static void __fastcall HookSpawnCollisionEffects(
	void* this_, void* edx,
	TESObjectREFR* a2, NiPoint3* aCoord, NiPoint3* a4,
	int a5, UInt32 material)
{
	NiPoint3 capturedPos = aCoord ? *aCoord : NiPoint3{0,0,0};
	NiPoint3 capturedNormal = a4 ? *a4 : NiPoint3{0,0,0};

	((SpawnCollisionEffects_t)s_spawnCall.GetOverwrittenAddr())(this_, a2, aCoord, a4, a5, material);

	if (!g_eventManagerInterface) return;
	if (!this_) return;

	//actor targets skip the impact-effects path entirely
	if (a2) {
		UInt8 typeID = a2->typeID;
		if (IsActorTypeID(typeID)) return;
	}

	auto* weapon = ProjectileGetSourceWeapon(this_);
	if (!weapon) return;

	void* impactData = ((GetImpactDataForMaterial_t)kAddr_GetImpactDataForMaterial)(weapon, material);
	if (!impactData) return;

	g_eventManagerInterface->DispatchEvent(
		"ITR:OnImpactDataSpawn", nullptr,
		(TESForm*)impactData,
		PackEventFloatArg(capturedPos.x),
		PackEventFloatArg(capturedPos.y),
		PackEventFloatArg(capturedPos.z),
		PackEventFloatArg(capturedNormal.x),
		PackEventFloatArg(capturedNormal.y),
		PackEventFloatArg(capturedNormal.z),
		(TESForm*)this_,
		(TESForm*)a2,
		(TESForm*)weapon,
		(int)material
	);
}

namespace OnImpactDataSpawnHandler {
bool Init(void* nvseInterface) {
	NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
	if (nvse->isEditor) return false;

	if (!s_spawnCall.WriteRelCall(kAddr_SpawnCollisionEffectsCall, HookSpawnCollisionEffects)) {
		Log("OnImpactDataSpawn: call site at 0x%X is not an E8 call, disabled", kAddr_SpawnCollisionEffectsCall);
		return false;
	}
	UInt32 original = s_spawnCall.GetOverwrittenAddr();
	Log("OnImpactDataSpawn: %08X hooked, original=%08X vanilla=%08X", kAddr_SpawnCollisionEffectsCall,
		original, kAddr_SpawnCollisionEffects);

	return true;
}
}
