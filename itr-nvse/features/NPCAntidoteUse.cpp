//allows NPCs to use antidotes when poisoned during combat

#include "NPCAntidoteUse.h"
#include "internal/CombatItemUse.h"

namespace NPCAntidoteUse
{
	static CombatItemUse::TimerPool s_timers;
	static float g_cooldown = 10.0f;
	static float g_healthThreshold = 25.0f;
	static bool g_enabled = false;

	static float GetHealthPercent(Actor* actor)
	{
		auto* avOwner = ActorGetActorValueOwner(actor);
		float current = ActorValueOwnerGetValue(avOwner, 0x10);
		float base = ActorValueOwnerGetBaseValue(avOwner, 0x10);

		if (base <= 0.0f) return 100.0f;
		return (current / base) * 100.0f;
	}

	static bool IsPoisoned(Actor* actor)
	{
		auto* effectList = MagicTargetGetEffectList(ActorGetMagicTarget(actor));
		if (!effectList) return false;

		for (auto* node = effectList->Head(); node; node = node->Next())
		{
			auto* effect = node->Item();
			if (effect && effect->spellType == 5) //poison
				return true;
		}
		return false;
	}

	void Init(float cureTimer, float healthThreshold)
	{
		g_cooldown = cureTimer;
		g_healthThreshold = healthThreshold;
		g_enabled = true;
	}

	void UpdateSettings(float cureTimer, float healthThreshold)
	{
		g_cooldown = cureTimer;
		g_healthThreshold = healthThreshold;
	}

	void Check(void* combatState)
	{
		if (!g_enabled) return;

		auto* actor = CombatItemUse::GetCombatActor(combatState);
		if (!actor) return;

		//below the threshold the actor is dying, yield to vanilla low-health item use
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
