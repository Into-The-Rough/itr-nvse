#include "ITR.h"
#include "commands/CommandTable.h"
#include "commands/DetectionSoundCommands.h"
#include "commands/BarterCommands.h"
#include "nvse/PluginAPI.h"
#include "nvse/GameAPI.h"
#include "nvse/GameObjects.h"
#include "nvse/GameForms.h"
#include "nvse/GameData.h"
#include "nvse/GameScript.h"
#include "nvse/CommandTable.h"
#include "nvse/ParamInfos.h"
#include "nvse/SafeWrite.h"
#include "internal/SafeWrite.h"
#include "internal/ScopedLock.h"

#include "internal/settings.h"
#include "internal/EngineFunctions.h"
#include "internal/GameGlobals.h"
#include "internal/EventDispatch.h"
#include "internal/ConsoleCommand.h"

#include "handlers/DialogueTextFilter.h"
#include "handlers/OnStealHandler.h"
#include "handlers/OnWeaponDropHandler.h"
#include "handlers/OnConsoleHandler.h"
#include "handlers/OnWeaponJamHandler.h"
#include "handlers/OnKeyStateHandler.h"
#include "handlers/KeyHeldHandler.h"
#include "handlers/DoubleTapHandler.h"
#include "handlers/OnFrenzyHandler.h"
#include "handlers/OnEffectHandler.h"
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
#include "handlers/OnMenuListRefreshHandler.h"
#include "handlers/OnWitnessedHandler.h"
#include "handlers/OnImpactDataSpawnHandler.h"
#include "handlers/OnNearMissHandler.h"
#include "handlers/OnProjectileImpactHandler.h"
#include "internal/ProjectileLogic.h"
#include "handlers/OnSprayDecalHandler.h"
#include "handlers/OnWoundSprayHandler.h"
#include "handlers/OnVATSStateHandler.h"
#include "handlers/OnCasinoBanHandler.h"
#include "handlers/OnPrePickUpHandler.h"
#include "handlers/OnPreWeaponSwitchHandler.h"
#include "handlers/OnKnockdownHandler.h"
#include "handlers/OnPreFastTravelHandler.h"
#include "handlers/OnTileValueChangeHandler.h"
#include "handlers/OnDialogueMenuBuildHandler.h"
#include "handlers/OnPreDeathHandler.h"
#include "handlers/OnPreDamageHandler.h"

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
#include "fixes/ItemModFlagSafety.h"
#include "fixes/DoorPackageOwnershipFix.h"
#include "fixes/NPCDoorUnlockBlock.h"
#include "fixes/VATSSpeechFix.h"
#include "fixes/CombatItemTimerFix.h"
#include "fixes/CompanionNoInfamy.h"
#include "fixes/PathingNullActorFix.h"
#include "fixes/NavMeshInfoCrashFix.h"
#include "fixes/InitHavokCrashFix.h"
#include "fixes/OwnedCorpses.h"
#include "fixes/DetectionFollowerCrashFix.h"
#include "fixes/GetLineOfSightCrashFix.h"
#include "fixes/LockpickOwnerKarmaFix.h"
#include "fixes/InlineGlyphFix.h"
#include "features/MessageBoxQuickClose.h"
#include "features/PreventWeaponSwitch.h"
#include "features/RadioInjection.h"
#include "features/WakeyWakey.h"
#include "features/ELMO.h"
#include "commands/GroundCommands.h"
#include "commands/GestureCommand.h"
#include "commands/ToggleAllPrimitives.h"
#include "commands/CenterOnCellAltCommand.h"
#include "commands/StartNewGameCommand.h"
#include "features/LocationVisitPopup.h"
#include "features/QuickReadNote.h"
#include "features/VATSExtender.h"
#include "features/VATSHighlightDepthFix.h"
#include "features/WeatherChangeEvent.h"
#include "features/CameraOverride.h"
#include "features/CompanionNoBlock.h"
#include "features/DoorPinchFix.h"
#include "features/PlayerUpdateHook.h"
#include "features/NPCAntidoteUse.h"
#include "features/NPCDoctorsBagUse.h"
#include "features/NoWeaponSearch.h"
#include "features/AutoQuickLoad.h"
#include "features/AltTabMute.h"
#include "features/PerkRuntimeFramework.h"
#include "features/AimZoomFirstPersonOnly.h"
#include "features/ConsoleInputSuppression.h"
#include "features/AggroThreshold.h"

#include "commands/ImperativeCommands.h"
#include "commands/StringCommands.h"
#include "commands/RadioCommands.h"
#include "commands/ForceCombatTargetCommands.h"
#include "commands/CrouchCommands.h"
#include "commands/ChallengeCommands.h"
#include "commands/DialogueCommands.h"
#include "commands/WeaponEmissiveCommands.h"
#include "commands/UICommands.h"
#include "commands/ActorValueCommands.h"
#include "commands/ExteriorDoorCommands.h"
#include "commands/HavokCommands.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>

#include "internal/CallTemplates.h"
#include "internal/Detours.h"

#define kMessage_MainGameLoop 20
#define kMessage_ReloadConfig 25  //sent via ReloadPluginConfig console command


const _ExtractArgs ExtractArgs = (_ExtractArgs)0x005ACCB0;
const _FormHeap_Free FormHeap_Free = (_FormHeap_Free)0x00401030;

//set true only while a user-typed console line (or bat file from console) is
//running. hooks the two MenuConsole::Idle call sites into Script::Run. neither
//bConsoleMode (TLS) nor Interface::IsConsoleVisible distinguish typed input
//from ScriptRunner / RunScriptLine, but these call sites are unique to
//MenuConsole::Idle.
static thread_local bool s_inConsoleDispatch = false;

using ConsoleScriptRun_t = int(__thiscall*)(Script*, void*, int, TESObjectREFR*);

static Detours::CallDetour s_consoleDispatchBatDetour;
static Detours::CallDetour s_consoleDispatchInputDetour;

static const char* GetScriptText(Script* script)
{
	return script ? script->text : nullptr;
}

static void DispatchConsoleCommandEvent(const char* fullCommand, TESObjectREFR* calleeRef)
{
	if (!g_eventManagerInterface || !fullCommand || !fullCommand[0])
		return;

	char commandName[128];
	if (!ConsoleCommand::ExtractCommandName(fullCommand, commandName, sizeof(commandName)))
		return;

	EventDispatch::DispatchConsoleCommand(commandName, fullCommand, calleeRef);
}

static int ConsoleScriptRunCommon(Script* script, void* scriptContext, int a3,
	TESObjectREFR* calleeRef, ConsoleScriptRun_t orig)
{
	//dispatch before raising the flag so event handlers are not misattributed to typed input
	DispatchConsoleCommandEvent(GetScriptText(script), calleeRef);
	const bool prev = s_inConsoleDispatch;
	s_inConsoleDispatch = true;
	const int result = orig(script, scriptContext, a3, calleeRef);
	s_inConsoleDispatch = prev;
	return result;
}

static int __fastcall Hook_ConsoleScriptRun_Bat(Script* script, void* /*edx*/, void* scriptContext, int a3, TESObjectREFR* calleeRef)
{
	return ConsoleScriptRunCommon(script, scriptContext, a3, calleeRef,
		(ConsoleScriptRun_t)s_consoleDispatchBatDetour.GetOverwrittenAddr());
}

static int __fastcall Hook_ConsoleScriptRun_Input(Script* script, void* /*edx*/, void* scriptContext, int a3, TESObjectREFR* calleeRef)
{
	return ConsoleScriptRunCommon(script, scriptContext, a3, calleeRef,
		(ConsoleScriptRun_t)s_consoleDispatchInputDetour.GetOverwrittenAddr());
}

bool IsConsoleMode()
{
	return s_inConsoleDispatch;
}

extern void Log(const char* fmt, ...);

static void InitConsoleDispatchHooks()
{
	const bool a = s_consoleDispatchBatDetour.WriteRelCall(0x71C3E8, (UInt32)Hook_ConsoleScriptRun_Bat);
	const bool b = s_consoleDispatchInputDetour.WriteRelCall(0x71C846, (UInt32)Hook_ConsoleScriptRun_Input);
	Log("ConsoleDispatch hooks: bat=%d input=%d", a ? 1 : 0, b ? 1 : 0);
}

typedef void* (*_GetSingleton)(bool canCreateNew);
static const _GetSingleton ConsoleManager_GetSingleton = (_GetSingleton)0x0071B160;

void Console_Print(const char* fmt, ...)
{
	void* consoleManager = ConsoleManager_GetSingleton(true);
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
	return *g_thePlayerPtr;
}

PluginHandle g_pluginHandle = kPluginHandle_Invalid;
NVSEMessagingInterface* g_msgInterface = nullptr;
NVSEConsoleInterface* g_consoleInterface = nullptr;
NVSEArrayVarInterface* g_arrInterface = nullptr;
NVSECommandTableInterface* g_cmdTableInterface = nullptr;

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

//gated by Debug\bDebugLog, all Log() calls are no-ops while closed
static void OpenDebugLog()
{
	if (Settings::bDebugLog && !g_logFile)
	{
		char logPath[MAX_PATH];
		GetModuleFileNameA(nullptr, logPath, MAX_PATH);
		char* lastSlash = strrchr(logPath, '\\');
		if (lastSlash) *lastSlash = '\0';
		strcat_s(logPath, "\\itr-nvse.log");
		fopen_s(&g_logFile, logPath, "w");
	}
	else if (!Settings::bDebugLog && g_logFile)
	{
		InitCriticalSectionOnce(&g_logLockInit, &g_logLock);
		ScopedLock lock(&g_logLock);
		FILE* f = g_logFile;
		g_logFile = nullptr;
		fclose(f);
	}
}

void Log(const char* fmt, ...)
{
	if (!g_logFile) return;
	InitCriticalSectionOnce(&g_logLockInit, &g_logLock);
	ScopedLock lock(&g_logLock);
	if (!g_logFile) return; //closed by a config reload between the check and the lock
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
		if (memcmp((void*)0xAEDFBD, VATSSpeechFix::kStewieTimescalePatch, sizeof(VATSSpeechFix::kStewieTimescalePatch)) == 0)
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
static NVSEInterface* s_nvseInterface = nullptr;

static void ApplyProjectileSettings()
{
	//clamp direct ini edits so a bad value cannot produce negative or amplified damage/speed
	auto pct = [](int v) { return (v < 0 ? 0 : (v > 100 ? 100 : v)) / 100.0f; };
	auto angle = [](int v) { return (float)(v < 0 ? 0 : (v > 90 ? 90 : v)); };

	ProjectileLogic::Config cfg = {};
	cfg.ricochetEnabled = Settings::bProjectileRicochet != 0;
	cfg.penetrationEnabled = Settings::bProjectilePenetration != 0;
	cfg.maxRicochetAngleDeg = angle(Settings::iRicochetMaxAngleDeg);
	//floor above zero: Decide gates on energy >= min, a zero minimum never terminates a decaying chain
	cfg.minRicochetEnergy = pct(Settings::iRicochetMinEnergyPct < 5 ? 5 : Settings::iRicochetMinEnergyPct);
	cfg.ricochetDamageFalloff = pct(Settings::iRicochetDamagePct);
	cfg.penetrationDamageFalloff = pct(Settings::iPenetrationDamagePct);
	//energy retention must stay below 100% or the continuation chain never decays and penetrates forever
	cfg.penetrationEnergyFalloff = pct(Settings::iPenetrationEnergyPct > 95 ? 95 : Settings::iPenetrationEnergyPct);
	OnProjectileImpactHandler::FillDefaultMaterials(cfg);
	OnProjectileImpactHandler::UpdateSettings(cfg, Settings::bMaterialProjectiles != 0);
}

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
					SaveFileSizeHandler::Init((void*)s_nvseInterface);
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
				DoorPinchFix::Init(Settings::bDoorPinchFix != 0, Settings::iDoorPinchDistance, Settings::iDoorPinchTimeoutMs);
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
				CompanionNoBlock::Init(Settings::bCompanionNoBlock != 0, Settings::iCompanionNoBlockReleaseFrames,
				                       Settings::iCompanionNoBlockRestoreDistance, Settings::bCompanionNoBlockInteriorOnly != 0);
				if (Settings::bPathingNullActorFix)
					PathingNullActorFix::Init();
				if (Settings::bNavMeshInfoCrashFix)
					NavMeshInfoCrashFix::Init();
				if (Settings::bInitHavokCrashFix)
					InitHavokCrashFix::Init();
				if (Settings::bDetectionFollowerCrashFix)
					DetectionFollowerCrashFix::Init();
				if (Settings::bGetLineOfSightCrashFix)
					GetLineOfSightCrashFix::Init();
				LockpickOwnerKarmaFix::Init(Settings::bLockpickOwnerKarmaFix != 0);
				if (Settings::bInlineGlyphFix)
					InlineGlyphFix::Init();
				AimZoomFirstPersonOnly::Init(Settings::bAimZoomFirstPersonOnly != 0);
				CrouchCommands::InstallHooks();
				ConsoleInputSuppression::InstallHook();
				ItemModFlagSafety::Init();
				ToggleAllPrimitives::InstallHooks();
				EventDispatch::RegisterEvents();
				WeatherChangeEvent::Init();
				WakeyWakey::Init(Settings::bWakeyWakey != 0, Settings::fWakeDistance,
					Settings::fQuietWakeDistance, Settings::iWakeCooldownMs);
				OnJumpLandHandler::InstallListenerProbes();
				OnSoundPlayedHandler::InstallListenerProbes();
				OnEntryPointHandler::InstallListenerProbe();
				OnCombatProcedureHandler::InstallListenerProbe();
				OnKnockdownHandler::InstallListenerProbe();
				OnPreDeathHandler::InstallListenerProbe();
				OnPreDamageHandler::InstallListenerProbe();
				OnNearMissHandler::InstallListenerProbe();
				OnProjectileImpactHandler::InstallHook();
				ApplyProjectileSettings();
				PerkRuntimeFramework::BuildIndex();
				g_hooksInstalled = true;
			}
			break;

		case NVSEMessagingInterface::kMessage_PostPostLoad:
			ItemModFlagSafety::InitJIPModFlagGate(); //after JIP's PostLoad installs GetEntryDataModFlagsHook
			DialogueCameraHandler::InstallCameraHooks(); //always install - hooks check bDialogueCamera at runtime
			InitVATSSpeechFix();
			AshPileNames::Init();
			if (Settings::bVATSExtender)
				VATSExtender::Init();
			if (Settings::bVATSHighlightDepthFix)
				VATSHighlightDepthFix::Init();
			if (Settings::bSuppressObjectives || Settings::bSuppressReputation)
				ELMO::Init(Settings::bSuppressObjectives != 0, Settings::bSuppressReputation != 0);
			break;

		case NVSEMessagingInterface::kMessage_PreLoadGame:
			g_isLoadingSave = true;
			StartNewGameCommand::Abort();
			HavokCommands::ClearState();
			DetectionSoundCommands::ClearState();
			BarterCommands::ClearState();
			CompanionNoBlock::ClearState();
			DoorPinchFix::ClearState();
			OnJumpLandHandler::ClearState();
			DialogueTextFilter::ClearState();
			OnNearMissHandler::ClearState();
			OnProjectileImpactHandler::ClearState();
			OnEffectHandler::ClearState();
			NoWeaponSearch::ClearState();
			PreventWeaponSwitch::ClearState();
			OnPreWeaponSwitchHandler::ClearState();
			OnKnockdownHandler::ClearState();
			OnTileValueChangeHandler::ClearState();
			OnDialogueMenuBuildHandler::ClearState();
			OnDialogueMenuBuildHandler::ClearRules();
			RadioInjection::ClearState();
			OwnedBeds::ClearState();
			OnWitnessedHandler::ClearState();
			OnContactHandler::ClearState();
			OnCasinoBanHandler::ClearState();
			OnVATSStateHandler::ClearState();
			OnCombatProcedureHandler::ClearState();
			OnSoundPlayedHandler::ClearState();
			FallDamageHandler::ClearState();
			DialogueCameraHandler::ClearState();
			ForceCombatTargetCommands::ClearState();
			GroundCommands::ClearState();
			WeaponEmissiveCommands::ClearState();
			ExteriorDoorCommands::ClearCache();
			GestureCommand::Reset();
			ToggleAllPrimitives::Reset();
			CrouchCommands::ClearState();
			QuickReadNote::ClearState();
			LocationVisitPopup::ClearState();
			VATSExtender::ClearState();
			break;

		case NVSEMessagingInterface::kMessage_NewGame:
		case NVSEMessagingInterface::kMessage_PostLoadGame:
			g_isLoadingSave = false;
			if (msg->type == NVSEMessagingInterface::kMessage_NewGame)
				CenterOnCellAltCommand::OnNewGame();
			else
				CenterOnCellAltCommand::ClearPending();
			if (msg->type == NVSEMessagingInterface::kMessage_PostLoadGame && Settings::bMusicResetOnLoad)
			{
				ResetMusicStateForLoad();
			}
			WeaponEmissiveCommands::ClearState();
			GroundCommands::ClearState();
			HavokCommands::ClearState();
			GestureCommand::Reset();
			CrouchCommands::ClearState();
			ForceCombatTargetCommands::ClearState();
			OnCasinoBanHandler::ClearState();
			OnContactHandler::ClearState();
			OnVATSStateHandler::ClearState();
			NoWeaponSearch::ClearState();
			PreventWeaponSwitch::ClearState();
			OnPreWeaponSwitchHandler::ClearState();
			OnKnockdownHandler::ClearState();
			OnTileValueChangeHandler::ClearState();
			OnDialogueMenuBuildHandler::ClearState();
			OnDialogueMenuBuildHandler::ClearRules();
			RadioInjection::ClearState();
			OwnedBeds::ClearState();
			OnWitnessedHandler::ClearState();
			ToggleAllPrimitives::Reset();
			ExteriorDoorCommands::ClearCache();
			DetectionSoundCommands::ClearState();
			BarterCommands::ClearState();
			CompanionNoBlock::ClearState();
			DoorPinchFix::ClearState();

			OnEntryPointHandler::BuildEntryMap();
			PerkRuntimeFramework::BuildIndex();
			OnJumpLandHandler::ClearState();
			OnJumpLandHandler::InstallListenerProbes();
			OnSoundPlayedHandler::InstallListenerProbes();
			OnEntryPointHandler::InstallListenerProbe();
			OnCombatProcedureHandler::InstallListenerProbe();
			OnKnockdownHandler::InstallListenerProbe();
			OnPreDeathHandler::InstallListenerProbe();
			OnPreDamageHandler::InstallListenerProbe();
			OnNearMissHandler::InstallListenerProbe();
			OnProjectileImpactHandler::InstallHook();
			DialogueTextFilter::ClearState();
			OnNearMissHandler::ClearState();
			OnProjectileImpactHandler::ClearState();
			OnEffectHandler::ClearState();
			OnCombatProcedureHandler::ClearState();
			OnSoundPlayedHandler::ClearState();
			FallDamageHandler::ClearState();
			DialogueCameraHandler::ClearState();
			QuickReadNote::ClearState();
			LocationVisitPopup::ClearState();
			VATSExtender::ClearState();
			if (Settings::bAutoGodMode && !g_godModeExecuted)
			{
				SetGodModeEnabled(true);
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
					OpenDebugLog();
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
					DoorPinchFix::UpdateSettings(Settings::bDoorPinchFix != 0, Settings::iDoorPinchDistance, Settings::iDoorPinchTimeoutMs);
					ApplyVATSSpeechFixSetting();
					ReversePickpocketNoKarmaFix::SetEnabled(Settings::bReversePickpocketNoKarma != 0);
					ApplyProjectileSettings();
					CompanionNoInfamy::SetEnabled(Settings::bCompanionNoInfamy != 0);
					CompanionNoBlock::UpdateSettings(Settings::bCompanionNoBlock != 0, Settings::iCompanionNoBlockReleaseFrames,
					                                 Settings::iCompanionNoBlockRestoreDistance, Settings::bCompanionNoBlockInteriorOnly != 0);
					NPCDoorUnlockBlock::SetLevel(Settings::iNPCDoorUnlockBlock);
					WakeyWakey::UpdateSettings(Settings::bWakeyWakey != 0, Settings::fWakeDistance, Settings::fQuietWakeDistance, Settings::iWakeCooldownMs);
					NPCAntidoteUse::UpdateSettings(Settings::bNPCAntidoteUse != 0, Settings::fCombatItemCureTimer, Settings::fCureHealthThreshold);
					NPCDoctorsBagUse::UpdateSettings(Settings::bNPCDoctorsBagUse != 0, Settings::fDoctorsBagUseTimer);
					LockpickOwnerKarmaFix::SetEnabled(Settings::bLockpickOwnerKarmaFix != 0);
					InlineGlyphFix::SetEnabled(Settings::bInlineGlyphFix != 0);
					AggroThreshold::SetEnabled(Settings::bAggroThreshold != 0);
					AimZoomFirstPersonOnly::SetEnabled(Settings::bAimZoomFirstPersonOnly != 0);
					if (Settings::bAutoQuickLoad)
						AutoQuickLoad::InstallHook(); //idempotent, covers enabling at runtime

					if (*g_thePlayerPtr)
					{
						if (Settings::bAutoGodMode && !oldGodMode)
						{
							SetGodModeEnabled(true);
							Console_Print("itr-nvse: God mode enabled");
						}
						else if (!Settings::bAutoGodMode && oldGodMode)
						{
							SetGodModeEnabled(false);
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
			OnCasinoBanHandler::Update();
			OnCombatProcedureHandler::Update();
			OnPreWeaponSwitchHandler::Update();
			OnKnockdownHandler::Update();
			OnPreDeathHandler::Update();
			OnPreDamageHandler::Update();
			OnEntryPointHandler::Update();
			OnNearMissHandler::Update();
			OnProjectileImpactHandler::Update();
			OnEffectHandler::Update();
			CompanionNoBlock::Update();
			OnContactHandler::Update();
			OnTileValueChangeHandler::Update();
			OnWitnessedHandler::Update();
			RadioInjection::Update();
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
			HavokCommands::Update();
			DoorPinchFix::Update();
			GestureCommand::Update();
			ToggleAllPrimitives::Update();
			DetectionSoundCommands::Update();
			CenterOnCellAltCommand::Update();
			StartNewGameCommand::Update();
			break;
	}
}

static void RegisterHandlers(NVSEInterface* nvse)
{
	auto logInit = [](const char* name, bool ok) {
		Log(ok ? "%s initialised" : "%s failed to initialise", name);
	};

	InitConsoleDispatchHooks();
	logInit("DialogueTextFilter", DialogueTextFilter::Init((void*)nvse));
	logInit("OnStealHandler", OnStealHandler::Init((void*)nvse));
	logInit("OnWeaponDropHandler", OnWeaponDropHandler::Init((void*)nvse));
	logInit("OnConsoleHandler", OnConsoleHandler::Init((void*)nvse));
	logInit("OnWeaponJamHandler", OnWeaponJamHandler::Init((void*)nvse));
	logInit("OnKeyStateHandler", OnKeyStateHandler::Init((void*)nvse));
	logInit("ConsoleInputSuppression", ConsoleInputSuppression::Init((void*)nvse));
	logInit("KeyHeldHandler", KeyHeldHandler::Init());
	logInit("DoubleTapHandler", DoubleTapHandler::Init());
	logInit("OnFrenzyHandler", OnFrenzyHandler::Init((void*)nvse));
	logInit("OnEffectHandler", OnEffectHandler::Init((void*)nvse));
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
	logInit("OnMenuListRefreshHandler", OnMenuListRefreshHandler::Init((void*)nvse));
	logInit("OnWitnessedHandler", OnWitnessedHandler::Init((void*)nvse));
	logInit("OnImpactDataSpawnHandler", OnImpactDataSpawnHandler::Init((void*)nvse));
	logInit("OnNearMissHandler", OnNearMissHandler::Init((void*)nvse));
	logInit("OnProjectileImpactHandler", OnProjectileImpactHandler::Init((void*)nvse));
	logInit("OnSprayDecalHandler", OnSprayDecalHandler::Init((void*)nvse));
	logInit("OnWoundSprayHandler", OnWoundSprayHandler::Init((void*)nvse));
	logInit("OnVATSStateHandler", OnVATSStateHandler::Init((void*)nvse));
	logInit("OnCasinoBanHandler", OnCasinoBanHandler::Init((void*)nvse));
	logInit("OnPrePickUpHandler", OnPrePickUpHandler::Init((void*)nvse));
	logInit("OnPreFastTravelHandler", OnPreFastTravelHandler::Init((void*)nvse));
	logInit("OnKnockdownHandler", OnKnockdownHandler::Init((void*)nvse));
	logInit("OnPreDeathHandler", OnPreDeathHandler::Init((void*)nvse));
	logInit("OnPreDamageHandler", OnPreDamageHandler::Init((void*)nvse));
	logInit("OnTileValueChangeHandler", OnTileValueChangeHandler::Init((void*)nvse));
	logInit("OnDialogueMenuBuildHandler", OnDialogueMenuBuildHandler::Init((void*)nvse));
	logInit("BarterCommands", BarterCommands::InitHooks());
	NoWeaponSearch::Init();
	bool preSwitchOk = OnPreWeaponSwitchHandler::Init((void*)nvse);
	logInit("OnPreWeaponSwitchHandler", preSwitchOk);
	if (!preSwitchOk)
		Log("OnPreWeaponSwitchHandler: 0x9DA7C0 detour offline - SetPreventWeaponSwitch will NOT be enforced this session");
	PreventWeaponSwitch::Init();
	OnPreWeaponSwitchHandler::SetExternalBlockCheck(&PreventWeaponSwitch::Get);
	RadioInjection::Init((void*)nvse);
	logInit("PerkRuntimeFramework", PerkRuntimeFramework::Init((void*)nvse));
	AggroThreshold::Init(Settings::bAggroThreshold != 0);
}

namespace ITR
{
	bool Init(void* nvsePtr)
	{
		NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
		s_nvseInterface = nvse;

		g_pluginHandle = nvse->GetPluginHandle();

		g_msgInterface = (NVSEMessagingInterface*)nvse->QueryInterface(kInterface_Messaging);
		g_consoleInterface = (NVSEConsoleInterface*)nvse->QueryInterface(kInterface_Console);
		g_arrInterface = (NVSEArrayVarInterface*)nvse->QueryInterface(kInterface_ArrayVar);
		g_cmdTableInterface = (NVSECommandTableInterface*)nvse->QueryInterface(kInterface_CommandTable);

		if (!g_msgInterface || !g_arrInterface)
			return false;

		Settings::Load();
		OpenDebugLog();

		if (Settings::bAutoQuickLoad)
			AutoQuickLoad::InstallHook();

		if (Settings::bConsoleLogCleaner)
			DeleteConsoleLog();

		if (Settings::bMessageBoxQuickClose)
			MessageBoxQuickClose::Init();

		g_msgInterface->RegisterListener(g_pluginHandle, "NVSE", MessageHandler);

		EventDispatch::InitEventManager((void*)nvse);
		CameraOverride::Init();
		ImperativeCommands::Init((void*)nvse);
		ForceCombatTargetCommands::Init();
		StringCommands::Init((void*)nvse);
		RadioCommands::Init((void*)nvse);
		RegisterHandlers(nvse);

		return true;
	}
}
