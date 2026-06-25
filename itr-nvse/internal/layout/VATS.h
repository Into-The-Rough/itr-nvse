//VATS layout - camera mode/kills, target list, body-part visibility
#pragma once

#include <cstddef>

#include "nvse/GameObjects.h"
#include "internal/DialogueLayout.h" //BSSimpleListNodeView (shared list node)

struct VATSCameraDataView {
	UInt8 pad00[0x08];
	UInt32 mode;
	UInt8 pad0C[0x3C - 0x0C];
	UInt32 numKills;
};

enum VATSTargetType : UInt32 {
	kVATSTargetType_Projectile = 2,
};

struct VATSBodyPartView {
	float screenPosX;
	float screenPosY;
	float relativePosX;
	float relativePosY;
	float relativePosZ;
	float posX;
	float posY;
	float posZ;
	UInt32 bodyPartID;
	float percentVisible;
	float hitChance;
	bool isOnScreen;
	bool chanceCalculated;
	bool firstTimeShown;
	bool needsRecalc;
	void* bodyPartPercent;
	float unk34;
	UInt8 unk38;
	UInt8 pad39[3];
};

struct VATSTargetView {
	TESObjectREFR* targetRef;
	UInt32 type;
	BSSimpleListNodeView<VATSBodyPartView*> bodyParts;
	UInt8 unk10;
	UInt8 pad11[3];
	UInt32 unk14[3];
	UInt8 unk20;
	UInt8 pad21[3];
};

static_assert(offsetof(VATSCameraDataView, mode) == 0x08);
static_assert(offsetof(VATSCameraDataView, numKills) == 0x3C);
static_assert(offsetof(VATSTargetView, targetRef) == 0x00);
static_assert(offsetof(VATSTargetView, type) == 0x04);
static_assert(offsetof(VATSTargetView, bodyParts) == 0x08);
static_assert(sizeof(VATSBodyPartView) == 0x3C);
static_assert(offsetof(VATSBodyPartView, percentVisible) == 0x24);
static_assert(offsetof(VATSBodyPartView, hitChance) == 0x28);
static_assert(offsetof(VATSBodyPartView, isOnScreen) == 0x2C);
static_assert(offsetof(VATSBodyPartView, chanceCalculated) == 0x2D);
static_assert(sizeof(VATSTargetView) == 0x24);

inline UInt32 VATSCameraDataGetMode(void* vatsCameraData)
{
	return vatsCameraData ? reinterpret_cast<VATSCameraDataView*>(vatsCameraData)->mode : 0;
}

inline UInt32 VATSCameraDataGetNumKills(void* vatsCameraData)
{
	return vatsCameraData ? reinterpret_cast<VATSCameraDataView*>(vatsCameraData)->numKills : 0;
}

inline BSSimpleListNodeView<VATSTargetView*>* VATSTargetListGetHead(void* targetList)
{
	return reinterpret_cast<BSSimpleListNodeView<VATSTargetView*>*>(targetList);
}

inline bool VATSTargetNodeIsEmpty(BSSimpleListNodeView<VATSTargetView*>* node)
{
	return !node || !node->item;
}

inline bool VATSBodyPartNodeIsEmpty(BSSimpleListNodeView<VATSBodyPartView*>* node)
{
	return !node || !node->item;
}

inline bool VATSTargetIsProjectile(VATSTargetView* target)
{
	return target && target->type == kVATSTargetType_Projectile;
}

inline void VATSBodyPartForceVisible(VATSBodyPartView* part)
{
	if (!part) return;
	part->percentVisible = 1.0f;
	part->chanceCalculated = true;
}
