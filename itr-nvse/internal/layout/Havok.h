#pragma once

#include <cstddef>

struct HkpWorldObjectView {
	UInt8 pad00[0x08];
	void* world;
};
static_assert(offsetof(HkpWorldObjectView, world) == 0x08);
