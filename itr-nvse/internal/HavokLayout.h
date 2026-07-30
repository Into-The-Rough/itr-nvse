#pragma once

#include <cstddef>

#include "common/ITypes.h"
#include "CallTemplates.h"

constexpr UInt32 kHkpWorldObject_Collidable = 0x10;
constexpr UInt8 kHkpWorldObject_CollisionTypeRigidBody = 1;
constexpr UInt8 kHkpFilterFlag_NoCollision = 0x40; //collisionFilterInfo byte at hkObject+0x2D

//havok layouts missing from the sdk, checked against falloutnv.exe

template <typename T>
struct HkArrayView {
	T* data;
	SInt32 size;
	UInt32 capacityAndFlags;
};

struct BhkCollisionObjectView {
	UInt8 pad00[0x08];
	void* niNode;
	UInt8 pad0C[0x10 - 0x0C];
	void* worldObject;
};

struct HkpWorldObjectView {
	UInt8 pad00[0x08];
	void* object;
	UInt8 pad0C[0x28 - 0x0C];
	UInt8 collisionType;
	UInt8 pad29[3];
	UInt32 collisionFilterInfo;
};

struct HkpCollidableView {
	UInt8 pad00[0x1C];
	UInt32 collisionFilterInfo;
};

struct HkpSimulationIslandView {
	UInt8 pad00[0x26];
	UInt8 stateFlags;
};

struct HkpRigidBodyView {
	UInt8 pad000[0xCC];
	HkpSimulationIslandView* simulationIsland;
	UInt8 pad0D0[0xE8 - 0xD0];
	UInt8 motionType;
};

struct ProcessControllerView {
	UInt8 pad000[0x138];
	void* characterController;
};

struct BhkCharacterPhantomView {
	UInt8 pad00[0x08];
	void* havokPhantom;
};

struct HkpRootCdPointView {
	UInt8 pad00[0x48];
	void* rootCollidableB;
	UInt8 pad4C[0x70 - 0x4C];
};

struct HkpAllCdPointCollectorView {
	UInt8 pad000[0x10];
	HkArrayView<HkpRootCdPointView> hits;
	UInt8 pad01C[0x3A0 - 0x1C];
};

struct BhkCharacterPointCollectorView {
	HkpAllCdPointCollectorView cdPointCollector;
	UInt32 unk3A0;
	HkArrayView<void*> contactBodies;
	UInt8 pad3B0[0x3C8 - 0x3B0];
};

struct BhkSerializableView {
	UInt8 pad00[0x08];
	void* hkObject;
	UInt32 unk0C;
};

struct BhkCharacterProxyView {
	BhkSerializableView serializable;
	BhkCharacterPointCollectorView pointCollector;
	UInt32 unk3D8[2];
};

struct HkpCharacterContextView {
	UInt8 pad00[0x10];
	UInt32 hkState;
	UInt8 pad14[0x30 - 0x14];
};

struct BhkCharacterControllerView {
	BhkCharacterProxyView proxy;
	HkpCharacterContextView chrContext;
	UInt8 pad410[0x548 - 0x410];
	float fallTime;
	UInt8 pad54C[0x594 - 0x54C];
	BhkCharacterPhantomView* characterPhantom;
	UInt8 pad598[0x608 - 0x598];
	UInt8 noContact;
	UInt8 pad609[0x60C - 0x609];
	void* bodyUnderFeet;
};

struct HkpContactPointAddedEventView {
	void* bodyA;
	void* bodyB;
};

static_assert(sizeof(HkArrayView<void*>) == 0x0C);
static_assert(offsetof(BhkCollisionObjectView, niNode) == 0x08);
static_assert(offsetof(BhkCollisionObjectView, worldObject) == 0x10);
static_assert(offsetof(HkpWorldObjectView, object) == 0x08);
static_assert(offsetof(HkpWorldObjectView, collisionType) == 0x28);
static_assert(offsetof(HkpWorldObjectView, collisionFilterInfo) == 0x2C);
static_assert(offsetof(HkpCollidableView, collisionFilterInfo) == 0x1C);
static_assert(offsetof(HkpSimulationIslandView, stateFlags) == 0x26);
static_assert(offsetof(HkpRigidBodyView, simulationIsland) == 0xCC);
static_assert(offsetof(HkpRigidBodyView, motionType) == 0xE8);
static_assert(offsetof(ProcessControllerView, characterController) == 0x138);
static_assert(offsetof(BhkCharacterPhantomView, havokPhantom) == 0x08);

static_assert(sizeof(HkpRootCdPointView) == 0x70);
static_assert(offsetof(HkpRootCdPointView, rootCollidableB) == 0x48);
static_assert(sizeof(HkpAllCdPointCollectorView) == 0x3A0);
static_assert(offsetof(HkpAllCdPointCollectorView, hits) == 0x10);
static_assert(offsetof(HkpAllCdPointCollectorView, hits.data) == 0x10);
static_assert(offsetof(HkpAllCdPointCollectorView, hits.size) == 0x14);
static_assert(sizeof(BhkCharacterPointCollectorView) == 0x3C8);
static_assert(offsetof(BhkCharacterPointCollectorView, contactBodies) == 0x3A4);
static_assert(offsetof(BhkCharacterPointCollectorView, contactBodies.data) == 0x3A4);
static_assert(offsetof(BhkCharacterPointCollectorView, contactBodies.size) == 0x3A8);
static_assert(sizeof(BhkSerializableView) == 0x10);
static_assert(offsetof(BhkSerializableView, hkObject) == 0x08);
static_assert(sizeof(BhkCharacterProxyView) == 0x3E0);
static_assert(offsetof(BhkCharacterProxyView, serializable.hkObject) == 0x08);
static_assert(offsetof(BhkCharacterProxyView, pointCollector) == 0x10);
static_assert(offsetof(BhkCharacterProxyView, pointCollector.contactBodies) == 0x3B4);
static_assert(sizeof(HkpCharacterContextView) == 0x30);
static_assert(offsetof(HkpCharacterContextView, hkState) == 0x10);

static_assert(offsetof(BhkCharacterControllerView, proxy.serializable.hkObject) == 0x08);
static_assert(offsetof(BhkCharacterControllerView, proxy.pointCollector.contactBodies) == 0x3B4);
static_assert(offsetof(BhkCharacterControllerView, proxy.pointCollector.contactBodies.data) == 0x3B4);
static_assert(offsetof(BhkCharacterControllerView, proxy.pointCollector.contactBodies.size) == 0x3B8);
static_assert(offsetof(BhkCharacterControllerView, chrContext.hkState) == 0x3F0);
static_assert(offsetof(BhkCharacterControllerView, fallTime) == 0x548);
static_assert(offsetof(BhkCharacterControllerView, characterPhantom) == 0x594);
static_assert(offsetof(BhkCharacterControllerView, noContact) == 0x608);
static_assert(offsetof(BhkCharacterControllerView, bodyUnderFeet) == 0x60C);

static_assert(offsetof(HkpContactPointAddedEventView, bodyA) == 0x00);
static_assert(offsetof(HkpContactPointAddedEventView, bodyB) == 0x04);

inline void* HkpWorldObjectFromCollidableRoot(void* root)
{
	return root ? static_cast<UInt8*>(root) - kHkpWorldObject_Collidable : nullptr;
}

inline BhkCollisionObjectView* BhkCollisionObjectAsView(void* collisionObject)
{
	return static_cast<BhkCollisionObjectView*>(collisionObject);
}

inline HkpWorldObjectView* HkpWorldObjectAsView(void* worldObject)
{
	return static_cast<HkpWorldObjectView*>(worldObject);
}

inline UInt32 HkpWorldObjectGetCollisionFilterInfo(void* worldObject)
{
	return worldObject ? HkpWorldObjectAsView(worldObject)->collisionFilterInfo : 0;
}

inline UInt32 HkpCollidableGetCollisionFilterInfo(void* collidable)
{
	return collidable ? static_cast<HkpCollidableView*>(collidable)->collisionFilterInfo : 0;
}

inline HkpRigidBodyView* HkpRigidBodyAsView(void* rigidBody)
{
	return static_cast<HkpRigidBodyView*>(rigidBody);
}

inline void* BhkWorldObjectGetHavokObject(void* worldObject)
{
	return worldObject ? static_cast<BhkSerializableView*>(worldObject)->hkObject : nullptr;
}

inline UInt8* HkpWorldObjectGetCollisionFilterFlags(void* hkObject)
{
	return hkObject ? static_cast<UInt8*>(hkObject) + 0x2D : nullptr;
}

inline void BhkWorldObjectUpdateCollisionFilter(void* worldObject)
{
	if (!worldObject)
		return;
	void** vtbl = *static_cast<void***>(worldObject);
	reinterpret_cast<void(__thiscall*)(void*)>(vtbl[0xC4 / 4])(worldObject); //bhkWorldObject::UpdateCollisionFilter -> hkpWorld::updateCollisionFilterOnEntity
}

inline bool HkpRigidBodyIsMobile(void* rigidBody)
{
	auto* view = HkpRigidBodyAsView(rigidBody);
	if (!view)
		return false;
	int motionType = view->motionType;
	return CdeclCall<bool>(0xC8CCE0, &motionType);
}

inline bool HkpRigidBodyIsActive(void* rigidBody)
{
	auto* view = HkpRigidBodyAsView(rigidBody);
	if (!view || !view->simulationIsland)
		return false;

	UInt8 activeState = (view->simulationIsland->stateFlags >> 2) & 3;
	return activeState != 0;
}
