//detour utilities for function hooking
#pragma once
#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <limits>

namespace Detours {

namespace detail {

struct DecodedInstruction {
	UInt32 length = 0;
	UInt32 relOffset = 0;
	UInt32 relSize = 0;
	bool hasRelativeImm = false;
};

inline bool GetModRmLength(const UInt8* code, UInt32 maxLength, bool addressSize16, UInt32& length) {
	if (addressSize16 || maxLength == 0)
		return false;

	UInt32 size = 1;
	const UInt8 modrm = code[0];
	const UInt8 mod = modrm >> 6;
	const UInt8 rm = modrm & 7;

	if (mod != 3 && rm == 4) {
		if (size >= maxLength)
			return false;
		const UInt8 sib = code[size++];
		if (mod == 0 && (sib & 7) == 5)
			size += 4;
	}

	if (mod == 0) {
		if (rm == 5)
			size += 4;
	} else if (mod == 1) {
		size += 1;
	} else if (mod == 2) {
		size += 4;
	}

	if (size > maxLength)
		return false;

	length = size;
	return true;
}

inline bool DecodeInstruction(const UInt8* code, UInt32 maxLength, DecodedInstruction& instruction) {
	bool operandSize16 = false;
	bool addressSize16 = false;
	UInt32 offset = 0;

	const auto hasRemaining = [&](UInt32 bytesNeeded) {
		return offset <= maxLength && bytesNeeded <= maxLength - offset;
	};

	for (;;) {
		if (!hasRemaining(1))
			return false;

		switch (code[offset]) {
		case 0xF0:
		case 0xF2:
		case 0xF3:
		case 0x2E:
		case 0x36:
		case 0x3E:
		case 0x26:
		case 0x64:
		case 0x65:
			++offset;
			continue;
		case 0x66:
			operandSize16 = true;
			++offset;
			continue;
		case 0x67:
			addressSize16 = true;
			++offset;
			continue;
		default:
			break;
		}
		break;
	}

	const auto decodeModRm = [&](UInt32 immediateSize) {
		UInt32 modRmLength = 0;
		if (!GetModRmLength(code + offset, maxLength - offset, addressSize16, modRmLength))
			return false;
		if (modRmLength > maxLength - offset || immediateSize > maxLength - offset - modRmLength)
			return false;

		offset += modRmLength + immediateSize;
		instruction.length = offset;
		return true;
	};

	const auto decodeRel32 = [&]() {
		if (operandSize16 || !hasRemaining(4))
			return false;

		instruction.hasRelativeImm = true;
		instruction.relOffset = offset;
		instruction.relSize = 4;
		offset += 4;
		instruction.length = offset;
		return true;
	};

	if (!hasRemaining(1))
		return false;

	const UInt8 opcode = code[offset++];
	switch (opcode) {
	case 0x0F: {
		if (!hasRemaining(1))
			return false;
		const UInt8 opcode2 = code[offset++];
		if (opcode2 >= 0x40 && opcode2 <= 0x4F)
			return decodeModRm(0);
		if (opcode2 >= 0x80 && opcode2 <= 0x8F)
			return decodeRel32();
		if (opcode2 >= 0x90 && opcode2 <= 0x9F)
			return decodeModRm(0);

		switch (opcode2) {
		case 0x1F:
		case 0xAF:
		case 0xB6:
		case 0xB7:
		case 0xBE:
		case 0xBF:
			return decodeModRm(0);
		case 0xBA:
			return decodeModRm(1);
		default:
			return false;
		}
	}

	case 0x40: case 0x41: case 0x42: case 0x43:
	case 0x44: case 0x45: case 0x46: case 0x47:
	case 0x48: case 0x49: case 0x4A: case 0x4B:
	case 0x4C: case 0x4D: case 0x4E: case 0x4F:
	case 0x50: case 0x51: case 0x52: case 0x53:
	case 0x54: case 0x55: case 0x56: case 0x57:
	case 0x58: case 0x59: case 0x5A: case 0x5B:
	case 0x5C: case 0x5D: case 0x5E: case 0x5F:
	case 0x90: case 0x91: case 0x92: case 0x93:
	case 0x94: case 0x95: case 0x96: case 0x97:
	case 0xC3:
	case 0xC9:
	case 0xCB:
		instruction.length = offset;
		return true;

	case 0x68:
	case 0xA9:
		if (!hasRemaining(operandSize16 ? 2 : 4))
			return false;
		offset += operandSize16 ? 2 : 4;
		instruction.length = offset;
		return true;

	case 0x6A:
	case 0xA8:
		if (!hasRemaining(1))
			return false;
		offset += 1;
		instruction.length = offset;
		return true;

	case 0xA0:
	case 0xA1:
	case 0xA2:
	case 0xA3:
		if (!hasRemaining(addressSize16 ? 2 : 4))
			return false;
		offset += addressSize16 ? 2 : 4;
		instruction.length = offset;
		return true;

	case 0xB8: case 0xB9: case 0xBA: case 0xBB:
	case 0xBC: case 0xBD: case 0xBE: case 0xBF:
		if (!hasRemaining(operandSize16 ? 2 : 4))
			return false;
		offset += operandSize16 ? 2 : 4;
		instruction.length = offset;
		return true;

	case 0xC2:
	case 0xCA:
		if (!hasRemaining(2))
			return false;
		offset += 2;
		instruction.length = offset;
		return true;

	case 0xE8:
	case 0xE9:
		return decodeRel32();

	case 0xE0:
	case 0xE1:
	case 0xE2:
	case 0xE3:
	case 0xEB:
		return false;

	case 0x70: case 0x71: case 0x72: case 0x73:
	case 0x74: case 0x75: case 0x76: case 0x77:
	case 0x78: case 0x79: case 0x7A: case 0x7B:
	case 0x7C: case 0x7D: case 0x7E: case 0x7F:
		return false;

	case 0x69:
	case 0x81:
	case 0xC7:
		return decodeModRm(operandSize16 ? 2 : 4);

	case 0x6B:
	case 0x80:
	case 0x82:
	case 0x83:
	case 0xC0:
	case 0xC1:
	case 0xC6:
		return decodeModRm(1);

	case 0x8B:
	case 0x89:
	case 0x8D:
	case 0x8F:
	case 0x63:
	case 0x84:
	case 0x85:
	case 0x86:
	case 0x87:
	case 0x88:
	case 0x8A:
	case 0x8C:
	case 0x8E:
	case 0xD8:
	case 0xD9:
	case 0xDA:
	case 0xDB:
	case 0xDC:
	case 0xDD:
	case 0xDE:
	case 0xDF:
	case 0xFE:
	case 0xFF:
		return decodeModRm(0);

	case 0xF6: {
		if (!hasRemaining(1))
			return false;
		const UInt8 modrm = code[offset];
		return decodeModRm(((modrm >> 3) & 7) <= 1 ? 1 : 0);
	}

	case 0xF7: {
		if (!hasRemaining(1))
			return false;
		const UInt8 modrm = code[offset];
		return decodeModRm(((modrm >> 3) & 7) <= 1 ? (operandSize16 ? 2 : 4) : 0);
	}

	default:
		return false;
	}
}

inline bool RelocateRelativeImmediate(UInt8* trampolineCode, UInt32 trampolineAddr, UInt32 sourceAddr, const DecodedInstruction& instruction) {
	if (!instruction.hasRelativeImm || instruction.relSize != 4)
		return true;

	const auto originalDisp = *reinterpret_cast<std::int32_t*>(trampolineCode + instruction.relOffset);
	const auto originalTarget = static_cast<std::int64_t>(sourceAddr) + instruction.length + originalDisp;
	const auto relocatedDisp = originalTarget - (static_cast<std::int64_t>(trampolineAddr) + instruction.length);

	if (relocatedDisp < (std::numeric_limits<std::int32_t>::min)() ||
		relocatedDisp > (std::numeric_limits<std::int32_t>::max)())
		return false;

	*reinterpret_cast<std::int32_t*>(trampolineCode + instruction.relOffset) = static_cast<std::int32_t>(relocatedDisp);
	return true;
}

} // namespace detail

//hook a CALL instruction (E8), capture original target
class CallDetour {
	UInt32 overwritten_addr = 0;
public:
	bool WriteRelCall(UInt32 src, UInt32 dst) {
		if (*reinterpret_cast<UInt8*>(src) != 0xE8)
			return false;

		DWORD oldProtect;
		if (!VirtualProtect((void*)src, 5, PAGE_EXECUTE_READWRITE, &oldProtect))
			return false;

		overwritten_addr = *(UInt32*)(src + 1) + src + 5;
		*(UInt32*)(src + 1) = dst - src - 5;
		VirtualProtect((void*)src, 5, oldProtect, &oldProtect);
		FlushInstructionCache(GetCurrentProcess(), (void*)src, 5);
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
	UInt8 originalBytes[27] = {};
	UInt32 prologueSize = 0;
	UInt32 sourceAddr = 0;

public:
	~JumpDetour() {
		if (trampoline) {
			VirtualFree(trampoline, 0, MEM_RELEASE);
		}
	}

	//src: address to hook, dst: hook function, size: bytes to copy (must be 5-27)
	bool WriteRelJump(UInt32 src, UInt32 dst, UInt32 size, UInt8** trampolineOut = nullptr) {
		if (size < 5 || size > 27) return false; //5 min for jmp, 27 max (32 - 5 for footer)
		if (trampoline) return false; //already initialized

		//reject if already hooked (starts with JMP)
		if (*reinterpret_cast<UInt8*>(src) == 0xE9)
			return false;

		sourceAddr = src;
		prologueSize = size;

		//allocate executable memory for trampoline
		trampoline = (UInt8*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
		if (!trampoline) return false;

		memcpy(originalBytes, (void*)src, size);
		memcpy(trampoline, originalBytes, size);

		auto fail = [&]() {
			VirtualFree(trampoline, 0, MEM_RELEASE);
			trampoline = nullptr;
			prologueSize = 0;
			sourceAddr = 0;
			memset(originalBytes, 0, sizeof(originalBytes));
			if (trampolineOut)
				*trampolineOut = nullptr;
			return false;
		};

		for (UInt32 offset = 0; offset < size; ) {
			detail::DecodedInstruction instruction;
			if (!detail::DecodeInstruction(originalBytes + offset, size - offset, instruction))
				return fail();
			if (!detail::RelocateRelativeImmediate(trampoline + offset, (UInt32)(trampoline + offset), src + offset, instruction))
				return fail();
			offset += instruction.length;
		}

		//add jmp back to (src + size)
		trampoline[size] = 0xE9;
		*(UInt32*)(trampoline + size + 1) = (src + size) - (UInt32)(trampoline + size) - 5;
		FlushInstructionCache(GetCurrentProcess(), trampoline, size + 5);
		if (trampolineOut) {
			*trampolineOut = trampoline;
			MemoryBarrier();
		}

		//patch source with jmp to hook
		DWORD oldProtect;
		if (!VirtualProtect((void*)src, size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
			return fail();
		}

		*(UInt8*)src = 0xE9;
		*(UInt32*)(src + 1) = dst - src - 5;
		for (UInt32 i = 5; i < size; i++)
			*(UInt8*)(src + i) = 0x90;

		VirtualProtect((void*)src, size, oldProtect, &oldProtect);
		FlushInstructionCache(GetCurrentProcess(), (void*)src, size);
		return true;
	}

	template<typename T>
	bool WriteRelJump(UInt32 src, T dst, UInt32 size, UInt8** trampolineOut = nullptr) {
		return WriteRelJump(src, (UInt32)dst, size, trampolineOut);
	}

	bool Remove() {
		if (!trampoline || !sourceAddr || !prologueSize) return false;

		DWORD oldProtect;
		if (!VirtualProtect((void*)sourceAddr, prologueSize, PAGE_EXECUTE_READWRITE, &oldProtect))
			return false;

		memcpy((void*)sourceAddr, originalBytes, prologueSize);
		VirtualProtect((void*)sourceAddr, prologueSize, oldProtect, &oldProtect);
		FlushInstructionCache(GetCurrentProcess(), (void*)sourceAddr, prologueSize);

		VirtualFree(trampoline, 0, MEM_RELEASE);
		trampoline = nullptr;
		prologueSize = 0;
		sourceAddr = 0;
		memset(originalBytes, 0, sizeof(originalBytes));
		return true;
	}

	bool IsInstalled() const { return trampoline != nullptr; }

	UInt32 GetOverwrittenAddr() const { return (UInt32)trampoline; }

	template<typename T>
	T GetTrampoline() const { return (T)(void*)trampoline; }
};

} //namespace Detours
