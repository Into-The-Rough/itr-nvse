//allows NPCs to use antidotes when poisoned during combat

#include "NPCAntidoteUse.h"
#include <Windows.h>
#include <cstdint>

namespace NPCAntidoteUse
{
	//config
	float g_fCombatItemCureTimer = 10.0f;
	float g_fCureHealthThreshold = 25.0f;
	bool g_enabled = false;

	//NVAntivenomEffect "Cure Animal Poison" [MGEF:000E2C6D]
	constexpr uint32_t kFormID_CurePoisonEffect = 0xE2C6D;

	//addresses
	constexpr uint32_t kAddr_DrinkPotion = 0x8C1F80; //Actor::DrinkPotion
	constexpr uint32_t kAddr_GetPackageOwner = 0x97AE90;
	constexpr uint32_t kAddr_GetExtraData = 0x410220; //BaseExtraList::GetByType

	//simple timer using real-time milliseconds
	constexpr int MAX_TIMERS = 64;
	uint32_t g_timerRefIDs[MAX_TIMERS] = {0};
	DWORD g_timerValues[MAX_TIMERS] = {0};
	int g_timerCount = 0;

	bool IsTimerReady(uint32_t refID)
	{
		DWORD now = GetTickCount();
		DWORD cooldownMs = (DWORD)(g_fCombatItemCureTimer * 1000.0f);
		for (int i = 0; i < g_timerCount; i++)
		{
			if (g_timerRefIDs[i] == refID)
				return (now - g_timerValues[i]) >= cooldownMs;
		}
		return true;
	}

	void ResetTimer(uint32_t refID)
	{
		DWORD now = GetTickCount();
		for (int i = 0; i < g_timerCount; i++)
		{
			if (g_timerRefIDs[i] == refID)
			{
				g_timerValues[i] = now;
				return;
			}
		}
		if (g_timerCount < MAX_TIMERS)
		{
			g_timerRefIDs[g_timerCount] = refID;
			g_timerValues[g_timerCount] = now;
			g_timerCount++;
		}
	}

	typedef void* (__thiscall *GetPackageOwner_t)(void* controller);
	GetPackageOwner_t GetPackageOwner = (GetPackageOwner_t)kAddr_GetPackageOwner;

	uint32_t GetRefID(void* actor) { return *(uint32_t*)((char*)actor + 0x0C); }
	uint32_t GetTypeID(void* form) { return *(uint32_t*)((char*)form + 0x04); }

	float GetHealthPercent(void* actor)
	{
		//ActorValueOwner at Actor+0xA4, vtable[3]=GetActorValue, vtable[1]=GetBaseActorValue
		void* avOwner = (char*)actor + 0xA4;
		void** vtbl = *(void***)avOwner;
		if (!vtbl) return 100.0f;

		typedef float(__thiscall* GetAV_t)(void*, uint32_t);
		float current = ((GetAV_t)vtbl[3])(avOwner, 0x10); //Health
		float base = ((GetAV_t)vtbl[1])(avOwner, 0x10);

		if (base <= 0.0f) return 100.0f;
		return (current / base) * 100.0f;
	}

	bool IsPoisoned(void* actor)
	{
		//MagicTarget at 0x94, vtable[2] = GetEffectList
		void* magicTarget = (char*)actor + 0x94;
		void** vtbl = *(void***)magicTarget;
		if (!vtbl) return false;

		typedef void* (__thiscall* GetEffectList_t)(void*);
		void* effectList = ((GetEffectList_t)vtbl[2])(magicTarget);
		if (!effectList) return false;

		void* node = *(void**)effectList;
		void* next = *(void**)((char*)effectList + 4);

		while (true)
		{
			if (node)
			{
				uint32_t spellType = *(uint32_t*)((char*)node + 0x2C);
				if (spellType == 5) //poison
					return true;
			}
			if (!next) break;
			node = *(void**)next;
			next = *(void**)((char*)next + 4);
		}
		return false;
	}

	bool HasCurePoisonEffect(void* alchItem)
	{
		//tList head at AlchemyItem+0x40
		void* listHead = (char*)alchItem + 0x40;
		void* data = *(void**)listHead;
		void* next = *(void**)((char*)listHead + 4);

		while (true)
		{
			if (data)
			{
				void* setting = *(void**)((char*)data + 0x14); //EffectItem::setting
				if (setting)
				{
					uint32_t settingRefID = *(uint32_t*)((char*)setting + 0x0C);
					if (settingRefID == kFormID_CurePoisonEffect)
						return true;
				}
			}
			if (!next) break;
			data = *(void**)next;
			next = *(void**)((char*)next + 4);
		}
		return false;
	}

	void* FindAntidoteInInventory(void* actor)
	{
		void* extraDataList = (char*)actor + 0x44;

		typedef void* (__thiscall* GetExtra_t)(void*, uint32_t);
		void* extraCC = ((GetExtra_t)kAddr_GetExtraData)(extraDataList, 0x15);
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
				if (form && GetTypeID(form) == 0x2F) //AlchemyItem
				{
					if (HasCurePoisonEffect(form))
						return form;
				}
			}
			if (!nodeNext) break;
			nodeData = *(void**)nodeNext;
			nodeNext = *(void**)((char*)nodeNext + 4);
			if (++count > 500) break;
		}
		return nullptr;
	}

	void DrinkPotion(void* actor, void* potion)
	{
		typedef bool(__thiscall* Fn)(void*, void*, void*, bool);
		((Fn)kAddr_DrinkPotion)(actor, potion, nullptr, true);
	}

	void Check(void* combatState)
	{
		if (!g_enabled || !combatState)
			return;

		void* controller = *(void**)((char*)combatState + 0x1C4);
		if (!controller) return;

		void* actor = GetPackageOwner(controller);
		if (!actor) return;

		uint32_t refID = GetRefID(actor);

		float health = GetHealthPercent(actor);
		if (health < g_fCureHealthThreshold)
			return;

		if (!IsPoisoned(actor))
			return;

		if (!IsTimerReady(refID))
			return;

		void* antidote = FindAntidoteInInventory(actor);
		if (!antidote)
			return;

		DrinkPotion(actor, antidote);
		ResetTimer(refID);
	}
}

void NPCAntidoteUse_Init(float cureTimer, float healthThreshold)
{
	NPCAntidoteUse::g_fCombatItemCureTimer = cureTimer;
	NPCAntidoteUse::g_fCureHealthThreshold = healthThreshold;
	NPCAntidoteUse::g_enabled = true;
}

void NPCAntidoteUse_Check(void* combatState)
{
	NPCAntidoteUse::Check(combatState);
}
