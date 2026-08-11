//cancellable pre-fast-travel event. handlers SetFunctionValue 0 to veto.
//once any handler vetoes, later handlers can't un-veto.

#include <cmath>
#include <cstring>

#include "OnPreFastTravelHandler.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/EventDispatch.h"
#include "internal/Detours.h"
#include "internal/GameLayout.h"
#include "internal/globals.h"

namespace OnPreFastTravelHandler {

constexpr char kEventName[] = "ITR:OnPreFastTravel";

//sub_93CDF0 (0x93CDF0) universal fast travel executor, __userpurge: ecx=PlayerCharacter*,
//ebx=passthrough forwarded untouched to sub_4539A0/sub_93CCE0/sub_972D30, stack arg a3=
//destination TESObjectREFR*, epilogue retn 4. all destructive work (time advance, position
//move, weather reset, loading) lives inside it, so this fires post-confirm, pre-execution.
//its only call site 0x93BF22 belongs to JIP LN, which replaces the E8 rather than chaining
//it and installs on a refcount mid-session, so anything we put there is silently stolen.
//hook the prologue and chain whatever already owns it - JIP's own hook ends in a hardcoded
//jmp 0x93CDF0, so JIP-driven travel still reaches us
constexpr UInt32 kAddr_FastTravelExecutor = 0x93CDF0;
constexpr UInt8 kPrologue_FastTravelExecutor[] = { 0x55, 0x8B, 0xEC, 0x6A, 0xFF }; //push ebp, mov ebp esp, push -1

static Detours::JumpDetour s_travelDetour;
static UInt8* s_travelTrampoline = nullptr; //filled before the patch goes live, for the shim's indirect jmp

static bool DispatchResultCb(NVSEArrayVarInterface::Element& result, void* shouldTravelAddr)
{
	UInt32& shouldTravel = *static_cast<UInt32*>(shouldTravelAddr);
	if (shouldTravel && result.IsValid())
	{
		if (result.type == NVSEArrayVarInterface::Element::kType_Numeric)
			shouldTravel = (result.num != 0.0) ? 1 : 0;
	}
	return true;
}

bool __cdecl ShouldAllowFastTravel(TESObjectREFR* player, TESObjectREFR* marker)
{
	if (!g_eventManagerInterface)
		return true;

	TESForm* destWorldspace = nullptr;
	float distance = 0.0f;
	if (marker)
	{
		void* cell = *(void**)((UInt8*)marker + 0x40);   //TESObjectREFR::parentCell
		if (cell)
			destWorldspace = *(TESForm**)((UInt8*)cell + 0xC0);   //TESObjectCELL::worldSpace, null for interiors
		if (player)
		{
			float dx = *(float*)((UInt8*)marker + 0x30) - *(float*)((UInt8*)player + 0x30);   //0x30 posX
			float dy = *(float*)((UInt8*)marker + 0x34) - *(float*)((UInt8*)player + 0x34);   //0x34 posY
			float dz = *(float*)((UInt8*)marker + 0x38) - *(float*)((UInt8*)player + 0x38);   //0x38 posZ
			distance = sqrtf(dx * dx + dy * dy + dz * dz);
		}
	}

	//a handler triggering fast travel re-enters the executor synchronously, cap the
	//depth and let travels past it proceed, breaking the cycle
	static UInt32 s_depth = 0;
	if (s_depth >= 16)
	{
		static volatile LONG loggedDepth = 0;
		if (InterlockedCompareExchange(&loggedDepth, 1, 0) == 0)
			Log("OnPreFastTravel: recursion cap hit, letting travel proceed");
		return true;
	}

	UInt32 shouldTravel = 1;
	s_depth++;
	g_eventManagerInterface->DispatchEventAlt(kEventName, DispatchResultCb, &shouldTravel,
		player, player, marker, destWorldspace, PackEventFloatArg(distance), &shouldTravel);
	s_depth--;

	if (!shouldTravel)
		Log("OnPreFastTravel: vetoed (marker %08X)", marker ? marker->refID : 0);

	return shouldTravel != 0;
}

//naked shim on the executor prologue. the jmp lands on the first byte, so entry state is the
//same as a call entry: ecx=PlayerCharacter*, ebx=passthrough, [esp]=retaddr, [esp+4]=a3
//destination marker already pushed by the caller. ecx is caller-saved under cdecl so
//ShouldAllowFastTravel's call can clobber it, save/restore it explicitly, ebx is callee-saved
//so cdecl already guarantees it survives untouched. on allow, tail-jump to the trampoline,
//which replays the stolen prologue and continues inside sub_93CDF0 with the stack untouched,
//so its own retn 4 returns straight to the real caller. on deny, emulate that same retn 4. on
//deny the caller sub_93BEA0 still frees the queued request and runs sub_5D14D0(player,1),
//which only sets bit0 of player+0x66D - benign.
__declspec(naked) void FastTravelExecutor_Shim()
{
	__asm
	{
		push ebx                    //passthrough, save/restore for symmetry with ecx below
		push ecx                    //PlayerCharacter*
		mov  eax, [esp+0xC]         //a3, past the two saves and retaddr
		push eax
		push ecx
		call ShouldAllowFastTravel
		add  esp, 8
		pop  ecx
		pop  ebx
		test al, al
		jz   deny
		jmp  s_travelTrampoline     //allow: replay stolen prologue then continue, callee retn 4 returns to caller
	deny:
		ret  4                      //callee stack cleanup, matches executor retn 4
	}
}

bool Init(void* nvseInterface)
{
	NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
	if (nvse->isEditor) return false;

	if (!g_eventManagerInterface)
	{
		Log("OnPreFastTravelHandler: g_eventManagerInterface not ready, aborting Init");
		return false;
	}

	using P = NVSEEventManagerInterface::ParamType;
	using F = NVSEEventManagerInterface::EventFlags;
	static P params[] = {
		P::eParamType_AnyForm,
		P::eParamType_AnyForm,
		P::eParamType_AnyForm,
		P::eParamType_Float,
		P::eParamType_IntPtr,
	};
	g_eventManagerInterface->RegisterEvent(kEventName, 5, params, F::kFlag_FlushOnLoad);

	//an existing E9 means another plugin already owns the prologue, chain it instead of
	//validating vanilla bytes that are no longer there
	const bool chaining = *(UInt8*)kAddr_FastTravelExecutor == 0xE9;
	if (!chaining && std::memcmp((void*)kAddr_FastTravelExecutor, kPrologue_FastTravelExecutor,
		sizeof(kPrologue_FastTravelExecutor)) != 0)
	{
		Log("OnPreFastTravel: executor prologue at 0x%X differs from expected, hook disabled", kAddr_FastTravelExecutor);
		return false;
	}

	if (!s_travelDetour.WriteRelJumpChainable(kAddr_FastTravelExecutor, FastTravelExecutor_Shim,
		sizeof(kPrologue_FastTravelExecutor), &s_travelTrampoline) || !s_travelTrampoline)
	{
		Log("OnPreFastTravel: executor prologue at 0x%X could not be detoured", kAddr_FastTravelExecutor);
		return false;
	}

	Log("OnPreFastTravel: %08X hooked, trampoline=%08X, chained=%d", kAddr_FastTravelExecutor,
		(UInt32)s_travelTrampoline, chaining ? 1 : 0);
	return true;
}

}
