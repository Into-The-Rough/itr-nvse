//prevents player from earning infamy when a companion kills a faction member

#include "CompanionNoInfamy.h"
#include <Windows.h>
#include <cstdint>

extern void Log(const char* fmt, ...);

namespace CompanionNoInfamy
{
	static bool g_enabled = false;
	static bool g_initialized = false;

	//original bytes at 0x8C0E6E (5 bytes - call instruction)
	static uint8_t g_origBytes[5] = {0};

	//Actor::HandleMajorCrimeFactionReputations = 0x8B7D20
	static const uint32_t kAddr_HandleMajorCrimeFactionReputations = 0x8B7D20;
	//call site in Actor::MurderAlarm
	static const uint32_t kAddr_MurderAlarmReputationCall = 0x8C0E6E;

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

	//hook checks IsTeammate at [ebp-0x15] in MurderAlarm's stack frame
	//if IsTeammate is true (companion did the kill), skip reputation loss
	//if false (player kill), call original function
	__declspec(naked) void MurderAlarmReputationHook()
	{
		__asm
		{
			cmp byte ptr [ebp-0x15], 0  //check IsTeammate
			jnz skipCall                 //if companion kill, skip reputation loss
			mov eax, kAddr_HandleMajorCrimeFactionReputations
			jmp eax
		skipCall:
			ret 8  //clean up 2 stack args (a2, a3) and return
		}
	}

	void ApplyPatch()
	{
		WriteRelCall(kAddr_MurderAlarmReputationCall, (uint32_t)MurderAlarmReputationHook);
	}

	void RemovePatch()
	{
		for (int i = 0; i < 5; i++)
			SafeWrite8(kAddr_MurderAlarmReputationCall + i, g_origBytes[i]);
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
		Log("CompanionNoInfamy %s", enabled ? "enabled" : "disabled");
	}

	void Init(bool enabled)
	{
		//save original bytes before any patching
		memcpy(g_origBytes, (void*)kAddr_MurderAlarmReputationCall, 5);

		g_initialized = true;

		if (enabled)
		{
			ApplyPatch();
			g_enabled = true;
		}

		Log("CompanionNoInfamy initialized (enabled=%d)", enabled);
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
