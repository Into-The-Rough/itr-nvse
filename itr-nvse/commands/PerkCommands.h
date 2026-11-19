#pragma once

namespace PerkCommands {
	bool Init(void* nvse);
	void RegisterCommands(void* nvse);
	void RegisterEligibleExCommands(void* nvse);
}
