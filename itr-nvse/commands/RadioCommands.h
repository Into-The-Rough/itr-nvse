#pragma once

namespace RadioCommands {
	void Init(void* nvse);
	void RegisterCommands(void* nvse); //0x402E: GetPlayingRadioTrack, GetPlayingRadioTrackFileName
	void RegisterRadioTextCommands(void* nvse); //0x4031: GetPlayingRadioText
	void RegisterIsPlayingCommands(void* nvse); //0x401C: IsRadioPlaying
	void RegisterChangeTrackCommands(void* nvse); //0x4024: ChangeRadioTrack
}
