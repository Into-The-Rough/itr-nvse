#pragma once

#include <cstddef>

#include "internal/GameSDK.h"

#include "internal/layout/Reputation.h"
#include "internal/layout/Challenge.h"
#include "internal/layout/Projectile.h"
#include "internal/layout/FaceGen.h"
#include "internal/layout/Combat.h"
#include "internal/layout/Magic.h"
#include "internal/layout/VATS.h"
#include "internal/layout/Player.h"
#include "internal/layout/ExtraData.h"
#include "internal/layout/Process.h"

struct BGSVoiceTypeEditorIDView {
	UInt8 pad00[0x1C];
	String editorID;
};

struct TESTopicInfoListNodeView {
	TESTopic::Info* item;
	TESTopicInfoListNodeView* next;
};

struct SettingView {
	void* vtbl;
	union {
		UInt32 uint;
		SInt32 i;
		float f;
		char* str;
	} data;
	char* name;
};

struct BGSTalkingActivatorView {
	TESObjectACTI base;
	Actor* talkingActor;
	BGSVoiceType* voiceType;
};

struct DecalCreationDataView {
	float origin[3];
	float direction[3];
};

struct TESTopicInfoVoiceParentView {
	UInt8 pad00[0x50];
	TESTopic* parentTopic;
};

struct TESActorBaseVoiceFallbackView {
	UInt8 pad00[0x94];
	BGSVoiceType* voiceType;
};

static_assert(offsetof(TESForm, typeID) == 0x04);
static_assert(offsetof(TESForm, refID) == 0x0C);
static_assert(sizeof(TESObjectACTI) == 0x90);
static_assert(offsetof(BGSTalkingActivatorView, talkingActor) == 0x90);
static_assert(offsetof(SettingView, data) == 0x04);
static_assert(offsetof(TESObjectREFR, baseForm) == 0x20);
static_assert(offsetof(TESObjectREFR, rotZ) == 0x2C);
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
static_assert(sizeof(Character) == 0x1C8);
static_assert(sizeof(CharacterView) == sizeof(Character));
static_assert(offsetof(CharacterView, flags) == offsetof(TESForm, flags));
static_assert(offsetof(BaseProcess, processLevel) == 0x28);
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
static_assert(offsetof(DataHandler, perkList) == 0x178);
static_assert(offsetof(ModList, loadedMods) == 0x0C);
static_assert(offsetof(ModInfo, name) == 0x20);
static_assert(offsetof(BGSVoiceTypeEditorIDView, editorID) == 0x1C);
static_assert(offsetof(ExtraContainerChanges, data) == 0x0C);
static_assert(offsetof(ExtraContainerChanges::EntryData, extendData) == 0x00);
static_assert(offsetof(ExtraContainerChanges::EntryData, countDelta) == 0x04);
static_assert(offsetof(ExtraContainerChanges::EntryData, type) == 0x08);
static_assert(sizeof(ExtraContainerChanges::EntryData) == 0x0C);
static_assert(offsetof(TESActorBaseVoiceFallbackView, voiceType) == 0x94);


static_assert(offsetof(DecalCreationDataView, origin) == 0x00);
static_assert(offsetof(DecalCreationDataView, direction) == 0x0C);
inline UInt8 TESFormGetTypeID(TESForm* form)
{
	return form ? form->typeID : 0;
}

inline bool TESFormIsActorRef(TESForm* form)
{
	UInt8 typeID = TESFormGetTypeID(form);
	return typeID == kFormType_ACHR || typeID == kFormType_ACRE;
}

inline bool TESFormIsWeapon(TESForm* form)
{
	return TESFormGetTypeID(form) == kFormType_Weapon;
}

inline TESForm* TESObjectREFRGetBaseForm(TESObjectREFR* ref)
{
	return ref ? ref->baseForm : nullptr;
}

inline float* TESObjectREFRGetRotZ(TESObjectREFR* ref)
{
	return ref ? &ref->rotZ : nullptr;
}

inline float SettingGetFloat(SettingView* setting, float fallback = 0.0f)
{
	return setting ? setting->data.f : fallback;
}

inline Actor* BGSTalkingActivatorGetTalkingActor(TESForm* form)
{
	return form ? reinterpret_cast<BGSTalkingActivatorView*>(form)->talkingActor : nullptr;
}
inline ActorValueOwner* ActorGetActorValueOwner(Actor* actor)
{
	return actor ? &actor->avOwner : nullptr;
}

inline MagicTarget* ActorGetMagicTarget(Actor* actor)
{
	return actor ? &actor->magicTarget : nullptr;
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

inline tList<BGSPerk>* DataHandlerGetPerkList(DataHandler* dataHandler)
{
	return dataHandler ? &dataHandler->perkList : nullptr;
}
inline TESTopic* TESTopicInfoGetParentTopic(TESTopicInfo* topicInfo)
{
	return topicInfo ? reinterpret_cast<TESTopicInfoVoiceParentView*>(topicInfo)->parentTopic : nullptr;
}

inline BGSVoiceType* TESActorBaseGetLegacyVoiceTypeFallback(TESForm* baseForm)
{
	return baseForm ? reinterpret_cast<TESActorBaseVoiceFallbackView*>(baseForm)->voiceType : nullptr;
}
inline NiNode* TESObjectREFRGetNiNodeRaw(TESObjectREFR* ref)
{
	return ref && ref->renderState ? ref->renderState->niNode : nullptr;
}
