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
#include <Windows.h>
#include <math.h>

extern void Log(const char* fmt, ...);

namespace OnProjectileImpactHandler {

constexpr char kEvent_Pre[]      = "ITR:OnPreProjectileImpact";
constexpr char kEvent_Ricochet[] = "ITR:OnRicochet";
constexpr char kEvent_Penetrate[]= "ITR:OnPenetrate";

constexpr UInt32 kVtblSlot_ProcessImpacts = 0x108FD58; //MissileProjectile vtbl base 0x108FA44 + index 197*4

constexpr UInt32 kAddr_SpawnCollisionEffects = 0x9C20E0; //Projectile::SpawnCollisionEffects
constexpr UInt32 kAddr_ClearImpactData       = 0x9C4DA0; //walks+frees proj+0x88 tList, resets impacts
constexpr UInt32 kAddr_SpawnAndFireProjectile= 0x9BCA60; //__cdecl engine projectile spawn/launch

//far side stand-off, past a typical FNV wall/door (~10-25 units thick) so the continuation clears the collider
constexpr float kPenetrateBackoff = 30.0f;

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

//first populated impact node - node[0] is the ImpactData, node[1] the next link
static void* FirstImpactNode(void* proj)
{
	for (void** node = (void**)((UInt8*)proj + kProjImpact_ListHead); node; node = (void**)node[1])
		if (node[0]) return node[0];
	return nullptr;
}

//dampened remaining-flight energy in 0..1, drops as the round travels so a deflection chain self-limits
static float EnergyProxy(ProjectileImpactView* pv)
{
	if (pv->range <= 0.0f) return 1.0f;
	return Clamp01(1.0f - pv->distTravelled / pv->range);
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
static void SpawnContinuation(void* base, void* cell, float px, float py, float pz,
	const float unitDir[3], float speedMult, float hitDamage, float distSeed,
	TESObjectREFR* owner, TESObjectWEAP* weapon, float vecMag)
{
	void* cont = ((SpawnAndFireProjectile_t)kAddr_SpawnAndFireProjectile)(
		base, nullptr, 0, nullptr, px, py, pz, 0.0f, 0.0f, 0, 0, 0, 0, 0.0f, 0.0f, cell);
	if (!cont) return;
	ProjectileImpactView* c = (ProjectileImpactView*)cont;
	*(TESObjectREFR**)((UInt8*)cont + 0xFC) = owner;  //sourceRef, keeps our player-only+VATS gate and damage credit
	*(TESObjectWEAP**)((UInt8*)cont + 0xF8) = weapon; //sourceWeap
	c->hitDamage = hitDamage;
	c->speedMult = speedMult;
	c->vector104[0] = unitDir[0]*vecMag; c->vector104[1] = unitDir[1]*vecMag; c->vector104[2] = unitDir[2]*vecMag;
	BuildForwardRotate(unitDir, c->transformRotate);
	c->distTravelled = distSeed; //seeds the energy proxy so each penetration decays and the chain terminates
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
	if (outcome == ProjectileLogic::kOutcome_Normal) {
		char killed = CallOriginalProcessImpacts(proj, a2, a3);
		if (killed) s_hitSet.Release(projKey); //non-zero return means the wrapper handled/killed the round, free its slot
		return killed;
	}

	//cancellable gate, dispatched synchronously because a veto must be known before we act.
	//rare (only material-matched player deflections). the projectile is not passed to handlers,
	//so a listener has no handle to free it mid-dispatch, and re-entry is depth-capped
	if (s_preProbe.hasListeners && g_eventManagerInterface) {
		static UInt32 s_depth = 0;
		if (s_depth >= 8) //recursion cap, bail to non-deflection rather than deflect with the veto ignored
			return CallOriginalProcessImpacts(proj, a2, a3);
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
			return CallOriginalProcessImpacts(proj, a2, a3);
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

	//penetrate - emerge a decayed continuation past the surface, then let the engine apply the full
	//vanilla entry hit (damage/destruction/callbacks) to the struck ref and kill the original round
	if (pv->range > 0.0f) { //no range means the energy proxy cannot decay, skip to avoid an endless chain
		void* base = *(void**)((UInt8*)proj + 0x20); //refr baseForm = BGSProjectile
		void* cell = *(void**)((UInt8*)proj + 0x40); //parentCell for placement
		float dl = sqrtf(ctx.dir[0]*ctx.dir[0] + ctx.dir[1]*ctx.dir[1] + ctx.dir[2]*ctx.dir[2]);
		if (base && cell && dl > 1e-6f) {
			float ux = ctx.dir[0]/dl, uy = ctx.dir[1]/dl, uz = ctx.dir[2]/dl;
			float unit[3] = { ux, uy, uz };
			float px = ctx.pos[0] + ux*kPenetrateBackoff;
			float py = ctx.pos[1] + uy*kPenetrateBackoff;
			float pz = ctx.pos[2] + uz*kPenetrateBackoff;
			//each hop multiplies energy by the falloff, distTravelled is seeded so energy = energyOld*falloff
			//once it drops below minRicochetEnergy Decide returns Normal and the chain stops
			float energyNew = ctx.energy * s_cfg.penetrationEnergyFalloff;
			float distSeed = pv->range * (1.0f - energyNew);
			if (distSeed < 0.0f) distSeed = 0.0f;
			SpawnContinuation(base, cell, px, py, pz, unit,
				pv->speedMult * s_cfg.penetrationEnergyFalloff,
				ProjectileLogic::PenetrationDamage(pv->hitDamage, s_cfg),
				distSeed, (TESObjectREFR*)*g_thePlayerPtr, weap, dl);
		}
	}
	EnqueueEvent(2, ctx.sourceWeapFormID, strikeRefID, ctx.material,
		ProjectileLogic::PenetrationDamage(pv->hitDamage, s_cfg));
	char killed = CallOriginalProcessImpacts(proj, a2, a3);
	if (killed) s_hitSet.Release(projKey); //original destroyed by the vanilla entry hit, free its slot
	return killed;
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
	if (s_queueLockInit) {
		EnterCriticalSection(&s_queueLock);
		s_queueCount = 0;
		LeaveCriticalSection(&s_queueLock);
	}
}

}
