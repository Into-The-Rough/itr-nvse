#include "ITR.h"
#include "commands/CommandTable.h"
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
#include "handlers/OnContactHandler.h"
#include "handlers/OnSoundPlayedHandler.h"
#include "handlers/OnJumpLandHandler.h"
#include "handlers/FallDamageHandler.h"
#include "handlers/DialogueCameraHandler.h"
#include "handlers/FakeHitHandler.h"
#include "handlers/SaveFileSizeHandler.h"
#include "handlers/OwnerNameInfoHandler.h"
#include "handlers/OnMenuFilterChangeHandler.h"
#include "handlers/OnMenuSideChangeHandler.h"
#include "handlers/OnWitnessedHandler.h"
#include "handlers/OnImpactDataSpawnHandler.h"
#include "handlers/OnSprayDecalHandler.h"

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
#include "commands/GroundCommands.h"
#include "commands/GestureCommand.h"
#include "commands/ToggleAllPrimitives.h"
#include "features/LocationVisitPopup.h"
#include "features/QuickReadNote.h"
#include "features/VATSExtender.h"
#include "features/CameraOverride.h"
#include "features/PlayerUpdateHook.h"
#include "features/NPCAntidoteUse.h"
#include "features/NPCDoctorsBagUse.h"
#include "features/NoWeaponSearch.h"
#include "features/AutoQuickLoad.h"
#include "features/AltTabMute.h"

#include "commands/ImperativeCommands.h"
#include "commands/StringCommands.h"
#include "commands/RadioCommands.h"
#include "commands/ChallengeCommands.h"
#include "commands/DialogueCommands.h"
#include "commands/WeaponEmissiveCommands.h"
#include "commands/UICommands.h"
#include "commands/ActorValueCommands.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>

#include "internal/CallTemplates.h"

#define kMessage_MainGameLoop 20
#define kMessage_ReloadConfig 25  //sent via ReloadPluginConfig console command


const _ExtractArgs ExtractArgs = (_ExtractArgs)0x005ACCB0;
const _FormHeap_Free FormHeap_Free = (_FormHeap_Free)0x00401030;

struct TLSData {
	UInt32 pad000[(0x260 - 0x000) >> 2];
	void* lastNiNode;
	TESObjectREFR* lastNiNodeREFR;
	UInt8 consoleMode;
	UInt8 pad269[3];
};
static_assert(offsetof(TLSData, consoleMode) == 0x268);

static UInt32* g_TlsIndexPtr = (UInt32*)0x0126FD98;
static UInt8* g_consoleOpen = (UInt8*)0x11DEA2E;

static TLSData* GetTLSData()
{
	return (TLSData*)__readfsdword(0x2C + (*g_TlsIndexPtr * 4));
}

bool IsConsoleMode()
{
	if (!*g_consoleOpen)
		return false;

	TLSData* tlsData = GetTLSData();
	return tlsData ? tlsData->consoleMode != 0 : false;
}

typedef void* (*_GetSingleton)(bool canCreateNew);
static const _GetSingleton ConsoleManager_GetSingleton = (_GetSingleton)0x0071B160;

void Console_Print(const char* fmt, ...)
{
	if (!IsConsoleMode())
		return;

	void* consoleManager = ConsoleManager_GetSingleton(false);
	if (!consoleManager)
		return;

	va_list args;
	va_start(args, fmt);
	// 0x71D0A0 is MenuConsole::Print(fmt, va_list), not a simple Print(const char*).
	typedef void (__thiscall *_ConsolePrint)(void*, char*, va_list);
	((_ConsolePrint)0x0071D0A0)(consoleManager, const_cast<char*>(fmt), args);
	va_end(args);
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

static bool g_godModeExecuted = false;
bool g_isLoadingSave = false;

void Log(const char* fmt, ...); //forward decl

typedef void (__cdecl *_StopPlayingMusic)();
static const _StopPlayingMusic StopPlayingMusic = (_StopPlayingMusic)0x8304A0;
typedef void (__cdecl *_MusicClearStopFlags)();
static const _MusicClearStopFlags MusicClearStopFlags = (_MusicClearStopFlags)0x8304C0;
typedef void (__cdecl *_PlayingMusicClearPauseAll)();
static const _PlayingMusicClearPauseAll PlayingMusicClearPauseAll = (_PlayingMusicClearPauseAll)0x830660;

static void ResetMusicStateForLoad()
{
	StopPlayingMusic();
	MusicClearStopFlags();
	PlayingMusicClearPauseAll();
}

static FILE* g_logFile = nullptr;
static CRITICAL_SECTION g_logLock;
static volatile LONG g_logLockInit = 0;
static bool g_vatsSpeechFixInitialized = false;
static bool g_vatsSpeechFixDisabledByStewie = false;

void Log(const char* fmt, ...)
{
	if (!g_logFile) return;
	InitCriticalSectionOnce(&g_logLockInit, &g_logLock);
	ScopedLock lock(&g_logLock);
	va_list args;
	va_start(args, fmt);
	vfprintf(g_logFile, fmt, args);
	fprintf(g_logFile, "\n");
	fflush(g_logFile);
	va_end(args);
}

static void InitVATSSpeechFix()
{
	g_vatsSpeechFixDisabledByStewie = false;
	if (GetModuleHandleA("nvse_stewie_tweaks.dll"))
	{
		static const UInt8 kStewieTimescalePatch[] = {
			0xD9, 0xE1, 0x66, 0x66, 0x66, 0x66, 0x0F,
			0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00
		};

		if (memcmp((void*)0xAEDFBD, kStewieTimescalePatch, sizeof(kStewieTimescalePatch)) == 0)
		{
			g_vatsSpeechFixDisabledByStewie = true;
		}
		else
		{
			char gameDir[MAX_PATH];
			GetModuleFileNameA(nullptr, gameDir, MAX_PATH);
			char* lastSlash = strrchr(gameDir, '\\');
			if (!lastSlash)
			{
				g_vatsSpeechFixDisabledByStewie = true;
			}
			else
			{
				*lastSlash = '\0';

				char stewieIniPath[MAX_PATH];
				sprintf_s(stewieIniPath, "%s\\Data\\NVSE\\Plugins\\nvse_stewie_tweaks.ini", gameDir);
				g_vatsSpeechFixDisabledByStewie = GetPrivateProfileIntA("Inlines", "bAudio", 1, stewieIniPath) != 0;
			}
		}
	}

	if (g_vatsSpeechFixDisabledByStewie)
	{
		Log("VATSSpeechFix disabled: Stewie Tweaks Inlines.bAudio owns audio hooks");
		return;
	}

	VATSSpeechFix::Init(Settings::bVATSSpeechFix != 0);
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
		VATSSpeechFix::SetEnabled(Settings::bVATSSpeechFix != 0);
}

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
					PlayerUpdateHook::Init(Settings::bQuickDrop, Settings::iQuickDropModifierKey, Settings::iQuickDropControlID,
					                       Settings::bQuick180, Settings::iQuick180ModifierKey, Settings::iQuick180ControlID);
				if (Settings::bSlowMotionPhysicsFix)
					SlowMotionPhysicsFix::Init();
				if (Settings::bExplodingPantsFix)
					ExplodingPantsFix::Init();
				KillActorXPFix::Init(Settings::bKillActorXPFix != 0);
				ReversePickpocketNoKarmaFix::Init(Settings::bReversePickpocketNoKarma != 0);
				if (Settings::bSaveFileSize)
					SaveFileSizeHandler::Init();
				if (Settings::bVATSProjectileFix)
					VATSProjectileFix::Init();
				if (Settings::bVATSLimbFix)
					VATSLimbFix::Init();
				OwnedBeds::Init(Settings::bOwnedBeds != 0);
				OwnedCorpses::Init(Settings::bOwnedCorpses != 0);
				if (Settings::bLocationVisitPopup)
					LocationVisitPopup::Init(Settings::iLocationVisitCooldownSeconds, Settings::bLocationVisitDisableSound != 0);
				FriendlyFire::Init(Settings::bFriendlyFire != 0);
				NoDoorFade::Init(Settings::bNoDoorFade != 0);
				if (Settings::bArmorDTDRFix)
					ArmorDTDRFix::Init();
				if (Settings::bQuickReadNote)
					QuickReadNote::Init(Settings::iQuickReadNoteTimeoutMs, Settings::iQuickReadNoteControlID, Settings::iQuickReadNoteMaxLines);
				if (Settings::bDoorPackageOwnershipFix)
					DoorPackageOwnershipFix::Init();
				NPCDoorUnlockBlock::Init(Settings::iNPCDoorUnlockBlock);
				if (Settings::bCombatItemTimerFix)
					CombatItemTimerFix::Init();
				if (Settings::bNPCAntidoteUse)
					NPCAntidoteUse::Init(Settings::fCombatItemCureTimer, Settings::fCureHealthThreshold);
				if (Settings::bNPCDoctorsBagUse)
					NPCDoctorsBagUse::Init(Settings::fDoctorsBagUseTimer);
				CompanionNoInfamy::Init(Settings::bCompanionNoInfamy != 0);
				CompanionWeightlessOverencumberedFix::Init(Settings::bCompanionWeightlessOverencumberedFix != 0);
				if (Settings::bPathingNullActorFix)
					PathingNullActorFix::Init();
				if (Settings::bNavMeshInfoCrashFix)
					NavMeshInfoCrashFix::Init();
				if (Settings::bInitHavokCrashFix)
					InitHavokCrashFix::Init();
				if (Settings::bDetectionFollowerCrashFix)
					DetectionFollowerCrashFix::Init();
				EventDispatch::RegisterEvents();
				g_hooksInstalled = true;
			}
			break;

		case NVSEMessagingInterface::kMessage_PostPostLoad:
			DialogueCameraHandler::InstallCameraHooks(); //always install - hooks check bDialogueCamera at runtime
			InitVATSSpeechFix();
			AshPileNames::Init();
			if (Settings::bVATSExtender)
				VATSExtender::Init();
			if (Settings::bSuppressObjectives || Settings::bSuppressReputation)
				ELMO::Init(Settings::bSuppressObjectives != 0, Settings::bSuppressReputation != 0);
			break;

		case NVSEMessagingInterface::kMessage_PreLoadGame:
			g_isLoadingSave = true;
			break;

		case NVSEMessagingInterface::kMessage_NewGame:
		case NVSEMessagingInterface::kMessage_PostLoadGame:
			g_isLoadingSave = false;
			if (msg->type == NVSEMessagingInterface::kMessage_PostLoadGame && Settings::bMusicResetOnLoad)
			{
				ResetMusicStateForLoad();
			}
			WeaponEmissiveCommands::ClearState();
			GroundCommands::ClearState();
			GestureCommand::Reset();
			ImperativeCommands::ClearState();
			OnContactHandler::ClearState();
			ToggleAllPrimitives::Reset();

			OnEntryPointHandler::BuildEntryMap();
			if (Settings::bAutoGodMode && !g_godModeExecuted)
			{
				*(UInt8*)0x11E07BA = 1;
				g_godModeExecuted = true;
			}
			break;

		case kMessage_ReloadConfig:
			if (msg->data && msg->dataLen > 0)
			{
				const char* pluginName = (const char*)msg->data;
				if (_stricmp(pluginName, "itr-nvse") == 0)
				{
					bool oldGodMode = Settings::bAutoGodMode;
					bool oldSuppressObjectives = Settings::bSuppressObjectives != 0;
					bool oldSuppressReputation = Settings::bSuppressReputation != 0;
					Settings::Load();
					DialogueCameraHandler::SetEnabled(Settings::bDialogueCamera != 0);

					LocationVisitPopup::UpdateSettings(Settings::iLocationVisitCooldownSeconds, Settings::bLocationVisitDisableSound != 0);

					if (Settings::bQuickDrop || Settings::bQuick180)
						PlayerUpdateHook::UpdateSettings(Settings::iQuickDropModifierKey, Settings::iQuickDropControlID,
						                                 Settings::iQuick180ModifierKey, Settings::iQuick180ControlID);

					OwnerNameInfoHandler::UpdateSettings();

					if (Settings::bQuickReadNote)
						QuickReadNote::UpdateSettings(Settings::iQuickReadNoteTimeoutMs, Settings::iQuickReadNoteControlID, Settings::iQuickReadNoteMaxLines);

					FriendlyFire::SetEnabled(Settings::bFriendlyFire != 0);
					OwnedBeds::SetEnabled(Settings::bOwnedBeds != 0);
					OwnedCorpses::SetEnabled(Settings::bOwnedCorpses != 0);
					KillActorXPFix::SetEnabled(Settings::bKillActorXPFix != 0);
					NoDoorFade::SetEnabled(Settings::bNoDoorFade != 0);
					ApplyVATSSpeechFixSetting();
					ReversePickpocketNoKarmaFix::SetEnabled(Settings::bReversePickpocketNoKarma != 0);
					CompanionNoInfamy::SetEnabled(Settings::bCompanionNoInfamy != 0);
					CompanionWeightlessOverencumberedFix::SetEnabled(Settings::bCompanionWeightlessOverencumberedFix != 0);
					NPCDoorUnlockBlock::SetLevel(Settings::iNPCDoorUnlockBlock);

					if (*g_thePlayer)
					{
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
					}

					if ((Settings::bSuppressObjectives != 0) != oldSuppressObjectives ||
						(Settings::bSuppressReputation != 0) != oldSuppressReputation)
					{
						Console_Print("itr-nvse: Suppress Objectives/Reputation changes require restart");
					}

					Console_Print("itr-nvse: Config reloaded");
				}
			}
			break;

		case kMessage_MainGameLoop:
			AshPileNames::Update();
			OnConsoleHandler::Update();
			DialogueTextFilter::Update();
			if (Settings::bLocationVisitPopup)
				LocationVisitPopup::Update();
			OwnerNameInfoHandler::Update();
			KeyHeldHandler::Update();
			DoubleTapHandler::Update();
			OnSoundPlayedHandler::Update();
			OnJumpLandHandler::Update();
			OnCombatProcedureHandler::Update();
			OnContactHandler::Update();
			OnMenuFilterChangeHandler::Update();
			OnMenuSideChangeHandler::Update();
			if (Settings::bQuickReadNote)
				QuickReadNote::Update();
			if (Settings::bDialogueCamera)
				DialogueCameraHandler::Update();
			AutoQuickLoad::Update();
			if (Settings::bAltTabMute)
				AltTabMute::Update();
			GroundCommands::Update();
			ImperativeCommands::Update();
			GestureCommand::Update();
			ToggleAllPrimitives::Update();
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


static void RegisterHandlers(NVSEInterface* nvse)
{
	auto logInit = [](const char* name, bool ok) {
		Log(ok ? "%s initialized" : "%s failed to initialize", name);
	};

	logInit("DialogueTextFilter", DialogueTextFilter::Init((void*)nvse));
	logInit("OnStealHandler", OnStealHandler::Init((void*)nvse));
	logInit("OnWeaponDropHandler", OnWeaponDropHandler::Init((void*)nvse));
	logInit("OnConsoleHandler", OnConsoleHandler::Init((void*)nvse));
	logInit("OnWeaponJamHandler", OnWeaponJamHandler::Init((void*)nvse));
	logInit("OnKeyStateHandler", OnKeyStateHandler::Init((void*)nvse));
	logInit("KeyHeldHandler", KeyHeldHandler::Init());
	logInit("DoubleTapHandler", DoubleTapHandler::Init());
	logInit("OnFrenzyHandler", OnFrenzyHandler::Init((void*)nvse));
	logInit("CornerMessageHandler", CornerMessageHandler::Init((void*)nvse));
	logInit("OnEntryPointHandler", OnEntryPointHandler::Init((void*)nvse));
	logInit("OnCombatProcedureHandler", OnCombatProcedureHandler::Init((void*)nvse));
	logInit("OnSoundPlayedHandler", OnSoundPlayedHandler::Init((void*)nvse));
	logInit("OnJumpLandHandler", OnJumpLandHandler::Init((void*)nvse));
	logInit("OnContactHandler", OnContactHandler::Init((void*)nvse));
	logInit("FallDamageHandler", FallDamageHandler::Init((void*)nvse));
	if (Settings::bDialogueCamera)
		logInit("DialogueCameraHandler", DialogueCameraHandler::Init((void*)nvse));
	logInit("FakeHitHandler", FakeHitHandler::Init((void*)nvse));
	if (Settings::bOwnerNameInfo)
		logInit("OwnerNameInfoHandler", OwnerNameInfoHandler::Init());
	logInit("OnMenuFilterChangeHandler", OnMenuFilterChangeHandler::Init((void*)nvse));
	logInit("OnMenuSideChangeHandler", OnMenuSideChangeHandler::Init((void*)nvse));
	logInit("OnWitnessedHandler", OnWitnessedHandler::Init((void*)nvse));
	logInit("OnImpactDataSpawnHandler", OnImpactDataSpawnHandler::Init((void*)nvse));
	logInit("OnSprayDecalHandler", OnSprayDecalHandler::Init((void*)nvse));
	NoWeaponSearch::Init();
	PreventWeaponSwitch::Init();
}

namespace ITR
{
	bool Init(void* nvsePtr)
	{
		NVSEInterface* nvse = (NVSEInterface*)nvsePtr;

		g_pluginHandle = nvse->GetPluginHandle();

		InitLog();

		g_msgInterface = (NVSEMessagingInterface*)nvse->QueryInterface(kInterface_Messaging);
		g_consoleInterface = (NVSEConsoleInterface*)nvse->QueryInterface(kInterface_Console);
		g_arrInterface = (NVSEArrayVarInterface*)nvse->QueryInterface(kInterface_ArrayVar);
		g_cmdTableInterface = (NVSECommandTableInterface*)nvse->QueryInterface(kInterface_CommandTable);

		if (!g_msgInterface || !g_arrInterface)
			return false;

		Settings::Load();

		if (Settings::bAutoQuickLoad)
			AutoQuickLoad::InstallHook();

		if (Settings::bConsoleLogCleaner)
			DeleteConsoleLog();

		if (Settings::bMessageBoxQuickClose)
			MessageBoxQuickClose::Init();

		g_msgInterface->RegisterListener(g_pluginHandle, "NVSE", MessageHandler);

		EventDispatch::InitEventManager((void*)nvse);
		ImperativeCommands::Init((void*)nvse);
		StringCommands::Init((void*)nvse);
		RadioCommands::Init((void*)nvse);
		RegisterHandlers(nvse);

		return true;
	}
}
