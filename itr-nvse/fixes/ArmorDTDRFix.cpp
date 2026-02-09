//npc armor DT/DR cache invalidation fix
//vanilla ResetArmorRating doesn't dirty the HighProcess CachedActorValues
//NOT hot-reloadable - requires game restart

#include "ArmorDTDRFix.h"
#include "internal/NVSEMinimal.h"
#include "internal/Detours.h"

extern void Log(const char* fmt, ...);

namespace ArmorDTDRFix
{
	//Character::ResetArmorRating: 9260672 dec
	//HighProcess::DirtyCachedActorValues: 9439104 dec
	constexpr uint32_t kAddr_ResetArmorRating = 0x8D4E80;
	constexpr uint32_t kAddr_DirtyCachedActorValues = 0x900780;
	constexpr uint32_t kAV_DamageResistance = 18;
	constexpr uint32_t kAV_DamageThreshold = 76;

	struct BaseProcess {
		char pad[0x28];
		uint32_t processLevel; //0=High, 1=MiddleHigh, 2=MiddleLow, 3=Low
	};

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

		__try {
			s_originalResetArmorRating(character);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			Log("ArmorDTDRFix: exception calling ResetArmorRating trampoline, disabling fix");
			s_disabled = true;
			s_originalResetArmorRating = nullptr;
			s_detour.Remove();
			return;
		}

		//baseProcess is uninitialized during Character::Character construction
		//refID is 0 until the form is fully created, skip cache dirtying for half-constructed actors
		uint32_t refID = *(uint32_t*)((char*)character + 0x0C);
		if (!refID) return;

		BaseProcess* process = ((Actor*)character)->baseProcess;
		if (process && process->processLevel == 0) {
			ThisCall(kAddr_DirtyCachedActorValues, process, kAV_DamageThreshold);
			ThisCall(kAddr_DirtyCachedActorValues, process, kAV_DamageResistance);
		}
	}

	//prologue: 7 bytes
	void Init() {
		if (s_detour.WriteRelJump(kAddr_ResetArmorRating, Hook_ResetArmorRating, 7)) {
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
