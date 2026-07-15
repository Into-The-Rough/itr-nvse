#pragma once

namespace NPCAntidoteUse {
	void Init(float cureTimer, float healthThreshold);
	void UpdateSettings(bool enabled, float cureTimer, float healthThreshold);
	void Check(void* combatState);
}
