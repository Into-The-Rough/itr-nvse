#pragma once

class TESObjectREFR;

namespace CollisionToggle
{
	bool IsDisabled(TESObjectREFR* ref);
	void SetEnabled(TESObjectREFR* ref, bool enabled);
}
