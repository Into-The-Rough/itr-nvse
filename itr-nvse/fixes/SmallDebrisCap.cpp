#include <Windows.h>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <unordered_set>

#include "SmallDebrisCap.h"
#include "SmallDebrisCapEngine.h"
#include "internal/Detours.h"
#include "internal/CallTemplates.h"
#include "internal/ScopedLock.h"
#include "internal/settings.h"

#include "internal/globals.h"

namespace SmallDebrisCap {
	constexpr uint32_t kMaxTracked = 65536;
	constexpr uint32_t kPendingSlots = 4096;
	constexpr uint32_t kMaxFading = 256;
	constexpr uint32_t kMarksPerFrame = 32;
	constexpr uint32_t kSettleMs = 250;
	constexpr uint32_t kSettleTimeoutMs = 5000;

	enum class State : uint8_t {
		Settling,
		Counted,
		Marked
	};

	struct Chunk {
		void* node;
		uint32_t addTick;
		uint32_t countTick;
		uint32_t markTick;
		State state;
		bool whole;
	};

	struct FadeEntry {
		void* node;
		void* root;
		float fade;
	};

	static Chunk s_chunks[kMaxTracked];
	static uint32_t s_count = 0;
	static std::unordered_set<void*> s_members;
	static void* s_pending[kPendingSlots];
	static uint32_t s_pendingCount = 0;
	static CRITICAL_SECTION s_pendingLock;
	static bool s_accepting = false;
	static FadeEntry s_fading[kMaxFading];
	static uint32_t s_fadingCount = 0;
	static CRITICAL_SECTION s_fadeLock;
	static bool s_initialized = false;
	static bool s_installed = false;
	static bool s_loading = false;
	static bool s_enabled = false;
	static bool s_busy = false;
	static bool s_resetRequested = false;
	static uint32_t s_limit = 8192;
	static uint32_t s_delayMs = 3000;
	static Detours::CallDetour s_addBody;
	typedef void(__thiscall* OnVisibleFn)(void*, void*);
	static OnVisibleFn s_originalVisible = nullptr;

	static void ClearFading()
	{
		FadeEntry old[kMaxFading];
		uint32_t count;
		{
			ScopedLock lock(&s_fadeLock);
			count = s_fadingCount;
			std::copy_n(s_fading, count, old);
			s_fadingCount = 0;
		}
		for (uint32_t i = 0; i < count; ++i) Engine::Release(old[i].root);
	}

	static void GateQueue(bool accepting)
	{
		ScopedLock lock(&s_pendingLock);
		s_accepting = accepting;
	}

	static uint32_t TakePending(void** nodes)
	{
		ScopedLock lock(&s_pendingLock);
		uint32_t count = s_pendingCount;
		std::copy_n(s_pending, count, nodes);
		s_pendingCount = 0;
		return count;
	}

	static void Reset()
	{
		ClearFading();
		void* pending[kPendingSlots];
		uint32_t count = TakePending(pending);
		for (uint32_t i = 0; i < count; ++i) Engine::Release(pending[i]);
		while (s_count) {
			Chunk chunk = s_chunks[--s_count];
			s_members.erase(chunk.node);
			if (chunk.state == State::Marked) Engine::Fade(chunk.node, 1.0f);
			Engine::Release(chunk.node);
		}
	}

	static void FinishWork()
	{
		//a disable or delete callback can request a reset while an update owns the pool
		while (s_resetRequested) {
			s_resetRequested = false;
			Reset();
		}
		s_busy = false;
		GateQueue(s_enabled && s_installed && !s_loading);
	}

	static Chunk Remove(uint32_t index)
	{
		Chunk result = s_chunks[index];
		s_chunks[index] = s_chunks[--s_count];
		s_members.erase(result.node);
		return result;
	}

	static void DrainPending(uint32_t now)
	{
		void* pending[kPendingSlots];
		uint32_t count = TakePending(pending);
		for (uint32_t i = 0; i < count; ++i) {
			void* node = pending[i];
			if (s_resetRequested || s_count >= s_limit || !s_members.insert(node).second) {
				Engine::Release(node);
				continue;
			}
			s_chunks[s_count++] = {node, now, 0, 0, State::Settling, false};
		}
	}

	//worker threads retain the node before publishing it
	static void QueueBody(void* body)
	{
		ScopedLock lock(&s_pendingLock);
		if (!s_accepting || s_pendingCount == kPendingSlots) return;
		if (void* node = Engine::DebrisNode(body)) {
			Engine::Retain(node);
			s_pending[s_pendingCount++] = node;
		}
	}

	static void __fastcall AddBodyHook(void* world, void* edx, void* body)
	{
		ThisCall<void>(s_addBody.GetOverwrittenAddr(), world, body);
		QueueBody(body);
	}

	static void __fastcall OnVisibleHook(void* root, void* edx, void* culler)
	{
		s_originalVisible(root, culler);
		ScopedLock lock(&s_fadeLock);
		for (uint32_t i = 0; i < s_fadingCount; ++i)
			if (s_fading[i].root == root) Engine::Fade(s_fading[i].node, s_fading[i].fade);
	}

	static bool Install()
	{
		if (!s_addBody.WriteRelCall(0xC8584D, AddBodyHook)) {
			Log("SmallDebrisCap: 0xC8584D is not a call, skipping");
			return false;
		}

		//bsfadenode onvisible
		auto* slot = reinterpret_cast<OnVisibleFn*>(0x10A9064);
		DWORD protect;
		if (!VirtualProtect(slot, sizeof(*slot), PAGE_EXECUTE_READWRITE, &protect)) {
			s_addBody.Remove();
			Log("SmallDebrisCap: OnVisible slot not writable, skipping");
			return false;
		}
		s_originalVisible = *slot;
		*slot = reinterpret_cast<OnVisibleFn>(OnVisibleHook);
		VirtualProtect(slot, sizeof(*slot), protect, &protect);
		FlushInstructionCache(GetCurrentProcess(), slot, sizeof(*slot));
		return true;
	}

	void InitHooks()
	{
		if (s_initialized) return;
		InitializeCriticalSection(&s_pendingLock);
		InitializeCriticalSection(&s_fadeLock);
		s_initialized = true;
		UpdateSettings();
	}

	void UpdateSettings()
	{
		if (!s_initialized) return;
		GateQueue(false);
		const bool wasEnabled = s_enabled;
		s_enabled = Settings::bSmallDebrisCap != 0;
		s_limit = static_cast<uint32_t>(std::clamp(Settings::iSmallDebrisMaxTrackedChunks, 256, static_cast<int>(kMaxTracked)));
		float delay = Settings::fSmallDebrisCullDelay;
		if (!std::isfinite(delay)) delay = 3.0f;
		s_delayMs = static_cast<uint32_t>(std::clamp(delay, 0.0f, 3600.0f) * 1000.0f);
		if (s_enabled && !s_installed) s_installed = Install();
		if (wasEnabled != s_enabled) s_resetRequested = true;
		if (!s_busy) {
			s_busy = true;
			s_members.reserve(s_limit);
			FinishWork();
		}
	}

	void ClearState(bool loading)
	{
		if (!s_initialized) return;
		s_loading = loading;
		GateQueue(false);
		s_resetRequested = true;
		if (!s_busy) {
			s_busy = true;
			FinishWork();
		}
	}

	void Update()
	{
		if (!s_initialized || !s_enabled || !s_installed || s_loading || s_busy) return;
		s_busy = true;
		ClearFading();
		const uint32_t now = GetTickCount();
		DrainPending(now);
		float fadeSeconds = Engine::FadeTime();
		if (!std::isfinite(fadeSeconds)) fadeSeconds = 1.0f;
		const uint32_t fadeMs = static_cast<uint32_t>(std::clamp(fadeSeconds, 0.01f, 3600.0f) * 1000.0f);
		uint32_t counted = 0;
		uint32_t marked = 0;
		for (uint32_t i = 0; i < s_count && !s_resetRequested;) {
			Chunk& chunk = s_chunks[i];
			bool inScene = Engine::InScene(chunk.node);
			if (chunk.state == State::Settling) {
				uint32_t age = now - chunk.addTick;
				if (age < kSettleMs || (!inScene && age < kSettleTimeoutMs)) {
					++i;
					continue;
				}
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
			if (chunk.state == State::Counted) {
				++counted;
				++i;
				continue;
			}
			if (now - chunk.markTick >= s_delayMs + fadeMs) {
				//removed before the engine call so a reentrant reset cannot touch this chunk
				Chunk removed = Remove(i);
				Engine::Cull(removed.node, removed.whole, s_resetRequested);
				Engine::Release(removed.node);
				continue;
			}
			++marked;
			++i;
		}

		const uint32_t maxCount = static_cast<uint32_t>((std::max)(Engine::MaxCount(), static_cast<int32_t>(0)));
		uint32_t toMark = counted > maxCount ? counted - maxCount : 0;
		toMark = (std::min)(toMark, (std::min)(kMarksPerFrame, kMaxFading - marked));
		for (uint32_t n = 0; n < toMark && !s_resetRequested; ++n) {
			uint32_t oldest = s_count;
			for (uint32_t i = 0; i < s_count; ++i)
				if (s_chunks[i].state == State::Counted
					&& (oldest == s_count || static_cast<int32_t>(s_chunks[i].countTick - s_chunks[oldest].countTick) < 0)) oldest = i;
			if (oldest == s_count) break;
			s_chunks[oldest].state = State::Marked;
			s_chunks[oldest].markTick = now;
		}

		if (!s_resetRequested) {
			ScopedLock lock(&s_fadeLock);
			for (uint32_t i = 0; i < s_count && s_fadingCount < kMaxFading; ++i) {
				const Chunk& chunk = s_chunks[i];
				if (chunk.state != State::Marked || now - chunk.markTick < s_delayMs) continue;
				float fade = (std::max)(1.0f - static_cast<float>(now - chunk.markTick - s_delayMs) / fadeMs, 0.0f);
				Engine::Fade(chunk.node, fade);
				if (void* root = Engine::Root(chunk.node)) {
					Engine::Retain(root);
					s_fading[s_fadingCount++] = {chunk.node, root, fade};
				}
			}
		}
		FinishWork();
	}
}
