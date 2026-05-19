#pragma once

namespace CompanionNoBlock
{
	void Init(bool enabled, int distance, int restoreDistance, bool interiorOnly, bool debugLog);
	void UpdateSettings(bool enabled, int distance, int restoreDistance, bool interiorOnly, bool debugLog);
	void Update();
	void ClearState();
}
