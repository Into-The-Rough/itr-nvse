#include "HavokBroadPhaseNullFix.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/SafeWrite.h"
#include "internal/globals.h"

namespace HavokBroadPhaseNullFix
{
	void Init()
	{
		static constexpr UInt32 kArgumentLoad = 0xD0D7D8;
		static constexpr UInt32 kUnlock = 0xC91210;
		static constexpr UInt8 expectedCall[] = {
			0x8B, 0x82, 0xCC, 0x00, 0x00, 0x00, 0x50, 0x8B, 0xCD,
			0xE8, 0x2A, 0x3A, 0xF8, 0xFF
		};
		static constexpr UInt8 expectedUnlock[] = {
			0x8B, 0x81, 0xB8, 0x00, 0x00, 0x00, 0x85, 0xC0, 0x74, 0x0A,
			0x89, 0x44, 0x24, 0x04, 0xFF, 0x25, 0x00, 0xF1, 0xFD, 0x00,
			0xC2, 0x04, 0x00
		};
		if (memcmp((void*)kArgumentLoad, expectedCall, sizeof(expectedCall)) != 0 ||
			memcmp((void*)kUnlock, expectedUnlock, sizeof(expectedUnlock)) != 0)
		{
			Log("HavokBroadPhaseNullFix: collision or unlock bytes changed, skipping");
			return;
		}

		//0xc91210 ignores the argument and unlocks world+0xb8, even when the entity was removed
		static constexpr UInt8 argument[] = { 0xB8, 0x00, 0x00, 0x00, 0x00, 0x90 };
		SafeWrite::WriteBuf(kArgumentLoad, argument, sizeof(argument));
		Log("HavokBroadPhaseNullFix: installed at 0xD0D7D8, world unlock preserved");
	}
}
