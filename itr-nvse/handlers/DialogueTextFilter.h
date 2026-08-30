#pragma once

#include "common/ITypes.h"

namespace DialogueTextFilter {
	bool Init(void* nvseInterface);
	void Update();
	void ClearState();
	void Suppress(bool suppress);
	const char* GetSpokenLine(UInt32 speakerRefID);
}
