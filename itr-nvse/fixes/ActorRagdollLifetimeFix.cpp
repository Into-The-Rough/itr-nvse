#include "ActorRagdollLifetimeFix.h"
#include "internal/GameLayout.h"
#include "internal/SafeWrite.h"
#include "internal/globals.h"

namespace ActorRagdollLifetimeFix
{
	static constexpr UInt32 kControllerLoad = 0x87DD29;
	static constexpr UInt32 kPenetrationLoad = 0x87DD67;
	static constexpr UInt32 kControllerResume = 0x87DD2F;
	static constexpr UInt32 kPenetrationResume = 0x87DD6D;

	//actor::~actor keeps the captured object for deletion, callbacks must see a null owner slot
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
		static constexpr UInt8 controllerLoad[] = { 0x8B, 0x88, 0xAC, 0x00, 0x00, 0x00 };
		static constexpr UInt8 penetrationLoad[] = { 0x8B, 0x82, 0xB0, 0x00, 0x00, 0x00 };
		if (memcmp((void*)kControllerLoad, controllerLoad, sizeof(controllerLoad)) != 0 ||
			memcmp((void*)kPenetrationLoad, penetrationLoad, sizeof(penetrationLoad)) != 0)
		{
			Log("ActorRagdollLifetimeFix: destructor bytes changed, skipping both hooks");
			return;
		}

		SafeWrite::WriteRelJump(kControllerLoad, (UInt32)DetachController);
		SafeWrite::Write8(kControllerLoad + 5, 0x90);
		SafeWrite::WriteRelJump(kPenetrationLoad, (UInt32)DetachPenetrationUtil);
		SafeWrite::Write8(kPenetrationLoad + 5, 0x90);
		Log("ActorRagdollLifetimeFix: installed at 0x87DD29 and 0x87DD67");
	}
}
