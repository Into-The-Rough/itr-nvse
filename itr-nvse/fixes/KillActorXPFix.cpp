//prevents XP reward when using "kill" command on already-dead actors

#include "KillActorXPFix.h"
#include "internal/NVSEMinimal.h"

#include "internal/globals.h"

namespace KillActorXPFix
{
	static bool g_enabled = false;

	constexpr uint32_t kAddr_XPBlockStart = 0x5BE379;
	constexpr uint32_t kAddr_XPBlockEnd = 0x5BE3FA;
	constexpr uint32_t kAddr_ActorGetLevel = 0x87F9F0;
	constexpr uint32_t kAddr_ReturnAfterHook = 0x5BE381;

	__declspec(naked) void Hook_XPBlockStart()
	{
		__asm
		{
			//check if fix is enabled
			cmp g_enabled, 0
			je normal_path

			mov ecx, [ebp-0x10]
			mov eax, [ecx + 0x108] //lifeState: 0=alive,1=dying,2=dead
			cmp eax, 1
			je skip_xp
			cmp eax, 2
			je skip_xp

		normal_path:
			mov ecx, [ebp-0x10]
			mov eax, kAddr_ActorGetLevel
			call eax
			mov eax, kAddr_ReturnAfterHook
			jmp eax

		skip_xp:
			mov eax, kAddr_XPBlockEnd
			jmp eax
		}
	}

	void SetEnabled(bool enabled) {
		g_enabled = enabled;
		Log("KillActorXPFix %s", enabled ? "enabled" : "disabled");
	}

	void Init(bool enabled)
	{
		SafeWrite::WriteRelJump(kAddr_XPBlockStart, (UInt32)Hook_XPBlockStart);
		SafeWrite::Write8(kAddr_XPBlockStart + 5, 0x90);
		SafeWrite::Write8(kAddr_XPBlockStart + 6, 0x90);
		SafeWrite::Write8(kAddr_XPBlockStart + 7, 0x90);
		g_enabled = enabled;
		Log("KillActorXPFix initialized (enabled=%d)", enabled);
	}
}

