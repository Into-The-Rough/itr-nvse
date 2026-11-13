#pragma once

#include <cstddef>

#include "nvse/GameForms.h"
#include "nvse/GameObjects.h"

struct ProjectileImpactView {
	TESObjectREFR* ref;
	NiVector3 pos;
};

struct ProjectileView {
	UInt8 pad00[0x88];
	tList<ProjectileImpactView> impacts;
	UInt8 pad90[0x38];
	UInt32 flags;
	float power;
	float speedMult;
	float range;
	float lifeTime;
	UInt8 padDC[0x1C];
	TESObjectWEAP* sourceWeap;
	TESObjectREFR* sourceRef;
	TESObjectREFR* liveGrenadeTarget;
	NiVector3 vector;
};

inline constexpr UInt32 kProjectileRefFlag_AltTrigger = 0x400;
inline constexpr UInt32 kProjectileRefFlag_Hitscan = 0x1;

static_assert(offsetof(ProjectileImpactView, ref) == 0x00);
static_assert(offsetof(ProjectileImpactView, pos) == 0x04);
static_assert(offsetof(ProjectileView, impacts) == 0x88);
static_assert(offsetof(ProjectileView, flags) == 0xC8);
static_assert(offsetof(ProjectileView, range) == 0xD4);
static_assert(offsetof(ProjectileView, lifeTime) == 0xD8);
static_assert(offsetof(ProjectileView, sourceWeap) == 0xF8);
static_assert(offsetof(ProjectileView, sourceRef) == 0xFC);
static_assert(offsetof(ProjectileView, vector) == 0x104);

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
	return projectileRef && (reinterpret_cast<ProjectileView*>(projectileRef)->flags & flag) != 0;
}
