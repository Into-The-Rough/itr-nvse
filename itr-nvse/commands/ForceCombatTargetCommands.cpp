//ForceCombatTarget - force an actor onto a specific combat target via hooked target selection

#include "ForceCombatTargetCommands.h"
#include "internal/EngineFunctions.h"
#include "internal/GameLayout.h"
#include "internal/Detours.h"
#include "internal/ScopedLock.h"
#include "nvse/PluginAPI.h"
#include "nvse/GameAPI.h"
#include "nvse/GameObjects.h"
#include "nvse/GameForms.h"
#include "nvse/CommandTable.h"
#include "nvse/ParamInfos.h"
#include <unordered_map>

extern const _ExtractArgs ExtractArgs;
#include "internal/globals.h"

static bool IsActorRef(TESObjectREFR* ref)
{
	if (!ref || !ref->baseForm) return false;
	return ref->baseForm->typeID == kFormType_Creature || ref->baseForm->typeID == kFormType_NPC;
}

typedef void* (__thiscall *_GetCombatTargetForActor)(void* combatGroup, Actor* target);
typedef bool (__thiscall *_CombatGroupCanAddTarget)(void* combatGroup, Actor* target);
typedef UInt8 (__thiscall *_CharacterIsGuardForForceCombatTarget)(Character*);
typedef void (__thiscall *_CombatControllerSetTarget)(void* combatController, Actor* target);
typedef void (__thiscall *_CombatControllerAddCombatTarget)(void* combatController, Actor* target, SInt32 a3, SInt32 a4, float a5, float a6);
typedef void* (__thiscall *_CombatManagerAddCombatant)(void* combatManager, Actor* actor, Actor* target, SInt32 a4, SInt32 a5);
typedef void (__thiscall *_ActorStartCombat)(Actor* actor, Actor* target, void* combatGroup, bool ignoreActorLimit, bool isAggressor, bool a6, UInt32 a7, bool a8, TESPackage* package);
typedef void (__thiscall *_ActorPutCreatedPackage)(Actor* actor, void* package, UInt32 unk1, UInt32 unk2);
typedef void (__thiscall *_ProcessComputeLastTimeProcessed)(void* process);
typedef void (__thiscall *_ProcessSavePackageToExtraData)(void* process, Actor* actor);
typedef void (__thiscall *_CombatControllerSetByte0C4)(void* combatController);
static const _GetCombatTargetForActor GetCombatTargetForActor = (_GetCombatTargetForActor)0x9865D0;
static const _CombatGroupCanAddTarget CombatGroupCanAddTarget = (_CombatGroupCanAddTarget)0x9866D0;
static const _CharacterIsGuardForForceCombatTarget CharacterIsGuardForForceCombatTarget = (_CharacterIsGuardForForceCombatTarget)0x8D1ED0;
static const _CombatControllerSetTarget CombatControllerSetTarget = (_CombatControllerSetTarget)0x980830;
static const _CombatControllerAddCombatTarget CombatControllerAddCombatTarget = (_CombatControllerAddCombatTarget)0x97F930;
static const _CombatManagerAddCombatant CombatManagerAddCombatant = (_CombatManagerAddCombatant)0x992110;
static const _ActorStartCombat ActorStartCombat = (_ActorStartCombat)0x89FCF0;
static const _ActorPutCreatedPackage ActorPutCreatedPackage = (_ActorPutCreatedPackage)0x87EAC0;
static const _ProcessComputeLastTimeProcessed ProcessComputeLastTimeProcessed = (_ProcessComputeLastTimeProcessed)0x907650;
static const _ProcessSavePackageToExtraData ProcessSavePackageToExtraData = (_ProcessSavePackageToExtraData)0x9130F0;
static const _CombatControllerSetByte0C4 CombatControllerSetByte0C4 = (_CombatControllerSetByte0C4)0x8A0250;
static void** g_combatManager = reinterpret_cast<void**>(0x11F1958);

namespace
{
	using EvaluateCombatTargets_t = Actor* (__thiscall*)(void* combatGroup, Actor* actor);
	using CanAttackActor_t = bool (__thiscall*)(Actor* actor, Actor* target);

	enum class ForceCombatTargetResult
	{
		kSuccess,
		kInvalidArgs,
		kNoProcess,
		kActorDead,
		kHookFailed,
		kNoCombatController,
		kNoCombatGroup,
		kCannotAddTarget,
		kAddTargetFailed,
	};

	static CRITICAL_SECTION g_forceCombatTargetLock;
	static volatile LONG g_forceCombatTargetLockInit = 0;
	static std::unordered_map<UInt32, UInt32> g_forcedCombatTargets;
	//cheap idle gate so the hooks skip all map/lock/engine work when nothing is forced
	static volatile bool s_hasForcedTargets = false;
	static Detours::JumpDetour s_forceCombatTargetDetour;
	static Detours::JumpDetour s_forceCombatCanAttackDetour;
	static EvaluateCombatTargets_t s_evaluateCombatTargetsOriginal = nullptr;
	static CanAttackActor_t s_canAttackActorOriginal = nullptr;
	static bool g_forceCombatTargetHookInstalled = false;
	static UInt32 GetForcedCombatTargetRefID(UInt32 actorRefID);

	static bool IsForcedCombatTargetPair(Actor* actor, Actor* target)
	{
		if (!actor || !target || actor == target)
			return false;
		//map lookup first so dead-checks only run for pairs we actually forced
		if (GetForcedCombatTargetRefID(actor->refID) != target->refID)
			return false;
		return !Engine::Actor_IsDead(actor, false) && !Engine::Actor_IsDead(target, false);
	}

	static void* TryAddCombatantBootstrap(Actor* actor, Actor* target, bool ignoreActorLimit)
	{
		if (!actor || !target || !g_combatManager || !*g_combatManager)
			return nullptr;

		//call through the trampoline, 0x8B0670 is detoured to Hook_CanAttackActor
		//only reached behind the g_forceCombatTargetHookInstalled gate so the original is set
		if (!s_canAttackActorOriginal(actor, target))
			return nullptr;

		void* combatController = CombatManagerAddCombatant(*g_combatManager, actor, target, 0, 0);
		if (!combatController)
			return nullptr;

		if (ignoreActorLimit)
			CombatControllerSetByte0C4(combatController);

		void* process = Engine::Actor_GetProcess(actor);
		if (process)
		{
			ProcessComputeLastTimeProcessed(process);
			ProcessSavePackageToExtraData(process, actor);
		}

		ActorPutCreatedPackage(actor, combatController, 0, 1);
		actor->unk104 = 1;
		return combatController;
	}

	static void EnsureForceCombatTargetLockInit()
	{
		InitCriticalSectionOnce(&g_forceCombatTargetLockInit, &g_forceCombatTargetLock);
	}

	static UInt32 GetForcedCombatTargetRefID(UInt32 actorRefID)
	{
		if (!actorRefID || g_forceCombatTargetLockInit != 2)
			return 0;

		ScopedLock lock(&g_forceCombatTargetLock);
		auto it = g_forcedCombatTargets.find(actorRefID);
		return it != g_forcedCombatTargets.end() ? it->second : 0;
	}

	static void SetForcedCombatTargetRefID(UInt32 actorRefID, UInt32 targetRefID)
	{
		if (!actorRefID || !targetRefID)
			return;

		EnsureForceCombatTargetLockInit();
		ScopedLock lock(&g_forceCombatTargetLock);
		g_forcedCombatTargets[actorRefID] = targetRefID;
		s_hasForcedTargets = true;
	}

	static void ClearForcedCombatTargetRefID(UInt32 actorRefID)
	{
		if (!actorRefID || g_forceCombatTargetLockInit != 2)
			return;

		ScopedLock lock(&g_forceCombatTargetLock);
		g_forcedCombatTargets.erase(actorRefID);
		s_hasForcedTargets = !g_forcedCombatTargets.empty();
	}

	static Actor* __fastcall Hook_EvaluateCombatTargets(void* combatGroup, void*, Actor* actor)
	{
		if (!s_hasForcedTargets || !combatGroup || !actor)
			return s_evaluateCombatTargetsOriginal(combatGroup, actor);

		UInt32 forcedTargetRefID = GetForcedCombatTargetRefID(actor->refID);
		if (!forcedTargetRefID)
			return s_evaluateCombatTargetsOriginal(combatGroup, actor);

		TESForm* forcedForm = (TESForm*)Engine::LookupFormByID(forcedTargetRefID);
		Actor* forcedTarget = forcedForm && IsActorRef((TESObjectREFR*)forcedForm) ? (Actor*)forcedForm : nullptr;
		if (!forcedTarget || forcedTarget == actor || Engine::Actor_IsDead(forcedTarget, false))
		{
			ClearForcedCombatTargetRefID(actor->refID);
			return s_evaluateCombatTargetsOriginal(combatGroup, actor);
		}

		if (GetCombatTargetForActor(combatGroup, forcedTarget))
			return forcedTarget;

		return s_evaluateCombatTargetsOriginal(combatGroup, actor);
	}

	static bool __fastcall Hook_CanAttackActor(Actor* actor, void*, Actor* target)
	{
		//zero-cost vanilla path while no targets are forced, stale flag read is benign
		if (!s_hasForcedTargets)
			return s_canAttackActorOriginal(actor, target);
		if (IsForcedCombatTargetPair(actor, target))
			return true;
		return s_canAttackActorOriginal(actor, target);
	}

	//installed once from Init at plugin load, never mid-game
	//trampolineOut publishes the original before the jmp goes live so the hooks can call it unconditionally
	static bool InstallForceCombatTargetHooks()
	{
		if (!s_forceCombatTargetDetour.WriteRelJump(0x986C60, Hook_EvaluateCombatTargets, 10,
				(UInt8**)&s_evaluateCombatTargetsOriginal))
			return false;

		if (!s_forceCombatCanAttackDetour.WriteRelJump(0x8B0670, Hook_CanAttackActor, 6,
				(UInt8**)&s_canAttackActorOriginal))
		{
			if (s_forceCombatTargetDetour.Remove())
				s_evaluateCombatTargetsOriginal = nullptr;
			return false;
		}

		g_forceCombatTargetHookInstalled = true;
		return true;
	}

	static const char* ForceCombatTargetResultToString(ForceCombatTargetResult result)
	{
		switch (result)
		{
		case ForceCombatTargetResult::kSuccess:
			return "success";
		case ForceCombatTargetResult::kInvalidArgs:
			return "invalid actor/target";
		case ForceCombatTargetResult::kNoProcess:
			return "actor or target has no current process";
		case ForceCombatTargetResult::kActorDead:
			return "actor or target is dead";
		case ForceCombatTargetResult::kHookFailed:
			return "target-selection hook unavailable";
		case ForceCombatTargetResult::kNoCombatController:
			return "failed to create or get combat controller";
		case ForceCombatTargetResult::kNoCombatGroup:
			return "combat controller has no combat group";
		case ForceCombatTargetResult::kCannotAddTarget:
			return "combat group rejected target (likely faction/aggression relation)";
		case ForceCombatTargetResult::kAddTargetFailed:
			return "target was not added to combat group";
		default:
			return "unknown";
		}
	}

	static ForceCombatTargetResult TryForceCombatTarget(Actor* actor, Actor* target)
	{
		if (!actor || !target || actor == target)
			return ForceCombatTargetResult::kInvalidArgs;
		if (!Engine::Actor_GetProcess(actor) || !Engine::Actor_GetProcess(target))
			return ForceCombatTargetResult::kNoProcess;
		if (Engine::Actor_IsDead(actor, false) || Engine::Actor_IsDead(target, false))
			return ForceCombatTargetResult::kActorDead;
		//hooks are installed eagerly at plugin load, just check the result
		if (!g_forceCombatTargetHookInstalled)
			return ForceCombatTargetResult::kHookFailed;
		SetForcedCombatTargetRefID(actor->refID, target->refID);

		void* combatController = Engine::Actor_GetCombatController(actor);
		if (!combatController)
		{
			const bool ignoreActorLimit = true;
			bool isGuard = actor->baseForm && actor->baseForm->typeID == kFormType_NPC
				&& CharacterIsGuardForForceCombatTarget((Character*)actor) != 0;
			ActorStartCombat(actor, target, nullptr, ignoreActorLimit, !isGuard, false, 0, true, nullptr);
			combatController = Engine::Actor_GetCombatController(actor);
			if (!combatController)
				combatController = TryAddCombatantBootstrap(actor, target, ignoreActorLimit);
		}

		if (!combatController)
		{
			ClearForcedCombatTargetRefID(actor->refID);
			return ForceCombatTargetResult::kNoCombatController;
		}

		void* combatGroup = CombatControllerGetCombatGroup(combatController);
		if (!combatGroup)
		{
			ClearForcedCombatTargetRefID(actor->refID);
			return ForceCombatTargetResult::kNoCombatGroup;
		}

		if (!GetCombatTargetForActor(combatGroup, target))
		{
			if (!CombatGroupCanAddTarget(combatGroup, target))
			{
				ClearForcedCombatTargetRefID(actor->refID);
				return ForceCombatTargetResult::kCannotAddTarget;
			}
			CombatControllerAddCombatTarget(combatController, target, 0, 0, 0.0f, 0.0f);
		}

		if (!GetCombatTargetForActor(combatGroup, target))
		{
			ClearForcedCombatTargetRefID(actor->refID);
			return ForceCombatTargetResult::kAddTargetFailed;
		}

		CombatControllerSetTarget(combatController, target);
		return ForceCombatTargetResult::kSuccess;
	}

	static void ClearForcedCombatTarget(Actor* actor)
	{
		if (!actor)
			return;

		ClearForcedCombatTargetRefID(actor->refID);

		void* combatController = Engine::Actor_GetCombatController(actor);
		if (!combatController)
			return;

		void* combatGroup = CombatControllerGetCombatGroup(combatController);
		if (!combatGroup || !s_evaluateCombatTargetsOriginal)
			return;

		CombatControllerSetTarget(combatController, s_evaluateCombatTargetsOriginal(combatGroup, actor));
	}
}

static ParamInfo kParams_ForceCombatTarget[1] = {
	{"target", kParamType_Actor, 1},
};
DEFINE_COMMAND_PLUGIN(ForceCombatTarget, "Force actor to target a specific combat target; pass 0 to clear", 1, 1, kParams_ForceCombatTarget);

bool Cmd_ForceCombatTarget_Execute(COMMAND_ARGS)
{
	*result = 0;

	if (!thisObj || !IsActorRef(thisObj))
		return true;

	Actor* actor = (Actor*)thisObj;
	Actor* target = nullptr;
	if (!ExtractArgs(EXTRACT_ARGS, &target))
		return true;

	if (!target)
	{
		ClearForcedCombatTarget(actor);
		*result = 1;
		if (IsConsoleMode())
			Console_Print("ForceCombatTarget >> cleared");
		return true;
	}

	ForceCombatTargetResult forceResult = TryForceCombatTarget(actor, target);
	if (forceResult != ForceCombatTargetResult::kSuccess)
	{
		if (IsConsoleMode())
			Console_Print("ForceCombatTarget >> failed: %s", ForceCombatTargetResultToString(forceResult));
		return true;
	}

	*result = 1;
	if (IsConsoleMode())
		Console_Print("ForceCombatTarget >> %08X", target->refID);
	return true;
}

namespace ForceCombatTargetCommands {

void Init()
{
	EnsureForceCombatTargetLockInit();
	if (!InstallForceCombatTargetHooks())
		Log("ForceCombatTarget: hook install failed (site already patched by another mod?)");
}

void ClearState()
{
	if (g_forceCombatTargetLockInit == 2)
	{
		ScopedLock lock(&g_forceCombatTargetLock);
		g_forcedCombatTargets.clear();
		s_hasForcedTargets = false;
	}
}

void RegisterCommands(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterCommand(&kCommandInfo_ForceCombatTarget);
}

}
