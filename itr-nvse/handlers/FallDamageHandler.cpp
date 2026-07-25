#include "nvse/PluginAPI.h"
#include "nvse/CommandTable.h"
#include "nvse/GameAPI.h"
#include "nvse/GameObjects.h"
#include "nvse/ParamInfos.h"
#include "FallDamageHandler.h"
#include "internal/FallDamageLogic.h"
#include "internal/Detours.h"
#include "internal/ScopedLock.h"
#include <unordered_map>

static float g_globalFallDamageMult = 1.0f;
static std::unordered_map<UInt32, float> g_actorFallDamageMults;
static CRITICAL_SECTION g_fdhLock;
static volatile LONG g_fdhLockInit = 0;

#include "internal/globals.h"

//returns in st(0), the asm hook below consumes it there
static float __cdecl GetFallDamageMultForActor(UInt32 refID)
{
	InitCriticalSectionOnce(&g_fdhLockInit, &g_fdhLock);
	ScopedLock lock(&g_fdhLock);
	return FallDamageLogic::ResolveMultiplier(refID, g_globalFallDamageMult, g_actorFallDamageMults);
}

namespace FallDamageHook
{
	static const UInt32 kHookAddr = 0x8A63EC;
	//fld [ebp-0x28] / fcomp qword ds:[0x1012060], the compare that gates applying the damage.
	//stewie's bPowerArmorScalesFallDamage takes the same bytes, so chain rather than assume vanilla
	static const UInt8 kVanillaBytes[9] = { 0xD9, 0x45, 0xD8, 0xDC, 0x1D, 0x60, 0x20, 0x01, 0x01 };

	static Detours::JumpDetour s_detour;
	static UInt8* s_trampoline = nullptr;

	__declspec(naked) void Hook()
	{
		__asm
		{
			push ecx                            //caller-saved, cdecl call below clobbers them
			push edx

			mov eax, [ebp-0x54]                 //fall-damage target actor local
			test eax, eax
			jz use_global
			mov eax, [eax+0x0C]                 //TESObjectREFR::refID at +0x0C
			jmp do_call

		use_global:
			xor eax, eax                        //no actor -> fall back to global multiplier

		do_call:
			push eax                            //cdecl arg: refID (or 0)
			call GetFallDamageMultForActor
			add esp, 4

			pop edx
			pop ecx

			fmul dword ptr [ebp-0x28]           //st(0) holds the multiplier, scale the damage float
			fstp dword ptr [ebp-0x28]

			//trampoline replays the compare and returns to 0x8A63F5, or enters the previous owner
			//which scales the same float again and replays it there
			jmp s_trampoline
		}
	}

	void Install()
	{
		const bool chained = *(UInt8*)kHookAddr == 0xE9;
		if (!chained && memcmp((void*)kHookAddr, kVanillaBytes, sizeof(kVanillaBytes)) != 0)
			return;
		//5 bytes when chaining so the trampoline re-emits the foreign jump, 9 when vanilla so it
		//replays the whole compare
		s_detour.WriteRelJumpChainable(kHookAddr, (UInt32)Hook, chained ? 5 : 9, &s_trampoline);
	}
}

static Actor* RefToActor(TESObjectREFR* ref)
{
	if (ref)
	{
		UInt8 typeID = ref->typeID;
		if (typeID == kFormType_ACHR || typeID == kFormType_ACRE)
			return (Actor*)ref;
	}
	return nullptr;
}

static ParamInfo kParams_SetFallDamageMult[2] = {
	{ "multiplier", kParamType_Float, 0 },
	{ "actorRef", kParamType_Actor, 1 },
};

DEFINE_COMMAND_PLUGIN(SetFallDamageMult, "Sets fall damage multiplier (global or per-actor)", 0, 2, kParams_SetFallDamageMult);

bool Cmd_SetFallDamageMult_Execute(COMMAND_ARGS)
{
	*result = 0;
	float mult = 1.0f;
	Actor* actor = nullptr;

	if (!ExtractArgs(EXTRACT_ARGS, &mult, &actor))
		return true;

	if (!actor)
		actor = RefToActor(thisObj);

	FallDamageHandler::SetMultiplier(mult, actor);

	if (IsConsoleMode())
	{
		float effectiveMult = FallDamageHandler::GetMultiplier(actor);
		if (actor)
		{
			if (!FallDamageHandler::HasOverride(actor))
				Console_Print("SetFallDamageMult >> Cleared override for %08X (using global %.2f)", actor->refID, g_globalFallDamageMult);
			else
				Console_Print("SetFallDamageMult >> Set %08X to %.2f", actor->refID, effectiveMult);
		}
		else
			Console_Print("SetFallDamageMult >> Set global to %.2f", effectiveMult);
	}

	*result = 1;
	return true;
}

static ParamInfo kParams_GetFallDamageMult[1] = {
	{ "actorRef", kParamType_Actor, 1 },
};

DEFINE_COMMAND_PLUGIN(GetFallDamageMult, "Gets fall damage multiplier (global or per-actor)", 0, 1, kParams_GetFallDamageMult);

bool Cmd_GetFallDamageMult_Execute(COMMAND_ARGS)
{
	Actor* actor = nullptr;
	ExtractArgs(EXTRACT_ARGS, &actor);
	if (!actor)
		actor = RefToActor(thisObj);

	*result = FallDamageHandler::GetMultiplier(actor);

	if (IsConsoleMode())
	{
		if (actor)
			Console_Print("GetFallDamageMult >> %08X = %.2f", actor->refID, *result);
		else
			Console_Print("GetFallDamageMult >> global = %.2f", *result);
	}

	return true;
}

static ParamInfo kParams_ClearFallDamageMult[1] = {
	{ "actorRef", kParamType_Actor, 1 },
};

DEFINE_COMMAND_PLUGIN(ClearFallDamageMult, "Clears fall damage multiplier override", 0, 1, kParams_ClearFallDamageMult);

bool Cmd_ClearFallDamageMult_Execute(COMMAND_ARGS)
{
	*result = 0;
	Actor* actor = nullptr;

	ExtractArgs(EXTRACT_ARGS, &actor);

	if (!actor)
		actor = RefToActor(thisObj);

	size_t count = 0;
	if (!actor)
	{
		InitCriticalSectionOnce(&g_fdhLockInit, &g_fdhLock);
		ScopedLock lock(&g_fdhLock);
		count = g_actorFallDamageMults.size();
	}
	FallDamageHandler::ClearMultiplier(actor);

	if (IsConsoleMode())
	{
		if (actor)
			Console_Print("ClearFallDamageMult >> Cleared %08X", actor->refID);
		else
			Console_Print("ClearFallDamageMult >> Cleared all (%d actors + global)", count);
	}

	*result = 1;
	return true;
}

namespace FallDamageHandler {
//called at PostPostLoad so a plugin that took 0x8A63EC during its own load is already visible
//and can be chained
void InstallHook()
{
	FallDamageHook::Install();
}

bool HasOverride(Actor* actor)
{
	if (!actor)
		return false;

	InitCriticalSectionOnce(&g_fdhLockInit, &g_fdhLock);
	ScopedLock lock(&g_fdhLock);
	return g_actorFallDamageMults.find(actor->refID) != g_actorFallDamageMults.end();
}

void SetMultiplier(float mult, Actor* actor)
{
	mult = FallDamageLogic::ClampMultiplier(mult);

	InitCriticalSectionOnce(&g_fdhLockInit, &g_fdhLock);
	ScopedLock lock(&g_fdhLock);
	if (actor)
	{
		if (FallDamageLogic::StoresActorOverride(mult))
			g_actorFallDamageMults[actor->refID] = mult;
		else
			g_actorFallDamageMults.erase(actor->refID);
	}
	else
	{
		g_globalFallDamageMult = mult;
	}
}

float GetMultiplier(Actor* actor)
{
	return GetFallDamageMultForActor(actor ? actor->refID : 0);
}

void ClearMultiplier(Actor* actor)
{
	InitCriticalSectionOnce(&g_fdhLockInit, &g_fdhLock);
	ScopedLock lock(&g_fdhLock);
	if (actor)
	{
		g_actorFallDamageMults.erase(actor->refID);
	}
	else
	{
		g_actorFallDamageMults.clear();
		g_globalFallDamageMult = 1.0f;
	}
}

//load teardown, refIDs from the previous session go stale across loads
void ClearState()
{
	InitCriticalSectionOnce(&g_fdhLockInit, &g_fdhLock);
	ScopedLock lock(&g_fdhLock);
	g_actorFallDamageMults.clear();
}

void RegisterCommands(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterCommand(&kCommandInfo_SetFallDamageMult);
	nvse->RegisterCommand(&kCommandInfo_GetFallDamageMult);
	nvse->RegisterCommand(&kCommandInfo_ClearFallDamageMult);
}
}
