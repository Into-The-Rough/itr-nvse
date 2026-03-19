#pragma once

namespace FallDamageHandler {
	bool Init(void* nvse);
	void RegisterCommands(void* nvse);
	UInt32 GetSetMultOpcode();
	UInt32 GetGetMultOpcode();
}
