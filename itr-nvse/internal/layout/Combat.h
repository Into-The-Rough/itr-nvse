#pragma once

#include <cstddef>

#include "nvse/GameObjects.h"

struct CombatControllerView {
	UInt8 pad00[0x80];
	void* combatGroup;
	void* attackProcedure; //set by 0x980110
	void* movementProcedure; //set by 0x9801B0, where the cover action parks CombatProcedureBeInCover
	UInt8 pad8C[0x9C - 0x8C]; //0x8C is the BSSimpleArray<CombatProcedure*> that 0x980400 walks
	void* combatState; //0x586150
	UInt8 padA0[0xBC - 0xA0];
	Actor* packageOwner;
};

struct CombatStateView {
	UInt8 pad00[0xE4];
	void* coverLocationA; //0x97B050, the slot the from-cover combat actions hand to CombatProcedureBeInCover
	void* coverLocationB; //0x97FB60, filled by the cover search in 0x99E060
	UInt8 padEC[0x1C4 - 0xEC];
	void* combatController;
};

struct CombatProcedureView {
	void* vtbl;
	void* combatController;
	UInt32 status;
};

//CombatProcedureBeInCover, 0x6C bytes from ctor 0x9D2440, type id 7 from vtbl 0x10910F4 slot 13
//state names come from CombatProcedureBeInCover::DisplayDebugText 0x9D3470
enum {
	kCoverState_Initializing = 0,
	kCoverState_WaitingBehindCover = 1,
	kCoverState_MovingOut = 2,
	kCoverState_WaitingOutOfCover = 3,
	kCoverState_FiringOutOfCover = 4,
	kCoverState_MovingIn = 5,
	kCoverState_MovingInAndRotate = 6,
	kCoverState_HoldingGround = 7,
	kCoverState_Count = 8,
};

struct CombatProcedureBeInCoverView {
	void* vtbl;
	void* combatController;
	UInt32 status;
	UInt32 unk0C;
	UInt32 coverState;
	void* coverLocation;
	float coverPos[3];
	float firePos[3];
	float fireEyePos[3]; //firePos raised by 0.75 * actor height in the procedure's INITIALIZING step
};

struct BGSWorldLocationView {
	float x;
	float y;
	float z;
	TESForm* form; //serialised as a refID via 0x865DF0
};

template<typename T>
struct BSSimpleArrayView {
	void* vtbl;
	T* data;
	UInt32 size;
	UInt32 capacity;
};

//0x98BFB0 writes 28-byte entries into CombatGroup+0x110, 0x98C0D0 purges entries with investigated != 0
struct CombatSearchLocationView {
	BGSWorldLocationView location;
	float timestamp;
	UInt32 investigated;
	float priority;
};

struct CombatGroupSearchView {
	UInt8 pad00[0xD8];
	float searchUpdateTimer[2]; //0x98ACF0 polls +0xD8 via 0x8D7F80
	float searchAreaTimer[2]; //0x98ACF0 expires +0xE0 via 0x97F220 and polls via 0x8D7F80
	UInt8 padE8[0x110 - 0xE8];
	BSSimpleArrayView<CombatSearchLocationView> searchLocations;
};

//verified: sub_9856E0 serialises six game-time floats at 0x4C..0x60 and the two byte counters at 0x64/0x65
struct CombatTargetView {
	Actor* target;
	SInt32 detectionLevel;
	BGSWorldLocationView lastSeenLocation;
	BGSWorldLocationView detectedLocation;
	BGSWorldLocationView lastFullyVisibleLocation;
	BGSWorldLocationView initialTargetLocation;
	UInt16 searchCount;
	UInt16 attackerCount;
	float timestamps[6];
	UInt8 inLOSCount;
	UInt8 inFullLOSCount;
	UInt8 pad66[2];
};

struct ActorHitDataView {
	void* source; //0x00 attacker
	void* target; //0x04 victim
	void* projectile; //0x08
	UInt32 weaponAV; //0x0C
	SInt32 hitLocation; //0x10
	float healthDmg; //0x14
	float wpnBaseDmg; //0x18
	float fatigueDmg; //0x1C
	float limbDmg; //0x20
	float blockDTMod; //0x24
	float armorDmg; //0x28
	float weaponDmg; //0x2C
	void* weapon; //0x30
};

static_assert(offsetof(CombatControllerView, combatGroup) == 0x80);
static_assert(offsetof(CombatControllerView, attackProcedure) == 0x84);
static_assert(offsetof(CombatControllerView, movementProcedure) == 0x88);
static_assert(offsetof(CombatControllerView, combatState) == 0x9C);
static_assert(offsetof(CombatControllerView, packageOwner) == 0xBC);
static_assert(offsetof(CombatStateView, coverLocationA) == 0xE4);
static_assert(offsetof(CombatStateView, coverLocationB) == 0xE8);
static_assert(offsetof(CombatStateView, combatController) == 0x1C4);
static_assert(offsetof(CombatProcedureView, combatController) == 0x04);
static_assert(offsetof(CombatProcedureView, status) == 0x08);
static_assert(offsetof(CombatProcedureBeInCoverView, coverState) == 0x10);
static_assert(offsetof(CombatProcedureBeInCoverView, coverLocation) == 0x14);
static_assert(offsetof(CombatProcedureBeInCoverView, coverPos) == 0x18);
static_assert(offsetof(CombatProcedureBeInCoverView, firePos) == 0x24);
static_assert(offsetof(CombatProcedureBeInCoverView, fireEyePos) == 0x30);
static_assert(sizeof(BGSWorldLocationView) == 0x10);
static_assert(sizeof(BSSimpleArrayView<void*>) == 0x10);
static_assert(offsetof(CombatSearchLocationView, timestamp) == 0x10);
static_assert(offsetof(CombatSearchLocationView, investigated) == 0x14);
static_assert(offsetof(CombatSearchLocationView, priority) == 0x18);
static_assert(sizeof(CombatSearchLocationView) == 0x1C);
static_assert(offsetof(CombatGroupSearchView, searchUpdateTimer) == 0xD8);
static_assert(offsetof(CombatGroupSearchView, searchAreaTimer) == 0xE0);
static_assert(offsetof(CombatGroupSearchView, searchLocations) == 0x110);
static_assert(offsetof(CombatTargetView, target) == 0x00);
static_assert(offsetof(CombatTargetView, detectionLevel) == 0x04);
static_assert(offsetof(CombatTargetView, lastSeenLocation) == 0x08);
static_assert(offsetof(CombatTargetView, detectedLocation) == 0x18);
static_assert(offsetof(CombatTargetView, lastFullyVisibleLocation) == 0x28);
static_assert(offsetof(CombatTargetView, initialTargetLocation) == 0x38);
static_assert(offsetof(CombatTargetView, searchCount) == 0x48);
static_assert(offsetof(CombatTargetView, attackerCount) == 0x4A);
static_assert(offsetof(CombatTargetView, timestamps) == 0x4C);
static_assert(offsetof(CombatTargetView, inLOSCount) == 0x64);
static_assert(offsetof(CombatTargetView, inFullLOSCount) == 0x65);
static_assert(sizeof(CombatTargetView) == 0x68);
static_assert(offsetof(ActorHitDataView, hitLocation) == 0x10);
static_assert(offsetof(ActorHitDataView, healthDmg) == 0x14);
static_assert(offsetof(ActorHitDataView, fatigueDmg) == 0x1C);
static_assert(offsetof(ActorHitDataView, limbDmg) == 0x20);
static_assert(offsetof(ActorHitDataView, weapon) == 0x30);

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

inline void CombatGroupMarkSearchLocationsInvestigated(void* combatGroup)
{
	if (!combatGroup)
		return;
	auto& locs = reinterpret_cast<CombatGroupSearchView*>(combatGroup)->searchLocations;
	if (!locs.data)
		return;
	for (UInt32 i = 0; i < locs.size; ++i)
		locs.data[i].investigated = 1;
}

inline float* CombatGroupGetSearchUpdateTimer(void* combatGroup)
{
	return combatGroup ? reinterpret_cast<CombatGroupSearchView*>(combatGroup)->searchUpdateTimer : nullptr;
}

inline float* CombatGroupGetSearchAreaTimer(void* combatGroup)
{
	return combatGroup ? reinterpret_cast<CombatGroupSearchView*>(combatGroup)->searchAreaTimer : nullptr;
}
