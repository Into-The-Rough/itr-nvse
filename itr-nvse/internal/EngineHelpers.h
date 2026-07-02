//engine-behaviour helpers over SDK types - vtable dispatch, pointer recovery, list walks.
//kept out of the layout headers, which hold only structs, asserts, and simple field accessors,
//and out of EngineFunctions.h, which stays SDK-light for the fixed-address pointers.
#pragma once

#include "nvse/GameForms.h"
#include "nvse/GameObjects.h"
#include "nvse/GameProcess.h"
#include "nvse/GameEffects.h"
#include "internal/CallTemplates.h"

class ScopedCellRefLock
{
public:
	explicit ScopedCellRefLock(TESObjectCELL* cell) : cell_(cell)
	{
		if (cell_)
			ThisCall<void>(0x541AC0, cell_); //TESObjectCELL::LockRefLists
	}

	~ScopedCellRefLock()
	{
		if (cell_)
			ThisCall<void>(0x541AE0, cell_); //TESObjectCELL::UnlockRefLists
	}

	ScopedCellRefLock(const ScopedCellRefLock&) = delete;
	ScopedCellRefLock& operator=(const ScopedCellRefLock&) = delete;

private:
	TESObjectCELL* cell_;
};

inline Actor* ActorValueOwnerToActor(ActorValueOwner* owner)
{
	return owner ? reinterpret_cast<Actor*>(reinterpret_cast<UInt8*>(owner) - offsetof(Actor, avOwner)) : nullptr;
}

inline float ActorValueOwnerGetBaseValue(ActorValueOwner* owner, UInt32 avCode)
{
	if (!owner) return 0.0f;
	using GetAV_t = float (__thiscall*)(ActorValueOwner*, UInt32);
	auto** vtbl = *reinterpret_cast<void***>(owner);
	return reinterpret_cast<GetAV_t>(vtbl[1])(owner, avCode);
}

inline float ActorValueOwnerGetValue(ActorValueOwner* owner, UInt32 avCode)
{
	if (!owner) return 0.0f;
	using GetAV_t = float (__thiscall*)(ActorValueOwner*, UInt32);
	auto** vtbl = *reinterpret_cast<void***>(owner);
	return reinterpret_cast<GetAV_t>(vtbl[3])(owner, avCode);
}

inline Actor* MagicTargetToActor(MagicTarget* magicTarget)
{
	if (!magicTarget) return nullptr;
	using IsActor_t = bool (__thiscall*)(MagicTarget*);
	auto** vtbl = *reinterpret_cast<void***>(magicTarget);
	if (!reinterpret_cast<IsActor_t>(vtbl[3])(magicTarget)) return nullptr;
	return reinterpret_cast<Actor*>(reinterpret_cast<UInt8*>(magicTarget) - offsetof(Actor, magicTarget));
}

inline EffectNode* MagicTargetGetEffectList(MagicTarget* magicTarget)
{
	return magicTarget ? magicTarget->GetEffectList() : nullptr;
}

inline void BaseProcessSetAmmoInfo(BaseProcess* process, BaseProcess::AmmoInfo* ammoInfo)
{
	if (!process) return;
	using SetAmmoInfo_t = void (__thiscall *)(BaseProcess*, BaseProcess::AmmoInfo*);
	auto* vtbl = *reinterpret_cast<UInt32**>(process);
	if (!vtbl) return;
	reinterpret_cast<SetAmmoInfo_t>(vtbl[90])(process, ammoInfo);
}

inline TESQuest* TESTopicGetQuestForInfo(TESTopic* topic, TESTopicInfo* topicInfo)
{
	if (!topic || !topicInfo) return nullptr;
	//infos is tList<Info*> - the SDK types the node item as Info**, reinterpret the slot back to the stored Info*
	for (auto* node = topic->infos.Head(); node && node->item; node = node->next) {
		auto* info = reinterpret_cast<TESTopic::Info*>(node->item);
		auto& infoArray = info->infoArray;
		if (!infoArray.data) continue;
		for (UInt32 i = 0; i < infoArray.firstFreeEntry; ++i)
			if (infoArray.data[i] == topicInfo)
				return info->quest;
	}
	return nullptr;
}
