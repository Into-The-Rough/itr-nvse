#pragma once

namespace ImperativeCommands {
	bool Init(void* nvse);
	void Update();
	void ClearState();
	void RegisterCommands(void* nvse);   //0x401C: IsRadioPlaying
	void RegisterCommands2(void* nvse);  //0x4021-0x4029
	void RegisterCommands3(void* nvse);  //0x4030: UseAidItem
	void RegisterCommands6(void* nvse);  //0x4032: ResurrectActorEx
	void RegisterCommands4(void* nvse);  //0x4035-0x4037
	void RegisterCommands5(void* nvse);  //0x403B: SetRaceAlt
	void RegisterCommands7(void* nvse);  //0x405B: ForceCrouch, DisableCrouching
	void RegisterCommands8(void* nvse);  //0x405F: SetOnContactWatch, GetOnContactWatch
}
