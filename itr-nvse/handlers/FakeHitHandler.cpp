#include "FakeHitHandler.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/EngineFunctions.h"
#include "internal/GameGlobals.h"
#include "internal/GameSDK.h"

#include "internal/NiLayout.h"
#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <cmath>

#define EXTRACT_ARGS_EX paramInfo, scriptData, opcodeOffsetPtr, scriptObj, eventList

typedef bool (*ExtractArgsEx_t)(ParamInfo* paramInfo, void* scriptData, UInt32* opcodeOffsetPtr, Script* scriptObj, ScriptEventList* eventList, ...);

struct NiPoint3 {
	float x, y, z;
	void Normalize() {
		float len = sqrtf(x*x + y*y + z*z);
		if (len > 0.0001f) { x /= len; y /= len; z /= len; }
	}
};

struct ActorHitData {
	Actor* source;
	Actor* target;
	void* projectile;
	UInt32 weaponAV;
	SInt32 hitLocation;
	float healthDmg;
	float wpnBaseDmg;
	float fatigueDmg;
	float limbDmg;
	float blockDTMod;
	float armorDmg;
	float weaponDmg;
	TESObjectWEAP* weapon;
	float weapHealthPerc;
	NiPoint3 impactPos;
	NiPoint3 impactAngle;
	void* critHitEffect;
	void* ptr54;
	UInt32 flags;
	float dmgMult;
	SInt32 unk60;
};

enum MaterialType { kMaterial_Organic = 6 };

struct DecalColor {
	float r, g, b;
};

struct Decal {
	enum Type { kDecalType_Skinned = 2 };
	NiPoint3 worldPos, rotation, point18;
	void* actor;
	NiNode* node;
	UInt32 unk2C;
	TESTexture* textureSet;
	SInt32 index;
	float width, height, depth, rng44;
	TESObjectCELL* parentCell;
	float parallaxScale;
	NiNode* skinnedDecal;
	float specular, epsilon, placementRadius;
	DecalColor vertexColor;
	UInt32 hitLocationFlags;
	UInt8 whichUVQuadrant, byte71, byte72, isParallax, isAlphaTest, alphaBlend, parallaxPasses, modelSpace, forceFade, twoSided;
	UInt8 pad7A[2];
};

//GetBodyPartData dispatches through TESActorBase; NPCs route via race and creatures directly.
static BGSBodyPartData* GetActorBodyPartData(Actor* actor) {
	if (!actor || !actor->baseForm) return nullptr;
	UInt8 typeID = actor->baseForm->typeID;
	if (typeID != kFormType_NPC && typeID != kFormType_Creature) return nullptr;
	return static_cast<TESActorBase*>(actor->baseForm)->GetBodyPartData();
}

static void CopyHitData(BaseProcess* process, ActorHitData* hitData) {
	((void (__thiscall*)(BaseProcess*, ActorHitData*))((*(void***)process)[0x1DD]))(process, hitData);
}

//slot 0x1DE resets hitData240. Without this, knockdown can leave stale last-hit data.
static void ResetHitData(BaseProcess* process) {
	((void (__thiscall*)(BaseProcess*))((*(void***)process)[0x1DE]))(process);
}

static void DamageHealthAndFatigue(Actor* actor, float healthDmg, float fatigueDmg, Actor* source) {
	((void (__thiscall*)(Actor*, float, float, Actor*))((*(void***)actor)[0xCE]))(actor, healthDmg, fatigueDmg, source);
}

static void DamageActorValue(Actor* actor, UInt32 avCode, float damage, Actor* attacker) {
	((void (__thiscall*)(Actor*, UInt32, float, Actor*))((*(void***)actor)[0xEB]))(actor, avCode, damage, attacker);
}

static NiPoint3 GetRefPos(TESObjectREFR* ref) {
	return { ref->posX, ref->posY, ref->posZ };
}

struct Sound {
	UInt32 soundKey;
	UInt8 byte04;
	UInt8 pad05[3];
	UInt32 unk08;
	Sound() : soundKey(0xFFFFFFFF), byte04(0), unk08(0) {}
};

static NVSEScriptInterface* g_scriptInterface = nullptr;
static ExtractArgsEx_t extractArgs = nullptr;

typedef void (__thiscall* AddGeometryDecal_t)(void* decalMgr, Decal* decal, UInt32 decalType, bool ignoreDistToPlayer);
typedef void* (__cdecl* LoadTempEffectParticle_t)(TESObjectCELL* cell, float duration, const char* modelPath, NiPoint3 rotation, NiPoint3 position, float scale, int flags, void* attachNode);
typedef void* (__cdecl* GetObjectByName_t)(void* rootNode, const char* name);
typedef void (__thiscall* InitSoundForm_t)(void* audioMgr, Sound* sound, UInt32 formRefID, UInt32 flags);
typedef void (__thiscall* Sound_SetPos_t)(Sound* sound, float x, float y, float z);
typedef void (__thiscall* Sound_SetNiNode_t)(Sound* sound, NiNode* node);

static AddGeometryDecal_t AddGeometryDecal = (AddGeometryDecal_t)0x4A10D0;
static LoadTempEffectParticle_t LoadTempEffectParticle = (LoadTempEffectParticle_t)0x6890B0;
static GetObjectByName_t GetObjectByName = (GetObjectByName_t)0x4AAE30;
static InitSoundForm_t InitSoundForm = (InitSoundForm_t)0xAE5870;
static Sound_SetPos_t Sound_SetPos = (Sound_SetPos_t)0xAD8B60;
static Sound_SetNiNode_t Sound_SetNiNode = (Sound_SetNiNode_t)0xAD8F20;

//0x522BA0 - TESObjectWEAP::GetImpactData(material). reads weapon+0x24C, remaps raw
//material 0-31 to an impactDatas slot via 0x58E8F0. null if the weapon has no set
typedef BGSImpactData* (__thiscall* GetWeaponImpactData_t)(TESObjectWEAP*, UInt32);
static GetWeaponImpactData_t GetWeaponImpactData = (GetWeaponImpactData_t)0x522BA0;

static BGSImpactData* GetOrganicImpactData(TESObjectWEAP* weapon) {
	return weapon ? GetWeaponImpactData(weapon, kMaterial_Organic) : nullptr;
}

static const char* GetImpactModelPath(BGSImpactData* impactData) {
	return impactData ? impactData->model.nifPath.m_data : nullptr;
}

static const char* GetBodyPartNodeName(SInt32 loc) {
	switch (loc) {
		case 0: return "Bip01 Spine2"; case 1: case 2: case 13: return "Bip01 Head";
		case 3: return "Bip01 L UpperArm"; case 4: return "Bip01 L Forearm";
		case 5: return "Bip01 R UpperArm"; case 6: return "Bip01 R Forearm";
		case 7: return "Bip01 L Thigh"; case 8: return "Bip01 L Calf"; case 9: return "Bip01 L Foot";
		case 10: return "Bip01 R Thigh"; case 11: return "Bip01 R Calf"; case 12: return "Bip01 R Foot";
		case 14: return "Weapon"; default: return "Bip01 Spine2";
	}
}

static const char* GetBodyPartNodeNameAlt(SInt32 loc) {
	switch (loc) {
		case 0: return "Bip01 Spine1"; case 1: case 2: case 13: return "Bip01 Neck1";
		case 3: return "Bip01 L Clavicle"; case 4: return "Bip01 L Hand";
		case 5: return "Bip01 R Clavicle"; case 6: return "Bip01 R Hand";
		case 7: case 10: return "Bip01 Pelvis"; case 8: return "Bip01 L Thigh"; case 9: return "Bip01 L Calf";
		case 11: return "Bip01 R Thigh"; case 12: return "Bip01 R Calf"; case 14: return "Bip01 R Hand";
		default: return "Bip01 Spine1";
	}
}

static float GetBodyPartZOffset(SInt32 loc) {
	switch (loc) {
		case 1: case 2: case 13: return 120.0f;
		case 3: case 5: return 100.0f; case 4: case 6: return 85.0f;
		case 7: case 10: return 50.0f; case 8: case 11: return 25.0f; case 9: case 12: return 5.0f;
		default: return 80.0f;
	}
}

static bool GetBoneWorldPosition(Actor* actor, const char* boneName, NiPoint3* outPos) {
	NiNode* rootNode = actor->GetNiNode();
	if (!rootNode) return false;
	void* bone = GetObjectByName(rootNode, boneName);
	if (!bone) return false;
	float* worldPos = NiAVObjectAsView(bone)->world.translate;
	outPos->x = worldPos[0];
	outPos->y = worldPos[1];
	outPos->z = worldPos[2];
	return true;
}

static NiPoint3 GetBodyImpactPosition(Actor* target, SInt32 hitLocation) {
	NiPoint3 effectPos;
	if (GetBoneWorldPosition(target, GetBodyPartNodeName(hitLocation), &effectPos) ||
		GetBoneWorldPosition(target, GetBodyPartNodeNameAlt(hitLocation), &effectPos)) {
		return effectPos;
	}

	NiPoint3 refPos = GetRefPos(target);
	return { refPos.x, refPos.y, refPos.z + GetBodyPartZOffset(hitLocation) };
}

static NiPoint3 GetImpactDirection(Actor* attacker, const NiPoint3& effectPos) {
	NiPoint3 effectRot = { 0, 1, 0 };
	if (attacker) {
		NiPoint3 attackerPos = GetRefPos(attacker);
		effectRot.x = attackerPos.x - effectPos.x;
		effectRot.y = attackerPos.y - effectPos.y;
		effectRot.z = attackerPos.z - effectPos.z;
		effectRot.Normalize();
	}
	return effectRot;
}

static void PlaceBloodEffect(Actor* target, Actor* attacker, TESObjectWEAP* weapon, SInt32 hitLocation) {
	if (!target || !target->parentCell) return;
	BGSImpactData* impactData = GetOrganicImpactData(weapon);
	if (!impactData) return;

	const char* modelPath = GetImpactModelPath(impactData);
	if (!modelPath || !modelPath[0]) return;

	NiPoint3 effectPos = GetBodyImpactPosition(target, hitLocation);
	NiPoint3 effectRot = GetImpactDirection(attacker, effectPos);

	LoadTempEffectParticle(target->parentCell, impactData->data.effectDuration, modelPath, effectRot, effectPos, 1.0f, 7, nullptr);
}

static void PlaceSkinnedBloodDecal(Actor* target, Actor* attacker, TESObjectWEAP* weapon, SInt32 hitLocation) {
	if (!target || !target->parentCell) return;
	BGSImpactData* impactData = GetOrganicImpactData(weapon);
	if (!impactData || !impactData->textureSet) return;

	NiNode* actorNode = target->GetNiNode();
	void* decalMgr = GetDecalManager();
	if (!actorNode || !decalMgr) return;

	NiPoint3 effectPos = GetBodyImpactPosition(target, hitLocation);
	NiPoint3 effectRot = GetImpactDirection(attacker, effectPos);

	DecalData* di = &impactData->decalData;
	Decal decal;
	memset(&decal, 0, sizeof(Decal));
	decal.worldPos = effectPos; decal.rotation = effectRot; decal.point18 = effectRot;
	decal.actor = target; decal.node = actorNode; decal.textureSet = impactData->textureSet;
	decal.index = -1; decal.width = di->maxWidth; decal.height = di->maxHeight;
	decal.depth = di->depth > 0 ? di->depth : 48.0f; decal.rng44 = 1.0f;
	decal.parentCell = target->parentCell; decal.parallaxScale = di->parallaxScale;
	decal.specular = di->shininess; decal.epsilon = impactData->data.angleThreshold;
	decal.placementRadius = impactData->data.placementRadius;
	decal.vertexColor.r = di->color.red / 255.0f;
	decal.vertexColor.g = di->color.green / 255.0f;
	decal.vertexColor.b = di->color.blue / 255.0f;
	decal.hitLocationFlags = (1 << hitLocation);
	decal.isParallax = (di->flags & 1) ? 1 : 0;
	decal.isAlphaTest = (di->flags & 4) ? 1 : 0;
	decal.alphaBlend = (di->flags & 2) ? 1 : 0;
	decal.parallaxPasses = di->parallaxPasses;
	decal.modelSpace = 1;

	AddGeometryDecal(decalMgr, &decal, Decal::kDecalType_Skinned, false);
}

static void PlayImpactSound(Actor* target, TESObjectWEAP* weapon, SInt32 hitLocation) {
	if (!target) return;
	BGSImpactData* impactData = GetOrganicImpactData(weapon);
	if (!impactData) return;

	NiPoint3 effectPos = GetBodyImpactPosition(target, hitLocation);

	NiNode* actorNode = target->GetNiNode();
	TESSound* sound1 = impactData->sound1;
	if (sound1 && sound1->refID) {
		Sound snd;
		InitSoundForm(g_audioManager, &snd, sound1->refID, 0x102);
		if (snd.soundKey != 0xFFFFFFFF) {
			Sound_SetPos(&snd, effectPos.x, effectPos.y, effectPos.z);
			if (actorNode) Sound_SetNiNode(&snd, actorNode);
			Engine::BSSoundHandle_Play(&snd, false);
		}
	}
	TESSound* sound2 = impactData->sound2;
	if (sound2 && sound2->refID) {
		Sound snd2;
		InitSoundForm(g_audioManager, &snd2, sound2->refID, 0x102);
		if (snd2.soundKey != 0xFFFFFFFF) {
			Sound_SetPos(&snd2, effectPos.x, effectPos.y, effectPos.z);
			if (actorNode) Sound_SetNiNode(&snd2, actorNode);
			Engine::BSSoundHandle_Play(&snd2, false);
		}
	}
}

static bool IsActorTypeID(UInt8 typeID) {
	return typeID == kFormType_ACHR || typeID == kFormType_ACRE;
}

static bool IsWeaponForm(TESForm* form) {
	return form && form->typeID == kFormType_Weapon;
}

//0x5AC750 - fires OnHit/OnHitWith script blocks and NVSE events
typedef bool (__cdecl* MarkScriptEvent_t)(TESForm* eventSource, BaseExtraList* extraDataList, UInt32 eventMask);
static MarkScriptEvent_t MarkScriptEvent = (MarkScriptEvent_t)0x5AC750;

static BaseExtraList* GetExtraDataList(TESObjectREFR* ref) {
	return ref ? &ref->extraDataList : nullptr;
}

static ActorHitData BuildHitData(Actor* target, Actor* attacker, TESObjectWEAP* weapon,
	float damage, float fatigueDmg, float limbDmg, SInt32 hitLocation, UInt32 flags)
{
	ActorHitData hitData;
	memset(&hitData, 0, sizeof(ActorHitData));
	hitData.source = attacker;
	hitData.target = target;
	hitData.projectile = nullptr;
	hitData.weaponAV = weapon ? weapon->weaponSkill : 0;
	hitData.hitLocation = hitLocation;
	hitData.healthDmg = damage;
	hitData.wpnBaseDmg = damage;
	hitData.fatigueDmg = fatigueDmg;
	hitData.limbDmg = limbDmg;
	hitData.weapon = weapon;
	hitData.weapHealthPerc = weapon ? 1.0f : 0.0f;
	NiPoint3 targetPos = GetRefPos(target);
	hitData.impactPos.x = targetPos.x;
	hitData.impactPos.y = targetPos.y;
	hitData.impactPos.z = targetPos.z + 50.0f;
	hitData.impactAngle.z = 1.0f;
	hitData.flags = flags;
	hitData.dmgMult = 1.0f;
	hitData.unk60 = hitLocation;
	return hitData;
}

//0x8C0460 - triggers crime/combat when player attacks an NPC
typedef void (__thiscall* AttackAlarm_t)(Actor* victim, void* attacker, UInt32 minorCrime, int unk);
static AttackAlarm_t AttackAlarm = (AttackAlarm_t)0x8C0460;

static void ApplyHit(Actor* target, Actor* attacker, ActorHitData* hitData,
	float damage, float fatigueDmg, float limbDmg, SInt32 hitLocation,
	TESObjectWEAP* weapon, bool skipOnHit)
{
	CopyHitData(target->baseProcess, hitData);
	DamageHealthAndFatigue(target, damage, fatigueDmg, attacker);
	//limb damage hits the body part's condition AV. resolve it from the actor's
	//body part data - the AV varies per part and per race. DamageActorValue takes
	//a positive damage amount (it gates out negatives for condition AVs 0x19-0x1F)
	if (limbDmg > 0.0f && hitLocation >= 0 && hitLocation <= 14) {
		if (BGSBodyPartData* bpd = GetActorBodyPartData(target))
			if (BGSBodyPart* part = bpd->bodyParts[hitLocation])
				DamageActorValue(target, part->actorValue, limbDmg, attacker);
	}

	if (!skipOnHit) {
		//script events: OnHit (0x80) and OnHitWith (0x100)
		BaseExtraList* targetExtra = GetExtraDataList(target);
		if (attacker)
			MarkScriptEvent(attacker, targetExtra, 0x80);
		if (weapon)
			MarkScriptEvent(weapon, targetExtra, 0x100);

		//hostility: if player is the attacker, trigger crime/combat AI
		if (attacker && attacker == *(Actor**)g_thePlayerPtr)
			AttackAlarm(target, attacker, 0, 0);
	}

	if (weapon) {
		SInt32 bloodLoc = (hitLocation >= 0) ? hitLocation : 0;
		PlaceBloodEffect(target, attacker, weapon, bloodLoc);
		PlaceSkinnedBloodDecal(target, attacker, weapon, bloodLoc);
		PlayImpactSound(target, weapon, bloodLoc);
	}

	ResetHitData(target->baseProcess);
}

static bool Cmd_FakeHit_Execute(COMMAND_ARGS) {
	*result = 0;
	if (!thisObj || !extractArgs) return true;

	if (!IsActorTypeID(thisObj->typeID)) return true;

	Actor* attacker = nullptr;
	float damage = -1.0f;
	TESForm* weaponForm = nullptr;
	SInt32 hitLocation = 0;
	UInt32 flags = 0;
	UInt32 bSkipOnHit = 0;

	if (!extractArgs(EXTRACT_ARGS_EX, &attacker, &damage, &weaponForm, &hitLocation, &flags, &bSkipOnHit)) return true;

	if (attacker && !IsActorTypeID(attacker->typeID)) attacker = nullptr; //non-actor attacker would be misused as Actor* in damage/crime paths

	Actor* target = static_cast<Actor*>(thisObj);
	if (!target->baseProcess) return true;

	if (!IsWeaponForm(weaponForm)) weaponForm = nullptr;
	TESObjectWEAP* weapon = static_cast<TESObjectWEAP*>(weaponForm);
	if (damage < 0.0f) damage = weapon ? (float)weapon->attackDmg.damage : 1.0f;

	auto hitData = BuildHitData(target, attacker, weapon, damage, 0.0f, 0.0f, hitLocation, flags);
	ApplyHit(target, attacker, &hitData, damage, 0.0f, 0.0f, hitLocation, weapon, bSkipOnHit != 0);

	*result = 1;
	return true;
}

static bool Cmd_FakeHitEx_Execute(COMMAND_ARGS) {
	*result = 0;
	if (!thisObj || !extractArgs) return true;

	if (!IsActorTypeID(thisObj->typeID)) return true;

	Actor* attacker = nullptr;
	float damage = 0.0f, fatigueDmg = 0.0f, limbDmg = 0.0f;
	SInt32 hitLocation = 0;
	TESForm* weaponForm = nullptr;
	UInt32 flags = 0;
	UInt32 bSkipOnHit = 0;

	if (!extractArgs(EXTRACT_ARGS_EX, &attacker, &damage, &fatigueDmg, &limbDmg, &weaponForm, &hitLocation, &flags, &bSkipOnHit)) return true;

	if (attacker && !IsActorTypeID(attacker->typeID)) attacker = nullptr; //non-actor attacker would be misused as Actor* in damage/crime paths

	Actor* target = static_cast<Actor*>(thisObj);
	if (!target->baseProcess) return true;

	if (!IsWeaponForm(weaponForm)) weaponForm = nullptr;
	TESObjectWEAP* weapon = static_cast<TESObjectWEAP*>(weaponForm);

	auto hitData = BuildHitData(target, attacker, weapon, damage, fatigueDmg, limbDmg, hitLocation, flags);
	ApplyHit(target, attacker, &hitData, damage, fatigueDmg, limbDmg, hitLocation, weapon, bSkipOnHit != 0);

	*result = 1;
	return true;
}

static void SpawnObjectImpactEffect(TESObjectREFR* obj, BGSImpactData* impactData) {
	if (!obj || !impactData) return;

	NiPoint3 pos = GetRefPos(obj);

	if (obj->parentCell) {
		const char* modelPath = GetImpactModelPath(impactData);
		if (modelPath && modelPath[0]) {
			NiPoint3 rot = { 0, 0, 1 };
			LoadTempEffectParticle(obj->parentCell, impactData->data.effectDuration, modelPath, rot, pos, 1.0f, 7, nullptr);
		}
	}

	TESSound* sounds[2] = { impactData->sound1, impactData->sound2 };
	for (TESSound* sound : sounds) {
		if (!sound || !sound->refID) continue;
		Sound snd;
		InitSoundForm(g_audioManager, &snd, sound->refID, 0x102);
		if (snd.soundKey != 0xFFFFFFFF) {
			Sound_SetPos(&snd, pos.x, pos.y, pos.z);
			Engine::BSSoundHandle_Play(&snd, false);
		}
	}
}

static bool Cmd_FakeImpact_Execute(COMMAND_ARGS) {
	*result = 0;
	if (!thisObj || !extractArgs) return true;

	TESForm* weaponForm = nullptr;
	SInt32 materialType = -1;
	if (!extractArgs(EXTRACT_ARGS_EX, &weaponForm, &materialType)) return true;

	if (!IsWeaponForm(weaponForm)) return true;

	//materialType is a raw MATERIAL_TYPE (0-31), remapped to a slot by GetWeaponImpactData.
	//<0 = omitted; true auto-detect needs a collision (raycast), so fall back to 0 for now
	UInt32 material = (materialType < 0) ? 0 : (UInt32)materialType;
	BGSImpactData* impactData = GetWeaponImpactData(static_cast<TESObjectWEAP*>(weaponForm), material);
	if (!impactData) return true;

	SpawnObjectImpactEffect(thisObj, impactData);
	*result = 1;
	return true;
}

static ParamInfo kParams_FakeHit[6] = {
	{"attacker", kParamType_ObjectRef, 1}, {"damage", kParamType_Float, 1},
	{"weapon", kParamType_ObjectID, 1}, {"hitLocation", kParamType_Integer, 1},
	{"flags", kParamType_Integer, 1}, {"bSkipOnHit", kParamType_Integer, 1}
};

static ParamInfo kParams_FakeHitEx[8] = {
	{"attacker", kParamType_ObjectRef, 1}, {"damage", kParamType_Float, 1},
	{"fatigueDamage", kParamType_Float, 1}, {"limbDamage", kParamType_Float, 1},
	{"weapon", kParamType_ObjectID, 1}, {"hitLocation", kParamType_Integer, 1},
	{"flags", kParamType_Integer, 1}, {"bSkipOnHit", kParamType_Integer, 1}
};

static CommandInfo kCommandInfo_FakeHit = {
	"FakeHit", "", 0, "Simulates a hit on an actor with OnHit events. bSkipOnHit=1 to skip events.", 1, 6, kParams_FakeHit,
	Cmd_FakeHit_Execute, nullptr, nullptr, 0
};

static CommandInfo kCommandInfo_FakeHitEx = {
	"FakeHitEx", "", 0, "Extended FakeHit with fatigue/limb damage. bSkipOnHit=1 to skip events.", 1, 8, kParams_FakeHitEx,
	Cmd_FakeHitEx_Execute, nullptr, nullptr, 0
};

static ParamInfo kParams_FakeImpact[2] = {
	{"weapon", kParamType_ObjectID, 0},
	{"materialType", kParamType_Integer, 1}
};

static CommandInfo kCommandInfo_FakeImpact = {
	"FakeImpact", "", 0, "Spawns a weapon's impact effect (particle + impact sounds) on the calling object. materialType (0-31, optional) picks the impact-data-set slot; omit to use the default.", 1, 2, kParams_FakeImpact,
	Cmd_FakeImpact_Execute, nullptr, nullptr, 0
};

namespace FakeHitHandler {
bool Init(void* nvse)
{
	NVSEInterface* nvseIntf = (NVSEInterface*)nvse;
	g_scriptInterface = (NVSEScriptInterface*)nvseIntf->QueryInterface(kInterface_Script);
	if (g_scriptInterface) extractArgs = (ExtractArgsEx_t)g_scriptInterface->ExtractArgsEx;
	if (!extractArgs) return false;

	return true;
}

void RegisterCommands(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterCommand(&kCommandInfo_FakeHit);
	nvse->RegisterCommand(&kCommandInfo_FakeHitEx);
}

void RegisterCommands2(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterCommand(&kCommandInfo_FakeImpact);
}
}
