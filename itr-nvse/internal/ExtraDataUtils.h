#pragma once

#include <cstddef>
#include <cstdint>

namespace ExtraDataUtils {

template <typename ExtraData>
inline ExtraData* GetExtraDataByType(ExtraData* head, const std::uint8_t* presentBits, std::size_t presentBitSize, std::uint32_t type)
{
	if (!head)
		return nullptr;

	const std::uint32_t byteIndex = type >> 3;
	const std::uint8_t bitMask = static_cast<std::uint8_t>(1 << (type & 7));
	if (presentBits && byteIndex < presentBitSize && !(presentBits[byteIndex] & bitMask))
		return nullptr;

	for (ExtraData* data = head; data; data = data->next)
		if (data->type == type)
			return data;

	return nullptr;
}

}
