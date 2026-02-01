//prevents player from earning infamy when a companion kills a faction member

#include "CompanionNoInfamy.h"
#include <Windows.h>
#include <cstdint>

namespace CompanionNoInfamy
{
	static bool g_enabled = false;
	static bool g_initialized = false;

	//original bytes at hook sites (5 bytes each - call instruction)
	static uint8_t g_origBytesMurder[5] = {0};
	static uint8_t g_origBytesAttack[5] = {0};
	static uint8_t g_origBytesKill[5] = {0};

	static const uint32_t kAddr_HandleMajorCrimeFactionReputations = 0x8B7D20;
	static const uint32_t kAddr_HandleMinorCrimeFactionReputations = 0x8B7C00;
	static const uint32_t kAddr_MurderAlarmReputationCall = 0x8C0E6E;
	static const uint32_t kAddr_AttackAlarmReputationCall = 0x8C0930;
	static const uint32_t kAddr_ActorKillReputationCall = 0x89F3DF;

	void SafeWrite8(uint32_t addr, uint8_t val)
	{
		DWORD oldProtect;
		VirtualProtect((void*)addr, 1, PAGE_EXECUTE_READWRITE, &oldProtect);
		*(uint8_t*)addr = val;
		VirtualProtect((void*)addr, 1, oldProtect, &oldProtect);
	}

	void SafeWrite32(uint32_t addr, uint32_t val)
	{
		DWORD oldProtect;
		VirtualProtect((void*)addr, 4, PAGE_EXECUTE_READWRITE, &oldProtect);
		*(uint32_t*)addr = val;
		VirtualProtect((void*)addr, 4, oldProtect, &oldProtect);
	}

	void WriteRelCall(uint32_t src, uint32_t dst)
	{
		SafeWrite8(src, 0xE8);
		SafeWrite32(src + 1, dst - src - 5);
	}

	//MurderAlarm: checks IsTeammate at [ebp-0x15]
	__declspec(naked) void MurderAlarmReputationHook()
	{
		__asm
		{
			cmp byte ptr [ebp-0x15], 0
			jnz skipCall
			mov eax, kAddr_HandleMajorCrimeFactionReputations
			jmp eax
		skipCall:
			ret 8
		}
	}

	//AttackAlarm: checks if attacker at [ebp+8] is teammate
	//note: AttackAlarm early-exits if attacker != player, so this rarely fires
	__declspec(naked) void AttackAlarmReputationHook()
	{
		__asm
		{
			mov ecx, [ebp+8]
			mov eax, 0x8BE4B0  //Actor::IsTeammate
			call eax
			test al, al
			jnz skipAttack
			mov eax, kAddr_HandleMajorCrimeFactionReputations
			jmp eax
		skipAttack:
			ret 8
		}
	}

	//Actor::Kill: checks if attacker at [ebx+8] is player
	__declspec(naked) void ActorKillReputationHook()
	{
		__asm
		{
			mov eax, [ebx+8]
			cmp eax, dword ptr ds:[0x11DEA3C]  //PlayerCharacter::pSingleton
			jne skipKillReputation
			mov eax, kAddr_HandleMinorCrimeFactionReputations
			jmp eax
		skipKillReputation:
			ret 8
		}
	}

	void ApplyPatch()
	{
		WriteRelCall(kAddr_MurderAlarmReputationCall, (uint32_t)MurderAlarmReputationHook);
		WriteRelCall(kAddr_AttackAlarmReputationCall, (uint32_t)AttackAlarmReputationHook);
		WriteRelCall(kAddr_ActorKillReputationCall, (uint32_t)ActorKillReputationHook);
	}

	void RemovePatch()
	{
		for (int i = 0; i < 5; i++) {
			SafeWrite8(kAddr_MurderAlarmReputationCall + i, g_origBytesMurder[i]);
			SafeWrite8(kAddr_AttackAlarmReputationCall + i, g_origBytesAttack[i]);
			SafeWrite8(kAddr_ActorKillReputationCall + i, g_origBytesKill[i]);
		}
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
		memcpy(g_origBytesMurder, (void*)kAddr_MurderAlarmReputationCall, 5);
		memcpy(g_origBytesAttack, (void*)kAddr_AttackAlarmReputationCall, 5);
		memcpy(g_origBytesKill, (void*)kAddr_ActorKillReputationCall, 5);

		g_initialized = true;

		if (enabled)
		{
			ApplyPatch();
			g_enabled = true;
		}
	}
}

void CompanionNoInfamy_Init(bool enabled)
{
	CompanionNoInfamy::Init(enabled);
}

void CompanionNoInfamy_SetEnabled(bool enabled)
{
	CompanionNoInfamy::SetEnabled(enabled);
}
