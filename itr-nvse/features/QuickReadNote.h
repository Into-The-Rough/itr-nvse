#pragma once

namespace QuickReadNote {
	void Init(int timeoutMs, int controlID, int maxLines);
	void Update();
	void UpdateSettings(int timeoutMs, int controlID, int maxLines);
}
