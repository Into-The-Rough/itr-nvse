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

//material-aware deflection reads these, all verified against the IDA Projectile struct
//impactDataList 0x88 tList head, hasImpacted 0x90, transform 0x94 NiTransform (rotate[9] first)
//power 0xCC speedMult 0xD0 lifeTime 0xD8 hitDamage 0xDC, vector104 0x104 move dir, distTravelled 0x110, range 0x14C
struct ProjectileImpactView {
	UInt8 pad00[0x90];
	UInt8 hasImpacted;
	UInt8 pad91[3];
	float transformRotate[9]; //0x94, row-major NiMatrix3, columns are basis axes, local +Y is travel direction
	UInt8 pad0B8[0xCC - 0xB8];
	float power;              //0xCC
	float speedMult;          //0xD0
	UInt8 padD4[0xD8 - 0xD4];
	float lifeTime;           //0xD8
	float hitDamage;          //0xDC
	UInt8 padE0[0x104 - 0xE0];
	float vector104[3];       //0x104
	float distTravelled;      //0x110
	UInt8 pad114[0x14C - 0x114];
	float range;              //0x14C
};

static_assert(offsetof(ProjectileImpactView, hasImpacted) == 0x90);
static_assert(offsetof(ProjectileImpactView, transformRotate) == 0x94);
static_assert(offsetof(ProjectileImpactView, power) == 0xCC);
static_assert(offsetof(ProjectileImpactView, speedMult) == 0xD0);
static_assert(offsetof(ProjectileImpactView, lifeTime) == 0xD8);
static_assert(offsetof(ProjectileImpactView, hitDamage) == 0xDC);
static_assert(offsetof(ProjectileImpactView, vector104) == 0x104);
static_assert(offsetof(ProjectileImpactView, distTravelled) == 0x110);
static_assert(offsetof(ProjectileImpactView, range) == 0x14C);

//tList<ImpactData> head at proj+0x88, node[0] = ImpactData*, node[1] = next
//ImpactData fields refr +0x00, pos +0x04, normal +0x10, hkpRigidBody* +0x1C, materialType +0x20
inline constexpr UInt32 kProjImpact_ListHead = 0x88;

//BGSProjectile flags u16 at base+0x60, bit 1 = hitscan (BGSProjectile::IsHitScan 0x9A7F80
//tests bit 1 of the field at this+0x60 via 0x4FD420/0x4EA950)
inline constexpr UInt32 kBGSProjFlags_Offset = 0x60;
inline constexpr UInt16 kBGSProjFlag_HitScan = 0x1;

//refr baseForm at +0x20 is the BGSProjectile, BGSProjectile::explosion at +0x84
//a non-null explosion marks missile/grenade/rocket rounds, ballistic bullets have none
inline bool ProjectileBaseHasExplosion(void* projectile)
{
	if (!projectile) return false;
	void* base = *(void**)((UInt8*)projectile + 0x20);
	if (!base) return false;
	return *(void**)((UInt8*)base + 0x84) != nullptr;
}
