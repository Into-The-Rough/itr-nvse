#pragma once

#include <cstddef>
#include <cstring>

namespace RadioInjectionLogic {

inline char LowerASCII(char c)
{
	return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

inline bool HasSoundRoot(const char* path)
{
	static const char prefix[] = "data\\sound\\";
	if (!path)
		return false;
	for (size_t i = 0; i < sizeof(prefix) - 1; i++)
		if (!path[i] || LowerASCII(path[i]) != prefix[i])
			return false;
	return true;
}

inline bool BuildEnginePath(const char* path, char* out, size_t outSize)
{
	if (!path || !path[0] || !out || !outSize)
		return false;

	static const char prefix[] = "data\\sound\\";
	const bool rooted = HasSoundRoot(path);
	const size_t prefixLen = rooted ? 0 : sizeof(prefix) - 1;
	const size_t pathLen = std::strlen(path);
	if (prefixLen + pathLen >= outSize)
		return false;

	if (prefixLen)
		std::memcpy(out, prefix, prefixLen);
	std::memcpy(out + prefixLen, path, pathLen + 1);
	return true;
}

}
