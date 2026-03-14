#include "ITR.h"
#include "nvse/PluginAPI.h"
#include "nvse/GameAPI.h"
#include "nvse/GameObjects.h"
#include "nvse/GameForms.h"
#include "nvse/GameData.h"
#include "nvse/CommandTable.h"
#include "nvse/ParamInfos.h"
#include "nvse/SafeWrite.h"
#include "internal/SafeWrite.h"
#include "internal/ScopedLock.h"

#include "internal/settings.h"
#include "internal/EngineFunctions.h"
#include "internal/EventDispatch.h"

#include "handlers/DialogueTextFilter.h"
#include "handlers/OnStealHandler.h"
#include "handlers/OnWeaponDropHandler.h"
#include "handlers/OnConsoleHandler.h"
#include "handlers/OnWeaponJamHandler.h"
#include "handlers/OnKeyStateHandler.h"
#include "handlers/KeyHeldHandler.h"
#include "handlers/DoubleTapHandler.h"
#include "handlers/OnFrenzyHandler.h"
#include "handlers/CornerMessageHandler.h"
#include "handlers/OnEntryPointHandler.h"
#include "handlers/OnCombatProcedureHandler.h"
#include "handlers/OnSoundPlayedHandler.h"
#include "handlers/OnJumpLandHandler.h"
#include "handlers/FallDamageHandler.h"
#include "handlers/DialogueCameraHandler.h"
#include "handlers/FakeHitHandler.h"
#include "handlers/SaveFileSizeHandler.h"
#include "handlers/OwnerNameInfoHandler.h"
#include "handlers/OnMenuFilterChangeHandler.h"
#include "handlers/OnMenuSideChangeHandler.h"

#include "fixes/SlowMotionPhysicsFix.h"
#include "fixes/VATSProjectileFix.h"
#include "fixes/VATSLimbFix.h"
#include "fixes/KillActorXPFix.h"
#include "fixes/ExplodingPantsFix.h"
#include "fixes/OwnedBeds.h"
#include "fixes/AshPileNames.h"
#include "fixes/ReversePickpocketNoKarmaFix.h"
#include "fixes/FriendlyFire.h"
#include "fixes/NoDoorFade.h"
#include "fixes/ArmorDTDRFix.h"
#include "fixes/DoorPackageOwnershipFix.h"
#include "fixes/NPCDoorUnlockBlock.h"
#include "fixes/VATSSpeechFix.h"
#include "fixes/CombatItemTimerFix.h"
#include "fixes/CompanionNoInfamy.h"
#include "fixes/CompanionWeightlessOverencumberedFix.h"
#include "fixes/PathingNullActorFix.h"
#include "fixes/NavMeshInfoCrashFix.h"
#include "fixes/InitHavokCrashFix.h"
#include "fixes/OwnedCorpses.h"
#include "fixes/DetectionFollowerCrashFix.h"
#include "features/MessageBoxQuickClose.h"
#include "features/PreventWeaponSwitch.h"
#include "features/ELMO.h"
#include "features/LocationVisitPopup.h"
#include "features/QuickReadNote.h"
#include "features/VATSExtender.h"
#include "features/CameraOverride.h"
#include "features/PlayerUpdateHook.h"
#include "features/NPCAntidoteUse.h"
#include "features/NPCDoctorsBagUse.h"

#include "commands/ImperativeCommands.h"
#include "commands/StringCommands.h"
#include "commands/RadioCommands.h"
#include "commands/ChallengeCommands.h"
#include "commands/DialogueCommands.h"
#include "commands/WeaponEmissiveCommands.h"
#include "commands/UICommands.h"
#include "commands/ActorValueCommands.h"

#include <cstdio>
#include <cstring>

#include "internal/CallTemplates.h"

#define kMessage_MainGameLoop 20
#define kMessage_ReloadConfig 25  //sent via ReloadPluginConfig console command

#ifndef kMenuType_Start
#define kMenuType_Start 0x3F5
#endif

const _ExtractArgs ExtractArgs = (_ExtractArgs)0x005ACCB0;
const _FormHeap_Free FormHeap_Free = (_FormHeap_Free)0x00401030;

struct TLSData {
	UInt32 unk000[1257];
	bool consoleMode;
};

static UInt32* g_TlsIndexPtr = (UInt32*)0x0126FD98;

static TLSData* GetTLSData()
{
	return (TLSData*)__readfsdword(0x2C + (*g_TlsIndexPtr * 4));
}

bool IsConsoleMode()
{
	TLSData* tlsData = GetTLSData();
	return tlsData ? tlsData->consoleMode : false;
}

typedef void* (*_GetSingleton)(bool canCreateNew);
static const _GetSingleton ConsoleManager_GetSingleton = (_GetSingleton)0x0071B160;

void Console_Print(const char* fmt, ...)
{
	void* consoleManager = ConsoleManager_GetSingleton(true);
	if (!consoleManager) return;

	va_list args;
	va_start(args, fmt);
	char buf[4096];
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	typedef void (__thiscall *_ConsolePrint)(void*, const char*);
	((_ConsolePrint)0x0071D0A0)(consoleManager, buf);
}

PlayerCharacter* PlayerCharacter::GetSingleton()
{
	return *(PlayerCharacter**)0x011DEA3C;
}

PluginHandle g_pluginHandle = kPluginHandle_Invalid;
NVSEMessagingInterface* g_msgInterface = nullptr;
NVSEConsoleInterface* g_consoleInterface = nullptr;
NVSEArrayVarInterface* g_arrInterface = nullptr;
NVSECommandTableInterface* g_cmdTableInterface = nullptr;
static PlayerCharacter** g_thePlayer = (PlayerCharacter**)0x011DEA3C;
static UInt8* g_MenuVisibilityArray = (UInt8*)0x011F308F;

static bool g_godModeExecuted = false;
static bool g_quickLoadDone = false;
static DWORD g_quickLoadStartTime = 0;

void Log(const char* fmt, ...); //forward decl

typedef void (__thiscall *_PollControls)(void*);
static const _PollControls PollControls = (_PollControls)0x86F390;

//hooked at 0x86E88C - injects F9 keypress AFTER PollControls reads hardware
//so the game sees it as a real keypress when it checks GetUserAction(QuickLoad)
void __fastcall PollControlsHook(void* tesMain, void* edx)
{
	PollControls(tesMain);
	if (g_quickLoadDone == false && g_quickLoadStartTime)
	{
		if ((GetTickCount() - g_quickLoadStartTime) >= (DWORD)Settings::iAutoQuickLoadDelayMs)
		{
			//DIK_F9=0x43, currKeyStates at +0x18F8
			auto input = *(UInt8**)0x11F35CC;
			if (input) input[0x18F8 + 0x43] = 0x80;
			g_quickLoadDone = true;
			Log("AutoQuickLoad: injected F9 (after %dms)", GetTickCount() - g_quickLoadStartTime);
		}
	}
}

typedef void (__cdecl *_StopPlayingMusic)();
static const _StopPlayingMusic StopPlayingMusic = (_StopPlayingMusic)0x8304A0;
typedef void (__cdecl *_MusicClearStopFlags)();
static const _MusicClearStopFlags MusicClearStopFlags = (_MusicClearStopFlags)0x8304C0;
typedef void (__cdecl *_PlayingMusicClearPauseAll)();
static const _PlayingMusicClearPauseAll PlayingMusicClearPauseAll = (_PlayingMusicClearPauseAll)0x830660;

constexpr UInt32 kNumVolumeChannels = 12;
#define INI_MUSIC_VOLUME_ADDR 0x11F6E44

struct BSAudioManager
{
	void* vtable;
	UInt8 pad004[0x13C];
	float volumes[kNumVolumeChannels];

	static BSAudioManager* Get() { return (BSAudioManager*)0x11F6EF0; }
};

static HWND g_gameWindow = nullptr;
static bool g_wasInFocus = true;
static float g_savedVolumes[kNumVolumeChannels] = {0};
static float g_savedIniMusicVolume = 0.0f;
static bool g_volumesSaved = false;

static void ResetMusicStateForLoad()
{
	StopPlayingMusic();
	MusicClearStopFlags();
	PlayingMusicClearPauseAll();
}

static void OnFocusLost()
{
	BSAudioManager* audioMgr = BSAudioManager::Get();
	for (UInt32 i = 0; i < kNumVolumeChannels; i++)
	{
		g_savedVolumes[i] = audioMgr->volumes[i];
		audioMgr->volumes[i] = 0.0f;
	}
	float* iniMusicVolume = (float*)INI_MUSIC_VOLUME_ADDR;
	g_savedIniMusicVolume = *iniMusicVolume;
	*iniMusicVolume = 0.0f;
	g_volumesSaved = true;
}

static void OnFocusGained()
{
	if (!g_volumesSaved) return;
	BSAudioManager* audioMgr = BSAudioManager::Get();
	for (UInt32 i = 0; i < kNumVolumeChannels; i++)
	{
		audioMgr->volumes[i] = g_savedVolumes[i];
	}
	float* iniMusicVolume = (float*)INI_MUSIC_VOLUME_ADDR;
	*iniMusicVolume = g_savedIniMusicVolume;
}

static FILE* g_logFile = nullptr;
static bool g_vatsSpeechFixInitialized = false;
static bool g_vatsSpeechFixDisabledByStewie = false;

void Log(const char* fmt, ...)
{
	if (!g_logFile) return;
	va_list args;
	va_start(args, fmt);
	vfprintf(g_logFile, fmt, args);
	fprintf(g_logFile, "\n");
	fflush(g_logFile);
	va_end(args);
}

static bool IsStewieAudioInlineActive()
{
	if (!GetModuleHandleA("nvse_stewie_tweaks.dll"))
		return false;

	static const UInt8 kStewieTimescalePatch[] = {
		0xD9, 0xE1, 0x66, 0x66, 0x66, 0x66, 0x0F,
		0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00
	};

	if (memcmp((void*)0xAEDFBD, kStewieTimescalePatch, sizeof(kStewieTimescalePatch)) == 0)
		return true;

	char gameDir[MAX_PATH];
	GetModuleFileNameA(nullptr, gameDir, MAX_PATH);
	char* lastSlash = strrchr(gameDir, '\\');
	if (!lastSlash)
		return true;
	*lastSlash = '\0';

	char stewieIniPath[MAX_PATH];
	sprintf_s(stewieIniPath, "%s\\Data\\NVSE\\Plugins\\nvse_stewie_tweaks.ini", gameDir);
	return GetPrivateProfileIntA("Inlines", "bAudio", 1, stewieIniPath) != 0;
}

static void InitVATSSpeechFixWithCompatibility()
{
	g_vatsSpeechFixDisabledByStewie = IsStewieAudioInlineActive();
	if (g_vatsSpeechFixDisabledByStewie)
	{
		Log("VATSSpeechFix disabled: Stewie Tweaks Inlines.bAudio owns audio hooks");
		return;
	}

	VATSSpeechFix_Init(Settings::bVATSSpeechFix != 0);
	g_vatsSpeechFixInitialized = true;
}

static void ApplyVATSSpeechFixSetting()
{
	if (g_vatsSpeechFixDisabledByStewie)
	{
		if (Settings::bVATSSpeechFix)
			Log("VATSSpeechFix remains disabled: Stewie Tweaks Inlines.bAudio owns audio hooks");
		return;
	}

	if (g_vatsSpeechFixInitialized)
		VATSSpeechFix_SetEnabled(Settings::bVATSSpeechFix != 0);
}

static bool IsGameLoading()
{
	//BGSSaveLoadManager singleton at 0x11DE134, bIsLoadingGame at offset 0x26
	void* mgr = *(void**)0x11DE134;
	if (!mgr) return false;
	return *(bool*)((char*)mgr + 0x26);
}

namespace NoWeaponSearch
{
	static const int MAX_DISABLED = 64;
	static UInt32 g_disabled[MAX_DISABLED] = {0};
	static int g_count = 0;
	static CRITICAL_SECTION g_lock;
	static volatile LONG g_lockInit = 0;

	static void EnsureLockInit()
	{
		InitCriticalSectionOnce(&g_lockInit, &g_lock);
	}

	typedef bool (__thiscall *CombatItemSearch_t)(void* combatState);
	CombatItemSearch_t Original = (CombatItemSearch_t)0x99F6D0;

	static bool IsDisabled_Unlocked(UInt32 refID)
	{
		for (int i = 0; i < g_count; i++)
			if (g_disabled[i] == refID)
				return true;
		return false;
	}

	bool IsDisabled(UInt32 refID)
	{
		ScopedLock lock(&g_lock);
		return IsDisabled_Unlocked(refID);
	}

	bool __fastcall Hook(void* combatState, void* edx)
	{
		if (IsGameLoading())
			return Original(combatState);

		//bail if actor isn't fully loaded (cell transition)
		void* controller = *(void**)((char*)combatState + 0x1C4);
		if (!controller)
			return Original(combatState);
		Actor* actor = (Actor*)Engine::CombatController_GetPackageOwner(controller);
		if (!actor || !*(void**)((char*)actor + 0x68) || !*(void**)((char*)actor + 0x64))
			return Original(combatState);

		if (Settings::bNPCAntidoteUse)
			NPCAntidoteUse_Check(combatState);
		if (Settings::bNPCDoctorsBagUse)
			NPCDoctorsBagUse_Check(combatState);

		bool isDisabled = false;
		{
			ScopedLock lock(&g_lock);
			if (g_count > 0 && IsDisabled_Unlocked(actor->refID))
				isDisabled = true;
		}

		if (isDisabled)
			return false;

		return Original(combatState);
	}

	void Set(Actor* actor, bool disable)
	{
		if (!actor) return;
		UInt32 refID = actor->refID;

		ScopedLock lock(&g_lock);
		if (disable)
		{
			if (IsDisabled_Unlocked(refID)) return;
			if (g_count < MAX_DISABLED)
				g_disabled[g_count++] = refID;
		}
		else
		{
			for (int i = 0; i < g_count; i++)
			{
				if (g_disabled[i] == refID)
				{
					g_disabled[i] = g_disabled[--g_count];
					g_disabled[g_count] = 0;
					break;
				}
			}
		}
	}

	bool Get(Actor* actor)
	{
		if (!actor) return false;
		ScopedLock lock(&g_lock);
		return IsDisabled_Unlocked(actor->refID);
	}

	void Init()
	{
		EnsureLockInit();
		SafeWrite::WriteRelCall(0x998D50, (UInt32)Hook);
		Log("NoWeaponSearch: Hook installed at 0x998D50");
	}
}

static ParamInfo kParams_SetNoWeaponSearch[1] = {
	{"disable", kParamType_Integer, 0}
};

inline bool IsActorRef(TESObjectREFR* ref) {
	if (!ref) return false;
	return ThisCall<bool>(*(UInt32*)(*(UInt32*)ref + 0x100), ref);
}

bool Cmd_SetNoWeaponSearch_Execute(COMMAND_ARGS)
{
	*result = 0;
	UInt32 disable = 0;
	if (!ExtractArgs(EXTRACT_ARGS, &disable))
		return true;

	if (IsActorRef(thisObj))
	{
		NoWeaponSearch::Set((Actor*)thisObj, disable != 0);
		*result = 1;
	}
	return true;
}

bool Cmd_GetNoWeaponSearch_Execute(COMMAND_ARGS)
{
	*result = 0;
	if (IsActorRef(thisObj))
		*result = NoWeaponSearch::Get((Actor*)thisObj) ? 1 : 0;
	return true;
}

static CommandInfo kCommandInfo_SetNoWeaponSearch = {
	"SetNoWeaponSearch", "", 0, "Disable weapon searching for actor",
	1, 1, kParams_SetNoWeaponSearch, Cmd_SetNoWeaponSearch_Execute, nullptr, nullptr, 0
};

static CommandInfo kCommandInfo_GetNoWeaponSearch = {
	"GetNoWeaponSearch", "", 0, "Check if weapon searching is disabled",
	1, 0, nullptr, Cmd_GetNoWeaponSearch_Execute, nullptr, nullptr, 0
};

static void DeleteConsoleLog()
{
	char gameDir[MAX_PATH];
	GetModuleFileNameA(nullptr, gameDir, MAX_PATH);
	char* lastSlash = strrchr(gameDir, '\\');
	if (lastSlash) *lastSlash = '\0';

	char iniPath[MAX_PATH];
	sprintf_s(iniPath, "%s\\Data\\NVSE\\Plugins\\nvse_stewie_tweaks.ini", gameDir);

	char logName[256];
	GetPrivateProfileStringA("Main", "sConsoleOutputFile", "consoleout.txt",
	                         logName, sizeof(logName), iniPath);

	char logPath[MAX_PATH];
	sprintf_s(logPath, "%s\\%s", gameDir, logName);
	DeleteFileA(logPath);
}

static bool g_hooksInstalled = false;

static void MessageHandler(NVSEMessagingInterface::Message* msg)
{
	switch (msg->type)
	{
		case NVSEMessagingInterface::kMessage_PostLoad:
			if (!g_hooksInstalled)
			{
				if (Settings::bQuickDrop || Settings::bQuick180)
					PlayerUpdateHook_Init(Settings::bQuickDrop, Settings::iQuickDropModifierKey, Settings::iQuickDropControlID,
					                      Settings::bQuick180, Settings::iQuick180ModifierKey, Settings::iQuick180ControlID);
				if (Settings::bSlowMotionPhysicsFix)
					SlowMotionPhysicsFix_Init();
				if (Settings::bExplodingPantsFix)
					ExplodingPantsFix_Init();
				KillActorXPFix_Init(Settings::bKillActorXPFix != 0);
				ReversePickpocketNoKarmaFix_Init(Settings::bReversePickpocketNoKarma != 0);
				if (Settings::bSaveFileSize)
					SFSH_Init();
				if (Settings::bVATSProjectileFix)
					VATSProjectileFix_Init();
				if (Settings::bVATSLimbFix)
					VATSLimbFix_Init();
				OwnedBeds_Init(Settings::bOwnedBeds != 0);
				OwnedCorpses_Init(Settings::bOwnedCorpses != 0);
				if (Settings::bLocationVisitPopup)
					LocationVisitPopup_Init(Settings::iLocationVisitCooldownSeconds, Settings::bLocationVisitDisableSound != 0);
				FriendlyFire_Init(Settings::bFriendlyFire != 0);
				NoDoorFade_Init(Settings::bNoDoorFade != 0);
				if (Settings::bArmorDTDRFix)
					ArmorDTDRFix_Init();
				if (Settings::bQuickReadNote)
					QuickReadNote_Init(Settings::iQuickReadNoteTimeoutMs, Settings::iQuickReadNoteControlID, Settings::iQuickReadNoteMaxLines);
				if (Settings::bDoorPackageOwnershipFix)
					DoorPackageOwnershipFix_Init();
				NPCDoorUnlockBlock_Init(Settings::iNPCDoorUnlockBlock);
				if (Settings::bCombatItemTimerFix)
					CombatItemTimerFix_Init();
				if (Settings::bNPCAntidoteUse)
					NPCAntidoteUse_Init(Settings::fCombatItemCureTimer, Settings::fCureHealthThreshold);
				if (Settings::bNPCDoctorsBagUse)
					NPCDoctorsBagUse_Init(Settings::fDoctorsBagUseTimer);
				CompanionNoInfamy_Init(Settings::bCompanionNoInfamy != 0);
				CompanionWeightlessOverencumberedFix_Init(Settings::bCompanionWeightlessOverencumberedFix != 0);
				if (Settings::bPathingNullActorFix)
					PathingNullActorFix_Init();
				if (Settings::bNavMeshInfoCrashFix)
					NavMeshInfoCrashFix_Init();
				if (Settings::bInitHavokCrashFix)
					InitHavokCrashFix_Init();
				if (Settings::bDetectionFollowerCrashFix)
					DetectionFollowerCrashFix_Init();
				ITR_RegisterEvents();
				g_hooksInstalled = true;
			}
			break;

			case NVSEMessagingInterface::kMessage_PostPostLoad:
				DCH_InstallCameraHooks(); //always install - used by CameraOverride and DialogueCamera
				InitVATSSpeechFixWithCompatibility();
				AshPileNames_Init();
				if (Settings::bVATSExtender)
					VATSExtender_Init();
				if (Settings::bSuppressObjectives || Settings::bSuppressReputation)
					ELMO_Init(Settings::bSuppressObjectives != 0, Settings::bSuppressReputation != 0);
				break;

			case NVSEMessagingInterface::kMessage_NewGame:
			case NVSEMessagingInterface::kMessage_PostLoadGame:
				if (msg->type == NVSEMessagingInterface::kMessage_PostLoadGame && Settings::bMusicResetOnLoad)
				{
					ResetMusicStateForLoad();
					Log("Music state reset for post-load");
				}
				WeaponEmissive_ClearState();

				OEPH_BuildEntryMap();
				if (Settings::bAutoGodMode && !g_godModeExecuted)
				{
					*(UInt8*)0x11E07BA = 1;
					g_godModeExecuted = true;
					Log("AutoGodMode: Enabled god mode");
				}
				break;

		case kMessage_ReloadConfig:
			if (msg->data && msg->dataLen > 0)
			{
				const char* pluginName = (const char*)msg->data;
				if (_stricmp(pluginName, "itr-nvse") == 0)
				{
					bool oldGodMode = Settings::bAutoGodMode;
					Settings::Load();

					LocationVisitPopup_UpdateSettings(Settings::iLocationVisitCooldownSeconds, Settings::bLocationVisitDisableSound != 0);

					if (Settings::bQuickDrop || Settings::bQuick180)
						PlayerUpdateHook_UpdateSettings(
						    Settings::iQuickDropModifierKey,
                            Settings::iQuickDropControlID,
						    Settings::iQuick180ModifierKey,
						    Settings::iQuick180ControlID
                        );

					if (Settings::bOwnerNameInfo)
						ONI_UpdateSettings();

					if (Settings::bQuickReadNote)
						QuickReadNote_UpdateSettings(Settings::iQuickReadNoteTimeoutMs, Settings::iQuickReadNoteControlID, Settings::iQuickReadNoteMaxLines);

					FriendlyFire_SetEnabled(Settings::bFriendlyFire != 0);
					OwnedBeds_SetEnabled(Settings::bOwnedBeds != 0);
					OwnedCorpses_SetEnabled(Settings::bOwnedCorpses != 0);
					KillActorXPFix_SetEnabled(Settings::bKillActorXPFix != 0);
					NoDoorFade_SetEnabled(Settings::bNoDoorFade != 0);
					ApplyVATSSpeechFixSetting();
					ReversePickpocketNoKarmaFix_SetEnabled(Settings::bReversePickpocketNoKarma != 0);
					CompanionNoInfamy_SetEnabled(Settings::bCompanionNoInfamy != 0);
					CompanionWeightlessOverencumberedFix_SetEnabled(Settings::bCompanionWeightlessOverencumberedFix != 0);
					NPCDoorUnlockBlock_SetLevel(Settings::iNPCDoorUnlockBlock);

					//apply god mode immediately if setting changed
					if (Settings::bAutoGodMode && !oldGodMode)
					{
						*(UInt8*)0x11E07BA = 1;
						Console_Print("itr-nvse: God mode enabled");
					}
					else if (!Settings::bAutoGodMode && oldGodMode)
					{
						*(UInt8*)0x11E07BA = 0;
						Console_Print("itr-nvse: God mode disabled");
					}

					Log("Config reloaded via ReloadPluginConfig");
					Console_Print("itr-nvse: Config reloaded");
				}
			}
			break;

        case kMessage_MainGameLoop:
            AshPileNames_Update();
            OCH_Update();
			DTF_Update();
			if (Settings::bLocationVisitPopup)
				LocationVisitPopup_Update();
			ONI_Update();
			KHH_Update();
			DTH_Update();
			OSPH_Update();
			OJLH_Update();
			OCPH_Update();
			OMFCH_Update();
			OMSCH_Update();
			if (Settings::bQuickReadNote)
				QuickReadNote_Update();
			if (Settings::bDialogueCamera)
				DCH_Update();
			if (Settings::bAutoQuickLoad && !g_quickLoadDone && !g_quickLoadStartTime)
			{
				if (g_MenuVisibilityArray[kMenuType_Start])
				{
					g_quickLoadStartTime = GetTickCount();
					Log("AutoQuickLoad: start menu detected, loading in %dms", Settings::iAutoQuickLoadDelayMs);
				}
			}
			if (Settings::bAltTabMute)
			{
				if (!g_gameWindow)
				{
					g_gameWindow = FindWindowA(nullptr, "Fallout: New Vegas");
				}
				if (g_gameWindow)
				{
					bool currentlyInFocus = (GetForegroundWindow() == g_gameWindow);
					if (currentlyInFocus != g_wasInFocus)
					{
						if (!currentlyInFocus)
							OnFocusLost();
						else
							OnFocusGained();
						g_wasInFocus = currentlyInFocus;
					}
				}
			}
			break;
	}
}

static void InitLog()
{
	char logPath[MAX_PATH];
	GetModuleFileNameA(GetModuleHandleA("itr-nvse.dll"), logPath, MAX_PATH);
	char* lastSlash = strrchr(logPath, '\\');
	if (lastSlash) strcpy_s(lastSlash + 1, MAX_PATH - (lastSlash + 1 - logPath), "itr-nvse.log");
	g_logFile = fopen(logPath, "w");
}

static void LogSettings()
{
	Log("Settings loaded:");
	Log("  bAutoGodMode: %d", Settings::bAutoGodMode);
	Log("  bAutoQuickLoad: %d", Settings::bAutoQuickLoad);
	Log("  bMessageBoxQuickClose: %d", Settings::bMessageBoxQuickClose);
	Log("  bConsoleLogCleaner: %d", Settings::bConsoleLogCleaner);
	Log("  bAltTabMute: %d", Settings::bAltTabMute);
	Log("  bQuickDrop: %d", Settings::bQuickDrop);
	Log("  bQuick180: %d", Settings::bQuick180);
	Log("  bSlowMotionPhysicsFix: %d", Settings::bSlowMotionPhysicsFix);
	Log("  bExplodingPantsFix: %d", Settings::bExplodingPantsFix);
	Log("  bKillActorXPFix: %d", Settings::bKillActorXPFix);
	Log("  bReversePickpocketNoKarma: %d", Settings::bReversePickpocketNoKarma);
	Log("  bOwnerNameInfo: %d", Settings::bOwnerNameInfo);
	Log("  bDialogueCamera: %d", Settings::bDialogueCamera);
	Log("  bVATSProjectileFix: %d", Settings::bVATSProjectileFix);
	Log("  bVATSLimbFix: %d", Settings::bVATSLimbFix);
	Log("  bOwnedBeds: %d", Settings::bOwnedBeds);
	Log("  bAshPileNames: %d", Settings::bAshPileNames);
	Log("  bLocationVisitPopup: %d", Settings::bLocationVisitPopup);
	Log("  bVATSExtender: %d", Settings::bVATSExtender);
	Log("  bSuppressObjectives: %d", Settings::bSuppressObjectives);
	Log("  bSuppressReputation: %d", Settings::bSuppressReputation);
	Log("  bNoDoorFade: %d", Settings::bNoDoorFade);
	Log("  bQuickReadNote: %d", Settings::bQuickReadNote);
	Log("  bDoorPackageOwnershipFix: %d", Settings::bDoorPackageOwnershipFix);
	Log("  iNPCDoorUnlockBlock: %d", Settings::iNPCDoorUnlockBlock);
	Log("  bVATSSpeechFix: %d", Settings::bVATSSpeechFix);
	Log("  bCombatItemTimerFix: %d", Settings::bCombatItemTimerFix);
	Log("  bNPCAntidoteUse: %d", Settings::bNPCAntidoteUse);
	Log("  bNPCDoctorsBagUse: %d", Settings::bNPCDoctorsBagUse);
	Log("  bCompanionNoInfamy: %d", Settings::bCompanionNoInfamy);
	Log("  bOwnedCorpses: %d", Settings::bOwnedCorpses);
	Log("  bCompanionWeightlessOverencumberedFix: %d", Settings::bCompanionWeightlessOverencumberedFix);
	if (Settings::bQuickDrop) Log("QuickDrop enabled (modifier=%d, control=%d)", Settings::iQuickDropModifierKey, Settings::iQuickDropControlID);
	if (Settings::bQuick180) Log("Quick180 enabled (modifier=%d, control=%d)", Settings::iQuick180ModifierKey, Settings::iQuick180ControlID);
	if (Settings::bNPCAntidoteUse) Log("NPCAntidoteUse enabled (timer=%.1f, healthThreshold=%.1f)", Settings::fCombatItemCureTimer, Settings::fCureHealthThreshold);
	if (Settings::bNPCDoctorsBagUse) Log("NPCDoctorsBagUse enabled (timer=%.1f)", Settings::fDoctorsBagUseTimer);
}

static void RegisterHandlers(NVSEInterface* nvse)
{
	if (DTF_Init((void*)nvse))
		Log("DialogueTextFilter initialized");
	else
		Log("DialogueTextFilter failed to initialize");

	if (OSH_Init((void*)nvse))
		Log("OnStealHandler initialized");
	else
		Log("OnStealHandler failed to initialize");

	if (OWDH_Init((void*)nvse))
		Log("OnWeaponDropHandler initialized");
	else
		Log("OnWeaponDropHandler failed to initialize");

	if (OCH_Init((void*)nvse))
		Log("OnConsoleHandler initialized");
	else
		Log("OnConsoleHandler failed to initialize");

	if (OWJH_Init((void*)nvse))
		Log("OnWeaponJamHandler initialized");
	else
		Log("OnWeaponJamHandler failed to initialize");

	if (OKSH_Init((void*)nvse))
		Log("OnKeyStateHandler initialized");
	else
		Log("OnKeyStateHandler failed to initialize");

	if (KHH_Init())
		Log("KeyHeldHandler initialized");
	else
		Log("KeyHeldHandler failed to initialize");

	if (DTH_Init())
		Log("DoubleTapHandler initialized");
	else
		Log("DoubleTapHandler failed to initialize");

	if (OFH_Init((void*)nvse))
		Log("OnFrenzyHandler initialized");
	else
		Log("OnFrenzyHandler failed to initialize");

	if (CMH_Init((void*)nvse))
		Log("CornerMessageHandler initialized");
	else
		Log("CornerMessageHandler failed to initialize");

	nvse->SetOpcodeBase(0x401D);
	CameraOverride_RegisterCommands(nvse);
	Log("Registered SetCameraAngle at opcode 0x401D");

	if (OEPH_Init((void*)nvse))
		Log("OnEntryPointHandler initialized");
	else
		Log("OnEntryPointHandler failed to initialize");

	if (OCPH_Init((void*)nvse))
		Log("OnCombatProcedureHandler initialized");
	else
		Log("OnCombatProcedureHandler failed to initialize");

	if (OSPH_Init((void*)nvse))
		Log("OnSoundPlayedHandler initialized");
	else
		Log("OnSoundPlayedHandler failed to initialize");

	if (OJLH_Init((void*)nvse))
		Log("OnJumpLandHandler initialized");
	else
		Log("OnJumpLandHandler failed to initialize");

	if (FDH_Init((void*)nvse))
		Log("FallDamageHandler initialized (SetMult=0x%04X, GetMult=0x%04X)", FDH_GetSetMultOpcode(), FDH_GetGetMultOpcode());
	else
		Log("FallDamageHandler failed to initialize");

	if (Settings::bDialogueCamera)
	{
		if (DCH_Init((void*)nvse))
			Log("DialogueCameraHandler initialized");
		else
			Log("DialogueCameraHandler failed to initialize");
	}

	if (FakeHit_Init((void*)nvse))
		Log("FakeHitHandler initialized");
	else
		Log("FakeHitHandler failed to initialize");

	if (Settings::bOwnerNameInfo)
	{
		if (ONI_Init())
			Log("OwnerNameInfoHandler initialized");
		else
			Log("OwnerNameInfoHandler failed to initialize");
	}

	if (OMFCH_Init((void*)nvse))
		Log("OnMenuFilterChangeHandler initialized");
	else
		Log("OnMenuFilterChangeHandler failed to initialize");

	if (OMSCH_Init((void*)nvse))
		Log("OnMenuSideChangeHandler initialized");
	else
		Log("OnMenuSideChangeHandler failed to initialize");

	if (Settings::bSaveFileSize)
		Log("SaveFileSizeHandler will initialize in PostLoad");

	NoWeaponSearch::Init();
	nvse->SetOpcodeBase(0x402A);
	nvse->RegisterCommand(&kCommandInfo_SetNoWeaponSearch);
	nvse->RegisterCommand(&kCommandInfo_GetNoWeaponSearch);
	Log("Registered SetNoWeaponSearch/GetNoWeaponSearch at 0x402A-0x402B");

	PreventWeaponSwitch_Init();
	nvse->SetOpcodeBase(0x402C);
	PreventWeaponSwitch_RegisterCommands(nvse);
	Log("Registered SetPreventWeaponSwitch/GetPreventWeaponSwitch at 0x402C-0x402D");
}

namespace ITR
{
	bool Init(void* nvsePtr)
	{
		NVSEInterface* nvse = (NVSEInterface*)nvsePtr;

		g_pluginHandle = nvse->GetPluginHandle();

		InitLog();
		Log("itr-nvse v1.0.0 loading...");

		g_msgInterface = (NVSEMessagingInterface*)nvse->QueryInterface(kInterface_Messaging);
		g_consoleInterface = (NVSEConsoleInterface*)nvse->QueryInterface(kInterface_Console);
		g_arrInterface = (NVSEArrayVarInterface*)nvse->QueryInterface(kInterface_ArrayVar);
		g_cmdTableInterface = (NVSECommandTableInterface*)nvse->QueryInterface(kInterface_CommandTable);

		if (!g_msgInterface || !g_arrInterface)
		{
			Log("Failed to get required interfaces");
			return false;
		}

		Settings::Load();
		LogSettings();

		if (Settings::bAutoQuickLoad)
		{
			SafeWrite::WriteRelCall(0x86E88C, (UInt32)PollControlsHook);
			Log("AutoQuickLoad: hooked PollControls, delay=%dms", Settings::iAutoQuickLoadDelayMs);
		}

		if (Settings::bConsoleLogCleaner)
		{
			DeleteConsoleLog();
			Log("ConsoleLogCleaner: Deleted console log");
		}

		if (Settings::bMessageBoxQuickClose)
		{
			MBQC_Init();
			Log("MessageBoxQuickClose enabled");
		}

		g_msgInterface->RegisterListener(g_pluginHandle, "NVSE", MessageHandler);

		ITR_InitEventManager((void*)nvse);
		ImperativeCommands_Init((void*)nvse);
		StringCommands_Init((void*)nvse);
		RadioCommands_Init((void*)nvse);
		ChallengeCommands_Init((void*)nvse);
		DialogueCommands_Init((void*)nvse);
		WeaponEmissiveCommands_Init((void*)nvse);
		UICommands_Init((void*)nvse);
		ActorValueCommands_Init((void*)nvse);
		RegisterHandlers(nvse);

		Log("itr-nvse loaded successfully");
		return true;
	}
}
