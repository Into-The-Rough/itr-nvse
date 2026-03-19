//allows giving weightless items to overencumbered companions in container menu

#include "CompanionWeightlessOverencumberedFix.h"
#include "internal/NVSEMinimal.h"
#include <cstdint>
#include <cmath>

#include "internal/globals.h"

namespace CompanionWeightlessOverencumberedFix
{
	static bool g_enabled = false;
	static bool g_initialized = false;

	//ContainerMenu::TransferItem overburdened branch
	constexpr UInt32 kAddr_OverburdenedBranch = 0x75DE17;
	constexpr UInt32 kAddr_OverburdenedPath = 0x75DE1C;
	constexpr UInt32 kAddr_TransferAllowedPath = 0x75DEA5;
	constexpr UInt32 kAddr_sTeammateOverencumbered = 0x11D2628;

	bool __cdecl IsWeightlessTransfer(UInt32 frameBase)
	{
		double totalWeight = *(double*)(frameBase - 0x14C);     //[ebp-0x14C]
		double inventoryWeight = *(double*)(frameBase - 0x144); //[ebp-0x144]
		double addedWeight = totalWeight - inventoryWeight;
		if (!std::isfinite(addedWeight))
			return false;
		return addedWeight <= 0.0;
	}

	__declspec(naked) void Hook_OverburdenedBranch()
	{
		__asm
		{
			cmp g_enabled, 0
			je vanilla_path

			push ebp
			call IsWeightlessTransfer
			add esp, 4
			test al, al
			jnz allow_transfer

		vanilla_path:
			mov ecx, kAddr_sTeammateOverencumbered
			jmp kAddr_OverburdenedPath

		allow_transfer:
			jmp kAddr_TransferAllowedPath
		}
	}

	void SetEnabled(bool enabled)
	{
		if (!g_initialized) return;
		g_enabled = enabled;
		Log("CompanionWeightlessOverencumberedFix %s", enabled ? "enabled" : "disabled");
	}

	void Init(bool enabled)
	{
		if (!g_initialized)
		{
			UInt8 opcode = *(UInt8*)kAddr_OverburdenedBranch;
			//0xB9 = original mov ecx, 0xE9 = our jmp from previous DLL load (hot reload)
			if (opcode != 0xB9 && opcode != 0xE9)
			{
				Log("ERROR: CompanionWeightlessOverencumberedFix unexpected byte 0x%02X at 0x%08X", opcode, kAddr_OverburdenedBranch);
				return;
			}

			SafeWrite::WriteRelJump(kAddr_OverburdenedBranch, (UInt32)Hook_OverburdenedBranch);
			g_initialized = true;
		}

		g_enabled = enabled;
		Log("CompanionWeightlessOverencumberedFix initialized (enabled=%d)", enabled);
	}
}

