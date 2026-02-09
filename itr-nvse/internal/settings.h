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

	// OwnedBeds settings
	int bOwnedBeds = 0;  // Disabled by default - allow sleeping in owned beds with consequences

	// AshPileNames settings
	int bAshPileNames = 0;  // Disabled by default - show original NPC name for ash piles

	// LocationVisitPopup settings
	int bLocationVisitPopup = 1;  // Enabled by default - show popup when revisiting discovered locations
	int iLocationVisitCooldownSeconds = 300;  // Cooldown between popups for same location
	int bLocationVisitDisableSound = 0;  // Play sound with popup

	// VATSExtender settings
	int bVATSExtender = 1;  // Enabled by default - extend VATS target highlighting limit

	// ELMO settings - convert popups to corner messages
	int bSuppressObjectives = 1;  // Enabled by default - quest objectives as corner messages
	int bSuppressReputation = 1;  // Enabled by default - reputation changes as corner messages

	// FriendlyFire settings
	int bFriendlyFire = 0;  // Disabled by default - allow player to damage allies

	// NoDoorFade settings
	int bNoDoorFade = 0;  // Disabled by default - skip actor fade-out when entering doors

	// ArmorDTDRFix settings
	int bArmorDTDRFix = 1;  // Enabled by default - fix NPC armor DT/DR not updating on equip

	// QuickReadNote settings
	int bQuickReadNote = 1;  // Enabled by default - quick view notes on pickup
	int iQuickReadNoteTimeoutMs = 5000;
	int iQuickReadNoteControlID = 6;  // Aim/Block
	int iQuickReadNoteMaxLines = 0;  // 0 = auto-calculate from screen height

	// DoorPackageOwnershipFix settings
	int bDoorPackageOwnershipFix = 1;  // Enabled by default - fix NPCs locking doors they don't own

	// NPCDoorUnlockBlock settings
	// 0 = vanilla, 1 = only direct door owners can bypass locks, 2 = must use key/lockpicks
	int iNPCDoorUnlockBlock = 0;

	// VATSSpeechFix settings
	int bVATSSpeechFix = 1;

	// CombatItemTimerFix settings
	int bCombatItemTimerFix = 1;  // Enabled by default - fixes stimpak timer using wrong game setting

	// NPCAntidoteUse settings
	int bNPCAntidoteUse = 1;  // Enabled by default - NPCs use antidotes when poisoned
	float fCombatItemCureTimer = 10.0f;  // Cooldown between cure item uses
	float fCureHealthThreshold = 25.0f;  // Don't cure if health below this (prioritize stimpak)

	// NPCDoctorsBagUse settings
	int bNPCDoctorsBagUse = 1;  // Enabled by default - NPCs use doctor's bags when crippled
	float fDoctorsBagUseTimer = 15.0f;  // Cooldown between uses

	// CompanionNoInfamy settings
	int bCompanionNoInfamy = 1;  // Enabled by default - companions killing faction members doesn't give player infamy

	// MusicResetOnLoad settings
	int bMusicResetOnLoad = 1;  // Enabled by default - reset music state when loading a save

	static char iniPath[MAX_PATH];

	inline int GetINIInt(const char* section, const char* key, int defaultValue)
	{
		return GetPrivateProfileIntA(section, key, defaultValue, iniPath);
	}

	inline void Load()
	{
		GetModuleFileNameA(nullptr, iniPath, MAX_PATH);
		char* lastSlash = strrchr(iniPath, '\\');
		if (lastSlash) *lastSlash = '\0';
		strcat_s(iniPath, "\\Data\\config\\itr-nvse.ini");

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
		bOwnedBeds = GetINIInt("Tweaks", "bOwnedBeds", 0);
		bAshPileNames = GetINIInt("Tweaks", "bAshPileNames", 0);
		bLocationVisitPopup = GetINIInt("Tweaks", "bLocationVisitPopup", 1);
		iLocationVisitCooldownSeconds = GetINIInt("LocationVisitPopup", "iCooldownSeconds", 300);
		bLocationVisitDisableSound = GetINIInt("LocationVisitPopup", "bDisableSound", 0);
		bVATSExtender = GetINIInt("Tweaks", "bVATSExtender", 1);
		bSuppressObjectives = GetINIInt("Tweaks", "bSuppressObjectives", 1);
		bSuppressReputation = GetINIInt("Tweaks", "bSuppressReputation", 1);
		bFriendlyFire = GetINIInt("Tweaks", "bFriendlyFire", 0);
		bNoDoorFade = GetINIInt("Tweaks", "bNoDoorFade", 0);
		bArmorDTDRFix = GetINIInt("Tweaks", "bArmorDTDRFix", 1);
		bQuickReadNote = GetINIInt("Tweaks", "bQuickReadNote", 1);
		iQuickReadNoteTimeoutMs = GetINIInt("QuickReadNote", "iTimeoutMs", 5000);
		iQuickReadNoteControlID = GetINIInt("QuickReadNote", "iControlID", 6);
		iQuickReadNoteMaxLines = GetINIInt("QuickReadNote", "iMaxLines", 0);

		bDoorPackageOwnershipFix = GetINIInt("Tweaks", "bDoorPackageOwnershipFix", 1);

		iNPCDoorUnlockBlock = GetINIInt("Tweaks", "iNPCDoorUnlockBlock", 0);

		bVATSSpeechFix = GetINIInt("Tweaks", "bVATSSpeechFix", 1);

		bCombatItemTimerFix = GetINIInt("Tweaks", "bCombatItemTimerFix", 1);

		bNPCAntidoteUse = GetINIInt("Tweaks", "bNPCAntidoteUse", 1);
		fCombatItemCureTimer = (float)GetINIInt("NPCAntidoteUse", "iCureTimer", 10);
		fCureHealthThreshold = (float)GetINIInt("NPCAntidoteUse", "iHealthThreshold", 25);

		bNPCDoctorsBagUse = GetINIInt("Tweaks", "bNPCDoctorsBagUse", 1);
		fDoctorsBagUseTimer = (float)GetINIInt("NPCDoctorsBagUse", "iUseTimer", 15);

		bCompanionNoInfamy = GetINIInt("Tweaks", "bCompanionNoInfamy", 1);

		bMusicResetOnLoad = GetINIInt("Tweaks", "bMusicResetOnLoad", 1);

	}
}
