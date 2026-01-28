//when an NPC's weapon clip empties, switch to another ranged weapon instead of reloading
//uses game's CombatProcedureSwitchWeapon to properly integrate with combat AI
//after switch completes, silently fills clip via Actor::Reload(a3=0) so NPC fires immediately

#include "SwitchWeaponOnEmpty.h"
#include "nvse/GameForms.h"
#include "nvse/GameObjects.h"
#include "nvse/GameExtraData.h"
#include <Windows.h>

extern void Log(const char* fmt, ...);

namespace SwitchWeaponOnEmpty
{
	constexpr UInt32 kAddr_ReloadAlt = 0x8A83C0;
	constexpr UInt32 kAddr_GetEquippedWeapon = 0x8A1710;
	constexpr UInt32 kAddr_Reload = 0x8A8420;

	//combat procedure system
	constexpr UInt32 kAddr_GetCombatController = 0x8A02D0;
	constexpr UInt32 kAddr_CombatProcSwitchWeapon_Ctor = 0x9DA720;
	constexpr UInt32 kAddr_CombatController_SetActionProcedure = 0x980110;
	constexpr UInt32 kAddr_GameHeapAlloc = 0xAA3E40;
	constexpr UInt32 kAddr_GameHeap = 0x11F6238;
	constexpr UInt32 kSizeCombatProcSwitchWeapon = 0x18;

	typedef TESObjectWEAP* (__thiscall *GetEquippedWeapon_t)(Actor*);
	typedef void (__thiscall *ReloadAlt_t)(Actor*, TESObjectWEAP*, UInt32, bool, int);
	typedef char (__thiscall *Reload_t)(Actor*, TESObjectWEAP*, UInt32, bool);
	typedef void* (__thiscall *GetCombatController_t)(Actor*);
	typedef void* (__thiscall *CombatProcSwitchWeapon_Ctor_t)(void*, TESObjectWEAP*);
	typedef void (__thiscall *CombatController_SetActionProcedure_t)(void*, void*);

	#define GameHeapAlloc(size) ((void*(__thiscall*)(void*, UInt32))(kAddr_GameHeapAlloc))((void*)kAddr_GameHeap, size)

	static UInt8 g_trampoline[32];
	static ReloadAlt_t Original = nullptr;

	//track recent switches to prevent rapid ping-pong
	struct RecentSwitch {
		UInt32 actorRefID;
		UInt32 fromWeaponRefID;
		UInt32 toWeaponRefID;
		int frameCount;
	};
	static const int kMaxRecent = 32;
	static RecentSwitch g_recent[kMaxRecent] = {0};
	static int g_recentCount = 0;

	//pending clip fills - after weapon switch completes, silently fill the clip
	struct PendingFill {
		Actor* actor;
		TESObjectWEAP* targetWeapon;
		int framesLeft;
	};
	static const int kMaxPending = 32;
	static PendingFill g_pending[kMaxPending] = {0};
	static int g_pendingCount = 0;

	static bool WasSwitchRecent(UInt32 actorRefID, UInt32 fromWeapon, UInt32 toWeapon)
	{
		for (int i = 0; i < g_recentCount; i++)
		{
			if (g_recent[i].actorRefID == actorRefID)
			{
				if (toWeapon == g_recent[i].fromWeaponRefID)
				{
					Log("  WasSwitchRecent: blocked reverse switch %08X -> %08X", fromWeapon, toWeapon);
					return true;
				}
			}
		}
		return false;
	}

	static void AddRecentSwitch(UInt32 actorRefID, UInt32 fromWeapon, UInt32 toWeapon)
	{
		for (int i = 0; i < g_recentCount; i++)
		{
			if (g_recent[i].actorRefID == actorRefID)
			{
				g_recent[i].fromWeaponRefID = fromWeapon;
				g_recent[i].toWeaponRefID = toWeapon;
				g_recent[i].frameCount = 0;
				return;
			}
		}
		if (g_recentCount < kMaxRecent)
		{
			g_recent[g_recentCount++] = {actorRefID, fromWeapon, toWeapon, 0};
		}
	}

	static void CleanupRecentSwitches()
	{
		for (int i = g_recentCount - 1; i >= 0; i--)
		{
			g_recent[i].frameCount++;
			if (g_recent[i].frameCount > 60) //~1 second
			{
				g_recent[i] = g_recent[--g_recentCount];
				g_recent[g_recentCount] = {0};
			}
		}
	}

	static bool HasPendingFill(Actor* actor)
	{
		for (int i = 0; i < g_pendingCount; i++)
			if (g_pending[i].actor == actor)
				return true;
		return false;
	}

	static void ClearRecentSwitch(UInt32 actorRefID)
	{
		for (int i = 0; i < g_recentCount; i++)
		{
			if (g_recent[i].actorRefID == actorRefID)
			{
				g_recent[i] = g_recent[--g_recentCount];
				g_recent[g_recentCount] = {0};
				return;
			}
		}
	}

	static void AddPendingFill(Actor* actor, TESObjectWEAP* weapon)
	{
		//replace existing for same actor
		for (int i = 0; i < g_pendingCount; i++)
		{
			if (g_pending[i].actor == actor)
			{
				g_pending[i].targetWeapon = weapon;
				g_pending[i].framesLeft = 600;
				return;
			}
		}
		if (g_pendingCount < kMaxPending)
			g_pending[g_pendingCount++] = {actor, weapon, 600};
	}

	static void ProcessPendingFills()
	{
		GetEquippedWeapon_t getEquipped = (GetEquippedWeapon_t)kAddr_GetEquippedWeapon;
		Reload_t reload = (Reload_t)kAddr_Reload;

		for (int i = g_pendingCount - 1; i >= 0; i--)
		{
			PendingFill& pf = g_pending[i];
			if (!pf.actor || !pf.targetWeapon)
			{
				pf = g_pending[--g_pendingCount];
				continue;
			}

			TESObjectWEAP* equipped = getEquipped(pf.actor);
			if (equipped == pf.targetWeapon)
			{
				//weapon switch completed - silently fill clip (a3=0 = no animation)
				char result = reload(pf.actor, pf.targetWeapon, 0, false);
				Log("  PendingFill: actor %08X weapon %08X clip filled (result=%d)", pf.actor->refID, pf.targetWeapon->refID, result);
				ClearRecentSwitch(pf.actor->refID);
				pf = g_pending[--g_pendingCount];
				continue;
			}

			if (pf.framesLeft % 60 == 0)
			{
				TESObjectWEAP* cur = getEquipped(pf.actor);
				Log("  PendingFill: actor %08X waiting for %08X (currently %08X, %d frames left)",
					pf.actor->refID, pf.targetWeapon->refID, cur ? cur->refID : 0, pf.framesLeft);
			}

			if (--pf.framesLeft <= 0)
			{
				Log("  PendingFill: actor %08X timed out waiting for weapon %08X", pf.actor->refID, pf.targetWeapon->refID);
				pf = g_pending[--g_pendingCount];
			}
		}
	}

	static BSExtraData* GetExtraDataByType(BaseExtraList* list, UInt32 type)
	{
		if (!list) return nullptr;
		UInt32 index = (type >> 3);
		UInt8 bitMask = 1 << (type % 8);
		if (!(list->m_presenceBitfield[index] & bitMask))
			return nullptr;
		for (BSExtraData* traverse = list->m_data; traverse; traverse = traverse->next)
			if (traverse->type == type)
				return traverse;
		return nullptr;
	}

	bool IsRangedGun(TESObjectWEAP* weapon)
	{
		if (!weapon) return false;
		UInt8 type = weapon->eWeaponType;
		return type >= TESObjectWEAP::kWeapType_OneHandPistol && type <= TESObjectWEAP::kWeapType_TwoHandLauncher;
	}

	bool IsPlayer(Actor* actor)
	{
		return actor->refID == 0x14;
	}

	bool HasAmmoForWeapon(Actor* actor, TESObjectWEAP* weapon)
	{
		if (!weapon) return false;
		TESForm* ammoForm = weapon->ammo.ammo;
		if (!ammoForm) return true;

		ExtraContainerChanges* xChanges = (ExtraContainerChanges*)GetExtraDataByType(&actor->extraDataList, kExtraData_ContainerChanges);
		if (!xChanges || !xChanges->data || !xChanges->data->objList) return false;

		int iterCount = 0;
		for (auto iter = xChanges->data->objList->Begin(); !iter.End(); ++iter)
		{
			if (++iterCount > 1000) return false;
			ExtraContainerChanges::EntryData* entry = iter.Get();
			if (!entry || !entry->type) continue;

			if (entry->type == ammoForm && entry->countDelta > 0)
				return true;

			if (ammoForm->typeID == kFormType_ListForm)
			{
				BGSListForm* ammoList = (BGSListForm*)ammoForm;
				int listIterCount = 0;
				for (auto listIter = ammoList->list.Begin(); !listIter.End(); ++listIter)
				{
					if (++listIterCount > 1000) return false;
					if (listIter.Get() == entry->type && entry->countDelta > 0)
						return true;
				}
			}
		}
		return false;
	}

	TESObjectWEAP* FindWeaponWithAmmo(Actor* actor, TESObjectWEAP* excludeWeapon)
	{
		ExtraContainerChanges* xChanges = (ExtraContainerChanges*)GetExtraDataByType(&actor->extraDataList, kExtraData_ContainerChanges);
		if (!xChanges || !xChanges->data || !xChanges->data->objList) return nullptr;

		int iterCount = 0;
		for (auto iter = xChanges->data->objList->Begin(); !iter.End(); ++iter)
		{
			if (++iterCount > 1000) return nullptr;
			ExtraContainerChanges::EntryData* entry = iter.Get();
			if (!entry || !entry->type) continue;
			if (entry->type->typeID != kFormType_Weapon) continue;

			TESObjectWEAP* weapon = (TESObjectWEAP*)entry->type;
			if (weapon == excludeWeapon) continue;
			if (!IsRangedGun(weapon)) continue;

			if (HasAmmoForWeapon(actor, weapon))
			{
				Log("  FindWeaponWithAmmo: found %08X", weapon->refID);
				return weapon;
			}
		}
		return nullptr;
	}

	bool QueueWeaponSwitch(Actor* actor, TESObjectWEAP* targetWeapon)
	{
		GetCombatController_t getCombatCtrl = (GetCombatController_t)kAddr_GetCombatController;
		void* combatCtrl = getCombatCtrl(actor);
		if (!combatCtrl)
		{
			Log("  QueueWeaponSwitch: no combat controller");
			return false;
		}

		void* proc = GameHeapAlloc(kSizeCombatProcSwitchWeapon);
		if (!proc)
		{
			Log("  QueueWeaponSwitch: allocation failed");
			return false;
		}

		CombatProcSwitchWeapon_Ctor_t ctor = (CombatProcSwitchWeapon_Ctor_t)kAddr_CombatProcSwitchWeapon_Ctor;
		ctor(proc, targetWeapon);

		CombatController_SetActionProcedure_t setActionProc = (CombatController_SetActionProcedure_t)kAddr_CombatController_SetActionProcedure;
		setActionProc(combatCtrl, proc);

		Log("  QueueWeaponSwitch: procedure %p for weapon %08X", proc, targetWeapon->refID);
		return true;
	}

	void __fastcall Hook_ReloadAlt(Actor* actor, void* edx, TESObjectWEAP* weapon, UInt32 a3, bool a4, int a5)
	{
		if (!actor || !weapon)
		{
			Original(actor, weapon, a3, a4, a5);
			return;
		}

		if (IsPlayer(actor) || !IsRangedGun(weapon))
		{
			Original(actor, weapon, a3, a4, a5);
			return;
		}

		//only in combat
		UInt32 vtbl = *(UInt32*)actor;
		typedef bool (__thiscall *IsInCombat_t)(Actor*);
		IsInCombat_t isInCombatFn = (IsInCombat_t)(*(UInt32*)(vtbl + 0x10A * 4));
		if (!isInCombatFn(actor))
		{
			Original(actor, weapon, a3, a4, a5);
			return;
		}

		Log("SwitchWeaponOnEmpty: actor %08X weapon %08X clip empty", actor->refID, weapon->refID);

		//switch already in progress - suppress reload, PendingFill will handle clip
		if (HasPendingFill(actor))
		{
			Log("  -> suppressed reload, pending fill active");
			return;
		}

		TESObjectWEAP* altWeapon = FindWeaponWithAmmo(actor, weapon);
		if (!altWeapon)
		{
			Log("  -> no alt weapon, reloading");
			Original(actor, weapon, a3, a4, a5);
			return;
		}

		if (WasSwitchRecent(actor->refID, weapon->refID, altWeapon->refID))
		{
			Log("  -> blocked ping-pong, reloading");
			Original(actor, weapon, a3, a4, a5);
			return;
		}

		Log("  -> switching to %08X", altWeapon->refID);

		if (QueueWeaponSwitch(actor, altWeapon))
		{
			AddRecentSwitch(actor->refID, weapon->refID, altWeapon->refID);
			AddPendingFill(actor, altWeapon);
			return;
		}

		Log("  -> switch failed, reloading");
		Original(actor, weapon, a3, a4, a5);
	}

	void Update()
	{
		CleanupRecentSwitches();
		ProcessPendingFills();
	}

	void Init()
	{
		DWORD oldProtect;
		VirtualProtect(g_trampoline, sizeof(g_trampoline), PAGE_EXECUTE_READWRITE, &oldProtect);
		memcpy(g_trampoline, (void*)kAddr_ReloadAlt, 7);
		g_trampoline[7] = 0xE9;
		*(UInt32*)(g_trampoline + 8) = (kAddr_ReloadAlt + 7) - (UInt32)(g_trampoline + 7) - 5;
		Original = (ReloadAlt_t)(void*)g_trampoline;

		VirtualProtect((void*)kAddr_ReloadAlt, 7, PAGE_EXECUTE_READWRITE, &oldProtect);
		*(UInt8*)kAddr_ReloadAlt = 0xE9;
		*(UInt32*)(kAddr_ReloadAlt + 1) = (UInt32)Hook_ReloadAlt - kAddr_ReloadAlt - 5;
		*(UInt8*)(kAddr_ReloadAlt + 5) = 0x90;
		*(UInt8*)(kAddr_ReloadAlt + 6) = 0x90;
		VirtualProtect((void*)kAddr_ReloadAlt, 7, oldProtect, &oldProtect);

		Log("SwitchWeaponOnEmpty: hooked ReloadAlt at 0x%X", kAddr_ReloadAlt);
	}
}

void SwitchWeaponOnEmpty_Init()
{
	SwitchWeaponOnEmpty::Init();
}

void SwitchWeaponOnEmpty_Update()
{
	SwitchWeaponOnEmpty::Update();
}
