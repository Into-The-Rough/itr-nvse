#pragma once
#include <cstddef>
#include "internal/CallTemplates.h"

class TESObjectCELL;
class TESWorldSpace;
class TESObjectREFR;

struct PathPoint3 {
	float x;
	float y;
	float z;
};

struct PathingLocationView {
	void* vtbl;
	PathPoint3 location;
	void* navMeshInfo;
	void* navMeshes;
	TESObjectCELL* cell;
	TESWorldSpace* worldSpace;
	UInt32 cellCoords;
	UInt16 triangle;
	UInt8 flags;
	UInt8 clientData;
};

//PathingCoverLocation extends PathingLocation, cover bytes written by 0x6E4500 during
//cover-point generation in 0x6D62E0
struct PathingCoverLocationView {
	PathingLocationView location;
	PathPoint3 edgeA;
	PathPoint3 edgeB;
	PathPoint3 coverNormal;
	UInt8 heightBucket; //2 = crouch-only cover (0x6E4820 tests for it), >= 4 = standing cover
	UInt8 crouchCover;
	UInt8 standCover;
	UInt8 leftOpen;
	UInt8 rightOpen;
	UInt8 edgeSlot; //0 or 1, the slot passed to NavMesh::GetEdgeEndpoints
	UInt8 unk52;
	UInt8 pad53;
};

//the 120-byte cover object shared by the combat state slots and CombatProcedureBeInCover,
//ctor 0x6D3150, embedded PathingCoverLocation proven by the add ecx, 14h calls in 0x99E060
struct CombatCoverLocationView {
	UInt8 unk00;
	UInt8 reserved; //0x5DC960
	UInt8 unk02;
	UInt8 pad03;
	UInt32 unk04;
	PathPoint3 position;
	PathingCoverLocationView cover;
	UInt32 reservationKey; //0x6E4570, (navmesh << 16) + 4 * triangle + edgeSlot
	SInt32 unk6C;
	UInt8 tail70[8];
};

struct PathingNodeView {
	UInt32 flags;
	PathingLocationView pathingLocation;
	PathPoint3 tangent;
	TESObjectREFR* actionRef;
};

template<typename T>
struct PathingSimpleArrayView {
	void* vtbl;
	T* data;
	UInt32 size;
	UInt32 allocSize;
};

struct PathingSolutionView {
	void* vtbl;
	UInt32 refCount;
	PathingSimpleArrayView<void> virtualNodes;
	SInt32 firstLoadedVirtualNodeIndex;
	SInt32 lastLoadedVirtualNodeIndex;
	PathingSimpleArrayView<PathingNodeView> currentNodes;
	PathingSimpleArrayView<UInt32> previousNodes;
	UInt8 incompletePath;
};

struct ScopedPathingRequest {
	alignas(4) UInt8 data[0xB0] = {};

	ScopedPathingRequest()
	{
		ThisCall<void>(0x6E2420, data); //PathingRequest ctor
	}

	~ScopedPathingRequest()
	{
		ThisCall<void>(0x6E2620, data); //PathingRequest dtor
	}

	void* Get()
	{
		return data;
	}
};

struct ScopedPathingSolution {
	alignas(4) UInt8 data[sizeof(PathingSolutionView)] = {};

	ScopedPathingSolution()
	{
		ThisCall<void>(0x6E7650, data); //PathingSolution ctor
	}

	~ScopedPathingSolution()
	{
		ThisCall<void>(0x6E7720, data); //PathingSolution dtor
	}

	PathingSolutionView* Get()
	{
		return reinterpret_cast<PathingSolutionView*>(data);
	}
};

//NavMeshPtr, the owning NiPointer<NavMesh> the engine hands back from a NavMeshInfo
struct ScopedNavMeshPtr {
	void* navMesh = nullptr;

	~ScopedNavMeshPtr()
	{
		Release();
	}

	void Release()
	{
		if (navMesh) {
			ThisCall<void>(0x42FDA0, &navMesh); //NavMeshPtr dtor, drops the strong ref
			navMesh = nullptr;
		}
	}

	//the engine assigns through NiPointer::operator=, which releases whatever the slot already holds
	void** Slot()
	{
		return &navMesh;
	}

	void* Get() const
	{
		return navMesh;
	}
};

static_assert(sizeof(PathPoint3) == 0x0C);
static_assert(sizeof(PathingLocationView) == 0x28);
static_assert(offsetof(PathingCoverLocationView, edgeA) == 0x28);
static_assert(offsetof(PathingCoverLocationView, heightBucket) == 0x4C);
static_assert(offsetof(PathingCoverLocationView, edgeSlot) == 0x51);
static_assert(sizeof(PathingCoverLocationView) == 0x54);
static_assert(offsetof(CombatCoverLocationView, position) == 0x08);
static_assert(offsetof(CombatCoverLocationView, cover) == 0x14);
static_assert(offsetof(CombatCoverLocationView, reservationKey) == 0x68);
static_assert(sizeof(CombatCoverLocationView) == 0x78);
static_assert(sizeof(PathingNodeView) == 0x3C);
static_assert(sizeof(PathingSimpleArrayView<void>) == 0x10);
static_assert(sizeof(PathingSolutionView) == 0x44);
