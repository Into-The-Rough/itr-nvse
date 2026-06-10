//prevents player from earning infamy when a companion kills a faction member

#include "CompanionNoInfamy.h"
#include "internal/NVSEMinimal.h"
#include "internal/CallTemplates.h"
#include "internal/Detours.h"
#include "internal/GameGlobals.h"

namespace CompanionNoInfamy
{
	static bool g_enabled = false;
	static bool g_initialized = false;
	static Detours::CallDetour s_murderAlarmReputationCall;
	static Detours::CallDetour s_attackAlarmReputationCall;
	static Detours::CallDetour s_actorKillReputationCall;

	static const UInt32 kAddr_MurderAlarmReputationCall = 0x8C0E6E;
	static const UInt32 kAddr_AttackAlarmReputationCall = 0x8C0930;
	static const UInt32 kAddr_ActorKillReputationCall = 0x89F3DF;

	static void __fastcall Hook_MurderAlarmReputation(Actor* actor, UInt32 isTeammate, UInt32 a2, UInt32 a3)
	{
		//hook may outlive a refused remove
		if (g_enabled && isTeammate)
			return;

		ThisCall<void>(s_murderAlarmReputationCall.GetOverwrittenAddr(), actor, a2, static_cast<char>(a3)); //0x8B7D20 HandleMajorCrimeFactionReputations in vanilla
	}

	__declspec(naked) static void MurderAlarmReputationHook_Wrapper()
	{
		__asm
		{
			movzx edx, byte ptr [ebp-0x15]     //bIsTeammate local -> fastcall arg2; ecx has actor already
			jmp Hook_MurderAlarmReputation     //tail-jmp; typed Hook owns stack cleanup
		}
	}

	static void __fastcall Hook_AttackAlarmReputation(Actor* actor, Actor* attacker, UInt32 a2, UInt32 a3)
	{
		if (g_enabled && attacker != *(Actor**)g_thePlayerPtr)
			return;

		ThisCall<void>(s_attackAlarmReputationCall.GetOverwrittenAddr(), actor, a2, static_cast<char>(a3)); //0x8B7D20 HandleMajorCrimeFactionReputations in vanilla
	}

	__declspec(naked) static void AttackAlarmReputationHook_Wrapper()
	{
		__asm
		{
			mov edx, [ebp+8]                   //attacker = caller arg0 -> fastcall arg2
			jmp Hook_AttackAlarmReputation
		}
	}

	static void __fastcall Hook_ActorKillReputation(Actor* actor, Actor* attacker, UInt32 a2, UInt32 a3)
	{
		if (g_enabled && attacker != *(Actor**)g_thePlayerPtr)
			return;

		ThisCall<void>(s_actorKillReputationCall.GetOverwrittenAddr(), actor, a2, static_cast<bool>(a3)); //0x8B7C00 HandleMinorCrimeFactionReputations in vanilla
	}

	__declspec(naked) static void ActorKillReputationHook_Wrapper()
	{
		__asm
		{
			mov edx, [ebx+8]                   //attacker lives in an ebx-based struct here, not ebp
			jmp Hook_ActorKillReputation
		}
	}

	void RemovePatch();

	bool ApplyPatch()
	{
		//a refused remove leaves the detour installed
		const bool ok = (s_murderAlarmReputationCall.IsInstalled() || s_murderAlarmReputationCall.WriteRelCall(kAddr_MurderAlarmReputationCall, MurderAlarmReputationHook_Wrapper)) &&
			(s_attackAlarmReputationCall.IsInstalled() || s_attackAlarmReputationCall.WriteRelCall(kAddr_AttackAlarmReputationCall, AttackAlarmReputationHook_Wrapper)) &&
			(s_actorKillReputationCall.IsInstalled() || s_actorKillReputationCall.WriteRelCall(kAddr_ActorKillReputationCall, ActorKillReputationHook_Wrapper));
		if (!ok)
			RemovePatch();
		return ok;
	}

	void RemovePatch()
	{
		s_murderAlarmReputationCall.Remove();
		s_attackAlarmReputationCall.Remove();
		s_actorKillReputationCall.Remove();
	}

	void SetEnabled(bool enabled)
	{
		if (!g_initialized) return;
		if (enabled == g_enabled) return;

		if (enabled) {
			if (!ApplyPatch())
				return;
		} else {
			RemovePatch();
		}

		g_enabled = enabled;
	}

	void Init(bool enabled)
	{
		g_initialized = true;

		if (enabled)
		{
			g_enabled = ApplyPatch();
		}
	}
}
