#pragma once

#include <cstddef>

#include "common/ITypes.h"

struct BSSoundHandleView {
	UInt32 uiSoundID; //0x00
	bool bAssumeSuccess; //0x04
	UInt32 uiState; //0x08
};

struct BSGameSoundView {
	void* vtbl; //0x00
	UInt32 mapKey; //0x04
	UInt32 soundFlags; //0x08
	UInt32 flags00C; //0x0C
	UInt32 stateFlags; //0x10
	UInt32 duration; //0x14
	UInt16 staticAttenuation; //0x18
	UInt16 unk01A; //0x1A
	UInt16 unk01C; //0x1C
	UInt16 unk01E; //0x1E
	UInt16 unk020; //0x20
	UInt16 unk022; //0x22
	float volume; //0x24
	float flt028; //0x28
	float flt02C; //0x2C
	UInt32 unk030; //0x30
	UInt16 baseSamplingFreq; //0x34
	char filePath[254]; //0x36
};

struct SoundMapEntryView {
	SoundMapEntryView* next;
	UInt32 key;
	void* data;
};

struct SoundMapView {
	void* vtbl;
	UInt32 numBuckets;
	SoundMapEntryView** buckets;
	UInt32 numItems;

	BSGameSoundView* Lookup(UInt32 key)
	{
		if (!buckets || !numBuckets) return nullptr;
		for (SoundMapEntryView* e = buckets[key % numBuckets]; e; e = e->next)
			if (e->key == key)
				return static_cast<BSGameSoundView*>(e->data);
		return nullptr;
	}
};

struct BSAudioManagerView {
	UInt8 pad00[0x54];
	SoundMapView playingSounds;
};

static_assert(offsetof(BSSoundHandleView, uiSoundID) == 0x00);
static_assert(offsetof(BSSoundHandleView, bAssumeSuccess) == 0x04);
static_assert(offsetof(BSSoundHandleView, uiState) == 0x08);
static_assert(sizeof(BSSoundHandleView) == 0x0C);
static_assert(offsetof(BSGameSoundView, soundFlags) == 0x08);
static_assert(offsetof(BSGameSoundView, filePath) == 0x36);
static_assert(offsetof(BSAudioManagerView, playingSounds) == 0x54);
