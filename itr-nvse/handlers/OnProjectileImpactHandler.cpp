//material-aware projectile deflection, player-fired bullets only
//swaps MissileProjectile's ProcessImpacts vtable slot (MissileProjectile vtbl 0x108FA44, index 197 at 0x108FD58)
//the slot holds the ProcessImpacts wrapper 0x9B8B10 which calls inner ProcessImpacts 0x9C1B70 then, on a
//non-zero return, runs a switch on proj+0x150 whose case 1 is Projectile::Kill, so skipping the wrapper and
//returning 0 keeps the round alive. hard materials ricochet, thin materials penetrate

#include "OnProjectileImpactHandler.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/EventDispatch.h"
#include "internal/GameGlobals.h"
#include "internal/EngineFunctions.h"
#include "internal/SafeWrite.h"
#include "internal/ProjectileLogic.h"
#include "internal/layout/Projectile.h"
#include "internal/RayCast.h"
#include <Windows.h>
#include <math.h>
#include <string.h>

extern void Log(const char* fmt, ...);

namespace OnProjectileImpactHandler {

constexpr char kEvent_Pre[]      = "ITR:OnPreProjectileImpact";
constexpr char kEvent_Ricochet[] = "ITR:OnRicochet";
constexpr char kEvent_Penetrate[]= "ITR:OnPenetrate";

constexpr UInt32 kVtblSlot_ProcessImpacts = 0x108FD58; //MissileProjectile vtbl base 0x108FA44 + index 197*4

constexpr UInt32 kAddr_SpawnCollisionEffects = 0x9C20E0; //Projectile::SpawnCollisionEffects
constexpr UInt32 kAddr_ClearImpactData       = 0x9C4DA0; //walks+frees proj+0x88 tList, resets impacts
constexpr UInt32 kAddr_SpawnAndFireProjectile= 0x9BCA60; //__cdecl engine projectile spawn/launch

//exit probe - a reverse raycast finds the true backface, thicker than this is a wall not a pane
constexpr float kMaxPenetrateThickness = 48.0f;
constexpr float kExitSkin = 0.5f;  //probe end just past the entry face so the ray cannot re-hit it
constexpr float kExitEpsilon = 2.0f; //spawn this far beyond the found backface

constexpr UInt32 kMaxPenetrationDepth = 5; //hard chain cap regardless of energy settings

struct NiPoint3 { float x, y, z; };
typedef void (__thiscall* SpawnCollisionEffects_t)(void*, TESObjectREFR*, NiPoint3*, NiPoint3*, int, UInt32);
typedef void (__thiscall* ClearImpactData_t)(void*);
//owner 0 / weapon 0 / a3 0 skips the weapon-discharge side effects (recoil, fire sound, perks) which are
//gated on owner==pPlayer, so a clean projectile is created, we set owner/weapon/damage/dir on the result
typedef void* (__cdecl* SpawnAndFireProjectile_t)(
	void* base, void* owner, int a3, void* weapon,
	float px, float py, float pz, float rotz, float rotx,
	int a10, int a11, char a12, char a13, float a14, float a15, void* cell);

static UInt32 s_originalProcessImpacts = 0;

static ProjectileLogic::Config s_cfg = {};
static volatile LONG s_masterEnabled = 0;

static ProjectileLogic::HitSet s_hitSet;

struct QueuedEvent { UInt8 kind; UInt32 weapID; UInt32 strikeID; UInt32 material; float fval; };
static QueuedEvent s_queue[64];
static UInt32 s_queueCount = 0;
static CRITICAL_SECTION s_queueLock;
static bool s_queueLockInit = false;

static void PreProbe(TESObjectREFR*, void*) {}
static void RicochetProbe(TESObjectREFR*, void*) {}
static void PenetrateProbe(TESObjectREFR*, void*) {}
static EventDispatch::ListenerProbe s_preProbe       = { kEvent_Pre,       "ITR_OnPreProjectileImpactProbe", PreProbe };
static EventDispatch::ListenerProbe s_ricochetProbe  = { kEvent_Ricochet,  "ITR_OnRicochetProbe",            RicochetProbe };
static EventDispatch::ListenerProbe s_penetrateProbe = { kEvent_Penetrate, "ITR_OnPenetrateProbe",           PenetrateProbe };

static float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

//per-projectile penetration depth keyed by refID, freed on death, cleared on load
//256 slots so realistic projectile traffic never forces an eviction of a live chain.
//residual: if all 256 hold live penetrating chains at once, RecordDepth returns without
//storing (no live entry is reset to 0), and the energy decay + min-energy floor remain the
//backstop - a chain past its slot simply loses its hard cap, not its soft bound
constexpr UInt32 kDepthSlots = 256;
struct DepthEntry { UInt32 refID; UInt32 depth; };
static DepthEntry s_depthTable[kDepthSlots];

static UInt32 DepthOf(UInt32 refID)
{
	if (!refID) return 0;
	for (UInt32 i = 0; i < kDepthSlots; i++)
		if (s_depthTable[i].refID == refID) return s_depthTable[i].depth;
	return 0;
}

static void RecordDepth(UInt32 refID, UInt32 depth)
{
	if (!refID) return;
	for (UInt32 i = 0; i < kDepthSlots; i++)
		if (s_depthTable[i].refID == refID) { s_depthTable[i].depth = depth; return; }
	for (UInt32 i = 0; i < kDepthSlots; i++) //only ever take a free slot, never overwrite a live entry
		if (!s_depthTable[i].refID) { s_depthTable[i] = { refID, depth }; return; }
}

static void RemoveDepth(UInt32 refID)
{
	if (!refID) return;
	for (UInt32 i = 0; i < kDepthSlots; i++)
		if (s_depthTable[i].refID == refID) { s_depthTable[i] = { 0, 0 }; return; }
}

//reverse raycast from deep on the far side back toward the entry, the first hit along that
//direction is the penetrated collider's backface, i.e. the true exit point. no hit within
//kMaxPenetrateThickness means the wall is too thick to pass, caller skips the continuation.
//stacked panels inside the probe window collapse into the nearest-to-far-side backface,
//so multiples within 48 units traverse as one hit - documented residual gap
static bool FindExitPoint(const float pos[3], const float unit[3], float out[3])
{
	float s[3], e[3]; //world-space probe start (far side) and end (just past the entry face)
	for (int i = 0; i < 3; i++) {
		s[i] = pos[i] + unit[i]*kMaxPenetrateThickness;
		e[i] = pos[i] + unit[i]*kExitSkin;
	}
	RayCastData ray = {};
	for (int i = 0; i < 3; i++) {
		ray.pos0[i] = s[i] * kHavokScale;
		ray.pos1[i] = e[i] * kHavokScale;
	}
	ray.hitFraction = 1.0f;
	ray.unk44[0] = 0xFFFFFFFF;
	ray.unk44[6] = 0xFFFFFFFF;
	ray.layerType = 6; //static world geometry, same filter as the project's ground probe
	if (!Engine::TESPickObject(&ray, true)) return false;
	if (ray.hitFraction >= 1.0f) return false;
	for (int i = 0; i < 3; i++)
		out[i] = s[i] + (e[i] - s[i])*ray.hitFraction + unit[i]*kExitEpsilon;
	return true;
}

//base BGSProjectile is shared, clear its hitscan bit only while the engine spawns from it so
//an in-spawn hitscan resolve cannot fire along the zeroed trajectory, restore on scope exit
struct HitscanBracket {
	UInt16* flags;
	UInt16 saved;
	HitscanBracket(void* base) {
		flags = (UInt16*)((UInt8*)base + kBGSProjFlags_Offset);
		saved = *flags;
		*flags = saved & ~kBGSProjFlag_HitScan;
	}
	~HitscanBracket() { *flags = saved; }
};

//first populated impact node - node[0] is the ImpactData, node[1] the next link
static void* FirstImpactNode(void* proj)
{
	for (void** node = (void**)((UInt8*)proj + kProjImpact_ListHead); node; node = (void**)node[1])
		if (node[0]) return node[0];
	return nullptr;
}

//0xD4 is the engine range-kill field (Do3DLoaded copies BGSProjectile+0x6C here, Update kills when
//distTravelled 0x110 exceeds it), 0x14C is a constant 10000 secondary field, only a fallback
static float EngineRange(ProjectileImpactView* pv)
{
	return pv->rangeEngine > 0.0f ? pv->rangeEngine : pv->range;
}

//dampened remaining-flight energy in 0..1, drops as the round travels so a deflection chain self-limits
static float EnergyProxy(ProjectileImpactView* pv)
{
	float r = EngineRange(pv);
	if (r <= 0.0f) return 1.0f;
	return Clamp01(1.0f - pv->distTravelled / r);
}

//row-major NiMatrix3 with column 1 (local +Y) set to the travel direction, matching the engine convention
//where sub_9B7010 rebuilds orientation from vector104 as the +Y-facing axis
static void BuildForwardRotate(const float dir[3], float rotate[9])
{
	float y0 = dir[0], y1 = dir[1], y2 = dir[2];
	float x0 = y1, x1 = -y0, x2 = 0.0f; //cross(dir, worldUp=(0,0,1))
	float xl = sqrtf(x0*x0 + x1*x1 + x2*x2);
	if (xl < 1e-4f) {
		x0 = 0.0f; x1 = y2; x2 = -y1; //dir near vertical, fall back to worldUp=(1,0,0)
		xl = sqrtf(x0*x0 + x1*x1 + x2*x2);
		if (xl < 1e-4f) { rotate[0]=1;rotate[1]=0;rotate[2]=0;rotate[3]=0;rotate[4]=1;rotate[5]=0;rotate[6]=0;rotate[7]=0;rotate[8]=1; return; }
	}
	x0 /= xl; x1 /= xl; x2 /= xl;
	float z0 = x1*y2 - x2*y1; //cross(X, Y) = up
	float z1 = x2*y0 - x0*y2;
	float z2 = x0*y1 - x1*y0;
	rotate[0]=x0; rotate[1]=y0; rotate[2]=z0; //columns X | Y | Z
	rotate[3]=x1; rotate[4]=y1; rotate[5]=z1;
	rotate[6]=x2; rotate[7]=y2; rotate[8]=z2;
}

static void EnqueueEvent(UInt8 kind, UInt32 weapID, UInt32 strikeID, UInt32 material, float fval)
{
	if (!s_queueLockInit) return;
	EnterCriticalSection(&s_queueLock);
	if (s_queueCount < 64)
		s_queue[s_queueCount++] = { kind, weapID, strikeID, material, fval };
	LeaveCriticalSection(&s_queueLock);
}

static void SpawnImpactEffect(void* proj, void* node)
{
	TESObjectREFR* refr = *(TESObjectREFR**)node;              //+0x00
	NiPoint3* pos    = (NiPoint3*)((UInt8*)node + 0x04);
	NiPoint3* normal = (NiPoint3*)((UInt8*)node + 0x10);
	void* rb         = *(void**)((UInt8*)node + 0x1C);         //hkpRigidBody*
	UInt32 material  = *(UInt32*)((UInt8*)node + 0x20);
	int arg5 = rb ? (int)((UInt8*)rb + 0x10) : 0;              //sub_460140 returns rb+0x10
	((SpawnCollisionEffects_t)kAddr_SpawnCollisionEffects)(proj, refr, pos, normal, arg5, material);
}

//emerge a fresh ballistic round on the far side, angles left at 0 since vector104+transform below drive it
//and the engine re-orients from vector104 every frame (sub_9BF470 reads sub_9B7010 -> proj+0x104)
static bool SpawnContinuation(ProjectileImpactView* orig, void* base, void* cell,
	const float spawnPos[3], const float unitDir[3], float speedMult, float hitDamage,
	float distSeed, TESObjectREFR* owner, TESObjectWEAP* weapon, float vecMag, UInt32 depth)
{
	void* cont;
	{
		HitscanBracket guard(base);
		cont = ((SpawnAndFireProjectile_t)kAddr_SpawnAndFireProjectile)(
			base, nullptr, 0, nullptr, spawnPos[0], spawnPos[1], spawnPos[2],
			0.0f, 0.0f, 0, 0, 0, 0, 0.0f, 0.0f, cell);
	}
	if (!cont) return false;
	ProjectileImpactView* c = (ProjectileImpactView*)cont;
	*(TESObjectREFR**)((UInt8*)cont + 0xFC) = owner;  //sourceRef, keeps our player-only+VATS gate and damage credit
	*(TESObjectWEAP**)((UInt8*)cont + 0xF8) = weapon; //sourceWeap
	//runtime flags wholesale from the original so hitscan/gravity/VATS semantics survive
	//(Do3DLoaded 0x9B7CC0 derives 0x1/0x2/0x8/0x20/0x2000/0x8000 from the base hitscan bit,
	//which was masked during spawn, the original's word is the correct post-init state)
	*(UInt32*)((UInt8*)cont + 0xC8) = *(UInt32*)((UInt8*)orig + 0xC8);
	c->lifeTime = orig->lifeTime;       //total flight time carries over for the lifetime kill checks
	c->rangeEngine = orig->rangeEngine; //0xD4 engine range-kill field (Do3DLoaded sets it, copy anyway to match perk/context tweaks)
	c->range = orig->range;             //0x14C secondary range consumed by OnNearMiss
	*(float*)((UInt8*)cont + 0xF4) = *(float*)((UInt8*)orig + 0xF4); //wpnHealthPerc damage context
	c->hitDamage = hitDamage;
	c->speedMult = speedMult;
	c->vector104[0] = unitDir[0]*vecMag; c->vector104[1] = unitDir[1]*vecMag; c->vector104[2] = unitDir[2]*vecMag;
	BuildForwardRotate(unitDir, c->transformRotate);
	c->distTravelled = distSeed; //seeds the energy proxy so each penetration decays and the chain terminates
	//drop anything the engine resolved during the spawn call so the round starts clean
	c->hasImpacted = 0;
	((ClearImpactData_t)kAddr_ClearImpactData)(cont);
	RecordDepth(((TESObjectREFR*)cont)->refID, depth);
	return true;
}

static char __cdecl HandleImpact(void* proj, int a2, int a3);

//vtable slot ABI is __usercall - proj@ecx, a2@edi, a3@esi, returns al
static char CallOriginalProcessImpacts(void* proj, int a2, int a3)
{
	char origRet;
	void* fn = (void*)s_originalProcessImpacts;
	__asm {
		push edi
		push esi
		mov  ecx, proj
		mov  edi, a2
		mov  esi, a3
		call fn
		mov  origRet, al
		pop  esi
		pop  edi
	}
	return origRet;
}

static __declspec(naked) void ProcessImpactsThunk()
{
	__asm {
		push esi          //a3
		push edi          //a2
		push ecx          //proj
		call HandleImpact
		add  esp, 0Ch
		retn
	}
}

//run vanilla, and on the kill-return free the round's bookkeeping using the pre-captured refID
//(projKey = (void*)refID) so no field is read off proj after Projectile::Kill frees it
static char CallOriginalReap(void* proj, int a2, int a3, void* projKey)
{
	char killed = CallOriginalProcessImpacts(proj, a2, a3);
	if (killed) {
		s_hitSet.Release(projKey);
		RemoveDepth((UInt32)(uintptr_t)projKey);
	}
	return killed;
}

static char __cdecl HandleImpact(void* proj, int a2, int a3)
{
	if (!s_masterEnabled || !proj)
		return CallOriginalProcessImpacts(proj, a2, a3);

	//player-fired bullets only, the projectile object survives so VATS owner globals
	//dword_11F21E8/dword_11F21EC stay attached - VATS deflection is intentional
	if (ProjectileGetSourceRef(proj) != (TESObjectREFR*)*g_thePlayerPtr)
		return CallOriginalProcessImpacts(proj, a2, a3);

	//bullets only - rockets/grenades share the MissileProjectile vtable but carry an explosion form
	if (ProjectileBaseHasExplosion(proj))
		return CallOriginalProcessImpacts(proj, a2, a3);

	void* node = FirstImpactNode(proj);
	if (!node)
		return CallOriginalProcessImpacts(proj, a2, a3);

	ProjectileImpactView* pv = (ProjectileImpactView*)proj;
	TESObjectREFR* strikeRefr = *(TESObjectREFR**)node;
	UInt32 strikeRefID = strikeRefr ? strikeRefr->refID : 0;

	//key the dedup set on the projectile's own refID, not its heap pointer - a dead projectile's
	//address can be reused but its refID cannot within its flight, so no stale entry can alias
	void* projKey = (void*)((TESObjectREFR*)proj)->refID;

	//a still-overlapping deflected round re-reports the same ref next frame, dedup on world refs only
	//static geometry reports refID 0 and the reflected round leaves the surface, so no dedup needed there.
	//discard the repeat impact and keep the round alive, letting vanilla process it here would kill it
	if (strikeRefID && s_hitSet.AlreadyHit(projKey, strikeRefID)) {
		pv->hasImpacted = 0;
		((ClearImpactData_t)kAddr_ClearImpactData)(proj);
		return 0;
	}

	ProjectileLogic::ImpactContext ctx = {};
	ctx.strikeRefID = strikeRefID;
	const float* npos = (const float*)((UInt8*)node + 0x04);
	const float* nnrm = (const float*)((UInt8*)node + 0x10);
	ctx.pos[0]=npos[0]; ctx.pos[1]=npos[1]; ctx.pos[2]=npos[2];
	ctx.normal[0]=nnrm[0]; ctx.normal[1]=nnrm[1]; ctx.normal[2]=nnrm[2];
	ctx.dir[0]=pv->vector104[0]; ctx.dir[1]=pv->vector104[1]; ctx.dir[2]=pv->vector104[2];
	ctx.material = *(UInt32*)((UInt8*)node + 0x20);
	ctx.energy = EnergyProxy(pv);
	ctx.hitDamage = pv->hitDamage;
	TESObjectWEAP* weap = ProjectileGetSourceWeapon(proj);
	ctx.sourceWeapFormID = weap ? weap->refID : 0;
	ctx.incidenceDeg = -1.0f; //let Decide derive the grazing angle from dir+normal

	ProjectileLogic::Outcome outcome = ProjectileLogic::Decide(ctx, s_cfg);

	//resolve penetration feasibility up front so the pre-event reports the real outcome and the
	//exit raycast is computed once and reused for the spawn. an infeasible penetration downgrades
	//to Normal (vanilla hit), so listeners never see a penetration that will not happen
	bool penReady = false;
	float penExit[3], penUnit[3], penDistSeed = 0.0f, penVecMag = 0.0f;
	void* penBase = nullptr; void* penCell = nullptr; UInt32 penDepth = 0;
	if (outcome == ProjectileLogic::kOutcome_Penetrate) {
		UInt32 chainDepth = DepthOf((UInt32)(uintptr_t)projKey);
		float range = EngineRange(pv);
		float dl = sqrtf(ctx.dir[0]*ctx.dir[0] + ctx.dir[1]*ctx.dir[1] + ctx.dir[2]*ctx.dir[2]);
		void* base = *(void**)((UInt8*)proj + 0x20); //refr baseForm = BGSProjectile
		void* cell = *(void**)((UInt8*)proj + 0x40); //parentCell for placement
		//depth cap is the hard stop, energy decay + the ini min-energy floor are the soft bound.
		//range 0 means the energy proxy cannot decay, skip to avoid an endless chain
		if (chainDepth < kMaxPenetrationDepth && range > 0.0f && base && cell && dl > 1e-6f) {
			penUnit[0] = ctx.dir[0]/dl; penUnit[1] = ctx.dir[1]/dl; penUnit[2] = ctx.dir[2]/dl;
			if (FindExitPoint(ctx.pos, penUnit, penExit)) {
				//each hop multiplies energy by the falloff, distTravelled is seeded so energy = energyOld*falloff
				//once it drops below minRicochetEnergy Decide returns Normal and the chain stops
				float energyNew = ctx.energy * s_cfg.penetrationEnergyFalloff;
				penDistSeed = range * (1.0f - energyNew);
				if (penDistSeed < 0.0f) penDistSeed = 0.0f;
				penBase = base; penCell = cell; penVecMag = dl; penDepth = chainDepth + 1;
				penReady = true;
			}
		}
		if (!penReady)
			outcome = ProjectileLogic::kOutcome_Normal;
	}

	if (outcome == ProjectileLogic::kOutcome_Normal)
		return CallOriginalReap(proj, a2, a3, projKey);

	//cancellable gate, dispatched synchronously because a veto must be known before we act.
	//rare (only material-matched player deflections), reports the accurate feasible outcome.
	//the projectile is not passed to handlers, so a listener has no handle to free it mid-dispatch,
	//and re-entry is depth-capped
	if (s_preProbe.hasListeners && g_eventManagerInterface) {
		static UInt32 s_depth = 0;
		if (s_depth >= 8) //recursion cap, bail to non-deflection rather than deflect with the veto ignored
			return CallOriginalReap(proj, a2, a3, projKey);
		UInt32 allow = 1;
		s_depth++;
		struct Cb { static bool Fn(NVSEArrayVarInterface::Element& r, void* d) {
			UInt32& a = *(UInt32*)d;
			if (a && r.IsValid() && r.type == NVSEArrayVarInterface::Element::kType_Numeric)
				a = (r.num != 0.0) ? 1 : 0;
			return true;
		}};
		g_eventManagerInterface->DispatchEventAlt(kEvent_Pre, Cb::Fn, &allow,
			nullptr, (TESForm*)weap, (TESForm*)strikeRefr, (int)ctx.material, (int)outcome);
		s_depth--;
		if (!allow)
			return CallOriginalReap(proj, a2, a3, projKey);
	}

	if (strikeRefID)
		s_hitSet.RecordHit(projKey, strikeRefID);

	if (outcome == ProjectileLogic::kOutcome_Ricochet) {
		float refl[3];
		ProjectileLogic::ReflectVector(ctx.dir, ctx.normal, refl);
		float rl = sqrtf(refl[0]*refl[0] + refl[1]*refl[1] + refl[2]*refl[2]);
		float vl = sqrtf(ctx.dir[0]*ctx.dir[0] + ctx.dir[1]*ctx.dir[1] + ctx.dir[2]*ctx.dir[2]);
		if (rl > 1e-6f) {
			float ux = refl[0]/rl, uy = refl[1]/rl, uz = refl[2]/rl;
			pv->vector104[0] = ux*vl; pv->vector104[1] = uy*vl; pv->vector104[2] = uz*vl; //keep move-delta magnitude
			float unit[3] = { ux, uy, uz };
			BuildForwardRotate(unit, pv->transformRotate); //keep mesh/tracer coherent this frame
		}
		pv->hitDamage = ProjectileLogic::RicochetDamage(pv->hitDamage, s_cfg);
		SpawnImpactEffect(proj, node);
		pv->hasImpacted = 0;
		((ClearImpactData_t)kAddr_ClearImpactData)(proj); //else the populated list reprocesses same frame
		EnqueueEvent(1, ctx.sourceWeapFormID, strikeRefID, ctx.material,
			ProjectileLogic::GrazingAngleDeg(ctx.dir, ctx.normal));
		return 0;
	}

	//penetrate - feasibility and the exit point were resolved above, emerge the decayed continuation
	//then let the engine apply the full vanilla entry hit (damage/destruction/callbacks) and kill the original
	bool spawned = SpawnContinuation(pv, penBase, penCell, penExit, penUnit,
		pv->speedMult * s_cfg.penetrationEnergyFalloff,
		ProjectileLogic::PenetrationDamage(pv->hitDamage, s_cfg),
		penDistSeed, (TESObjectREFR*)*g_thePlayerPtr, weap, penVecMag, penDepth);
	if (spawned) //a rare null spawn still takes the vanilla hit, just no continuation and no event
		EnqueueEvent(2, ctx.sourceWeapFormID, strikeRefID, ctx.material,
			ProjectileLogic::PenetrationDamage(pv->hitDamage, s_cfg));
	return CallOriginalReap(proj, a2, a3, projKey);
}

bool Init(void* nvseInterface)
{
	NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
	if (nvse->isEditor) return false;
	if (!g_eventManagerInterface) {
		Log("OnProjectileImpactHandler: event manager not ready, aborting Init");
		return false;
	}

	using P = NVSEEventManagerInterface::ParamType;
	using F = NVSEEventManagerInterface::EventFlags;
	static P preParams[] = { P::eParamType_AnyForm, P::eParamType_AnyForm, P::eParamType_Int, P::eParamType_Int };
	static P outParams[] = { P::eParamType_AnyForm, P::eParamType_AnyForm, P::eParamType_Int, P::eParamType_Float };
	g_eventManagerInterface->RegisterEvent(kEvent_Pre, 4, preParams, F::kFlag_FlushOnLoad);
	g_eventManagerInterface->RegisterEvent(kEvent_Ricochet, 4, outParams, F::kFlag_FlushOnLoad);
	g_eventManagerInterface->RegisterEvent(kEvent_Penetrate, 4, outParams, F::kFlag_FlushOnLoad);

	InitializeCriticalSection(&s_queueLock);
	s_queueLockInit = true;
	return true;
}

void InstallHook()
{
	static bool s_hooked = false;
	s_preProbe.Install();
	s_ricochetProbe.Install();
	s_penetrateProbe.Install();
	if (s_hooked) return; //re-swapping would capture the thunk as its own original and recurse
	s_hooked = true;
	s_originalProcessImpacts = *(UInt32*)kVtblSlot_ProcessImpacts; //chain-tolerant, record whatever owns the slot
	SafeWrite::Write32(kVtblSlot_ProcessImpacts, (UInt32)&ProcessImpactsThunk);
}

//MaterialType ids read at ImpactData+0x20 (engine MaterialType enum)
//hard ricochet-prone: Stone 0, Metal 4, HollowMetal 9  thin penetration-prone: Glass 3, Wood 5, Cloth 7
void FillDefaultMaterials(ProjectileLogic::Config& cfg)
{
	cfg.hardMaterials[0] = 0; cfg.hardMaterials[1] = 4; cfg.hardMaterials[2] = 9;
	cfg.hardMaterialCount = 3;
	cfg.thinMaterials[0] = 3; cfg.thinMaterials[1] = 5; cfg.thinMaterials[2] = 7;
	cfg.thinMaterialCount = 3;
}

void UpdateSettings(const ProjectileLogic::Config& cfg, bool masterEnabled)
{
	s_cfg = cfg;
	InterlockedExchange(&s_masterEnabled, masterEnabled ? 1 : 0);
}

void Update()
{
	s_preProbe.Refresh(false);
	s_ricochetProbe.Refresh(false);
	s_penetrateProbe.Refresh(false);

	if (!s_queueLockInit) return;
	QueuedEvent batch[64];
	UInt32 n = 0;
	EnterCriticalSection(&s_queueLock);
	n = s_queueCount;
	for (UInt32 i = 0; i < n; i++) batch[i] = s_queue[i];
	s_queueCount = 0;
	LeaveCriticalSection(&s_queueLock);

	if (!g_eventManagerInterface) return;
	for (UInt32 i = 0; i < n; i++) {
		const QueuedEvent& e = batch[i];
		TESForm* weap = e.weapID ? (TESForm*)Engine::LookupFormByID(e.weapID) : nullptr;
		TESForm* strike = e.strikeID ? (TESForm*)Engine::LookupFormByID(e.strikeID) : nullptr;
		const char* name = e.kind == 1 ? kEvent_Ricochet : kEvent_Penetrate;
		g_eventManagerInterface->DispatchEvent(name, nullptr, weap, strike, (int)e.material, PackEventFloatArg(e.fval));
	}
}

void ClearState()
{
	s_hitSet.Clear();
	memset(s_depthTable, 0, sizeof(s_depthTable));
	if (s_queueLockInit) {
		EnterCriticalSection(&s_queueLock);
		s_queueCount = 0;
		LeaveCriticalSection(&s_queueLock);
	}
}

}
