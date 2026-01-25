#pragma once

void PlayerUpdateHook_Init(bool quickDrop, int quickDropModKey, int quickDropControlID,
                           bool quick180, int quick180ModKey, int quick180ControlID);

void PlayerUpdateHook_UpdateSettings(int quickDropModKey, int quickDropControlID,
                                     int quick180ModKey, int quick180ControlID);
