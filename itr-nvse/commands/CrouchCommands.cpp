//ForceCrouch / DisableCrouching
//
//hooks Actor::SetMovementFlag (0x8B39F0) and CombatController::SetShouldSneak
//(0x981520) at function level to cover all callers

#include "CrouchCommands.h"
#include "internal/Detours.h"
#include "internal/ScopedLock.h"
#include "nvse/PluginAPI.h"
#include "nvse/GameAPI.h"
#include "nvse/GameObjects.h"
#include "nvse/GameForms.h"
#include "nvse/CommandTable.h"
#include "nvse/ParamInfos.h"
#include <set>

extern const _ExtractArgs ExtractArgs;
#include "internal/globals.h"

static CRITICAL_SECTION g_crouchLock;
static volatile LONG g_crouchLockInit = 0;
static std::set<UInt32> g_crouchDisabledActors;

static void EnsureCrouchLock() {
	InitCriticalSectionOnce(&g_crouchLockInit, &g_crouchLock);
}

static bool IsCrouchDisabled(UInt32 refID) {
	if (!refID || g_crouchLockInit != 2)
		return false;

	ScopedLock lock(&g_crouchLock);
	return g_crouchDisabledActors.count(refID) != 0;
}

static bool IsActorRef(TESObjectREFR* ref)
{
	if (!ref || !ref->baseForm) return false;
	return ref->baseForm->typeID == kFormType_Creature || ref->baseForm->typeID == kFormType_NPC;
}

typedef void* (__thiscall *_GetCombatController)(Actor*);
static const _GetCombatController GetCombatController = (_GetCombatController)0x8A02D0;

typedef void (__thiscall *_SetShouldSneak)(void*, bool);
typedef void (__thiscall *_SetMovementFlag)(Actor*, UInt32);
static _SetShouldSneak OrigSetShouldSneak = nullptr;
static _SetMovementFlag OrigSetMovementFlag = nullptr;

//trampoline for Actor::SetMovementFlag - strips 0x400 for disabled actors
static void __fastcall Hook_SetMovementFlag(Actor* actor, void* edx, UInt32 flags) {
	if ((flags & 0x400) && IsCrouchDisabled(actor->refID))
		flags &= ~0x400;
	OrigSetMovementFlag(actor, flags);
}

//trampoline for CombatController::SetShouldSneak - forces false for disabled actors
static void __fastcall Hook_SetShouldSneak(void* cc, void* edx, bool shouldSneak) {
	if (shouldSneak) {
		Actor* owner = *(Actor**)((UInt8*)cc + 0xBC);
		if (owner && IsCrouchDisabled(owner->refID))
			shouldSneak = false;
	}
	OrigSetShouldSneak(cc, shouldSneak);
}

static Detours::JumpDetour s_moveFlagDetour;
static Detours::JumpDetour s_sneakDetour;

static bool g_crouchHookInstalled = false;
static bool InstallCrouchHooks() {
	if (g_crouchHookInstalled) return true;
	EnsureCrouchLock();

	//verified in IDA: both targets start with the same 7-byte prologue
	if (!s_moveFlagDetour.WriteRelJump(0x8B39F0, Hook_SetMovementFlag, 7,
			(UInt8**)&OrigSetMovementFlag))
		return false;
	if (!s_sneakDetour.WriteRelJump(0x981520, Hook_SetShouldSneak, 7,
			(UInt8**)&OrigSetShouldSneak))
	{
		if (s_moveFlagDetour.Remove())
			OrigSetMovementFlag = nullptr;
		return false;
	}

	g_crouchHookInstalled = true;
	return true;
}

static ParamInfo kParams_ForceCrouch[1] = {
	{"crouch", kParamType_Integer, 0},
};
DEFINE_COMMAND_PLUGIN(ForceCrouch, "Force actor to crouch (1) or stand (0)", 1, 1, kParams_ForceCrouch);

bool Cmd_ForceCrouch_Execute(COMMAND_ARGS)
{
	*result = 0;
	UInt32 crouch = 0;
	if (!ExtractArgs(EXTRACT_ARGS, &crouch)) return true;
	if (!IsActorRef(thisObj)) return true;

	auto* actor = (Actor*)thisObj;
	typedef UInt32 (__thiscall *_GetFlags)(Actor*);
	auto GetMoveFlags = (_GetFlags)0x8846E0;

	if (!InstallCrouchHooks()) return true;

	void* cc = GetCombatController(actor);
	if (cc)
		OrigSetShouldSneak(cc, (bool)crouch);
	*(UInt8*)((UInt8*)actor + 0x125) = crouch ? 1 : 0; //bForceSneak
	//read-modify-write to preserve other movement bits
	UInt32 flags = GetMoveFlags(actor);
	if (crouch)
		flags |= 0x400;
	else
		flags &= ~0x400;
	OrigSetMovementFlag(actor, flags);

	*result = 1;
	if (IsConsoleMode())
		Console_Print("ForceCrouch >> %s", crouch ? "crouch" : "stand");
	return true;
}

static ParamInfo kParams_DisableCrouching[1] = {
	{"disable", kParamType_Integer, 0},
};
DEFINE_COMMAND_PLUGIN(DisableCrouching, "Prevent actor from crouching (1=disable, 0=enable)", 1, 1, kParams_DisableCrouching);

bool Cmd_DisableCrouching_Execute(COMMAND_ARGS)
{
	*result = 0;
	UInt32 disable = 0;
	if (!ExtractArgs(EXTRACT_ARGS, &disable)) return true;
	if (!IsActorRef(thisObj)) return true;

	if (!InstallCrouchHooks()) return true;

	auto* actor = (Actor*)thisObj;
	if (disable) {
		{
			ScopedLock lock(&g_crouchLock);
			g_crouchDisabledActors.insert(actor->refID);
		}
		//force stand immediately
		void* cc = GetCombatController(actor);
		if (cc)
			OrigSetShouldSneak(cc, false);
		*(UInt8*)((UInt8*)actor + 0x125) = 0;
	} else {
		ScopedLock lock(&g_crouchLock);
		g_crouchDisabledActors.erase(actor->refID);
	}

	*result = 1;
	if (IsConsoleMode())
		Console_Print("DisableCrouching >> %s", disable ? "disabled" : "enabled");
	return true;
}

namespace CrouchCommands {

void ClearState()
{
	if (g_crouchLockInit == 2) {
		ScopedLock lock(&g_crouchLock);
		g_crouchDisabledActors.clear();
	}
}

void RegisterCommands(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterCommand(&kCommandInfo_ForceCrouch);
	nvse->RegisterCommand(&kCommandInfo_DisableCrouching);
}

}
