#pragma once

namespace DialogueTextFilter {
	bool Init(void* nvseInterface);
	void Update();
	void ClearState();
	void Suppress(bool suppress);
}
