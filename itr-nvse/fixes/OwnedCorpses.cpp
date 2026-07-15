//dead actors inherit cell/zone ownership so looting corpses can be stealing

#include "OwnedCorpses.h"
#include "internal/EngineFunctions.h"
#include "internal/GameSDK.h"

#include "internal/globals.h"
#include "internal/CallTemplates.h"
#include "internal/Detours.h"

namespace OwnedCorpses
{
	static bool g_enabled = false;
	static bool g_installed = false;
	static Detours::JumpDetour g_detour;
	static Detours::JumpDetour g_stealAlarmDetour;

	//find best faction from dead actor's base form
	//prefers factions with reputation records (real social factions, not sniffer/utility)
	static TESFaction* GetBestFaction(TESObjectREFR* ref)
	{
		if (!ref || !ref->baseForm) return nullptr;

		auto* actorBase = static_cast<TESActorBase*>(ref->baseForm);
		auto* node = actorBase->baseData.factionList.Head();
		TESFaction* fallback = nullptr;

		while (node && node->item)
		{
			TESFaction* faction = node->item->faction;
			if (faction)
			{
				if (!fallback) fallback = faction;
				if (faction->reputation)
					return faction;
			}
			node = node->next;
		}

		return fallback;
	}

	//non-actor ownership fallback: encounter zone -> cell owner
	//same chain vanilla uses for items/containers, skipping teleport/furniture/door checks
	static void* GetZoneOrCellOwner(void* ref)
	{
		void* zone = ThisCall<void*>(0x567D20, ref); //GetEncounterZone
		if (zone)
		{
			void* noZone = CdeclCall<void*>(0x546A90); //GetNoZoneZone
			if (zone != noZone)
			{
				void* zoneOwner = ThisCall<void*>(0x9611E0, zone); //zone->GetOwner
				if (zoneOwner) return zoneOwner;
			}
		}

		void* cell = ThisCall<void*>(0x8D6F30, ref); //GetParentCell
		if (cell)
			return ThisCall<void*>(0x546A40, cell); //cell->GetOwner

		return nullptr;
	}

	//reimplementation of GetOwnerRawForm (0x567790) with dead actor ownership
	void* __fastcall GetOwnerRawFormHook(void* ref, void* edx)
	{
		if (!Engine::TESObjectREFR_IsActor(static_cast<TESObjectREFR*>(ref)))
			return g_detour.GetTrampoline<void*(__thiscall*)(void*)>()(ref);

		void* owner = ThisCall<void*>(0x567770, ref); //GetMyOwner (ExtraOwnership)
		if (!g_enabled || owner)
			return owner;

		if (!Engine::Actor_IsDead(static_cast<Actor*>(ref), false))
			return owner;

		//dead actor: zone/cell ownership first, then faction fallback
		void* zoneOwner = GetZoneOrCellOwner(ref);
		if (zoneOwner) return zoneOwner;

		return GetBestFaction(static_cast<TESObjectREFR*>(ref));
	}

	static bool __fastcall IsDeadForStealAlarm(Actor* actor, void*)
	{
		return Engine::Actor_IsDead(actor, false);
	}

	//StealAlarm witness hook
	//vanilla uses the dead body as witness (can't detect - no alarm)
	//skip dead actors so it falls through to faction-based witness search
	static UInt32 kStealAlarmUseWitness = 0x8BFC52;
	static UInt32 kStealAlarmFactionSearch = 0x8BFBBE;

	__declspec(naked) void StealAlarmWitnessHook()
	{
		__asm
		{
			mov edx, [ebp - 0x1C] //v32 (actor container ref)

			//check if our feature is enabled
			cmp g_enabled, 0
			je useAsWitness

			mov ecx, edx
			call IsDeadForStealAlarm
			test al, al
			jnz factionSearch //dead - find a real witness

		useAsWitness:
			mov edx, [ebp - 0x1C]
			mov [ebp - 0x10], edx //ActorRefInHigh = v32
			jmp kStealAlarmUseWitness

		factionSearch:
			jmp kStealAlarmFactionSearch
		}
	}

	void SetEnabled(bool enabled)
	{
		if (!g_installed) return;
		g_enabled = enabled;
	}

	void Init(bool enabled)
	{
		//GetOwnerRawForm prologue: push ebp; mov ebp,esp; sub esp,10h = 6 bytes
		if (!g_detour.WriteRelJump(0x567790, GetOwnerRawFormHook, 6))
		{
			Log("OwnedCorpses: failed to hook GetOwnerRawForm");
			return;
		}

		//StealAlarm witness selection: 11 bytes at 0x8BFBB3
		//original: mov edx,[ebp-1C]; mov [ebp-10],edx; jmp 8BFC52
		if (!g_stealAlarmDetour.WriteRelJump(0x8BFBB3, StealAlarmWitnessHook, 11))
		{
			Log("OwnedCorpses: failed to hook StealAlarm");
			g_detour.Remove();
			return;
		}

		g_installed = true;
		g_enabled = enabled;
	}
}

