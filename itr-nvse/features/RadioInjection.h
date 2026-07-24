#pragma once

namespace RadioInjection {
	void Init(void* nvseInterface);
	void RegisterCommands(void* nvse);
	void Update();
	void BeginTrackAdvance();
	void ClearState();
}
