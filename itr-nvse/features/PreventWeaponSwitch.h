#pragma once

class Actor;

namespace PreventWeaponSwitch {
	void Init();
	void RegisterCommands(void* nvse);
	void Set(Actor* actor, bool block);
	bool Get(Actor* actor);
}
