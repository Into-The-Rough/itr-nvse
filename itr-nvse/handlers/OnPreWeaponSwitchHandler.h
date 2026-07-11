#pragma once

class Actor;

namespace OnPreWeaponSwitchHandler {
	typedef bool (*ExternalBlockCheck_t)(Actor*);   //returns true if switch must be blocked
	void SetExternalBlockCheck(ExternalBlockCheck_t fn);
	bool Init(void* nvseInterface);                  //installs the 0x9DA7C0 detour + registers event
	void Update();                                   //main-thread drain
	void ClearState();
}
