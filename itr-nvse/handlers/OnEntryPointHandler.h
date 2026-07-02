#pragma once

namespace OnEntryPointHandler {
	bool Init(void* nvseInterface);
	void BuildEntryMap();
	void InstallListenerProbe();
	void Update();
}
