//blocks NPCs from unlocking doors they don't own
//level 0 = vanilla, 1 = ownership required, 2 = key/lockpicks required

#include "NPCDoorUnlockBlock.h"
#include <Windows.h>
#include "common/ITypes.h"
#include "internal/Detours.h"

#include "internal/globals.h"

namespace NPCDoorUnlockBlock
{
	static int g_blockLevel = 0;

	typedef bool (__cdecl* _CanActorIgnoreLock)(void* doorRef, void* actor, bool activate, bool movement);
	typedef bool (__thiscall* _IsAnOwner)(void* refr, void* actor, bool checkFaction);
	typedef bool (__thiscall* _IsFollowing)(void* actor);

	static _IsAnOwner IsAnOwner = (_IsAnOwner)0x5785E0;
	static _IsFollowing IsFollowing = (_IsFollowing)0x8842C0;

	static Detours::JumpDetour s_detour;

	inline void* GetPlayer()
	{
		return *(void**)0x11DEA3C;
	}

	bool __cdecl CanActorIgnoreLock_Hook(void* doorRef, void* actor, bool activate, bool movement)
	{
		if (g_blockLevel == 0)
			return s_detour.GetTrampoline<_CanActorIgnoreLock>()(doorRef, actor, activate, movement);

		//always allow player
		if (actor == GetPlayer())
			return s_detour.GetTrampoline<_CanActorIgnoreLock>()(doorRef, actor, activate, movement);

		//always allow companions (vanilla behavior)
		if (IsFollowing(actor))
			return s_detour.GetTrampoline<_CanActorIgnoreLock>()(doorRef, actor, activate, movement);

		//level 2: nobody can bypass locks, must use key/lockpicks
		if (g_blockLevel >= 2)
			return false;

		//level 1: only allow direct door ownership (not cell ownership, not guard status)
		if (IsAnOwner(doorRef, actor, true))
			return true;

		return false;
	}

	void SetLevel(int level)
	{
		g_blockLevel = level;
	}

	//prologue: push ebp; mov ebp, esp = 5 bytes
	void Init()
	{
		if (!s_detour.WriteRelJump(0x518F00, CanActorIgnoreLock_Hook, 5)) //CanActorIgnoreLock
			Log("ERROR: NPCDoorUnlockBlock hook failed");
	}
}

void NPCDoorUnlockBlock_Init(int level)
{
	NPCDoorUnlockBlock::Init();
	NPCDoorUnlockBlock::SetLevel(level);
}

void NPCDoorUnlockBlock_SetLevel(int level)
{
	NPCDoorUnlockBlock::SetLevel(level);
}
