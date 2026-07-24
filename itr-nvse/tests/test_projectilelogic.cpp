//tests for internal/ProjectileLogic.h

#include "test.h"
#include <cstdint>

typedef uint32_t UInt32;
#include "../internal/ProjectileLogic.h"

using namespace ProjectileLogic;

static Config MakeConfig()
{
	Config cfg;
	std::memset(&cfg, 0, sizeof(cfg));
	cfg.ricochetEnabled = true;
	cfg.penetrationEnabled = true;
	cfg.maxRicochetAngleDeg = 55.0f;
	cfg.minRicochetEnergy = 0.25f;
	cfg.hardMaterials[0] = 10; cfg.hardMaterials[1] = 11; cfg.hardMaterialCount = 2;
	cfg.thinMaterials[0] = 20; cfg.thinMaterials[1] = 21; cfg.thinMaterialCount = 2;
	cfg.ricochetDamageFalloff = 0.5f;
	cfg.penetrationDamageFalloff = 0.7f;
	cfg.penetrationEnergyFalloff = 0.6f;
	return cfg;
}

static ImpactContext MakeCtx(UInt32 material, float energy, float grazingDeg)
{
	ImpactContext ctx;
	std::memset(&ctx, 0, sizeof(ctx));
	ctx.strikeRefID = 0x1234;
	ctx.material = material;
	ctx.energy = energy;
	ctx.hitDamage = 50.0f;
	ctx.incidenceDeg = grazingDeg;
	return ctx;
}

TEST(Material_RawMetalConvertsToMetal)
{
	ASSERT_EQ(CanonicalMaterial(5), 4u);
	return true;
}

TEST(Material_RawWoodConvertsToWood)
{
	ASSERT_EQ(CanonicalMaterial(9), 5u);
	return true;
}

TEST(Material_RawClothConvertsToCloth)
{
	ASSERT_EQ(CanonicalMaterial(1), 7u);
	return true;
}

TEST(Material_RawHollowMetalConvertsToHollowMetal)
{
	ASSERT_EQ(CanonicalMaterial(16), 9u);
	ASSERT_EQ(CanonicalMaterial(29), 9u);
	return true;
}

TEST(Material_OutOfRangeIsUnknown)
{
	ASSERT_EQ(CanonicalMaterial(32), 0xFFFFFFFFu);
	return true;
}

TEST(Decide_HeadOnHardHitNormal)
{
	Config cfg = MakeConfig();
	//90 deg grazing = head-on, above the 55 threshold, no ricochet
	ImpactContext ctx = MakeCtx(10, 0.9f, 90.0f);
	ASSERT_EQ(Decide(ctx, cfg), kOutcome_Normal);
	return true;
}

TEST(Decide_GrazingHardHitRicochet)
{
	Config cfg = MakeConfig();
	ImpactContext ctx = MakeCtx(10, 0.9f, 15.0f);
	ASSERT_EQ(Decide(ctx, cfg), kOutcome_Ricochet);
	return true;
}

TEST(Decide_ThinMaterialPenetrate)
{
	Config cfg = MakeConfig();
	//head-on thin material still penetrates, angle is irrelevant to penetration
	ImpactContext ctx = MakeCtx(20, 0.9f, 90.0f);
	ASSERT_EQ(Decide(ctx, cfg), kOutcome_Penetrate);
	return true;
}

TEST(Decide_UnknownMaterialNormal)
{
	Config cfg = MakeConfig();
	ImpactContext ctx = MakeCtx(99, 0.9f, 15.0f);
	ASSERT_EQ(Decide(ctx, cfg), kOutcome_Normal);
	return true;
}

TEST(Decide_RicochetDisabled)
{
	Config cfg = MakeConfig();
	cfg.ricochetEnabled = false;
	ImpactContext ctx = MakeCtx(10, 0.9f, 15.0f);
	ASSERT_EQ(Decide(ctx, cfg), kOutcome_Normal);
	return true;
}

TEST(Decide_PenetrationDisabled)
{
	Config cfg = MakeConfig();
	cfg.penetrationEnabled = false;
	ImpactContext ctx = MakeCtx(20, 0.9f, 90.0f);
	ASSERT_EQ(Decide(ctx, cfg), kOutcome_Normal);
	return true;
}

TEST(Decide_RicochetEnergyTooLow)
{
	Config cfg = MakeConfig();
	//grazing hard hit but energy below minRicochetEnergy
	ImpactContext ctx = MakeCtx(10, 0.1f, 15.0f);
	ASSERT_EQ(Decide(ctx, cfg), kOutcome_Normal);
	return true;
}

TEST(Decide_PenetrationEnergyTooLow)
{
	Config cfg = MakeConfig();
	ImpactContext ctx = MakeCtx(20, 0.1f, 90.0f);
	ASSERT_EQ(Decide(ctx, cfg), kOutcome_Normal);
	return true;
}

TEST(Decide_RicochetAngleBoundaryInclusive)
{
	Config cfg = MakeConfig();
	//exactly at the threshold ricochets (at or below)
	ImpactContext ctx = MakeCtx(10, 0.9f, 55.0f);
	ASSERT_EQ(Decide(ctx, cfg), kOutcome_Ricochet);
	return true;
}

TEST(Decide_RicochetAngleJustAboveThreshold)
{
	Config cfg = MakeConfig();
	ImpactContext ctx = MakeCtx(10, 0.9f, 55.5f);
	ASSERT_EQ(Decide(ctx, cfg), kOutcome_Normal);
	return true;
}

TEST(Decide_EnergyExactlyAtThreshold)
{
	Config cfg = MakeConfig();
	//>= threshold triggers
	ImpactContext ctx = MakeCtx(10, 0.25f, 15.0f);
	ASSERT_EQ(Decide(ctx, cfg), kOutcome_Ricochet);
	return true;
}

TEST(Decide_ComputesGrazingFromVectors)
{
	Config cfg = MakeConfig();
	//incidenceDeg negative forces computation from dir+normal
	//dir grazing along the surface, tiny into it, normal up
	ImpactContext ctx = MakeCtx(10, 0.9f, -1.0f);
	ctx.dir[0] = 1.0f; ctx.dir[1] = -0.1f; ctx.dir[2] = 0.0f;
	ctx.normal[1] = 1.0f;
	//grazing angle here is small, well below 55, so ricochet
	ASSERT_EQ(Decide(ctx, cfg), kOutcome_Ricochet);
	return true;
}

TEST(Decide_ComputesHeadOnFromVectors)
{
	Config cfg = MakeConfig();
	ImpactContext ctx = MakeCtx(10, 0.9f, -1.0f);
	ctx.dir[1] = -1.0f;      //straight down
	ctx.normal[1] = 1.0f;    //surface faces up
	ASSERT_EQ(Decide(ctx, cfg), kOutcome_Normal);
	return true;
}

TEST(GrazingAngle_HeadOnIs90)
{
	float dir[3] = {0.0f, -1.0f, 0.0f};
	float normal[3] = {0.0f, 1.0f, 0.0f};
	ASSERT_NEAR(GrazingAngleDeg(dir, normal), 90.0f, 0.001f);
	return true;
}

TEST(GrazingAngle_ParallelIs0)
{
	float dir[3] = {1.0f, 0.0f, 0.0f};
	float normal[3] = {0.0f, 1.0f, 0.0f};
	ASSERT_NEAR(GrazingAngleDeg(dir, normal), 0.0f, 0.001f);
	return true;
}

TEST(GrazingAngle_45Deg)
{
	float dir[3] = {1.0f, -1.0f, 0.0f};
	float normal[3] = {0.0f, 1.0f, 0.0f};
	ASSERT_NEAR(GrazingAngleDeg(dir, normal), 45.0f, 0.01f);
	return true;
}

TEST(GrazingAngle_DegenerateVectorsSafe)
{
	float dir[3] = {0.0f, 0.0f, 0.0f};
	float normal[3] = {0.0f, 1.0f, 0.0f};
	ASSERT_NEAR(GrazingAngleDeg(dir, normal), 90.0f, 0.001f);
	return true;
}

TEST(Reflect_StraightBounce)
{
	float dir[3] = {0.0f, -1.0f, 0.0f};
	float normal[3] = {0.0f, 1.0f, 0.0f};
	float out[3];
	ReflectVector(dir, normal, out);
	ASSERT_NEAR(out[0], 0.0f, 0.0001f);
	ASSERT_NEAR(out[1], 1.0f, 0.0001f);
	ASSERT_NEAR(out[2], 0.0f, 0.0001f);
	return true;
}

TEST(Reflect_45DegOffFloor)
{
	float dir[3] = {1.0f, -1.0f, 0.0f};
	float normal[3] = {0.0f, 1.0f, 0.0f};
	float out[3];
	ReflectVector(dir, normal, out);
	ASSERT_NEAR(out[0], 1.0f, 0.0001f);
	ASSERT_NEAR(out[1], 1.0f, 0.0001f);
	ASSERT_NEAR(out[2], 0.0f, 0.0001f);
	return true;
}

TEST(Reflect_MirrorsAngleToNormal)
{
	float dir[3] = {0.6f, -0.8f, 0.0f};
	float normal[3] = {0.0f, 1.0f, 0.0f};
	float out[3];
	ReflectVector(dir, normal, out);
	//incident angle to normal equals reflected angle to normal (sign of dot flips)
	float dinN = dir[0]*normal[0] + dir[1]*normal[1] + dir[2]*normal[2];
	float outdotN = out[0]*normal[0] + out[1]*normal[1] + out[2]*normal[2];
	ASSERT_NEAR(fabs(dinN), fabs(outdotN), 0.0001f);
	//speed preserved
	float dl = sqrt(dir[0]*dir[0] + dir[1]*dir[1] + dir[2]*dir[2]);
	float ol = sqrt(out[0]*out[0] + out[1]*out[1] + out[2]*out[2]);
	ASSERT_NEAR(dl, ol, 0.0001f);
	return true;
}

TEST(Reflect_NonUnitNormalSafe)
{
	float dir[3] = {0.0f, -1.0f, 0.0f};
	float normal[3] = {0.0f, 3.0f, 0.0f};  //non-unit
	float out[3];
	ReflectVector(dir, normal, out);
	ASSERT_NEAR(out[1], 1.0f, 0.0001f);
	return true;
}

TEST(Reflect_ZeroNormalPassesThrough)
{
	float dir[3] = {0.5f, -0.5f, 0.1f};
	float normal[3] = {0.0f, 0.0f, 0.0f};
	float out[3];
	ReflectVector(dir, normal, out);
	ASSERT_NEAR(out[0], dir[0], 0.0001f);
	ASSERT_NEAR(out[1], dir[1], 0.0001f);
	ASSERT_NEAR(out[2], dir[2], 0.0001f);
	return true;
}

TEST(Damage_RicochetFalloff)
{
	Config cfg = MakeConfig();
	ASSERT_NEAR(RicochetDamage(100.0f, cfg), 50.0f, 0.0001f);
	return true;
}

TEST(Damage_PenetrationFalloff)
{
	Config cfg = MakeConfig();
	ASSERT_NEAR(PenetrationDamage(100.0f, cfg), 70.0f, 0.0001f);
	return true;
}

TEST(Energy_PenetrationFalloff)
{
	Config cfg = MakeConfig();
	ASSERT_NEAR(PenetrationEnergy(1.0f, cfg), 0.6f, 0.0001f);
	return true;
}

static void* Ptr(uintptr_t v) { return reinterpret_cast<void*>(v); }

TEST(HitSet_InitiallyEmpty)
{
	HitSet hs;
	ASSERT_EQ(hs.ActiveCount(), 0);
	ASSERT(!hs.AlreadyHit(Ptr(0x1000), 5));
	return true;
}

TEST(HitSet_RecordAndCheck)
{
	HitSet hs;
	hs.RecordHit(Ptr(0x1000), 5);
	ASSERT(hs.AlreadyHit(Ptr(0x1000), 5));
	ASSERT(!hs.AlreadyHit(Ptr(0x1000), 6));
	ASSERT_EQ(hs.ActiveCount(), 1);
	return true;
}

TEST(HitSet_NoDoubleHitAcrossRefs)
{
	HitSet hs;
	void* p = Ptr(0x1000);
	hs.RecordHit(p, 5);
	hs.RecordHit(p, 6);
	ASSERT(hs.AlreadyHit(p, 5));
	ASSERT(hs.AlreadyHit(p, 6));
	ASSERT_EQ(hs.ActiveCount(), 1);
	return true;
}

TEST(HitSet_DuplicateRefNotStored)
{
	HitSet hs;
	void* p = Ptr(0x1000);
	hs.RecordHit(p, 5);
	hs.RecordHit(p, 5);
	hs.RecordHit(p, 5);
	ASSERT_EQ(hs.entries[0].refCount, 1u);
	return true;
}

TEST(HitSet_ReleaseFreesSlot)
{
	HitSet hs;
	void* p = Ptr(0x1000);
	hs.RecordHit(p, 5);
	hs.Release(p);
	ASSERT_EQ(hs.ActiveCount(), 0);
	ASSERT(!hs.AlreadyHit(p, 5));
	return true;
}

TEST(HitSet_ReleaseUnknownSafe)
{
	HitSet hs;
	hs.Release(Ptr(0x9999));
	ASSERT_EQ(hs.ActiveCount(), 0);
	return true;
}

TEST(HitSet_RecycledPointerDoesNotInheritStale)
{
	HitSet hs;
	void* p = Ptr(0x2000);
	hs.RecordHit(p, 100);
	ASSERT(hs.AlreadyHit(p, 100));
	//projectile dies, slot freed
	hs.Release(p);
	//heap hands the same pointer value to a new projectile
	hs.RecordHit(p, 200);
	ASSERT(!hs.AlreadyHit(p, 100));  //stale hit not inherited
	ASSERT(hs.AlreadyHit(p, 200));
	return true;
}

TEST(HitSet_FullTableEvictsLruNoLeak)
{
	HitSet hs;
	for (int i = 0; i < HitSet::kMaxProjectiles; i++)
		hs.RecordHit(Ptr(0x10000 + i), 1);
	ASSERT_EQ(hs.ActiveCount(), HitSet::kMaxProjectiles);

	//first inserted is the least-recently-used, gets evicted for the new one
	hs.RecordHit(Ptr(0x20000), 1);
	ASSERT_EQ(hs.ActiveCount(), HitSet::kMaxProjectiles);  //no leak, no growth
	ASSERT(hs.AlreadyHit(Ptr(0x20000), 1));
	ASSERT(!hs.AlreadyHit(Ptr(0x10000), 1));  //lru evicted
	return true;
}

TEST(HitSet_EvictedPointerReusedStartsClean)
{
	HitSet hs;
	for (int i = 0; i < HitSet::kMaxProjectiles; i++)
		hs.RecordHit(Ptr(0x10000 + i), 1);
	//evict the lru (0x10000) by inserting a new projectile, no Release was called
	hs.RecordHit(Ptr(0x20000), 1);
	//the evicted pointer value comes back as a fresh projectile
	hs.RecordHit(Ptr(0x10000), 2);
	ASSERT(!hs.AlreadyHit(Ptr(0x10000), 1));  //old ref did not survive eviction
	ASSERT(hs.AlreadyHit(Ptr(0x10000), 2));
	return true;
}

TEST(HitSet_RefCapDoesNotOverflow)
{
	HitSet hs;
	void* p = Ptr(0x3000);
	for (UInt32 r = 0; r < 20; r++)
		hs.RecordHit(p, 1000 + r);
	ASSERT_EQ(hs.entries[0].refCount, (UInt32)HitSet::kMaxRefsPerProjectile);
	//first refs retained
	ASSERT(hs.AlreadyHit(p, 1000));
	ASSERT(hs.AlreadyHit(p, 1007));
	//beyond the cap not recorded
	ASSERT(!hs.AlreadyHit(p, 1008));
	return true;
}

TEST(HitSet_NullProjNeverMatches)
{
	HitSet hs;
	//zeroed slots use proj==0 as empty, a null query must not alias them
	ASSERT(!hs.AlreadyHit(0, 0));
	hs.RecordHit(Ptr(0x1000), 5);
	ASSERT(!hs.AlreadyHit(0, 5));
	return true;
}

TEST(HitSet_ClearResets)
{
	HitSet hs;
	hs.RecordHit(Ptr(0x1000), 5);
	hs.RecordHit(Ptr(0x2000), 6);
	hs.Clear();
	ASSERT_EQ(hs.ActiveCount(), 0);
	ASSERT(!hs.AlreadyHit(Ptr(0x1000), 5));
	return true;
}

//liveness mocks - function pointers, so no captures
static UInt32 g_deadRefID = 0;
static bool MockIsLive(UInt32 refID) { return refID != g_deadRefID; }
static bool AllLive(UInt32) { return true; }

//fill every slot with a distinct committed live entry, refIDs base..base+kSlots-1
static void FillLive(DepthTracker& t, UInt32 base)
{
	for (int i = 0; i < DepthTracker::kSlots; i++) {
		int s = t.Reserve(base + (UInt32)i, AllLive);
		t.Commit(s, base + (UInt32)i, 1);
	}
}

TEST(Depth_InitiallyEmpty)
{
	DepthTracker t;
	ASSERT_EQ(t.ActiveCount(), 0);
	ASSERT_EQ(t.DepthOf(0x1000), 0u);
	return true;
}

TEST(Depth_FullLiveTableRejectsReserve)
{
	DepthTracker t;
	FillLive(t, 0x1000);
	ASSERT_EQ(t.ActiveCount(), DepthTracker::kSlots);
	//no free slot, every entry resolves live, so a fresh chain cannot reserve
	ASSERT_EQ(t.Reserve(0x99999, AllLive), DepthTracker::kNone);
	return true;
}

TEST(Depth_VerifiedDeadEntryReclaimed)
{
	DepthTracker t;
	FillLive(t, 0x1000);
	g_deadRefID = 0x1005;             //one entry is now a dead refID
	int s = t.Reserve(0x99999, MockIsLive);
	ASSERT(s != DepthTracker::kNone); //the dead slot is reclaimed
	t.Commit(s, 0x99999, 1);
	ASSERT_EQ(t.DepthOf(0x99999), 1u);
	ASSERT_EQ(t.DepthOf(0x1005), 0u); //the dead entry is gone
	g_deadRefID = 0;
	return true;
}

TEST(Depth_ReservedSlotExcludedFromReentrantReserve)
{
	DepthTracker t;
	//fill all but one slot with live entries, leaving exactly one free
	for (int i = 0; i < DepthTracker::kSlots - 1; i++) {
		int s = t.Reserve(0x1000 + (UInt32)i, AllLive);
		t.Commit(s, 0x1000 + (UInt32)i, 1);
	}
	int mine = t.Reserve(0xA000, AllLive); //takes the last free slot, now reserved
	ASSERT(mine != DepthTracker::kNone);
	//a re-entrant impact cannot steal the reserved slot, and everything else is live
	ASSERT_EQ(t.Reserve(0xB000, AllLive), DepthTracker::kNone);
	return true;
}

TEST(Depth_ReleaseMakesSlotReusable)
{
	DepthTracker t;
	for (int i = 0; i < DepthTracker::kSlots - 1; i++) {
		int s = t.Reserve(0x1000 + (UInt32)i, AllLive);
		t.Commit(s, 0x1000 + (UInt32)i, 1);
	}
	int mine = t.Reserve(0xA000, AllLive);
	ASSERT(mine != DepthTracker::kNone);
	t.Release(mine); //veto or spawn failure gives the slot back
	int again = t.Reserve(0xB000, AllLive);
	ASSERT(again != DepthTracker::kNone); //slot is reusable
	return true;
}

TEST(Depth_ReleaseRestoresPriorFreeSlot)
{
	DepthTracker t;
	int s = t.Reserve(0x1000, AllLive); //reserves a free slot, holds nothing
	t.Release(s);
	ASSERT_EQ(t.ActiveCount(), 0); //released free slot leaves the table empty
	ASSERT_EQ(t.DepthOf(0x1000), 0u);
	return true;
}

TEST(Depth_CommitTransfersParentSlotToChild)
{
	DepthTracker t;
	int p = t.Reserve(0xBEEF, AllLive); //parent at depth 2
	t.Commit(p, 0xBEEF, 2);
	ASSERT_EQ(t.ActiveCount(), 1);
	ASSERT_EQ(t.DepthOf(0xBEEF), 2u);
	//the parent penetrates - reserve prefers its own slot, commit hands it to the child
	int slot = t.Reserve(0xBEEF, AllLive);
	ASSERT_EQ(slot, p);               //parent's own slot
	t.Commit(slot, 0xCAFE, 3);
	ASSERT_EQ(t.ActiveCount(), 1);    //no growth, the slot transferred
	ASSERT_EQ(t.DepthOf(0xCAFE), 3u);
	ASSERT_EQ(t.DepthOf(0xBEEF), 0u); //parent no longer tracked
	return true;
}

TEST(Depth_MaxDepthChainCannotContinue)
{
	DepthTracker t;
	int s = t.Reserve(0xC0DE, AllLive);
	t.Commit(s, 0xC0DE, 5);
	//the handler gates penetration on DepthOf < kMaxPenetrationDepth (5), so a depth-5 round is refused
	UInt32 d = t.DepthOf(0xC0DE);
	ASSERT_EQ(d, 5u);
	ASSERT(!(d < 5u)); //the handler's cap check would fail here, no further continuation
	return true;
}

TEST(Depth_RemoveFreesCommittedSlot)
{
	DepthTracker t;
	int s = t.Reserve(0x1000, AllLive);
	t.Commit(s, 0x1000, 1);
	t.Remove(0x1000);
	ASSERT_EQ(t.ActiveCount(), 0);
	ASSERT_EQ(t.DepthOf(0x1000), 0u);
	return true;
}

TEST(Depth_RemoveSkipsReservedSlot)
{
	DepthTracker t;
	int p = t.Reserve(0xBEEF, AllLive);
	t.Commit(p, 0xBEEF, 1);
	int r = t.Reserve(0xBEEF, AllLive);  //reserves the parent's own slot (still holds 0xBEEF)
	ASSERT_EQ(r, p);
	t.Remove(0xBEEF);                    //must not clear a slot mid-reservation
	ASSERT_EQ(t.DepthOf(0xBEEF), 0u);    //DepthOf skips reserved, so it reads as untracked
	t.Release(r);
	ASSERT_EQ(t.DepthOf(0xBEEF), 1u);    //release restores it, Remove did not wipe it
	return true;
}

TEST(Depth_ClearResets)
{
	DepthTracker t;
	int s = t.Reserve(0x1000, AllLive);
	t.Commit(s, 0x1000, 3);
	t.Clear();
	ASSERT_EQ(t.ActiveCount(), 0);
	ASSERT_EQ(t.DepthOf(0x1000), 0u);
	return true;
}
