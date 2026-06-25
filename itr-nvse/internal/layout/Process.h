//Actor process layout - process manager iteration + padded actor/process views
#pragma once

#include <cstddef>

#include "common/ITypes.h"

#include "nvse/GameProcess.h"

template <typename T>
struct NiTArrayLite {
	void** vtbl;
	T* data;
	UInt16 capacity;
	UInt16 firstFreeEntry;
	UInt16 numObjs;
	UInt16 growSize;
};

struct ProcessManagerLite {
	UInt32 unk000;
	NiTArrayLite<void*> objects;
	UInt32 beginOffsets[4];
	UInt32 endOffsets[4];
};

struct ProcessDamageDealtView {
	UInt8 pad00[0xAC];
	float damageDealt;
};

struct ActorForceSneakView {
	UInt8 pad00[0x125];
	UInt8 forceSneak;
};

struct ActorTeammateView {
	UInt8 pad00[0x18D];
	bool isTeammate;
};

struct HighProcessFadeView {
	UInt8 pad00[0x3EC];
	float delayTime;
};

struct HighProcessQueuedGreetView {
	UInt8 pad00[0x3E4];
	void* queuedGreetTopic;
};

static_assert(offsetof(ActorForceSneakView, forceSneak) == 0x125);
static_assert(offsetof(ActorTeammateView, isTeammate) == 0x18D);
static_assert(offsetof(ProcessDamageDealtView, damageDealt) == 0xAC);
static_assert(offsetof(HighProcessFadeView, delayTime) == 0x3EC);
static_assert(offsetof(HighProcessQueuedGreetView, queuedGreetTopic) == 0x3E4);
static_assert(sizeof(NiTArrayLite<void*>) == 0x10);
static_assert(offsetof(ProcessManagerLite, objects) == 0x04);
static_assert(offsetof(ProcessManagerLite, beginOffsets) == 0x14);
static_assert(offsetof(ProcessManagerLite, endOffsets) == 0x24);

inline float* LowProcessGetDamageDealtCounter(BaseProcess* process)
{
	return process ? &reinterpret_cast<ProcessDamageDealtView*>(process)->damageDealt : nullptr;
}

inline void ActorSetForceSneak(Actor* actor, UInt8 forceSneak)
{
	if (actor) reinterpret_cast<ActorForceSneakView*>(actor)->forceSneak = forceSneak;
}

inline bool ActorIsTeammate(Actor* actor)
{
	return actor && reinterpret_cast<ActorTeammateView*>(actor)->isTeammate;
}

inline void HighProcessSetDelayTime(void* process, float delayTime)
{
	if (process) reinterpret_cast<HighProcessFadeView*>(process)->delayTime = delayTime;
}

inline void* HighProcessGetQueuedGreetTopic(BaseProcess* process)
{
	return process ? reinterpret_cast<HighProcessQueuedGreetView*>(process)->queuedGreetTopic : nullptr;
}
