#pragma once
#include <cstdint>

template <typename T_Ret = uint32_t, typename ...Args>
__forceinline T_Ret ThisCall(uint32_t addr, const void* _this, Args ...args) {
	return ((T_Ret(__thiscall*)(const void*, Args...))addr)(_this, args...);
}

template <typename T_Ret = void, typename ...Args>
__forceinline T_Ret CdeclCall(uint32_t addr, Args ...args) {
	return ((T_Ret(__cdecl*)(Args...))addr)(args...);
}
