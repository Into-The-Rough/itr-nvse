#include "ActorRagdollLifetimeFix.h"
#include <cstdint>
#include "internal/GameLayout.h"
#include "internal/SafeWrite.h"
#include "internal/globals.h"

namespace ActorRagdollLifetimeFix {
	static constexpr uint32_t kControllerLoad = 0x87DD29;
	static constexpr uint32_t kPenetrationLoad = 0x87DD67;
	static constexpr uint32_t kControllerResume = 0x87DD2F;
	static constexpr uint32_t kPenetrationResume = 0x87DD6D;

	//the destructor keeps the captured object for deletion, callbacks see a null owner slot
	__declspec(naked) void DetachController()
	{
		__asm
		{
			mov ecx, [eax + 0xAC]
			mov dword ptr [eax + 0xAC], 0
			jmp kControllerResume
		}
	}

	__declspec(naked) void DetachPenetrationUtil()
	{
		__asm
		{
			mov eax, [edx + 0xB0]
			mov dword ptr [edx + 0xB0], 0
			jmp kPenetrationResume
		}
	}

	void Init()
	{
		static constexpr uint8_t controllerLoad[] = {0x8B, 0x88, 0xAC, 0x00, 0x00, 0x00};
		static constexpr uint8_t penetrationLoad[] = {0x8B, 0x82, 0xB0, 0x00, 0x00, 0x00};
		if (memcmp((void*)kControllerLoad, controllerLoad, sizeof(controllerLoad)) != 0 || memcmp((void*)kPenetrationLoad, penetrationLoad, sizeof(penetrationLoad)) != 0) {
			Log("ActorRagdollLifetimeFix: destructor bytes changed, skipping both hooks");
			return;
		}

		SafeWrite::WriteRelJump(kControllerLoad, (uint32_t)DetachController);
		SafeWrite::Write8(kControllerLoad + 5, 0x90);
		SafeWrite::WriteRelJump(kPenetrationLoad, (uint32_t)DetachPenetrationUtil);
		SafeWrite::Write8(kPenetrationLoad + 5, 0x90);
	}
}
