#include "JIPExtraDataSerializationFix.h"

#include "internal/SafeWrite.h"
#include "internal/globals.h"

#include <Windows.h>
#include <cstring>

namespace JIPExtraDataSerializationFix
{
	constexpr unsigned char kBrokenPattern[] = {
		0x8B, 0x46, 0x10,             // mov eax, [esi+10h]
		0x89, 0x44, 0x24, 0x2C,       // mov [esp+2Ch], eax
		0x8D, 0x14, 0x80,             // lea edx, [eax+eax*4]
		0x89, 0x54, 0x24, 0x24,       // mov [esp+24h], edx
		0x85, 0xC0,                   // test eax, eax
		0x74, 0x4D                    // jz ...
	};

	constexpr unsigned char kFixedPattern[] = {
		0x8B, 0x46, 0x10,
		0x89, 0x44, 0x24, 0x2C,
		0x8D, 0x14, 0xC0,             // lea edx, [eax+eax*8]
		0x89, 0x54, 0x24, 0x24,
		0x85, 0xC0,
		0x74, 0x4D
	};

	constexpr size_t kPatchOffset = 9;

	static unsigned char* FindUniquePattern(unsigned char* base, size_t size, const unsigned char* pattern, size_t patternSize, int& matches)
	{
		unsigned char* result = nullptr;
		matches = 0;

		if (!base || size < patternSize)
			return nullptr;

		for (size_t i = 0; i <= size - patternSize; ++i)
		{
			if (std::memcmp(base + i, pattern, patternSize) == 0)
			{
				result = base + i;
				++matches;
			}
		}

		return matches == 1 ? result : nullptr;
	}

	static bool GetModuleRange(HMODULE module, unsigned char*& base, size_t& size)
	{
		base = reinterpret_cast<unsigned char*>(module);
		size = 0;

		if (!base)
			return false;

		auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
			return false;

		auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE)
			return false;

		size = nt->OptionalHeader.SizeOfImage;
		return size != 0;
	}

	bool Init()
	{
		static bool s_done = false;
		if (s_done)
			return true;

		HMODULE jip = GetModuleHandleA("jip_nvse.dll");
		if (!jip)
		{
			Log("JIPExtraDataSerializationFix: jip_nvse.dll not loaded, skipping");
			return false;
		}

		unsigned char* base = nullptr;
		size_t size = 0;
		if (!GetModuleRange(jip, base, size))
		{
			Log("JIPExtraDataSerializationFix: could not read jip_nvse.dll image headers");
			return false;
		}

		int fixedMatches = 0;
		if (FindUniquePattern(base, size, kFixedPattern, sizeof(kFixedPattern), fixedMatches))
		{
			s_done = true;
			Log("JIPExtraDataSerializationFix: JIP JPED save-size patch already present");
			return true;
		}

		int brokenMatches = 0;
		unsigned char* patch = FindUniquePattern(base, size, kBrokenPattern, sizeof(kBrokenPattern), brokenMatches);
		if (!patch)
		{
			Log("JIPExtraDataSerializationFix: expected JIP JPED save-size signature not found (broken=%d fixed=%d)",
				brokenMatches, fixedMatches);
			return false;
		}

		if (patch[kPatchOffset] != 0x80)
		{
			Log("JIPExtraDataSerializationFix: unexpected patch byte 0x%02X at RVA 0x%X",
				patch[kPatchOffset], static_cast<unsigned>(patch + kPatchOffset - base));
			return false;
		}

		SafeWrite::Write8(reinterpret_cast<UInt32>(patch + kPatchOffset), 0xC0);
		s_done = patch[kPatchOffset] == 0xC0;
		Log(s_done
			? "JIPExtraDataSerializationFix: patched JIP JPED keySize multiplier at RVA 0x%X"
			: "JIPExtraDataSerializationFix: failed to patch JIP JPED keySize multiplier at RVA 0x%X",
			static_cast<unsigned>(patch + kPatchOffset - base));
		return s_done;
	}
}
