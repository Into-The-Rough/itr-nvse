#pragma once

namespace ImperativeCommands {
	bool Init(void* nvse);
	void ClearState();
	void RegisterCommands(void* nvse);  //0x4021-0x4023: GetRefsSortedByDistance, Duplicate, GetAvailableRecipes
	void RegisterCommands2(void* nvse); //0x4025-0x4028: combat target location getters
	void RegisterDebugCommands(void* nvse); //0x4029: DumpCombatTarget, debug builds only
	void RegisterCommands3(void* nvse); //0x4030: UseAidItem
	void RegisterCommands4(void* nvse); //0x4035: SetCreatureCombatSkill
	void RegisterCommands5(void* nvse); //0x4037: ForceReload
	void RegisterCommands6(void* nvse); //0x403B: SetRaceAlt
	void RegisterCommands7(void* nvse); //0x405F: SetOnContactWatch, GetOnContactWatch
	void RegisterCommands8(void* nvse); //0x4066: RefillAmmo
}
