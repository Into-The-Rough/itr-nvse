//cancellable pre-fast-travel event. handlers SetFunctionValue 0 to veto.
//once any handler vetoes, later handlers can't un-veto.

#include <cmath>

#include "OnPreFastTravelHandler.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/EventDispatch.h"
#include "internal/Detours.h"
#include "internal/GameLayout.h"
#include "internal/globals.h"

namespace OnPreFastTravelHandler {

constexpr char kEventName[] = "ITR:OnPreFastTravel";

//sub_93CDF0 universal fast travel executor. __userpurge: ecx=PlayerCharacter*,
//ebx=passthrough forwarded untouched to sub_4539A0/sub_93CCE0/sub_972D30,
//stack arg a3=destination TESObjectREFR*. epilogue is retn 4. all destructive work
//(time advance, position move, weather reset, loading) lives inside it, so this
//fires post-confirm, pre-execution.
constexpr UInt32 kAddr_FastTravelExecutor = 0x93CDF0;

static Detours::JumpDetour s_travelDetour;
static UInt8* s_travelTrampoline = nullptr; //for inline asm indirect jump

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

//naked shim on the executor prologue. the target is __userpurge (ecx + ebx + one
//stack arg, callee cleans the stack), so the full register state must survive to
//the trampoline on allow, and we emulate the callee's retn 4 on deny. ebx carries a
//passthrough the executor forwards untouched to its callees, so pushad/popad keeps
//it intact. on deny the caller sub_93BEA0 still frees the queued request and runs
//sub_5D14D0(player,1), which only sets bit0 of player+0x66D - benign.
__declspec(naked) void FastTravelExecutor_Shim()
{
	__asm
	{
		pushad
		pushfd
		mov  eax, [esp+0x28]        //a3 destination marker, orig [esp+4] past pushad+pushfd
		push eax
		push ecx                    //PlayerCharacter*, preserved by pushad
		call ShouldAllowFastTravel
		add  esp, 8
		test al, al
		jz   deny
		popfd
		popad
		jmp  s_travelTrampoline     //allow: replay stolen prologue then continue
	deny:
		popfd
		popad
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

	if (!s_travelDetour.WriteRelJump(kAddr_FastTravelExecutor, (UInt32)FastTravelExecutor_Shim, 5, &s_travelTrampoline))
	{
		Log("OnPreFastTravel: failed to hook fast travel executor at 0x%X", kAddr_FastTravelExecutor);
		return false;
	}

	Log("OnPreFastTravel: hook installed");
	return true;
}

}
