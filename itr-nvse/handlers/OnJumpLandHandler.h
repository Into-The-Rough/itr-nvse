#pragma once

namespace OnJumpLandHandler {
	bool Init(void* nvseInterface);
	void InstallListenerProbes();
	void ClearState();
	void Update();
}
