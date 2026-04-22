#pragma once
#include <Windows.h>

namespace Settings
{
	inline int bAutoGodMode = 0;
	inline int bAutoQuickLoad = 0;
	inline int iAutoQuickLoadDelayMs = 0;
	inline int bMessageBoxQuickClose = 1;
	inline int bConsoleLogCleaner = 1;
	inline int bAltTabMute = 1;
	inline int bQuickDrop = 0;
	inline int bQuick180 = 0;

	inline int iQuickDropModifierKey = VK_SHIFT;
	inline int iQuickDropControlID = 7; //ready weapon
	inline int iQuick180ModifierKey = VK_SHIFT;
	inline int iQuick180ControlID = 5; //run

	inline int bSlowMotionPhysicsFix = 1;
	inline int bExplodingPantsFix = 0;
	inline int bKillActorXPFix = 1;
	inline int bReversePickpocketNoKarma = 0;
	inline int bSaveFileSize = 1;
	inline int bOwnerNameInfo = 0;
	inline int bDialogueCamera = 0;
	inline int bSmoothCameraAngleInterp = 0;
	inline int iShakeAmplitude = 3;
	inline int bVATSProjectileFix = 1;
	inline int bVATSLimbFix = 0;
	inline int bOwnedBeds = 0;
	inline int bAshPileNames = 0;

	inline int bLocationVisitPopup = 0;
	inline int iLocationVisitCooldownSeconds = 300;
	inline int bLocationVisitDisableSound = 0;

	inline int bVATSExtender = 0;
	inline int bSuppressObjectives = 0;
	inline int bSuppressReputation = 0;
	inline int bFriendlyFire = 0;
	inline int bNoDoorFade = 0;
	inline int bArmorDTDRFix = 1;

	inline int bQuickReadNote = 0;
	inline int iQuickReadNoteTimeoutMs = 5000;
	inline int iQuickReadNoteControlID = 6; //aim/block
	inline int iQuickReadNoteMaxLines = 0; //0 = auto from screen height

	inline int bDoorPackageOwnershipFix = 1;

	//0 = vanilla, 1 = only direct door owners bypass locks, 2 = must use key/lockpicks
	inline int iNPCDoorUnlockBlock = 0;

	inline int bVATSSpeechFix = 0;
	inline int bCombatItemTimerFix = 1;

	inline int bNPCAntidoteUse = 0;
	inline float fCombatItemCureTimer = 10.0f;
	inline float fCureHealthThreshold = 25.0f; //below this, prioritize stimpak over cure

	inline int bNPCDoctorsBagUse = 0;
	inline float fDoctorsBagUseTimer = 15.0f;

	inline int bCompanionNoInfamy = 0;
	inline int bPathingNullActorFix = 1;
	inline int bNavMeshInfoCrashFix = 1;
	inline int bInitHavokCrashFix = 1;
	inline int bMusicResetOnLoad = 1;
	inline int bOwnedCorpses = 0;
	inline int bDetectionFollowerCrashFix = 1;

	inline int iWitnessDetectionThreshold = 25;
	inline float fWitnessSearchRadius = 2048.0f;

	inline char iniPath[MAX_PATH];

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
		iAutoQuickLoadDelayMs = GetINIInt("AutoQuickLoad", "iDelayMs", 1500);

		iQuickDropModifierKey = GetINIInt("QuickDrop", "iModifierKey", VK_SHIFT);
		iQuickDropControlID = GetINIInt("QuickDrop", "iControlID", 7);
		iQuick180ModifierKey = GetINIInt("Quick180", "iModifierKey", VK_SHIFT);
		iQuick180ControlID = GetINIInt("Quick180", "iControlID", 5);

		bSlowMotionPhysicsFix = GetINIInt("Tweaks", "bSlowMotionPhysicsFix", 1);
		bExplodingPantsFix = GetINIInt("Tweaks", "bExplodingPantsFix", 0);
		bKillActorXPFix = GetINIInt("Tweaks", "bKillActorXPFix", 1);
		bReversePickpocketNoKarma = GetINIInt("Tweaks", "bReversePickpocketNoKarma", 0);
		bSaveFileSize = GetINIInt("Tweaks", "bSaveFileSize", 1);
		bOwnerNameInfo = GetINIInt("Tweaks", "bOwnerNameInfo", 0);
		bDialogueCamera = GetINIInt("Tweaks", "bDialogueCamera", 0);
		bSmoothCameraAngleInterp = GetINIInt("DialogueCamera", "bSmoothCameraAngleInterp", 0);
		iShakeAmplitude = GetINIInt("DialogueCamera", "iShakeAmplitude", 3);
		bVATSProjectileFix = GetINIInt("Tweaks", "bVATSProjectileFix", 1);
		bVATSLimbFix = GetINIInt("Tweaks", "bVATSLimbFix", 0);
		bOwnedBeds = GetINIInt("Tweaks", "bOwnedBeds", 0);
		bAshPileNames = GetINIInt("Tweaks", "bAshPileNames", 0);
		bLocationVisitPopup = GetINIInt("Tweaks", "bLocationVisitPopup", 0);
		iLocationVisitCooldownSeconds = GetINIInt("LocationVisitPopup", "iCooldownSeconds", 300);
		bLocationVisitDisableSound = GetINIInt("LocationVisitPopup", "bDisableSound", 0);
		bVATSExtender = GetINIInt("Tweaks", "bVATSExtender", 0);
		bSuppressObjectives = GetINIInt("Tweaks", "bSuppressObjectives", 0);
		bSuppressReputation = GetINIInt("Tweaks", "bSuppressReputation", 0);
		bFriendlyFire = GetINIInt("Tweaks", "bFriendlyFire", 0);
		bNoDoorFade = GetINIInt("Tweaks", "bNoDoorFade", 0);
		bArmorDTDRFix = GetINIInt("Tweaks", "bArmorDTDRFix", 1);
		bQuickReadNote = GetINIInt("Tweaks", "bQuickReadNote", 0);
		iQuickReadNoteTimeoutMs = GetINIInt("QuickReadNote", "iTimeoutMs", 5000);
		iQuickReadNoteControlID = GetINIInt("QuickReadNote", "iControlID", 6);
		iQuickReadNoteMaxLines = GetINIInt("QuickReadNote", "iMaxLines", 0);

		bDoorPackageOwnershipFix = GetINIInt("Tweaks", "bDoorPackageOwnershipFix", 1);
		iNPCDoorUnlockBlock = GetINIInt("Tweaks", "iNPCDoorUnlockBlock", 0);
		bVATSSpeechFix = GetINIInt("Tweaks", "bVATSSpeechFix", 0);
		bCombatItemTimerFix = GetINIInt("Tweaks", "bCombatItemTimerFix", 1);

		bNPCAntidoteUse = GetINIInt("Tweaks", "bNPCAntidoteUse", 0);
		fCombatItemCureTimer = (float)GetINIInt("NPCAntidoteUse", "iCureTimer", 10);
		fCureHealthThreshold = (float)GetINIInt("NPCAntidoteUse", "iHealthThreshold", 25);

		bNPCDoctorsBagUse = GetINIInt("Tweaks", "bNPCDoctorsBagUse", 0);
		fDoctorsBagUseTimer = (float)GetINIInt("NPCDoctorsBagUse", "iUseTimer", 15);

		bCompanionNoInfamy = GetINIInt("Tweaks", "bCompanionNoInfamy", 0);

		bPathingNullActorFix = GetINIInt("Tweaks", "bPathingNullActorFix", 1);
		bNavMeshInfoCrashFix = GetINIInt("Tweaks", "bNavMeshInfoCrashFix", 1);
		bInitHavokCrashFix = GetINIInt("Tweaks", "bInitHavokCrashFix", 1);

		bMusicResetOnLoad = GetINIInt("Tweaks", "bMusicResetOnLoad", 1);
		bOwnedCorpses = GetINIInt("Tweaks", "bOwnedCorpses", 0);
		bDetectionFollowerCrashFix = GetINIInt("Tweaks", "bDetectionFollowerCrashFix", 1);

		iWitnessDetectionThreshold = GetINIInt("OnWitnessed", "iDetectionThreshold", 25);
		fWitnessSearchRadius = (float)GetINIInt("OnWitnessed", "iSearchRadius", 2048);

	}
}
