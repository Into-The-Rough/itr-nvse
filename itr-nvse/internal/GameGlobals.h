//shared engine data singletons, one source of truth
//functions live in EngineFunctions.h
#pragma once

#include <cstddef>

#include "common/ITypes.h"

class PlayerCharacter;
class TESObjectREFR;
class BGSDefaultObjectManager;
class BSSpinLock;
struct TileMenuArrayView;

struct GameSettingView {
	void* vtbl;
	union {
		UInt32 uint;
		SInt32 i;
		float f;
		char* str;
	} data;
	const char* name;
};

inline PlayerCharacter** const g_thePlayerPtr = (PlayerCharacter**)0x11DEA3C;
inline void** const g_tesPtr = (void**)0x11DEA10;
inline void* const g_processManager = (void*)0x11E0E80;
inline void** const g_dataHandlerPtr = (void**)0x11C3F2C; //DataHandler*
inline void** const g_interfaceManagerPtr = (void**)0x11D8A80;
inline void** const g_inputGlobalsPtr = (void**)0x11F35CC; //OSInputGlobals*
inline void** const g_osGlobalsPtr = (void**)0x11DEA0C; //OSGlobals*
inline void* const g_gameHeap = (void*)0x11F6238;
inline void* const g_audioManager = (void*)0x11F6EF0; //BSAudioManager
inline void** const g_bsWin32AudioPtr = (void**)0x11F6D98;
inline UInt32* const g_mapMenuVoicePlaybackAudioFlagsPtr = (UInt32*)0x7974CA;
inline void** const g_saveGameManagerPtr = (void**)0x11DE134;
inline void** const g_modelLoaderPtr = (void**)0x11C3B3C;
inline BGSDefaultObjectManager** const g_defaultObjectManagerPtr = (BGSDefaultObjectManager**)0x11CA80C;
inline void** const g_combatManagerPtr = (void**)0x11F1958;
inline void** const g_decalManagerPtr = (void**)0x11C57F8;
inline void*** const g_fontManagerPtr = (void***)0x11F33F8;
inline BSSpinLock* const g_processListsActorLock = (BSSpinLock*)0x11F11A0;
inline UInt8* const g_consoleStatePtr = (UInt8*)0x11DEA2E;
inline UInt8* const g_godModeEnabledPtr = (UInt8*)0x11E07BA;
inline float* const g_globalTimeMultiplierPtr = (float*)0x11AC3A0;
inline float* const g_iniMusicVolumePtr = (float*)0x11F6E44;
inline void** const g_hudMainMenuPtr = (void**)0x11D96C0;
inline char* const g_crosshairRefName = (char*)0x11D9C48;
inline void** const g_inventoryMenuPtr = (void**)0x11D9EA4;
inline void** const g_containerMenuPtr = (void**)0x11D93F8;
inline void** const g_containerMenuSelectionPtr = (void**)0x11D93FC;
inline void** const g_barterMenuPtr = (void**)0x11D8FA4;
inline void** const g_barterMenuSelectionPtr = (void**)0x11D8FA8;
inline UInt32* const g_barterMenuTraitIsBarterSelectedPtr = (UInt32*)0x11D8FB4;
inline void** const g_recipeMenuPtr = (void**)0x11D8E90;
inline SInt32* const g_recipeMenuCategoryPtr = (SInt32*)0x119FBF4;
inline void** const g_mapMenuPtr = (void**)0x11DA368;
inline UInt32* const g_mapMenuCurrentTabTraitPtr = (UInt32*)0x11DA360;
inline UInt32* const g_screenHeightPtr = (UInt32*)0x11F9434;
inline UInt8* const g_inDialogueOrHolotapePlayingPtr = (UInt8*)0x11DCFA4;
inline UInt8* const g_menuVisibilityArray = (UInt8*)0x11F308F;
inline UInt8* const g_shouldRestoreFirstPersonPtr = (UInt8*)0x11F21D0;
inline void** const g_dialogMenuPtr = (void**)0x11D9510;
inline bool* const g_dialogMenuVisiblePtr = (bool*)0x11D9514;
inline UInt32* const g_vatsModePtr = (UInt32*)0x11F2258;
inline void** const g_vatsMenuPtr = (void**)0x11DB0D4;
inline TESObjectREFR** const g_vatsMenuCurrentTargetPtr = (TESObjectREFR**)0x11F21CC;
inline void** const g_vatsRenderedTexturePtr = (void**)0x11DEB38;
inline void* const g_vatsTargetList = (void*)0x11DB150;
inline void* const g_fActorAlertSoundTimerSetting = (void*)0x11CD8D8;
inline void* const g_fDialogueTextMinSecondsPerCharSetting = (void*)0x11D2178;
inline void* const g_fDefaultWorldFOVSetting = (void*)0x120315C;
inline void* const g_fDefaultFirstPersonFOVSetting = (void*)0x1203168;
inline void* const g_fIronSightsZoomDefaultSetting = (void*)0x11E0970;
inline GameSettingView* const g_fHavokMaxTimeSetting = (GameSettingView*)0x1267B34;
inline GameSettingView** const g_fCombatItemRestoreTimerSettingPtr = (GameSettingView**)0x11CFDD8;
inline GameSettingView** const g_fCombatItemBuffTimerSettingPtr = (GameSettingView**)0x11CF480;
inline GameSettingView* const g_sFullHealthSetting = (GameSettingView*)0x11D2AF0;
inline UInt32* const g_voiceLipDistanceLimitPtr = (UInt32*)(0x11CD7D4 + 4);
inline TileMenuArrayView* const g_tileMenuArray = (TileMenuArrayView*)0x11F3508;
inline void* const g_textureManager = (void*)0x11A9598;
inline void* const g_primitivesEnabledSetting = (void*)0x11CA2DC;
inline void** const g_currentRadioPtr = (void**)0x11DD42C;
inline char* const g_currentRadioSong = (char*)0x11DD448;
inline void* const g_dynamicRadios = (void*)0x11DD58C;
inline UInt8* const g_radioEnabledPtr = (UInt8*)0x11DD434;
inline constexpr UInt32 kSetting_ReputationPositiveSound = 0x11CBCB0;
inline constexpr UInt32 kSetting_ReputationNegativeSound = 0x11CBA30;

struct OSInputGlobalsView {
	UInt8 pad00[0x18F8];
	UInt8 currKeyStates[256];
};

struct OSGlobalsView {
	UInt8 pad00[0x08];
	void* window;
};

struct SaveGameManagerView {
	UInt8 pad00[0x26];
	UInt8 isLoading;
};

static_assert(offsetof(OSInputGlobalsView, currKeyStates) == 0x18F8);
static_assert(offsetof(OSGlobalsView, window) == 0x08);
static_assert(offsetof(SaveGameManagerView, isLoading) == 0x26);
static_assert(offsetof(GameSettingView, data) == 0x04);
static_assert(sizeof(GameSettingView) == 0x0C);

inline UInt8* OSInputGlobalsGetCurrentKeyStates(void* input)
{
	return input ? reinterpret_cast<OSInputGlobalsView*>(input)->currKeyStates : nullptr;
}

inline void OSInputGlobalsSetKeyState(void* input, UInt32 key, UInt8 state)
{
	if (input && key < 256)
		OSInputGlobalsGetCurrentKeyStates(input)[key] = state;
}

inline void* OSGlobalsGetWindow(void* osGlobals)
{
	return osGlobals ? reinterpret_cast<OSGlobalsView*>(osGlobals)->window : nullptr;
}

inline bool IsGameLoading()
{
	void* mgr = *g_saveGameManagerPtr;
	if (!mgr) return false;
	return reinterpret_cast<SaveGameManagerView*>(mgr)->isLoading != 0;
}

inline void* GetInterfaceManager()
{
	return *g_interfaceManagerPtr;
}

inline void* GetTES()
{
	return *g_tesPtr;
}

inline void* GetModelLoader()
{
	return *g_modelLoaderPtr;
}

inline BGSDefaultObjectManager* GetDefaultObjectManager()
{
	return *g_defaultObjectManagerPtr;
}

inline void* GetCombatManager()
{
	return *g_combatManagerPtr;
}

inline void* GetDecalManager()
{
	return *g_decalManagerPtr;
}

inline void** GetFontManager()
{
	return *g_fontManagerPtr;
}

inline BSSpinLock* GetProcessListsActorLock()
{
	return g_processListsActorLock;
}

inline void* GetBSWin32Audio()
{
	return *g_bsWin32AudioPtr;
}

inline UInt32 GetMapMenuVoicePlaybackAudioFlags()
{
	return *g_mapMenuVoicePlaybackAudioFlagsPtr;
}

inline bool IsConsoleStateOpen()
{
	return *g_consoleStatePtr != 0;
}

inline void SetGodModeEnabled(bool enabled)
{
	*g_godModeEnabledPtr = enabled ? 1 : 0;
}

inline float GetGlobalTimeMultiplier()
{
	return *g_globalTimeMultiplierPtr;
}

inline float GetGameSettingFloat(GameSettingView* setting, float fallback = 0.0f)
{
	return setting ? setting->data.f : fallback;
}

inline const char* GetGameSettingString(GameSettingView* setting)
{
	return setting ? setting->data.str : nullptr;
}

inline float GetIniMusicVolume()
{
	return *g_iniMusicVolumePtr;
}

inline void SetIniMusicVolume(float volume)
{
	*g_iniMusicVolumePtr = volume;
}

inline float GetHavokMaxTime()
{
	return GetGameSettingFloat(g_fHavokMaxTimeSetting);
}

inline float GetCombatItemRestoreTimer()
{
	return GetGameSettingFloat(*g_fCombatItemRestoreTimerSettingPtr);
}

inline float GetCombatItemBuffTimer()
{
	return GetGameSettingFloat(*g_fCombatItemBuffTimerSettingPtr);
}

inline const char* GetFullHealthMessage()
{
	return GetGameSettingString(g_sFullHealthSetting);
}

inline UInt32* GetVoiceLipDistanceLimit()
{
	return g_voiceLipDistanceLimitPtr;
}

inline TileMenuArrayView* GetTileMenuArray()
{
	return g_tileMenuArray;
}

inline void* GetTextureManager()
{
	return g_textureManager;
}

inline void* GetPrimitivesEnabledSetting()
{
	return g_primitivesEnabledSetting;
}

inline void* GetHUDMainMenu()
{
	return *g_hudMainMenuPtr;
}

inline const char* GetCrosshairRefName()
{
	return g_crosshairRefName;
}

inline char* GetMutableCrosshairRefName()
{
	return g_crosshairRefName;
}

inline void* GetInventoryMenu()
{
	return *g_inventoryMenuPtr;
}

inline void* GetContainerMenu()
{
	return *g_containerMenuPtr;
}

inline void* GetContainerMenuSelection()
{
	return *g_containerMenuSelectionPtr;
}

inline void* GetBarterMenu()
{
	return *g_barterMenuPtr;
}

inline void* GetBarterMenuSelection()
{
	return *g_barterMenuSelectionPtr;
}

inline UInt32 GetBarterMenuSelectedTrait()
{
	return *g_barterMenuTraitIsBarterSelectedPtr;
}

inline void* GetRecipeMenu()
{
	return *g_recipeMenuPtr;
}

inline SInt32 GetRecipeMenuCategory()
{
	return *g_recipeMenuCategoryPtr;
}

inline void* GetMapMenu()
{
	return *g_mapMenuPtr;
}

inline UInt32 GetMapMenuCurrentTabTrait()
{
	return *g_mapMenuCurrentTabTraitPtr;
}

inline UInt32 GetGameScreenHeight()
{
	return *g_screenHeightPtr;
}

inline void SetInDialogueOrHolotapePlaying(bool value)
{
	*g_inDialogueOrHolotapePlayingPtr = value ? 1 : 0;
}

inline bool IsMenuVisible(UInt32 menuType)
{
	return g_menuVisibilityArray[menuType] != 0;
}

inline void* GetDialogMenu()
{
	return *g_dialogMenuPtr;
}

inline bool IsDialogMenuOpen()
{
	return *g_dialogMenuVisiblePtr;
}

inline void* GetCurrentRadio()
{
	return *g_currentRadioPtr;
}

inline char* GetCurrentRadioSong()
{
	return g_currentRadioSong;
}

inline void* GetDynamicRadios()
{
	return g_dynamicRadios;
}

inline bool IsRadioEnabled()
{
	return *g_radioEnabledPtr != 0;
}

inline UInt32 VATSGetMode()
{
	return *g_vatsModePtr;
}

inline void* VATSGetMenu()
{
	return *g_vatsMenuPtr;
}

inline TESObjectREFR* VATSGetCurrentTarget()
{
	return *g_vatsMenuCurrentTargetPtr;
}

inline void* VATSGetRenderedTexture()
{
	return *g_vatsRenderedTexturePtr;
}

inline bool IsVATSActive()
{
	return VATSGetMode() != 0;
}
