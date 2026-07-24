#pragma once

namespace OnCombatProcedureHandler {
	bool Init(void* nvseInterface);
	void InstallListenerProbe();
	void Update();
	void ClearState();
}
