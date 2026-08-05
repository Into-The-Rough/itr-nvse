#pragma once

namespace PerkRuntimeFramework
{
	bool Init(void* nvse);
	void RegisterCommands(void* nvse);
	void RegisterCommands2(void* nvse);
	void BuildIndex();
}
