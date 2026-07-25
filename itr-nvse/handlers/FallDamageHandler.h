#pragma once

class Actor;

namespace FallDamageHandler {
	void InstallHook();
	void RegisterCommands(void* nvse);
	void ClearState();
	bool HasOverride(Actor* actor);
	void SetMultiplier(float mult, Actor* actor = nullptr);
	float GetMultiplier(Actor* actor = nullptr);
	void ClearMultiplier(Actor* actor = nullptr);
}
