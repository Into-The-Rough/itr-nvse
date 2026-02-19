//allows NPCs to use antidotes when poisoned during combat

#include "NPCAntidoteUse.h"
#include "internal/CombatItemUse.h"

namespace NPCAntidoteUse
{
	static CombatItemUse::TimerPool s_timers;
	static float g_cooldown = 10.0f;
	static float g_healthThreshold = 25.0f;
	static bool g_enabled = false;

	static float GetHealthPercent(void* actor)
	{
		//ActorValueOwner at Actor+0xA4
		void* avOwner = (char*)actor + 0xA4;
		void** vtbl = *(void***)avOwner;

		typedef float(__thiscall* GetAV_t)(void*, uint32_t);
		float current = ((GetAV_t)vtbl[3])(avOwner, 0x10);
		float base = ((GetAV_t)vtbl[1])(avOwner, 0x10);

		if (base <= 0.0f) return 100.0f;
		return (current / base) * 100.0f;
	}

	static bool IsPoisoned(void* actor)
	{
		//MagicTarget at 0x94, vtable[2] = GetEffectList
		void* magicTarget = (char*)actor + 0x94;
		void** vtbl = *(void***)magicTarget;

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

	void Check(void* combatState)
	{
		if (!g_enabled) return;

		auto* actor = CombatItemUse::GetCombatActor(combatState);
		if (!actor) return;

		if (GetHealthPercent(actor) < g_healthThreshold) return;
		if (!IsPoisoned(actor)) return;

		uint32_t refID = CombatItemUse::GetRefID(actor);
		if (!s_timers.TryAcquire(refID, g_cooldown)) return;

		//NVAntivenomEffect "Cure Animal Poison" [MGEF:000E2C6D]
		void* item = CombatItemUse::FindAlchemyItemWithEffect(actor, 0xE2C6D);
		if (!item) return;

		CombatItemUse::UseItem(actor, item);
	}
}

void NPCAntidoteUse_Init(float cureTimer, float healthThreshold)
{
	NPCAntidoteUse::g_cooldown = cureTimer;
	NPCAntidoteUse::g_healthThreshold = healthThreshold;
	NPCAntidoteUse::g_enabled = true;
}

void NPCAntidoteUse_Check(void* combatState)
{
	NPCAntidoteUse::Check(combatState);
}
