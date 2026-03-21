//prevents player from earning infamy when a companion kills a faction member

#include "CompanionNoInfamy.h"
#include "internal/NVSEMinimal.h"
#include "internal/CallTemplates.h"
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
	static const UInt32 kAddr_PlayerSingleton = 0x11DEA3C;

	static void __fastcall Hook_MurderAlarmReputation(Actor* actor, UInt32 isTeammate, UInt32 a2, UInt32 a3)
	{
		if (isTeammate)
			return;

		ThisCall<void>(kAddr_HandleMajorCrimeFactionReputations, actor, a2, static_cast<char>(a3));
	}

	__declspec(naked) static void MurderAlarmReputationHook_Wrapper()
	{
		__asm
		{
			movzx edx, byte ptr [ebp-0x15]
			jmp Hook_MurderAlarmReputation
		}
	}

	static void __fastcall Hook_AttackAlarmReputation(Actor* actor, Actor* attacker, UInt32 a2, UInt32 a3)
	{
		if (attacker != *(Actor**)kAddr_PlayerSingleton)
			return;

		ThisCall<void>(kAddr_HandleMajorCrimeFactionReputations, actor, a2, static_cast<char>(a3));
	}

	__declspec(naked) static void AttackAlarmReputationHook_Wrapper()
	{
		__asm
		{
			mov edx, [ebp+8]
			jmp Hook_AttackAlarmReputation
		}
	}

	static void __fastcall Hook_ActorKillReputation(Actor* actor, Actor* attacker, UInt32 a2, UInt32 a3)
	{
		if (attacker != *(Actor**)kAddr_PlayerSingleton)
			return;

		ThisCall<void>(kAddr_HandleMinorCrimeFactionReputations, actor, a2, static_cast<bool>(a3));
	}

	__declspec(naked) static void ActorKillReputationHook_Wrapper()
	{
		__asm
		{
			mov edx, [ebx+8]
			jmp Hook_ActorKillReputation
		}
	}

	void ApplyPatch()
	{
		SafeWrite::WriteRelCall(kAddr_MurderAlarmReputationCall, (UInt32)MurderAlarmReputationHook_Wrapper);
		SafeWrite::WriteRelCall(kAddr_AttackAlarmReputationCall, (UInt32)AttackAlarmReputationHook_Wrapper);
		SafeWrite::WriteRelCall(kAddr_ActorKillReputationCall, (UInt32)ActorKillReputationHook_Wrapper);
	}

	void RemovePatch()
	{
		SafeWrite::WriteBuf(kAddr_MurderAlarmReputationCall, g_origBytesMurder, sizeof(g_origBytesMurder));
		SafeWrite::WriteBuf(kAddr_AttackAlarmReputationCall, g_origBytesAttack, sizeof(g_origBytesAttack));
		SafeWrite::WriteBuf(kAddr_ActorKillReputationCall, g_origBytesKill, sizeof(g_origBytesKill));
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

