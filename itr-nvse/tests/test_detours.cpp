//tests for internal/Detours.h

#include "test.h"
#include <Windows.h>
#include <cstring>

typedef unsigned int UInt32;
typedef unsigned short UInt16;
typedef unsigned char UInt8;
typedef signed int SInt32;

#include "../internal/Detours.h"

namespace
{
	struct ScopedCodeBuffer
	{
		UInt8* ptr = nullptr;

		explicit ScopedCodeBuffer(size_t size)
		{
			ptr = static_cast<UInt8*>(VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
		}

		~ScopedCodeBuffer()
		{
			if (ptr)
				VirtualFree(ptr, 0, MEM_RELEASE);
		}
	};
}

TEST(JumpDetour_RelocatesRel32Call)
{
	ScopedCodeBuffer source(64);
	ASSERT(source.ptr != nullptr);

	UInt8 original[14] = {
		0x83, 0xEC, 0x08, 0xDD, 0x1C, 0x24, 0xE8,
		0x00, 0x00, 0x00, 0x00, 0x83, 0xC4, 0x08
	};

	const UInt32 callSite = reinterpret_cast<UInt32>(source.ptr + 6);
	const UInt32 callTarget = reinterpret_cast<UInt32>(source.ptr + 32);
	*reinterpret_cast<SInt32*>(&original[7]) = static_cast<SInt32>(callTarget - (callSite + 5));

	memcpy(source.ptr, original, sizeof(original));
	memset(source.ptr + sizeof(original), 0x90, 64 - sizeof(original));

	Detours::JumpDetour detour;
	UInt8* trampoline = nullptr;
	ASSERT(detour.WriteRelJump(reinterpret_cast<UInt32>(source.ptr), reinterpret_cast<UInt32>(source.ptr + 48), sizeof(original), &trampoline));
	ASSERT(trampoline != nullptr);
	ASSERT(detour.IsInstalled());
	ASSERT_EQ(source.ptr[0], 0xE9);
	ASSERT_EQ(trampoline[6], 0xE8);

	const SInt32 relocatedDisp = *reinterpret_cast<SInt32*>(trampoline + 7);
	const UInt32 relocatedTarget = reinterpret_cast<UInt32>(trampoline + 11) + relocatedDisp;
	ASSERT_EQ(relocatedTarget, callTarget);
	ASSERT_EQ(trampoline[14], 0xE9);

	ASSERT(detour.Remove());
	ASSERT_EQ(memcmp(source.ptr, original, sizeof(original)), 0);
	return true;
}

TEST(JumpDetour_AcceptsCmovccPrologue)
{
	ScopedCodeBuffer source(32);
	ASSERT(source.ptr != nullptr);

	const UInt8 original[5] = { 0x0F, 0x45, 0xC1, 0x90, 0x90 };
	memcpy(source.ptr, original, sizeof(original));

	Detours::JumpDetour detour;
	ASSERT(detour.WriteRelJump(reinterpret_cast<UInt32>(source.ptr), reinterpret_cast<UInt32>(source.ptr + 16), sizeof(original)));
	ASSERT(detour.Remove());
	ASSERT_EQ(memcmp(source.ptr, original, sizeof(original)), 0);
	return true;
}

TEST(JumpDetour_RejectsTruncatedInstruction)
{
	ScopedCodeBuffer source(32);
	ASSERT(source.ptr != nullptr);

	const UInt8 original[5] = { 0x90, 0x90, 0x90, 0x0F, 0x45 };
	memcpy(source.ptr, original, sizeof(original));

	Detours::JumpDetour detour;
	ASSERT(!detour.WriteRelJump(reinterpret_cast<UInt32>(source.ptr), reinterpret_cast<UInt32>(source.ptr + 16), sizeof(original)));
	ASSERT(!detour.IsInstalled());
	ASSERT_EQ(memcmp(source.ptr, original, sizeof(original)), 0);
	return true;
}
