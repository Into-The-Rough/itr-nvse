//ExtraData views - weapon mod flags, ash pile ref, dismembered limbs
#pragma once

#include <cstddef>

#include "nvse/GameForms.h"
#include "nvse/GameObjects.h"
#include "nvse/GameExtraData.h"

struct ExtraWeaponModFlagsView {
	void* vtbl;
	UInt8 type;
	UInt8 pad05[3];
	BSExtraData* next;
	UInt8 flags;
	UInt8 pad0D[3];
};

struct ExtraAshPileRefView {
	void* vtbl;
	UInt8 type;
	UInt8 pad05[3];
	BSExtraData* next;
	TESObjectREFR* sourceRef;
};

struct ExtraDismemberedLimbsView : BSExtraData {
	UInt16 dismemberedMask;
	UInt8 pad0E[2];
	SInt32 unk10;
	TESObjectWEAP* weapon;
	SInt32 unk18;
	bool wasEaten;
	UInt8 pad1D[3];
};

static_assert(sizeof(ExtraDismemberedLimbsView) == 0x20);
static_assert(offsetof(ExtraDismemberedLimbsView, dismemberedMask) == 0x0C);
static_assert(offsetof(ExtraDismemberedLimbsView, weapon) == 0x14);
static_assert(offsetof(ExtraDismemberedLimbsView, wasEaten) == 0x1C);
static_assert(sizeof(ExtraWeaponModFlagsView) == 0x10, "ExtraWeaponModFlags layout changed");
static_assert(offsetof(ExtraWeaponModFlagsView, flags) == 0x0C, "ExtraWeaponModFlags flags offset changed");
static_assert(sizeof(ExtraAshPileRefView) == 0x10, "ExtraAshPileRef layout changed");
static_assert(offsetof(ExtraAshPileRefView, sourceRef) == 0x0C, "ExtraAshPileRef sourceRef offset changed");

inline TESObjectREFR* ExtraAshPileRefGetSourceRef(BSExtraData* extraData)
{
	return extraData ? reinterpret_cast<ExtraAshPileRefView*>(extraData)->sourceRef : nullptr;
}

inline UInt16 ExtraDismemberedLimbsGetMask(BSExtraData* extraData)
{
	return extraData ? reinterpret_cast<ExtraDismemberedLimbsView*>(extraData)->dismemberedMask : 0;
}
