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

	//memory-indirect targets so the naked wrapper doesn't have to stage them in EAX.
	//Actor::GetLevel returns the level in AX; the engine reads AX at kAddr_ReturnAfterHook
	//(movzx edx, ax). Any post-call register staging would clobber that return value.
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

	void SetEnabled(bool enabled) {
		g_enabled = enabled;
	}

	void Init(bool enabled)
	{
		SafeWrite::WriteRelJump(kAddr_XPBlockStart, (UInt32)Hook_XPBlockStart);
		SafeWrite::Write8(kAddr_XPBlockStart + 5, 0x90);
		SafeWrite::Write8(kAddr_XPBlockStart + 6, 0x90);
		SafeWrite::Write8(kAddr_XPBlockStart + 7, 0x90);
		g_enabled = enabled;
	}
}

