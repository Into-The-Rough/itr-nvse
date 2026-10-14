#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_set>

#include "SmallDebrisCap.h"
#include "SmallDebrisCapEngine.h"
#include "internal/Detours.h"
#include "internal/CallTemplates.h"
#include "internal/ScopedLock.h"
#include "internal/settings.h"

namespace SmallDebrisCap {
namespace {

constexpr UInt32 kMaxTracked = 65536;
constexpr UInt32 kPendingSlots = 4096;
constexpr UInt32 kMaxFading = 256;
constexpr UInt32 kMarksPerFrame = 32;
constexpr UInt32 kSettleMs = 250;
constexpr UInt32 kSettleTimeoutMs = 5000;

enum class State : UInt8 { Settling, Counted, Marked };

struct Chunk {
	void* node;
	UInt32 addTick;
	UInt32 countTick;
	UInt32 markTick;
	State state;
	bool whole;
};

struct FadeEntry {
	void* node;
	void* root;
	float fade;
};

Chunk g_chunks[kMaxTracked];
UInt32 g_count = 0;
std::unordered_set<void*> g_members;
void* g_pending[kPendingSlots];
UInt32 g_pendingCount = 0;
CRITICAL_SECTION g_pendingLock;
bool g_accepting = false;
FadeEntry g_fading[kMaxFading];
UInt32 g_fadingCount = 0;
CRITICAL_SECTION g_fadeLock;
bool g_initialized = false;
bool g_installed = false;
bool g_loading = false;
bool g_enabled = false;
bool g_busy = false;
bool g_resetRequested = false;
UInt32 g_limit = 8192;
UInt32 g_delayMs = 3000;
FILE* g_log = nullptr;
Detours::CallDetour g_addBody;
using Visible = void(__thiscall*)(void*, void*);
Visible g_originalVisible = nullptr;

void ClearFading()
{
	FadeEntry old[kMaxFading];
	UInt32 count;
	{
		ScopedLock lock(&g_fadeLock);
		count = g_fadingCount;
		std::copy_n(g_fading, count, old);
		g_fadingCount = 0;
	}
	for (UInt32 i = 0; i < count; ++i) Engine::Release(old[i].root);
}

void GateQueue(bool accepting)
{
	ScopedLock lock(&g_pendingLock);
	g_accepting = accepting;
}

UInt32 TakePending(void** nodes)
{
	ScopedLock lock(&g_pendingLock);
	UInt32 count = g_pendingCount;
	std::copy_n(g_pending, count, nodes);
	g_pendingCount = 0;
	return count;
}

void Reset()
{
	ClearFading();
	void* pending[kPendingSlots];
	UInt32 count = TakePending(pending);
	for (UInt32 i = 0; i < count; ++i) Engine::Release(pending[i]);
	while (g_count) {
		Chunk chunk = g_chunks[--g_count];
		g_members.erase(chunk.node);
		if (chunk.state == State::Marked) Engine::Fade(chunk.node, 1.0f);
		Engine::Release(chunk.node);
	}
}

void FinishWork()
{
	//a disable/delete callback can request a reset while an update owns the pool
	while (g_resetRequested) {
		g_resetRequested = false;
		Reset();
	}
	g_busy = false;
	GateQueue(g_enabled && g_installed && !g_loading);
}

Chunk Remove(UInt32 index)
{
	Chunk result = g_chunks[index];
	g_chunks[index] = g_chunks[--g_count];
	g_members.erase(result.node);
	return result;
}

void DrainPending(UInt32 now)
{
	void* pending[kPendingSlots];
	UInt32 count = TakePending(pending);
	for (UInt32 i = 0; i < count; ++i) {
		void* node = pending[i];
		if (g_resetRequested || g_count >= g_limit || !g_members.insert(node).second) {
			Engine::Release(node);
			continue;
		}
		g_chunks[g_count++] = {node, now, 0, 0, State::Settling, false};
	}
}

void QueueBody(void* body)
{
	//publish the node and its owning reference together, before a consumer can see it
	ScopedLock lock(&g_pendingLock);
	if (!g_accepting || g_pendingCount == kPendingSlots) return;
	if (void* node = Engine::DebrisNode(body)) {
		Engine::Retain(node);
		g_pending[g_pendingCount++] = node;
	}
}

void __fastcall AddBodyHook(void* world, void*, void* body)
{
	ThisCall<void>(g_addBody.GetOverwrittenAddr(), world, body);
	QueueBody(body);
}

void __fastcall OnVisibleHook(void* root, void*, void* culler)
{
	g_originalVisible(root, culler);
	ScopedLock lock(&g_fadeLock);
	for (UInt32 i = 0; i < g_fadingCount; ++i)
		if (g_fading[i].root == root) Engine::Fade(g_fading[i].node, g_fading[i].fade);
}

bool Install()
{
	//0xc8584d is an e8 __thiscall(bhkWorld*, hkpWorldObject*) with one stack argument
	if (!g_addBody.WriteRelCall(0xC8584D, AddBodyHook)) return false;
	//bsfadenode::onvisible, capturing the current slot preserves earlier plugin hooks
	auto* slot = reinterpret_cast<Visible*>(0x10A9064);
	DWORD protect;
	if (!VirtualProtect(slot, sizeof(*slot), PAGE_EXECUTE_READWRITE, &protect)) {
		g_addBody.Remove();
		return false;
	}
	g_originalVisible = *slot;
	*slot = reinterpret_cast<Visible>(OnVisibleHook);
	VirtualProtect(slot, sizeof(*slot), protect, &protect);
	FlushInstructionCache(GetCurrentProcess(), slot, sizeof(*slot));
	return true;
}

void OpenLog()
{
	char path[MAX_PATH];
	GetModuleFileNameA(nullptr, path, MAX_PATH);
	char* slash = strrchr(path, '\\');
	if (!slash) return;
	strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), "ITR-SmallDebrisCap.log");
	fopen_s(&g_log, path, "w");
}

}

void InitHooks()
{
	if (g_initialized) return;
	InitializeCriticalSection(&g_pendingLock);
	InitializeCriticalSection(&g_fadeLock);
	g_initialized = true;
	OpenLog();
	UpdateSettings();
}

void UpdateSettings()
{
	if (!g_initialized) return;
	GateQueue(false);
	const bool wasEnabled = g_enabled;
	g_enabled = Settings::bSmallDebrisCap != 0;
	g_limit = static_cast<UInt32>(std::clamp(Settings::GetINIInt("SmallDebrisCap", "iMaxTrackedChunks", 8192), 256, static_cast<int>(kMaxTracked)));
	char text[32];
	GetPrivateProfileStringA("SmallDebrisCap", "fCullDelay", "3.0", text, sizeof(text), Settings::iniPath);
	float delay = static_cast<float>(atof(text));
	if (!std::isfinite(delay)) delay = 3.0f;
	g_delayMs = static_cast<UInt32>(std::clamp(delay, 0.0f, 3600.0f) * 1000.0f);
	if (g_enabled && !g_installed) g_installed = Install();
	if (wasEnabled != g_enabled) g_resetRequested = true;
	if (!g_busy) {
		g_busy = true;
		g_members.reserve(g_limit);
		FinishWork();
	}
	if (g_log) {
		fprintf(g_log, "enabled=%u installed=%u maxTracked=%u cullDelayMs=%u\n", g_enabled, g_installed, g_limit, g_delayMs);
		fflush(g_log);
	}
}

void ClearState(bool loading)
{
	if (!g_initialized) return;
	g_loading = loading;
	GateQueue(false);
	g_resetRequested = true;
	if (!g_busy) {
		g_busy = true;
		FinishWork();
	}
}

void Update()
{
	if (!g_initialized || !g_enabled || !g_installed || g_loading || g_busy) return;
	g_busy = true;
	ClearFading();
	const UInt32 now = GetTickCount();
	DrainPending(now);
	float fadeSeconds = Engine::FadeTime();
	if (!std::isfinite(fadeSeconds)) fadeSeconds = 1.0f;
	const UInt32 fadeMs = static_cast<UInt32>(std::clamp(fadeSeconds, 0.01f, 3600.0f) * 1000.0f);
	UInt32 counted = 0;
	UInt32 marked = 0;
	for (UInt32 i = 0; i < g_count && !g_resetRequested;) {
		Chunk& chunk = g_chunks[i];
		bool inScene = Engine::InScene(chunk.node);
		if (chunk.state == State::Settling) {
			UInt32 age = now - chunk.addTick;
			if (age < kSettleMs || (!inScene && age < kSettleTimeoutMs)) { ++i; continue; }
			if (inScene) {
				chunk.whole = Engine::IsFadeNode(chunk.node);
				if (chunk.whole && !Engine::CullableRef(chunk.node)) inScene = false;
			}
			if (inScene) {
				chunk.state = State::Counted;
				chunk.countTick = now;
			}
		}
		if (!inScene) {
			Engine::Release(Remove(i).node);
			continue;
		}
		if (chunk.state == State::Counted) { ++counted; ++i; continue; }
		if (now - chunk.markTick >= g_delayMs + fadeMs) {
			//remove before calling the engine, so reentrant resets cannot invalidate this chunk
			Chunk removed = Remove(i);
			Engine::Cull(removed.node, removed.whole, g_resetRequested);
			Engine::Release(removed.node);
			continue;
		}
		++marked;
		++i;
	}

	const UInt32 maxCount = static_cast<UInt32>((std::max)(Engine::MaxCount(), static_cast<SInt32>(0)));
	UInt32 toMark = counted > maxCount ? counted - maxCount : 0;
	toMark = (std::min)(toMark, (std::min)(kMarksPerFrame, kMaxFading - marked));
	for (UInt32 n = 0; n < toMark && !g_resetRequested; ++n) {
		UInt32 oldest = g_count;
		for (UInt32 i = 0; i < g_count; ++i)
			if (g_chunks[i].state == State::Counted &&
				(oldest == g_count || static_cast<SInt32>(g_chunks[i].countTick - g_chunks[oldest].countTick) < 0)) oldest = i;
		if (oldest == g_count) break;
		g_chunks[oldest].state = State::Marked;
		g_chunks[oldest].markTick = now;
	}

	if (!g_resetRequested) {
		ScopedLock lock(&g_fadeLock);
		for (UInt32 i = 0; i < g_count && g_fadingCount < kMaxFading; ++i) {
			const Chunk& chunk = g_chunks[i];
			if (chunk.state != State::Marked || now - chunk.markTick < g_delayMs) continue;
			float fade = (std::max)(1.0f - static_cast<float>(now - chunk.markTick - g_delayMs) / fadeMs, 0.0f);
			Engine::Fade(chunk.node, fade);
			if (void* root = Engine::Root(chunk.node)) {
				Engine::Retain(root);
				g_fading[g_fadingCount++] = {chunk.node, root, fade};
			}
		}
	}
	FinishWork();
}

}
