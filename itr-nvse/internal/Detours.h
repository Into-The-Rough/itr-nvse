//detour utilities for function hooking
#pragma once
#include <Windows.h>
#include <cstdint>

namespace Detours {

//hook a CALL instruction (E8), capture original target
class CallDetour {
	UInt32 overwritten_addr = 0;
public:
	bool WriteRelCall(UInt32 src, UInt32 dst) {
		if (*reinterpret_cast<UInt8*>(src) != 0xE8) {
			return false;
		}
		overwritten_addr = *(UInt32*)(src + 1) + src + 5;
		DWORD oldProtect;
		VirtualProtect((void*)src, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
		*(UInt32*)(src + 1) = dst - src - 5;
		VirtualProtect((void*)src, 5, oldProtect, &oldProtect);
		return true;
	}

	template<typename T>
	bool WriteRelCall(UInt32 src, T dst) {
		return WriteRelCall(src, (UInt32)dst);
	}

	UInt32 GetOverwrittenAddr() const { return overwritten_addr; }
};

//hook function prologue with trampoline
//copies original bytes, builds jmp back, patches source
class JumpDetour {
	UInt8* trampoline = nullptr;
	UInt32 prologueSize = 0;

public:
	~JumpDetour() {
		if (trampoline) {
			VirtualFree(trampoline, 0, MEM_RELEASE);
		}
	}

	//src: address to hook, dst: hook function, size: bytes to copy (must be >= 5)
	bool WriteRelJump(UInt32 src, UInt32 dst, UInt32 size) {
		if (size < 5) return false;
		if (trampoline) return false; //already initialized

		prologueSize = size;

		//allocate executable memory for trampoline
		trampoline = (UInt8*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
		if (!trampoline) return false;

		//copy original bytes
		memcpy(trampoline, (void*)src, size);

		//add jmp back to (src + size)
		trampoline[size] = 0xE9;
		*(UInt32*)(trampoline + size + 1) = (src + size) - (UInt32)(trampoline + size) - 5;

		//patch source with jmp to hook
		DWORD oldProtect;
		VirtualProtect((void*)src, size, PAGE_EXECUTE_READWRITE, &oldProtect);
		*(UInt8*)src = 0xE9;
		*(UInt32*)(src + 1) = dst - src - 5;
		//nop remaining bytes
		for (UInt32 i = 5; i < size; i++)
			*(UInt8*)(src + i) = 0x90;
		VirtualProtect((void*)src, size, oldProtect, &oldProtect);

		return true;
	}

	template<typename T>
	bool WriteRelJump(UInt32 src, T dst, UInt32 size) {
		return WriteRelJump(src, (UInt32)dst, size);
	}

	UInt32 GetOverwrittenAddr() const { return (UInt32)trampoline; }

	template<typename T>
	T GetTrampoline() const { return (T)(void*)trampoline; }
};

} //namespace Detours
