//caution/combat mode never clears - combat groups only drop members while the member is
//loaded, in combat and high-processed, so actors that die or unload outside that window stay
//grouped and keep the player flagged in combat. the engine's own prune (0x98EB30) is wired
//only to savegame load, so run it on combat-state change, stop combat for actors demoted out
//of high process, and reconcile the bIsDetected flag

#include "StuckCombatStateFix.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/Detours.h"
#include "internal/CallTemplates.h"
#include "internal/GameGlobals.h"
#include "internal/GameLayout.h"
#include "internal/EngineFunctions.h"

#include "internal/globals.h"

namespace StuckCombatStateFix
{
	static Detours::JumpDetour s_stopCombatDetour;
	static Detours::CallDetour s_demoteDetours[3];
	static UInt8* s_stopCombatOrig = nullptr;

	static bool s_dirty = false;
	static UInt32 s_demotedRefIDs[64];
	static UInt32 s_demotedCount = 0;

	constexpr UInt32 kCombatManagerPtr = 0x11F1958;
	constexpr UInt32 kGroupMemberCount = 0x990890;
	//E8 sites calling ProcessLists::RemoveActorFromDetectionLists (0x973CB0) in the three
	//high->low demotion virtuals 0x882B90/0x883240/0x883800
	constexpr UInt32 kDemoteSites[3] = { 0x882E7B, 0x883468, 0x883C3A };

	static bool ActorInCombat(Actor* apActor)
	{
		return *((UInt8*)apActor + 0x104) != 0;
	}

	//0x8A06C0 Actor::StopCombat, shared impl in Actor/Character/Creature vtables (slot +0x434).
	//every combat end funnels through here (Actor::Kill at 0x89DD8B, wants-stop, invalid target),
	//so a stale membership can only appear after one of these calls
	static void __fastcall Hook_StopCombat(Actor* apThis, void*, Actor* apTarget)
	{
		s_dirty = true;
		ThisCall<void>((UInt32)s_stopCombatOrig, apThis, apTarget);
	}

	static void QueueDemoted(Actor* apActor)
	{
		if (!apActor || !ActorInCombat(apActor))
			return;
		UInt32 refID = ((TESForm*)apActor)->refID;
		for (UInt32 i = 0; i < s_demotedCount; i++)
			if (s_demotedRefIDs[i] == refID)
				return;
		if (s_demotedCount >= 64)
		{
			Log("StuckCombatStateFix: demoted queue full, dropping %08X", refID);
			return;
		}
		s_demotedRefIDs[s_demotedCount++] = refID;
	}

	template <int Index>
	static void __fastcall Hook_Demote(void* apProcessLists, void*, Actor* apActor)
	{
		ThisCall<void>(s_demoteDetours[Index].GetOverwrittenAddr(), apProcessLists, apActor);
		QueueDemoted(apActor);
	}

	static void PruneGroups()
	{
		void* manager = *(void**)kCombatManagerPtr;
		if (!manager)
			return;
		UInt32 count = ThisCall<UInt32>(0x658930, manager); //CombatManager::GetGroupCount
		for (UInt32 i = 0; i < count; i++)
		{
			void** slot = ThisCall<void**>(0x877A30, manager, i); //CombatManager::GetGroupAt
			if (!slot || !*slot)
				continue;
			UInt32 before = ThisCall<UInt32>(kGroupMemberCount, *slot);
			//CombatGroup::PruneMembers, load-only in vanilla (0x993790 <- FinishLoadGlobalData
			//case 5), stack arg is popped but never read
			ThisCall<UInt32>(0x98EB30, *slot, 0);
			UInt32 after = ThisCall<UInt32>(kGroupMemberCount, *slot);
			if (before != after)
				Log("StuckCombatStateFix: pruned %u stale member(s) from combat group %u/%u", before - after, i, count);
		}
		//emptied groups are destroyed by the engine's own CombatManager::UpdateGroups (0x991600)
	}

	static void ReconcileIsDetected()
	{
		PlayerCharacter* player = *g_thePlayerPtr;
		if (!player)
			return;
		UInt8* isDetected = (UInt8*)player + 0x5F8;
		if (!*isDetected)
			return;
		bool unseen = false;
		if (ThisCall<bool>(0x953C50, player, &unseen)) //PlayerCharacter::GetCombatStatus
			return;
		//no combat - clear only if no live hostile detector remains, same criterion the
		//setter at 0x973A1E uses to assert the flag
		SInt32 highest = ThisCall<SInt32>(0x973710, g_processManager, player, 0); //ProcessLists::GetHighestDetectionLevelForActor
		if (highest <= 0)
		{
			*isDetected = 0;
			Log("StuckCombatStateFix: cleared stale bIsDetected");
		}
	}

	void Update()
	{
		for (UInt32 i = 0; i < s_demotedCount; i++)
		{
			TESForm* form = (TESForm*)Engine::LookupFormByID(s_demotedRefIDs[i]);
			if (!form || !TESFormIsActorRef(form))
				continue;
			Actor* actor = (Actor*)form;
			if (!ActorInCombat(actor))
				continue;
			//mirrors the teammate-gated StopCombat in MiddleLowProcess::Update 0x90ADAA
			Log("StuckCombatStateFix: stopping combat for demoted actor %08X", form->refID);
			//through the vtable so other plugins' StopCombat hooks chain
			void** vtbl = *(void***)actor;
			((void(__thiscall*)(Actor*, Actor*))vtbl[0x434 / 4])(actor, nullptr);
		}
		s_demotedCount = 0;

		if (s_dirty)
		{
			s_dirty = false;
			PruneGroups();
		}

		ReconcileIsDetected();
	}

	void ClearState()
	{
		s_demotedCount = 0;
		s_dirty = false;
	}

	//push ebp / mov ebp,esp / sub esp,18h
	static constexpr UInt8 kStopCombatPrologue[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x18 };

	void Init()
	{
		if (memcmp((void*)0x8A06C0, kStopCombatPrologue, sizeof(kStopCombatPrologue)) != 0)
		{
			Log("StuckCombatStateFix: StopCombat prologue bytes changed, fix disabled");
			return;
		}
		if (!s_stopCombatDetour.WriteRelJump(0x8A06C0, Hook_StopCombat, 6, &s_stopCombatOrig))
		{
			Log("StuckCombatStateFix: StopCombat prologue already patched, fix disabled");
			return;
		}
		if (!s_demoteDetours[0].WriteRelCall(kDemoteSites[0], Hook_Demote<0>))
			Log("StuckCombatStateFix: demote site %08X not hookable", kDemoteSites[0]);
		if (!s_demoteDetours[1].WriteRelCall(kDemoteSites[1], Hook_Demote<1>))
			Log("StuckCombatStateFix: demote site %08X not hookable", kDemoteSites[1]);
		if (!s_demoteDetours[2].WriteRelCall(kDemoteSites[2], Hook_Demote<2>))
			Log("StuckCombatStateFix: demote site %08X not hookable", kDemoteSites[2]);
		Log("StuckCombatStateFix: installed");
	}
}
