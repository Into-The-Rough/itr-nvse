#pragma once
//case-insensitive substring matching - extracted from DialogueTextFilter

#include <cstring>

namespace StringMatch {

//case-insensitive substring search
//returns true if needle is found anywhere in haystack (or if needle is empty/wildcard)
inline bool ContainsSubstringCI(const char* haystack, const char* needle)
{
	if (!haystack) return false;
	if (!needle || !*needle || (needle[0] == '*' && needle[1] == '\0'))
		return true;
	size_t needleLen = strlen(needle);
	while (*haystack) {
		if (_strnicmp(haystack, needle, needleLen) == 0)
			return true;
		++haystack;
	}
	return false;
}

}
