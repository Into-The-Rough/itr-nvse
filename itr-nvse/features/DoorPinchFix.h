#pragma once

namespace DoorPinchFix
{
	void Init(bool enabled, int distance, int timeoutMs);
	void UpdateSettings(bool enabled, int distance, int timeoutMs);
	void Update();
	void ClearState();
}
