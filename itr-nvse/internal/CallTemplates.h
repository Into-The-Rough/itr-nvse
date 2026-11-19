#pragma once

template<typename T_Ret = UInt32, typename... Args>
__forceinline T_Ret ThisCall(UInt32 addr, const void* _this, Args... args)
{
	return ((T_Ret(__thiscall*)(const void*, Args...))addr)(_this, args...);
}

template<typename T_Ret = void, typename... Args>
__forceinline T_Ret CdeclCall(UInt32 addr, Args... args)
{
	return ((T_Ret(__cdecl*)(Args...))addr)(args...);
}
