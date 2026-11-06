//fades and removes the oldest small debris (layer 19) once there is more than iDebrisMaxCount

#include <Windows.h>
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
	constexpr UInt32 kMaxTracked = 65536;
	constexpr UInt32 kPendingSlots = 4096;
	constexpr UInt32 kMaxFading = 256;
	constexpr UInt32 kMarksPerFrame = 32;
	constexpr UInt32 kSettleMs = 250;
	constexpr UInt32 kSettleTimeoutMs = 5000;

	enum class State : UInt8 {
		Settling,
		Counted,
		Marked
	};

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

	static Chunk s_chunks[kMaxTracked];
	static UInt32 s_count = 0;
	static std::unordered_set<void*> s_members;
	static void* s_pending[kPendingSlots];
	static UInt32 s_pendingCount = 0;
	static CRITICAL_SECTION s_pendingLock;
	static bool s_accepting = false;
	static FadeEntry s_fading[kMaxFading];
	static UInt32 s_fadingCount = 0;
	static CRITICAL_SECTION s_fadeLock;
	static bool s_initialized = false;
	static bool s_installed = false;
	static bool s_loading = false;
	static bool s_enabled = false;
	static bool s_busy = false;
	static bool s_resetRequested = false;
	static UInt32 s_limit = 8192;
	static UInt32 s_delayMs = 3000;
	static Detours::CallDetour s_addBody;
	typedef void(__thiscall* OnVisibleFn)(void*, void*);
	static OnVisibleFn s_originalVisible = nullptr;

	static void ClearFading()
	{
		FadeEntry old[kMaxFading];
		UInt32 count;
		{
			ScopedLock lock(&s_fadeLock);
			count = s_fadingCount;
			std::copy_n(s_fading, count, old);
			s_fadingCount = 0;
		}
		for (UInt32 i = 0; i < count; ++i) Engine::Release(old[i].root);
	}

	static void GateQueue(bool accepting)
	{
		ScopedLock lock(&s_pendingLock);
		s_accepting = accepting;
	}

	static UInt32 TakePending(void** nodes)
	{
		ScopedLock lock(&s_pendingLock);
		UInt32 count = s_pendingCount;
		std::copy_n(s_pending, count, nodes);
		s_pendingCount = 0;
		return count;
	}

	static void Reset()
	{
		ClearFading();
		void* pending[kPendingSlots];
		UInt32 count = TakePending(pending);
		for (UInt32 i = 0; i < count; ++i) Engine::Release(pending[i]);
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

	static Chunk Remove(UInt32 index)
	{
		Chunk result = s_chunks[index];
		s_chunks[index] = s_chunks[--s_count];
		s_members.erase(result.node);
		return result;
	}

	static void DrainPending(UInt32 now)
	{
		void* pending[kPendingSlots];
		UInt32 count = TakePending(pending);
		for (UInt32 i = 0; i < count; ++i) {
			void* node = pending[i];
			if (s_resetRequested || s_count >= s_limit || !s_members.insert(node).second) {
				Engine::Release(node);
				continue;
			}
			s_chunks[s_count++] = {node, now, 0, 0, State::Settling, false};
		}
	}

	//runs on havok worker threads, the node is retained under the lock before the main thread can see it
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
		for (UInt32 i = 0; i < s_fadingCount; ++i)
			if (s_fading[i].root == root) Engine::Fade(s_fading[i].node, s_fading[i].fade);
	}

	static bool Install()
	{
		//0xC8584D: call bhkWorld add world object, thiscall(world, hkpWorldObject)
		if (!s_addBody.WriteRelCall(0xC8584D, AddBodyHook)) {
			Log("SmallDebrisCap: 0xC8584D is not a call, skipping");
			return false;
		}

		//BSFadeNode::OnVisible vtable slot, the current value keeps earlier plugin hooks chained
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
		s_limit = static_cast<UInt32>(std::clamp(Settings::iSmallDebrisMaxTrackedChunks, 256, static_cast<int>(kMaxTracked)));
		float delay = Settings::fSmallDebrisCullDelay;
		if (!std::isfinite(delay)) delay = 3.0f;
		s_delayMs = static_cast<UInt32>(std::clamp(delay, 0.0f, 3600.0f) * 1000.0f);
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
		const UInt32 now = GetTickCount();
		DrainPending(now);
		float fadeSeconds = Engine::FadeTime();
		if (!std::isfinite(fadeSeconds)) fadeSeconds = 1.0f;
		const UInt32 fadeMs = static_cast<UInt32>(std::clamp(fadeSeconds, 0.01f, 3600.0f) * 1000.0f);
		UInt32 counted = 0;
		UInt32 marked = 0;
		for (UInt32 i = 0; i < s_count && !s_resetRequested;) {
			Chunk& chunk = s_chunks[i];
			bool inScene = Engine::InScene(chunk.node);
			if (chunk.state == State::Settling) {
				UInt32 age = now - chunk.addTick;
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

		const UInt32 maxCount = static_cast<UInt32>((std::max)(Engine::MaxCount(), static_cast<SInt32>(0)));
		UInt32 toMark = counted > maxCount ? counted - maxCount : 0;
		toMark = (std::min)(toMark, (std::min)(kMarksPerFrame, kMaxFading - marked));
		for (UInt32 n = 0; n < toMark && !s_resetRequested; ++n) {
			UInt32 oldest = s_count;
			for (UInt32 i = 0; i < s_count; ++i)
				if (s_chunks[i].state == State::Counted
					&& (oldest == s_count || static_cast<SInt32>(s_chunks[i].countTick - s_chunks[oldest].countTick) < 0)) oldest = i;
			if (oldest == s_count) break;
			s_chunks[oldest].state = State::Marked;
			s_chunks[oldest].markTick = now;
		}

		if (!s_resetRequested) {
			ScopedLock lock(&s_fadeLock);
			for (UInt32 i = 0; i < s_count && s_fadingCount < kMaxFading; ++i) {
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
