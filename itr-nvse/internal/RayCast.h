//shared Havok raycast payload for TES::PickObject (0x458440)
#pragma once

struct alignas(16) RayCastData {
	float pos0[4];
	float pos1[4];
	UInt8 byte20;
	UInt8 pad21[3];
	UInt8 layerType;
	UInt8 filterFlags;
	UInt16 group;
	UInt32 unk28[6];
	float hitFraction;
	UInt32 unk44[15];
	void* cdBody;
	UInt32 unk84[3];
	float vector90[4];
	UInt32 unkA0[3];
	UInt8 byteAC;
	UInt8 padAD[3];
};
static_assert(sizeof(RayCastData) == 0xB0, "RayCastData size mismatch");

constexpr float kHavokScale = 0.1428571f;
