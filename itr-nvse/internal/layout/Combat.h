//Combat layout - combat controller/state/procedure/target + world location
#pragma once

#include <cstddef>

#include "nvse/GameObjects.h"

struct CombatControllerView {
	UInt8 pad00[0x80];
	void* combatGroup;
	UInt8 pad84[0xBC - 0x84];
	Actor* packageOwner;
};

struct CombatStateView {
	UInt8 pad00[0x1C4];
	void* combatController;
};

struct CombatProcedureView {
	void* vtbl;
	void* combatController;
	UInt32 status;
};

struct BGSWorldLocationView {
	float x;
	float y;
	float z;
	UInt32 unk0C;
};

struct CombatTargetView {
	Actor* target;
	SInt32 detectionLevel;
	BGSWorldLocationView lastSeenLocation;
	BGSWorldLocationView detectedLocation;
	BGSWorldLocationView lastFullyVisibleLocation;
	BGSWorldLocationView initialTargetLocation;
	UInt16 searchCount;
	UInt16 attackerCount;
	UInt8 inLOSCount;
	UInt8 inFullLOSCount;
	UInt8 pad4E[2];
	float timestamps[6];
};

static_assert(offsetof(CombatControllerView, combatGroup) == 0x80);
static_assert(offsetof(CombatControllerView, packageOwner) == 0xBC);
static_assert(offsetof(CombatStateView, combatController) == 0x1C4);
static_assert(offsetof(CombatProcedureView, combatController) == 0x04);
static_assert(offsetof(CombatProcedureView, status) == 0x08);
static_assert(sizeof(BGSWorldLocationView) == 0x10);
static_assert(offsetof(CombatTargetView, target) == 0x00);
static_assert(offsetof(CombatTargetView, detectionLevel) == 0x04);
static_assert(offsetof(CombatTargetView, lastSeenLocation) == 0x08);
static_assert(offsetof(CombatTargetView, detectedLocation) == 0x18);
static_assert(offsetof(CombatTargetView, lastFullyVisibleLocation) == 0x28);
static_assert(offsetof(CombatTargetView, initialTargetLocation) == 0x38);
static_assert(offsetof(CombatTargetView, searchCount) == 0x48);
static_assert(offsetof(CombatTargetView, attackerCount) == 0x4A);
static_assert(offsetof(CombatTargetView, inLOSCount) == 0x4C);
static_assert(offsetof(CombatTargetView, inFullLOSCount) == 0x4D);
static_assert(offsetof(CombatTargetView, timestamps) == 0x50);
static_assert(sizeof(CombatTargetView) == 0x68);

inline void* CombatControllerGetCombatGroup(void* combatController)
{
	return combatController ? reinterpret_cast<CombatControllerView*>(combatController)->combatGroup : nullptr;
}

inline Actor* CombatControllerGetPackageOwner(void* combatController)
{
	return combatController ? reinterpret_cast<CombatControllerView*>(combatController)->packageOwner : nullptr;
}

inline void* CombatStateGetCombatController(void* combatState)
{
	return combatState ? reinterpret_cast<CombatStateView*>(combatState)->combatController : nullptr;
}

inline void* CombatProcedureGetCombatController(void* procedure)
{
	return procedure ? reinterpret_cast<CombatProcedureView*>(procedure)->combatController : nullptr;
}

inline void CombatProcedureSetStatus(void* procedure, UInt32 status)
{
	if (procedure) reinterpret_cast<CombatProcedureView*>(procedure)->status = status;
}

inline CombatTargetView* CombatTargetAsView(void* combatTarget)
{
	return reinterpret_cast<CombatTargetView*>(combatTarget);
}

inline const float* BGSWorldLocationGetPosition(const BGSWorldLocationView& location)
{
	return &location.x;
}

inline const BGSWorldLocationView* CombatTargetGetLastSeenLocation(void* combatTarget)
{
	return combatTarget ? &CombatTargetAsView(combatTarget)->lastSeenLocation : nullptr;
}

inline const BGSWorldLocationView* CombatTargetGetDetectedLocation(void* combatTarget)
{
	return combatTarget ? &CombatTargetAsView(combatTarget)->detectedLocation : nullptr;
}

inline const BGSWorldLocationView* CombatTargetGetLastFullyVisibleLocation(void* combatTarget)
{
	return combatTarget ? &CombatTargetAsView(combatTarget)->lastFullyVisibleLocation : nullptr;
}

inline const BGSWorldLocationView* CombatTargetGetInitialLocation(void* combatTarget)
{
	return combatTarget ? &CombatTargetAsView(combatTarget)->initialTargetLocation : nullptr;
}
