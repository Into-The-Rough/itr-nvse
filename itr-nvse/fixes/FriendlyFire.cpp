//allows NPCs to hurt each other with non-explosive weapons

#include "FriendlyFire.h"
#include <Windows.h>
#include <cstdint>

#include "internal/globals.h"
#include "internal/SafeWrite.h"

namespace FriendlyFire
{
	static bool g_enabled = false;
	static bool g_initialized = false;

	//original bytes at 0x9C314E (5 bytes)
	static uint8_t g_origBytes1[5] = {0};
	//original byte at 0x899D5A (1 byte)
	static uint8_t g_origByte2 = 0;

	void ApplyPatch()
	{
		//bypass IsInCombatWithActor in Projectile::9C30D0
		//mov al, 1; nop x3
		SafeWrite::Write8(0x9C314E, 0xB0);
		SafeWrite::Write8(0x9C314F, 0x01);
		SafeWrite::Write8(0x9C3150, 0x90);
		SafeWrite::Write8(0x9C3151, 0x90);
		SafeWrite::Write8(0x9C3152, 0x90);

		//bypass IsActoraCombatTarget in Actor::899CB0
		SafeWrite::Write8(0x899D5A, 0xEB);
	}

	void RemovePatch()
	{
		//restore original bytes at 0x9C314E
		for (int i = 0; i < 5; i++)
			SafeWrite::Write8(0x9C314E + i, g_origBytes1[i]);

		//restore original byte at 0x899D5A
		SafeWrite::Write8(0x899D5A, g_origByte2);
	}

	void SetEnabled(bool enabled)
	{
		if (!g_initialized) return;
		if (enabled == g_enabled) return;

		if (enabled)
			ApplyPatch();
		else
			RemovePatch();

		g_enabled = enabled;
	}

	void Init(bool enabled)
	{
		//save original bytes before any patching
		memcpy(g_origBytes1, (void*)0x9C314E, 5);
		g_origByte2 = *(uint8_t*)0x899D5A;

		g_initialized = true;

		if (enabled)
		{
			ApplyPatch();
			g_enabled = true;
		}

	}
}

