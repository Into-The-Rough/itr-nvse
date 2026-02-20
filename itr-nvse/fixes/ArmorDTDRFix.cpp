//npc armor DT/DR cache invalidation fix
//vanilla ResetArmorRating doesn't dirty the HighProcess CachedActorValues
//NOT hot-reloadable - requires game restart

#include "ArmorDTDRFix.h"
#include "internal/NVSEMinimal.h"
#include "internal/Detours.h"

#include "internal/globals.h"

namespace ArmorDTDRFix
{
	//HighProcess::DirtyCachedActorValues
	constexpr uint32_t kAddr_DirtyCachedActorValues = 0x900780;

	struct BaseProcess {
		char pad[0x28];
		uint32_t processLevel; //0=High, 1=MiddleHigh, 2=MiddleLow, 3=Low
	};
	static_assert(offsetof(BaseProcess, processLevel) == 0x28);

	struct Actor {
		char pad[0x68];
		BaseProcess* baseProcess;
	};

	template <typename T_Ret = void, typename... Args>
	__forceinline T_Ret ThisCall(uint32_t addr, void* _this, Args... args) {
		return ((T_Ret(__thiscall*)(void*, Args...))addr)(_this, args...);
	}

	typedef void(__thiscall* ResetArmorRating_t)(void*);

	static Detours::JumpDetour s_detour;
	static ResetArmorRating_t s_originalResetArmorRating = nullptr;
	static bool s_disabled = false;

	void __fastcall Hook_ResetArmorRating(void* character, void* edx) {
		if (!character || s_disabled || !s_originalResetArmorRating) return;

		s_originalResetArmorRating(character);

		//baseProcess is uninitialized during Character::Character construction
		//refID is 0 until the form is fully created, skip cache dirtying for half-constructed actors
		uint32_t refID = *(uint32_t*)((char*)character + 0x0C);
		if (!refID) return;

		BaseProcess* process = ((Actor*)character)->baseProcess;
		if (process && process->processLevel == 0) {
			ThisCall(kAddr_DirtyCachedActorValues, process, 76); //kAV_DamageThreshold
			ThisCall(kAddr_DirtyCachedActorValues, process, 18); //kAV_DamageResistance
		}
	}

	//prologue: 7 bytes
	void Init() {
		if (s_detour.WriteRelJump(0x8D4E80, Hook_ResetArmorRating, 7)) { //Character::ResetArmorRating
			s_originalResetArmorRating = s_detour.GetTrampoline<ResetArmorRating_t>();
			if (!s_originalResetArmorRating) {
				Log("ArmorDTDRFix failed: trampoline not created");
				s_disabled = true;
				s_detour.Remove();
				return;
			}
			Log("ArmorDTDRFix installed");
		}
		else {
			Log("ArmorDTDRFix failed to install");
		}
	}
}

void ArmorDTDRFix_Init()
{
	ArmorDTDRFix::Init();
}
