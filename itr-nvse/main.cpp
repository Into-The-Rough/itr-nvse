#include "nvse/PluginAPI.h"
#include "nvse/GameAPI.h"
#include "nvse/GameObjects.h"
#include "nvse/GameForms.h"
#include "nvse/GameData.h"
#include "nvse/CommandTable.h"
#include "nvse/ParamInfos.h"

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

#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>
#include <cmath>

#define ITR_VERSION 100
#define ITR_VERSION_STR "1.0.0"

// Missing NVSE message types (added in xNVSE v6.0+)
#define kMessage_MainGameLoop 20
#define kMessage_ReloadConfig 25

// Missing menu type
#ifndef kMenuType_Start
#define kMenuType_Start 0x3F5
#endif

//=============================================================================
// Required NVSE symbols (normally from GameAPI.cpp)
//=============================================================================

// Function pointers from game executable
const _ExtractArgs ExtractArgs = (_ExtractArgs)0x005ACCB0;
const _FormHeap_Free FormHeap_Free = (_FormHeap_Free)0x00401030;

// TLS access for IsConsoleMode
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

// Console printing
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

// PlayerCharacter singleton
PlayerCharacter* PlayerCharacter::GetSingleton()
{
	return *(PlayerCharacter**)0x011DEA3C;
}

//=============================================================================
// Global state
//=============================================================================

PluginHandle g_pluginHandle = kPluginHandle_Invalid;
NVSEMessagingInterface* g_msgInterface = nullptr;
NVSEConsoleInterface* g_consoleInterface = nullptr;
NVSEArrayVarInterface* g_arrInterface = nullptr;


static PlayerCharacter** g_thePlayer = (PlayerCharacter**)0x011DEA3C;
static UInt8* g_MenuVisibilityArray = (UInt8*)0x011F308F;
static SaveGameManager** g_saveGameManager = (SaveGameManager**)0x011DE134;

// AutoGodMode state
static bool g_godModeExecuted = false;

// AutoQuickLoad state
static bool g_quickLoadExecuted = false;
static int g_framesOnMenu = 0;

typedef bool (__thiscall *_LoadQuicksave)(SaveGameManager* mgr);
static const _LoadQuicksave LoadQuicksave = (_LoadQuicksave)0x8509F0;

// AltTabMute state
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

// Simple log file (declared early so hooks can use it)
static FILE* g_logFile = nullptr;

static void Log(const char* fmt, ...)
{
	if (!g_logFile) return;
	va_list args;
	va_start(args, fmt);
	vfprintf(g_logFile, fmt, args);
	fprintf(g_logFile, "\n");
	fflush(g_logFile);
	va_end(args);
}

//=============================================================================
// QuickDrop & Quick180 (single combined hook)
//=============================================================================

namespace PlayerUpdateHook
{
	constexpr float PI = 3.14159265358979323846f;
	constexpr uint32_t kAddr_OSGlobals = 0x11DEA0C;
	constexpr uint32_t kAddr_OSInputGlobals = 0x11F35CC;
	constexpr uint32_t kAddr_GetControlState = 0xA24660;
	constexpr uint32_t kAddr_PlayerUpdateCall = 0x940C78;
	constexpr uint32_t kAddr_TryDropWeapon = 0x89F580;
	constexpr uint32_t kAddr_GetEquippedWeapon = 0x8A1710;
	constexpr uint32_t kOffset_OSGlobals_Window = 0x08;
	constexpr uint32_t kOffset_Actor_RotZ = 0x2C;

	enum KeyState { isHeld, isPressed, isDepressed, isChanged };

	bool g_quickDropLastPressed = false;
	bool g_quick180LastPressed = false;
	uint32_t g_originalCallTarget = 0;

	void PatchWrite32(uint32_t addr, uint32_t data) {
		DWORD oldProtect;
		VirtualProtect((void*)addr, 4, PAGE_EXECUTE_READWRITE, &oldProtect);
		*(uint32_t*)addr = data;
		VirtualProtect((void*)addr, 4, oldProtect, &oldProtect);
	}

	void PatchCall(uint32_t jumpSrc, uint32_t jumpTgt) {
		PatchWrite32(jumpSrc + 1, jumpTgt - jumpSrc - 5);
	}

	uint32_t ReadCallTarget(uint32_t jumpSrc) {
		return *(uint32_t*)(jumpSrc + 1) + jumpSrc + 5;
	}

	bool GetControlState(void* input, uint32_t controlCode, KeyState state) {
		return ((bool(__thiscall*)(void*, uint32_t, KeyState))kAddr_GetControlState)(input, controlCode, state);
	}

	void* GetEquippedWeapon(void* actor) {
		return ((void*(__thiscall*)(void*))kAddr_GetEquippedWeapon)(actor);
	}

	void TryDropWeapon(void* actor) {
		((void(__thiscall*)(void*))kAddr_TryDropWeapon)(actor);
	}

	void RotatePlayer180(void* player) {
		float* rotZ = (float*)((uint8_t*)player + kOffset_Actor_RotZ);
		*rotZ += PI;
		while (*rotZ > PI) *rotZ -= 2.0f * PI;
		while (*rotZ < -PI) *rotZ += 2.0f * PI;
	}

	void __fastcall PlayerUpdate_Hook(void* player, void* edx, float timeDelta) {
		((void(__thiscall*)(void*, float))g_originalCallTarget)(player, timeDelta);

		void* osGlobals = *(void**)kAddr_OSGlobals;
		void* inputGlobals = *(void**)kAddr_OSInputGlobals;

		if (!osGlobals) {
			g_quickDropLastPressed = false;
			g_quick180LastPressed = false;
			return;
		}

		HWND gameWindow = *(HWND*)((uint8_t*)osGlobals + kOffset_OSGlobals_Window);
		if (GetForegroundWindow() != gameWindow) {
			g_quickDropLastPressed = false;
			g_quick180LastPressed = false;
			return;
		}

		// QuickDrop
		if (Settings::bQuickDrop) {
			bool modifierHeld = (Settings::iQuickDropModifierKey == 0) || ((GetAsyncKeyState(Settings::iQuickDropModifierKey) & 0x8000) != 0);
			bool controlPressed = GetControlState(inputGlobals, Settings::iQuickDropControlID, isPressed);
			if (controlPressed && !g_quickDropLastPressed && modifierHeld) {
				if (GetEquippedWeapon(player)) {
					TryDropWeapon(player);
				}
			}
			g_quickDropLastPressed = controlPressed;
		}

		// Quick180
		if (Settings::bQuick180) {
			bool modifierHeld = (Settings::iQuick180ModifierKey == 0) || ((GetAsyncKeyState(Settings::iQuick180ModifierKey) & 0x8000) != 0);
			bool controlPressed = GetControlState(inputGlobals, Settings::iQuick180ControlID, isPressed);
			if (controlPressed && !g_quick180LastPressed && modifierHeld) {
				RotatePlayer180(player);
			}
			g_quick180LastPressed = controlPressed;
		}
	}

	void Init() {
		g_originalCallTarget = ReadCallTarget(kAddr_PlayerUpdateCall);
		PatchCall(kAddr_PlayerUpdateCall, (uint32_t)PlayerUpdate_Hook);
		Log("PlayerUpdateHook installed (chaining to 0x%08X)", g_originalCallTarget);
	}
}

//=============================================================================
// Slow Motion Physics Fix
//=============================================================================

namespace SlowMotionPhysicsFix
{
	constexpr uint32_t kAddr_StepDeltaTimeCall = 0xC6AFF9;
	constexpr uint32_t kAddr_SetFrameTimeMarkerCall = 0xC6AF85;
	constexpr float kMinStepTime = 0.001f;
	constexpr int kMaxStepsPerFrame = 16;

	static uint32_t* g_VATSMode = (uint32_t*)0x11F2258;
	static uint32_t originalStepDeltaTime = 0;
	static uint32_t originalSetFrameTimeMarker = 0;

	static bool IsVATSActive() { return *g_VATSMode != 0; }

	void PatchWrite32(uint32_t addr, uint32_t data) {
		DWORD oldProtect;
		VirtualProtect((void*)addr, 4, PAGE_EXECUTE_READWRITE, &oldProtect);
		*(uint32_t*)addr = data;
		VirtualProtect((void*)addr, 4, oldProtect, &oldProtect);
	}

	void PatchCall(uint32_t jumpSrc, uint32_t jumpTgt) {
		PatchWrite32(jumpSrc + 1, jumpTgt - jumpSrc - 5);
	}

	uint32_t ReadCallTarget(uint32_t jumpSrc) {
		return *(uint32_t*)(jumpSrc + 1) + jumpSrc + 5;
	}

	void __fastcall Hook_SetFrameTimeMarker(void* hkpWorld, void* edx, float delta) {
		if (!IsVATSActive()) {
			float maxDelta = kMaxStepsPerFrame * kMinStepTime;
			if (delta > maxDelta)
				delta = maxDelta;
		}
		((void(__thiscall*)(void*, float))originalSetFrameTimeMarker)(hkpWorld, delta);
	}

	void __fastcall Hook_StepDeltaTime(void* hkpWorld, void* edx, float stepTime) {
		if (IsVATSActive()) {
			((void(__thiscall*)(void*, float))originalStepDeltaTime)(hkpWorld, stepTime);
			return;
		}
		if (stepTime < kMinStepTime)
			stepTime = kMinStepTime;
		((void(__thiscall*)(void*, float))originalStepDeltaTime)(hkpWorld, stepTime);
	}

	void Init() {
		originalSetFrameTimeMarker = ReadCallTarget(kAddr_SetFrameTimeMarkerCall);
		PatchCall(kAddr_SetFrameTimeMarkerCall, (uint32_t)Hook_SetFrameTimeMarker);
		originalStepDeltaTime = ReadCallTarget(kAddr_StepDeltaTimeCall);
		PatchCall(kAddr_StepDeltaTimeCall, (uint32_t)Hook_StepDeltaTime);
		Log("SlowMotionPhysicsFix installed");
	}
}

//=============================================================================
// Exploding Pants Fix
//=============================================================================

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
		// Call original IsAltTrigger
		if (((bool(__thiscall*)(void*))kAddr_IsAltTrigger)(projBase))
			return true;
		// Check projectile flag 0x400 at offset 0xC8
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

//=============================================================================
// VATS Projectile Fix - fixes projectile hit chance in VATS
//=============================================================================

namespace VATSProjectileFix
{
	struct SimpleListNode {
		void* item;
		SimpleListNode* next;
		bool IsEmpty() { return !item; }
		SimpleListNode* GetNext() { return next; }
	};

	struct VATSTarget {
		void* pReference;
		UInt32 eType;
		SimpleListNode bodyParts;
	};

	struct VATSBodyPart {
		float screenPosX;
		float screenPosY;
		float relativePosX;
		float relativePosY;
		float relativePosZ;
		float posX;
		float posY;
		float posZ;
		UInt32 eBodyPart;
		float fPercentVisible;
		float fHitChance;
		bool bIsOnScreen;
		bool bChanceCalculated;
		bool bFirstTimeShown;
		bool bNeedsRecalc;
	};

	constexpr UInt32 kAddr_VATSMenuUpdate = 0x7F3E00;
	constexpr UInt32 kAddr_UpdateHitChance = 0x7F1290;
	constexpr UInt32 kAddr_FindTarget = 0x7F3C90;
	constexpr UInt32 kAddr_HookSite = 0x7ED349;
	constexpr UInt32 kAddr_pTargetRef = 0x11F21CC;

	static UInt32 s_previousTarget = 0;

	template <typename T_Ret = UInt32, typename ...Args>
	__forceinline T_Ret VATSThisCall(UInt32 _addr, const void* _this, Args ...args) {
		return ((T_Ret(__thiscall*)(const void*, Args...))_addr)(_this, std::forward<Args>(args)...);
	}

	static bool __fastcall VATSMenuUpdate_Hook(void* pThis)
	{
		bool result = VATSThisCall<bool>(s_previousTarget, pThis);
		if (!result) return result;

		void** ppTargetRef = (void**)kAddr_pTargetRef;
		void* pTargetRef = *ppTargetRef;
		if (!pTargetRef) return result;

		SimpleListNode* pTargetEntry = VATSThisCall<SimpleListNode*>(kAddr_FindTarget, pThis, pTargetRef);
		if (!pTargetEntry || pTargetEntry->IsEmpty()) return result;

		VATSTarget* pTarget = (VATSTarget*)pTargetEntry->item;
		if (!pTarget) return result;

		//type 2 = projectile
		if (pTarget->eType != 2) return result;

		SimpleListNode* pIter = &pTarget->bodyParts;
		while (pIter && !pIter->IsEmpty()) {
			VATSBodyPart* pPart = (VATSBodyPart*)pIter->item;
			if (pPart) {
				pPart->fPercentVisible = 1.0f;
				pPart->bChanceCalculated = true;
				VATSThisCall<double>(kAddr_UpdateHitChance, pThis, pIter);
			}
			pIter = pIter->GetNext();
		}

		return result;
	}

	void PatchWrite32(uint32_t addr, uint32_t data) {
		DWORD oldProtect;
		VirtualProtect((void*)addr, 4, PAGE_EXECUTE_READWRITE, &oldProtect);
		*(uint32_t*)addr = data;
		VirtualProtect((void*)addr, 4, oldProtect, &oldProtect);
	}

	void PatchCall(uint32_t jumpSrc, uint32_t jumpTgt) {
		PatchWrite32(jumpSrc + 1, jumpTgt - jumpSrc - 5);
	}

	void Init()
	{
		SInt32 currentDisp = *(SInt32*)(kAddr_HookSite + 1);
		s_previousTarget = kAddr_HookSite + 5 + currentDisp;
		PatchCall(kAddr_HookSite, (UInt32)VATSMenuUpdate_Hook);
		Log("VATSProjectileFix installed");
	}
}

//=============================================================================
// VATS Limb Fix - hide dismembered limbs in VATS
//=============================================================================

namespace VATSLimbFix
{
	constexpr uintptr_t kAddr_SetPartitionVisible = 0x5E4810;
	constexpr uintptr_t kAddr_VATSTargetRef = 0x11F21CC;
	constexpr uintptr_t kAddr_VATSTargetsList = 0x11DB150;
	constexpr uint8_t kExtraData_DismemberedLimbs = 0x5F;

	static uint8_t g_trampolineSetPartitionVisible[64];

	struct BSExtraData {
		void** vtbl;
		uint8_t type;
		uint8_t pad[3];
		BSExtraData* next;
	};

	struct BaseExtraList {
		void** vtbl;
		BSExtraData* head;
		uint8_t presentBits[0x15];
		uint8_t pad1D[3];
	};

	struct ExtraDismemberedLimbs : BSExtraData {
		uint16_t dismemberedMask;
		uint8_t pad0E[2];
		int32_t unk10;
		void* weapon;
		int32_t unk18;
		bool wasEaten;
		uint8_t pad1D[3];
	};

	struct LimbFixREFR {
		void** vtbl;
		char pad04[0x40];
		BaseExtraList extraDataList;
	};

	struct VATSTargetLimb {
		LimbFixREFR* pReference;
		uint32_t eType;
	};

	struct VATSTargetNode {
		VATSTargetLimb* data;
		VATSTargetNode* next;
	};

	struct VATSTargetList {
		VATSTargetNode head;
	};

	BSExtraData* GetExtraDataByType(BaseExtraList* list, uint8_t type) {
		if (!list || !list->head) return nullptr;
		uint8_t byteIndex = type >> 3;
		uint8_t bitMask = 1 << (type & 7);
		if (byteIndex < sizeof(list->presentBits) && !(list->presentBits[byteIndex] & bitMask))
			return nullptr;
		for (BSExtraData* data = list->head; data; data = data->next) {
			if (data->type == type)
				return data;
		}
		return nullptr;
	}

	uint16_t GetDismemberMask(LimbFixREFR* ref) {
		if (!ref) return 0;
		ExtraDismemberedLimbs* xDismember = (ExtraDismemberedLimbs*)GetExtraDataByType(&ref->extraDataList, kExtraData_DismemberedLimbs);
		return xDismember ? xDismember->dismemberedMask : 0;
	}

	bool IsLimbDismemberedOnAnyVATSTarget(uint16_t limbID) {
		LimbFixREFR** pTargetRef = (LimbFixREFR**)kAddr_VATSTargetRef;
		if (pTargetRef && *pTargetRef) {
			uint16_t mask = GetDismemberMask(*pTargetRef);
			if (mask & (1 << limbID))
				return true;
		}

		VATSTargetList* targetList = (VATSTargetList*)kAddr_VATSTargetsList;
		if (targetList) {
			VATSTargetNode* node = &targetList->head;
			while (node && node->data) {
				VATSTargetLimb* target = node->data;
				if (target && target->pReference) {
					uint16_t mask = GetDismemberMask(target->pReference);
					if (mask & (1 << limbID))
						return true;
				}
				node = node->next;
			}
		}
		return false;
	}

	void __fastcall SetPartitionVisible_Hook(void* skinInstance, void* edx, uint16_t limbID, char visible) {
		if (visible) {
			if (IsLimbDismemberedOnAnyVATSTarget(limbID))
				return;
		}
		typedef void (__thiscall *SetPartitionVisible_t)(void*, uint16_t, char);
		((SetPartitionVisible_t)(void*)g_trampolineSetPartitionVisible)(skinInstance, limbID, visible);
	}

	void PatchWrite8(uint32_t addr, uint8_t data) {
		DWORD oldProtect;
		VirtualProtect((void*)addr, 1, PAGE_EXECUTE_READWRITE, &oldProtect);
		*(uint8_t*)addr = data;
		VirtualProtect((void*)addr, 1, oldProtect, &oldProtect);
	}

	void WriteRelJump(uintptr_t src, uintptr_t dst) {
		DWORD oldProtect;
		VirtualProtect((void*)src, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
		*(uint8_t*)src = 0xE9;
		*(uint32_t*)(src + 1) = dst - src - 5;
		VirtualProtect((void*)src, 5, oldProtect, &oldProtect);
	}

	void Init()
	{
		//build trampoline: copy 6 byte prologue then jump back
		memcpy(g_trampolineSetPartitionVisible, (void*)kAddr_SetPartitionVisible, 6);
		g_trampolineSetPartitionVisible[6] = 0xE9;
		*(uint32_t*)(g_trampolineSetPartitionVisible + 7) = (kAddr_SetPartitionVisible + 6) - ((uintptr_t)g_trampolineSetPartitionVisible + 11);
		DWORD oldProtect;
		VirtualProtect(g_trampolineSetPartitionVisible, sizeof(g_trampolineSetPartitionVisible), PAGE_EXECUTE_READWRITE, &oldProtect);

		WriteRelJump(kAddr_SetPartitionVisible, (uintptr_t)SetPartitionVisible_Hook);
		PatchWrite8(kAddr_SetPartitionVisible + 5, 0x90);
		Log("VATSLimbFix installed");
	}
}

//=============================================================================
// Owned Beds - allow sleeping in owned beds with consequences
//=============================================================================

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

//=============================================================================
// Ash Pile Names - show original NPC name for ash piles
//=============================================================================

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

//=============================================================================
// Location Visit Popup - show popup when revisiting discovered locations
//=============================================================================

namespace LocationVisitPopup
{
	static DWORD g_cooldownMs = 5 * 60 * 1000;
	static bool g_disableSound = false;
	static int g_muxDetected = -1;

	static const UInt32 kMaxMarkers = 256;
	static UInt32 s_markers[kMaxMarkers];
	static DWORD s_popupTime[kMaxMarkers];
	static bool s_hasLeft[kMaxMarkers];
	static DWORD s_lastSeen[kMaxMarkers];
	static UInt32 s_count = 0;

	template <typename T_Ret = void, typename ...Args>
	__forceinline T_Ret LVPThisCall(UInt32 _addr, void* _this, Args ...args) {
		return ((T_Ret(__thiscall*)(void*, Args...))_addr)(_this, std::forward<Args>(args)...);
	}

	static void PatchWrite8(UInt32 addr, UInt8 data) {
		DWORD oldProtect;
		VirtualProtect((void*)addr, 1, PAGE_EXECUTE_READWRITE, &oldProtect);
		*(UInt8*)addr = data;
		VirtualProtect((void*)addr, 1, oldProtect, &oldProtect);
	}

	static void PatchWrite32(UInt32 addr, UInt32 data) {
		DWORD oldProtect;
		VirtualProtect((void*)addr, 4, PAGE_EXECUTE_READWRITE, &oldProtect);
		*(UInt32*)addr = data;
		VirtualProtect((void*)addr, 4, oldProtect, &oldProtect);
	}

	static void WriteRelJump(UInt32 jumpSrc, UInt32 jumpTgt) {
		PatchWrite8(jumpSrc, 0xE9);
		PatchWrite32(jumpSrc + 1, jumpTgt - jumpSrc - 5);
	}

	static void UpdateCooldowns() {
		DWORD now = GetTickCount();
		for (UInt32 i = 0; i < s_count; i++)
			if (!s_hasLeft[i] && now - s_lastSeen[i] > 3000)
				s_hasLeft[i] = true;
	}

	static int CheckMarker(UInt32 refID) {
		DWORD now = GetTickCount();
		for (UInt32 i = 0; i < s_count; i++) {
			if (s_markers[i] == refID) {
				s_lastSeen[i] = now;
				if (!s_hasLeft[i])
					return 1;
				if (s_popupTime[i] && now - s_popupTime[i] < g_cooldownMs)
					return 1;
				return 0;
			}
		}
		if (s_count < kMaxMarkers) {
			s_markers[s_count] = refID;
			s_popupTime[s_count] = 0;
			s_hasLeft[s_count] = false;
			s_lastSeen[s_count] = now;
			s_count++;
		}
		return 1;
	}

	static void MarkShown(UInt32 refID) {
		DWORD now = GetTickCount();
		for (UInt32 i = 0; i < s_count; i++) {
			if (s_markers[i] == refID) {
				s_popupTime[i] = now;
				return;
			}
		}
	}

	typedef void(__cdecl* SetCustomQuestText_t)(const char*, const char*, int, int, int, int, const char*);
	static SetCustomQuestText_t SetCustomQuestText = (SetCustomQuestText_t)0x76B960;

	static void* GetHUDMainMenuTile() {
		void** g_interfaceManager = (void**)0x11D8A80;
		if (!*g_interfaceManager) return nullptr;
		void* hudMenu = *(void**)((UInt8*)*g_interfaceManager + 0x64);
		if (!hudMenu) return nullptr;
		return *(void**)((UInt8*)hudMenu + 0x30);
	}

	static bool CheckMUXInstalled() {
		void* tile = GetHUDMainMenuTile();
		if (!tile) return false;
		int traitIndex = LVPThisCall<int>(0xA0B110, tile, "_UXQV+Location");
		return traitIndex != -1;
	}

	static void ShowPopupMUX(const char* name) {
		void* tile = GetHUDMainMenuTile();
		if (!tile) return;
		int locTrait = LVPThisCall<int>(0xA0B110, tile, "_UXQV+Location");
		int alphaTrait = LVPThisCall<int>(0xA0B110, tile, "_UXQV+Alpha");
		if (locTrait != -1)
			LVPThisCall<void>(0xA0B270, tile, locTrait, name, true);
		if (alphaTrait != -1)
			LVPThisCall<void>(0xA0A280, tile, alphaTrait, 255.0f, true);
	}

	static void ShowPopupVanilla(const char* name) {
		SetCustomQuestText("", name, 1, 0, -1, -1, g_disableSound ? "" : "UIPopUpQuestNew");
	}

	void __cdecl OnInDiscoveredMarkerRadius(UInt32 markerRefID, void* markerDataPtr) {
		if (!markerDataPtr)
			return;
		UpdateCooldowns();
		if (CheckMarker(markerRefID) != 0)
			return;
		const char* name = *(const char**)((UInt8*)markerDataPtr + 4);
		if (name && name[0]) {
			MarkShown(markerRefID);
			if (g_muxDetected == -1)
				g_muxDetected = CheckMUXInstalled() ? 1 : 0;
			if (g_muxDetected)
				ShowPopupMUX(name);
			else
				ShowPopupVanilla(name);
		}
	}

	__declspec(naked) void CheckDiscoveredMarkerHook() {
		static const UInt32 kRetnAddr = 0x7795E4;
		__asm {
			movzx edx, byte ptr[ebp - 0x90]
			test edx, edx
			jz skipCheck
			mov eax, [ebp - 0x8C]
			test eax, eax
			jz skipCheck
			mov ecx, [eax]
			test ecx, ecx
			jz skipCheck
			mov eax, [eax + 4]
			test eax, eax
			jz skipCheck
			pushad
			push ecx
			push dword ptr[eax + 0x0C]
			call OnInDiscoveredMarkerRadius
			add esp, 8
			popad
			movzx edx, byte ptr[ebp - 0x90]
		skipCheck:
			jmp kRetnAddr
		}
	}

	void Init() {
		g_cooldownMs = Settings::iLocationVisitCooldownSeconds * 1000;
		g_disableSound = Settings::bLocationVisitDisableSound != 0;
		WriteRelJump(0x7795DD, (UInt32)CheckDiscoveredMarkerHook);
		PatchWrite8(0x7795E2, 0x90);
		PatchWrite8(0x7795E3, 0x90);
		Log("LocationVisitPopup installed (cooldown=%ds, sound=%s)",
			Settings::iLocationVisitCooldownSeconds, g_disableSound ? "off" : "on");
	}
}

//=============================================================================
// Kill Actor XP Fix - prevents XP reward when using "kill" command on already-dead actors
//=============================================================================

namespace KillActorXPFix
{
	// Cmd_KillActor_Execute XP block:
	// 0x5BE379: mov ecx, [ebp-10h]   ; load actor (3 bytes)
	// 0x5BE37C: call Actor::GetLevel ; start XP calc (5 bytes)
	// ...
	// 0x5BE3FA: (after XP block)     ; skip target

	constexpr uint32_t kAddr_XPBlockStart = 0x5BE379;      // Start of XP reward code
	constexpr uint32_t kAddr_XPBlockEnd = 0x5BE3FA;        // Jump here to skip XP
	constexpr uint32_t kAddr_ActorGetLevel = 0x87F9F0;     // Actor::GetLevel
	constexpr uint32_t kAddr_ReturnAfterHook = 0x5BE381;   // Return address after our hook (after the call)
	constexpr uint32_t kOffset_Actor_LifeState = 0x108;    // Actor::lifeState offset

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

	void WriteRelJump(uint32_t src, uint32_t dst) {
		PatchWrite8(src, 0xE9);  // jmp rel32
		PatchWrite32(src + 1, dst - src - 5);
	}

	__declspec(naked) void Hook_XPBlockStart()
	{
		__asm
		{
			// Load actor pointer from [ebp-10h]
			mov ecx, [ebp-0x10]

			// Check if actor is dead (lifeState at offset 0x108)
			// lifeState: 0=alive, 1=dying, 2=dead, 6=essential
			mov eax, [ecx + 0x108]  // actor->lifeState
			cmp eax, 1              // dying?
			je skip_xp
			cmp eax, 2              // dead?
			je skip_xp

			// Actor is alive - continue with XP reward
			// ecx already has actor, call Actor::GetLevel
			mov eax, kAddr_ActorGetLevel
			call eax
			// Return to after the call instruction
			mov eax, kAddr_ReturnAfterHook
			jmp eax

		skip_xp:
			// Actor already dead - skip XP block entirely
			mov eax, kAddr_XPBlockEnd
			jmp eax
		}
	}

	void Init()
	{
		// Overwrite 8 bytes at 0x5BE379:
		// Original: mov ecx, [ebp-10h] (3 bytes) + call Actor::GetLevel (5 bytes)
		// New: jmp Hook_XPBlockStart (5 bytes) + 3 NOPs
		WriteRelJump(kAddr_XPBlockStart, (uint32_t)Hook_XPBlockStart);
		PatchWrite8(kAddr_XPBlockStart + 5, 0x90);  // NOP
		PatchWrite8(kAddr_XPBlockStart + 6, 0x90);  // NOP
		PatchWrite8(kAddr_XPBlockStart + 7, 0x90);  // NOP
		Log("KillActorXPFix installed");
	}
}

//=============================================================================
// Reverse Pickpocket No Karma
//=============================================================================

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

//=============================================================================
// Camera Override
//=============================================================================

namespace CameraOverride
{
	static FILE* g_camLog = nullptr;
	static int g_logThrottle = 0;

	static void CamLog(const char* fmt, ...) {
		if (!g_camLog) {
			g_camLog = fopen("CameraOverride.log", "w");
			if (!g_camLog) return;
		}
		va_list args;
		va_start(args, fmt);
		vfprintf(g_camLog, fmt, args);
		va_end(args);
		fprintf(g_camLog, "\n");
		fflush(g_camLog);
	}

	struct Mat3 {
		float m[3][3];

		void Identity() {
			m[0][0] = 1; m[0][1] = 0; m[0][2] = 0;
			m[1][0] = 0; m[1][1] = 1; m[1][2] = 0;
			m[2][0] = 0; m[2][1] = 0; m[2][2] = 1;
		}

		void RotateX(float rad) {
			float s = sinf(rad), c = cosf(rad);
			m[0][0] = 1; m[0][1] = 0; m[0][2] = 0;
			m[1][0] = 0; m[1][1] = c; m[1][2] = s;
			m[2][0] = 0; m[2][1] = -s; m[2][2] = c;
		}

		void RotateY(float rad) {
			float s = sinf(rad), c = cosf(rad);
			m[0][0] = c; m[0][1] = 0; m[0][2] = -s;
			m[1][0] = 0; m[1][1] = 1; m[1][2] = 0;
			m[2][0] = s; m[2][1] = 0; m[2][2] = c;
		}

		void RotateZ(float rad) {
			float s = sinf(rad), c = cosf(rad);
			m[0][0] = c; m[0][1] = s; m[0][2] = 0;
			m[1][0] = -s; m[1][1] = c; m[1][2] = 0;
			m[2][0] = 0; m[2][1] = 0; m[2][2] = 1;
		}

		Mat3 operator*(const Mat3& b) const {
			Mat3 r;
			for (int i = 0; i < 3; i++) {
				for (int j = 0; j < 3; j++) {
					r.m[i][j] = m[i][0]*b.m[0][j] + m[i][1]*b.m[1][j] + m[i][2]*b.m[2][j];
				}
			}
			return r;
		}
	};

	struct Vec3 { float x, y, z; };

	static Mat3 g_rotation;
	static Vec3 g_translation;
	static bool g_overrideRot = false;
	static bool g_overridePos = false;

	//scene graph and camera access
	static void** g_sceneGraph = (void**)0x11DEB7C;
	static void** g_thePlayer = (void**)0x11DEA3C;

	//NiAVObject offsets
	//m_local at +0x34 (NiTransform: rotate=0x0, translate=0x24, scale=0x30)
	//so m_local.rotate at +0x34, m_local.translate at +0x58
	static constexpr UInt32 kOffset_LocalRotate = 0x34;
	static constexpr UInt32 kOffset_LocalTranslate = 0x58;

	//NiNode::GetAt virtual at vtable[0x30/4 = 12]
	typedef void* (__thiscall *GetAt_t)(void* node, UInt32 index);

	//NiAVObject::Update virtual at vtable[0x68/4 = 26]
	typedef void (__thiscall *NiAVObjectUpdate_t)(void* node, void* updateData);

	static void* GetCameraRootNode() {
		void* sceneGraph = *g_sceneGraph;
		if (!sceneGraph) return nullptr;
		//call virtual GetAt(0) to get camera root node
		void** vtable = *(void***)sceneGraph;
		GetAt_t getAt = (GetAt_t)vtable[12];
		return getAt(sceneGraph, 0);
	}

	static bool IsThirdPerson() {
		void* player = *g_thePlayer;
		if (!player) return false;
		return *(bool*)((UInt8*)player + 0x650);
	}

	void Init() {
		g_rotation.Identity();
		g_translation = {0, 0, 0};
		Log("CameraOverride installed (direct manipulation mode)");
	}

	//called every frame to apply camera override
	void Update() {
		if (!g_overrideRot && !g_overridePos) return;
		if (!IsThirdPerson()) return;

		void* cameraRoot = GetCameraRootNode();
		if (!cameraRoot) return;

		if (g_logThrottle++ % 60 == 0) {
			CamLog("Update: cameraRoot=%p overrideRot=%d overridePos=%d", cameraRoot, g_overrideRot, g_overridePos);
		}

		//directly set m_local.rotate and m_local.translate
		if (g_overridePos) {
			Vec3* localTranslate = (Vec3*)((UInt8*)cameraRoot + kOffset_LocalTranslate);
			*localTranslate = g_translation;
			if (g_logThrottle % 60 == 1) {
				CamLog("  set translate to (%.1f, %.1f, %.1f)", g_translation.x, g_translation.y, g_translation.z);
			}
		}

		if (g_overrideRot) {
			Mat3* localRotate = (Mat3*)((UInt8*)cameraRoot + kOffset_LocalRotate);
			*localRotate = g_rotation;
			if (g_logThrottle % 60 == 1) {
				CamLog("  set rotation matrix");
			}
		}

		//call NiAVObject::Update to recalculate world transform
		void** vtable = *(void***)cameraRoot;
		NiAVObjectUpdate_t updateFunc = (NiAVObjectUpdate_t)vtable[26];
		//pass null for update data (0.0 time, no flags)
		UInt8 updateData[12] = {0};
		updateFunc(cameraRoot, updateData);
	}

	//public API for other modules
	void SetRotation(bool enable, int axis, float degrees) {
		CamLog("SetRotation: enable=%d axis=%d degrees=%.1f", enable, axis, degrees);
		g_overrideRot = enable;
		if (!enable) {
			g_rotation.Identity();
			return;
		}

		float rad = degrees * 0.01745329252f; //deg to rad
		Mat3 rot;
		switch (axis) {
			case 0: g_rotation.Identity(); return;
			case 1: rot.RotateX(rad); break;
			case 2: rot.RotateY(rad); break;
			case 3: rot.RotateZ(rad); break;
			default: return;
		}
		g_rotation = g_rotation * rot;
		CamLog("  g_overrideRot is now %d", g_overrideRot);
	}

	void SetTranslation(bool enable, float x, float y, float z) {
		CamLog("SetTranslation: enable=%d pos=(%.1f, %.1f, %.1f)", enable, x, y, z);
		g_overridePos = enable;
		g_translation = {x, y, z};
		CamLog("  g_overridePos is now %d", g_overridePos);
	}

	void ResetRotation() {
		CamLog("ResetRotation called");
		g_rotation.Identity();
	}
}

//=============================================================================
// Console Log Cleaner
//=============================================================================

static void DeleteConsoleLog()
{
	char gameDir[MAX_PATH];
	GetModuleFileNameA(nullptr, gameDir, MAX_PATH);
	char* lastSlash = strrchr(gameDir, '\\');
	if (lastSlash) *lastSlash = '\0';

	// Check Stewie's Tweaks INI for custom console log name
	char iniPath[MAX_PATH];
	sprintf_s(iniPath, "%s\\Data\\NVSE\\Plugins\\nvse_stewie_tweaks.ini", gameDir);

	char logName[256];
	GetPrivateProfileStringA("Main", "sConsoleOutputFile", "consoleout.txt",
	                         logName, sizeof(logName), iniPath);

	char logPath[MAX_PATH];
	sprintf_s(logPath, "%s\\%s", gameDir, logName);
	DeleteFileA(logPath);
}

//=============================================================================
// GetRefsSortedByDistance Command
//=============================================================================

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

//=============================================================================
// Duplicate Command
//=============================================================================

// PlaceAtMe game function - spawns a ref from a base form at a location
// TESObjectREFR* PlaceAtMe(TESObjectREFR* refr, TESForm* form, UInt32 count, UInt32 distance, UInt32 direction, float health)
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

//=============================================================================
// SetCameraAngle Command - Sets absolute camera rotation (requires JohnnyGuitar)
//=============================================================================

static ParamInfo kParams_SetCameraAngle[4] = {
	{ "enable",  kParamType_Integer, 0 },
	{ "angleX",  kParamType_Float,   0 },
	{ "angleY",  kParamType_Float,   0 },
	{ "angleZ",  kParamType_Float,   0 },
};

DEFINE_COMMAND_PLUGIN(SetCameraAngle, "Sets absolute camera rotation angles (requires JohnnyGuitar)", 0, 4, kParams_SetCameraAngle);

bool Cmd_SetCameraAngle_Execute(COMMAND_ARGS)
{
	*result = 0;

	UInt32 enable = 0;
	float angleX = 0.0f;
	float angleY = 0.0f;
	float angleZ = 0.0f;

	if (!ExtractArgs(EXTRACT_ARGS, &enable, &angleX, &angleY, &angleZ))
		return true;

	//reset rotation matrix
	CameraOverride::ResetRotation();

	if (enable)
	{
		if (angleX != 0.0f)
			CameraOverride::SetRotation(true, 1, angleX);
		if (angleY != 0.0f)
			CameraOverride::SetRotation(true, 2, angleY);
		if (angleZ != 0.0f)
			CameraOverride::SetRotation(true, 3, angleZ);
	}
	else
	{
		CameraOverride::SetRotation(false, 0, 0);
	}

	if (IsConsoleMode())
	{
		if (enable)
			Console_Print("SetCameraAngle >> Set to X:%.1f Y:%.1f Z:%.1f", angleX, angleY, angleZ);
		else
			Console_Print("SetCameraAngle >> Disabled");
	}

	return true;
}

//=============================================================================
// GetAvailableRecipes Command - Returns array of recipes player can craft
//=============================================================================

// Condition evaluation function: bool __thiscall tList<Condition>::Evaluate(TESObjectREFR* runOnRef, TESForm* arg2, bool* result, bool arg4)
typedef bool (__thiscall *_ConditionList_Evaluate)(void* conditionList, TESObjectREFR* runOnRef, TESForm* arg2, bool* result, bool arg4);
static const _ConditionList_Evaluate ConditionList_Evaluate = (_ConditionList_Evaluate)0x680C60;

// Get actor value (for skill checks)
typedef SInt32 (__thiscall *_GetActorValue)(void* actorValueOwner, UInt32 avCode);
static const _GetActorValue GetActorValue = (_GetActorValue)0x66EF50;

// Get item count from inventory: TESObjectREFR::GetItemCountinContainer at 0x575610
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

	// Validate category filter if provided
	if (categoryFilter && categoryFilter->typeID != kFormType_RecipeCategory)
		categoryFilter = nullptr;

	PlayerCharacter* player = PlayerCharacter::GetSingleton();
	if (!player) return true;

	// Get recipe list from data handler (direct pointer access since DataHandler::Get() isn't linked)
	DataHandler* dataHandler = *(DataHandler**)0x011C3F2C;
	if (!dataHandler) return true;

	std::vector<TESForm*> availableRecipes;

	// Iterate through all recipes
	tList<TESRecipe>* recipeList = &dataHandler->recipeList;

	for (auto iter = recipeList->Begin(); !iter.End(); ++iter)
	{
		TESRecipe* recipe = iter.Get();
		if (!recipe) continue;

		// Filter by category if specified
		if (categoryFilter)
		{
			TESRecipeCategory* cat = recipe->category;
			TESRecipeCategory* subCat = recipe->subCategory;
			if (cat != categoryFilter && subCat != categoryFilter)
				continue;
		}

		// 1. Evaluate conditions
		void* conditionList = &recipe->conditions;
		bool evalResult = false;
		bool conditionsPassed = ConditionList_Evaluate(conditionList, player, nullptr, &evalResult, false);
		if (!conditionsPassed)
			continue;

		// 2. Check skill requirement
		if (recipe->reqSkill != (UInt32)-1 && recipe->reqSkillLevel > 0)
		{
			// ActorValueOwner is at offset 0xA4 in Actor (Actor inherits from MobileObject -> TESObjectREFR)
			void* actorValueOwner = (void*)((UInt8*)player + 0xA4);
			SInt32 playerSkill = GetActorValue(actorValueOwner, recipe->reqSkill);
			if (playerSkill < (SInt32)recipe->reqSkillLevel)
				continue;
		}

		// 3. Check if player has all required input items
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

		// All checks passed - recipe is available
		availableRecipes.push_back(recipe);
	}

	// Create and return array
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

//=============================================================================
// ClampToGround Command
//=============================================================================

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

//=============================================================================
// Message Handler
//=============================================================================

static bool g_hooksInstalled = false;

static void MessageHandler(NVSEMessagingInterface::Message* msg)
{
	switch (msg->type)
	{
		case NVSEMessagingInterface::kMessage_PostLoad:
			// Install hooks AFTER all plugins loaded - ensures proper chaining
			if (!g_hooksInstalled)
			{
				if (Settings::bQuickDrop || Settings::bQuick180)
					PlayerUpdateHook::Init();
				if (Settings::bSlowMotionPhysicsFix)
					SlowMotionPhysicsFix::Init();
				if (Settings::bExplodingPantsFix)
					ExplodingPantsFix::Init();
				if (Settings::bKillActorXPFix)
					KillActorXPFix::Init();
				if (Settings::bReversePickpocketNoKarma)
					ReversePickpocketNoKarmaFix::Init();
				if (Settings::bSaveFileSize)
					SFSH_Init();
				if (Settings::bVATSProjectileFix)
					VATSProjectileFix::Init();
				if (Settings::bVATSLimbFix)
					VATSLimbFix::Init();
				if (Settings::bOwnedBeds)
					OwnedBeds::Init();
				if (Settings::bLocationVisitPopup)
					LocationVisitPopup::Init();
				g_hooksInstalled = true;
			}
			break;

		case NVSEMessagingInterface::kMessage_PostPostLoad:
			// Install camera hooks after ALL plugins loaded (chains to JohnnyGuitar if present)
			if (Settings::bDialogueCamera)
				DCH_InstallCameraHooks();
			if (Settings::bAshPileNames)
				AshPileNames::Init();
			break;

		case NVSEMessagingInterface::kMessage_NewGame:
		case NVSEMessagingInterface::kMessage_PostLoadGame:
			// Build perk entry map for OnEntryPointHandler
			OEPH_BuildEntryMap();

			// AutoGodMode
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
			//init camera override on first frame (after JG has hooked)
			CameraOverride::Init();

			// OwnerNameInfo update (every frame)
			ONI_Update();

			// KeyHeldHandler update (every frame)
			KHH_Update();

			// DoubleTapHandler update (every frame)
			DTH_Update();

			// OnSoundPlayedHandler update (every frame - process queued sound events)
			OSPH_Update();

			// DialogueCameraHandler update (every frame)
			if (Settings::bDialogueCamera)
				DCH_Update();

			// AutoQuickLoad
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

			// AltTabMute
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

//=============================================================================
// Plugin Entry Points
//=============================================================================

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

	// Open log file next to our DLL
	char logPath[MAX_PATH];
	GetModuleFileNameA(GetModuleHandleA("itr-nvse.dll"), logPath, MAX_PATH);
	char* lastSlash = strrchr(logPath, '\\');
	if (lastSlash) strcpy_s(lastSlash + 1, MAX_PATH - (lastSlash + 1 - logPath), "itr-nvse.log");
	g_logFile = fopen(logPath, "w");

	Log("itr-nvse v%s loading...", ITR_VERSION_STR);

	// Get interfaces
	g_msgInterface = (NVSEMessagingInterface*)nvse->QueryInterface(kInterface_Messaging);
	g_consoleInterface = (NVSEConsoleInterface*)nvse->QueryInterface(kInterface_Console);
	g_arrInterface = (NVSEArrayVarInterface*)nvse->QueryInterface(kInterface_ArrayVar);

	if (!g_msgInterface || !g_arrInterface)
	{
		Log("Failed to get required interfaces");
		return false;
	}

	// Load INI settings
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
	Log("  iAutoQuickLoadFrameDelay: %d", Settings::iAutoQuickLoadFrameDelay);

	// QuickDrop/Quick180 hooks are installed in PostLoad message handler
	// This ensures we chain properly with other plugins (e.g., Stewie Tweaks)
	if (Settings::bQuickDrop) Log("QuickDrop enabled (modifier=%d, control=%d)", Settings::iQuickDropModifierKey, Settings::iQuickDropControlID);
	if (Settings::bQuick180) Log("Quick180 enabled (modifier=%d, control=%d)", Settings::iQuick180ModifierKey, Settings::iQuick180ControlID);

	// Delete console log if enabled
	if (Settings::bConsoleLogCleaner)
	{
		DeleteConsoleLog();
		Log("ConsoleLogCleaner: Deleted console log");
	}

	// Initialize MessageBoxQuickClose if enabled
	if (Settings::bMessageBoxQuickClose)
	{
		MBQC_Init();
		Log("MessageBoxQuickClose enabled");
	}

	// Register message listener
	g_msgInterface->RegisterListener(g_pluginHandle, "NVSE", MessageHandler);

	// Register itr-nvse commands
	nvse->SetOpcodeBase(0x4040);
	nvse->RegisterTypedCommand(&kCommandInfo_GetRefsSortedByDistance, kRetnType_Array);
	Log("Registered GetRefsSortedByDistance at opcode 0x4040");

	// Initialize DialogueTextFilter module (registers SetOnDialogueTextEventHandler at 0x3B00)
	if (DTF_Init((void*)nvse)) {
		Log("DialogueTextFilter module initialized (opcode 0x%04X)", DTF_GetOpcode());
	} else {
		Log("DialogueTextFilter module failed to initialize");
	}

	// Initialize OnStealHandler module (registers SetOnStealEventHandler at 0x3B01)
	if (OSH_Init((void*)nvse)) {
		Log("OnStealHandler module initialized (opcode 0x%04X)", OSH_GetOpcode());
	} else {
		Log("OnStealHandler module failed to initialize");
	}

	// Initialize OnWeaponDropHandler module (registers SetOnWeaponDropEventHandler at 0x3B02)
	if (OWDH_Init((void*)nvse)) {
		Log("OnWeaponDropHandler module initialized (opcode 0x%04X)", OWDH_GetOpcode());
	} else {
		Log("OnWeaponDropHandler module failed to initialize");
	}

	// Register Duplicate command at 0x3B03
	nvse->SetOpcodeBase(0x3B03);
	nvse->RegisterTypedCommand(&kCommandInfo_Duplicate, kRetnType_Form);
	Log("Registered Duplicate at opcode 0x3B03");

	// Initialize OnConsoleHandler module (registers at 0x3B04 and 0x3B05)
	if (OCH_Init((void*)nvse)) {
		Log("OnConsoleHandler module initialized (open=0x%04X, close=0x%04X)",
		    OCH_GetOpenOpcode(), OCH_GetCloseOpcode());
	} else {
		Log("OnConsoleHandler module failed to initialize");
	}

	// Initialize OnWeaponJamHandler module (registers at 0x3B06)
	if (OWJH_Init((void*)nvse)) {
		Log("OnWeaponJamHandler module initialized (opcode 0x%04X)", OWJH_GetOpcode());
	} else {
		Log("OnWeaponJamHandler module failed to initialize");
	}

	// Initialize OnKeyStateHandler module (registers at 0x3B07-0x3B0A)
	if (OKSH_Init((void*)nvse)) {
		Log("OnKeyStateHandler module initialized (disabled=0x%04X, enabled=0x%04X)",
		    OKSH_GetDisabledOpcode(), OKSH_GetEnabledOpcode());
	} else {
		Log("OnKeyStateHandler module failed to initialize");
	}

	// Initialize KeyHeldHandler module (registers at 0x3B0B-0x3B0E)
	if (KHH_Init((void*)nvse)) {
		Log("KeyHeldHandler module initialized");
	} else {
		Log("KeyHeldHandler module failed to initialize");
	}

	// Initialize DoubleTapHandler module (registers at 0x3B0F-0x3B12)
	if (DTH_Init((void*)nvse)) {
		Log("DoubleTapHandler module initialized");
	} else {
		Log("DoubleTapHandler module failed to initialize");
	}

	// Initialize OnFrenzyHandler module (registers at 0x3B13)
	if (OFH_Init((void*)nvse)) {
		Log("OnFrenzyHandler module initialized (opcode 0x%04X)", OFH_GetOpcode());
	} else {
		Log("OnFrenzyHandler module failed to initialize");
	}

	// Initialize CornerMessageHandler module (registers at 0x3B14)
	if (CMH_Init((void*)nvse)) {
		Log("CornerMessageHandler module initialized (opcode 0x%04X)", CMH_GetOpcode());
	} else {
		Log("CornerMessageHandler module failed to initialize");
	}

	// Register SetCameraAngle command at 0x3B15
	nvse->SetOpcodeBase(0x3B15);
	nvse->RegisterCommand(&kCommandInfo_SetCameraAngle);
	Log("Registered SetCameraAngle at opcode 0x3B15");

	// Register GetAvailableRecipes command at 0x3B16
	nvse->SetOpcodeBase(0x3B16);
	nvse->RegisterTypedCommand(&kCommandInfo_GetAvailableRecipes, kRetnType_Array);
	Log("Registered GetAvailableRecipes at opcode 0x3B16");

	// Register ClampToGround command at 0x3B1F
	nvse->SetOpcodeBase(0x3B1F);
	nvse->RegisterCommand(&kCommandInfo_ClampToGround);
	Log("Registered ClampToGround at opcode 0x3B1F");

	// Initialize OnEntryPointHandler module (registers at 0x3B17)
	if (OEPH_Init((void*)nvse)) {
		Log("OnEntryPointHandler module initialized (opcode 0x%04X)", OEPH_GetOpcode());
	} else {
		Log("OnEntryPointHandler module failed to initialize");
	}

	// Initialize OnCombatProcedureHandler module (registers at 0x3B18)
	if (OCPH_Init((void*)nvse)) {
		Log("OnCombatProcedureHandler module initialized (opcode 0x%04X)", OCPH_GetOpcode());
	} else {
		Log("OnCombatProcedureHandler module failed to initialize");
	}

	// Initialize OnSoundPlayedHandler module (registers at 0x3B19)
	if (OSPH_Init((void*)nvse)) {
		Log("OnSoundPlayedHandler module initialized (opcode 0x%04X)", OSPH_GetOpcode());
	} else {
		Log("OnSoundPlayedHandler module failed to initialize");
	}

	// Initialize OnFastTravelHandler module (registers at 0x3B1E)
	if (OFTH_Init((void*)nvse)) {
		Log("OnFastTravelHandler module initialized (opcode 0x%04X)", OFTH_GetOpcode());
	} else {
		Log("OnFastTravelHandler module failed to initialize");
	}

	// Initialize FallDamageHandler module (registers at 0x3B1B-0x3B1D)
	if (FDH_Init((void*)nvse)) {
		Log("FallDamageHandler module initialized (SetMult=0x%04X, GetMult=0x%04X)",
		    FDH_GetSetMultOpcode(), FDH_GetGetMultOpcode());
	} else {
		Log("FallDamageHandler module failed to initialize");
	}

	// Initialize DialogueCameraHandler module (requires JohnnyGuitar.dll)
	if (Settings::bDialogueCamera)
	{
		if (DCH_Init((void*)nvse)) {
			Log("DialogueCameraHandler module initialized");
		} else {
			Log("DialogueCameraHandler module failed to initialize");
		}
	}

	// Initialize FakeHitHandler module (registers at 0x3F00-0x3F01)
	if (FakeHit_Init((void*)nvse)) {
		Log("FakeHitHandler module initialized");
	} else {
		Log("FakeHitHandler module failed to initialize");
	}

	// Initialize OwnerNameInfoHandler module
	if (Settings::bOwnerNameInfo)
	{
		if (ONI_Init()) {
			Log("OwnerNameInfoHandler module initialized");
		} else {
			Log("OwnerNameInfoHandler module failed to initialize");
		}
	}

	// SaveFileSizeHandler is deferred to PostLoad to chain after Stewie Tweaks
	if (Settings::bSaveFileSize)
	{
		Log("SaveFileSizeHandler will initialize in PostLoad");
	}

	Log("itr-nvse loaded successfully");

	return true;
}

}
