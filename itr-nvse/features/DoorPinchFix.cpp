#include "DoorPinchFix.h"

#include "internal/Detours.h"
#include "internal/EngineFunctions.h"
#include "internal/GameGlobals.h"
#include "nvse/GameForms.h"
#include "nvse/GameObjects.h"

#include <algorithm>
#include <vector>

namespace
{
	enum OpenState : UInt32
	{
		kOpenState_None = 0,
		kOpenState_Open = 1,
		kOpenState_Opening = 2,
		kOpenState_Closed = 3,
		kOpenState_Closing = 4,
	};

	struct DisabledDoor
	{
		UInt32 refID;
		DWORD restoreAt;
	};

	using HandleActivate_t = void(__cdecl*)(TESObjectREFR* itemActivated, TESObjectREFR* actionRef, void* openCloseForm);
	using GetOpenState_t = UInt32(__cdecl*)(TESObjectREFR* ref);
	using GetTeleport_t = void*(__thiscall*)(TESObjectREFR* ref);
	using GetSlidingDoor_t = bool(__thiscall*)(TESForm* doorBase);

	Detours::CallDetour g_handleActivateDetour;
	HandleActivate_t g_originalHandleActivate = reinterpret_cast<HandleActivate_t>(0x47A560);
	GetOpenState_t GetOpenState = reinterpret_cast<GetOpenState_t>(0x47B250);
	GetTeleport_t GetTeleport = reinterpret_cast<GetTeleport_t>(0x568E50);
	GetSlidingDoor_t GetSlidingDoor = reinterpret_cast<GetSlidingDoor_t>(0x518080);

	bool g_enabled = false;
	bool g_hookInstalled = false;
	int g_distance = 140;
	int g_timeoutMs = 8000;
	std::vector<DisabledDoor> g_disabledDoors;

	float Distance2DSq(TESObjectREFR* a, TESObjectREFR* b)
	{
		const float dx = a->posX - b->posX;
		const float dy = a->posY - b->posY;
		return dx * dx + dy * dy;
	}

	bool IsDoorRef(TESObjectREFR* ref)
	{
		return ref && ref->baseForm && ref->baseForm->typeID == kFormType_Door;
	}

	bool IsTracked(UInt32 refID)
	{
		return std::any_of(g_disabledDoors.begin(), g_disabledDoors.end(),
			[refID](const DisabledDoor& door) { return door.refID == refID; });
	}

	bool IsDoorMoving(UInt32 openState)
	{
		return openState == kOpenState_Opening || openState == kOpenState_Closing;
	}

	bool IsCollisionDisabled(TESObjectREFR* ref)
	{
		return ref && Engine::TESForm_GetNoCollision(ref);
	}

	void SetCollisionEnabled(TESObjectREFR* ref, bool enabled)
	{
		if (!ref)
			return;

		const bool noCollision = !enabled;
		if (Engine::TESForm_GetNoCollision(ref) != noCollision)
			Engine::TESForm_SetNoCollision(ref, noCollision);
	}

	void TrackDoor(TESObjectREFR* ref)
	{
		const UInt32 refID = ref ? ref->refID : 0;
		if (!refID || IsTracked(refID))
			return;

		g_disabledDoors.push_back({refID, GetTickCount() + static_cast<DWORD>(g_timeoutMs)});
	}

	void RestoreDoor(UInt32 refID)
	{
		auto* ref = reinterpret_cast<TESObjectREFR*>(Engine::LookupFormByID(refID));
		if (IsDoorRef(ref))
			SetCollisionEnabled(ref, true);
	}

	void RestoreAll()
	{
		for (const auto& door : g_disabledDoors)
			RestoreDoor(door.refID);
		g_disabledDoors.clear();
	}

	bool ShouldDisableDoor(TESObjectREFR* doorRef)
	{
		if (!g_enabled || !IsDoorRef(doorRef))
			return false;
		if (GetTeleport(doorRef))
			return false;
		if (GetSlidingDoor(doorRef->baseForm))
			return false;

		PlayerCharacter* player = *g_thePlayerPtr;
		if (!player || !player->parentCell || doorRef->parentCell != player->parentCell)
			return false;

		const float maxDistanceSq = static_cast<float>(g_distance * g_distance);
		return Distance2DSq(player, doorRef) <= maxDistanceSq;
	}

	void __cdecl Hook_HandleActivate(TESObjectREFR* itemActivated, TESObjectREFR* actionRef, void* openCloseForm)
	{
		g_originalHandleActivate(itemActivated, actionRef, openCloseForm);

		if (!ShouldDisableDoor(itemActivated))
			return;
		if (!IsDoorMoving(GetOpenState(itemActivated)))
			return;
		if (IsTracked(itemActivated->refID) || IsCollisionDisabled(itemActivated))
			return;

		SetCollisionEnabled(itemActivated, false);
		TrackDoor(itemActivated);
	}
}

namespace DoorPinchFix
{
	void Init(bool enabled, int distance, int timeoutMs)
	{
		if (!g_hookInstalled)
		{
			if (g_handleActivateDetour.WriteRelCall(0x518B40, Hook_HandleActivate))
			{
				g_originalHandleActivate = reinterpret_cast<HandleActivate_t>(g_handleActivateDetour.GetOverwrittenAddr());
				g_hookInstalled = true;
			}
		}

		UpdateSettings(enabled, distance, timeoutMs);
	}

	void UpdateSettings(bool enabled, int distance, int timeoutMs)
	{
		g_distance = (std::max)(distance, 32);
		g_timeoutMs = (std::max)(timeoutMs, 1000);

		if (g_enabled && !enabled)
			RestoreAll();

		g_enabled = enabled;
	}

	void Update()
	{
		if (g_disabledDoors.empty())
			return;

		const DWORD now = GetTickCount();
		for (auto it = g_disabledDoors.begin(); it != g_disabledDoors.end();)
		{
			auto* ref = reinterpret_cast<TESObjectREFR*>(Engine::LookupFormByID(it->refID));
			const bool shouldRestore = !g_enabled ||
				!IsDoorRef(ref) ||
				!IsDoorMoving(GetOpenState(ref)) ||
				static_cast<SInt32>(now - it->restoreAt) >= 0;

			if (!shouldRestore)
			{
				SetCollisionEnabled(ref, false);
				++it;
				continue;
			}

			if (IsDoorRef(ref))
				SetCollisionEnabled(ref, true);
			it = g_disabledDoors.erase(it);
		}
	}

	void ClearState()
	{
		RestoreAll();
	}
}
