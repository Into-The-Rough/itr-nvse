//dead actors inherit cell/zone ownership so looting corpses can be stealing

#include "OwnedCorpses.h"
#include "internal/NVSEMinimal.h"
#include "internal/EngineFunctions.h"
#include "internal/globals.h"
#include "internal/CallTemplates.h"
#include "internal/Detours.h"

namespace OwnedCorpses
{
	static bool g_enabled = false;
	static Detours::JumpDetour g_detour;
	static Detours::JumpDetour g_stealAlarmDetour;

	//find best faction from dead actor's base form
	//prefers factions with reputation records (real social factions, not sniffer/utility)
	static void* GetBestFaction(void* ref)
	{
		void* baseForm = *(void**)((char*)ref + 0x20);
		if (!baseForm) return nullptr;

		//tList<FactionListData> at TESActorBase+0x5C
		struct FactionEntry { void* faction; UInt8 rank; };
		struct FactionNode { FactionEntry* item; FactionNode* next; };

		auto* node = (FactionNode*)((char*)baseForm + 0x5C);
		void* fallback = nullptr;

		while (node && node->item)
		{
			void* faction = node->item->faction;
			if (faction)
			{
				if (!fallback) fallback = faction;
				//TESFaction::reputation at +0x38, non-null = real social faction
				if (*(void**)((char*)faction + 0x38))
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
		void* owner = ThisCall<void*>(0x567770, ref); //GetMyOwner (ExtraOwnership)

		bool isActor = ThisCall<bool>(*(UInt32*)(*(UInt32*)ref + 0x100), ref); //vtable+0x100
		if (isActor)
		{
			if (!g_enabled)
				return owner;

			bool isDead = ThisCall<bool>(*(UInt32*)(*(UInt32*)ref + 0x22C), ref, 0); //vtable+0x22C
			if (!isDead)
				return owner;

			if (owner)
				return owner;

			//dead actor: zone/cell ownership first, then faction fallback
			void* zoneOwner = GetZoneOrCellOwner(ref);
			if (zoneOwner) return zoneOwner;

			return GetBestFaction(ref);
		}

		//non-actor: full vanilla fallback chain via trampoline
		return g_detour.GetTrampoline<void*(__thiscall*)(void*)>()(ref);
	}

	//StealAlarm witness hook
	//vanilla uses the dead body as witness (can't detect → no alarm)
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

			//call IsDead(0) via vtable+0x22C
			push 0
			mov ecx, edx
			mov eax, [ecx]
			call [eax + 0x22C]
			test al, al
			jnz factionSearch //dead → find a real witness

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
			return;
		}

		g_enabled = enabled;
	}
}

