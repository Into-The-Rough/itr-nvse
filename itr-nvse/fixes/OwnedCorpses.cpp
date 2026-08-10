//dead actors inherit cell/zone ownership so looting corpses can be stealing

#include "OwnedCorpses.h"
#include "internal/EngineFunctions.h"
#include "internal/GameSDK.h"

#include "internal/globals.h"
#include "internal/CallTemplates.h"
#include "internal/Detours.h"

namespace OwnedCorpses
{
	constexpr UInt32 kAddr_GetOwnerRawForm = 0x567790;

	static bool g_enabled = false;
	static bool g_installed = false;
	static Detours::CallDetour g_crimeOwnerCall;
	static Detours::CallDetour g_alarmOwnerCall;
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

	//dead actors return before vanilla's encounter-zone and cell-owner fallback
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

	static void* GetCorpseOwner(void* ref, void* vanillaOwner)
	{
		if (vanillaOwner || !g_enabled)
			return vanillaOwner;

		if (!Engine::TESObjectREFR_IsActor(static_cast<TESObjectREFR*>(ref)))
			return nullptr;

		if (!Engine::Actor_IsDead(static_cast<Actor*>(ref), false))
			return nullptr;

		//dead actor: zone/cell ownership first, then faction fallback
		void* zoneOwner = GetZoneOrCellOwner(ref);
		if (zoneOwner) return zoneOwner;

		return GetBestFaction(static_cast<TESObjectREFR*>(ref));
	}

	typedef void* (__thiscall* GetOwnerRawForm_t)(void*);

	static void* __fastcall GetCrimeOwnerHook(void* ref, void*)
	{
		auto original = reinterpret_cast<GetOwnerRawForm_t>(g_crimeOwnerCall.GetOverwrittenAddr());
		return GetCorpseOwner(ref, original(ref));
	}

	static void* __fastcall GetAlarmOwnerHook(void* ref, void*)
	{
		auto original = reinterpret_cast<GetOwnerRawForm_t>(g_alarmOwnerCall.GetOverwrittenAddr());
		return GetCorpseOwner(ref, original(ref));
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
		//the earlier owner calls are base-form-gated and cannot receive actor corpses
		if (!g_crimeOwnerCall.WriteRelCall(0x579A92, GetCrimeOwnerHook))
		{
			Log("OwnedCorpses: activation owner call could not be detoured");
			return;
		}
		UInt32 crimeOriginal = g_crimeOwnerCall.GetOverwrittenAddr();
		Log("OwnedCorpses: %08X hooked, original=%08X vanilla=%08X", 0x579A92, crimeOriginal, kAddr_GetOwnerRawForm);

		//steal alarm resolves the actor-container owner here before witness selection
		if (!g_alarmOwnerCall.WriteRelCall(0x8BFB2C, GetAlarmOwnerHook))
		{
			Log("OwnedCorpses: alarm owner call could not be detoured");
			g_crimeOwnerCall.Remove();
			return;
		}
		UInt32 alarmOriginal = g_alarmOwnerCall.GetOverwrittenAddr();
		Log("OwnedCorpses: %08X hooked, original=%08X vanilla=%08X", 0x8BFB2C, alarmOriginal, kAddr_GetOwnerRawForm);

		//StealAlarm witness selection: 11 bytes at 0x8BFBB3
		//original: mov edx,[ebp-1C]; mov [ebp-10],edx; jmp 8BFC52
		if (!g_stealAlarmDetour.WriteRelJump(0x8BFBB3, StealAlarmWitnessHook, 11))
		{
			Log("OwnedCorpses: failed to hook StealAlarm");
			g_alarmOwnerCall.Remove();
			g_crimeOwnerCall.Remove();
			return;
		}

		g_installed = true;
		g_enabled = enabled;
	}
}
