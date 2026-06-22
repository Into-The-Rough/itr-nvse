//fires ITR:OnEffectApplied (effect commits, past resist/reflect/absorb) and
//ITR:OnEffectRemoved. passes (target, parentForm, effectItemIndex, caster) so
//scripts get the source spell/potion, unlike xNVSE OnMagicEffectHit.

#include <Windows.h>
#include <vector>

#include "OnEffectHandler.h"
#include "internal/NVSEMinimal.h"
#include "internal/Detours.h"
#include "internal/EventDispatch.h"
#include "internal/EngineFunctions.h"
#include "internal/ScopedLock.h"
#include "internal/globals.h"

class Actor;
class TESForm;

namespace {

//ActiveEffect layout - verified from ActiveEffect::ActiveEffect at 0x803D30
constexpr UInt32 kAE_pSpell  = 0x08;  //MagicItem*
constexpr UInt32 kAE_pEffect = 0x0C;  //EffectItem*
constexpr UInt32 kAE_pCaster = 0x28;  //MagicCaster*

//EffectItemList subobject GetEffectItemIndex wants, not the form base (engine: 0x823523)
constexpr UInt32 kMI_EffectItemList = 0x0C;

//TESForm::refID - standard FNV layout
constexpr UInt32 kTESForm_FormID = 0x0C;

//MagicTarget is embedded at +0x94 in Actor - verified in CheckAddEffect at 0x823651
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

//MagicTarget* -> owning Actor* via vtable slot 3 (MagicTargetIsActor); null if not an actor
void* MagicTargetToActor(void* magicTarget)
{
	if (!magicTarget) return nullptr;
	void** vtbl = *(void***)magicTarget;
	if (!((_MagicTarget_IsActor)vtbl[3])(magicTarget))
		return nullptr;
	return (char*)magicTarget - kMagicTargetOffset_InActor;
}

struct QueuedEffectEvent {
	const char* name;
	UInt32 targetRefID;
	UInt32 magicItemFormID;
	int effectIndex;
	UInt32 casterRefID;
};

//AddTarget/RemoveEffect run on the AI linear task thread, so resolve to ids while live and
//re-resolve on the main loop - never touch the form table or run scripts off-main
static std::vector<QueuedEffectEvent> s_pending;
static CRITICAL_SECTION s_lock;
static volatile LONG s_lockInit = 0;
constexpr size_t kMaxQueued = 256;

void QueueEffectEvent(const char* name, void* thisTgt, void* magicItem, void* caster, void* effectItem)
{
	void* target = MagicTargetToActor(thisTgt);
	if (!target || !magicItem) return;

	UInt32 magicItemFormID = MagicItem_GetMagicItemFormID(magicItem);
	if (!magicItemFormID) return;

	int index = effectItem
		? (int)MagicItem_GetEffectItemIndex((char*)magicItem + kMI_EffectItemList, effectItem)
		: 0;
	void* casterActor = caster ? MagicCaster_GetCasterStatsObject(caster) : nullptr;

	QueuedEffectEvent e{ name,
		*(UInt32*)((char*)target + kTESForm_FormID),
		magicItemFormID,
		index,
		casterActor ? *(UInt32*)((char*)casterActor + kTESForm_FormID) : 0 };

	InitCriticalSectionOnce(&s_lockInit, &s_lock);
	ScopedLock lock(&s_lock);
	if (s_pending.size() >= kMaxQueued) return;
	s_pending.push_back(e);
}

char __fastcall Hooked_AddTarget(void* thisTgt, void* /*edx*/,
	void* caster, void* parent, void* effect, bool extraFlag)
{
	const char result = s_detourAddTarget.GetTrampoline<_MagicTarget_AddTarget>()(
		thisTgt, caster, parent, effect, extraFlag);

	if (result == 1 && effect) {
		void* effectItem = *(void**)((char*)effect + kAE_pEffect);
		QueueEffectEvent("ITR:OnEffectApplied", thisTgt, parent, caster, effectItem);
	}
	return result;
}

void __fastcall Hooked_RemoveEffect(void* thisTgt, void* /*edx*/,
	void* effect, bool actuallyRemove)
{
	//resolve while the effect (and its caster) is still live, before the trampoline frees it
	if (actuallyRemove && effect) {
		void* parent     = *(void**)((char*)effect + kAE_pSpell);
		void* effectItem = *(void**)((char*)effect + kAE_pEffect);
		void* caster     = *(void**)((char*)effect + kAE_pCaster);
		if (parent)
			QueueEffectEvent("ITR:OnEffectRemoved", thisTgt, parent, caster, effectItem);
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

void ClearState()
{
	InitCriticalSectionOnce(&s_lockInit, &s_lock);
	ScopedLock lock(&s_lock);
	s_pending.clear();
}

void Update()
{
	if (!g_eventManagerInterface || g_isLoadingSave) return;
	InitCriticalSectionOnce(&s_lockInit, &s_lock);

	std::vector<QueuedEffectEvent> batch;
	{
		ScopedLock lock(&s_lock);
		if (s_pending.empty()) return;
		batch.swap(s_pending);
	}

	for (const auto& e : batch) {
		void* target = Engine::LookupFormByID(e.targetRefID);
		if (!target) continue;
		void* parentForm = Engine::LookupFormByID(e.magicItemFormID);
		if (!parentForm) continue;
		void* caster = e.casterRefID ? Engine::LookupFormByID(e.casterRefID) : nullptr;
		g_eventManagerInterface->DispatchEvent(e.name, nullptr,
			(TESForm*)target, (TESForm*)parentForm, e.effectIndex, (TESForm*)caster);
	}
}

}
