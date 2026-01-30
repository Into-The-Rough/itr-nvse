//blocks NPCs from unlocking doors they don't own
//level 0 = vanilla, 1 = ownership required, 2 = key/lockpicks required

#include "NPCDoorUnlockBlock.h"
#include <Windows.h>
#include "common/ITypes.h"

namespace NPCDoorUnlockBlock
{
	static int g_blockLevel = 0;

	//addresses
	constexpr UInt32 kAddr_CanActorIgnoreLock = 0x518F00;      //TESObjectDOOR::CanActorIgnoreLock
	constexpr UInt32 kAddr_IsAnOwner = 0x5785E0;               //TESObjectREFR::IsAnOwner
	constexpr UInt32 kAddr_IsFollowing = 0x8842C0;             //Actor::IsFollowing
	constexpr UInt32 kAddr_PlayerSingleton = 0x11DEA3C;

	typedef bool (__cdecl* _CanActorIgnoreLock)(void* doorRef, void* actor, bool activate, bool movement);
	typedef bool (__thiscall* _IsAnOwner)(void* refr, void* actor, bool checkFaction);
	typedef bool (__thiscall* _IsFollowing)(void* actor);

	static _IsAnOwner IsAnOwner = (_IsAnOwner)kAddr_IsAnOwner;
	static _IsFollowing IsFollowing = (_IsFollowing)kAddr_IsFollowing;

	//trampoline to call original function
	static UInt8 g_trampoline[16];
	static _CanActorIgnoreLock OriginalFunc = nullptr;

	inline void* GetPlayer()
	{
		return *(void**)kAddr_PlayerSingleton;
	}

	bool __cdecl CanActorIgnoreLock_Hook(void* doorRef, void* actor, bool activate, bool movement)
	{
		if (g_blockLevel == 0)
			return OriginalFunc(doorRef, actor, activate, movement);

		//always allow player
		if (actor == GetPlayer())
			return OriginalFunc(doorRef, actor, activate, movement);

		//always allow companions (vanilla behavior)
		if (IsFollowing(actor))
			return OriginalFunc(doorRef, actor, activate, movement);

		//level 2: nobody can bypass locks, must use key/lockpicks
		if (g_blockLevel >= 2)
			return false;

		//level 1: only allow direct door ownership (not cell ownership, not guard status)
		if (IsAnOwner(doorRef, actor, true))
			return true;

		return false;
	}

	void PatchMemory(UInt32 addr, const void* data, UInt32 size)
	{
		DWORD oldProtect;
		VirtualProtect((void*)addr, size, PAGE_EXECUTE_READWRITE, &oldProtect);
		memcpy((void*)addr, data, size);
		VirtualProtect((void*)addr, size, oldProtect, &oldProtect);
	}

	void SetLevel(int level)
	{
		g_blockLevel = level;
	}

	void Init()
	{
		//build trampoline: original prologue + jmp back
		//original: push ebp; mov ebp, esp (5 bytes)
		DWORD oldProtect;
		VirtualProtect(g_trampoline, sizeof(g_trampoline), PAGE_EXECUTE_READWRITE, &oldProtect);

		g_trampoline[0] = 0x55;                    //push ebp
		g_trampoline[1] = 0x8B;                    //mov ebp, esp
		g_trampoline[2] = 0xEC;
		g_trampoline[3] = 0xE9;                    //jmp
		UInt32 jmpBack = (kAddr_CanActorIgnoreLock + 5) - ((UInt32)&g_trampoline[3] + 5);
		*(UInt32*)&g_trampoline[4] = jmpBack;

		OriginalFunc = (_CanActorIgnoreLock)(void*)g_trampoline;

		//patch function start with jmp to our hook
		UInt8 patch[5];
		patch[0] = 0xE9;  //jmp
		UInt32 hookOffset = (UInt32)CanActorIgnoreLock_Hook - (kAddr_CanActorIgnoreLock + 5);
		*(UInt32*)&patch[1] = hookOffset;

		PatchMemory(kAddr_CanActorIgnoreLock, patch, 5);
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
