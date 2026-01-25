#include "ITR.h"
#include "nvse/PluginAPI.h"
#include "nvse/GameAPI.h"
#include "nvse/GameObjects.h"
#include "nvse/GameForms.h"
#include "nvse/GameData.h"
#include "nvse/CommandTable.h"
#include "nvse/ParamInfos.h"
#include "nvse/SafeWrite.h"

#include "internal/settings.h"

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
#include "handlers/OnFastTravelHandler.h"
#include "handlers/FallDamageHandler.h"
#include "handlers/DialogueCameraHandler.h"
#include "handlers/FakeHitHandler.h"
#include "handlers/SaveFileSizeHandler.h"
#include "handlers/OwnerNameInfoHandler.h"

#include "fixes/SlowMotionPhysicsFix.h"
#include "fixes/VATSProjectileFix.h"
#include "fixes/VATSLimbFix.h"
#include "fixes/KillActorXPFix.h"
#include "fixes/ExplodingPantsFix.h"
#include "fixes/OwnedBeds.h"
#include "fixes/AshPileNames.h"
#include "fixes/ReversePickpocketNoKarmaFix.h"

#include "features/MessageBoxQuickClose.h"
#include "features/PreventWeaponSwitch.h"
#include "features/ELMO.h"
#include "features/LocationVisitPopup.h"
#include "features/VATSExtender.h"
#include "features/CameraOverride.h"
#include "features/PlayerUpdateHook.h"

#include "commands/ImperativeCommands.h"

#include <cstdio>
#include <cstring>

template <typename T_Ret = uint32_t, typename ...Args>
__forceinline T_Ret ThisCall(uint32_t _addr, const void* _this, Args ...args) {
	return ((T_Ret(__thiscall*)(const void*, Args...))_addr)(_this, std::forward<Args>(args)...);
}

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

static PlayerCharacter** g_thePlayer = (PlayerCharacter**)0x011DEA3C;
static UInt8* g_MenuVisibilityArray = (UInt8*)0x011F308F;
static SaveGameManager** g_saveGameManager = (SaveGameManager**)0x011DE134;

static bool g_godModeExecuted = false;
static bool g_quickLoadExecuted = false;
static int g_framesOnMenu = 0;

typedef bool (__thiscall *_LoadQuicksave)(SaveGameManager* mgr);
static const _LoadQuicksave LoadQuicksave = (_LoadQuicksave)0x8509F0;

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

namespace NoWeaponSearch
{
	static const int MAX_DISABLED = 64;
	volatile UInt32 g_disabled[MAX_DISABLED] = {0};
	volatile int g_count = 0;

	typedef bool (__thiscall *CombatItemSearch_t)(void* combatState);
	CombatItemSearch_t Original = (CombatItemSearch_t)0x99F6D0;

	bool IsDisabled(UInt32 refID)
	{
		for (int i = 0; i < g_count; i++)
			if (g_disabled[i] == refID)
				return true;
		return false;
	}

	typedef Actor* (__thiscall *GetPackageOwner_t)(void* controller);
	GetPackageOwner_t GetPackageOwner = (GetPackageOwner_t)0x97AE90;

	bool __fastcall Hook(void* combatState, void* edx)
	{
		if (g_count == 0)
			return Original(combatState);

		void* controller = *(void**)((char*)combatState + 0x1C4);
		if (controller)
		{
			Actor* actor = GetPackageOwner(controller);
			if (actor && IsDisabled(actor->refID))
				return false;
		}

		return Original(combatState);
	}

	void Set(Actor* actor, bool disable)
	{
		if (!actor) return;
		UInt32 refID = actor->refID;

		if (disable)
		{
			if (IsDisabled(refID)) return;
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
		return actor && IsDisabled(actor->refID);
	}

	void WriteRelCall(UInt32 src, UInt32 dst)
	{
		DWORD oldProtect;
		VirtualProtect((void*)src, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
		*(UInt8*)src = 0xE8;
		*(UInt32*)(src + 1) = dst - src - 5;
		VirtualProtect((void*)src, 5, oldProtect, &oldProtect);
	}

	void Init()
	{
		WriteRelCall(0x998D50, (UInt32)Hook);
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
				if (Settings::bKillActorXPFix)
					KillActorXPFix_Init();
				if (Settings::bReversePickpocketNoKarma)
					ReversePickpocketNoKarmaFix_Init();
				if (Settings::bSaveFileSize)
					SFSH_Init();
				if (Settings::bVATSProjectileFix)
					VATSProjectileFix_Init();
				if (Settings::bVATSLimbFix)
					VATSLimbFix_Init();
				if (Settings::bOwnedBeds)
					OwnedBeds_Init();
				if (Settings::bLocationVisitPopup)
					LocationVisitPopup_Init(Settings::iLocationVisitCooldownSeconds, Settings::bLocationVisitDisableSound != 0);
				g_hooksInstalled = true;
			}
			break;

		case NVSEMessagingInterface::kMessage_PostPostLoad:
			if (Settings::bDialogueCamera)
				DCH_InstallCameraHooks();
			if (Settings::bAshPileNames)
				AshPileNames_Init();
			if (Settings::bVATSExtender)
				VATSExtender_Init();
			if (Settings::bSuppressObjectives || Settings::bSuppressReputation)
				ELMO_Init(Settings::bSuppressObjectives != 0, Settings::bSuppressReputation != 0);
			break;

		case NVSEMessagingInterface::kMessage_NewGame:
		case NVSEMessagingInterface::kMessage_PostLoadGame:
			OEPH_BuildEntryMap();
			if (Settings::bAutoGodMode && !g_godModeExecuted)
			{
				*(UInt8*)0x11E07BA = 1;
				g_godModeExecuted = true;
				Log("AutoGodMode: Enabled god mode");
			}
			break;

		case kMessage_ReloadConfig:
			//dataLen = length of plugin name, data = const char* pluginName
			if (msg->data && msg->dataLen > 0)
			{
				const char* pluginName = (const char*)msg->data;
				if (_stricmp(pluginName, "itr-nvse") == 0)
				{
					bool oldGodMode = Settings::bAutoGodMode;
					Settings::Load();

					//update runtime settings
					LocationVisitPopup_UpdateSettings(Settings::iLocationVisitCooldownSeconds, Settings::bLocationVisitDisableSound != 0);

					if (Settings::bQuickDrop || Settings::bQuick180)
						PlayerUpdateHook_UpdateSettings(Settings::iQuickDropModifierKey, Settings::iQuickDropControlID,
						                                Settings::iQuick180ModifierKey, Settings::iQuick180ControlID);

					if (Settings::bOwnerNameInfo)
						ONI_UpdateSettings();

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
					Console_Print("  Hot-reloaded: LocationVisit, QuickDrop/180 keys, OwnerNameInfo, AltTabMute, DialogueCamera");
					Console_Print("  Requires restart: Fixes, hooks (bSlowMotionPhysicsFix, bOwnedBeds, etc)");
				}
			}
			break;

		case kMessage_MainGameLoop:
			CameraOverride_Init();
			ONI_Update();
			KHH_Update();
			DTH_Update();
			OSPH_Update();
			if (Settings::bDialogueCamera)
				DCH_Update();
			if (Settings::bAutoQuickLoad && !g_quickLoadExecuted)
			{
				if (g_MenuVisibilityArray[kMenuType_Start])
				{
					g_framesOnMenu++;
					if (g_framesOnMenu > Settings::iAutoQuickLoadFrameDelay)
					{
						if (SaveGameManager* sgm = *g_saveGameManager)
						{
							LoadQuicksave(sgm);
							g_quickLoadExecuted = true;
							Log("AutoQuickLoad: Loaded quicksave");
						}
					}
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
	Log("  iAutoQuickLoadFrameDelay: %d", Settings::iAutoQuickLoadFrameDelay);

	if (Settings::bQuickDrop) Log("QuickDrop enabled (modifier=%d, control=%d)", Settings::iQuickDropModifierKey, Settings::iQuickDropControlID);
	if (Settings::bQuick180) Log("Quick180 enabled (modifier=%d, control=%d)", Settings::iQuick180ModifierKey, Settings::iQuick180ControlID);
}

static void RegisterHandlers(NVSEInterface* nvse)
{
	if (DTF_Init((void*)nvse))
		Log("DialogueTextFilter module initialized (opcode 0x%04X)", DTF_GetOpcode());
	else
		Log("DialogueTextFilter module failed to initialize");

	if (OSH_Init((void*)nvse))
		Log("OnStealHandler module initialized (opcode 0x%04X)", OSH_GetOpcode());
	else
		Log("OnStealHandler module failed to initialize");

	if (OWDH_Init((void*)nvse))
		Log("OnWeaponDropHandler module initialized (opcode 0x%04X)", OWDH_GetOpcode());
	else
		Log("OnWeaponDropHandler module failed to initialize");

	if (OCH_Init((void*)nvse))
		Log("OnConsoleHandler module initialized (open=0x%04X, close=0x%04X)", OCH_GetOpenOpcode(), OCH_GetCloseOpcode());
	else
		Log("OnConsoleHandler module failed to initialize");

	if (OWJH_Init((void*)nvse))
		Log("OnWeaponJamHandler module initialized (opcode 0x%04X)", OWJH_GetOpcode());
	else
		Log("OnWeaponJamHandler module failed to initialize");

	if (OKSH_Init((void*)nvse))
		Log("OnKeyStateHandler module initialized (disabled=0x%04X, enabled=0x%04X)", OKSH_GetDisabledOpcode(), OKSH_GetEnabledOpcode());
	else
		Log("OnKeyStateHandler module failed to initialize");

	if (KHH_Init((void*)nvse))
		Log("KeyHeldHandler module initialized");
	else
		Log("KeyHeldHandler module failed to initialize");

	if (DTH_Init((void*)nvse))
		Log("DoubleTapHandler module initialized");
	else
		Log("DoubleTapHandler module failed to initialize");

	if (OFH_Init((void*)nvse))
		Log("OnFrenzyHandler module initialized (opcode 0x%04X)", OFH_GetOpcode());
	else
		Log("OnFrenzyHandler module failed to initialize");

	if (CMH_Init((void*)nvse))
		Log("CornerMessageHandler module initialized (opcode 0x%04X)", CMH_GetOpcode());
	else
		Log("CornerMessageHandler module failed to initialize");

	nvse->SetOpcodeBase(0x3B15);
	CameraOverride_RegisterCommands(nvse);
	Log("Registered SetCameraAngle at opcode 0x3B15");

	if (OEPH_Init((void*)nvse))
		Log("OnEntryPointHandler module initialized (opcode 0x%04X)", OEPH_GetOpcode());
	else
		Log("OnEntryPointHandler module failed to initialize");

	if (OCPH_Init((void*)nvse))
		Log("OnCombatProcedureHandler module initialized (opcode 0x%04X)", OCPH_GetOpcode());
	else
		Log("OnCombatProcedureHandler module failed to initialize");

	if (OSPH_Init((void*)nvse))
		Log("OnSoundPlayedHandler module initialized (opcode 0x%04X)", OSPH_GetOpcode());
	else
		Log("OnSoundPlayedHandler module failed to initialize");

	if (OFTH_Init((void*)nvse))
		Log("OnFastTravelHandler module initialized (opcode 0x%04X)", OFTH_GetOpcode());
	else
		Log("OnFastTravelHandler module failed to initialize");

	if (FDH_Init((void*)nvse))
		Log("FallDamageHandler module initialized (SetMult=0x%04X, GetMult=0x%04X)", FDH_GetSetMultOpcode(), FDH_GetGetMultOpcode());
	else
		Log("FallDamageHandler module failed to initialize");

	if (Settings::bDialogueCamera)
	{
		if (DCH_Init((void*)nvse))
			Log("DialogueCameraHandler module initialized");
		else
			Log("DialogueCameraHandler module failed to initialize");
	}

	if (FakeHit_Init((void*)nvse))
		Log("FakeHitHandler module initialized");
	else
		Log("FakeHitHandler module failed to initialize");

	if (Settings::bOwnerNameInfo)
	{
		if (ONI_Init())
			Log("OwnerNameInfoHandler module initialized");
		else
			Log("OwnerNameInfoHandler module failed to initialize");
	}

	if (Settings::bSaveFileSize)
		Log("SaveFileSizeHandler will initialize in PostLoad");

	NoWeaponSearch::Init();
	nvse->SetOpcodeBase(0x3B20);
	nvse->RegisterCommand(&kCommandInfo_SetNoWeaponSearch);
	nvse->RegisterCommand(&kCommandInfo_GetNoWeaponSearch);
	Log("Registered SetNoWeaponSearch/GetNoWeaponSearch at 0x3B20-0x3B21");

	PreventWeaponSwitch_Init();
	nvse->SetOpcodeBase(0x3B22);
	PreventWeaponSwitch_RegisterCommands(nvse);
	Log("Registered SetPreventWeaponSwitch/GetPreventWeaponSwitch at 0x3B22-0x3B23");
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

		if (!g_msgInterface || !g_arrInterface)
		{
			Log("Failed to get required interfaces");
			return false;
		}

		Settings::Load();
		LogSettings();

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

		ImperativeCommands_Init((void*)nvse);
		RegisterHandlers(nvse);

		Log("itr-nvse loaded successfully");
		return true;
	}
}
