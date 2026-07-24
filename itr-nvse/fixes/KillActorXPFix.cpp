//prevents XP reward when using "kill" command on already-dead actors

#include "KillActorXPFix.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/SafeWrite.h"

#include "internal/globals.h"

namespace KillActorXPFix
{
	static bool g_enabled = false;

	constexpr uint32_t kAddr_XPBlockStart = 0x5BE379;
	constexpr uint32_t kAddr_XPBlockEnd = 0x5BE3FA;
	constexpr uint32_t kAddr_ActorGetLevel = 0x87F9F0;
	constexpr uint32_t kAddr_ReturnAfterHook = 0x5BE381;

	//memory-indirect targets so the naked wrapper doesn't have to stage them in eax
	//Actor::GetLevel returns the level in ax, the engine reads ax at kAddr_ReturnAfterHook
	//(movzx edx, ax) - any post-call register staging would clobber that return value
	static UInt32 s_actorGetLevel = kAddr_ActorGetLevel;
	static UInt32 s_returnAfterHook = kAddr_ReturnAfterHook;
	static UInt32 s_xpBlockEnd = kAddr_XPBlockEnd;

	__declspec(naked) void Hook_XPBlockStart()
	{
		__asm
		{
			cmp g_enabled, 0
			je normal_path               //fix off: run vanilla XP path

			mov ecx, [ebp-0x10]          //target actor local
			mov eax, [ecx + 0x108]       //lifeState: 0=alive,1=dying,2=dead
			cmp eax, 1
			je skip_xp
			cmp eax, 2
			je skip_xp

		normal_path:
			mov ecx, [ebp-0x10]          //replay stolen load for ActorGetLevel
			call [s_actorGetLevel]       //preserves EAX (=level) across the indirect jmp below
			jmp [s_returnAfterHook]

		skip_xp:
			jmp [s_xpBlockEnd]           //jump past the whole XP award block
		}
	}

	//mov ecx,[ebp-0x10] - call Actor::GetLevel (0x87F9F0)
	static constexpr UInt8 kExpected[8] = { 0x8B, 0x4D, 0xF0, 0xE8, 0x6F, 0x16, 0x2C, 0x00 };

	void SetEnabled(bool enabled) {
		g_enabled = enabled;
	}

	void Init(bool enabled)
	{
		if (memcmp((void*)kAddr_XPBlockStart, kExpected, sizeof(kExpected)) != 0)
		{
			Log("KillActorXPFix: 0x%X bytes changed, another plugin owns this site, skipping", kAddr_XPBlockStart);
			return;
		}

		SafeWrite::WriteRelJump(kAddr_XPBlockStart, (UInt32)Hook_XPBlockStart);
		SafeWrite::Write8(kAddr_XPBlockStart + 5, 0x90);
		SafeWrite::Write8(kAddr_XPBlockStart + 6, 0x90);
		SafeWrite::Write8(kAddr_XPBlockStart + 7, 0x90);
		g_enabled = enabled;
	}
}

