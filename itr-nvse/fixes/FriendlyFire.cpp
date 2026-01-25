//allows friendly fire - player can damage allies, allies can damage player

#include "FriendlyFire.h"
#include <Windows.h>
#include <cstdint>

extern void Log(const char* fmt, ...);

namespace FriendlyFire
{
	void SafeWrite8(uint32_t addr, uint8_t val)
	{
		DWORD oldProtect;
		VirtualProtect((void*)addr, 1, PAGE_EXECUTE_READWRITE, &oldProtect);
		*(uint8_t*)addr = val;
		VirtualProtect((void*)addr, 1, oldProtect, &oldProtect);
	}

	void Init()
	{
		//bypass IsInCombatWithActor in Projectile::9C30D0
		//mov al, 1; nop x3
		SafeWrite8(0x9C314E, 0xB0);
		SafeWrite8(0x9C314F, 0x01);
		SafeWrite8(0x9C3150, 0x90);
		SafeWrite8(0x9C3151, 0x90);
		SafeWrite8(0x9C3152, 0x90);

		//bypass IsActoraCombatTarget in Actor::899CB0
		SafeWrite8(0x899D5A, 0xEB);

		Log("FriendlyFire installed");
	}
}

void FriendlyFire_Init()
{
	FriendlyFire::Init();
}
