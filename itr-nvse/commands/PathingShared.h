//standalone PathingRequest/Solution machinery shared by PathingCommands and NavmeshCommands
#pragma once
#include <cstddef>
#include "internal/CallTemplates.h"

class TESObjectCELL;
class TESWorldSpace;
class TESObjectREFR;

struct PathPoint3
{
	float x;
	float y;
	float z;
};
static_assert(sizeof(PathPoint3) == 0x0C);

struct PathingLocationLayout
{
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
static_assert(sizeof(PathingLocationLayout) == 0x28);

//PathingCoverLocation extends PathingLocation, cover bytes written by 0x6E4500 during
//cover-point generation in 0x6D62E0
struct PathingCoverLocationLayout
{
	PathingLocationLayout location;
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
static_assert(offsetof(PathingCoverLocationLayout, edgeA) == 0x28);
static_assert(offsetof(PathingCoverLocationLayout, heightBucket) == 0x4C);
static_assert(offsetof(PathingCoverLocationLayout, edgeSlot) == 0x51);
static_assert(sizeof(PathingCoverLocationLayout) == 0x54);

//the 120-byte cover object shared by the combat state slots and CombatProcedureBeInCover,
//ctor 0x6D3150, embedded PathingCoverLocation proven by the add ecx, 14h calls in 0x99E060
struct CombatCoverLocationLayout
{
	UInt8 unk00;
	UInt8 reserved; //0x5DC960
	UInt8 unk02;
	UInt8 pad03;
	UInt32 unk04;
	PathPoint3 position;
	PathingCoverLocationLayout cover;
	UInt32 reservationKey; //0x6E4570, (navmesh << 16) + 4 * triangle + edgeSlot
	SInt32 unk6C;
	UInt8 tail70[8];
};
static_assert(offsetof(CombatCoverLocationLayout, position) == 0x08);
static_assert(offsetof(CombatCoverLocationLayout, cover) == 0x14);
static_assert(offsetof(CombatCoverLocationLayout, reservationKey) == 0x68);
static_assert(sizeof(CombatCoverLocationLayout) == 0x78);

struct PathingNodeLayout
{
	UInt32 flags;
	PathingLocationLayout pathingLocation;
	PathPoint3 tangent;
	TESObjectREFR* actionRef;
};
static_assert(sizeof(PathingNodeLayout) == 0x3C);

template <typename T>
struct BSSimpleArrayLayout
{
	void* vtbl;
	T* data;
	UInt32 size;
	UInt32 allocSize;
};
static_assert(sizeof(BSSimpleArrayLayout<void>) == 0x10);

struct PathingSolutionLayout
{
	void* vtbl;
	UInt32 refCount;
	BSSimpleArrayLayout<void> virtualNodes;
	SInt32 firstLoadedVirtualNodeIndex;
	SInt32 lastLoadedVirtualNodeIndex;
	BSSimpleArrayLayout<PathingNodeLayout> currentNodes;
	BSSimpleArrayLayout<UInt32> previousNodes;
	UInt8 incompletePath;
};
static_assert(sizeof(PathingSolutionLayout) == 0x44);

struct ScopedPathingRequest
{
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

struct ScopedPathingSolution
{
	alignas(4) UInt8 data[sizeof(PathingSolutionLayout)] = {};

	ScopedPathingSolution()
	{
		ThisCall<void>(0x6E7650, data); //PathingSolution ctor
	}

	~ScopedPathingSolution()
	{
		ThisCall<void>(0x6E7720, data); //PathingSolution dtor
	}

	PathingSolutionLayout* Get()
	{
		return reinterpret_cast<PathingSolutionLayout*>(data);
	}
};

//NavMeshPtr, the owning NiPointer<NavMesh> the engine hands back from a NavMeshInfo
struct ScopedNavMeshPtr
{
	void* navMesh = nullptr;

	~ScopedNavMeshPtr()
	{
		Release();
	}

	void Release()
	{
		if (navMesh)
		{
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
