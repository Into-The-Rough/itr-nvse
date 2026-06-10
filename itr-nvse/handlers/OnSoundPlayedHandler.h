#pragma once

namespace OnSoundPlayedHandler {
	bool Init(void* nvseInterface);
	void InstallListenerProbes();
	void Update();
}
