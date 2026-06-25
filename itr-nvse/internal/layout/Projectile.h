//Projectile layout - source ref/weapon and ref-flags read by near-miss / alt-trigger
#pragma once

#include <cstddef>

#include "nvse/GameForms.h"
#include "nvse/GameObjects.h"

struct ProjectileView {
	UInt8 pad00[0xF8];
	TESObjectWEAP* sourceWeap;
	TESObjectREFR* sourceRef;
};

struct ProjectileRefFlagsView {
	UInt8 pad00[0xC8];
	UInt32 flags;
};

inline constexpr UInt32 kProjectileRefFlag_AltTrigger = 0x400;

static_assert(offsetof(ProjectileView, sourceWeap) == 0xF8);
static_assert(offsetof(ProjectileView, sourceRef) == 0xFC);
static_assert(offsetof(ProjectileRefFlagsView, flags) == 0xC8);

inline TESObjectWEAP* ProjectileGetSourceWeapon(void* projectile)
{
	return projectile ? reinterpret_cast<ProjectileView*>(projectile)->sourceWeap : nullptr;
}

inline TESObjectREFR* ProjectileGetSourceRef(void* projectile)
{
	return projectile ? reinterpret_cast<ProjectileView*>(projectile)->sourceRef : nullptr;
}

inline bool ProjectileRefHasFlag(void* projectileRef, UInt32 flag)
{
	return projectileRef && (reinterpret_cast<ProjectileRefFlagsView*>(projectileRef)->flags & flag) != 0;
}
