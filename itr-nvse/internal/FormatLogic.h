#pragma once

#include <cstddef>
#include <cstdio>

namespace FormatLogic {

inline void FormatFileSize(unsigned long long bytes, char* out, std::size_t outSize)
{
	if (!out || outSize == 0)
		return;

	if (bytes >= 1048576ULL)
		sprintf_s(out, outSize, "%.1f MB", bytes / 1048576.0);
	else if (bytes >= 1024ULL)
		sprintf_s(out, outSize, "%.1f KB", bytes / 1024.0);
	else
		sprintf_s(out, outSize, "%llu B", bytes);
}

inline const char* SafeString(const char* value)
{
	return value ? value : "";
}

inline const char* FormatReputationMessage(char* out, std::size_t outSize, const char* factionName, const char* repTitle, const char* repDesc)
{
	if (!out || outSize == 0)
		return "";

	sprintf_s(out, outSize, "%s - %s. %s", SafeString(factionName), SafeString(repTitle), SafeString(repDesc));
	return out;
}

}
