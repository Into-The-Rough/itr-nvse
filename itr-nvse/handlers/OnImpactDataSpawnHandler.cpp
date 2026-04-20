//hooks Projectile::SpawnCollisionEffects to detect when an ImpactData spawns visuals.
//fires once per non-actor projectile impact, after the engine resolves the per-material
//ImpactData via the weapon's ImpactDataSet.

#include "OnImpactDataSpawnHandler.h"
#include "internal/NVSEMinimal.h"
#include "internal/Detours.h"
#include "internal/EventDispatch.h"

constexpr UInt32 kAddr_SpawnCollisionEffects = 0x9C20E0;
constexpr UInt32 kAddr_GetImpactDataForMaterial = 0x522BA0;

struct NiPoint3 { float x, y, z; };

typedef void (__thiscall* SpawnCollisionEffects_t)(void*, TESObjectREFR*, NiPoint3*, NiPoint3*, int, UInt32);
typedef void* (__thiscall* GetImpactDataForMaterial_t)(void*, UInt32);

static Detours::JumpDetour s_detour;

static bool IsActorTypeID(UInt8 typeID) {
	return typeID == 0x3B || typeID == 0x3C;
}

static void __fastcall HookSpawnCollisionEffects(
	void* this_, void* edx,
	TESObjectREFR* a2, NiPoint3* aCoord, NiPoint3* a4,
	int a5, UInt32 material)
{
	NiPoint3 capturedPos = aCoord ? *aCoord : NiPoint3{0,0,0};
	NiPoint3 capturedNormal = a4 ? *a4 : NiPoint3{0,0,0};

	s_detour.GetTrampoline<SpawnCollisionEffects_t>()(this_, a2, aCoord, a4, a5, material);

	if (!g_eventManagerInterface) return;
	if (!this_) return;

	//actor targets skip the impact-effects path entirely
	if (a2) {
		UInt8 typeID = *((UInt8*)a2 + 0x04);
		if (IsActorTypeID(typeID)) return;
	}

	//Projectile + 0xF8 = pSourceWeapon
	void* weapon = *(void**)((UInt8*)this_ + 0xF8);
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

	//prologue: push ebp; mov ebp,esp; push -1; push offset = 1+2+2+5 = 10 bytes
	if (!s_detour.WriteRelJump(kAddr_SpawnCollisionEffects, HookSpawnCollisionEffects, 10))
		return false;

	return true;
}
}
