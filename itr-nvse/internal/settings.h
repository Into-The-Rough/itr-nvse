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
	inline int bAimZoomFirstPersonOnly = 0;
	inline int bSuppressInputInConsole = 0;
	inline int bDebugLog = 0;

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
	inline int bVATSHighlightDepthFix = 0;
	inline int bSuppressObjectives = 0;
	inline int bSuppressReputation = 0;
	inline int bFriendlyFire = 0;
	inline int bNoDoorFade = 0;
	inline int bDoorPinchFix = 1;
	inline int iDoorPinchDistance = 140;
	inline int iDoorPinchTimeoutMs = 8000;
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
	inline int bStuckCombatStateFix = 0;
	inline int bLoopingSoundLoadFix = 0;

	inline int bNPCAntidoteUse = 0;
	inline float fCombatItemCureTimer = 10.0f;
	inline float fCureHealthThreshold = 25.0f; //below this, prioritize stimpak over cure

	inline int bNPCDoctorsBagUse = 0;
	inline float fDoctorsBagUseTimer = 15.0f;

	inline int bCompanionNoInfamy = 0;
	inline int bCompanionNoBlock = 0;
	inline int iCompanionNoBlockReleaseFrames = 30;
	inline int iCompanionNoBlockRestoreDistance = 220;
	inline int bCompanionNoBlockInteriorOnly = 1;
	inline int bPathingNullActorFix = 1;
	inline int bNavMeshInfoCrashFix = 1;
	inline int bInitHavokCrashFix = 1;
	inline int bMusicResetOnLoad = 1;
	inline int bOwnedCorpses = 0;
	inline int bDetectionFollowerCrashFix = 1;
	inline int bGetLineOfSightCrashFix = 1;
	inline int bLockpickOwnerKarmaFix = 1;
	inline int bInlineGlyphFix = 0;
	inline int iInlineGlyphVisualScalePercent = 88;

	inline int iWitnessDetectionThreshold = 25;
	inline float fWitnessSearchRadius = 2048.0f;

	inline float fNearMissRadius = 256.0f;
	inline int iNearMissCooldownMs = 250;

	inline int bWakeyWakey = 0;
	inline float fWakeDistance = 2500.0f;
	inline float fQuietWakeDistance = 1250.0f;
	inline int iWakeCooldownMs = 250;

	inline int bAggroThreshold = 0;
	inline int bIgnoreCreatures = 0;
	inline int bOnlyCombat = 1;
	inline int iSuppressionMode = 1; //0=friend, 1=ally
	inline int bIgnoreFriendlyFire = 0;
	inline int iAllyHitNonCombatAllowed = 1;
	inline int iAllyHitCombatAllowed = 2;
	inline int iFriendHitNonCombatAllowed = 2;
	inline int iFriendHitCombatAllowed = 2;

	inline char iniPath[MAX_PATH];

	inline int GetINIInt(const char* section, const char* key, int defaultValue)
	{
		return GetPrivateProfileIntA(section, key, defaultValue, iniPath);
	}

	inline bool IntegrationEnables(const char* section, const char* key)
	{
		char dirPath[MAX_PATH];
		DWORD len = GetModuleFileNameA(nullptr, dirPath, MAX_PATH);
		if (!len || len >= MAX_PATH) return false;
		char* slash = strrchr(dirPath, '\\');
		if (!slash) return false;
		strcpy_s(slash + 1, MAX_PATH - (slash + 1 - dirPath), "Data\\config\\itr\\");

		char searchPath[MAX_PATH];
		strcpy_s(searchPath, dirPath);
		strcat_s(searchPath, "*.ini");

		WIN32_FIND_DATAA fd;
		HANDLE find = FindFirstFileA(searchPath, &fd);
		if (find == INVALID_HANDLE_VALUE) return false;

		bool enabled = false;
		do {
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
			char path[MAX_PATH];
			strcpy_s(path, dirPath);
			strcat_s(path, fd.cFileName);
			if (GetPrivateProfileIntA(section, key, 0, path)) {
				enabled = true;
				break;
			}
		} while (FindNextFileA(find, &fd));

		FindClose(find);
		return enabled;
	}

	inline void Load()
	{
		GetModuleFileNameA(nullptr, iniPath, MAX_PATH);
		char* lastSlash = strrchr(iniPath, '\\');
		if (lastSlash) *lastSlash = '\0';
		strcat_s(iniPath, "\\Data\\config\\itr-nvse.ini");

		bDebugLog = GetINIInt("Debug", "bDebugLog", 0);

		bAutoGodMode = GetINIInt("Tweaks", "bAutoGodMode", 0);
		bAutoQuickLoad = GetINIInt("Tweaks", "bAutoQuickLoad", 0);
		bMessageBoxQuickClose = GetINIInt("Tweaks", "bMessageBoxQuickClose", 1);
		bConsoleLogCleaner = GetINIInt("Tweaks", "bConsoleLogCleaner", 1);
		bAltTabMute = GetINIInt("Tweaks", "bAltTabMute", 1);
		bQuickDrop = GetINIInt("Tweaks", "bQuickDrop", 0);
		bQuick180 = GetINIInt("Tweaks", "bQuick180", 0);
		bAimZoomFirstPersonOnly = GetINIInt("Tweaks", "bAimZoomFirstPersonOnly", 0);
		bSuppressInputInConsole = GetINIInt("Tweaks", "bSuppressInputInConsole", 0);
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
		if (IntegrationEnables("Tweaks", "bVATSLimbFix"))
			bVATSLimbFix = 1;
		bOwnedBeds = GetINIInt("Tweaks", "bOwnedBeds", 0);
		bAshPileNames = GetINIInt("Tweaks", "bAshPileNames", 0);
		bLocationVisitPopup = GetINIInt("Tweaks", "bLocationVisitPopup", 0);
		iLocationVisitCooldownSeconds = GetINIInt("LocationVisitPopup", "iCooldownSeconds", 300);
		bLocationVisitDisableSound = GetINIInt("LocationVisitPopup", "bDisableSound", 0);
		bVATSExtender = GetINIInt("Tweaks", "bVATSExtender", 0);
		bVATSHighlightDepthFix = GetINIInt("Tweaks", "bVATSHighlightDepthFix", 0);
		bSuppressObjectives = GetINIInt("Tweaks", "bSuppressObjectives", 0);
		bSuppressReputation = GetINIInt("Tweaks", "bSuppressReputation", 0);
		bFriendlyFire = GetINIInt("Tweaks", "bFriendlyFire", 0);
		bNoDoorFade = GetINIInt("Tweaks", "bNoDoorFade", 0);
		bDoorPinchFix = GetINIInt("Tweaks", "bDoorPinchFix", 1);
		iDoorPinchDistance = GetINIInt("DoorPinchFix", "iDistance", 140);
		iDoorPinchTimeoutMs = GetINIInt("DoorPinchFix", "iTimeoutMs", 8000);
		bArmorDTDRFix = GetINIInt("Tweaks", "bArmorDTDRFix", 1);
		bQuickReadNote = GetINIInt("Tweaks", "bQuickReadNote", 0);
		iQuickReadNoteTimeoutMs = GetINIInt("QuickReadNote", "iTimeoutMs", 5000);
		iQuickReadNoteControlID = GetINIInt("QuickReadNote", "iControlID", 6);
		iQuickReadNoteMaxLines = GetINIInt("QuickReadNote", "iMaxLines", 0);

		bDoorPackageOwnershipFix = GetINIInt("Tweaks", "bDoorPackageOwnershipFix", 1);
		iNPCDoorUnlockBlock = GetINIInt("Tweaks", "iNPCDoorUnlockBlock", 0);
		bVATSSpeechFix = GetINIInt("Tweaks", "bVATSSpeechFix", 0);
		bCombatItemTimerFix = GetINIInt("Tweaks", "bCombatItemTimerFix", 1);
		bStuckCombatStateFix = GetINIInt("Tweaks", "bStuckCombatStateFix", 0);
		bLoopingSoundLoadFix = GetINIInt("Tweaks", "bLoopingSoundLoadFix", 0);

		bNPCAntidoteUse = GetINIInt("Tweaks", "bNPCAntidoteUse", 0);
		fCombatItemCureTimer = (float)GetINIInt("NPCAntidoteUse", "iCureTimer", 10);
		fCureHealthThreshold = (float)GetINIInt("NPCAntidoteUse", "iHealthThreshold", 25);

		bNPCDoctorsBagUse = GetINIInt("Tweaks", "bNPCDoctorsBagUse", 0);
		fDoctorsBagUseTimer = (float)GetINIInt("NPCDoctorsBagUse", "iUseTimer", 15);

		bCompanionNoInfamy = GetINIInt("Tweaks", "bCompanionNoInfamy", 0);
		bCompanionNoBlock = GetINIInt("Tweaks", "bCompanionNoBlock", 0);
		iCompanionNoBlockReleaseFrames = GetINIInt("CompanionNoBlock", "iReleaseFrames", 30);
		iCompanionNoBlockRestoreDistance = GetINIInt("CompanionNoBlock", "iRestoreDistance", 220);
		bCompanionNoBlockInteriorOnly = GetINIInt("CompanionNoBlock", "bInteriorOnly", 1);

		bPathingNullActorFix = GetINIInt("Tweaks", "bPathingNullActorFix", 1);
		bNavMeshInfoCrashFix = GetINIInt("Tweaks", "bNavMeshInfoCrashFix", 1);
		bInitHavokCrashFix = GetINIInt("Tweaks", "bInitHavokCrashFix", 1);

		bMusicResetOnLoad = GetINIInt("Tweaks", "bMusicResetOnLoad", 1);
		bOwnedCorpses = GetINIInt("Tweaks", "bOwnedCorpses", 0);
		bDetectionFollowerCrashFix = GetINIInt("Tweaks", "bDetectionFollowerCrashFix", 1);
		bGetLineOfSightCrashFix = GetINIInt("Tweaks", "bGetLineOfSightCrashFix", 1);
		bLockpickOwnerKarmaFix = GetINIInt("Tweaks", "bLockpickOwnerKarmaFix", 1);
		bInlineGlyphFix = GetINIInt("Tweaks", "bInlineGlyphFix", 0);
		iInlineGlyphVisualScalePercent = GetINIInt("Tweaks", "iInlineGlyphVisualScalePercent", 88);

		iWitnessDetectionThreshold = GetINIInt("OnWitnessed", "iDetectionThreshold", 25);
		fWitnessSearchRadius = (float)GetINIInt("OnWitnessed", "iSearchRadius", 2048);

		fNearMissRadius = (float)GetINIInt("OnNearMiss", "iRadius", 256);

		bWakeyWakey = GetINIInt("WakeyWakey", "bEnable", 0);
		fWakeDistance = (float)GetINIInt("WakeyWakey", "iWakeDistance", 2500);
		fQuietWakeDistance = (float)GetINIInt("WakeyWakey", "iQuietWakeDistance", 1250);
		iWakeCooldownMs = GetINIInt("WakeyWakey", "iCooldownMs", 250);

		bAggroThreshold = GetINIInt("AggroThreshold", "bAggroThreshold", 0);
		bIgnoreCreatures = GetINIInt("AggroThreshold", "bIgnoreCreatures", 0);
		bOnlyCombat = GetINIInt("AggroThreshold", "bOnlyCombat", 1);
		iSuppressionMode = GetINIInt("AggroThreshold", "iSuppressionMode", 1);
		bIgnoreFriendlyFire = GetINIInt("AggroThreshold", "bIgnoreFriendlyFire", 0);
		iAllyHitNonCombatAllowed = GetINIInt("AggroThreshold", "iAllyHitNonCombatAllowed", 1);
		iAllyHitCombatAllowed = GetINIInt("AggroThreshold", "iAllyHitCombatAllowed", 2);
		iFriendHitNonCombatAllowed = GetINIInt("AggroThreshold", "iFriendHitNonCombatAllowed", 2);
		iFriendHitCombatAllowed = GetINIInt("AggroThreshold", "iFriendHitCombatAllowed", 2);
		//a per-actor floor stops the forward corridor re-dispatching every projectile update
		iNearMissCooldownMs = GetINIInt("OnNearMiss", "iCooldownMs", 250);
		if (iNearMissCooldownMs < 50) iNearMissCooldownMs = 50;

	}
}
