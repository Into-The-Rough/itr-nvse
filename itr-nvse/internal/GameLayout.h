#pragma once

#include <cstddef>

#include "internal/DialogueLayout.h"
#include "internal/MenuLayout.h"

#include "nvse/GameData.h"
#include "nvse/GameExtraData.h"
#include "nvse/GameForms.h"
#include "nvse/GameObjects.h"
#include "nvse/GameEffects.h"
#include "nvse/GameProcess.h"

template <typename T>
struct NiTArrayLite {
	void** vtbl;
	T* data;
	UInt16 capacity;
	UInt16 firstFreeEntry;
	UInt16 numObjs;
	UInt16 growSize;
};

struct ProcessManagerLite {
	UInt32 unk000;
	NiTArrayLite<void*> objects;
	UInt32 beginOffsets[4];
	UInt32 endOffsets[4];
};

struct ExtraWeaponModFlagsView {
	void* vtbl;
	UInt8 type;
	UInt8 pad05[3];
	BSExtraData* next;
	UInt8 flags;
	UInt8 pad0D[3];
};

struct ExtraAshPileRefView {
	void* vtbl;
	UInt8 type;
	UInt8 pad05[3];
	BSExtraData* next;
	TESObjectREFR* sourceRef;
};

struct BGSVoiceTypeEditorIDView {
	UInt8 pad00[0x1C];
	String editorID;
};

struct TESTopicInfoListNodeView {
	TESTopic::Info* item;
	TESTopicInfoListNodeView* next;
};

struct BGSTalkingActivatorView {
	TESObjectACTI base;
	Actor* talkingActor;
	BGSVoiceType* voiceType;
};

struct CombatControllerView {
	UInt8 pad00[0x80];
	void* combatGroup;
	UInt8 pad84[0xBC - 0x84];
	Actor* packageOwner;
};

struct CombatStateView {
	UInt8 pad00[0x1C4];
	void* combatController;
};

struct CombatProcedureView {
	void* vtbl;
	void* combatController;
	UInt32 status;
};

struct BGSWorldLocationView {
	float x;
	float y;
	float z;
	UInt32 unk0C;
};

struct CombatTargetView {
	Actor* target;
	SInt32 detectionLevel;
	BGSWorldLocationView lastSeenLocation;
	BGSWorldLocationView detectedLocation;
	BGSWorldLocationView lastFullyVisibleLocation;
	BGSWorldLocationView initialTargetLocation;
	UInt16 searchCount;
	UInt16 attackerCount;
	UInt8 inLOSCount;
	UInt8 inFullLOSCount;
	UInt8 pad4E[2];
	float timestamps[6];
};

struct ProcessDamageDealtView {
	UInt8 pad00[0xAC];
	float damageDealt;
};

struct ActorForceSneakView {
	UInt8 pad00[0x125];
	UInt8 forceSneak;
};

struct ActorTeammateView {
	UInt8 pad00[0x18D];
	bool isTeammate;
};

struct PlayerCasinoDataView {
	UInt8 pad00[0x610];
	void* casinoDataList;
};

struct ProjectileView {
	UInt8 pad00[0xF8];
	TESObjectWEAP* sourceWeap;
};

struct HighProcessFadeView {
	UInt8 pad00[0x3EC];
	float delayTime;
};

struct DecalCreationDataView {
	float origin[3];
	float direction[3];
};

struct VATSCameraDataView {
	UInt8 pad00[0x08];
	UInt32 mode;
	UInt8 pad0C[0x3C - 0x0C];
	UInt32 numKills;
};

struct PlayerKillCamView {
	UInt8 pad00[0xE18];
	float killCamTimer;
};

struct TESCasinoView {
	UInt8 pad00[0x210];
	UInt32 maxWinnings;
};

struct EffectItemListView {
	void* vtbl;
	BSSimpleListNodeView<EffectItem*> effects;
	UInt32 unk0C;
};

struct HighProcessQueuedGreetView {
	UInt8 pad00[0x3E4];
	void* queuedGreetTopic;
};

struct TESTopicInfoVoiceParentView {
	UInt8 pad00[0x50];
	TESTopic* parentTopic;
};

struct TESActorBaseVoiceFallbackView {
	UInt8 pad00[0x94];
	BGSVoiceType* voiceType;
};

struct TESChallengeDataView {
	UInt32 type;
	UInt32 threshold;
	UInt32 flags;
	UInt32 interval;
	UInt16 value1;
	UInt16 value2;
	UInt32 value3;
};

struct TESChallengeView {
	TESForm form;
	TESFullName fullName;
	TESDescription description;
	TESScriptableForm scriptable;
	TESIcon icon;
	BGSMessageIcon msgIcon;
	TESChallengeDataView data;
	UInt32 amount;
	UInt32 challengeFlags;
	TESForm* completionScript;
	TESForm* XNAM;
};

static_assert(offsetof(TESForm, typeID) == 0x04);
static_assert(offsetof(TESForm, refID) == 0x0C);
static_assert(sizeof(TESObjectACTI) == 0x90);
static_assert(offsetof(BGSTalkingActivatorView, talkingActor) == 0x90);
static_assert(offsetof(TESObjectREFR, baseForm) == 0x20);
static_assert(offsetof(TESObjectREFR, posX) == 0x30);
static_assert(offsetof(TESObjectREFR, posY) == 0x34);
static_assert(offsetof(TESObjectREFR, posZ) == 0x38);
static_assert(offsetof(TESObjectREFR, parentCell) == 0x40);
static_assert(offsetof(TESObjectREFR, extraDataList) == 0x44);
static_assert(offsetof(TESObjectREFR, renderState) == 0x64);
static_assert(offsetof(TESObjectREFR::RenderState, niNode) == 0x14);
static_assert(offsetof(MobileObject, baseProcess) == 0x68);
static_assert(offsetof(Actor, magicTarget) == 0x94);
static_assert(offsetof(Actor, avOwner) == 0xA4);
static_assert(offsetof(Actor, ragDollController) == 0xAC);
static_assert(offsetof(Actor, lifeState) == 0x108);
static_assert(offsetof(ActorForceSneakView, forceSneak) == 0x125);
static_assert(offsetof(ActorTeammateView, isTeammate) == 0x18D);
static_assert(offsetof(PlayerCharacter, actorMover) == 0x190);
static_assert(offsetof(PlayerCharacter, bThirdPerson) == 0x64C);
static_assert(offsetof(PlayerCharacter, playerNode) == 0x694);
static_assert(offsetof(PlayerCasinoDataView, casinoDataList) == 0x610);
static_assert(sizeof(Character) == 0x1C8);
static_assert(sizeof(CharacterView) == sizeof(Character));
static_assert(offsetof(CharacterView, flags) == offsetof(TESForm, flags));
static_assert(offsetof(BaseProcess, processLevel) == 0x28);
static_assert(offsetof(ProcessDamageDealtView, damageDealt) == 0xAC);
static_assert(offsetof(BaseProcess::WeaponInfo, weapon) == 0x08);
static_assert(offsetof(BaseProcess::AmmoInfo, count) == 0x04);
static_assert(offsetof(BaseProcess::AmmoInfo, ammo) == 0x08);
static_assert(offsetof(BGSAmmoForm, ammo) == 0x04);
static_assert(offsetof(TESObjectWEAP, ammo) == 0xA4);
static_assert(offsetof(TESObjectCELL, flags2) == 0x26);
static_assert(offsetof(TESObjectCELL, extraDataList) == 0x28);
static_assert(offsetof(ExtraOwnership, owner) == 0x0C);
static_assert(offsetof(TESAttackDamageForm, damage) == 0x04);
static_assert(offsetof(TESObjectWEAP, attackDmg) == 0x9C);
static_assert(offsetof(TESObjectWEAP, weaponSkill) == 0x15C);
static_assert(offsetof(TESObjectWEAP, impactDataSet) == 0x24C);
static_assert(offsetof(BGSBodyPart, actorValue) == 0x63);
static_assert(offsetof(BGSBodyPartData, bodyParts) == 0x34);
static_assert(offsetof(TESModel, nifPath) == 0x04);
static_assert(offsetof(BGSImpactData, model) == 0x18);
static_assert(offsetof(BGSImpactData, data) == 0x30);
static_assert(offsetof(BGSImpactData::Data, effectDuration) == 0x00);
static_assert(offsetof(BGSImpactData::Data, angleThreshold) == 0x08);
static_assert(offsetof(BGSImpactData::Data, placementRadius) == 0x0C);
static_assert(offsetof(BGSImpactData, textureSet) == 0x48);
static_assert(offsetof(BGSImpactData, sound1) == 0x4C);
static_assert(offsetof(BGSImpactData, sound2) == 0x50);
static_assert(offsetof(BGSImpactData, decalData) == 0x54);
static_assert(offsetof(DecalData, maxWidth) == 0x04);
static_assert(offsetof(DecalData, maxHeight) == 0x0C);
static_assert(offsetof(DecalData, depth) == 0x10);
static_assert(offsetof(DecalData, shininess) == 0x14);
static_assert(offsetof(DecalData, parallaxScale) == 0x18);
static_assert(offsetof(DecalData, parallaxPasses) == 0x1C);
static_assert(offsetof(DecalData, flags) == 0x1D);
static_assert(offsetof(DecalData, color) == 0x20);
static_assert(offsetof(TESActorBase, baseData) == 0x30);
static_assert(offsetof(TESActorBaseData, voiceType) == 0x20);
static_assert(offsetof(TESActorBaseData, factionList) == 0x2C);
static_assert(offsetof(TESActorBaseData::FactionListData, faction) == 0x00);
static_assert(offsetof(TESActorBase, fullName) == 0xD0);
static_assert(offsetof(TESFullName, name) == 0x04);
static_assert(offsetof(TESFaction, fullName) == 0x18);
static_assert(offsetof(TESFaction, reputation) == 0x38);
static_assert(offsetof(TESNPC, hairColor) == 0x1D8);
static_assert(sizeof(BGSNote) == 0x80);
static_assert(offsetof(BGSNote, noteText) == 0x6C);
static_assert(offsetof(BGSNote, unk07C) == 0x7C);
static_assert(offsetof(BGSNote, read) == 0x7D);
static_assert(sizeof(BGSNoteView) == sizeof(BGSNote));
static_assert(offsetof(BGSNoteView, noteType) == offsetof(BGSNote, unk07C));
static_assert(offsetof(BGSNoteView, read) == offsetof(BGSNote, read));
static_assert(offsetof(String, m_data) == 0x00);
static_assert(offsetof(String, m_dataLen) == 0x04);
static_assert(offsetof(String, m_bufLen) == 0x06);
static_assert(sizeof(String) == 0x08);
static_assert(sizeof(DialogueStringView) == sizeof(String));
static_assert(offsetof(DialogueStringView, m_data) == offsetof(String, m_data));
static_assert(offsetof(DialogueStringView, m_dataLen) == offsetof(String, m_dataLen));
static_assert(offsetof(DialogueStringView, m_bufLen) == offsetof(String, m_bufLen));
static_assert(offsetof(TESTopic, infos) == 0x2C);
static_assert(offsetof(TESTopic::Info, quest) == 0x00);
static_assert(offsetof(TESTopic::Info, infoArray) == 0x04);
static_assert(sizeof(TESTopicInfoListNodeView) == 0x08);
static_assert(offsetof(TESTopicInfoListNodeView, item) == 0x00);
static_assert(offsetof(TESTopicInfoListNodeView, next) == 0x04);
static_assert(offsetof(TESTopicInfoResponse, responseText) == 0x18);
static_assert(offsetof(TESTopicInfoResponse, next) == 0x28);
static_assert(offsetof(TESTopicInfoResponse, data) == 0x00);
static_assert(offsetof(TESTopicInfoResponse::Data, responseNumber) == 0x0C);
static_assert(offsetof(TESTopicInfo, flags1) == 0x25);
static_assert(offsetof(TESTopicInfo, flags2) == 0x26);
static_assert(offsetof(TESTopicInfo, unk48) == 0x48);
static_assert(offsetof(TESTopicInfoVoiceParentView, parentTopic) == 0x50);
static_assert(offsetof(MagicItem, list) == 0x0C);
static_assert(offsetof(ActiveEffect, magicItem) == 0x08);
static_assert(offsetof(ActiveEffect, effectItem) == 0x0C);
static_assert(offsetof(ActiveEffect, caster) == 0x28);
static_assert(offsetof(ActiveEffect, spellType) == 0x2C);
static_assert(offsetof(DialogueResponse, responseText) == 0x00);
static_assert(offsetof(DialogueResponse, emotionType) == 0x08);
static_assert(offsetof(DialogueResponse, emotionValue) == 0x0C);
static_assert(offsetof(DialogueResponse, voiceFileName) == 0x10);
static_assert(offsetof(DialogueResponse, speakerIdle) == 0x18);
static_assert(offsetof(DialogueResponse, listenerIdle) == 0x1C);
static_assert(sizeof(DialogueResponse) == 0x2C);
static_assert(sizeof(DialogueResponseView) == sizeof(DialogueResponse));
static_assert(offsetof(DialogueResponseView, responseText) == offsetof(DialogueResponse, responseText));
static_assert(offsetof(DialogueResponseView, emotionType) == offsetof(DialogueResponse, emotionType));
static_assert(offsetof(DialogueResponseView, emotionValue) == offsetof(DialogueResponse, emotionValue));
static_assert(offsetof(DialogueResponseView, voiceFileName) == offsetof(DialogueResponse, voiceFileName));
static_assert(offsetof(DialogueResponseView, speakerIdle) == offsetof(DialogueResponse, speakerIdle));
static_assert(offsetof(DialogueResponseView, listenerIdle) == offsetof(DialogueResponse, listenerIdle));
static_assert(offsetof(DialogueItem, currentResponse) == 0x08);
static_assert(offsetof(DialogueItem, currentTopicInfo) == 0x0C);
static_assert(offsetof(DialogueItem, currentTopic) == 0x10);
static_assert(offsetof(DialogueItem, currentQuest) == 0x14);
static_assert(offsetof(DialogueItem, currentSpeaker) == 0x18);
static_assert(sizeof(DialogueItem) == 0x1C);
static_assert(sizeof(DialogueItemView) == sizeof(DialogueItem));
static_assert(offsetof(DialogueItemView, responses) == 0x00);
static_assert(offsetof(DialogueItemView, currentResponse) == offsetof(DialogueItem, currentResponse));
static_assert(offsetof(DialogueItemView, currentTopicInfo) == offsetof(DialogueItem, currentTopicInfo));
static_assert(offsetof(DialogueItemView, currentTopic) == offsetof(DialogueItem, currentTopic));
static_assert(offsetof(DialogueItemView, currentQuest) == offsetof(DialogueItem, currentQuest));
static_assert(offsetof(DialogueItemView, currentSpeaker) == offsetof(DialogueItem, currentSpeaker));
static_assert(offsetof(DataHandler, modList) == 0x210);
static_assert(offsetof(DataHandler, soundList) == 0x0D0);
static_assert(offsetof(ModList, loadedMods) == 0x0C);
static_assert(offsetof(ModInfo, name) == 0x20);
static_assert(offsetof(BGSVoiceTypeEditorIDView, editorID) == 0x1C);
static_assert(offsetof(CombatControllerView, combatGroup) == 0x80);
static_assert(offsetof(CombatControllerView, packageOwner) == 0xBC);
static_assert(offsetof(CombatStateView, combatController) == 0x1C4);
static_assert(offsetof(CombatProcedureView, combatController) == 0x04);
static_assert(offsetof(CombatProcedureView, status) == 0x08);
static_assert(sizeof(BGSWorldLocationView) == 0x10);
static_assert(offsetof(CombatTargetView, target) == 0x00);
static_assert(offsetof(CombatTargetView, detectionLevel) == 0x04);
static_assert(offsetof(CombatTargetView, lastSeenLocation) == 0x08);
static_assert(offsetof(CombatTargetView, detectedLocation) == 0x18);
static_assert(offsetof(CombatTargetView, lastFullyVisibleLocation) == 0x28);
static_assert(offsetof(CombatTargetView, initialTargetLocation) == 0x38);
static_assert(offsetof(CombatTargetView, searchCount) == 0x48);
static_assert(offsetof(CombatTargetView, attackerCount) == 0x4A);
static_assert(offsetof(CombatTargetView, inLOSCount) == 0x4C);
static_assert(offsetof(CombatTargetView, inFullLOSCount) == 0x4D);
static_assert(offsetof(CombatTargetView, timestamps) == 0x50);
static_assert(sizeof(CombatTargetView) == 0x68);
static_assert(offsetof(ProjectileView, sourceWeap) == 0xF8);
static_assert(offsetof(HighProcessFadeView, delayTime) == 0x3EC);
static_assert(offsetof(VATSCameraDataView, mode) == 0x08);
static_assert(offsetof(VATSCameraDataView, numKills) == 0x3C);
static_assert(offsetof(PlayerKillCamView, killCamTimer) == 0xE18);
static_assert(offsetof(TESCasinoView, maxWinnings) == 0x210);
static_assert(offsetof(AlchemyItem, effects) == 0x3C);
static_assert(offsetof(EffectItem, setting) == 0x14);
static_assert(offsetof(ExtraContainerChanges, data) == 0x0C);
static_assert(offsetof(ExtraContainerChanges::EntryData, extendData) == 0x00);
static_assert(offsetof(ExtraContainerChanges::EntryData, countDelta) == 0x04);
static_assert(offsetof(ExtraContainerChanges::EntryData, type) == 0x08);
static_assert(sizeof(ExtraContainerChanges::EntryData) == 0x0C);
static_assert(offsetof(EffectItemListView, effects) == 0x04);
static_assert(offsetof(HighProcessQueuedGreetView, queuedGreetTopic) == 0x3E4);
static_assert(offsetof(TESActorBaseVoiceFallbackView, voiceType) == 0x94);
static_assert(sizeof(TESChallengeDataView) == 0x18);
static_assert(sizeof(TESChallengeView) == 0x7C);
static_assert(offsetof(TESChallengeView, fullName) == 0x18);
static_assert(offsetof(TESChallengeView, description) == 0x24);
static_assert(offsetof(TESChallengeView, icon) == 0x38);
static_assert(offsetof(TESChallengeView, data) == 0x54);
static_assert(offsetof(TESChallengeView, data.type) == 0x54);
static_assert(offsetof(TESChallengeView, data.threshold) == 0x58);
static_assert(offsetof(TESChallengeView, data.flags) == 0x5C);
static_assert(offsetof(TESChallengeView, data.interval) == 0x60);
static_assert(offsetof(TESChallengeView, data.value1) == 0x64);
static_assert(offsetof(TESChallengeView, amount) == 0x6C);
static_assert(offsetof(TESChallengeView, challengeFlags) == 0x70);
static_assert(offsetof(TESChallengeView, completionScript) == 0x74);

static_assert(sizeof(NiTArrayLite<void*>) == 0x10);
static_assert(offsetof(ProcessManagerLite, objects) == 0x04);
static_assert(offsetof(ProcessManagerLite, beginOffsets) == 0x14);
static_assert(offsetof(ProcessManagerLite, endOffsets) == 0x24);

static_assert(sizeof(ExtraWeaponModFlagsView) == 0x10, "ExtraWeaponModFlags layout changed");
static_assert(offsetof(ExtraWeaponModFlagsView, flags) == 0x0C, "ExtraWeaponModFlags flags offset changed");
static_assert(sizeof(ExtraAshPileRefView) == 0x10, "ExtraAshPileRef layout changed");
static_assert(offsetof(ExtraAshPileRefView, sourceRef) == 0x0C, "ExtraAshPileRef sourceRef offset changed");
static_assert(offsetof(DecalCreationDataView, origin) == 0x00);
static_assert(offsetof(DecalCreationDataView, direction) == 0x0C);

inline void BaseProcessSetAmmoInfo(BaseProcess* process, BaseProcess::AmmoInfo* ammoInfo)
{
	if (!process) return;
	using SetAmmoInfo_t = void (__thiscall *)(BaseProcess*, BaseProcess::AmmoInfo*);
	auto* vtbl = *reinterpret_cast<UInt32**>(process);
	if (!vtbl) return;
	reinterpret_cast<SetAmmoInfo_t>(vtbl[90])(process, ammoInfo);
}

inline UInt8 TESFormGetTypeID(TESForm* form)
{
	return form ? form->typeID : 0;
}

inline TESForm* TESObjectREFRGetBaseForm(TESObjectREFR* ref)
{
	return ref ? ref->baseForm : nullptr;
}

inline Actor* BGSTalkingActivatorGetTalkingActor(TESForm* form)
{
	return form ? reinterpret_cast<BGSTalkingActivatorView*>(form)->talkingActor : nullptr;
}

inline void* CombatControllerGetCombatGroup(void* combatController)
{
	return combatController ? reinterpret_cast<CombatControllerView*>(combatController)->combatGroup : nullptr;
}

inline Actor* CombatControllerGetPackageOwner(void* combatController)
{
	return combatController ? reinterpret_cast<CombatControllerView*>(combatController)->packageOwner : nullptr;
}

inline void* CombatStateGetCombatController(void* combatState)
{
	return combatState ? reinterpret_cast<CombatStateView*>(combatState)->combatController : nullptr;
}

inline void* CombatProcedureGetCombatController(void* procedure)
{
	return procedure ? reinterpret_cast<CombatProcedureView*>(procedure)->combatController : nullptr;
}

inline void CombatProcedureSetStatus(void* procedure, UInt32 status)
{
	if (procedure) reinterpret_cast<CombatProcedureView*>(procedure)->status = status;
}

inline CombatTargetView* CombatTargetAsView(void* combatTarget)
{
	return reinterpret_cast<CombatTargetView*>(combatTarget);
}

inline const float* BGSWorldLocationGetPosition(const BGSWorldLocationView& location)
{
	return &location.x;
}

inline const BGSWorldLocationView* CombatTargetGetLastSeenLocation(void* combatTarget)
{
	return combatTarget ? &CombatTargetAsView(combatTarget)->lastSeenLocation : nullptr;
}

inline const BGSWorldLocationView* CombatTargetGetDetectedLocation(void* combatTarget)
{
	return combatTarget ? &CombatTargetAsView(combatTarget)->detectedLocation : nullptr;
}

inline const BGSWorldLocationView* CombatTargetGetLastFullyVisibleLocation(void* combatTarget)
{
	return combatTarget ? &CombatTargetAsView(combatTarget)->lastFullyVisibleLocation : nullptr;
}

inline const BGSWorldLocationView* CombatTargetGetInitialLocation(void* combatTarget)
{
	return combatTarget ? &CombatTargetAsView(combatTarget)->initialTargetLocation : nullptr;
}

inline float* LowProcessGetDamageDealtCounter(BaseProcess* process)
{
	return process ? &reinterpret_cast<ProcessDamageDealtView*>(process)->damageDealt : nullptr;
}

inline ActorValueOwner* ActorGetActorValueOwner(Actor* actor)
{
	return actor ? &actor->avOwner : nullptr;
}

inline MagicTarget* ActorGetMagicTarget(Actor* actor)
{
	return actor ? &actor->magicTarget : nullptr;
}

inline void ActorSetForceSneak(Actor* actor, UInt8 forceSneak)
{
	if (actor) reinterpret_cast<ActorForceSneakView*>(actor)->forceSneak = forceSneak;
}

inline bool ActorIsTeammate(Actor* actor)
{
	return actor && reinterpret_cast<ActorTeammateView*>(actor)->isTeammate;
}

inline Actor* ActorValueOwnerToActor(ActorValueOwner* owner)
{
	return owner ? reinterpret_cast<Actor*>(reinterpret_cast<UInt8*>(owner) - offsetof(Actor, avOwner)) : nullptr;
}

inline void* PlayerCharacterGetCasinoDataList(PlayerCharacter* player)
{
	return player ? reinterpret_cast<PlayerCasinoDataView*>(player)->casinoDataList : nullptr;
}

inline float ActorValueOwnerGetBaseValue(ActorValueOwner* owner, UInt32 avCode)
{
	if (!owner) return 0.0f;
	using GetAV_t = float (__thiscall*)(ActorValueOwner*, UInt32);
	auto** vtbl = *reinterpret_cast<void***>(owner);
	return reinterpret_cast<GetAV_t>(vtbl[1])(owner, avCode);
}

inline float ActorValueOwnerGetValue(ActorValueOwner* owner, UInt32 avCode)
{
	if (!owner) return 0.0f;
	using GetAV_t = float (__thiscall*)(ActorValueOwner*, UInt32);
	auto** vtbl = *reinterpret_cast<void***>(owner);
	return reinterpret_cast<GetAV_t>(vtbl[3])(owner, avCode);
}

inline Actor* MagicTargetToActor(MagicTarget* magicTarget)
{
	if (!magicTarget) return nullptr;
	using IsActor_t = bool (__thiscall*)(MagicTarget*);
	auto** vtbl = *reinterpret_cast<void***>(magicTarget);
	if (!reinterpret_cast<IsActor_t>(vtbl[3])(magicTarget)) return nullptr;
	return reinterpret_cast<Actor*>(reinterpret_cast<UInt8*>(magicTarget) - offsetof(Actor, magicTarget));
}

inline EffectNode* MagicTargetGetEffectList(MagicTarget* magicTarget)
{
	return magicTarget ? magicTarget->GetEffectList() : nullptr;
}

inline EffectItemList* MagicItemGetEffectList(MagicItem* magicItem)
{
	return magicItem ? &magicItem->list : nullptr;
}

inline TESObjectWEAP* ProjectileGetSourceWeapon(void* projectile)
{
	return projectile ? reinterpret_cast<ProjectileView*>(projectile)->sourceWeap : nullptr;
}

inline void HighProcessSetDelayTime(void* process, float delayTime)
{
	if (process) reinterpret_cast<HighProcessFadeView*>(process)->delayTime = delayTime;
}

inline TESObjectREFR* ExtraAshPileRefGetSourceRef(BSExtraData* extraData)
{
	return extraData ? reinterpret_cast<ExtraAshPileRefView*>(extraData)->sourceRef : nullptr;
}

inline const float* DecalCreationDataGetOrigin(void* decalData)
{
	return decalData ? reinterpret_cast<DecalCreationDataView*>(decalData)->origin : nullptr;
}

inline const float* DecalCreationDataGetDirection(void* decalData)
{
	return decalData ? reinterpret_cast<DecalCreationDataView*>(decalData)->direction : nullptr;
}

inline BGSAmmoForm* TESObjectWEAPGetAmmoForm(TESObjectWEAP* weapon)
{
	return weapon ? &weapon->ammo : nullptr;
}

inline tList<TESSound>* DataHandlerGetSoundList(DataHandler* dataHandler)
{
	return dataHandler ? &dataHandler->soundList : nullptr;
}

inline UInt32 VATSCameraDataGetMode(void* vatsCameraData)
{
	return vatsCameraData ? reinterpret_cast<VATSCameraDataView*>(vatsCameraData)->mode : 0;
}

inline UInt32 VATSCameraDataGetNumKills(void* vatsCameraData)
{
	return vatsCameraData ? reinterpret_cast<VATSCameraDataView*>(vatsCameraData)->numKills : 0;
}

inline float PlayerCharacterGetKillCamTimer(PlayerCharacter* player)
{
	return player ? reinterpret_cast<PlayerKillCamView*>(player)->killCamTimer : 0.0f;
}

inline UInt32 TESCasinoGetMaxWinnings(TESForm* casino)
{
	return casino ? reinterpret_cast<TESCasinoView*>(casino)->maxWinnings : 0;
}

inline EffectItemListView* AlchemyItemGetEffectListView(AlchemyItem* item)
{
	return item ? reinterpret_cast<EffectItemListView*>(&item->effects) : nullptr;
}

inline TESTopic* TESTopicInfoGetParentTopic(TESTopicInfo* topicInfo)
{
	return topicInfo ? reinterpret_cast<TESTopicInfoVoiceParentView*>(topicInfo)->parentTopic : nullptr;
}

inline void* HighProcessGetQueuedGreetTopic(BaseProcess* process)
{
	return process ? reinterpret_cast<HighProcessQueuedGreetView*>(process)->queuedGreetTopic : nullptr;
}

inline BGSVoiceType* TESActorBaseGetLegacyVoiceTypeFallback(TESForm* baseForm)
{
	return baseForm ? reinterpret_cast<TESActorBaseVoiceFallbackView*>(baseForm)->voiceType : nullptr;
}

inline TESChallengeView* TESChallengeAsView(TESForm* form)
{
	return reinterpret_cast<TESChallengeView*>(form);
}

inline NiNode* TESObjectREFRGetNiNodeRaw(TESObjectREFR* ref)
{
	return ref && ref->renderState ? ref->renderState->niNode : nullptr;
}
