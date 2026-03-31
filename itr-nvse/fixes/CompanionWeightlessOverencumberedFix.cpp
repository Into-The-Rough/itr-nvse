//allows giving weightless items to overencumbered companions in container menu

#include "CompanionWeightlessOverencumberedFix.h"
#include "internal/NVSEMinimal.h"
#include "internal/CallTemplates.h"
#include <cstdint>
#include <cmath>
#include <limits>

#include "internal/globals.h"

namespace CompanionWeightlessOverencumberedFix
{
	static bool g_enabled = false;
	static bool g_initialized = false;

	constexpr UInt32 kAddr_GetMaxCarryWeightCall = 0x75DE01;
	constexpr UInt32 kAddr_GetMaxCarryWeightPerkModified = 0x8A0C20;
	constexpr UInt32 kAddr_GetFormWeight = 0x48EBC0;
	constexpr UInt32 kAddr_IsHardcore = 0x4D1360;
	constexpr UInt32 kAddr_PlayerSingleton = 0x11DEA3C;
	constexpr UInt32 kAddr_ContainerMenuSelection = 0x11D93FC;

	static double __fastcall Hook_GetMaxCarryWeightPerkModified(Actor* actor, UInt32 itemCount)
	{
		double maxCarryWeight = ThisCall<double>(kAddr_GetMaxCarryWeightPerkModified, actor);
		if (!g_enabled) return maxCarryWeight;

		TESForm* selectedForm = *(TESForm**)kAddr_ContainerMenuSelection;
		void* player = *(void**)kAddr_PlayerSingleton;
		if (!actor || !selectedForm || !player)
			return maxCarryWeight;

		bool isHardcore = ThisCall<bool>(kAddr_IsHardcore, player);
		double itemWeight = CdeclCall<double>(kAddr_GetFormWeight, selectedForm, isHardcore);
		double addedWeight = itemWeight * static_cast<double>(itemCount);
		if (!std::isfinite(addedWeight) || addedWeight > 0.0)
			return maxCarryWeight;

		//Force the immediate carry-weight compare at the call site to take the allow-transfer path.
		return (std::numeric_limits<double>::max)();
	}

	__declspec(naked) static void Hook_GetMaxCarryWeightPerkModified_Wrapper()
	{
		__asm
		{
			mov edx, [ebp+8]
			jmp Hook_GetMaxCarryWeightPerkModified
		}
	}

	void SetEnabled(bool enabled)
	{
		if (!g_initialized) return;
		g_enabled = enabled;
	}

	void Init(bool enabled)
	{
		if (!g_initialized)
		{
			UInt8 opcode = *(UInt8*)kAddr_GetMaxCarryWeightCall;
			if (opcode != 0xE8)
			{
				Log("ERROR: CompanionWeightlessOverencumberedFix unexpected byte 0x%02X at 0x%08X", opcode, kAddr_GetMaxCarryWeightCall);
				return;
			}

			SafeWrite::WriteRelCall(kAddr_GetMaxCarryWeightCall, (UInt32)Hook_GetMaxCarryWeightPerkModified_Wrapper);
			g_initialized = true;
		}

		g_enabled = enabled;
	}
}
