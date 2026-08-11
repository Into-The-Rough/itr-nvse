#pragma once

#include <cstddef>
#include <cstring>

namespace RadioInjectionLogic {

//the two radio outputs take different decoders: BSAudioManager rewrites the requested
//extension to wav/ogg, the media streamer opens mp3 only
enum SlotFormat {
	kSlot_Sound,
	kSlot_Stream,
};

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
	{
		char c = LowerASCII(path[i]);
		if (!c)
			return false;
		if (c == '/')
			c = '\\';
		if (c != prefix[i])
			return false;
	}
	return true;
}

//ext is lowercase and includes the dot
inline bool ExtensionIs(const char* path, const char* ext)
{
	const char* dot = path ? std::strrchr(path, '.') : nullptr;
	if (!dot)
		return false;
	size_t i = 0;
	for (; ext[i]; i++)
		if (LowerASCII(dot[i]) != ext[i])
			return false;
	return dot[i] == '\0';
}

//dj chatter and news share the sound slot with music, replacing them loses dialogue
//rather than a song, and voice lines are the one request always identifiable by path
inline bool IsVoicePath(const char* path)
{
	if (!path)
		return false;
	static const char needle[] = "\\voice\\";
	for (size_t i = 0; path[i]; i++)
	{
		size_t j = 0;
		for (; needle[j]; j++)
		{
			char c = path[i + j];
			if (!c)
				return false;
			if (c == '/')
				c = '\\';
			if (LowerASCII(c) != needle[j])
				break;
		}
		if (!needle[j])
			return true;
	}
	return false;
}

inline bool PathSuitsSlot(const char* path, SlotFormat slot)
{
	if (slot == kSlot_Stream)
		return ExtensionIs(path, ".mp3");
	return ExtensionIs(path, ".wav") || ExtensionIs(path, ".ogg");
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
