//minimal safe write utilities - can be included alongside any headers
#pragma once
#include <Windows.h>
#include <cstring>

namespace SafeWrite {
    inline void WriteBuf(UInt32 addr, const void* data, UInt32 size) {
        if (!size) return;

        DWORD oldProtect;
        VirtualProtect((void*)addr, size, PAGE_EXECUTE_READWRITE, &oldProtect);
        memcpy((void*)addr, data, size);
        VirtualProtect((void*)addr, size, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), (void*)addr, size);
    }

    inline void Write8(UInt32 addr, UInt8 data) {
        WriteBuf(addr, &data, 1);
    }

    inline void Write32(UInt32 addr, UInt32 data) {
        WriteBuf(addr, &data, 4);
    }

    inline void WriteRelCall(UInt32 src, UInt32 dst) {
        UInt8 patch[5] = { 0xE8 };
        *(UInt32*)(patch + 1) = dst - src - 5;
        WriteBuf(src, patch, sizeof(patch));
    }

    inline void WriteRelJump(UInt32 src, UInt32 dst) {
        UInt8 patch[5] = { 0xE9 };
        *(UInt32*)(patch + 1) = dst - src - 5;
        WriteBuf(src, patch, sizeof(patch));
    }

    inline UInt32 GetRelJumpTarget(UInt32 src) {
        return *(UInt32*)(src + 1) + src + 5;
    }

    inline void WriteNop(UInt32 addr, UInt32 size) {
        if (!size) return;

        DWORD oldProtect;
        VirtualProtect((void*)addr, size, PAGE_EXECUTE_READWRITE, &oldProtect);
        memset((void*)addr, 0x90, size);
        VirtualProtect((void*)addr, size, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), (void*)addr, size);
    }
}
