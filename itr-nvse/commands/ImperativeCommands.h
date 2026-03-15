#pragma once

bool ImperativeCommands_Init(void* nvse);
void ImperativeCommands_RegisterCommands(void* nvse);   //0x401C: IsRadioPlaying
void ImperativeCommands_RegisterCommands2(void* nvse);  //0x4021-0x4029
void ImperativeCommands_RegisterCommands3(void* nvse);  //0x4030: UseAidItem
void ImperativeCommands_RegisterCommands4(void* nvse);  //0x4035-0x4037
void ImperativeCommands_RegisterCommands5(void* nvse);  //0x403B: SetRaceAlt
