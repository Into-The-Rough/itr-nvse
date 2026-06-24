//allows NPCs to use doctor's bags when they have crippled limbs during combat

#include "NPCDoctorsBagUse.h"
#include "internal/CombatItemUse.h"

namespace NPCDoctorsBagUse
{
	static CombatItemUse::TimerPool s_timers;
	static float g_cooldown = 15.0f;
	static bool g_enabled = false;

	static float GetActorValue(Actor* actor, uint32_t avCode)
	{
		return ActorValueOwnerGetValue(ActorGetActorValueOwner(actor), avCode);
	}

	static bool HasCrippledLimb(Actor* actor)
	{
		if (GetActorValue(actor, 0x48) > 0.0f) //IgnoreCrippledLimbs
			return false;

		//PerceptionCondition(0x19) through RightMobilityCondition(0x1E)
		for (uint32_t av = 0x19; av <= 0x1E; av++)
			if (GetActorValue(actor, av) <= 0.0f)
				return true;

		return false;
	}

	void Init(float useTimer)
	{
		g_cooldown = useTimer;
		g_enabled = true;
	}

	void Check(void* combatState)
	{
		if (!g_enabled) return;

		auto* actor = CombatItemUse::GetCombatActor(combatState);
		if (!actor) return;

		if (!HasCrippledLimb(actor)) return;

		uint32_t refID = CombatItemUse::GetRefID(actor);
		if (!s_timers.TryAcquire(refID, g_cooldown)) return;

		//NVRestoreLimbs "Restore Limb Condition" [MGEF:000FFCA0]
		void* item = CombatItemUse::FindAlchemyItemWithEffect(actor, 0xFFCA0);
		if (!item) return;

		CombatItemUse::UseItem(actor, item);
	}
}
