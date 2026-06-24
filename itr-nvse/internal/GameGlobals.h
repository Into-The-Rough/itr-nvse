//shared engine data singletons, one source of truth
//functions live in EngineFunctions.h
#pragma once

#include <cstddef>

class PlayerCharacter;

inline PlayerCharacter** const g_thePlayerPtr = (PlayerCharacter**)0x11DEA3C;
inline void* const g_processManager = (void*)0x11E0E80;
inline void** const g_dataHandlerPtr = (void**)0x11C3F2C; //DataHandler*
inline void** const g_inputGlobalsPtr = (void**)0x11F35CC; //OSInputGlobals*
inline void* const g_audioManager = (void*)0x11F6EF0; //BSAudioManager
inline void** const g_saveGameManagerPtr = (void**)0x11DE134;

struct SaveGameManagerView {
	UInt8 pad00[0x26];
	UInt8 isLoading;
};

static_assert(offsetof(SaveGameManagerView, isLoading) == 0x26);

inline bool IsGameLoading()
{
	void* mgr = *g_saveGameManagerPtr;
	if (!mgr) return false;
	return reinterpret_cast<SaveGameManagerView*>(mgr)->isLoading != 0;
}
