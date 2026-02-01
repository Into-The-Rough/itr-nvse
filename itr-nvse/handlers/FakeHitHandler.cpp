//ported from FakeHitNVSE - kept self-contained to avoid header conflicts

#include "FakeHitHandler.h"
#include "internal/NVSEMinimal.h"
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

struct NiColor { float r, g, b; };
struct NiNode { void* vtbl; };

struct TESForm {
	void* vtbl;
	UInt32 typeID;
	UInt32 flags;
	UInt32 refID;
};

struct TESObjectREFR {
	void* vtbl;
	UInt8 pad04[0x30 - 4];
	float posX, posY, posZ;
	UInt8 pad3C[0x68 - 0x3C];
	UInt32 GetRefID() { return *(UInt32*)((UInt8*)this + 0x0C); }
};

struct Actor;
struct TESObjectWEAP;
struct BaseProcess;

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

struct BGSTextureSet;
struct TESObjectCELL;

struct DecalInfo {
	float minWidth, maxWidth, minHeight, maxHeight, depth, shininess, parallaxScale;
	UInt8 parallaxPasses, flags;
	UInt8 pad1E[2];
	struct { UInt8 r, g, b, a; } color;
};

struct BGSImpactData {
	void* vtbl;
	UInt32 typeID, flags, refID;
	UInt8 pad10[0x30 - 0x10];
	float effectDuration;
	UInt8 effectOrientation;
	UInt8 pad35[3];
	float angleThreshold, placementRadius;
	UInt8 soundLevel;
	UInt8 pad41[3];
	UInt8 noDecalData;
	UInt8 pad45[3];
	BGSTextureSet* textureSet;
	void* sound1;
	void* sound2;
	DecalInfo decalInfo;
};

struct BGSImpactDataSet {
	void* vtbl;
	UInt32 typeID, flags, refID;
	UInt8 pad10[0x1C - 0x10];
	BGSImpactData* impactDatas[12];
};

enum MaterialType { kMaterial_Organic = 6 };

struct Decal {
	enum Type { kDecalType_Skinned = 2 };
	NiPoint3 worldPos, rotation, point18;
	void* actor;
	NiNode* node;
	UInt32 unk2C;
	BGSTextureSet* textureSet;
	SInt32 index;
	float width, height, depth, rng44;
	TESObjectCELL* parentCell;
	float parallaxScale;
	NiNode* skinnedDecal;
	float specular, epsilon, placementRadius;
	NiColor vertexColor;
	UInt32 hitLocationFlags;
	UInt8 whichUVQuadrant, byte71, byte72, isParallax, isAlphaTest, alphaBlend, parallaxPasses, modelSpace, forceFade, twoSided;
	UInt8 pad7A[2];
};

struct TESObjectWEAP {
	UInt8 pad00[0x108];
	UInt16 attackDmg;
	UInt8 pad10A[0x1C4 - 0x10A];
	UInt8 weaponSkill;
	UInt8 pad1C5[0x24C - 0x1C5];
	BGSImpactDataSet* impactDataSet;
};

struct BaseProcess {
	void* vtbl;
	UInt32 unk04[9];
	UInt32 processLevel;
	void CopyHitData(ActorHitData* hitData) {
		((void (__thiscall*)(BaseProcess*, ActorHitData*))((*(void***)this)[0x1DD]))(this, hitData);
	}
};

struct Actor {
	UInt8 pad00[0x40];
	TESObjectCELL* parentCell;
	UInt8 pad44[0x68 - 0x44];
	BaseProcess* baseProcess;

	UInt32 GetRefID() { return *(UInt32*)((UInt8*)this + 0x0C); }
	NiPoint3* GetPos() { return (NiPoint3*)((UInt8*)this + 0x30); }
	TESObjectWEAP* GetEquippedWeapon() { return ((TESObjectWEAP* (__thiscall*)(Actor*))0x88D440)(this); }
	void DamageHealthAndFatigue(float healthDmg, float fatigueDmg, Actor* source) {
		((void (__thiscall*)(Actor*, float, float, Actor*))((*(void***)this)[0xCE]))(this, healthDmg, fatigueDmg, source);
	}
	void DamageActorValue(UInt32 avCode, float damage, Actor* attacker) {
		((void (__thiscall*)(Actor*, UInt32, float, Actor*))((*(void***)this)[0xEB]))(this, avCode, damage, attacker);
	}
	NiNode* GetNiNode() {
		void* renderData = *(void**)((UInt8*)this + 0x64);
		if (!renderData) return nullptr;
		return *(NiNode**)((UInt8*)renderData + 0x14);
	}
};

struct BSString { const char* m_data; UInt16 m_dataLen, m_bufLen; };
struct TESSound { void* vtbl; UInt32 typeID, flags, refID; };
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
typedef void (__thiscall* Sound_Play_t)(Sound* sound, UInt32 arg);

static AddGeometryDecal_t AddGeometryDecal = (AddGeometryDecal_t)0x4A10D0;
static LoadTempEffectParticle_t LoadTempEffectParticle = (LoadTempEffectParticle_t)0x6890B0;
static GetObjectByName_t GetObjectByName = (GetObjectByName_t)0x4AAE30;
static InitSoundForm_t InitSoundForm = (InitSoundForm_t)0xAE5870;
static Sound_SetPos_t Sound_SetPos = (Sound_SetPos_t)0xAD8B60;
static Sound_SetNiNode_t Sound_SetNiNode = (Sound_SetNiNode_t)0xAD8F20;
static Sound_Play_t Sound_Play = (Sound_Play_t)0xAD8830;
static void** g_decalManager = (void**)0x11C57F8;
static void* g_bsAudioManager = (void*)0x11F6EF0;

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
	NiPoint3* worldPos = (NiPoint3*)((UInt8*)bone + 0x8C);
	*outPos = *worldPos;
	return true;
}

static void PlaceBloodEffect(Actor* target, Actor* attacker, TESObjectWEAP* weapon, SInt32 hitLocation) {
	if (!target || !target->parentCell || !weapon || !weapon->impactDataSet) return;
	BGSImpactData* impactData = weapon->impactDataSet->impactDatas[kMaterial_Organic];
	if (!impactData) return;

	BSString* nifPath = (BSString*)((UInt8*)impactData + 0x1C);
	const char* modelPath = nifPath->m_data;
	if (!modelPath || !modelPath[0]) return;

	NiPoint3 effectPos;
	if (!GetBoneWorldPosition(target, GetBodyPartNodeName(hitLocation), &effectPos) &&
		!GetBoneWorldPosition(target, GetBodyPartNodeNameAlt(hitLocation), &effectPos)) {
		NiPoint3* p = target->GetPos();
		effectPos.x = p->x; effectPos.y = p->y; effectPos.z = p->z + GetBodyPartZOffset(hitLocation);
	}

	NiPoint3 effectRot = {0, 1, 0};
	if (attacker) {
		NiPoint3* ap = attacker->GetPos();
		effectRot.x = ap->x - effectPos.x; effectRot.y = ap->y - effectPos.y; effectRot.z = ap->z - effectPos.z;
		effectRot.Normalize();
	}

	LoadTempEffectParticle(target->parentCell, impactData->effectDuration, modelPath, effectRot, effectPos, 1.0f, 7, nullptr);
}

static void PlaceSkinnedBloodDecal(Actor* target, Actor* attacker, TESObjectWEAP* weapon, SInt32 hitLocation) {
	if (!target || !target->parentCell || !weapon || !weapon->impactDataSet) return;
	BGSImpactData* impactData = weapon->impactDataSet->impactDatas[kMaterial_Organic];
	if (!impactData || !impactData->textureSet) return;

	NiNode* actorNode = target->GetNiNode();
	void* decalMgr = *g_decalManager;
	if (!actorNode || !decalMgr) return;

	NiPoint3 effectPos;
	if (!GetBoneWorldPosition(target, GetBodyPartNodeName(hitLocation), &effectPos) &&
		!GetBoneWorldPosition(target, GetBodyPartNodeNameAlt(hitLocation), &effectPos)) {
		NiPoint3* p = target->GetPos();
		effectPos.x = p->x; effectPos.y = p->y; effectPos.z = p->z + GetBodyPartZOffset(hitLocation);
	}

	NiPoint3 effectRot = {0, 1, 0};
	if (attacker) {
		NiPoint3* ap = attacker->GetPos();
		effectRot.x = ap->x - effectPos.x; effectRot.y = ap->y - effectPos.y; effectRot.z = ap->z - effectPos.z;
		effectRot.Normalize();
	}

	DecalInfo* di = &impactData->decalInfo;
	Decal decal;
	memset(&decal, 0, sizeof(Decal));
	decal.worldPos = effectPos; decal.rotation = effectRot; decal.point18 = effectRot;
	decal.actor = target; decal.node = actorNode; decal.textureSet = impactData->textureSet;
	decal.index = -1; decal.width = di->maxWidth; decal.height = di->maxHeight;
	decal.depth = di->depth > 0 ? di->depth : 48.0f; decal.rng44 = 1.0f;
	decal.parentCell = target->parentCell; decal.parallaxScale = di->parallaxScale;
	decal.specular = di->shininess; decal.epsilon = impactData->angleThreshold;
	decal.placementRadius = impactData->placementRadius;
	decal.vertexColor.r = di->color.r / 255.0f;
	decal.vertexColor.g = di->color.g / 255.0f;
	decal.vertexColor.b = di->color.b / 255.0f;
	decal.hitLocationFlags = (1 << hitLocation);
	decal.isParallax = (di->flags & 1) ? 1 : 0;
	decal.isAlphaTest = (di->flags & 4) ? 1 : 0;
	decal.alphaBlend = (di->flags & 2) ? 1 : 0;
	decal.parallaxPasses = di->parallaxPasses;
	decal.modelSpace = 1;

	AddGeometryDecal(decalMgr, &decal, Decal::kDecalType_Skinned, false);
}

static void PlayImpactSound(Actor* target, TESObjectWEAP* weapon, SInt32 hitLocation) {
	if (!target || !weapon || !weapon->impactDataSet) return;
	BGSImpactData* impactData = weapon->impactDataSet->impactDatas[kMaterial_Organic];
	if (!impactData) return;

	NiPoint3 effectPos;
	if (!GetBoneWorldPosition(target, GetBodyPartNodeName(hitLocation), &effectPos) &&
		!GetBoneWorldPosition(target, GetBodyPartNodeNameAlt(hitLocation), &effectPos)) {
		NiPoint3* p = target->GetPos();
		effectPos.x = p->x; effectPos.y = p->y; effectPos.z = p->z + GetBodyPartZOffset(hitLocation);
	}

	NiNode* actorNode = target->GetNiNode();
	TESSound* sound1 = (TESSound*)impactData->sound1;
	if (sound1 && sound1->refID) {
		Sound snd;
		InitSoundForm(g_bsAudioManager, &snd, sound1->refID, 0x102);
		if (snd.soundKey != 0xFFFFFFFF) {
			Sound_SetPos(&snd, effectPos.x, effectPos.y, effectPos.z);
			if (actorNode) Sound_SetNiNode(&snd, actorNode);
			Sound_Play(&snd, 0);
		}
	}
	TESSound* sound2 = (TESSound*)impactData->sound2;
	if (sound2 && sound2->refID) {
		Sound snd2;
		InitSoundForm(g_bsAudioManager, &snd2, sound2->refID, 0x102);
		if (snd2.soundKey != 0xFFFFFFFF) {
			Sound_SetPos(&snd2, effectPos.x, effectPos.y, effectPos.z);
			if (actorNode) Sound_SetNiNode(&snd2, actorNode);
			Sound_Play(&snd2, 0);
		}
	}
}

static bool Cmd_FakeHit_Execute(COMMAND_ARGS) {
	*result = 0;
	if (!thisObj || !extractArgs) return true;

	Actor* attacker = nullptr;
	float damage = -1.0f;
	TESForm* weaponForm = nullptr;
	SInt32 hitLocation = -1;
	UInt32 flags = 0;

	if (!extractArgs(EXTRACT_ARGS_EX, &attacker, &damage, &weaponForm, &hitLocation, &flags)) return true;
	if (!attacker) return true;

	Actor* target = (Actor*)thisObj;
	if (!target->baseProcess) return true;

	TESObjectWEAP* weapon = weaponForm ? (TESObjectWEAP*)weaponForm : nullptr;
	if (damage < 0.0f) damage = weapon ? (float)weapon->attackDmg : 1.0f;

	ActorHitData hitData;
	memset(&hitData, 0, sizeof(ActorHitData));
	hitData.source = attacker; hitData.target = target;
	hitData.weaponAV = weapon ? weapon->weaponSkill : 0;
	hitData.hitLocation = hitLocation; hitData.healthDmg = damage; hitData.wpnBaseDmg = damage;
	hitData.weapon = weapon; hitData.weapHealthPerc = weapon ? 1.0f : 0.0f;
	NiPoint3* tp = target->GetPos();
	hitData.impactPos.x = tp->x; hitData.impactPos.y = tp->y; hitData.impactPos.z = tp->z + 50.0f;
	hitData.impactAngle.z = 1.0f; hitData.flags = flags; hitData.dmgMult = 1.0f; hitData.unk60 = hitLocation;

	target->baseProcess->CopyHitData(&hitData);
	target->DamageHealthAndFatigue(damage, 0.0f, attacker);

	if (weapon) {
		SInt32 bloodLoc = (hitLocation >= 0) ? hitLocation : 0;
		PlaceBloodEffect(target, attacker, weapon, bloodLoc);
		PlaceSkinnedBloodDecal(target, attacker, weapon, bloodLoc);
		PlayImpactSound(target, weapon, bloodLoc);
	}

	*result = 1;
	return true;
}

static bool Cmd_FakeHitEx_Execute(COMMAND_ARGS) {
	*result = 0;
	if (!thisObj || !extractArgs) return true;

	Actor* attacker = nullptr;
	float damage = 0.0f, fatigueDmg = 0.0f, limbDmg = 0.0f;
	SInt32 hitLocation = -1;
	UInt32 flags = 0;
	TESForm* weaponForm = nullptr;

	if (!extractArgs(EXTRACT_ARGS_EX, &attacker, &damage, &fatigueDmg, &limbDmg, &hitLocation, &flags, &weaponForm)) return true;

	Actor* target = (Actor*)thisObj;
	if (!target->baseProcess || target->baseProcess->processLevel > 1) return true;
	if (!attacker) return true;

	TESObjectWEAP* weapon = weaponForm ? (TESObjectWEAP*)weaponForm : nullptr;

	ActorHitData hitData;
	memset(&hitData, 0, sizeof(ActorHitData));
	hitData.source = attacker; hitData.target = target;
	hitData.weaponAV = weapon ? weapon->weaponSkill : 0;
	hitData.hitLocation = hitLocation; hitData.healthDmg = damage; hitData.wpnBaseDmg = damage;
	hitData.fatigueDmg = fatigueDmg; hitData.limbDmg = limbDmg;
	hitData.weapon = weapon; hitData.weapHealthPerc = weapon ? 1.0f : 0.0f;
	NiPoint3* tp = target->GetPos();
	hitData.impactPos.x = tp->x; hitData.impactPos.y = tp->y; hitData.impactPos.z = tp->z + 50.0f;
	hitData.impactAngle.z = 1.0f; hitData.flags = flags; hitData.dmgMult = 1.0f; hitData.unk60 = hitLocation;

	target->baseProcess->CopyHitData(&hitData);
	target->DamageHealthAndFatigue(damage, fatigueDmg, attacker);

	if (limbDmg > 0.0f && hitLocation >= 0 && hitLocation <= 6)
		target->DamageActorValue(40 + hitLocation, limbDmg, attacker);

	if (weapon) {
		SInt32 bloodLoc = (hitLocation >= 0) ? hitLocation : 0;
		PlaceBloodEffect(target, attacker, weapon, bloodLoc);
		PlaceSkinnedBloodDecal(target, attacker, weapon, bloodLoc);
		PlayImpactSound(target, weapon, bloodLoc);
	}

	*result = 1;
	return true;
}

static ParamInfo kParams_FakeHit[5] = {
	{"attacker", kParamType_ObjectRef, 0}, {"damage", kParamType_Float, 1},
	{"weapon", kParamType_ObjectID, 1}, {"hitLocation", kParamType_Integer, 1}, {"flags", kParamType_Integer, 1}
};

static ParamInfo kParams_FakeHitEx[7] = {
	{"attacker", kParamType_ObjectRef, 0}, {"damage", kParamType_Float, 0},
	{"fatigueDamage", kParamType_Float, 1}, {"limbDamage", kParamType_Float, 1},
	{"hitLocation", kParamType_Integer, 1}, {"flags", kParamType_Integer, 1}, {"weapon", kParamType_ObjectID, 1}
};

static CommandInfo kCommandInfo_FakeHit = {
	"FakeHit", "", 0, "Simulates a hit on an actor", 1, 5, kParams_FakeHit,
	Cmd_FakeHit_Execute, nullptr, nullptr, 0
};

static CommandInfo kCommandInfo_FakeHitEx = {
	"FakeHitEx", "", 0, "Extended FakeHit with fatigue and limb damage", 1, 7, kParams_FakeHitEx,
	Cmd_FakeHitEx_Execute, nullptr, nullptr, 0
};

bool FakeHit_Init(void* nvse)
{
	NVSEInterface* nvseIntf = (NVSEInterface*)nvse;
	g_scriptInterface = (NVSEScriptInterface*)nvseIntf->QueryInterface(kInterface_Script);
	if (g_scriptInterface) extractArgs = (ExtractArgsEx_t)g_scriptInterface->ExtractArgsEx;
	if (!extractArgs) return false;

	nvseIntf->SetOpcodeBase(0x401A);
	nvseIntf->RegisterCommand(&kCommandInfo_FakeHit);
	nvseIntf->RegisterCommand(&kCommandInfo_FakeHitEx);
	return true;
}
