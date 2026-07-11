//standalone PathingRequest/Solution machinery shared by PathingCommands and NavmeshCommands
#pragma once
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
