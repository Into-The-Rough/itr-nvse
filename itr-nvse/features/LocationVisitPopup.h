#pragma once

namespace LocationVisitPopup {
	void Init(int cooldownSeconds, bool disableSound);
	void UpdateSettings(int cooldownSeconds, bool disableSound);
	void Update();
}
