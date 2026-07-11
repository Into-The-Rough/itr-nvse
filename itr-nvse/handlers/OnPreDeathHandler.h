#pragma once

namespace OnPreDeathHandler {
	bool Init(void* nvseInterface);   //registers the event, installs both CallDetours inside Actor::Kill
	void InstallListenerProbe();
	void Update();                    //refreshes the listener probe
}
