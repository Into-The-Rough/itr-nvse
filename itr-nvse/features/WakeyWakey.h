#pragma once

namespace WakeyWakey {
	void Init(bool enable, float wakeDistance, float quietWakeDistance, int cooldownMs);
	void UpdateSettings(bool enable, float wakeDistance, float quietWakeDistance, int cooldownMs);
}
