#include "nvse/PluginAPI.h"
#include "nvse/GameAPI.h"
#include "nvse/GameObjects.h"
#include "nvse/GameForms.h"
#include "nvse/GameData.h"
#include "nvse/CommandTable.h"
#include "nvse/ParamInfos.h"
#include "nvse/SafeWrite.h"

template <typename T_Ret = uint32_t, typename ...Args>
__forceinline T_Ret ThisCall(uint32_t _addr, const void* _this, Args ...args) {
	return ((T_Ret(__thiscall*)(const void*, Args...))_addr)(_this, std::forward<Args>(args)...);
}

template <typename T_Ret = void, typename ...Args>
__forceinline T_Ret CdeclCall(uint32_t _addr, Args ...args) {
	return ((T_Ret(__cdecl*)(Args...))_addr)(std::forward<Args>(args)...);
}

#include "internal/settings.h"
#include "DialogueTextFilter.h"
#include "MessageBoxQuickClose.h"
#include "OnStealHandler.h"
#include "OnWeaponDropHandler.h"
#include "OnConsoleHandler.h"
#include "OnWeaponJamHandler.h"
#include "OnKeyStateHandler.h"
#include "KeyHeldHandler.h"
#include "DoubleTapHandler.h"
#include "OnFrenzyHandler.h"
#include "CornerMessageHandler.h"
#include "OnEntryPointHandler.h"
#include "OnCombatProcedureHandler.h"
#include "OnSoundPlayedHandler.h"
#include "OnFastTravelHandler.h"
#include "FallDamageHandler.h"
#include "DialogueCameraHandler.h"
#include "FakeHitHandler.h"
#include "SaveFileSizeHandler.h"
#include "OwnerNameInfoHandler.h"
#include "PreventWeaponSwitch.h"
#include "ELMO.h"
#include "LocationVisitPopup.h"
#include "VATSExtender.h"
#include "CameraOverride.h"
#include "VATSLimbFix.h"
#include "PlayerUpdateHook.h"
#include "SlowMotionPhysicsFix.h"
#include "VATSProjectileFix.h"
#include "KillActorXPFix.h"

#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>
#include <cmath>
#include <unordered_set>

#define ITR_VERSION 100
#define ITR_VERSION_STR "1.0.0"


#define kMessage_MainGameLoop 20
#define kMessage_ReloadConfig 25

#ifndef kMenuType_Start
#define kMenuType_Start 0x3F5
#endif

const _ExtractArgs ExtractArgs = (_ExtractArgs)0x005ACCB0;
const _FormHeap_Free FormHeap_Free = (_FormHeap_Free)0x00401030;

struct TLSData {
	UInt32 unk000[1257];
	bool consoleMode; // 0x13A4
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
	float volumes[kNumVolumeChannels];  // 0=Master, 1=Foot, 2=Voice, 3=Effects, 4=Music, 5=Radio

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

namespace ExplodingPantsFix
{
	static const uint32_t kAddr_IsAltTriggerCall = 0x9C3204;
	static const uint32_t kAddr_IsAltTrigger = 0x975300;
	static uint32_t g_retAddr = 0x9C3209;

	static void* g_currentProjectile = nullptr;

	void PatchWrite8(uint32_t addr, uint8_t data) {
		DWORD oldProtect;
		VirtualProtect((void*)addr, 1, PAGE_EXECUTE_READWRITE, &oldProtect);
		*(uint8_t*)addr = data;
		VirtualProtect((void*)addr, 1, oldProtect, &oldProtect);
	}

	void PatchWrite32(uint32_t addr, uint32_t data) {
		DWORD oldProtect;
		VirtualProtect((void*)addr, 4, PAGE_EXECUTE_READWRITE, &oldProtect);
		*(uint32_t*)addr = data;
		VirtualProtect((void*)addr, 4, oldProtect, &oldProtect);
	}

	void WriteRelCall(uint32_t src, uint32_t dst) {
		PatchWrite8(src, 0xE8);
		PatchWrite32(src + 1, dst - src - 5);
	}

	bool __cdecl Hook_IsAltTrigger(void* projBase) {
		if (((bool(__thiscall*)(void*))kAddr_IsAltTrigger)(projBase))
			return true;
		//flag 0x400 at offset 0xC8
		if (g_currentProjectile && (*(uint32_t*)((uint8_t*)g_currentProjectile + 0xC8) & 0x400))
			return true;
		return false;
	}

	__declspec(naked) void Hook_IsAltTrigger_Wrapper() {
		__asm {
			mov eax, [ebp-0A0h]
			mov g_currentProjectile, eax
			push ecx
			call Hook_IsAltTrigger
			add esp, 4
			jmp g_retAddr
		}
	}

	void Init() {
		WriteRelCall(kAddr_IsAltTriggerCall, (uint32_t)Hook_IsAltTrigger_Wrapper);
		Log("ExplodingPantsFix installed");
	}
}

namespace OwnedBeds
{
	constexpr UInt32 kAddr_IsAnOwner = 0x5785E0;
	constexpr UInt32 kAddr_IsAnOwnerCall = 0x509679;
	constexpr UInt32 kAddr_ResolveOwnership = 0x567790;
	constexpr UInt32 kAddr_PlayerSingleton = 0x011DEA3C;
	constexpr UInt32 kAddr_ProcessListsSingleton = 0x11E0E80;
	constexpr UInt32 kAddr_AttackAlarm = 0x8C0460;
	constexpr UInt32 kAddr_GetActorRefInHigh = 0x970B30;
	constexpr UInt32 kAddr_GetActorRefInHigh_0 = 0x970A20;
	constexpr UInt32 kAddr_GetDetectionLevelAgainstActor = 0x8A0D10;
	constexpr UInt32 kAddr_GetCurrentProcess = 0x8D8520;
	constexpr UInt32 kAddr_GetTopic = 0x61A2D0;
	constexpr UInt32 kAddr_ProcessGreet = 0x8DBE30;
	constexpr UInt8 kFormType_TESFaction = 8;
	constexpr UInt32 DT_COMBAT = 4;

	static bool g_playerWarnedAboutBed = false;

	template <typename T_Ret = void, typename... Args>
	__forceinline T_Ret OBThisCall(UInt32 addr, void* thisObj, Args... args) {
		return reinterpret_cast<T_Ret(__thiscall*)(void*, Args...)>(addr)(thisObj, args...);
	}

	inline UInt8 GetFormType(void* form) {
		return *((UInt8*)form + 4);
	}

	typedef bool (__thiscall *_IsAnOwner)(void* thisObj, void* actor, bool checkFaction);
	static _IsAnOwner IsAnOwner = (_IsAnOwner)kAddr_IsAnOwner;

	typedef void* (__cdecl *_GetTopic)(UInt32 type, int index);
	static _GetTopic GetTopic = (_GetTopic)kAddr_GetTopic;

	typedef void* (__thiscall *_MobileGetProcess)(void* actor);
	static _MobileGetProcess MobileGetProcess = (_MobileGetProcess)kAddr_GetCurrentProcess;

	typedef void (__thiscall *_ProcessGreet)(void* process, void* actor, void* topic, bool forceSub, bool stop, bool queue, bool sayCallback);
	static _ProcessGreet ProcessGreet = (_ProcessGreet)kAddr_ProcessGreet;

	static void SendAssaultAlarmToBedOwner(void* bedRef, void* owner) {
		void* player = *(void**)kAddr_PlayerSingleton;
		void* processList = (void*)kAddr_ProcessListsSingleton;
		void* nearbyActor = nullptr;

		UInt8 formType = GetFormType(owner);

		if (formType == kFormType_TESFaction) {
			nearbyActor = OBThisCall<void*>(kAddr_GetActorRefInHigh, processList, owner, true, true);
		} else {
			nearbyActor = OBThisCall<void*>(kAddr_GetActorRefInHigh_0, processList, owner, 0);
		}

		if (!nearbyActor || nearbyActor == player)
			return;

		bool hasLOS = false;
		bool a8 = false;
		SInt32 detectionLevel = OBThisCall<SInt32>(kAddr_GetDetectionLevelAgainstActor,
			nearbyActor, true, player, &hasLOS, false, false, 0, &a8);

		if (detectionLevel <= 0)
			return;

		if (!g_playerWarnedAboutBed) {
			void* topic = GetTopic(DT_COMBAT, 9);
			if (topic) {
				void* process = MobileGetProcess(nearbyActor);
				if (process) {
					ProcessGreet(process, nearbyActor, topic, false, false, true, false);
				}
			}
			g_playerWarnedAboutBed = true;
		} else {
			OBThisCall(kAddr_AttackAlarm, nearbyActor, player, false, 1);
		}
	}

	bool __fastcall IsAnOwnerHook(void* bedRef, void* edx, void* actor, bool checkFaction) {
		bool isOwner = IsAnOwner(bedRef, actor, checkFaction);

		if (!isOwner) {
			void* owner = OBThisCall<void*>(kAddr_ResolveOwnership, bedRef);
			if (owner) {
				SendAssaultAlarmToBedOwner(bedRef, owner);
			}
			return true;
		}
		return true;
	}

	void PatchWrite8(uint32_t addr, uint8_t data) {
		DWORD oldProtect;
		VirtualProtect((void*)addr, 1, PAGE_EXECUTE_READWRITE, &oldProtect);
		*(uint8_t*)addr = data;
		VirtualProtect((void*)addr, 1, oldProtect, &oldProtect);
	}

	void PatchWrite32(uint32_t addr, uint32_t data) {
		DWORD oldProtect;
		VirtualProtect((void*)addr, 4, PAGE_EXECUTE_READWRITE, &oldProtect);
		*(uint32_t*)addr = data;
		VirtualProtect((void*)addr, 4, oldProtect, &oldProtect);
	}

	void WriteRelCall(UInt32 addr, UInt32 target) {
		PatchWrite8(addr, 0xE8);
		PatchWrite32(addr + 1, target - addr - 5);
	}

	void Init()
	{
		WriteRelCall(kAddr_IsAnOwnerCall, (UInt32)IsAnOwnerHook);
		Log("OwnedBeds installed");
	}
}

namespace AshPileNames
{
	constexpr UInt32 kGetBaseFullNameAddr = 0x55D520;
	constexpr UInt32 kReturnAddr = 0x55D527;
	constexpr UInt32 kExtraData_AshPileRef = 0x89;

	static UInt8 g_trampoline[32];

	typedef const char* (__thiscall* _TrampolineFunc)(TESObjectREFR* thisRef);
	static _TrampolineFunc CallTrampoline = nullptr;

	static BSExtraData* GetExtraDataByType(BaseExtraList* list, UInt32 type)
	{
		if (!list) return nullptr;
		UInt32 index = (type >> 3);
		UInt8 bitMask = 1 << (type % 8);
		if (!(list->m_presenceBitfield[index] & bitMask))
			return nullptr;
		for (BSExtraData* traverse = list->m_data; traverse; traverse = traverse->next)
			if (traverse->type == type)
				return traverse;
		return nullptr;
	}

	static const char* GetActorNameFromAshPile(TESObjectREFR* ashPileRef)
	{
		if (!ashPileRef) return nullptr;

		BSExtraData* extraData = GetExtraDataByType(&ashPileRef->extraDataList, kExtraData_AshPileRef);
		if (!extraData) return nullptr;

		TESObjectREFR* sourceRef = *(TESObjectREFR**)((UInt8*)extraData + 0x0C);
		if (!sourceRef || !sourceRef->baseForm) return nullptr;

		TESForm* baseForm = sourceRef->baseForm;
		UInt8 formType = baseForm->typeID;

		if (formType != kFormType_NPC && formType != kFormType_Creature)
			return nullptr;

		TESActorBase* actorBase = (TESActorBase*)baseForm;
		const char* name = actorBase->fullName.name.m_data;

		if (name && name[0])
			return name;

		return nullptr;
	}

	static const char* __fastcall Hook_GetBaseFullName(TESObjectREFR* thisRef, void* edx)
	{
		const char* actorName = GetActorNameFromAshPile(thisRef);
		if (actorName)
			return actorName;
		return CallTrampoline(thisRef);
	}

	void Init()
	{
		DWORD oldProtect;
		VirtualProtect(g_trampoline, sizeof(g_trampoline), PAGE_EXECUTE_READWRITE, &oldProtect);

		memcpy(g_trampoline, (void*)kGetBaseFullNameAddr, 7);
		g_trampoline[7] = 0xE9;
		*(UInt32*)&g_trampoline[8] = kReturnAddr - ((UInt32)&g_trampoline[7] + 5);

		CallTrampoline = (_TrampolineFunc)(void*)g_trampoline;

		DWORD oldProtect2;
		VirtualProtect((void*)kGetBaseFullNameAddr, 7, PAGE_EXECUTE_READWRITE, &oldProtect2);
		*(UInt8*)kGetBaseFullNameAddr = 0xE9;
		*(UInt32*)(kGetBaseFullNameAddr + 1) = (UInt32)Hook_GetBaseFullName - kGetBaseFullNameAddr - 5;
		*(UInt8*)(kGetBaseFullNameAddr + 5) = 0x90;
		*(UInt8*)(kGetBaseFullNameAddr + 6) = 0x90;
		VirtualProtect((void*)kGetBaseFullNameAddr, 7, oldProtect2, &oldProtect2);

		Log("AshPileNames installed");
	}
}

namespace NoWeaponSearch
{
	//simple fixed array instead of STL for thread safety
	static const int MAX_DISABLED = 64;
	volatile UInt32 g_disabled[MAX_DISABLED] = {0};
	volatile int g_count = 0;

	//0x99F6D0 = CombatState::CombatItemSearch
	//0x998D50 = call site in CombatState::998A50
	typedef bool (__thiscall *CombatItemSearch_t)(void* combatState);
	CombatItemSearch_t Original = (CombatItemSearch_t)0x99F6D0;

	bool IsDisabled(UInt32 refID)
	{
		for (int i = 0; i < g_count; i++)
			if (g_disabled[i] == refID)
				return true;
		return false;
	}

	//0x97AE90 = CombatController::GetPackageOwner
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
		//hook call site at 0x998D50
		WriteRelCall(0x998D50, (UInt32)Hook);
		Log("NoWeaponSearch: Hook installed at 0x998D50");
	}
}

static ParamInfo kParams_SetNoWeaponSearch[1] = {
	{"disable", kParamType_Integer, 0}
};

//IsActor is virtual at vtable index 0x100 (256 bytes / 4 = slot 64)
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

namespace ReversePickpocketNoKarmaFix
{
	constexpr uint32_t kAddr_TryPickpocket = 0x75E0B0;
	constexpr uint32_t kAddr_IsLiveGrenade = 0x75D510;
	constexpr uint32_t kAddr_CallSite1 = 0x75DBDA;
	constexpr uint32_t kAddr_CallSite2 = 0x75DFA7;
	constexpr uint32_t kAddr_CurrentEntry = 0x11D93FC;
	constexpr uint32_t kAddr_Player = 0x11DEA3C;

	typedef bool (__thiscall *_IsLiveGrenade)(void*, void*, void*, void*);

	void PatchWrite8(uint32_t addr, uint8_t data) {
		DWORD oldProtect;
		VirtualProtect((void*)addr, 1, PAGE_EXECUTE_READWRITE, &oldProtect);
		*(uint8_t*)addr = data;
		VirtualProtect((void*)addr, 1, oldProtect, &oldProtect);
	}

	void PatchWrite32(uint32_t addr, uint32_t data) {
		DWORD oldProtect;
		VirtualProtect((void*)addr, 4, PAGE_EXECUTE_READWRITE, &oldProtect);
		*(uint32_t*)addr = data;
		VirtualProtect((void*)addr, 4, oldProtect, &oldProtect);
	}

	void WriteRelCall(uint32_t src, uint32_t dst) {
		PatchWrite8(src, 0xE8);
		PatchWrite32(src + 1, dst - src - 5);
	}

	bool __fastcall ShouldSkipKarma(void* menu, void* actor)
	{
		void* entry = *(void**)kAddr_CurrentEntry;
		void* player = *(void**)kAddr_Player;

		uint32_t currentItems = *(uint32_t*)((uint32_t)menu + 0xF8);
		bool isReverse = (currentItems == (uint32_t)menu + 0x98);

		if (isReverse && entry)
		{
			bool isLiveGrenade = ((_IsLiveGrenade)kAddr_IsLiveGrenade)(menu, entry, player, actor);
			if (!isLiveGrenade)
				return true;
		}
		return false;
	}

	__declspec(naked) void Hook_TryPickpocket()
	{
		__asm
		{
			push ecx
			mov edx, [esp+8]
			call ShouldSkipKarma
			pop ecx

			test al, al
			jnz skip

			jmp kAddr_TryPickpocket

		skip:
			mov al, 1
			ret 12
		}
	}

	void Init()
	{
		WriteRelCall(kAddr_CallSite1, (uint32_t)Hook_TryPickpocket);
		WriteRelCall(kAddr_CallSite2, (uint32_t)Hook_TryPickpocket);
		Log("ReversePickpocketNoKarmaFix installed");
	}
}


static void DeleteConsoleLog()
{
	char gameDir[MAX_PATH];
	GetModuleFileNameA(nullptr, gameDir, MAX_PATH);
	char* lastSlash = strrchr(gameDir, '\\');
	if (lastSlash) *lastSlash = '\0';

	//stewie's tweaks custom console log name
	char iniPath[MAX_PATH];
	sprintf_s(iniPath, "%s\\Data\\NVSE\\Plugins\\nvse_stewie_tweaks.ini", gameDir);

	char logName[256];
	GetPrivateProfileStringA("Main", "sConsoleOutputFile", "consoleout.txt",
	                         logName, sizeof(logName), iniPath);

	char logPath[MAX_PATH];
	sprintf_s(logPath, "%s\\%s", gameDir, logName);
	DeleteFileA(logPath);
}


static ParamInfo kParams_GetRefsSortedByDistance[5] = {
	{ "maxDistance",      kParamType_Float,   0 },
	{ "formType",         kParamType_Integer, 1 },
	{ "cellDepth",        kParamType_Integer, 1 },
	{ "includeTakenRefs", kParamType_Integer, 1 },
	{ "baseForm",         kParamType_AnyForm, 1 },
};

DEFINE_COMMAND_PLUGIN(GetRefsSortedByDistance, "Returns array of refs sorted by distance from player", 0, 5, kParams_GetRefsSortedByDistance);

enum {
	kFormTypeFilter_AnyType = 0,
	kFormTypeFilter_Actor = 200,
	kFormTypeFilter_InventoryItem = 201,
};

static bool IsInventoryItemType(UInt8 formType)
{
	return formType == kFormType_Armor || formType == kFormType_Book ||
	       formType == kFormType_Clothing || formType == kFormType_Ingredient ||
	       formType == kFormType_Misc || formType == kFormType_Weapon ||
	       formType == kFormType_Ammo || formType == kFormType_Key ||
	       formType == kFormType_AlchemyItem || formType == kFormType_Note;
}

static bool IsTakenRef(TESObjectREFR* refr)
{
	if (!refr->IsDeleted()) return false;
	UInt8 formType = refr->baseForm->typeID;
	return IsInventoryItemType(formType);
}

static bool MatchesBaseForm(TESObjectREFR* refr, TESForm* baseForm)
{
	if (!baseForm) return true;
	return refr->baseForm == baseForm;
}

static bool MatchesFormType(TESObjectREFR* refr, UInt32 formType, bool includeTakenRefs)
{
	if (!refr || !refr->baseForm) return false;
	if (!includeTakenRefs && IsTakenRef(refr)) return false;

	UInt8 baseType = refr->baseForm->typeID;

	switch (formType)
	{
		case kFormTypeFilter_AnyType:
			return true;
		case kFormTypeFilter_Actor:
			if (refr->baseForm->refID == 7) return false;
			return baseType == kFormType_Creature || baseType == kFormType_NPC;
		case kFormTypeFilter_InventoryItem:
			return IsInventoryItemType(baseType);
		default:
			if (baseType == kFormType_NPC && refr->baseForm->refID == 7) return false;
			return baseType == formType;
	}
}

static float CalcDistanceSquared(TESObjectREFR* a, TESObjectREFR* b)
{
	float dx = a->posX - b->posX;
	float dy = a->posY - b->posY;
	float dz = a->posZ - b->posZ;
	return dx * dx + dy * dy + dz * dz;
}

bool Cmd_GetRefsSortedByDistance_Execute(COMMAND_ARGS)
{
	*result = 0;

	float maxDistance = 0;
	UInt32 formType = kFormTypeFilter_AnyType;
	SInt32 cellDepth = 0;
	UInt32 includeTakenRefs = 0;
	TESForm* baseForm = nullptr;

	if (!ExtractArgs(EXTRACT_ARGS, &maxDistance, &formType, &cellDepth, &includeTakenRefs, &baseForm))
		return true;

	if (maxDistance <= 0)
	{
		if (IsConsoleMode()) Console_Print("GetRefsSortedByDistance >> maxDistance must be > 0");
		return true;
	}

	PlayerCharacter* player = PlayerCharacter::GetSingleton();
	if (!player || !player->parentCell) return true;

	float maxDistSq = maxDistance * maxDistance;

	struct RefWithDist {
		TESObjectREFR* ref;
		float distance;
	};
	std::vector<RefWithDist> refs;

	TESObjectCELL* playerCell = player->parentCell;

	if (cellDepth == -1) cellDepth = 5;

	auto ProcessCell = [&](TESObjectCELL* cell)
	{
		if (!cell) return;
		for (auto iter = cell->objectList.Begin(); !iter.End(); ++iter)
		{
			TESObjectREFR* refr = iter.Get();
			if (!refr || refr == player) continue;
			if (!MatchesFormType(refr, formType, includeTakenRefs != 0)) continue;
			if (!MatchesBaseForm(refr, baseForm)) continue;

			float distSq = CalcDistanceSquared(refr, player);
			if (distSq > maxDistSq) continue;

			refs.push_back({ refr, sqrtf(distSq) });
		}
	};

	ProcessCell(playerCell);

	TESWorldSpace* world = playerCell->worldSpace;
	if (world && cellDepth > 0 && !playerCell->IsInterior() && playerCell->coords)
	{
		SInt32 baseX = (SInt32)playerCell->coords->x;
		SInt32 baseY = (SInt32)playerCell->coords->y;

		for (SInt32 dx = -cellDepth; dx <= cellDepth; dx++)
		{
			for (SInt32 dy = -cellDepth; dy <= cellDepth; dy++)
			{
				if (dx == 0 && dy == 0) continue;
				UInt32 key = ((baseX + dx) << 16) | ((baseY + dy) & 0xFFFF);
				TESObjectCELL* cell = world->cellMap->Lookup(key);
				ProcessCell(cell);
			}
		}
	}

	std::sort(refs.begin(), refs.end(), [](const RefWithDist& a, const RefWithDist& b) {
		return a.distance < b.distance;
	});

	NVSEArrayVarInterface::Array* arr = g_arrInterface->CreateArray(nullptr, 0, scriptObj);
	for (const auto& item : refs)
	{
		NVSEArrayVarInterface::Element elem(item.ref);
		g_arrInterface->AppendElement(arr, elem);
	}

	g_arrInterface->AssignCommandResult(arr, result);

	if (IsConsoleMode())
	{
		Console_Print("GetRefsSortedByDistance >> Found %d refs within %.1f units", refs.size(), maxDistance);
	}

	return true;
}

typedef TESObjectREFR* (*_PlaceAtMe)(TESObjectREFR*, TESForm*, UInt32, UInt32, UInt32, float);
static const _PlaceAtMe PlaceAtMe = (_PlaceAtMe)0x5C4B30;

static ParamInfo kParams_Duplicate[1] = {
	{ "count", kParamType_Integer, 1 },  // optional, defaults to 1
};

DEFINE_COMMAND_PLUGIN(Duplicate, "Duplicates the reference and returns the new ref", 1, 1, kParams_Duplicate);

bool Cmd_Duplicate_Execute(COMMAND_ARGS)
{
	*result = 0;

	UInt32 count = 1;

	ExtractArgs(EXTRACT_ARGS, &count);

	if (count < 1) count = 1;

	if (!thisObj || !thisObj->baseForm)
	{
		if (IsConsoleMode())
			Console_Print("Duplicate >> No reference selected");
		return true;
	}

	TESObjectREFR* lastRef = nullptr;
	UInt32 created = 0;

	for (UInt32 i = 0; i < count; i++)
	{
		TESObjectREFR* newRef = PlaceAtMe(
			thisObj,           // spawn location
			thisObj->baseForm, // form to spawn
			1,                 // count
			0,                 // distance
			0,                 // direction
			1.0f);             // health (1.0 = full)

		if (newRef)
		{
			lastRef = newRef;
			created++;
		}
	}

	if (lastRef)
	{
		*((UInt32*)result) = lastRef->refID;
		if (IsConsoleMode())
			Console_Print("Duplicate >> Created %d ref(s), last: %08X", created, lastRef->refID);
	}
	else
	{
		if (IsConsoleMode())
			Console_Print("Duplicate >> Failed to create reference");
	}

	return true;
}

typedef bool (__thiscall *_ConditionList_Evaluate)(void* conditionList, TESObjectREFR* runOnRef, TESForm* arg2, bool* result, bool arg4);
static const _ConditionList_Evaluate ConditionList_Evaluate = (_ConditionList_Evaluate)0x680C60;

typedef SInt32 (__thiscall *_GetActorValue)(void* actorValueOwner, UInt32 avCode);
static const _GetActorValue GetActorValue = (_GetActorValue)0x66EF50;

typedef SInt32 (__thiscall *_GetItemCount)(TESObjectREFR* container, TESForm* item);
static const _GetItemCount GetItemCount = (_GetItemCount)0x575610;

static ParamInfo kParams_GetAvailableRecipes[1] = {
	{ "category", kParamType_AnyForm, 1 },  // optional category filter
};

DEFINE_COMMAND_PLUGIN(GetAvailableRecipes, "Returns array of recipes player can craft", 0, 1, kParams_GetAvailableRecipes);

bool Cmd_GetAvailableRecipes_Execute(COMMAND_ARGS)
{
	*result = 0;

	TESForm* categoryFilter = nullptr;
	ExtractArgs(EXTRACT_ARGS, &categoryFilter);

	if (categoryFilter && categoryFilter->typeID != kFormType_RecipeCategory)
		categoryFilter = nullptr;

	PlayerCharacter* player = PlayerCharacter::GetSingleton();
	if (!player) return true;

	DataHandler* dataHandler = *(DataHandler**)0x011C3F2C;
	if (!dataHandler) return true;

	std::vector<TESForm*> availableRecipes;
	tList<TESRecipe>* recipeList = &dataHandler->recipeList;

	for (auto iter = recipeList->Begin(); !iter.End(); ++iter)
	{
		TESRecipe* recipe = iter.Get();
		if (!recipe) continue;

		if (categoryFilter)
		{
			TESRecipeCategory* cat = recipe->category;
			TESRecipeCategory* subCat = recipe->subCategory;
			if (cat != categoryFilter && subCat != categoryFilter)
				continue;
		}

		void* conditionList = &recipe->conditions;
		bool evalResult = false;
		bool conditionsPassed = ConditionList_Evaluate(conditionList, player, nullptr, &evalResult, false);
		if (!conditionsPassed)
			continue;

		if (recipe->reqSkill != (UInt32)-1 && recipe->reqSkillLevel > 0)
		{
			void* actorValueOwner = (void*)((UInt8*)player + 0xA4); //ActorValueOwner at 0xA4
			SInt32 playerSkill = GetActorValue(actorValueOwner, recipe->reqSkill);
			if (playerSkill < (SInt32)recipe->reqSkillLevel)
				continue;
		}

		bool hasAllInputs = true;
		for (auto inputIter = recipe->inputs.Begin(); !inputIter.End(); ++inputIter)
		{
			ComponentEntry* component = inputIter.Get();
			if (!component || !component->item)
				continue;

			UInt32 playerCount = GetItemCount(player, component->item);
			if (playerCount < component->quantity)
			{
				hasAllInputs = false;
				break;
			}
		}
		if (!hasAllInputs)
			continue;

		availableRecipes.push_back(recipe);
	}

	if (!availableRecipes.empty() && g_arrInterface)
	{
		NVSEArrayVarInterface::Array* arr = g_arrInterface->CreateArray(nullptr, 0, scriptObj);
		for (TESForm* recipe : availableRecipes)
		{
			NVSEArrayVarInterface::Element elem(recipe);
			g_arrInterface->AppendElement(arr, elem);
		}
		g_arrInterface->AssignCommandResult(arr, result);
	}

	if (IsConsoleMode())
	{
		Console_Print("GetAvailableRecipes >> Found %d craftable recipes", availableRecipes.size());
	}

	return true;
}


//TESObjectREFR::ClampToGround at 0x576470
typedef bool (__thiscall *_ClampToGround)(TESObjectREFR*);
static const _ClampToGround RefClampToGround = (_ClampToGround)0x576470;

//TESObjectREFR::SetLocationOnReference at 0x575830
typedef void (__thiscall *_SetLocationOnReference)(TESObjectREFR*, float*);
static const _SetLocationOnReference SetLocationOnReference = (_SetLocationOnReference)0x575830;

DEFINE_COMMAND_PLUGIN(ClampToGround, "Clamps the reference to the ground", 1, 0, nullptr);

bool Cmd_ClampToGround_Execute(COMMAND_ARGS)
{
	*result = 0;

	if (!thisObj)
	{
		if (IsConsoleMode())
			Console_Print("ClampToGround >> No reference selected");
		return true;
	}

	if (RefClampToGround(thisObj))
	{
		SetLocationOnReference(thisObj, &thisObj->posX);
		*result = 1;
	}

	return true;
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
					ExplodingPantsFix::Init();
				if (Settings::bKillActorXPFix)
					KillActorXPFix_Init();
				if (Settings::bReversePickpocketNoKarma)
					ReversePickpocketNoKarmaFix::Init();
				if (Settings::bSaveFileSize)
					SFSH_Init();
				if (Settings::bVATSProjectileFix)
					VATSProjectileFix_Init();
				if (Settings::bVATSLimbFix)
					VATSLimbFix_Init();
				if (Settings::bOwnedBeds)
					OwnedBeds::Init();
				if (Settings::bLocationVisitPopup)
					LocationVisitPopup_Init(Settings::iLocationVisitCooldownSeconds, Settings::bLocationVisitDisableSound != 0);
				g_hooksInstalled = true;
			}
			break;

		case NVSEMessagingInterface::kMessage_PostPostLoad:
			if (Settings::bDialogueCamera)
				DCH_InstallCameraHooks();
			if (Settings::bAshPileNames)
				AshPileNames::Init();
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
				//g_bIsGodMode at 0x11E07BA
				*(UInt8*)0x11E07BA = 1;
				g_godModeExecuted = true;
				Log("AutoGodMode: Enabled god mode");
			}
			break;

		case kMessage_ReloadConfig:
			Console_Print("itr-nvse: Got config!");
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


extern "C" {

__declspec(dllexport) bool NVSEPlugin_Query(const NVSEInterface* nvse, PluginInfo* info)
{
	info->infoVersion = PluginInfo::kInfoVersion;
	info->name = "itr-nvse";
	info->version = ITR_VERSION;

	if (nvse->isEditor) return true;
	if (nvse->nvseVersion < NVSE_VERSION_INTEGER) return false;
	if (nvse->runtimeVersion < RUNTIME_VERSION_1_4_0_525) return false;
	if (nvse->isNogore) return false;

	return true;
}

__declspec(dllexport) bool NVSEPlugin_Load(const NVSEInterface* nvse)
{
	g_pluginHandle = nvse->GetPluginHandle();

	if (nvse->isEditor) return true;

	char logPath[MAX_PATH];
	GetModuleFileNameA(GetModuleHandleA("itr-nvse.dll"), logPath, MAX_PATH);
	char* lastSlash = strrchr(logPath, '\\');
	if (lastSlash) strcpy_s(lastSlash + 1, MAX_PATH - (lastSlash + 1 - logPath), "itr-nvse.log");
	g_logFile = fopen(logPath, "w");

	Log("itr-nvse v%s loading...", ITR_VERSION_STR);

	g_msgInterface = (NVSEMessagingInterface*)nvse->QueryInterface(kInterface_Messaging);
	g_consoleInterface = (NVSEConsoleInterface*)nvse->QueryInterface(kInterface_Console);
	g_arrInterface = (NVSEArrayVarInterface*)nvse->QueryInterface(kInterface_ArrayVar);

	if (!g_msgInterface || !g_arrInterface)
	{
		Log("Failed to get required interfaces");
		return false;
	}

	Settings::Load();

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

	nvse->SetOpcodeBase(0x4040);
	nvse->RegisterTypedCommand(&kCommandInfo_GetRefsSortedByDistance, kRetnType_Array);
	Log("Registered GetRefsSortedByDistance at opcode 0x4040");

	if (DTF_Init((void*)nvse)) {
		Log("DialogueTextFilter module initialized (opcode 0x%04X)", DTF_GetOpcode());
	} else {
		Log("DialogueTextFilter module failed to initialize");
	}

	if (OSH_Init((void*)nvse)) {
		Log("OnStealHandler module initialized (opcode 0x%04X)", OSH_GetOpcode());
	} else {
		Log("OnStealHandler module failed to initialize");
	}

	if (OWDH_Init((void*)nvse)) {
		Log("OnWeaponDropHandler module initialized (opcode 0x%04X)", OWDH_GetOpcode());
	} else {
		Log("OnWeaponDropHandler module failed to initialize");
	}

	nvse->SetOpcodeBase(0x3B03);
	nvse->RegisterTypedCommand(&kCommandInfo_Duplicate, kRetnType_Form);
	Log("Registered Duplicate at opcode 0x3B03");

	if (OCH_Init((void*)nvse)) {
		Log("OnConsoleHandler module initialized (open=0x%04X, close=0x%04X)",
		    OCH_GetOpenOpcode(), OCH_GetCloseOpcode());
	} else {
		Log("OnConsoleHandler module failed to initialize");
	}

	if (OWJH_Init((void*)nvse)) {
		Log("OnWeaponJamHandler module initialized (opcode 0x%04X)", OWJH_GetOpcode());
	} else {
		Log("OnWeaponJamHandler module failed to initialize");
	}

	if (OKSH_Init((void*)nvse)) {
		Log("OnKeyStateHandler module initialized (disabled=0x%04X, enabled=0x%04X)",
		    OKSH_GetDisabledOpcode(), OKSH_GetEnabledOpcode());
	} else {
		Log("OnKeyStateHandler module failed to initialize");
	}

	if (KHH_Init((void*)nvse)) {
		Log("KeyHeldHandler module initialized");
	} else {
		Log("KeyHeldHandler module failed to initialize");
	}

	if (DTH_Init((void*)nvse)) {
		Log("DoubleTapHandler module initialized");
	} else {
		Log("DoubleTapHandler module failed to initialize");
	}

	if (OFH_Init((void*)nvse)) {
		Log("OnFrenzyHandler module initialized (opcode 0x%04X)", OFH_GetOpcode());
	} else {
		Log("OnFrenzyHandler module failed to initialize");
	}

	if (CMH_Init((void*)nvse)) {
		Log("CornerMessageHandler module initialized (opcode 0x%04X)", CMH_GetOpcode());
	} else {
		Log("CornerMessageHandler module failed to initialize");
	}

	nvse->SetOpcodeBase(0x3B15);
	CameraOverride_RegisterCommands(nvse);
	Log("Registered SetCameraAngle at opcode 0x3B15");

	nvse->SetOpcodeBase(0x3B16);
	nvse->RegisterTypedCommand(&kCommandInfo_GetAvailableRecipes, kRetnType_Array);
	Log("Registered GetAvailableRecipes at opcode 0x3B16");

	nvse->SetOpcodeBase(0x3B1F);
	nvse->RegisterCommand(&kCommandInfo_ClampToGround);
	Log("Registered ClampToGround at opcode 0x3B1F");

	if (OEPH_Init((void*)nvse)) {
		Log("OnEntryPointHandler module initialized (opcode 0x%04X)", OEPH_GetOpcode());
	} else {
		Log("OnEntryPointHandler module failed to initialize");
	}

	if (OCPH_Init((void*)nvse)) {
		Log("OnCombatProcedureHandler module initialized (opcode 0x%04X)", OCPH_GetOpcode());
	} else {
		Log("OnCombatProcedureHandler module failed to initialize");
	}

	if (OSPH_Init((void*)nvse)) {
		Log("OnSoundPlayedHandler module initialized (opcode 0x%04X)", OSPH_GetOpcode());
	} else {
		Log("OnSoundPlayedHandler module failed to initialize");
	}

	if (OFTH_Init((void*)nvse)) {
		Log("OnFastTravelHandler module initialized (opcode 0x%04X)", OFTH_GetOpcode());
	} else {
		Log("OnFastTravelHandler module failed to initialize");
	}

	if (FDH_Init((void*)nvse)) {
		Log("FallDamageHandler module initialized (SetMult=0x%04X, GetMult=0x%04X)",
		    FDH_GetSetMultOpcode(), FDH_GetGetMultOpcode());
	} else {
		Log("FallDamageHandler module failed to initialize");
	}

	if (Settings::bDialogueCamera)
	{
		if (DCH_Init((void*)nvse)) {
			Log("DialogueCameraHandler module initialized");
		} else {
			Log("DialogueCameraHandler module failed to initialize");
		}
	}

	if (FakeHit_Init((void*)nvse)) {
		Log("FakeHitHandler module initialized");
	} else {
		Log("FakeHitHandler module failed to initialize");
	}

	if (Settings::bOwnerNameInfo)
	{
		if (ONI_Init()) {
			Log("OwnerNameInfoHandler module initialized");
		} else {
			Log("OwnerNameInfoHandler module failed to initialize");
		}
	}

	if (Settings::bSaveFileSize)
	{
		Log("SaveFileSizeHandler will initialize in PostLoad");
	}

	NoWeaponSearch::Init();
	nvse->SetOpcodeBase(0x3B20);
	nvse->RegisterCommand(&kCommandInfo_SetNoWeaponSearch);
	nvse->RegisterCommand(&kCommandInfo_GetNoWeaponSearch);
	Log("Registered SetNoWeaponSearch/GetNoWeaponSearch at 0x3B20-0x3B21");

	PreventWeaponSwitch_Init();
	nvse->SetOpcodeBase(0x3B22);
	PreventWeaponSwitch_RegisterCommands(nvse);
	Log("Registered SetPreventWeaponSwitch/GetPreventWeaponSwitch at 0x3B22-0x3B23");

	Log("itr-nvse loaded successfully");

	return true;
}

}
