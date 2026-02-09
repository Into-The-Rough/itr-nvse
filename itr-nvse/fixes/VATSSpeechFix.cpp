//prevents voice/dialogue sounds from slowing down during VATS
//ported directly from SoundFilteringSoftware

#include "VATSSpeechFix.h"
#include "nvse/PluginAPI.h"
#include "internal/Detours.h"
#include <Windows.h>

extern void Log(const char* fmt, ...);

namespace VATSSpeechFix
{
	static bool g_enabled = false;

	namespace GameAddr {
		constexpr UInt32 BSWin32GameSound_Vtbl_Func09 = 0x10A3C18;
		constexpr UInt32 BSWin32GameSound_Vtbl_Func10 = 0x10A3C1C;
		constexpr UInt32 Func10_TimescalePatch = 0xAEDFBD;
	}

	namespace AudioFlags {
		constexpr UInt32 kAudioFlags_IgnoreTimescale = 0x200000;
	}

	//exact struct from SoundFilteringSoftware
	class BSGameSound
	{
	public:
		void*       vtable;              //000
		UInt32      mapKey;              //004
		UInt32      soundFlags;          //008
		UInt32      flags00C;            //00C
		UInt32      stateFlags;          //010
		UInt32      duration;            //014
		UInt16      staticAttenuation;   //018
		UInt16      unk01A;              //01A
		UInt16      unk01C;              //01C
		UInt16      unk01E;              //01E
		UInt16      unk020;              //020
		UInt16      unk022;              //022
		float       volume;              //024
		float       flt028;              //028
		float       flt02C;              //02C
		UInt32      unk030;              //030
		UInt16      baseSamplingFreq;    //034
		char        filePath[254];       //036
	};

	class BSWin32GameSound : public BSGameSound
	{
	public:
	};

	static bool IsVoiceSound(const char* filePath)
	{
		if (!filePath) return false;
		const char* p = filePath;
		while (*p)
		{
			if ((p[0] == 'v' || p[0] == 'V') &&
				(p[1] == 'o' || p[1] == 'O') &&
				(p[2] == 'i' || p[2] == 'I') &&
				(p[3] == 'c' || p[3] == 'C') &&
				(p[4] == 'e' || p[4] == 'E'))
			{
				return true;
			}
			p++;
		}
		return false;
	}

	typedef void (__thiscall *BSWin32GameSound_Func09_t)(BSWin32GameSound* thisPtr);
	static BSWin32GameSound_Func09_t OriginalFunc09 = nullptr;

	typedef char (__thiscall *BSWin32GameSound_Func10_t)(BSWin32GameSound* thisPtr);
	static BSWin32GameSound_Func10_t OriginalFunc10 = nullptr;

	static UInt8* s_trampolineTimescale = nullptr; //for inline asm indirect jump
	static UInt32 s_timescalePatchAddr = GameAddr::Func10_TimescalePatch; //for inline asm
	static Detours::JumpDetour s_timescaleDetour;

	__declspec(naked) void HookedTimescaleNaked()
	{
		__asm {
			//check if enabled
			cmp g_enabled, 0
			je skip_to_original

			test dword ptr [esi+0x08], 0x200000
			jnz skip_timescale

		skip_to_original:
			jmp s_trampolineTimescale

		skip_timescale:
			fabs
			push eax
			mov eax, s_timescalePatchAddr
			add eax, 14
			xchg eax, [esp]
			ret
		}
	}

	static bool SafeWrite32(UInt32 addr, UInt32 data)
	{
		DWORD oldProtect;
		if (!VirtualProtect((void*)addr, 4, PAGE_EXECUTE_READWRITE, &oldProtect))
			return false;
		*((UInt32*)addr) = data;
		VirtualProtect((void*)addr, 4, oldProtect, &oldProtect);
		return true;
	}

	void __fastcall HookedFunc09(BSWin32GameSound* thisPtr, void* edx)
	{
		OriginalFunc09(thisPtr);

		if (g_enabled && thisPtr)
		{
			bool isVoice = (thisPtr->soundFlags & 0x4) != 0;
			if (!isVoice && thisPtr->filePath[0] != '\0')
				isVoice = IsVoiceSound(thisPtr->filePath);
			if (isVoice)
				thisPtr->soundFlags |= AudioFlags::kAudioFlags_IgnoreTimescale;
		}
	}

	char __fastcall HookedFunc10(BSWin32GameSound* thisPtr, void* edx)
	{
		if (g_enabled && thisPtr)
		{
			bool isVoice = (thisPtr->soundFlags & 0x4) != 0;
			if (!isVoice && thisPtr->filePath[0] != '\0')
				isVoice = IsVoiceSound(thisPtr->filePath);
			if (isVoice && !(thisPtr->soundFlags & AudioFlags::kAudioFlags_IgnoreTimescale))
				thisPtr->soundFlags |= AudioFlags::kAudioFlags_IgnoreTimescale;
		}

		return OriginalFunc10(thisPtr);
	}

	void SetEnabled(bool enabled)
	{
		g_enabled = enabled;
		Log("VATSSpeechFix %s", enabled ? "enabled" : "disabled");
	}

	void Init(bool enabled)
	{
		//hook vtable Func09 (sound initialization)
		OriginalFunc09 = *(BSWin32GameSound_Func09_t*)GameAddr::BSWin32GameSound_Vtbl_Func09;
		SafeWrite32(GameAddr::BSWin32GameSound_Vtbl_Func09, (UInt32)HookedFunc09);

		//hook vtable Func10 (sound playback)
		OriginalFunc10 = *(BSWin32GameSound_Func10_t*)GameAddr::BSWin32GameSound_Vtbl_Func10;
		SafeWrite32(GameAddr::BSWin32GameSound_Vtbl_Func10, (UInt32)HookedFunc10);

		//timescale hook - checks flag 0x200000 and skips timescale application if set
		//prologue: 14 bytes
		if (s_timescaleDetour.WriteRelJump(s_timescalePatchAddr, HookedTimescaleNaked, 14))
			s_trampolineTimescale = (UInt8*)s_timescaleDetour.GetOverwrittenAddr();

		g_enabled = enabled;
		Log("VATSSpeechFix initialized (enabled=%d)", enabled);
	}
}

void VATSSpeechFix_Init(bool enabled)
{
	VATSSpeechFix::Init(enabled);
}

void VATSSpeechFix_SetEnabled(bool enabled)
{
	VATSSpeechFix::SetEnabled(enabled);
}
