#pragma once
#include <Windows.h>

namespace Settings
{
	// Tweaks
	int bAutoGodMode = 0;
	int bAutoQuickLoad = 0;
	int bMessageBoxQuickClose = 1;  // Enabled by default
	int bConsoleLogCleaner = 1;     // Enabled by default
	int bAltTabMute = 1;            // Enabled by default
	int bQuickDrop = 0;             // Disabled by default (needs keybind config)
	int bQuick180 = 0;              // Disabled by default (needs keybind config)

	// AutoQuickLoad settings
	int iAutoQuickLoadFrameDelay = 5;

	// QuickDrop settings
	int iQuickDropModifierKey = VK_SHIFT;
	int iQuickDropControlID = 7;  // Ready weapon control

	// Quick180 settings
	int iQuick180ModifierKey = VK_SHIFT;
	int iQuick180ControlID = 5;   // Run control

	// SlowMotionPhysicsFix settings
	int bSlowMotionPhysicsFix = 1;  // Enabled by default

	// ExplodingPantsFix settings
	int bExplodingPantsFix = 1;  // Enabled by default

	// KillActorXPFix settings
	int bKillActorXPFix = 1;  // Enabled by default - prevents XP from kill command on already-dead actors

	// ReversePickpocketNoKarma settings
	int bReversePickpocketNoKarma = 1;  // Enabled by default - no karma loss when placing items (except live grenades)

	// SaveFileSize settings
	int bSaveFileSize = 1;  // Enabled by default - show file size in save/load menu

	// OwnerNameInfo settings
	int bOwnerNameInfo = 1;  // Enabled by default - show owner name on crosshair prompt

	// DialogueCamera settings
	int bDialogueCamera = 0;  // Disabled by default - cinematic camera angles during dialogue

	// VATSProjectileFix settings
	int bVATSProjectileFix = 1;  // Enabled by default - fix projectile hit chance in VATS

	// VATSLimbFix settings
	int bVATSLimbFix = 0;  // Disabled by default - hide dismembered limbs in VATS

	static char iniPath[MAX_PATH];

	inline int GetINIInt(const char* section, const char* key, int defaultValue)
	{
		return GetPrivateProfileIntA(section, key, defaultValue, iniPath);
	}

	inline void Load()
	{
		// Find INI next to our DLL (works with mod managers)
		GetModuleFileNameA(GetModuleHandleA("itr-nvse.dll"), iniPath, MAX_PATH);
		char* lastSlash = strrchr(iniPath, '\\');
		if (lastSlash) strcpy_s(lastSlash + 1, MAX_PATH - (lastSlash + 1 - iniPath), "itr-nvse.ini");

		// Load settings
		bAutoGodMode = GetINIInt("Tweaks", "bAutoGodMode", 0);
		bAutoQuickLoad = GetINIInt("Tweaks", "bAutoQuickLoad", 0);
		bMessageBoxQuickClose = GetINIInt("Tweaks", "bMessageBoxQuickClose", 1);
		bConsoleLogCleaner = GetINIInt("Tweaks", "bConsoleLogCleaner", 1);
		bAltTabMute = GetINIInt("Tweaks", "bAltTabMute", 1);
		bQuickDrop = GetINIInt("Tweaks", "bQuickDrop", 0);
		bQuick180 = GetINIInt("Tweaks", "bQuick180", 0);

		iAutoQuickLoadFrameDelay = GetINIInt("AutoQuickLoad", "iFrameDelay", 5);

		iQuickDropModifierKey = GetINIInt("QuickDrop", "iModifierKey", VK_SHIFT);
		iQuickDropControlID = GetINIInt("QuickDrop", "iControlID", 7);

		iQuick180ModifierKey = GetINIInt("Quick180", "iModifierKey", VK_SHIFT);
		iQuick180ControlID = GetINIInt("Quick180", "iControlID", 5);

		bSlowMotionPhysicsFix = GetINIInt("Tweaks", "bSlowMotionPhysicsFix", 1);
		bExplodingPantsFix = GetINIInt("Tweaks", "bExplodingPantsFix", 1);
		bKillActorXPFix = GetINIInt("Tweaks", "bKillActorXPFix", 1);
		bReversePickpocketNoKarma = GetINIInt("Tweaks", "bReversePickpocketNoKarma", 1);
		bSaveFileSize = GetINIInt("Tweaks", "bSaveFileSize", 1);
		bOwnerNameInfo = GetINIInt("Tweaks", "bOwnerNameInfo", 1);
		bDialogueCamera = GetINIInt("Tweaks", "bDialogueCamera", 0);
		bVATSProjectileFix = GetINIInt("Tweaks", "bVATSProjectileFix", 1);
		bVATSLimbFix = GetINIInt("Tweaks", "bVATSLimbFix", 0);
	}
}
