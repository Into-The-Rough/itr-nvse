#pragma once

namespace CompanionNoBlock
{
	void Init(bool enabled, int distance, int restoreDistance, bool interiorOnly);
	void UpdateSettings(bool enabled, int distance, int restoreDistance, bool interiorOnly);
	void Update();
	void ClearState();
}
