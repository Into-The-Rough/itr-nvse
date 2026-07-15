#pragma once

namespace NPCAntidoteUse {
	void Init(float cureTimer, float healthThreshold);
	void UpdateSettings(float cureTimer, float healthThreshold);
	void Check(void* combatState);
}
