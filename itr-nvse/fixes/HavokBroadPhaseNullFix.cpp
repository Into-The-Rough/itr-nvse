#include "HavokBroadPhaseNullFix.h"
#include <cstdint>
#include "internal/NVSEPluginAPI.h"
#include "internal/SafeWrite.h"
#include "internal/globals.h"

namespace HavokBroadPhaseNullFix {
	void Init()
	{
		static constexpr uint32_t kArgumentLoad = 0xD0D7D8;
		static constexpr uint32_t kUnlock = 0xC91210;
		static constexpr uint8_t expectedCall[] = {
			0x8B, 0x82, 0xCC, 0x00, 0x00, 0x00, 0x50, 0x8B, 0xCD,
			0xE8, 0x2A, 0x3A, 0xF8, 0xFF};
		static constexpr uint8_t expectedUnlock[] = {
			0x8B, 0x81, 0xB8, 0x00, 0x00, 0x00, 0x85, 0xC0, 0x74, 0x0A,
			0x89, 0x44, 0x24, 0x04, 0xFF, 0x25, 0x00, 0xF1, 0xFD, 0x00,
			0xC2, 0x04, 0x00};
		if (memcmp((void*)kArgumentLoad, expectedCall, sizeof(expectedCall)) != 0 || memcmp((void*)kUnlock, expectedUnlock, sizeof(expectedUnlock)) != 0) {
			Log("HavokBroadPhaseNullFix: collision or unlock bytes changed, skipping");
			return;
		}

		//0xC91210 ignores the argument and unlocks world+0xB8, so a null argument keeps the unlock
		static constexpr uint8_t argument[] = {0xB8, 0x00, 0x00, 0x00, 0x00, 0x90};
		SafeWrite::WriteBuf(kArgumentLoad, argument, sizeof(argument));
	}
}
