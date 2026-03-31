#include "nvse/PluginAPI.h"
#include "nvse/CommandTable.h"
#include "nvse/GameAPI.h"
#include "nvse/GameObjects.h"
#include "nvse/ParamInfos.h"
#include "FallDamageHandler.h"
#include "internal/SafeWrite.h"
#include "internal/ScopedLock.h"
#include <unordered_map>

static float g_globalFallDamageMult = 1.0f;
static std::unordered_map<UInt32, float> g_actorFallDamageMults;
static CRITICAL_SECTION g_fdhLock;
static volatile LONG g_fdhLockInit = 0;

static UInt32 s_setMultOpcode = 0;
static UInt32 s_getMultOpcode = 0;

#include "internal/globals.h"

//Returns the effective multiplier in st(0) for the asm hook.
static float __cdecl GetFallDamageMultForActor(UInt32 refID)
{
	InitCriticalSectionOnce(&g_fdhLockInit, &g_fdhLock);
	ScopedLock lock(&g_fdhLock);
	if (refID && !g_actorFallDamageMults.empty())
	{
		auto it = g_actorFallDamageMults.find(refID);
		if (it != g_actorFallDamageMults.end())
			return it->second;
	}
	return g_globalFallDamageMult;
}

namespace FallDamageHook
{
	static const UInt32 kHookAddr = 0x8A63EC;
	static const UInt32 kRetnAddr = 0x8A63F5;

	void __fastcall ApplyFallDamageMult(Actor* actor, float* damage)
	{
		float mult = GetFallDamageMultForActor(actor ? actor->refID : 0);
		*damage *= mult;
	}

	__declspec(naked) void Hook()
	{
		__asm
		{
			push ecx
			push edx

			mov eax, [ebp-0x54]
			test eax, eax
			jz use_global
			mov eax, [eax+0x0C]  //refID at offset 0x0C
			jmp do_call

		use_global:
			xor eax, eax

		do_call:
			push eax  //refID arg
			call GetFallDamageMultForActor
			add esp, 4

			pop edx
			pop ecx

			fmul dword ptr [ebp-0x28]
			fstp dword ptr [ebp-0x28]

			fld dword ptr [ebp-0x28]
			fcomp qword ptr ds:[0x01012060]
			jmp kRetnAddr
		}
	}

	void Init()
	{
		SafeWrite::WriteRelJump(kHookAddr, (UInt32)Hook);
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

	if (mult < 0.0f) mult = 0.0f;

	{
		InitCriticalSectionOnce(&g_fdhLockInit, &g_fdhLock);
		ScopedLock lock(&g_fdhLock);
		if (actor)
		{
			if (mult == 1.0f)
				g_actorFallDamageMults.erase(actor->refID);
			else
				g_actorFallDamageMults[actor->refID] = mult;
		}
		else
		{
			g_globalFallDamageMult = mult;
		}
	}

	if (IsConsoleMode())
	{
		if (actor)
		{
			if (mult == 1.0f)
				Console_Print("SetFallDamageMult >> Cleared override for %08X (using global %.2f)", actor->refID, g_globalFallDamageMult);
			else
				Console_Print("SetFallDamageMult >> Set %08X to %.2f", actor->refID, mult);
		}
		else
			Console_Print("SetFallDamageMult >> Set global to %.2f", mult);
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

	*result = GetFallDamageMultForActor(actor ? actor->refID : 0);

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
	{
		InitCriticalSectionOnce(&g_fdhLockInit, &g_fdhLock);
		ScopedLock lock(&g_fdhLock);
		if (actor)
		{
			g_actorFallDamageMults.erase(actor->refID);
		}
		else
		{
			count = g_actorFallDamageMults.size();
			g_actorFallDamageMults.clear();
			g_globalFallDamageMult = 1.0f;
		}
	}

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
bool Init(void* nvse)
{
	FallDamageHook::Init();
	s_setMultOpcode = 0x4017;
	s_getMultOpcode = 0x4018;
	return true;
}

void RegisterCommands(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterCommand(&kCommandInfo_SetFallDamageMult);
	nvse->RegisterCommand(&kCommandInfo_GetFallDamageMult);
	nvse->RegisterCommand(&kCommandInfo_ClearFallDamageMult);
}

UInt32 GetSetMultOpcode() { return s_setMultOpcode; }
UInt32 GetGetMultOpcode() { return s_getMultOpcode; }
}
