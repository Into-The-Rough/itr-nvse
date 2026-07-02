#pragma once

class Actor;

namespace PreventWeaponSwitch {
	void Init();
	void ClearState();
	void RegisterCommands(void* nvse);
	void Set(Actor* actor, bool block);
	bool Get(Actor* actor);
}
