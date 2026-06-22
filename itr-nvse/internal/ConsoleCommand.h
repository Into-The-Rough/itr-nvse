#pragma once

#include <cstddef>

namespace ConsoleCommand {

inline bool IsSpace(char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

inline char ToLowerAscii(char c)
{
	return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

inline bool ExtractCommandName(const char* line, char* out, std::size_t outSize)
{
	if (!line || !out || outSize == 0)
		return false;

	out[0] = '\0';

	const char* tokenStart = line;
	while (IsSpace(*tokenStart))
		++tokenStart;

	if (!*tokenStart || *tokenStart == ';')
		return false;

	const char* tokenEnd = tokenStart;
	while (*tokenEnd && !IsSpace(*tokenEnd))
		++tokenEnd;

	const char* commandStart = tokenStart;
	for (const char* it = tokenStart; it < tokenEnd; ++it)
	{
		if (*it == '.' && it + 1 < tokenEnd)
			commandStart = it + 1;
		else if (*it == '>' && it > tokenStart && *(it - 1) == '-' && it + 1 < tokenEnd)
			commandStart = it + 1;
	}

	std::size_t len = static_cast<std::size_t>(tokenEnd - commandStart);
	if (len == 0)
		return false;
	if (len >= outSize)
		len = outSize - 1;

	for (std::size_t i = 0; i < len; ++i)
		out[i] = ToLowerAscii(commandStart[i]);
	out[len] = '\0';
	return true;
}

}
