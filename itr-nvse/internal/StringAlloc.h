//backing definition for the CopyCString extern in nvse/Utilities.h, used by string array Elements
#pragma once
#include <cstring>

inline char* CopyCString(const char* src)
{
	using FormHeapAllocate_t = void* (__cdecl*)(UInt32);
	const auto FormHeapAllocate = reinterpret_cast<FormHeapAllocate_t>(0x401000);

	const size_t size = src ? strlen(src) : 0;
	auto* result = static_cast<char*>(FormHeapAllocate(static_cast<UInt32>(size + 1)));
	if (!result)
		return nullptr;
	result[size] = 0;
	if (size)
		memcpy(result, src, size);
	return result;
}
