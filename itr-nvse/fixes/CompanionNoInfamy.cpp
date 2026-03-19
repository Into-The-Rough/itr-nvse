//prevents player from earning infamy when a companion kills a faction member

#include "CompanionNoInfamy.h"
#include "internal/NVSEMinimal.h"
#include <cstring>

namespace CompanionNoInfamy
{
	static bool g_enabled = false;
	static bool g_initialized = false;

	//original bytes at hook sites (5 bytes each - call instruction)
	static UInt8 g_origBytesMurder[5] = {0};
	static UInt8 g_origBytesAttack[5] = {0};
	static UInt8 g_origBytesKill[5] = {0};

	static const UInt32 kAddr_HandleMajorCrimeFactionReputations = 0x8B7D20;
	static const UInt32 kAddr_HandleMinorCrimeFactionReputations = 0x8B7C00;
	static const UInt32 kAddr_MurderAlarmReputationCall = 0x8C0E6E;
	static const UInt32 kAddr_AttackAlarmReputationCall = 0x8C0930;
	static const UInt32 kAddr_ActorKillReputationCall = 0x89F3DF;

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

	//AttackAlarm: only apply rep hit if attacker is the player
	//[ebp+8] compared as integer, no dereference - safe even if stack frame is wrong
	__declspec(naked) void AttackAlarmReputationHook()
	{
		__asm
		{
			mov eax, [ebp+8]
			cmp eax, dword ptr ds:[0x11DEA3C]  //PlayerCharacter::pSingleton
			jne skipAttack
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
		SafeWrite::WriteRelCall(kAddr_MurderAlarmReputationCall, (UInt32)MurderAlarmReputationHook);
		SafeWrite::WriteRelCall(kAddr_AttackAlarmReputationCall, (UInt32)AttackAlarmReputationHook);
		SafeWrite::WriteRelCall(kAddr_ActorKillReputationCall, (UInt32)ActorKillReputationHook);
	}

	void RemovePatch()
	{
		for (int i = 0; i < 5; i++) {
			SafeWrite::Write8(kAddr_MurderAlarmReputationCall + i, g_origBytesMurder[i]);
			SafeWrite::Write8(kAddr_AttackAlarmReputationCall + i, g_origBytesAttack[i]);
			SafeWrite::Write8(kAddr_ActorKillReputationCall + i, g_origBytesKill[i]);
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

