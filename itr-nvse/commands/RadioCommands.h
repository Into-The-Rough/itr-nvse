#pragma once

namespace RadioCommands {
	void Init(void* nvse);
	void RegisterCommands(void* nvse);  //0x402E: GetPlayingRadioTrack, GetPlayingRadioTrackFileName
	void RegisterCommands2(void* nvse); //0x4031: GetPlayingRadioText
	void RegisterCommands3(void* nvse); //0x401C: IsRadioPlaying
	void RegisterCommands4(void* nvse); //0x4024: ChangeRadioTrack
}
