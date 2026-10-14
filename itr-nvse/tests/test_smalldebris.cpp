#define NOMINMAX
#include <Windows.h>
#include <atomic>
#include <thread>
#include "../fixes/SmallDebrisCap.cpp"
#undef ASSERT
#include "test.h"

namespace DebrisTest {
struct Node {
	std::atomic<int> refs{1};
	bool inScene = true;
	bool fadeNode = false;
	bool cullable = true;
	float fade = 1.0f;
};
static bool invalidRelease;
static bool resetInCull;
static bool retainedDuringCull;
static unsigned culls;
static SInt32 maxCount;
static float fadeTime;

static void Start()
{
	using namespace SmallDebrisCap;
	if (!g_initialized) {
		InitializeCriticalSection(&g_pendingLock);
		InitializeCriticalSection(&g_fadeLock);
		g_initialized = true;
	}
	ClearState(true);
	g_enabled = true;
	g_installed = true;
	g_limit = 8192;
	g_delayMs = 0;
	Settings::bSmallDebrisCap = 1;
	ClearState(false);
	invalidRelease = resetInCull = retainedDuringCull = false;
	culls = 0;
	maxCount = 0;
	fadeTime = 0.01f;
}

static void Track(Node& node, SmallDebrisCap::State state, bool whole = false)
{
	using namespace SmallDebrisCap;
	Engine::Retain(&node);
	g_members.insert(&node);
	g_chunks[g_count++] = {&node, GetTickCount() - 1000, GetTickCount() - 1000, GetTickCount() - 1000, state, whole};
}
}

namespace SmallDebrisCap::Engine {
void Retain(void* p) { ++static_cast<DebrisTest::Node*>(p)->refs; }
void Release(void* p) { if (--static_cast<DebrisTest::Node*>(p)->refs < 1) DebrisTest::invalidRelease = true; }
bool IsFadeNode(void* p) { return static_cast<DebrisTest::Node*>(p)->fadeNode; }
void* Root(void* p) { return p; }
bool InScene(void* p) { return static_cast<DebrisTest::Node*>(p)->inScene; }
void* DebrisNode(void* p) { return p; }
TESObjectREFR* CullableRef(void* p) { return static_cast<DebrisTest::Node*>(p)->cullable ? reinterpret_cast<TESObjectREFR*>(p) : nullptr; }
void Fade(void* p, float fraction) { static_cast<DebrisTest::Node*>(p)->fade = fraction; }
void Cull(void* p, bool, const bool& cancelled)
{
	++DebrisTest::culls;
	if (DebrisTest::resetInCull) {
		ClearState(false);
		Update();
		DebrisTest::retainedDuringCull = static_cast<DebrisTest::Node*>(p)->refs == 2 && cancelled;
	}
}
SInt32 MaxCount() { return DebrisTest::maxCount; }
float FadeTime() { return DebrisTest::fadeTime; }
}

TEST(debris_queue_publishes_owned_nodes_and_drains_all_producers)
{
	using namespace SmallDebrisCap;
	DebrisTest::Start();
	DebrisTest::Node nodes[5000];
	std::thread workers[4];
	for (int w = 0; w < 4; ++w)
		workers[w] = std::thread([&, w] { for (int i = w * 1250; i < (w + 1) * 1250; ++i) QueueBody(&nodes[i]); });
	for (auto& worker : workers) worker.join();
	ASSERT_EQ(g_pendingCount, kPendingSlots);
	unsigned held = 0;
	for (auto& node : nodes) held += node.refs == 2;
	ASSERT_EQ(held, kPendingSlots);
	ClearState(true);
	QueueBody(&nodes[0]);
	ASSERT_EQ(g_pendingCount, 0u);
	for (auto& node : nodes) ASSERT_EQ(node.refs, 1);
	ASSERT(!DebrisTest::invalidRelease);
	return true;
}

TEST(debris_disable_restores_fade_and_releases_pool_and_snapshot)
{
	using namespace SmallDebrisCap;
	DebrisTest::Start();
	DebrisTest::Node node;
	node.fade = 0.25f;
	DebrisTest::Track(node, State::Marked);
	Engine::Retain(&node);
	g_fading[g_fadingCount++] = {&node, &node, 0.25f};
	Settings::bSmallDebrisCap = 0;
	UpdateSettings();
	ASSERT_EQ(g_count, 0u);
	ASSERT_EQ(g_fadingCount, 0u);
	ASSERT_EQ(node.refs, 1);
	ASSERT_EQ(node.fade, 1.0f);
	ASSERT(!g_accepting);
	ASSERT(!DebrisTest::invalidRelease);
	return true;
}

TEST(debris_cull_callback_can_reset_without_reusing_or_releasing_live_chunk)
{
	using namespace SmallDebrisCap;
	DebrisTest::Start();
	DebrisTest::Node first, second;
	DebrisTest::Track(first, State::Marked);
	DebrisTest::Track(second, State::Marked);
	DebrisTest::resetInCull = true;
	Update();
	ASSERT_EQ(DebrisTest::culls, 1u);
	ASSERT(DebrisTest::retainedDuringCull);
	ASSERT_EQ(g_count, 0u);
	ASSERT_EQ(first.refs, 1);
	ASSERT_EQ(second.refs, 1);
	ASSERT(!DebrisTest::invalidRelease);
	return true;
}

TEST(debris_fading_budget_and_oldest_selection_survive_pool_compaction)
{
	using namespace SmallDebrisCap;
	DebrisTest::Start();
	DebrisTest::fadeTime = 1000.0f;
	DebrisTest::Node nodes[300];
	for (auto& node : nodes) DebrisTest::Track(node, State::Counted);
	g_chunks[299].countTick = GetTickCount() - 2000;
	Update();
	ASSERT_EQ(g_chunks[299].state, State::Marked);
	for (int frame = 0; frame < 10; ++frame) Update();
	unsigned marked = 0;
	for (UInt32 i = 0; i < g_count; ++i) marked += g_chunks[i].state == State::Marked;
	ASSERT_EQ(marked, kMaxFading);
	ClearState(true);
	for (auto& node : nodes) ASSERT_EQ(node.refs, 1);
	ASSERT(!DebrisTest::invalidRelease);
	return true;
}

TEST(debris_keeps_protected_model_roots_and_deduplicates_bodies)
{
	using namespace SmallDebrisCap;
	DebrisTest::Start();
	DebrisTest::Node protectedRoot, piece;
	protectedRoot.fadeNode = true;
	protectedRoot.cullable = false;
	DebrisTest::Track(protectedRoot, State::Settling);
	QueueBody(&piece);
	QueueBody(&piece);
	Update();
	ASSERT_EQ(protectedRoot.refs, 1);
	ASSERT_EQ(piece.refs, 2);
	ASSERT_EQ(g_count, 1u);
	ClearState(true);
	ASSERT_EQ(piece.refs, 1);
	ASSERT(!DebrisTest::invalidRelease);
	return true;
}

TEST(debris_unrelated_config_reload_keeps_tracked_nodes)
{
	using namespace SmallDebrisCap;
	DebrisTest::Start();
	DebrisTest::Node node;
	DebrisTest::Track(node, State::Counted);
	UpdateSettings();
	ASSERT_EQ(g_count, 1u);
	ASSERT_EQ(node.refs, 2);
	ASSERT(g_accepting);
	ClearState(true);
	ASSERT_EQ(node.refs, 1);
	return true;
}
