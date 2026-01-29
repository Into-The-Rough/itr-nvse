//prevents voice/dialogue sounds from slowing down during VATS
//ported directly from SoundFilteringSoftware

#include "VATSSpeechFix.h"
#include "nvse/PluginAPI.h"
#include <Windows.h>

extern void Log(const char* fmt, ...);

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
	//we only need filePath and soundFlags, don't care about rest
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

static UInt8 s_originalTimescaleBytes[14] = {0};
static UInt8* s_trampolineTimescale = nullptr;
static UInt32 s_timescalePatchAddr = GameAddr::Func10_TimescalePatch;

__declspec(naked) void HookedTimescaleNaked()
{
	__asm {
		test dword ptr [esi+0x08], 0x200000
		jnz skip_timescale
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

	if (thisPtr)
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
	if (thisPtr)
	{
		bool isVoice = (thisPtr->soundFlags & 0x4) != 0;
		if (!isVoice && thisPtr->filePath[0] != '\0')
			isVoice = IsVoiceSound(thisPtr->filePath);
		if (isVoice && !(thisPtr->soundFlags & AudioFlags::kAudioFlags_IgnoreTimescale))
			thisPtr->soundFlags |= AudioFlags::kAudioFlags_IgnoreTimescale;
	}

	return OriginalFunc10(thisPtr);
}

void VATSSpeechFix_Init()
{
	//hook vtable Func09 (sound initialization)
	OriginalFunc09 = *(BSWin32GameSound_Func09_t*)GameAddr::BSWin32GameSound_Vtbl_Func09;
	SafeWrite32(GameAddr::BSWin32GameSound_Vtbl_Func09, (UInt32)HookedFunc09);

	//hook vtable Func10 (sound playback)
	OriginalFunc10 = *(BSWin32GameSound_Func10_t*)GameAddr::BSWin32GameSound_Vtbl_Func10;
	SafeWrite32(GameAddr::BSWin32GameSound_Vtbl_Func10, (UInt32)HookedFunc10);

	//timescale hook - checks flag 0x200000 and skips timescale application if set
	s_trampolineTimescale = (UInt8*)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (s_trampolineTimescale)
	{
		memcpy(s_originalTimescaleBytes, (void*)s_timescalePatchAddr, 14);
		memcpy(s_trampolineTimescale, s_originalTimescaleBytes, 14);
		s_trampolineTimescale[14] = 0xE9; //jmp
		*(UInt32*)(s_trampolineTimescale + 15) = (s_timescalePatchAddr + 14) - (UInt32)(s_trampolineTimescale + 14) - 5;

		DWORD oldProtect;
		if (VirtualProtect((void*)s_timescalePatchAddr, 14, PAGE_EXECUTE_READWRITE, &oldProtect))
		{
			*((UInt8*)s_timescalePatchAddr) = 0xE9; //jmp
			*((UInt32*)(s_timescalePatchAddr + 1)) = (UInt32)HookedTimescaleNaked - s_timescalePatchAddr - 5;
			for (int i = 5; i < 14; i++)
				*((UInt8*)(s_timescalePatchAddr + i)) = 0x90; //nop
			VirtualProtect((void*)s_timescalePatchAddr, 14, oldProtect, &oldProtect);
		}
	}

	Log("VATSSpeechFix installed");
}
