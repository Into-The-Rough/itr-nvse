#include "WeatherChangeEvent.h"

#include <cstddef>

#include "internal/NVSEPluginAPI.h"
#include "internal/Detours.h"
#include "internal/EventDispatch.h"
#include "internal/globals.h"

namespace {
	struct Sky {
		UInt8    pad00[0x10];
		TESForm* currWeather;   //0x10
		TESForm* transWeather;  //0x14 outgoing weather, non-null only during a blend
	};

	static_assert(offsetof(Sky, currWeather) == 0x10, "Sky current weather offset changed");
	static_assert(offsetof(Sky, transWeather) == 0x14, "Sky transition weather offset changed");

	constexpr UInt32 kCallSite_UpdateWeather = 0x63ADCF; //sole caller, inside Sky::Update
	constexpr UInt32 kAddr_UpdateWeather = 0x63D1D0;
	constexpr UInt32 kAddr_ForceWeather = 0x63D0E0;
	constexpr UInt32 kForceWeatherCallSites[] = { 0x5B764E, 0x5B76C1 };
	constexpr UInt32 kForceWeatherCallCount = sizeof(kForceWeatherCallSites) / sizeof(kForceWeatherCallSites[0]);

	enum { kPhase_Start = 0, kPhase_Complete = 1 };

	using UpdateWeather_t = void(__thiscall*)(Sky*);
	using ForceWeather_t = void(__thiscall*)(Sky*, TESForm*, char);

	Detours::CallDetour g_updateDetour;
	Detours::CallDetour g_forceDetours[kForceWeatherCallCount];
	UInt32 g_dispatchDepth = 0;

	void Dispatch(int phase, TESForm* next, TESForm* prev) {
		if (!g_eventManagerInterface || g_dispatchDepth >= 16)
			return;
		g_dispatchDepth++;
		g_eventManagerInterface->DispatchEvent("ITR:OnWeatherChange", nullptr, phase, next, prev);
		g_dispatchDepth--;
	}

	void __fastcall UpdateWeatherHook(Sky* sky, void*) {
		TESForm* prevCurr = sky->currWeather;
		TESForm* prevTrans = sky->transWeather;
		auto original = reinterpret_cast<UpdateWeather_t>(g_updateDetour.GetOverwrittenAddr());
		original(sky);
		TESForm* next = sky->currWeather;
		TESForm* trans = sky->transWeather;

		if (next != prevCurr) {
			Dispatch(kPhase_Start, next, prevCurr);
			if (!trans)
				Dispatch(kPhase_Complete, next, prevCurr);
		}
		else if (prevTrans && !trans) {
			Dispatch(kPhase_Complete, next, prevTrans);
		}
	}

	template <UInt32 N>
	void __fastcall ForceWeatherHook(Sky* sky, void*, TESForm* weather, char flag) {
		TESForm* prevCurr = sky->currWeather;
		auto original = reinterpret_cast<ForceWeather_t>(g_forceDetours[N].GetOverwrittenAddr());
		original(sky, weather, flag);
		TESForm* next = sky->currWeather;
		if (next != prevCurr) {
			Dispatch(kPhase_Start, next, prevCurr);
			Dispatch(kPhase_Complete, next, prevCurr);
		}
	}

	void RemoveHooks() {
		for (UInt32 i = kForceWeatherCallCount; i > 0; i--)
			g_forceDetours[i - 1].Remove();
		g_updateDetour.Remove();
	}
}

namespace WeatherChangeEvent {
	bool Init() {
		if (!g_updateDetour.WriteRelCall(kCallSite_UpdateWeather, UpdateWeatherHook)) {
			Log("WeatherChangeEvent: update call site %08X unusable", kCallSite_UpdateWeather);
			return false;
		}
		Log("WeatherChangeEvent: %08X hooked, original=%08X vanilla=%08X", kCallSite_UpdateWeather,
			g_updateDetour.GetOverwrittenAddr(), kAddr_UpdateWeather);

		typedef void (__fastcall* ForceWeatherHook_t)(Sky*, void*, TESForm*, char);
		static const ForceWeatherHook_t hooks[kForceWeatherCallCount] = {
			ForceWeatherHook<0>, ForceWeatherHook<1>
		};

		for (UInt32 i = 0; i < kForceWeatherCallCount; i++) {
			if (!g_forceDetours[i].WriteRelCall(kForceWeatherCallSites[i], hooks[i])) {
				Log("WeatherChangeEvent: ForceWeather call site %08X unusable, backing out", kForceWeatherCallSites[i]);
				RemoveHooks();
				return false;
			}
			Log("WeatherChangeEvent: %08X hooked, original=%08X vanilla=%08X", kForceWeatherCallSites[i],
				g_forceDetours[i].GetOverwrittenAddr(), kAddr_ForceWeather);
		}

		return true;
	}
}
