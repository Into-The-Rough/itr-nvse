//prevents voice/dialogue sounds from slowing down during VATS

#include "VATSSpeechFix.h"
#include "nvse/PluginAPI.h"
#include "internal/Detours.h"
#include <Windows.h>
#include <cstddef>

#include "internal/globals.h"

namespace VATSSpeechFix
{
	static volatile LONG g_enabled = FALSE;
	static bool g_hooksInstalled = false;

	namespace GameAddr {
		constexpr UInt32 BSWin32GameSound_Vtbl_Func09 = 0x10A3C18;
		constexpr UInt32 BSWin32GameSound_Vtbl_Func10 = 0x10A3C1C;
	}

	namespace AudioFlags {
		constexpr UInt32 kAudioFlags_IgnoreTimescale = 0x200000;
	}

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
	static_assert(offsetof(BSGameSound, soundFlags) == 0x08);

	enum : UInt32 {
		kBSGameSoundSoundFlagsOffset = offsetof(BSGameSound, soundFlags),
		kVatsSpeechIgnoreTimescaleFlag = AudioFlags::kAudioFlags_IgnoreTimescale,
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
	static UInt32 s_timescalePatchAddr = 0xAEDFBD; //for inline asm
	static Detours::JumpDetour s_timescaleDetour;

	static constexpr UInt8 kVanillaTimescalePatch[] = {
		0x83, 0xEC, 0x08, 0xDD, 0x1C, 0x24, 0xE8,
		0x16, 0x8D, 0x3D, 0x00, 0x83, 0xC4, 0x08
	};

	enum class TimescalePatchOwner {
		Vanilla,
		Stewie,
		Other,
	};

	static TimescalePatchOwner s_timescalePatchOwner = TimescalePatchOwner::Other;

	static TimescalePatchOwner GetTimescalePatchOwner()
	{
		auto* bytes = reinterpret_cast<const UInt8*>(s_timescalePatchAddr);
		if (memcmp(bytes, kVanillaTimescalePatch, sizeof(kVanillaTimescalePatch)) == 0)
			return TimescalePatchOwner::Vanilla;
		if (memcmp(bytes, kStewieTimescalePatch, sizeof(kStewieTimescalePatch)) == 0)
			return TimescalePatchOwner::Stewie;
		return TimescalePatchOwner::Other;
	}

	__declspec(naked) void HookedTimescaleNaked()
	{
		__asm {
			cmp g_enabled, 0
			je skip_to_original

			push eax                               //eax is the only reg we touch to read the sound this-ptr
			mov eax, [ebp-0x144]                    //BSWin32GameSound this, sub_AED990 stores ecx here at 0xAED999
			test dword ptr [eax+kBSGameSoundSoundFlagsOffset], kVatsSpeechIgnoreTimescaleFlag
			pop eax                                 //pop preserves ZF from the test
			jnz skip_timescale

		skip_to_original:
			jmp s_trampolineTimescale              //non-VATS sound: replay stolen bytes via trampoline

		skip_timescale:
			fabs                                   //already-loaded timescale on fpu top, undo slowmo multiply
			push eax                               //stash eax so we can build return address
			mov eax, s_timescalePatchAddr
			add eax, 14                            //skip past the 14-byte stolen region
			xchg eax, [esp]                        //swap return target into [esp], restore eax
			ret                                    //jump-by-ret to post-patch site
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

	static void StoreEnabled(bool enabled)
	{
		InterlockedExchange(&g_enabled, enabled ? TRUE : FALSE);
	}

	void __fastcall HookedFunc09(BSWin32GameSound* thisPtr, void* edx)
	{
		OriginalFunc09(thisPtr);

		if (g_enabled != FALSE && thisPtr)
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
		if (g_enabled != FALSE && thisPtr)
		{
			bool isVoice = (thisPtr->soundFlags & 0x4) != 0;
			if (!isVoice && thisPtr->filePath[0] != '\0')
				isVoice = IsVoiceSound(thisPtr->filePath);
			if (isVoice && !(thisPtr->soundFlags & AudioFlags::kAudioFlags_IgnoreTimescale))
				thisPtr->soundFlags |= AudioFlags::kAudioFlags_IgnoreTimescale;
		}

		return OriginalFunc10(thisPtr);
	}

	static bool InstallHooks()
	{
		if (g_hooksInstalled)
			return true;

		s_timescalePatchOwner = GetTimescalePatchOwner();
		if (s_timescalePatchOwner == TimescalePatchOwner::Other)
		{
			Log("VATSSpeechFix: skipping 0x%X, inline site already owned by another patch", s_timescalePatchAddr);
			return false;
		}

		//install the fallible inline detour before touching the vtable, so a failure here
		//leaves nothing half-patched to recurse into on a later retry
		if (s_timescalePatchOwner == TimescalePatchOwner::Vanilla)
		{
			if (!s_timescaleDetour.WriteRelJump(s_timescalePatchAddr, HookedTimescaleNaked, sizeof(kVanillaTimescalePatch), &s_trampolineTimescale))
			{
				Log("VATSSpeechFix: failed to install inline timescale detour");
				return false;
			}
			Log("VATSSpeechFix: installed inline timescale detour");
		}
		else
		{
			Log("VATSSpeechFix: using Stewie audio inline at 0x%X", s_timescalePatchAddr);
		}

		//capture the real originals only if the slots aren't already ours, a guarded retry
		//must never recapture HookedFunc09/10 as the passthrough target
		auto slot09 = *(BSWin32GameSound_Func09_t*)GameAddr::BSWin32GameSound_Vtbl_Func09;
		auto slot10 = *(BSWin32GameSound_Func10_t*)GameAddr::BSWin32GameSound_Vtbl_Func10;
		if ((void*)slot09 != (void*)HookedFunc09) OriginalFunc09 = slot09;
		if ((void*)slot10 != (void*)HookedFunc10) OriginalFunc10 = slot10;

		bool ok09 = SafeWrite32(GameAddr::BSWin32GameSound_Vtbl_Func09, (UInt32)HookedFunc09);
		bool ok10 = ok09 && SafeWrite32(GameAddr::BSWin32GameSound_Vtbl_Func10, (UInt32)HookedFunc10);
		if (!ok10)
		{
			Log("VATSSpeechFix: failed to hook sound vtable");
			if (ok09) SafeWrite32(GameAddr::BSWin32GameSound_Vtbl_Func09, (UInt32)OriginalFunc09);
			if (s_timescalePatchOwner == TimescalePatchOwner::Vanilla) s_timescaleDetour.Remove();
			return false;
		}

		g_hooksInstalled = true;
		return true;
	}

	void SetEnabled(bool enabled)
	{
		if (enabled && !InstallHooks())
		{
			StoreEnabled(false);
			return;
		}

		StoreEnabled(enabled);
	}

	void Init(bool enabled)
	{
		if (enabled && !InstallHooks())
		{
			StoreEnabled(false);
			return;
		}

		StoreEnabled(enabled);
	}
}

