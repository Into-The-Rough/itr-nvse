#pragma once
#include <Windows.h>
#include <cstdint>

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
			if (csInit == 2) return;
			if (InterlockedCompareExchange(&csInit, 1, 0) == 0)
			{
				InitializeCriticalSection(&cs);
				InterlockedExchange(&csInit, 2);
				return;
			}
			while (csInit != 2) Sleep(0);
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
			}
			if (count < kMax)
			{
				refIDs[count] = refID;
				values[count] = now;
				count++;
			}
			LeaveCriticalSection(&cs);
			return true;
		}
	};

	inline uint32_t GetRefID(void* actor) { return *(uint32_t*)((char*)actor + 0x0C); }

	inline bool IsCharacter(void* actor)
	{
		uint8_t typeID = *(uint8_t*)((char*)actor + 0x04);
		return typeID == 0x3B || typeID == 0x43;
	}

	//extracts the combat actor from a combatState, returns null if invalid
	inline void* GetCombatActor(void* combatState)
	{
		if (!combatState) return nullptr;

		void* controller = *(void**)((char*)combatState + 0x1C4);
		if (!controller) return nullptr;

		typedef void* (__thiscall* GetPackageOwner_t)(void*);
		void* actor = ((GetPackageOwner_t)0x97AE90)(controller);
		if (!actor) return nullptr;

		if (!IsCharacter(actor)) return nullptr;

		//skip actors not fully loaded (cell transition spawning)
		if (!*(void**)((char*)actor + 0x68)) return nullptr;

		return actor;
	}

	inline bool AlchemyItemHasEffect(void* alchItem, uint32_t effectFormID)
	{
		void* listHead = (char*)alchItem + 0x40;
		void* data = *(void**)listHead;
		void* next = *(void**)((char*)listHead + 4);

		while (true)
		{
			if (data)
			{
				void* setting = *(void**)((char*)data + 0x14);
				if (setting && *(uint32_t*)((char*)setting + 0x0C) == effectFormID)
					return true;
			}
			if (!next) break;
			data = *(void**)next;
			next = *(void**)((char*)next + 4);
		}
		return false;
	}

	inline void* FindAlchemyItemWithEffect(void* actor, uint32_t effectFormID)
	{
		void* extraDataList = (char*)actor + 0x44;

		typedef void* (__thiscall* GetExtra_t)(void*, uint32_t);
		void* extraCC = ((GetExtra_t)0x410220)(extraDataList, 0x15);
		if (!extraCC) return nullptr;

		void* data = *(void**)((char*)extraCC + 0x0C);
		if (!data) return nullptr;

		void* objList = *(void**)data;
		if (!objList) return nullptr;

		void* nodeData = *(void**)objList;
		void* nodeNext = *(void**)((char*)objList + 4);

		int count = 0;
		while (true)
		{
			if (nodeData)
			{
				void* form = *(void**)((char*)nodeData + 0x08);
				uint32_t typeID = form ? *(uint32_t*)((char*)form + 0x04) : 0;
				if (typeID == 0x2F && AlchemyItemHasEffect(form, effectFormID))
					return form;
			}
			if (!nodeNext) break;
			nodeData = *(void**)nodeNext;
			nodeNext = *(void**)((char*)nodeNext + 4);
			if (++count > 500) break;
		}
		return nullptr;
	}

	inline void UseItem(void* actor, void* item)
	{
		//play eating animation (spoof as stimpak)
		typedef void* (__thiscall* GetProcess_t)(void*);
		void* process = ((GetProcess_t)0x8D8520)(actor);
		if (process)
		{
			typedef void* (*LookupFormByID_t)(uint32_t);
			void* food = ((LookupFormByID_t)0x483A00)(0x00015169); //stimpak
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
