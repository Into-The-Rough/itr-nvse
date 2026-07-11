#pragma once

namespace OnKnockdownHandler {
	bool Init(void* nvseInterface);
	void InstallListenerProbe();
	void Update();
	void ClearState();
}
