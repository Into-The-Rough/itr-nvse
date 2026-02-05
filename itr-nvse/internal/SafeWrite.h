//minimal safe write utilities - can be included alongside any headers
#pragma once
#include <Windows.h>

namespace SafeWrite {
    inline void Write8(UInt32 addr, UInt8 data) {
        DWORD oldProtect;
        VirtualProtect((void*)addr, 1, PAGE_EXECUTE_READWRITE, &oldProtect);
        *(UInt8*)addr = data;
        VirtualProtect((void*)addr, 1, oldProtect, &oldProtect);
    }

    inline void Write32(UInt32 addr, UInt32 data) {
        DWORD oldProtect;
        VirtualProtect((void*)addr, 4, PAGE_EXECUTE_READWRITE, &oldProtect);
        *(UInt32*)addr = data;
        VirtualProtect((void*)addr, 4, oldProtect, &oldProtect);
    }

    inline void WriteRelCall(UInt32 src, UInt32 dst) {
        Write8(src, 0xE8);
        Write32(src + 1, dst - src - 5);
    }

    inline void WriteRelJump(UInt32 src, UInt32 dst) {
        Write8(src, 0xE9);
        Write32(src + 1, dst - src - 5);
    }

    inline UInt32 GetRelJumpTarget(UInt32 src) {
        return *(UInt32*)(src + 1) + src + 5;
    }

    inline void WriteNop(UInt32 addr, UInt32 size) {
        DWORD oldProtect;
        VirtualProtect((void*)addr, size, PAGE_EXECUTE_READWRITE, &oldProtect);
        memset((void*)addr, 0x90, size);
        VirtualProtect((void*)addr, size, oldProtect, &oldProtect);
    }
}
