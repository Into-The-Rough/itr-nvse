//fires ITR:OnEffectApplied (effect commits, past resist/reflect/absorb) and
//ITR:OnEffectRemoved. passes (target, parentForm, effectItemIndex, caster) so
//scripts get the source spell/potion, unlike xNVSE OnMagicEffectHit.

#include <Windows.h>
#include <vector>

#include "OnEffectHandler.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/Detours.h"
#include "internal/EventDispatch.h"
#include "internal/EngineFunctions.h"
#include "internal/GameLayout.h"
#include "internal/ScopedLock.h"
#include "internal/globals.h"

namespace {

constexpr UInt32 kAddr_MagicTarget_AddTarget        = 0x8230F0;
constexpr UInt32 kAddr_MagicTarget_RemoveEffect     = 0x8251C0;
constexpr UInt32 kAddr_MagicCaster_GetCasterStats   = 0x815410;
constexpr UInt32 kAddr_MagicItem_GetEffectIndex     = 0x406E70;
constexpr UInt32 kAddr_MagicItem_GetMagicItemFormID = 0x40A1E0;

using _MagicCaster_GetCasterStatsObject = void*  (__thiscall*)(MagicCaster*);
using _MagicItem_GetEffectItemIndex     = char   (__thiscall*)(EffectItemList*, EffectItem*);
using _MagicItem_GetMagicItemFormID     = UInt32 (__thiscall*)(MagicItem*);
using _MagicTarget_AddTarget            = char   (__thiscall*)(MagicTarget*, MagicCaster*, MagicItem*, ActiveEffect*, bool);
using _MagicTarget_RemoveEffect         = void   (__thiscall*)(MagicTarget*, ActiveEffect*, bool);

const auto MagicCaster_GetCasterStatsObject =
	reinterpret_cast<_MagicCaster_GetCasterStatsObject>(kAddr_MagicCaster_GetCasterStats);
const auto MagicItem_GetEffectItemIndex =
	reinterpret_cast<_MagicItem_GetEffectItemIndex>(kAddr_MagicItem_GetEffectIndex);
const auto MagicItem_GetMagicItemFormID =
	reinterpret_cast<_MagicItem_GetMagicItemFormID>(kAddr_MagicItem_GetMagicItemFormID);

Detours::JumpDetour s_detourAddTarget;
Detours::JumpDetour s_detourRemoveEffect;

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

void QueueEffectEvent(const char* name, MagicTarget* thisTgt, MagicItem* magicItem, MagicCaster* caster, EffectItem* effectItem)
{
	Actor* target = MagicTargetToActor(thisTgt);
	if (!target || !magicItem) return;

	UInt32 magicItemFormID = MagicItem_GetMagicItemFormID(magicItem);
	if (!magicItemFormID) return;

	int index = effectItem
		? (int)MagicItem_GetEffectItemIndex(MagicItemGetEffectList(magicItem), effectItem)
		: 0;
	auto* casterActor = static_cast<Actor*>(caster ? MagicCaster_GetCasterStatsObject(caster) : nullptr);

	QueuedEffectEvent e{ name,
		target->refID,
		magicItemFormID,
		index,
		casterActor ? casterActor->refID : 0 };

	InitCriticalSectionOnce(&s_lockInit, &s_lock);
	ScopedLock lock(&s_lock);
	if (s_pending.size() >= kMaxQueued) return;
	s_pending.push_back(e);
}

char __fastcall Hooked_AddTarget(MagicTarget* thisTgt, void* /*edx*/,
	MagicCaster* caster, MagicItem* parent, ActiveEffect* effect, bool extraFlag)
{
	const char result = s_detourAddTarget.GetTrampoline<_MagicTarget_AddTarget>()(
		thisTgt, caster, parent, effect, extraFlag);

	if (result == 1 && effect) {
		QueueEffectEvent("ITR:OnEffectApplied", thisTgt, parent, caster, effect->effectItem);
	}
	return result;
}

void __fastcall Hooked_RemoveEffect(MagicTarget* thisTgt, void* /*edx*/,
	ActiveEffect* effect, bool actuallyRemove)
{
	//resolve while the effect (and its caster) is still live, before the trampoline frees it
	if (actuallyRemove && effect) {
		if (effect->magicItem)
			QueueEffectEvent("ITR:OnEffectRemoved", thisTgt, effect->magicItem, effect->caster, effect->effectItem);
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
