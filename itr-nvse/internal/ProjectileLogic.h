#pragma once
//material-aware projectile decision core - pure host-testable logic
//the engine hook fills ImpactContext/Config from engine memory and consumes Decide/ReflectVector/HitSet
//no engine calls, no addresses - keep material-id knowledge in Config, not in here

#include <cmath>
#include <cstring>

namespace ProjectileLogic {

enum Outcome { kOutcome_Normal, kOutcome_Ricochet, kOutcome_Penetrate };

//impactdata stores a 0..31 havok material id; jip converts it to the 12 game material categories
inline UInt32 CanonicalMaterial(UInt32 raw)
{
	static const unsigned char map[32] = {
		0, 7, 1, 3, 2, 4, 6, 6,
		8, 5, 0, 4, 5, 4, 4, 4,
		9, 9, 1, 0, 4, 4, 9, 9,
		3, 9, 4, 4, 9, 9, 5, 5
	};
	return raw < 32 ? map[raw] : 0xFFFFFFFF;
}

struct ImpactContext {
	UInt32 strikeRefID;
	float pos[3];
	float normal[3];
	float dir[3];
	UInt32 material;
	float energy;         //0..1 proxy of remaining projectile energy
	float hitDamage;
	UInt32 sourceWeapFormID;
	float incidenceDeg;   //grazing angle from surface plane, -1 = compute from dir+normal
};

struct Config {
	bool ricochetEnabled;
	bool penetrationEnabled;
	float maxRicochetAngleDeg;  //grazing threshold from surface plane, ricochet at or below this
	float minRicochetEnergy;    //shared minimum energy to trigger any special outcome
	UInt32 hardMaterials[32];
	UInt32 hardMaterialCount;
	UInt32 thinMaterials[32];
	UInt32 thinMaterialCount;
	float ricochetDamageFalloff;
	float penetrationDamageFalloff;
	float penetrationEnergyFalloff;
};

//angle convention: incidenceDeg is the grazing angle measured from the surface PLANE
//0 = travelling parallel to the surface (grazing), 90 = head-on/perpendicular
//a shallow (small) grazing angle ricochets, a head-on hit does not
inline float GrazingAngleDeg(const float dir[3], const float normal[3])
{
	float dl = std::sqrt(dir[0]*dir[0] + dir[1]*dir[1] + dir[2]*dir[2]);
	float nl = std::sqrt(normal[0]*normal[0] + normal[1]*normal[1] + normal[2]*normal[2]);
	if (dl <= 0.0f || nl <= 0.0f)
		return 90.0f;
	float dot = (dir[0]*normal[0] + dir[1]*normal[1] + dir[2]*normal[2]) / (dl * nl);
	if (dot < 0.0f) dot = -dot;
	if (dot > 1.0f) dot = 1.0f;
	//angle from normal is acos(dot), grazing from plane is 90 minus that = asin(dot)
	return std::asin(dot) * (180.0f / 3.14159265358979323846f);
}

inline bool InSet(UInt32 id, const UInt32* set, UInt32 count)
{
	for (UInt32 i = 0; i < count; i++)
		if (set[i] == id)
			return true;
	return false;
}

inline Outcome Decide(const ImpactContext& ctx, const Config& cfg)
{
	float grazing = ctx.incidenceDeg;
	if (grazing < 0.0f)
		grazing = GrazingAngleDeg(ctx.dir, ctx.normal);

	if (cfg.penetrationEnabled
		&& InSet(ctx.material, cfg.thinMaterials, cfg.thinMaterialCount)
		&& ctx.energy >= cfg.minRicochetEnergy)
		return kOutcome_Penetrate;

	if (cfg.ricochetEnabled
		&& InSet(ctx.material, cfg.hardMaterials, cfg.hardMaterialCount)
		&& grazing <= cfg.maxRicochetAngleDeg
		&& ctx.energy >= cfg.minRicochetEnergy)
		return kOutcome_Ricochet;

	return kOutcome_Normal;
}

//out = dir - 2*(dir . normal)*normal, unit-safe against a non-unit normal
inline void ReflectVector(const float dir[3], const float normal[3], float out[3])
{
	float nl2 = normal[0]*normal[0] + normal[1]*normal[1] + normal[2]*normal[2];
	if (nl2 <= 0.0f) {
		out[0] = dir[0]; out[1] = dir[1]; out[2] = dir[2];
		return;
	}
	float d = (dir[0]*normal[0] + dir[1]*normal[1] + dir[2]*normal[2]) / nl2;
	out[0] = dir[0] - 2.0f * d * normal[0];
	out[1] = dir[1] - 2.0f * d * normal[1];
	out[2] = dir[2] - 2.0f * d * normal[2];
}

inline float RicochetDamage(float dmg, const Config& cfg) { return dmg * cfg.ricochetDamageFalloff; }
inline float PenetrationDamage(float dmg, const Config& cfg) { return dmg * cfg.penetrationDamageFalloff; }
inline float PenetrationEnergy(float energy, const Config& cfg) { return energy * cfg.penetrationEnergyFalloff; }

//tracks which refIDs a projectile has already struck so a ricochet/penetration cannot double-hit
//dedups struck refIDs per projectile key. the caller must pass a key that is stable for one
//projectile's lifetime and not reused by a later projectile - the handler keys on projectile
//refID for exactly this reason, since a heap pointer can be recycled but a refID cannot mid-flight.
//Release frees a key's slot on death, and a full table evicts the least-recently-used entry
//via gen so the table cannot leak even if a Release is missed
class HitSet {
public:
	static const int kMaxProjectiles = 64;
	static const int kMaxRefsPerProjectile = 8;

	struct Entry {
		void* proj;
		UInt32 gen;
		UInt32 refs[kMaxRefsPerProjectile];
		UInt32 refCount;
	};

	Entry entries[kMaxProjectiles];
	UInt32 nextGen;

	HitSet() { Clear(); }

	void Clear()
	{
		std::memset(entries, 0, sizeof(entries));
		nextGen = 1;
	}

	bool AlreadyHit(void* proj, UInt32 refID)
	{
		Entry* e = Find(proj);
		if (!e)
			return false;
		for (UInt32 i = 0; i < e->refCount; i++)
			if (e->refs[i] == refID)
				return true;
		return false;
	}

	void RecordHit(void* proj, UInt32 refID)
	{
		Entry* e = Find(proj);
		if (!e)
			e = Allocate(proj);
		e->gen = nextGen++;
		for (UInt32 i = 0; i < e->refCount; i++)
			if (e->refs[i] == refID)
				return;
		if (e->refCount < kMaxRefsPerProjectile)
			e->refs[e->refCount++] = refID;
	}

	void Release(void* proj)
	{
		Entry* e = Find(proj);
		if (e) {
			e->proj = 0;
			e->refCount = 0;
			e->gen = 0;
		}
	}

	int ActiveCount() const
	{
		int n = 0;
		for (int i = 0; i < kMaxProjectiles; i++)
			if (entries[i].proj)
				n++;
		return n;
	}

private:
	Entry* Find(void* proj)
	{
		if (!proj)
			return 0;
		for (int i = 0; i < kMaxProjectiles; i++)
			if (entries[i].proj == proj)
				return &entries[i];
		return 0;
	}

	Entry* Allocate(void* proj)
	{
		int slot = -1;
		for (int i = 0; i < kMaxProjectiles; i++)
			if (!entries[i].proj) { slot = i; break; }
		if (slot < 0) {
			slot = 0;
			for (int i = 1; i < kMaxProjectiles; i++)
				if (entries[i].gen < entries[slot].gen)
					slot = i;
		}
		entries[slot].proj = proj;
		entries[slot].refCount = 0;
		entries[slot].gen = nextGen++;
		return &entries[slot];
	}
};

//penetration depth per projectile refID with explicit slot reservation. liveness is proven by
//refID RESOLUTION through an injected predicate, never by elapsed time, so a live continuation is
//never reclaimed. a slot is Reserve()d before the synchronous pre-event dispatch and is invisible to
//any re-entrant Reserve() until it is Commit()ed to the spawned child or Release()d (veto/failure),
//so a re-entrant impact can never consume the slot we are about to hand to our continuation.
//when capacity is needed an occupied slot is reclaimed only if isLive(refID) is false
class DepthTracker {
public:
	static const int kSlots = 256;
	static const int kNone = -1;

	typedef bool (*LivePredicate)(UInt32 refID);

	struct Slot { UInt32 refID; UInt32 depth; bool reserved; };
	Slot slots[kSlots];

	DepthTracker() { Clear(); }

	void Clear() { std::memset(slots, 0, sizeof(slots)); }

	UInt32 DepthOf(UInt32 refID) const
	{
		if (!refID) return 0;
		for (int i = 0; i < kSlots; i++)
			if (slots[i].refID == refID && !slots[i].reserved) return slots[i].depth;
		return 0;
	}

	//reserve a slot for a continuation of parentRefID. prefers the parent's own slot (the parent is
	//dying, its slot transfers to the child), else a free slot, else a slot whose refID is verified
	//dead. returns the reserved slot index, or kNone when the table is full of live entries
	int Reserve(UInt32 parentRefID, LivePredicate isLive)
	{
		int freeSlot = kNone, deadSlot = kNone;
		for (int i = 0; i < kSlots; i++) {
			if (slots[i].reserved) continue; //reserved slots are invisible to re-entrant callers
			if (parentRefID && slots[i].refID == parentRefID) { slots[i].reserved = true; return i; }
			if (!slots[i].refID) { if (freeSlot == kNone) freeSlot = i; }
			else if (deadSlot == kNone && isLive && !isLive(slots[i].refID)) deadSlot = i;
		}
		int slot = freeSlot != kNone ? freeSlot : deadSlot;
		if (slot == kNone) return kNone;
		slots[slot].reserved = true;
		return slot;
	}

	//hand the reserved slot to the spawned continuation, clearing the reservation
	void Commit(int slot, UInt32 childRefID, UInt32 depth)
	{
		if (slot < 0 || slot >= kSlots) return;
		slots[slot].refID = childRefID;
		slots[slot].depth = depth;
		slots[slot].reserved = false;
	}

	//give the reserved slot back - its prior contents are untouched by Reserve so they simply resume
	void Release(int slot)
	{
		if (slot < 0 || slot >= kSlots) return;
		slots[slot].reserved = false;
	}

	void Remove(UInt32 refID)
	{
		if (!refID) return;
		for (int i = 0; i < kSlots; i++)
			if (slots[i].refID == refID && !slots[i].reserved) { slots[i].refID = 0; slots[i].depth = 0; return; }
	}

	int ActiveCount() const
	{
		int n = 0;
		for (int i = 0; i < kSlots; i++)
			if (slots[i].refID || slots[i].reserved) n++;
		return n;
	}
};

}
