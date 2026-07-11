#pragma once

namespace RadioInjection {
	void Init(void* nvseInterface);
	void RegisterCommands(void* nvse);
	void ClearState();
}
