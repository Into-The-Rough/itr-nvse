#pragma once

namespace PlayerUpdateHook {
	void Init(bool quickDrop, int quickDropModKey, int quickDropControlID,
	          bool quick180, int quick180ModKey, int quick180ControlID);
	void UpdateSettings(int quickDropModKey, int quickDropControlID,
	                    int quick180ModKey, int quick180ControlID);
}
