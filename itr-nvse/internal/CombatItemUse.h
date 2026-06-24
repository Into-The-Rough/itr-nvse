#pragma once
#include <Windows.h>
#include <cstdint>
#include "EngineFunctions.h"
#include "GameLayout.h"
#include "ScopedLock.h"

namespace CombatItemUse
{
	struct TimerPool
	{
		static constexpr int kMax = 64;
		uint32_t refIDs[kMax] = {0};
		DWORD values[kMax] = {0};
		int count = 0;
		CRITICAL_SECTION cs;
		volatile LONG csInit = 0;

		void EnsureInit()
		{
			InitCriticalSectionOnce(&csInit, &cs);
		}

		TimerPool() = default;
		TimerPool(const TimerPool&) = delete;
		TimerPool& operator=(const TimerPool&) = delete;

		//checks cooldown and stamps timestamp atomically
		bool TryAcquire(uint32_t refID, float cooldownSec)
		{
			EnsureInit();
			EnterCriticalSection(&cs);
			DWORD now = GetTickCount();
			DWORD cooldownMs = (DWORD)(cooldownSec * 1000.0f);
			int freeSlot = -1;
			for (int i = 0; i < count; i++)
			{
				if (refIDs[i] == refID)
				{
					if ((now - values[i]) < cooldownMs)
					{
						LeaveCriticalSection(&cs);
						return false;
					}
					values[i] = now;
					LeaveCriticalSection(&cs);
					return true;
				}
				if (freeSlot < 0 && (now - values[i]) >= cooldownMs)
					freeSlot = i; //expired entry, reusable once the pool fills
			}
			if (count < kMax)
			{
				refIDs[count] = refID;
				values[count] = now;
				count++;
			}
			else if (freeSlot >= 0)
			{
				refIDs[freeSlot] = refID;
				values[freeSlot] = now;
			}
			else
			{
				LeaveCriticalSection(&cs);
				return false; //all slots within cooldown, too many actors at once
			}
			LeaveCriticalSection(&cs);
			return true;
		}
	};

	inline uint32_t GetRefID(Actor* actor) { return actor ? actor->refID : 0; }

	inline bool IsCharacter(TESObjectREFR* actor)
	{
		return actor && actor->typeID == kFormType_ACHR;
	}

	//extracts the combat actor from a combatState, returns null if invalid
	inline Actor* GetCombatActor(void* combatState)
	{
		if (!combatState) return nullptr;

		void* controller = CombatStateGetCombatController(combatState);
		if (!controller) return nullptr;

		auto* actor = static_cast<Actor*>(Engine::CombatController_GetPackageOwner(controller));
		if (!actor) return nullptr;

		if (!IsCharacter(actor)) return nullptr;

		//skip actors not fully loaded (cell transition spawning)
		if (!actor->baseProcess) return nullptr;

		return actor;
	}

	inline bool AlchemyItemHasEffect(AlchemyItem* alchItem, uint32_t effectFormID)
	{
		auto* effectList = AlchemyItemGetEffectListView(alchItem);
		if (!effectList) return false;

		for (auto* node = &effectList->effects; node; node = node->next)
		{
			EffectItem* effect = node->item;
			EffectSetting* setting = effect ? effect->setting : nullptr;
			if (setting && setting->refID == effectFormID)
				return true;
		}
		return false;
	}

	inline TESForm* FindAlchemyItemWithEffect(Actor* actor, uint32_t effectFormID)
	{
		if (!actor) return nullptr;

		auto* extraCC = static_cast<ExtraContainerChanges*>(
			Engine::BaseExtraList_GetByType(&actor->extraDataList, kExtraData_ContainerChanges));
		if (!extraCC) return nullptr;

		auto* objList = extraCC->data ? extraCC->data->objList : nullptr;
		if (!objList) return nullptr;

		int count = 0;
		for (auto* node = objList->Head(); node; node = node->Next())
		{
			if (++count > 500) break;
			auto* entry = node->Item();
			TESForm* form = entry ? entry->type : nullptr;
			if (form && form->typeID == kFormType_AlchemyItem &&
				AlchemyItemHasEffect(static_cast<AlchemyItem*>(form), effectFormID))
				return form;
		}
		return nullptr;
	}

	inline void UseItem(void* actor, void* item)
	{
		//play eating animation (spoof as stimpak)
		void* process = Engine::Actor_GetProcess(actor);
		if (process)
		{
			void* food = Engine::LookupFormByID(0x00015169); //stimpak
			if (food)
			{
				typedef void(__cdecl* SetUsedItem_t)(void*);
				((SetUsedItem_t)0x600900)(food);

				typedef bool(__thiscall* FindIdle_t)(void*, void*, void*, void*);
				((FindIdle_t)0x8FF0B0)(process, actor, food, nullptr);
			}
		}

		//consume item
		typedef bool(__thiscall* UseAlchItem_t)(void*, void*, void*, bool);
		((UseAlchItem_t)0x8C1F80)(actor, item, nullptr, true);
	}
}
