//fires ITR:OnEffectApplied when a magic effect commits to a target
//(past resist/reflect/absorb gates) and ITR:OnEffectRemoved when the engine
//actually removes an effect from a target.
//
//gap this fills: xNVSE OnMagicEffectHit fires before the resistance check
//and only passes the base EffectSetting + target. it can't say which
//spell/potion the effect came from, so scripts can't read per-slot
//magnitude/duration. these events pass (target, parentForm, effectItemIndex,
//caster) so scripts can call GetNthEffectMagnitude etc.

#include <Windows.h>

#include "OnEffectHandler.h"
#include "internal/NVSEMinimal.h"
#include "internal/Detours.h"
#include "internal/EventDispatch.h"
#include "internal/EngineFunctions.h"

class Actor;
class TESForm;

namespace {

//ActiveEffect layout — verified from ActiveEffect::ActiveEffect at 0x803D30
constexpr UInt32 kAE_pSpell  = 0x08;  //MagicItem*
constexpr UInt32 kAE_pEffect = 0x0C;  //EffectItem*
constexpr UInt32 kAE_pCaster = 0x28;  //MagicCaster*

//inside a MagicItem (SpellItem/AlchemyItem/etc.), the EffectItemList subobject
//starts at offset 0x0C. GetEffectItemIndex expects that subobject ptr, not the
//form base. engine itself does this at 0x823523, 0x8058EE
constexpr UInt32 kMI_EffectItemList = 0x0C;

//TESForm::refID — standard FNV layout
constexpr UInt32 kTESForm_FormID = 0x0C;

//MagicTarget is embedded at +0x94 in Actor — verified in CheckAddEffect at 0x823651
constexpr UInt32 kMagicTargetOffset_InActor = 0x94;

constexpr UInt32 kAddr_MagicTarget_AddTarget        = 0x8230F0;
constexpr UInt32 kAddr_MagicTarget_RemoveEffect     = 0x8251C0;
constexpr UInt32 kAddr_MagicCaster_GetCasterStats   = 0x815410;
constexpr UInt32 kAddr_MagicItem_GetEffectIndex     = 0x406E70;
constexpr UInt32 kAddr_MagicItem_GetMagicItemFormID = 0x40A1E0;

using _MagicCaster_GetCasterStatsObject = void*  (__thiscall*)(void*);
using _MagicItem_GetEffectItemIndex     = char   (__thiscall*)(void*, void*);
using _MagicItem_GetMagicItemFormID     = UInt32 (__thiscall*)(void*);
using _MagicTarget_IsActor              = bool   (__thiscall*)(void*);
using _MagicTarget_AddTarget            = char   (__thiscall*)(void*, void*, void*, void*, bool);
using _MagicTarget_RemoveEffect         = void   (__thiscall*)(void*, void*, bool);

const auto MagicCaster_GetCasterStatsObject =
	reinterpret_cast<_MagicCaster_GetCasterStatsObject>(kAddr_MagicCaster_GetCasterStats);
const auto MagicItem_GetEffectItemIndex =
	reinterpret_cast<_MagicItem_GetEffectItemIndex>(kAddr_MagicItem_GetEffectIndex);
const auto MagicItem_GetMagicItemFormID =
	reinterpret_cast<_MagicItem_GetMagicItemFormID>(kAddr_MagicItem_GetMagicItemFormID);

Detours::JumpDetour s_detourAddTarget;
Detours::JumpDetour s_detourRemoveEffect;

//convert MagicTarget* to owning Actor* if it belongs to one
//uses vtable slot 3 (MagicTargetIsActor) — verified at 0x82363C in CheckAddEffect.
//non-actor magic targets (activators, etc.) return nullptr — HUD scripts don't care
void* MagicTargetToActor(void* magicTarget)
{
	if (!magicTarget) return nullptr;
	void** vtbl = *(void***)magicTarget;
	if (!((_MagicTarget_IsActor)vtbl[3])(magicTarget))
		return nullptr;
	return (char*)magicTarget - kMagicTargetOffset_InActor;
}

void Dispatch(const char* name, void* target, void* magicItem, void* caster, void* effectItem)
{
	if (!g_eventManagerInterface || !target || !magicItem) return;

	//`magicItem` is a MagicItem subobject pointer (the engine's apMagicItem), NOT
	//the owning TESForm. for SpellItem/EnchantmentItem the subobject lives at +0x18
	//in the form; AlchemyItem etc. have their own layout. ask the engine for the
	//formID then resolve to the form base — that's what scripts can use as a ref.
	//runtime MagicItemObjects (perk-built abilities) return 0 → filter out.
	UInt32 formID = MagicItem_GetMagicItemFormID(magicItem);
	if (!formID) return;
	void* parentForm = Engine::LookupFormByID(formID);
	if (!parentForm) return;

	int index = effectItem
		? (int)MagicItem_GetEffectItemIndex((char*)magicItem + kMI_EffectItemList, effectItem)
		: 0;
	void* casterActor = caster ? MagicCaster_GetCasterStatsObject(caster) : nullptr;

	g_eventManagerInterface->DispatchEvent(name, nullptr,
		(TESForm*)target,
		(TESForm*)parentForm,
		index,
		(TESForm*)casterActor);
}

char __fastcall Hooked_AddTarget(void* thisTgt, void* /*edx*/,
	void* caster, void* parent, void* effect, bool extraFlag)
{
	const char result = s_detourAddTarget.GetTrampoline<_MagicTarget_AddTarget>()(
		thisTgt, caster, parent, effect, extraFlag);

	if (result == 1 && effect) {
		void* target = MagicTargetToActor(thisTgt);
		if (target) {
			void* effectItem = *(void**)((char*)effect + kAE_pEffect);
			Dispatch("ITR:OnEffectApplied", target, parent, caster, effectItem);
		}
	}
	return result;
}

void __fastcall Hooked_RemoveEffect(void* thisTgt, void* /*edx*/,
	void* effect, bool actuallyRemove)
{
	//fire only on the actual-removal path. the queue-for-later path
	//(actuallyRemove==false) just sets a flag — UpdateTarget eventually
	//calls RemoveEffect again with actuallyRemove==true, which is where
	//EffectRemoved + destruction happens. firing here covers all paths once.
	if (actuallyRemove && effect) {
		void* target = MagicTargetToActor(thisTgt);
		if (target) {
			void* parent     = *(void**)((char*)effect + kAE_pSpell);
			void* effectItem = *(void**)((char*)effect + kAE_pEffect);
			void* caster     = *(void**)((char*)effect + kAE_pCaster);
			if (parent)
				Dispatch("ITR:OnEffectRemoved", target, parent, caster, effectItem);
		}
	}

	s_detourRemoveEffect.GetTrampoline<_MagicTarget_RemoveEffect>()(
		thisTgt, effect, actuallyRemove);
}

} //anon namespace

namespace OnEffectHandler {

bool Init(void* nvseInterface)
{
	NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
	if (nvse->isEditor) return false;

	//MagicTarget::AddTarget prologue: push ebp + mov ebp,esp + sub esp,0Ch = 6 bytes
	if (!s_detourAddTarget.WriteRelJump(kAddr_MagicTarget_AddTarget, Hooked_AddTarget, 6))
		return false;

	//MagicTarget::RemoveEffect prologue: push ebp + mov ebp,esp + sub esp,10h = 6 bytes
	if (!s_detourRemoveEffect.WriteRelJump(kAddr_MagicTarget_RemoveEffect, Hooked_RemoveEffect, 6))
		return false;

	return true;
}

}
